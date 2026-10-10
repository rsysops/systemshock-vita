// On-device profiler for the VITA_PROFILE build: see docs/PERFORMANCE-CPU.md,
// "How to measure", for the spec this implements.

#ifdef VITA_PROFILE

#include <psp2/kernel/processmgr.h>
#include <stdio.h>
#include <string.h>
#include <vita2d.h>

#include "Shock.h"
#include "fix.h"
#include "mainloop.h"
#include "rastq.h"
#include "rastqthr.h"
#include "vprof.h"
#include "VitaGpu.h"
#include "hudkeep.h"

#define VPROF_WINDOWS_PER_VARIANT 5
#define VPROF_FIXDIV_CHECK_CASES 1000000
// A stutter: a frame of at least this long that is also half again as long
// as the average frame of the window before. The worst few are logged, each
// on a line of its own.
#define VPROF_SPIKE_MIN_US 25000
#define VPROF_SPIKES_KEPT 4
#define VPROF_WORKERS (RASTQ_THREADS - 1)

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
static vgpu_counters_t g_vgpu_at_frame_begin;
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

#if VPROF_ALTERNATE
int vprof_variant = 0;
static int g_windows_in_variant = 0;
#else
int vprof_variant = 1;
#endif

// What the workers had done when the window began, and their share of the
// last whole window: in jobs, awake without one, asleep.
typedef struct {
    double job, spin, sleep; // percent of the window
    unsigned jobs, sleeps;
} vprof_worker_share_t;
static rastq_worker_times g_workers_at_window[VPROF_WORKERS];
// The window that just ended left its last reading there: the next one
// starts from it, so that no time falls between the two (writing the log
// takes tens of milliseconds).
static int g_workers_read;
static vprof_worker_share_t g_worker_share[VPROF_WORKERS];
// The GPU's lists per frame in that window: cut by all the threads, by the
// caller alone, and the calls in the latter
static double g_lists_shared, g_lists_solo, g_lists_solo_calls;

// Frames of the window longer than one, two, three and six screen refreshes
static const SceInt64 SLOW_US[] = {20000, 34000, 50000, 100000};
#define VPROF_SLOW_STEPS ((int)(sizeof(SLOW_US) / sizeof(SLOW_US[0])))
static int g_slow[VPROF_SLOW_STEPS];
// Frames left out of the window's timings, and the longest
static int g_skipped;
static SceInt64 g_skipped_max_us;

typedef struct {
    SceInt64 at_us; // when the frame ended
    SceInt64 frame_us;
    short mode;
    SceInt64 phase_us[VPROF_PHASE_COUNT];
    unsigned long long gpu_wait_us, gpu_submit_us, upload_us, texture_bytes, swap_wait_us;
    unsigned cmds, views;
} vprof_spike_t;
// The worst stutters since the log was last written, in the order they came
static vprof_spike_t g_spikes[VPROF_SPIKES_KEPT];
static int g_spikes_kept;
static unsigned g_spikes_total;
static vprof_spike_t g_last_spike;
static SceInt64 g_prev_frame_avg_us;

// How long the last write to the log took: between two frames, so in no
// frame's time, but the screen waits for it all the same.
static SceInt64 g_logwrite_us;

// Music synthesis time, added by the audio thread and taken by the main
// thread once per window, so only these two touch it, atomically.
static unsigned g_audio_us = 0;
// The same for an audio log's callback, and its longest call
static unsigned g_alog_us = 0;
static unsigned g_alog_max_us = 0;
static unsigned g_alog_dry = 0; // its calls that had nothing to give
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
        fprintf(fp, "%s\n", vgpu_report());
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
    __atomic_store_n(&g_alog_us, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&g_alog_max_us, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&g_alog_dry, 0, __ATOMIC_RELAXED);
    rastq_stats.cmds = 0;
    rastq_stats.flushes = 0;
    rastq_stats.copied = 0;
    rastq_stats.batches = 0;
    rastq_stats.wait_us = 0;
    rastq_stats.late_us = 0;
    rastq_stats.gpu_scenes = 0;
    rastq_stats.gpu_polys = 0;
    rastq_stats.gpu_culled = 0;
    rastq_stats.gpu_cpu_calls = 0;
    rastq_stats.gpu_pieces = 0;
    for (i = 0; i < RASTQ_GPU_KINDS; i++)
        rastq_stats.gpu_kinds[i] = 0;
    for (i = 0; i < RASTQ_GPU_WHYS; i++)
        rastq_stats.gpu_whys[i] = 0;
    rastq_stats.gpu_submit_us = 0;
    rastq_stats.gpu_wait_us = 0;
    rastq_stats.gpu_overlap_us = 0;
    rastq_stats.gpu_cpu_us = 0;
    memset(&vgpu_counters, 0, sizeof(vgpu_counters));
    // the checks' counts run on, like the other self-checks'
    hudkeep_stats.text_hits = hudkeep_stats.text_misses = 0;
    hudkeep_stats.scaled_hits = hudkeep_stats.scaled_misses = 0;
    rastq_stats.gpu_prepare_us = 0;
    for (i = 0; i < RASTQ_THREADS; i++)
        rastq_stats.gpu_cut[i] = 0;
    for (i = 0; i < RASTQ_SOLO_REASONS; i++)
        rastq_stats.solo[i] = 0;
    for (i = 0; i < RASTQ_THREADS; i++)
        rastq_stats.busy_us[i] = 0;
    rastq_stats.gpu_lists_shared = 0;
    rastq_stats.gpu_lists_solo = 0;
    rastq_stats.gpu_solo_calls = 0;
    if (!g_workers_read) {
        for (i = 0; i < VPROF_WORKERS; i++)
            rastq_threads_times(i + 1, &g_workers_at_window[i]);
    }
    g_workers_read = 0;
    for (i = 0; i < VPROF_SLOW_STEPS; i++)
        g_slow[i] = 0;
    g_skipped = 0;
    g_skipped_max_us = 0;
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

void vprof_alog_add(unsigned micros) {
    __atomic_fetch_add(&g_alog_us, micros, __ATOMIC_RELAXED);
    // only the audio thread raises it
    if (micros > __atomic_load_n(&g_alog_max_us, __ATOMIC_RELAXED))
        __atomic_store_n(&g_alog_max_us, micros, __ATOMIC_RELAXED);
}

void vprof_alog_dry(void) { __atomic_fetch_add(&g_alog_dry, 1, __ATOMIC_RELAXED); }

void vprof_audio_add(unsigned micros, int cpu) {
    __atomic_fetch_add(&g_audio_us, micros, __ATOMIC_RELAXED);
    __atomic_store_n(&g_audio_cpu, cpu, __ATOMIC_RELAXED);
}

// The workers' share of the window that ends now.
static void vprof_workers_share(SceInt64 now) {
    double elapsed = (double)(now - g_window_start_us);
    int i;

    for (i = 0; i < VPROF_WORKERS; i++) {
        vprof_worker_share_t *share = &g_worker_share[i];
        const rastq_worker_times *from = &g_workers_at_window[i]; // until the end of this turn
        rastq_worker_times t;
        int slept;

        rastq_threads_times(i + 1, &t);
        // The counts wrap round, hence unsigned differences. A sleep under
        // way is read with a later clock than the window's, so that it can
        // come out a hair too long, and the next one a hair negative.
        slept = (int)(t.sleep_us - from->sleep_us);
        if (slept < 0)
            slept = 0;
        share->job = elapsed > 0 ? 100.0 * (unsigned)(t.job_us - from->job_us) / elapsed : 0.0;
        share->sleep = elapsed > 0 ? 100.0 * slept / elapsed : 0.0;
        if (share->sleep > 100.0)
            share->sleep = 100.0;
        share->spin = 100.0 - share->job - share->sleep;
        if (share->spin < 0.0)
            share->spin = 0.0;
        share->jobs = t.jobs - from->jobs;
        share->sleeps = t.sleeps - from->sleeps;
        g_workers_at_window[i] = t;
    }
    g_workers_read = 1;
}

// Where most of a stutter went: a load when one took over half of it,
// otherwise the largest of the frame's five parts or of what they leave.
static const char *vprof_spike_cause(const vprof_spike_t *s, SceInt64 *us) {
    static const vprof_phase_t parts[] = {VPROF_INPUT, VPROF_SIM, VPROF_RENDER3D, VPROF_UI2D, VPROF_PRESENT};
    static const char *const names[] = {"input", "sim", "render3d", "ui2d", "present"};
    const char *name = "other";
    SceInt64 other = s->frame_us;
    unsigned i;

    if (2 * s->phase_us[VPROF_SNDLOAD] > s->frame_us) {
        *us = s->phase_us[VPROF_SNDLOAD];
        return "sndload";
    }
    if (2 * s->phase_us[VPROF_ALOGLOAD] > s->frame_us) {
        *us = s->phase_us[VPROF_ALOGLOAD];
        return "alogload";
    }
    if (2 * s->phase_us[VPROF_RESLOAD] > s->frame_us) {
        *us = s->phase_us[VPROF_RESLOAD];
        return "resload";
    }
    for (i = 0; i < sizeof(parts) / sizeof(parts[0]); i++)
        other -= s->phase_us[parts[i]];
    *us = other;
    for (i = 0; i < sizeof(parts) / sizeof(parts[0]); i++) {
        if (s->phase_us[parts[i]] > *us) {
            *us = s->phase_us[parts[i]];
            name = names[i];
        }
    }
    return name;
}

// The frame that just ended was a stutter: keeps it for the log if it is
// among the worst since the last write.
static void vprof_spike_add(SceInt64 now, SceInt64 elapsed) {
    vprof_spike_t *s;
    int i, least = 0;

    if (g_spikes_kept == VPROF_SPIKES_KEPT) {
        for (i = 1; i < VPROF_SPIKES_KEPT; i++)
            if (g_spikes[i].frame_us < g_spikes[least].frame_us)
                least = i;
        if (g_spikes[least].frame_us < elapsed) {
            memmove(&g_spikes[least], &g_spikes[least + 1],
                    (VPROF_SPIKES_KEPT - 1 - least) * sizeof(g_spikes[0]));
            g_spikes_kept--;
        }
    }
    s = g_spikes_kept < VPROF_SPIKES_KEPT ? &g_spikes[g_spikes_kept++] : &g_last_spike;
    s->at_us = now;
    s->frame_us = elapsed;
    s->mode = g_window_loop_mode;
    for (i = 0; i < VPROF_PHASE_COUNT; i++)
        s->phase_us[i] = g_frame_phase_total_us[i];
    s->gpu_wait_us = rastq_stats.gpu_wait_us - g_rastq_at_frame_begin.gpu_wait_us;
    s->gpu_submit_us = rastq_stats.gpu_submit_us - g_rastq_at_frame_begin.gpu_submit_us;
    s->upload_us = vgpu_counters.upload_us - g_vgpu_at_frame_begin.upload_us;
    s->texture_bytes = vgpu_counters.texture_bytes - g_vgpu_at_frame_begin.texture_bytes;
    s->swap_wait_us = vgpu_counters.swap_wait_us - g_vgpu_at_frame_begin.swap_wait_us;
    s->cmds = rastq_stats.cmds - g_rastq_at_frame_begin.cmds;
    s->views = rastq_stats.views - g_rastq_at_frame_begin.views;
    g_last_spike = *s;
    g_spikes_total++;
}

static void vprof_spikes_write(FILE *fp) {
    int i;

    for (i = 0; i < g_spikes_kept; i++) {
        const vprof_spike_t *s = &g_spikes[i];
        SceInt64 other = s->frame_us - s->phase_us[VPROF_INPUT] - s->phase_us[VPROF_SIM] -
                         s->phase_us[VPROF_RENDER3D] - s->phase_us[VPROF_UI2D] - s->phase_us[VPROF_PRESENT];

        fprintf(fp,
                "spike t=%.2f mode=%d frame=%.2f input=%.2f sim=%.2f render3d=%.2f ui2d=%.2f present=%.2f "
                "other=%.2f | traverse=%.2f sendview=%.2f raster=%.2f record=%.2f helpscan=%.2f hud=%.2f "
                "viewout=%.2f sndload=%.2f resload=%.2f alogload=%.2f | gpuwait=%.2f gpusubmit=%.2f "
                "gpuupload=%.2f gputex=%.1fKB swapwait=%.2f cmds=%u views=%u\n",
                (double)s->at_us / 1000000.0, s->mode, us_to_ms(s->frame_us),
                us_to_ms(s->phase_us[VPROF_INPUT]), us_to_ms(s->phase_us[VPROF_SIM]),
                us_to_ms(s->phase_us[VPROF_RENDER3D]), us_to_ms(s->phase_us[VPROF_UI2D]),
                us_to_ms(s->phase_us[VPROF_PRESENT]), us_to_ms(other),
                us_to_ms(s->phase_us[VPROF_TRAVERSE]), us_to_ms(s->phase_us[VPROF_SENDVIEW]),
                us_to_ms(s->phase_us[VPROF_RASTER]), us_to_ms(s->phase_us[VPROF_RECORD]),
                us_to_ms(s->phase_us[VPROF_HELPSCAN]), us_to_ms(s->phase_us[VPROF_HUD]),
                us_to_ms(s->phase_us[VPROF_VIEWOUT]), us_to_ms(s->phase_us[VPROF_SNDLOAD]),
                us_to_ms(s->phase_us[VPROF_RESLOAD]), us_to_ms(s->phase_us[VPROF_ALOGLOAD]),
                us_to_ms((SceInt64)s->gpu_wait_us), us_to_ms((SceInt64)s->gpu_submit_us),
                us_to_ms((SceInt64)s->upload_us), (double)s->texture_bytes / 1024.0,
                us_to_ms((SceInt64)s->swap_wait_us), s->cmds, s->views);
    }
    g_spikes_kept = 0;
}

static void vprof_window_flush(SceInt64 now) {
    FILE *fp;
    double fps;
    double frame_avg, frame_max;
    double raster_calls_per_frame, raster_call_avg_ms;
    SceInt64 write_t0;
    int snd_ready, snd_total, snd_decode_ms, snd_bytes;

    if (!g_window_open || g_frame_samples == 0) {
        return;
    }

    frame_avg = us_to_ms(g_frame_total_us) / g_frame_samples;
    frame_max = us_to_ms(g_frame_max_us);
    fps = frame_avg > 0.0 ? 1000.0 / frame_avg : 0.0;

    raster_calls_per_frame = (double)g_call_accum[VPROF_RASTER].calls / g_frame_samples;
    raster_call_avg_ms = us_to_ms(g_call_accum[VPROF_RASTER].total_us) /
                          (g_call_accum[VPROF_RASTER].samples ? g_call_accum[VPROF_RASTER].samples : 1);

    vprof_workers_share(now);
    g_lists_shared = (double)rastq_stats.gpu_lists_shared / g_frame_samples;
    g_lists_solo = (double)rastq_stats.gpu_lists_solo / g_frame_samples;
    g_lists_solo_calls = (double)rastq_stats.gpu_solo_calls / g_frame_samples;
    g_prev_frame_avg_us = g_frame_total_us / g_frame_samples;
    snd_preload_stats(&snd_ready, &snd_total, &snd_decode_ms, &snd_bytes);

    write_t0 = sceKernelGetProcessTimeWide();
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
                "split=%d/%d/%d wcpu=%d/%d/%d leaks=%u | helpscan=%.2f/%.2f | "
                "gpuscenes=%.2f gpupolys=%.1f gpusubmit=%.2f gpuwait=%.2f gpufallbacks=%u gpudiff=%llu/%llu "
                "gpuculled=%.1f gpucpu=%.1f/%.2f gputex=%.1fKB swapwait=%.2f | "
                "gpukinds=flat:%.1f,plain:%.1f,clut:%.1f,lit:%.1f,shaded:%.1f,line:%.1f,point:%.1f gpupieces=%.1f "
                "gpuwhy=tlucbm:%.1f,spoly:%.1f,tlucpoly:%.1f,poly:%.1f,fill:%.1f,verts:%.1f,light:%.1f,clip:%.1f,"
                "size:%.1f,other:%.1f | "
                "gpuprepare=%.2f gpuupload=%.2f gpudraw=%.2f gpudraws=%.1f | "
                "helprend=%.2f/%.2f stars=%.2f/%.2f hud=%.2f/%.2f viewout=%.2f/%.2f | "
                "sndload=%.2f/%.2f resload=%.2f/%.2f | "
                "gpuoverlap=%.2f hudparts=hand:%.2f,label:%.2f,text:%.2f,buttons:%.2f,mfd:%.2f,inv:%.2f,"
                "vitals:%.2f,icons:%.2f | "
                "hudkept=text:%.1f/%.2f,scaled:%.1f/%.2f hudcheck=%u/%u | gpucut=%.1f/%.1f/%.1f | "
                "wjob=%.1f/%.1f%% wspin=%.1f/%.1f%% wsleep=%.1f/%.1f%% wjobs=%.2f/%.2f wsleeps=%u/%u | "
                "cutlists=shared:%.2f,solo:%.2f,solocalls:%.1f | "
                "slow=20:%d,34:%d,50:%d,100:%d skipped=%d/%.2f logwrite=%.2f | "
                "sndready=%d/%d snddecode=%d sndmem=%.1fMB alogload=%.2f/%.2f alogcpu=%.1f%% alogcbmax=%.2f "
                "alogdry=%u\n",
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
                frame_avg_ms(VPROF_HELPSCAN, g_frame_samples), frame_max_ms(VPROF_HELPSCAN),
                (double)rastq_stats.gpu_scenes / g_frame_samples,
                (double)rastq_stats.gpu_polys / g_frame_samples,
                us_to_ms(rastq_stats.gpu_submit_us) / g_frame_samples,
                us_to_ms(rastq_stats.gpu_wait_us) / g_frame_samples,
                rastq_stats.gpu_fallbacks,
                rastq_stats.gpu_check_diff,
                rastq_stats.gpu_check_pixels,
                (double)rastq_stats.gpu_culled / g_frame_samples,
                (double)rastq_stats.gpu_cpu_calls / g_frame_samples,
                us_to_ms(rastq_stats.gpu_cpu_us) / g_frame_samples,
                (double)vgpu_counters.texture_bytes / 1024.0 / g_frame_samples,
                us_to_ms(vgpu_counters.swap_wait_us) / g_frame_samples,
                (double)rastq_stats.gpu_kinds[RASTQ_GPU_KIND_FLAT] / g_frame_samples,
                (double)rastq_stats.gpu_kinds[RASTQ_GPU_KIND_PLAIN] / g_frame_samples,
                (double)rastq_stats.gpu_kinds[RASTQ_GPU_KIND_CLUT] / g_frame_samples,
                (double)rastq_stats.gpu_kinds[RASTQ_GPU_KIND_LIT] / g_frame_samples,
                (double)rastq_stats.gpu_kinds[RASTQ_GPU_KIND_SHADED] / g_frame_samples,
                (double)rastq_stats.gpu_kinds[RASTQ_GPU_KIND_LINE] / g_frame_samples,
                (double)rastq_stats.gpu_kinds[RASTQ_GPU_KIND_POINT] / g_frame_samples,
                (double)rastq_stats.gpu_pieces / g_frame_samples,
                (double)rastq_stats.gpu_whys[RASTQ_GPU_WHY_TLUC_BITMAP] / g_frame_samples,
                (double)rastq_stats.gpu_whys[RASTQ_GPU_WHY_SHADED_POLY] / g_frame_samples,
                (double)rastq_stats.gpu_whys[RASTQ_GPU_WHY_TLUC_POLY] / g_frame_samples,
                (double)rastq_stats.gpu_whys[RASTQ_GPU_WHY_OTHER_POLY] / g_frame_samples,
                (double)rastq_stats.gpu_whys[RASTQ_GPU_WHY_FILL] / g_frame_samples,
                (double)rastq_stats.gpu_whys[RASTQ_GPU_WHY_VERTS] / g_frame_samples,
                (double)rastq_stats.gpu_whys[RASTQ_GPU_WHY_LIGHT] / g_frame_samples,
                (double)rastq_stats.gpu_whys[RASTQ_GPU_WHY_CLIP] / g_frame_samples,
                (double)rastq_stats.gpu_whys[RASTQ_GPU_WHY_SIZE] / g_frame_samples,
                (double)rastq_stats.gpu_whys[RASTQ_GPU_WHY_OTHER] / g_frame_samples,
                us_to_ms(rastq_stats.gpu_prepare_us) / g_frame_samples,
                us_to_ms(vgpu_counters.upload_us) / g_frame_samples,
                us_to_ms(vgpu_counters.draw_us) / g_frame_samples,
                (double)vgpu_counters.draws / g_frame_samples,
                frame_avg_ms(VPROF_HELPREND, g_frame_samples), frame_max_ms(VPROF_HELPREND),
                frame_avg_ms(VPROF_STARS, g_frame_samples), frame_max_ms(VPROF_STARS),
                frame_avg_ms(VPROF_HUD, g_frame_samples), frame_max_ms(VPROF_HUD),
                frame_avg_ms(VPROF_VIEWOUT, g_frame_samples), frame_max_ms(VPROF_VIEWOUT),
                frame_avg_ms(VPROF_SNDLOAD, g_frame_samples), frame_max_ms(VPROF_SNDLOAD),
                frame_avg_ms(VPROF_RESLOAD, g_frame_samples), frame_max_ms(VPROF_RESLOAD),
                us_to_ms(rastq_stats.gpu_overlap_us) / g_frame_samples,
                frame_avg_ms(VPROF_HUD_HAND, g_frame_samples),
                frame_avg_ms(VPROF_HUD_LABEL, g_frame_samples),
                frame_avg_ms(VPROF_HUD_TEXT, g_frame_samples),
                frame_avg_ms(VPROF_HUD_BUTTONS, g_frame_samples),
                frame_avg_ms(VPROF_HUD_MFD, g_frame_samples),
                frame_avg_ms(VPROF_HUD_INV, g_frame_samples),
                frame_avg_ms(VPROF_HUD_VITALS, g_frame_samples),
                frame_avg_ms(VPROF_HUD_ICONS, g_frame_samples),
                (double)hudkeep_stats.text_hits / g_frame_samples,
                (double)hudkeep_stats.text_misses / g_frame_samples,
                (double)hudkeep_stats.scaled_hits / g_frame_samples,
                (double)hudkeep_stats.scaled_misses / g_frame_samples,
                hudkeep_stats.check_bad, hudkeep_stats.checks,
                (double)rastq_stats.gpu_cut[0] / g_frame_samples,
                (double)rastq_stats.gpu_cut[1] / g_frame_samples,
                (double)rastq_stats.gpu_cut[2] / g_frame_samples,
                g_worker_share[0].job, g_worker_share[1].job,
                g_worker_share[0].spin, g_worker_share[1].spin,
                g_worker_share[0].sleep, g_worker_share[1].sleep,
                (double)g_worker_share[0].jobs / g_frame_samples,
                (double)g_worker_share[1].jobs / g_frame_samples,
                g_worker_share[0].sleeps, g_worker_share[1].sleeps,
                g_lists_shared, g_lists_solo, g_lists_solo_calls,
                g_slow[0], g_slow[1], g_slow[2], g_slow[3],
                g_skipped, us_to_ms(g_skipped_max_us),
                us_to_ms(g_logwrite_us),
                snd_ready, snd_total, snd_decode_ms, (double)snd_bytes / (1024.0 * 1024.0),
                frame_avg_ms(VPROF_ALOGLOAD, g_frame_samples), frame_max_ms(VPROF_ALOGLOAD),
                now > g_window_start_us
                    ? 100.0 * __atomic_load_n(&g_alog_us, __ATOMIC_RELAXED) / (double)(now - g_window_start_us)
                    : 0.0,
                us_to_ms(__atomic_load_n(&g_alog_max_us, __ATOMIC_RELAXED)),
                __atomic_load_n(&g_alog_dry, __ATOMIC_RELAXED));
        vprof_spikes_write(fp);
        fclose(fp);
    }
    g_logwrite_us = sceKernelGetProcessTimeWide() - write_t0;
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
    g_vgpu_at_frame_begin = vgpu_counters;
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
        rastq_stats.gpu_fallbacks = now_stats.gpu_fallbacks;
        rastq_stats.gpu_check_pixels = now_stats.gpu_check_pixels;
        rastq_stats.gpu_check_diff = now_stats.gpu_check_diff;
        vgpu_counters = g_vgpu_at_frame_begin;
        g_skipped++;
        if (elapsed > g_skipped_max_us) {
            g_skipped_max_us = elapsed;
        }
    } else {
        for (i = 0; i < VPROF_SLOW_STEPS; i++) {
            if (elapsed > SLOW_US[i]) {
                g_slow[i]++;
            }
        }
        if (elapsed >= VPROF_SPIKE_MIN_US && 2 * elapsed >= 3 * g_prev_frame_avg_us) {
            vprof_spike_add(now, elapsed);
        }
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
#if VPROF_ALTERNATE
        // Switch only at a window boundary, so no window mixes two variants.
        if (++g_windows_in_variant >= VPROF_WINDOWS_PER_VARIANT) {
            g_windows_in_variant = 0;
            vprof_variant = (vprof_variant + 1) % VPROF_VARIANT_COUNT;
        }
#endif
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
    snprintf(line, sizeof(line),
             "mode=%d var=%d age=%llds check=%u/%u leaks=%u gpuwait=%.1fms gpucpu=%.0f gpudiff=%.1f/1000",
             _current_loop, vprof_variant, (long long)(window_age_us / 1000000), rastq_stats.check_bad_rows,
             rastq_stats.check_runs, rastq_stats.check_leak_rows, us_to_ms(rastq_stats.gpu_wait_us) / samples,
             (double)rastq_stats.gpu_cpu_calls / samples,
             rastq_stats.gpu_check_pixels
                 ? 1000.0 * (double)rastq_stats.gpu_check_diff / (double)rastq_stats.gpu_check_pixels
                 : 0.0);
    vita2d_pgf_draw_text(g_pgf, 4, 64, 0xffffffff, 1.0f, line);

    // of the last whole window
    snprintf(line, sizeof(line),
             "workers job=%.0f/%.0f%% spin=%.0f/%.0f%% sleep=%.0f/%.0f%% lists=%.1f shared, %.1f solo (%.0f calls)",
             g_worker_share[0].job, g_worker_share[1].job, g_worker_share[0].spin, g_worker_share[1].spin,
             g_worker_share[0].sleep, g_worker_share[1].sleep, g_lists_shared, g_lists_solo, g_lists_solo_calls);
    vita2d_pgf_draw_text(g_pgf, 4, 80, 0xffffffff, 1.0f, line);

    if (g_spikes_total == 0) {
        snprintf(line, sizeof(line), "spikes=0");
    } else {
        SceInt64 cause_us;
        const char *cause = vprof_spike_cause(&g_last_spike, &cause_us);

        snprintf(line, sizeof(line), "spikes=%u last=%.1fms t=%lld (%s %.1f)", g_spikes_total,
                 us_to_ms(g_last_spike.frame_us), (long long)(g_last_spike.at_us / 1000000), cause,
                 us_to_ms(cause_us));
    }
    vita2d_pgf_draw_text(g_pgf, 4, 96, 0xffffffff, 1.0f, line);
}

#endif // VITA_PROFILE
