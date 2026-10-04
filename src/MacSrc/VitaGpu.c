// The Vita's GPU as a filler for the rasterizer queue: see VitaGpu.h.

#if defined(VITA) && defined(VITA_PROFILE)

#include <psp2/gxm.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vita2d.h>
#include <vitashark.h>

#include "Shock.h"
#include "VitaGpu.h"
#include "rastq.h"
#include "vprof.h"

#define SHACCCG_PATH "ur0:data/libshacccg.suprx"
#define GPU_BLOCK_ALIGN (256 * 1024) // CDRAM blocks come in multiples of this
#define MAX_BLOCKS 12
#define MAX_TARGETS 2
#define MAX_VERTICES 16384
#define MAX_INDICES 49152
#define MAX_DRAWS 2048
// The bitmaps of one scene, copied where the GPU can read them
#define MAX_SCENE_TEXTURES 512
#define TEXTURE_HEAP_BYTES (4 * 1024 * 1024)
// Start-up check canvas
#define CHECK_W 256
#define CHECK_H 32
// Frame pairs written for looking at on a PC: the 3rd comparison, and each
// one after it that differs by more pixels than any before and by more than
// this share of them, up to MAX_DUMPS in all. Writing one stops the game for
// a second or two.
#define DUMP_SHARE 0.05
#define MAX_DUMPS 5

// What the vertex shaders get, each reading what it needs.
// - Shader A works the texel out itself: x and y in clip space, the texel
//   coordinates times q, q, the table row as row / q + flat_row, and the
//   bitmap's size and whether it wraps.
// - Shader B leaves the perspective division and the wrap to the GPU: x and
//   y times w, u and v as shares of the bitmap, w where A has q, and the
//   table row in `row`.
// - The flat-colour shaders, used at start-up and when the textured ones
//   didn't compile, read the position and, where the others have u, the
//   palette index as the 0..1 value the 8-bit target stores.
typedef struct {
    float x, y;
    float u, v, q;
    float row, flat_row;
    float inv_w, inv_h, wrap, unused;
} vgpu_vertex;

// A set of textured shaders
typedef struct {
    SceGxmVertexProgram *vertex;
    SceGxmFragmentProgram *opaque, *trans; // trans: texel 0 leaves the pixel alone
    unsigned bitmap_unit, tables_unit, trans_bitmap_unit, trans_tables_unit;
    int ok;
} gpu_shader;
enum { SHADER_A, SHADER_B, SHADERS };

typedef struct {
    void *base;
    size_t size;
    SceUID uid;
} gpu_block;

// A size of canvas the GPU has drawn into
typedef struct {
    int w, h;
    SceGxmRenderTarget *target;
    SceGxmDepthStencilSurface depth_stencil;
    void *depth;
} gpu_target;

// Consecutive polygons with the same bitmap and shader
typedef struct {
    const SceGxmTexture *texture;
    int trans;
    unsigned first, count; // indices
} gpu_draw;

static SceGxmContext *context;
static SceGxmVertexProgram *flat_vertex_program;
static SceGxmFragmentProgram *flat_fragment_program;
static gpu_shader shaders[SHADERS];
static int textured;          // shader A is there: without it, flat colours
static int scene_shader;      // the one the scene under way is drawn with
static int check_shader = -1; // >= 0: the one a start-up check asks for

// The views' canvases: see vgpu_set_canvases
static uchar *canvas_pixels[VGPU_CANVASES];
static int canvas_w, canvas_h, canvas_stride, canvas_turn;

static gpu_block blocks[MAX_BLOCKS];
static gpu_target targets[MAX_TARGETS];
static SceGxmColorSurface scene_color;
static int in_scene;

static vgpu_vertex *vertices;
static uint16_t *indices;
static unsigned vertex_count, index_count;
static gpu_draw draws[MAX_DRAWS];
static unsigned draw_count;
static float to_clip_x, to_clip_y;

static uchar *tables_pixels; // RASTQ_GPU_TABLE_ROWS rows of 256
static SceGxmTexture tables_texture;
static uchar *texture_heap;
static size_t texture_heap_used;
static SceGxmTexture scene_textures[MAX_SCENE_TEXTURES];
static struct {
    const uchar *bits;
    int w, h;
} scene_bitmaps[MAX_SCENE_TEXTURES];
static int scene_texture_count;

unsigned long long vgpu_texture_bytes, vgpu_swap_wait_us;

static int ready;
static int begin_errors_logged, refusals_logged;
static char report[640] = "gpu: not initialized";

extern vita2d_texture *texBuffer;

// The vertex shaders take positions already in clip space. The fragment
// shaders write a palette index to the 8-bit target.
static const char flat_vertex_source[] =
    "void main(\n"
    "    float2 aPosition,\n"
    "    float aIndex,\n"
    "    out float4 vPosition : POSITION,\n"
    "    out float vIndex : TEXCOORD0)\n"
    "{\n"
    "    vPosition = float4(aPosition, 0.5f, 1.0f);\n"
    "    vIndex = aIndex;\n"
    "}\n";

static const char flat_fragment_source[] =
    "float4 main(float vIndex : TEXCOORD0)\n"
    "{\n"
    "    return float4(vIndex, vIndex, vIndex, 1.0f);\n"
    "}\n";

static const char tmap_vertex_source[] =
    "void main(\n"
    "    float2 aPosition,\n"
    "    float3 aTex,\n"
    "    float2 aRow,\n"
    "    float4 aParams,\n"
    "    out float4 vPosition : POSITION,\n"
    "    out float3 vTex : TEXCOORD0,\n"
    "    out float2 vRow : TEXCOORD1,\n"
    "    out float4 vParams : TEXCOORD2)\n"
    "{\n"
    "    vPosition = float4(aPosition, 0.5f, 1.0f);\n"
    "    vTex = aTex;\n"
    "    vRow = aRow;\n"
    "    vParams = aParams;\n"
    "}\n";

// What every software mapper does for a pixel: the texel at (u, v) rounded
// down, passed through a row of the tables. vParams is 1 / width,
// 1 / height and whether the coordinates wrap. The texel coordinates are
// taken to the middle of the texel, and the index to a quarter above itself,
// so that rounding can't land on a neighbour. The first %s is what a
// transparent bitmap adds.
static const char tmap_fragment_source[] =
    "float4 main(\n"
    "    float3 vTex : TEXCOORD0,\n"
    "    float2 vRow : TEXCOORD1,\n"
    "    float4 vParams : TEXCOORD2,\n"
    "    uniform sampler2D uBitmap : TEXUNIT0,\n"
    "    uniform sampler2D uTables : TEXUNIT1)\n"
    "{\n"
    "    float2 uv = floor(vTex.xy / vTex.z);\n"
    "    float2 st = (uv + 0.5f) * vParams.xy;\n"
    "    st -= floor(st) * vParams.z;\n"
    "    float pal = tex2D(uBitmap, st).x;\n"
    "%s"
    "    float tab = floor(vRow.x / vTex.z + vRow.y);\n"
    "    float2 lut = float2(pal * (255.0f / 256.0f) + 0.5f / 256.0f, (tab + 0.5f) / %d.0f);\n"
    "    float shade = tex2D(uTables, lut).x + 0.25f / 255.0f;\n"
    "    return float4(shade, shade, shade, 1.0f);\n"
    "}\n";
static const char trans_fragment_lines[] = "    if (pal < 0.5f / 255.0f) discard;\n";

// Shader B: the same result with less to do per pixel. The vertices carry a
// real w, so the GPU interpolates u, v and the table row with the
// perspective itself, and the bitmap is read at the coordinates as they
// arrive (the kind of read this GPU does fastest), its texture set to repeat
// or to clamp. Both textures pick the nearest texel, which is the rounding
// down. Only the table lookup is at coordinates worked out here.
static const char direct_vertex_source[] =
    "void main(\n"
    "    float2 aPosition,\n"
    "    float3 aTex,\n"
    "    float2 aRow,\n"
    "    out float4 vPosition : POSITION,\n"
    "    out float2 vTex : TEXCOORD0,\n"
    "    out float vRow : TEXCOORD1)\n"
    "{\n"
    "    vPosition = float4(aPosition, 0.5f * aTex.z, aTex.z);\n"
    "    vTex = aTex.xy;\n"
    "    vRow = aRow.x;\n"
    "}\n";

static const char direct_fragment_source[] =
    "float4 main(\n"
    "    float2 vTex : TEXCOORD0,\n"
    "    float vRow : TEXCOORD1,\n"
    "    uniform sampler2D uBitmap : TEXUNIT0,\n"
    "    uniform sampler2D uTables : TEXUNIT1)\n"
    "{\n"
    "    float pal = tex2D(uBitmap, vTex).x;\n"
    "%s"
    "    float2 lut = float2(pal * (255.0f / 256.0f) + 0.5f / 256.0f, vRow / %d.0f);\n"
    "    float shade = tex2D(uTables, lut).x + 0.25f / 255.0f;\n"
    "    return float4(shade, shade, shade, 1.0f);\n"
    "}\n";

static void gpu_log(const char *format, ...) {
    char path[256];
    va_list args;
    FILE *f;

    snprintf(path, sizeof(path), "%sgpu.txt", VITA_PATH);
    f = fopen(path, "a");
    if (f == NULL)
        return;
    va_start(args, format);
    vfprintf(f, format, args);
    va_end(args);
    fputc('\n', f);
    fclose(f);
}

static void shark_log(const char *msg, shark_log_level level, int line) {
    gpu_log("  shader compiler (%d) line %d: %s", (int)level, line, msg);
}

// ---- memory the GPU can reach --------------------------------------------

static int vgpu_free(void *block);

static void *vgpu_alloc(size_t bytes) {
    size_t size = (bytes + GPU_BLOCK_ALIGN - 1) & ~(size_t)(GPU_BLOCK_ALIGN - 1);
    void *base = NULL;
    SceUID uid;
    int i, err;

    if (context == NULL)
        return NULL; // not set up (yet): memory can't be mapped for the GPU
    for (i = 0; i < MAX_BLOCKS && blocks[i].base != NULL; i++)
        ;
    if (i == MAX_BLOCKS)
        return NULL;
    uid = sceKernelAllocMemBlock("vgpu", SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, size, NULL);
    if (uid < 0) {
        gpu_log("alloc of %u bytes failed: 0x%08x", (unsigned)size, (unsigned)uid);
        return NULL;
    }
    if (sceKernelGetMemBlockBase(uid, &base) < 0 || base == NULL) {
        sceKernelFreeMemBlock(uid);
        return NULL;
    }
    err = sceGxmMapMemory(base, size, SCE_GXM_MEMORY_ATTRIB_RW);
    if (err < 0) {
        gpu_log("map of %u bytes failed: 0x%08x", (unsigned)size, (unsigned)err);
        sceKernelFreeMemBlock(uid);
        return NULL;
    }
    blocks[i].base = base;
    blocks[i].size = size;
    blocks[i].uid = uid;
    return base;
}

static gpu_block *block_of(const void *p) {
    int i;
    for (i = 0; i < MAX_BLOCKS; i++)
        if (blocks[i].base != NULL && (const char *)p >= (const char *)blocks[i].base &&
            (const char *)p < (const char *)blocks[i].base + blocks[i].size)
            return &blocks[i];
    return NULL;
}

static void drop_target(gpu_target *t) {
    void *depth = t->depth;

    if (t->target != NULL) {
        sceGxmFinish(context);
        sceGxmDestroyRenderTarget(t->target);
    }
    memset(t, 0, sizeof(*t));
    if (depth != NULL)
        vgpu_free(depth);
}

static int vgpu_free(void *block) {
    gpu_block *b = block_of(block);

    if (b == NULL)
        return 0;
    sceGxmUnmapMemory(b->base);
    sceKernelFreeMemBlock(b->uid);
    memset(b, 0, sizeof(*b));
    return 1;
}

void vgpu_set_canvases(void *const *pixels, int width, int height, int stride) {
    int i;

    // nothing the GPU still has to do may touch the old ones
    if (context != NULL)
        sceGxmFinish(context);
    for (i = 0; i < VGPU_CANVASES; i++)
        canvas_pixels[i] = pixels != NULL ? pixels[i] : NULL;
    canvas_w = width;
    canvas_h = height;
    canvas_stride = stride;
    if (pixels != NULL)
        gpu_log("%d canvases %dx%d, %d bytes a row, the first at %p", VGPU_CANVASES, width, height, stride, pixels[0]);
}

static int is_canvas(const uchar *bits) {
    int i;
    for (i = 0; i < VGPU_CANVASES; i++)
        if (bits != NULL && canvas_pixels[i] == bits)
            return 1;
    return 0;
}

unsigned char *vgpu_canvas(int width, int height, int row) {
    if (!ready || width > canvas_w || height > canvas_h || row != canvas_stride)
        return NULL;
    // The screen is drawn from a canvas some time after the frame is
    // presented, so each frame takes the canvas used least recently: it is
    // touched again only after two newer frames were handed to the screen,
    // and vita2d lets at most two wait.
    canvas_turn = (canvas_turn + 1) % VGPU_CANVASES;
    return canvas_pixels[canvas_turn];
}

// ---- render targets --------------------------------------------------------

static gpu_target *target_for(int w, int h) {
    SceGxmRenderTargetParams params;
    gpu_target *t = NULL;
    int aligned_w = (w + SCE_GXM_TILE_SIZEX - 1) & ~(SCE_GXM_TILE_SIZEX - 1);
    int aligned_h = (h + SCE_GXM_TILE_SIZEY - 1) & ~(SCE_GXM_TILE_SIZEY - 1);
    int i, err;

    for (i = 0; i < MAX_TARGETS; i++) {
        if (targets[i].target != NULL && targets[i].w == w && targets[i].h == h)
            return &targets[i];
        if (targets[i].target == NULL && t == NULL)
            t = &targets[i];
    }
    if (t == NULL) {
        drop_target(&targets[0]);
        t = &targets[0];
    }

    memset(&params, 0, sizeof(params));
    params.width = (uint16_t)w;
    params.height = (uint16_t)h;
    params.scenesPerFrame = 8;
    params.multisampleMode = SCE_GXM_MULTISAMPLE_NONE;
    params.driverMemBlock = -1;
    err = sceGxmCreateRenderTarget(&params, &t->target);
    if (err < 0) {
        gpu_log("render target %dx%d failed: 0x%08x", w, h, (unsigned)err);
        t->target = NULL;
        return NULL;
    }
    // The list is drawn back to front, without depth tests, but a scene still
    // gets a depth buffer of its own.
    t->depth = vgpu_alloc((size_t)4 * aligned_w * aligned_h);
    if (t->depth == NULL) {
        drop_target(t);
        return NULL;
    }
    err = sceGxmDepthStencilSurfaceInit(&t->depth_stencil, SCE_GXM_DEPTH_STENCIL_FORMAT_S8D24,
                                        SCE_GXM_DEPTH_STENCIL_SURFACE_TILED, aligned_w, t->depth, NULL);
    if (err < 0) {
        gpu_log("depth surface failed: 0x%08x", (unsigned)err);
        drop_target(t);
        return NULL;
    }
    t->w = w;
    t->h = h;
    return t;
}

// ---- drawing ---------------------------------------------------------------

static void refused(const char *why, const uchar *bits, int w, int h, int row, int err) {
    if (refusals_logged++ < 5)
        gpu_log("refused a %dx%d canvas at %p, %d bytes a row: %s (0x%08x)", w, h, (const void *)bits, row, why,
                (unsigned)err);
}

static int vgpu_begin(uchar *bits, int w, int h, int row, const uchar *tables, int rows) {
    gpu_target *t;
    int err;

    if (!ready)
        return 0;
    if (block_of(bits) == NULL && (!is_canvas(bits) || row != canvas_stride || w > canvas_w || h > canvas_h)) {
        // A canvas elsewhere can't be rendered into. Say so, a few times:
        // the caller falls back to the CPU without a word.
        refused("not a GPU canvas", bits, w, h, row, 0);
        return 0;
    }
    t = target_for(w, h);
    if (t == NULL) {
        refused("no render target", bits, w, h, row, 0);
        return 0;
    }
    err = sceGxmColorSurfaceInit(&scene_color, SCE_GXM_COLOR_FORMAT_U8_R, SCE_GXM_COLOR_SURFACE_LINEAR,
                                 SCE_GXM_COLOR_SURFACE_SCALE_NONE, SCE_GXM_OUTPUT_REGISTER_SIZE_32BIT, w, h, row, bits);
    if (err < 0) {
        refused("no 8-bit color surface", bits, w, h, row, err);
        return 0;
    }
    // The last scene has finished (vgpu_end waits), so its tables and
    // bitmaps can be replaced.
    if (tables != NULL && tables_pixels != NULL)
        memcpy(tables_pixels, tables, (size_t)rows * 256);
    err = sceGxmBeginScene(context, 0, t->target, NULL, NULL, NULL, &scene_color, &t->depth_stencil);
    if (err < 0) {
        if (begin_errors_logged++ < 5)
            gpu_log("begin scene failed: 0x%08x", (unsigned)err);
        return 0;
    }
    sceGxmSetCullMode(context, SCE_GXM_CULL_NONE);
    sceGxmSetFrontDepthFunc(context, SCE_GXM_DEPTH_FUNC_ALWAYS);
    sceGxmSetBackDepthFunc(context, SCE_GXM_DEPTH_FUNC_ALWAYS);
    sceGxmSetFrontDepthWriteEnable(context, SCE_GXM_DEPTH_WRITE_DISABLED);
    sceGxmSetBackDepthWriteEnable(context, SCE_GXM_DEPTH_WRITE_DISABLED);

    in_scene = 1;
    // Profile builds alternate the two textured shaders (see vprof.h).
    scene_shader = check_shader >= 0 ? check_shader : vprof_variant == 2 && shaders[SHADER_B].ok ? SHADER_B : SHADER_A;
    vertex_count = index_count = draw_count = 0;
    scene_texture_count = 0;
    texture_heap_used = 0;
    to_clip_x = 2.0f / (float)w;
    to_clip_y = 2.0f / (float)h;
    return 1;
}

// The bitmap where the GPU can read it: copied once per scene. NULL if the
// scene has no room for it.
static const SceGxmTexture *texture_for(const grs_bitmap *bm, int wrap) {
    // a linear texture's rows are a multiple of 8 pixels apart
    size_t stride = ((size_t)bm->w + 7) & ~(size_t)7;
    size_t bytes = (stride * bm->h + SCE_GXM_TEXTURE_ALIGNMENT - 1) & ~(size_t)(SCE_GXM_TEXTURE_ALIGNMENT - 1);
    SceGxmTexture *texture;
    uchar *copy;
    int i, y;

    for (i = 0; i < scene_texture_count; i++)
        if (scene_bitmaps[i].bits == bm->bits && scene_bitmaps[i].w == bm->w && scene_bitmaps[i].h == bm->h)
            return &scene_textures[i];
    if (scene_texture_count == MAX_SCENE_TEXTURES || texture_heap_used + bytes > TEXTURE_HEAP_BYTES)
        return NULL;
    copy = texture_heap + texture_heap_used;
    for (y = 0; y < bm->h; y++)
        memcpy(copy + y * stride, bm->bits + (size_t)y * bm->row, (size_t)bm->w);
    texture = &scene_textures[scene_texture_count];
    if (sceGxmTextureInitLinear(texture, copy, SCE_GXM_TEXTURE_FORMAT_U8_RRRR, bm->w, bm->h, 0) < 0)
        return NULL;
    sceGxmTextureSetMinFilter(texture, SCE_GXM_TEXTURE_FILTER_POINT);
    sceGxmTextureSetMagFilter(texture, SCE_GXM_TEXTURE_FILTER_POINT);
    // shader A wraps the coordinates itself, and to it either mode is the same
    sceGxmTextureSetUAddrMode(texture, wrap ? SCE_GXM_TEXTURE_ADDR_REPEAT : SCE_GXM_TEXTURE_ADDR_CLAMP);
    sceGxmTextureSetVAddrMode(texture, wrap ? SCE_GXM_TEXTURE_ADDR_REPEAT : SCE_GXM_TEXTURE_ADDR_CLAMP);
    scene_bitmaps[scene_texture_count].bits = bm->bits;
    scene_bitmaps[scene_texture_count].w = bm->w;
    scene_bitmaps[scene_texture_count].h = bm->h;
    scene_texture_count++;
    texture_heap_used += bytes;
    vgpu_texture_bytes += bytes;
    return texture;
}

// A convex polygon as a fan of triangles. 0 if the scene has no room for it.
static int add_polygon(const SceGxmTexture *texture, int trans, int n, const vgpu_vertex *verts) {
    unsigned first = vertex_count;
    gpu_draw *d = draw_count ? &draws[draw_count - 1] : NULL;
    int i;

    if (n < 3)
        return 1;
    if (vertex_count + n > MAX_VERTICES || index_count + 3 * (n - 2) > MAX_INDICES)
        return 0;
    if (d == NULL || d->texture != texture || d->trans != trans) {
        if (draw_count == MAX_DRAWS)
            return 0;
        d = &draws[draw_count++];
        d->texture = texture;
        d->trans = trans;
        d->first = index_count;
        d->count = 0;
    }
    memcpy(&vertices[vertex_count], verts, n * sizeof(vgpu_vertex));
    vertex_count += n;
    for (i = 1; i + 1 < n; i++) {
        indices[index_count++] = (uint16_t)first;
        indices[index_count++] = (uint16_t)(first + i);
        indices[index_count++] = (uint16_t)(first + i + 1);
    }
    d->count += 3 * (n - 2);
    return 1;
}

// The software mappers fill the pixels whose integer coordinates are inside
// the polygon; the GPU, those whose centres are. Half a pixel makes them the
// same points.
static void place(vgpu_vertex *out, const rastq_gpu_vertex *in) {
    memset(out, 0, sizeof(*out));
    out->x = (in->x + 0.5f) * to_clip_x - 1.0f;
    out->y = 1.0f - (in->y + 0.5f) * to_clip_y;
}

// A textured vertex for the scene's shader: u and v in texels times q, the
// row as row / q + flat_row, of a bitmap `w` by `h`.
static void fill(vgpu_vertex *out, const rastq_gpu_vertex *in, float u, float v, float q, float row, float flat_row,
                 int w, int h, int wrap) {
    place(out, in);
    if (scene_shader == SHADER_B) {
        float depth = 1.0f / q;
        out->x *= depth;
        out->y *= depth;
        out->u = u * depth / (float)w;
        out->v = v * depth / (float)h;
        out->q = depth;
        out->row = row * depth + flat_row;
        return;
    }
    out->u = u;
    out->v = v;
    out->q = q;
    out->row = row;
    out->flat_row = flat_row;
    out->inv_w = 1.0f / (float)w;
    out->inv_h = 1.0f / (float)h;
    out->wrap = wrap ? 1.0f : 0.0f;
}

static int vgpu_flat(int n, const rastq_gpu_vertex *v, int color) {
    vgpu_vertex out[RASTQ_GPU_VERTS];
    int i;

    if (n > RASTQ_GPU_VERTS)
        return 1;
    for (i = 0; i < n; i++) {
        if (!textured) {
            // The 8-bit target stores round or floor of 255 times the output:
            // a quarter above the index gives the index either way.
            place(&out[i], &v[i]);
            out[i].u = ((float)color + 0.25f) / 255.0f;
        } else {
            // the texel of that value in the tables' unchanged row
            fill(&out[i], &v[i], (float)color + 0.5f, RASTQ_GPU_PLAIN_ROW + 0.5f, 1.0f, RASTQ_GPU_PLAIN_ROW + 0.5f, 0,
                 256, RASTQ_GPU_TABLE_ROWS, 0);
        }
    }
    return add_polygon(&tables_texture, 0, n, out);
}

static int vgpu_tmap(const grs_bitmap *bm, int flags, int n, const rastq_gpu_vertex *v) {
    const SceGxmTexture *texture;
    vgpu_vertex out[RASTQ_GPU_VERTS];
    int i, wrap = (flags & RASTQ_GPU_WRAP) != 0;

    if (!textured) // one colour for the lot: the texel in the middle
        return vgpu_flat(n, v, bm->bits[(size_t)(bm->h / 2) * bm->row + bm->w / 2]);
    if (n > RASTQ_GPU_VERTS)
        return 1;
    texture = texture_for(bm, wrap);
    if (texture == NULL)
        return 0;
    for (i = 0; i < n; i++)
        fill(&out[i], &v[i], v[i].u, v[i].v, v[i].q, v[i].row, v[i].flat_row, bm->w, bm->h, wrap);
    return add_polygon(texture, (flags & RASTQ_GPU_TRANS) != 0, n, out);
}

static void vgpu_end(void) {
    unsigned k;

    if (!in_scene)
        return;
    if (index_count != 0) {
        sceGxmSetVertexStream(context, 0, vertices);
        if (!textured) {
            sceGxmSetVertexProgram(context, flat_vertex_program);
            sceGxmSetFragmentProgram(context, flat_fragment_program);
            sceGxmDraw(context, SCE_GXM_PRIMITIVE_TRIANGLES, SCE_GXM_INDEX_FORMAT_U16, indices, index_count);
        } else {
            const gpu_shader *sh = &shaders[scene_shader];

            sceGxmSetVertexProgram(context, sh->vertex);
            for (k = 0; k < draw_count; k++) {
                const gpu_draw *d = &draws[k];
                sceGxmSetFragmentProgram(context, d->trans ? sh->trans : sh->opaque);
                sceGxmSetFragmentTexture(context, d->trans ? sh->trans_bitmap_unit : sh->bitmap_unit, d->texture);
                sceGxmSetFragmentTexture(context, d->trans ? sh->trans_tables_unit : sh->tables_unit, &tables_texture);
                sceGxmDraw(context, SCE_GXM_PRIMITIVE_TRIANGLES, SCE_GXM_INDEX_FORMAT_U16, indices + d->first,
                           d->count);
            }
        }
    }
    sceGxmEndScene(context, NULL, NULL);
    // the CPU goes on drawing into the canvas: the GPU must be done with it
    sceGxmFinish(context);
    in_scene = 0;
}

// ---- frame pairs for looking at on a PC -------------------------------------

static void write_rows(const char *path, const uchar *pixels, int w, int h, int row) {
    FILE *f = fopen(path, "wb");
    int y;

    if (f == NULL)
        return;
    for (y = 0; y < h; y++)
        fwrite(pixels + (size_t)y * row, 1, (size_t)w, f);
    fclose(f);
}

static void vgpu_compared(const uchar *gpu, const uchar *cpu, int w, int h, int row, unsigned differing) {
    static int comparisons, dumps;
    static unsigned worst;
    char dir[200], path[256];
    FILE *f;

    comparisons++;
    if (dumps >= MAX_DUMPS)
        return;
    if (comparisons != 3 && (differing <= worst || differing <= (unsigned)(DUMP_SHARE * w * h)))
        return;
    if (differing > worst)
        worst = differing;
    dumps++;
    snprintf(dir, sizeof(dir), "%sgpudumps", VITA_PATH);
    sceIoMkdir(dir, 0777);
    snprintf(path, sizeof(path), "%s/%02d-gpu.raw", dir, dumps);
    write_rows(path, gpu, w, h, row);
    snprintf(path, sizeof(path), "%s/%02d-cpu.raw", dir, dumps);
    write_rows(path, cpu, w, h, row);
    snprintf(path, sizeof(path), "%s/%02d-palette.raw", dir, dumps);
    f = fopen(path, "wb");
    if (f != NULL) {
        // 256 colours, 4 bytes each: red, green, blue, alpha
        fwrite(vita2d_texture_get_palette(texBuffer), 4, 256, f);
        fclose(f);
    }
    snprintf(path, sizeof(path), "%s/%02d-info.txt", dir, dumps);
    f = fopen(path, "w");
    if (f != NULL) {
        fprintf(f, "width=%d height=%d comparison=%d differing=%u\n", w, h, comparisons, differing);
        fclose(f);
    }
}

static const rastq_gpu queue_hooks = {vgpu_begin, vgpu_flat, vgpu_tmap, vgpu_end, vgpu_compared};

// ---- set-up ----------------------------------------------------------------

static SceGxmProgram *compile(const char *source, shark_type type, const char *what) {
    uint32_t size = (uint32_t)strlen(source);
    SceGxmProgram *program = shark_compile_shader(source, &size, type);
    SceGxmProgram *copy = NULL;

    if (program == NULL) {
        gpu_log("%s shader did not compile", what);
    } else {
        copy = malloc(size);
        if (copy != NULL)
            memcpy(copy, program, size);
        gpu_log("%s shader compiled: %u bytes", what, (unsigned)size);
    }
    shark_clear_output();
    return copy;
}

static void attribute(SceGxmVertexAttribute *a, const SceGxmProgramParameter *parameter, size_t offset, int count) {
    a->streamIndex = 0;
    a->offset = (uint16_t)offset;
    a->format = SCE_GXM_ATTRIBUTE_FORMAT_F32;
    a->componentCount = (uint8_t)count;
    a->regIndex = (uint16_t)sceGxmProgramParameterGetResourceIndex(parameter);
}

// Registers a vertex and a fragment shader and makes the programs. The
// attributes are named in `names`, `count` of them, at `offsets` with
// `sizes` components; the vertex program is made once, when *vertex_program
// is still NULL.
static int make_programs(const char *what, const char *vertex_source, const char *fragment_source,
                         const char *const *names, const size_t *offsets, const int *sizes, int count,
                         SceGxmVertexProgram **vertex_program, SceGxmFragmentProgram **fragment_program,
                         const SceGxmProgram **fragment_out) {
    static const SceGxmProgram *last_vertex;
    static SceGxmShaderPatcherId last_vertex_id;
    SceGxmShaderPatcher *patcher = vita2d_get_shader_patcher();
    SceGxmProgram *fragment;
    SceGxmShaderPatcherId fragment_id;
    SceGxmVertexAttribute attributes[4];
    SceGxmVertexStream stream;
    char name[64];
    int i, err;

    if (*vertex_program == NULL) {
        SceGxmProgram *vertex;

        snprintf(name, sizeof(name), "%s vertex", what);
        vertex = compile(vertex_source, SHARK_VERTEX_SHADER, name);
        if (vertex == NULL)
            return 0;
        if ((err = sceGxmShaderPatcherRegisterProgram(patcher, vertex, &last_vertex_id)) < 0) {
            gpu_log("registering the %s shader failed: 0x%08x", name, (unsigned)err);
            return 0;
        }
        for (i = 0; i < count; i++) {
            const SceGxmProgramParameter *parameter = sceGxmProgramFindParameterByName(vertex, names[i]);
            if (parameter == NULL) {
                gpu_log("%s shader: no parameter %s", name, names[i]);
                return 0;
            }
            attribute(&attributes[i], parameter, offsets[i], sizes[i]);
        }
        stream.stride = sizeof(vgpu_vertex);
        stream.indexSource = SCE_GXM_INDEX_SOURCE_INDEX_16BIT;
        err = sceGxmShaderPatcherCreateVertexProgram(patcher, last_vertex_id, attributes, count, &stream, 1,
                                                     vertex_program);
        if (err < 0) {
            gpu_log("%s program failed: 0x%08x", name, (unsigned)err);
            *vertex_program = NULL;
            return 0;
        }
        last_vertex = vertex;
    }

    snprintf(name, sizeof(name), "%s fragment", what);
    fragment = compile(fragment_source, SHARK_FRAGMENT_SHADER, name);
    if (fragment == NULL)
        return 0;
    if ((err = sceGxmShaderPatcherRegisterProgram(patcher, fragment, &fragment_id)) < 0) {
        gpu_log("registering the %s shader failed: 0x%08x", name, (unsigned)err);
        return 0;
    }
    err = sceGxmShaderPatcherCreateFragmentProgram(patcher, fragment_id, SCE_GXM_OUTPUT_REGISTER_FORMAT_UCHAR4,
                                                   SCE_GXM_MULTISAMPLE_NONE, NULL, last_vertex, fragment_program);
    if (err < 0) {
        gpu_log("%s program failed: 0x%08x", name, (unsigned)err);
        *fragment_program = NULL;
        return 0;
    }
    if (fragment_out != NULL)
        *fragment_out = fragment;
    return 1;
}

static int make_flat_programs(void) {
    static const char *const names[] = {"aPosition", "aIndex"};
    static const size_t offsets[] = {offsetof(vgpu_vertex, x), offsetof(vgpu_vertex, u)};
    static const int sizes[] = {2, 1};

    return make_programs("flat", flat_vertex_source, flat_fragment_source, names, offsets, sizes, 2,
                         &flat_vertex_program, &flat_fragment_program, NULL);
}

// Where a fragment shader wants a texture: -1 if it has no such sampler.
static int sampler_unit(const SceGxmProgram *fragment, const char *name) {
    const SceGxmProgramParameter *parameter = sceGxmProgramFindParameterByName(fragment, name);
    return parameter != NULL ? (int)sceGxmProgramParameterGetResourceIndex(parameter) : -1;
}

// Compiles one set of textured shaders.
static int make_shader(int which) {
    static const char *const names[] = {"aPosition", "aTex", "aRow", "aParams"};
    static const size_t offsets[] = {offsetof(vgpu_vertex, x), offsetof(vgpu_vertex, u), offsetof(vgpu_vertex, row),
                                     offsetof(vgpu_vertex, inv_w)};
    static const int sizes[] = {2, 3, 2, 4};
    static char source[2048];
    const char *vertex_source = which == SHADER_A ? tmap_vertex_source : direct_vertex_source;
    const char *fragment_source = which == SHADER_A ? tmap_fragment_source : direct_fragment_source;
    const char *label = which == SHADER_A ? "shader A" : "shader B";
    int attributes = which == SHADER_A ? 4 : 3; // B has no use for the bitmap's size
    gpu_shader *sh = &shaders[which];
    const SceGxmProgram *opaque = NULL, *trans = NULL;
    char name[48];
    int units[4];

    snprintf(source, sizeof(source), fragment_source, "", RASTQ_GPU_TABLE_ROWS);
    if (!make_programs(label, vertex_source, source, names, offsets, sizes, attributes, &sh->vertex, &sh->opaque,
                       &opaque))
        return 0;
    snprintf(source, sizeof(source), fragment_source, trans_fragment_lines, RASTQ_GPU_TABLE_ROWS);
    snprintf(name, sizeof(name), "%s, transparent", label);
    if (!make_programs(name, vertex_source, source, names, offsets, sizes, attributes, &sh->vertex, &sh->trans,
                       &trans))
        return 0;
    units[0] = sampler_unit(opaque, "uBitmap");
    units[1] = sampler_unit(opaque, "uTables");
    units[2] = sampler_unit(trans, "uBitmap");
    units[3] = sampler_unit(trans, "uTables");
    gpu_log("%s texture units: bitmap %d, tables %d; transparent: bitmap %d, tables %d", label, units[0], units[1],
            units[2], units[3]);
    if (units[0] < 0 || units[1] < 0 || units[2] < 0 || units[3] < 0)
        return 0;
    sh->bitmap_unit = (unsigned)units[0];
    sh->tables_unit = (unsigned)units[1];
    sh->trans_bitmap_unit = (unsigned)units[2];
    sh->trans_tables_unit = (unsigned)units[3];
    return 1;
}

// The memory the textured shaders draw from: the tables and the bitmaps.
static int make_textures(void) {
    tables_pixels = vgpu_alloc(RASTQ_GPU_TABLE_ROWS * 256);
    texture_heap = vgpu_alloc(TEXTURE_HEAP_BYTES);
    if (tables_pixels == NULL || texture_heap == NULL) {
        gpu_log("no memory for the tables and the bitmaps");
        return 0;
    }
    memset(tables_pixels, 0, RASTQ_GPU_TABLE_ROWS * 256);
    if (sceGxmTextureInitLinear(&tables_texture, tables_pixels, SCE_GXM_TEXTURE_FORMAT_U8_RRRR, 256,
                                RASTQ_GPU_TABLE_ROWS, 0) < 0)
        return 0;
    sceGxmTextureSetMinFilter(&tables_texture, SCE_GXM_TEXTURE_FILTER_POINT);
    sceGxmTextureSetMagFilter(&tables_texture, SCE_GXM_TEXTURE_FILTER_POINT);
    sceGxmTextureSetUAddrMode(&tables_texture, SCE_GXM_TEXTURE_ADDR_CLAMP);
    sceGxmTextureSetVAddrMode(&tables_texture, SCE_GXM_TEXTURE_ADDR_CLAMP);
    return 1;
}

// A rectangle of the check canvas, columns [x0, x1) and rows [y0, y1).
static void corners(rastq_gpu_vertex v[4], int x0, int x1, int y0, int y1) {
    memset(v, 0, 4 * sizeof(v[0]));
    v[0].x = v[3].x = (float)x0;
    v[1].x = v[2].x = (float)x1;
    v[0].y = v[1].y = (float)y0;
    v[2].y = v[3].y = (float)y1;
}

static void quad(int x0, int x1, int color) {
    rastq_gpu_vertex v[4];
    corners(v, x0, x1, 0, CHECK_H);
    vgpu_flat(4, v, color);
}

// Draws one column per palette index and reads it back, then draws over the
// left half only and looks at what is left of the right half. Returns the
// number of wrong indices, or -1 if nothing could be drawn.
static int index_check(int *first_bad, int *got, int *kept) {
    uchar *canvas = vgpu_alloc(CHECK_W * CHECK_H);
    int k, bad = 0;

    *first_bad = *got = -1;
    *kept = 0;
    if (canvas == NULL)
        return -1;
    memset(canvas, 0xEE, CHECK_W * CHECK_H);
    if (!vgpu_begin(canvas, CHECK_W, CHECK_H, CHECK_W, NULL, 0)) {
        vgpu_free(canvas);
        return -1;
    }
    for (k = 0; k < 256; k++)
        quad(k, k + 1, k);
    vgpu_end();
    for (k = 0; k < 256; k++) {
        int value = canvas[(CHECK_H / 2) * CHECK_W + k];
        if (value != k) {
            if (bad++ == 0) {
                *first_bad = k;
                *got = value;
            }
        }
    }

    // Does a scene keep the pixels it doesn't draw? A view is drawn in
    // several scenes when something in it has to be drawn by the CPU.
    memset(canvas, 0x55, CHECK_W * CHECK_H);
    if (vgpu_begin(canvas, CHECK_W, CHECK_H, CHECK_W, NULL, 0)) {
        quad(0, CHECK_W / 2, 0x11);
        vgpu_end();
        *kept = canvas[(CHECK_H / 2) * CHECK_W + CHECK_W - 8] == 0x55 && canvas[(CHECK_H / 2) * CHECK_W + 8] == 0x11;
    }
    vgpu_free(canvas);
    return bad;
}

// One set of textured shaders, on four bands of the check canvas, each drawn
// so that column x holds a value that is known:
//   plain: texel x of a row of 256, unchanged
//   table: the same through a table of the scene
//   light: the same through the light table, the level rising along the row
//   wrap:  the first row of a 16x16 bitmap with a transparent column,
//          repeated 16 times, and through the perspective division
// Sets the number of wrong columns of each band. Returns 0 if nothing could
// be drawn.
#define CHECK_BANDS 4
static int texture_check(int shader, int bad[CHECK_BANDS]) {
    enum { TABLE_ROW = RASTQ_GPU_PLAIN_ROW + 1, ROWS = RASTQ_GPU_PLAIN_ROW + 2, BAND_H = CHECK_H / CHECK_BANDS };
    static uchar ramp[8 * 256], tiles[16 * 16], tables[ROWS * 256];
    uchar *canvas = vgpu_alloc(CHECK_W * CHECK_H);
    rastq_gpu_vertex v[4];
    grs_bitmap ramp_bm, tiles_bm;
    int band, x, i, k;

    for (band = 0; band < CHECK_BANDS; band++)
        bad[band] = -1;
    if (canvas == NULL)
        return 0;
    for (i = 0; i < 256; i++) {
        for (k = 0; k < 8; k++)
            ramp[k * 256 + i] = (uchar)i;
        tiles[i] = (i & 15) == 5 ? 0 : (uchar)i;
        for (k = 0; k < RASTQ_GPU_LIGHT_ROWS; k++)
            tables[k * 256 + i] = (uchar)(i * 3 + k * 17 + 1);
        tables[RASTQ_GPU_PLAIN_ROW * 256 + i] = (uchar)i;
        tables[TABLE_ROW * 256 + i] = (uchar)(255 - i);
    }
    memset(&ramp_bm, 0, sizeof(ramp_bm));
    ramp_bm.bits = ramp;
    ramp_bm.w = ramp_bm.row = 256;
    ramp_bm.h = 8;
    tiles_bm = ramp_bm;
    tiles_bm.bits = tiles;
    tiles_bm.w = tiles_bm.row = tiles_bm.h = 16;

    memset(canvas, 0xEE, CHECK_W * CHECK_H);
    check_shader = shader;
    if (!vgpu_begin(canvas, CHECK_W, CHECK_H, CHECK_W, tables, ROWS)) {
        check_shader = -1;
        vgpu_free(canvas);
        return 0;
    }
    for (band = 0; band < CHECK_BANDS; band++) {
        float q = band == 3 ? 0.5f : 1.0f;

        corners(v, 0, CHECK_W, band * BAND_H, (band + 1) * BAND_H);
        for (i = 0; i < 4; i++) {
            // the middle of texel x in column x, and of row 0
            v[i].u = (v[i].x + 0.5f) * q;
            v[i].v = 0.5f * q;
            v[i].q = q;
            v[i].row = (band == 1 ? TABLE_ROW + 0.5f : band == 2 ? (v[i].x + 0.5f) / 16.0f : RASTQ_GPU_PLAIN_ROW + 0.5f) * q;
        }
        if (band < 3)
            vgpu_tmap(&ramp_bm, 0, 4, v);
        else
            vgpu_tmap(&tiles_bm, RASTQ_GPU_TRANS | RASTQ_GPU_WRAP, 4, v);
    }
    vgpu_end();
    check_shader = -1;

    for (band = 0; band < CHECK_BANDS; band++) {
        const uchar *row = canvas + (band * BAND_H + BAND_H / 2) * CHECK_W;

        bad[band] = 0;
        for (x = 0; x < 256; x++) {
            int want = band == 0   ? x
                       : band == 1 ? tables[TABLE_ROW * 256 + x]
                       : band == 2 ? tables[(x / 16) * 256 + x]
                       : tiles[x & 15] == 0 ? 0xEE // texel 0: left as it was
                                            : tiles[x & 15];
            if (row[x] != want) {
                if (bad[band]++ == 0)
                    gpu_log("texture check, shader %c, band %d: column %d holds %d, not %d", 'A' + shader, band, x,
                            row[x], want);
            }
        }
    }
    vgpu_free(canvas);
    return 1;
}

// How long the CPU takes to write and to read half a megabyte, in
// microseconds: a view's canvas is about that size.
static void time_memory(uchar *p, size_t size, int *write_us, int *read_us) {
    static volatile unsigned sum; // so that the reads can't be left out
    long long t0 = sceKernelGetProcessTimeWide(), t1, t2;
    const uint32_t *words = (const uint32_t *)p;
    unsigned total = 0;
    size_t at;

    memset(p, 0x5a, size);
    t1 = sceKernelGetProcessTimeWide();
    for (at = 0; at < size / 4; at++)
        total += words[at];
    sum = total;
    t2 = sceKernelGetProcessTimeWide();
    *write_us = (int)(t1 - t0);
    *read_us = (int)(t2 - t1);
}

void vgpu_init(void) {
    size_t test_bytes = 2 * GPU_BLOCK_ALIGN;
    int bad, first_bad, got, kept;
    int tex_bad[SHADERS][CHECK_BANDS] = {{-1, -1, -1, -1}, {-1, -1, -1, -1}};
    int gpu_write = -1, gpu_read = -1, ram_write = -1, ram_read = -1;
    uchar *gpu_mem, *ram_mem;
    int err, which;

    gpu_log("---- start");
    context = vita2d_get_context();

    // Each stage is logged before it starts, so that if one of them takes the
    // game down, gpu.txt says which.
    gpu_log("loading the shader compiler");
    shark_install_log_cb(shark_log);
    err = shark_init(SHACCCG_PATH);
    if (err < 0) {
        gpu_log("no shader compiler: 0x%08x (is %s there?)", (unsigned)err, SHACCCG_PATH);
        snprintf(report, sizeof(report), "gpu: no shader compiler (0x%08x)", (unsigned)err);
        return;
    }
    gpu_log("compiling the flat shaders");
    if (!make_flat_programs()) {
        shark_end();
        snprintf(report, sizeof(report), "gpu: shaders failed, see gpu.txt");
        return;
    }

    vertices = vgpu_alloc(MAX_VERTICES * sizeof(vgpu_vertex) + MAX_INDICES * sizeof(uint16_t));
    if (vertices == NULL) {
        shark_end();
        snprintf(report, sizeof(report), "gpu: no memory for vertices");
        return;
    }
    indices = (uint16_t *)(vertices + MAX_VERTICES);
    ready = 1;

    gpu_log("drawing the index check");
    bad = index_check(&first_bad, &got, &kept);
    gpu_log("index check drawn: %d wrong", bad);

    // Without shader A everything is drawn in flat colours, as in the first
    // GPU builds; without shader B, or if it draws its patterns wrong (it
    // counts on the textures repeating), A takes its place.
    gpu_log("compiling the textured shaders");
    if (bad >= 0 && make_textures()) {
        shaders[SHADER_A].ok = make_shader(SHADER_A);
        shaders[SHADER_B].ok = make_shader(SHADER_B);
    }
    shark_end();
    for (which = 0; which < SHADERS; which++) {
        int *wrong = tex_bad[which];

        if (!shaders[which].ok)
            continue;
        gpu_log("drawing the texture check with shader %c", 'A' + which);
        textured = 1; // for the drawing of the check itself
        if (!texture_check(which, wrong))
            shaders[which].ok = 0;
        gpu_log("texture check drawn: %d %d %d %d wrong", wrong[0], wrong[1], wrong[2], wrong[3]);
        if (which == SHADER_B && (wrong[0] || wrong[1] || wrong[2] || wrong[3]))
            shaders[which].ok = 0;
    }
    textured = shaders[SHADER_A].ok;

    gpu_log("timing memory");
    gpu_mem = vgpu_alloc(test_bytes);
    ram_mem = malloc(test_bytes);
    if (gpu_mem != NULL)
        time_memory(gpu_mem, test_bytes, &gpu_write, &gpu_read);
    if (ram_mem != NULL)
        time_memory(ram_mem, test_bytes, &ram_write, &ram_read);
    if (gpu_mem != NULL)
        vgpu_free(gpu_mem);
    free(ram_mem);

    snprintf(report, sizeof(report),
             "gpu: ready index_check mismatches=%d/256 first_bad=%d got=%d undrawn_kept=%d | "
             "shader_a=%s check plain=%d table=%d light=%d wrap=%d | "
             "shader_b=%s check plain=%d table=%d light=%d wrap=%d (wrong columns of 256) | 512KB us: "
             "gpu_write=%d gpu_read=%d ram_write=%d ram_read=%d",
             bad, first_bad, got, kept, textured ? "on" : "off (flat colours)", tex_bad[0][0], tex_bad[0][1],
             tex_bad[0][2], tex_bad[0][3], shaders[SHADER_B].ok ? "on" : "off (shader A instead)", tex_bad[1][0],
             tex_bad[1][1], tex_bad[1][2], tex_bad[1][3], gpu_write, gpu_read, ram_write, ram_read);
    gpu_log("%s", report);
    if (bad < 0) {
        ready = 0;
        return;
    }
    rastq_set_gpu(&queue_hooks);
}

const char *vgpu_report(void) { return report; }

#endif // VITA && VITA_PROFILE
