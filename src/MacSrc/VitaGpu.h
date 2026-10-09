#ifndef __VITAGPU_H
#define __VITAGPU_H

// The Vita's GPU as a filler for the rasterizer queue's draw list
// (src/Libraries/3D/Source/rastq.h): see docs/PERFORMANCE-GPU.md.
//
// It draws flat polygons, the texture maps on ordinary 8-bit bitmaps and the
// polygons shaded between colours; the queue has the CPU draw the rest. Its shaders are compiled when the game
// starts, which needs ur0:data/libshacccg.suprx.

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Sets the GPU path up, after vita2d: compiles its shaders, runs its start-up
// checks and offers itself to the rasterizer queue. Without it, or if any of
// that fails, everything stays on the CPU. Details go to gpu.txt in the
// game's data folder.
void vgpu_init(void);

// Whether that worked: the GPU can be given views.
int vgpu_available(void);
// Whether it also draws what is shaded between colours, which cyberspace is
// made of. Without that a cyberspace view is better left to the CPU.
int vgpu_shades(void);

// One line on how the set-up and the start-up checks went, for the profiler.
const char *vgpu_report(void);

// The canvases the GPU draws views into: memory it can render to, `stride`
// bytes a row, all the same size. NULL takes them away again.
#define VGPU_CANVASES 3
void vgpu_set_canvases(void *const *pixels, int width, int height, int stride);

// The canvas for the next view, if a view of this size can be drawn there,
// with the bytes from one of its rows to the next in *row (a canvas is as
// wide as the screen, and a view draws into its top left). NULL if not.
// Each call hands out the canvas used longest ago, so call it once per frame.
unsigned char *vgpu_canvas(int width, int height, int *row);

// In Shock.c: whether a finished view in a GPU canvas, to go at (x, y) on
// the screen, will be shown from there, so that it needn't be copied to the
// screen buffer: alone if it fills the screen, laid over the screen buffer's
// picture otherwise. When it won't, the caller copies the view to the screen
// buffer, which is then what is shown.
int VitaShowView(const unsigned char *bits, int x, int y, int width, int height);

// In Shock.c: to be called before the game draws on the screen without having
// drawn the view first (a pause, a panel, another screen). If the view was
// last shown from a GPU canvas, the screen buffer gets it.
void VitaSyncView(void);

// For the profiler, which resets them.
typedef struct {
    unsigned long long texture_bytes; // bitmaps copied to GPU memory
    unsigned long long upload_us;     // copying them
    unsigned long long draw_us;       // issuing a scene's draws
    unsigned long long swap_wait_us;  // waiting to hand a frame to the screen
    unsigned draws;                   // draws issued
} vgpu_counters_t;
extern vgpu_counters_t vgpu_counters;

#ifdef __cplusplus
}
#endif

#endif // __VITAGPU_H
