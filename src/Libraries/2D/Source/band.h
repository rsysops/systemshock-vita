#ifndef __BAND_H
#define __BAND_H

// Row bands. The mappers only write canvas rows top <= y < bot of the calling
// thread's band; everything else they do (edge walks, per-row and per-column
// steps) is unchanged, so several threads drawing the same call with adjacent
// bands produce exactly the pixels of one full pass. The band covers every
// row unless the rasterizer queue (3D/Source/rastq.c) is replaying on several
// threads. See docs/PERFORMANCE.md, "Row bands, bit-exact".
//
// Threads that draw different calls of a list at the same time can't share
// the canvas's fill state and clip, so a band can carry those for its thread.
// The mappers then use the band's instead of the canvas's.

#include <stdint.h>

#include "fix.h"
#include "grs.h"
#include "lgslot.h"

#define GR_BAND_TOP (-0x40000000)
#define GR_BAND_BOT 0x40000000

typedef struct {
    int top, bot;
    // The thread's own drawing state, used instead of the canvas's when
    // own_state is set:
    int own_state;
    int32_t fill_type;      // as grd_gc.fill_type
    intptr_t fill_parm;     // as grd_gc.fill_parm
    void (**table)();       // as grd_function_table, for that fill type
    const grs_clip *clip;   // as &grd_gc.clip
} grs_band;

extern grs_band grd_bands[LG_MAX_SLOTS];

// The calling thread's band
#define gr_band() (&grd_bands[lg_slot()])

// The drawing state a mapper called with band b must use
#define gr_band_fill_type(b) ((b)->own_state ? (b)->fill_type : grd_gc.fill_type)
#define gr_band_fill_parm(b) ((b)->own_state ? (b)->fill_parm : grd_gc.fill_parm)
#define gr_band_table(b) ((b)->own_state ? (b)->table : grd_function_table)
#define gr_band_clip(b) ((b)->own_state ? (b)->clip : &grd_gc.clip)

// Is canvas row y in the band a mapper call was given? (tli: its loop info)
#define gr_row_in_band(tli, y) ((y) >= (tli)->band_top && (y) < (tli)->band_bot)

// x + n*dx as n wrapping additions would give it
#define gr_band_skip(x, dx, n) ((fix)((uint32_t)(x) + (uint32_t)(dx) * (uint32_t)(n)))

// Does the mapper this entry of grd_tmap_init_table sets up honour the band?
// Calls through any other entry must be made with a band covering every row.
int gr_band_safe_init(void (*init)());

// The perspective mappers draw slanted scanlines (pertyp.h). hscan lines
// advance along x and drift in y; vscan lines advance along y.
struct grs_per_info_s;

// Does a scanline about to be clipped miss the band entirely? It then needs
// none of its clipping divisions.
int gr_band_hscan_miss(const struct grs_per_info_s *pi, const grs_band *band);
int gr_band_vscan_miss(const struct grs_per_info_s *pi, const grs_band *band);

// Narrows a clipped scanline to the pixels whose row is in the band, stepping
// the lighting over the skipped ones if lit. Returns 0 if none are left.
int gr_band_hscan(struct grs_per_info_s *pi, const grs_band *band, int lit);
int gr_band_vscan(struct grs_per_info_s *pi, const grs_band *band, int lit);

#endif // __BAND_H
