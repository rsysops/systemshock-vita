#ifndef __BAND_H
#define __BAND_H

// Row bands. The mappers only write canvas rows top <= y < bot of the calling
// thread's band; everything else they do (edge walks, per-row and per-column
// steps) is unchanged, so several threads drawing the same call with adjacent
// bands produce exactly the pixels of one full pass. The band covers every
// row unless the rasterizer queue (3D/Source/rastq.c) is replaying on several
// threads. See docs/PERFORMANCE.md, "Row bands, bit-exact".

#include "fix.h"
#include "lgslot.h"

#define GR_BAND_TOP (-0x40000000)
#define GR_BAND_BOT 0x40000000

typedef struct {
    int top, bot;
} grs_band;

extern grs_band grd_bands[LG_MAX_SLOTS];

// The calling thread's band
#define gr_band() (&grd_bands[lg_slot()])

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
