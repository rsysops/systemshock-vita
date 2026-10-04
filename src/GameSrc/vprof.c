// On-device profiler for the VITA_PROFILE build: see docs/PERFORMANCE.md,
// "How to measure", for the spec this implements.

#ifdef VITA_PROFILE

#include <psp2/kernel/processmgr.h>
#include <stdio.h>
#include <vita2d.h>

#include "Shock.h"
#include "fix.h"
#include "mainloop.h"
#include "rastq.h"
#include "vprof.h"

#define VPROF_WINDOWS_PER_VARIANT 5
#define VPROF_FIXDIV_CHECK_CASES 1000000

// Per-call stats (one sample per VPROF_RUN/VPROF_MARK_END invocation).
// Phases other than VPROF_RASTER fire exactly once per mainloop iteration,
// so these already happen to equal "per frame" for them; VPROF_TRAVERSE,
// VPROF_SENDVIEW and VPROF_RASTER can fire more than once per frame (once
// per fr_rend() view: the main view, plus one per visible hacked security
// camera), so they need the separate per-frame accumulation below instead.
typedef struct {
    SceInt64 total_us;
    SceInt64 max_us;
    int samples;
    int calls; // only meaningful for VPROF_RASTER
} vprof_call_accum_t;

// Per-frame totals: every call to a phase within one game frame (one
// mainloop() iteration, possibly spanning several fr_rend() views) is
// summed here first, then folded into the window once per frame. This is
// what nests correctly against frame_avg/render3d (traverse <= render3d
// <= frame_avg always holds).
typedef struct {
    SceInt64 total_us;
    SceInt64 max_us;
} vprof_frame_accum_t;

static vprof_call_accum_t g_call_accum[VPROF_PHASE_COUNT];
static SceInt64 g_mark_t0[VPROF_PHASE_COUNT];

// A discarded frame leaves no trace: its per-call stats are rolled back to
// this copy, taken when the frame began.
static vprof_call_accum_t g_call_accum_at_frame_begin[VPROF_PHASE_COUNT];
static rastq_stats_t g_rastq_at_frame_begin;
static int g_frame_discard = 0;

// 3D views drawn in the window's frames
static unsigned g_views;

static SceInt64 g_frame_phase_total_us[VPROF_PHASE_COUNT];
static vprof_frame_accum_t g_frame_accum[VPROF_PHASE_COUNT];

static SceInt64 g_frame_t0;
static SceInt64 g_frame_total_us;
static SceInt64 g_frame_max_us;
static int g_frame_samples;

static SceInt64 g_window_start_us;
static short g_window_loop_mode = -1;
static int g_window_open = 0;

int vprof_variant = 0;
static int g_windows_in_variant = 0;

// Music synthesis time, added by the audio thread and taken by the main
// thread once per window, so only these two touch it, atomically.
static unsigned g_audio_us = 0;
static int g_audio_cpu = -1;
static int g_main_cpu = -1;

static int g_startup_done = 0;
static int g_fixdiv_mismatches = -1;
static unsigned g_fixdiv_checked = 0;

static vita2d_pgf *g_pgf = NULL;

static const char *PROFILE_FILENAME = "profile.txt";

static FILE *vprof_open_log(void) {
    char path[256];
    snprintf(path, sizeof(path), "%s%s", VITA_PATH, PROFILE_FILENAME);
    return fopen(path, "a");
}

// One-off checks run before the first frame, logged once at the top of the
// session.
static void vprof_startup_checks(void) {
    FILE *fp;

    g_fixdiv_mismatches = fix_div_selfcheck(VPROF_FIXDIV_CHECK_CASES, &g_fixdiv_checked);
    fp = vprof_open_log();
    if (fp != NULL) {
        fprintf(fp, "fixdiv_check mismatches=%d/%u\n", g_fixdiv_mismatches, g_fixdiv_checked);
        fclose(fp);
    }
}

static void vprof_window_reset(SceInt64 now, short loop_mode) {
    int i;
    for (i = 0; i < VPROF_PHASE_COUNT; i++) {
        g_call_accum[i].total_us = 0;
        g_call_accum[i].max_us = 0;
        g_call_accum[i].samples = 0;
        g_call_accum[i].calls = 0;
        g_frame_phase_total_us[i] = 0;
        g_frame_accum[i].total_us = 0;
        g_frame_accum[i].max_us = 0;
    }
    g_frame_total_us = 0;
    g_frame_max_us = 0;
    g_frame_samples = 0;
    __atomic_store_n(&g_audio_us, 0, __ATOMIC_RELAXED);
    rastq_stats.cmds = 0;
    rastq_stats.flushes = 0;
    rastq_stats.copied = 0;
    rastq_stats.batches = 0;
    rastq_stats.wait_us = 0;
    rastq_stats.late_us = 0;
    for (i = 0; i < RASTQ_SOLO_REASONS; i++)
        rastq_stats.solo[i] = 0;
    for (i = 0; i < RASTQ_THREADS; i++)
        rastq_stats.busy_us[i] = 0;
    g_views = 0;
    g_window_start_us = now;
    g_window_loop_mode = loop_mode;
    g_window_open = 1;
}

static double us_to_ms(SceInt64 us) {
    return (double)us / 1000.0;
}

// ms/frame average for a phase, across however many frames are in the
// window so far (shares the same denominator as frame_avg/fps).
static double frame_avg_ms(vprof_phase_t phase, int frames) {
    return us_to_ms(g_frame_accum[phase].total_us) / (frames ? frames : 1);
}

static double frame_max_ms(vprof_phase_t phase) {
    return us_to_ms(g_frame_accum[phase].max_us);
}

// Music synthesis so far in this window, as a percentage of one core.
static double music_pct(SceInt64 now) {
    SceInt64 elapsed = now - g_window_start_us;
    return elapsed > 0 ? 100.0 * __atomic_load_n(&g_audio_us, __ATOMIC_RELAXED) / elapsed : 0.0;
}

void vprof_audio_add(unsigned micros, int cpu) {
    __atomic_fetch_add(&g_audio_us, micros, __ATOMIC_RELAXED);
    __atomic_store_n(&g_audio_cpu, cpu, __ATOMIC_RELAXED);
}

static void vprof_window_flush(SceInt64 now) {
    FILE *fp;
    double fps;
    double frame_avg, frame_max;
    double raster_calls_per_frame, raster_call_avg_ms;

    if (!g_window_open || g_frame_samples == 0) {
        return;
    }

    frame_avg = us_to_ms(g_frame_total_us) / g_frame_samples;
    frame_max = us_to_ms(g_frame_max_us);
    fps = frame_avg > 0.0 ? 1000.0 / frame_avg : 0.0;

    raster_calls_per_frame = (double)g_call_accum[VPROF_RASTER].calls / g_frame_samples;
    raster_call_avg_ms = us_to_ms(g_call_accum[VPROF_RASTER].total_us) /
                          (g_call_accum[VPROF_RASTER].samples ? g_call_accum[VPROF_RASTER].samples : 1);

    fp = vprof_open_log();
    if (fp != NULL) {
        fprintf(fp,
                "t=%lld mode=%d var=%d fps=%.1f frame_avg=%.2f frame_max=%.2f "
                "input=%.2f/%.2f sim=%.2f/%.2f render3d=%.2f/%.2f "
                "ui2d=%.2f/%.2f present=%.2f/%.2f | "
                "traverse=%.2f/%.2f sendview=%.2f/%.2f raster=%.2f/%.2f "
                "calls_per_frame=%.1f raster_call_avg=%.3f | music=%.1f%% acpu=%d mcpu=%d | "
                "record=%.2f/%.2f cmds=%.1f copied=%.1fKB flushes=%.2f check=%u/%u | "
                "views=%.2f batches=%.1f solo=%.1f/%.1f wait=%.2f busy=%.2f/%.2f/%.2f late=%u "
                "split=%d/%d/%d wcpu=%d/%d/%d leaks=%u | helpscan=%.2f/%.2f\n",
                (long long)(now / 1000000),
                g_window_loop_mode,
                vprof_variant,
                fps,
                frame_avg,
                frame_max,
                frame_avg_ms(VPROF_INPUT, g_frame_samples), frame_max_ms(VPROF_INPUT),
                frame_avg_ms(VPROF_SIM, g_frame_samples), frame_max_ms(VPROF_SIM),
                frame_avg_ms(VPROF_RENDER3D, g_frame_samples), frame_max_ms(VPROF_RENDER3D),
                frame_avg_ms(VPROF_UI2D, g_frame_samples), frame_max_ms(VPROF_UI2D),
                frame_avg_ms(VPROF_PRESENT, g_frame_samples), frame_max_ms(VPROF_PRESENT),
                frame_avg_ms(VPROF_TRAVERSE, g_frame_samples), frame_max_ms(VPROF_TRAVERSE),
                frame_avg_ms(VPROF_SENDVIEW, g_frame_samples), frame_max_ms(VPROF_SENDVIEW),
                frame_avg_ms(VPROF_RASTER, g_frame_samples), frame_max_ms(VPROF_RASTER),
                raster_calls_per_frame,
                raster_call_avg_ms,
                music_pct(now),
                __atomic_load_n(&g_audio_cpu, __ATOMIC_RELAXED),
                g_main_cpu,
                frame_avg_ms(VPROF_RECORD, g_frame_samples), frame_max_ms(VPROF_RECORD),
                (double)rastq_stats.cmds / g_frame_samples,
                (double)rastq_stats.copied / 1024.0 / g_frame_samples,
                (double)rastq_stats.flushes / g_frame_samples,
                rastq_stats.check_bad_rows,
                rastq_stats.check_runs,
                (double)g_views / g_frame_samples,
                (double)rastq_stats.batches / g_frame_samples,
                (double)rastq_stats.solo[RASTQ_SOLO_MAPPER] / g_frame_samples,
                (double)rastq_stats.solo[RASTQ_SOLO_DIRECT] / g_frame_samples,
                us_to_ms(rastq_stats.wait_us) / g_frame_samples,
                us_to_ms(rastq_stats.busy_us[0]) / g_frame_samples,
                us_to_ms(rastq_stats.busy_us[1]) / g_frame_samples,
                us_to_ms(rastq_stats.busy_us[2]) / g_frame_samples,
                rastq_stats.late_us,
                rastq_stats.rows[1], rastq_stats.rows[2], rastq_stats.rows[3],
                rastq_stats.cpu[0], rastq_stats.cpu[1], rastq_stats.cpu[2],
                rastq_stats.check_leak_rows,
                frame_avg_ms(VPROF_HELPSCAN, g_frame_samples), frame_max_ms(VPROF_HELPSCAN));
        fclose(fp);
    }
}

void vprof_record(vprof_phase_t phase, long long micros) {
    vprof_call_accum_t *a = &g_call_accum[phase];
    a->total_us += micros;
    a->samples++;
    if (micros > a->max_us) {
        a->max_us = micros;
    }
    if (phase == VPROF_RASTER) {
        a->calls++;
    }

    g_frame_phase_total_us[phase] += micros;
}

void vprof_mark_begin(vprof_phase_t phase) {
    g_mark_t0[phase] = sceKernelGetProcessTimeWide();
}

void vprof_mark_end(vprof_phase_t phase) {
    vprof_record(phase, sceKernelGetProcessTimeWide() - g_mark_t0[phase]);
}

void vprof_frame_begin(void) {
    SceInt64 now;
    int i;

    if (!g_startup_done) {
        g_startup_done = 1;
        vprof_startup_checks();
    }

    now = sceKernelGetProcessTimeWide();
    if (!g_window_open) {
        vprof_window_reset(now, _current_loop);
    } else if (_current_loop != g_window_loop_mode) {
        // Loop mode changed mid-window: drop it rather than log a blended
        // line, then start a fresh window for the new mode.
        vprof_window_reset(now, _current_loop);
    }

    for (i = 0; i < VPROF_PHASE_COUNT; i++) {
        g_frame_phase_total_us[i] = 0;
        g_call_accum_at_frame_begin[i] = g_call_accum[i];
    }
    g_rastq_at_frame_begin = rastq_stats;
    g_frame_discard = 0;

    g_frame_t0 = now;
}

void vprof_frame_discard(void) {
    g_frame_discard = 1;
}

void vprof_frame_end(void) {
    SceInt64 now = sceKernelGetProcessTimeWide();
    SceInt64 elapsed = now - g_frame_t0;
    int i;

    g_main_cpu = sceKernelGetCpuId();

    if (g_frame_discard) {
        // The frame's counts go, but not the self-check's results, nor what
        // tells the next views apart.
        rastq_stats_t now_stats = rastq_stats;

        for (i = 0; i < VPROF_PHASE_COUNT; i++) {
            g_call_accum[i] = g_call_accum_at_frame_begin[i];
        }
        rastq_stats = g_rastq_at_frame_begin;
        rastq_stats.views = now_stats.views;
        rastq_stats.check_runs = now_stats.check_runs;
        rastq_stats.check_bad_rows = now_stats.check_bad_rows;
        rastq_stats.check_leak_rows = now_stats.check_leak_rows;
    } else {
        g_views += rastq_stats.views - g_rastq_at_frame_begin.views;
        g_frame_total_us += elapsed;
        g_frame_samples++;
        if (elapsed > g_frame_max_us) {
            g_frame_max_us = elapsed;
        }
        for (i = 0; i < VPROF_PHASE_COUNT; i++) {
            g_frame_accum[i].total_us += g_frame_phase_total_us[i];
            if (g_frame_phase_total_us[i] > g_frame_accum[i].max_us) {
                g_frame_accum[i].max_us = g_frame_phase_total_us[i];
            }
        }
    }

    if (now - g_window_start_us >= 1000000) {
        vprof_window_flush(now);
        // Switch only at a window boundary, so no window mixes two variants.
        if (++g_windows_in_variant >= VPROF_WINDOWS_PER_VARIANT) {
            g_windows_in_variant = 0;
            vprof_variant = (vprof_variant + 1) % VPROF_VARIANT_COUNT;
        }
        vprof_window_reset(now, _current_loop);
    }
}

void vprof_overlay_draw(void) {
    char line[128];
    double frame_avg, frame_max, fps;
    double raster_calls_per_frame, raster_call_avg_ms;
    int samples = g_frame_samples > 0 ? g_frame_samples : 1;
    SceInt64 now = sceKernelGetProcessTimeWide();
    SceInt64 window_age_us = now - g_window_start_us;

    if (g_pgf == NULL) {
        g_pgf = vita2d_load_default_pgf();
    }
    if (g_pgf == NULL) {
        return;
    }

    frame_avg = us_to_ms(g_frame_total_us) / samples;
    frame_max = us_to_ms(g_frame_max_us);
    fps = frame_avg > 0.0 ? 1000.0 / frame_avg : 0.0;

    snprintf(line, sizeof(line), "fps=%.1f frame=%.2f/%.2fms music=%.0f%%", fps, frame_avg, frame_max,
             music_pct(now));
    vita2d_pgf_draw_text(g_pgf, 4, 16, 0xffffffff, 1.0f, line);

    snprintf(line, sizeof(line),
             "input=%.1f sim=%.1f render3d=%.1f ui2d=%.1f present=%.1f",
             frame_avg_ms(VPROF_INPUT, samples),
             frame_avg_ms(VPROF_SIM, samples),
             frame_avg_ms(VPROF_RENDER3D, samples),
             frame_avg_ms(VPROF_UI2D, samples),
             frame_avg_ms(VPROF_PRESENT, samples));
    vita2d_pgf_draw_text(g_pgf, 4, 32, 0xffffffff, 1.0f, line);

    raster_calls_per_frame = (double)g_call_accum[VPROF_RASTER].calls / samples;
    raster_call_avg_ms = us_to_ms(g_call_accum[VPROF_RASTER].total_us) /
                          (g_call_accum[VPROF_RASTER].samples ? g_call_accum[VPROF_RASTER].samples : 1);

    (void)raster_calls_per_frame;
    snprintf(line, sizeof(line), "traverse=%.1f raster=%.1fms record=%.2fms cmds=%.0f batches=%.0f wait=%.1fms",
             frame_avg_ms(VPROF_TRAVERSE, samples),
             frame_avg_ms(VPROF_RASTER, samples),
             frame_avg_ms(VPROF_RECORD, samples),
             (double)rastq_stats.cmds / samples,
             (double)rastq_stats.batches / samples,
             us_to_ms(rastq_stats.wait_us) / samples);
    vita2d_pgf_draw_text(g_pgf, 4, 48, 0xffffffff, 1.0f, line);

    (void)raster_call_avg_ms;
    snprintf(line, sizeof(line), "mode=%d var=%d age=%llds split=%d/%d/%d check=%u/%u leaks=%u", _current_loop,
             vprof_variant, (long long)(window_age_us / 1000000), rastq_stats.rows[1], rastq_stats.rows[2],
             rastq_stats.rows[3], rastq_stats.check_bad_rows, rastq_stats.check_runs, rastq_stats.check_leak_rows);
    vita2d_pgf_draw_text(g_pgf, 4, 64, 0xffffffff, 1.0f, line);
}

#endif // VITA_PROFILE
