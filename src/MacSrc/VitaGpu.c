// The Vita's GPU as a filler for the rasterizer queue: see VitaGpu.h.

#include <psp2/gxm.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <SDL.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vita2d.h>
#include <vitashark.h>

#include "ShadeLut.h"
#include "Shock.h"
#include "VitaGpu.h"
#include "rastq.h"
#include "tlucdat.h"
#include "vprof.h"

#define SHACCCG_PATH "ur0:data/libshacccg.suprx"
#define GPU_BLOCK_ALIGN (256 * 1024) // CDRAM blocks come in multiples of this
#define MAX_BLOCKS 16
// The canvases' size and the start-up checks', each for the canvas and for
// its side buffer (see target_for)
#define MAX_TARGETS 4
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
// this share of them, up to MAX_DUMPS in all. The game only copies one (some
// tens of ms, in a frame the timings leave out); a thread of its own writes
// it to the card.
#define DUMP_SHARE 0.05
#define MAX_DUMPS 5
#define DUMP_APART_US 30000000
// What a smooth scene's textured shader writes in alpha where the light is one
// of the light table's levels; every other shader writes 255 (docs/SHADOWS.md)
#define SIDE_LIT 0x5A
// The Vita's screen, which vgpu_draw_view draws on
#define SCREEN_W 960
#define SCREEN_H 544
// The smooth shadows' tables of colours (ShadeLut.h), in turn: one is made
// while the screen may still be drawing from the two before
#define COLOUR_TABLES 3
#define COLOUR_TABLE_BYTES (SHADE_ROWS * 256 * 4)

// What the vertex shaders get.
// - The textured shader leaves the perspective division to the GPU: x and y
//   times w, u and v as shares of the bitmap, and w. The five light values
//   of rastq.h, which are linear on screen, are multiplied by w too: the
//   hardware's interpolation then gives each of them times the pixel's w,
//   and the quotients the shader takes are the same.
// - The flat-colour shaders read the position and, where the other has u,
//   the palette index as the 0..1 value the 8-bit target stores.
// - The shaded shaders read the position, red where the light values are,
//   and green and blue where u, v, w and the depth are.
typedef struct {
    float x, y;
    float u, v, w;
    float left, span, along, width;
    float depth;
} vgpu_vertex;

typedef struct {
    void *base;
    size_t size;
    SceUID uid;
} gpu_block;

// A size of canvas the GPU has drawn into, for the canvas or for its side
// buffer
typedef struct {
    int w, h;
    int side;
    SceGxmRenderTarget *target;
    SceGxmDepthStencilSurface depth_stencil;
    void *depth;
} gpu_target;

// Which shaders draw a polygon
enum {
    DRAW_TMAP,   // a bitmap through the tables
    DRAW_TRANS,  // the same, texel 0 leaving the pixel alone
    DRAW_LIT,    // DRAW_TMAP through the light table: the side shader in a smooth scene
    DRAW_LIT_TRANS, // the same, texel 0 leaving the pixel alone
    DRAW_FLAT,   // one palette index
    DRAW_SHADED, // between colours, through the colours' table
    DRAW_TLUC,   // what the canvas holds, through a row of the tables
    DRAW_TLUCBM, // a translucent bitmap: its texel and what the canvas holds, through the matrix and the tables
    DRAW_TLUCBM_TRANS, // the same, texel 0 leaving the pixel alone
    DRAW_IMPORT_CLEAR, // smooth: the 32-bit buffer made the canvas, nothing lit
    DRAW_IMPORT_KEEP,  // smooth: the same where the CPU drew since the last scene, the rest kept
};

// Consecutive polygons with the same bitmap and shaders
typedef struct {
    const SceGxmTexture *texture;
    int kind;
    unsigned first, count; // indices
} gpu_draw;

static SceGxmContext *context;
static SceGxmVertexProgram *flat_vertex_program;
static SceGxmFragmentProgram *flat_fragment_program;
static SceGxmVertexProgram *tmap_vertex_program;
static SceGxmFragmentProgram *tmap_fragment_program, *trans_fragment_program; // trans: texel 0 leaves the pixel alone
static unsigned bitmap_unit, tables_unit, trans_bitmap_unit, trans_tables_unit;
static SceGxmVertexProgram *shaded_vertex_program;
static SceGxmFragmentProgram *shaded_fragment_program;
static unsigned colours_unit;
// The translucent shaders: a fragment shader for each way the pixel under
// the one being drawn may come and go (see tluc_ways). The start-up check
// picks the first that draws right.
#define TLUC_WAYS 2
static SceGxmVertexProgram *tluc_vertex_program;
static SceGxmFragmentProgram *tluc_fragment_programs[TLUC_WAYS];
static int tluc_units[TLUC_WAYS];
static int tluc_way = -1; // the one in use; -1: none, the CPU draws those
// A translucent bitmap's shaders, with the textured ones' vertex program, and
// the matrix they look up (see rastq.h, tluc_tmap)
static SceGxmFragmentProgram *tlucbm_fragment_program, *tlucbm_trans_fragment_program;
static int tlucbm_units[2][3]; // opaque and transparent: bitmap, tables, matrix
static uchar *matrix_pixels;   // 256 rows of 256
static SceGxmTexture matrix_texture;
static unsigned matrix_version; // of the content copied there; 0: none yet
static int tlucbm_on;
static int textured; // the textured shaders are there: without them, flat colours
static int shades;   // and the shaded ones

// The views' canvases: see vgpu_set_canvases
static uchar *canvas_pixels[VGPU_CANVASES];
static int canvas_w, canvas_h, canvas_stride, canvas_turn;

// The smooth shadows (vgpu_set_shadows, docs/SHADOWS.md). A smooth scene is
// drawn once, into the canvas's 32-bit side buffer: its first draw imports
// the 8-bit canvas there, and a scene of its own extracts the indices back
// into the canvas afterwards (extract_scene).
typedef struct {
    SceGxmColorSurface surface; // the 32-bit buffer, drawn into
    SceGxmTexture view;         // the same, read by the extract
    SceGxmTexture canvas;       // the 8-bit canvas, read by the import
    int tex_w, tex_h;           // the two textures' size
    int drawn;                  // a scene was drawn there this frame: the next import keeps it
} smooth_set;
static int shadows_wanted; // the prefs want them
static int side_on;        // their shaders draw right: the canvases get side buffers
static SceGxmFragmentProgram *side_fragment_program, *side_trans_fragment_program;
static unsigned side_bitmap_unit, side_tables_unit, side_trans_bitmap_unit, side_trans_tables_unit;
static SceGxmFragmentProgram *import_clear_program, *import_keep_program, *extract_program;
static unsigned import_clear_unit, import_keep_unit, extract_unit;
// The screen's side (vgpu_draw_view): its program and its four textures'
// units (the view through its palette, the canvas, the side buffer, the
// colours), whether its check passed, and the colours: the tables, made from
// the palette and the light table when either changes, and a palette of the
// start-up check's
static SceGxmFragmentProgram *display_program;
static unsigned display_units[4];
static int display_on;
static uchar *colour_block;
static uchar *colour_pixels[COLOUR_TABLES];
static SceGxmTexture colour_textures[COLOUR_TABLES];
static int colour_turn = -1; // the one made last; -1: none yet
static uint32_t *check_palette;
static uchar pal_raw[768], pal_gamma[256];
static int pal_black, have_palette, palette_changed;
static uchar ltab_seen[SHADE_ROWS * 256];
static int have_ltab, ltab_changed;
static float lut_k[SHADE_ROWS];
static uchar lut_constant[256];
// The table as last made, in RAM, and the palette it was made from: a
// palette that cycles changes a few entries, and only the texels that use
// them are made again before the whole is copied to the next texture
static uchar lut_master[COLOUR_TABLE_BYTES];
static uchar built_raw[768], built_gamma[256];
static int built_black, built_valid;
static uchar *side_block; // all of them
static uchar *side_pixels[VGPU_CANVASES];
static smooth_set side_sets[VGPU_CANVASES];
static int side_wanted[VGPU_CANVASES]; // this frame's view is drawn the smooth way
static int side_errors_logged;
// The start-up check's, and the canvas it goes with
static smooth_set *check_set;
static const uchar *check_side_bits;

static gpu_block blocks[MAX_BLOCKS];
static gpu_target targets[MAX_TARGETS];
static SceGxmColorSurface scene_color;
static gpu_target *scene_target, *scene_side_target; // the 8-bit canvas's, the 32-bit buffer's
static int scene_w, scene_h;           // the target's
static int scene_view_w, scene_view_h; // the view's, in its top left
static int scene_canvas;       // which canvas the scene draws into; -1: none of them
static smooth_set *scene_set; // the scene is smooth, drawn there; NULL: the 8-bit way
static int in_scene;
static int scene_out; // a scene ended and not waited for: the GPU may still be drawing it

static vgpu_vertex *vertices;
static uint16_t *indices;
static unsigned vertex_count, index_count;
static gpu_draw draws[MAX_DRAWS];
static unsigned draw_count;
static float to_clip_x, to_clip_y;

static uchar *tables_pixels; // RASTQ_GPU_TABLE_ROWS rows of 256
static SceGxmTexture tables_texture;
// The palette index of each colour of five bits a channel: 32 rows (blue)
// of 1024 (green times 32, plus red)
#define COLOURS_W 1024
#define COLOURS_H 32
static uchar *colours_pixels;
static SceGxmTexture colours_texture;
static const uchar *scene_colours; // the scene's, not copied yet; NULL: copied, or none
static uchar *texture_heap;
static size_t texture_heap_used;
static SceGxmTexture scene_textures[MAX_SCENE_TEXTURES];
static struct {
    const uchar *bits;
    int w, h;
} scene_bitmaps[MAX_SCENE_TEXTURES];
static int scene_texture_count;

vgpu_counters_t vgpu_counters;

static int ready;
static int no_compiler; // the shader compiler module didn't load
static int begin_errors_logged, refusals_logged;
static char report[1024] = "gpu: not initialized";

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

// What every software mapper does for a pixel: the texel at (u, v) rounded
// down, passed through a row of the tables. The vertices carry a real w, so
// the GPU interpolates u and v with the perspective itself, and the bitmap
// is read at the coordinates as they arrive (the kind of read this GPU does
// fastest), its texture set to repeat or to clamp. Both textures pick the
// nearest texel, which is the rounding down; the index goes out a quarter
// above itself, so that rounding can't land on a neighbour.
//
// The row is the mapper's light level, (left + span * along / width) / depth
// (see rastq.h), or one row for the whole polygon. %s is what a transparent
// bitmap adds.
static const char tmap_vertex_source[] =
    "void main(\n"
    "    float2 aPosition,\n"
    "    float3 aTex,\n"
    "    float4 aLight,\n"
    "    float aDepth,\n"
    "    out float4 vPosition : POSITION,\n"
    "    out float2 vTex : TEXCOORD0,\n"
    "    out float4 vLight : TEXCOORD1,\n"
    "    out float vDepth : TEXCOORD2)\n"
    "{\n"
    "    vPosition = float4(aPosition, 0.5f * aTex.z, aTex.z);\n"
    "    vTex = aTex.xy;\n"
    "    vLight = aLight;\n"
    "    vDepth = aDepth;\n"
    "}\n";

static const char tmap_fragment_source[] =
    "float4 main(\n"
    "    float2 vTex : TEXCOORD0,\n"
    "    float4 vLight : TEXCOORD1,\n"
    "    float vDepth : TEXCOORD2,\n"
    "    uniform sampler2D uBitmap : TEXUNIT0,\n"
    "    uniform sampler2D uTables : TEXUNIT1)\n"
    "{\n"
    "    float pal = tex2D(uBitmap, vTex).x;\n"
    "%s"
    "    float part = vLight.z / max(vLight.w, 0.0001f);\n"
    "    float tab = (vLight.x + vLight.y * part) / vDepth;\n"
    "    float2 lut = float2(pal * (255.0f / 256.0f) + 0.5f / 256.0f, tab / %d.0f);\n"
    "    float shade = tex2D(uTables, lut).x + 0.25f / 255.0f;\n"
    "    return float4(shade, shade, shade, 1.0f);\n"
    "}\n";
static const char trans_fragment_lines[] = "    if (pal < 0.5f / 255.0f) discard;\n";

// The textured shader of a smooth scene (docs/SHADOWS.md), for its lit
// polygons only, into the 32-bit buffer: its pixel, worked out in the same
// words, and what the screen needs to light it in full colour. Red is the
// palette index the canvas gets, green the texel, blue the light level in
// sixteenths of a row (0 to 255, rounded as the output rounds), and alpha
// SIDE_LIT. A lit polygon's level is always one of the light table's rows
// (gpu_classify leaves the others to the CPU): nothing to compare. Every
// other shader, drawn into the buffer as it is, writes 255 there; so do
// the textured polygons of one table row, drawn by the textured shader. %s
// is what a transparent bitmap adds; then the tables' height, and SIDE_LIT.
static const char side_fragment_source[] =
    "float4 main(\n"
    "    float2 vTex : TEXCOORD0,\n"
    "    float4 vLight : TEXCOORD1,\n"
    "    float vDepth : TEXCOORD2,\n"
    "    uniform sampler2D uBitmap : TEXUNIT0,\n"
    "    uniform sampler2D uTables : TEXUNIT1)\n"
    "{\n"
    "    float pal = tex2D(uBitmap, vTex).x;\n"
    "%s"
    "    float part = vLight.z / max(vLight.w, 0.0001f);\n"
    "    float tab = (vLight.x + vLight.y * part) / vDepth;\n"
    "    float2 lut = float2(pal * (255.0f / 256.0f) + 0.5f / 256.0f, tab / %d.0f);\n"
    "    float shade = tex2D(uTables, lut).x + 0.25f / 255.0f;\n"
    "    return float4(shade, pal + 0.25f / 255.0f, tab * (16.0f / 255.0f), %d.25f / 255.0f);\n"
    "}\n";

// A smooth scene's first draw, over all of it, the canvas texel for pixel:
// the frame's first scene makes the 32-bit buffer the canvas, nothing lit
// (what it held is three frames old); a later one only where the canvas
// isn't what the buffer has, which is where the CPU drew in between, and
// keeps the rest as it is, marks and all. Read and written as the target's
// bytes, the way the translucent bitmaps' shader is.
static const char import_clear_fragment_source[] =
    "float4 main(\n"
    "    float2 vTex : TEXCOORD0,\n"
    "    uniform sampler2D uCanvas : TEXUNIT0)\n"
    "{\n"
    "    float index = tex2D(uCanvas, vTex).x;\n"
    "    return float4(index + 0.25f / 255.0f, 0.0f, 0.0f, 1.0f);\n"
    "}\n";
static const char import_keep_fragment_source[] =
    "__nativecolor __regformat unsigned char4 main(\n"
    "    float2 vTex : TEXCOORD0,\n"
    "    __regformat unsigned char4 under : FRAGCOLOR,\n"
    "    uniform sampler2D uCanvas : TEXUNIT0)\n"
    "{\n"
    "    float index = floor(tex2D(uCanvas, vTex).x * 255.0f + 0.5f);\n"
    "    float held = under.x;\n"
    "    if (abs(held - index) < 0.5f)\n"
    "        return under;\n"
    "    return float4(index, 0.0f, 0.0f, 255.0f);\n"
    "}\n";
// After a smooth scene, the 8-bit canvas gets the indices: the 32-bit
// buffer's red, texel for pixel.
static const char extract_fragment_source[] =
    "float4 main(\n"
    "    float2 vTex : TEXCOORD0,\n"
    "    uniform sampler2D uView : TEXUNIT0)\n"
    "{\n"
    "    float index = tex2D(uView, vTex).x;\n"
    "    return float4(index + 0.25f / 255.0f, 0.0f, 0.0f, 1.0f);\n"
    "}\n";

// The screen's: the view with smooth shadows. Where the side buffer marks the
// pixel lit and holds the canvas's index there, the texel's colour at that
// light from the table of colours (ShadeLut.h): u in the middle of the
// texel's column, v the level in rows over the table's 16, read between two
// rows' middles. Elsewhere the view as it was, its index through the palette.
// %d is SIDE_LIT.
static const char display_fragment_source[] =
    "float4 main(\n"
    "    float2 vTex : TEXCOORD0,\n"
    "    uniform sampler2D uView : TEXUNIT0,\n"
    "    uniform sampler2D uIndex : TEXUNIT1,\n"
    "    uniform sampler2D uSide : TEXUNIT2,\n"
    "    uniform sampler2D uColours : TEXUNIT3)\n"
    "{\n"
    "    float4 side = tex2D(uSide, vTex);\n"
    "    float index = tex2D(uIndex, vTex).x;\n"
    "    if (abs(side.w - %d.0f / 255.0f) < 0.5f / 255.0f && abs(side.x - index) < 0.5f / 255.0f)\n"
    "        return tex2D(uColours, float2(side.y * (255.0f / 256.0f) + 0.5f / 256.0f, side.z * (255.0f / 256.0f)));\n"
    "    return tex2D(uView, vTex);\n"
    "}\n";

// A polygon shaded between colours: red, green and blue from 0 to 256, each
// left + span * along / width as the light is (see rastq.h), and the pixel
// takes the palette index the game has for their eighths, rounded down.
static const char shaded_vertex_source[] =
    "void main(\n"
    "    float2 aPosition,\n"
    "    float3 aTex,\n"
    "    float4 aLight,\n"
    "    float aDepth,\n"
    "    out float4 vPosition : POSITION,\n"
    "    out float4 vRed : TEXCOORD0,\n"
    "    out float4 vRest : TEXCOORD1)\n"
    "{\n"
    "    vPosition = float4(aPosition, 0.5f, 1.0f);\n"
    "    vRed = aLight;\n"
    "    vRest = float4(aTex, aDepth);\n"
    "}\n";

static const char shaded_fragment_source[] =
    "float4 main(\n"
    "    float4 vRed : TEXCOORD0,\n"
    "    float4 vRest : TEXCOORD1,\n"
    "    uniform sampler2D uColours : TEXUNIT0)\n"
    "{\n"
    "    float part = vRed.z / max(vRed.w, 0.0001f);\n"
    "    float3 tint = float3(vRed.x + vRed.y * part, vRest.x + vRest.y * part, vRest.z + vRest.w * part);\n"
    "    float3 fifth = clamp(floor(tint / 8.0f), 0.0f, 31.0f);\n"
    "    float2 spot = float2((fifth.x + fifth.y * 32.0f + 0.5f) / 1024.0f, (fifth.z + 0.5f) / 32.0f);\n"
    "    float shade = tex2D(uColours, spot).x + 0.25f / 255.0f;\n"
    "    return float4(shade, shade, shade, 1.0f);\n"
    "}\n";

// A translucent polygon: the pixel becomes what the canvas holds there
// passed through a row of the tables. The canvas's pixel is the one the calls
// before this one in the scene left, which this GPU hands a fragment shader
// that asks for it (its "programmable blending"). The vertex shader is the
// flat one's, the row where the index is: its share of the tables' height.
//
// The compiler wants the shader that reads the target declared __nativecolor:
// the target's values then come and go as they are, and both must be
// declared as the target's bytes (__regformat). Whether those count from 0 to
// 255 or from 0 to 1 the compiler doesn't say: the console's check does, and
// the shader is written both ways, the one that draws right there first.
static const char tluc_fragment_source[] =
    "__nativecolor %s main(\n"
    "    float vIndex : TEXCOORD0,\n"
    "    %s under : FRAGCOLOR,\n"
    "    uniform sampler2D uTables : TEXUNIT0)\n"
    "{\n"
    "    float index = under.x;\n"
    "    float2 lut = float2(%s, vIndex);\n"
    "    float shade = tex2D(uTables, lut).x;\n"
    "    shade = %s;\n"
    "    return float4(shade, shade, shade, %s);\n"
    "}\n";
#define TLUC_BYTES "__regformat unsigned char4"
// the index read from 0 to 1, or from 0 to 255: the middle of its table entry
#define TLUC_IN_1 "index * (255.0f / 256.0f) + 0.5f / 256.0f"
#define TLUC_IN_255 "(index + 0.5f) / 256.0f"
// and written, a quarter above itself as the other shaders write it
#define TLUC_OUT_1 "shade + 0.25f / 255.0f", "1.0f"
#define TLUC_OUT_255 "shade * 255.0f + 0.25f", "255.0f"
static const char *const tluc_ways[TLUC_WAYS][5] = {
    // what the shader gives, what it reads, the two lookups
    {TLUC_BYTES, TLUC_BYTES, TLUC_IN_255, TLUC_OUT_255}, // the one that draws right on the console
    {TLUC_BYTES, TLUC_BYTES, TLUC_IN_1, TLUC_OUT_1},
};

// A translucent bitmap: the textured shader, with one more lookup between
// the texel and the tables: the matrix, in the texel's row at the pixel the
// canvas holds, read as the translucent polygons' shader reads it (the way
// the console's check picked: the target's bytes, in and out). The first %s
// is what a transparent bitmap adds.
static const char tlucbm_fragment_source[] =
    "__nativecolor __regformat unsigned char4 main(\n"
    "    float2 vTex : TEXCOORD0,\n"
    "    float4 vLight : TEXCOORD1,\n"
    "    float vDepth : TEXCOORD2,\n"
    "    __regformat unsigned char4 under : FRAGCOLOR,\n"
    "    uniform sampler2D uBitmap : TEXUNIT0,\n"
    "    uniform sampler2D uTables : TEXUNIT1,\n"
    "    uniform sampler2D uMatrix : TEXUNIT2)\n"
    "{\n"
    "    float pal = tex2D(uBitmap, vTex).x;\n"
    "%s"
    "    float index = under.x;\n"
    "    float2 cell = float2((index + 0.5f) / 256.0f, pal * (255.0f / 256.0f) + 0.5f / 256.0f);\n"
    "    float mixed = tex2D(uMatrix, cell).x;\n"
    "    float part = vLight.z / max(vLight.w, 0.0001f);\n"
    "    float tab = (vLight.x + vLight.y * part) / vDepth;\n"
    "    float2 lut = float2(mixed * (255.0f / 256.0f) + 0.5f / 256.0f, tab / %d.0f);\n"
    "    float shade = tex2D(uTables, lut).x * 255.0f + 0.25f;\n"
    "    return float4(shade, shade, shade, 255.0f);\n"
    "}\n";

// ---- gpu.txt -----------------------------------------------------------------
//
// How the start-up went, a line a stage, and what goes wrong after it. The
// profile builds write each line as it comes, opening and closing the file,
// so that a stage that takes the game down is the file's last line. The
// game's own build keeps the lines in memory and writes nothing as long as
// nothing is wrong: at the first error (gpu_err) it writes what it has kept,
// and from then on every line as it comes.

static int log_started; // the file is this run's: lines are added to it
#ifndef VITA_PROFILE
static char log_kept[6144];
static size_t log_kept_len;
static int log_failed; // an error has been logged: the file is being written
#endif

static void log_write(const char *text, size_t length) {
    char path[256];
    FILE *f;

    snprintf(path, sizeof(path), "%sgpu.txt", VITA_PATH);
    f = fopen(path, log_started ? "a" : "w"); // a new file for each run of the game
    if (f == NULL)
        return;
    log_started = 1;
    fwrite(text, 1, length, f);
    fclose(f);
}

static void log_line(int error, const char *format, va_list args) {
    char line[1024];
    int length = vsnprintf(line, sizeof(line) - 1, format, args);

    if (length < 0)
        return;
    if (length > (int)sizeof(line) - 2)
        length = (int)sizeof(line) - 2;
    line[length++] = '\n';
#ifdef VITA_PROFILE
    (void)error;
    log_write(line, (size_t)length);
#else
    if (error && !log_failed) {
        log_failed = 1;
        if (log_kept_len != 0)
            log_write(log_kept, log_kept_len);
        log_kept_len = 0;
    }
    if (log_failed)
        log_write(line, (size_t)length);
    else if (log_kept_len + (size_t)length <= sizeof(log_kept)) {
        // (a line that doesn't fit is left out: a start-up is a third of this)
        memcpy(log_kept + log_kept_len, line, (size_t)length);
        log_kept_len += (size_t)length;
    }
#endif
}

static void gpu_log(const char *format, ...) {
    va_list args;

    va_start(args, format);
    log_line(0, format, args);
    va_end(args);
}

// Something wrong: what makes the game's own build write the file.
static void gpu_err(const char *format, ...) {
    va_list args;

    va_start(args, format);
    log_line(1, format, args);
    va_end(args);
}

static void shark_log(const char *msg, shark_log_level level, int line) {
    if (level == SHARK_LOG_ERROR)
        gpu_err("  shader compiler (%d) line %d: %s", (int)level, line, msg);
    else
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
        gpu_err("alloc of %u bytes failed: 0x%08x", (unsigned)size, (unsigned)uid);
        return NULL;
    }
    if (sceKernelGetMemBlockBase(uid, &base) < 0 || base == NULL) {
        sceKernelFreeMemBlock(uid);
        return NULL;
    }
    err = sceGxmMapMemory(base, size, SCE_GXM_MEMORY_ATTRIB_RW);
    if (err < 0) {
        gpu_err("map of %u bytes failed: 0x%08x", (unsigned)size, (unsigned)err);
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

void vgpu_set_shadows(int on) { shadows_wanted = on; }

// Called each time the game sets its palette, which is every frame where
// part of it cycles: only a palette that differs marks the colours stale.
void vgpu_set_palette(const unsigned char raw[768], const unsigned char gamma[256], int black255) {
    if (have_palette && pal_black == black255 && memcmp(pal_raw, raw, sizeof(pal_raw)) == 0 &&
        memcmp(pal_gamma, gamma, sizeof(pal_gamma)) == 0)
        return;
    memcpy(pal_raw, raw, sizeof(pal_raw));
    memcpy(pal_gamma, gamma, sizeof(pal_gamma));
    pal_black = black255;
    have_palette = palette_changed = 1;
}

static void side_free(void) {
    int i;

    if (side_block != NULL)
        vgpu_free(side_block);
    side_block = NULL;
    for (i = 0; i < VGPU_CANVASES; i++) {
        side_pixels[i] = NULL;
        side_wanted[i] = side_sets[i].drawn = 0;
    }
}

static void point_and_clamp(SceGxmTexture *t) {
    sceGxmTextureSetMinFilter(t, SCE_GXM_TEXTURE_FILTER_POINT);
    sceGxmTextureSetMagFilter(t, SCE_GXM_TEXTURE_FILTER_POINT);
    sceGxmTextureSetUAddrMode(t, SCE_GXM_TEXTURE_ADDR_CLAMP);
    sceGxmTextureSetVAddrMode(t, SCE_GXM_TEXTURE_ADDR_CLAMP);
}

// A 32-bit buffer `side` and the 8-bit canvas it goes with, both `width` by
// `height` with `stride` pixels a row, set up for smooth scenes. The
// textures are `stride` wide. 0 if the GPU won't have them.
static int smooth_set_init(smooth_set *s, uchar *side, uchar *canvas, int width, int height, int stride) {
    int err;

    memset(s, 0, sizeof(*s));
    err = sceGxmColorSurfaceInit(&s->surface, SCE_GXM_COLOR_FORMAT_U8U8U8U8_ABGR, SCE_GXM_COLOR_SURFACE_LINEAR,
                                 SCE_GXM_COLOR_SURFACE_SCALE_NONE, SCE_GXM_OUTPUT_REGISTER_SIZE_32BIT, width, height,
                                 stride, side);
    if (err < 0) {
        gpu_err("no 32-bit color surface: 0x%08x", (unsigned)err);
        return 0;
    }
    err = sceGxmTextureInitLinear(&s->view, side, SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR, stride, height, 0);
    if (err >= 0)
        err = sceGxmTextureInitLinear(&s->canvas, canvas, SCE_GXM_TEXTURE_FORMAT_U8_RRRR, stride, height, 0);
    if (err < 0) {
        gpu_err("no texture over a side buffer or its canvas: 0x%08x", (unsigned)err);
        return 0;
    }
    point_and_clamp(&s->view);
    point_and_clamp(&s->canvas);
    s->tex_w = stride;
    s->tex_h = height;
    return 1;
}

// A side buffer for each canvas: as many pixels, of four bytes, the same
// number of pixels a row. Without them the shadows are the original ones.
static void side_make(int width, int height, int stride) {
    size_t one = ((size_t)stride * 4 * height + 4095) & ~(size_t)4095;
    int i;

    side_block = vgpu_alloc(one * VGPU_CANVASES);
    if (side_block == NULL) {
        gpu_err("no memory for the side buffers: the original shadows");
        return;
    }
    for (i = 0; i < VGPU_CANVASES; i++) {
        side_pixels[i] = side_block + one * i;
        if (!smooth_set_init(&side_sets[i], side_pixels[i], canvas_pixels[i], width, height, stride)) {
            gpu_err("side buffer %d: the original shadows", i);
            side_free();
            return;
        }
    }
    gpu_log("%d side buffers %dx%d, %d pixels a row, %u bytes in all", VGPU_CANVASES, width, height, stride,
            (unsigned)(one * VGPU_CANVASES));
}

#ifdef VITA_PROFILE
static void smooth_timing(void);
#endif

void vgpu_set_canvases(void *const *pixels, int width, int height, int stride) {
    int i;

    // nothing the GPU still has to do may touch the old ones
    if (context != NULL)
        sceGxmFinish(context);
    side_free();
    for (i = 0; i < VGPU_CANVASES; i++)
        canvas_pixels[i] = pixels != NULL ? pixels[i] : NULL;
    canvas_w = width;
    canvas_h = height;
    canvas_stride = stride;
    if (pixels != NULL)
        gpu_log("%d canvases %dx%d, %d bytes a row, the first at %p", VGPU_CANVASES, width, height, stride, pixels[0]);
    if (pixels != NULL && side_on && vgpu_available())
        side_make(width, height, stride);
#ifdef VITA_PROFILE
    if (side_pixels[0] != NULL)
        smooth_timing();
#endif
}

// Which canvas `bits` is: -1 if none.
static int canvas_index(const uchar *bits) {
    int i;
    for (i = 0; i < VGPU_CANVASES; i++)
        if (bits != NULL && canvas_pixels[i] == bits)
            return i;
    return -1;
}

static int is_canvas(const uchar *bits) { return canvas_index(bits) >= 0; }

unsigned char *vgpu_canvas(int width, int height, int *row) {
    if (!vgpu_available() || canvas_pixels[0] == NULL || width > canvas_w || height > canvas_h)
        return NULL;
    *row = canvas_stride;
    // The screen is drawn from a canvas some time after the frame is
    // presented, so each frame takes the canvas used least recently: it is
    // touched again only after two newer frames were handed to the screen,
    // and vita2d lets at most two wait.
    canvas_turn = (canvas_turn + 1) % VGPU_CANVASES;
    // its side buffer is made the canvas again by the frame's first scene
    side_wanted[canvas_turn] = side_pixels[canvas_turn] != NULL;
    side_sets[canvas_turn].drawn = 0;
#ifdef VITA_PROFILE
    if (vprof_variant == 0) // the 8-bit way throughout, to time the smooth one against
        side_wanted[canvas_turn] = 0;
#endif
    return canvas_pixels[canvas_turn];
}

void vgpu_canvas_smooth(int on) {
    if (!on)
        side_wanted[canvas_turn] = 0;
}

// ---- render targets --------------------------------------------------------

// The smooth scenes draw on a target of their own, so that neither has more scenes
// a frame than a target takes (scenesPerFrame).
static gpu_target *target_for(int w, int h, int side) {
    SceGxmRenderTargetParams params;
    gpu_target *t = NULL;
    int aligned_w = (w + SCE_GXM_TILE_SIZEX - 1) & ~(SCE_GXM_TILE_SIZEX - 1);
    int aligned_h = (h + SCE_GXM_TILE_SIZEY - 1) & ~(SCE_GXM_TILE_SIZEY - 1);
    int i, err;

    for (i = 0; i < MAX_TARGETS; i++) {
        if (targets[i].target != NULL && targets[i].w == w && targets[i].h == h && targets[i].side == side)
            return &targets[i];
        if (targets[i].target == NULL && t == NULL)
            t = &targets[i];
    }
    if (t == NULL) {
        // All taken: one of another size goes, never the other target of
        // this size, which the scene may be about to use. There is always
        // one: a size has two.
        for (i = 0; i < MAX_TARGETS && t == NULL; i++)
            if (targets[i].w != w || targets[i].h != h)
                t = &targets[i];
        drop_target(t);
    }

    memset(&params, 0, sizeof(params));
    params.width = (uint16_t)w;
    params.height = (uint16_t)h;
    // 8 is the most a target takes (SCE_GXM_MAX_SCENES_PER_RENDERTARGET): 16
    // and no target is made at all (SCE_GXM_ERROR_INVALID_VALUE, docs/SHADOWS.md)
    params.scenesPerFrame = 8;
    params.multisampleMode = SCE_GXM_MULTISAMPLE_NONE;
    params.driverMemBlock = -1;
    err = sceGxmCreateRenderTarget(&params, &t->target);
    if (err < 0) {
        gpu_err("render target %dx%d failed: 0x%08x", w, h, (unsigned)err);
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
        gpu_err("depth surface failed: 0x%08x", (unsigned)err);
        drop_target(t);
        return NULL;
    }
    t->w = w;
    t->h = h;
    t->side = side;
    return t;
}

// ---- drawing ---------------------------------------------------------------

static void refused(const char *why, const uchar *bits, int w, int h, int row, int err) {
    if (refusals_logged++ < 5)
        gpu_err("refused a %dx%d canvas at %p, %d bytes a row: %s (0x%08x)", w, h, (const void *)bits, row, why,
                (unsigned)err);
}

// Returns once the GPU has drawn the scenes ended so far.
static void vgpu_finish(void) {
    if (scene_out)
        sceGxmFinish(context);
    scene_out = 0;
}

static void full_quad(vgpu_vertex out[4], int w, int h, int tex_w, int tex_h);
static int add_polygon(const SceGxmTexture *texture, int kind, int n, const vgpu_vertex *verts);

// What a scene's draws have to start from: polygons drawn back to front,
// whichever way they face, over everything.
static void scene_state(void) {
    sceGxmSetCullMode(context, SCE_GXM_CULL_NONE);
    sceGxmSetFrontDepthFunc(context, SCE_GXM_DEPTH_FUNC_ALWAYS);
    sceGxmSetBackDepthFunc(context, SCE_GXM_DEPTH_FUNC_ALWAYS);
    sceGxmSetFrontDepthWriteEnable(context, SCE_GXM_DEPTH_WRITE_DISABLED);
    sceGxmSetBackDepthWriteEnable(context, SCE_GXM_DEPTH_WRITE_DISABLED);
}

static int vgpu_begin(uchar *bits, int w, int h, int row, const uchar *tables, int rows, const uchar *ipal) {
    gpu_target *t;
    int target_w = w, target_h = h, err;

    if (!ready)
        return 0;
    // the tables, vertices and bitmaps of a scene still out are in use
    vgpu_finish();
    // A view draws into the top left of one of the view canvases, whatever
    // its own size; the start-up checks into memory of their own.
    if (is_canvas(bits)) {
        if (row != canvas_stride || w > canvas_w || h > canvas_h) {
            refused("too big for a GPU canvas", bits, w, h, row, 0);
            return 0;
        }
        target_w = canvas_w;
        target_h = canvas_h;
    } else if (block_of(bits) == NULL) {
        // A canvas elsewhere can't be rendered into. Say so, a few times:
        // the caller falls back to the CPU without a word.
        refused("not a GPU canvas", bits, w, h, row, 0);
        return 0;
    }
    t = target_for(target_w, target_h, 0);
    if (t == NULL) {
        refused("no render target", bits, w, h, row, 0);
        return 0;
    }
    // Smooth, if the canvas has a side buffer this frame: drawn there, on a
    // target of its own, found before the scene begins (making one may wait
    // for the GPU). Without that target, the 8-bit way.
    scene_target = t;
    scene_w = target_w;
    scene_h = target_h;
    scene_view_w = w;
    scene_view_h = h;
    scene_canvas = canvas_index(bits);
    scene_set = NULL;
    if (scene_canvas >= 0 && side_wanted[scene_canvas])
        scene_set = &side_sets[scene_canvas];
    else if (scene_canvas < 0 && check_set != NULL && bits == check_side_bits)
        scene_set = check_set;
    scene_side_target = scene_set != NULL ? target_for(target_w, target_h, 1) : NULL;
    if (scene_set != NULL && scene_side_target == NULL) {
        if (side_errors_logged++ < 5)
            gpu_err("no render target for the smooth scenes: the original shadows this frame");
        scene_set = NULL;
        if (scene_canvas >= 0)
            side_wanted[scene_canvas] = 0;
    }
    // the canvas: drawn into on the 8-bit way, the extract's on the smooth one
    err = sceGxmColorSurfaceInit(&scene_color, SCE_GXM_COLOR_FORMAT_U8_R, SCE_GXM_COLOR_SURFACE_LINEAR,
                                 SCE_GXM_COLOR_SURFACE_SCALE_NONE, SCE_GXM_OUTPUT_REGISTER_SIZE_32BIT, target_w,
                                 target_h, row, bits);
    if (err < 0) {
        refused("no 8-bit color surface", bits, w, h, row, err);
        return 0;
    }
    // The last scene has finished, so its tables and bitmaps can be
    // replaced.
    if (tables != NULL && tables_pixels != NULL)
        memcpy(tables_pixels, tables, (size_t)rows * 256);
    // the light table a smooth view is drawn with: the colours follow it
    if (scene_set != NULL && scene_canvas >= 0 && tables != NULL && rows >= SHADE_ROWS &&
        (!have_ltab || memcmp(ltab_seen, tables, sizeof(ltab_seen)) != 0)) {
        memcpy(ltab_seen, tables, sizeof(ltab_seen));
        have_ltab = ltab_changed = 1;
    }
    err = -1;
    if (scene_set != NULL) {
        err = sceGxmBeginScene(context, 0, scene_side_target->target, NULL, NULL, NULL, &scene_set->surface,
                               &scene_side_target->depth_stencil);
        if (err < 0) {
            if (side_errors_logged++ < 5)
                gpu_err("begin smooth scene failed: 0x%08x: the original shadows this frame", (unsigned)err);
            scene_set = NULL;
            if (scene_canvas >= 0)
                side_wanted[scene_canvas] = 0;
        }
    }
    if (scene_set == NULL)
        err = sceGxmBeginScene(context, 0, t->target, NULL, NULL, NULL, &scene_color, &t->depth_stencil);
    if (err < 0) {
        if (begin_errors_logged++ < 5)
            gpu_err("begin scene failed: 0x%08x", (unsigned)err);
        return 0;
    }
    scene_state();

    in_scene = 1;
    scene_colours = ipal;
    vertex_count = index_count = draw_count = 0;
    scene_texture_count = 0;
    texture_heap_used = 0;
    to_clip_x = 2.0f / (float)target_w;
    to_clip_y = 2.0f / (float)target_h;

    // A smooth scene starts from the canvas as it is now (see
    // import_clear_fragment_source).
    if (scene_set != NULL) {
        vgpu_vertex q[4];

        full_quad(q, target_w, target_h, scene_set->tex_w, scene_set->tex_h);
        add_polygon(&scene_set->canvas, scene_set->drawn ? DRAW_IMPORT_KEEP : DRAW_IMPORT_CLEAR, 4, q);
    }
    return 1;
}

// The bitmap where the GPU can read it: copied once per scene. NULL if the
// scene has no room for it.
static const SceGxmTexture *texture_for(const grs_bitmap *bm, int wrap) {
    // a linear texture's rows are a multiple of 8 pixels apart
    size_t stride = ((size_t)bm->w + 7) & ~(size_t)7;
    size_t bytes = (stride * bm->h + SCE_GXM_TEXTURE_ALIGNMENT - 1) & ~(size_t)(SCE_GXM_TEXTURE_ALIGNMENT - 1);
    SceGxmTexture *texture;
    long long copy_start;
    uchar *copy;
    int i, y;

    for (i = 0; i < scene_texture_count; i++)
        if (scene_bitmaps[i].bits == bm->bits && scene_bitmaps[i].w == bm->w && scene_bitmaps[i].h == bm->h)
            return &scene_textures[i];
    if (scene_texture_count == MAX_SCENE_TEXTURES || texture_heap_used + bytes > TEXTURE_HEAP_BYTES)
        return NULL;
    copy_start = sceKernelGetProcessTimeWide();
    copy = texture_heap + texture_heap_used;
    for (y = 0; y < bm->h; y++)
        memcpy(copy + y * stride, bm->bits + (size_t)y * bm->row, (size_t)bm->w);
    texture = &scene_textures[scene_texture_count];
    if (sceGxmTextureInitLinear(texture, copy, SCE_GXM_TEXTURE_FORMAT_U8_RRRR, bm->w, bm->h, 0) < 0)
        return NULL;
    sceGxmTextureSetMinFilter(texture, SCE_GXM_TEXTURE_FILTER_POINT);
    sceGxmTextureSetMagFilter(texture, SCE_GXM_TEXTURE_FILTER_POINT);
    sceGxmTextureSetUAddrMode(texture, wrap ? SCE_GXM_TEXTURE_ADDR_REPEAT : SCE_GXM_TEXTURE_ADDR_CLAMP);
    sceGxmTextureSetVAddrMode(texture, wrap ? SCE_GXM_TEXTURE_ADDR_REPEAT : SCE_GXM_TEXTURE_ADDR_CLAMP);
    scene_bitmaps[scene_texture_count].bits = bm->bits;
    scene_bitmaps[scene_texture_count].w = bm->w;
    scene_bitmaps[scene_texture_count].h = bm->h;
    scene_texture_count++;
    texture_heap_used += bytes;
    vgpu_counters.texture_bytes += bytes;
    vgpu_counters.upload_us += (unsigned long long)(sceKernelGetProcessTimeWide() - copy_start);
    return texture;
}

// A convex polygon as a fan of triangles. 0 if the scene has no room for it.
static int add_polygon(const SceGxmTexture *texture, int kind, int n, const vgpu_vertex *verts) {
    unsigned first = vertex_count;
    gpu_draw *d = draw_count ? &draws[draw_count - 1] : NULL;
    int i;

    if (n < 3)
        return 1;
    if (vertex_count + n > MAX_VERTICES || index_count + 3 * (n - 2) > MAX_INDICES)
        return 0;
    if (d == NULL || d->texture != texture || d->kind != kind) {
        if (draw_count == MAX_DRAWS)
            return 0;
        d = &draws[draw_count++];
        d->texture = texture;
        d->kind = kind;
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

// A textured vertex: u and v in texels times q, of a bitmap `w` by `h`, and
// the light values, all as the queue has them.
static void fill(vgpu_vertex *out, const rastq_gpu_vertex *in, float u, float v, int w, int h) {
    float depth = 1.0f / in->q;

    place(out, in);
    out->x *= depth;
    out->y *= depth;
    out->u = u * depth / (float)w;
    out->v = v * depth / (float)h;
    out->w = depth;
    out->left = in->left * depth;
    out->span = in->span * depth;
    out->along = in->along * depth;
    out->width = in->width * depth;
    out->depth = in->depth * depth;
}

// Whether a polygon covers all of the view: a rectangle from its top left
// to its bottom right, or more.
static int covers_view(int n, const rastq_gpu_vertex *v) {
    float x0 = v[0].x, x1 = v[0].x, y0 = v[0].y, y1 = v[0].y;
    int i;

    if (n != 4)
        return 0;
    for (i = 1; i < 4; i++) {
        x0 = v[i].x < x0 ? v[i].x : x0;
        x1 = v[i].x > x1 ? v[i].x : x1;
        y0 = v[i].y < y0 ? v[i].y : y0;
        y1 = v[i].y > y1 ? v[i].y : y1;
    }
    // and nothing but a rectangle: each corner on two of its sides
    for (i = 0; i < 4; i++)
        if ((v[i].x != x0 && v[i].x != x1) || (v[i].y != y0 && v[i].y != y1))
            return 0;
    return x0 <= 0.0f && y0 <= 0.0f && x1 >= (float)scene_view_w && y1 >= (float)scene_view_h;
}

static int vgpu_flat(int n, const rastq_gpu_vertex *v, int color) {
    vgpu_vertex out[RASTQ_GPU_VERTS];
    int i;

    if (n > RASTQ_GPU_VERTS)
        return 1;
    // The view's clear, the frame's first draw: everything the clear import
    // would have brought in is drawn over, so it goes. What the view
    // doesn't cover is never shown.
    if (scene_set != NULL && draw_count == 1 && draws[0].kind == DRAW_IMPORT_CLEAR && covers_view(n, v))
        vertex_count = index_count = draw_count = 0;
    for (i = 0; i < n; i++) {
        // The 8-bit target stores round or floor of 255 times the output:
        // a quarter above the index gives the index either way.
        place(&out[i], &v[i]);
        out[i].u = ((float)color + 0.25f) / 255.0f;
    }
    return add_polygon(NULL, DRAW_FLAT, n, out);
}

// A quad over the whole target, `w` by `h`, past its edges so that every
// pixel's centre is inside, for the textured vertex program: pixel (x, y)
// reads the middle of texel (x, y) of a texture `tex_w` by `tex_h`.
static void full_quad(vgpu_vertex out[4], int w, int h, int tex_w, int tex_h) {
    rastq_gpu_vertex v[4];
    int i;

    memset(v, 0, sizeof(v));
    v[0].x = v[3].x = -1.0f;
    v[1].x = v[2].x = (float)(w + 1);
    v[0].y = v[1].y = -1.0f;
    v[2].y = v[3].y = (float)(h + 1);
    for (i = 0; i < 4; i++) {
        place(&out[i], &v[i]);
        out[i].u = (v[i].x + 0.5f) / (float)tex_w;
        out[i].v = (v[i].y + 0.5f) / (float)tex_h;
        out[i].w = out[i].width = out[i].depth = 1.0f;
    }
}

static int vgpu_shaded(int n, const rastq_gpu_vertex *v) {
    vgpu_vertex out[RASTQ_GPU_VERTS];
    int i;

    if (n > RASTQ_GPU_VERTS)
        return 1;
    if (scene_colours != NULL) {
        // the scene's first shaded polygon: the last scene has finished
        memcpy(colours_pixels, scene_colours, COLOURS_W * COLOURS_H);
        scene_colours = NULL;
    }
    for (i = 0; i < n; i++) {
        place(&out[i], &v[i]);
        out[i].left = v[i].left;
        out[i].span = v[i].span;
        out[i].along = v[i].along;
        out[i].width = v[i].width;
        out[i].u = v[i].g_left;
        out[i].v = v[i].g_span;
        out[i].w = v[i].b_left;
        out[i].depth = v[i].b_span;
    }
    return add_polygon(&colours_texture, DRAW_SHADED, n, out);
}

static int vgpu_tluc(int n, const rastq_gpu_vertex *v) {
    vgpu_vertex out[RASTQ_GPU_VERTS];
    int i;

    if (n > RASTQ_GPU_VERTS || tluc_way < 0)
        return 1;
    for (i = 0; i < n; i++) {
        place(&out[i], &v[i]);
        out[i].u = v[i].left / (float)RASTQ_GPU_TABLE_ROWS;
    }
    return add_polygon(&tables_texture, DRAW_TLUC, n, out);
}

static int vgpu_tluc_tmap(const grs_bitmap *bm, int flags, int n, const rastq_gpu_vertex *v, const uchar *matrix,
                          unsigned version) {
    const SceGxmTexture *texture;
    vgpu_vertex out[RASTQ_GPU_VERTS];
    int i;

    if (n > RASTQ_GPU_VERTS || !tlucbm_on)
        return 1;
    if (matrix_version != version) {
        // the last scene has finished, and this one's calls so far were
        // given the same matrix or none
        memcpy(matrix_pixels, matrix, 256 * 256);
        matrix_version = version;
    }
    texture = texture_for(bm, (flags & RASTQ_GPU_WRAP) != 0);
    if (texture == NULL)
        return 0;
    for (i = 0; i < n; i++)
        fill(&out[i], &v[i], v[i].u, v[i].v, bm->w, bm->h);
    return add_polygon(texture, (flags & RASTQ_GPU_TRANS) ? DRAW_TLUCBM_TRANS : DRAW_TLUCBM, n, out);
}

static int vgpu_tmap(const grs_bitmap *bm, int flags, int n, const rastq_gpu_vertex *v) {
    const SceGxmTexture *texture;
    vgpu_vertex out[RASTQ_GPU_VERTS];
    int i, lit, kind;

    if (n > RASTQ_GPU_VERTS)
        return 1;
    texture = texture_for(bm, (flags & RASTQ_GPU_WRAP) != 0);
    if (texture == NULL)
        return 0;
    for (i = 0; i < n; i++)
        fill(&out[i], &v[i], v[i].u, v[i].v, bm->w, bm->h);
    // Lit: its light level is one of the light table's rows (see rastq.h; a
    // polygon of one row of another table has that row and a half)
    lit = (v[0].left + v[0].span * v[0].along / (v[0].width != 0.0f ? v[0].width : 1.0f)) /
              (v[0].depth != 0.0f ? v[0].depth : 1.0f) <
          (float)RASTQ_GPU_LIGHT_ROWS;
    if (flags & RASTQ_GPU_TRANS)
        kind = lit ? DRAW_LIT_TRANS : DRAW_TRANS;
    else
        kind = lit ? DRAW_LIT : DRAW_TMAP;
    return add_polygon(texture, kind, n, out);
}

// The scene's draws, into the canvas, or the smooth way into its 32-bit
// buffer (`smooth`): the same, but for the import and, for the textured
// polygons, the side shaders, which add the texel, the light and the mark.
static void issue_draws(int smooth) {
    int last = -1;
    unsigned k;

    sceGxmSetVertexStream(context, 0, vertices);
    for (k = 0; k < draw_count; k++) {
        const gpu_draw *d = &draws[k];
        int kind = d->kind;

        if (kind == DRAW_IMPORT_CLEAR || kind == DRAW_IMPORT_KEEP) {
            int keep = kind == DRAW_IMPORT_KEEP;

            sceGxmSetVertexProgram(context, tmap_vertex_program);
            sceGxmSetFragmentProgram(context, keep ? import_keep_program : import_clear_program);
            sceGxmSetFragmentTexture(context, keep ? import_keep_unit : import_clear_unit, d->texture);
        } else if (kind == DRAW_FLAT) {
            if (last != DRAW_FLAT) {
                sceGxmSetVertexProgram(context, flat_vertex_program);
                sceGxmSetFragmentProgram(context, flat_fragment_program);
            }
        } else if (kind == DRAW_SHADED) {
            if (last != DRAW_SHADED) {
                sceGxmSetVertexProgram(context, shaded_vertex_program);
                sceGxmSetFragmentProgram(context, shaded_fragment_program);
                sceGxmSetFragmentTexture(context, colours_unit, &colours_texture);
            }
        } else if (kind == DRAW_TLUCBM || kind == DRAW_TLUCBM_TRANS) {
            const int *units = tlucbm_units[kind == DRAW_TLUCBM_TRANS];

            sceGxmSetVertexProgram(context, tmap_vertex_program);
            sceGxmSetFragmentProgram(context, kind == DRAW_TLUCBM_TRANS ? tlucbm_trans_fragment_program
                                                                        : tlucbm_fragment_program);
            sceGxmSetFragmentTexture(context, (unsigned)units[0], d->texture);
            sceGxmSetFragmentTexture(context, (unsigned)units[1], &tables_texture);
            sceGxmSetFragmentTexture(context, (unsigned)units[2], &matrix_texture);
        } else if (kind == DRAW_TLUC) {
            if (last != DRAW_TLUC) {
                sceGxmSetVertexProgram(context, tluc_vertex_program);
                sceGxmSetFragmentProgram(context, tluc_fragment_programs[tluc_way]);
                sceGxmSetFragmentTexture(context, (unsigned)tluc_units[tluc_way], &tables_texture);
            }
        } else if (smooth && (kind == DRAW_LIT || kind == DRAW_LIT_TRANS)) {
            int trans = kind == DRAW_LIT_TRANS;
            if (last < DRAW_TMAP || last > DRAW_LIT_TRANS)
                sceGxmSetVertexProgram(context, tmap_vertex_program);
            sceGxmSetFragmentProgram(context, trans ? side_trans_fragment_program : side_fragment_program);
            sceGxmSetFragmentTexture(context, trans ? side_trans_bitmap_unit : side_bitmap_unit, d->texture);
            sceGxmSetFragmentTexture(context, trans ? side_trans_tables_unit : side_tables_unit, &tables_texture);
        } else {
            int trans = kind == DRAW_TRANS || kind == DRAW_LIT_TRANS;
            if (last < DRAW_TMAP || last > DRAW_LIT_TRANS)
                sceGxmSetVertexProgram(context, tmap_vertex_program);
            sceGxmSetFragmentProgram(context, trans ? trans_fragment_program : tmap_fragment_program);
            sceGxmSetFragmentTexture(context, trans ? trans_bitmap_unit : bitmap_unit, d->texture);
            sceGxmSetFragmentTexture(context, trans ? trans_tables_unit : tables_unit, &tables_texture);
        }
        last = kind;
        sceGxmDraw(context, SCE_GXM_PRIMITIVE_TRIANGLES, SCE_GXM_INDEX_FORMAT_U16, indices + d->first, d->count);
    }
}

// After a smooth scene: its indices into the 8-bit canvas, in a scene of
// its own on the canvas's target, one quad drawn from the vertices' spare
// room. If that scene can't begin, the canvas misses this one's drawing, and
// the frame goes on the 8-bit way.
static void extract_scene(void) {
    long long start = sceKernelGetProcessTimeWide();
    vgpu_vertex *q = &vertices[MAX_VERTICES];
    uint16_t *qi = &indices[MAX_INDICES];
    int err;

    full_quad(q, scene_w, scene_h, scene_set->tex_w, scene_set->tex_h);
    qi[0] = qi[3] = MAX_VERTICES;
    qi[1] = MAX_VERTICES + 1;
    qi[2] = qi[4] = MAX_VERTICES + 2;
    qi[5] = MAX_VERTICES + 3;
    err = sceGxmBeginScene(context, 0, scene_target->target, NULL, NULL, NULL, &scene_color,
                           &scene_target->depth_stencil);
    if (err < 0) {
        if (side_errors_logged++ < 5)
            gpu_err("extract scene failed: 0x%08x", (unsigned)err);
        if (scene_canvas >= 0)
            side_wanted[scene_canvas] = 0;
        scene_set->drawn = 0;
        return;
    }
    scene_state();
    sceGxmSetVertexStream(context, 0, vertices);
    sceGxmSetVertexProgram(context, tmap_vertex_program);
    sceGxmSetFragmentProgram(context, extract_program);
    sceGxmSetFragmentTexture(context, extract_unit, &scene_set->view);
    sceGxmDraw(context, SCE_GXM_PRIMITIVE_TRIANGLES, SCE_GXM_INDEX_FORMAT_U16, qi, 6);
    sceGxmEndScene(context, NULL, NULL);
    scene_set->drawn = 1;
    vgpu_counters.side_scenes++;
    vgpu_counters.side_draw_us += (unsigned long long)(sceKernelGetProcessTimeWide() - start);
}

static void vgpu_end(void) {
    long long start = sceKernelGetProcessTimeWide();

    if (!in_scene)
        return;
    if (index_count != 0)
        issue_draws(scene_set != NULL);
    vgpu_counters.draws += draw_count;
    vgpu_counters.draw_us += (unsigned long long)(sceKernelGetProcessTimeWide() - start);
    sceGxmEndScene(context, NULL, NULL);
    if (scene_set != NULL)
        extract_scene();
    // The GPU has the scene. Whoever touches the canvas next waits for it
    // (vgpu_finish); until then the CPU is free for something else.
    scene_out = 1;
    in_scene = 0;
}

// ---- the screen's side of the smooth shadows ---------------------------------

// The tables of colours and the start-up check's palette, in GPU memory. 0
// if there is no room.
static int make_colour_tables(void) {
    int i;

    colour_block = vgpu_alloc(COLOUR_TABLES * COLOUR_TABLE_BYTES + 256 * 4);
    if (colour_block == NULL) {
        gpu_err("no memory for the tables of colours");
        return 0;
    }
    for (i = 0; i < COLOUR_TABLES; i++) {
        colour_pixels[i] = colour_block + i * COLOUR_TABLE_BYTES;
        memset(colour_pixels[i], 0, COLOUR_TABLE_BYTES);
        if (sceGxmTextureInitLinear(&colour_textures[i], colour_pixels[i], SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR, 256,
                                    SHADE_ROWS, 0) < 0)
            return 0;
        // read between two rows; at a texel's middle across, so that only
        // the rows blend
        sceGxmTextureSetMinFilter(&colour_textures[i], SCE_GXM_TEXTURE_FILTER_LINEAR);
        sceGxmTextureSetMagFilter(&colour_textures[i], SCE_GXM_TEXTURE_FILTER_LINEAR);
        sceGxmTextureSetUAddrMode(&colour_textures[i], SCE_GXM_TEXTURE_ADDR_CLAMP);
        sceGxmTextureSetVAddrMode(&colour_textures[i], SCE_GXM_TEXTURE_ADDR_CLAMP);
    }
    // 64-byte aligned, as a palette must be: it follows 3 tables of 16 KB
    check_palette = (uint32_t *)(colour_block + COLOUR_TABLES * COLOUR_TABLE_BYTES);
    return 1;
}

// The table of colours as the palette and the light table are now: all of
// it when the light table, the gamma or index 255 changed (or there is none
// yet), else only the texels whose palette entries changed, in RAM; then
// copied to the next texture, in one go, if anything changed.
static void colours_make(void) {
    long long t0 = sceKernelGetProcessTimeWide();
    uchar changed[256], only[256];
    int i, n, next;

    if (ltab_changed)
        shade_lut_ratios(lut_k, lut_constant, pal_raw, ltab_seen);
    if (!built_valid || ltab_changed || built_black != pal_black ||
        memcmp(built_gamma, pal_gamma, sizeof(built_gamma)) != 0) {
        shade_lut_fill(lut_master, lut_k, lut_constant, pal_raw, ltab_seen, pal_gamma, pal_black, NULL);
        n = 256;
    } else {
        for (i = 0; i < 256; i++)
            changed[i] = memcmp(&built_raw[3 * i], &pal_raw[3 * i], 3) != 0;
        n = shade_lut_affected(only, changed, ltab_seen);
        if (n != 0)
            shade_lut_fill(lut_master, lut_k, lut_constant, pal_raw, ltab_seen, pal_gamma, pal_black, only);
    }
    memcpy(built_raw, pal_raw, sizeof(built_raw));
    memcpy(built_gamma, pal_gamma, sizeof(built_gamma));
    built_black = pal_black;
    built_valid = 1;
    ltab_changed = palette_changed = 0;
    if (n == 0 && colour_turn >= 0)
        return; // the colours the screen has are still right
    next = (colour_turn + 1) % COLOUR_TABLES;
    memcpy(colour_pixels[next], lut_master, COLOUR_TABLE_BYTES);
    colour_turn = next;
    vgpu_counters.lut_builds++;
    vgpu_counters.lut_texels += (unsigned long long)n;
    vgpu_counters.lut_us += (unsigned long long)(sceKernelGetProcessTimeWide() - t0);
}

// The screen's quad, a strip of 4 vertices: (x0, y0) to (x1, y1) on a
// target `target_w` by `target_h`, reading (0, 0) to (u1, v1) of the
// textures.
static void display_quad(vgpu_vertex v[4], float x0, float y0, float x1, float y1, float u1, float v1, int target_w,
                         int target_h) {
    int i;

    memset(v, 0, 4 * sizeof(v[0]));
    for (i = 0; i < 4; i++) {
        float x = i & 1 ? x1 : x0, y = i & 2 ? y1 : y0;

        v[i].x = 2.0f * x / (float)target_w - 1.0f;
        v[i].y = 1.0f - 2.0f * y / (float)target_h;
        v[i].u = i & 1 ? u1 : 0.0f;
        v[i].v = i & 2 ? v1 : 0.0f;
        v[i].w = v[i].width = v[i].depth = 1.0f;
    }
}

static void display_draw(const SceGxmTexture *view, const smooth_set *set, const vgpu_vertex *v, const uint16_t *idx) {
    sceGxmSetVertexProgram(context, tmap_vertex_program);
    sceGxmSetFragmentProgram(context, display_program);
    sceGxmSetFragmentTexture(context, display_units[0], view);
    sceGxmSetFragmentTexture(context, display_units[1], &set->canvas);
    sceGxmSetFragmentTexture(context, display_units[2], &set->view);
    sceGxmSetFragmentTexture(context, display_units[3], &colour_textures[colour_turn]);
    sceGxmSetVertexStream(context, 0, v);
    sceGxmDraw(context, SCE_GXM_PRIMITIVE_TRIANGLE_STRIP, SCE_GXM_INDEX_FORMAT_U16, idx, 4);
}

int vgpu_draw_view(const void *texture, const void *bits, int tex_w, float x, float y, float sx, float sy, int w,
                   int h) {
    int i = canvas_index(bits);
    const smooth_set *set;
    vgpu_vertex *v;
    uint16_t *idx;

    // a view drawn the smooth way this frame, all of it
    if (!display_on || i < 0 || !side_wanted[i] || !side_sets[i].drawn || !have_palette || !have_ltab)
        return 0;
    set = &side_sets[i];
    if (tex_w != set->tex_w || w > set->tex_w || h > set->tex_h)
        return 0;
    if (colour_turn < 0 || palette_changed || ltab_changed)
        colours_make();
    v = vita2d_pool_memalign(4 * sizeof(vgpu_vertex), sizeof(void *));
    idx = vita2d_pool_memalign(4 * sizeof(uint16_t), sizeof(uint16_t));
    if (v == NULL || idx == NULL)
        return 0;
    display_quad(v, x, y, x + (float)w * sx, y + (float)h * sy, (float)w / (float)set->tex_w,
                 (float)h / (float)set->tex_h, SCREEN_W, SCREEN_H);
    for (i = 0; i < 4; i++)
        idx[i] = (uint16_t)i;
    display_draw((const SceGxmTexture *)texture, set, v, idx);
    vgpu_counters.smooth_frames++;
    return 1;
}

#ifdef VITA_PROFILE
// ---- frame pairs for looking at on a PC -------------------------------------

// Rows of `w` bytes, `row` apart, copied one after the other into `to`. A
// memcpy reads GPU memory in bursts; byte by byte it is many times slower.
static void copy_rows(uchar *to, const uchar *pixels, int w, int h, int row) {
    int y;

    for (y = 0; y < h; y++)
        memcpy(to + (size_t)y * w, pixels + (size_t)y * row, (size_t)w);
}

// The dump being written: copies in RAM, and what the files are called. One at
// a time; while `busy`, a dump that comes due is let go.
#define DUMP_FILES 6
static struct {
    uchar *gpu, *cpu, *side;
    uchar palette[4 * 256];
    uchar ltab[256 * RASTQ_GPU_LIGHT_ROWS];
    char info[200];
    struct {
        const char *name; // NN-<name>
        const void *data;
        size_t bytes;
    } files[DUMP_FILES];
    int count, number;
    size_t pixels; // what the copies have room for
} dump;
static SDL_Thread *dump_thread;
static SDL_sem *dump_ready;
static SDL_atomic_t dump_busy;
unsigned vgpu_dump_us; // the last dump's writing, in the background

static int dump_writer(void *unused) {
    char dir[200], path[256];
    int i;

    (void)unused;
    // on any core, when nothing of the game's wants it, as the log's writer
    sceKernelChangeThreadCpuAffinityMask(sceKernelGetThreadId(), SCE_KERNEL_CPU_MASK_USER_ALL);
    SDL_SetThreadPriority(SDL_THREAD_PRIORITY_LOW);
    for (;;) {
        long long t0;

        SDL_SemWait(dump_ready);
        t0 = sceKernelGetProcessTimeWide();
        snprintf(dir, sizeof(dir), "%sgpudumps", VITA_PATH);
        sceIoMkdir(dir, 0777);
        for (i = 0; i < dump.count; i++) {
            FILE *f;

            snprintf(path, sizeof(path), "%s/%02d-%s", dir, dump.number, dump.files[i].name);
            f = fopen(path, "wb");
            if (f == NULL)
                continue;
            // the whole file at once: row by row, it was 1,600 writes to the
            // card and 8 s
            fwrite(dump.files[i].data, 1, dump.files[i].bytes, f);
            fclose(f);
        }
        __atomic_store_n(&vgpu_dump_us, (unsigned)(sceKernelGetProcessTimeWide() - t0), __ATOMIC_RELAXED);
        SDL_AtomicSet(&dump_busy, 0);
    }
    return 0;
}

static void dump_add(const char *name, const void *data, size_t bytes) {
    dump.files[dump.count].name = name;
    dump.files[dump.count].data = data;
    dump.files[dump.count].bytes = bytes;
    dump.count++;
}

// Whether a dump can be taken now: the writer is there and idle, and the
// copies have their room.
static int dump_can_start(int w, int h) {
    size_t pixels = (size_t)w * h;

    if (SDL_AtomicGet(&dump_busy))
        return 0;
    if (pixels > dump.pixels) {
        // made for the first dump's canvas, again for a bigger one
        free(dump.gpu);
        free(dump.cpu);
        free(dump.side);
        dump.gpu = malloc(pixels);
        dump.cpu = malloc(pixels);
        dump.side = malloc(4 * pixels);
        dump.pixels = pixels;
        if (dump.gpu == NULL || dump.cpu == NULL || dump.side == NULL) {
            free(dump.gpu);
            free(dump.cpu);
            free(dump.side);
            dump.gpu = dump.cpu = dump.side = NULL;
            dump.pixels = 0;
            return 0;
        }
    }
    if (dump_ready == NULL)
        dump_ready = SDL_CreateSemaphore(0);
    if (dump_thread == NULL && dump_ready != NULL) {
        dump_thread = SDL_CreateThread(dump_writer, "vgpu_dump", NULL);
        if (dump_thread != NULL)
            SDL_DetachThread(dump_thread);
    }
    return dump_thread != NULL;
}

// `gpu` is the GPU's picture, `cpu` the canvas, now holding the CPU's.
static void vgpu_compared_now(const uchar *gpu, const uchar *cpu, int w, int h, int row, unsigned differing);

static void vgpu_compared(const uchar *gpu, const uchar *cpu, int w, int h, int row, unsigned differing) {
    long long t0 = sceKernelGetProcessTimeWide();
    unsigned took;

    vgpu_compared_now(gpu, cpu, w, h, row, differing);
    took = (unsigned)(sceKernelGetProcessTimeWide() - t0);
    if (took > vgpu_counters.compare_max_us)
        vgpu_counters.compare_max_us = took;
}

static void vgpu_compared_now(const uchar *gpu, const uchar *cpu, int w, int h, int row, unsigned differing) {
    // a side buffer's row, out of GPU memory
    static uchar side_row[4 * 1024];
    static int comparisons, dumps;
    static unsigned worst;
    static long long last_dump_us;
    long long now = sceKernelGetProcessTimeWide();
    int side = canvas_index(cpu);
    unsigned long long lit = 0, mismatch = 0;
    int x, y;

    // The side buffer against the GPU's picture: where it is marked lit, the
    // index there must be the canvas's, or the screen would light a pixel
    // that isn't the one drawn.
    if (side >= 0 && !(side_wanted[side] && side_sets[side].drawn))
        side = -1;
    if (side >= 0 && 4 * w > (int)sizeof(side_row))
        side = -1;
    if (side >= 0) {
        for (y = 0; y < h; y++) {
            const uchar *p = side_row;
            const uchar *g = gpu + (size_t)y * row;

            memcpy(side_row, side_pixels[side] + (size_t)y * canvas_stride * 4, (size_t)4 * w);
            for (x = 0; x < w; x++, p += 4) {
                if (p[3] == SIDE_LIT) {
                    lit++;
                    mismatch += p[0] != g[x];
                }
            }
        }
        vgpu_counters.side_pixels += (unsigned long long)w * h;
        vgpu_counters.side_lit += lit;
        vgpu_counters.side_mismatch += mismatch;
    }

    comparisons++;
    if (dumps >= MAX_DUMPS)
        return;
    if (comparisons != 3 && (differing <= worst || differing <= (unsigned)(DUMP_SHARE * w * h)))
        return;
    // few, and apart: walking into a room sets record after record
    if (dumps != 0 && now - last_dump_us < DUMP_APART_US)
        return;
    // the last one is still being written: the next that qualifies goes
    if (!dump_can_start(w, h))
        return;
    if (differing > worst)
        worst = differing;
    last_dump_us = now;
    dumps++;

    // Copies of all of it, for the writer to take its time over
    dump.count = 0;
    dump.number = dumps;
    copy_rows(dump.gpu, gpu, w, h, row);
    dump_add("gpu.raw", dump.gpu, (size_t)w * h);
    copy_rows(dump.cpu, cpu, w, h, row);
    dump_add("cpu.raw", dump.cpu, (size_t)w * h);
    // 256 colours, 4 bytes each: red, green, blue, alpha
    memcpy(dump.palette, vita2d_texture_get_palette(texBuffer), sizeof(dump.palette));
    dump_add("palette.raw", dump.palette, sizeof(dump.palette));
    if (side >= 0) {
        // the side buffer: w pixels a row of 4 bytes, index, texel, level, mark
        copy_rows(dump.side, side_pixels[side], 4 * w, h, 4 * canvas_stride);
        dump_add("side.raw", dump.side, (size_t)4 * w * h);
        // and the light table it was drawn with: 16 rows of 256
        memcpy(dump.ltab, tables_pixels, sizeof(dump.ltab));
        dump_add("ltab.raw", dump.ltab, sizeof(dump.ltab));
    }
    snprintf(dump.info, sizeof(dump.info),
             "width=%d height=%d comparison=%d differing=%u side=%d lit=%llu mismatch=%llu\n", w, h, comparisons,
             differing, side >= 0, lit, mismatch);
    dump_add("info.txt", dump.info, strlen(dump.info));
    SDL_AtomicSet(&dump_busy, 1);
    SDL_SemPost(dump_ready);
}

#else
#define vgpu_compared NULL
#endif

// What the queue is given: without `shaded` or `tluc` if their shaders
// aren't there (vgpu_init).
static rastq_gpu queue_hooks = {vgpu_begin, vgpu_flat,   vgpu_tmap,     vgpu_shaded,   vgpu_end,
                                vgpu_finish, vgpu_compared, vgpu_tluc, vgpu_tluc_tmap};

// ---- set-up ----------------------------------------------------------------

static SceGxmProgram *compile(const char *source, shark_type type, const char *what) {
    uint32_t size = (uint32_t)strlen(source);
    SceGxmProgram *program = shark_compile_shader(source, &size, type);
    SceGxmProgram *copy = NULL;

    if (program == NULL) {
        gpu_err("%s shader did not compile", what);
    } else {
        copy = malloc(size);
        if (copy != NULL) {
            memcpy(copy, program, size);
            gpu_log("%s shader compiled: %u bytes", what, (unsigned)size);
        } else {
            gpu_err("%s shader compiled, no memory for its copy", what);
        }
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
            gpu_err("registering the %s shader failed: 0x%08x", name, (unsigned)err);
            return 0;
        }
        for (i = 0; i < count; i++) {
            const SceGxmProgramParameter *parameter = sceGxmProgramFindParameterByName(vertex, names[i]);
            if (parameter == NULL) {
                gpu_err("%s shader: no parameter %s", name, names[i]);
                return 0;
            }
            attribute(&attributes[i], parameter, offsets[i], sizes[i]);
        }
        stream.stride = sizeof(vgpu_vertex);
        stream.indexSource = SCE_GXM_INDEX_SOURCE_INDEX_16BIT;
        err = sceGxmShaderPatcherCreateVertexProgram(patcher, last_vertex_id, attributes, count, &stream, 1,
                                                     vertex_program);
        if (err < 0) {
            gpu_err("%s program failed: 0x%08x", name, (unsigned)err);
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
        gpu_err("registering the %s shader failed: 0x%08x", name, (unsigned)err);
        return 0;
    }
    err = sceGxmShaderPatcherCreateFragmentProgram(patcher, fragment_id, SCE_GXM_OUTPUT_REGISTER_FORMAT_UCHAR4,
                                                   SCE_GXM_MULTISAMPLE_NONE, NULL, last_vertex, fragment_program);
    if (err < 0) {
        gpu_err("%s program failed: 0x%08x", name, (unsigned)err);
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

static int make_tmap_programs(void) {
    static const char *const names[] = {"aPosition", "aTex", "aLight", "aDepth"};
    static const size_t offsets[] = {offsetof(vgpu_vertex, x), offsetof(vgpu_vertex, u), offsetof(vgpu_vertex, left),
                                     offsetof(vgpu_vertex, depth)};
    static const int sizes[] = {2, 3, 4, 1};
    static char source[2048];
    const SceGxmProgram *opaque = NULL, *trans = NULL;
    int units[4];

    snprintf(source, sizeof(source), tmap_fragment_source, "", RASTQ_GPU_TABLE_ROWS);
    if (!make_programs("textured", tmap_vertex_source, source, names, offsets, sizes, 4, &tmap_vertex_program,
                       &tmap_fragment_program, &opaque))
        return 0;
    snprintf(source, sizeof(source), tmap_fragment_source, trans_fragment_lines, RASTQ_GPU_TABLE_ROWS);
    if (!make_programs("transparent", tmap_vertex_source, source, names, offsets, sizes, 4, &tmap_vertex_program,
                       &trans_fragment_program, &trans))
        return 0;
    units[0] = sampler_unit(opaque, "uBitmap");
    units[1] = sampler_unit(opaque, "uTables");
    units[2] = sampler_unit(trans, "uBitmap");
    units[3] = sampler_unit(trans, "uTables");
    gpu_log("texture units: bitmap %d, tables %d; transparent: bitmap %d, tables %d", units[0], units[1], units[2],
            units[3]);
    if (units[0] < 0 || units[1] < 0 || units[2] < 0 || units[3] < 0)
        return 0;
    bitmap_unit = (unsigned)units[0];
    tables_unit = (unsigned)units[1];
    trans_bitmap_unit = (unsigned)units[2];
    trans_tables_unit = (unsigned)units[3];
    return 1;
}

// The translucent bitmaps' two shaders, right after the textured ones, whose
// vertex program they share. 0 if either doesn't compile.
static int make_tlucbm_programs(void) {
    static const char *const names[] = {"aPosition", "aTex", "aLight", "aDepth"};
    static const size_t offsets[] = {offsetof(vgpu_vertex, x), offsetof(vgpu_vertex, u), offsetof(vgpu_vertex, left),
                                     offsetof(vgpu_vertex, depth)};
    static const int sizes[] = {2, 3, 4, 1};
    static const char *const samplers[3] = {"uBitmap", "uTables", "uMatrix"};
    static char source[2048];
    int trans, k;

    for (trans = 0; trans < 2; trans++) {
        const SceGxmProgram *fragment = NULL;

        snprintf(source, sizeof(source), tlucbm_fragment_source, trans ? trans_fragment_lines : "",
                 RASTQ_GPU_TABLE_ROWS);
        if (!make_programs(trans ? "translucent transparent bitmap" : "translucent bitmap", tmap_vertex_source, source,
                           names, offsets, sizes, 4, &tmap_vertex_program,
                           trans ? &tlucbm_trans_fragment_program : &tlucbm_fragment_program, &fragment))
            return 0;
        for (k = 0; k < 3; k++)
            tlucbm_units[trans][k] = sampler_unit(fragment, samplers[k]);
        gpu_log("texture units: translucent bitmap%s: bitmap %d, tables %d, matrix %d", trans ? ", transparent" : "",
                tlucbm_units[trans][0], tlucbm_units[trans][1], tlucbm_units[trans][2]);
        if (tlucbm_units[trans][0] < 0 || tlucbm_units[trans][1] < 0 || tlucbm_units[trans][2] < 0)
            return 0;
    }
    return 1;
}

// The smooth scenes' two textured shaders, right after the translucent bitmaps'
// ones: they take the textured ones' vertex program too. 0 if either doesn't
// compile.
static int make_side_programs(void) {
    static const char *const names[] = {"aPosition", "aTex", "aLight", "aDepth"};
    static const size_t offsets[] = {offsetof(vgpu_vertex, x), offsetof(vgpu_vertex, u), offsetof(vgpu_vertex, left),
                                     offsetof(vgpu_vertex, depth)};
    static const int sizes[] = {2, 3, 4, 1};
    static char source[2048];
    const SceGxmProgram *opaque = NULL, *trans = NULL;
    int units[4];

    snprintf(source, sizeof(source), side_fragment_source, "", RASTQ_GPU_TABLE_ROWS, SIDE_LIT);
    if (!make_programs("side", tmap_vertex_source, source, names, offsets, sizes, 4, &tmap_vertex_program,
                       &side_fragment_program, &opaque))
        return 0;
    snprintf(source, sizeof(source), side_fragment_source, trans_fragment_lines, RASTQ_GPU_TABLE_ROWS, SIDE_LIT);
    if (!make_programs("side transparent", tmap_vertex_source, source, names, offsets, sizes, 4,
                       &tmap_vertex_program, &side_trans_fragment_program, &trans))
        return 0;
    units[0] = sampler_unit(opaque, "uBitmap");
    units[1] = sampler_unit(opaque, "uTables");
    units[2] = sampler_unit(trans, "uBitmap");
    units[3] = sampler_unit(trans, "uTables");
    gpu_log("texture units: side: bitmap %d, tables %d; transparent: bitmap %d, tables %d", units[0], units[1],
            units[2], units[3]);
    if (units[0] < 0 || units[1] < 0 || units[2] < 0 || units[3] < 0)
        return 0;
    side_bitmap_unit = (unsigned)units[0];
    side_tables_unit = (unsigned)units[1];
    side_trans_bitmap_unit = (unsigned)units[2];
    side_trans_tables_unit = (unsigned)units[3];
    return 1;
}

// The smooth scenes' import and extract shaders, right after the side ones,
// with the textured vertex program again. 0 if any of them doesn't compile.
static int make_single_pass_programs(void) {
    static const char *const names[] = {"aPosition", "aTex", "aLight", "aDepth"};
    static const size_t offsets[] = {offsetof(vgpu_vertex, x), offsetof(vgpu_vertex, u), offsetof(vgpu_vertex, left),
                                     offsetof(vgpu_vertex, depth)};
    static const int sizes[] = {2, 3, 4, 1};
    static const struct {
        const char *what, *source, *sampler;
        SceGxmFragmentProgram **program;
        unsigned *unit;
    } shaders[] = {
        {"import clear", import_clear_fragment_source, "uCanvas", &import_clear_program, &import_clear_unit},
        {"import keep", import_keep_fragment_source, "uCanvas", &import_keep_program, &import_keep_unit},
        {"extract", extract_fragment_source, "uView", &extract_program, &extract_unit},
    };
    size_t k;

    for (k = 0; k < sizeof(shaders) / sizeof(shaders[0]); k++) {
        const SceGxmProgram *fragment = NULL;
        int unit;

        if (!make_programs(shaders[k].what, tmap_vertex_source, shaders[k].source, names, offsets, sizes, 4,
                           &tmap_vertex_program, shaders[k].program, &fragment))
            return 0;
        unit = sampler_unit(fragment, shaders[k].sampler);
        gpu_log("texture unit: %s, %s %d", shaders[k].what, shaders[k].sampler, unit);
        if (unit < 0)
            return 0;
        *shaders[k].unit = (unsigned)unit;
    }
    // the screen's, with its four textures
    {
        static const char *const samplers[4] = {"uView", "uIndex", "uSide", "uColours"};
        static char source[2048];
        const SceGxmProgram *fragment = NULL;

        snprintf(source, sizeof(source), display_fragment_source, SIDE_LIT);
        if (!make_programs("display", tmap_vertex_source, source, names, offsets, sizes, 4, &tmap_vertex_program,
                           &display_program, &fragment))
            return 0;
        for (k = 0; k < 4; k++) {
            int unit = sampler_unit(fragment, samplers[k]);
            if (unit < 0)
                return 0;
            display_units[k] = (unsigned)unit;
        }
        gpu_log("texture units: display: view %u, index %u, side %u, colours %u", display_units[0], display_units[1],
                display_units[2], display_units[3]);
    }
    return 1;
}

static int make_shaded_programs(void) {
    static const char *const names[] = {"aPosition", "aTex", "aLight", "aDepth"};
    static const size_t offsets[] = {offsetof(vgpu_vertex, x), offsetof(vgpu_vertex, u), offsetof(vgpu_vertex, left),
                                     offsetof(vgpu_vertex, depth)};
    static const int sizes[] = {2, 3, 4, 1};
    const SceGxmProgram *fragment = NULL;
    int unit;

    if (!make_programs("shaded", shaded_vertex_source, shaded_fragment_source, names, offsets, sizes, 4,
                       &shaded_vertex_program, &shaded_fragment_program, &fragment))
        return 0;
    unit = sampler_unit(fragment, "uColours");
    gpu_log("texture unit: colours %d", unit);
    if (unit < 0)
        return 0;
    colours_unit = (unsigned)unit;
    return 1;
}

// One way's translucent shader. 0 if it doesn't compile; the check says
// whether it draws right. No other shaders may be made between two of these:
// they share their vertex program.
static int make_tluc_program(int way) {
    static const char *const names[] = {"aPosition", "aIndex"};
    static const size_t offsets[] = {offsetof(vgpu_vertex, x), offsetof(vgpu_vertex, u)};
    static const int sizes[] = {2, 1};
    static char source[1024];
    const SceGxmProgram *fragment = NULL;
    char what[32];

    snprintf(what, sizeof(what), "translucent %d", way);
    snprintf(source, sizeof(source), tluc_fragment_source, tluc_ways[way][0], tluc_ways[way][1], tluc_ways[way][2],
             tluc_ways[way][3], tluc_ways[way][4]);
    tluc_units[way] = -1;
    if (!make_programs(what, flat_vertex_source, source, names, offsets, sizes, 2, &tluc_vertex_program,
                       &tluc_fragment_programs[way], &fragment)) {
        tluc_fragment_programs[way] = NULL;
        return 0;
    }
    tluc_units[way] = sampler_unit(fragment, "uTables");
    gpu_log("texture unit: translucent %d, tables %d", way, tluc_units[way]);
    if (tluc_units[way] < 0)
        tluc_fragment_programs[way] = NULL;
    return tluc_units[way] >= 0;
}

// The memory the textured and shaded shaders draw from: the tables, the
// colours' table and the bitmaps.
static int make_textures(void) {
    tables_pixels = vgpu_alloc(RASTQ_GPU_TABLE_ROWS * 256);
    colours_pixels = vgpu_alloc(COLOURS_W * COLOURS_H);
    matrix_pixels = vgpu_alloc(256 * 256);
    texture_heap = vgpu_alloc(TEXTURE_HEAP_BYTES);
    if (tables_pixels == NULL || colours_pixels == NULL || matrix_pixels == NULL || texture_heap == NULL) {
        gpu_err("no memory for the tables and the bitmaps");
        return 0;
    }
    memset(colours_pixels, 0, COLOURS_W * COLOURS_H);
    if (sceGxmTextureInitLinear(&colours_texture, colours_pixels, SCE_GXM_TEXTURE_FORMAT_U8_RRRR, COLOURS_W,
                                COLOURS_H, 0) < 0)
        return 0;
    sceGxmTextureSetMinFilter(&colours_texture, SCE_GXM_TEXTURE_FILTER_POINT);
    sceGxmTextureSetMagFilter(&colours_texture, SCE_GXM_TEXTURE_FILTER_POINT);
    sceGxmTextureSetUAddrMode(&colours_texture, SCE_GXM_TEXTURE_ADDR_CLAMP);
    sceGxmTextureSetVAddrMode(&colours_texture, SCE_GXM_TEXTURE_ADDR_CLAMP);
    memset(matrix_pixels, 0, 256 * 256);
    if (sceGxmTextureInitLinear(&matrix_texture, matrix_pixels, SCE_GXM_TEXTURE_FORMAT_U8_RRRR, 256, 256, 0) < 0)
        return 0;
    sceGxmTextureSetMinFilter(&matrix_texture, SCE_GXM_TEXTURE_FILTER_POINT);
    sceGxmTextureSetMagFilter(&matrix_texture, SCE_GXM_TEXTURE_FILTER_POINT);
    sceGxmTextureSetUAddrMode(&matrix_texture, SCE_GXM_TEXTURE_ADDR_CLAMP);
    sceGxmTextureSetVAddrMode(&matrix_texture, SCE_GXM_TEXTURE_ADDR_CLAMP);
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
    int i;

    memset(v, 0, 4 * sizeof(v[0]));
    v[0].x = v[3].x = (float)x0;
    v[1].x = v[2].x = (float)x1;
    v[0].y = v[1].y = (float)y0;
    v[2].y = v[3].y = (float)y1;
    for (i = 0; i < 4; i++) {
        v[i].q = v[i].width = v[i].depth = 1.0f;
        v[i].left = RASTQ_GPU_PLAIN_ROW + 0.5f;
    }
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
    if (!vgpu_begin(canvas, CHECK_W, CHECK_H, CHECK_W, NULL, 0, NULL)) {
        vgpu_free(canvas);
        return -1;
    }
    for (k = 0; k < 256; k++)
        quad(k, k + 1, k);
    vgpu_end();
    vgpu_finish();
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
    if (vgpu_begin(canvas, CHECK_W, CHECK_H, CHECK_W, NULL, 0, NULL)) {
        quad(0, CHECK_W / 2, 0x11);
        vgpu_end();
        vgpu_finish();
        *kept = canvas[(CHECK_H / 2) * CHECK_W + CHECK_W - 8] == 0x55 && canvas[(CHECK_H / 2) * CHECK_W + 8] == 0x11;
    }
    vgpu_free(canvas);
    return bad;
}

// The textured shaders, on four bands of the check canvas, each drawn so
// that column x holds a value that is known:
//   plain: texel x of a row of 256, unchanged
//   table: the same through a table of the scene
//   light: the same through the light table, the level worked out from a
//          left value, a span and the distance along the row
//   wrap:  the first row of a 16x16 bitmap with a transparent column,
//          repeated 16 times, through the perspective division, the row
//          divided by a depth
// Sets the number of wrong columns of each band. Returns 0 if nothing could
// be drawn.
#define CHECK_BANDS 4
static int texture_check(int bad[CHECK_BANDS]) {
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
    if (!vgpu_begin(canvas, CHECK_W, CHECK_H, CHECK_W, tables, ROWS, NULL)) {
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
            if (band == 1) {
                v[i].left = TABLE_ROW + 0.5f;
            } else if (band == 2) {
                // level (x + 0.5) / 16 in column x: 16 levels over the 256
                v[i].left = 0.5f / 16.0f;
                v[i].span = 16.0f;
                v[i].along = v[i].x;
                v[i].width = (float)CHECK_W;
            } else if (band == 3) {
                v[i].left = 2.0f * (RASTQ_GPU_PLAIN_ROW + 0.5f);
                v[i].depth = 2.0f;
            }
        }
        if (band < 3)
            vgpu_tmap(&ramp_bm, 0, 4, v);
        else
            vgpu_tmap(&tiles_bm, RASTQ_GPU_TRANS | RASTQ_GPU_WRAP, 4, v);
    }
    vgpu_end();
    vgpu_finish();

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
                    gpu_err("texture check, band %d: column %d holds %d, not %d", band, x, row[x], want);
            }
        }
    }
    vgpu_free(canvas);
    return 1;
}

// The smooth scenes, on four bands of the check canvas and its 32-bit
// buffer, both filled beforehand with values a draw wouldn't leave (the
// buffer with marks). Two scenes, with a CPU write to the canvas between
// them, as the queue does when it draws a call itself. The first begins as
// a view does, with a flat clear over all of it, which takes the place of
// the clear import:
//   lit:   the light band of texture_check, at level (x + 0.25) / 16: in
//          column x the canvas has the light table's entry, the buffer
//          that, the texel x, the level in sixteenths, x, and the mark
//   plain: texel x through the row that leaves it as it is, on the left
//          half; nothing drawn on the right, which keeps the clear's 0xEE
//   tluc:  a flat colour on the left half, the lit band on the right, and in
//          the second scene a translucent polygon over that, if the
//          translucent shaders draw: the table's entry through its row
//   cpu:   the lit band, the CPU writing 0x44 over its left half before the
//          second scene: the left the CPU's, the right still lit
// The canvas must hold the buffer's red everywhere, and only lit pixels the
// mark. Sets the number of wrong columns of each band. Returns 0 if nothing
// could be drawn.
#define SIDE_BANDS 4
static int side_check(int bad[SIDE_BANDS]) {
    enum {
        ROW_A = RASTQ_GPU_PLAIN_ROW + 1,
        ROWS,
        BAND_H = CHECK_H / SIDE_BANDS,
        HALF = CHECK_W / 2,
        FLAT = 0x33,
        BEFORE = 0xEE, // the clear
        FILLED = 0xDD, // the canvas before it
        BY_CPU = 0x44
    };
    static uchar ramp[8 * 256], tables[ROWS * 256];
    static smooth_set set;
    uchar *canvas = vgpu_alloc(CHECK_W * CHECK_H);
    uchar *side = vgpu_alloc(CHECK_W * 4 * CHECK_H);
    rastq_gpu_vertex v[4];
    grs_bitmap ramp_bm;
    int band, scene, x, y, i, k, tluc_drawn = 0, ok = 0;

    for (band = 0; band < SIDE_BANDS; band++)
        bad[band] = -1;
    if (canvas == NULL || side == NULL)
        goto done;
    for (i = 0; i < 256; i++) {
        for (k = 0; k < 8; k++)
            ramp[k * 256 + i] = (uchar)i;
        for (k = 0; k < RASTQ_GPU_LIGHT_ROWS; k++)
            tables[k * 256 + i] = (uchar)(i * 3 + k * 17 + 1);
        tables[RASTQ_GPU_PLAIN_ROW * 256 + i] = (uchar)i;
        tables[ROW_A * 256 + i] = (uchar)(i * 7 + 3);
    }
    memset(&ramp_bm, 0, sizeof(ramp_bm));
    ramp_bm.bits = ramp;
    ramp_bm.w = ramp_bm.row = 256;
    ramp_bm.h = 8;
    if (!smooth_set_init(&set, side, canvas, CHECK_W, CHECK_H, CHECK_W))
        goto done;
    memset(canvas, FILLED, CHECK_W * CHECK_H);
    memset(side, SIDE_LIT, CHECK_W * 4 * CHECK_H);
    check_set = &set;
    check_side_bits = canvas;

    for (scene = 0; scene < 2; scene++) {
        if (scene == 1) {
            // the CPU draws on the canvas between two scenes
            for (y = 3 * BAND_H; y < 4 * BAND_H; y++)
                memset(canvas + y * CHECK_W, BY_CPU, HALF);
        }
        if (!vgpu_begin(canvas, CHECK_W, CHECK_H, CHECK_W, tables, ROWS, NULL))
            goto done;
        if (scene == 0) {
            corners(v, 0, CHECK_W, 0, CHECK_H);
            vgpu_flat(4, v, BEFORE);
        }
        for (band = 0; band < SIDE_BANDS; band++) {
            int y0 = band * BAND_H, y1 = y0 + BAND_H;

            if (scene == 1) {
                if (band == 2 && tluc_way >= 0) {
                    corners(v, HALF, CHECK_W, y0, y1);
                    for (i = 0; i < 4; i++)
                        v[i].left = ROW_A + 0.5f;
                    vgpu_tluc(4, v);
                    tluc_drawn = 1;
                }
                continue;
            }
            if (band == 2) {
                corners(v, 0, HALF, y0, y1);
                vgpu_flat(4, v, FLAT);
            }
            corners(v, band == 2 ? HALF : 0, band == 1 ? HALF : CHECK_W, y0, y1);
            for (i = 0; i < 4; i++) {
                // the middle of texel x in column x, and of row 0
                v[i].u = v[i].x + 0.5f;
                v[i].v = 0.5f;
                if (band != 1) {
                    // level (x + 0.25) / 16 in column x: x sixteenths,
                    // whether the output rounds or truncates
                    v[i].left = 0.25f / 16.0f;
                    v[i].span = 16.0f;
                    v[i].along = v[i].x;
                    v[i].width = (float)CHECK_W;
                }
            }
            vgpu_tmap(&ramp_bm, 0, 4, v);
        }
        vgpu_end();
        vgpu_finish();
    }

    for (band = 0; band < SIDE_BANDS; band++) {
        y = band * BAND_H + BAND_H / 2;
        bad[band] = 0;
        for (x = 0; x < 256; x++) {
            const uchar *p = side + ((size_t)y * CHECK_W + x) * 4;
            int drawn = canvas[y * CHECK_W + x], entry = tables[(x / 16) * 256 + x], left = x < HALF, want = -1;
            int lit = 0, wrong;

            if (band == 0 || (band >= 2 && !left && !(band == 2 && tluc_drawn))) {
                want = entry;
                lit = 1;
            } else if (band == 1) {
                want = left ? x : BEFORE;
            } else if (band == 2) {
                want = left ? FLAT : tables[ROW_A * 256 + entry];
            } else {
                want = BY_CPU;
            }
            wrong = drawn != want || p[0] != want || (p[3] == SIDE_LIT) != lit;
            if (lit)
                wrong |= p[1] != x || p[2] != x;
            if (wrong && bad[band]++ < 2)
                gpu_err("side check, band %d: column %d holds %d %d %d %d, canvas %d, not %d%s", band, x, p[0], p[1],
                        p[2], p[3], drawn, want, lit ? " lit" : "");
        }
    }
    ok = 1;

done:
    check_set = NULL;
    check_side_bits = NULL;
    if (canvas != NULL)
        vgpu_free(canvas);
    if (side != NULL)
        vgpu_free(side);
    return ok;
}

// The screen's shader, into a 32-bit buffer of its own, from a check canvas
// whose column x holds index x, seen through a palette of its own, and a side
// buffer the CPU fills, on three bands:
//   lit:      marked, the canvas's index, texel x, x sixteenths of a row:
//             the table of colours read between its two rows there, as the
//             CPU works it out (within 3 a channel)
//   mismatch: marked, but another index: the palette's colour
//   unmarked: the palette's colour
// The table is one made up for it. Sets the number of wrong columns of each
// band. Returns 0 if nothing could be drawn.
#define DISPLAY_BANDS 3
static int display_check(int bad[DISPLAY_BANDS]) {
    enum { BAND_H = CHECK_H / 4 };
    static smooth_set set;
    uchar *canvas = vgpu_alloc(CHECK_W * CHECK_H);
    uchar *side = vgpu_alloc(CHECK_W * 4 * CHECK_H);
    uchar *out = vgpu_alloc(CHECK_W * 4 * CHECK_H);
    uchar *table = colour_pixels[0];
    SceGxmColorSurface surface;
    SceGxmTexture p8;
    gpu_target *t = target_for(CHECK_W, CHECK_H, 1);
    int band, x, y, c, ok = 0;

    for (band = 0; band < DISPLAY_BANDS; band++)
        bad[band] = -1;
    if (canvas == NULL || side == NULL || out == NULL || t == NULL || table == NULL)
        goto done;
    if (!smooth_set_init(&set, side, canvas, CHECK_W, CHECK_H, CHECK_W))
        goto done;
    if (sceGxmColorSurfaceInit(&surface, SCE_GXM_COLOR_FORMAT_U8U8U8U8_ABGR, SCE_GXM_COLOR_SURFACE_LINEAR,
                               SCE_GXM_COLOR_SURFACE_SCALE_NONE, SCE_GXM_OUTPUT_REGISTER_SIZE_32BIT, CHECK_W, CHECK_H,
                               CHECK_W, out) < 0 ||
        sceGxmTextureInitLinear(&p8, canvas, SCE_GXM_TEXTURE_FORMAT_P8_ABGR, CHECK_W, CHECK_H, 0) < 0 ||
        sceGxmTextureSetPalette(&p8, check_palette) < 0) {
        gpu_err("display check: no surface or no paletted texture");
        goto done;
    }
    point_and_clamp(&p8);
    for (x = 0; x < 256; x++) {
        // red x, green 255 - x, blue 3x
        check_palette[x] = (uint32_t)x | (uint32_t)(255 - x) << 8 | (uint32_t)((x * 3) & 255) << 16 | 0xFF000000u;
        for (y = 0; y < SHADE_ROWS; y++) {
            uchar *o = &table[(y * 256 + x) * 4];
            o[0] = (uchar)x;
            o[1] = (uchar)(y * 17);
            o[2] = (uchar)(255 - x);
            o[3] = 255;
        }
    }
    for (y = 0; y < CHECK_H; y++) {
        band = y / BAND_H;
        for (x = 0; x < CHECK_W; x++) {
            uchar *p = side + ((size_t)y * CHECK_W + x) * 4;

            canvas[y * CHECK_W + x] = (uchar)x;
            p[0] = (uchar)(band == 1 ? x ^ 1 : x);
            p[1] = (uchar)x;
            p[2] = (uchar)x;
            p[3] = band == 2 ? 0xFF : SIDE_LIT;
        }
    }
    memset(out, 0, CHECK_W * 4 * CHECK_H);
    colour_turn = 0;

    if (sceGxmBeginScene(context, 0, t->target, NULL, NULL, NULL, &surface, &t->depth_stencil) < 0)
        goto done;
    scene_state();
    display_quad(vertices, 0.0f, 0.0f, (float)CHECK_W, (float)CHECK_H, 1.0f, 1.0f, CHECK_W, CHECK_H);
    for (x = 0; x < 4; x++)
        indices[x] = (uint16_t)x;
    display_draw(&p8, &set, vertices, indices);
    sceGxmEndScene(context, NULL, NULL);
    sceGxmFinish(context);

    for (band = 0; band < DISPLAY_BANDS; band++) {
        y = band * BAND_H + BAND_H / 2;
        bad[band] = 0;
        for (x = 0; x < 256; x++) {
            const uchar *o = out + ((size_t)y * CHECK_W + x) * 4;
            int want[3], wrong = 0;

            if (band == 0) {
                // level x / 16 rows; row r's colour is at r + 0.5
                float at = (float)x / 16.0f - 0.5f, part;
                int r0, r1;

                if (at < 0.0f)
                    at = 0.0f;
                if (at > SHADE_ROWS - 1)
                    at = SHADE_ROWS - 1;
                r0 = (int)at;
                r1 = r0 + 1 < SHADE_ROWS ? r0 + 1 : r0;
                part = at - (float)r0;
                for (c = 0; c < 3; c++) {
                    float a = table[(r0 * 256 + x) * 4 + c], b = table[(r1 * 256 + x) * 4 + c];
                    want[c] = (int)(a + (b - a) * part + 0.5f);
                }
            } else {
                want[0] = x;
                want[1] = 255 - x;
                want[2] = (x * 3) & 255;
            }
            for (c = 0; c < 3; c++)
                wrong |= o[c] - want[c] > 3 || want[c] - o[c] > 3;
            if (wrong && bad[band]++ < 2)
                gpu_err("display check, band %d: column %d shows %d %d %d, not %d %d %d", band, x, o[0], o[1], o[2],
                        want[0], want[1], want[2]);
        }
    }
    ok = 1;

done:
    colour_turn = -1; // the game's table is made when it is needed
    if (canvas != NULL)
        vgpu_free(canvas);
    if (side != NULL)
        vgpu_free(side);
    if (out != NULL)
        vgpu_free(out);
    return ok;
}

// Draws with the shaded shaders, in three bands, each with one of red, green
// and blue going from 0 to 256 across the 256 columns while the other two
// stay put, and compares each column with the colours' table. The value
// that moves is given as the queue gives a cut polygon's: a left value, a
// span, and how far along the pixel is. Sets the number of wrong columns of
// each band. Returns 0 if nothing could be drawn.
#define COLOUR_BANDS 3
static int colour_check(int bad[COLOUR_BANDS]) {
    enum { BAND_H = CHECK_H / 4 };
    static const float still[3] = {100.5f, 44.5f, 201.5f};
    static uchar table[COLOURS_W * COLOURS_H];
    uchar *canvas = vgpu_alloc(CHECK_W * CHECK_H);
    rastq_gpu_vertex v[4];
    int band, x, i;

    for (band = 0; band < COLOUR_BANDS; band++)
        bad[band] = -1;
    if (canvas == NULL)
        return 0;
    for (i = 0; i < COLOURS_W * COLOURS_H; i++)
        table[i] = (uchar)(1 + ((i & 31) * 7 + ((i >> 5) & 31) * 13 + ((i >> 10) & 31) * 29) % 255);
    memset(canvas, 0, CHECK_W * CHECK_H);
    if (!vgpu_begin(canvas, CHECK_W, CHECK_H, CHECK_W, NULL, 0, table)) {
        vgpu_free(canvas);
        return 0;
    }
    for (band = 0; band < COLOUR_BANDS; band++) {
        corners(v, 0, CHECK_W, band * BAND_H, (band + 1) * BAND_H);
        for (i = 0; i < 4; i++) {
            v[i].left = band == 0 ? 0.5f : still[0]; // the one that moves: x + 0.5 in column x
            v[i].span = band == 0 ? 256.0f : 0;
            v[i].g_left = band == 1 ? 0.5f : still[1];
            v[i].g_span = band == 1 ? 256.0f : 0;
            v[i].b_left = band == 2 ? 0.5f : still[2];
            v[i].b_span = band == 2 ? 256.0f : 0;
            v[i].along = v[i].x;
            v[i].width = (float)CHECK_W;
        }
        vgpu_shaded(4, v);
    }
    vgpu_end();
    vgpu_finish();

    for (band = 0; band < COLOUR_BANDS; band++) {
        const uchar *row = canvas + (band * BAND_H + BAND_H / 2) * CHECK_W;

        bad[band] = 0;
        for (x = 0; x < 256; x++) {
            int fifth[3] = {(int)still[0] / 8, (int)still[1] / 8, (int)still[2] / 8}, want;
            fifth[band] = x / 8;
            want = table[fifth[0] | (fifth[1] << 5) | (fifth[2] << 10)];
            if (row[x] != want) {
                if (bad[band]++ == 0)
                    gpu_err("colour check, band %d: column %d holds %d, not %d", band, x, row[x], want);
            }
        }
    }
    vgpu_free(canvas);
    return 1;
}

// The translucent shaders, the way of tluc_way, on four bands of the check
// canvas over a ground whose column x holds x, drawn in the same scene but
// for one band:
//   under: one polygon through table A: A[x]
//   twice: two of them, one over the other: A[A[x]]
//   kept:  one through table B in a scene of its own, over what the first
//          scene left: B[x]
//   mixed: one through A, a flat colour over its left quarter, then one
//          through B over the lot: B[A[x]], and B[the colour] on the left
// Sets the number of wrong columns of each band. Returns 0 if nothing could
// be drawn.
#define TLUC_BANDS 4
static int tluc_check(int bad[TLUC_BANDS]) {
    enum {
        ROW_A = RASTQ_GPU_PLAIN_ROW + 1,
        ROW_B,
        ROWS,
        BAND_H = CHECK_H / TLUC_BANDS,
        OVER = 7,
        OVER_W = CHECK_W / 4
    };
    static uchar ramp[8 * 256], tables[ROWS * 256];
    uchar *canvas = vgpu_alloc(CHECK_W * CHECK_H);
    const uchar *a = &tables[ROW_A * 256], *b = &tables[ROW_B * 256];
    rastq_gpu_vertex v[4];
    grs_bitmap ramp_bm;
    int band, scene, x, i, k;

    for (band = 0; band < TLUC_BANDS; band++)
        bad[band] = -1;
    if (canvas == NULL)
        return 0;
    for (i = 0; i < 256; i++) {
        for (k = 0; k < 8; k++)
            ramp[k * 256 + i] = (uchar)i;
        tables[RASTQ_GPU_PLAIN_ROW * 256 + i] = (uchar)i;
        tables[ROW_A * 256 + i] = (uchar)(i * 7 + 3);
        tables[ROW_B * 256 + i] = (uchar)(i * 13 + 101);
    }
    memset(&ramp_bm, 0, sizeof(ramp_bm));
    ramp_bm.bits = ramp;
    ramp_bm.w = ramp_bm.row = 256;
    ramp_bm.h = 8;

    memset(canvas, 0xEE, CHECK_W * CHECK_H);
    for (scene = 0; scene < 2; scene++) {
        if (!vgpu_begin(canvas, CHECK_W, CHECK_H, CHECK_W, tables, ROWS, NULL)) {
            vgpu_free(canvas);
            return 0;
        }
        if (scene == 0) {
            // the ground: texel x in column x
            corners(v, 0, CHECK_W, 0, CHECK_H);
            for (i = 0; i < 4; i++) {
                v[i].u = v[i].x + 0.5f;
                v[i].v = 0.5f;
            }
            vgpu_tmap(&ramp_bm, 0, 4, v);
        }
        for (band = 0; band < TLUC_BANDS; band++) {
            int y0 = band * BAND_H, y1 = y0 + BAND_H;

            if ((band == 2) != (scene == 1))
                continue;
            corners(v, 0, CHECK_W, y0, y1);
            for (i = 0; i < 4; i++)
                v[i].left = (band == 2 ? ROW_B : ROW_A) + 0.5f;
            vgpu_tluc(4, v);
            if (band == 1)
                vgpu_tluc(4, v);
            if (band == 3) {
                corners(v, 0, OVER_W, y0, y1);
                vgpu_flat(4, v, OVER);
                corners(v, 0, CHECK_W, y0, y1);
                for (i = 0; i < 4; i++)
                    v[i].left = ROW_B + 0.5f;
                vgpu_tluc(4, v);
            }
        }
        vgpu_end();
        vgpu_finish();
    }

    for (band = 0; band < TLUC_BANDS; band++) {
        const uchar *row = canvas + (band * BAND_H + BAND_H / 2) * CHECK_W;

        bad[band] = 0;
        for (x = 0; x < 256; x++) {
            int want = band == 0 ? a[x] : band == 1 ? a[a[x]] : band == 2 ? b[x] : x < OVER_W ? b[OVER] : b[a[x]];
            if (row[x] != want) {
                // a few of them: they tell how the pixel reached the shader
                if (bad[band]++ < 4)
                    gpu_err("translucency check, way %d, band %d: column %d holds %d, not %d", tluc_way, band, x,
                            row[x], want);
            }
        }
    }
    vgpu_free(canvas);
    return 1;
}

// The translucent bitmaps' shaders, on four bands of the check canvas over a
// ground whose column x holds x. The bitmap's texel in column x is known,
// some with a row of their own in the matrix, some without, some 0:
//   opaque: every texel: matrix[texel][x]
//   trans:  texel 0 leaves the ground: x
//   table:  the opaque one through table A: A[matrix[texel][x]]
//   twice:  the transparent one drawn two times over
// Sets the number of wrong columns of each band. Returns 0 if nothing could
// be drawn.
#define TLUCBM_BANDS 4
#define TLUCBM_CHECK_VERSION 0xC0DEu // not a version the queue will have
static int tlucbm_check(int bad[TLUCBM_BANDS]) {
    enum { ROW_A = RASTQ_GPU_PLAIN_ROW + 1, ROWS, BAND_H = CHECK_H / TLUCBM_BANDS };
    static uchar ramp[8 * 256], stripe[8 * 256], tables[ROWS * 256], matrix[256 * 256];
    uchar *canvas = vgpu_alloc(CHECK_W * CHECK_H);
    const uchar *a = &tables[ROW_A * 256];
    rastq_gpu_vertex v[4];
    grs_bitmap ramp_bm, stripe_bm;
    int band, x, i, k;

    for (band = 0; band < TLUCBM_BANDS; band++)
        bad[band] = -1;
    if (canvas == NULL)
        return 0;
    for (i = 0; i < 256; i++) {
        uchar texel = (i % 16) == 7 ? 0 : (uchar)(i * 3);

        for (k = 0; k < 8; k++) {
            ramp[k * 256 + i] = (uchar)i;
            stripe[k * 256 + i] = texel;
        }
        tables[RASTQ_GPU_PLAIN_ROW * 256 + i] = (uchar)i;
        tables[ROW_A * 256 + i] = (uchar)(i * 7 + 3);
        // texel i: one in three has a table
        for (k = 0; k < 256; k++)
            matrix[i * 256 + k] = (i % 3) == 1 ? (uchar)(k * 5 + i) : (uchar)i;
    }
    memset(&ramp_bm, 0, sizeof(ramp_bm));
    ramp_bm.bits = ramp;
    ramp_bm.w = ramp_bm.row = 256;
    ramp_bm.h = 8;
    stripe_bm = ramp_bm;
    stripe_bm.bits = stripe;

    memset(canvas, 0xEE, CHECK_W * CHECK_H);
    if (!vgpu_begin(canvas, CHECK_W, CHECK_H, CHECK_W, tables, ROWS, NULL)) {
        vgpu_free(canvas);
        return 0;
    }
    for (band = -1; band < TLUCBM_BANDS; band++) {
        // first the ground, all over
        corners(v, 0, CHECK_W, band < 0 ? 0 : band * BAND_H, band < 0 ? CHECK_H : (band + 1) * BAND_H);
        for (i = 0; i < 4; i++) {
            v[i].u = v[i].x + 0.5f;
            v[i].v = 0.5f;
            if (band == 2)
                v[i].left = ROW_A + 0.5f;
        }
        if (band < 0) {
            vgpu_tmap(&ramp_bm, 0, 4, v);
            continue;
        }
        vgpu_tluc_tmap(&stripe_bm, band == 1 || band == 3 ? RASTQ_GPU_TRANS : 0, 4, v, matrix, TLUCBM_CHECK_VERSION);
        if (band == 3)
            vgpu_tluc_tmap(&stripe_bm, RASTQ_GPU_TRANS, 4, v, matrix, TLUCBM_CHECK_VERSION);
    }
    vgpu_end();
    vgpu_finish();

    for (band = 0; band < TLUCBM_BANDS; band++) {
        const uchar *row = canvas + (band * BAND_H + BAND_H / 2) * CHECK_W;

        bad[band] = 0;
        for (x = 0; x < 256; x++) {
            int texel = stripe[x], once = matrix[texel * 256 + x], want;

            if (band == 0)
                want = once;
            else if (band == 1)
                want = texel == 0 ? x : once;
            else if (band == 2)
                want = a[once];
            else
                want = texel == 0 ? x : matrix[texel * 256 + once];
            if (row[x] != want) {
                if (bad[band]++ < 4)
                    gpu_err("translucent bitmap check, band %d: column %d holds %d, not %d (texel %d)", band, x,
                            row[x], want, texel);
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

// How long the CPU takes to pass `size` bytes of a canvas through a
// translucency table, in microseconds: pixel by pixel in place, as the 2D
// library did, and its way now (gr_tluc8_span), a canvas row at a time.
static void time_tluc_fill(uchar *p, size_t size, int *in_place_us, int *copied_us) {
    static uchar table[256];
    long long t0, t1, t2;
    size_t at;

    for (at = 0; at < 256; at++)
        table[at] = (uchar)(at * 7 + 3);
    memset(p, 0x5a, size);
    t0 = sceKernelGetProcessTimeWide();
    for (at = 0; at < size; at++)
        p[at] = table[p[at]];
    t1 = sceKernelGetProcessTimeWide();
    for (at = 0; at < size; at += 960)
        gr_tluc8_span(p + at, (int)(size - at < 960 ? size - at : 960), table, NULL);
    t2 = sceKernelGetProcessTimeWide();
    *in_place_us = (int)(t1 - t0);
    *copied_us = (int)(t2 - t1);
}

#ifdef VITA_PROFILE
// ---- what a smooth frame's full-screen work costs (docs/SHADOWS.md, 2b) ----
//
// Each case is one scene on canvas 0 or its side buffer, begun, drawn,
// ended and waited for, TIMING_RUNS times: the minimum and the median of
// the times from the begin to the end of the wait. The draws come from the
// start of the vertex buffer: a full-screen quad (vertices 0-3, indices
// 0-5) and a one-pixel one (4-7, 6-11).
#define TIMING_RUNS 10

static const SceGxmTexture *timing_texture; // what the import or the extract reads

static void timing_full(void) {
    sceGxmDraw(context, SCE_GXM_PRIMITIVE_TRIANGLES, SCE_GXM_INDEX_FORMAT_U16, indices, 6);
}

static void timing_flat(void) {
    sceGxmSetVertexProgram(context, flat_vertex_program);
    sceGxmSetFragmentProgram(context, flat_fragment_program);
    sceGxmDraw(context, SCE_GXM_PRIMITIVE_TRIANGLES, SCE_GXM_INDEX_FORMAT_U16, indices + 6, 6);
}

// A flat colour over all of it: the view's own clear, which now replaces the
// clear import when it comes first
static void timing_flat_full(void) {
    sceGxmSetVertexProgram(context, flat_vertex_program);
    sceGxmSetFragmentProgram(context, flat_fragment_program);
    timing_full();
}

static void timing_clear(void) {
    sceGxmSetVertexProgram(context, tmap_vertex_program);
    sceGxmSetFragmentProgram(context, import_clear_program);
    sceGxmSetFragmentTexture(context, import_clear_unit, timing_texture);
    timing_full();
}

static void timing_keep(void) {
    sceGxmSetVertexProgram(context, tmap_vertex_program);
    sceGxmSetFragmentProgram(context, import_keep_program);
    sceGxmSetFragmentTexture(context, import_keep_unit, timing_texture);
    timing_full();
}

static void timing_extract(void) {
    sceGxmSetVertexProgram(context, tmap_vertex_program);
    sceGxmSetFragmentProgram(context, extract_program);
    sceGxmSetFragmentTexture(context, extract_unit, timing_texture);
    timing_full();
}

// A full-screen textured polygon, the tables as its bitmap too, at light 0
static void timing_tmap(void) {
    sceGxmSetVertexProgram(context, tmap_vertex_program);
    sceGxmSetFragmentProgram(context, tmap_fragment_program);
    sceGxmSetFragmentTexture(context, bitmap_unit, &tables_texture);
    sceGxmSetFragmentTexture(context, tables_unit, &tables_texture);
    timing_full();
}

static void timing_side(void) {
    sceGxmSetVertexProgram(context, tmap_vertex_program);
    sceGxmSetFragmentProgram(context, side_fragment_program);
    sceGxmSetFragmentTexture(context, side_bitmap_unit, &tables_texture);
    sceGxmSetFragmentTexture(context, side_tables_unit, &tables_texture);
    timing_full();
}

// The screen's shader over the whole canvas: canvas 0's side buffer, its
// canvas through the check's palette, the first table of colours
static SceGxmTexture timing_p8;

static void timing_show(void) {
    display_draw(&timing_p8, &side_sets[0], &vertices[8], &indices[12]);
}

static int timing_compare(const void *a, const void *b) { return *(const int *)a - *(const int *)b; }

// One case: "name=min/median" added to `line`, -1/-1 if its scene won't begin
// (or there is nothing to draw it on).
static void timing_case(char *line, size_t size, const char *name, gpu_target *target, SceGxmColorSurface *surface,
                        void (*draws)(void)) {
    int us[TIMING_RUNS], i, n = 0;
    size_t at = strlen(line);

    for (i = 0; i < TIMING_RUNS && target != NULL && surface != NULL; i++) {
        long long t0 = sceKernelGetProcessTimeWide();
        int err = sceGxmBeginScene(context, 0, target->target, NULL, NULL, NULL, surface, &target->depth_stencil);

        if (err < 0) {
            gpu_err("smooth timing, %s: begin scene failed: 0x%08x", name, (unsigned)err);
            break;
        }
        scene_state();
        sceGxmSetVertexStream(context, 0, vertices);
        draws();
        sceGxmEndScene(context, NULL, NULL);
        sceGxmFinish(context);
        us[n++] = (int)(sceKernelGetProcessTimeWide() - t0);
    }
    if (n == 0) {
        snprintf(line + at, size - at, " %s=-1/-1", name);
        return;
    }
    qsort(us, (size_t)n, sizeof(us[0]), timing_compare);
    snprintf(line + at, size - at, " %s=%d/%d", name, us[0], us[n / 2]);
}

// The quad over the whole canvas, reading a texture tex_w by tex_h texel for
// pixel
static void timing_quad(int tex_w, int tex_h) {
    full_quad(&vertices[0], canvas_w, canvas_h, tex_w, tex_h);
}

static void smooth_timing(void) {
    int w = canvas_w, h = canvas_h, tiled_w = (canvas_w + 31) & ~31, tiled_h = (canvas_h + 31) & ~31, i;
    smooth_set *set = &side_sets[0];
    gpu_target *t8 = target_for(w, h, 0), *t32 = target_for(w, h, 1);
    SceGxmColorSurface surface8, tiled_surface, *tiled = NULL;
    SceGxmTexture tiled_texture;
    uchar *tiled_pixels = vgpu_alloc((size_t)tiled_w * tiled_h * 4);
    rastq_gpu_vertex v[4];
    char line[600] = "smooth timing us min/median:";

    gpu_log("smooth timing: start, %d runs a case", TIMING_RUNS);
    sceGxmFinish(context);
    to_clip_x = 2.0f / (float)w;
    to_clip_y = 2.0f / (float)h;
    // the one-pixel quad, palette index 0
    memset(v, 0, sizeof(v));
    v[1].x = v[2].x = 1.0f;
    v[2].y = v[3].y = 1.0f;
    for (i = 0; i < 4; i++) {
        place(&vertices[4 + i], &v[i]);
        vertices[4 + i].u = 0.25f / 255.0f;
    }
    for (i = 0; i < 2; i++) {
        uint16_t first = (uint16_t)(4 * i);
        indices[6 * i + 0] = indices[6 * i + 3] = first;
        indices[6 * i + 1] = first + 1;
        indices[6 * i + 2] = indices[6 * i + 4] = first + 2;
        indices[6 * i + 5] = first + 3;
    }
    if (sceGxmColorSurfaceInit(&surface8, SCE_GXM_COLOR_FORMAT_U8_R, SCE_GXM_COLOR_SURFACE_LINEAR,
                               SCE_GXM_COLOR_SURFACE_SCALE_NONE, SCE_GXM_OUTPUT_REGISTER_SIZE_32BIT, w, h,
                               canvas_stride, canvas_pixels[0]) < 0)
        t8 = NULL;
    if (tiled_pixels != NULL) {
        int err = sceGxmColorSurfaceInit(&tiled_surface, SCE_GXM_COLOR_FORMAT_U8U8U8U8_ABGR,
                                         SCE_GXM_COLOR_SURFACE_TILED, SCE_GXM_COLOR_SURFACE_SCALE_NONE,
                                         SCE_GXM_OUTPUT_REGISTER_SIZE_32BIT, w, h, tiled_w, tiled_pixels);
        if (err >= 0)
            err = sceGxmTextureInitTiled(&tiled_texture, tiled_pixels, SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR, tiled_w,
                                         tiled_h, 0);
        if (err < 0) {
            gpu_err("smooth timing: no tiled surface or texture: 0x%08x", (unsigned)err);
        } else {
            point_and_clamp(&tiled_texture);
            tiled = &tiled_surface;
        }
    }

    timing_quad(set->tex_w, set->tex_h);
    timing_case(line, sizeof(line), "empty8", t8, &surface8, timing_flat);
    timing_case(line, sizeof(line), "empty32", t32, &set->surface, timing_flat);
    timing_case(line, sizeof(line), "empty32t", t32, tiled, timing_flat);
    timing_texture = &set->canvas;
    timing_case(line, sizeof(line), "clear", t32, &set->surface, timing_clear);
    timing_case(line, sizeof(line), "flat32", t32, &set->surface, timing_flat_full);
    timing_case(line, sizeof(line), "keep", t32, &set->surface, timing_keep);
    timing_texture = &set->view;
    timing_case(line, sizeof(line), "extract", t8, &surface8, timing_extract);
    timing_quad(tiled_w, tiled_h);
    timing_texture = &tiled_texture;
    timing_case(line, sizeof(line), "extractt", t8, tiled != NULL ? &surface8 : NULL, timing_extract);
    timing_quad(set->tex_w, set->tex_h);
    timing_case(line, sizeof(line), "tmap8", t8, &surface8, timing_tmap);
    timing_case(line, sizeof(line), "tmap32", t32, &set->surface, timing_tmap);
    timing_case(line, sizeof(line), "side32", t32, &set->surface, timing_side);
    timing_case(line, sizeof(line), "side32t", t32, tiled, timing_side);
    // the screen's shader, into canvas 1's side buffer as if it were the screen
    if (display_on && side_pixels[1] != NULL &&
        sceGxmTextureInitLinear(&timing_p8, canvas_pixels[0], SCE_GXM_TEXTURE_FORMAT_P8_ABGR, set->tex_w, set->tex_h,
                                0) >= 0 &&
        sceGxmTextureSetPalette(&timing_p8, check_palette) >= 0) {
        point_and_clamp(&timing_p8);
        display_quad(&vertices[8], 0.0f, 0.0f, (float)w, (float)h, (float)w / (float)set->tex_w,
                     (float)h / (float)set->tex_h, w, h);
        for (i = 0; i < 4; i++)
            indices[12 + i] = (uint16_t)(8 + i);
        colour_turn = 0;
        timing_case(line, sizeof(line), "show", t32, &side_sets[1].surface, timing_show);
        colour_turn = -1;
    } else {
        timing_case(line, sizeof(line), "show", NULL, NULL, timing_show);
    }

    if (tiled_pixels != NULL)
        vgpu_free(tiled_pixels);
    // the canvas as it was made: the game's first frame starts from it
    for (i = 0; i < h; i++)
        memset(canvas_pixels[0] + (size_t)i * canvas_stride, 0, (size_t)w);
    gpu_log("%s", line);
}
#endif

// The GPU's memory, given back when it turns itself off after its set-up:
// nothing draws with it then (vgpu_canvas says no, and the queue never gets
// the hooks).
static void vgpu_release(void) {
    int i;

    sceGxmFinish(context);
    for (i = 0; i < MAX_TARGETS; i++)
        drop_target(&targets[i]);
    vgpu_free(vertices);
    vgpu_free(tables_pixels);
    vgpu_free(colours_pixels);
    vgpu_free(matrix_pixels);
    vgpu_free(texture_heap);
    if (colour_block != NULL)
        vgpu_free(colour_block);
    colour_block = NULL;
    vertices = NULL;
    indices = NULL;
    tables_pixels = colours_pixels = matrix_pixels = texture_heap = NULL;
    gpu_log("GPU off: its memory freed");
}

void vgpu_init(void) {
    size_t test_bytes = 2 * GPU_BLOCK_ALIGN;
    int bad, first_bad, got, kept, tex_bad[CHECK_BANDS] = {-1, -1, -1, -1};
    int colour_bad[COLOUR_BANDS] = {-1, -1, -1};
    int tluc_bad[TLUC_BANDS] = {-1, -1, -1, -1}, tluc_made = 0, way;
    int fill_in_place = -1, fill_copied = -1;
    int tlucbm_bad[TLUCBM_BANDS] = {-1, -1, -1, -1};
    int side_bad[SIDE_BANDS] = {-1, -1, -1, -1}, side_made = 0;
    int display_bad[DISPLAY_BANDS] = {-1, -1, -1};
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
        no_compiler = 1;
        gpu_err("no shader compiler: 0x%08x (is %s there?)", (unsigned)err, SHACCCG_PATH);
        snprintf(report, sizeof(report), "gpu: no shader compiler (0x%08x)", (unsigned)err);
        return;
    }
    gpu_log("compiling the flat shaders");
    if (!make_flat_programs()) {
        shark_end();
        snprintf(report, sizeof(report), "gpu: shaders failed, see gpu.txt");
        return;
    }

    // and room past the scenes' for the extract's quad (extract_scene)
    vertices = vgpu_alloc((MAX_VERTICES + 4) * sizeof(vgpu_vertex) + (MAX_INDICES + 6) * sizeof(uint16_t));
    if (vertices == NULL) {
        shark_end();
        snprintf(report, sizeof(report), "gpu: no memory for vertices");
        return;
    }
    indices = (uint16_t *)(vertices + MAX_VERTICES + 4);
    ready = 1;

    gpu_log("drawing the index check");
    bad = index_check(&first_bad, &got, &kept);
    if (bad != 0)
        gpu_err("index check drawn: %d wrong", bad);
    else
        gpu_log("index check drawn: %d wrong", bad);

    // Without the textured shaders the GPU isn't used (see below).
    gpu_log("compiling the textured shaders");
    textured = bad >= 0 && make_textures() && make_tmap_programs();
    // Without the shaded shaders the GPU still draws the station; what is
    // shaded between colours is then the CPU's, and so is cyberspace.
    // Without the translucent bitmaps' shaders those are the CPU's. Made here:
    // they take the textured ones' vertex program, the last one compiled.
    if (textured) {
        gpu_log("compiling the translucent bitmaps' shaders");
        if (make_tlucbm_programs()) {
            tlucbm_on = 1;
            gpu_log("drawing the translucent bitmap check");
            if (!tlucbm_check(tlucbm_bad) || tlucbm_bad[0] || tlucbm_bad[1] || tlucbm_bad[2] || tlucbm_bad[3])
                tlucbm_on = 0;
            gpu_log("translucent bitmap check drawn: %d %d %d %d wrong", tlucbm_bad[0], tlucbm_bad[1], tlucbm_bad[2],
                    tlucbm_bad[3]);
        }
        // The smooth shadows' shaders: the same vertex program again.
        if (shadows_wanted) {
            gpu_log("compiling the smooth shadows' shaders");
            side_made = make_side_programs() && make_single_pass_programs();
        }
    }
    if (textured) {
        gpu_log("compiling the shaded shaders");
        shades = make_shaded_programs();
        // Without the translucent ones either: those polygons are the CPU's.
        // One way at a time, each checked as soon as it compiles, up to the
        // first that draws right.
        gpu_log("compiling the translucent shaders");
        for (way = 0; way < TLUC_WAYS && tluc_way < 0; way++) {
            if (!make_tluc_program(way)) {
                if (tluc_vertex_program == NULL)
                    break; // their vertex shader: no way without it
                continue;
            }
            tluc_made++;
            tluc_way = way;
            gpu_log("drawing the translucency check, way %d", way);
            if (!tluc_check(tluc_bad) || tluc_bad[0] || tluc_bad[1] || tluc_bad[2] || tluc_bad[3])
                tluc_way = -1;
            gpu_log("translucency check drawn: %d %d %d %d wrong", tluc_bad[0], tluc_bad[1], tluc_bad[2],
                    tluc_bad[3]);
        }
    }
    shark_end();
    if (textured) {
        gpu_log("drawing the texture check");
        if (!texture_check(tex_bad))
            textured = 0;
        gpu_log("texture check drawn: %d %d %d %d wrong", tex_bad[0], tex_bad[1], tex_bad[2], tex_bad[3]);
    }
    // Any wrong pixel there, and the shadows are the original ones.
    if (textured && side_made) {
        gpu_log("drawing the side check");
        side_on = side_check(side_bad) && !side_bad[0] && !side_bad[1] && !side_bad[2] && !side_bad[3];
        gpu_log("side check drawn: %d %d %d %d wrong", side_bad[0], side_bad[1], side_bad[2], side_bad[3]);
    }
    // Without the screen's side the side buffers would be no use: the
    // shadows are the original ones.
    if (side_on) {
        gpu_log("drawing the display check");
        display_on = make_colour_tables() && display_check(display_bad) && !display_bad[0] && !display_bad[1] &&
                     !display_bad[2];
        gpu_log("display check drawn: %d %d %d wrong", display_bad[0], display_bad[1], display_bad[2]);
        if (!display_on)
            side_on = 0;
    }
    if (textured && shades) {
        gpu_log("drawing the colour check");
        if (!colour_check(colour_bad))
            shades = 0;
        gpu_log("colour check drawn: %d %d %d wrong", colour_bad[0], colour_bad[1], colour_bad[2]);
    }

    gpu_log("timing memory");
    gpu_mem = vgpu_alloc(test_bytes);
    ram_mem = malloc(test_bytes);
    if (gpu_mem != NULL)
        time_memory(gpu_mem, test_bytes, &gpu_write, &gpu_read);
    if (ram_mem != NULL)
        time_memory(ram_mem, test_bytes, &ram_write, &ram_read);
    if (gpu_mem != NULL)
        time_tluc_fill(gpu_mem, 64 * 1024, &fill_in_place, &fill_copied);
    if (gpu_mem != NULL)
        vgpu_free(gpu_mem);
    free(ram_mem);

    snprintf(report, sizeof(report),
             "gpu: ready index_check mismatches=%d/256 first_bad=%d got=%d undrawn_kept=%d | textures=%s "
             "texture_check plain=%d table=%d light=%d wrap=%d colour_check red=%d green=%d blue=%d (wrong columns "
             "of 256) | 512KB us: gpu_write=%d gpu_read=%d ram_write=%d ram_read=%d | tluc=%s:%d/%d "
             "tluc_check under=%d twice=%d kept=%d mixed=%d | 64KB tluc_fill us: in_place=%d copied=%d | "
             "tlucbm=%s tlucbm_check opaque=%d trans=%d table=%d twice=%d | side=%s side_check lit=%d plain=%d "
             "flat=%d tluc=%d display_check lit=%d mismatch=%d unmarked=%d",
             bad, first_bad, got, kept, !textured ? "off (GPU not used)" : shades ? "on" : "on, not the shaded ones",
             tex_bad[0], tex_bad[1], tex_bad[2],
             tex_bad[3], colour_bad[0], colour_bad[1], colour_bad[2], gpu_write, gpu_read, ram_write, ram_read,
             tluc_way >= 0 ? "on" : "off (CPU)", tluc_way, tluc_made, tluc_bad[0], tluc_bad[1], tluc_bad[2],
             tluc_bad[3], fill_in_place, fill_copied, tlucbm_on ? "on" : "off (CPU)", tlucbm_bad[0], tlucbm_bad[1],
             tlucbm_bad[2], tlucbm_bad[3], !shadows_wanted ? "off (original)" : side_on ? "on" : "off (failed)",
             side_bad[0], side_bad[1], side_bad[2], side_bad[3], display_bad[0], display_bad[1], display_bad[2]);
    // anything less than everything on is something wrong
    if (bad != 0 || !kept || !textured || !shades || tluc_way < 0 || !tlucbm_on || (shadows_wanted && !side_on))
        gpu_err("%s", report);
    else
        gpu_log("%s", report);
    // Flat colours were for finding out whether the GPU path could work at
    // all: as a renderer it is the textured shaders or nothing.
    if (bad < 0 || !textured) {
        ready = 0;
        vgpu_release();
        return;
    }
    if (!shades)
        queue_hooks.shaded = NULL;
    if (tluc_way < 0)
        queue_hooks.tluc = NULL;
    if (!tlucbm_on)
        queue_hooks.tluc_tmap = NULL;
    rastq_set_gpu(&queue_hooks);
}

const char *vgpu_report(void) { return report; }

int vgpu_available(void) { return ready && textured; }

int vgpu_shades(void) { return vgpu_available() && shades; }

int vgpu_compiler_missing(void) { return no_compiler; }

