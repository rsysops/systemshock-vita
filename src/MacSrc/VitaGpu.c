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

#define SHACCCG_PATH "ur0:data/libshacccg.suprx"
#define GPU_BLOCK_ALIGN (256 * 1024) // CDRAM blocks come in multiples of this
#define MAX_BLOCKS 8
#define MAX_TARGETS 2
#define MAX_VERTICES 16384
#define MAX_INDICES 49152
// Start-up check canvas
#define CHECK_W 256
#define CHECK_H 32
// Frame pairs written for looking at on a PC: the 3rd comparison, and those
// that differ by more than this many pixels, up to MAX_DUMPS in all. Writing
// one stops the game for a second or two.
#define DUMP_DIFFERING 1000
#define MAX_DUMPS 5

typedef struct {
    float x, y; // clip space
    float index; // palette index, as the 0..1 value the 8-bit target stores
} vgpu_vertex;

typedef struct {
    void *base;
    size_t size;
    SceUID uid;
} gpu_block;

typedef struct {
    uchar *bits;
    int w, h, row;
    SceGxmRenderTarget *target;
    SceGxmColorSurface color;
    SceGxmDepthStencilSurface depth_stencil;
    void *depth;
} gpu_target;

static SceGxmContext *context;
static SceGxmVertexProgram *flat_vertex_program;
static SceGxmFragmentProgram *flat_fragment_program;

// The views' canvas: see vgpu_set_canvas
static uchar *canvas_pixels;
static int canvas_w, canvas_h, canvas_stride;

static gpu_block blocks[MAX_BLOCKS];
static gpu_target targets[MAX_TARGETS];
static gpu_target *scene; // the target of the scene under way

static vgpu_vertex *vertices;
static uint16_t *indices;
static unsigned vertex_count, index_count;
static float to_clip_x, to_clip_y;

static int ready;
static int begin_errors_logged, refusals_logged;
static char report[256] = "gpu: not initialized";

extern vita2d_texture *texBuffer;

// The vertex shader takes positions already in clip space. The fragment
// shader writes the palette index to the 8-bit target.
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
    gpu_block *depth;

    if (t->target != NULL) {
        sceGxmFinish(context);
        sceGxmDestroyRenderTarget(t->target);
    }
    depth = t->depth != NULL ? block_of(t->depth) : NULL;
    memset(t, 0, sizeof(*t));
    if (depth != NULL)
        vgpu_free(depth->base);
}

static int vgpu_free(void *block) {
    gpu_block *b = block_of(block);
    int i;

    if (b == NULL)
        return 0;
    // a view's canvas goes: so do the render targets laid over it
    for (i = 0; i < MAX_TARGETS; i++)
        if (targets[i].bits != NULL && block_of(targets[i].bits) == b)
            drop_target(&targets[i]);
    sceGxmUnmapMemory(b->base);
    sceKernelFreeMemBlock(b->uid);
    memset(b, 0, sizeof(*b));
    return 1;
}

void vgpu_set_canvas(void *pixels, int width, int height, int stride) {
    int i;

    // render targets laid over the old canvas go with it
    for (i = 0; i < MAX_TARGETS; i++)
        if (targets[i].bits != NULL && targets[i].bits == canvas_pixels)
            drop_target(&targets[i]);
    canvas_pixels = pixels;
    canvas_w = width;
    canvas_h = height;
    canvas_stride = stride;
    gpu_log("canvas %dx%d, %d bytes a row, at %p", width, height, stride, pixels);
}

unsigned char *vgpu_canvas(int width, int height, int row) {
    if (!ready || canvas_pixels == NULL || width > canvas_w || height > canvas_h || row != canvas_stride)
        return NULL;
    return canvas_pixels;
}

// ---- render targets --------------------------------------------------------

static gpu_target *target_for(uchar *bits, int w, int h, int row) {
    SceGxmRenderTargetParams params;
    gpu_target *t = NULL;
    int aligned_w = (w + SCE_GXM_TILE_SIZEX - 1) & ~(SCE_GXM_TILE_SIZEX - 1);
    int aligned_h = (h + SCE_GXM_TILE_SIZEY - 1) & ~(SCE_GXM_TILE_SIZEY - 1);
    int i, err;

    for (i = 0; i < MAX_TARGETS; i++) {
        if (targets[i].bits == bits && targets[i].w == w && targets[i].h == h && targets[i].row == row)
            return &targets[i];
        if (targets[i].bits == NULL && t == NULL)
            t = &targets[i];
    }
    if (t == NULL) {
        drop_target(&targets[0]);
        t = &targets[0];
    }

    memset(&params, 0, sizeof(params));
    params.width = (uint16_t)w;
    params.height = (uint16_t)h;
    params.scenesPerFrame = 4;
    params.multisampleMode = SCE_GXM_MULTISAMPLE_NONE;
    params.driverMemBlock = -1;
    err = sceGxmCreateRenderTarget(&params, &t->target);
    if (err < 0) {
        gpu_log("render target %dx%d failed: 0x%08x", w, h, (unsigned)err);
        t->target = NULL;
        return NULL;
    }
    err = sceGxmColorSurfaceInit(&t->color, SCE_GXM_COLOR_FORMAT_U8_R, SCE_GXM_COLOR_SURFACE_LINEAR,
                                 SCE_GXM_COLOR_SURFACE_SCALE_NONE, SCE_GXM_OUTPUT_REGISTER_SIZE_32BIT, w, h, row, bits);
    if (err < 0) {
        gpu_log("8-bit color surface %dx%d stride %d failed: 0x%08x", w, h, row, (unsigned)err);
        drop_target(t);
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
    t->bits = bits;
    t->w = w;
    t->h = h;
    t->row = row;
    return t;
}

// ---- drawing ---------------------------------------------------------------

static int vgpu_begin(uchar *bits, int w, int h, int row) {
    gpu_target *t;
    int err;

    if (!ready)
        return 0;
    if (block_of(bits) == NULL && (bits != canvas_pixels || row != canvas_stride || w > canvas_w || h > canvas_h)) {
        // A canvas elsewhere can't be rendered into. Say so, a few times:
        // the caller falls back to the CPU without a word.
        if (refusals_logged++ < 5)
            gpu_log("refused a %dx%d canvas at %p, %d bytes a row: not the GPU canvas", w, h, (void *)bits, row);
        return 0;
    }
    t = target_for(bits, w, h, row);
    if (t == NULL) {
        if (refusals_logged++ < 5)
            gpu_log("refused a %dx%d canvas at %p: no render target", w, h, (void *)bits);
        return 0;
    }
    err = sceGxmBeginScene(context, 0, t->target, NULL, NULL, NULL, &t->color, &t->depth_stencil);
    if (err < 0) {
        if (begin_errors_logged++ < 5)
            gpu_log("begin scene failed: 0x%08x", (unsigned)err);
        return 0;
    }
    sceGxmSetVertexProgram(context, flat_vertex_program);
    sceGxmSetFragmentProgram(context, flat_fragment_program);
    sceGxmSetCullMode(context, SCE_GXM_CULL_NONE);
    sceGxmSetFrontDepthFunc(context, SCE_GXM_DEPTH_FUNC_ALWAYS);
    sceGxmSetBackDepthFunc(context, SCE_GXM_DEPTH_FUNC_ALWAYS);
    sceGxmSetFrontDepthWriteEnable(context, SCE_GXM_DEPTH_WRITE_DISABLED);
    sceGxmSetBackDepthWriteEnable(context, SCE_GXM_DEPTH_WRITE_DISABLED);

    scene = t;
    vertex_count = index_count = 0;
    to_clip_x = 2.0f / (float)w;
    to_clip_y = 2.0f / (float)h;
    return 1;
}

// A convex polygon in canvas pixels, as a fan of triangles.
static void add_polygon(int n, const float *xy, int color) {
    // The 8-bit target stores round or floor of 255 times the output: a
    // quarter above the index gives the index either way.
    float index = ((float)color + 0.25f) / 255.0f;
    unsigned first = vertex_count;
    int i;

    if (n < 3 || vertex_count + n > MAX_VERTICES || index_count + 3 * (n - 2) > MAX_INDICES)
        return;
    for (i = 0; i < n; i++) {
        vgpu_vertex *v = &vertices[vertex_count++];
        v->x = xy[2 * i] * to_clip_x - 1.0f;
        v->y = 1.0f - xy[2 * i + 1] * to_clip_y;
        v->index = index;
    }
    for (i = 1; i + 1 < n; i++) {
        indices[index_count++] = (uint16_t)first;
        indices[index_count++] = (uint16_t)(first + i);
        indices[index_count++] = (uint16_t)(first + i + 1);
    }
}

static void vgpu_flat_poly(int n, const grs_vertex *verts, int color) {
    float xy[2 * 16];
    int i;

    if (n > 16)
        return;
    // The software mappers fill the pixels whose integer coordinates are
    // inside the polygon; the GPU, those whose centres are. Half a pixel
    // makes them the same points.
    for (i = 0; i < n; i++) {
        xy[2 * i] = (float)verts[i].x / 65536.0f + 0.5f;
        xy[2 * i + 1] = (float)verts[i].y / 65536.0f + 0.5f;
    }
    add_polygon(n, xy, color);
}

static void vgpu_end(void) {
    if (scene == NULL)
        return;
    if (index_count != 0) {
        sceGxmSetVertexStream(context, 0, vertices);
        sceGxmDraw(context, SCE_GXM_PRIMITIVE_TRIANGLES, SCE_GXM_INDEX_FORMAT_U16, indices, index_count);
    }
    sceGxmEndScene(context, NULL, NULL);
    // the CPU goes on drawing into the canvas: the GPU must be done with it
    sceGxmFinish(context);
    scene = NULL;
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
    char dir[200], path[256];
    FILE *f;

    comparisons++;
    if (dumps >= MAX_DUMPS || (comparisons != 3 && differing <= DUMP_DIFFERING))
        return;
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

static const rastq_gpu queue_hooks = {vgpu_begin, vgpu_flat_poly, vgpu_end, vgpu_compared};

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

static int make_programs(void) {
    SceGxmShaderPatcher *patcher = vita2d_get_shader_patcher();
    SceGxmProgram *vertex = compile(flat_vertex_source, SHARK_VERTEX_SHADER, "flat vertex");
    SceGxmProgram *fragment = compile(flat_fragment_source, SHARK_FRAGMENT_SHADER, "flat fragment");
    SceGxmShaderPatcherId vertex_id, fragment_id;
    const SceGxmProgramParameter *position, *index;
    SceGxmVertexAttribute attributes[2];
    SceGxmVertexStream stream;
    int err;

    if (vertex == NULL || fragment == NULL)
        return 0;
    if ((err = sceGxmShaderPatcherRegisterProgram(patcher, vertex, &vertex_id)) < 0 ||
        (err = sceGxmShaderPatcherRegisterProgram(patcher, fragment, &fragment_id)) < 0) {
        gpu_log("registering the shaders failed: 0x%08x", (unsigned)err);
        return 0;
    }
    position = sceGxmProgramFindParameterByName(vertex, "aPosition");
    index = sceGxmProgramFindParameterByName(vertex, "aIndex");
    if (position == NULL || index == NULL) {
        gpu_log("vertex shader parameters not found");
        return 0;
    }
    attributes[0].streamIndex = 0;
    attributes[0].offset = offsetof(vgpu_vertex, x);
    attributes[0].format = SCE_GXM_ATTRIBUTE_FORMAT_F32;
    attributes[0].componentCount = 2;
    attributes[0].regIndex = (uint16_t)sceGxmProgramParameterGetResourceIndex(position);
    attributes[1].streamIndex = 0;
    attributes[1].offset = offsetof(vgpu_vertex, index);
    attributes[1].format = SCE_GXM_ATTRIBUTE_FORMAT_F32;
    attributes[1].componentCount = 1;
    attributes[1].regIndex = (uint16_t)sceGxmProgramParameterGetResourceIndex(index);
    stream.stride = sizeof(vgpu_vertex);
    stream.indexSource = SCE_GXM_INDEX_SOURCE_INDEX_16BIT;

    err = sceGxmShaderPatcherCreateVertexProgram(patcher, vertex_id, attributes, 2, &stream, 1, &flat_vertex_program);
    if (err < 0) {
        gpu_log("vertex program failed: 0x%08x", (unsigned)err);
        return 0;
    }
    err = sceGxmShaderPatcherCreateFragmentProgram(patcher, fragment_id, SCE_GXM_OUTPUT_REGISTER_FORMAT_UCHAR4,
                                                   SCE_GXM_MULTISAMPLE_NONE, NULL, vertex, &flat_fragment_program);
    if (err < 0) {
        gpu_log("fragment program failed: 0x%08x", (unsigned)err);
        return 0;
    }
    return 1;
}

static void quad(int x0, int x1, int color) {
    float xy[8] = {(float)x0, 0.0f, (float)x1, 0.0f, (float)x1, (float)CHECK_H, (float)x0, (float)CHECK_H};
    add_polygon(4, xy, color);
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
    if (!vgpu_begin(canvas, CHECK_W, CHECK_H, CHECK_W)) {
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
    if (vgpu_begin(canvas, CHECK_W, CHECK_H, CHECK_W)) {
        quad(0, CHECK_W / 2, 0x11);
        vgpu_end();
        *kept = canvas[(CHECK_H / 2) * CHECK_W + CHECK_W - 8] == 0x55 && canvas[(CHECK_H / 2) * CHECK_W + 8] == 0x11;
    }
    vgpu_free(canvas);
    return bad;
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
    int gpu_write = -1, gpu_read = -1, ram_write = -1, ram_read = -1;
    uchar *gpu_mem, *ram_mem;
    int err;

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
    gpu_log("compiling the shaders");
    if (!make_programs()) {
        shark_end();
        snprintf(report, sizeof(report), "gpu: shaders failed, see gpu.txt");
        return;
    }
    shark_end();

    vertices = vgpu_alloc(MAX_VERTICES * sizeof(vgpu_vertex) + MAX_INDICES * sizeof(uint16_t));
    if (vertices == NULL) {
        snprintf(report, sizeof(report), "gpu: no memory for vertices");
        return;
    }
    indices = (uint16_t *)(vertices + MAX_VERTICES);
    ready = 1;

    gpu_log("drawing the index check");
    bad = index_check(&first_bad, &got, &kept);
    gpu_log("index check drawn: %d wrong; timing memory", bad);

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
             "gpu: ready index_check mismatches=%d/256 first_bad=%d got=%d undrawn_kept=%d | 512KB us: "
             "gpu_write=%d gpu_read=%d ram_write=%d ram_read=%d",
             bad, first_bad, got, kept, gpu_write, gpu_read, ram_write, ram_read);
    gpu_log("%s", report);
    if (bad < 0) {
        ready = 0;
        return;
    }
    rastq_set_gpu(&queue_hooks);
}

const char *vgpu_report(void) { return report; }

#endif // VITA && VITA_PROFILE
