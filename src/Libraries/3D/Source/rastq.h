#ifndef __RASTQ_H
#define __RASTQ_H

// Records the calls that hand a finished 2D polygon to the pixel-filling
// mappers during a 3D pass, and replays them later in the same order. See
// docs/PERFORMANCE.md, "Record, then replay".

#include <stddef.h>

#include "2d.h"

typedef void (*rastq_tmap_func)(grs_bitmap *bm, int n, grs_vertex **vpl, grs_tmap_info *ti);

enum {
    RASTQ_OFF,          // draw directly
    RASTQ_COPY_ALL,     // record, copying every bitmap's pixels
    RASTQ_TRUST_STABLE, // record, pointing at pixels registered as stable
};

typedef struct {
    unsigned views;           // passes recorded
    unsigned cmds;            // calls recorded
    unsigned flushes;         // replays of a non-empty list
    unsigned long long copied; // pixel bytes copied
    unsigned check_runs;      // self-check comparisons made
    unsigned check_bad_rows;  // canvas rows that differed in them
} rastq_stats_t;

extern rastq_stats_t rastq_stats;

void rastq_set_mode(int mode);
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
#endif

#endif // __RASTQ_H
