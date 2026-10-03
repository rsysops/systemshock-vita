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
    VPROF_PHASE_COUNT
} vprof_phase_t;

#if defined(VITA) && defined(VITA_PROFILE)

#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>

// Variants of the code under test, alternated every few 1 s windows so they
// can be compared within one session. Current step, the rasterizer queue
// (the RASTQ_ modes of rastq.h): 0 = draw directly, 1 = record and replay
// copying every bitmap, 2 = record and replay without copying terrain
// textures.
#define VPROF_VARIANT_COUNT 3
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

#endif // defined(VITA) && defined(VITA_PROFILE)

#endif // __VPROF_H
