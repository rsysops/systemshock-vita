#ifndef __HUDKEEP_H
#define __HUDKEEP_H

// Pictures kept of what the HUD draws identically frame after frame, to
// copy in place of drawing it again. A kept picture gives the pixels the
// drawing would: where that can't be said, these draw nothing and return 0,
// and the caller draws as it always did. See docs/PERFORMANCE-GPU.md, step
// G9.

#include "2d.h"

// The string s outlined: in the colour `shadow` at the eight places around
// (x, y), then in the canvas's colour at (x, y), each with ss_scale_string.
// x and y are canvas pixels. Returns 1 if it drew that.
int hudkeep_outlined(char *s, short x, short y, uchar shadow);

// What gr_scale_bitmap(bm, x, y, w, h) draws. Returns 1 if it drew that.
int hudkeep_scaled(grs_bitmap *bm, short x, short y, short w, short h);

typedef struct {
    unsigned text_hits, text_misses;     // outlined strings copied, and drawn into a picture
    unsigned scaled_hits, scaled_misses; // the same for stretched bitmaps
    unsigned checks, check_bad;          // copies compared with the drawing, and those that differed
} hudkeep_stats_t;
extern hudkeep_stats_t hudkeep_stats;

#endif // __HUDKEEP_H
