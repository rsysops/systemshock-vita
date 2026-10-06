// Checks that recording the rasterizer's draw calls and replaying them
// (src/Libraries/3D/Source/rastq.c) gives the same pixels as drawing them
// directly: on one thread, band after band of rows, and on three threads that
// each draw one band. Random scenes go through every mapper family the game
// uses, with the argument storage reused between calls the way the game
// reuses it.
//
// It also checks what the queue hands a GPU: a stand-in fills that with the
// arithmetic of the Vita's shader, and its picture must be close to the
// mappers' (see docs/PERFORMANCE-GPU.md, step G3).
//
// Usage: rastq_test [frames] [seed]
// RASTQ_GPU_EXPLAIN=frames prints how each frame of the GPU comparison did;
// any other value also draws each of its calls alone with both.
// RASTQ_GPU_EVEN gives the comparison's polygons light that fits one plane,
// to tell what differs because of the light from the rest.
// With RASTQ_HASH set it only draws directly and prints a checksum per frame,
// to compare two builds of the libraries. RASTQ_REFERENCE builds it for
// libraries from before row bands, where that is all it can do.

#include <math.h>
#include <stddef.h>
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

enum { OP_TMAP, OP_SPRITE, OP_POLY, OP_RECT, OP_LINE, OP_CLINE, OP_POINT };
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
// the scenes of the GPU comparison come from a generator of their own
static uint64_t gpu_rng_state = 0xD1B54A32D192ED03ULL;
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
static int cw, ch, crow; // crow: bytes from one row to the next
#define RESULTS 5
static uchar *canvas_bits, *background, *result[RESULTS];
// Where the stand-in GPU draws until it is waited for, and a second view's
// canvas, for the CPU to draw while the GPU has the first
static uchar *shadow_bits, *other_bits;
static grs_canvas other_canvas;
static uchar *ltab, *ipal, *smooth_ipal, *stab, *unpack, *scratch;
static uchar tluc_tables[32][256];
static MemStack temp_stack;
static uchar *temp_mem;

#define N_TEXTURES 10
#define N_SPRITES 8
static picture_t textures[N_TEXTURES];
static picture_t sprites[N_SPRITES];
// The same without the texel-to-texel noise, for the GPU comparison: there a
// texel picked a fraction of a pixel away must usually be the same colour.
static picture_t smooth_textures[N_TEXTURES];
static picture_t smooth_sprites[N_SPRITES];

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

static void make_picture(picture_t *p, uchar *pool, int w, int h, int trans, int noise) {
    int i, n = w * h;
    for (i = 0; i < n; i++) {
        // blocks of colour, some of them transparent
        int block = ((i % w) / 4 + (i / w) / 4 * 7) % 11;
        pool[i] = (trans && block < 3) ? 0 : (uchar)(1 + (block * 37 + (noise ? (rnd() & 3) : 0)) % 255);
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
    uchar *pool, *smooth;
    int i;

    for (i = 0; i < N_TEXTURES; i++)
        pool_bytes += (size_t)tex_size[i] * tex_size[i];
    for (i = 0; i < N_SPRITES; i++)
        pool_bytes += (size_t)spr_w[i] * spr_h[i];
    pool = malloc(pool_bytes);
    memset(pool, 0x5a, pool_bytes);
    smooth = malloc(pool_bytes);
    memset(smooth, 0x5a, pool_bytes);
    for (i = 0; i < N_TEXTURES; i++) {
        make_picture(&textures[i], pool + at, tex_size[i], tex_size[i], i >= 5, 1);
        make_picture(&smooth_textures[i], smooth + at, tex_size[i], tex_size[i], i >= 5, 0);
        at += (size_t)tex_size[i] * tex_size[i];
    }
    for (i = 0; i < N_SPRITES; i++) {
        make_picture(&sprites[i], pool + at, spr_w[i], spr_h[i], 1, 1);
        make_picture(&smooth_sprites[i], smooth + at, spr_w[i], spr_h[i], 1, 0);
        at += (size_t)spr_w[i] * spr_h[i];
    }
    rastq_stable_pixels(pool, pool_bytes);
    rastq_stable_pixels(smooth, pool_bytes);

    ltab = malloc(65536 + 256);
    fill_random(ltab, 65536 + 256);
    ipal = malloc(65536);
    fill_random(ipal, 65536);
    // For comparing a GPU with the mappers: neighbouring colours have
    // neighbouring palette indices, so that a shade a pixel further on is
    // a small difference and not any colour at all.
    smooth_ipal = malloc(32768);
    for (i = 0; i < 32768; i++)
        smooth_ipal[i] = (uchar)(1 + ((i & 31) * 3 + ((i >> 5) & 31) * 2 + ((i >> 10) & 31)) % 255);
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

static void set_canvas(int w, int h, int row) {
    int i;
    cw = w;
    ch = h;
    crow = row;
    free(canvas_bits);
    free(background);
    canvas_bits = malloc((size_t)row * h);
    background = malloc((size_t)row * h);
    for (i = 0; i < RESULTS; i++) {
        free(result[i]);
        result[i] = malloc((size_t)row * h);
    }
    free(shadow_bits);
    free(other_bits);
    shadow_bits = malloc((size_t)row * h);
    other_bits = malloc((size_t)row * h);
    gr_init_canvas(&other_canvas, other_bits, BMT_FLAT8, (short)w, (short)h);
    other_canvas.bm.row = (ushort)row;
    gr_init_canvas(&canvas, canvas_bits, BMT_FLAT8, (short)w, (short)h);
    canvas.bm.row = (ushort)row;
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

// Scenes for comparing a GPU's rendering with the mappers'. A GPU
// interpolates over triangles; the mappers do it along the polygon's edges
// and then across each row. The two agree when the value is the same linear
// function all over the polygon, so these scenes make the texture
// coordinates of the linear mapper one. They also stay closer to the viewer,
// where a pixel doesn't skip texels.
static int consistent;
// The light level too: one linear function, of the position on the polygon's
// plane for the floor and wall mappers, which carry it through the
// perspective division with the texture coordinates, and of the screen
// position for the others. Without this each corner has a level of its own,
// as in the game, and the queue has to hand the GPU the mapper's own way of
// working a level out for it to come out like the mappers.
static int even_light;
// What a scene is drawn over: one colour instead of noise, so that a pixel
// left alone by one rendering and its neighbour left alone by the other show
// the same thing.
static int consistent_background;
// The wall mapper doesn't wrap u: a wall's texture only repeats upwards.
static int tile_along_u;

// Replaces a vertex value by a random linear function of the screen position
// that stays within [lo, hi].
static void make_planar(op_t *o, size_t field, fix lo, fix hi) {
    double a = rnd_f(-1, 1), b = rnd_f(-1, 1), t[MAX_VERTS], t_min, t_max;
    int i;

    for (i = 0; i < o->n; i++)
        t[i] = a * o->v[i].x + b * o->v[i].y;
    t_min = t_max = t[0];
    for (i = 1; i < o->n; i++) {
        t_min = t[i] < t_min ? t[i] : t_min;
        t_max = t[i] > t_max ? t[i] : t_max;
    }
    for (i = 0; i < o->n; i++)
        *(fix *)((char *)&o->v[i] + field) =
            lo + (fix)((hi - lo) * (t_max > t_min ? (t[i] - t_min) / (t_max - t_min) : 0.5));
}

// Projects a quad given in view space (Z forward). Returns 0 if any corner
// misses the canvas: the unclipped mappers need them all inside.
static int project(op_t *o, double p[4][3], const grs_bitmap *bm) {
    static const int cu[4] = {0, 1, 1, 0}, cv[4] = {0, 0, 1, 1};
    double f = cw * 0.8;
    fix i_base = 0, i_along_u = 0, i_along_v = 0;
    int i, tiles_u = 1, tiles_v = 1;
    o->n = 4;
    if (consistent) {
        i_base = (fix)(rnd() % 0x050000);
        i_along_u = (fix)(rnd() % 0x050000);
        i_along_v = (fix)(rnd() % 0x050000);
        // a texture repeated along the polygon, as on a tall wall
        if (bm->w == (1 << bm->wlog) && bm->h == (1 << bm->hlog)) {
            tiles_u = tile_along_u ? rnd_in(1, 3) : 1;
            tiles_v = rnd_in(1, 3);
        }
    }
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
        o->v[i].u = (cu[i] ? tiles_u * 0x10000 - 1 : 1) << bm->wlog;
        o->v[i].v = (cv[i] ? tiles_v * 0x10000 - 1 : 1) << bm->hlog;
        o->v[i].w = fix_div(0x10000, (fix)(p[i][2] * 65536.0));
        o->v[i].i = (fix)(rnd() % 0x0f0000);
        if (consistent && even_light)
            o->v[i].i = i_base + cu[i] * i_along_u + cv[i] * i_along_v;
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
    double p[4][3], far = consistent ? 5 : 12 + 2 * (family != 1 && family != 4);

    pick_bitmap(o, &(consistent ? smooth_textures : textures)[rnd_in(0, N_TEXTURES - 1)], 1);
    // The row mappers also take textures whose sides aren't powers of two.
    // They read a texel past the end of those, so only from fixed memory.
    if (family != 2 && family != 3 && (rnd() & 3) == 0) {
        o->src = SRC_POOL;
        o->bm = (consistent ? smooth_sprites : sprites)[rnd_in(0, N_SPRITES - 1)].flat;
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
            double y = rnd_f(0.6, 3.0) * ((rnd() & 1) ? 1 : -1), x0 = rnd_f(-6, 4), z0 = rnd_f(1.5, far);
            double dx = rnd_f(0.5, 5), dz = rnd_f(0.5, consistent ? 3 : 8);
            double q[4][3] = {{x0, y, z0 + dz}, {x0 + dx, y, z0 + dz}, {x0 + dx, y, z0}, {x0, y, z0}};
            memcpy(p, q, sizeof(q));
        } else if (family == 2) { // a wall: depth constant along columns
            double x0 = rnd_f(-6, 6), z0 = rnd_f(1.5, far), x1 = x0 + rnd_f(-4, 4), z1 = rnd_f(1.5, far);
            double y0 = rnd_f(-2.5, 0.5), y1 = y0 + rnd_f(0.5, 3);
            double q[4][3] = {{x0, y1, z0}, {x1, y1, z1}, {x1, y0, z1}, {x0, y0, z0}};
            memcpy(p, q, sizeof(q));
        } else { // any plane
            double c[3] = {rnd_f(-4, 4), rnd_f(-2, 2), rnd_f(3, far)};
            double a[3] = {rnd_f(-2, 2), rnd_f(-2, 2), rnd_f(-2, 2)}, b[3] = {rnd_f(-2, 2), rnd_f(-2, 2), rnd_f(-2, 2)};
            int k;
            for (k = 0; k < 3; k++) {
                p[0][k] = c[k] - a[k] + b[k];
                p[1][k] = c[k] + a[k] + b[k];
                p[2][k] = c[k] + a[k] - b[k];
                p[3][k] = c[k] - a[k] - b[k];
            }
        }
        tile_along_u = family != 2;
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
    if (consistent && even_light && (family == 0 || family == 3 || family == 4)) {
        fix lo = (fix)(rnd() % 0x0e0000);
        make_planar(o, offsetof(grs_vertex, i), lo, lo + (fix)(rnd() % (uint32_t)(0x0f0000 - lo)));
    }
    if (consistent && (family == 0 || family == 4)) { // the linear mapper: no perspective
        int pow2 = o->bm.w == (1 << o->bm.wlog) && o->bm.h == (1 << o->bm.hlog);
        make_planar(o, offsetof(grs_vertex, u), 1 << o->bm.wlog, ((pow2 ? rnd_in(1, 3) : 1) * 0x10000 - 1) << o->bm.wlog);
        make_planar(o, offsetof(grs_vertex, v), 1 << o->bm.hlog, ((pow2 ? rnd_in(1, 3) : 1) * 0x10000 - 1) << o->bm.hlog);
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
    picture_t *pic = &(consistent ? smooth_sprites : sprites)[rnd_in(0, N_SPRITES - 1)];
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

// A line of the 3D library, in one colour or shaded between two, or a point.
// Ends may lie off the canvas: the lines are clipped.
static int make_mark(op_t *o) {
    int kind = rnd_in(0, 9), i;

    if (kind == 0) {
        o->op = OP_POINT;
        o->rx = rnd_in(-3, cw + 2);
        o->ry = rnd_in(-3, ch + 2);
        o->color = rnd_in(1, 255);
        return 1;
    }
    o->op = kind < 5 ? OP_LINE : OP_CLINE;
    o->color = rnd_in(1, 255);
    o->n = 2;
    for (i = 0; i < 2; i++) {
        o->v[i].x = (fix)(rnd_f(-cw * 0.1, cw * 1.1) * 65536.0);
        o->v[i].y = (fix)(rnd_f(-ch * 0.1, ch * 1.1) * 65536.0);
        // the colours of a shaded line are whole numbers
        o->v[i].u = rnd_in(0, 255);
        o->v[i].v = rnd_in(0, 255);
        o->v[i].w = rnd_in(0, 255);
    }
    if (rnd_in(0, 5) == 0) // on one row, or nearly
        o->v[1].y = o->v[0].y + (fix)(rnd_f(-1.5, 1.5) * 65536.0);
    if (rnd_in(0, 7) == 0) // straight down
        o->v[1].x = o->v[0].x;
    return 1;
}

static int make_scene(op_t *ops) {
    int n = rnd_in(20, MAX_OPS), count = 0, guard = 0;

    while (count < n && guard++ < 4000) {
        op_t *o = &ops[count];
        int what = rnd_in(0, 21), ok;

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
        else if (what >= 20)
            ok = make_mark(o);
        else { // something drawn outside the recorded calls
            o->op = OP_RECT;
            o->rw = rnd_in(4, cw / 3);
            o->rh = rnd_in(4, ch / 3);
            o->rx = rnd_in(0, cw - o->rw);
            o->ry = rnd_in(0, ch - o->rh);
            o->color = rnd_in(1, 255);
            ok = 1;
        }
        // Now and then a polygon wound the other way round, as the 3D
        // library emits for a face seen from behind: the mappers draw
        // nothing for those.
        if (ok && (o->op == OP_TMAP || o->op == OP_SPRITE || o->op == OP_POLY) && aux_in(0, 9) == 0) {
            int i;
            for (i = 0; i < o->n / 2; i++) {
                grs_vertex t = o->v[i];
                o->v[i] = o->v[o->n - 1 - i];
                o->v[o->n - 1 - i] = t;
            }
        }
        if (ok)
            count++;
    }
    gr_set_fill_type(FILL_NORM);
    return count;
}

// ---- drawing ----------------------------------------------------------

// Same starting point every time, including memory the mappers may read
// without having written it.
static void fresh_start(uchar *bits) {
    memcpy(bits, background, (size_t)crow * ch);
    memset(unpack - PAD, 0x11, UNPACK_BYTES + 2 * PAD);
    memset(scratch - PAD, 0x22, SCRATCH_BYTES + 2 * PAD);
    memset(temp_mem, 0x33, TEMP_BYTES);
    gr_set_fill_type(FILL_NORM);
    gr_set_fill_parm(0);
    gr_set_cliprect(0, 0, cw, ch);
}

// A view's calls, on the current canvas.
static void record_view(const op_t *ops, int count) {
    int k, i, y;

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
        } else if (o->op == OP_LINE || o->op == OP_CLINE) {
#ifndef RASTQ_REFERENCE
            rastq_line(o->op == OP_CLINE, o->color, &work_v[0], &work_v[1]);
#else
            // the libraries of then, where the 3D library drew its lines itself
            rastq_flush();
            ((int (*)(long, long, grs_vertex *, grs_vertex *))
                 grd_line_clip_fill_vector[o->op == OP_CLINE ? GR_WIRE_POLY_CLINE : GR_WIRE_POLY_LINE])(
                o->color, gr_get_fill_parm(), &work_v[0], &work_v[1]);
#endif
        } else if (o->op == OP_POINT) {
            gr_set_fcolor(o->color);
#ifndef RASTQ_REFERENCE
            rastq_point((short)o->rx, (short)o->ry);
#else
            rastq_flush();
            ((int (*)(short, short))grd_canvas_table[DRAW_POINT])((short)o->rx, (short)o->ry);
#endif
        } else {
            rastq_flush();
            for (y = 0; y < o->rh; y++)
                memset(grd_canvas->bm.bits + (size_t)(o->ry + y) * crow + o->rx, (int)o->color, (size_t)o->rw);
        }
    }
    rastq_end();
}

#ifndef RASTQ_REFERENCE
// What a second view of the same calls must look like when the CPU draws it
// while the GPU has the first (NULL: no second view), and what came of it
static const uchar *other_expected;
static unsigned left_out, other_views, other_bad, other_lost;

// As the game does with its help scan: another view, the CPU's, rendered
// between the end of the GPU's view and the wait for it. It must come out
// as the mappers draw it, and leave the GPU's scene out.
static void draw_other_view(const op_t *ops, int count) {
    gr_set_canvas(&other_canvas);
    fresh_start(other_bits);
    rastq_gpu_view(0);
    if (consistent_background)
        gr_clear(0x4d);
    record_view(ops, count);
    gr_set_canvas(&canvas);
    other_views++;
    other_bad += memcmp(other_bits, other_expected, (size_t)crow * ch) != 0;
    other_lost += !rastq_gpu_busy();
}
#endif

static void draw_scene(const op_t *ops, int count, int mode, uchar *out) {
    fresh_start(canvas_bits);
    rastq_set_mode(mode);
#ifndef RASTQ_REFERENCE
    // As a view starts: its canvas is one the GPU can draw into, and the
    // clear is the GPU's if the view is.
    rastq_gpu_view(1);
    if (consistent_background && !rastq_gpu_clear(0x4d))
        gr_clear(0x4d);
#endif
    record_view(ops, count);
#ifndef RASTQ_REFERENCE
    // As a view is sent: the GPU may still have its last scene, and nothing
    // reads the canvas before the wait.
    if (rastq_gpu_busy()) {
        left_out++;
        if (other_expected != NULL)
            draw_other_view(ops, count);
        rastq_gpu_finish();
    }
#endif
    memcpy(out, canvas_bits, (size_t)crow * ch);
}

static uint64_t checksum(const uchar *p) {
    uint64_t h = 1469598103934665603ULL;
    size_t i, total = (size_t)crow * ch;
    for (i = 0; i < total; i++)
        h = (h ^ p[i]) * 1099511628211ULL;
    return h;
}

static long differing(const uchar *a, const uchar *b) {
    long n = 0;
    size_t i, total = (size_t)crow * ch;
    for (i = 0; i < total; i++)
        n += a[i] != b[i];
    return n;
}

// The pixels of b whose value a has neither there nor within two pixels.
// Two renderings that put an edge or a texel boundary a pixel or two apart
// differ along it, but not by this count; a wrong texture, table or winding
// does.
static long differing_nearby(const uchar *a, const uchar *b) {
    long n = 0;
    int x, y, dx, dy;

    for (y = 0; y < ch; y++) {
        for (x = 0; x < cw; x++) {
            uchar want = b[(size_t)y * crow + x];
            int found = 0;
            for (dy = -2; dy <= 2 && !found; dy++)
                for (dx = -2; dx <= 2 && !found; dx++)
                    if (y + dy >= 0 && y + dy < ch && x + dx >= 0 && x + dx < cw)
                        found = a[(size_t)(y + dy) * crow + x + dx] == want;
            n += !found;
        }
    }
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

#ifndef RASTQ_REFERENCE
// A stand-in for the GPU, to check what the queue hands one: it fills the
// polygons the way the Vita's shader does (src/MacSrc/VitaGpu.c), in the
// same single-precision arithmetic. It isn't the mappers: its edges and its
// texel boundaries fall a little differently, so a small share of pixels
// differs from theirs. A wrong texture, table row or winding rule differs
// by far more.
static struct {
    uchar *bits;
    int w, h, row;
    const uchar *tables;
    int rows;
    const uchar *ipal;
} ref;
static unsigned ref_no_ipal; // shaded calls in a scene begun without the colours' table
static unsigned ref_compared;
// A scene handed over and not waited for: its pixels aren't on the canvas
static int ref_out;
static unsigned ref_unfinished;
// What the queue's comparison must hold against the GPU's picture when a
// scene is drawn in one go, and whether the last comparison did
static const uchar *ref_expected_cpu, *ref_expected_gpu;
static int ref_cpu_matched, ref_gpu_matched;
static int ref_begin(uchar *bits, int w, int h, int row, const uchar *tables, int rows, const uchar *ipal_table) {
    if (bits != canvas_bits || w != cw || h != ch)
        return 0;
    // It draws aside and the canvas gets the result at the wait, so that a
    // canvas read or written too early shows in the picture.
    ref_unfinished += ref_out;
    memcpy(shadow_bits, bits, (size_t)row * h);
    ref.bits = shadow_bits;
    ref.w = w;
    ref.h = h;
    ref.row = row;
    ref.tables = tables;
    ref.rows = rows;
    ref.ipal = ipal_table;
    return 1;
}

// The fragment shader. The hardware has divided u and v by q; the five
// light values arrive as they are on screen.
static void ref_pixel(uchar *dest, const grs_bitmap *bm, int flags, float u, float v, float left, float span,
                      float along, float width, float depth) {
    float s = u / bm->w, t = v / bm->h, row;
    int tx, ty, r, texel;

    if (flags & RASTQ_GPU_WRAP) { // the sampler repeats
        s -= floorf(s);
        t -= floorf(t);
    }
    // the sampler: nearest texel, clamped to the bitmap
    tx = (int)floorf(s * bm->w);
    ty = (int)floorf(t * bm->h);
    tx = tx < 0 ? 0 : tx >= bm->w ? bm->w - 1 : tx;
    ty = ty < 0 ? 0 : ty >= bm->h ? bm->h - 1 : ty;
    texel = bm->bits[(size_t)ty * bm->row + tx];
    if ((flags & RASTQ_GPU_TRANS) && texel == 0)
        return;
    row = (left + span * (along / fmaxf(width, 0.0001f))) / depth;
    r = (int)floorf(row); // the tables' sampler picks the nearest row
    r = r < 0 ? 0 : r >= ref.rows ? ref.rows - 1 : r;
    *dest = ref.tables[r * 256 + texel];
}

static double ref_edge(const rastq_gpu_vertex *a, const rastq_gpu_vertex *b, double x, double y) {
    return ((double)b->x - a->x) * (y - a->y) - ((double)b->y - a->y) * (x - a->x);
}

// Is a point on the edge a->b of a clockwise triangle inside it? Top and
// left edges are, as the mappers fill [left, right) and [top, bottom).
static int ref_owns(const rastq_gpu_vertex *a, const rastq_gpu_vertex *b) {
    return b->y < a->y || (b->y == a->y && b->x > a->x);
}

// The shaded calls' fragment shader: three colours, each as the light is,
// then the palette index of their eighths.
static void ref_shade(uchar *dest, float part, float r, float r_span, float g, float g_span, float b, float b_span) {
    int r5 = (int)floorf((r + r_span * part) / 8.0f), g5 = (int)floorf((g + g_span * part) / 8.0f);
    int b5 = (int)floorf((b + b_span * part) / 8.0f);

    r5 = r5 < 0 ? 0 : r5 > 31 ? 31 : r5;
    g5 = g5 < 0 ? 0 : g5 > 31 ? 31 : g5;
    b5 = b5 < 0 ? 0 : b5 > 31 ? 31 : b5;
    *dest = ref.ipal[r5 | (g5 << 5) | (b5 << 10)];
}

#define REF_SHADED 0x100 // with bm NULL: shaded between colours, not one colour

// A fan of triangles, both windings drawn, as the GPU is set up.
static void ref_polygon(const grs_bitmap *bm, int flags, int n, const rastq_gpu_vertex *v, int color) {
    int k;

    for (k = 1; k + 1 < n; k++) {
        const rastq_gpu_vertex *a = &v[0], *b = &v[k], *c = &v[k + 1];
        double area = ref_edge(a, b, c->x, c->y);
        int x0, x1, y0, y1, x, y;

        if (area == 0)
            continue;
        if (area < 0) {
            const rastq_gpu_vertex *t = b;
            b = c;
            c = t;
            area = -area;
        }
        x0 = (int)floor(fmin(a->x, fmin(b->x, c->x)));
        x1 = (int)ceil(fmax(a->x, fmax(b->x, c->x)));
        y0 = (int)floor(fmin(a->y, fmin(b->y, c->y)));
        y1 = (int)ceil(fmax(a->y, fmax(b->y, c->y)));
        x0 = x0 < 0 ? 0 : x0;
        y0 = y0 < 0 ? 0 : y0;
        x1 = x1 >= ref.w ? ref.w - 1 : x1;
        y1 = y1 >= ref.h ? ref.h - 1 : y1;
        for (y = y0; y <= y1; y++) {
            for (x = x0; x <= x1; x++) {
                double ea = ref_edge(b, c, x, y), eb = ref_edge(c, a, x, y), ec = ref_edge(a, b, x, y);
                uchar *dest = ref.bits + (size_t)y * ref.row + x;
                float la, lb, lc;

                if (ea < 0 || eb < 0 || ec < 0 || (ea == 0 && !ref_owns(b, c)) || (eb == 0 && !ref_owns(c, a)) ||
                    (ec == 0 && !ref_owns(a, b)))
                    continue;
                if (bm == NULL && !(flags & REF_SHADED)) {
                    *dest = (uchar)color;
                    continue;
                }
                la = (float)(ea / area);
                lb = (float)(eb / area);
                lc = (float)(ec / area);
                if (bm == NULL) {
#define REF_AT(field) (la * a->field + lb * b->field + lc * c->field)
                    ref_shade(dest, REF_AT(along) / fmaxf(REF_AT(width), 0.0001f), REF_AT(left), REF_AT(span),
                              REF_AT(g_left), REF_AT(g_span), REF_AT(b_left), REF_AT(b_span));
#undef REF_AT
                    continue;
                }
                {
                    // u and v with the perspective, the rest without
                    float q = la * a->q + lb * b->q + lc * c->q;
#define REF_AT(field) (la * a->field + lb * b->field + lc * c->field)
                    ref_pixel(dest, bm, flags, REF_AT(u) / q, REF_AT(v) / q, REF_AT(left), REF_AT(span), REF_AT(along),
                              REF_AT(width), REF_AT(depth));
#undef REF_AT
                }
            }
        }
    }
}

static int ref_flat(int n, const rastq_gpu_vertex *v, int color) {
    ref_polygon(NULL, 0, n, v, color);
    return 1;
}

static int ref_tmap(const grs_bitmap *bm, int flags, int n, const rastq_gpu_vertex *v) {
    ref_polygon(bm, flags, n, v, 0);
    return 1;
}

static int ref_shaded(int n, const rastq_gpu_vertex *v) {
    if (ref.ipal == NULL) {
        ref_no_ipal++;
        return 1;
    }
    ref_polygon(NULL, REF_SHADED, n, v, 0);
    return 1;
}

static void ref_end(void) { ref_out = 1; }

static void ref_finish(void) {
    if (ref_out)
        memcpy(canvas_bits, shadow_bits, (size_t)ref.row * ref.h);
    ref_out = 0;
}

static void ref_compared_cb(const uchar *gpu, const uchar *cpu, int w, int h, int row, unsigned differing) {
    (void)differing;
    ref_compared++;
    (void)w;
    ref_cpu_matched = ref_expected_cpu != NULL && memcmp(cpu, ref_expected_cpu, (size_t)row * h) == 0;
    // the GPU's side of a comparison is its finished picture
    ref_gpu_matched = ref_expected_gpu != NULL && memcmp(gpu, ref_expected_gpu, (size_t)row * h) == 0;
}

static const rastq_gpu ref_gpu = {ref_begin, ref_flat, ref_tmap, ref_shaded, ref_end, ref_finish, ref_compared_cb};

// The share of a frame's pixels that may differ between the stand-in and the
// mappers, and the share over a whole run.
#define GPU_FRAME_LIMIT 0.06
#define GPU_RUN_LIMIT 0.0015
#endif

// Finds the first call of a failing scene whose presence makes replay differ.
static void explain(const op_t *ops, int count, int split) {
    static const char *op_names[] = {"tmap", "sprite", "poly", "rect", "line", "shaded line", "point"};
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

#ifndef RASTQ_REFERENCE
// For a scene the stand-in draws too differently: each call drawn alone by
// both, with the pixels it covers and how many of them differ.
static void explain_gpu(const op_t *ops, int count) {
    static const char *op_names[] = {"tmap", "sprite", "poly", "rect", "line", "shaded line", "point"};
    int k;

    rastq_set_check_interval(0);
    for (k = 0; k < count; k++) {
        const op_t *o = &ops[k];
        long diff, covered;

        if (o->op == OP_RECT)
            continue;
        draw_scene(o, 1, RASTQ_OFF, result[1]);
        rastq_set_gpu(&ref_gpu);
        rastq_use_gpu(1);
        memset(&rastq_stats, 0, sizeof(rastq_stats));
        draw_scene(o, 1, RASTQ_TRUST_STABLE, result[2]);
        rastq_use_gpu(0);
        rastq_set_gpu(NULL);
        diff = differing(result[1], result[2]);
        memset(result[3], 0x4d, (size_t)crow * ch);
        covered = differing(result[1], result[3]);
        printf("    call %d: %ld of %ld pixels differ, %ld not near (%s): %s", k, diff, covered,
               differing_nearby(result[1], result[2]),
               rastq_stats.gpu_culled ? "culled" : rastq_stats.gpu_polys ? "gpu" : "cpu", op_names[o->op]);
        if (o->op == OP_TMAP || o->op == OP_SPRITE)
            printf(" %s tmap_type=%d flags=%d bm.type=%d bm.flags=%d %dx%d",
                   o->func == h_umap ? "h_umap" : o->func == v_umap ? "v_umap" : o->func == per_umap ? "per_umap" : "h_map",
                   o->ti.tmap_type, o->ti.flags, o->bm.type, o->bm.flags, o->bm.w, o->bm.h);
        if (o->op == OP_POLY)
            printf(" index=%d n=%d", o->index, o->n);
        printf(" fill=%d\n", o->fill_type);
    }
}
#endif

#ifndef RASTQ_REFERENCE
// ---- the overlays' drawing (docs/PERFORMANCE-GPU.md, step G9) -------------
// These have a generator of their own: the frames' must give the reference
// build the same scenes.
static uint64_t ov_state = 0x9E3779B97F4A7C15ULL;
static int ov_in(int lo, int hi) {
    ov_state ^= ov_state << 13;
    ov_state ^= ov_state >> 7;
    ov_state ^= ov_state << 17;
    return lo + (int)((uint32_t)(ov_state >> 16) % (uint32_t)(hi - lo + 1));
}

// A bitmap with transparent and opaque pixels in runs, as overlays have them.
static void ov_bitmap(grs_bitmap *bm, uchar *bits, int w, int h, int row, int trans) {
    int x, y, run = 0, clear = 0;
    for (y = 0; y < h; y++)
        for (x = 0; x < row; x++) {
            if (run == 0) {
                run = ov_in(1, 9);
                clear = ov_in(0, 2) != 0;
            }
            run--;
            bits[y * row + x] = clear ? 0 : (uchar)ov_in(0, 255);
        }
    gr_init_bitmap(bm, bits, BMT_FLAT8, trans ? BMF_TRANS : 0, (short)w, (short)h);
    bm->row = (ushort)row;
}

// gr_bitmap's transparent copy against one pixel at a time, and
// gr_scale_bitmap anywhere against the bitmap stretched once and copied
// (which is what the game keeps of its small HUD bitmaps). Returns the
// number of failures.
static int test_overlays(void) {
    static uchar bits[80 * 40], expect[960 * 544], kept[256 * 128];
    grs_bitmap bm;
    int n, x, y, bad_copy = 0, bad_scale = 0, stretched = 0;

    set_canvas(960, 544, 960);
    for (n = 0; n < 4000; n++) {
        int w = ov_in(1, 70), h = ov_in(1, 12), row = w + ov_in(0, 3);
        int px = ov_in(-80, cw + 10), py = ov_in(-14, ch + 2), fill = ov_in(0, 255);

        ov_bitmap(&bm, bits, w, h, row, 1);
        memset(canvas_bits, fill, (size_t)crow * ch);
        memset(expect, fill, (size_t)crow * ch);
        for (y = 0; y < h; y++)
            for (x = 0; x < w; x++)
                if (bits[y * row + x] != 0 && px + x >= 0 && px + x < cw && py + y >= 0 && py + y < ch)
                    expect[(size_t)(py + y) * crow + px + x] = bits[y * row + x];
        gr_set_cliprect(0, 0, cw, ch);
        gr_bitmap(&bm, (short)px, (short)py);
        bad_copy += memcmp(canvas_bits, expect, (size_t)crow * ch) != 0;
    }
    for (n = 0; n < 2000; n++) {
        int w = ov_in(1, 40), h = ov_in(1, 30), row = w + ov_in(0, 3), trans = ov_in(0, 1);
        // the game's stretch on a 960x544 screen, or any other
        int dw = ov_in(0, 1) ? w * 3 : ov_in(1, 250), dh = ov_in(0, 1) ? h * 544 / 200 : ov_in(1, 120);
        int px, py, fill = ov_in(1, 255);

        if (dh < 1)
            dh = 1;
        px = ov_in(0, cw - dw);
        py = ov_in(0, ch - dh);
        ov_bitmap(&bm, bits, w, h, row, trans);
        gr_set_cliprect(0, 0, cw, ch);
        // stretched once, at the corner of an empty canvas
        memset(canvas_bits, 0, (size_t)crow * ch);
        gr_scale_bitmap(&bm, 0, 0, (short)dw, (short)dh);
        for (y = 0; y < dh; y++)
            memcpy(kept + y * dw, canvas_bits + (size_t)y * crow, (size_t)dw);
        // and where it is asked for
        memset(canvas_bits, fill, (size_t)crow * ch);
        memset(expect, fill, (size_t)crow * ch);
        for (y = 0; y < dh; y++)
            for (x = 0; x < dw; x++)
                if (!trans || kept[y * dw + x] != 0) {
                    expect[(size_t)(py + y) * crow + px + x] = kept[y * dw + x];
                    stretched++;
                }
        gr_scale_bitmap(&bm, (short)px, (short)py, (short)dw, (short)dh);
        bad_scale += memcmp(canvas_bits, expect, (size_t)crow * ch) != 0;
    }
    printf("overlays: %d of 4000 transparent copies differ from pixel by pixel, %d of 2000 stretched bitmaps "
           "from stretched once and copied\n",
           bad_copy, bad_scale);
    // a stretch that draws nothing would pass
    if (stretched < 2000 * 100) {
        printf("overlays: the stretched bitmaps drew only %d pixels\n", stretched);
        return 1;
    }
    return bad_copy + bad_scale;
}
#endif

#ifndef RASTQ_REFERENCE
// Lines and points alone, drawn by the stand-in GPU from what the queue
// hands it and by the 2D library: the queue builds a line's strips from the
// library's own stepping, so the two must cover the same pixels but for a
// row here and there where an edge passes within a hundredth of a pixel of
// one. Returns the number of failures.
static int test_marks(void) {
    static op_t ops[MAX_OPS];
    uint64_t main_state = rng_state;
    long diff[2] = {0, 0}, far[2] = {0, 0}, drawn[2] = {0, 0}, on_cpu = 0;
    int pass, n, k, bad = 0;
    size_t i;

    rng_state = 0xD1B54A32D192ED03ULL;
    set_canvas(960, 544, 960);
    consistent_background = 1;
    grd_ipal = smooth_ipal;
    rastq_set_check_interval(0);
    for (pass = 0; pass < 2; pass++) { // in one colour and points, then shaded
        for (n = 0; n < 150; n++) {
            int count = 0;
            while (count < 40) {
                op_t *o = &ops[count];
                memset(o, 0, sizeof(*o));
                o->fill_type = FILL_NORM;
                o->clip[2] = (short)cw;
                o->clip[3] = (short)ch;
                if (rnd_in(0, 3) == 0) { // under a smaller clip rectangle
                    o->clip[0] = (short)rnd_in(0, cw / 3);
                    o->clip[1] = (short)rnd_in(0, ch / 3);
                    o->clip[2] = (short)rnd_in(cw * 2 / 3, cw);
                    o->clip[3] = (short)rnd_in(ch * 2 / 3, ch);
                }
                make_mark(o);
                if ((o->op == OP_CLINE) == pass)
                    count++;
            }
            draw_scene(ops, count, RASTQ_OFF, result[1]);
            rastq_set_gpu(&ref_gpu);
            rastq_use_gpu(1);
            memset(&rastq_stats, 0, sizeof(rastq_stats));
            draw_scene(ops, count, RASTQ_TRUST_STABLE, result[2]);
            rastq_use_gpu(0);
            rastq_set_gpu(NULL);
            on_cpu += rastq_stats.gpu_cpu_calls + rastq_stats.gpu_fallbacks + rastq_stats.solo[RASTQ_SOLO_DIRECT];
            diff[pass] += differing(result[1], result[2]);
            far[pass] += differing_nearby(result[1], result[2]);
            for (i = 0; i < (size_t)crow * ch; i++)
                drawn[pass] += result[1][i] != 0x4d;
            if (getenv("RASTQ_MARKS_EXPLAIN") != NULL && differing(result[1], result[2]) != 0) {
                for (k = 0; k < count; k++) {
                    draw_scene(&ops[k], 1, RASTQ_OFF, result[1]);
                    rastq_set_gpu(&ref_gpu);
                    rastq_use_gpu(1);
                    draw_scene(&ops[k], 1, RASTQ_TRUST_STABLE, result[2]);
                    rastq_use_gpu(0);
                    rastq_set_gpu(NULL);
                    if (differing(result[1], result[2]) != 0)
                        printf("    %s (%.2f,%.2f)-(%.2f,%.2f) clip %d,%d,%d,%d: %ld pixels differ\n",
                               ops[k].op == OP_POINT ? "point" : ops[k].op == OP_LINE ? "line" : "shaded line",
                               ops[k].v[0].x / 65536.0, ops[k].v[0].y / 65536.0, ops[k].v[1].x / 65536.0,
                               ops[k].v[1].y / 65536.0, ops[k].clip[0], ops[k].clip[1], ops[k].clip[2], ops[k].clip[3],
                               differing(result[1], result[2]));
                }
            }
        }
    }
    consistent_background = 0;
    grd_ipal = ipal;
    rng_state = main_state;
    printf("lines and points: in one colour %ld of %ld pixels differ from the 2D library's; shaded %ld of %ld, %ld "
           "not near one that matches\n",
           diff[0], drawn[0], diff[1], drawn[1], far[1]);
    // one colour: only the rows an edge all but touches a pixel; shaded: a
    // shade a pixel early or late besides
    if (on_cpu != 0) {
        printf("  %ld were left to the CPU\n", on_cpu);
        bad++;
    }
    if (drawn[0] == 0 || drawn[1] == 0 || diff[0] > drawn[0] / 100 || far[0] != 0 || far[1] > drawn[1] / 200)
        bad++;
    return bad;
}
#endif

int main(int argc, char **argv) {
    // width, height, bytes a row: the last is the paneled view as the GPU has
    // it on the Vita, in a canvas made for the full screen
    static const int sizes[4][3] = {{320, 200, 320}, {480, 272, 480}, {960, 544, 960}, {804, 293, 960}};
    static op_t ops[MAX_OPS];
    int frames = argc > 1 ? atoi(argv[1]) : 1000;
    long bad = 0, drawn = 0, total = 0;
    unsigned long long copied_all = 0, copied_trust = 0;
    unsigned flushes = 0, cmds = 0;
    unsigned batches = 0, solo_mapper = 0, solo_direct = 0;
    unsigned gpu_scenes = 0, gpu_polys = 0, gpu_culled = 0, gpu_cpu_calls = 0, gpu_whole_checks = 0;
    unsigned gpu_pieces = 0;
    unsigned cut_lists = 0, cut_bad = 0, cut_calls = 0, cut_by_workers = 0;
    long gpu_diff = 0, gpu_far = 0;
    int s, f;
    // RASTQ_HASH: only draw directly and print a checksum per frame, to
    // compare two builds of the libraries.
    int hash_only = getenv("RASTQ_HASH") != NULL;

    rng_state = argc > 2 ? strtoull(argv[2], NULL, 10) : 0x9E3779B97F4A7C15ULL;
    even_light = getenv("RASTQ_GPU_EVEN") != NULL;
    setup();

#ifndef RASTQ_REFERENCE
    if (!hash_only) {
        bad += test_overlays();
        bad += test_marks();
    }
#endif

    for (s = 0; s < 4; s++) {
        set_canvas(sizes[s][0], sizes[s][1], sizes[s][2]);
        for (f = 0; f < frames; f++) {
            int count, m;
            long d[3];

            fill_random(background, (size_t)crow * ch);
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

                // The queue's GPU path, with the stand-in, on a scene made
                // for it: what it draws may differ from the mappers' along edges and texel
                // boundaries, and hardly at all beyond two pixels from where
                // they have the same value. Then the queue's own comparison
                // of the two, which must leave the GPU's picture on the
                // canvas, and hold the mappers' against it (seen when the
                // scene is drawn in one go: otherwise each part is compared
                // on top of the GPU's earlier parts).
                {
                    static op_t gpu_ops[MAX_OPS];
                    uint64_t main_state = rng_state;
                    int gpu_count;

                    rng_state = gpu_rng_state;
                    consistent = 1;
                    gpu_count = make_scene(gpu_ops);
                    consistent = 0;
                    gpu_rng_state = rng_state;
                    rng_state = main_state;

                    consistent_background = 1;
                    grd_ipal = smooth_ipal;
                    rastq_set_check_interval(0);
                    draw_scene(gpu_ops, gpu_count, RASTQ_OFF, result[1]);
                    {
                        unsigned compared = ref_compared;
                        long diff, far;
                        int drawn_ok, check_ok, whole, k;

                        rastq_set_gpu(&ref_gpu);
                        rastq_use_gpu(1);
                        rastq_set_check_interval(0);
                        memset(&rastq_stats, 0, sizeof(rastq_stats));
                        other_expected = result[1];
                        draw_scene(gpu_ops, gpu_count, RASTQ_TRUST_STABLE, result[2]);
                        other_expected = NULL;
                        diff = differing(result[1], result[2]);
                        far = differing_nearby(result[1], result[2]);
                        gpu_diff += diff;
                        gpu_far += far;
                        gpu_pieces += rastq_stats.gpu_pieces;
                        gpu_scenes += rastq_stats.gpu_scenes;
                        gpu_polys += rastq_stats.gpu_polys;
                        gpu_culled += rastq_stats.gpu_culled;
                        gpu_cpu_calls += rastq_stats.gpu_cpu_calls;
                        drawn_ok = rastq_stats.gpu_scenes > 0 && rastq_stats.gpu_fallbacks == 0 &&
                                   far <= (long)(GPU_FRAME_LIMIT * cw * ch);
                        // The same list decided and cut by all the threads
                        // before it is handed over: the same pieces in the
                        // same order, so the same picture to the byte.
                        set_split(SPLIT_THREADS);
                        rastq_set_gpu_cut(1);
                        memset(&rastq_stats, 0, sizeof(rastq_stats));
                        draw_scene(gpu_ops, gpu_count, RASTQ_TRUST_STABLE, result[3]);
                        rastq_set_gpu_cut(RASTQ_GPU_CUT_CALLS);
                        set_split(SPLIT_NONE);
                        cut_lists++;
                        cut_calls += rastq_stats.gpu_cut[0] + rastq_stats.gpu_cut[1] + rastq_stats.gpu_cut[2];
                        cut_by_workers += rastq_stats.gpu_cut[1] + rastq_stats.gpu_cut[2];
                        if (differing(result[2], result[3]) != 0) {
                            cut_bad++;
                            drawn_ok = 0;
                        }
                        rastq_set_check_interval(1);
                        memset(&rastq_stats, 0, sizeof(rastq_stats));
                        ref_expected_cpu = result[1];
                        ref_expected_gpu = result[2];
                        draw_scene(gpu_ops, gpu_count, RASTQ_TRUST_STABLE, result[4]);
                        ref_expected_cpu = ref_expected_gpu = NULL;
                        // in one go: one comparison, and nothing drawn past the queue
                        whole = ref_compared - compared == 1 && rastq_stats.solo[RASTQ_SOLO_DIRECT] == 0;
                        for (k = 0; k < gpu_count; k++)
                            whole = whole && gpu_ops[k].op != OP_RECT;
                        check_ok = rastq_stats.gpu_check_pixels > 0 && ref_compared > compared &&
                                   (!whole || (ref_cpu_matched && ref_gpu_matched)) &&
                                   differing(result[2], result[4]) == 0;
                        gpu_whole_checks += whole;
                        rastq_use_gpu(0);
                        rastq_set_gpu(NULL);
                        if (getenv("RASTQ_GPU_EXPLAIN") != NULL && strcmp(getenv("RASTQ_GPU_EXPLAIN"), "bad") != 0) {
                            printf("  %dx%d frame %d: %.2f%% differ, %.3f%% not near\n", cw, ch, f,
                                   100.0 * diff / ((double)cw * ch), 100.0 * far / ((double)cw * ch));
                            if (strcmp(getenv("RASTQ_GPU_EXPLAIN"), "frames") != 0 &&
                                strcmp(getenv("RASTQ_GPU_EXPLAIN"), "bad") != 0)
                                explain_gpu(gpu_ops, gpu_count);
                        }
                        if (!drawn_ok || !check_ok) {
                            if (bad < 10)
                                printf("  %dx%d frame %d (%d calls), GPU path: %ld pixels differ from the "
                                       "mappers' (%.1f%%), %ld of them not near one that matches (%.2f%%), comparison %s\n",
                                       cw, ch, f, gpu_count, diff, 100.0 * diff / ((double)cw * ch), far,
                                       100.0 * far / ((double)cw * ch), check_ok ? "ok" : "not ok");
                            if (bad < 10 && getenv("RASTQ_GPU_EXPLAIN") != NULL &&
                                strcmp(getenv("RASTQ_GPU_EXPLAIN"), "bad") == 0) {
                                printf("    comparisons %u, cpu matched %d, check run differs from plain run by %ld, "
                                       "check pixels %llu\n",
                                       ref_compared - compared, ref_cpu_matched, differing(result[2], result[4]),
                                       rastq_stats.gpu_check_pixels);
                                explain_gpu(gpu_ops, gpu_count);
                            }
                            bad++;
                        }
                    }
                    consistent_background = 0;
                    grd_ipal = ipal;
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
#ifndef RASTQ_REFERENCE
    printf("  GPU path: %u calls in %u scenes, %u culled, %u drawn by the CPU\n", gpu_polys, gpu_scenes, gpu_culled,
           gpu_cpu_calls);
    if (gpu_whole_checks == 0) {
        printf("  no scene was compared in one go\n");
        bad++;
    }
    printf("  %u lists cut by all the threads, %u calls of which %u by the workers; %u pictures differ from one "
           "thread's\n",
           cut_lists, cut_calls, cut_by_workers, cut_bad);
    if (cut_lists == 0 || cut_calls == 0 || cut_bad != 0)
        bad++;
    printf("  the GPU was left its last scene %u times; %u views drawn by the CPU meanwhile\n", left_out, other_views);
    if (left_out == 0 || other_views == 0 || other_bad || other_lost || ref_unfinished) {
        printf("  of those views %u came out wrong and %u had the GPU waited for; %u scenes begun on one not "
               "waited for\n",
               other_bad, other_lost, ref_unfinished);
        bad++;
    }
    printf("  lit polygons in %u pieces; %.2f%% of pixels differ from the mappers', %.3f%% not near one that matches\n",
           gpu_pieces, 100.0 * gpu_diff / total, 100.0 * gpu_far / total);
    if (gpu_far > GPU_RUN_LIMIT * total) {
        printf("  that is more than %.2f%%\n", 100.0 * GPU_RUN_LIMIT);
        bad++;
    }
#endif
    printf("  %ld frames differ\n", bad);
    return bad != 0;
}
