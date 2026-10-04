#ifndef __RASTQ_H
#define __RASTQ_H

// Records the calls that hand a finished 2D polygon to the pixel-filling
// mappers during a 3D pass, and replays them later in the same order, on one
// thread or on several that each draw a band of rows. See
// docs/PERFORMANCE-CPU.md, "Multicore rasterizer".

#include <stddef.h>

#include "2d.h"

typedef void (*rastq_tmap_func)(grs_bitmap *bm, int n, grs_vertex **vpl, grs_tmap_info *ti);

enum {
    RASTQ_OFF,          // draw directly
    RASTQ_COPY_ALL,     // record, copying every bitmap's pixels
    RASTQ_TRUST_STABLE, // record, pointing at pixels registered as stable
};

#define RASTQ_THREADS 3
// A view with fewer rows than this is better replayed by one thread. On the
// Vita that is the on-screen help's scan, a third of the screen's size in
// flat colours that change with almost every call (docs/PERFORMANCE-CPU.md).
#define RASTQ_SMALL_VIEW_ROWS 272

// What the GPU drew of a view, by kind of call
enum {
    RASTQ_GPU_KIND_FLAT,  // a polygon in one colour
    RASTQ_GPU_KIND_PLAIN, // a texture map, texels as they are
    RASTQ_GPU_KIND_CLUT,  // through a colour table
    RASTQ_GPU_KIND_LIT,   // through the light table, the level varying
    RASTQ_GPU_KINDS
};

// Why the CPU drew a call of a view the GPU draws
enum {
    RASTQ_GPU_WHY_TLUC_BITMAP, // a translucent bitmap
    RASTQ_GPU_WHY_SHADED_POLY, // a polygon shaded from corner to corner
    RASTQ_GPU_WHY_TLUC_POLY,   // a translucent polygon
    RASTQ_GPU_WHY_OTHER_POLY,  // another kind of polygon
    RASTQ_GPU_WHY_FILL,        // a fill type other than the normal one
    RASTQ_GPU_WHY_VERTS,       // too many vertices
    RASTQ_GPU_WHY_LIGHT,       // a light level outside the light table
    RASTQ_GPU_WHY_CLIP,        // the perspective mapper under a clip rectangle
    RASTQ_GPU_WHY_SIZE,        // sides not powers of two, outside the linear mapper
    RASTQ_GPU_WHY_OTHER,       // anything else, a full scene included
    RASTQ_GPU_WHYS
};

// Why a call was drawn by the main thread alone
enum {
    RASTQ_SOLO_MAPPER, // recorded, but its mapper doesn't honour row bands
    RASTQ_SOLO_DIRECT, // not recordable: drawn directly after a flush
    RASTQ_SOLO_REASONS
};

typedef struct {
    unsigned views;            // 3D passes, recorded or not
    unsigned cmds;             // calls recorded
    unsigned flushes;          // replays of a non-empty list
    unsigned long long copied; // pixel bytes copied
    unsigned check_runs;       // self-check comparisons made
    unsigned check_bad_rows;   // canvas rows that differed in them
    unsigned check_leak_rows;  // rows a thread wrote outside its band
    // Replays split across threads:
    unsigned batches;                   // runs of calls handed to all threads
    unsigned solo[RASTQ_SOLO_REASONS];  // calls the main thread drew alone
    unsigned long long wait_us;         // main thread waiting for the others
    unsigned long long busy_us[RASTQ_THREADS]; // each thread drawing
    unsigned late_us;                   // the longest a worker took to start
    int rows[RASTQ_THREADS + 1];        // the last split of the biggest view
    int cpu[RASTQ_THREADS];             // the core each thread last drew on
    // Replays handed to a GPU:
    unsigned gpu_scenes;                // scenes the GPU drew
    unsigned gpu_polys;                 // calls in them
    unsigned gpu_culled;                // calls dropped for their winding
    unsigned gpu_cpu_calls;             // calls of GPU views the CPU drew
    unsigned gpu_kinds[RASTQ_GPU_KINDS]; // the GPU's calls, by kind
    unsigned gpu_whys[RASTQ_GPU_WHYS];  // the CPU's, by reason
    unsigned gpu_pieces;                 // pieces lit calls were cut into
    unsigned gpu_fallbacks;             // lists it refused, drawn by the CPU
    unsigned long long gpu_submit_us;   // sending the scenes
    unsigned long long gpu_wait_us;     // waiting for the GPU to finish them
    unsigned long long gpu_cpu_us;      // the CPU drawing between scenes
    unsigned long long gpu_check_pixels; // pixels compared with the CPU's
    unsigned long long gpu_check_diff;  // of those, how many differed
} rastq_stats_t;

extern rastq_stats_t rastq_stats;

void rastq_set_mode(int mode);
// How many threads replay: 1 for the caller alone, up to RASTQ_THREADS.
// Returns the number in use, which is 1 where there are no worker threads.
int rastq_set_threads(int threads);
// Views with fewer canvas rows than this are replayed by the caller alone.
void rastq_set_min_rows(int rows);

// A GPU that can fill the recorded calls in place of the CPU. On Vita it is
// src/MacSrc/VitaGpu.c; see docs/PERFORMANCE-GPU.md.
//
// Every software texture mapper ends the same way: it picks a texel from the
// bitmap and passes it through a table of 256 entries, or through none. The
// queue hands the GPU that, per call: the bitmap, how its texels are
// addressed, and a row of the scene's tables.

// A vertex of a call. A pixel belongs to the polygon when the point at its
// integer coordinates is inside it, as for the mappers.
typedef struct {
    float x, y;    // canvas pixels
    float u, v, q; // texel coordinates times q, and q (1: no perspective)
    // The table row to pass the texel through is the integer part of
    //   (left + span * along / width) / depth
    // with each of the five taken as a linear function of the screen
    // position (not through the perspective division). It is how a mapper
    // has a light level: the value where its row, column or line of one
    // depth meets the polygon's left edge, what the right edge has more,
    // how far along the pixel is, and the w the floor and wall mappers
    // divide by. One row for the whole polygon is (row + 0.5, 0, 0, 1, 1).
    float left, span, along, width, depth;
} rastq_gpu_vertex;

// How the texel at (u, v), both rounded down, is found
enum {
    RASTQ_GPU_TRANS = 1, // texel 0 leaves the pixel as it is
    // Both sides of the bitmap are powers of two, and u and v each wrap.
    // Without this they stay inside the bitmap, or are clamped to it.
    // (The row, floor and wall mappers move on to the next row where u
    // passes the width: the queue cuts such polygons at those places and
    // hands over parts whose v says so.)
    RASTQ_GPU_WRAP = 2,
};

// The tables of a scene: rows of 256 palette indices. The first rows are
// the light table, the next leaves a texel as it is, the rest are what the
// scene's calls need.
#define RASTQ_GPU_VERTS 16
#define RASTQ_GPU_LIGHT_ROWS 16
#define RASTQ_GPU_PLAIN_ROW 16
#define RASTQ_GPU_TABLE_ROWS 64

typedef struct {
    // Starts a scene on a canvas, with its tables (`rows` of them, valid
    // until the scene ends). 0 if it can't draw into that canvas.
    int (*begin)(uchar *bits, int w, int h, int row, const uchar *tables, int rows);
    // A polygon in one palette index. 0: no room left in this scene.
    int (*flat)(int n, const rastq_gpu_vertex *v, int color);
    // A texture-mapped polygon of at most RASTQ_GPU_VERTS vertices; the
    // bitmap's pixels are valid until the scene ends. 0: no room left in
    // this scene.
    int (*tmap)(const grs_bitmap *bm, int flags, int n, const rastq_gpu_vertex *v);
    // Ends the scene and returns once the canvas holds the result.
    void (*end)(void);
    // Told of each comparison of its result with the CPU's, and how many
    // pixels differed. May be NULL.
    void (*compared)(const uchar *gpu, const uchar *cpu, int w, int h, int row, unsigned differing);
} rastq_gpu;

// Offers a GPU to the queue (NULL: none), and says whether to use it.
void rastq_set_gpu(const rastq_gpu *gpu);
void rastq_use_gpu(int on);
// Will a view of that many rows be handed to the GPU? Its caller then gives
// it a canvas the GPU can draw into.
int rastq_gpu_next(int rows);
// In place of gr_clear before a view: if the GPU is to draw the view that
// rastq_begin starts next on the current canvas, the clear becomes the first
// thing in its scene and this returns 1. Otherwise it does nothing and
// returns 0.
int rastq_gpu_clear(int color);
// For a view the GPU is kept out of: has the view that rastq_begin starts
// next count, in gpu_kinds and gpu_whys, what the GPU could draw of it.
void rastq_gpu_survey(void);

// Pixels that don't change between a draw call and the end of its pass.
void rastq_stable_pixels(const uchar *pixels, size_t size);

// Bracket one view's 3D pass on the current canvas. rastq_end flushes.
void rastq_begin(void);
void rastq_end(void);
// Draw everything recorded so far. Call before drawing any other way.
void rastq_flush(void);

// The handoff calls: each draws directly or records.
void rastq_tmap(rastq_tmap_func func, grs_bitmap *bm, int n, grs_vertex **vpl, grs_tmap_info *ti);
void rastq_poly(int index, long color, int n, grs_vertex **vpl);
void rastq_hmap(grs_bitmap *bm, int n, grs_vertex **vpl, grs_tmap_info *ti);

#ifdef RASTQ_SELFCHECK
// Every nth recorded view also draws directly and compares the two results.
void rastq_set_check_interval(unsigned n);
// For tests: replay on the calling thread, band after band. `bounds` holds
// the bands - 1 rows where one band ends and the next begins. 0 bands
// returns to the normal replay.
void rastq_test_bands(int bands, const int *bounds);
// For tests: check that each recorded call, drawn alone, stays inside the
// rows recorded for it. Violations count as check_leak_rows.
void rastq_test_ranges(int on);
#endif

#endif // __RASTQ_H
