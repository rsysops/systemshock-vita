// Row bands: see band.h.

#include "band.h"
#include "pertyp.h"

_Static_assert(LG_MAX_SLOTS == 3, "one initializer per slot below");

grs_band grd_bands[LG_MAX_SLOTS] = {
    {GR_BAND_TOP, GR_BAND_BOT},
    {GR_BAND_TOP, GR_BAND_BOT},
    {GR_BAND_TOP, GR_BAND_BOT},
};

#define FULL(band) ((band)->top == GR_BAND_TOP && (band)->bot == GR_BAND_BOT)

static int imax3(int a, int b, int c) {
    int m = a > b ? a : b;
    return m > c ? m : c;
}

// The canvas row of column x of an hscan line, as the scanline functions
// compute it. It never decreases along x if the slope is positive, and never
// increases if it is negative.
static int hscan_row(const grs_per_info *pi, int x) { return fix_int(x * pi->scan_slope + fix_make(pi->yp, 0xffff)); }

// The first column in [x0, x1) from which the rows are past `limit` in the
// line's direction of travel, or x1 if there is none.
static int hscan_first_past(const grs_per_info *pi, int x0, int x1, int limit) {
    while (x0 < x1) {
        int mid = x0 + (x1 - x0) / 2;
        int row = hscan_row(pi, mid);
        if (pi->scan_slope > 0 ? row >= limit : row < limit)
            x1 = mid;
        else
            x0 = mid + 1;
    }
    return x0;
}

int gr_band_hscan_miss(const grs_per_info *pi, const grs_band *band) {
    int x0 = pi->x, x1 = imax3(pi->xl, pi->xr0, pi->xr);
    int r0, r1;

    if (FULL(band) || x1 <= x0)
        return 0;
    r0 = hscan_row(pi, x0);
    r1 = hscan_row(pi, x1 - 1);
    return (r0 < band->top && r1 < band->top) || (r0 >= band->bot && r1 >= band->bot);
}

int gr_band_hscan(grs_per_info *pi, const grs_band *band, int lit) {
    int x0 = pi->x, x1 = imax3(pi->xl, pi->xr0, pi->xr);
    int r0, r1, first, end;

    if (FULL(band) || x1 <= x0)
        return 1;
    r0 = hscan_row(pi, x0);
    r1 = hscan_row(pi, x1 - 1);
    if ((r0 < band->top && r1 < band->top) || (r0 >= band->bot && r1 >= band->bot))
        return 0;
    if (r0 >= band->top && r0 < band->bot && r1 >= band->top && r1 < band->bot)
        return 1;

    // columns [first, end) are the ones whose row is in the band
    if (pi->scan_slope > 0) {
        first = hscan_first_past(pi, x0, x1, band->top);
        end = hscan_first_past(pi, first, x1, band->bot);
    } else {
        first = hscan_first_past(pi, x0, x1, band->bot);
        end = hscan_first_past(pi, first, x1, band->top);
    }
    if (pi->x < first) {
        if (lit)
            pi->i = gr_band_skip(pi->i, pi->di, first - pi->x);
        pi->x = first;
    }
    if (pi->xr > end)
        pi->xr = end;
    if (pi->xr0 > end)
        pi->xr0 = end;
    if (pi->xl > pi->xr0)
        pi->xl = pi->xr0;
    return first < end;
}

int gr_band_vscan_miss(const grs_per_info *pi, const grs_band *band) {
    return imax3(pi->yl, pi->yr0, pi->yr) <= band->top || pi->y >= band->bot;
}

int gr_band_vscan(grs_per_info *pi, const grs_band *band, int lit) {
    if (FULL(band))
        return 1;
    if (pi->y < band->top) {
        if (lit)
            pi->i = gr_band_skip(pi->i, pi->di, band->top - pi->y);
        pi->y = band->top;
    }
    if (pi->yr > band->bot)
        pi->yr = band->bot;
    if (pi->yr0 > band->bot)
        pi->yr0 = band->bot;
    if (pi->yl > pi->yr0)
        pi->yl = pi->yr0;
    return pi->y < imax3(pi->yl, pi->yr0, pi->yr);
}
