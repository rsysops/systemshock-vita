// Record, then replay in row bands: see rastq.h and docs/PERFORMANCE-CPU.md.

#if defined(VITA_PROFILE) && !defined(RASTQ_SELFCHECK)
#define RASTQ_SELFCHECK
#endif

#include <math.h>
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>

#include "rastq.h"

#include "2d.h"
#include "band.h"
#include "clpcon.h"
#include "clpltab.h"
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
// Room kept free after every record. It must exceed the unpack buffer, so
// that a bitmap living there, the biggest a call can bring, always fits.
#define RASTQ_HEADROOM (256 * 1024)
// Rows added on each side of a call's vertices to bound the rows it may
// write: the mappers' edge walks can end a row beyond them.
#define RASTQ_ROW_MARGIN 2
// More than the threads: the tests replay in up to this many bands.
#define RASTQ_MAX_BANDS 16
// A band is never thinner than this share of the view.
#define RASTQ_MIN_BAND 0.05f

_Static_assert(RASTQ_THREADS == RASTQ_MAX_THREADS, "one stats entry per thread slot");

enum { RQ_TMAP, RQ_POLY, RQ_LINE, RQ_CLINE, RQ_POINT };
enum { RQ_RECORDED, RQ_DIRECT, RQ_DROPPED };
// The lines a mapper draws a polygon in
enum {
    GPU_SCAN_ROWS,
    GPU_SCAN_COLUMNS,
    GPU_SCAN_DEPTH, // lines of one depth, as the perspective mapper slants them
};
// What becomes of a call in a view the GPU draws
enum {
    GPU_CPU,  // nothing the GPU path does yet: drawn by the CPU, in its place
    GPU_FLAT, // a polygon in one palette index
    GPU_TMAP, // a texture map
    GPU_CULL, // wound the way the mappers draw nothing for
    GPU_SHADED, // a polygon shaded between its corners' colours
    GPU_LINE,   // a line, in one colour or shaded
    GPU_POINT,
};

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
    // set by gpu_classify:
    uchar gpu;       // GPU_*
    uchar gpu_flags; // RASTQ_GPU_*
    uchar gpu_row;   // table row, or the colour of a GPU_FLAT
    uchar gpu_lit;   // the row is each vertex's light level instead
    uchar gpu_flat_light; // which doesn't go through the perspective division
    uchar gpu_persp; // the vertices' w is the perspective divisor
    uchar gpu_carry; // u beyond the bitmap's width moves on to its next row
    uchar gpu_scan;  // GPU_SCAN_*: the lines the call's mapper draws
    uchar gpu_kind;  // RASTQ_GPU_KIND_*, for the counts
    uchar gpu_why;   // GPU_CPU: RASTQ_GPU_WHY_*
    // set by gpu_prepare when the list was cut ahead of the hand-over:
    uchar cut;      // the call's pieces are in a sink
    uchar cut_slot; // which thread's
    unsigned cut_first, cut_count; // which pieces of it
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
    const rastq_gpu *gpu;
    int use_gpu;   // hand views to the GPU
    int gpu_view;  // this view goes to the GPU
    int gpu_check; // this view: compare the GPU's result with the CPU's
    int gpu_survey; // this view isn't the GPU's: only count what it could draw of it
    int survey_next;
    int gpu_view_next; // the caller's word on the view that starts next
    int gpu_cut_min;   // lists of at least this many calls are cut by all the threads
    int gpu_out;       // a scene the GPU has and hasn't been waited for
    int gpu_out_timed; // that scene's times count
    long long gpu_out_us; // when it was handed over
    int ending;        // the flush under way is the view's last
    int clear_pending; // the view's canvas is still to be cleared
    int clear_color;
    int active;
    int checking;
    grs_canvas *canvas;
    unsigned count;
    size_t used;

    // the view being recorded
    rastq_split *balance; // NULL: this view isn't split
    unsigned view_batches;
    long long view_finish_us[RASTQ_THREADS];

    // the replay under way
    int bands;                      // 1: not split
    int rows[RASTQ_MAX_BANDS + 1];  // band k is rows [rows[k], rows[k + 1])
    int only_band;                  // >= 0: draw this band alone, for the leak check
    int test_bands;                 // > 0: draw band after band on the calling thread
    int test_bounds[RASTQ_MAX_BANDS];
    int test_ranges;
} rq = {.threads = 1, .only_band = -1, .gpu_cut_min = RASTQ_GPU_CUT_CALLS};

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

void rastq_set_gpu(const rastq_gpu *gpu) { rq.gpu = gpu; }

void rastq_use_gpu(int on) { rq.use_gpu = on; }

// Profile builds alternate what they compare (see vprof.h).
static void profile_settings(void) {
#if defined(__vita__) && defined(VITA_PROFILE)
    rq.mode = RASTQ_TRUST_STABLE;
    rq.use_gpu = vprof_variant != 0;
    rq.min_rows = RASTQ_SMALL_VIEW_ROWS;
#endif
}

// Small views stay on the CPU, as they stay on one thread.
int rastq_gpu_next(int rows) {
    profile_settings();
    return rq.gpu != NULL && rq.use_gpu && rq.mode != RASTQ_OFF && rows >= rq.min_rows;
}

static int gpu_takes(const grs_canvas *canvas) { return rastq_gpu_next(canvas->bm.h); }

void rastq_gpu_view(int on) { rq.gpu_view_next = on; }

void rastq_gpu_survey(void) { rq.survey_next = 1; }

void rastq_set_gpu_cut(int min_calls) { rq.gpu_cut_min = min_calls; }

// Waits for the scene the GPU still has, if it has one.
static void gpu_finish(void) {
    long long start, now;

    if (!rq.gpu_out)
        return;
    start = rastq_clock_us();
    rq.gpu->finish();
    now = rastq_clock_us();
    rq.gpu_out = 0;
    if (rq.gpu_out_timed) {
        rastq_stats.gpu_overlap_us += (unsigned long long)(start - rq.gpu_out_us);
        rastq_stats.gpu_wait_us += (unsigned long long)(now - start);
    }
}

int rastq_gpu_busy(void) { return rq.gpu_out; }

void rastq_gpu_finish(void) { gpu_finish(); }

int rastq_gpu_clear(int color) {
    if (!rq.gpu_view_next || !gpu_takes(grd_canvas))
        return 0;
    rq.clear_pending = 1;
    rq.clear_color = color;
    return 1;
}

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
    } else if (rq.balance != NULL) {
        float below = 0;
        rq.bands = rq.balance->threads;
        for (i = 1; i < rq.bands; i++) {
            below += rq.balance->share[i - 1];
            rq.rows[i] = (int)(below * h + 0.5f);
        }
    }
    rq.rows[0] = GR_BAND_TOP;
    rq.rows[rq.bands] = GR_BAND_BOT;
}

// ---- replay --------------------------------------------------------------

// Draws one recorded call inside `band`, which must be the calling thread's.
// The mappers modify their arguments, so each thread draws from its own copy.
// A line of the 3D library, as it draws them: clipped, in one colour or
// shaded between the colours in its ends' u, v and w.
static void draw_line_now(int shaded, long color, intptr_t parm, const grs_vertex *v0, const grs_vertex *v1) {
    grs_vertex a = *v0, b = *v1;
    ((int (*)(long, long, grs_vertex *, grs_vertex *))
         grd_line_clip_fill_vector[shaded ? GR_WIRE_POLY_CLINE : GR_WIRE_POLY_LINE])(color, (long)parm, &a, &b);
}

static void draw_cmd(const rastq_cmd *c, const grs_band *band) {
    grs_vertex verts[RASTQ_MAX_VERTS];
    grs_vertex *vpl[RASTQ_MAX_VERTS];
    grs_bitmap bm;
    grs_tmap_info ti;
    int i;

    if (c->row_bot <= band->top || c->row_top >= band->bot)
        return;
    if (c->kind == RQ_LINE || c->kind == RQ_CLINE) {
        draw_line_now(c->kind == RQ_CLINE, c->color, c->fill_parm, &c->verts[0], &c->verts[1]);
        return;
    }
    if (c->kind == RQ_POINT) {
        long color = gr_get_fcolor();
        gr_set_fcolor(c->color);
        ((int (*)(short, short))grd_canvas_table[DRAW_POINT])((short)fix_int(c->verts[0].x), (short)fix_int(c->verts[0].y));
        gr_set_fcolor(color);
        return;
    }
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

// Draws the batch on the calling thread, inside band `band` of the split.
// The threads are at different calls at any one time, so each keeps the
// drawing state of its call in its own band, not in the canvas.
static void draw_batch(int slot, int band) {
    grs_band *b = &grd_bands[slot];
    unsigned k;

    b->top = rq.rows[band];
    b->bot = rq.rows[band + 1];
    for (k = batch.first; k < batch.end; k++) {
        const rastq_cmd *c = &cmds[k];

        if (c->row_bot > b->top && c->row_top < b->bot) {
            // what gr_set_fill_type and the canvas would hold for this call
            b->own_state = 1;
            b->fill_type = c->fill_type;
            b->fill_parm = c->fill_parm;
            b->table = (*grd_function_fill_table)[c->fill_type];
            b->clip = &c->clip;
        }
        draw_cmd(c, b);
    }
    *b = full_band;
}

// What each thread does with a batch: slot s draws band s.
static void batch_job(int slot) {
    batch.begin_us[slot] = rastq_clock_us();
    draw_batch(slot, slot);
#ifdef VITA_PROFILE
    rastq_stats.cpu[slot] = rastq_thread_cpu();
#endif
    batch.end_us[slot] = rastq_clock_us();
}

// Draws cmds[first..end), which are band-safe.
static void run_batch(unsigned first, unsigned end) {
    int i;

    batch.first = first;
    batch.end = end;

    if (rq.only_band >= 0) {
        draw_batch(0, rq.only_band);
        return;
    }
    if (rq.test_bands > 0) {
        for (i = 0; i < rq.bands; i++)
            draw_batch(0, i);
        return;
    }

    batch.start_us = rastq_clock_us();
    VPROF_RUN(VPROF_RASTER, rastq_threads_run(batch_job, rq.threads));
    {
        long long now = rastq_clock_us();
        rastq_stats.wait_us += now - batch.end_us[0];
        for (i = 0; i < rq.threads; i++) {
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

static void replay(void) {
    unsigned k = 0, end;

    while (k < rq.count) {
        rastq_cmd *c = &cmds[k];

        if (rq.bands == 1) {
            apply_state(c);
            VPROF_RUN(VPROF_RASTER, draw_cmd(c, &full_band));
            k++;
        } else if (!c->band_safe) {
            // The other threads are idle, so this call's place in the
            // drawing order is kept.
            if (rq.only_band < 0) {
                apply_state(c);
                VPROF_RUN(VPROF_RASTER, draw_cmd(c, &full_band));
                if (!rq.checking)
                    rastq_stats.solo[RASTQ_SOLO_MAPPER]++;
            }
            k++;
        } else {
            // a whole run of band-safe calls is handed out at once
            for (end = k + 1; end < rq.count && cmds[end].band_safe; end++)
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

// ---- replay on a GPU -------------------------------------------------------

static uchar gpu_tables[RASTQ_GPU_TABLE_ROWS][256];
static const uchar *gpu_table_from[RASTQ_GPU_TABLE_ROWS]; // what each added row is a copy of
static int gpu_rows;
static int gpu_colours; // the list has calls shaded between colours
static long long gpu_lap_us;

// Adds the time since the last lap to a counter.
static void gpu_lap(unsigned long long *counter) {
    long long now = rastq_clock_us();
    if (!rq.gpu_check)
        *counter += (unsigned long long)(now - gpu_lap_us);
    gpu_lap_us = now;
}

// The clear a view starts with, when it wasn't left to the GPU after all
static void clear_on_cpu(void) {
    int32_t fill_type = grd_canvas->gc.fill_type;

    if (!rq.clear_pending)
        return;
    rq.clear_pending = 0;
    // as the view's own clear would be drawn, not as the last recorded call
    if (fill_type != FILL_NORM)
        gr_set_fill_type(FILL_NORM);
    gr_clear(rq.clear_color);
    if (fill_type != FILL_NORM)
        gr_set_fill_type(fill_type);
}

// The row of the scene's tables that holds a colour table, added if need be.
// -1 if there is no room for it.
static int gpu_table_row(const uchar *clut) {
    static atomic_flag adding = ATOMIC_FLAG_INIT;
    const uchar *ltab = gr_get_light_tab();
    int r, row = -1;

    if (ltab != NULL && (uintptr_t)clut >= (uintptr_t)ltab &&
        (uintptr_t)clut < (uintptr_t)ltab + RASTQ_GPU_LIGHT_ROWS * 256 && ((uintptr_t)clut - (uintptr_t)ltab) % 256 == 0)
        return (int)(((uintptr_t)clut - (uintptr_t)ltab) / 256);
    // several threads may be deciding calls at once: one at a time here
    while (atomic_flag_test_and_set(&adding))
        rastq_threads_relax();
    for (r = RASTQ_GPU_PLAIN_ROW + 1; r < gpu_rows && row < 0; r++)
        if (gpu_table_from[r] == clut)
            row = r;
    if (row < 0 && gpu_rows < RASTQ_GPU_TABLE_ROWS) {
        memcpy(gpu_tables[gpu_rows], clut, 256);
        gpu_table_from[gpu_rows] = clut;
        row = gpu_rows++;
    }
    atomic_flag_clear(&adding);
    return row;
}

// The mappers stop at the first row whose right edge is left of its left
// edge. For a polygon wound anticlockwise on screen that is its first row:
// they draw nothing, and neither must the GPU.
static int gpu_reversed(const rastq_cmd *c) {
    double area = 0;
    int i;

    for (i = 0; i < c->n; i++) {
        const grs_vertex *a = &c->verts[i], *b = &c->verts[(i + 1) % c->n];
        area += (double)a->x * b->y - (double)b->x * a->y;
    }
    return area < 0;
}

// A call the GPU path doesn't draw: the CPU will, for this reason.
static int gpu_leave(rastq_cmd *c, int why) {
    c->gpu_why = (uchar)why;
    return GPU_CPU;
}

// The floor and wall mappers don't take a polygon's depth as its vertices
// have it. h_umap and v_umap replace each vertex's w by a straight line
// across the rows (or columns): from the w of the first to the w of the last,
// over the whole number of rows between them. The texture and the light then
// follow that line, which for a wall seen at an angle is a pixel or two away
// from the true perspective in the middle. This gives the same w, in the
// same arithmetic. 0 if one of them isn't above zero.
static int gpu_scan_w(const rastq_cmd *c, fix *w) {
    int by_x = c->gpu_scan == GPU_SCAN_COLUMNS, first = 0, i, positive = 1;
    fix s0, w_min, w_max, dw;
    uint32_t s, s_min, s_max;

#define SCAN_AT(k) (by_x ? c->verts[k].x : c->verts[k].y)
    s_min = s_max = (uint32_t)fix_cint(SCAN_AT(0));
    w_min = w_max = c->verts[0].w;
    for (i = 1; i < c->n; i++) {
        s = (uint32_t)fix_cint(SCAN_AT(i));
        if (s < s_min) {
            s_min = s;
            w_min = c->verts[i].w;
            first = i;
        }
        if (s > s_max) {
            s_max = s;
            w_max = c->verts[i].w;
        }
    }
    if (s_min == s_max) { // the mapper draws nothing
        for (i = 0; i < c->n; i++)
            w[i] = c->verts[i].w;
        return 1;
    }
    while (w_max > 0x20000) {
        w_max >>= 2;
        w_min >>= 2;
    }
    dw = (w_max - w_min) / (int32_t)(s_max - s_min);
    s0 = SCAN_AT(first);
    for (i = 0; i < c->n; i++) {
        w[i] = w_min + fix_mul(SCAN_AT(i) - s0, dw);
        positive = positive && w[i] > 0;
    }
#undef SCAN_AT
    return positive;
}

// The mapper family per_umap draws a call with: its own, or the one it hands
// the polygon to. -1 if it draws nothing.
static int gpu_per_family(const rastq_cmd *c) {
    grs_vertex verts[RASTQ_GPU_VERTS];
    grs_vertex *vpl[RASTQ_GPU_VERTS];
    int i, family;

    for (i = 0; i < c->n; i++) {
        verts[i] = c->verts[i];
        vpl[i] = &verts[i];
    }
    family = gr_per_umap_family(c->n, vpl);
    // the wall mapper it hands over to has a one-dimensional cousin, the one
    // the 3D library asks for: the same to the GPU
    return family == GRC_WALL2D ? GRC_WALL1D : family;
}

// Says what the GPU does with a call, and with which table and addressing.
static int gpu_classify(rastq_cmd *c) {
    int type, family, shade, pow2, i;

    if (c->kind == RQ_LINE || c->kind == RQ_POINT) {
        // as gri_flat8_wire_poly_uline takes its colour
        if (c->fill_type == FILL_NORM)
            c->gpu_row = (uchar)c->color;
        else if (c->kind == RQ_LINE && c->fill_type == FILL_CLUT)
            c->gpu_row = ((const uchar *)c->fill_parm)[(uchar)c->color];
        else if (c->kind == RQ_LINE && c->fill_type == FILL_SOLID)
            c->gpu_row = (uchar)c->fill_parm;
        else
            return gpu_leave(c, RASTQ_GPU_WHY_FILL);
        c->gpu_kind = c->kind == RQ_LINE ? RASTQ_GPU_KIND_LINE : RASTQ_GPU_KIND_POINT;
        return c->kind == RQ_LINE ? GPU_LINE : GPU_POINT;
    }
    if (c->kind == RQ_CLINE) {
        if (c->fill_type != FILL_NORM)
            return gpu_leave(c, RASTQ_GPU_WHY_FILL);
        if (grd_ipal == NULL || rq.gpu->shaded == NULL)
            return gpu_leave(c, RASTQ_GPU_WHY_OTHER);
        c->gpu_kind = RASTQ_GPU_KIND_LINE;
        return GPU_LINE;
    }

    if (c->n > RASTQ_GPU_VERTS)
        return gpu_leave(c, RASTQ_GPU_WHY_VERTS);

    if (c->kind == RQ_POLY && c->index == FIX_UCPOLY) {
        // gri_cpoly_init; the colour-table fill has a loop of its own
        if (c->fill_type != FILL_NORM)
            return gpu_leave(c, RASTQ_GPU_WHY_FILL);
        if (grd_ipal == NULL || rq.gpu->shaded == NULL)
            return gpu_leave(c, RASTQ_GPU_WHY_OTHER);
        c->gpu_scan = GPU_SCAN_ROWS;
        c->gpu_kind = RASTQ_GPU_KIND_SHADED;
        return gpu_reversed(c) ? GPU_CULL : GPU_SHADED;
    }
    if (c->kind == RQ_POLY) {
        if (c->index == FIX_USPOLY)
            return gpu_leave(c, RASTQ_GPU_WHY_SHADED_POLY);
        if (c->index == FIX_TLUC8_UPOLY || c->index == FIX_TLUC8_SPOLY)
            return gpu_leave(c, RASTQ_GPU_WHY_TLUC_POLY);
        if (c->index != FIX_UPOLY)
            return gpu_leave(c, RASTQ_GPU_WHY_OTHER_POLY);
        // as gri_poly_init, gri_clut_poly_init and gri_solid_poly_init
        if (c->fill_type == FILL_NORM)
            c->gpu_row = (uchar)c->color;
        else if (c->fill_type == FILL_CLUT)
            c->gpu_row = ((const uchar *)c->fill_parm)[(uchar)c->color];
        else if (c->fill_type == FILL_SOLID)
            c->gpu_row = (uchar)c->fill_parm;
        else
            return gpu_leave(c, RASTQ_GPU_WHY_FILL);
        c->gpu_kind = RASTQ_GPU_KIND_FLAT;
        return gpu_reversed(c) ? GPU_CULL : GPU_FLAT;
    }

    if (c->bm.type == BMT_TLUC8)
        return gpu_leave(c, RASTQ_GPU_WHY_TLUC_BITMAP);
    // Under the colour-table fill type, which the game sets for 3D objects
    // lit as a whole, every mapper is its colour-table one, with the fill's
    // table (fl8ft.c): the light level and the call's own table count for
    // nothing.
    if (c->fill_type != FILL_NORM && c->fill_type != FILL_CLUT)
        return gpu_leave(c, RASTQ_GPU_WHY_FILL);
    if (c->bm.type != BMT_FLAT8 || c->bm.w != c->bm.row)
        return gpu_leave(c, RASTQ_GPU_WHY_OTHER);
    // The families the 3D library asks for, each with its mapper: plain,
    // lit and through a colour table, in that order, two entries apart.
    type = c->ti.tmap_type;
    if (type >= GRC_BILIN && type <= GRC_CLUT_BILIN && c->func == h_umap)
        family = GRC_BILIN;
    else if (type >= GRC_FLOOR && type <= GRC_CLUT_FLOOR && c->func == h_umap)
        family = GRC_FLOOR;
    else if (type >= GRC_WALL1D && type <= GRC_CLUT_WALL1D && c->func == v_umap)
        family = GRC_WALL1D;
    else if (type >= GRC_PER && type <= GRC_CLUT_PER && c->func == per_umap)
        family = GRC_PER;
    else
        return gpu_leave(c, RASTQ_GPU_WHY_OTHER);
    shade = type - family;

    c->gpu_flags = (c->bm.flags & BMF_TRANS) ? RASTQ_GPU_TRANS : 0;
    c->gpu_carry = 0;
    pow2 = c->bm.row == (1 << c->bm.wlog) && c->bm.h == (1 << c->bm.hlog);
    if (family == GRC_PER) {
        const grs_clip *clip = &c->clip;
        if (!pow2)
            return gpu_leave(c, RASTQ_GPU_WHY_SIZE);
        // the one mapper here that looks at the clip rectangle
        if (clip->i.left > 0 || clip->i.top > 0 || clip->i.right < rq.canvas->bm.w || clip->i.bot < rq.canvas->bm.h)
            return gpu_leave(c, RASTQ_GPU_WHY_CLIP);
        // per_umap hands a polygon that is flat enough, or lies like a floor
        // or a wall, to the mapper for that: the call is then one of theirs.
        family = gpu_per_family(c);
        if (family < 0)
            return gpu_leave(c, RASTQ_GPU_WHY_OTHER);
    }
    if (family == GRC_PER) {
        c->gpu_flags |= RASTQ_GPU_WRAP;
    } else if (pow2) {
        c->gpu_flags |= RASTQ_GPU_WRAP;
        c->gpu_carry = 1; // see gpu_emit_repeats
    } else if (family != GRC_BILIN) {
        // Only sprites have other sizes, and those go through the linear
        // mapper. (The lit floor mapper doesn't even step along a row of
        // such a bitmap.)
        return gpu_leave(c, RASTQ_GPU_WHY_SIZE);
    }

    // The linear mapper ignores w; the 3D library doesn't even set it then.
    c->gpu_persp = family != GRC_BILIN;
    c->gpu_scan = family == GRC_PER ? GPU_SCAN_DEPTH : family == GRC_WALL1D ? GPU_SCAN_COLUMNS : GPU_SCAN_ROWS;
    if (c->gpu_persp) {
        fix w[RASTQ_GPU_VERTS];
        for (i = 0; i < c->n; i++)
            if (c->verts[i].w <= 0)
                return gpu_leave(c, RASTQ_GPU_WHY_OTHER);
        if (c->gpu_scan != GPU_SCAN_DEPTH && !gpu_scan_w(c, w))
            return gpu_leave(c, RASTQ_GPU_WHY_OTHER);
    }

    c->gpu_lit = 0;
    if (c->fill_type == FILL_CLUT) {
        int row = gpu_table_row((const uchar *)c->fill_parm);
        if (row < 0)
            return gpu_leave(c, RASTQ_GPU_WHY_OTHER);
        c->gpu_row = (uchar)row;
        c->gpu_kind = RASTQ_GPU_KIND_CLUT;
    } else if (shade == 0) {
        c->gpu_row = RASTQ_GPU_PLAIN_ROW;
        c->gpu_kind = RASTQ_GPU_KIND_PLAIN;
    } else if (shade == GRC_LIT_BILIN - GRC_BILIN) {
        // g_ltab[texel + fix_light(i)]: the light level picks the row
        for (i = 0; i < c->n; i++)
            if (c->verts[i].i < 0 || c->verts[i].i >= fix_make(RASTQ_GPU_LIGHT_ROWS, 0))
                return gpu_leave(c, RASTQ_GPU_WHY_LIGHT);
        c->gpu_lit = 1;
        // The floor and wall mappers divide the light level by w along with
        // the texture coordinates; the perspective mapper steps it along
        // its scanlines.
        c->gpu_flat_light = family == GRC_PER;
        c->gpu_kind = RASTQ_GPU_KIND_LIT;
    } else if (shade == GRC_CLUT_BILIN - GRC_BILIN && (c->ti.flags & TMF_CLUT)) {
        int row = gpu_table_row(c->ti.clut != NULL ? c->ti.clut : gr_get_clut());
        if (row < 0)
            return gpu_leave(c, RASTQ_GPU_WHY_OTHER);
        c->gpu_row = (uchar)row;
        c->gpu_kind = RASTQ_GPU_KIND_CLUT;
    } else {
        return gpu_leave(c, RASTQ_GPU_WHY_OTHER);
    }
    return gpu_reversed(c) ? GPU_CULL : GPU_TMAP;
}

static void gpu_cut_list(void);

// The tables of the list's scenes, and what each call becomes. A long list
// of a view the GPU draws is cut into its pieces as well, by all the threads
// (see gpu_cut_list).
static void gpu_prepare(void) {
    const uchar *ltab = gr_get_light_tab();
    unsigned k;
    int i;

    if (ltab != NULL)
        memcpy(gpu_tables, ltab, RASTQ_GPU_LIGHT_ROWS * 256);
    for (i = 0; i < 256; i++)
        gpu_tables[RASTQ_GPU_PLAIN_ROW][i] = (uchar)i;
    gpu_rows = RASTQ_GPU_PLAIN_ROW + 1;
    if (rq.gpu_view && rq.threads > 1 && rq.count >= (unsigned)rq.gpu_cut_min) {
        gpu_cut_list();
    } else {
        for (k = 0; k < rq.count; k++) {
            cmds[k].gpu = (uchar)gpu_classify(&cmds[k]);
            cmds[k].cut = 0;
        }
    }
    gpu_colours = 0;
    for (k = 0; k < rq.count; k++)
        gpu_colours |= cmds[k].gpu == GPU_SHADED || (cmds[k].gpu == GPU_LINE && cmds[k].kind == RQ_CLINE);
}

static int gpu_scene_begin(void) {
    grs_bitmap *bm = &rq.canvas->bm;

    gpu_finish(); // a scene nobody waited for
    if (!rq.gpu->begin(bm->bits, bm->w, bm->h, bm->row, &gpu_tables[0][0], gpu_rows, gpu_colours ? grd_ipal : NULL))
        return 0;
    if (rq.clear_pending) {
        rastq_gpu_vertex v[4];
        int i;
        memset(v, 0, sizeof(v));
        v[1].x = v[2].x = (float)bm->w;
        v[2].y = v[3].y = (float)bm->h;
        for (i = 0; i < 4; i++) {
            v[i].q = v[i].width = v[i].depth = 1.0f;
            v[i].left = RASTQ_GPU_PLAIN_ROW + 0.5f;
        }
        rq.clear_pending = 0;
        rq.gpu->flat(4, v, rq.clear_color);
    }
    if (!rq.gpu_check)
        rastq_stats.gpu_scenes++;
    return 1;
}

// Hands the scene to the GPU. With wait, the canvas holds it on return;
// without, the GPU is left to it (see rastq_gpu_finish).
static void gpu_scene_end(int wait) {
    gpu_lap(&rastq_stats.gpu_submit_us);
    rq.gpu->end();
    gpu_lap(&rastq_stats.gpu_wait_us);
    rq.gpu_out = 1;
    rq.gpu_out_timed = !rq.gpu_check;
    rq.gpu_out_us = gpu_lap_us;
    if (wait) {
        gpu_finish();
        gpu_lap_us = rastq_clock_us();
    }
}

// A texture is taken to repeat no more than this many times along a polygon.
#define GPU_MAX_REPEATS 16
// Cutting a polygon adds vertices before a part of it is handed over.
#define GPU_WORK_VERTS (RASTQ_GPU_VERTS + 8)
#define GPU_LIGHT_NUDGE (1.0f / 256.0f)

typedef struct {
    rastq_gpu_vertex v;
    float s;
} slab_vertex;

// Keeps the part of a convex polygon where s >= bound (keep_above) or
// s <= bound, with the values of the vertices it adds taken along the edges.
static int slab_clip(const slab_vertex *in, int n, float bound, int keep_above, slab_vertex *out) {
    int i, m = 0;

    for (i = 0; i < n; i++) {
        const slab_vertex *a = &in[i], *b = &in[(i + 1) % n];
        int a_in = keep_above ? a->s >= bound : a->s <= bound;
        int b_in = keep_above ? b->s >= bound : b->s <= bound;

        if (a_in)
            out[m++] = *a;
        if (a_in != b_in) {
            float t = (bound - a->s) / (b->s - a->s);
            slab_vertex *o = &out[m++];
            o->v.x = a->v.x + t * (b->v.x - a->v.x);
            o->v.y = a->v.y + t * (b->v.y - a->v.y);
            o->v.u = a->v.u + t * (b->v.u - a->v.u);
            o->v.v = a->v.v + t * (b->v.v - a->v.v);
            o->v.q = a->v.q + t * (b->v.q - a->v.q);
            o->v.left = a->v.left + t * (b->v.left - a->v.left);
            o->v.span = a->v.span + t * (b->v.span - a->v.span);
            o->v.along = a->v.along + t * (b->v.along - a->v.along);
            o->v.width = a->v.width + t * (b->v.width - a->v.width);
            o->v.depth = a->v.depth + t * (b->v.depth - a->v.depth);
            o->v.g_left = a->v.g_left + t * (b->v.g_left - a->v.g_left);
            o->v.g_span = a->v.g_span + t * (b->v.g_span - a->v.g_span);
            o->v.b_left = a->v.b_left + t * (b->v.b_left - a->v.b_left);
            o->v.b_span = a->v.b_span + t * (b->v.b_span - a->v.b_span);
            o->s = bound;
        }
    }
    return m;
}

// ---- pieces kept for the hand-over ------------------------------------------
//
// Deciding what a call is and cutting it into pieces needs nothing of the
// GPU, and for a view of many lit polygons it is most of the sending. A long
// list is therefore gone through by all the threads first, each taking calls
// a few at a time and keeping their pieces in a sink of its own; the main
// thread then hands them to the GPU call by call, in the list's order.

enum { PUT_FLAT, PUT_TMAP, PUT_SHADED };

typedef struct {
    unsigned first; // of its vertices, in the sink
    uchar n, put;
} gpu_piece;

#define SINK_PIECES 4096
#define SINK_VERTS 8192

typedef struct {
    gpu_piece pieces[SINK_PIECES];
    rastq_gpu_vertex verts[SINK_VERTS];
    unsigned piece_count, vert_count;
    unsigned light_pieces; // for the counts
    unsigned calls;        // it went through
} gpu_sink;

static gpu_sink sinks[RASTQ_THREADS];

// A piece of a call: to the GPU (sink NULL; 0 if the scene has no room for
// it), or into a sink (0 if that is full).
static int gpu_put(gpu_sink *sink, const rastq_cmd *c, int put, int n, const rastq_gpu_vertex *v) {
    gpu_piece *piece;

    if (sink == NULL)
        return put == PUT_TMAP     ? rq.gpu->tmap(&c->bm, c->gpu_flags, n, v)
               : put == PUT_SHADED ? rq.gpu->shaded(n, v)
                                   : rq.gpu->flat(n, v, c->gpu_row);
    if (sink->piece_count == SINK_PIECES || sink->vert_count + n > SINK_VERTS)
        return 0;
    piece = &sink->pieces[sink->piece_count++];
    piece->first = sink->vert_count;
    piece->n = (uchar)n;
    piece->put = (uchar)put;
    memcpy(&sink->verts[sink->vert_count], v, n * sizeof(v[0]));
    sink->vert_count += n;
    return 1;
}

// The row, floor and wall mappers read bits[(v * width + u) mod size]: where
// a texture repeats along u, each repeat is one texel row further down than
// the last. A GPU wraps u and v each on their own, so the polygon is cut
// where u passes a multiple of the width (a straight line on screen) and
// each part's v moved by as many rows as it is repeats along. 0 if the
// scene has no room for it.
static int gpu_emit_repeats(gpu_sink *sink, const rastq_cmd *c, int count, const rastq_gpu_vertex *v) {
    slab_vertex poly[GPU_WORK_VERTS], kept[GPU_WORK_VERTS], part[GPU_WORK_VERTS];
    rastq_gpu_vertex out[GPU_WORK_VERTS];
    float width = (float)c->bm.w, u_min, u_max;
    int i, k, k_min, k_max, n;

    if (!c->gpu_carry)
        return gpu_put(sink, c, PUT_TMAP, count, v);
    u_min = u_max = v[0].u / v[0].q;
    for (i = 1; i < count; i++) {
        float u = v[i].u / v[i].q;
        u_min = u < u_min ? u : u_min;
        u_max = u > u_max ? u : u_max;
    }
    k_min = (int)floorf(u_min / width);
    k_max = (int)floorf(u_max / width);
    if (k_max - k_min > GPU_MAX_REPEATS || count > RASTQ_GPU_VERTS - 2)
        return gpu_put(sink, c, PUT_TMAP, count, v);

    for (k = k_min; k <= k_max; k++) {
        n = count;
        for (i = 0; i < n; i++)
            part[i].v = v[i];
        if (k_min != k_max) {
            // u >= k * width, then u <= (k + 1) * width
            for (i = 0; i < n; i++) {
                poly[i].v = v[i];
                poly[i].s = v[i].u - k * width * v[i].q;
            }
            n = slab_clip(poly, n, 0, 1, kept);
            for (i = 0; i < n; i++)
                kept[i].s = kept[i].v.u - (k + 1) * width * kept[i].v.q;
            n = slab_clip(kept, n, 0, 0, part);
            if (n < 3)
                continue;
        }
        for (i = 0; i < n; i++) {
            out[i] = part[i].v;
            out[i].v += k * out[i].q;
        }
        if (!gpu_put(sink, c, PUT_TMAP, n, out))
            return 0;
    }
    return 1;
}

// The light values of a piece of a lit polygon that lies between two of the
// lines its mapper draws, at `lo` and `hi`. Each vertex comes with its light
// value in `left`, the w it is divided by in `depth`, and its position along
// such a line in `along`. On each of the two lines the piece has one vertex
// or two: the mapper's light there runs from the value at one to the value
// at the other, so every vertex on the line gets that value, the difference,
// the distance between the two and its own distance from the first. Those
// four are then linear all over the piece, and (left + span * along / width)
// / depth is the mapper's light at any pixel of it.
static void gpu_light_piece(rastq_gpu_vertex *v, const float *s, int count, float lo, float hi) {
    float middle = 0.5f * (lo + hi);
    int side, i;

    for (side = 0; side < 2; side++) {
        int first = -1, last = -1;

        for (i = 0; i < count; i++) {
            if ((s[i] > middle) != side)
                continue;
            if (first < 0 || v[i].along < v[first].along)
                first = i;
            if (last < 0 || v[i].along > v[last].along)
                last = i;
        }
        if (first < 0)
            continue;
        {
            float left = v[first].left, span = v[last].left - left;
            float from = v[first].along, width = v[last].along - from;
            // a polygon shaded between colours has two more values like it
            float g_left = v[first].g_left, g_span = v[last].g_left - g_left;
            float b_left = v[first].b_left, b_span = v[last].b_left - b_left;

            for (i = 0; i < count; i++) {
                if ((s[i] > middle) != side)
                    continue;
                v[i].left = left;
                v[i].span = span;
                v[i].along -= from;
                v[i].width = width;
                v[i].g_left = g_left;
                v[i].g_span = g_span;
                v[i].b_left = b_left;
                v[i].b_span = b_span;
            }
        }
    }
}

// A lit polygon, cut along the lines its mapper draws: rows, columns, or
// lines of one depth. The mappers take a pixel's light level between the
// polygon's edges along such a line; a GPU takes it across a triangle, which
// for a quad whose corners' levels don't fit one plane gives straight bands
// with a crease where the mappers' are round.
//
// Cut at the level of each of its corners, a polygon falls into pieces that
// lie between two such lines with one edge on either side, and on those the
// mapper's light is a formula in values the GPU interpolates exactly (see
// gpu_light_piece).
//
// The texture's repeats are cut within each piece: such a cut runs through
// the polygon, and only values that are linear over the piece survive it.
// 0 if the scene has no room for it.
static int gpu_hand_over(gpu_sink *sink, const rastq_cmd *c, int count, const rastq_gpu_vertex *v) {
    return c->gpu == GPU_SHADED ? gpu_put(sink, c, PUT_SHADED, count, v) : gpu_emit_repeats(sink, c, count, v);
}

static int gpu_emit_cut(gpu_sink *sink, const rastq_cmd *c, int count, const rastq_gpu_vertex *v) {
    slab_vertex poly[GPU_WORK_VERTS], above[GPU_WORK_VERTS], piece[GPU_WORK_VERTS];
    rastq_gpu_vertex out[GPU_WORK_VERTS];
    float bounds[GPU_WORK_VERTS + 1], levels[GPU_WORK_VERTS];
    float along_x = 1, along_y = 0; // the direction of the mapper's lines
    float s_min, s_max, apart, lo;
    int scan = c->gpu_scan, i, k, pieces, n, m;

    if (scan == GPU_SCAN_DEPTH) {
        // q is linear on screen: its gradient, by least squares, is across
        // the lines of one depth
        float sxx = 0, sxy = 0, syy = 0, sxq = 0, syq = 0, det, gx, gy, length;
        for (i = 1; i < count; i++) {
            float dx = v[i].x - v[0].x, dy = v[i].y - v[0].y, dq = v[i].q - v[0].q;
            sxx += dx * dx;
            sxy += dx * dy;
            syy += dy * dy;
            sxq += dx * dq;
            syq += dy * dq;
        }
        det = sxx * syy - sxy * sxy;
        gx = det != 0 ? (sxq * syy - syq * sxy) / det : 0;
        gy = det != 0 ? (syq * sxx - sxq * sxy) / det : 0;
        length = sqrtf(gx * gx + gy * gy);
        if (length > 0) {
            along_x = -gy / length;
            along_y = gx / length;
        } else {
            scan = GPU_SCAN_ROWS; // one depth all over
        }
    }
    if (scan == GPU_SCAN_COLUMNS) {
        along_x = 0;
        along_y = 1;
    }
    for (i = 0; i < count; i++) {
        poly[i].v = v[i];
        poly[i].s = scan == GPU_SCAN_COLUMNS ? v[i].x : scan == GPU_SCAN_DEPTH ? v[i].q : v[i].y;
        poly[i].v.along = along_x * v[i].x + along_y * v[i].y;
    }
    s_min = s_max = poly[0].s;
    for (i = 1; i < count; i++) {
        s_min = poly[i].s < s_min ? poly[i].s : s_min;
        s_max = poly[i].s > s_max ? poly[i].s : s_max;
    }
    if (s_max <= s_min)
        return gpu_hand_over(sink, c, count, v);

    // Where to cut, bounds[1 .. pieces - 1]: the corners' levels in order,
    // those that are all but one taken once.
    apart = (s_max - s_min) * 1e-5f;
    for (i = 0; i < count; i++) {
        float level = poly[i].s;
        for (k = i; k > 0 && levels[k - 1] > level; k--)
            levels[k] = levels[k - 1];
        levels[k] = level;
    }
    pieces = 1;
    bounds[0] = s_min;
    for (i = 0; i < count; i++)
        if (levels[i] > s_min + apart && levels[i] < s_max - apart && levels[i] > bounds[pieces - 1] + apart)
            bounds[pieces++] = levels[i];
    bounds[pieces] = s_max;

    n = count;
    memcpy(above, poly, n * sizeof(poly[0]));
    lo = s_min;
    for (k = 1; k <= pieces; k++) {
        float s[GPU_WORK_VERTS];

        // the piece below this bound, and what is left above it
        if (k < pieces) {
            m = slab_clip(above, n, bounds[k], 0, piece);
            n = slab_clip(above, n, bounds[k], 1, poly);
            memcpy(above, poly, n * sizeof(poly[0]));
        } else {
            m = n;
            memcpy(piece, above, n * sizeof(poly[0]));
        }
        if (m >= 3) {
            for (i = 0; i < m; i++) {
                out[i] = piece[i].v;
                s[i] = piece[i].s;
            }
            gpu_light_piece(out, s, m, lo, bounds[k]);
            if (!gpu_hand_over(sink, c, m, out))
                return 0;
            if (sink != NULL)
                sink->light_pieces++;
            else if (!rq.gpu_check)
                rastq_stats.gpu_pieces++;
        }
        lo = bounds[k];
    }
    return 1;
}

// A colour from 0 to 256 is looked up by its eighth, rounded down. One that
// sits exactly on a multiple of 8, as most corners' do, mustn't fall on
// either side of it from one pixel to the next: nudged up, by far less than
// the steps the 3D library's colours come in.
#define GPU_COLOUR_NUDGE (1.0f / 64.0f)

// The colours of a shaded line, each a plane over the screen.
typedef struct {
    float at[3], by_x[3], by_y[3];
} line_colours;

// A four-cornered part of a line, in the line's colour or colours.
static int line_quad(const rastq_cmd *c, const float (*xy)[2], const line_colours *col) {
    rastq_gpu_vertex v[4];
    int i;

    memset(v, 0, sizeof(v));
    for (i = 0; i < 4; i++) {
        v[i].x = xy[i][0];
        v[i].y = xy[i][1];
        v[i].q = v[i].width = v[i].depth = 1.0f;
        v[i].left = RASTQ_GPU_PLAIN_ROW + 0.5f;
        if (col != NULL) {
            v[i].left = col->at[0] + col->by_x[0] * v[i].x + col->by_y[0] * v[i].y;
            v[i].g_left = col->at[1] + col->by_x[1] * v[i].x + col->by_y[1] * v[i].y;
            v[i].b_left = col->at[2] + col->by_x[2] * v[i].x + col->by_y[2] * v[i].y;
        }
    }
    return gpu_put(NULL, c, col != NULL ? PUT_SHADED : PUT_FLAT, 4, v);
}

// The pixels from `from` up to, not including, `to` of one row.
static int line_row(const rastq_cmd *c, int from, int to, int row, const line_colours *col) {
    float xy[4][2] = {{from - 0.5f, row - 0.5f}, {to - 0.5f, row - 0.5f}, {to - 0.5f, row + 0.5f}, {from - 0.5f, row + 0.5f}};
    return to <= from || line_quad(c, xy, col);
}

// A line, as gri_flat8_wire_poly_uline and its shaded cousin draw one: not
// one pixel a step, but row after row the pixels between where the line
// enters the row and where it leaves it, at least one. The first and the
// last row are cut at the line's ends, and are given as they come out; the
// rows between are a strip with the line's slope. 0 if the scene has no room
// for it.
static int gpu_emit_line(const rastq_cmd *c) {
    grs_vertex a = c->verts[0], b = c->verts[1];
    const grs_vertex *top, *bot;
    int shaded = c->kind == RQ_CLINE, y, y_max, x, x_new, d, k;
    fix dx, x_fix, x_first;
    line_colours colours, *col = shaded ? &colours : NULL;
    float r0[3], r1[3], step[3];

    // clipped as the CPU's is, under the call's clip rectangle
    grd_canvas->gc.clip = c->clip;
    if ((shaded ? gri_cline_clip(&a, &b) : gri_line_clip(&a, &b)) == CLIP_ALL)
        return 1;

    top = b.y > a.y ? &a : &b;
    bot = b.y > a.y ? &b : &a;
    y = fix_cint(top->y);
    y_max = fix_cint(bot->y);

    if (y_max - y <= 1) {
        // one row, from its left end to its right
        const grs_vertex *left = b.x > a.x ? &a : &b, *right = b.x > a.x ? &b : &a;
        x = fix_cint(left->x);
        x_new = fix_cint(right->x);
        if (shaded) {
            d = x_new - x;
            if (d <= 0)
                return 1; // the shaded loop draws nothing then
            r0[0] = (float)left->u, r0[1] = (float)left->v, r0[2] = (float)left->w;
            r1[0] = (float)right->u, r1[1] = (float)right->v, r1[2] = (float)right->w;
            for (k = 0; k < 3; k++) {
                colours.by_x[k] = (r1[k] - r0[k]) / d;
                colours.by_y[k] = 0;
                colours.at[k] = r0[k] - colours.by_x[k] * x + GPU_COLOUR_NUDGE;
            }
        }
        return line_row(c, x, x_new > x ? x_new : x + 1, y, col);
    }

    dx = fix_div(bot->x - top->x, bot->y - top->y);
    x = fix_cint(top->x);
    x_fix = x_first = top->x + fix_mul(fix_ceil(top->y) - top->y, dx); // the line at row y
    x_new = fix_cint(x_fix);

    if (shaded) {
        // The colours step once a pixel drawn: along x for a line that
        // runs more across than down, along y otherwise.
        int across = fix_abs(dx) > FIX_UNIT;
        d = across ? abs(fix_cint(bot->x) - fix_cint(top->x)) : y_max - y;
        r0[0] = (float)top->u, r0[1] = (float)top->v, r0[2] = (float)top->w;
        r1[0] = (float)bot->u, r1[1] = (float)bot->v, r1[2] = (float)bot->w;
        // The first row's stretch up to the line is one pixel at least,
        // even when the line starts in the pixel it is at on that row: the
        // pixel is then drawn again by what follows, one step further on.
        int again = x == x_new;
        for (k = 0; k < 3; k++) {
            step[k] = d > 0 ? (r1[k] - r0[k]) / d : 0;
            if (across) {
                // counted from the first pixel drawn: the leftmost, or
                // going left the one before the line's end
                colours.by_x[k] = dx >= 0 ? step[k] : -step[k];
                colours.by_y[k] = 0;
                colours.at[k] = r0[k] - colours.by_x[k] * (dx >= 0 ? x - again : x - 1 + again) + GPU_COLOUR_NUDGE;
            } else {
                // a pixel a row; going right the first row has drawn two
                colours.by_x[k] = 0;
                colours.by_y[k] = step[k];
                colours.at[k] = r0[k] - step[k] * (dx >= 0 ? y - 1 : y) + GPU_COLOUR_NUDGE;
            }
        }
    }

    {
        int rows = y_max - y, last = y_max - 1, first_to, first_from, last_from, last_to;
        fix x_last; // the line at the row before the last, or at the last
        float slope = (float)(dx / 65536.0), wide = slope < 0 ? -slope : slope;

        if (wide < 1.0f)
            wide = 1.0f;
        if (dx >= 0) {
            // row y: from the line's end to where it leaves the row;
            // row r: from the line at r to the line at r + 1;
            // the last: from the line there to its other end
            fix next = x_fix + dx;
            first_from = x;
            first_to = fix_cint(next) > x_new ? fix_cint(next) : x_new + 1;
            x_last = x_fix + (rows - 1) * dx;
            last_from = fix_cint(x_last);
            last_to = fix_cint(bot->x) > last_from ? fix_cint(bot->x) : last_from + 1;
        } else {
            // going left, a row runs from the line at that row back to the
            // line at the row above; the first from the line's end
            first_from = x_new;
            first_to = x > x_new ? x : x_new + 1;
            x_last = x_fix + (rows - 2) * dx;
            last_from = fix_cint(bot->x);
            last_to = fix_cint(x_last) > last_from ? fix_cint(x_last) : last_from + 1;
            if (fix_cint(x_last) == last_from)
                last_from = last_to - 1;
        }
        if (!line_row(c, first_from, first_to, y, col) || !line_row(c, last_from, last_to, last, col))
            return 0;
        if (rows > 2) {
            // rows y + 1 to last - 1: [line, line + wide) at each
            float at = (float)(x_first / 65536.0), y0 = y + 0.5f, y1 = last - 0.5f;
            float x0 = at + (y0 - y) * slope, x1 = at + (y1 - y) * slope;
            float xy[4][2] = {{x0, y0}, {x0 + wide, y0}, {x1 + wide, y1}, {x1, y1}};
            if (!line_quad(c, xy, col))
                return 0;
        }
    }
    return 1;
}

// A point is its pixel, if the clip rectangle has it.
static int gpu_emit_point(const rastq_cmd *c) {
    int x = fix_int(c->verts[0].x), y = fix_int(c->verts[0].y);

    if (x < c->clip.i.left || x >= c->clip.i.right || y < c->clip.i.top || y >= c->clip.i.bot)
        return 1;
    return line_row(c, x, x + 1, y, NULL);
}

// Hands a call to the scene under way, or its pieces to a sink. 0 if the
// scene, or the sink, has no room for it. Lines and points go to the scene
// only: their clipping uses the canvas's clip rectangle.
static int gpu_emit(gpu_sink *sink, const rastq_cmd *c) {
    rastq_gpu_vertex v[RASTQ_GPU_VERTS];
    fix w[RASTQ_GPU_VERTS];
    float w_max = 1.0f;
    int i;

    if (c->gpu == GPU_LINE)
        return gpu_emit_line(c);
    if (c->gpu == GPU_POINT)
        return gpu_emit_point(c);
    if (c->gpu == GPU_TMAP && c->gpu_persp) {
        if (c->gpu_scan == GPU_SCAN_DEPTH) {
            for (i = 0; i < c->n; i++)
                w[i] = c->verts[i].w;
        } else {
            gpu_scan_w(c, w);
        }
        w_max = 0;
        for (i = 0; i < c->n; i++)
            if ((float)w[i] > w_max)
                w_max = (float)w[i];
    }
    for (i = 0; i < c->n; i++) {
        const grs_vertex *p = &c->verts[i];

        v[i].x = (float)(p->x / 65536.0);
        v[i].y = (float)(p->y / 65536.0);
        v[i].q = 1.0f;
        v[i].u = v[i].v = 0;
        // one row of the tables for the whole polygon, until told otherwise
        v[i].left = RASTQ_GPU_PLAIN_ROW + 0.5f;
        v[i].span = v[i].along = 0;
        v[i].width = v[i].depth = 1.0f;
        v[i].g_left = v[i].g_span = v[i].b_left = v[i].b_span = 0;
        if (c->gpu == GPU_SHADED) {
            // the 3D library puts red, green and blue in u, v and w
            v[i].left = (float)(p->u / 65536.0) + GPU_COLOUR_NUDGE;
            v[i].g_left = (float)(p->v / 65536.0) + GPU_COLOUR_NUDGE;
            v[i].b_left = (float)(p->w / 65536.0) + GPU_COLOUR_NUDGE;
        }
        if (c->gpu == GPU_TMAP) {
            if (c->gpu_persp)
                v[i].q = (float)w[i] / w_max;
            v[i].u = (float)(p->u / 65536.0) * v[i].q;
            v[i].v = (float)(p->v / 65536.0) * v[i].q;
            if (!c->gpu_lit) {
                v[i].left = c->gpu_row + 0.5f;
            } else {
                // A level that sits exactly on a row of the table mustn't
                // fall on either side of it from one pixel to the next:
                // nudged up, as the wall mapper nudges it.
                float level = (float)(p->i / 65536.0) + GPU_LIGHT_NUDGE;
                if (level > RASTQ_GPU_LIGHT_ROWS - GPU_LIGHT_NUDGE)
                    level = RASTQ_GPU_LIGHT_ROWS - GPU_LIGHT_NUDGE;
                // The floor and wall mappers step the level times w along
                // the edges and divide by w; the others step the level.
                v[i].left = c->gpu_flat_light ? level : level * v[i].q;
                v[i].depth = c->gpu_flat_light ? 1.0f : v[i].q;
            }
        }
    }
    if (c->gpu == GPU_FLAT)
        return gpu_put(sink, c, PUT_FLAT, c->n, v);
    // The mappers take a colour along the edges, then across each row: a
    // polygon of four corners or more is cut at their rows, as for the light.
    if (c->gpu == GPU_SHADED)
        return c->n >= 4 && c->n <= RASTQ_GPU_VERTS - 4 ? gpu_emit_cut(sink, c, c->n, v)
                                                        : gpu_put(sink, c, PUT_SHADED, c->n, v);
    // Three light levels always fit one plane, so a lit triangle stays
    // whole: its level and its w, each linear, give the mapper's quotient.
    if (c->gpu_lit && c->n >= 4 && c->n <= RASTQ_GPU_VERTS - 4)
        return gpu_emit_cut(sink, c, c->n, v);
    return gpu_emit_repeats(sink, c, c->n, v);
}

// ---- the list decided and cut by all the threads ----------------------------

#define CUT_CHUNK 8
static struct {
    atomic_uint next, finished; // calls taken, and done with
    unsigned count;
} cutting;

static void cut_job(int slot) {
    gpu_sink *sink = &sinks[slot];

    for (;;) {
        unsigned first = atomic_fetch_add(&cutting.next, CUT_CHUNK), end, k;

        // A thread that comes to the job late ends up here, and leaves.
        if (first >= cutting.count)
            return;
        end = first + CUT_CHUNK < cutting.count ? first + CUT_CHUNK : cutting.count;
        for (k = first; k < end; k++) {
            rastq_cmd *c = &cmds[k];

            c->gpu = (uchar)gpu_classify(c);
            c->cut = 0;
            if (c->gpu == GPU_FLAT || c->gpu == GPU_TMAP || c->gpu == GPU_SHADED) {
                unsigned pieces = sink->piece_count, verts = sink->vert_count, light = sink->light_pieces;

                if (gpu_emit(sink, c)) {
                    c->cut = 1;
                    c->cut_slot = (uchar)slot;
                    c->cut_first = pieces;
                    c->cut_count = sink->piece_count - pieces;
                } else {
                    // no room in the sink: cut at the hand-over, as a short
                    // list's calls are
                    sink->piece_count = pieces;
                    sink->vert_count = verts;
                    sink->light_pieces = light;
                }
            }
        }
        sink->calls += end - first;
        atomic_fetch_add(&cutting.finished, end - first);
    }
}

// Has all the threads decide what the list's calls are and cut them.
static void gpu_cut_list(void) {
    int i;

    rastq_threads_settle(); // nobody is in cut_job now
    for (i = 0; i < RASTQ_THREADS; i++)
        sinks[i].piece_count = sinks[i].vert_count = sinks[i].light_pieces = sinks[i].calls = 0;
    cutting.count = rq.count;
    atomic_store(&cutting.finished, 0);
    atomic_store(&cutting.next, 0);
    // The workers aren't waited for: one that is held up elsewhere takes
    // what is left when it comes, or nothing.
    rastq_threads_kick(cut_job, rq.threads);
    cut_job(0);
    while (atomic_load(&cutting.finished) < cutting.count)
        rastq_threads_relax();
    for (i = 0; i < RASTQ_THREADS; i++) {
        if (!rq.gpu_check) {
            rastq_stats.gpu_pieces += sinks[i].light_pieces;
            rastq_stats.gpu_cut[i] += sinks[i].calls;
        }
    }
}

// Hands a call to the scene under way: its pieces if it was cut ahead.
static int gpu_hand(const rastq_cmd *c) {
    const gpu_sink *sink;
    unsigned i;

    if (!c->cut)
        return gpu_emit(NULL, c);
    sink = &sinks[c->cut_slot];
    for (i = 0; i < c->cut_count; i++) {
        const gpu_piece *piece = &sink->pieces[c->cut_first + i];
        if (!gpu_put(NULL, c, piece->put, piece->n, &sink->verts[piece->first]))
            return 0;
    }
    return 1;
}

// Draws the list with the GPU, in as few scenes as it takes: what the GPU
// path doesn't do yet is drawn by the CPU between two scenes, in its place.
// Returns 0, having drawn nothing, if the GPU can't draw into the canvas.
static int gpu_run(void) {
    int in_scene, dead = 0;
    unsigned k;

    gpu_lap_us = rastq_clock_us();
    gpu_prepare();
    gpu_lap(&rastq_stats.gpu_prepare_us);
    in_scene = gpu_scene_begin();
    if (!in_scene)
        return 0;

    for (k = 0; k < rq.count; k++) {
        const rastq_cmd *c = &cmds[k];
        int sent = 0;

        if (c->gpu == GPU_CULL) {
            if (!rq.gpu_check)
                rastq_stats.gpu_culled++;
            continue;
        }
        if (c->gpu != GPU_CPU && !dead) {
            if (!in_scene && !(in_scene = gpu_scene_begin()))
                dead = 1;
            if (in_scene && !(sent = gpu_hand(c))) {
                // the scene is full: the call opens the next one
                gpu_scene_end(1);
                in_scene = gpu_scene_begin();
                if (!in_scene)
                    dead = 1;
                else
                    sent = gpu_hand(c);
            }
        }
        if (sent) {
            if (!rq.gpu_check) {
                rastq_stats.gpu_polys++;
                rastq_stats.gpu_kinds[c->gpu_kind]++;
            }
            continue;
        }
        if (in_scene) {
            gpu_scene_end(1); // the CPU draws on the canvas next
            in_scene = 0;
        }
        clear_on_cpu();
        apply_state(c);
        draw_cmd(c, &full_band);
        if (!rq.gpu_check) {
            rastq_stats.gpu_cpu_calls++;
            rastq_stats.gpu_whys[c->gpu == GPU_CPU ? c->gpu_why : RASTQ_GPU_WHY_OTHER]++;
        }
        gpu_lap(&rastq_stats.gpu_cpu_us);
    }
    // The last scene is left to the GPU: whoever touches the canvas next
    // waits for it.
    if (in_scene)
        gpu_scene_end(0);
    return 1;
}

// For a view the GPU isn't given: what it could draw of the list, and why not
// the rest, in the same counts as for its own views.
static void gpu_survey(void) {
    unsigned k;

    gpu_prepare();
    for (k = 0; k < rq.count; k++) {
        const rastq_cmd *c = &cmds[k];
        if (c->gpu == GPU_CULL)
            rastq_stats.gpu_culled++;
        else if (c->gpu == GPU_CPU)
            rastq_stats.gpu_whys[c->gpu_why]++;
        else
            rastq_stats.gpu_kinds[c->gpu_kind]++;
    }
}

// Has the GPU draw the list. Returns 0 if it wouldn't, with the canvas as it
// was.
static int gpu_replay(void) {
    int ok = 0;

#ifdef RASTQ_SELFCHECK
    if (rq.gpu_check) {
        // A GPU doesn't fill exactly the pixels the software mappers do, nor
        // pick exactly their texels along the edges of one, so this counts
        // how many pixels differ instead of expecting none. Both renderings
        // start from the cleared canvas.
        grs_bitmap *bm = &rq.canvas->bm;
        unsigned differing = 0;
        int x, y;

        clear_on_cpu();
        memcpy(check_before, bm->bits, canvas_bytes());
        if (!gpu_run())
            return 0;
        gpu_finish();
        memcpy(check_direct, bm->bits, canvas_bytes());
        memcpy(bm->bits, check_before, canvas_bytes());
        replay();
        for (y = 0; y < bm->h; y++) {
            const uchar *cpu = bm->bits + (size_t)y * bm->row, *gpu = check_direct + (size_t)y * bm->row;
            for (x = 0; x < bm->w; x++)
                differing += cpu[x] != gpu[x];
        }
        rastq_stats.gpu_check_diff += differing;
        rastq_stats.gpu_check_pixels += (unsigned long long)bm->w * bm->h;
        if (rq.gpu->compared != NULL)
            rq.gpu->compared(check_direct, bm->bits, bm->w, bm->h, bm->row, differing);
        // the view goes on as the GPU drew it, like the frames around it
        memcpy(bm->bits, check_direct, canvas_bytes());
        return 1;
    }
#endif
    VPROF_RUN(VPROF_RASTER, ok = gpu_run());
    return ok;
}

void rastq_flush(void) {
    grs_canvas *prev;
    int32_t fill_type;
    intptr_t fill_parm;
    grs_clip clip;

    if (!rq.active || (rq.count == 0 && !rq.clear_pending))
        return;

    prev = grd_canvas;
    if (prev != rq.canvas)
        gr_set_canvas(rq.canvas);
    fill_type = grd_canvas->gc.fill_type;
    fill_parm = grd_canvas->gc.fill_parm;
    clip = grd_canvas->gc.clip;
    if (rq.count == 0) {
        // whatever comes next is drawn straight to the canvas
        clear_on_cpu();
        goto drawn;
    }
    choose_bands();

    if (rq.gpu_view) {
        if (gpu_replay()) {
            rastq_stats.flushes++;
            // what comes after a flush in the middle of a view is drawn
            // straight to the canvas
            if (!rq.ending)
                gpu_finish();
            goto drawn;
        }
        rastq_stats.gpu_fallbacks++;
        clear_on_cpu();
    }

    if (rq.gpu_survey && !rq.checking)
        gpu_survey();

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

drawn:
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

    profile_settings();
#if defined(__vita__) && defined(VITA_PROFILE)
    if (rq.threads != RASTQ_THREADS)
        rastq_set_threads(RASTQ_THREADS);
#endif
    // a clear left to a GPU that doesn't get this view after all
    if (rq.clear_pending && !(rq.gpu_view_next && gpu_takes(grd_canvas)))
        clear_on_cpu();
    if (rq.mode == RASTQ_OFF)
        return;

    rq.canvas = grd_canvas;
    rq.active = 1;
    rq.count = 0;
    rq.used = 0;
    rq.checking = 0;
    rq.gpu_check = 0;
    rq.gpu_view = rq.gpu_view_next && gpu_takes(rq.canvas);
    rq.gpu_survey = rq.survey_next && !rq.gpu_view;
    rq.survey_next = rq.gpu_view_next = 0;
    rq.balance = NULL;
    if (rq.threads > 1 && rq.canvas->bm.h >= rq.min_rows)
        rq.balance = find_split(rq.canvas, rq.threads);
    rq.view_batches = 0;
    for (i = 0; i < RASTQ_THREADS; i++)
        rq.view_finish_us[i] = 0;
#ifdef RASTQ_SELFCHECK
    if (check_interval != 0 && view % check_interval == 0 && canvas_bytes() <= RASTQ_CHECK_BYTES) {
        // On the CPU the view is also drawn directly and the two compared.
        // The GPU's result is compared with the CPU's when it is drawn.
        if (rq.gpu_view)
            rq.gpu_check = 1;
        else
            rq.checking = 1;
#if defined(__vita__) && defined(VITA_PROFILE)
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
    rq.ending = 1;
    rastq_flush();
    rq.ending = 0;
    if (rq.balance != NULL) {
        // A checked view also drew single bands, which says nothing about
        // how the threads compare.
        if (rq.view_batches != 0 && !rq.checking)
            rebalance(rq.balance, rq.view_finish_us);
        if (rq.canvas->bm.h >= rastq_stats.rows[RASTQ_THREADS]) {
            rastq_stats.rows[0] = 0;
            for (i = 1; i < rq.balance->threads; i++)
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
// call's pixels are copied, never before, so that nothing drawn by the flush
// can come between a call and the copy of its pixels.
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
    grs_tmap_info rti = *ti;
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

    // A sprite for the blend mapper is doubled now, the way that mapper
    // would when drawing, and recorded as what it then draws: a call every
    // thread can take part in. It leaves the unpack buffer as direct drawing
    // would.
    if (func == h_umap && gr_blend_prepare(&rbm, &rti)) {
        pixels = (size_t)rbm.row * rbm.h;
        margin = rbm.row;
        if (pixels + 2 * margin + n * sizeof(grs_vertex) + 16 > arena_free())
            return RQ_DIRECT; // can't happen: the headroom exceeds the unpack buffer
    }

    c = new_cmd(RQ_TMAP, n, vpl);
    c->func = func;
    c->bm = rbm;
    c->ti = rti;
    c->band_safe = (uchar)tmap_band_safe(func, &rbm, &rti);
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

// Lines and points join the list only in a view the GPU draws: they keep
// the list whole there, where every break is a scene to wait for. The CPU
// replays lose nothing by drawing them at once, as before.
static rastq_cmd *record_mark(int kind, long color, int n, const grs_vertex *v0, const grs_vertex *v1) {
    grs_vertex a = *v0, b = *v1;
    grs_vertex *vpl[2] = {&a, &b};
    rastq_cmd *c;

    start_batch();
    c = new_cmd(kind, n, vpl);
    c->color = color;
    c->band_safe = 0;
    return c;
}

void rastq_line(int shaded, long color, const grs_vertex *v0, const grs_vertex *v1) {
    if (recording() && rq.gpu_view) {
        VPROF_RUN(VPROF_RECORD, record_mark(shaded ? RQ_CLINE : RQ_LINE, color, 2, v0, v1));
        flush_if_low();
        return;
    }
    rastq_flush();
    draw_line_now(shaded, color, gr_get_fill_parm(), v0, v1);
}

int rastq_point(short x, short y) {
    if (recording() && rq.gpu_view) {
        grs_vertex v;
        memset(&v, 0, sizeof(v));
        v.x = fix_make(x, 0);
        v.y = fix_make(y, 0);
        VPROF_RUN(VPROF_RECORD, record_mark(RQ_POINT, gr_get_fcolor(), 1, &v, &v));
        flush_if_low();
        return 0; // CLIP_NONE
    }
    rastq_flush();
    return ((int (*)(short, short))grd_canvas_table[DRAW_POINT])(x, y);
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
