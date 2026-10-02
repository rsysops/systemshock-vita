// On-device profiler for the VITA_PROFILE build: see docs/PERFORMANCE.md,
// "How to measure", for the spec this implements.

#ifdef VITA_PROFILE

#include <psp2/kernel/processmgr.h>
#include <stdio.h>
#include <vita2d.h>

#include "Shock.h"
#include "mainloop.h"
#include "vprof.h"

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

static SceInt64 g_frame_phase_total_us[VPROF_PHASE_COUNT];
static vprof_frame_accum_t g_frame_accum[VPROF_PHASE_COUNT];

static SceInt64 g_frame_t0;
static SceInt64 g_frame_total_us;
static SceInt64 g_frame_max_us;
static int g_frame_samples;

static SceInt64 g_window_start_us;
static short g_window_loop_mode = -1;
static int g_window_open = 0;

static vita2d_pgf *g_pgf = NULL;

static const char *PROFILE_FILENAME = "profile.txt";

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

static void vprof_window_flush(SceInt64 now) {
    char path[256];
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

    snprintf(path, sizeof(path), "%s%s", VITA_PATH, PROFILE_FILENAME);
    fp = fopen(path, "a");
    if (fp != NULL) {
        fprintf(fp,
                "t=%lld mode=%d fps=%.1f frame_avg=%.2f frame_max=%.2f "
                "input=%.2f/%.2f sim=%.2f/%.2f render3d=%.2f/%.2f "
                "ui2d=%.2f/%.2f present=%.2f/%.2f | "
                "traverse=%.2f/%.2f sendview=%.2f/%.2f raster=%.2f/%.2f "
                "calls_per_frame=%.1f raster_call_avg=%.3f\n",
                (long long)(now / 1000000),
                g_window_loop_mode,
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
                raster_call_avg_ms);
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
    SceInt64 now = sceKernelGetProcessTimeWide();
    int i;

    if (!g_window_open) {
        vprof_window_reset(now, _current_loop);
    } else if (_current_loop != g_window_loop_mode) {
        // Loop mode changed mid-window: drop it rather than log a blended
        // line, then start a fresh window for the new mode.
        vprof_window_reset(now, _current_loop);
    }

    for (i = 0; i < VPROF_PHASE_COUNT; i++) {
        g_frame_phase_total_us[i] = 0;
    }

    g_frame_t0 = now;
}

void vprof_frame_end(void) {
    SceInt64 now = sceKernelGetProcessTimeWide();
    SceInt64 elapsed = now - g_frame_t0;
    int i;

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

    if (now - g_window_start_us >= 1000000) {
        vprof_window_flush(now);
        vprof_window_reset(now, _current_loop);
    }
}

void vprof_overlay_draw(void) {
    char line[128];
    double frame_avg, frame_max, fps;
    double raster_calls_per_frame, raster_call_avg_ms;
    int samples = g_frame_samples > 0 ? g_frame_samples : 1;
    SceInt64 window_age_us = sceKernelGetProcessTimeWide() - g_window_start_us;

    if (g_pgf == NULL) {
        g_pgf = vita2d_load_default_pgf();
    }
    if (g_pgf == NULL) {
        return;
    }

    frame_avg = us_to_ms(g_frame_total_us) / samples;
    frame_max = us_to_ms(g_frame_max_us);
    fps = frame_avg > 0.0 ? 1000.0 / frame_avg : 0.0;

    snprintf(line, sizeof(line), "fps=%.1f frame=%.2f/%.2fms", fps, frame_avg, frame_max);
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

    snprintf(line, sizeof(line), "traverse=%.1f sendview=%.1f raster=%.1fms calls/f=%.0f",
             frame_avg_ms(VPROF_TRAVERSE, samples),
             frame_avg_ms(VPROF_SENDVIEW, samples),
             frame_avg_ms(VPROF_RASTER, samples),
             raster_calls_per_frame);
    vita2d_pgf_draw_text(g_pgf, 4, 48, 0xffffffff, 1.0f, line);

    snprintf(line, sizeof(line), "mode=%d window_age=%llds call_avg=%.3fms", _current_loop,
             (long long)(window_age_us / 1000000), raster_call_avg_ms);
    vita2d_pgf_draw_text(g_pgf, 4, 64, 0xffffffff, 1.0f, line);
}

#endif // VITA_PROFILE
