#ifndef __VPROF_H
#define __VPROF_H

// On-device profiler for the VITA_PROFILE build. In a normal build every
// macro here expands to exactly the wrapped statement (or nothing), so
// there is no generated-code difference, not just no runtime cost.

typedef enum {
    VPROF_INPUT,
    VPROF_SIM,
    VPROF_RENDER3D,
    VPROF_UI2D,
    VPROF_PRESENT,
    VPROF_TRAVERSE,
    VPROF_SENDVIEW,
    VPROF_RASTER,
    VPROF_RECORD,
    VPROF_HELPSCAN,
    VPROF_HELPREND, // of the help scan: its render (the rest is its look at the pixels)
    VPROF_STARS,    // of sendview
    VPROF_HUD,      // of sendview: the overlays drawn into the view
    VPROF_VIEWOUT,  // of sendview: the cursor and the view's way to the screen
    VPROF_SNDLOAD,  // a sound effect decoded for its first use
    VPROF_RESLOAD,  // a resource read from the card
    VPROF_ALOGLOAD, // an audio log read and converted, before it plays
    VPROF_OUTLINED, // text drawn with an outline: copied from its picture, made into one, or drawn nine times
    // of hud:
    VPROF_HUD_HAND,    // the weapon in hand
    VPROF_HUD_LABEL,   // the help label
    VPROF_HUD_TEXT,    // compass and messages
    VPROF_HUD_BUTTONS, // full screen: the two button panels
    VPROF_HUD_MFD,     // full screen: the two side panels
    VPROF_HUD_INV,     // full screen: the inventory
    VPROF_HUD_VITALS,  // full screen: vitals and meters
    VPROF_HUD_ICONS,   // full screen: the side icons
    VPROF_PHASE_COUNT
} vprof_phase_t;

#if defined(__vita__) && defined(VITA_PROFILE)

#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>

// Variants of the code under test. Currently the sharing of the GPU's lists
// with the worker threads (rastq_gpu_frame in rastq.h): 0 = always, as it
// was, 1 = only while frames are short of time (docs/PERFORMANCE-GPU.md,
// step S2).
// With VPROF_ALTERNATE set they are alternated every VPROF_WINDOWS_PER_VARIANT
// 1 s windows, so that they can be compared within one session. Without it
// the build stays on variant 1, which is what the game does.
#define VPROF_VARIANT_COUNT 2
#define VPROF_ALTERNATE 0
// ten minutes, for when the battery's drain is what is compared: its gauge
// moves a few mAh every 35 s. Frame times need only a few seconds.
#define VPROF_WINDOWS_PER_VARIANT 600
extern int vprof_variant;

// Leaves the current frame out of the window's statistics.
void vprof_frame_discard(void);

// Called from the audio thread with the time spent synthesizing one buffer.
void vprof_audio_add(unsigned micros, int cpu);
// And with the time one of its calls spent on an audio log: reading it and
// converting it as it plays.
void vprof_alog_add(unsigned micros);
// And of a call that had no sound to give: the log wasn't read yet.
void vprof_alog_dry(void);
// Called from any thread with the time a line of the game's own log
// (log_log) took to write, and where it was written from.
void vprof_applog_add(unsigned micros, const void *caller);

// `what`, if not NULL, names this piece of the phase among the frame's notes
// (see vprof_note), with two numbers of its own.
void vprof_record(vprof_phase_t phase, long long micros, const char *what, int a, int b);
// One of a frame's pieces, timed: the longest few of each frame are kept, and
// a stutter's line gives its frame's (notes=). `who`, if not NULL, is an
// address that says more than the name: the function called, or the caller.
void vprof_note(const char *what, const void *who, int a, int b, long long micros);
void vprof_mark_begin(vprof_phase_t phase);
void vprof_mark_end(vprof_phase_t phase);
void vprof_frame_begin(void);
void vprof_frame_end(void);
void vprof_overlay_draw(void);

// A phase's timer: the statement's own text is its name among the notes.
#define VPROF_RUN(phase, code) VPROF_RUN_AS(phase, #code, 0, 0, code)
// The same under a chosen name, with two numbers that say which one it was.
#define VPROF_RUN_AS(phase, what, a, b, code)                                          \
    do {                                                                               \
        SceInt64 _vprof_t0 = sceKernelGetProcessTimeWide();                            \
        code;                                                                          \
        vprof_record((phase), sceKernelGetProcessTimeWide() - _vprof_t0, (what), (a), (b)); \
    } while (0)
// A piece that belongs to no phase: only a note.
#define VPROF_NOTE(what, who, a, b, code)                                                       \
    do {                                                                                        \
        SceInt64 _vprof_n0 = sceKernelGetProcessTimeWide();                                     \
        code;                                                                                   \
        vprof_note((what), (const void *)(who), (a), (b), sceKernelGetProcessTimeWide() - _vprof_n0); \
    } while (0)
#define VPROF_MARK_BEGIN(phase) vprof_mark_begin(phase)
#define VPROF_MARK_END(phase) vprof_mark_end(phase)
#define VPROF_FRAME_BEGIN() vprof_frame_begin()
#define VPROF_FRAME_END() vprof_frame_end()

#else

#define VPROF_RUN(phase, code) code
#define VPROF_RUN_AS(phase, what, a, b, code) code
#define VPROF_NOTE(what, who, a, b, code) code
#define VPROF_MARK_BEGIN(phase)
#define VPROF_MARK_END(phase)
#define VPROF_FRAME_BEGIN()
#define VPROF_FRAME_END()

#endif // defined(__vita__) && defined(VITA_PROFILE)

#endif // __VPROF_H
