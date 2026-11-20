#ifndef __VITAGPU_H
#define __VITAGPU_H

// The Vita's GPU as a filler for the rasterizer queue's draw list
// (src/Libraries/3D/Source/rastq.h): see docs/PERFORMANCE.md.
//
// It draws flat polygons, the texture maps on ordinary 8-bit bitmaps, the
// polygons shaded between colours, translucent polygons and bitmaps, lines and
// points; the queue has the CPU draw the rest. Its shaders are compiled when
// the game starts, which needs ur0:data/libshacccg.suprx.

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Sets the GPU path up, after vita2d: compiles its shaders, runs its start-up
// checks and offers itself to the rasterizer queue. Without the shader
// compiler, or without textured shaders that draw, everything stays on the
// CPU. A kind of call whose shaders don't compile stays on the CPU, and so do
// the translucent ones on any wrong pixel in their checks; the other checks'
// wrong pixels are only logged. Details go to gpu.txt in the game's data folder.
void vgpu_init(void);

// Whether that worked: the GPU can be given views.
int vgpu_available(void);
// Whether it also draws what is shaded between colours, which cyberspace is
// made of. Without that a cyberspace view is better left to the CPU.
int vgpu_shades(void);
// Whether the set-up stopped at the shader compiler: libshacccg.suprx isn't
// in ur0:data/, or wouldn't load.
int vgpu_compiler_missing(void);

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

// Smooth shadows (docs/SHADOWS.md): whether they are wanted, before
// vgpu_init. With them the GPU draws each scene of a view into a 32-bit side
// buffer of the view's canvas: per pixel, the palette index drawn, the
// texel, the light level in sixteenths and a mark for a lit texture, from
// which the screen gets the light in full colour. The indices then go to the
// canvas, which holds what it always held.
void vgpu_set_shadows(int on);
// Each time the palette changes: the palette before its gamma (256 times red,
// green, blue), the gamma row the screen's colours went through, and whether
// index 255 is shown black. The smooth shadows' colours follow.
void vgpu_set_palette(const unsigned char raw[768], const unsigned char gamma[256], int black255);
// In SDLDraw, inside vita2d's drawing: the view in a GPU canvas, its pixels
// at `bits` (a texture `tex_w` wide whose SceGxmTexture is `texture`, P8 with
// the palette), its top left `w` by `h` drawn at (x, y) on the screen,
// stretched by (sx, sy), with smooth shadows. 0 if this view has none: the
// caller draws it as before.
int vgpu_draw_view(const void *texture, const void *bits, int tex_w, float x, float y, float sx, float sy, int w,
                   int h);
// Called after vgpu_canvas handed out a canvas: 0 if this view has no use for
// smooth shadows (cyberspace).
void vgpu_canvas_smooth(int on);

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
    unsigned side_scenes;             // smooth scenes' indices extracted into their canvas
    unsigned long long side_draw_us;  // issuing those extracts
    // With the profile build's checks on, over the views compared with the CPU's: their
    // pixels with a side buffer, those marked lit there, and of those, the
    // ones whose index there isn't the canvas's. The profiler keeps these.
    unsigned long long side_pixels, side_lit, side_mismatch;
    unsigned smooth_frames;        // views shown with smooth shadows
    unsigned lut_builds;           // the colours' table made again
    unsigned long long lut_us;     // making it
    unsigned long long lut_texels; // the texels made again (256 for the whole table)
    // And the longest comparison of the window, on the game's thread: the
    // side buffer's check, and a frame dump's copy when there is one.
    unsigned compare_max_us;
} vgpu_counters_t;
extern vgpu_counters_t vgpu_counters;
// How long the last frame dump took its writer to put on the card, in the
// background (profile build, checks on).
extern unsigned vgpu_dump_us;

#ifdef __cplusplus
}
#endif

#endif // __VITAGPU_H
