// Record, then replay in row bands: see rastq.h and docs/PERFORMANCE.md.

#if defined(VITA_PROFILE) && !defined(RASTQ_SELFCHECK)
#define RASTQ_SELFCHECK
#endif

#include <stdint.h>
#include <string.h>

#include "rastq.h"

#include "band.h"
#include "general.h"
#include "lg.h"
#include "memall.h"
#include "rastqthr.h"
#include "tmapfcn.h"
#include "tmaptab.h"
#include "tmpalloc.h"
#include "vprof.h"

#ifndef RASTQ_ARENA_BYTES
#define RASTQ_ARENA_BYTES (4 * 1024 * 1024)
#endif
#define RASTQ_MAX_CMDS 4096
#define RASTQ_MAX_VERTS 100
// Room kept free after every record. It must exceed the unpack buffer, so a
// bitmap living there is always copied before a replay can overwrite it.
#define RASTQ_HEADROOM (256 * 1024)
// Rows added on each side of a call's vertices to bound the rows it may
// write: the mappers' edge walks can end a row beyond them.
#define RASTQ_ROW_MARGIN 2
#define RASTQ_MAX_BANDS 8
// A band is never thinner than this share of the view.
#define RASTQ_MIN_BAND 0.05f

_Static_assert(RASTQ_THREADS == RASTQ_MAX_THREADS, "one stats entry per thread slot");

enum { RQ_TMAP, RQ_POLY };
enum { RQ_RECORDED, RQ_DIRECT, RQ_DROPPED };

typedef struct {
    uchar kind;
    uchar n;
    uchar band_safe; // every mapper this call can reach honours row bands
    short index;     // RQ_POLY: entry of grd_canvas_table
    rastq_tmap_func func;
    long color;
    grs_vertex *verts;
    grs_bitmap bm;
    grs_tmap_info ti;
    int32_t fill_type;
    intptr_t fill_parm;
    grs_clip clip;
    int row_top, row_bot; // the call writes no row outside [row_top, row_bot)
} rastq_cmd;

// How one view's rows are shared between the threads. It is kept per canvas,
// because views of different sizes balance differently.
typedef struct {
    const uchar *bits;
    int w, h, threads;
    float share[RASTQ_THREADS];
    unsigned last_used;
} rastq_split;

static struct {
    int mode;
    int threads;  // threads a replay is split across
    int min_rows; // views with fewer rows aren't split
    int active;
    int checking;
    grs_canvas *canvas;
    unsigned count;
    size_t used;

    // the view being recorded
    rastq_split *split;
    unsigned view_batches;
    long long view_finish_us[RASTQ_THREADS];

    // the replay under way
    int bands;                      // 1: not split
    int rows[RASTQ_MAX_BANDS + 1];  // band k is rows [rows[k], rows[k + 1])
    int only_band;                  // >= 0: draw this band alone, for the leak check
    int test_bands;                 // > 0: draw band after band on the calling thread
    int test_bounds[RASTQ_MAX_BANDS];
    int test_ranges;
} rq = {.threads = 1, .only_band = -1};

// The run of calls the threads are drawing
static struct {
    unsigned first, end;
    long long start_us;
    long long begin_us[RASTQ_THREADS], end_us[RASTQ_THREADS];
} batch;

static rastq_cmd cmds[RASTQ_MAX_CMDS];
static uchar arena[RASTQ_ARENA_BYTES];

static struct {
    const uchar *pixels;
    size_t size;
} stable[4];
static int stable_count;

static rastq_split splits[4];
static unsigned split_clock;

rastq_stats_t rastq_stats;

static const grs_band full_band = {GR_BAND_TOP, GR_BAND_BOT};

#ifdef RASTQ_SELFCHECK
// Room for a 960x544 canvas
#define RASTQ_CHECK_BYTES (960 * 544)
static uchar check_before[RASTQ_CHECK_BYTES];
static uchar check_direct[RASTQ_CHECK_BYTES];
// An odd number, so that with two views a frame the check takes them in turn.
static unsigned check_interval = 63;

void rastq_set_check_interval(unsigned n) { check_interval = n; }

void rastq_test_bands(int bands, const int *bounds) {
    int i;
    rq.test_bands = bands > RASTQ_MAX_BANDS ? RASTQ_MAX_BANDS : bands;
    for (i = 0; i + 1 < rq.test_bands; i++)
        rq.test_bounds[i] = bounds[i];
}

void rastq_test_ranges(int on) { rq.test_ranges = on; }

static size_t canvas_bytes(void) { return (size_t)rq.canvas->bm.row * rq.canvas->bm.h; }

// Rows of the canvas outside [top, bot) that differ from `before`
static unsigned changed_rows_outside(const uchar *before, int top, int bot) {
    unsigned n = 0;
    int y;
    for (y = 0; y < rq.canvas->bm.h; y++) {
        size_t at = (size_t)y * rq.canvas->bm.row;
        if ((y < top || y >= bot) && memcmp(rq.canvas->bm.bits + at, before + at, rq.canvas->bm.w) != 0)
            n++;
    }
    return n;
}
#endif

void rastq_set_mode(int mode) { rq.mode = mode; }

int rastq_set_threads(int threads) {
    if (threads < 1)
        threads = 1;
    if (threads > 1)
        threads = rastq_threads_start(threads);
    if (threads != rq.threads) {
        rq.threads = threads;
        rastq_threads_pin_main(threads > 1);
    }
    return rq.threads;
}

void rastq_set_min_rows(int rows) { rq.min_rows = rows; }

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

// ---- the split of a view's rows between the threads ----------------------

static rastq_split *find_split(const grs_canvas *canvas, int threads) {
    rastq_split *s = &splits[0];
    int i;

    for (i = 0; i < (int)(sizeof(splits) / sizeof(splits[0])); i++) {
        if (splits[i].bits == canvas->bm.bits && splits[i].w == canvas->bm.w && splits[i].h == canvas->bm.h &&
            splits[i].threads == threads) {
            splits[i].last_used = ++split_clock;
            return &splits[i];
        }
        if (splits[i].last_used < s->last_used)
            s = &splits[i];
    }
    s->bits = canvas->bm.bits;
    s->w = canvas->bm.w;
    s->h = canvas->bm.h;
    s->threads = threads;
    for (i = 0; i < threads; i++)
        s->share[i] = 1.0f / threads;
    s->last_used = ++split_clock;
    return s;
}

// Moves the shares toward what would have made every thread finish at the
// same time: a thread that finished late, for any reason, gets fewer rows.
static void rebalance(rastq_split *s, const long long *finish_us) {
    float rate[RASTQ_THREADS], total = 0;
    int i;

    for (i = 0; i < s->threads; i++) {
        if (finish_us[i] <= 0)
            return;
        rate[i] = s->share[i] / (float)finish_us[i];
        total += rate[i];
    }
    for (i = 0; i < s->threads; i++) {
        s->share[i] = 0.5f * s->share[i] + 0.5f * rate[i] / total;
        if (s->share[i] < RASTQ_MIN_BAND)
            s->share[i] = RASTQ_MIN_BAND;
    }
    total = 0;
    for (i = 0; i < s->threads; i++)
        total += s->share[i];
    for (i = 0; i < s->threads; i++)
        s->share[i] /= total;
}

// Sets rq.bands and rq.rows for a replay on the recorded canvas.
static void choose_bands(void) {
    int h = rq.canvas->bm.h, i;

    rq.bands = 1;
    if (rq.test_bands > 0) {
        rq.bands = rq.test_bands;
        for (i = 1; i < rq.bands; i++)
            rq.rows[i] = rq.test_bounds[i - 1];
    } else if (rq.split != NULL) {
        float below = 0;
        rq.bands = rq.split->threads;
        for (i = 1; i < rq.bands; i++) {
            below += rq.split->share[i - 1];
            rq.rows[i] = (int)(below * h + 0.5f);
        }
    }
    rq.rows[0] = GR_BAND_TOP;
    rq.rows[rq.bands] = GR_BAND_BOT;
}

// ---- replay --------------------------------------------------------------

// Draws one recorded call inside `band`, which must be the calling thread's.
// The mappers modify their arguments, so each thread draws from its own copy.
static void draw_cmd(const rastq_cmd *c, const grs_band *band) {
    grs_vertex verts[RASTQ_MAX_VERTS];
    grs_vertex *vpl[RASTQ_MAX_VERTS];
    grs_bitmap bm;
    grs_tmap_info ti;
    int i;

    if (c->row_bot <= band->top || c->row_top >= band->bot)
        return;
    for (i = 0; i < c->n; i++) {
        verts[i] = c->verts[i];
        vpl[i] = &verts[i];
    }
    if (c->kind == RQ_TMAP) {
        bm = c->bm;
        ti = c->ti;
        c->func(&bm, c->n, vpl, &ti);
    } else {
        ((void (*)(long, int, grs_vertex **))grd_canvas_table[c->index])(c->color, c->n, vpl);
    }
}

static void set_band(int slot, int band) {
    grd_bands[slot].top = rq.rows[band];
    grd_bands[slot].bot = rq.rows[band + 1];
}

static void draw_batch(int slot) {
    unsigned k;
    for (k = batch.first; k < batch.end; k++)
        draw_cmd(&cmds[k], &grd_bands[slot]);
    grd_bands[slot] = full_band;
}

// What each thread does with a batch: slot s draws band s.
static void batch_job(int slot) {
    batch.begin_us[slot] = rastq_clock_us();
    set_band(slot, slot);
    draw_batch(slot);
#ifdef VITA_PROFILE
    rastq_stats.cpu[slot] = rastq_thread_cpu();
#endif
    batch.end_us[slot] = rastq_clock_us();
}

// Draws cmds[first..end), which are band-safe and share one canvas state
// that is already set.
static void run_batch(unsigned first, unsigned end) {
    int i;

    batch.first = first;
    batch.end = end;

    if (rq.only_band >= 0) {
        set_band(0, rq.only_band);
        draw_batch(0);
        return;
    }
    if (rq.test_bands > 0) {
        for (i = 0; i < rq.bands; i++) {
            set_band(0, i);
            draw_batch(0);
        }
        return;
    }

    batch.start_us = rastq_clock_us();
    VPROF_RUN(VPROF_RASTER, rastq_threads_run(batch_job, rq.bands));
    {
        long long now = rastq_clock_us();
        rastq_stats.wait_us += now - batch.end_us[0];
        for (i = 0; i < rq.bands; i++) {
            long long late = batch.begin_us[i] - batch.start_us;
            rastq_stats.busy_us[i] += batch.end_us[i] - batch.begin_us[i];
            rq.view_finish_us[i] += batch.end_us[i] - batch.start_us;
            if (late > rastq_stats.late_us)
                rastq_stats.late_us = (unsigned)late;
        }
        rq.view_batches++;
        if (!rq.checking)
            rastq_stats.batches++;
    }
}

static void apply_state(const rastq_cmd *c) {
    if (grd_canvas->gc.fill_type != c->fill_type)
        gr_set_fill_type(c->fill_type);
    grd_canvas->gc.fill_parm = c->fill_parm;
    grd_canvas->gc.clip = c->clip;
}

static int same_state(const rastq_cmd *a, const rastq_cmd *b) {
    return a->fill_type == b->fill_type && a->fill_parm == b->fill_parm &&
           memcmp(&a->clip, &b->clip, sizeof(a->clip)) == 0;
}

static void replay(void) {
    unsigned k = 0, end;

    while (k < rq.count) {
        rastq_cmd *c = &cmds[k];

        apply_state(c);
        if (rq.bands == 1) {
            VPROF_RUN(VPROF_RASTER, draw_cmd(c, &full_band));
            k++;
        } else if (!c->band_safe) {
            // The other threads are idle, so this call's place in the
            // drawing order is kept.
            if (rq.only_band < 0) {
                VPROF_RUN(VPROF_RASTER, draw_cmd(c, &full_band));
                if (!rq.checking)
                    rastq_stats.solo[RASTQ_SOLO_MAPPER]++;
            }
            k++;
        } else {
            // The workers only read the canvas state, so a batch ends where
            // the state changes.
            for (end = k + 1; end < rq.count && cmds[end].band_safe && same_state(c, &cmds[end]); end++)
                ;
            run_batch(k, end);
            k = end;
        }
    }
}

#ifdef RASTQ_SELFCHECK
// Draws each recorded call alone and counts the rows it changes outside the
// range recorded for it. Leaves the canvas as it was.
static void test_ranges(void) {
    unsigned k;

    if (canvas_bytes() > RASTQ_CHECK_BYTES)
        return;
    memcpy(check_direct, rq.canvas->bm.bits, canvas_bytes());
    for (k = 0; k < rq.count; k++) {
        apply_state(&cmds[k]);
        draw_cmd(&cmds[k], &full_band);
        rastq_stats.check_leak_rows += changed_rows_outside(check_direct, cmds[k].row_top, cmds[k].row_bot);
        memcpy(rq.canvas->bm.bits, check_direct, canvas_bytes());
    }
}
#endif

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
    choose_bands();

#ifdef RASTQ_SELFCHECK
    if (rq.test_ranges && !rq.checking)
        test_ranges();
    if (rq.checking) {
        // The batch was also drawn directly: keep that, rewind, and replay.
        memcpy(check_direct, rq.canvas->bm.bits, canvas_bytes());
        memcpy(rq.canvas->bm.bits, check_before, canvas_bytes());
    }
#endif

    replay();

#ifdef RASTQ_SELFCHECK
    if (rq.checking) {
        rastq_stats.check_bad_rows += changed_rows_outside(check_direct, 0, 0);
        if (rq.bands > 1) {
            // One band drawn alone must leave the rows of the others as they
            // were. The bands take turns.
            rq.only_band = (int)(rastq_stats.check_runs % (unsigned)rq.bands);
            memcpy(rq.canvas->bm.bits, check_before, canvas_bytes());
            replay();
            rastq_stats.check_leak_rows +=
                changed_rows_outside(check_before, rq.rows[rq.only_band], rq.rows[rq.only_band + 1]);
            rq.only_band = -1;
            memcpy(rq.canvas->bm.bits, check_direct, canvas_bytes());
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
    unsigned view = rastq_stats.views++;
    int i;

#if defined(VITA) && defined(VITA_PROFILE)
    // Profile builds alternate what they compare (see vprof.h).
    rq.mode = vprof_variant == 0 ? RASTQ_OFF : RASTQ_TRUST_STABLE;
    rq.min_rows = vprof_variant == 2 ? RASTQ_SMALL_VIEW_ROWS : 0;
    if (rq.threads != RASTQ_THREADS)
        rastq_set_threads(RASTQ_THREADS);
#endif
    if (rq.mode == RASTQ_OFF)
        return;

    rq.canvas = grd_canvas;
    rq.active = 1;
    rq.count = 0;
    rq.used = 0;
    rq.checking = 0;
    rq.split = NULL;
    if (rq.threads > 1 && rq.canvas->bm.h >= rq.min_rows)
        rq.split = find_split(rq.canvas, rq.threads);
    rq.view_batches = 0;
    for (i = 0; i < RASTQ_THREADS; i++)
        rq.view_finish_us[i] = 0;
#ifdef RASTQ_SELFCHECK
    if (check_interval != 0 && view % check_interval == 0 && canvas_bytes() <= RASTQ_CHECK_BYTES) {
        rq.checking = 1;
#if defined(VITA) && defined(VITA_PROFILE)
        // A checked frame draws everything several times: keep it out of the
        // timings.
        vprof_frame_discard();
#endif
    }
#endif
    (void)view;
}

void rastq_end(void) {
    int i;

    if (!rq.active)
        return;
    rastq_flush();
    if (rq.split != NULL) {
        // A checked view also drew single bands, which says nothing about
        // how the threads compare.
        if (rq.view_batches != 0 && !rq.checking)
            rebalance(rq.split, rq.view_finish_us);
        if (rq.canvas->bm.h >= rastq_stats.rows[RASTQ_THREADS]) {
            rastq_stats.rows[0] = 0;
            for (i = 1; i < rq.split->threads; i++)
                rastq_stats.rows[i] = rq.rows[i];
            for (; i <= RASTQ_THREADS; i++)
                rastq_stats.rows[i] = rq.canvas->bm.h;
        }
    }
    rq.active = 0;
    rq.checking = 0;
}

// ---- recording -----------------------------------------------------------

// Flushes once the next record might not fit. This runs after the current
// call's pixels are copied, never before: a replay can overwrite the unpack
// buffer they may live in.
static void flush_if_low(void) {
    if (rq.count == RASTQ_MAX_CMDS || arena_free() < RASTQ_HEADROOM)
        rastq_flush();
}

static rastq_cmd *new_cmd(int kind, int n, grs_vertex **vpl) {
    rastq_cmd *c = &cmds[rq.count++];
    fix y_min, y_max;
    int i;

    c->kind = (uchar)kind;
    c->n = (uchar)n;
    c->verts = arena_alloc(n * sizeof(grs_vertex));
    y_min = y_max = vpl[0]->y;
    for (i = 0; i < n; i++) {
        c->verts[i] = *vpl[i];
        if (vpl[i]->y < y_min)
            y_min = vpl[i]->y;
        if (vpl[i]->y > y_max)
            y_max = vpl[i]->y;
    }
    c->row_top = fix_int(y_min) - RASTQ_ROW_MARGIN;
    c->row_bot = fix_cint(y_max) + RASTQ_ROW_MARGIN;
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

static int safe_entry(int index) { return gr_band_safe_init(grd_tmap_init_table[index]); }

// Do all the mappers this call can end up in honour row bands? It follows
// the choices h_umap, v_umap and per_umap make with the current fill type.
static int tmap_band_safe(rastq_tmap_func func, const grs_bitmap *bm, const grs_tmap_info *ti) {
    int trans = bm->flags & BMF_TRANS;
    int index = trans + ti->tmap_type + GRD_FUNCS * bm->type;

    if (func != h_umap && func != v_umap && func != per_umap)
        return 0;
    // an opaque bitmap with a solid fill is a flat polygon for all three
    if (func == per_umap && trans + 2 * grd_gc.fill_type != 2 * FILL_SOLID) {
        // per_umap picks one of its two scan directions, or decides the
        // polygon is flat enough for the linear, floor or wall mapper
        return safe_entry(index) && safe_entry(index + (GRC_PER_VSCAN - GRC_PER)) &&
               safe_entry(index + (GRC_BILIN - GRC_PER)) && safe_entry(index + (GRC_FLOOR - GRC_PER)) &&
               safe_entry(index + (GRC_WALL2D - GRC_PER));
    }
    return safe_entry(index);
}

static int poly_band_safe(int index) {
    void (*draw)() = grd_canvas_table[index];
    int type;

    // how temptm.c tags each polygon kind for h_umap
    if (draw == (void (*)())temp_upoly)
        type = 0;
    else if (draw == (void (*)())temp_uspoly)
        type = 1;
    else if (draw == (void (*)())temp_ucpoly)
        type = 2;
    else if (draw == (void (*)())temp_utpoly)
        type = 3;
    else
        return 0;
    return safe_entry(GRC_POLY + GRD_FUNCS * type);
}

static int record_bitmap(rastq_tmap_func func, grs_bitmap *bm, int n, grs_vertex **vpl, grs_tmap_info *ti) {
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

    c = new_cmd(RQ_TMAP, n, vpl);
    c->func = func;
    c->bm = rbm;
    c->ti = *ti;
    c->band_safe = (uchar)tmap_band_safe(func, &rbm, ti);
    if (rq.mode != RASTQ_TRUST_STABLE || !is_stable(rbm.bits, pixels)) {
        uchar *copy = arena_alloc(pixels + 2 * margin);
        memcpy(copy, rbm.bits - margin, pixels + 2 * margin);
        c->bm.bits = copy + margin;
        if (!rq.checking)
            rastq_stats.copied += pixels + 2 * margin;
    }
    return RQ_RECORDED;
}

// A call that can't be recorded is drawn at once, after everything before it.
static void before_direct(void) {
    rastq_flush();
    if (!rq.checking)
        rastq_stats.solo[RASTQ_SOLO_DIRECT]++;
}

void rastq_tmap(rastq_tmap_func func, grs_bitmap *bm, int n, grs_vertex **vpl, grs_tmap_info *ti) {
    int r = RQ_DIRECT;

    if (!recording()) {
        VPROF_RUN(VPROF_RASTER, func(bm, n, vpl, ti));
        return;
    }
    VPROF_RUN(VPROF_RECORD, r = record_bitmap(func, bm, n, vpl, ti));
    if (r == RQ_DROPPED)
        return;
    if (r == RQ_DIRECT)
        before_direct();
    if (r == RQ_DIRECT || rq.checking)
        VPROF_RUN(VPROF_RASTER, func(bm, n, vpl, ti));
    flush_if_low();
}

// A sprite is clipped here, as h_map would clip it when drawing, and recorded
// as the clipped polygon. Clipping leaves the lighting of the vertices it
// creates uninitialized, so every thread must draw from this one result.
static int record_sprite(grs_bitmap *bm, int n, grs_vertex **vpl, grs_tmap_info *ti) {
    grs_vertex **cpl = NULL;
    int m = gr_clip_poly(n, 4, vpl, &cpl);
    int r = RQ_DROPPED;

    if (m > 2)
        r = record_bitmap(h_umap, bm, m, cpl, ti);
    gr_free_temp(cpl);
    return r;
}

void rastq_hmap(grs_bitmap *bm, int n, grs_vertex **vpl, grs_tmap_info *ti) {
    int r = RQ_DIRECT;

    if (!recording()) {
        VPROF_RUN(VPROF_RASTER, h_map(bm, n, vpl, ti));
        return;
    }
    VPROF_RUN(VPROF_RECORD, r = record_sprite(bm, n, vpl, ti));
    if (r == RQ_DROPPED)
        return;
    if (r == RQ_DIRECT)
        before_direct();
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
    c->band_safe = (uchar)poly_band_safe(index);
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
        before_direct();
    if (r == RQ_DIRECT || rq.checking)
        VPROF_RUN(VPROF_RASTER, ((void (*)(long, int, grs_vertex **))grd_canvas_table[index])(color, n, vpl));
    flush_if_low();
}
