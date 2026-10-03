// Record, then replay: see rastq.h and docs/PERFORMANCE.md.

#if defined(VITA_PROFILE) && !defined(RASTQ_SELFCHECK)
#define RASTQ_SELFCHECK
#endif

#include <stdint.h>
#include <string.h>

#include "rastq.h"
#include "tmapfcn.h"
#include "vprof.h"

#ifndef RASTQ_ARENA_BYTES
#define RASTQ_ARENA_BYTES (4 * 1024 * 1024)
#endif
#define RASTQ_MAX_CMDS 4096
#define RASTQ_MAX_VERTS 100
// Room kept free after every record. It must exceed the unpack buffer, so a
// bitmap living there is always copied before a replay can overwrite it.
#define RASTQ_HEADROOM (256 * 1024)

enum { RQ_TMAP, RQ_POLY, RQ_HMAP };
enum { RQ_RECORDED, RQ_DIRECT, RQ_DROPPED };

typedef struct {
    uchar kind;
    uchar n;
    short index; // RQ_POLY: entry of grd_canvas_table
    rastq_tmap_func func;
    long color;
    grs_vertex *verts;
    grs_bitmap bm;
    grs_tmap_info ti;
    int32_t fill_type;
    intptr_t fill_parm;
    grs_clip clip;
} rastq_cmd;

static struct {
    int mode;
    int active;
    int checking;
    grs_canvas *canvas;
    unsigned count;
    size_t used;
} rq;

static rastq_cmd cmds[RASTQ_MAX_CMDS];
static uchar arena[RASTQ_ARENA_BYTES];

static struct {
    const uchar *pixels;
    size_t size;
} stable[4];
static int stable_count;

rastq_stats_t rastq_stats;

#ifdef RASTQ_SELFCHECK
// Room for a 960x544 canvas
#define RASTQ_CHECK_BYTES (960 * 544)
static uchar check_before[RASTQ_CHECK_BYTES];
static uchar check_direct[RASTQ_CHECK_BYTES];
static unsigned check_interval = 64;

void rastq_set_check_interval(unsigned n) { check_interval = n; }

static size_t canvas_bytes(void) { return (size_t)rq.canvas->bm.row * rq.canvas->bm.h; }
#endif

void rastq_set_mode(int mode) { rq.mode = mode; }

void rastq_stable_pixels(const uchar *pixels, size_t size) {
    int i;
    for (i = 0; i < stable_count; i++) {
        if (stable[i].pixels == pixels) {
            stable[i].size = size;
            return;
        }
    }
    if (stable_count < (int)(sizeof(stable) / sizeof(stable[0]))) {
        stable[stable_count].pixels = pixels;
        stable[stable_count].size = size;
        stable_count++;
    }
}

static int is_stable(const uchar *pixels, size_t size) {
    int i;
    for (i = 0; i < stable_count; i++) {
        if (pixels >= stable[i].pixels && pixels + size <= stable[i].pixels + stable[i].size)
            return 1;
    }
    return 0;
}

static void *arena_alloc(size_t size) {
    void *p = arena + rq.used;
    rq.used += (size + 7) & ~(size_t)7;
    return p;
}

static size_t arena_free(void) { return sizeof(arena) - rq.used; }

static int recording(void) { return rq.active && grd_canvas == rq.canvas; }

static void replay(void) {
    grs_vertex *vpl[RASTQ_MAX_VERTS];
    unsigned k;
    int i;

    for (k = 0; k < rq.count; k++) {
        rastq_cmd *c = &cmds[k];

        if (grd_canvas->gc.fill_type != c->fill_type)
            gr_set_fill_type(c->fill_type);
        grd_canvas->gc.fill_parm = c->fill_parm;
        grd_canvas->gc.clip = c->clip;
        for (i = 0; i < c->n; i++)
            vpl[i] = &c->verts[i];

        switch (c->kind) {
        case RQ_TMAP:
            VPROF_RUN(VPROF_RASTER, c->func(&c->bm, c->n, vpl, &c->ti));
            break;
        case RQ_POLY:
            VPROF_RUN(VPROF_RASTER,
                      ((void (*)(long, int, grs_vertex **))grd_canvas_table[c->index])(c->color, c->n, vpl));
            break;
        case RQ_HMAP:
            VPROF_RUN(VPROF_RASTER, h_map(&c->bm, c->n, vpl, &c->ti));
            break;
        }
    }
}

void rastq_flush(void) {
    grs_canvas *prev;
    int32_t fill_type;
    intptr_t fill_parm;
    grs_clip clip;

    if (!rq.active || rq.count == 0)
        return;

    prev = grd_canvas;
    if (prev != rq.canvas)
        gr_set_canvas(rq.canvas);
    fill_type = grd_canvas->gc.fill_type;
    fill_parm = grd_canvas->gc.fill_parm;
    clip = grd_canvas->gc.clip;

#ifdef RASTQ_SELFCHECK
    if (rq.checking) {
        // The batch was also drawn directly: keep that, rewind, and replay.
        memcpy(check_direct, rq.canvas->bm.bits, canvas_bytes());
        memcpy(rq.canvas->bm.bits, check_before, canvas_bytes());
    }
#endif

    replay();

#ifdef RASTQ_SELFCHECK
    if (rq.checking) {
        int y;
        for (y = 0; y < rq.canvas->bm.h; y++) {
            size_t at = (size_t)y * rq.canvas->bm.row;
            if (memcmp(rq.canvas->bm.bits + at, check_direct + at, rq.canvas->bm.w) != 0)
                rastq_stats.check_bad_rows++;
        }
        rastq_stats.check_runs++;
    } else
#endif
        rastq_stats.flushes++;

    if (grd_canvas->gc.fill_type != fill_type)
        gr_set_fill_type(fill_type);
    grd_canvas->gc.fill_parm = fill_parm;
    grd_canvas->gc.clip = clip;
    if (prev != rq.canvas)
        gr_set_canvas(prev);

    rq.count = 0;
    rq.used = 0;
}

void rastq_begin(void) {
#if defined(VITA) && defined(VITA_PROFILE)
    // Profile builds alternate the modes to compare them (see vprof.h).
    rq.mode = vprof_variant;
#endif
    if (rq.mode == RASTQ_OFF)
        return;

    rq.canvas = grd_canvas;
    rq.active = 1;
    rq.count = 0;
    rq.used = 0;
    rq.checking = 0;
#ifdef RASTQ_SELFCHECK
    if (check_interval != 0 && rastq_stats.views % check_interval == 0 && canvas_bytes() <= RASTQ_CHECK_BYTES) {
        rq.checking = 1;
#if defined(VITA) && defined(VITA_PROFILE)
        // A checked frame draws everything twice: keep it out of the timings.
        vprof_frame_discard();
#endif
    }
#endif
    rastq_stats.views++;
}

void rastq_end(void) {
    if (!rq.active)
        return;
    rastq_flush();
    rq.active = 0;
    rq.checking = 0;
}

// Flushes once the next record might not fit. This runs after the current
// call's pixels are copied, never before: a replay can overwrite the unpack
// buffer they may live in.
static void flush_if_low(void) {
    if (rq.count == RASTQ_MAX_CMDS || arena_free() < RASTQ_HEADROOM)
        rastq_flush();
}

static rastq_cmd *new_cmd(int kind, int n, grs_vertex **vpl) {
    rastq_cmd *c = &cmds[rq.count++];
    int i;

    c->kind = (uchar)kind;
    c->n = (uchar)n;
    c->verts = arena_alloc(n * sizeof(grs_vertex));
    for (i = 0; i < n; i++)
        c->verts[i] = *vpl[i];
    c->fill_type = grd_canvas->gc.fill_type;
    c->fill_parm = grd_canvas->gc.fill_parm;
    c->clip = grd_canvas->gc.clip;
    if (!rq.checking)
        rastq_stats.cmds++;
    return c;
}

static void start_batch(void) {
#ifdef RASTQ_SELFCHECK
    if (rq.checking && rq.count == 0)
        memcpy(check_before, rq.canvas->bm.bits, canvas_bytes());
#endif
}

static int record_bitmap(int kind, rastq_tmap_func func, grs_bitmap *bm, int n, grs_vertex **vpl,
                         grs_tmap_info *ti) {
    grs_bitmap rbm = *bm;
    size_t pixels, margin, need;
    rastq_cmd *c;

    if (n > RASTQ_MAX_VERTS || (bm->type != BMT_FLAT8 && bm->type != BMT_TLUC8 && bm->type != BMT_RSD8))
        return RQ_DIRECT;
    // per_umap unpacks RSD8 through rsd8_pm_init, which always takes the
    // horizontal-scan mapper; given the unpacked bitmap it may pick the
    // vertical one. Only the compressed bitmap reproduces that.
    if (func == per_umap && bm->type == BMT_RSD8)
        return RQ_DIRECT;
    if (bm->type == BMT_RSD8 && grd_unpack_buf == NULL)
        return RQ_DROPPED; // the mappers draw nothing without an unpack buffer

    // A copy takes one more row on each side of the bitmap. The 1D wall
    // mappers mask the texture row but not the column, which can land a few
    // texels outside the bitmap, so they read just before its start or after
    // its end; the copy must show them what the original's neighbours would.
    pixels = (size_t)bm->row * bm->h;
    margin = bm->row;
    need = pixels + 2 * margin + n * sizeof(grs_vertex) + 16;
    if (need > arena_free()) {
        rastq_flush();
        if (need > arena_free())
            return RQ_DIRECT;
    }
    start_batch();

    // Unpack now what the mapper would unpack when drawing: the same call
    // picks the same mapper for the result.
    if (bm->type == BMT_RSD8 && gr_rsd8_convert(bm, &rbm) != GR_UNPACK_RSD8_OK)
        return RQ_DROPPED;

    c = new_cmd(kind, n, vpl);
    c->func = func;
    c->bm = rbm;
    c->ti = *ti;
    if (rq.mode != RASTQ_TRUST_STABLE || !is_stable(rbm.bits, pixels)) {
        uchar *copy = arena_alloc(pixels + 2 * margin);
        memcpy(copy, rbm.bits - margin, pixels + 2 * margin);
        c->bm.bits = copy + margin;
        if (!rq.checking)
            rastq_stats.copied += pixels + 2 * margin;
    }
    return RQ_RECORDED;
}

void rastq_tmap(rastq_tmap_func func, grs_bitmap *bm, int n, grs_vertex **vpl, grs_tmap_info *ti) {
    int r = RQ_DIRECT;

    if (!recording()) {
        VPROF_RUN(VPROF_RASTER, func(bm, n, vpl, ti));
        return;
    }
    VPROF_RUN(VPROF_RECORD, r = record_bitmap(RQ_TMAP, func, bm, n, vpl, ti));
    if (r == RQ_DROPPED)
        return;
    if (r == RQ_DIRECT)
        rastq_flush();
    if (r == RQ_DIRECT || rq.checking)
        VPROF_RUN(VPROF_RASTER, func(bm, n, vpl, ti));
    flush_if_low();
}

void rastq_hmap(grs_bitmap *bm, int n, grs_vertex **vpl, grs_tmap_info *ti) {
    int r = RQ_DIRECT;

    if (!recording()) {
        VPROF_RUN(VPROF_RASTER, h_map(bm, n, vpl, ti));
        return;
    }
    VPROF_RUN(VPROF_RECORD, r = record_bitmap(RQ_HMAP, NULL, bm, n, vpl, ti));
    if (r == RQ_DROPPED)
        return;
    if (r == RQ_DIRECT)
        rastq_flush();
    if (r == RQ_DIRECT || rq.checking)
        VPROF_RUN(VPROF_RASTER, h_map(bm, n, vpl, ti));
    flush_if_low();
}

static int record_poly(int index, long color, int n, grs_vertex **vpl) {
    rastq_cmd *c;

    // Translucent shaded polygons clip when drawn and light the vertices
    // that creates from stale temporary memory, so deferring would change it.
    if (n > RASTQ_MAX_VERTS || index == FIX_TLUC8_SPOLY)
        return RQ_DIRECT;
    start_batch();
    c = new_cmd(RQ_POLY, n, vpl);
    c->index = (short)index;
    c->color = color;
    return RQ_RECORDED;
}

void rastq_poly(int index, long color, int n, grs_vertex **vpl) {
    int r = RQ_DIRECT;

    if (!recording()) {
        VPROF_RUN(VPROF_RASTER, ((void (*)(long, int, grs_vertex **))grd_canvas_table[index])(color, n, vpl));
        return;
    }
    VPROF_RUN(VPROF_RECORD, r = record_poly(index, color, n, vpl));
    if (r == RQ_DIRECT)
        rastq_flush();
    if (r == RQ_DIRECT || rq.checking)
        VPROF_RUN(VPROF_RASTER, ((void (*)(long, int, grs_vertex **))grd_canvas_table[index])(color, n, vpl));
    flush_if_low();
}
