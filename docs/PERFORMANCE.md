# Vita performance investigation notes

Findings from an investigation into codebase health and native-resolution
performance on PS Vita. No code was changed as part of this investigation —
this file records the evidence and recommendations for future work.

## Summary

The codebase is coupled 1994-vintage C, not a neglected/poorly-written
project — that's expected for a Night Dive Mac source release ported
forward. The Vita layer itself is a thin overlay on a much larger untouched
upstream ([Shockolate](https://github.com/Interrupt/systemshock)) codebase.
There is no automated test suite. Performance at native resolution is
CPU-bound and **single-threaded** — three of the Vita's four CPU cores sit
mostly idle every frame — so this is not simply a "needs overclock" problem.

## Code organization

- ~1,742 `extern` globals across `src/GameSrc/Headers` and
  `src/Libraries/*/Source` — state is passed via globals rather than
  parameters/structs throughout (e.g. `objs[]` indexed directly all over
  `src/GameSrc/objsim.c`).
- `src/GameSrc/objsim.c` is 2,815 lines / ~44 functions with 20 switch
  statements (some nested) dispatching on object class/triple-ID. `goto` is
  used throughout `GameSrc` (`newai.c`, `invent.c`, `email.c`, `olh.c`,
  `mfdgames.c`, etc.).
- Only 37 of 364 `.c/.cc/.cpp` files touch `VITA` (~10%), ~54
  `#ifdef VITA` occurrences total. Where the port does touch shared code,
  it's usually inlined directly into existing cross-platform functions
  rather than isolated — e.g. `src/Libraries/INPUT/Source/sdl_events.c`
  (1,601 lines) has 8 `#ifdef VITA` blocks scattered through shared SDL
  event-handling functions. The `src/vita/` directory referenced by CMake
  (`vita_SRCS`) is vestigial and unused — there's no real platform-isolation
  layer.
- Since the Vita fork commit (`311b5940`), there have only been 4 commits,
  all by one maintainer, all narrow reactive bug fixes — not refactor work.
- **Testing infrastructure is effectively none.** `CMakeLists.txt`'s
  `ENABLE_EXAMPLES` option (commented "can be broken!") builds test-like
  targets (`playmov`, `TestSimpleMain`, `BoxTest`, `FixTest`, etc.), but
  their source files have already been deleted from the tree — dead,
  broken CMake config, not a working test suite. CI (`.travis.yml`,
  `appveyor.yml`) only compile-checks; nothing runs tests.

**Conclusion**: a full refactor is not recommended. With no automated
tests and ~1,742 globals coupling everything together, a "tests first,
then refactor component by component" strategy would cost far more in
scaffolding than it returns, and diverging from the shared `GameSrc`/
`Libraries` code forfeits future upstream fixes. Targeted, validated by
manual on-device play-testing (as the existing single-maintainer fixes
already are), is the practical approach.

## Frame pacing / vsync judder

- `mainloop()` (`src/GameSrc/mainloop.c`) is a flat loop with no explicit
  frame cap of its own.
- Simulation is correctly delta-time-based: `update_state()`
  (`src/GameSrc/gametime.c`) derives real elapsed time from
  `SDL_GetTicks()`-driven `TickCount()`, and `physics_run()`/
  `advance_animations()` scale motion by that real `deltat` — positions
  don't break at variable framerate.
- **Presentation is not decoupled from vsync.** `SDLDraw()`
  (`src/MacSrc/Shock.c:497`) calls `vita2d_swap_buffers()` unconditionally
  every loop iteration, and there is no `vita2d_set_vblank_wait(0)` call
  anywhere in the codebase — every frame blocks on the display's 60Hz
  vblank. Effective present rate can only land on 60/N submultiples (60,
  30, 20, 15…). When per-frame CPU cost falls between one and two vblank
  periods (~16.7–33.3ms — the "around 30fps" zone), actual frame time
  oscillates between those two buckets, producing visible judder even
  though average FPS reads ~30. Comfortably above 30 (locks to 60) or
  comfortably below (locks to 20/15) feels smooth because it lands cleanly
  on one submultiple instead of oscillating between two.
- There is no in-game option to disable/tune this — checked
  `src/MacSrc/Prefs.c`, no fps/vsync setting exists.

## Single-threaded engine

- The only thread in the entire codebase is the XMIDI music sequencer
  (`SDL_CreateThread(MyThread, ...)` in `src/MacSrc/Xmi.c:704`). The game
  loop, AI, physics, and the entire CPU software rasterizer all run on one
  core.
- This means three of the Vita's four Cortex-A9 cores are essentially idle
  every frame — the largest untapped hardware resource identified in this
  investigation.

## Resolution scaling

- Internal render resolution genuinely changes with the video setting, not
  just final upscaling — `src/MacSrc/Shock.c:159-178` compares the chosen
  `width`/`height` against `VITA_FULLSCREEN_WIDTH`/`VITA_FULLSCREEN_HEIGHT`
  (960×544, defined at `Shock.c:59-60`) and computes a scaled `destRect`
  for letterboxing. So native res (960×544) is 4× the pixels of 480×272
  for the CPU software rasterizer (`src/GameSrc/fr*.c`) to fill. Vita
  builds use `-DENABLE_OPENGL=OFF`; GXM/vita2d is only used for the final
  texture blit/present, not 3D scene rasterization — the 3D cost is 100%
  CPU-bound.

## Measured data (PSVshell, stock clocks — no overclock)

| Resolution | CPU cores | FPS | RAM | VRAM |
|---|---|---|---|---|
| 480×272 | 1 core 100%, 1 core 40-60%, 2 cores 10-20% | 30-45, brief 60 peaks | 280/365MB | 22/112MB (26MB phys free) |
| 960×544 | same pattern | 15-30 max | same | same |

**Interpretation**: FPS roughly halves (not quarters) for 4× the pixels,
implying a large fixed per-frame cost independent of resolution (scene/BSP
traversal, object simulation, AI) plus a smaller resolution-scaling
pixel-fill cost. The 40-60%-loaded core is most likely OS/system overhead
(audio mixing, touch/input polling, display compositor) rather than a
second game thread — there isn't one.

This stacks with the vsync-quantization finding above: at 480×272, frame
times sit mostly in the 1-2 vblank straddle zone (30-60fps) — the
judder-prone band. At 960×544, the engine is CPU-bound into the 2-4 vblank
band (15-30fps) regardless of vsync pacing. **The frame-pacing fix alone
will not fix native-resolution performance** — it will smooth judder
within whatever throughput ceiling exists, but that ceiling itself needs
to move for native res to feel good.

## Existing (unwired) GPU rendering path

`src/MacSrc/OpenGL.cc` (960 lines) is a real, already-implemented
hardware-accelerated 3D renderer for desktop platforms — not just a blit
layer. It has genuine polygon/texture-map draw calls (`opengl_draw_tmap`,
`opengl_draw_poly`, `opengl_bitmap`) that substitute for the CPU
rasterizer, plus real shader compilation
(`CreateShader("main.vert", "texture.frag", ...)`). It's built on desktop
OpenGL via SDL2's GL context API (`SDL_GL_CreateContext`,
`SDL_GL_MakeCurrent`, `#include <GL/gl.h>`).

The Vita has no desktop OpenGL — the GPU is only reachable via SceGxm
directly, or a community wrapper like **vitaGL** (OpenGL ES-ish over
SceGxm). The `vita/Dockerfile` comment (line 6) explicitly notes the
VitaSDK image ships both plain `sdl2` and `sdl2_vitagl`, and this project
deliberately links the plain one. Setting `-DENABLE_OPENGL=ON` as-is would
not build on Vita (no `GL/gl.h`, no GL-capable SDL2 linked) — it is not a
flag that's "there but off."

Wiring this existing renderer to vitaGL is a scoped but real porting
project (swap to `sdl2_vitagl` + link `vitaGL`, adapt shader
syntax/entry-points for GLES compatibility) — an alternative to
parallelizing the software rasterizer, since the GPU-rendering abstraction
already exists in the codebase.

## AI frame-coupling — do not touch

`wait_frames`/`DEFAULT_FRAMES`/`COMBAT_FRAMES` in `src/GameSrc/newai.c`
(around line 874-967) decrement once per `ai_run()` call (i.e. once per
render frame) rather than scaling by real elapsed time, unlike
`physics_run()`/`advance_animations()`. This means AI reaction speed
varies with sustained FPS.

This is **original/upstream design, not something the Vita port
introduced** — `newai.c` was last changed 2020-06-14, well before the
Vita port commit (`311b5940`, 2022-02-21), and there is no `#ifdef VITA`
anywhere near this logic. Changing it would alter AI pacing relative to
the original game. **Recommendation: leave this alone** to preserve the
original experience.

## Ranked recommendations

1. **Frame-pacing fix** — decouple `SDLDraw()`/`mainloop.c` from vsync's
   default blocking swap (e.g. `vita2d_set_vblank_wait(0)` plus explicit
   pacing, or `sceDisplayWaitVblankStartMulti()`). Small, safe, removes
   judder within the current throughput ceiling.
2. **Parallelize the software rasterizer** across the three idle CPU
   cores (tile/scanline-split, a well-trodden pattern for old
   single-threaded software renderers on multicore hardware). Highest
   leverage for native-resolution throughput; largest engineering effort
   and risk of everything on this list.
3. **vitaGL/GPU offload** — wire the existing `OpenGL.cc` renderer to
   vitaGL as an alternative to (2), moving resolution-scaling cost to the
   GPU instead of spreading it across CPU cores. Real but scoped porting
   project.
4. **Do not touch AI frame-coupling** (`newai.c`) — original/upstream
   behavior; changing it would diverge from the original game's feel.
