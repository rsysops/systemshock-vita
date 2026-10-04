#ifndef __RASTQ_H
#define __RASTQ_H

// Records the calls that hand a finished 2D polygon to the pixel-filling
// mappers during a 3D pass, and replays them later in the same order, on one
// thread or on several that each draw a band of rows. See
// docs/PERFORMANCE.md, "Multicore rasterizer".

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
// flat colours that change with almost every call (docs/PERFORMANCE.md).
#define RASTQ_SMALL_VIEW_ROWS 272

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
} rastq_stats_t;

extern rastq_stats_t rastq_stats;

void rastq_set_mode(int mode);
// How many threads replay: 1 for the caller alone, up to RASTQ_THREADS.
// Returns the number in use, which is 1 where there are no worker threads.
int rastq_set_threads(int threads);
// Views with fewer canvas rows than this are replayed by the caller alone.
void rastq_set_min_rows(int rows);

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
