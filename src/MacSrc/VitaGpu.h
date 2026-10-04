#ifndef __VITAGPU_H
#define __VITAGPU_H

// The Vita's GPU as a filler for the rasterizer queue's draw list
// (src/Libraries/3D/Source/rastq.h): see docs/PERFORMANCE-GPU.md.
//
// So far, in profile builds only: flat polygons and the texture maps on
// ordinary 8-bit bitmaps; the queue has the CPU draw the rest.

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(VITA) && defined(VITA_PROFILE)

// Sets the GPU path up, after vita2d: compiles its shaders, runs its start-up
// checks and offers itself to the rasterizer queue. Without it, or if any of
// that fails, everything stays on the CPU. Details go to gpu.txt next to
// profile.txt.
void vgpu_init(void);

// One line on how the set-up and the start-up checks went, for the profiler.
const char *vgpu_report(void);

// The canvases the GPU draws views into: memory it can render to, `stride`
// bytes a row, all the same size. NULL takes them away again.
#define VGPU_CANVASES 3
void vgpu_set_canvases(void *const *pixels, int width, int height, int stride);

// The canvas for the next view, if a view of this size and row length can
// be drawn there; NULL if not. Each call hands out the one used longest ago,
// so call it once per frame.
unsigned char *vgpu_canvas(int width, int height, int row);

// In Shock.c: whether a finished full-screen view in a GPU canvas will be
// shown from there, so that it needn't be copied to the screen buffer.
int VitaShowView(const unsigned char *bits, int width, int height);

// For the profiler, which resets them: bitmap bytes copied to GPU memory,
// and time spent waiting to hand a frame to the screen.
extern unsigned long long vgpu_texture_bytes, vgpu_swap_wait_us;

#else

#define vgpu_init() ((void)0)
#define vgpu_canvas(width, height, row) NULL
#define VitaShowView(bits, width, height) 0

#endif

#ifdef __cplusplus
}
#endif

#endif // __VITAGPU_H
