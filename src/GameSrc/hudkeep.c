// Pictures kept of what the HUD draws identically frame after frame: see
// hudkeep.h.

#include <stdlib.h>
#include <string.h>

#include "hudkeep.h"
#include "gr2ss.h"
#include "vprof.h"

hudkeep_stats_t hudkeep_stats;

// In the profile build, one copy in this many is also drawn the old way and
// the two compared: about one frame in 64 for a text, and one in 40 for the
// stretched bitmaps, which come some fifty to a frame (the count isn't a
// multiple of that, so the check goes round them).
#define CHECK_TEXT_EVERY 64
#define CHECK_SCALED_EVERY 2039

static unsigned long keep_clock; // counts uses, to find the one unused the longest

static int fill_is_normal(void) { return gr_get_fill_type() == FILL_NORM; }

#ifdef VITA_PROFILE_CHECKS
#define CHECK_MARGIN 8
#define CHECK_BYTES (1100 * 160)
static uchar check_before[CHECK_BYTES], check_copied[CHECK_BYTES];

// The part of the canvas a check looks at: the picture's place and a margin
// around it, for a drawing that goes further than its picture.
typedef struct {
    int x, y, w, h;
} check_area;

static int check_start(check_area *a, unsigned *count, unsigned every, int x, int y, int w, int h) {
    if (++*count % every != 0)
        return 0;
    a->x = x - CHECK_MARGIN;
    a->y = y - CHECK_MARGIN;
    a->w = w + 2 * CHECK_MARGIN;
    a->h = h + 2 * CHECK_MARGIN;
    if (a->x < 0) {
        a->w += a->x;
        a->x = 0;
    }
    if (a->y < 0) {
        a->h += a->y;
        a->y = 0;
    }
    if (a->x + a->w > grd_bm.w)
        a->w = grd_bm.w - a->x;
    if (a->y + a->h > grd_bm.h)
        a->h = grd_bm.h - a->y;
    if (a->w <= 0 || a->h <= 0 || a->w * a->h > CHECK_BYTES)
        return 0;
    return 1;
}

static void area_get(const check_area *a, uchar *to) {
    int row;
    for (row = 0; row < a->h; row++)
        memcpy(to + (size_t)row * a->w, grd_bm.bits + (size_t)(a->y + row) * grd_bm.row + a->x, (size_t)a->w);
}

static void area_put(const check_area *a, const uchar *from) {
    int row;
    for (row = 0; row < a->h; row++)
        memcpy(grd_bm.bits + (size_t)(a->y + row) * grd_bm.row + a->x, from + (size_t)row * a->w, (size_t)a->w);
}

// Before the copy: remembers the canvas. After it: takes the copy's result
// away again, for the caller to draw the old way, then check_end compares.
static void check_copied_now(const check_area *a) {
    area_get(a, check_copied);
    area_put(a, check_before);
}

static void check_end(const check_area *a) {
    int row, bad = 0;
    for (row = 0; row < a->h && !bad; row++)
        bad = memcmp(grd_bm.bits + (size_t)(a->y + row) * grd_bm.row + a->x, check_copied + (size_t)row * a->w,
                     (size_t)a->w) != 0;
    hudkeep_stats.checks++;
    hudkeep_stats.check_bad += bad;
    // a checked frame draws things twice: keep it out of the timings
#if defined(__vita__) && defined(VITA_PROFILE)
    vprof_frame_discard();
#endif
}
#endif

// ---- outlined text ----------------------------------------------------------

// A text is drawn into this, then the part it covers is kept. It is all 0
// between two texts.
#define TEXT_W 1024
#define TEXT_H 128
#define TEXT_AT 2 // where in it
#define TEXT_CHARS 256
// Room for the texts of the side panels, the inventory and the HUD together:
// a page isn't redrawn every frame, and its texts must still be here when
// it is.
#define TEXT_KEPT 256
// A text that changes all the time (a counter) leaves a picture for each of
// its values, never asked for again. Beyond this many, those are the ones a
// new text replaces, so that they don't push out the texts asked for again
// and again. It must stay well above what a page shows at once (35 to 55
// texts in the capture of step S5): a page's texts have all been asked for
// once when it is first drawn, and with fewer allowed they replace one
// another at every redraw.
#define TEXT_ONCE 128

typedef struct {
    int used;
    unsigned long last;
    unsigned hash; // of s
    char s[TEXT_CHARS];
    grs_font *font;
    ushort font_id;
    short font_w, font_h, font_min, font_max; // of the font, in case another takes its address
    long color;
    uchar shadow;
    char mode, type; // the screen conversion in force
    int again;       // asked for more than once
    int direct;      // too large to keep: drawn the old way
    grs_bitmap bm;   // the part covered, transparent elsewhere
    short dx, dy;    // where it goes from the string's x, y
} kept_text;

static kept_text texts[TEXT_KEPT];
static uchar *text_bits;
static grs_canvas text_canvas;

#ifdef VITA_PROFILE_CHECKS
// The nine draws a picture stands for, to hold it against.
static void outline(char *s, short x, short y, uchar shadow, long color) {
    gr_set_fcolor(shadow);
    ss_scale_string(s, x - 1, y - 1);
    ss_scale_string(s, x, y - 1);
    ss_scale_string(s, x + 1, y - 1);
    ss_scale_string(s, x, y + 1);
    ss_scale_string(s, x - 1, y + 1);
    ss_scale_string(s, x + 1, y + 1);
    ss_scale_string(s, x - 1, y);
    ss_scale_string(s, x + 1, y);
    gr_set_fcolor(color);
    ss_scale_string(s, x, y);
}
#endif

// What is drawn of the scratch canvas between (xa, ya) and (xb, yb), the
// latter excluded. Returns 0 if nothing is.
static int text_bounds(int xa, int ya, int xb, int yb, int *x0, int *y0, int *x1, int *y1) {
    int x, y;

    *x0 = xb;
    *y0 = yb;
    *x1 = *y1 = -1;
    for (y = ya; y < yb; y++) {
        const uchar *row = text_bits + y * TEXT_W;
        for (x = xa; x < xb; x++) {
            if (row[x] == 0)
                continue;
            if (x < *x0)
                *x0 = x;
            if (x > *x1)
                *x1 = x;
            if (y < *y0)
                *y0 = y;
            *y1 = y;
        }
    }
    return *x1 >= 0;
}

// The nine draws' pixels from one draw: `from` is the text drawn once, w x h
// with `row` bytes to a line and 0 where there is nothing. `to`, (w + 2) x
// (h + 2), gets it with one pixel more on every side: the text, and the
// outline's colour on every empty pixel that touches it, which is where one
// of the eight draws around would have put it.
static void text_grow(const uchar *from, int row, int w, int h, uchar *to, uchar shadow) {
    int to_w = w + 2, x, y;

    memset(to, 0, (size_t)to_w * (h + 2));
    for (y = 0; y < h; y++) {
        const uchar *f = from + (size_t)y * row;
        uchar *above = to + (size_t)y * to_w, *level = above + to_w, *below = level + to_w;
        for (x = 0; x < w; x++) {
            if (f[x] == 0)
                continue;
            above[x] = above[x + 1] = above[x + 2] = shadow;
            level[x] = level[x + 1] = level[x + 2] = shadow;
            below[x] = below[x + 1] = below[x + 2] = shadow;
        }
    }
    for (y = 0; y < h; y++) {
        const uchar *f = from + (size_t)y * row;
        uchar *level = to + (size_t)(y + 1) * to_w + 1;
        for (x = 0; x < w; x++)
            if (f[x] != 0)
                level[x] = f[x];
    }
}

// Makes the text's picture: draws it once into the scratch canvas and keeps
// what it covers, with its outline grown around it. Each of the nine draws
// is that one moved by a pixel, so they need not be made.
static void text_make(kept_text *t, char *s) {
    short sw, sh; // how much the text covers from where its drawing starts
    int x0, y0, x1, y1, w, h, y, same_font;
    uchar *bits;

    t->direct = 0;
    gr_init_bitmap(&t->bm, NULL, BMT_FLAT8, BMF_TRANS, 0, 0);

    gr_push_canvas(&text_canvas);
    gr_set_fill_type(FILL_NORM);
    gr_set_cliprect(0, 0, TEXT_W, TEXT_H);
    gr_set_font(t->font);
    gr_set_fcolor(t->color);
    ss_scale_string_sized(s, TEXT_AT, TEXT_AT, &sw, &sh);
    same_font = gr_get_font() == t->font;
    gr_pop_canvas();

    // Left to be drawn the old way:
    // - a text of which it can't be said how far it goes;
    // - one that hasn't all its room here, the outline's pixel around it
    //   too: what goes past an edge is cut, or not drawn;
    // - one whose draw leaves another font set for the next (ss_scale_string
    //   would with the medium LED font, were it not a font of colours): its
    //   nine draws aren't one draw moved.
    if (sw < 0 || TEXT_AT + sw + 1 > TEXT_W || TEXT_AT + sh + 1 > TEXT_H || !same_font) {
        memset(text_bits, 0, TEXT_W * TEXT_H);
        t->direct = 1;
        return;
    }

    if (text_bounds(TEXT_AT, TEXT_AT, TEXT_AT + sw, TEXT_AT + sh, &x0, &y0, &x1, &y1)) {
        // the picture has the outline's pixel more on every side
        w = x1 - x0 + 1;
        h = y1 - y0 + 1;
        bits = malloc((size_t)(w + 2) * (h + 2));
        if (bits == NULL)
            t->direct = 1;
        else {
            text_grow(text_bits + (size_t)y0 * TEXT_W + x0, TEXT_W, w, h, bits, t->shadow);
            gr_init_bitmap(&t->bm, bits, BMT_FLAT8, BMF_TRANS, (short)(w + 2), (short)(h + 2));
            t->dx = (short)(x0 - 1 - TEXT_AT);
            t->dy = (short)(y0 - 1 - TEXT_AT);
        }
    } // else nothing drawn, nothing to copy

    for (y = TEXT_AT; y < TEXT_AT + sh; y++)
        memset(text_bits + (size_t)y * TEXT_W + TEXT_AT, 0, (size_t)sw);
}

int hudkeep_outlined(char *s, short x, short y, uchar shadow) {
    grs_font *font = gr_get_font();
    long color = gr_get_fcolor();
    kept_text *t = NULL, *spare = NULL, *oldest = NULL, *oldest_once = NULL;
    unsigned hash = 5381;
    size_t length;
    int i, once = 0;

    // Colour 0 is the picture's "nothing here", and a font with colours of
    // its own isn't drawn in the outline's.
    if (shadow == 0 || (color & 0xff) == 0 || font == NULL || font->id == 0xcccc || !fill_is_normal())
        return 0;
    for (length = 0; s[length] != '\0'; length++)
        hash = hash * 33 + (uchar)s[length];
    if (length >= TEXT_CHARS)
        return 0;
    if (text_bits == NULL) {
        text_bits = calloc(TEXT_W, TEXT_H);
        if (text_bits == NULL)
            return 0;
        gr_init_canvas(&text_canvas, text_bits, BMT_FLAT8, TEXT_W, TEXT_H);
    }

    for (i = 0; i < TEXT_KEPT; i++) {
        kept_text *k = &texts[i];
        if (!k->used) {
            if (spare == NULL)
                spare = k;
            continue;
        }
        if (k->hash == hash && k->font == font && k->font_id == font->id && k->font_w == font->w &&
            k->font_h == font->h && k->font_min == font->min && k->font_max == font->max && k->color == color &&
            k->shadow == shadow && k->mode == convert_use_mode && k->type == convert_type && strcmp(k->s, s) == 0) {
            t = k;
            break;
        }
        if (oldest == NULL || k->last < oldest->last)
            oldest = k;
        if (!k->again) {
            once++;
            if (oldest_once == NULL || k->last < oldest_once->last)
                oldest_once = k;
        }
    }
    if (t == NULL) {
        t = spare != NULL ? spare : once > TEXT_ONCE ? oldest_once : oldest;
        if (t->used)
            free(t->bm.bits);
        t->used = 1;
        t->hash = hash;
        memcpy(t->s, s, length + 1);
        t->font = font;
        t->font_id = font->id;
        t->font_w = font->w;
        t->font_h = font->h;
        t->font_min = font->min;
        t->font_max = font->max;
        t->color = color;
        t->shadow = shadow;
        t->mode = convert_use_mode;
        t->type = convert_type;
        t->again = 0;
        // Made at its first asking: a picture costs less than the nine draws
        // it replaces, even for a text never seen again.
        text_make(t, s);
        hudkeep_stats.text_misses++;
    } else {
        t->again = 1;
        if (t->direct)
            hudkeep_stats.text_misses++;
        else
            hudkeep_stats.text_hits++;
    }
    t->last = ++keep_clock;
    if (t->direct)
        return 0;
    if (t->bm.bits == NULL)
        return 1;

#ifdef VITA_PROFILE_CHECKS
    {
        static unsigned count;
        check_area a;
        if (check_start(&a, &count, CHECK_TEXT_EVERY, x + t->dx, y + t->dy, t->bm.w, t->bm.h)) {
            area_get(&a, check_before);
            gr_bitmap(&t->bm, x + t->dx, y + t->dy);
            check_copied_now(&a);
            outline(s, x, y, shadow, color);
            check_end(&a);
            return 1;
        }
    }
#endif
    gr_bitmap(&t->bm, x + t->dx, y + t->dy);
    return 1;
}

// ---- stretched bitmaps ------------------------------------------------------

#define SCALED_KEPT 24
#define SCALED_SOURCE_BYTES 4096 // larger bitmaps are stretched every time
#define SCALED_BYTES 32768

typedef struct {
    int used;
    unsigned long last;
    short w, h; // of the source
    ushort flags;
    uchar *source;  // its pixels, w to a row
    grs_bitmap out; // it stretched
} kept_scaled;

static kept_scaled scaled[SCALED_KEPT];

static int same_source(const kept_scaled *k, const grs_bitmap *bm) {
    int row;
    for (row = 0; row < bm->h; row++)
        if (memcmp(k->source + (size_t)row * bm->w, bm->bits + (size_t)row * bm->row, (size_t)bm->w) != 0)
            return 0;
    return 1;
}

// Stretches the bitmap into a picture of its own. 0: no memory for it.
static int scaled_make(kept_scaled *k, grs_bitmap *bm, short w, short h) {
    grs_canvas canvas;
    uchar *bits = malloc((size_t)w * h), *source = malloc((size_t)bm->w * bm->h);
    int row;

    if (bits == NULL || source == NULL) {
        free(bits);
        free(source);
        return 0;
    }
    for (row = 0; row < bm->h; row++)
        memcpy(source + (size_t)row * bm->w, bm->bits + (size_t)row * bm->row, (size_t)bm->w);
    memset(bits, 0, (size_t)w * h);
    gr_init_canvas(&canvas, bits, BMT_FLAT8, w, h);
    gr_push_canvas(&canvas);
    gr_set_fill_type(FILL_NORM);
    gr_set_cliprect(0, 0, w, h);
    gr_scale_bitmap(bm, 0, 0, w, h);
    gr_pop_canvas();

    k->w = bm->w;
    k->h = bm->h;
    k->flags = bm->flags;
    k->source = source;
    gr_init_bitmap(&k->out, bits, BMT_FLAT8, bm->flags, w, h);
    return 1;
}

int hudkeep_scaled(grs_bitmap *bm, short x, short y, short w, short h) {
    kept_scaled *k = NULL, *oldest = &scaled[0];
    int i;

    // Only what is sure to give the same pixels: a plain 8-bit bitmap, at
    // most transparent, landing whole inside the clip rectangle (a bitmap
    // cut by an edge isn't stretched the same way).
    if (bm->type != BMT_FLAT8 || (bm->flags & ~BMF_TRANS) != 0 || bm->bits == NULL || bm->w <= 0 || bm->h <= 0 ||
        w <= 0 || h <= 0 || (int)bm->w * bm->h > SCALED_SOURCE_BYTES || (int)w * h > SCALED_BYTES ||
        !fill_is_normal() || x < gr_get_clip_l() || y < gr_get_clip_t() || x + w > gr_get_clip_r() ||
        y + h > gr_get_clip_b())
        return 0;

    for (i = 0; i < SCALED_KEPT; i++) {
        kept_scaled *c = &scaled[i];
        if (!c->used) {
            oldest = c;
            oldest->last = 0;
            continue;
        }
        if (c->w == bm->w && c->h == bm->h && c->flags == bm->flags && c->out.w == w && c->out.h == h &&
            same_source(c, bm)) {
            k = c;
            break;
        }
        if (oldest->used && c->last < oldest->last)
            oldest = c;
    }
    if (k == NULL) {
        k = oldest;
        if (k->used) {
            free(k->source);
            free(k->out.bits);
            k->used = 0;
        }
        if (!scaled_make(k, bm, w, h))
            return 0;
        k->used = 1;
        hudkeep_stats.scaled_misses++;
    } else
        hudkeep_stats.scaled_hits++;
    k->last = ++keep_clock;

#ifdef VITA_PROFILE_CHECKS
    {
        static unsigned count;
        check_area a;
        if (check_start(&a, &count, CHECK_SCALED_EVERY, x, y, w, h)) {
            area_get(&a, check_before);
            gr_bitmap(&k->out, x, y);
            check_copied_now(&a);
            gr_scale_bitmap(bm, x, y, w, h);
            check_end(&a);
            return 1;
        }
    }
#endif
    gr_bitmap(&k->out, x, y);
    return 1;
}
