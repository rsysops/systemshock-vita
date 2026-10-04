// Checks that recording the rasterizer's draw calls and replaying them
// (src/Libraries/3D/Source/rastq.c) gives the same pixels as drawing them
// directly: on one thread, band after band of rows, and on three threads that
// each draw one band. Random scenes go through every mapper family the game
// uses, with the argument storage reused between calls the way the game
// reuses it.
//
// Usage: rastq_test [frames] [seed]
// With RASTQ_HASH set it only draws directly and prints a checksum per frame,
// to compare two builds of the libraries. RASTQ_REFERENCE builds it for
// libraries from before row bands, where that is all it can do.

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "2d.h"
#include "fix.h"
#include "lg.h"
#include "memall.h"
#include "rastq.h"
#include "tmapfcn.h"
#include "tmpalloc.h"

// What the 2D library expects from the game
intptr_t *gScreenAddress;
int32_t gScreenRowbytes;
void SetSDLPalette(int index, int count, uchar *pal) {
    (void)index;
    (void)count;
    (void)pal;
}

extern void gr_null(void);
extern void gr_not_imp(void);

#define MAX_OPS 96
#define MAX_VERTS 8
#define PAD 65536 // some mappers read a texel just outside a bitmap, on either side
#define UNPACK_BYTES 111616
#define TEMP_BYTES 65536
#define SCRATCH_BYTES 131072

enum { OP_TMAP, OP_SPRITE, OP_POLY, OP_RECT };
// Where a bitmap's pixels are when the call is made
enum {
    SRC_POOL,     // texture memory that never changes: registered as stable
    SRC_SCRATCH,  // a buffer every such call overwrites first
    SRC_UNPACKED, // unpacked from RSD by the caller into the unpack buffer
    SRC_RSD,      // still RSD-compressed: the mapper unpacks it
};

typedef struct {
    int op;
    int fill_type;
    intptr_t fill_parm;
    short clip[4];
    int n;
    grs_vertex v[MAX_VERTS];
    rastq_tmap_func func;
    grs_tmap_info ti;
    grs_bitmap bm;
    int src;
    const uchar *pixels; // SRC_SCRATCH: what to put in the scratch buffer
    size_t pixel_bytes;
    grs_bitmap rsd;      // SRC_UNPACKED: what to unpack
    int index;
    long color;
    int rx, ry, rw, rh;
} op_t;

typedef struct {
    grs_bitmap flat;  // pixels in the pool
    grs_bitmap rsd;   // the same picture, RSD-compressed
} picture_t;

static uint64_t rng_state;
static uint32_t rnd(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return (uint32_t)(rng_state >> 16);
}
static int rnd_in(int lo, int hi) { return lo + (int)(rnd() % (uint32_t)(hi - lo + 1)); }
// A second generator for how a scene is replayed, so that adding a replay
// test doesn't change the scenes a seed gives.
static uint64_t aux_state = 0x2545F4914F6CDD1DULL;
static int aux_in(int lo, int hi) {
    aux_state ^= aux_state << 13;
    aux_state ^= aux_state >> 7;
    aux_state ^= aux_state << 17;
    return lo + (int)((uint32_t)(aux_state >> 16) % (uint32_t)(hi - lo + 1));
}
static double rnd_f(double lo, double hi) { return lo + (hi - lo) * (rnd() / 4294967295.0); }

static grs_screen screen;
static grs_canvas canvas;
static int cw, ch;
#define RESULTS 5
static uchar *canvas_bits, *background, *result[RESULTS];
static uchar *ltab, *ipal, *stab, *unpack, *scratch;
static uchar tluc_tables[32][256];
static MemStack temp_stack;
static uchar *temp_mem;

#define N_TEXTURES 10
#define N_SPRITES 8
static picture_t textures[N_TEXTURES];
static picture_t sprites[N_SPRITES];

// The game hands the mappers pointers into a few reused globals.
static grs_vertex work_v[MAX_VERTS];
static grs_vertex *work_vpl[MAX_VERTS];
static grs_bitmap work_bm;
static grs_tmap_info work_ti;

static void fill_random(uchar *p, size_t n) {
    size_t i;
    for (i = 0; i < n; i++)
        p[i] = (uchar)rnd();
}

// One RSD token stream for a w x h picture; colour 0 becomes skips.
static uchar *rsd_encode(const uchar *px, int n) {
    uchar *out = malloc((size_t)n * 2 + 16), *o = out;
    int i = 0;
    while (i < n) {
        int run = 1;
        while (i + run < n && px[i + run] == px[i] && run < 127)
            run++;
        if (px[i] == 0) {
            *o++ = (uchar)(0x80 | run);
            i += run;
        } else if (run >= 3) {
            *o++ = 0;
            *o++ = (uchar)run;
            *o++ = px[i];
            i += run;
        } else {
            int len = 0;
            while (i + len < n && len < 127 && px[i + len] != 0 &&
                   !(i + len + 2 < n && px[i + len] == px[i + len + 1] && px[i + len] == px[i + len + 2]))
                len++;
            *o++ = (uchar)len;
            memcpy(o, px + i, (size_t)len);
            o += len;
            i += len;
        }
    }
    *o++ = 0x80; // end of stream
    *o++ = 0;
    *o++ = 0;
    return out;
}

static void make_picture(picture_t *p, uchar *pool, int w, int h, int trans) {
    int i, n = w * h;
    for (i = 0; i < n; i++) {
        // blocks of colour, some of them transparent
        int block = ((i % w) / 4 + (i / w) / 4 * 7) % 11;
        pool[i] = (trans && block < 3) ? 0 : (uchar)(1 + (block * 37 + (rnd() & 3)) % 255);
    }
    gr_init_bm(&p->flat, pool, BMT_FLAT8, trans ? BMF_TRANS : 0, (short)w, (short)h);
    p->rsd = p->flat;
    p->rsd.type = BMT_RSD8;
    p->rsd.bits = rsd_encode(pool, n);
}

static void setup(void) {
    static const int tex_size[N_TEXTURES] = {16, 32, 64, 64, 128, 16, 32, 64, 128, 64};
    static const int spr_w[N_SPRITES] = {37, 80, 23, 64, 100, 51, 16, 90};
    static const int spr_h[N_SPRITES] = {53, 60, 91, 64, 96, 33, 20, 120};
    size_t pool_bytes = 2 * PAD, at = PAD;
    uchar *pool;
    int i;

    for (i = 0; i < N_TEXTURES; i++)
        pool_bytes += (size_t)tex_size[i] * tex_size[i];
    for (i = 0; i < N_SPRITES; i++)
        pool_bytes += (size_t)spr_w[i] * spr_h[i];
    pool = malloc(pool_bytes);
    memset(pool, 0x5a, pool_bytes);
    for (i = 0; i < N_TEXTURES; i++) {
        make_picture(&textures[i], pool + at, tex_size[i], tex_size[i], i >= 5);
        at += (size_t)tex_size[i] * tex_size[i];
    }
    for (i = 0; i < N_SPRITES; i++) {
        make_picture(&sprites[i], pool + at, spr_w[i], spr_h[i], 1);
        at += (size_t)spr_w[i] * spr_h[i];
    }
    rastq_stable_pixels(pool, pool_bytes);

    ltab = malloc(65536 + 256);
    fill_random(ltab, 65536 + 256);
    ipal = malloc(65536);
    fill_random(ipal, 65536);
    stab = malloc(65536 + 256);
    fill_random(stab, 65536 + 256);
    for (i = 0; i < 32; i++)
        fill_random(tluc_tables[i], 256);
    for (i = 0; i < 256; i++)
        tluc8tab[i] = (i & 1) ? tluc_tables[i % 32] : NULL;
    tluc8stab = stab;
    grd_ipal = ipal;

    unpack = (uchar *)malloc(UNPACK_BYTES + 2 * PAD) + PAD;
    gr_set_unpack_buf(unpack);
    scratch = (uchar *)malloc(SCRATCH_BYTES + 2 * PAD) + PAD;
    temp_mem = malloc(TEMP_BYTES);
    temp_stack.baseptr = temp_mem;
    temp_stack.sz = TEMP_BYTES;
    MemStackInit(&temp_stack);
    temp_mem_init(&temp_stack);

    memset(&screen, 0, sizeof(screen));
    screen.ltab = ltab;
    screen.clut = ltab;
    screen.c = &canvas;
    grd_screen = &screen;
}

static void set_canvas(int w, int h) {
    int i;
    cw = w;
    ch = h;
    free(canvas_bits);
    free(background);
    canvas_bits = malloc((size_t)w * h);
    background = malloc((size_t)w * h);
    for (i = 0; i < RESULTS; i++) {
        free(result[i]);
        result[i] = malloc((size_t)w * h);
    }
    gr_init_canvas(&canvas, canvas_bits, BMT_FLAT8, (short)w, (short)h);
    gr_set_canvas(&canvas);
}

// ---- scene generation -------------------------------------------------

static int usable(int index) {
    void (*f)() = grd_function_table[index];
    return f != (void (*)())gr_null && f != (void (*)())gr_not_imp;
}

// Clockwise on screen, as the 3D library emits them.
static void fix_winding(op_t *o) {
    double area = 0;
    int i;
    for (i = 0; i < o->n; i++) {
        grs_vertex *a = &o->v[i], *b = &o->v[(i + 1) % o->n];
        area += (double)a->x * b->y - (double)b->x * a->y;
    }
    if (area < 0) {
        for (i = 0; i < o->n / 2; i++) {
            grs_vertex t = o->v[i];
            o->v[i] = o->v[o->n - 1 - i];
            o->v[o->n - 1 - i] = t;
        }
    }
}

// Projects a quad given in view space (Z forward). Returns 0 if any corner
// misses the canvas: the unclipped mappers need them all inside.
static int project(op_t *o, double p[4][3], const grs_bitmap *bm) {
    static const int cu[4] = {0, 1, 1, 0}, cv[4] = {0, 0, 1, 1};
    double f = cw * 0.8;
    int i;
    o->n = 4;
    for (i = 0; i < 4; i++) {
        double sx, sy;
        if (p[i][2] < 1.0)
            return 0;
        sx = cw / 2.0 + f * p[i][0] / p[i][2];
        sy = ch / 2.0 - f * p[i][1] / p[i][2];
        if (sx < 1 || sy < 1 || sx > cw - 2 || sy > ch - 2)
            return 0;
        o->v[i].x = (fix)(sx * 65536.0);
        o->v[i].y = (fix)(sy * 65536.0);
        o->v[i].u = (cu[i] ? 0xffff : 1) << bm->wlog;
        o->v[i].v = (cv[i] ? 0xffff : 1) << bm->hlog;
        o->v[i].w = fix_div(0x10000, (fix)(p[i][2] * 65536.0));
        o->v[i].i = (fix)(rnd() % 0x0f0000);
    }
    fix_winding(o);
    return 1;
}

static void pick_bitmap(op_t *o, picture_t *pic, int allow_rsd) {
    int how = rnd_in(0, allow_rsd ? 5 : 3);
    o->bm = pic->flat;
    o->src = SRC_POOL;
    if (how == 3) {
        o->src = SRC_SCRATCH;
        o->pixels = pic->flat.bits;
        o->pixel_bytes = (size_t)pic->flat.row * pic->flat.h;
    } else if (how == 4) {
        o->src = SRC_UNPACKED;
        o->rsd = pic->rsd;
    } else if (how == 5) {
        o->src = SRC_RSD;
        o->bm = pic->rsd;
    }
    if ((rnd() & 7) == 0 && o->src != SRC_RSD && o->src != SRC_UNPACKED)
        o->bm.type = BMT_TLUC8;
}

static int make_tmap(op_t *o) {
    static const int lin[3] = {GRC_BILIN, GRC_LIT_BILIN, GRC_CLUT_BILIN};
    static const int floor_[3] = {GRC_FLOOR, GRC_LIT_FLOOR, GRC_CLUT_FLOOR};
    static const int wall[3] = {GRC_WALL1D, GRC_LIT_WALL1D, GRC_CLUT_WALL1D};
    static const int per[3] = {GRC_PER, GRC_LIT_PER, GRC_CLUT_PER};
    int family = rnd_in(0, 4), light = rnd_in(0, 2), tries, index;
    double p[4][3];

    pick_bitmap(o, &textures[rnd_in(0, N_TEXTURES - 1)], 1);
    // The row mappers also take textures whose sides aren't powers of two.
    // They read a texel past the end of those, so only from fixed memory.
    if (family != 2 && family != 3 && (rnd() & 3) == 0) {
        o->src = SRC_POOL;
        o->bm = sprites[rnd_in(0, N_SPRITES - 1)].flat;
        if (rnd() & 1)
            o->bm.flags &= ~BMF_TRANS;
        if ((rnd() & 7) == 0)
            o->bm.type = BMT_TLUC8;
    }
    o->op = OP_TMAP;
    o->ti.flags = 0;
    o->ti.clut = NULL;

    for (tries = 0; tries < 60; tries++) {
        if (family == 1 || family == 4) { // a floor or ceiling: depth constant along rows
            double y = rnd_f(0.6, 3.0) * ((rnd() & 1) ? 1 : -1), x0 = rnd_f(-6, 4), z0 = rnd_f(1.5, 12);
            double dx = rnd_f(0.5, 5), dz = rnd_f(0.5, 8);
            double q[4][3] = {{x0, y, z0 + dz}, {x0 + dx, y, z0 + dz}, {x0 + dx, y, z0}, {x0, y, z0}};
            memcpy(p, q, sizeof(q));
        } else if (family == 2) { // a wall: depth constant along columns
            double x0 = rnd_f(-6, 6), z0 = rnd_f(1.5, 14), x1 = x0 + rnd_f(-4, 4), z1 = rnd_f(1.5, 14);
            double y0 = rnd_f(-2.5, 0.5), y1 = y0 + rnd_f(0.5, 3);
            double q[4][3] = {{x0, y1, z0}, {x1, y1, z1}, {x1, y0, z1}, {x0, y0, z0}};
            memcpy(p, q, sizeof(q));
        } else { // any plane
            double c[3] = {rnd_f(-4, 4), rnd_f(-2, 2), rnd_f(3, 14)};
            double a[3] = {rnd_f(-2, 2), rnd_f(-2, 2), rnd_f(-2, 2)}, b[3] = {rnd_f(-2, 2), rnd_f(-2, 2), rnd_f(-2, 2)};
            int k;
            for (k = 0; k < 3; k++) {
                p[0][k] = c[k] - a[k] + b[k];
                p[1][k] = c[k] + a[k] + b[k];
                p[2][k] = c[k] + a[k] - b[k];
                p[3][k] = c[k] - a[k] - b[k];
            }
        }
        if (project(o, p, &o->bm))
            break;
    }
    if (tries == 60)
        return 0;

    switch (family) {
    case 0:
        o->func = h_umap;
        o->ti.tmap_type = (short)lin[light];
        break;
    case 1:
        o->func = h_umap;
        o->ti.tmap_type = (short)floor_[light];
        o->ti.flags = TMF_FLOOR;
        break;
    case 2:
        o->func = v_umap;
        o->ti.tmap_type = (short)wall[light];
        o->ti.flags = TMF_WALL;
        break;
    case 3:
        o->func = per_umap;
        o->ti.tmap_type = (short)per[light];
        break;
    default: // a floor the 3D library judged flat enough for the linear mapper
        o->func = h_umap;
        o->ti.tmap_type = (short)lin[light];
        o->ti.flags = TMF_FLOOR;
        break;
    }
    if (light == 2) {
        o->ti.flags |= TMF_CLUT;
        o->ti.clut = ltab + (rnd_in(0, 15) << 8);
    }
    // The perspective mapper is the one unclipped mapper that looks at the
    // clip rectangle: it ends its scanlines there.
    if (family == 3 && (rnd() & 3) == 0) {
        o->clip[0] = (short)rnd_in(0, cw / 3);
        o->clip[1] = (short)rnd_in(0, ch / 3);
        o->clip[2] = (short)rnd_in(cw * 2 / 3, cw);
        o->clip[3] = (short)rnd_in(ch * 2 / 3, ch);
    }

    // Only combinations the 2D library implements: the others hang h_umap.
    gr_set_fill_type(o->fill_type);
    index = (o->bm.flags & BMF_TRANS) + o->ti.tmap_type + GRD_FUNCS * (o->bm.type == BMT_RSD8 ? BMT_FLAT8 : o->bm.type);
    if (!usable(index))
        return 0;
    if (o->func == per_umap && !usable(index + (GRC_PER_VSCAN - GRC_PER)))
        return 0;
    return 1;
}

static int make_sprite(op_t *o) {
    picture_t *pic = &sprites[rnd_in(0, N_SPRITES - 1)];
    int blend = (rnd() & 3) == 0 && pic->flat.w * pic->flat.h <= 12000;
    double scale = rnd_f(0.6, 3.0);
    double w = pic->flat.w * scale, h = pic->flat.h * scale;
    double x = rnd_f(-w * 0.5, cw - w * 0.5), y = rnd_f(-h * 0.5, ch - h * 0.5);
    int i, index;

    o->op = OP_SPRITE;
    pick_bitmap(o, pic, !blend);
    if (blend)
        o->bm.type = BMT_FLAT8; // the blend mapper only exists for transparent FLAT8
    o->n = 4;
    for (i = 0; i < 4; i++) {
        int right = (i == 1 || i == 2), low = (i >= 2);
        o->v[i].x = (fix)((x + (right ? w : 0)) * 65536.0);
        o->v[i].y = (fix)((y + (low ? h : 0)) * 65536.0);
        o->v[i].u = right ? (pic->flat.w << 16) : 0;
        o->v[i].v = low ? (pic->flat.h << 16) : 0;
        if (blend) {
            o->v[i].u <<= 1;
            o->v[i].v <<= 1;
        }
        o->v[i].w = (fix)rnd(); // never set by the 3D library for sprites
        o->v[i].i = (fix)rnd();
    }
    o->ti.tmap_type = (short)(blend ? GRC_POLY : GRC_CLUT_BILIN);
    o->ti.flags = TMF_CLUT;
    o->ti.clut = ltab + (rnd_in(0, 15) << 8);
    if ((rnd() & 3) == 0) { // a smaller clip rectangle, as inside a window
        o->clip[0] = (short)rnd_in(0, cw / 3);
        o->clip[1] = (short)rnd_in(0, ch / 3);
        o->clip[2] = (short)rnd_in(cw * 2 / 3, cw);
        o->clip[3] = (short)rnd_in(ch * 2 / 3, ch);
    }

    gr_set_fill_type(o->fill_type);
    index = (o->bm.flags & BMF_TRANS) + o->ti.tmap_type + GRD_FUNCS * (o->bm.type == BMT_RSD8 ? BMT_FLAT8 : o->bm.type);
    return usable(index);
}

static int make_poly(op_t *o) {
    static const int kinds[5] = {FIX_UPOLY, FIX_TLUC8_UPOLY, FIX_USPOLY, FIX_UCPOLY, FIX_TLUC8_SPOLY};
    static const int type_of[5] = {0, 3, 1, 2, 4}; // how temptm.c tags each kind
    int k = rnd_in(0, 4), i;
    double cx = rnd_f(cw * 0.15, cw * 0.85), cy = rnd_f(ch * 0.15, ch * 0.85);
    double r = rnd_f(4, (cw < ch ? cw : ch) * 0.14), a0 = rnd_f(0, 6.28);

    o->op = OP_POLY;
    o->index = kinds[k];
    o->color = (k == 1 || k == 4) ? (rnd_in(0, 127) * 2 + 1) : rnd_in(1, 255);
    if (k >= 2)
        o->color = 0;
    o->n = rnd_in(3, 6);
    for (i = 0; i < o->n; i++) {
        double a = a0 + 6.2831853 * i / o->n, rr = r * rnd_f(0.6, 1.0);
        o->v[i].x = (fix)((cx + cos(a) * rr) * 65536.0);
        o->v[i].y = (fix)((cy + sin(a) * rr) * 65536.0);
        o->v[i].i = (k == 2) ? (fix)(rnd() % 0xff0000) : (fix)(rnd() % 0x0f0000);
        o->v[i].u = (fix)(rnd() % 0xffc000);
        o->v[i].v = (fix)(rnd() % 0xffc000);
        o->v[i].w = (fix)(rnd() % 0xffc000);
    }
    fix_winding(o);
    gr_set_fill_type(o->fill_type);
    return usable(GRC_POLY + GRD_FUNCS * type_of[k]);
}

static int make_scene(op_t *ops) {
    int n = rnd_in(20, MAX_OPS), count = 0, guard = 0;

    while (count < n && guard++ < 4000) {
        op_t *o = &ops[count];
        int what = rnd_in(0, 19), ok;

        memset(o, 0, sizeof(*o));
        o->fill_type = FILL_NORM;
        o->clip[2] = (short)cw;
        o->clip[3] = (short)ch;
        if ((rnd() & 7) == 0) {
            o->fill_type = FILL_CLUT;
            o->fill_parm = (intptr_t)(ltab + (rnd_in(0, 15) << 8));
        } else if ((rnd() & 15) == 0) {
            o->fill_type = FILL_SOLID;
            o->fill_parm = rnd_in(1, 255);
        }

        if (what < 10)
            ok = make_tmap(o);
        else if (what < 14)
            ok = make_sprite(o);
        else if (what < 19)
            ok = make_poly(o);
        else { // something drawn outside the recorded calls, like a 3D line
            o->op = OP_RECT;
            o->rw = rnd_in(4, cw / 3);
            o->rh = rnd_in(4, ch / 3);
            o->rx = rnd_in(0, cw - o->rw);
            o->ry = rnd_in(0, ch - o->rh);
            o->color = rnd_in(1, 255);
            ok = 1;
        }
        if (ok)
            count++;
    }
    gr_set_fill_type(FILL_NORM);
    return count;
}

// ---- drawing ----------------------------------------------------------

static void draw_scene(const op_t *ops, int count, int mode, uchar *out) {
    int k, i, y;

    // Same starting point every time, including memory the mappers may read
    // without having written it.
    memcpy(canvas_bits, background, (size_t)cw * ch);
    memset(unpack - PAD, 0x11, UNPACK_BYTES + 2 * PAD);
    memset(scratch - PAD, 0x22, SCRATCH_BYTES + 2 * PAD);
    memset(temp_mem, 0x33, TEMP_BYTES);
    gr_set_fill_type(FILL_NORM);
    gr_set_fill_parm(0);
    gr_set_cliprect(0, 0, cw, ch);

    rastq_set_mode(mode);
    rastq_begin();
    for (k = 0; k < count; k++) {
        const op_t *o = &ops[k];

        if (gr_get_fill_type() != o->fill_type)
            gr_set_fill_type(o->fill_type);
        gr_set_fill_parm(o->fill_parm);
        gr_set_cliprect(o->clip[0], o->clip[1], o->clip[2], o->clip[3]);
        for (i = 0; i < o->n; i++) {
            work_v[i] = o->v[i];
            work_vpl[i] = &work_v[i];
        }

        if (o->op == OP_TMAP || o->op == OP_SPRITE) {
            work_bm = o->bm;
            work_ti = o->ti;
            if (o->src == SRC_SCRATCH) {
                memcpy(scratch, o->pixels, o->pixel_bytes);
                work_bm.bits = scratch;
            } else if (o->src == SRC_UNPACKED) {
                grs_bitmap rsd = o->rsd;
                int type = o->bm.type;
                gr_rsd8_convert(&rsd, &work_bm);
                work_bm.type = (uchar)type;
            }
            if (o->op == OP_TMAP)
                rastq_tmap(o->func, &work_bm, o->n, work_vpl, &work_ti);
            else
                rastq_hmap(&work_bm, o->n, work_vpl, &work_ti);
        } else if (o->op == OP_POLY) {
            rastq_poly(o->index, o->color, o->n, work_vpl);
        } else {
            rastq_flush();
            for (y = 0; y < o->rh; y++)
                memset(canvas_bits + (size_t)(o->ry + y) * cw + o->rx, (int)o->color, (size_t)o->rw);
        }
    }
    rastq_end();
    memcpy(out, canvas_bits, (size_t)cw * ch);
}

static uint64_t checksum(const uchar *p) {
    uint64_t h = 1469598103934665603ULL;
    size_t i, total = (size_t)cw * ch;
    for (i = 0; i < total; i++)
        h = (h ^ p[i]) * 1099511628211ULL;
    return h;
}

static long differing(const uchar *a, const uchar *b) {
    long n = 0;
    size_t i, total = (size_t)cw * ch;
    for (i = 0; i < total; i++)
        n += a[i] != b[i];
    return n;
}

#ifndef RASTQ_REFERENCE
// How the replays of a frame are split
enum { SPLIT_NONE, SPLIT_BANDS, SPLIT_THREADS };
static int band_count, band_bounds[16];

static void set_split(int split) {
    rastq_test_bands(split == SPLIT_BANDS ? band_count : 0, band_bounds);
    rastq_set_threads(split == SPLIT_THREADS ? RASTQ_THREADS : 1);
}
#else
#define SPLIT_NONE 0
#define set_split(split) ((void)0)
#endif

// Finds the first call of a failing scene whose presence makes replay differ.
static void explain(const op_t *ops, int count, int split) {
    static const char *op_names[] = {"tmap", "sprite", "poly", "rect"};
    static const char *src_names[] = {"pool", "scratch", "unpacked", "rsd"};
    int k;

    rastq_set_check_interval(0);
    for (k = 1; k <= count; k++) {
        const op_t *o = &ops[k - 1];
        draw_scene(ops, k, RASTQ_OFF, result[0]);
        set_split(split);
        draw_scene(ops, k, RASTQ_COPY_ALL, result[1]);
        set_split(SPLIT_NONE);
        if (!differing(result[0], result[1]))
            continue;
        printf("    first difference at call %d of %d: %s", k, count, op_names[o->op]);
        if (o->op == OP_TMAP || o->op == OP_SPRITE)
            printf(" %s tmap_type=%d flags=%d bm.type=%d bm.flags=%d %dx%d src=%s",
                   o->func == h_umap ? "h_umap" : o->func == v_umap ? "v_umap" : o->func == per_umap ? "per_umap" : "h_map",
                   o->ti.tmap_type, o->ti.flags, o->bm.type, o->bm.flags, o->bm.w, o->bm.h, src_names[o->src]);
        if (o->op == OP_POLY)
            printf(" index=%d n=%d", o->index, o->n);
        printf(" fill=%d clip=%d,%d,%d,%d\n", o->fill_type, o->clip[0], o->clip[1], o->clip[2], o->clip[3]);
        return;
    }
}

int main(int argc, char **argv) {
    static const int sizes[3][2] = {{320, 200}, {480, 272}, {960, 544}};
    static op_t ops[MAX_OPS];
    int frames = argc > 1 ? atoi(argv[1]) : 1000;
    long bad = 0, drawn = 0, total = 0;
    unsigned long long copied_all = 0, copied_trust = 0;
    unsigned flushes = 0, cmds = 0;
    unsigned batches = 0, solo_mapper = 0, solo_direct = 0;
    int s, f;
    // RASTQ_HASH: only draw directly and print a checksum per frame, to
    // compare two builds of the libraries.
    int hash_only = getenv("RASTQ_HASH") != NULL;

    rng_state = argc > 2 ? strtoull(argv[2], NULL, 10) : 0x9E3779B97F4A7C15ULL;
    setup();

    for (s = 0; s < 3; s++) {
        set_canvas(sizes[s][0], sizes[s][1]);
        for (f = 0; f < frames; f++) {
            int count, m;
            long d[3];

            fill_random(background, (size_t)cw * ch);
            count = make_scene(ops);

            rastq_set_check_interval(0);
            draw_scene(ops, count, RASTQ_OFF, result[0]);
            if (hash_only) {
                printf("%dx%d %d %016llx\n", cw, ch, f, (unsigned long long)checksum(result[0]));
                continue;
            }
            memset(&rastq_stats, 0, sizeof(rastq_stats));
            draw_scene(ops, count, RASTQ_COPY_ALL, result[1]);
            copied_all += rastq_stats.copied;
            flushes += rastq_stats.flushes;
            cmds += rastq_stats.cmds;
            memset(&rastq_stats, 0, sizeof(rastq_stats));
            draw_scene(ops, count, RASTQ_TRUST_STABLE, result[2]);
            copied_trust += rastq_stats.copied;
            // and once with the recorder's own self-check running
            memset(&rastq_stats, 0, sizeof(rastq_stats));
            rastq_set_check_interval(1);
            draw_scene(ops, count, RASTQ_COPY_ALL, result[3]);

            for (m = 0; m < 3; m++)
                d[m] = differing(result[0], result[m + 1]);
            drawn += differing(result[0], background);
            total += (long)cw * ch;
            if (d[0] || d[1] || d[2] || rastq_stats.check_bad_rows || rastq_stats.check_runs == 0) {
                if (bad < 10)
                    printf("  %dx%d frame %d (%d calls): differing pixels copy=%ld trust=%ld checked=%ld, "
                           "self-check %u bad rows in %u runs\n",
                           cw, ch, f, count, d[0], d[1], d[2], rastq_stats.check_bad_rows, rastq_stats.check_runs);
                if (bad < 10)
                    explain(ops, count, SPLIT_NONE);
                bad++;
            }
#ifndef RASTQ_REFERENCE
            {
                // The same scene replayed in row bands: on this thread, band
                // after band at random boundaries, then on three threads.
                // Each is also run under the recorder's self-check, which
                // draws one band alone and looks for rows written outside it.
                static const char *names[] = {"", "bands", "threads"};
                int split, b, c;

                band_count = aux_in(1, 12);
                for (b = 0; b < band_count - 1; b++)
                    band_bounds[b] = aux_in(0, ch);
                for (b = 0; b < band_count - 1; b++)
                    for (c = b + 1; c < band_count - 1; c++)
                        if (band_bounds[c] < band_bounds[b]) {
                            int t = band_bounds[b];
                            band_bounds[b] = band_bounds[c];
                            band_bounds[c] = t;
                        }

                for (split = SPLIT_BANDS; split <= SPLIT_THREADS; split++) {
                    long diff;

                    set_split(split);
                    rastq_set_check_interval(0);
                    memset(&rastq_stats, 0, sizeof(rastq_stats));
                    draw_scene(ops, count, RASTQ_TRUST_STABLE, result[4]);
                    diff = differing(result[0], result[4]);
                    batches += rastq_stats.batches;
                    solo_mapper += rastq_stats.solo[RASTQ_SOLO_MAPPER];
                    solo_direct += rastq_stats.solo[RASTQ_SOLO_DIRECT];
                    memset(&rastq_stats, 0, sizeof(rastq_stats));
                    rastq_set_check_interval(1);
                    draw_scene(ops, count, RASTQ_COPY_ALL, result[4]);
                    set_split(SPLIT_NONE);
                    if (diff || rastq_stats.check_bad_rows || rastq_stats.check_leak_rows ||
                        rastq_stats.check_runs == 0) {
                        if (bad < 10) {
                            printf("  %dx%d frame %d (%d calls), %s: %ld differing pixels, self-check %u bad rows "
                                   "and %u rows outside a band in %u runs\n",
                                   cw, ch, f, count, names[split], diff, rastq_stats.check_bad_rows,
                                   rastq_stats.check_leak_rows, rastq_stats.check_runs);
                            explain(ops, count, split);
                        }
                        bad++;
                    }
                }

                // Now and then, each call drawn alone must stay inside the
                // rows recorded for it: the threads skip calls by them.
                if (f % 8 == 0) {
                    rastq_set_check_interval(0);
                    rastq_test_ranges(1);
                    memset(&rastq_stats, 0, sizeof(rastq_stats));
                    draw_scene(ops, count, RASTQ_COPY_ALL, result[4]);
                    rastq_test_ranges(0);
                    if (rastq_stats.check_leak_rows) {
                        if (bad < 10)
                            printf("  %dx%d frame %d (%d calls): %u rows written outside a call's recorded range\n", cw,
                                   ch, f, count, rastq_stats.check_leak_rows);
                        bad++;
                    }
                }
            }
#endif
        }
    }

    if (hash_only)
        return 0;
    printf("  %d frames per size, %u calls recorded, %u flushes, %.0f%% of pixels drawn\n", frames, cmds, flushes,
           100.0 * drawn / total);
    printf("  pixels copied: %.1f MB copying all, %.1f MB trusting stable textures\n", copied_all / 1048576.0,
           copied_trust / 1048576.0);
    printf("  in bands: %u batches, %u calls drawn alone for their mapper, %u drawn directly\n", batches, solo_mapper,
           solo_direct);
    printf("  %ld frames differ\n", bad);
    return bad != 0;
}
