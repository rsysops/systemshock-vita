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

// Variants of the code under test. Currently the rasterizer queue (rastq.h):
// 0 = record and replay on three cores, 1 = record and have the GPU draw
// what it can (see docs/PERFORMANCE-GPU.md).
// With VPROF_ALTERNATE set they are alternated every few 1 s windows, so
// that they can be compared within one session. Without it the build stays
// on variant 1, which is what the game does.
#define VPROF_VARIANT_COUNT 2
#define VPROF_ALTERNATE 0
extern int vprof_variant;

// Leaves the current frame out of the window's statistics.
void vprof_frame_discard(void);

// Called from the audio thread with the time spent synthesizing one buffer.
void vprof_audio_add(unsigned micros, int cpu);

void vprof_record(vprof_phase_t phase, long long micros);
void vprof_mark_begin(vprof_phase_t phase);
void vprof_mark_end(vprof_phase_t phase);
void vprof_frame_begin(void);
void vprof_frame_end(void);
void vprof_overlay_draw(void);

#define VPROF_RUN(phase, code)                                               \
    do {                                                                     \
        SceInt64 _vprof_t0 = sceKernelGetProcessTimeWide();                  \
        code;                                                                \
        vprof_record((phase), sceKernelGetProcessTimeWide() - _vprof_t0);    \
    } while (0)
#define VPROF_MARK_BEGIN(phase) vprof_mark_begin(phase)
#define VPROF_MARK_END(phase) vprof_mark_end(phase)
#define VPROF_FRAME_BEGIN() vprof_frame_begin()
#define VPROF_FRAME_END() vprof_frame_end()

#else

#define VPROF_RUN(phase, code) code
#define VPROF_MARK_BEGIN(phase)
#define VPROF_MARK_END(phase)
#define VPROF_FRAME_BEGIN()
#define VPROF_FRAME_END()

#endif // defined(__vita__) && defined(VITA_PROFILE)

#endif // __VPROF_H
