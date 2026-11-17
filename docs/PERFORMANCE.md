# Vita performance notes

What was measured on the PS Vita build, what was tried, and what it gave,
in the order it was done. Three parts:

1. **Three cores.** The investigation, then the 3D view's pixels filled on
   three cores: 27.0 to 44.8 fps at 960×544.
2. **The GPU renderer.** The GPU fills them, with the look of the software
   renderer: every place measured at the screen's rate, where three cores
   gave 37 to 47 fps.
3. **Smoothness.** The stutters and the waits left at that rate: 8.5
   stutters a minute down to 0.6 between two of its captures, and the steps
   still to take.

The captures each part rests on were in `docs/profiles-cpu/`,
`docs/profiles-gpu/` and `docs/profiles-perf/`, under the names the
steps give them. They are no longer kept in the tree: they are in the
repository's history, up to the merge of the smoothness work into `dev`
(commit `1eb33db0`), and `git show 1eb33db0:docs/profiles-perf/...`
brings one back. File names, line numbers, options and settings in a part
are the tree's when that part was written: [ARCHITECTURE.md](ARCHITECTURE.md)
describes today's. How to build the profiler and read its log is in
[PROFILING.md](PROFILING.md).

## Contents

- [Part 1: three cores](#part-1-three-cores)
  - [Summary](#summary)
  - [Code organization](#code-organization)
  - [Frame pacing / vsync judder](#frame-pacing--vsync-judder)
  - [Single-threaded engine](#single-threaded-engine)
  - [Resolution scaling](#resolution-scaling)
  - [Measured data (PSVshell, stock clocks — no overclock)](#measured-data-psvshell-stock-clocks--no-overclock)
  - [Existing (unwired) GPU rendering path](#existing-unwired-gpu-rendering-path)
  - [AI frame-coupling — do not touch](#ai-frame-coupling--do-not-touch)
  - [How to measure](#how-to-measure)
  - [Baseline](#baseline)
  - [Speed-ups that don't need threads](#speed-ups-that-dont-need-threads)
  - [Multicore rasterizer: the design that worked](#multicore-rasterizer-the-design-that-worked)
  - [Pitfalls found, in the order they were hit](#pitfalls-found-in-the-order-they-were-hit)
  - [Results of the first attempt](#results-of-the-first-attempt)
  - [Results of the retry](#results-of-the-retry)
  - [Recommended order for the retry](#recommended-order-for-the-retry)
  - [Ranked recommendations](#ranked-recommendations)
- [Part 2: the GPU renderer](#part-2-the-gpu-renderer)
  - [Starting point](#starting-point)
  - [Decisions](#decisions)
  - [The approach](#the-approach)
  - [Roadmap](#roadmap)
  - [How it is checked](#how-it-is-checked)
  - [Step G1: feasibility](#step-g1-feasibility)
  - [Step G2: keep the CPU out of GPU memory](#step-g2-keep-the-cpu-out-of-gpu-memory)
  - [Step G3: two fixes, then textures](#step-g3-two-fixes-then-textures)
  - [Step G4: light like the CPU's, and a cheaper shader](#step-g4-light-like-the-cpus-and-a-cheaper-shader)
  - [Step G5: light per pixel, one scene a frame](#step-g5-light-per-pixel-one-scene-a-frame)
  - [Step G6: the GPU renderer in the game](#step-g6-the-gpu-renderer-in-the-game)
  - [Step G7: holding the screen's rate](#step-g7-holding-the-screens-rate)
  - [Step G8: use the GPU's time](#step-g8-use-the-gpus-time)
  - [Step G9: a cheaper HUD](#step-g9-a-cheaper-hud)
  - [Step G10: cyberspace on the GPU](#step-g10-cyberspace-on-the-gpu)
  - [Step G11: the sending on three cores](#step-g11-the-sending-on-three-cores)
  - [Step G12: the paneled view without the copy](#step-g12-the-paneled-view-without-the-copy)
  - [Where the GPU renderer stands after G12](#where-the-gpu-renderer-stands-after-g12)
- [Part 3: smoothness](#part-3-smoothness)
  - [After G12: the worker cores and the stutters](#after-g12-the-worker-cores-and-the-stutters)
  - [The next steps](#the-next-steps)
    - [S1: sound effects decoded before they are played](#s1-sound-effects-decoded-before-they-are-played)
    - [S1b: audio logs](#s1b-audio-logs)
    - [S2: the draw list shared only when the frame is short of time](#s2-the-draw-list-shared-only-when-the-frame-is-short-of-time)
    - [S3: the other stutters](#s3-the-other-stutters)
    - [S4: a quieter profile build](#s4-a-quieter-profile-build)
    - [S5: the side panels' update](#s5-the-side-panels-update)
    - [S6: art read from memory, not from the card](#s6-art-read-from-memory-not-from-the-card)
    - [S7: clicks](#s7-clicks)
    - [S7b: what the timers named](#s7b-what-the-timers-named)
    - [S7c](#s7c)
    - [S8: saving and loading a game](#s8-saving-and-loading-a-game)
    - [S8b: the game file's packing](#s8b-the-game-files-packing)
    - [S8c: a level's pieces written together](#s8c-a-levels-pieces-written-together)
    - [S9: stepping onto a new map square](#s9-stepping-onto-a-new-map-square)
    - [S9b: the music's scores kept](#s9b-the-musics-scores-kept)
    - [S9c: the first time into a zone, and the trigger](#s9c-the-first-time-into-a-zone-and-the-trigger)
    - [S10: changing level](#s10-changing-level)
    - [S11: heavy views](#s11-heavy-views)
    - [S11b: translucent polygons on the GPU](#s11b-translucent-polygons-on-the-gpu)
    - [S11c: the shader as the compiler wants it, and a cheaper fill](#s11c-the-shader-as-the-compiler-wants-it-and-a-cheaper-fill)
    - [S12: frames shown a refresh late](#s12-frames-shown-a-refresh-late)
    - [S12b: the audio thread off the game's core](#s12b-the-audio-thread-off-the-games-core)
    - [S13: translucent bitmaps on the GPU](#s13-translucent-bitmaps-on-the-gpu)
    - [S13b: a worker thread held up by the music](#s13b-a-worker-thread-held-up-by-the-music)

## Part 1: three cores


Findings from an investigation into codebase health and native-resolution
performance on PS Vita, and from a **first attempt at a multicore
software rasterizer**. That attempt's code was discarded so that it could
be redone cleanly, with profiling done only at 960×544 and always from
the same spot in the game.

This part records everything that attempt learned, so it can be rebuilt
without the old code:
- how to measure
- the speed-ups that don't need threads
- the design that worked, and the pitfalls hit along the way
- the measured results
- a recommended order for the retry

### Summary

- **The codebase.** It is coupled 1994-vintage C, not a neglected or
  poorly written project; that's expected for a Night Dive Mac source
  release ported forward. The Vita layer is a thin overlay on a much
  larger, untouched upstream
  ([Shockolate](https://github.com/Interrupt/systemshock)) codebase. There
  is no automated test suite.
- **Native-resolution performance is CPU-bound and single-threaded.** At
  960×544, about two thirds of each frame is the software rasterizer
  filling pixels: ~43 ms of a ~57 ms frame (~17.7 fps). The game loop,
  AI and scene traversal are small next to it.
- **The three cores a game gets really do run in parallel.** A
  benchmark ran the same compute loop and random memory reads on 1, 2 and
  3 threads at once: the wall time didn't change. Splitting the pixel
  filling across cores, by screen rows, works and stays bit-exact.
- **Two speed-ups don't need threads.**
  - A bit-exact FPU replacement for `fix_div` (the game's 64-bit software
    division).
  - Switching the music from the Nuked to the DOSBox OPL3 emulator. Nuked
    keeps a core about half busy (52% measured in the retry); DOSBox
    takes 12%.
- **Best result of the first attempt, at 960×544:** a median of 26.4 fps
  (p75 30.7) with the pixel filling on 3 cores, against 18.3 fps for the
  same recorded frames drawn on one core in the same session, and 17.7 fps
  for the original game. Those runs were not all taken at the same spot,
  so only numbers measured within one session compare fairly; see "How to
  measure".

### Code organization

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

### Frame pacing / vsync judder

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

### Single-threaded engine

- **Threads that exist.** The game loop, AI, physics and the whole CPU
  software rasterizer run on the main thread. The only other threads are
  for music and sound:
  - the XMIDI sequencer (`SDL_CreateThread(MyThread, ...)` in
    `src/MacSrc/Xmi.c`)
  - SDL_mixer's audio callback thread
    (`Mix_OpenAudio(48000, AUDIO_S16SYS, 2, 2048)` in
    `src/MacSrc/SDLSound.c`)
- **The music was expensive.** The audio callback synthesizes it with
  libADLMIDI, which the game ran with the **Nuked OPL3** emulator
  (`ADLMIDI_EMU_NUKED_174`, set in `AdlMidiInit`,
  `src/MusicSrc/MusicDevice.c`) at the full 48 kHz rate. Nuked is the
  accurate kind of OPL3 emulator and by far the slowest. The first attempt
  saw it keep one core about **45% busy**, and the retry measured 52%. It
  is the "40–60%" core in the PSVshell table below, not OS overhead. The
  Vita now defaults to the DOSBox emulator (12%); see "Music: DOSBox OPL3
  instead of Nuked".
- **Games get 3 cores.** A Vita game can use cores 0, 1 and 2 (masks
  `SCE_KERNEL_CPU_MASK_USER_0..2`, `0x10000 << n`); core 3 belongs to the
  system. Apart from the music thread's share, two of those three cores
  sit idle every frame.

### Resolution scaling

- Internal render resolution genuinely changes with the video setting, not
  just final upscaling — `src/MacSrc/Shock.c:159-178` compares the chosen
  `width`/`height` against `VITA_FULLSCREEN_WIDTH`/`VITA_FULLSCREEN_HEIGHT`
  (960×544, defined at `Shock.c:59-60`) and computes a scaled `destRect`
  for letterboxing. So native res (960×544) is 4× the pixels of 480×272
  for the CPU software rasterizer (`src/GameSrc/fr*.c`) to fill. Vita
  builds use `-DENABLE_OPENGL=OFF`; GXM/vita2d is only used for the final
  texture blit/present, not 3D scene rasterization — the 3D cost is 100%
  CPU-bound.

### Measured data (PSVshell, stock clocks — no overclock)

| Resolution | CPU cores | FPS | RAM | VRAM |
|---|---|---|---|---|
| 480×272 | 1 core 100%, 1 core 40-60%, 2 cores 10-20% | 30-45, brief 60 peaks | 280/365MB | 22/112MB (26MB phys free) |
| 960×544 | same pattern | 15-30 max | same | same |

**Interpretation**: FPS roughly halves (not quarters) for 4× the pixels,
implying a large fixed per-frame cost independent of resolution (scene/BSP
traversal, object simulation, AI) plus a smaller resolution-scaling
pixel-fill cost. The 40-60%-loaded core turned out to be the music
synthesis (Nuked OPL3; see "Single-threaded engine"). The later profiler
measurements also refine the "large fixed cost" reading: at 960×544,
pixel filling is about two thirds of the frame.

This stacks with the vsync-quantization finding above: at 480×272, frame
times sit mostly in the 1-2 vblank straddle zone (30-60fps) — the
judder-prone band. At 960×544, the engine is CPU-bound into the 2-4 vblank
band (15-30fps) regardless of vsync pacing. **The frame-pacing fix alone
will not fix native-resolution performance** — it will smooth judder
within whatever throughput ceiling exists, but that ceiling itself needs
to move for native res to feel good.

### Existing (unwired) GPU rendering path

*`OpenGL.cc`, its `shaders/` folder and the `ENABLE_OPENGL` option were
removed from the tree on 2026-10-08. This section describes them as they
were.*

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

The GPU work that followed the multicore retry took another route, a
palette-indexed renderer fed by the recorder's draw list: see
Part 2.

### AI frame-coupling — do not touch

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

### How to measure

The first attempt's profiles were taken in different places in the game,
so absolute numbers from different files don't compare. Variants
measured within one session, alternating every 5 s, do compare. For the
retry:

- **Conditions.**
  - 960×544, detail Max, stock clocks (444 MHz ARM), Multicore setting as
    under test.
  - Start a new game and stand still in the very first area, for at least
    30 s, without moving the view.
  - If the build runs a start-up benchmark, wait ~5 s at the menu first.
- **Compare variants inside one session.** Let the profile build switch
  between variants every few seconds (the first attempt used 5 s), and
  start a new log line at each switch. Never compare two runs taken at
  different spots.
  - The retry's profile build does this itself: code under test reads
    `vprof_variant`, which cycles through `VPROF_VARIANT_COUNT` values
    (both in `src/Libraries/H/vprof.h`) every 5 windows. As the retry
    leaves it, variant 0 draws on one core and variant 1 on three. It only switches
    at a window boundary, so no window mixes two variants, and each log
    line carries `var=N`. Each step just redefines what the variants mean.
  - Each log line also carries `music=` (the time spent synthesizing
    music, as a share of one core), `acpu=` and `mcpu=` (the last core the
    audio and main threads were seen on). The music time is measured on
    the audio thread and handed over atomically (`vprof_audio_add`),
    because `vprof_record` is for the main thread only.
  - Since step 4 it also carries the recorder's figures: `record=` (time
    spent recording), `cmds=`, `copied=` and `flushes=` per frame, and
    `check=` (rows that differed / self-checks run).
  - Since step 5, per frame unless noted:
    - `views=` 3D views drawn
    - `batches=` runs of calls handed to all threads
    - `solo=a/b` calls the main thread drew alone: `a` recorded but with a
      mapper that isn't band-aware, `b` not recordable and drawn directly
    - `wait=` time the main thread spent waiting for the workers
    - `busy=` time each thread spent drawing (main/worker 1/worker 2)
    - `late=` the longest a worker took to start a batch in the window (µs)
    - `split=` the rows where the main view's bands end
    - `wcpu=` the core each thread last drew on
    - `leaks=` rows a thread wrote outside its band, in the self-checks
    - `helpscan=` time in the on-screen help's scan (`olh_scan_objects`)
    - with split replays `raster=` is wall time on the main thread, and
      `calls_per_frame=` counts batches plus calls drawn alone
- **What the profiler looked like (re-create it).**
  - **Build.**
    - The `ENABLE_VITA_PROFILE` CMake option adds `-DVITA_PROFILE` and links
      `-lScePgf_stub` for the overlay font.
    - `./build.sh profile` builds into `build-profile/`, so normal and
      profile builds don't share a cache.
    - In a normal build every macro compiles to nothing; check it with
      `arm-vita-eabi-nm build/systemshock | grep vprof_`.
  - **Timers.**
    - `sceKernelGetProcessTimeWide()` (µs) around each phase, via a
      `VPROF_RUN(phase, code)` macro in a header the libraries can include
      (`src/Libraries/H/`).
    - Per-frame totals are folded into 1 s windows that log avg/max per
      phase.
    - A window is dropped when the loop mode (menu, game, fullscreen,
      cutscene…) or the replay variant changes inside it.
  - **Phases and where they were hooked.**
    - `loopLine` macro in `src/GameSrc/Headers/mainloop.h`: every
      `game_loop` call is already wrapped in it with an ID, so mapping IDs
      to phases gives
      - input: `ML|1`
      - render3d: `GL|0x1A`
      - ui2d: `GL|0x17/18/19/1B`
      - sim: other `GL` IDs
    - `pump_events` → input.
    - Around `SDLDraw()` → present.
    - In `fr_rend` (`src/GameSrc/frmain.c`):
      - traverse: `fr_pipe_start` … `fr_pipe_end`
      - sendview: `fr_send_view()`
    - At the three 3D→2D handoff sites (see "Multicore rasterizer") → raster,
      counting calls per frame as well.
    - "other" = frame − (input + sim + render3d + ui2d + present).
  - **Output.**
    - Appended once per second to `ux0:data/systemshock/profile.txt`.
    - Also a 4-line overlay drawn in `SDLDraw` after the game texture with
      `vita2d_load_default_pgf()` / `vita2d_pgf_draw_text`. The overlay is
      drawn on top of the frame, so it never leaves pixels in the game's
      canvas.

### Baseline

From `docs/profiles-cpu/profile-step-1.txt`, the retry's own profiler (see "How to measure"
below), original single-threaded game, 960×544, detail Max, stock clocks,
standing still in the first area for about 3 minutes. Two long steady
segments appear, at two different in-game loop modes with different 3D
viewport sizes — both are reported rather than picking one:

| mode | fps | frame | render3d | traverse | raster | sendview | sim | present | raster calls/frame |
|---|---|---|---|---|---|---|---|---|---|
| `FULLSCREEN_LOOP` (full 3D view) | 24.7 | 40.4 ms | 33.0 ms | 30.7 ms | 28.1 ms | 3.15 ms | 0.71 ms | 2.92 ms | 131 |
| `GAME_LOOP` (paneled 3D view) | 45.5 | 22.0 ms | 16.2 ms | 16.4 ms | 14.0 ms | 0.55 ms | 0.64 ms | 2.87 ms | 115 |

- **Faster than the old indicative numbers.** The now-superseded
  `docs/profile-960x544.txt` read 17.7 fps / 56.6 ms from a single,
  unlabeled run. The current engine measures meaningfully faster in both
  loop modes — whether from since-landed upstream fixes or a different
  spot in the game is unclear and wasn't investigated further here.
- **Render3d still dominates, and raster still dominates render3d.**
  `render3d` is 82% (`FULLSCREEN_LOOP`) / 74% (`GAME_LOOP`) of the frame;
  `raster` alone is 68% / 64% of the frame and 85–93% of `render3d`. This
  matches the old finding ("raster is two thirds of the mean frame") and
  confirms pixel filling is still the right thing to parallelize.
  `traverse` fully contains `raster` and nests correctly throughout both
  logs (`traverse` ≤ `render3d` ≤ `frame_avg` always holds) — see the
  note on multi-view frames below for why that wasn't true on the first
  attempt at this capture.
- **`GAME_LOOP`'s paneled 3D view renders roughly half the pixels** of
  `FULLSCREEN_LOOP`'s full 3D view: render3d/traverse/raster are all ~2×
  smaller and fps is ~2× higher, consistent with a smaller 3D viewport
  when the classic UI panels are up around it.
- **Polygon/call count matches the old finding's order of magnitude.**
  ~115–131 raster-handoff calls/frame is the same ballpark as the old
  "~30–40 polygons, 90 at most" — this count is per handoff call (a
  polygon can dispatch through more than one lit/CLUT variant), not
  strictly one per polygon, so it runs a bit higher.
- **Multi-view frames are real and already accounted for.** Every frame
  draws two 3D views while On-Screen Help is on, which is the default: the
  main view, and the help's scan of it at a third of the size (see "One
  split per 3D view" below). Of the 131 calls here, about 82 are the main
  view's and 49 the scan's. `render_run()` adds one more view per visible
  hacked security-camera monitor.
- **Splitting by screen rows suits that.** Every thread has rows of
  nearly every polygon to draw.

The same run at 480×272 (`docs/profile-480x272.txt`, now superseded) gave
43.5 fps with raster 14.6 ms; the retry drops that resolution.

### Speed-ups that don't need threads

Each of these can be done and measured on its own, before any threading.

#### FPU `fix_div`

- **Why it's slow.** `fix_div` (`src/Libraries/FIX/Source/fix.c`)
  computes `((int64_t)a << 16) / b`. The Cortex-A9 has no integer divider,
  so that is a call to the 64-bit library division `__aeabi_ldivmod`, and
  the mappers call `fix_div` on every row, column and scanline.
- **The bit-exact replacement.**
  - Compute `(double)a * 65536.0 / (double)b`. Both operands convert to
    double exactly, and `vdiv.f64` is a hardware instruction.
  - If `b == 0` or the result is not strictly inside ±2147483646, use the
    original int64 code. That covers overflow and the edge cases, and keeps
    their results and `gOVResult` codes exactly as before.
  - Otherwise just convert to int32. No remainder check is needed: a
    non-integer quotient is at least `1/|b|` from an integer, and the
    double's rounding (even a few ulps under `-Ofast`) could only cross one
    if `|a · 65536| ≥ 2^53`, but it is below 2^47. So the first attempt's
    ±1 remainder correction could never fire; with it removed, the retry's
    harness (`tests/fix_div/run.sh`) still finds 0 differences in 10⁸
    cases.
- **Verification.**
  - 10⁸ random pairs of varied magnitudes, plus near-exact quotients and
    ± small offsets around edge values (0, ±1, ±65536, `INT32_MIN`/`MAX`,
    …).
  - The reference must be the **original function compiled with the same
    flags**. Code that left-shifts negative values is undefined
    behaviour, so a reference inlined differently disagrees at
    `INT32_MIN` under `-Ofast`.
  - 0 differences at `-O2` and at `-Ofast`.
  - Check the generated code with `arm-vita-eabi-objdump`: `vdiv.f64` on
    the fast path, `__aeabi_ldivmod` only on the fallback.
  - **Retry's results.**
    - Host harness `tests/fix_div/run.sh`: 10⁸ generated cases plus 400
      edge pairs, with gcc and clang at `-O2` and `-Ofast`, comparing the
      return value and `gOVResult`. 0 differences. It does catch
      deliberate mistakes: widening the positive range bound gave 16,351
      mismatches in 10⁶ cases, and dropping the `gOVResult` reset was
      caught too.
    - On device, the profile build compares old and new at startup with
      the real ARM compiler and flags: `fixdiv_check mismatches=0/1000400`.
    - objdump of the normal build: convert, `vmul.f64`, `vdiv.f64`, range
      check, convert, return; `__aeabi_ldivmod` only after a zero divisor
      or an out-of-range result.
- **Measured effect.**
  - The first attempt measured it together with the overhead cuts below:
    the recorded frames replayed on one core ran at 18.8 fps vs 17.0–19.9
    for the other variants in that session.
  - **Retry, measured alone** (`docs/profiles-cpu/profile-step-2.txt`): full 3D view
    (`FULLSCREEN_LOOP`), 960×544, standing still in the first area for
    3 min 17 s, the two variants alternating every 5 s. Medians of 95 and
    98 windows:

    | variant | fps | frame | render3d | traverse | raster | sim | raster call avg |
    |---|---|---|---|---|---|---|---|
    | original `fix_div` | 24.2 | 41.3 ms | 33.7 ms | 31.4 ms | 28.7 ms | 0.73 ms | 0.219 ms |
    | FPU `fix_div` | 27.0 | 37.1 ms | 29.8 ms | 27.4 ms | 25.0 ms | 0.59 ms | 0.191 ms |

  - That is **+11.6% fps, −4.2 ms per frame, raster −3.8 ms (−13%)**. The
    FPU version won in all 20 pairs of neighbouring 5 s blocks (median
    +2.8 fps, minimum +0.2), and the quartiles don't overlap (24.2–24.3
    vs 26.7–27.0 fps).
  - The profile build pays for choosing the variant on every call: the
    original variant ran at 24.2 fps there vs 24.7 in the step 1 baseline.
    Normal builds call the FPU version directly.
- **`gOVResult`.** Also set `gOVResult = 0` only when it isn't 0 already.
  It is a global written by every `fix_div`/`fix_mul_div` call. Its only
  reader is `src/Libraries/3D/Source/points.c`, right after its own
  division. With several cores dividing, an unconditional store bounces
  that cache line between them.
  - Do the check through `volatile`. `-Ofast` allows store data races, and
    GCC turned `if (gOVResult) gOVResult = 0;` back into an unconditional
    store.

#### Music: DOSBox OPL3 instead of Nuked

- **The change, as built.**
  - The ADLMIDI device takes either emulator (`AdlMidiSetEmulator` in
    `src/MusicSrc/MusicDevice.c`). Both are compiled into `ADLMIDI_SRC`.
  - The Vita defaults to DOSBox. Other platforms keep Nuked 1.7.4
    (`ADLMIDI_EMU_NUKED_174`), which is what the game has always used, not
    libADLMIDI's default Nuked.
  - On Vita, "Midi Player" in the sound options offers "DOSBox OPL3" and
    "Nuked OPL3" in place of ADLMIDI and Native MIDI.
- **First attempt.**
  - Nuked occupied ~45% of a core: a worker on that core started ~10 ms
    into every ~22 ms frame at 480×272.
  - With DOSBox, no worker start was delayed in any window (0 of 90).
- **Retry, measured alone** (`docs/profiles-cpu/profile-step-3b.txt`): full 3D view
  (`FULLSCREEN_LOOP`), 960×544, standing still in the first area for
  3 min 41 s, the two emulators alternating every 5 s. Medians of 109 and
  106 windows:

  | emulator | synth's share of one core | fps | frame | raster |
  |---|---|---|---|---|
  | Nuked 1.7.4 | 51.8% (p25–p75 51.0–53.0) | 27.1 | 37.0 ms | 24.8 ms |
  | DOSBox | 11.6% (10.9–12.2) | 26.8 | 37.3 ms | 25.1 ms |

  - DOSBox frees **about 40% of a core**: a median of 40.4 points per pair
    of neighbouring 5 s blocks, 38.8–42.5 across 22 pairs.
  - No meaningful fps change (median −0.2 fps per pair, range −0.7 to
    +1.1), as expected while the game is single-threaded: the audio thread
    was seen on all three cores, the main thread on cores 1 and 2, and
    never both on the same core in any window.
  - An earlier capture (`docs/profiles-cpu/profile-step-3.txt`, 5 min 47 s) read 53.7%
    vs 9.2%, with DOSBox still run at the PCM rate. Running it at the
    chip's native rate (see below) costs about 2.4 points of a core.
- **Pitfalls found, and their fixes.**
  - **DOSBox chip setup is expensive, and repeated.**
    - `Chip::Setup` (`chips/dosbox/dbopl.cpp`) rebuilds its attack-rate
      table by simulating every envelope sample by sample, and libADLMIDI
      creates a new chip on every reset and bank change: 9 setups in
      `AdlMidiInit`, 3 per `StopTheMusic`, about 24 before the intro's
      first frame.
    - Symptoms: the intro loaded slowly with its audio ahead of the video,
      skipping it froze the game for 1–2 s, and the profile capture showed
      ~700 ms frames after switches to DOSBox.
    - Fix: the table only depends on the sample rate, so it is cached per
      rate. Rendered audio is byte-identical with and without the cache.
  - **The instrument bank is per device.**
    - Only `ReadXMI` set it (`setupMode` → `adl_setBank(45)`), so a device
      recreated by a "Midi Player" change played with libADLMIDI's default
      bank until the next theme load. This bug is older than the Vita fork.
    - Fix: `InitDecXMI` (`src/MacSrc/Xmi.c`) re-applies the last mode.
  - **DOSBox must run at the chip's native rate.**
    - The game sets `adl_setRunAtPcmRate(1)`. Nuked 1.7.4 can't
      (`canRunAtPcmRate()` is false), so it ignores the setting and always
      runs at 49716 Hz. DOSBox really runs at 48000 Hz: it then plays
      about 7 cents sharp and feedback-heavy instruments lose their
      movement.
    - Fix: DOSBox runs natively and libADLMIDI resamples.
  - **Nuked 1.7.4 has undefined shifts, and the Vita keeps their sound.**
    - `OPL3_SlotGeneratePhase` (`chips/nuked/nukedopl3_174.c`) shifted by
      32 bits or more in two places. x86 wraps the shift count, so it
      mostly worked there; ARM gives 0.
    - On the Vita this turned waveforms 6 and 7 into a one-sided pulse:
      full level for half the period, silence for the other half. 19
      programs of the game's bank use them, and ten came out 2–6 dB
      quieter than on a PC.
    - The shifts are now defined. The Vita keeps that pulse on purpose, on
      both emulators, because its mix is the preferred one:
      `OPL3_HALF_SQUARE_WAVES`, set for Vita builds in `CMakeLists.txt`.
      Other platforms get the accurate waveforms.
    - Checks, rendering every program of the bank on the host: the new
      Nuked code is byte-identical to the original code under ARM's shift
      rule; DOSBox is within about 1 dB of it on every audible instrument;
      `-fsanitize=shift` reports nothing in either mode.
- **Sound.** On the Vita the two emulators have the same mix by
  construction; it was compared by ear on the new-game music.
- **Native MIDI is not an alternative.** The "Native MIDI" backend sends
  notes to the OS synthesizer (winmm on Windows, ALSA on Linux); the Vita
  has none, and its setting no longer offers it. FluidSynth is disabled in
  the Vita build and costs more than Nuked.

#### Pre-existing engine bugs worth knowing

- **Clipped bitmaps get uninitialized lighting.**
  `gr_clip_poly(n, 4, …)` in `h_map` (`2D/Source/Gen/gentm.c`)
  interpolates only x, y, u and v, so new vertices' lighting `i` is
  whatever was in temporary memory. Lit sprites cut by the screen edge
  therefore get garbage lighting on those vertices. It is harmless on one
  thread, but anything that clips per thread reads different garbage
  (see below).
- **One texel past non-power-of-2 textures.** The linear, floor and wall
  mappers sometimes read one texel past the end of those textures,
  through the `vtab` path, due to rounding at span ends. The value read
  depends on whatever memory follows the texture.
- **One entry outside the row table of those textures (fixed in step 5).**
  The same rounding makes the mappers ask the table (`gr_make_vtab`,
  `2D/Source/vtab.c`) for line −1 or line h. Those entries were whatever
  temporary memory held around the table, so the texel drawn depended on
  what had used that memory before. That can't be the same on three
  threads, each with its own temporary memory: the harness saw it as a
  rare one-pixel difference, and a crash under the thread sanitizer. The
  table now has both entries, repeating the first and the last line.
  Drawing on one core gave the same pixels before and after on 2,100 test
  frames.

### Multicore rasterizer: the design that worked

#### Why split the rasterizer, and where

- **Most of the engine can't be split.**
  - ~1,700 globals couple AI, physics and scene traversal.
  - The renderer changes game state while it draws, so the scene can't be
    traversed twice:
    - `CLEAR_AS_WE_GO` in `GameSrc/Headers/frflags.h` erases tile
      visibility bits
    - "dealt" bits are set on objects and tiles
    - object drawing calls `rand()` and spins object angles
- **At the 3D→2D handoff the problem is simple.** All 3D work (transform,
  frustum clip, projection, lighting) is done, and what remains is 2D
  polygons drawn in painter's order with no z-buffer. The three handoff
  sites are:
  - `src/Libraries/3D/Source/tmap.c`, in `draw_tmap_common`: calls to
    `tmap_func` (`h_umap`, `v_umap` or `per_umap`)
  - `src/Libraries/3D/Source/polygon.c`: the
    `grd_canvas_table[poly_index[gour_flag]]` call (`temp_upoly`,
    `temp_uspoly`, …, which draw through `h_umap`)
  - `src/Libraries/3D/Source/Bitmap.c`: the two `h_map` calls (sprites)
- **The inner loops are friendly.** Their state is in stack structs; they
  only read `grd_bm`, `grd_screen->ltab` and the canvas fill type.

#### Record, then replay

Built in the retry's steps 4 to 6 as `src/Libraries/3D/Source/rastq.c`.
The notes below say what the code does.

- **Recording.** Between `fr_pipe_start` and `fr_pipe_end` in `fr_rend`,
  each handoff call is recorded instead of drawn. A record holds:
  - copies of the vertices (taken through the pointer list), the
    `grs_tmap_info` and the `grs_bitmap`. The 3D library passes pointers
    into reused globals, and the mappers modify them.
  - the canvas `fill_type`, `fill_parm` and `clip`
  - the bitmap pixels, **plus one row on each side**, unless they are
    stable (next point). Polygon records copy no pixels: there `bits` is a
    colour.
  - a flag saying whether every mapper the call can reach is band-aware
    (see "Band-safe whitelist" below)
  - the range of rows the call can write: its vertices' rows plus 2 on
    each side. A thread skips a record whose range misses its band before
    copying anything.
- **Which pixels must be copied.**
  - Reused within a frame, so copied: the RSD unpack buffer
    `grd_unpack_buf`, the teleport effect buffer, the shared text-screen
    bitmaps, `static_bitmap`, and sprite frames in resource memory, which
    are unlocked right after the draw. The `grs_bitmap` struct
    `get_texture_map` returns is shared too.
  - Stable for the whole frame, so used in place: terrain texture pixels
    in `tmap_static_mem` and `tmap_big_buffer`, written only when a level
    loads. They are registered with `rastq_stable_pixels`.
  - Why the extra rows: the 1D wall loop (`HandleWallLoop1D_C` in
    `Flat8/fl8w.c`) masks the texture row but not the column, which can
    land up to a dozen texels outside the bitmap. A copy has to show it
    what the original's neighbours would.
- **Recording-time conversions.**
  - **RSD8 bitmaps** are unpacked at record time with `gr_rsd8_convert`
    and recorded as the resulting FLAT8/TLUC8 bitmap. That is the same
    init function the RSD path would chain to. If unpacking fails,
    nothing is drawn, like the original.
  - **Sprites for the blend mapper** (`gri_trans_blend_clut_lin_umap_init`,
    `Flat8/fl8bl.c`) are doubled at record time by the mapper's own code
    (`gr_blend_prepare`), into the unpack buffer as when drawing, and
    recorded as what that mapper goes on to draw: the doubled bitmap
    through the CLUT linear mapper. That makes them band-safe; until
    step 6 the main thread drew one such sprite alone in every frame of
    the first area.
  - **Except for `per_umap`.** `rsd8_pm_init` always takes the
    horizontal-scan mapper, while the unpacked bitmap may get the
    vertical-scan one. Those calls are drawn directly.
  - **`h_map`** (sprites) is clipped once at record time
    (`gr_clip_poly(n,4,…)`) and recorded as `h_umap`, so every band draws
    the same clipped vertices. The lighting the clip leaves uninitialized
    (see "Pre-existing engine bugs") doesn't matter here: the game never
    lights a sprite per vertex (`g3_full_light_bitmap` has no callers), so
    its sprites go through the plain, CLUT or blend mappers.
- **Drawn directly, after a flush.**
  - Translucent shaded polygons (`FIX_TLUC8_SPOLY`, `temp_stpoly`): they
    clip when drawn and light the new vertices from stale temporary
    memory.
  - RSD8 through `per_umap`, as above.
  - Bitmap types other than FLAT8, TLUC8 and RSD8.
- **Flushing.** Draw everything recorded before any draw that isn't
  recorded:
  - `draw_line_common` (3D lines)
  - `vx_render` (cyberspace voxels, called from `gameobj.c`); the first
    attempt missed this one
  - `g3_draw_point`, which has no callers
  - the end of the pass, before `fr_send_view`, because `star_render`
    reads the sky pixels

  A handoff call while `grd_canvas` isn't the recorded canvas is drawn
  directly without a flush: draw order only matters within one canvas.
  The only canvas switch inside the pass is text-screen rendering
  (`objsim.c`), which makes no 3D calls.
- **The arena.** 4 MB and 4096 records. Flush **after** storing a record
  once less than 256 KB remains, never before, so that nothing the flush
  draws comes between a call and the copy of its pixels. (Until step 6 a
  replay could overwrite the unpack buffer those pixels may live in.)
- **Replay.**
  - Walk the list in order on the recorded canvas.
  - Set the canvas state per record: `gr_set_fill_type` (the macro, which
    also re-derives `grd_function_table`), `fill_parm` and `clip`.
  - Restore the canvas state afterwards. If the flush happens while
    another canvas is current, switch to the recorded canvas for the
    replay and back.
  - When the view is split: a run of consecutive band-safe commands is
    one batch, drawn by all threads at once, one row band each. Each
    thread keeps the fill type, fill parm and clip of the command it is
    drawing in its own band (see "Per-thread state"), so the commands of a
    batch needn't share them. Commands that aren't band-safe are drawn by
    the main thread alone with a full band.
  - Until step 6 the workers read that state from the canvas, so a batch
    ended wherever it changed: 4 batches for the main view, and 6.2 ms of
    waiting per frame.
  - A view with fewer than 272 rows (`RASTQ_SMALL_VIEW_ROWS`) is replayed
    by the main thread alone: see "Results of the retry".
- **A limit that step 6 removed.** Until then the blend mapper doubled
  its sprite into the unpack buffer during the replay, so a wall texture
  taken from that buffer could see different leftover bytes past its end
  than direct drawing would. Blend sprites are now doubled at record
  time, which leaves the buffer as direct drawing does, and a replay no
  longer writes to it. The harness draws such walls again, without a
  difference.
- **Measured cost** (`docs/profiles-cpu/profile-step-4.txt`): full 3D view, 960×544,
  standing still in the first area, three variants alternating every 5 s,
  self-checked frames left out. Medians:

  | variant | windows | fps | frame | raster | record | pixels copied / frame |
  |---|---|---|---|---|---|---|
  | direct drawing | 130 | 27.0 | 37.0 ms | 24.8 ms | – | – |
  | replay, copying every bitmap | 134 | 24.4 | 41.0 ms | 25.9 ms | 1.71 ms | 831 KB |
  | replay, terrain textures in place | 129 | 26.8 | 37.4 ms | 24.7 ms | 0.28 ms | 41 KB |

  - Against neighbouring direct-drawing blocks, copying everything costs
    **+4.0 ms per frame (−2.6 fps)** over 27 comparisons, and leaving
    terrain textures in place costs **+0.3 ms (−0.2 fps)** over 26.
  - Copying is paid twice: the copy itself (1.7 ms), then drawing from
    freshly copied memory (raster +1.2 ms).
  - About 133 records and 2 flushes per frame.
  - So recording is close to free only with terrain textures used in
    place (`RASTQ_TRUST_STABLE`). Step 5 builds on that mode.

#### Row bands, bit-exact

Each thread draws only rows `[top, bot)` of the canvas. The first band's
top is `-0x40000000` and the last band's bottom is `+0x40000000`, so no
row falls outside. Outside a replay the band covers everything, which
must leave single-threaded behaviour untouched.

Built in the retry's step 5. The band is per thread (`grd_bands`,
`2D/Source/band.h`); `h_umap` and `v_umap` copy it into the loop info
(`band_top`, `band_bot`) once per call.

The rule that keeps the result identical to one pass: keep every edge
walk and per-row/per-column step exactly as it is, and only skip writing
pixels outside the band. Rules per loop family:

- **Row loops (`h_umap` family)**: linear, lit linear, floor, lit floor,
  translucent-solid linear and floor, tluc8 linear, flat, Gouraud and RGB
  polygons. Add "row in band" to each loop's draw condition, for example
  `if ((d = fix_ceil(right.x) − fix_ceil(left.x)) > 0 && in_band(y))`.
  - The punt test (`else if (d < 0) return TRUE`) must stay identical.
  - Every value the draw block changes must be recomputed at the end of
    the row; this held for all of them.
  - Some loops do `tli->y += tli->n` up front, so they need a local row
    counter.
  - The polygon and tluc8 loops don't track y: derive it once from the
    destination pointer, `(d − grd_bm.bits) / grd_bm.row`. The Gouraud
    and RGB polygon loops also skip their per-row divisions outside the
    band.
- **`h_umap` itself.** Return at once if the polygon's rows miss the
  band, and stop the segment loop once `y >= band.bot`.
- **Floors (cost).** The end-of-row `fix_div`s only produce u/v/i for the
  next row. Keep the incremental `left/right += d` and `w += dw`, and do
  the divisions only if row `y+1` is in the band.
- **Wall columns (`v_umap` family)**: wall, wall 1D, lit wall, lit wall
  1D, solid wall.
  - Clip each column's row span to the band, and advance u, v and i by
    the skipped rows with wrapping 32-bit arithmetic: `x + dx·n` equals n
    additions. It must be unsigned arithmetic (`gr_band_skip`), because
    `-Ofast` treats signed overflow as impossible.
  - Skip a column's setup divisions entirely when its span misses the
    band.
  - Likewise do the end-of-column divisions only when the next column's
    span reaches the band.
  - **Exception: the lit 2D wall** (`gri_lit_wall_umap_loop`) carries
    `di` from the column block into the next column's lighting
    (`if (di >= -256 && di <= 256) i += 1024`), so only its pixel range
    may be clipped. The lit 1D wall recomputes `di` first, so it can skip
    like the others.
- **Perspective shells** (`gri_per_umap_hscan/vscan` in `Flat8/fl8p.c`,
  `gri_lit_per_umap_hscan/vscan` in `Flat8/fl8lp.c`). The scanline
  functions they call derive x, y, u, v and the edge tests from the start
  pixel in closed form, and step lighting by `di` per pixel. The helpers
  are `gr_band_hscan`, `gr_band_vscan` and their `_miss` variants in
  `2D/Source/band.c`. So:
  - **Early reject.** Before the per-scanline clip divisions, take the
    scanline's unclipped range (`[x, max(xl, xr0, xr))` for hscan, rows
    `[y, max(yl, yr0, yr))` for vscan). If its rows miss the band, skip
    it; the clip only narrows the range.
  - **hscan narrowing.** The row of column x is
    `fix_int(x·scan_slope + fix_make(yp, 0xffff))`, monotonic in x.
    Binary-search the first column in the band and the first one past it,
    then clamp `x`, `xl`, `xr0` and `xr`, the way the canvas clip does.
  - **vscan narrowing.** Clamp `y` to `band.top` and `yl`, `yr0`, `yr` to
    `band.bot`.
  - **Lit shells.** Add `skip·di` to `pi.i`, after the shell has computed
    `di` from the range the canvas clip left.
- **Band-safe whitelist.**
  - Decide at record time, with the current fill type, which init
    function the mapper's table would pick, and check it against a list
    of initializers whose loops are band-aware (`gr_band_safe_init`,
    `2D/Source/Flat8/fl8band.c`). The table index is:
    - `h_umap` and `v_umap`:
      `(bm->flags & BMF_TRANS) + ti->tmap_type + GRD_FUNCS * bm->type`
    - `per_umap`: the hscan and vscan initializers plus its linear, floor
      and wall fallbacks, all five of which must be on the list
    - polygons: `GRC_POLY + GRD_FUNCS * type`
  - Not split, and drawn on the main thread instead:
    - scalers (`fl8s.c`, `fl8ns.c`)
    - anything unknown
  - The blend mapper (`fl8bl.c`) isn't on the list either, but the
    recorder no longer sends it anything: see "Recording-time
    conversions".
  - Opaque bitmaps with SOLID fill resolve to `gri_solid_poly_init`, a
    row loop, so they are safe.
- **Per-thread state.**
  - **Thread slot.** `lg_slot()` (`LG/Source/lgslot.c`) looks up
    `sceKernelGetThreadId()` among the registered workers, and returns 0
    without any call while there are none. Don't use `__thread`: on this
    toolchain it is emulated TLS, a function call per access.
  - **Temporary memory.** Each slot has its own `temp_malloc` stack
    (`LG/Source/tmpalloc.c`). The mappers allocate vtabs, clip buffers and
    perspective setup from it.
  - **Per-thread copies.** Each thread gets its own copies of the
    vertices, `grs_bitmap` and `grs_tmap_info`, because `h_umap` rewrites
    vertex `w` and `per_umap` rewrites `ti` and `bm->bits`.
  - **Drawing state.** A band (`grs_band`) can carry the fill type, fill
    parm, function table and clip of the call its thread is drawing. Five
    places read them from the band when it has them, and from the canvas
    otherwise: `h_umap` and `v_umap` (`Gen/gentm.c`), `per_umap`
    (`permap.c`) and the two pairs of perspective shells, for the clip.
    The table is `(*grd_function_fill_table)[fill_type]`, which is what
    `gr_set_fill_type` puts in `grd_function_table`.
  - **Everything else is shared and read-only during a replay:** the
    canvas's bitmap, the fill tables, the light tables and the recorded
    pixels. The thread sanitizer confirmed it: the one global the mappers
    write is `gOVResult`, which `fix_mul_div` now also stores
    conditionally on Vita.

#### Threads

Built in the retry's step 5 as `src/Libraries/3D/Source/rastqthr.c`. The
same hand-out code runs on the PC with pthreads, for the harness.

- **Placement.**
  - The main thread is pinned to core 0 with
    `sceKernelChangeThreadCpuAffinityMask(own id, SCE_KERNEL_CPU_MASK_USER_0)`.
    The call returns the **previous** mask, `0x70000`, not an error code.
    A game can use three of the Vita's four cores.
  - Two workers, created with masks `USER_1` and `USER_2` (`0x20000`,
    `0x40000`).
- **Priority.**
  - Workers run one step below the main thread: main 160, workers 161.
  - Read the main priority with `sceKernelGetThreadInfo`, setting
    `info.size` first.
  - Then music, SDL audio and other default-priority threads on cores 1–2
    preempt a spinning worker instead of sharing time slices with it.
- **Hand-out.**
  - A batch is a whole run of band-safe calls, so a view is normally
    handed out once.
  - The main thread publishes a batch by bumping a generation counter,
    draws its own band, then spins until a done counter reaches the
    number of workers. Every worker answers every batch, so the job never
    changes under one.
  - Workers **keep spinning** on the counter between batches and between
    frames, with a `yield` hint. They sleep on a semaphore only after
    100 ms without work (menus, pauses).
  - The wake-up handshake avoids lost batches:
    - a worker sets `asleep = 1`, then re-reads the counter before
      sleeping
    - the main thread bumps the counter, then signals only workers whose
      `asleep` is set
    - all of these accesses are seq_cst
- **Balance.**
  - Bands are shares of the view's height, moved after each view toward
    equal **finish times**, measured from hand-out to each thread's end
    and summed over the view's batches. A thread's new share is
    proportional to `share / finish time`, with 50% smoothing and a 5%
    minimum band.
  - Balancing on drawing time alone ignores a worker that started late.
  - Standing still in the first area it settles around rows 255 and 405
    of 544: the main thread takes the top 47%.
  - It only works well with one hand-out per view. With 4 batches per
    view (step 5) each batch waited for its own slowest thread, and the
    main thread waited 6.2 ms per frame; with one (step 6), 0.8 ms.
- **One split per 3D view.** Every frame also draws a smaller 3D view, on
  a 320×181 canvas at 960×544. Key the split on the canvas (bits, w, h); a
  shared split lets each view pull the other's balance.
  - That view is the on-screen help's scan: `olh_scan_objs`
    (`src/GameSrc/olhscan.c`) redraws the scene at a third of the size in
    flat colours, to find the object the help text should name. The
    retry's log shows `views=2.00` in every window with a 3D view.
  - It was meant to run 4 times a second, but the timer in
    `olh_scan_objects` (`src/GameSrc/olh.c`) is never updated, so it runs
    every frame. It costs 3.5 ms per frame, almost all scene traversal.
  - It changes its fill colour with almost every call. In step 5, where
    a batch ended at every such change, that made 39 batches, and
    splitting the view was slower than not. The retry keeps views under
    272 rows on the main thread: see "Results of the retry".
  - `render_hack_cameras()` in `src/GameSrc/render.c` adds one more view
    per visible hacked security-camera monitor.

#### Verification

- **Host harness** (built natively on Linux, no Vita needed).
  - **Build.**
    - Compile the 2D, LG and FIX library sources listed in
      `src/Libraries/CMakeLists.txt`, skipping the headers listed as
      sources and the duplicate `fix_pow.c`, with `-fcommon` and the
      library include dirs.
    - Stub `gScreenAddress`, `gScreenRowbytes` and `SetSDLPalette`.
  - **Set-up.**
    - `grd_screen->ltab` (64 KB + 256) and `clut`, `grd_ipal` (64 KB),
      `tluc8stab` (64 KB + 256), some `tluc8tab[]` entries.
    - A FLAT8 canvas via `gr_init_canvas` + `gr_set_canvas`.
  - **Input.**
    - Random perspective-projected quads: screen x/y from X/Z and Y/Z,
      `w = 1/Z`, u/v at texture corners, `i` in 0..0x0f0000.
    - Every mapper family (linear, floor, wall 2D and 1D, perspective,
      each plain, lit and clut; clipped bitmaps; the 5 polygon kinds).
    - NORM, CLUT and SOLID fills; opaque, transparent, translucent and
      non-power-of-2 textures.
  - **Gotchas.**
    - Skip combinations whose init entry is `gr_null` or `gr_not_imp`;
      they hang `h_umap`.
    - Pad textures on both sides, e.g. 64 KB of a fixed byte, because
      mappers read just outside them.
    - Until step 6 the 1D wall mapper could only be given textures from
      fixed memory: from the unpack buffer it reads leftovers whose timing
      a replay changed (see "A limit that step 6 removed" under "Record,
      then replay").
  - **Test.** Draw each list once in a single pass, then again band by
    band with the replay's batching rules, and compare all pixels. Also
    check that a single command drawn with one band writes nothing
    outside it.
  - **MemorySanitizer** (`clang -fsanitize=memory
    -fsanitize-memory-track-origins -fno-sanitize-memory-param-retval
    -fsanitize-recover=memory`): its only finding was the
    clipped-lighting bug above.
  - **First attempt's result:** 36,000 random frames at 320×200, 480×272
    and 960×544, 0 differing pixels.
  - **Retry, step 4:** the harness exists as `tests/rastq/run.sh`, without
    bands yet.
    - Each random scene is drawn directly, then through record and replay
      in both modes (copying every bitmap, and with stable textures in
      place), and once more with the recorder's own self-check on. All
      pixels are compared.
    - Scenes also include bitmaps whose buffer is overwritten between
      calls, RSD8 bitmaps, and draws outside the recorder between
      polygons.
    - It builds three ways: the default arena, a 320 KB arena that forces
      flushes inside a scene, and with the address and undefined-behaviour
      sanitizers on the recorder and the test.
    - It fails on deliberate mistakes in the recorder: not copying pixels,
      trusting every buffer as stable, not replaying the clip, the fill
      type or the fill parm.
    - Result: 0 differing frames over about 108,000 scenes (six seeds, two
      arena sizes, three canvas sizes).
  - **Retry, step 5:** `tests/rastq/run.sh` now also covers the bands.
    - **Unchanged with a full band.** It builds the 2D and LG libraries
      of the last commit before the bands (`0ca600bf`, with today's row
      table), draws the scenes directly with both, and compares a
      checksum per frame.
    - **Bands on one thread.** Each scene is replayed in 1 to 5 bands at
      random boundaries, band after band, and compared with direct
      drawing. Under the recorder's self-check one band is also drawn
      alone, and must leave the other rows untouched.
    - **Real threads.** The same on three pthreads, through the game's
      own hand-out code, also built with the thread sanitizer on the
      libraries.
    - **Row ranges.** Every eighth scene, each call drawn alone must stay
      inside the rows recorded for it.
    - Scenes now include textures whose sides aren't powers of two in the
      row mappers. That is what found the row-table bug (see
      "Pre-existing engine bugs").
    - It fails on deliberate mistakes: a row, floor or polygon loop
      without its band test; a wall column not stepped over skipped rows;
      a lit wall column or lit perspective scanline without its lighting
      step; a perspective scanline narrowed one column short; one
      temporary memory stack for all threads; the band-safe flag ignored;
      a row range one row short.
    - Coverage, measured with gcov: every band-related line of the
      mappers runs.
    - Result: 0 differing frames on four seeds of 2,100 scenes each, with
      both arena sizes. The sanitizer builds run a tenth of the scenes,
      also without a difference or a report.
  - **Retry, step 6:** the same harness, with what the step added.
    - Scenes replay in 1 to 12 bands, and perspective calls get clip
      rectangles of their own, so a shell that read the canvas's clip
      would show.
    - Blend sprites, which the scenes already had, are now recorded
      doubled and count as band-safe.
    - 1D walls are drawn from the unpack buffer again, compressed or
      unpacked by the caller.
    - While the step compared them, the batch-per-state mode and the strip
      queue were tested alongside the mode that was kept.
    - It fails on deliberate mistakes: a thread's fill type, fill parm or
      function table not updated between calls; a perspective shell or
      `per_umap` reading the canvas instead of its band; a blend sprite
      recorded for the wrong mapper, or copied with its size before
      doubling.
    - Result: 0 differing frames on four seeds of 2,100 scenes each.
- **On-device self-check (profile builds).**
  - First attempt: every ~100 views, replay the view normally, then again
    on one thread, and compare row by row (`check=rows/checks`, always
    `0/N`). Then replay once more with an empty band: it must write
    nothing (`leaks`, always 0), and its time is the work every band
    repeats.
  - Retry, step 4: every 64th recorded view is recorded **and** drawn
    directly. At each flush the directly drawn canvas is kept aside, the
    canvas is rewound to its state at the start of the batch, the batch is
    replayed, and the two are compared row by row. That frame is left out
    of the timings (`vprof_frame_discard`).
  - Result (`docs/profiles-cpu/profile-step-4.txt`): `check=0/400`, 200 checks standing
    still and 200 walking from the medical room to the main hallway.
  - Retry, step 5: the same check, every 63rd view, with the replay now
    split across the three cores. 63 is odd so that, with two views per
    frame, the main view and the help scan take turns. After the
    comparison the canvas is rewound once more and a single band is
    replayed alone; the rows of the other bands must not change
    (`leaks=`). The bands take turns.
  - Result (`docs/profiles-cpu/profile-step-5.txt`): `check=0/311` and `leaks=0`, about
    200 checks standing still and 110 walking. A second session
    (`docs/profiles-cpu/profile-step-5b.txt`) gave `check=0/310` and `leaks=0`.
  - Retry, step 6 (`docs/profiles-cpu/profile-step-6.txt`): `check=0/588` and
    `leaks=0`, spread over the four ways of splitting it compared.

### Pitfalls found, in the order they were hit

1. **Every band repeated the setup work.** The first version skipped
   pixel writes but still ran every row, column and scanline setup, full
   of `fix_div`, on every thread.
   - The three threads' drawing times added up to 1.8× the single-thread
     time at 960×544, and 2.5× at 480×272.
   - The early rejects above plus the FPU `fix_div` brought that to
     ~5%: an empty-band replay costs ~1.5 ms of a ~37 ms one.
2. **A global stored on every division.** `gOVResult`, see above.
3. **Semaphore wake-ups are slow in practice.**
   - The start-up benchmark measured a wake-up through a semaphore at
     32–56 µs, but only because the main thread blocked right after
     signalling.
   - In game, the main thread signals and then draws its own band, and a
     woken worker only started once the main thread finished it. In the
     timeline, workers started at 5.3–5.5 ms, just as the main band ended
     at 5.3 ms.
   - Keeping the workers spinning made them start 0.0–0.2 ms after
     hand-out.
4. **The music emulator took core 1.** Even with spinning workers, one of
   them (usually on core 1) started 5–15 ms late in most frames: the
   Nuked OPL3 music thread was running there. The DOSBox emulator fixed
   it.
5. **Mixed balance.**
   - Different-sized 3D views shared one band split.
   - Frames with many batches (9–38, when the fill state changes often)
     end each batch with its own small imbalance.
   - After the other fixes, the main thread still waited ~6.6 ms per
     frame for the workers at 960×544.
6. **Tools that helped.**
   - Logging the core each thread runs on (`sceKernelGetCpuId`).
   - Per-batch timelines: each worker's start and end, the main band's
     end, all done.
   - A start-up benchmark: a compute loop and random reads over 8 MB on
     1, 2 and 3 cores, plus hand-off latency. It showed at once that the
     cores run in parallel: the 36.3 ms compute loop and the 61–63 ms of
     reads take the same wall time on 1, 2 or 3 threads. It also showed
     hand-off to a spinning worker takes 7–8 µs.

### Results of the first attempt

Within a row, the variants alternated every 5 s in one session, so they
compare. Between rows, the spot in the game differed, so don't compare
across rows. Medians of in-game windows at 960×544:

| session (file) | what changed | variants: fps (replay ms) |
|---|---|---|
| `profile-960x544.txt` | original game | 17.7 (raster 43.3) |
| `profile-960x544-multithread.txt` | first multicore build | multicore 14.6 (replay 50.0) |
| `profile-960x544-profile.txt` | early rejects, FPU `fix_div` | sleep-per-batch 17.0 (38.2), spin-in-replay 19.9 (35.3), workers-only 14.1 (49.3), **one thread 18.8** (36.9) |
| `profile-step3.txt` (960×544 part) | workers stay awake added | sleep-per-batch 20.7 (34.0), spin-in-replay 20.4 (33.2), one thread 18.6 (38.7), **stay-awake 22.3** (30.5) |
| `profile-step4.txt` (960×544 part) | DOSBox music, priority, finish-time balance | one thread 18.3 (41.4), **stay-awake 26.4** (25.0; p25–p75 24.2–30.7 fps) |

- **The best frames.** In round 3 the stay-awake windows where no worker
  was delayed reached 30–33 fps; in round 4 the single-batch windows
  reached 30–35 fps.
- **480×272** reached the 60 fps display cap with the round-4 build.
- **The last build** (one split per view) was never measured.

### Results of the retry

Every row is one session on the Vita at 960×544, full 3D view, standing
still in the first area, with the variants alternating every 5 s. The
one-core figure of each session is its own reference.

| step (file) | what changed | reference | result |
|---|---|---|---|
| 2 (`profile-step-2.txt`) | FPU `fix_div` | 24.2 fps | 27.0 fps (+11.6%) |
| 3 (`profile-step-3b.txt`) | DOSBox music | 27.1 fps, music 52% of a core | 26.8 fps, music 12% |
| 4 (`profile-step-4.txt`) | record + replay on one core | 27.0 fps | 26.8 fps (+0.3 ms) |
| 5 (`profile-step-5.txt`, repeated in `profile-step-5b.txt`) | row bands on three cores | 27.0 fps | 35.3 fps (+31%) |
| 6 (`profile-step-6.txt`) | one hand-out per view | 35.4 fps (step 5) | 44.8 fps (+27%) |

From the step 1 baseline of 24.7 fps that is +81%; from one core after
step 2 (27.0 fps), +66%.

Step 5 in detail (medians; t=32–324; music 11% of a core):

| variant | windows | fps | frame | raster (wall) | batches / frame | main waits |
|---|---|---|---|---|---|---|
| one core, direct | 95 | 27.0 | 37.0 ms | 24.4 ms | – | – |
| three cores, every view split | 95 | 35.0 | 28.6 ms | 15.5 ms | 43 | 6.5 ms |
| three cores, help scan on the main thread | 98 | 35.3 | 28.3 ms | 15.3 ms | 4 | 6.2 ms |

- **+8.4 fps over one core** against neighbouring one-core blocks, in 19
  comparisons (7.9 to 9.0), with the help scan on the main thread; +8.0
  (7.6 to 8.5) with every view split. From the step 1 baseline of
  24.7 fps that is +43%.
- **Identical frames:** `check=0/311`, `leaks=0`.
- **A second session the next day repeated it** (`docs/profiles-cpu/profile-step-5b.txt`,
  t=31–311): 27.1, 35.0 and 35.3 fps for the three variants, +8.2 fps
  (7.9 to 8.8 in 18 comparisons) with the help scan on the main thread,
  the same 6.2 ms of waiting, and `check=0/310`, `leaks=0`.
- **The help scan isn't worth splitting.** Split, it is 39 batches for a
  181-row view, 0.4 fps slower (0.0 to 1.2 in 19 comparisons) and
  unsteady: its worst time in a window is typically 6 ms, and up to 12,
  instead of 3.7 ms.
  Normal builds therefore keep views under 272 rows on the main thread
  (`RASTQ_SMALL_VIEW_ROWS`).
- **What is left on the table.**
  - *The main thread waits 6.2 ms per frame.* Each thread draws for about
    8.7 ms, so the raster could take about 9 ms instead of 15. The main
    view is 4 batches, and each ends when its slowest thread does, while
    the split is balanced for the view as a whole.
  - *Workers sometimes start late:* in most seconds the worst batch
    started about 5 ms late, and up to 7. The music thread was seen on
    cores 1 and 2 only, which are the workers' cores.
  - *One recorded call per frame is drawn by the main thread alone,*
    because its mapper isn't band-aware. It is one of the things that cut
    the main view into batches.
  - *The threads' drawing adds up to 26.2 ms* against 24.4 ms on one
    core: 7% of work repeated in every band.
  - *The help scan costs 3.5 ms per frame,* 9% of the one-core frame.
- **Walking** to the main hallway: 25–42 fps on three cores against 14–28
  on one. The scene differs from block to block there, so this only shows
  the gain holds up.
- **In the game:** "Multicore" in Vita Options, on by default. Off is the
  one-core drawing, call by call, as before.

Step 6 in detail (medians; t=31–363; music 11% of a core). All four
variants draw on three cores; the first is step 5 as it was committed:

| variant | windows | fps | frame | raster (wall) | main waits | threads' drawing, summed |
|---|---|---|---|---|---|---|
| fixed bands, a batch per drawing state (step 5) | 80 | 35.4 | 28.3 ms | 15.4 ms | 6.2 ms | 26.0 ms |
| fixed bands, one hand-out per run | 81 | 44.8 | 22.3 ms | 9.6 ms | 0.8 ms | 25.2 ms |
| 6 row strips taken from a queue | 85 | 41.5 | 24.1 ms | 11.2 ms | 2.3 ms | 27.2 ms |
| 12 row strips taken from a queue | 83 | 40.4 | 24.7 ms | 11.9 ms | 1.8 ms | 29.9 ms |

- **+9.4 fps over step 5** for the fixed bands with one hand-out, against
  the nearest step 5 blocks in 17 comparisons; +6.0 for 6 strips and +5.1
  for 12. Its one weak block was its very first (32.9 fps), before the
  bands had balanced.
- **Identical frames:** `check=0/588`, `leaks=0`.
- **What made the difference.** Each thread now carries its own drawing
  state, and the blend sprite is prepared when it is recorded, so the
  main view is one batch instead of four. The finish-time balance then
  does its job: the main thread's wait falls from 6.2 to 0.8 ms.
- **Why the strip queue lost.** In the queue variants the view is cut
  into 6 or 12 equal strips and each thread takes the next free one. That
  also removes most of the wait, and needs no balancing, but every strip
  repeats part of each call's set-up: the threads' drawing adds up to
  27.2 ms with 6 strips and 29.9 ms with 12, against 25.2 ms with three
  bands. The queue and the batch-per-state path were removed after this
  capture; the code keeps the fixed bands only.
- **Walking** to the main hallway (79–80 windows per variant; the scenes
  differ, so only the waiting compares): the kept variant waits 1.8 ms at
  the median and 3.2 ms at worst, against 4.3 ms and 12.9 ms for step 5.
  The balance follows a changing scene well enough.
- **What is left.**
  - *One frame a second is about 6 ms longer:* the worst frame of a
    window is 28 ms against 22 ms on average. In most seconds a worker
    still starts about 4 ms late once, with the music thread on its core.
  - *The help scan costs 3.6 ms per frame,* now 16% of the frame.
  - *The threads' drawing adds up to 25.2 ms* against about 24.3 ms on
    one core: 4% of work repeated in the bands.
- **For players:** switching On-Screen Help off in the game's options
  skips the help scan, which is worth about 3.6 ms per frame here.

### Recommended order for the retry

1. ~~**Profiler and baseline.** 960×544, standing still in the first
   area.~~ **Done** — see "Baseline" above and `docs/profiles-cpu/profile-step-1.txt`.
2. ~~**FPU `fix_div`**, measured alone (bit-exactness test first).~~
   **Done**: +11.6% fps — see "FPU `fix_div`" above and
   `docs/profiles-cpu/profile-step-2.txt`.
3. ~~**DOSBox music** on Vita, measured alone.~~ **Done**: about 40% of
   a core freed, fps unchanged — see "Music: DOSBox OPL3 instead of
   Nuked" above, `docs/profiles-cpu/profile-step-3.txt` and
   `docs/profiles-cpu/profile-step-3b.txt`.
4. ~~**Recording and single-thread replay only.**~~ **Done**: identical
   frames (`check=0/400` on device) at +0.3 ms per frame with terrain
   textures used in place — see "Record, then replay" above and
   `docs/profiles-cpu/profile-step-4.txt`.
5. ~~**Bands and workers, straight in the final form.**~~ **Done**:
   +31% fps (27.0 to 35.3) with identical frames (`check=0/311`,
   `leaks=0` on device) — see "Row bands, bit-exact", "Threads",
   "Results of the retry", `docs/profiles-cpu/profile-step-5.txt` and
   `docs/profiles-cpu/profile-step-5b.txt`.
6. ~~**Then look at the remaining main-thread wait.**~~ **Done**: +27%
   fps over step 5 (35.4 to 44.8) with identical frames (`check=0/588`
   on device), by handing a view out once instead of in a batch per
   drawing state — see "Results of the retry" and
   `docs/profiles-cpu/profile-step-6.txt`. Row strips taken from a queue were tried
   and were slower.

The retry's order ends here. Ideas it leaves open, none of them planned:

- **The late worker.** About once a second a worker starts about 4 ms
  late. The music thread runs on cores 1 and 2, which are the workers'.
  Keeping it off the core of a busy worker might remove the long frame.
- **The on-screen help scan** redraws the scene every frame (3.6 ms). Its
  code has a 4-per-second timer that is never updated, upstream as well,
  so every frame is how the game has always behaved. Restoring the timer
  would make the help label and its pointer lag behind objects while
  turning; it was left alone for that reason.
- **Core 3** is reserved for the system. The stock system gives a game's
  threads cores 0 to 2 only.

### Ranked recommendations

1. **Multicore software rasterizer, redone as above.** Done in the
   retry's steps 5 and 6: 27.0 to 44.8 fps at 960×544 (+66%), bit-exact.
   The first attempt's last session measured +49%.
2. **Speed-ups that don't need threads.** The FPU `fix_div` and the
   DOSBox music emulator are done.
3. **Frame-pacing fix.** Decouple `SDLDraw()`/`mainloop.c` from vsync's
   default blocking swap, for example with `vita2d_set_vblank_wait(0)`
   plus explicit pacing, or with `sceDisplayWaitVblankStartMulti()`. It
   is small and safe, and removes judder within whatever throughput the
   above reaches.
4. **GPU offload.** Under way as its own feature, on a different route
   than wiring `OpenGL.cc` to vitaGL: see Part 2.
5. **Do not touch AI frame-coupling** (`newai.c`). It is
   original/upstream behaviour, and changing it would diverge from the
   original game's feel.

## Part 2: the GPU renderer


The multicore work (Part 1) took the Vita build from
27.0 to 44.8 fps at 960×544 by filling the 3D view's pixels on three
cores. This part follows the next step: having the GPU fill them.

Captures for this work are in `docs/profiles-gpu/`, named
`profile-step-N.txt`; frame dumps are under `docs/profiles-gpu/dumps/`.

### Starting point

Standing still in the first area, full 3D view, 960×544, three cores
(`docs/profiles-cpu/profile-step-6.txt`):

| part of the frame | time |
|---|---|
| whole frame | 22.3 ms (44.8 fps) |
| pixel filling, wall time on three cores | 9.6 ms |
| the rest of drawing the 3D views (traversal, recording, the help scan) | 2.9 ms |
| stars, HUD overlay and copy of the view to the screen (`sendview`) | 3.4 ms |
| showing the frame (`present`) | 3.2 ms |
| game simulation and input | 0.8 ms |
| everything else | 2.4 ms |

If the GPU took over most of the 9.6 ms the frame would be about 13 ms,
which is the display's 60 fps limit, with cores 1 and 2 left idle. That
is an estimate; nothing here is measured yet.

### Decisions

- **Same look as the software renderer.** 8-bit palette, the game's own
  light tables, sharp texels. A GPU fills polygons by slightly different
  rules, so the result can't be pixel-identical, but colours and shading
  are the same data.
- **The CPU renderers stay, selectable**, as the reference and as the
  fallback for anything the GPU path doesn't draw.
- **Shaders are compiled on the Vita, each time the game starts**, with
  Sony's compiler module (`ur0:data/libshacccg.suprx`, through
  vitaShaRK). The module is a requirement of the GPU renderer; no
  compiled programs are shipped. (The first plan was to ship them at the
  end; dropped at step G6.) Without the module the game runs on its CPU
  renderers.
- **The GPU renderer is the default** (step G6), with "3 cores" and
  "1 core" selectable in Vita Options. (The setting has been removed
  since: the game uses the GPU, or three cores when it can't.)

### The approach

- **The recorder is the GPU's draw list.** `src/Libraries/3D/Source/rastq.c`
  already turns a view into a list of 2D polygons in drawing order, with
  their textures and drawing state. The CPU paths fill that list on one
  or three threads; the GPU path sends it to the GPU.
- **The GPU draws palette indices.** Its render target is an 8-bit
  surface (`SCE_GXM_COLOR_FORMAT_U8_R`) laid over the view's own canvas.
  Textures stay 8-bit, and lighting is a lookup in the game's light table
  (`grd_screen->ltab`), held as a second texture. What lands in the
  canvas is the same kind of data the software mappers write, so the HUD
  overlay, the stars, palette effects and the final palette-to-screen
  step (already done by the GPU through vita2d) are untouched.
- **No depth buffer.** The list is in back-to-front order, as the
  software renderer needs it.
- **On top of what is linked already.** vita2d owns the GXM context and
  shader patcher (`vita2d_get_context`, `vita2d_get_shader_patcher`), and
  SDL2's Vita build already pulls vitaShaRK into the binary.

#### Why not the existing OpenGL renderer

`src/MacSrc/OpenGL.cc` was the PC port's GPU renderer, and vitaGL could
have run something like it. It was not chosen, and has since been removed
from the tree (2026-10-08):

- It draws in true colour with its own lighting (a brightness factor per
  vertex), so it looks like the PC's OpenGL mode, not like the software
  renderer.
- It takes over before projection, with 3D points, so it can't share the
  recorder or its checks.
- It leaves cyberspace, the help scan and the security cameras to the
  CPU.
- vitaGL and vita2d each want to own the GPU context.

### Roadmap

| step | what | what it proves |
|---|---|---|
| G1 | Feasibility: GPU set-up, a shader compiled on the Vita, the list drawn as flat-coloured polygons into an 8-bit canvas | **done**: the pieces work, coverage matches, a view costs about 1.4 ms |
| G2 | Buffers: keep the CPU out of the memory the GPU draws into, and show the GPU's canvas without copying it | **done**: a GPU frame is faster overall in flat colours (45 fps → the screen's 60), with two faults to fix |
| G3 | G2's two faults (the canvas shown while it is wiped; polygons the CPU skips), then textures, light-table and CLUT lookups, perspective, sprites, transparency | **done**: textures and geometry are right; light bands are angular where the CPU's are round; a textured scene costs the GPU 5.5 ms, so the frame is no faster than three cores yet |
| G4 | Light interpolated the way the mappers do it; the shader's cost; which calls still go to the CPU and split the frame into scenes | **done**: round shadows again, 95% of pixels identical; shader B takes 3.2 ms a scene against 6.0; 53 fps standing still against 44.5 on three cores |
| G5 | Light worked out per pixel in place of slabs; the colour-table fill type on the GPU, so that a frame is one scene; shader A dropped | **done**: shadow edges as fine as the CPU's; the screen's rate at the test spot (a 14.6 ms frame against 22.4 on three cores) and while walking (62.5 fps against 36.3) |
| G6 | The GPU renderer in the normal build behind a "Renderer" setting, default on; the paneled view; the screen buffer when the view stops being drawn; cyberspace and low resolution left to the CPU | **done**: played through both views, pause, panels, video mail and cyberspace without a glitch; 62.9 fps against 35.9 while walking and fighting |
| G7 | Holding the screen's rate: the help scan's repeated questions, the queue told per view whether it is the GPU's, and measurements of where a GPU frame's CPU time, `sendview` and the input stalls go | **done**: the door is back at the screen's rate; the stalls are sound effects decoded for their first use; what is left under 60 fps (58.5 at one angle) is the HUD and the help scan on the CPU, next to 3.4 ms of the main thread waiting for the GPU |
| G8 | The help scan run while the GPU draws, in place of the main thread waiting; the HUD measured part by part | **done**: where the scan runs the main thread waits 0.4 ms for the GPU in place of 3.4; G7's heavy angle and the paneled view with labels are at the screen's rate with 1.8 ms to spare. A heavier angle (132 polygons in 354 pieces, no scan) is at 53 to 57 fps |
| G9 | A cheaper HUD: outlined text drawn once and copied; the vitals' arrows and the inventory's buttons stretched once and copied; transparent copies four pixels at a time | **done**: the full-screen HUD from 2.2-3.3 ms to 1.35-1.9, from 4.4 to 1.5 with a help label, on every renderer, no pixel changed in 438 checks. The hallway angle is at 59 fps in full screen, 55 paneled |
| G10 | Cyberspace on the GPU: polygons shaded between colours, lines and points; flat polygons through the flat shaders | **done**: one renderer for all of the 3D world; 0.03% of cyberspace's pixels differ from the CPU's; 2 ms a frame ahead of three cores there, the view going to the screen without a copy. Voxel objects still split a frame into several scenes |
| G11 | The sending shared between the three cores: deciding what each call is and cutting it up, the hand-over staying in order | **done**: the workers take a third of the calls each; the sending at the hallway angle from 5.1 ms to 3.35; that angle at 62.3 fps in full screen (59.0) and 60.2 paneled (55.3), the paneled view with nothing to spare |
| G12 | The paneled view shown from GPU memory, laid over the screen's picture, in place of copied into it | **done**: the view's way to the screen from 2.9 ms to 0.09; the hallway angle paneled at 62.8 fps with 2.4 ms to spare (60.2 fps and 0.1 ms), ahead of three cores again |

G2 was not in the first roadmap. G1 showed that the memory question
decides whether the GPU path is worth anything, so it comes before
textures.

Open questions, and the step that answers each:

- ~~Does an 8-bit render target give exact palette indices?~~ Yes (G1).
- ~~How fast can the CPU read and write memory the GPU renders into?~~
  Writing is 3 times slower than in ordinary memory, reading 4 times
  (G1). G2 is about that.
- ~~How long does the GPU take per view, and what does waiting for it
  cost?~~ About 1.4 ms for the main view in flat colours (G1).
- ~~Is a GPU frame faster overall once the CPU stays out of GPU
  memory?~~ Yes, in flat colours: it reaches the screen's refresh rate
  where three cores give 45 fps (G2).
- ~~What do textures and the table lookups cost the GPU?~~ 5.5 ms for a
  full-screen scene, against 1.3 ms in flat colours, whatever the number
  of polygons: it is the shader's work per pixel (G3). G4 is about that.
- Can a shader read the pixel already drawn? Translucent surfaces need
  it; if not, those calls fall back to the CPU. (G4)

### How it is checked

GPU code only runs on the Vita, and its output isn't identical to the
CPU's, so the multicore work's "0 differing rows" can't be the test.
Instead the profile build:

- compares, every 63rd view, the GPU's result with the CPU's rendering
  of the same list (since G3 the real one, with textures), and logs the
  share of pixels that differ;
- writes some of those pairs to `ux0:data/systemshock/gpudumps/`, to be
  looked at on a PC: the third comparison, and each later one that
  differs by more pixels than any before it and by more than 5% of them,
  five in all. Writing a pair stops the game for a second or two;
- draws known patterns at start-up and reads them back (`gpu.txt`);
- logs the time spent sending the list and waiting for the GPU.

The PC harness (`tests/rastq/run.sh`) keeps guarding the recorder and the
CPU paths. Since G3 it also checks what the queue hands the GPU, with a
stand-in that fills polygons in the shader's arithmetic: see step G3.

### Step G1: feasibility

Profile builds only; the normal build has no GPU code.

#### What was built

- **`src/MacSrc/VitaGpu.c`**, the GPU module.
  - At start-up it loads the shader compiler, compiles one vertex and one
    fragment program from Cg source in the file, and registers them with
    vita2d's shader patcher. Each stage is written to
    `ux0:data/systemshock/gpu.txt` before it starts, so a crash shows
    where it happened.
  - `vgpu_alloc` hands out CDRAM mapped for the GPU. The main 3D view
    draws into the pixels of an SDL surface, `offscreenDrawSurface`
    (`SetupOffscreenBitmaps` in `src/MacSrc/ShockBitmap.c`); in profile
    builds those pixels come from `vgpu_alloc`. The GPU is therefore set
    up before that surface is first created.
  - A render target is made over a canvas the first time a scene is
    begun on it: an 8-bit colour surface with the canvas's own stride,
    and a depth buffer that is never tested.
  - A scene is one indexed triangle list. Polygons are fanned into
    triangles on the CPU, with positions already in clip space.
  - After `sceGxmEndScene` it calls `sceGxmFinish`, because the CPU goes
    on to draw the stars and the HUD into the same canvas.
- **The recorder's GPU path** (`rastq.c`): `rastq_set_gpu` takes a set of
  hooks (`begin`, `flat_poly`, `end`, `compared`), and a view that goes
  to the GPU is flushed through them instead of being replayed. If
  `begin` refuses, the CPU replays as before.
- **Flat colours for now.** Each call is drawn as one polygon in one
  palette index: a polygon's own colour, or the texel in the middle of a
  bitmap. Vertices are moved by half a pixel, because the software
  mappers fill the pixels whose integer coordinates are inside a polygon
  and a GPU those whose centres are.
- **The index sent is `(index + 0.25) / 255`,** which gives the index
  whether the 8-bit target rounds or truncates.

#### What the capture will show

- **In `gpu.txt` and the first lines of the log:**
  - whether the shaders compiled;
  - `index_check mismatches=N/256`: one column per palette index, drawn
    and read back;
  - `undrawn_kept=`: whether a scene keeps the pixels it doesn't draw,
    which a view drawn in several scenes needs;
  - how long the CPU takes to write and to read 512 KB of GPU memory and
    of ordinary memory.
- **Per window, when variant 1 (GPU) is active:**
  - `gpusubmit=` and `gpuwait=`: time per frame to send the list, and to
    wait for the GPU;
  - `gpufallbacks=`: lists the GPU refused;
  - `gpudiff=a/b`: over the comparisons so far, `a` pixels of `b`
    differed between the GPU's flat fill and the CPU's.
- **Variant 0 (three cores) now draws into GPU memory too,** so its fps
  against step 6's 44.8 says what that memory costs the CPU.
- **Frame pairs** in `gpudumps/`: the GPU's and the CPU's flat rendering
  of the same view, with the palette.

The PC harness exercises the recorder's GPU path with a stand-in that
fills the flat polygons on the CPU; its comparison must then find no
difference.

#### First run (`docs/profiles-gpu/profile-step-1.txt`, `gpu.txt`)

The set-up worked; no game view reached the GPU.

- **Shaders compile on the Vita:** a 284-byte vertex program and a
  228-byte fragment program.
- **The 8-bit target stores exact indices:**
  `index_check mismatches=0/256`.
- **A scene keeps the pixels it doesn't draw** (`undrawn_kept=1`).
- **The CPU writes GPU memory about 3 times slower** than ordinary
  memory: 737 µs against 257 µs for 512 KB. The read timing of this run
  is void: the compiler had removed the loop.
- **Every view was refused.** `gpufallbacks` reached 7084 and
  `gpuscenes` stayed 0, so both variants were the three-core replay
  (44.8 and 45.1 fps). The GPU module only draws into canvases in GPU
  memory, and this build allocated that memory in `fr_place_view`, where
  the main view never gets its canvas: it is handed the pixels of
  `offscreenDrawSurface`. The second build allocates those instead, and
  logs a refusal with its reason.

#### Second run (`docs/profiles-gpu/profile-step-1b.txt`, `gpudumps-step-1b/`)

The GPU drew the main view, in flat colours, in the GPU variant.
Standing still in the first area, medians of 203 and 205 windows:

| | three cores | GPU |
|---|---|---|
| fps | 19.5 | 28.4 |
| frame | 51.2 ms | 35.3 ms |
| pixel filling (`raster`) | 17.8 ms | 1.8 ms |
| of which sending the list / waiting for the GPU | – | 0.09 / 1.28 ms |
| stars, HUD, copy to the screen (`sendview`) | 8.1 ms | 8.1 ms |
| help scan | 18.8 ms | 19.0 ms |

- **The GPU is fast.** 87 flat polygons covering the 960×544 view cost
  0.09 ms to send and 1.28 ms to wait for. Three cores take 9.5 ms in
  ordinary memory.
- **Coverage matches the CPU's.** With the half-pixel shift, 16 pixels of
  522,240 differ between the GPU's flat fill and the CPU's, all on
  polygon edges (`gpudumps-step-1b/01` to `03`). No list was refused.
- **Everything the CPU does in the canvas got slower**, in both variants,
  because this build put the main view's canvas in GPU memory (CDRAM)
  for both. The start-up timing shows why: 512 KB takes 0.71 ms to write
  and 4.9 ms to read there, against 0.25 ms and 1.2 ms in ordinary
  memory.
  - *Three-core filling:* 17.8 ms instead of 9.5.
  - *`sendview`:* 8.1 ms instead of 3.2. It ends with a copy of the whole
    canvas to the screen buffer, which reads all of it.
  - *The help scan:* 18.8 ms instead of 3.5. `olh_init_single_scan`
    gives the scan the main view's own pixels as its canvas (it draws in
    the top-left corner, after the main view has been sent), so it was
    rasterizing on one core in GPU memory.
- **So the GPU frame was slower than today's 44.8 fps,** although its
  pixel filling is five times faster.
- **Still open:**
  - While walking, a few comparisons differed by tens of thousands of
    pixels (the running total jumps from 6,775 to 53,965 within a few
    checks). None of the five dumped pairs is one of them; G2 dumps
    exactly those.
  - The dumps were written every 30th comparison in this build, and each
    one froze the game for about two seconds.

#### Go for G2

The approach holds: exact indices, matching coverage, a cheap GPU. What
has to change is where the pixels live.

### Step G2: keep the CPU out of GPU memory

Profile builds only.

#### What was built

- **Two canvases.** The view keeps its ordinary-memory canvas
  (`offscreenDrawSurface`, as upstream). The GPU has its own: a second
  vita2d texture, 8-bit paletted like the screen's and given the same
  palette (`MakeViewTexture` in `src/MacSrc/Shock.c`).
- **A view picks its canvas per frame.** `fr_start_view`
  (`src/GameSrc/frsetup.c`) points the main view's draw canvas at the
  GPU's when `rastq_gpu_next()` says the frame goes to the GPU, and back
  at its own memory otherwise. Stars, HUD overlay and cursor follow the
  draw canvas. The help scan, the security cameras and the 360 view
  never use the GPU canvas, so the scan is back in ordinary memory.
- **Present from the canvas.** In the full-screen 3D view, `fr_send_view`
  asks `VitaShowView` before copying the view to the screen buffer. When
  it agrees, the copy is skipped and the next `SDLDraw` draws the GPU
  canvas texture instead of the screen texture, which also skips the copy
  of the screen buffer to its texture. Anything drawn straight to the
  screen buffer during such a frame doesn't show; the paneled view keeps
  the copy.
- **Variants:** 0 = three cores, 1 = GPU with the view copied to the
  screen as before, 2 = GPU with a full-screen view shown from its
  canvas.

#### Results (`docs/profiles-gpu/profile-step-2.txt`, `gpudumps-step-2/`)

Standing still in the first area, 960x544, medians of the 1 s windows.
The GPU variants are in flat colours.

| ms per frame | 0: three cores | 1: GPU, view copied | 2: GPU, shown from its canvas |
|---|---|---|---|
| pixel filling (`raster`) | 9.49 | 1.73 | 1.72 |
| of which waiting for the GPU | | 1.29 | 1.28 |
| 3D pass (`traverse`) | 12.32 | 4.50 | 4.40 |
| stars, HUD, copy to screen (`sendview`) | 3.19 | 7.71 | 2.30 |
| help scan | 3.53 | 3.54 | 3.49 |
| `present` | 3.17 | 3.02 | 5.04 |
| whole frame | 22.21 | 19.35 | 15.92 |
| fps | 45.0 | 51.7 | 62.8 |

- **The memory fix worked.** The three-core path and the help scan are
  back at their step-6 speed: neither touches GPU memory any more.
- **Copying the view out of GPU memory costs what the GPU saves.**
  Variant 1's `sendview` is 4.5 ms longer than the three cores': the
  copy to the screen buffer reads the whole canvas, in the slow
  direction. Showing the canvas itself (variant 2) removes it.
- **Variant 2 is at the screen's refresh rate.** The menus, which do no
  work, log 61 fps with about 15 ms of every frame spent in `present`:
  that is the display's 60 Hz by the profiler's clock. Variant 2's
  `present` is 2 ms longer than the others' although it copies less,
  which is the same wait. Its real work is about 11 to 13 ms a frame, so
  62.8 fps is a floor for the flat-coloured GPU path, not its speed. The
  next capture logs the wait on its own.
- **Exactness unchanged**: `check=0/377`, `leaks=0`.
- **`gpufallbacks=136`**: the paneled (non-full-screen) view, whose
  804-pixel-wide canvas the GPU module refuses (`gpu.txt`). Expected;
  that view is on the roadmap for G6.

Two faults:

- **Variant 2 glitches and flickers** (seen on the Vita: blocks of the
  picture black, HUD text cut where the blocks meet). Every view starts
  by wiping its canvas on the CPU (`gr_clear` in `fr_start_view`,
  `src/GameSrc/frsetup.c`). vita2d draws the screen a moment after
  `SDLDraw` asks for it, tile by tile, from the canvas texture; the next
  frame's wipe lands in the middle of that. Variant 1 is immune: it
  copies the finished view before the next frame starts. So a canvas
  that is on its way to the screen must not be written, by the wipe or
  by the next frame's drawing.
- **The GPU draws polygons the CPU skips.** Standing still, 47 pixels of
  522,240 differ per comparison. While walking, some comparisons differ
  by thousands (dumps 02 to 05: 2,709 to 15,800 pixels): a whole
  polygon present on the GPU's side only. The software fillers stop at
  the first row whose right edge is left of its left edge, which for a
  polygon wound the other way round is the first row; the GPU scene has
  culling off and draws both windings.

#### Go for G3

A GPU frame is faster overall once nothing on the CPU reads or wipes the
GPU's memory. G3 starts with the two faults:

- three GPU canvases used in turn, as vita2d does with the screen's own
  buffers, so that a canvas is reused only after two newer frames have
  been handed to the screen; and the wipe done by the GPU, as the first
  rectangle of its scene;
- polygons wound the way the software fillers reject are dropped before
  they reach the GPU.

Then textures: every software mapper ends by picking a texel and
optionally passing it through a 256-entry table (a CLUT, a row of the
light table, a solid colour). One shader can do that with the call's
bitmap as one texture and the tables as rows of a second one.

### Step G3: two fixes, then textures

Profile builds only.

#### The two fixes

- **Three GPU canvases, used in turn** (`MakeViewTextures` in
  `src/MacSrc/Shock.c`, `vgpu_canvas` in `src/MacSrc/VitaGpu.c`). Each
  frame takes the canvas used longest ago. vita2d lets at most two
  frames wait for the screen, so by the time a canvas comes round again
  two newer frames have been handed over and its own has been drawn. One
  render target serves the three; each scene gets a colour surface over
  the canvas it draws into.
- **The clear is the GPU's.** `fr_start_view` asks `rastq_gpu_clear`
  instead of clearing; the clear becomes the first rectangle of the
  view's first scene. If the GPU doesn't take the view after all, or a
  call has to be drawn straight to the canvas first, the queue clears on
  the CPU.
- **Winding.** A call wound anticlockwise on screen (`gpu_reversed` in
  `rastq.c`) is dropped before it reaches the GPU: the mappers draw
  nothing for it.

#### Textures

What a mapper does for a pixel, and what the shader does:

| mapper | texel | then |
|---|---|---|
| plain | `bits[...]` | as is |
| colour table | `bits[...]` | `clut[texel]` |
| lit | `bits[...]` | `ltab[texel + 256 * light level]` |
| transparent | any of the above | skipped when the texel is 0 |

- **Tables.** The scene's tables are one texture, 256 wide: rows 0 to 15
  are the light table (`grd_screen->ltab`), row 16 leaves a texel as it
  is, the rest are the colour tables its calls use (a table inside the
  light table, as the 3D library builds for evenly lit polygons, is one
  of rows 0 to 15). The shader looks the texel up in the call's bitmap,
  then (texel, row) in the tables.
- **Texel addressing**, as the mappers have it and not as a GPU would by
  itself (`rastq.h`):
  - the row, floor and wall mappers read
    `bits[(v * width + u) mod (width * height)]`: u beyond the width
    moves on to the next row. The shader carries u over the same way;
  - the perspective mapper wraps u and v each on their own;
  - sprites, the only bitmaps whose sides aren't powers of two, don't
    wrap: clamped.
- **Perspective.** Vertices carry (u·q, v·q, q) and the shader divides.
  q is the vertex's w (1/z) for the floor, wall and perspective mappers,
  and 1 for the linear mapper, which ignores w (the 3D library doesn't
  set it then).
- **Light level.** The floor and wall mappers carry it through the
  perspective division with u and v; the perspective mapper steps it
  along its scanlines, and the linear mapper has no division. So the
  table row is `row / q + flat_row`, each mapper using one of the two.
- **Bitmaps** are copied into GPU memory once per scene
  (`texture_for`), as 8-bit textures sampled without filtering.
- **One vertex format and one shader pair** (plus the pair's variant
  that discards texel 0). A flat polygon goes through it too, as a
  "texture map" of the tables texture's unchanged row at its colour, so
  a scene is a run of draws that only change texture.

What the GPU path covers, decided per call by `gpu_classify`:

- texture maps on `BMT_FLAT8` bitmaps with the normal fill type, in the
  families the 3D library asks for (linear, floor, wall, perspective;
  plain, lit, colour table; opaque or transparent), sprites included;
- flat polygons (`FIX_UPOLY`), in the normal, colour-table and solid
  fill types.

Everything else (translucent bitmaps and polygons, shaded polygons, more
than 16 vertices, a light level outside the table, the perspective
mapper under a clip rectangle) is drawn by the CPU, in its place in the
list: the scene under way is ended and waited for, the CPU draws, and
the next GPU call opens a new scene. The picture stays right; the cost
is logged (`gpucpu=calls/ms`, `gpuscenes`).

If the textured shaders don't compile on the Vita, the build draws in
flat colours as in G2 and says so (`textures=off` in the report line).

#### Checked on the PC

`tests/rastq/rastq_test.c` has a stand-in for the GPU: it implements the
queue's GPU interface and fills polygons as triangle fans with the
shader's arithmetic in single precision. Scenes made for it are drawn by
the mappers and by the stand-in, and the two compared.

They can't be equal, for three reasons that are not mistakes:

- the mappers don't sample exactly where a GPU does. The floor and wall
  mappers take the slope of 1/z from spans in whole pixels, which moves
  texel boundaries by a pixel or two in the middle of a long wall; the
  perspective mapper works in 16.16 fixed point with a w of 12 bits or
  so;
- a GPU interpolates a light level over triangles, the mappers along a
  polygon's edges and then across each row. The two agree only when the
  level is one linear function over the whole polygon. The comparison
  scenes make it one; the game's quads often aren't, so on the Vita the
  light bands of a wall will sit a little differently;
- where one pixel covers several texels, the two pick different ones.

So the test counts the pixels of the stand-in whose value the mappers
have neither there nor within two pixels, on scenes with noise-free
textures over a flat background. Over 900 frames:

| | share of pixels |
|---|---|
| differ from the mappers' | 2.32% |
| and have no match within two pixels | 0.157% |
| limit on the second, per run / per frame | 0.5% / 6% |

Flat polygons come out identical to the mappers', pixel for pixel.

Deliberate mistakes in the translation, and the second figure with each:

| mistake | no match within two pixels |
|---|---|
| none | 0.16% |
| no culling / the other winding culled | 5.5% / 44% |
| colour table ignored | 23% |
| light table not sent | 36% |
| light one row off (through the division / not) | 10.9% / 1.5% |
| the two ways of carrying light swapped | 1.0% |
| transparency dropped | 9.9% |
| no perspective / perspective for the linear mapper | 10.4% / 8.2% |
| u and v swapped | 38% |
| u three texels off | 25% |
| u carried over as two separate wraps | 0.71% |
| clear dropped / in the wrong colour | 28% |
| the CPU's calls skipped | 13% |
| two separate wraps as u carried over (perspective mapper) | 0.34%, **under the limit** |
| colour-table fill of a flat polygon not applied | 0.31%, **under the limit** |
| solid fill of a flat polygon not applied | 0.19%, **under the limit** |

The last three move the figure the right way but stay under the limit:
few pixels are concerned, or the shift is within the two pixels. The
queue's own comparison (as on the Vita) is exercised too: it must leave
the mappers' picture on the canvas.

One upstream bug met on the way: the lit floor mapper doesn't step along
a row of a bitmap whose sides aren't powers of two
(`gri_lit_floor_umap_loop`, the `GRL_OPAQUE` case). The game never gets
there (the 3D library refuses such bitmaps for everything but sprites),
and the GPU path leaves those calls to the CPU.

#### What the capture will show

Variants, 5 s each: 0 = three cores, 1 = GPU with the view copied to the
screen, 2 = GPU with the view shown from its canvas.

- `gpu.txt`: the shaders' sizes, the texture units, and
  `texture_check plain= table= light= wrap=`: wrong columns out of 256
  for four patterns drawn with the textured shaders at start-up. All
  four should be 0;
- `gpudiff=`: pixels differing from the CPU's rendering in the periodic
  comparisons. A few percent is expected, for the reasons above;
- `gpuwait=`, `gpusubmit=`: what textures cost the GPU and the sending;
- `gputex=`: KB of bitmaps copied to GPU memory per frame;
- `gpuscenes=`, `gpucpu=calls/ms`: how often the CPU has to step in, and
  what that costs: what G4 and G5 are worth;
- `gpuculled=`: calls dropped for their winding;
- `swapwait=`: ms per frame spent waiting to hand a frame to the screen,
  so that the work in a frame is its time minus this.

#### Results (`docs/profiles-gpu/profile-step-3.txt`, `gpu.txt`, `gpudumps-step-3/`)

What works:

- **The shaders compile on the Vita** (356, 596 and 696 bytes) and the
  start-up patterns come back exact: `texture_check plain=0 table=0
  light=0 wrap=0`. Table lookups through an 8-bit texture are exact for
  all 256 indices.
- **The glitch of G2's third phase is gone**: no black blocks and no
  flicker on the Vita with the three canvases and the GPU's clear.
- **Textures and geometry are right.** In the dumped comparisons, 76 to
  81% of the pixels are identical to the CPU's; of the rest, all but
  0.6 to 1.0% of the frame are the same texel shown one light level
  brighter or darker (found by looking each differing pair up in
  `SHADTABL.DAT`). Edges, texel positions and winding account for that
  last percent.
- `check=0/292`, `leaks=0`; 10 calls a frame culled for their winding.

What doesn't yet:

- **Light bands are angular where the CPU's are round**, and whole
  stretches of wall sit one level off. `gpudiff` is 16.6% standing
  still. The cause is the one named in "Checked on the PC": a quad's
  four corners rarely have light levels that fit one plane, the mappers
  interpolate along the edges and then along each row (or column), and
  the GPU interpolates over the two triangles of the quad. Confirmed on
  the PC by giving the comparison scenes a light level of their own at
  each corner:

  | stand-in against the mappers | no match within two pixels |
  |---|---|
  | light level one linear function per polygon (G3's scenes) | 0.16% |
  | a level of its own at each corner | 3.7% |
  | the same, lit polygons cut into slabs 8 pixels thick along the mapper's scan lines | 0.5% |

  A slab's two long sides are lines the mapper draws (rows for the
  linear and floor mappers, columns for the wall mapper, lines of one
  depth for the perspective mapper), with the values at their ends taken
  along the polygon's edges. Between them the GPU's interpolation has
  little room to differ. What is left of the 0.5% is in floors a few
  rows high whose texture is squeezed several texels to the pixel.
- **Once a second the picture shows the CPU's rendering for one frame**:
  the periodic comparison leaves the CPU's result on the canvas. It
  should put the GPU's back.
- **The GPU path is not faster yet.** Standing still in the first area,
  medians of the 1 s windows:

  | ms per frame | 0: three cores | 1: GPU, view copied | 2: GPU, shown from its canvas | G2's variant 2 (flat) |
  |---|---|---|---|---|
  | pixel filling (`raster`) | 9.57 | 12.23 | 12.10 | 1.72 |
  | of which waiting for the GPU | | 10.80 | 10.71 | 1.28 |
  | of which sending (`gpusubmit`) | | 1.00 | 0.96 | 0.08 |
  | `sendview` | 3.20 | 7.65 | 2.32 | 2.30 |
  | `present` | 3.23 | 3.15 | 1.07 | 5.04 |
  | whole frame | 22.45 | 29.06 | 21.38 | 15.92 |
  | fps | 44.5 | 34.4 | 46.8 | 62.8 |

  - **A textured scene takes the GPU about 5.5 ms**, and the number of
    polygons has nothing to do with it: 5.0 to 6.6 ms in the windows
    with one scene a frame, for 5 polygons as for 123. It is the work
    per pixel: two texture reads at computed coordinates, a division
    and three roundings, against a constant in G2.
  - **Each further scene adds about 2.5 ms**: 5.8 ms with one scene a
    frame, 8.7 with two, 10.75 with three (medians over all windows).
    The test spot has three, because 9 calls a frame are drawn by the
    CPU (`gpucpu=9.0/0.07`): the CPU's own time for them is nothing,
    what costs is ending a scene and starting another.
  - Sending is 1 ms, of which the copy of 158 KB of bitmaps a frame.
  - `swapwait` is 0.08 ms: no variant waits for the screen any more
    (the menus do, 5.8 ms).
  - Variant 1 pays 4.4 ms for copying the view out of GPU memory, as in
    G2. It has told what it could and can go.

#### Go for G4

The look is one fix away and the speed two:

- lit polygons cut into slabs along the mapper's scan lines, as tried
  on the PC;
- a cheaper shader. The GPU can do the perspective division and the
  wrap itself if the vertices carry a real w and the bitmap is read at
  interpolated coordinates; what stays computed is the table lookup.
  The row, floor and wall mappers' carrying of u into the next row is
  then not reproduced (a texture repeated along u would sit one texel
  lower at each repeat on the CPU, not on the GPU);
- fewer scenes: a count of the CPU-drawn calls by kind, to know which
  kinds to move to the GPU first.

### Step G4: light like the CPU's, and a cheaper shader

Profile builds only.

#### Light in slabs

A lit polygon with four or more corners is cut, before it reaches the
GPU, into slabs 8 pixels thick along the lines its mapper draws
(`gpu_emit_slabs` in `src/Libraries/3D/Source/rastq.c`):

| mapper | slabs along |
|---|---|
| linear, floor | rows |
| wall | columns |
| perspective | lines of one depth (equal w) |

"Mapper" is the one that really draws the call. `per_umap` hands a
polygon that is flat enough, or lies like a floor or a wall, to the
linear, floor or wall mapper, and each carries light its own way; the
queue asks it which (`gr_per_umap_family` in
`src/Libraries/2D/Source/permap.c`) and treats the call as one of that
mapper's.

The cut is a convex clip that takes every vertex value along the
polygon's edges (`slab_clip`). A slab's two long sides are then lines
the mapper draws, with the mapper's values at their ends; in between the
GPU has 8 pixels to differ in. A triangle is left whole: three levels
always fit one plane.

Two smaller things:

- a lit level gets 1/256 of a level added, as the wall mapper adds it,
  so that one sitting exactly on a row of the table doesn't fall on
  either side of it from pixel to pixel;
- the periodic comparison puts the GPU's picture back on the canvas
  (it left the CPU's, which showed for one frame a second). When a view
  is drawn in several parts, each part is now compared on top of the
  GPU's earlier parts.

#### The floor and wall mappers' depth

`h_umap` and `v_umap` don't take a floor's or a wall's depth as its
vertices have it. They replace each vertex's w by a straight line across
the rows (or columns), from the w of the first to the w of the last,
over the whole number of rows between them, and the texture and the
light follow that line. For a wall seen at an angle it lies a pixel or
two off the true perspective in the middle, which is the drift G3 put
down to "spans in whole pixels". `gpu_scan_w` computes the same w in the
same fixed-point arithmetic, and the GPU's vertices carry it.

#### Repeats cut on the CPU

The row, floor and wall mappers read `bits[(v * width + u) mod size]`,
so a texture repeated along u sits one texel row lower at each repeat.
G3's shader reproduced that per pixel. It is now done before the GPU:
the polygon (or each slab of it) is cut where u passes a multiple of the
width, a straight line on screen, and each part's v is moved by its
number of repeats (`gpu_emit_repeats`). The GPU then only ever wraps u
and v each on their own, which a texture's repeat mode can do, and the
shader loses that work. Slabs first, repeats within each: the cut
between two repeats runs through the polygon's inside, where only a
slab's long sides have the mapper's light.

#### Shader B

Shader A (G3's, minus the carry) works the texel out per pixel: a
division, the rounding, the wrap, then two texture reads at coordinates
it computed.

Shader B leaves that to the hardware:

- the vertices carry a real w (1/q), so the GPU interpolates u, v and
  the table row with the perspective itself;
- the bitmap is read at the coordinates as they arrive, its texture set
  to repeat (or to clamp, for sprites). Nearest-texel sampling is the
  rounding down;
- the tables texture is read at (texel, row / 64): nearest-texel
  sampling picks the row, so there is no rounding to compute.

What it changes in the picture: the perspective mapper's light level,
which that mapper steps along its scan lines without the perspective
division, goes through the division like everything else. Inside a slab
cut along those very lines the two are the same to well under a pixel.

Both shaders read the same vertex layout, each its own fields
(`vgpu_vertex` in `src/MacSrc/VitaGpu.c`); `fill` writes a vertex for
the shader of the scene under way. The start-up check now draws its four
patterns with each shader. Shader B counts on linear textures repeating:
if any of its columns is wrong, it is switched off and shader A takes
its place (`shader_b=off` in the report line).

#### Checked on the PC

The comparison scenes now have a light level of their own at each
corner, as the game does, and the stand-in has shader B's arithmetic as
a second mode. Over 900 frames per shader:

| | shader A | shader B |
|---|---|---|
| pixels that differ from the mappers' | 2.39% | 2.39% |
| and have no match within two pixels | 0.203% | 0.203% |
| limit on the second, per run | 0.5% | 0.5% |

The same figure step by step, over 100 frames per canvas size (shader
A; B within 0.01 of it each time):

| | no match within two pixels |
|---|---|
| G3's translation on these scenes (no slabs) | 3.7% |
| slabs | 0.39% |
| repeats cut on the CPU, within each slab | 0.32% |
| the dispatcher asked which mapper draws | 0.28% |
| the floor and wall mappers' depth | 0.20% |
| G3's figure, on scenes whose light fits a plane | 0.16% |

Deliberate mistakes, and the second figure with each (60 frames;
0.18% without any):

| mistake | no match within two pixels |
|---|---|
| no slabs | 2.7% |
| a cut vertex's light, u or position not interpolated | 6.6%, 9.9%, 12.9% |
| the second slab of every polygon dropped | 0.76% |
| row slabs cut along columns | 0.64% |
| repeats not cut / v not moved / moved the other way | 0.75% / 0.70% / 1.7% |
| the two ways of carrying light swapped | 0.89% |
| no culling, colour table ignored, transparency dropped, no perspective, light one row up | 5.6% to 25% |
| the comparison leaving the CPU's picture | caught by the test of the comparison itself |
| wall slabs cut along rows | 0.34%, **under the limit** |
| perspective slabs cut along rows | 0.30%, **under the limit** |
| the floor and wall mappers' depth not used | 0.24%, **under the limit** |
| the dispatcher not asked | 0.19%, **under the limit** |

The last four move the figure the right way but not past the limit: with
slabs this thin their direction matters little for those two mappers,
and the last two concern few polygons, though each of those is then
visibly off (one frame of the 900 was at 17% before them).

#### What the capture will show

Variants, 5 s each, both GPU ones shown from the canvases: 0 = three
cores, 1 = GPU with shader A, 2 = GPU with shader B.

- `gpu.txt`: `shader_a=` and `shader_b=` with the wrong columns of each
  one's four patterns; all eight figures should be 0;
- `gpudiff=`: 16.6% in G3; a few percent expected with slabs;
- `gpuwait=` in variant 1 against G3's 10.7 ms: what slabs cost (more
  triangles) and what dropping the carry saves; in variant 2 against
  variant 1: what shader B saves;
- `gpukinds=flat:,plain:,clut:,lit:` and `gpuslabs=`: the GPU's calls
  per frame by kind, and the pieces lit ones were cut into;
- `gpuwhy=...`: the CPU-drawn calls per frame by reason, which is what
  splits a frame into scenes and what G5 has to move to the GPU.

#### Results (`docs/profiles-gpu/profile-step-4.txt`, `gpu.txt`, `gpudumps-step-4/`)

Standing still in the first area, medians of the 1 s windows:

| ms per frame | 0: three cores | 1: GPU, shader A | 2: GPU, shader B | G3's variant 2 |
|---|---|---|---|---|
| pixel filling (`raster`) | 9.54 | 13.88 | 9.54 | 12.10 |
| of which waiting for the GPU | | 9.78 | 5.54 | 10.71 |
| of which sending (`gpusubmit`) | | 3.68 | 3.58 | 0.96 |
| `sendview` | 3.25 | 2.36 | 2.35 | 2.32 |
| `present` | 3.21 | 1.04 | 1.03 | 1.07 |
| whole frame | 22.45 | 23.14 | 18.78 | 21.38 |
| fps | 44.5 | 43.2 | 53.3 | 46.8 |

While walking (medians over the walk): 37.3 fps on three cores, 55.8
with shader A, 61.5 with shader B, which is the screen's rate.

- **Both shaders compile and their start-up patterns are exact**
  (`shader_a=on ... shader_b=on`, all eight counts 0). Linear textures
  do repeat.
- **Shader B is the cheaper by a wide margin.** Waiting for the GPU,
  over all windows, by scenes per frame:

  | scenes a frame | shader A | shader B |
  |---|---|---|
  | 1 | 5.97 ms | 3.15 ms |
  | 2 | 8.03 ms | 4.38 ms |
  | 3 | 9.78 ms | 5.54 ms |

  A scene costs 3.2 ms with B, a further one 1.2 ms.
- **The light is right in shape.** Round bands again on the Vita, and
  the once-a-second frame is gone. In the dumped comparison at the test
  spot 95.1% of the pixels are identical to the CPU's (81% in G3);
  `gpudiff` standing still is 4.7% (16.6% in G3). What differs is the
  same texel one light level off, along the edges of the bands.
- **But the bands' edges are coarser than the CPU's.** Inside a slab
  the GPU still interpolates over two triangles, so an edge moves in
  steps of the slab's 8 pixels where the mappers move it pixel by
  pixel.
- **Slabs cost the CPU.** 499 slabs a frame at the test spot, and
  sending went from 0.96 to 3.6 ms. Nearly everything is lit:
  `gpukinds=flat:1,plain:0,clut:2,lit:65`.
- **One reason for every CPU-drawn call**: `gpuwhy=...fill:9.0`, and
  over the whole run 7.4 a frame, all `fill`. No translucent bitmap,
  shaded or translucent polygon came up. Those calls are texture maps
  under `FILL_CLUT`, which the game sets for lit 3D objects
  (`gameobj.c`, `interp.c`). Under that fill type every mapper is its
  colour-table one with the fill's table (`fl8ft.c`), so they are calls
  the GPU path already knows how to draw.
- `check=0/360`, `leaks=0`; `gpufallbacks=69` is the paneled view.

#### Go for G5

- **Light per pixel in place of slabs.** A mapper's light at a pixel is
  the value on the left edge at that row, the value on the right edge,
  and how far along the row the pixel is, divided by the row's depth
  for the floor and wall mappers. Left value, right minus left, distance
  from the left edge, row width and row depth are each a linear function
  of the screen position between two corner levels of the polygon, and a
  GPU interpolates those exactly. The shader then computes
  `(left + (right - left) * distance / width) / depth`. A polygon is
  cut only at its corners' levels, a few pieces in place of its slabs.
- **`FILL_CLUT` on the GPU**: one scene a frame.
- **Shader A goes.**

### Step G5: light per pixel, one scene a frame

Profile builds only.

#### Light per pixel

What the queue hands the GPU for a vertex's table row is no longer a
row but five values (`rastq_gpu_vertex` in
`src/Libraries/3D/Source/rastq.h`), each a linear function of the screen
position, and the row is

    (left + span * along / width) / depth

- `left`: the mapper's value where its line (row, column, line of one
  depth) meets the polygon's left edge;
- `span`: what the right edge has more;
- `along`, `width`: the pixel's distance from the left edge along the
  line, and the line's length inside the polygon;
- `depth`: the w the floor and wall mappers divide by; 1 for the others,
  which step the level itself.

For the floor and wall mappers `left` and `span` are the level times w,
as those mappers step it along the edges. One row for a whole polygon is
(row + 0.5, 0, 0, 1, 1).

A lit polygon is cut at the level of each of its corners
(`gpu_emit_cut` in `rastq.c`). A piece then lies between two of the
mapper's lines with one edge on either side, and on each of its two
lines `gpu_light_piece` gives every vertex the value at the line's first
end, the difference to its other end, the distance between the two and
the vertex's own distance from the first. All four are linear over the
piece, so the GPU interpolates them exactly whatever triangles it draws,
and they survive the cuts at a texture's repeats unchanged.

The shader (`src/MacSrc/VitaGpu.c`) gets the five multiplied by the
vertex's w. The hardware's perspective interpolation then yields each
of them times the pixel's w, which cancels in the quotients. Per pixel
it is two divisions more than G4's shader B.

A lit triangle stays whole: its level (times w, for floors and walls)
and its w, each linear, already give the mapper's quotient.

#### The colour-table fill

Under `FILL_CLUT` the 2D library's table (`fl8ft.c`, "clut fill type")
has, for every texture mapper, its colour-table variant, and `h_umap`,
`v_umap` and `per_umap` take the table from the fill: the call's light
level and its own table count for nothing. `gpu_classify` now takes such
calls as colour-table ones with the fill's table. They were G4's only
reason for a second and third scene.

#### One shader

Shader A is gone. The textured shader is G4's B with the light formula;
the flat-colour pair stays as the fallback when the textured one doesn't
compile.

#### Checked on the PC

The stand-in computes the row with the same formula, and the comparison
runs twice: light per pixel, and light in slabs as in G4 (kept for one
more capture: `rastq_test_gpu_slabs`, variant 1 on the Vita). Over 900
frames each:

| | light per pixel | light in slabs |
|---|---|---|
| pieces lit polygons are cut into | 5,781 | 58,887 |
| pixels that differ from the mappers' | 2.52% | 2.71% |
| and have no match within two pixels | 0.099% | 0.211% |
| limit on the second, per run | 0.15% | 0.5% |

Light per pixel leaves less to chance than G3's scenes whose light fit
one plane (0.16%), with a tenth of the slabs' pieces.

Deliberate mistakes, with the second figure for light per pixel
(60 frames; 0.086% without any):

| mistake | no match within two pixels |
|---|---|
| left and right values swapped | 6.4% |
| the distance not taken from the left edge | 6.7% |
| both lines of a piece given the first one's values | 5.9% |
| the width doubled | 5.0% |
| columns taken as rows | 3.1% |
| the depth not applied | 2.8% |
| a cut vertex's light not interpolated | 0.81% |
| not cut at the corners' levels | 0.43% |
| the fill's colour table ignored / the light kept under that fill | 7.5% / 7.3% |
| repeats not cut, or v not moved | 0.67% |
| the two ways of carrying light swapped | 0.82% |
| no culling, colour table ignored, transparency dropped, no perspective, light one row up | 6.9% to 25% |
| lines of one depth taken the wrong way | 0.20% |
| the floor and wall mappers' depth not used | 0.18% |
| the dispatcher not asked | 0.094%, **under the limit** |

The test fails on all but the last, which concerns too few polygons to
move the figure.

#### What the capture will show

Variants, 5 s each, both GPU ones shown from the canvases: 0 = three
cores, 1 = GPU with light in slabs, 2 = GPU with light per pixel.

- `gpu.txt`: `textures=on texture_check plain=0 table=0 light=0 wrap=0`:
  the light band now goes through the formula, the wrap band through a
  depth;
- `gpuscenes=` should be 1 at the test spot, `gpuwhy=` all zeros there;
- `gpuwait=` in variant 2 against variant 1: what the two divisions per
  pixel cost; `gpusubmit=` and `gpuslabs=`: what not cutting slabs
  saves;
- `gpudiff=`: 4.7% in G4.

#### Results (`docs/profiles-gpu/profile-step-5.txt`, `gpu.txt`, `gpudumps-step-5/`)

Standing still in the first area, medians of the 1 s windows:

| ms per frame | 0: three cores | 1: GPU, light in slabs | 2: GPU, light per pixel |
|---|---|---|---|
| pixel filling (`raster`) | 9.51 | 8.36 | 5.44 |
| of which waiting for the GPU | | 3.80 | 3.38 |
| of which sending (`gpusubmit`) | | 4.20 | 1.70 |
| 3D pass (`traverse`) | 12.36 | 11.01 | 8.11 |
| `sendview` | 3.23 | 2.33 | 2.31 |
| help scan | 3.55 | 3.54 | 3.52 |
| `present` | 3.22 | 1.05 | 2.32 |
| of which waiting for the screen | 0.09 | 0.07 | 1.35 |
| whole frame | 22.42 | 17.57 | 15.95 |
| fps | 44.6 | 56.9 | 62.7 |

While walking: 36.3 fps on three cores, 61.5 with slabs, 62.5 with light
per pixel.

- **Variant 2 is at the screen's rate**, with 1.35 ms of each frame
  spent waiting for it: its work is 14.6 ms a frame, against 22.4 on
  three cores.
- **Shadow edges are as fine as the CPU's** on the Vita.
- **Light per pixel costs the GPU less than slabs did**: 3.38 ms against
  3.80. The two divisions a pixel weigh less than the triangles of 499
  slabs; the polygons now go over in 76 pieces, and sending takes
  1.70 ms against 4.20.
- **One scene a frame**, and nothing drawn by the CPU: `gpuwhy=` is all
  zeros at the test spot, and over the walk too but for a translucent
  polygon now and then. The colour-table fills are 9 of the
  `clut:11` calls.
- **The shader compiles and its start-up patterns are exact**
  (`texture_check plain=0 table=0 light=0 wrap=0`).
- **The picture**: 96.2% of the pixels identical to the CPU's in the
  dumped comparison at the test spot. What differs is thin lines where a
  texel boundary or a band's edge falls one pixel to the side. `gpudiff`
  is 4.3% standing still, both GPU variants taken together.
- `check=0/288`, `leaks=0`; `gpufallbacks=65` is the paneled view.

Where the 14.6 ms of a GPU frame go now: the 3D pass without the GPU
about 2.7 ms, sending 1.7 ms, the GPU 3.4 ms, stars, HUD and cursor
2.3 ms, the help scan 3.5 ms, presenting 1 ms, the game itself 0.7 ms.

#### Go for G6

The GPU path draws the game's picture, as fast as the screen shows it
where it was measured. What is left is making it the game's renderer
and not a variant of the profile build:

- the slab path goes (light per pixel is better on every count);
- the GPU code into the normal build, behind a setting in Vita Options,
  with the CPU renderers as the other choice and as the fallback;
- the shaders shipped compiled, so that `libshacccg.suprx` is no longer
  needed;
- the paneled (non-full-screen) view, whose 804-pixel-wide canvas the
  GPU module refuses today;
- a frame without a 3D view after frames shown from a GPU canvas (pause,
  full-screen map): the screen buffer doesn't hold the last view;
- more of the game than the first area: the kinds of call that still
  fall to the CPU elsewhere (translucent and shaded polygons, lines,
  voxels), and cyberspace.

### Step G6: the GPU renderer in the game

In every Vita build from here on.

#### The Renderer setting

(Removed since: with nothing to gain from the slower choices, the game now
always does what "GPU" did, three cores included when the GPU can't be
used. `main()` in `src/MacSrc/Shock.c` sets the queue up. What follows is
the setting as it was.)

Vita Options' "Multicore" button is now "Renderer", with three choices
(`gShockPrefs.renderer`, `vita-renderer` in the prefs file; an older
file's `vita-multicore = 0` is read as one core):

| choice | what draws a 3D view |
|---|---|
| GPU (default) | the queue records the pass and the GPU draws it; three cores for the views the GPU isn't given |
| 3 cores | the queue records the pass and three cores fill it (Part 1) |
| 1 core | the original drawing, call by call |

`VitaApplyRenderer` (`src/MacSrc/Prefs.c`) sets the queue's mode, its
threads and `rastq_use_gpu`. When the GPU path couldn't be set up
(`vgpu_available`: no compiler module, or a shader that doesn't
compile), the button offers the two CPU choices and a "GPU" setting
behaves as "3 cores".

The profile build ignores the setting and alternates, 5 s each:
0 = three cores, 1 = GPU. The periodic comparison, its frame dumps and
the timings are the profile build's only; `gpu.txt` is written anew at
each start by both.

The slab path of G4 is gone (`gpu_emit_cut` cuts at the corners' levels
only), and so is drawing in flat colours when the textured shader
fails: without it the GPU isn't used.

#### The paneled view

The view of the paneled screen is 804 x 293, in a canvas of its own
whose rows are 804 bytes apart, which the GPU module refused. Now:

- `fr_start_view` gives the draw canvas a GPU canvas's pixels and its
  row length (960), and puts the view's own back for a CPU frame. The
  view draws into the canvas's top left;
- `vgpu_begin` takes any view up to the canvas's size, with the
  canvas-sized render target and colour surface. One render target
  serves every view;
- `fr_send_view` copies the finished view to the screen buffer as
  before (`Fast_Slot_Copy`): the game draws on the screen over this view
  (the help overlay, messages), so the screen buffer has to hold it. The
  copy reads GPU memory.

#### Where the GPU is not used

- **Cyberspace** (`_frp.faces.cyber`): its walls are flat and shaded
  polygons and its objects wireframes; shaded polygons and lines are the
  CPU's in the GPU path, drawn into GPU memory between two scenes. It
  keeps the three-core renderer. In the profile build the queue counts
  what the GPU could draw of such a view and why not the rest
  (`rastq_gpu_survey`, into `gpukinds=` and `gpuwhy=`).
- **Low resolution** (`DoubleSize`): the view is doubled out of its
  canvas, which reads all of it.
- Views of fewer than 272 rows (`rastq_gpu_next` now takes the view's
  rows), the help scan, the security cameras, the 360 view: as before.

#### The screen buffer when the view stops being drawn

A full-screen GPU view is shown from its canvas and not copied to the
screen buffer. Three rules keep the two in step (`src/MacSrc/Shock.c`):

- `SDLDraw` shows the view's canvas for a frame that drew one, and goes
  on showing the last one while nothing says the screen buffer is to be
  shown (a frame without a view, a palette fade);
- `VitaShowView` refuses while the game is paused, and whenever the
  canvas isn't the full screen: `fr_send_view` then copies the view to
  the screen buffer as on the CPU, and the screen buffer is shown. That
  is how the options panel gets the view under it (`wrapper_start`
  pauses, then renders);
- `VitaSyncView` copies the last shown canvas into the screen buffer,
  once. It is called before the game draws on the screen without having
  drawn the view: at the top of `game_loop`'s paused branch, in
  `loopmode_switch`, before a video mail plays, before the help overlay
  and the wait cursor.

The cursor needs nothing: `rend_mouse_hide` saves what is under it from
the canvas before drawing it there, so the cursor code's save-under is
the view without the cursor, in step with the canvas.

A place that draws on the screen without one of these would show as a
picture that stays frozen on the last view.

#### Checked on the PC

The stand-in comparison has a fourth canvas, 804 x 293 with rows 960
bytes apart, for the paneled view. Over 1,200 frames: 2.42% of pixels
differ from the mappers', 0.093% without a match within two pixels
(limit 0.15%). `tests/rastq/run.sh` passes; both Vita builds are free of
warnings.

#### Results (`docs/profiles-gpu/profile-step-6.txt`, `gpu.txt`, `gpudumps-step-6/`)

Played on the Vita with the normal build: the three renderers switched
in game, the full-screen and the paneled view, pause, the options panel,
a video mail, cyberspace. No glitch and no frozen picture.

The capture (profile build, full-screen view throughout), medians of the
1 s windows:

| | 0: three cores | 1: GPU |
|---|---|---|
| standing still | 44.6 fps (22.4 ms) | 62.7 fps (15.96 ms, of which 1.3 waiting for the screen) |
| walking and fighting | 35.9 fps | 62.9 fps |
| cyberspace (three cores in both) | 62.5 fps | 61.6 fps |

- One scene a frame everywhere outside cyberspace, nothing drawn by the
  CPU; `check=0`, `leaks=0`; `texture_check` all zeros.
- The paneled view wasn't in the capture: its copy is still unmeasured.
- Dumped comparisons: 96.4% of pixels identical at the test spot; 79 to
  86% facing a wall from close, where the light changes slowly across
  the wall and a band's edge a few hundredths of a level off moves far.
  The pictures look the same.

What the capture explains:

- **Drops to about 50 fps at some angles.** 9 of the 255 GPU windows are
  under 57 fps without a stall. At the test spot a GPU frame has 2 ms to
  spare, and four things grow:

  | | usual | in the slow windows |
  |---|---|---|
  | help scan | 3.5 ms | 6 to 7 ms, once 15.6 ms |
  | sending to the GPU (`gpusubmit`) | 1.7 ms | up to 5.0 ms (135 polygons in 362 pieces) |
  | stars, HUD and cursor (`sendview`) | 2.3 ms | 4 to 5 ms |
  | waiting for the GPU | 3.4 ms | up to 5.8 ms |

  The help scan's case is the door seen from close: after its render,
  `olh_scan_objs` goes over its 58,000 pixels and calls `olh_candidate`
  for every one that is on an object, a distance computation each time,
  for the same object again and again.
- **2-second freezes in the profile build, three in a row**: the frame
  dumps. Dumps 02 to 04 were written within 15 seconds, each a new
  record of differing pixels on entering a room.
- **An fps counter above 60**: the counter is 1000 / average frame time.
  Three 1 s windows, each the first of a GPU phase, are at 66.8, 68.1
  and 108.3: frames that the screen didn't hold back (it holds a frame
  back only when two finished ones are waiting). Why a whole second can
  pass like that is not understood.
- **Stutters when fighting**: eight stalls of 130 to 260 ms, all in the
  input phase, in the three-core windows as in the GPU ones. Not the
  renderer. Unmeasured suspects: `snd_sample_play` decodes and converts
  each sound effect the first time it is played (`Mix_LoadWAV_RW`, on
  the main thread; the result is kept), and resource loads from the
  memory card.

One thing wrong in the queue: in cyberspace `fr_start_view` keeps the
view off the GPU, but the queue takes any large view for the GPU's while
the GPU renderer is on. On every flush it prepared the list, was refused
the canvas and fell back (`gpufallbacks` from 28 to 55,618 over the
cyberspace part). The picture is right and cyberspace runs at the
screen's rate, but the work is wasted, and the count of what the GPU
could draw of cyberspace (`rastq_gpu_survey`) never ran.

#### Go for G7

- The help scan asks `olh_candidate` once per object and scan.
- `fr_start_view` tells the queue whether a view is the GPU's.
- Frame dumps no closer than 30 seconds.
- Measurements, in the profile build, of what else eats the 2 ms: the
  sending split into its parts, the wait into issuing the draws and the
  GPU working, `sendview` into stars, HUD and cursor, the help scan into
  its render and its look at the pixels; and of sound and resource
  loading, for the stalls.

Cyberspace on the GPU is dropped from the roadmap: it is at the screen's
rate on three cores.

### Step G7: holding the screen's rate

Built and run on a Vita twice. Results at the end of this section.

#### Fixes

- **The help scan asks once per object** (`olh_scan_objs` in
  `src/GameSrc/olhscan.c`). It keeps `olh_candidate`'s answer for each
  object of a scan, by the object's colour in the scan's picture, where
  it asked again for every pixel on the object. The answer depends on
  the object alone, so the scan finds what it found before. In all
  builds and for every renderer.
- **The queue is told whether a view is the GPU's**
  (`rastq_gpu_view`, called by `fr_start_view` for every view). The
  queue used to take any large view for the GPU's while the GPU
  renderer was on, and to be refused the canvas when `fr_start_view`
  had kept the view for the CPU (cyberspace, low resolution). Such a
  view is now replayed on three cores without the attempt, and the
  profile build's count of what the GPU could draw of it runs.
- **Frame dumps at least 30 seconds apart** (`vgpu_compared`), so that
  walking into a room doesn't freeze the profile build three times.

#### New in the log (profile build)

| field | what |
|---|---|
| `gpuprepare=` | of the sending: deciding what each call is, and the tables (ms a frame) |
| `gpusubmit=` | the rest of the sending: cutting calls up and handing them over |
| `gpuupload=` | of `gpusubmit`: copying bitmaps into GPU memory |
| `gpudraw=`, `gpudraws=` | of `gpuwait`: issuing the scene's draws, and how many |
| `helprend=` | of `helpscan`: its render; the rest is its look at the pixels |
| `stars=`, `hud=`, `viewout=` | of `sendview`: the stars, the overlays drawn into the view, and the cursor with the view's way to the screen |
| `sndload=` | a sound effect decoded for its first use (`snd_sample_play`) |
| `resload=` | a resource read from the card (`ResLoadResource`) |

Each is the average per frame and, after the slash where there is one,
the most in one frame of the window.

#### Results (`docs/profiles-gpu/profile-step-7.txt`, `profile-step-7b.txt`, `gpu.txt`, `gpudumps-step-7/`, `gpudumps-step-7b/`)

Two captures with the profile build. The first has the door of step 6
and a long stay in cyberspace. The second has what the first lacked: a
long stay at an angle that costs more, and the paneled view with the
help labels on. They agree where they overlap. The table is the second
one, medians of the 1 s windows, cut by the log's `mode=` (1 full
screen, 0 paneled) and the check numbers noted while playing:

| | 0: three cores | 1: GPU | GPU: waiting for the screen |
|---|---|---|---|
| standing still | 46.4 fps (21.6 ms) | 62.1 fps | 2.0 ms |
| moving | 34.5 fps | 62.1 fps | 1.0 ms |
| the heavy angle | 32.1 fps | 58.5 fps (17.1 ms) | none: 0.5 ms too much |
| paneled view, help labels on | 53.9 fps | 61.7 fps | 0.1 ms |
| paneled view, no help scan | 60.1 fps | 62.7 fps | 5.2 ms |
| cyberspace (three cores in both) | 62.4 fps | 62.6 fps | 4.6 ms |

- **The door of step 6** is at 60 to 62 fps (first capture): the help
  scan no longer asks the same question for every pixel. Its most in one
  frame is still 17 to 19 ms, once a capture, and that frame has a
  resource load inside the scan's render.
- **Cyberspace**: `gpufallbacks=0`, the queue no longer tries. The count
  of what the GPU could draw of it ran: 43 flat polygons a frame, and
  2.5 of a kind it doesn't draw.
- **The stalls** are measured now. 10 in the second capture: 8 have a
  sound effect decoded for its first use in them (`sndload` 124 to
  237 ms in one frame), one a resource load alone (54 ms), one is the
  load of the cyberspace level (1.9 s). In the three-core windows as in
  the GPU ones. Not the renderer, and not this branch's work: noted in
  `docs/TODO.md`.
- **Dumps**: 4 and 3, none closer than 30 seconds, no freeze. 6.5% and
  5.9% of the compared pixels differ. `check=0`, `leaks=0`,
  `texture_check` all zeros.
- **The paneled view** is in both captures. The first has it without the
  help scan, which had stopped (help off, an object on the cursor or a
  panel open: the log doesn't say which).

Where a GPU frame goes, in ms a frame:

| | standing still | the heavy angle | paneled, labels on |
|---|---|---|---|
| waiting for the GPU (`gpuwait`) | 3.4 | 3.5 | 2.2 |
| help scan (`helpscan`), of which its render | 2.9, 1.2 | 3.6, 1.7 | 2.3, 1.4 |
| HUD drawn over the view (`hud`) | 2.2 | 4.5 | 2.3 |
| sending to the GPU (`gpuprepare` + `gpusubmit`) | 1.8 | 2.2 | 1.8 |
| the view's way to the screen (`viewout`) | 0.1 | 0.1 | 2.9 |
| polygons | 77 | 48 | 45 |
| whole frame, without the wait for the screen | 14.1 | 17.1 | 16.1 |

- **The heavy angle isn't heavy for the GPU.** It has fewer polygons
  than the test spot, and the GPU takes the same time. What grows is on
  the CPU, and it grows the same on three cores: the HUD by 2.3 ms, the
  help scan by 0.7 ms, the sending by 0.4 ms.
- **The HUD moves in steps**: 2.2, 2.7 and 4.5 ms in full screen, 0.5
  and 2.2 ms in the paneled view. One step is 1.75 ms in both views, and
  in the paneled view it comes and goes with the help scan. A lead, not
  measured: the help label's text. `draw_shadowed_string` draws a string
  nine times, eight shifted copies for the outline and then the text.
  The rest of the full-screen HUD is `fullscreen_overlay`, which redraws
  the button panels, the two side panels, the inventory, the vitals and
  the icons over the view every frame.
- **The paneled view on the GPU** pays 2.9 ms to copy the view out of
  GPU memory onto the screen (0.4 ms from ordinary memory on three
  cores). Without the help scan it has 5 ms to spare; with it, none.
- **The wait is the largest fixed part**, and the main thread does
  nothing during it. `gpudraw` is 0.1 ms of it: the rest is the GPU
  working.

#### Go for G8

- The queue hands a scene to the GPU without waiting for it; a separate
  call waits. It still waits at once where the CPU draws on the canvas
  within the frame.
- `fr_send_view` waits before the HUD, and before that runs the help
  scan when the view is the GPU's: the scan never touches the picture,
  and it takes about as long as the GPU does. A view that shows stars
  waits first, as today: they read the picture.
- The game loop skips its own scan when the render ran it.
- The HUD measured part by part (profile build): hand, help label, other
  texts, button panels, side panels, inventory, vitals, icons.

Expected, not measured: the heavy angle from 17.1 ms to about 14, the
paneled view with labels from 16.1 to about 14. How much of the wait
needs the CPU is not known.

Left out: the stalls (their own task, later); the paneled view's copy
(G8 gives it the time).

### Step G8: use the GPU's time

Built and run on a Vita. Results at the end of this section.

#### The queue hands the last scene over and doesn't wait

`rastq_gpu` has a `finish` next to `end`: `end` gives the scene to the
GPU (`sceGxmEndScene`), `finish` returns once the canvas holds it
(`sceGxmFinish`). The queue still waits at once wherever the CPU touches
the canvas within the view: before a call left to the CPU, between two
scenes, in the profile build's comparison, and after a flush in the
middle of a view (voxels and direct calls are drawn straight after it).
Only the last scene of `rastq_end` is left to the GPU. Until
`rastq_gpu_finish` the canvas is the GPU's; `rastq_gpu_busy` says so.

#### The view scans while the GPU draws

`fr_send_view`, for a view the GPU is still drawing:

1. If the view shows a star field (`star_field_seen`, which is
   `star_render`'s own test), it waits first: the stars go into the
   pixels the field left, so they read the picture.
2. Otherwise the stars' block draws nothing and closes the view's 3D
   frame, as it always did.
3. It runs the help scan (`olh_scan_in_render`), a render of its own
   into a canvas of its own, on the three cores.
4. It waits for the GPU (`rastq_gpu_finish`), then draws the HUD and the
   cursor and sends the view, as before.

The scan is the game loop's: `game_loop` marks its `render_run` as the
one the scan may run in, `olh_scan_in_render` runs `olh_scan_objects`
under the loop's own conditions, and the loop skips its scan if the
render ran it. Other renders (video mail, the wait cursor) never scan,
as before. With the CPU renderers, or stars in view, the scan stays
where it was.

What the game sees: the same scan, a game step earlier (before
`physics_run` in place of after), so the label is the one of the picture
it is drawn on and not of the frame before.

A second render between a view's render and its HUD changes four things
the HUD and the send use, and `fr_send_view` puts them back: `_fr`,
`_fr_curflags`, the current canvas, and `current_num_hudobjs`. The last
one is a trap: the scan's render stores the rectangles of HUD objects
(the target box, the beam's end) in its own small coordinates
(`SET_HUDOBJ_RECT`). The next frame used to clear them before anything
read them; run before the HUD, they would give a second, misplaced
target box. The HUD's path makes no 3D calls, so the scan's 3D frame
changes nothing for it.

#### Checked on the PC

The test's stand-in GPU now draws aside and copies its picture to the
canvas only at `finish`, so a canvas read or written before the wait
shows as a wrong picture. `draw_scene` does what `fr_send_view` does:
when the GPU still has the view's last scene, the CPU renders the same
calls as a second view into another canvas, which must come out as the
mappers draw it and leave the GPU's scene out, and only then waits. The
comparison's GPU side must be the finished picture.

`tests/rastq/run.sh 300`: passes; the GPU is left its last scene in 682
of 1,200 frames (the others end on a call the CPU draws). Each of these,
put into `rastq.c` on purpose, fails the test: no wait before a call
left to the CPU, none after a flush in mid-view, none in the comparison,
and waiting at the end after all.

Not checkable on the PC: the game's side (the scan inside the render,
the HUD after it), and how much of the GPU's time the CPU really gets.

#### New in the log (profile build)

| field | what |
|---|---|
| `gpuwait=` | now: issuing the scene's draws, and the time the main thread is really blocked on the GPU |
| `gpuoverlap=` | the time between handing the last scene over and starting to wait for it: what the CPU did meanwhile |
| `hudparts=` | of `hud`: `hand` the weapon in hand, `label` the help label, `text` compass and messages, and in full screen `buttons` the two button panels, `mfd` the two side panels, `inv` the inventory, `vitals` vitals and meters, `icons` the side icons |

`render3d` and `sendview` now contain the help scan when it runs in the
render; `helpscan` is still its own time.

#### What the capture will show

- `gpuoverlap` about the help scan's time and `gpuwait` near zero with
  help on: the wait is hidden. If `gpuwait` stays at 3 ms, the GPU needs
  a core the scan is using and the gain is smaller.
- The heavy angle and the paneled view with labels at the screen's rate
  (`swapwait` above 2 ms).
- `hudparts`: whether `label` is the 1.75 ms step, and which part of the
  full-screen HUD is the rest.
- To look at in the normal build: the help labels follow objects as
  before; one target box, in its place; a window onto space; pause,
  video mail, the paneled view.

#### Results (`docs/profiles-gpu/profile-step-8.txt`, `gpu.txt`, `gpudumps-step-8/`)

Medians of the 1 s windows, cut by the check numbers noted while
playing:

| | 0: three cores | 1: GPU | GPU: waiting for the screen |
|---|---|---|---|
| standing still | 46.2 fps | 63.3 fps | 4.7 ms (2.0 in G7) |
| moving | 33.1 fps | 62.8 fps | 3.2 ms |
| G7's heavy angle, labels on | 31.5 fps | 63.3 fps (58.5 in G7) | 1.8 ms |
| the same, paneled | 51.0 fps | 63.1 fps | 1.8 ms (0.1 in G7) |
| the hallway angle, full screen, no scan | 37.0 fps | 55.4 fps (18.0 ms) | none |
| the same, paneled | 58.3 fps | 57.0 fps (17.6 ms) | none |
| the hallway angle again, help off | 35.1 fps | 53.2 fps (18.8 ms) | none |
| turning on the spot, help on | 40.7 fps | 62.8 fps | 3.3 ms |
| the same spot, help off | 48.9 fps | 62.7 fps | 5.4 ms |
| cyberspace (three cores in both) | 62.8 fps | 62.6 fps | 3.4 ms |

- **The wait is hidden where the scan runs.** `gpuwait` is 0.2 to
  0.45 ms (3.4 in G7) and `gpuoverlap` 2.4 to 3.8 ms, the scan's time:
  the GPU doesn't need the cores the scan uses. No glitch was reported
  from the play-through.
- **Without the scan nothing changes**: `gpuwait` 3.2 to 3.7 ms,
  `gpuoverlap` 0.02 ms.
- `check=0`, `leaks=0`, `gpufallbacks=0`, 6.2% of the compared pixels
  differ, 3 dumps.
- **Stalls**: 14. Seven have a sound effect's first decoding in them,
  three a resource load alone, two are the cyberspace level loading
  (2.5 s in, 7.7 s out), and two (129 and 67 ms) show nothing in the
  timers there are.

The HUD's parts (`hudparts`, ms a frame, the same on three cores):

| | full screen | paneled |
|---|---|---|
| help label, when one shows | 1.6 to 1.9 | 1.75 |
| inventory | 1.2 | |
| vitals and meters | 0.9 | |
| a side panel, when one is open | 0.6 | |
| weapon in hand | 0.4 | 0.4 |
| messages, when there is one | 1.0 | |
| button panels, side icons | 0.1 | |

The label is G7's 1.75 ms step: `draw_shadowed_string`'s nine draws.

**The hallway angle** (past the large door of the first rooms, looking
down the main hallway) is a different case from G7's:

| ms a frame, GPU renderer | standing still | the hallway angle |
|---|---|---|
| polygons, and the pieces they are cut into | 77, 76 | 132 to 139, 354 to 365 |
| sending (`gpuprepare` + `gpusubmit`) | 1.8 | 5.0 |
| blocked on the GPU (`gpuwait`) | 0.45 | 3.7 |
| help scan | 2.9 | none |
| HUD | 2.2 | 3.3 |
| whole frame, without the wait for the screen | 11.1 | 18.0 to 18.8 |

- It is heavy for the sending: lit polygons there are cut into 2.7
  pieces each, on one core, while the two worker cores do nothing.
- The wait has nothing to hide behind: the scan doesn't run there.
- In the paneled view the GPU is slower than three cores there (57.0
  against 58.3 fps): the sending, plus the 2.9 ms copy of the view out
  of GPU memory.

**Why the scan doesn't run there**: not the port. The level has a
trigger past that door that switches on-screen help off and saves the
option as off (`trap_questbit_func` in `src/GameSrc/trigger.c`, quest
bit 0x2091, "special hack for auto shutoff of on-line help"); a new
game switches it on again. That fits both G7 captures, where the scan
also ended for good while leaving the first rooms. Kept as the original
has it.

#### Go for G9

A cheaper HUD, for every renderer:

- the help label drawn into a small bitmap when its text changes and
  copied each frame, in place of nine string draws a frame;
- the copies of the inventory, the vitals and the side panels over the
  view: large bitmaps, mostly see-through. How much there is to save is
  to be read from the code first.

Expected, not measured: 1.5 ms off a full-screen frame, 3 ms with a
label. The hallway angle would be at 16.5 to 17 ms, near the screen's
rate and not safely at it.

Then G10: the sending shared between the three cores (deciding what
each call is and cutting it up, the hand-over staying in order), about
2 ms off the frames with many pieces.

Left out: the wait when the scan doesn't run (nothing else in the frame
is free to run in that time); the paneled view's copy; the stalls.

### Step G9: a cheaper HUD

Built and run on a Vita. Results at the end of this section.

The HUD is redrawn identically frame after frame, on every renderer.
Three changes make that drawing cheaper without changing a pixel of it.

#### Outlined text is drawn once and kept

`draw_shadowed_string` (`src/GameSrc/tools.c`) draws a string nine
times: eight copies in the outline's colour around its place, then the
text. `hudkeep_outlined` (`src/GameSrc/hudkeep.c`) makes those nine
draws, with the same calls, into a canvas of its own, keeps the part
they cover and copies it each frame, transparent where nothing was
drawn. It covers the help label and the HUD's messages, which both end
in that function.

- A text is known by its string, font, colours and screen mode. Eight
  are kept; the one unused the longest goes.
- A text is made into a picture the second time it is asked for, so one
  that changes every frame (a counter) is drawn as before and costs
  nothing more.
- Drawn as before whenever the copy couldn't be the same: a colour that
  is palette index 0 (the picture's "nothing here"), a text that
  reaches the edge of the 1024x128 canvas it is made in, a fill type
  other than the normal one.

#### Small stretched bitmaps are kept

`ss_bitmap` stretches a 320x200 bitmap to the screen's size at every
call. The vitals are up to 46 arrows of a few pixels and two icons a
frame, each a stretch of its own; the inventory's page buttons are a
292x10 bitmap stretched to 876x27 every frame. `ss_kept_bitmap`
(`src/GameSrc/gr2ss.c`, `hudkeep_scaled`) keeps the stretched result
and copies it.

- A bitmap is known by its pixels, compared byte for byte at every
  call, not by its address: one that changed is stretched again.
- Only uncompressed 8-bit bitmaps of at most 4,096 pixels, stretched to
  at most 32,768, landing whole inside the clip rectangle. Anything else
  is stretched as before (the vitals' icons, if they are compressed).
- It rests on the stretch giving the same pixels wherever the bitmap
  goes, which the PC test checks.

Asked for where the game draws the same small bitmap frame after frame:
`draw_status_arrow` and the icons in `src/GameSrc/vitals.c`, the page
buttons in `inv_update_fullscreen`. `ss_bitmap` itself is unchanged.

#### Transparent copies go four pixels at a time

`flat8_flat8_ubitmap` (`src/Libraries/2D/Source/Flat8/fl8fl8.c`) copied
a transparent bitmap one pixel at a time, testing each. It now reads
four: all transparent, it skips them; none transparent, it copies them;
otherwise one by one as before. A bitmap copied onto itself goes the
old way. This is the copy of the inventory and the side panels over the
view, and of the kept pictures above.

#### Checked on the PC

`tests/rastq/rastq_test.c` (`test_overlays`), in `tests/rastq/run.sh`:

- 4,000 transparent copies of random bitmaps, at random places and
  partly off the canvas, against one pixel at a time: none differs. A
  wrong test of the four pixels, put in on purpose, fails 3,230 of them.
- 2,000 random bitmaps stretched at random places against the same
  bitmap stretched once at the corner and copied: none differs.

Not checkable on the PC: the game's texts and bitmaps themselves. The
profile build checks them as it runs.

#### New in the log (profile build)

| field | what |
|---|---|
| `hudkept=text:a/b,scaled:c/d` | per frame: outlined texts copied / drawn, stretched bitmaps copied / stretched |
| `hudcheck=bad/runs` | kept pictures also drawn the old way and compared (one text in 64, one bitmap in 2,039), and how many differed. `bad` must stay 0. A checked frame is left out of the timings |

#### The build

`vita/segment-gap.ld`, passed to the linker from `CMakeLists.txt`: the
profile package stopped building with "Cannot allocate 4584 bytes for
SCE data at end of segment 0; segment 1 overlaps". `vita-elf-create`
puts the module's data after the code segment and needs room there; the
default linker script starts the data segment at the next 64 KB
boundary, so the build failed whenever the code happened to end within
4.5 KB of one, which this step's code did. The fragment keeps 8 KB free
after the code. Both packages start on the Vita with it.

#### What the capture will show

- `hudparts`: `label` from 1.75 ms to a few tenths, `vitals` and `inv`
  down; `text` when a message shows.
- `hudcheck` with `bad` at 0, and `hudkept` showing the copies are used
  (about one text and fifty bitmaps a frame).
- The full-screen HUD from 3.3 ms to about 2, and from 5 to about 2.2
  with a label. Estimates: how the vitals' 0.9 ms splits between arrows,
  icons and meters isn't measured.
- To look at in the normal build: the outline and the text of labels
  and messages, the health and energy bars as they change, the
  inventory's pages and buttons, a side panel, in both views.

#### Results (`docs/profiles-gpu/profile-step-9.txt`, `gpu.txt`, `gpudumps-step-9/`)

Medians of the 1 s windows, cut by the check numbers noted while
playing:

| | 0: three cores | 1: GPU | GPU: waiting for the screen |
|---|---|---|---|
| standing still | 46.8 fps | 63.2 fps | 5.6 ms (4.7 in G8) |
| G7's heavy angle, labels on | 29.8 fps | 63.5 fps | 5.9 ms (1.8 in G8) |
| a fight | 37.7 fps | 63.2 fps | 4.6 ms |
| the hallway angle, full screen | 39.1 fps | 59.0 fps (16.95 ms; 55.4 fps in G8) | none |
| the hallway angle, paneled | 61.4 fps | 55.3 fps (18.1 ms) | none |
| fights, paneled | 61.9 fps | 63.0 fps | 3.4 ms |

The HUD (`hud`, `hudparts`), ms a frame on the GPU renderer; three
cores gain the same:

| | G8 | G9 |
|---|---|---|
| full screen, no label | 2.2 to 3.3 | 1.35 to 1.9 |
| full screen with a help label | 4.4 | 1.5 |
| `label` | 1.76 | 0.20 |
| `inv` | 1.2 | 0.6 |
| `vitals` | 0.9 | 0.65 |
| `hand`, when a weapon is out | 0.4 | 0.4 |

- **`hudcheck=0/438`**: no kept picture differed from the old drawing.
  No glitch was reported from the play-through.
- `hudkept`: one text and 28 to 30 stretched bitmaps copied a frame,
  none made again once the first frames are past.
- **The vitals gained little.** The arrows are the 29 copies; what is
  left is the two icons and the meters, which this step didn't touch.
- `check=0`, `leaks=0`, `gpufallbacks=0`, 6.6% of the compared pixels
  differ, 5 dumps.
- **Stalls**: 24, most of them in the fights. 19 have a sound effect's
  first decoding in them (20 to 190 ms), three a resource load alone,
  two show nothing in the timers there are.

What is left at the hallway angle, GPU renderer, ms a frame:

| | full screen | paneled |
|---|---|---|
| sending (`gpuprepare` + `gpusubmit`), 133 polygons in 354 pieces | 5.1 | 4.8 |
| blocked on the GPU, no scan to run meanwhile | 3.8 | 2.4 |
| the view's way to the screen (`viewout`) | 0.1 | 2.9 |
| HUD | 1.8 | 0.5 |
| whole frame | 16.95 | 18.1 |

In the paneled view three cores draw that angle in 16.3 ms: the GPU
loses there by the 2.9 ms copy of the view out of GPU memory.

#### Go for G10

The sending shared between the three cores: deciding what each call is
and cutting it into pieces is done by the main thread alone while the
two worker cores do nothing. The pieces are still handed to the GPU in
the order the game drew them.

Expected, not measured: about 2 ms off the frames with many pieces, the
hallway angle near 14.7 ms in full screen and 15.8 ms paneled. How the
5 ms split between the cutting and the hand-over, which stays on one
core, is not known.

After that, to choose from: the paneled view shown from GPU memory in
place of copied (2.9 ms; everything the game draws over the view on the
screen has to be looked at); the wait when the help scan doesn't run
(2.4 to 3.8 ms); the stalls.

### Step G10: cyberspace on the GPU

Built and run on a Vita; results at the end of this section. Asked for
ahead of the sending on three cores, which becomes G11.

Cyberspace was the one part of the 3D world the GPU renderer left to
the CPU (step G6), and it runs there at the screen's rate (62.7 fps,
3.2 ms to spare). This step is not for speed: it makes one renderer
draw all of it. The profile build alternates the two renderers in
cyberspace as elsewhere, so the capture says which is faster.

#### What cyberspace is drawn with

| | a frame (G8) | before this step |
|---|---|---|
| flat polygons | about 75 | on the GPU |
| polygons shaded between colours (`FIX_UCPOLY`: walls, floors) | 7, up to 27 | the CPU's |
| lines, in one colour or shaded (tile outlines, wireframes, explosions) | not counted | drawn at once after a flush: 7 flushes a frame |
| points | not counted | the same |

No texture, no translucency and no brightness-shaded polygon came up in
the two visits measured.

#### Polygons shaded between colours

The CPU steps red, green and blue (0 to 256) along the polygon's edges
and across each row, and writes `grd_ipal` of their top five bits each:
the palette index of that colour. On the GPU:

- a third pair of shaders (`shaded_*` in `src/MacSrc/VitaGpu.c`) takes
  the three colours as the textured ones take the light, `left + span *
  along / width`, divides each by 8, rounds down and reads the palette
  index from a 1024x32 texture: `grd_ipal`, copied once per scene that
  needs it;
- the queue cuts a polygon of four corners or more at its corners' rows
  (`gpu_emit_cut`, as for the light since G5), so that the three values
  are the mappers' at every pixel; a triangle goes whole;
- a colour that sits on a multiple of 8, as corners' do, is nudged up by
  a 64th so that it can't fall on either side from pixel to pixel.

Only under the normal fill type; the colour-table fill has a loop of
its own on the CPU and stays there.

#### Lines and points

In a view the GPU draws, `g3_draw_line`, `g3_draw_cline` and
`g3_draw_point` no longer flush the queue: the queue records them
(`rastq_line`, `rastq_point`) and the list stays whole. For the CPU
renderers nothing changes: flush, then draw.

The 2D library's wire line is not one pixel a step. Row after row it
draws the pixels between where the line enters the row and where it
leaves it, at least one, as a polygon's edge would be walked. The queue
turns that into three flat or shaded quads: the first row and the last,
which are cut at the line's ends, as the whole pixels they come out to,
and the rows between as one strip with the line's slope and the width
of its step (`gpu_emit_line`). The line is clipped first by the
library's own clipper. A shaded line's colours step once a pixel drawn,
which is along x or along y: a plane over the strip. A point is its
pixel.

When the CPU draws such a view after all (the comparison, a refused
frame), it draws the recorded lines and points in their place, alone on
the main thread as other calls that can't be split into bands.

#### Flat polygons through the flat shaders

Since G3 a flat polygon went through the textured shaders, as a texel
of the tables' unchanged row: two texture reads a pixel to write one
value. It goes through the flat shaders of G1 again (`DRAW_FLAT`), the
clear at the start of a frame included. A scene's draws carry which
shaders they use, and the three kinds mix in one scene in the order the
game drew them.

#### Checked on the PC

- The stand-in GPU has the shaded shader's arithmetic; the scenes have
  shaded polygons, lines and points, and a colours' table in which
  neighbouring colours have neighbouring indices when a GPU is compared.
  `tests/rastq/run.sh 300`: passes, 0.082% of pixels not near one that
  matches (limit 0.15%). Putting green in blue's place, or not cutting
  the polygons, fails it.
- Lines and points alone, 12,000 of them, some clipped: of 1,474,085
  pixels in one colour 27 differ from the 2D library's; of 1,846,605
  shaded, 11,099 differ (a shade a pixel early or late) and 1,141 have
  no match within two pixels.
- The reference libraries draw the test's lines as the 3D library used
  to and give the same frames: the lines' drawing on the CPU hasn't
  changed.

Not checkable on the PC: how the Vita's GPU fills a slanted strip one
pixel wide. Its edges fall between pixels at positions the GPU rounds;
a line may come out a pixel off in a row here and there. The profile
build's comparison will count it.

#### New at start-up and in the log

- `gpu.txt`: the shaded shaders are compiled and checked like the
  others (`colour check drawn: r g b wrong`: one band each for red,
  green and blue going from 0 to 256, compared column by column with
  the table). Without them the GPU still draws the station and
  cyberspace stays on three cores (`textures=on, not the shaded ones`).
- `gpukinds=` gains `shaded:`, `line:` and `point:`, calls a frame.
- In cyberspace the two phases now differ: `var=1` is the GPU.

#### What the capture will show

- In cyberspace: `gpuscenes` 1 a frame and `gpucpu` 0 if nothing else
  turns up; otherwise `gpuwhy` names it.
- `gpuwait` in cyberspace (nothing hides it there) against the three
  cores' `raster`: which renderer is faster, and by how much.
- `gpudiff` and the dumps in cyberspace: how far the GPU's lines and
  colour bands are from the CPU's.
- In the station: `gpuwait` a little lower if the flat clear matters.

#### Results (`docs/profiles-gpu/profile-step-10.txt`, `gpu.txt`, `gpudumps-step-10/`)

`gpu.txt`: the shaded shaders compile (360 and 512 bytes) and
`colour check drawn: 0 0 0 wrong`.

Medians of the 1 s windows, with the time left waiting for the screen:

| | 0: three cores | 1: GPU |
|---|---|---|
| station, standing still | 46.8 fps | 63.2 fps, 5.4 ms |
| station, moving | 36.9 fps | 63.1 fps, 6.2 ms |
| cyberspace, standing still | 62.7 fps, 5.5 ms | 63.2 fps, 7.6 ms |
| cyberspace, moving | 62.3 fps, 5.2 ms | 63.4 fps, 7.5 ms |

- **Cyberspace on the GPU is 2 ms a frame ahead of three cores**, where
  about the same was expected. The drawing costs the same (`render3d`
  6.9 against 6.7 ms): the gain is at the screen. The GPU's canvas is
  shown as it is; the three-core picture is copied into the screen's
  texture first (`present` without its wait: 0.9 against 3.1 ms).
- **The picture is the CPU's but for 0.03% of the compared pixels** in
  cyberspace (6.0% in the station): the strips of `gpu_emit_line` cover
  the 2D library's pixels on the real GPU too.
- A cyberspace frame, medians and the most in a window: 186 lines (405),
  16 flat polygons (173), 3 shaded polygons (17), no points. `gpucpu`
  and every `gpuwhy` stay at 0: no call of a kind the GPU path doesn't
  draw.
- **Voxel objects split the frame.** In 39 of the 145 cyberspace
  windows a frame is 2 to 5 scenes, with as many flushes and nothing
  drawn by the CPU through the queue. The one flush left outside the
  queue is `FAUBJ_VOX` in `src/GameSrc/gameobj.c`: voxels are drawn
  straight onto the canvas, so each such object ends a scene, is waited
  for, and is drawn into GPU memory by the CPU. `gpuwait` goes from
  1.95 ms to 5.7 ms in those windows; the heaviest frame is 14.1 ms and
  still at the screen's rate. None came up in the station's captures.
- `check=0`, `leaks=0`, `hudcheck=0/669`, `gpufallbacks=0`, 4 dumps.
- No fight in cyberspace in this capture: shots and explosions there
  are not measured.

**A freeze at check 791**, in cyberspace. The log ends in the fifth
window of a three-core phase, after four complete ones: the GPU's
drawing wasn't running. What this step changed for that phase is the
way the 3D library's lines reach the 2D library, which is the same call
as before. Nothing in the log points at the renderer; the memory card
adapter is known to freeze the console now and then. Not explained.

#### Go for G11

The sending shared between the three cores, as written under "Go for
G10" in step G9: about 2 ms off the frames with many pieces, for the
hallway angle (59 fps in full screen, 55 paneled).

After that, to choose from: voxel objects through the queue, so that a
frame with them is one scene again (up to 3.7 ms in those frames); the
paneled view shown from GPU memory in place of copied (2.9 ms); the wait
when the help scan doesn't run (2.4 to 3.8 ms); the stalls.

### Step G11: the sending on three cores

Built and run on a Vita. Results at the end of this section.

At the hallway angle the main thread takes 5.1 ms to send a frame: 133
calls decided (`gpuprepare`, 1.25 ms), then cut into 354 pieces and
handed to the GPU (`gpusubmit`, 3.8 ms), while the two worker cores do
nothing.

#### The list is decided and cut by all the threads

`gpu_cut_list` in `src/Libraries/3D/Source/rastq.c`. A list of at least
`RASTQ_GPU_CUT_CALLS` (64) calls of a view the GPU draws is gone through
by all the threads before anything is handed over:

- each thread takes calls eight at a time from a shared counter, decides
  what each is (`gpu_classify`) and cuts it (`gpu_emit`), keeping the
  pieces in a sink of its own (`gpu_sink`: 4,096 pieces, 8,192
  vertices);
- the main thread then goes through the list in its order as before and
  hands each call's pieces to the GPU (`gpu_hand`).

The cutting code is the same: it puts a piece where it is told to
(`gpu_put`), the GPU or a sink. The pieces and their order are those of
one thread's cutting, so the picture is the same to the byte.

Left to the main thread, at the hand-over: lines and points (their
clipping goes through the canvas's clip rectangle), and a call whose
pieces don't fit its thread's sink. The scene's colour tables are the
one thing the threads share while deciding: `gpu_table_row` adds to
them one thread at a time. A shorter list is decided and cut call by
call as before.

#### Workers that are started and not waited for

`rastq_threads_kick` in `src/Libraries/3D/Source/rastqthr.c`.
`rastq_threads_run`, which the three-core replay uses, returns when
every worker has answered. A worker the system holds up answers
milliseconds late: in the captures the latest start within a second of
three-core drawing is 4 to 7 ms (the sound threads share the workers'
cores). A band of the picture has to be waited for. Here the wait would
cost more than the step gains.

`rastq_threads_kick` starts the job on the workers and returns. The
main thread takes calls like the others and waits only for calls a
worker has taken. A worker that comes late finds the counter past the
list's end and leaves without touching anything. Before the next job of
either kind the pool lets such a latecomer through
(`rastq_threads_settle`), and `gpu_cut_list` does so before it resets
what the job looks at.

A worker goes on looking for work for 25 ms after such a job
(`IDLE_AFTER_KICK_US`), then sleeps: the worker cores are busy only
while heavy frames keep coming. After a three-core band it is 100 ms, as
before.

#### Checked on the PC

- Every GPU scene of the test is drawn a second time with its lists cut
  by all the threads, whatever their length: 1,200 scenes, 59,960
  calls, about a third of them cut by the workers, and no picture
  differs from the one-thread one by a byte.
- The thread-sanitizer build of `tests/rastq/run.sh` goes through the
  same and reports nothing.

Not checkable on the PC: how fast the Vita's workers come to the job,
which decides the gain.

#### New in the log (profile build)

| field | what |
|---|---|
| `gpuprepare=` | now, for a shared list: the deciding and the cutting, by all the threads, as the main thread waits for it |
| `gpusubmit=` | for a shared list: the hand-over alone |
| `gpucut=a/b/c` | calls a frame decided and cut by the main thread and by each worker, of the lists they shared (all 0: the lists were short) |

#### What the capture will show

- At the hallway angle: `gpucut` with the workers taking about a third
  each, and `gpuprepare` + `gpusubmit` under the 5.1 ms of G9. If the
  workers' numbers are near 0 they come too late, and the main thread
  does the work as before: no gain, no loss.
- The hallway angle at the screen's rate in full screen (16.95 ms in
  G9) and paneled (18.1 ms).
- Nothing else changes: `gpudiff` as before.

#### Results (`docs/profiles-gpu/profile-step-11.txt`, `gpu.txt`, `gpudumps-step-11/`)

Medians of the 1 s windows, cut by the check numbers noted while
playing:

| | 0: three cores | 1: GPU | GPU: waiting for the screen |
|---|---|---|---|
| standing still | 46.8 fps | 63.5 fps | 5.9 ms |
| moving | 38.8 fps | 63.1 fps | 6.3 ms |
| fights | 37.1 fps | 62.3 fps | 5.2 ms |
| the hallway angle, full screen | 40.7 fps | 62.3 fps (16.06 ms; 59.0 fps in G9) | 1.2 ms |
| the hallway angle, paneled | 62.2 fps | 60.2 fps (16.6 ms; 55.3 fps in G9) | 0.1 ms |
| cyberspace, with fights | 62.8 fps | 63.1 fps | 7.6 ms |

- **The workers come to the job in time.** At the hallway angle
  `gpucut=52.8/46.6/46.7`: a third of the calls each, the main thread a
  few more. In cyberspace, whose lists of lines are long too,
  `78/68/73`.
- **The sending**, `gpuprepare` + `gpusubmit`, ms a frame:

  | | G9 | G11 |
  |---|---|---|
  | the hallway angle, 135 calls in 358 pieces | 1.25 + 3.82 = 5.07 | 1.48 + 1.87 = 3.35 |
  | standing still, 77 calls | 0.56 + 1.21 = 1.77 | 0.49 + 0.88 = 1.37 |

  What is left in `gpusubmit` is the hand-over, 0.8 ms of it the
  bitmaps copied into GPU memory (`gpuupload`).
- **Cyberspace with fights**, which G10's capture lacked: one scene a
  frame at the median, nothing left to the CPU, 63.1 fps.
- `check=0`, `leaks=0`, `hudcheck=0/745`, `gpufallbacks=0`, 4 dumps;
  4.6% of the compared pixels differ over the run, the station and
  cyberspace together.
- **Stalls**: 14. Eight have a sound effect's first decoding in them,
  three a resource load, one is the cyberspace level loading, and two
  (60 ms in a fight, 98 ms in cyberspace) show little or nothing in the
  timers there are.
- No freeze.

The hallway angle in the paneled view is at the screen's rate with
nothing to spare: 6 of its 55 windows are under 59 fps, the lowest at
57.0, and three cores do 62.2 fps there. Its frame, ms:

| | |
|---|---|
| sending (`gpuprepare` + `gpusubmit`) | 3.3 |
| the view copied out of GPU memory onto the screen (`viewout`) | 2.9 |
| blocked on the GPU, no scan to run meanwhile (`gpuwait`) | 2.7 |
| HUD | 0.5 |
| whole frame | 16.6 |

#### Go for G12

The paneled view shown as the full-screen view is: the GPU's canvas
laid over the view's window when the frame is put on screen, in place
of `Fast_Slot_Copy` reading it out of GPU memory every frame (2.9 ms
against 0.4 ms from ordinary memory). What the game draws over the view
on the screen (messages, the help overlay, the pause text) has to keep
showing: that is the part to plan first.

Expected, not measured: 2.5 ms off every paneled frame, the hallway
angle there from 16.6 ms to about 13.9.

After that, to choose from: voxel objects through the queue (up to
3.7 ms in the frames that have them); the wait when the help scan
doesn't run (2.7 to 3.9 ms); the stalls.

### Step G12: the paneled view without the copy

Built and run on a Vita. Results at the end of this section.

Since G6 a full-screen view is shown from the GPU canvas it was drawn
into, and the paneled view is copied out of that canvas into the screen
buffer every frame, because the game draws on the screen over it:
`viewout` 2.9 ms (the CPU reads GPU memory four times slower than its
own), against 0.4 ms when three cores draw the view in ordinary memory.

#### The view laid over the panels

`src/MacSrc/Shock.c`:

- `VitaShowView` takes where the view goes on the screen and accepts a
  view smaller than the screen; `fr_send_view`'s paneled branch asks it
  first and copies (`Fast_Slot_Copy`) only on a no;
- `SDLDraw`, when the view doesn't fill the screen, draws the screen
  buffer's picture as it always did and then the view's window from its
  canvas over it (`vita2d_draw_texture_part_scale`), at the place
  `Fast_Slot_Copy` would have put it (`Fast_Slot_Place`);
- `VitaSyncView` copies the view's window back into the screen buffer,
  for the callers that draw over the view without drawing it first (the
  help overlay, video mail, the wait cursor, the pause, another
  screen). They are the ones G6 gave the full-screen view.

The screen buffer still goes to the screen's texture every frame, as
before: the saving is the read of the canvas.

#### What the game draws over the paneled view

Looked for in the code, since anything drawn on the screen buffer inside
the view's window is now behind the canvas:

- the cursor: `rend_mouse_hide` draws it into the view itself every
  frame, in both views;
- the HUD, the weapon in hand and the window's border: drawn into the
  view by its draw callback;
- messages: `message_info` puts them on the message line, outside the
  window;
- the zoom rectangle from an object to a panel (`ZoomDrawProc`, drawn
  on the screen buffer around every `SDLDraw` for 286 ms): while one is
  under way `VitaShowView` says no and the view is copied as before;
- while the game is paused it says no too, as it did.

Not changed: in full screen the zoom rectangle hasn't shown with the GPU
renderer since G6, for the same reason. A copy of the whole view while
it plays would cost about 5 ms a frame.

Nothing of this runs on the PC: it is the Vita's screen code.

#### What the capture will show

- `viewout` in the paneled view at about 0.1 ms, as in full screen.
- The hallway angle in the paneled view with about 2.8 ms to spare
  (0.1 ms in G11), ahead of three cores again.
- To look at in the normal build, paneled view: the cursor across the
  window's edge, an object dragged out of the view, a panel opening
  with its zoom rectangle, pause, options, the help overlay, a video
  mail, the map, switching views, cyberspace and back.

#### Results (`docs/profiles-gpu/profile-step-12.txt`, `gpu.txt`, `gpudumps-step-12/`)

Medians of the 1 s windows, with the frame's time without its wait for
the screen:

| | 0: three cores | 1: GPU | GPU: waiting for the screen |
|---|---|---|---|
| start room, paneled | 62.4 fps (12.3 ms) | 62.8 fps (8.3 ms) | 7.6 ms |
| start room, full screen | 47.2 fps (21.1 ms) | 63.5 fps (9.9 ms) | 5.9 ms |
| the hallway angle, paneled | 61.5 fps (15.9 ms) | 62.8 fps (13.5 ms; 16.6 in G11) | 2.4 ms |
| the hallway angle, full screen | 40.0 fps (24.9 ms) | 62.1 fps (15.3 ms) | 0.8 ms |

- `viewout` in the paneled view: 0.09 ms on the GPU (2.92 in G11),
  0.44 ms on three cores.
- No window of the GPU renderer under 59 fps in either view.
- No glitch was reported from the paneled view: cursor, zoom rectangle,
  pause, overlays, view switches.
- `check=0`, `leaks=0`, `hudcheck=0/225`, `gpufallbacks=0`, 3 dumps,
  6.3% of the compared pixels differ.
- **Stalls**: 5. Three have a sound effect's first decoding in them,
  one a resource load, one is the capture's last second.

### Where the GPU renderer stands after G12

From the captures of steps G10 to G12:

| | three cores | GPU |
|---|---|---|
| start room, full screen | 47 fps | 63 fps |
| walking and fighting | 37 to 39 fps | 62 to 63 fps |
| the hallway angle, full screen | 40 fps | 62 fps |
| the hallway angle, paneled | 61.5 fps | 63 fps |
| cyberspace | 63 fps | 63 fps |

Every place measured is at the screen's rate with the GPU renderer, in
both views. The thinnest margin is the hallway angle in full screen:
0.8 to 1.2 ms to spare. Its frame, ms: 3.9 blocked on the GPU, 3.5
sending, 1.9 HUD, the rest the game and the traversal.

What is left, none of it started:

- **The wait when the help scan doesn't run** (2.5 to 3.9 ms), which
  after the first rooms is always: the level switches help off. Nothing
  else in the frame is free to run meanwhile; hiding it means handing
  the GPU its scene in parts while the view is still being traversed.
  Several scenes a frame cost GPU time of their own (the voxel frames
  of G10 suggest under 1 ms each), and the scene's buffers and tables
  would have to outlive one another. A large change with an uncertain
  gain.
- **Voxel objects** (`FAUBJ_VOX`): drawn straight onto the canvas, they
  split a frame into 2 to 5 scenes where they appear (cyberspace, still
  at 61 fps).
- **Translucent surfaces and brightness-shaded polygons**: the CPU's,
  in their place in the frame. None had come up in a capture until
  S11's, where a force bridge took the game to 13 fps, and to 4 from
  underneath: translucent polygons are the GPU's since S11c (Part 3),
  and the bridge is at the screen's rate. Translucent bitmaps came up
  in S12b's capture, on level 3, and are the GPU's with S13.
- **The zoom rectangle in full screen**: not shown with the GPU
  renderer since G6.
- **The stalls**: a sound effect decoded on the main thread for its
  first use, 0.1 to 0.2 s. Not the renderer; in `docs/TODO.md`. Counted
  and sized in the next section.

## Part 3: smoothness

With the game at the screen's rate, what was left to see was its
stutters, single long frames, and its waits. This part names them,
capture after capture, and takes them out one step at a time. Its
captures are in `docs/profiles-perf/`.

### After G12: the worker cores and the stutters

Two things seen in the normal build, at a steady 60 fps: the two worker
cores at 100% in the start room, about 15% down the slope to the left
door, and 100% again facing the wall between the two doors; and
stutters. The profile build was given what it lacked to show both (see
`docs/PROFILING.md`): each worker's time in jobs, awake without one and
asleep (`wjob`, `wspin`, `wsleep`), the GPU's lists by who cut them
(`cutlists=`), and a `spike` line for each stutter with that one
frame's times. It also stays on the GPU path now; the switch to three
cores every 5 seconds is off unless `VPROF_ALTERNATE` is set.

#### Results (`docs/profiles-perf/profile-stutters.txt`, `gpudumps-stutters/`; `docs/profiles-gpu/gpu.txt`)

A new game, the start room, down the slope and back, then on into the
level: 164 logged seconds, 35 spike lines.

**The worker cores.** Medians of the seconds in the game, full screen,
without a stall:

| | lists shared (84 s) | lists not shared (37 s) |
|---|---|---|
| calls recorded a frame | 128 (92 to 147) | 19 (4 to 52) |
| worker in a job | 6.9% | 2.9% |
| worker awake without a job | 89% | 10.0% |
| worker asleep | 4.5% | 87% |
| fps, and time to spare in the frame | 63.0, 4.2 ms | 63.2, 7.5 ms |

- **The 100% is waiting, not work.** Where the view's list has 64
  calls or more (`RASTQ_GPU_CUT_CALLS`: the start room, the wall
  between the doors) the workers get one job a frame and, watching
  25 ms for the next (`IDLE_AFTER_KICK_US`), don't sleep. Down the
  slope the list has 4 to 52 calls, there is no job, and they sleep.
  The change is sharp: seconds 77 to 79, 144 to 146, 165 to 166.
- The cores are busy all the same: the waiting is a loop that runs all
  the time, so it costs battery and heat. How much was not measured.
- The work is small: under 7% of a core, part of it the profile
  build's self-check drawing (the 2.9% where nothing is shared is all
  self-check).
- Two of the figures are the profile build's doing, and would not be
  there in the normal build:
  - with lists shared, the 4.5% asleep is one sleep a second, as long
    as the log write: the main thread is busy writing, no job comes
    for 25 ms and the workers doze off. Without a log they never
    sleep: PSVShell's 100%;
  - with lists not shared, the 10.0% awake is the 100 ms a worker
    watches after the self-check's job, once a second. Without a
    self-check they sleep throughout.
- What the sharing buys, from G11: 0.4 ms a frame standing still (1.77
  to 1.37 ms), in a frame that has 4.2 ms to spare. At the hallway
  angle it bought 1.7 ms where about 1 ms is spare, which is what it
  was built for.
- The 15% seen in the normal build is not the workers, as far as this
  shows. The audio thread was seen on all three cores (`acpu`) and the
  music's synthesis alone is 11% of one. Likely that; not measured per
  core.
- In the menus, where the workers have nothing to do, they read 100%
  asleep.

**The stutters.** 28 in the game (7 more in the menus, the intro and
loading):

| cause | count | the frame | of which the cause |
|---|---|---|---|
| a sound effect decoded for its first use (`sndload`) | 12 | 36 to 294 ms | 16 to 239 ms |
| a resource read from the card (`resload`) | 10 | 25 to 122 ms | 12 to 100 ms |
| a frame that draws 6 or 7 views (up to 1009 calls) | 2 | 36 and 87 ms | |
| the game's rules (`sim`), nothing in the timers | 1 | 37 ms | 26 ms |
| the panels (`ui2d`) | 1 | 29 ms | 18 ms |
| nothing standing out | 1 | 26 ms | |
| leaving the game | 1 | 175 ms | |

Of the 9 over 100 ms, 7 are sound, one a resource read (122 ms) and
one leaving the game. A sound's stutter has its resource read in it as
well (2 to 29 ms). Three resource reads are over 70 ms. The frames
with 6 or 7 views are drawn from the input handling; what they are is
not identified.

**The profile build's own hitches**, none of them in the fps or in the
spike lines, all of them seen on the screen:

- writing the log: 52 ms each second at the median, 26 to 134 ms
  (`logwrite`);
- self-check frames: 3 a second, the longest about 68 ms, up to 99
  (`skipped`);
- a frame dump: 3.8 to 4.0 s each, five in the session.

By eye the profile build stutters several times a second on its own:
the spike lines are what to go by, not the feel.

`check=0/112`, `leaks=0`, `hudcheck=0/170`, `gpufallbacks=0`, 5 dumps,
7.1% of the compared pixels differ (6.3% in G12).

### The next steps

Taken one at a time: a plan, a build checked on the PC, a capture
on the Vita (`docs/profiles-perf/profile-step-sN.txt`), its results
written here. S1 to S7 are done, with S7b and S7c after it. Of what
those left, S8 is done with S8b and S8c, and S10 with them for
both ways of changing level; S9 is done with S9b and S9c; S11 is
measured, and its one finding, the force bridges, is done with S11b
(whose shader did not compile) and S11c: the bridge at 60 fps from 4
to 20. That roadmap is finished. S12, after it, is what S11's capture
showed besides: frames that start late where the music is heavy. It
is measured, and done with S12b: the audio thread, on the main thread's
core, held the game up; kept on the two other cores, no frame starts
late and the 20 to 23 ms frames of a quiet second are gone. S13 is
what S12b's capture showed besides, a place on level 3 at 39 to 56
fps: translucent bitmaps drawn by the CPU. Done: the GPU draws them.
The place is heavy besides, and S13b cures what its capture pointed
at, a worker thread held up by the music: the main thread's wait for
it, up to 6.6 ms, is gone, and two thirds of the frames over 23 ms.

| step | what | for |
|---|---|---|
| S1 | Sound effects decoded before they are played | **done**: none of the 12 stutters is left, for 10.4 s of a background thread at start-up and 23.4 MB |
| S1b | Audio logs converted while they play, and read by a thread of their own | **done**: starting a log takes the main thread 0.6 ms, from 1.6 to 5.4 s; the voice starts 0.13 to 0.38 s later |
| S2 | The draw list shared with the workers only when the frame is short of time | **done**: the workers asleep where frames have time to spare, at the same fps; 23 mA less drawn from the battery, a little under 6% |
| S3 | The other stutters named: resource reads, time in the input handling, the frames with 6 or 7 views, the `sim` frames | **done**: every stutter has a name; three quarters are the side panels' update |
| S4 | A quieter profile build | **done**: the log costs the main thread 0.4 ms a second, from 52; the build that measures has no self-checks, and leaves the game alone |
| S5 | The side panels' update: find what its 16 ms are, then make it cheap | **done**: it was the panels' outlined text, drawn nine times each; now one draw the first time and a copy after. No stutter is a panel paying for its text any more, from 189 of 257 |
| S6 | Art read before it is needed | **done**: the art files held whole in memory, 14 MB read by a thread of its own at start-up. No stutter has a read from the card in it any more, from 18 of 32; 3.2 stutters a minute, from 8.5; a new game loads in 3 s, from 6 |
| S7 | Clicks: finding what is under the cursor, the inventory's redraw and its handler, the mail reader's | **done**: the view under the cursor is drawn for 21 rows of the 544, and no click in the 3D view stutters any more, from 29; 1.4 stutters a minute, from 3.2. The mail's clicks are named: 19 ms of `audiolog_play` |
| S7b | The game's own log, written in the middle of the game; the text strings read from the card; the panels' art unpacked when first shown | **done in part**: no read from the card in the game any more. A log line is 1.7 ms, not the 19 I took it for, and the panels' art was unloaded again when a game started |
| S7c | The wait cursor's frame at each log's start; the panels' art held; the logs' files opened at start-up | **done**: no mail or log opening stutters any more; 0.6 stutters a minute |
| S8 | Saving and loading a game | **done**, with S8b and S8c: a save 0.9 s from 9 to 12; a load 0.7 s; a new game 0.85 s from 3. Here: the game file copied in one read and one write (2.69 s to 0.32) |
| S8b | The game file's packing, at a save and at a change of level | **done**: the part of the file to close up read whole, packed in memory, written back whole: 6.35 s to 0.45. A save 1.9 s; cyberspace in 1.7 s, out 1.3 s |
| S8c | A level's pieces written one by one | **done**: a file being written collects its resources in memory and hands them to the card together: 1 s to 0.03. A save 0.88 s; cyberspace in 0.78 s, out 0.47 s |
| S9 | Stepping onto a new map square | **done**, with S9b and S9c. Measured here: the music changing zone, 37 to 70 ms at every crossing of a zone's edge, its score read, taken apart and thrown away each time; and one trigger, 25 ms |
| S9b | The music's scores kept once read | **done**: a crossing back into a zone 10 to 18 ms, from 37 to 70; no difference to the ear |
| S9c | The first time into a zone; the trigger near the start | **done**: the scores and their tables read ahead by a thread; the help's switch-off no longer saves the preferences in its frame. A crossing 3 to 8 ms, the first into a zone 12 to 15; the trigger's stutter gone |
| S10 | Changing level: cyberspace and the lift | **done**, by S8b and S8c: cyberspace in 0.78 s, out 0.47 s, from up to 7.2; the lift 0.37 s (S9's capture) |
| S11 | Heavy views: the render, and clicks there; the maps | measured: the maps cost nothing, a force bridge 13 fps, 4 from underneath, now 60 (S11b, S11c). 4 of the 6 stutters S7c left; a look-up under the cursor still 7 ms there |
| S12 | Frames shown a refresh late, where the music is heavy | measured: the audio thread at work on the main thread's core when it should wake (all 42 late frames); **done** with S12b, which keeps it on cores 1 and 2: no late frame in 18086, a frame over 20 ms in 1% of the quiet seconds from 27%. 25 of S11's 683 seconds have one; 21 of them among the 105 seconds with the music at 18% of a core or more |
| S13 | Translucent bitmaps, drawn by the CPU in GPU views | **done**: the GPU draws them, nothing is left to the CPU there and a frame is one scene; the slowest second 47.9 fps from 38.6. S13b, **done**: the workers' share of the GPU's list was held up by the music (a wait of over 3 ms in 45% of the seconds, none with the cure; a frame over 23 ms in 7% of the seconds from 21%). On level 3 (squares 27 to 29, 32 to 36) 39 to 56 fps for seconds, 2 to 8 ms a frame of CPU drawing and up to three scenes a frame |

The order is by what the player gains. S4 is the smallest and could go
whenever it helps. S5 to S7 came out of S3's capture, in the order of
what they weigh. S8 to S11 are what S7c's capture left, in the order of
what a player waits for; S9 is the smallest and can go whenever.

#### S1: sound effects decoded before they are played

Done; results
below.

When an effect is first played, two things happen on the main thread
(`play_digi_fx_master` in `src/GameSrc/digifx.c`): it is read from the
card (`ResLock`, 2 to 29 ms) and it is decoded and converted to the
mixer's format (`Mix_LoadWAV_RW` in `snd_sample_play`,
`src/MacSrc/SDLSound.c`, 16 to 239 ms). `DIGIFX.RES` holds 115 effects
(resources 201 to 315), all uncompressed 8-bit mono at 11 or 22 kHz,
1.9 MB in all, 128 s of sound; in the mixer's format (48 kHz, stereo,
16-bit) that is 24.6 MB, of a heap of 256 MB.

- **A background thread decodes them all once** (`snd_preload`), started
  from `digifx_init` when the resource files are open and the mixer is
  up. The logos, the menus and a level's loading (over 20 s in both
  captures) give it the time.
- **It reads the file itself**, through a handle of its own: the
  resource system has one file position and shared lists, and is not
  made for two threads. The main thread only says where each effect's
  bytes are (`ResFilePlace`, new in the resource library). So the
  start-up pays nothing, and a decoded effect's raw bytes are never
  loaded at all.
- **Nothing gets worse than before.** An effect is untouched, claimed
  or ready. Whoever gets to an untouched one first claims it: the
  thread on its way through the list, or the main thread for an effect
  played before the thread got there, which is then read and decoded as
  it always was. If the thread is on that very effect, the main thread
  waits for it, never longer than decoding it itself. A decode that
  fails leaves the effect untouched, to be tried at its next play.
- **Its priority is below the workers'** (two steps under the main
  thread's; the workers are one), on any core: it runs when a core has
  nothing better to do and can't hold up a worker the main thread waits
  for. In the game, if it isn't through, it gets what the main thread's
  wait for the screen leaves on core 0.
- **Left to the old way**: an effect that is compressed, or that a mod
  file replaces (it is then in another file).
- At exit the thread is told to stop and waited for, before the mixer
  closes.
- Not chosen: decoding everything on the main thread at start-up, 10 s
  more before the menu.
- Not run on the PC: there is no SDL on it. That `Mix_LoadWAV_RW`
  (SDL_mixer 2.8.2) is safe on a second thread is what its design says
  (it works on buffers of its own and only reads the opened device's
  format); on the Vita it decoded all 115 without a fault.

New in the log: `sndready=a/b` (effects decoded, of those to decode
ahead), `snddecode` (what the thread has spent, ms), `sndmem` (what its
effects take up, MB), and `alogload`, a timer on an audio log's loading
for S1b, in the second's line and in the spike line.

Results (`docs/profiles-perf/profile-step-s1.txt`): a new game, the walk
of the stutter capture, on into the level with fights, and eight audio
logs played. 512 logged seconds, 482 of them in the game, 50 spike
lines.

- **The thread is through before the first menu**: `sndready=115/115`
  in the first logged second (t=20, the intro).
- **No `sndload` in any second or spike line.** The stutter capture had
  12 in 154 s of play.
- **The cost**: 10.4 s of the thread's time (`snddecode`), 23.4 MB
  (`sndmem`). 63.1 fps at the median, as before.
- `check=0/241`, `leaks=0`, `hudcheck=0/569`, `gpufallbacks=0`, 8.4% of
  the compared pixels differ (7.1% in the stutter capture; S1 draws
  nothing).
- Not in the log, to confirm by ear and by hand: every effect at its
  volume and side in the normal build; quitting without a hang.

What stutters now, 44 in the game:

| cause | count | the frame |
|---|---|---|
| time in the input handling, nothing in the timers | 13 | 26 to 45 ms, 14 to 29 ms of it `input` |
| a resource read from the card | 13 | 25 to 117 ms, 12 to 98 ms of it the read |
| an audio log | 8 | 1.6 to 5.5 s |
| the panels (`ui2d`) | 3 | 25 to 27 ms |
| the game's rules (`sim`) | 1 | 41 ms |
| nothing standing out | 5 | 25 to 28 ms |
| leaving the game | 1 | 173 ms |

Of the 10 over 100 ms, 8 are audio logs. The frames with time in the
input handling are new as a group: the stutter capture was too short to
show them.

#### S1b: audio logs

`audiolog_play` (`src/GameSrc/audiolog.c`) opens
the logs' file, reads the whole log and converts it on the main thread
before it plays, every time it is played. The 118 logs are 53 MB (12 KB
to 1.3 MB each, 432 KB at the median), about 460 MB once converted:
they can't be made ready ahead as the effects are.

Measured in S1's capture (`alogload`), eight plays, each a freeze:

| | shortest | longest |
|---|---|---|
| the whole freeze | 1.55 s | 5.42 s |
| of which reading from the card | 0.10 s | 0.35 s |
| of which converting | 1.45 s | 5.07 s |

- Converting is over 90% of it: 75 to 80 ms for a second of sound (the
  effects' 128 s took the thread 10.4 s too).
- The same log played twice froze twice, 4.74 s each (t=195 and
  t=201): nothing is kept.
- So a thread alone would trade the freeze for a wait of up to 5 s
  before the voice starts. Converted block by block while it plays, a
  log starts at once and costs under a tenth of a core for as long as
  it plays: that is the way to go, with the read taken off the main
  thread.

Done, in two passes; results at the end.

**First pass: converted as it plays.** The cutscenes already convert
their sound as it plays (`src/GameSrc/cutsloop.c`): an `SDL_AudioStream`
fed from the mixer's callback with `AmovReadNextAudioChunk`, 8 KB of the
movie at a time. A log is a movie of the same format with no picture,
and was made to play the same way, read from its file block by block in
the callback. A capture of that (15 logs started, 340 s with a log
playing; not kept, the second pass's took its place):

- **The freeze is gone**: the frame in which a log starts is 103 to
  144 ms, from 1.6 to 5.5 s. Converting costs 7.1% of a core at the
  median while a log plays, and the game stays at 63 fps.
- **Starting a log still took 53 to 78 ms on the main thread**, 62 ms
  nearly every time whatever the log's size: the logs' file opened and
  its directory read, closed, and opened again for the reading, at
  every log.
- **The audio thread waited for the card.** Its longest call was 8.8 ms
  in a typical second, but over 43 ms, where the sound runs dry, in 24
  of the 340 seconds, up to 475 ms: each time a second in which the
  profiler was writing to the card (its log, up to 610 ms; a frame
  dump). The log's 8 KB read queues behind any other access to the
  card. The normal build has no such writes but has the game's own
  resource reads, 12 to 98 ms.

**Second pass, as built: the card is another thread's business.**

- **A log is read whole into memory by a thread of its own**
  (`alog_reader` in `audiolog.c`), started when the log is asked for:
  0.10 to 0.35 s for the sizes measured in S1. The audio thread gives
  silence until the log is there, then converts it from memory as it
  plays, a block in slices of 1 KB (a whole block is about 28 ms of
  converting in one go, a slice 3 to 4 ms). It never touches the card,
  nor the resource system (the end of a log used to close the logs'
  file from there).
- So the voice starts a tenth to a third of a second after it is asked
  for, and nothing on the card can make it gap after that. Playing
  while the rest is still being read would start sooner, for more code
  and a first second that depends on the card again; not chosen.
- **The logs' file and the barks' stay open**, reopened only when the
  language changes: finding a log is a look in the resource table
  (`ResFilePlace`). Their ids are in no other of the game's 41 files.
- **The main thread never waits for the reader.** A log cancelled while
  it is still being read is set aside and its memory freed once the
  reader has let go of it.
- **Stopping.** The music's callback is put back first, which waits for
  a call of the log's that is under way, and none comes after; only
  then are the stream and the movie let go of. The old code took a
  lock of its own around the buffer, but that lock was never created:
  nothing guarded it.
- **A log compressed in its file** (none is, in the three languages) is
  read whole through the resource system on the main thread, then
  converted as it plays.
- **The fade at the end.** Checking the two ways of reading against
  each other on the PC showed that `AmovReadNextAudioChunk` tapered the
  wrong 512 bytes: the end of the last block, which is padding when the
  last chunk is short, where the whole-track reader tapers the last 512
  bytes of sound. A log would have ended without its fade. It now
  tapers the same bytes; the cutscenes, which read this way already,
  get their fade back too.
- **Checked on the PC** (`tests/alog/run.sh <DATA folder>`): for every
  log and bark of the three languages, 675 in all and 201 MB of sound,
  the bytes read from the file at the place `ResFilePlace` gives and
  played block by block from memory are those of the log read whole as
  before. The conversion itself can't be run there: no SDL.

New in the log: `alogcpu` (the share of a core the audio thread spends
on a log while it plays), `alogcbmax` (its longest call in the second,
ms) and `alogdry` (its calls that gave silence because the log wasn't
read yet). `alogload` is what is left on the main thread to start a
log.

Results (`docs/profiles-perf/profile-step-s1b.txt`): 209 logged seconds,
200 of them in the game, 7 logs started, 134 seconds with a log
playing.

| | before S1b | first pass | second pass |
|---|---|---|---|
| main thread, to start a log (`alogload`) | 1.6 to 5.4 s | 53 to 78 ms | 0.5 to 0.7 ms |
| audio thread's longest call while a log plays (`alogcbmax`) | | 8.8 ms typical, up to 475 ms | 3.3 ms typical, 4.8 ms at most |
| seconds in which the sound could run dry (a call over 43 ms) | | 24 of 340 | 0 of 134 |

- **Starting a log costs the main thread 0.6 ms.** The first log and
  the first bark of the session are the exceptions, 43 and 60 ms: the
  one time their file is opened. Opening both when the game starts
  would remove that too; not done.
- **The audio thread never waits**: no call over 5 ms in any second,
  the profiler's log writes (up to 164 ms in this capture) included.
- **The voice starts 0.13 to 0.38 s after it is asked for**: 3 to 9
  silent calls of the audio thread (`alogdry`) at each start, and none
  anywhere else.
- Converting costs 6.4% of a core while a log plays (`alogcpu`);
  62.8 fps with a log and without.
- No sound effect decoded on the main thread, as since S1.
  `check=0/76`, `leaks=0`, `hudcheck=0/337`, `gpufallbacks=0`, 8.5% of
  the compared pixels differ.
- Not in the log, to confirm by ear and by hand: the voice clean from
  start to end, the music back after each log, cancelling and
  restarting, a change of language, a cutscene's ending.

What is left when a log starts is not the log's, and goes to S3: the
frame is still 49 to 63 ms, 31 to 46 ms of it in the input handling
(once 39 ms in the game's rules), with `alogload` under 1 ms. Picking
a log from the reader also has a resource read of 54 to 66 ms in the
panels, and input-only frames of 93 to 161 ms just before.

#### S2: the draw list shared only when the frame is short of time

Done; results below.

Any list of 64 calls was cut by all three threads (`gpu_cut_list` in
`rastq.c`), which keeps the two workers awake all the time: 0.4 ms
saved in the start room's frame, which has 4.2 ms to spare, 1.7 ms at
the hallway angle, where 1 ms is spare. A count can't tell the two
apart (131 calls and 133).

- **The signal is the time the main thread works on a frame**, from the
  end of one hand-over to the screen to the start of the next
  (`SDLDraw` in `Shock.c`: two clock reads a frame, in the normal build
  too). What it has to spare is the screen's 16.67 ms less that. The
  wait for the screen, the first idea, misleads: after any long frame
  the next ones don't wait at all, cheap as they are, until the display
  has frames in hand again.
- **The rule** (`rastq_gpu_frame` in `rastq.c`). The queue starts at
  rest: the main thread cuts its lists alone and the workers sleep. It
  shares as soon as three frames in a row have less than 1.5 ms to
  spare; a long frame or two on their own are a stutter, not a busy
  scene. It rests
  again after 120 frames (2 s) with more than 3.5 ms to spare while
  sharing. A rest given up within a second was one too early: the next
  waits twice as long, up to 64 s, and 2 s again once a rest has held
  10 s.
- With the figures measured so far: the start room rests (3.8 ms to
  spare cutting alone); the hallway angle shares and stays shared in
  both views (1.0 and 2.4 ms to spare when shared).
- Lists under 64 calls are cut by the main thread as before. The
  three-core software renderer, used when the GPU can't be, is not
  touched.
- **Checked on the PC** (`tests/rastq/run.sh`): the rule fed made-up
  frames (an easy scene stays at rest, stutters included; a tight one
  shares from its third frame and stays; a scene that is easy only
  when shared has its rests spaced out, doubling to the cap), and the
  queue's pictures as before.
- **The measurement came with the change**: standing in one place,
  with the lists always shared (variant 0, as before S2) against the
  rule (variant 1), turn about, with the battery's remaining capacity
  in the log. The roadmap had the battery measured first, start room
  against slope, but those differ in what the GPU draws too. The SDK
  gives no reading of the current: the drain is the slope of the
  capacity (`scePowerGetBatteryRemainCapacity`, mAh).

New in the log: `batt` (mAh), `battmv`, `batttemp`, `charging`, and
`rested:` in `cutlists=` (lists long enough to share that the main
thread cut alone because the queue was at rest).

A first capture, with stretches of 5 minutes and a trigger of two
frames (not kept; the second took its place), showed three things:

- **At the hallway angle the rule shares**, all the time and with no
  attempt to rest: 61.9 fps in full screen and 62.9 paneled, against
  62.1 and 62.8 in G12. Walking and fighting, no flip-flopping.
- **Two frames in a row were not enough to tell a busy scene.**
  Standing still, sharing came on 23 times in 300 s, 2 s each time:
  the profiler's log write and a self-check frame each make one long
  frame, and about every 13 s they fall side by side. The workers were
  awake 29% of the time for it. Hence three frames.
- **Five minutes are too short for the battery.** Its gauge moves 3 to
  5 mAh every 35 s, and once jumped up 11 mAh, a correction of its own.
  The rule read 15 to 20 mA less, about one gauge count a stretch.

Results (`docs/profiles-perf/profile-step-s2.txt`): 2732 logged seconds
standing in the start room, unplugged, with the three-frame trigger and
stretches of 10 minutes.

| stretch | variant | battery drain | workers awake | fps | to spare |
|---|---|---|---|---|---|
| 1 | the rule | 385 mA | 11% | 63.4 | 5.1 ms |
| 2 | always shared | 405 mA | 95% | 63.3 | 5.6 ms |
| 3 | the rule | 384 mA | 11% | 63.4 | 5.1 ms |
| 4 | always shared | 411 mA | 95% | 63.4 | 5.6 ms |
| 5 (5.5 minutes) | the rule | 397 mA | 11% | 63.4 | 5.1 ms |

- **The rule draws about 23 mA less, a little under 6%**: 384.5 mA
  against 408 over the four full stretches. The two stretches of each
  variant agree within 1 and 6 mA, and one gauge count is worth 6 mA
  over 10 minutes. The fifth stretch is half as long and twice as
  uncertain. No correction of the gauge in this run.
- At those drains a full 2200 mAh battery lasts about 5 h 43 standing
  in the start room, against 5 h 24.
- In the profile build the rule still leaves the workers awake 11% of
  the time: the self-check's job, once a second, and the 100 ms they
  watch after it. With nothing to wake them, as in the normal build,
  the saving would be somewhat larger: near 7% by proportion, not
  measured.
- **The three-frame trigger holds**: standing still, sharing came on
  2, 1 and 0 times in the three stretches under the rule.
- **No cost in frame rate.** Cutting its lists alone costs the main
  thread 0.45 ms (`gpuprepare` + `gpusubmit` 1.80 ms against 1.37), in
  a frame with 5.6 ms to spare.
- `check=0/2477`, `leaks=0`, `hudcheck=0/2069`, `gpufallbacks=0`.
- The hallway angle was not visited again: the trigger's third frame
  only delays the sharing by 17 ms.

#### S3: the other stutters

In S1's capture: 13 have a resource read
from the card in them (12 to 98 ms); 13 have 14 to 29 ms in the input
handling with nothing in the timers; the stutter capture also had 2
frames that draw 6 or 7 views from the input handling, and 26 ms in the
game's rules. S1b's capture adds what surrounds the start of a log: 31
to 46 ms in the input handling in the frame it starts, and, when it is
picked from the reader, a resource read of 54 to 66 ms in the panels
and input-only frames of 93 to 161 ms. None can be fixed before it is
named.

Done; results below. One mechanism in place of a timer for each
suspect: the profiler keeps the six longest pieces of each frame that
it is told about, and a stutter's line ends with them (`notes=`, see
`docs/PROFILING.md`).

- **Every timer there was names itself**: `VPROF_RUN` passes its
  statement's own text. That alone names the parts of the game's rules
  (`ai_run()`, `gamesys_run()`, ...), the panels' updates
  (`inventory_draw()`, `mfd_update()`, ...), `input_chk()` and
  `pump_events()`.
- **Who handled an input**: the two places where the interface library
  calls into the game, a region's handler (`UI/Source/event.c`) and a
  hotkey's function (`hotkey.c`), with the function's address and the
  event's type or the key.
- **Which resource was read**: its id and size (`RES/Source/resload.c`).
- **Who asked for a 3D view**: `fr_rend` (`frmain.c`), with its
  caller's address, for the frames with 6 or 7 views.
- **The three parts of `input_chk()`**: polling the interface, the
  mouse, the movement keys.
- Addresses are turned into names on the PC, from the build's own file.
- Profile build only. In the normal build the code of the six files
  touched is the same as before, instruction for instruction.

Results (`docs/profiles-perf/profile-step-s3.txt`): 717 logged seconds,
710 of them in the game, with the panels, the inventory and the mail
reader used on purpose. 266 spike lines; the addresses in their notes
are 0x3a000 above those of the build's file (its `sym` line). 260 are
in the game, 3 of them a level being loaded (1.9 to 7.9 s). The other
257, each counted once:

| what | count | the frame | the named piece |
|---|---|---|---|
| the side panels' update | 189 | 25 to 44 ms | `mfd_update()`: 16.5 ms at the median, 11 to 20 |
| art read when first seen | 15 | 28 to 104 ms | resource reads inside `render_run()`, 10 to 82 ms a frame; twice from the hacked cameras' views |
| the inventory redrawn | 14 | 25 to 35 ms | `inventory_draw()`, 9 to 22 ms |
| a click in the 3D view | 13 | 26 to 58 ms | `view3d_mouse_handler`, 8 to 37 ms: `fr_get_at` draws the view again to find what is under the cursor, for 3 to 5 views in the frame |
| a click in the inventory | 5 | 34 to 166 ms | `inventory_mouse_handler`, 16 to 86 ms |
| the mail reader's input | 5 | 25 to 31 ms | `email_invpanel_input_handler`, 11 to 20 ms |
| creatures' art read when first needed | 4 | 32 to 44 ms | resource reads inside `ai_run()` and `advance_animations()`, 18 to 29 ms |
| the options panel | 4 | 45 to 186 ms | `opanel_kb_handler` |
| other | 8 | 26 to 39 ms | two more panel clicks (`mfd_view_callback_full`), six with nothing standing out |

- **The side panels are three quarters of it.** Any redraw of a side
  panel costs about 16 ms, a whole frame. 128 of the 189 came between
  t=600 and t=660, one every 0.27 s, while a panel was updating
  continuously. Which panel function, and what in it, the notes don't
  say: `mfd_update()` is one piece to them. That is S5.
- **The resource reads are slow by the read, not by the byte.** 42
  reads were noted outside the level loads, 9 KB at the median, and
  each takes 6.5 ms at the median: a frame that needs eight of them is
  over 50 ms. They are object and creature art (`OBJART2.RES`,
  `OBJART3.RES`: 28 reads), wall textures (`CITMAT.RES`: 7) and 3D
  models (`OBJ3D.RES`: 5). That is S6.
- **The frames with 6 or 7 views of the earlier captures are clicks in
  the 3D view**: the game finds what is under the cursor by drawing the
  view again, small, once or several times (`fr_get_at`). With the
  inventory's redraw and its handler, that is S7.
- 22 stutters a minute, against 5.5 in S1's capture: the panels were
  used on purpose this time.
- `sndready=115/115`, no `sndload` or `alogload` in any spike line.

#### S4: a quieter profile build

Done; results below. The profile
build hitched on its own: 25 to 135 ms each second for its log write,
about three self-check frames a second of 68 ms, 4 s for each frame
dump. None of it in the figures, all of it on the screen, and some of
it in what is measured (S1b's audio thread waiting for the card, S2's
workers woken once a second).

- **The log is written by a thread of its own.** The main thread builds
  the second's lines in memory and queues them (64 KB of queue; a line
  that finds it full is dropped and counted, never waited for); the
  writer takes them out four times a second, through a file it keeps
  open and reopens every 30 writes. At exit what is left is written.
- **Two flavours**: `./build.sh profile` builds without the self-checks
  and the frame dumps, for measuring; `./build.sh profile checks`
  builds with them, into `build-profile-checks/`, for work on the
  drawing code. Without them their code isn't compiled in at all
  (`VITA_PROFILE_CHECKS`), so the measuring flavour's queue and HUD
  code are the normal build's plus the timers.
- In the log, `logwrite` is now what the lines cost the main thread;
  `logio` is the writer's longest write, `logdrop` the lines dropped.
- The normal build's code is unchanged.

Results (`docs/profiles-perf/profile-step-s4.txt`): a session of 627
logged seconds with the measuring flavour, then one of 391 with the
checks flavour.

| | before S4 | measuring flavour | checks flavour |
|---|---|---|---|
| the log's cost to the main thread, each second (`logwrite`) | about 52 ms, 25 to 135 | 0.37 ms, 0.69 at most | 0.38 ms, 0.70 at most |
| seconds with a frame drawn twice for a check (`skipped`) | nearly all, about 3 frames in each | none of 627 | 380 of 391 |
| workers, where the lists aren't shared | asleep 88%, watching 10% | asleep 100% | asleep 88%, watching 10% |
| `fps` | 62 to 63 | 60.0 | 61.2 |

- **The log no longer holds the screen up.** The writing itself is the
  writer thread's: 26 ms at the median, up to 155 in the first session
  and 311 in the second (`logio`). No line was dropped.
- **The measuring flavour does nothing of its own to the game**: no
  frame skipped, no check run (`check=0/0`), the workers fully asleep
  where frames have time to spare.
- **Its `fps` reads 60.0, the screen's rate.** The 62 to 63 of every
  capture before it came from the checks: a check's long frame is left
  out of the average, the short frames that catch up after it are not.
  The frame's time was the figure to go by; the fps now agrees with it.
- **The checks flavour still checks**: `check=0/118`, `leaks=0`,
  `hudcheck=0/1032`, 8.7% of the compared pixels differ.
- **What stutters is the game's own**: 61 in the first session, 6.3 a
  minute, every one with a name. The side panels' update 19, art read
  when first seen 11, the inventory's handler 8, the mail reader's 8,
  creatures' art 5, clicks in the 3D view 4, the rest 6. S5 to S7's
  list, as S3 left it.
- The addresses of the notes were 0 above the file's in the first
  session and 0x79000 in the second (0x3a000 in S3's capture): the
  offset is to be taken from each session's `sym` line.

#### S5: the side panels' update

Done; results below.

What the 16 ms are was in the captures already. In full screen a
panel's text is drawn with an outline, so that it reads over the 3D
view (`mfd_full_draw_string` in `src/GameSrc/newmfd.c`, then
`draw_shadowed_string` in `tools.c`): the string eight times in the
outline's colour, a pixel around its place, then once more as the
text. Step G9 kept such texts as pictures for the HUD's messages
(`hudkeep_outlined`), but it kept 8, made a picture only of a text
asked for twice, and made it by the nine draws, clearing and searching
a canvas of 1024x128 each time. A panel has more than 8 texts: redrawn,
it found none of them kept and paid nine draws for each. In the seconds
with a side-panel stutter, 49 texts a second were drawn afresh in S3's
capture and 20 in S4's; under 2 in the other seconds.

- **One draw, not nine.** The outline is the text's shape grown by a
  pixel in the eight directions. The text is drawn once, then the
  outline's colour goes on every empty pixel that touches it
  (`text_grow` in `src/GameSrc/hudkeep.c`): the nine draws' pixels.
- **Kept from the first time, and 256 of them.** A picture now costs
  less than the nine draws, so every text gets one when first asked
  for. A text that changes all the time (a counter) leaves a picture
  per value, never asked for again: past 128 of those, they are the
  ones a new text replaces, so that they don't push out a panel's
  texts between two of its redraws. (96 and 32 in the first capture:
  too few, see the results.)
- **Less to clear and to search.** The draw says how far the text goes
  (`ss_scale_string_sized` in `gr2ss.c`): only that much of the scratch
  canvas is searched, and cleared afterwards.
- **A flaw of G9's keep, found by the new test and fixed**: a text too
  large for the scratch canvas was only noticed when its pixels reached
  the canvas's far edge. One that ended on a blank line or a space was
  kept cut. With the text's size known, what doesn't fit is left to the
  nine draws, always. The captures' `hudcheck=` never caught such a
  text (0 of 1,032 in S4's): it takes one over 1024x128 pixels.
- What stays with the nine draws: a text too large, one with characters
  its font hasn't, palette colour 0, an unusual fill type.
- **Checked on the PC** (`tests/hudkeep/run.sh <DATA folder>`): the
  game's own `hudkeep.c` and `gr2ss.c`, with the 12 one-colour fonts of
  `GAMESCR.RES` in each of the four screen modes, 400 texts and 1,200
  askings for each at changing places, a sixth of them at the canvas's
  edges or past them: 57,600 askings, 48,760 copied from a picture,
  each the same bytes as the nine draws, and the scratch canvas left
  clean every time. The rest were left to the nine draws, with nothing
  drawn. (The six other fonts have colours of their own and are never
  outlined.)
- **New in the log** (profile build): `outlined=`, the time of all the
  outlined text of a frame, in the second's line and in a stutter's;
  the note `mfd@address#function/panel`, which names the panel function
  a redraw ran.
- Unlike the profiler's steps, this one changes the normal build: it is
  the game's text drawing.

Results, from two captures. `docs/profiles-perf/profile-step-s5.txt`:
a session of 328 logged seconds with the checks flavour, then one of
339 with the measuring flavour, 302 of them in the game.
`profile-step-s5b.txt`: the measuring flavour again, 245 seconds, 225
in the game, with 256 texts kept in place of 96. Panels, inventory and
mail reader used on purpose in all of them.

| | S3 | S4, measuring | S5, measuring | S5b |
|---|---|---|---|---|
| seconds logged | 717 | 627 | 339 | 245 |
| stutters in the game | 257 | 61 | 29 | 32 |
| of them, a side panel's redraw | 189 | 19 | 2, both art read from the card | 1, the same |
| `mfd_update()` in those | 16.5 ms at the median | 14.7 ms | 12 and 66 ms, the read 11 and 62 | 64 ms, the read 61 |
| a click in the inventory or on a panel, a page of a mail by every sign: its handler | | 30 to 40 ms for most | 24 ms | 24 ms |

- **The pictures are the nine draws' on the console**:
  `hudcheck=0/789` in the checks session, with `check=0/111` and
  `leaks=0`.
- **No panel redraw stutters for its text any more**, in either
  capture. The stutters that still have `mfd_update()` in them are a
  picture read from the card inside a panel function: the weapon's
  (`mfd_weapon_expose`, 9 KB, 11 ms) and the portrait of a mail's
  sender (`mfd_emailmug_expose`, 119 KB, 61 to 62 ms). Both are S6's
  kind.
- **Outlined text takes 0.07 to 0.08 ms of a frame on average**
  (`outlined=`), 0.09 to 0.14 ms in the median second's worst frame.
- **What still costs is a page of a mail: 5.3 to 6.5 ms of text, at a
  click.** 8 clicks of the first measuring session and 7 of S5b have
  it, with 24 to 42 pictures made at each. The mail reader draws
  a message word by word, each word an outlined text of its own
  (`email_draw_string` in `src/GameSrc/email.c`), and a click on
  "more", in the inventory panel or on the mail panel's buttons, shows
  a page of new words: new pictures, about 0.17 ms each. The capture
  doesn't say which texts were made; what points to the mail is the
  count, the two handlers (`inventory_mouse_handler`,
  `mfd_view_callback_full`), and the sender's portrait read before
  these clicks begin in each session.
- **That page cost about three times as much before**: these handlers
  took 30 to 40 ms in S4's capture for most of their stutters, and 24
  now. The mail reader's key handler (`email_invpanel_input_handler`),
  11 stutters of 13 to 19 ms in S4's capture, is in neither of S5's.
  The 18 ms of the handler that aren't text are S7's to look at.
- **A fault in the replacement rule, found on the way and fixed, but
  not what the capture showed.** As first built the keep held 96 texts
  and, past 32 asked for once, a new text replaced the oldest of
  those. A page of more than 32 texts is drawn only when it changes,
  so its texts have all been asked for once, and with the keep full
  they replace one another at every redraw, for good: the PC test now
  has the case, and on that rule a page of 100 texts made 100 pictures
  at each redraw, and 100 again after a counter's 1,000 values. With
  256 kept and the rule holding from 128, none after the first
  showing. I first took the first capture's bursts for this. S5b says
  otherwise: its bursts begin at t=94, with 128 pictures made in all
  and nothing yet replaced.
- The other stutters are the list S3 left. In S5b: art read when first
  seen 17 (`render_run()`), creatures' art 4 (`ai_run()`,
  `advance_animations()`), clicks 10 (the 7 mail pages, the portrait's
  read among them), one in `physics_run()`.

#### S6: art read from memory, not from the card

Done; results below.

What the reads are, from the four captures since S3 and the data files
themselves:

- **It is the card, not the size and not unpacking.** The stutter
  frames read 77 different resources, 210 times. All but the panels'
  art (`MFDART.RES`) are stored unpacked, and 1 KB takes 5 ms,
  sometimes 12 to 20: finding the place in the file is what costs.
- **They come from few files, and small ones**: `OBJART3.RES` (80 of
  the reads), `CITMAT.RES` (40), `OBJART2.RES` (39), `OBJ3D.RES` (23),
  `TEXTURE.RES` (14), `MFDART.RES` (10), then `VIDMAIL`, `GAMESCR`,
  `OBJART` and `HANDART`. With the panels' art in the two other
  languages and `SIDEART`, 13 files and 14 MB. The game has 256 MB of
  heap.
- **A level change throws the creatures' art away**:
  `free_dynamic_memory` (`src/GameSrc/cybmem.c`) closes `texture`,
  `handart`, `objart2` and `objart3`, which frees what was read from
  them, and they are opened again for the new level. That art is read
  again piece by piece on every level.

So there is no guessing at what a level needs: the files themselves are
kept in memory.

- **A thread reads them once, at start-up** (`src/GameSrc/resimage.c`),
  below the game's threads as the one that decodes the sound effects,
  128 KB at a time through a handle of its own. Most read first, the
  textures last: a level's are loaded with the level anyway.
- **The resource library reads a file from its copy**
  (`ResUseImage` in `src/Libraries/RES/Source/resfile.c`): the file's
  handle is opened on the memory (`fmemopen`) in place of the card, so
  everything that reads through it is as it was, the directory at
  opening and the packed resources too. A file already open when its
  copy arrives is switched over there and then. Only files opened to
  be read: a saved game is never one. The library has no thread of its
  own: the main thread hands it the copies, once a frame and before a
  level's files are opened again.
- **Until its copy is there a file is read from the card**, as before:
  the menus and the first level's load, by and large. A file that
  can't be read or has no memory stays on the card.
- **Checked on the PC** (`tests/resimage/run.sh <DATA folder>`): for
  the 13 files, 1,668 resources, each read from the disk, then from
  the copy arriving while the file is open, then from a file opened on
  its copy: the same bytes, and the same tables for the compound ones.
- **New in the log** (profile build): `resimg=a/b`, `resimgmb` and
  `resimgms` (the files in memory, their size, the reader's time); a
  read from a copy is noted `resm#id/bytes`, one from the card still
  `res#`.
- What does not change: which bytes a read gives, what is kept loaded
  and when it is dropped.

Results (`docs/profiles-perf/profile-step-s6.txt`): the measuring
flavour, 989 logged seconds, 950 in the game. Two new games, the first
level walked through at length, mails read, and cyberspace entered and
left twice. The lift wasn't found, but cyberspace is a level change
that closes and reopens the creatures' files as the lift does
(`DYNMEM_PARTIAL` in `go_to_different_level`,
`src/GameSrc/saveload.c`); the lift closes the textures' file as well,
and that one was reopened by the second new game.

| | S5b | S6 |
|---|---|---|
| seconds in the game | 225 | 950 |
| stutters in the game | 32, 8.5 a minute | 50, 3.2 a minute |
| of them, with art read from the card (`render_run()`, `ai_run()`, `advance_animations()`) | 18 | 0 |
| a new game's load | 5.8 to 6.2 s | 3.1 and 2.9 s |
| the mail sender's portrait | 61 ms, read from the card | 21 ms, unpacked from memory |

- **The files are in memory before the first frame**: `resimg=13/13`,
  13.9 MB, read in 5.7 s during start-up. The sound effects' thread
  took 0.3 s longer beside it (10.9 s).
- **No stutter has a read from the card in it any more**, on the first
  level or after either return from cyberspace. The frame's reads now
  average 0.003 ms.
- **A new game loads in half the time**: the level's own art comes from
  memory too (the objects' art, 752 KB, in 3.6 ms from about 250; a
  texture of 288 KB in 1.2 ms from about 80).
- **The portrait still makes its click a stutter**: read from memory,
  it has to be unpacked, 21 ms. The panels' art is the one packed
  file. With the mail's page it is S7's.
- **What stutters now is clicks: 42 of the 50.** 29 in the 3D view
  (`view3d_mouse_handler`, most with `fr_get_at` drawing the view
  again), 10 in the inventory, mail pages mostly, 1 on a panel, 2 in
  the options panel. The rest: 2 in `physics_run()` (27 and 29 ms, as
  in S5's captures), 3 in the frame's presentation, 3 with nothing
  standing out. That is S7.
- Left over, none of it in a stutter but the portrait: 21 of the 950
  seconds have a read of over 1 ms, 5 ms typically, 13 once. The log
  names a read only in a stutter's line. The text strings' file isn't
  among the 13 and is the likely one. And the title screen's art
  (`INTRO.RES`, 14 and 6 ms) is read from the card each time the main
  menu shows.
- Long frames that are not stutters: the levels' loads (cyberspace in
  2.7 and 3.1 s, out in 7.1 and 2.8 s), 9.4 s in the options panel's
  handler at t=547 (a save or a load), and 19 s at t=339 in a click
  that played something through, the screen being refreshed all the
  while.

#### S7: clicks

Done; results below. Clicks are 42 of the 50 stutters
of S6's capture, and two different things.

**A click in the 3D view (29 of them).** To find what is under the
cursor the game draws the view again in flat colours, one for each
object, and reads the pixel there (`fr_get_at` in
`src/GameSrc/frutil.c`). That drawing is the whole view, 960x544, on
the CPU's three threads, the workers woken for it: 10 to 20 ms each
time. A double-click does it up to three times (`view3d_dclick`), a
shot twice (`weapons.c`). Of all those pixels it reads the one under
the cursor and, failing an object there, a spiral of some five pixels
around it (`check_around`).

- **Only the rows that are read are drawn**: the cursor's and ten
  either side, 21 of 544. The queue already replays a view in bands of
  rows and skips every call that doesn't reach a band;
  `rastq_rows_next(top, bot)` asks it for one band and no more, on
  the calling thread, the workers left asleep. A call whose mapper
  knows nothing of bands is drawn whole when it reaches the rows and
  left out otherwise.
- The walk through the level's geometry is still done for each look-up:
  it is the filling that goes.
- A view picked for a self-check is replayed whole, so the checks
  flavour checks what it did.
- The on-screen help's scan is a view of its own, read all over, and is
  not touched.
- **Checked on the PC** (`tests/rastq/run.sh`): each of the test's
  scenes replayed for a few rows at a random place, on one thread and
  with three set up: those rows are the whole replay's, pixel for
  pixel; and under the self-check the view comes out whole.

**A click in the inventory or on a panel (13).** Opening a mail takes
24 ms in its handler, 6 of them the page's text (S5). The other 18 are
in no timer, and the code doesn't say: opening a mail starts its audio
log, redraws the inventory's page, skips through the message's lines,
changes a side panel, makes a cursor and draws the text (`read_email`,
`email_draw_text` in `src/GameSrc/email.c`). So, in the profile build
only:

- timers with notes on each of those, on the inventory page's own
  functions (`inv_select@`, `inv_use@`, `inv_drop@`) and on the panel
  handler a click on a side panel reaches (`mfd_handler@`);
- a stutter's line keeps its 10 longest pieces, from 6: the six were
  taken by pieces that hold one another (`input_chk()`, `uiPoll`, the
  handler), and what is inside fell off the end;
- `resmax=`, the slowest read of each second by its id, for the 5 ms
  reads S6 left unnamed.

Results (`docs/profiles-perf/profile-step-s7.txt`): the measuring
flavour, 560 logged seconds, 548 in the game, with clicks in the 3D
view, shots, mails and the options panel.

| | S6 | S7 |
|---|---|---|
| seconds in the game | 950 | 548 |
| stutters in the game | 50, 3.2 a minute | 13, 1.4 a minute |
| of them, a click in the 3D view | 29 | 0 |
| seconds with such a click: its handler's worst time, median and ninth decile | 5.2 and 17.0 ms (113 seconds) | 3.7 and 7.8 ms (84 seconds) |

- **No click in the 3D view stutters any more.** The PC test's full
  run: 12,000 views replayed for a few rows, none differing in them.
- **Opening a mail is `audiolog_play`: 19.1 to 19.4 ms of the 24**, the
  same every time, in all 8 of these stutters (7 from the inventory,
  1 from the mail's panel); 71 ms the first time, with the logs' file
  to open. The page's text is 4.2 to 5.0 ms. The log's reading and
  starting, S1b's timer inside it, is 0.6 ms: the 19 ms are around it.
- **The 3 stutters of the options panel, 28 to 29 ms, are its volume
  sliders.** They and `audiolog_play` have this in common: a change of
  the music's volume (`UpdateVolumeXMI` in `src/MacSrc/Xmi.c`), which
  begins by writing a line to the game's own log. That log was at
  level INFO, and each line goes out unbuffered, in three pieces, to
  the console's debug output (`log_log`). The capture names the
  function, not the line: that a line costs 19 ms is what S7b
  measures.
- **The 5 ms reads S6 left are the text strings**: `resmax=` names
  ids 9a3 to aa1, from the card, 4.7 to 5.1 ms and 13.3 once. A mail's
  text is read when it is first opened.
- The two others: one in `physics_run()` (28 ms, in every capture some
  100 s into the game), one with the security cameras' views.
- Long frames that are not stutters: 12 s in the options panel (a save
  or a load), cyberspace in 1.5 s and out in 7.2 s, a video mail's
  3.8 s.

#### S7b: what the timers named

Done in part; results below.

- **The game's log at warnings and errors only** (`main()` in
  `src/MacSrc/Shock.c`): the INFO lines, which come in the middle of
  the game, are no longer written.
- **The log is timed** (profile build): `applog=lines:ms` and
  `applogmax=address:ms` in the second's line, a note `log@address` in
  a stutter's; and notes on what `audiolog_play` does around its log,
  should the 19 ms be elsewhere.
- **The text strings in memory**: the three languages' files, 0.5 MB,
  join the art files of S6 (16 files now).
- **The panels' art unpacked when its file is opened**, at start-up
  and at a change of language (`resimage_load_all`): the one packed
  file, 19 pictures, 250 KB. The mail sender's portrait cost 21 ms the
  first time it was shown.

Results (`docs/profiles-perf/profile-step-s7b.txt`): the measuring
flavour, 263 logged seconds, 224 in the game, mails and logs opened one
after another: 13 stutters.

- **No read from the card in the game any more**: `resmax=` names only
  reads from memory, the longest the portrait's.
- **A line of the game's log is 1.7 ms, not 19.** Three were written in
  the whole capture (`applog=`), each 1.71 to 1.76 ms. The reading S7
  ended on was wrong. The log stays at warnings only, which is what it
  is worth: 1.7 ms for each INFO line that would come in the game.
- **Opening a mail or a log is still 13 to 17 ms of `audiolog_play`**
  (19 with its log line), in 7 of the 8 such stutters; the eighth is
  the first log of the session, 73 ms, which also opens the logs' file
  of 50 MB and reads its list. None of the pieces with a timer is
  it: the stopping of the log before, the music's volume and the two
  hooks are each under 3 ms where they show at all, the reading and
  starting 0.6 ms. What is left has no timer: `begin_wait()` and
  `end_wait()`, the wait cursor around those 0.6 ms. `begin_wait()`
  (`src/GameSrc/tools.c`) presents a whole frame to show its cursor,
  and a present waits for the screen, up to 16.7 ms. That too is read
  from the code, not measured: S7c times it.
- **The portrait was unpacked in the game all the same** (21 ms): it
  was loaded at start-up, and starting a game drops every resource
  that is loaded and not held (`flush_resource_cache` in
  `empty_slate`, `setup.c`).
- The 5 others: 3 double-clicks in a heavy view, where a look-up is
  7 ms, two or three of them on a render of 14 to 18 ms; one in
  `physics_run()`, 70 ms with nothing named inside; one unnamed.
  Two of the log openings came from the 3D view: a log picked up.

#### S7c

Done; results below.

- **No wait cursor for a log that its thread reads** (`audiolog_play`):
  it is kept for the one case that still reads on the main thread, a
  log packed in its file.
- **The panels' art held, not just loaded** (`resimage_hold_all`): kept
  locked from the opening of its file, which the emptying of the cache
  leaves alone, and let go of before the file is closed at a change of
  language.
- **The logs' file and the barks' opened at start-up** and at a change
  of language (`audiolog_open_files`), not by the first log played.
- **Timers** (profile build): `wait_cursor@address` on the frame
  `begin_wait()` presents, wherever it is asked for; `audiolog_bark_play@`
  for a bark and where it was set off from; and on what `physics_run()`
  is made of. A bark set off by walking somewhere would be a log
  started from inside `physics_run()`: its 28 ms of the earlier
  captures and its 70 ms of this one may be that. The notes will say.

Left as they are: the double-clicks in a heavy view (what is left of a
look-up is the walk through the level's geometry), and the options
panel's 28 ms a key, which is a menu.

Results (`docs/profiles-perf/profile-step-s7c.txt`): the measuring
flavour, 642 logged seconds, 617 in the game: 6 stutters, 0.6 a
minute.

| | S5b | S6 | S7 | S7b | S7c |
|---|---|---|---|---|---|
| seconds in the game | 225 | 950 | 548 | 224 | 617 |
| stutters a minute | 8.5 | 3.2 | 1.4 | 3.5, mails opened one after another | 0.6 |

- **Opening a mail or a log no longer stutters.** 6 logs were started
  in the game: the click's handler took 2.9 to 9.4 ms, and the frames
  16.7 to 22.4. `audiolog_play` was 13 to 17 ms of them before, 73 for
  the first of a session. It was the wait cursor's frame: taking it out
  is what changed.
- **The panels' art is not unpacked in the game any more**: no read of
  it there (`resmax=`).
- **`physics_run()`'s stutter has a name**: `compare_locs`, 65.5 ms of
  a 66 ms `physics_run()`. That is what the game does when the player
  steps onto a new map square (`src/GameSrc/physics.c`): the music is
  changed where its zone changes, a new score loaded
  (`fade_into_location`), and the square's triggers are run. Which of
  the two, the next capture's notes will say. Every capture has had
  this stutter in the same place.
- The 5 others: three frames in a row in a heavy view (16 to 24 ms of
  `render_run()`, with 4 to 7 ms of sound in two), one with the
  security cameras' views, one in a present.

**The freeze at quitting.** Quitting to the main menu froze the game:
21.2 s in the capture, in the options panel's handler. It is the
memory card, and not the game's doing:

- the profiler's own writer was held in a write to `profile.txt` from
  t=673, for 30.8 s (`logio=30769`; 26 ms as a rule, and no capture
  before has one over 0.2 s);
- the game went on at 60 fps for ten seconds, nothing of its own
  touching the card;
- at t=684 the quit closed the options panel, which saves the
  preferences (`SavePrefs`, `src/MacSrc/Prefs.c`): a small file written
  to the card. That write waited until the card came back, at t=705.

Why the card held a write for 30 s the capture doesn't say. The normal
build has no log to write. But the panel saved the preferences every
time it closed, changed or not, and any stall of the card then froze
the game the same way: **the preferences are now written only when
they have changed**. The text is built in memory and held against what
was last written. With the normal build, quitting to the main menu
several times over did not freeze, and a changed setting was still
there after the game was started again.

**What is left** is four things, two of them waits and not stutters.
They are the next steps, S8 to S11. For each, what is measured is kept
apart from what is only read in the code: twice in S5 to S7 a reading
was taken for a cause and was wrong, so each step times its suspect
before anything is changed.

#### S8: saving and loading a game

Done: loads and new games here, saves in S8b and S8c. 9.4 and 12.0 s in
the options panel's handler before; a new game 3 s.

- Read in the code (`src/GameSrc/gamewrap.c`): a save copies the whole
  game-in-progress file to its slot and a load copies it back, one byte
  at a time (`copy_file`: `fgetc`, `fputc`), through the C library's
  1 KB buffers: a read and a write on the card for every kilobyte of a
  file of 0.6 to 1 MB. A new game copies the 613 KB archive the same
  way. Before its copy a save also rewrites the current level into the
  file, which is S10's subject.
- First: timers on the copy, on the level's rewrite and on the rest, to
  see how the 9 to 12 s divide.
- Then, if the copy is it: the file read whole and written in one go.
- Checks: on the PC, the copy is byte for byte the file; on the Vita, a
  save of the build before loads in the new one and the other way
  round. Nothing in the file's format changes.

As built:

- **The copy is one read and one write** (`file_copy` in
  `src/GameSrc/filecopy.c`): the file whole into memory, then out. A
  read or a write that fails is now an error; the old copy didn't look.
  The destination is opened only once the source is read, so a source
  that can't be read leaves it as it was.
- **The old copy is timed beside it, in the same capture.** The
  profile build's first save of a session also copies the game file
  the old way, a byte at a time, into a scratch file it then removes
  (`copy_bytewise` in the notes, `copy_file#0/bytes` for the new one).
  So the reading above gets its measurement and the fix its result
  from one save. (Taken out again after S8c's capture: timed in three,
  2.7, 3.5 and 4.4 s against 0.32 to 0.40.)
- **A save's, a load's and a new game's pieces have timers** (profile
  build): the file's head, the level, the copy; in the level's saving
  and loading, the opening, the pieces, the closing and what sets the
  level up.
- **The resource library's writing is summed over the frame**:
  `reswrite=` and, inside it, `respack=`, in a stutter's line. That is
  S10's measurement, taken here since a save goes through it.
- **Checked on the PC** (`tests/filecopy/run.sh`): files of 0 bytes to
  3 MB, each copied onto nothing, onto a shorter file and onto a longer
  one: 36 copies, each the source's bytes and no more; a missing source
  is an error and leaves the destination alone.

Results (`docs/profiles-perf/profile-step-s8.txt`): the measuring
flavour, two sessions: a new game, a save, cyberspace in and out, a
second save, loads; then the game started again and both saves loaded.

| | before | S8 |
|---|---|---|
| a new game | 2.9 to 3.1 s | 0.68 s |
| a load | not told apart from a save | 0.70 to 0.79 s |
| the game file's copy, 676 KB | 2.69 s, the old way, in the same save | 0.32 s |
| a save | 9.4 and 12.0 s | 10.8 s the first (8.1 without the old copy run beside it), 3.5 s the second |

- **The copy is 8 times faster**, measured both ways in one save:
  2,686 ms a byte at a time, 318 ms whole. 337 to 345 ms for the
  613 KB archive and for the loads' copies.
- **A load is 0.7 s**: the copy 0.34, the level 0.12 to 0.16, the
  player and the schedules 0.05.
- **The copy was 2.7 s of a save, not the 9 to 12.** The reading this
  step started from named a real cost and not the main one.
- **A save is the game file's packing.** The first save's pieces:

| piece | time |
|---|---|
| opening the game file to write the level into it (`level_save_open`) | 6.40 s |
| the level's 34 pieces written (`level_save_pieces`) | 0.94 s |
| the copy to the slot | 0.32 s |
| the player and the schedules (`save_head`) | 0.30 s |
| closing (`level_save_close`) | 0.08 s |

- `respack=6347`: the opening's 6.4 s are the resource library packing
  the file. The second save: 1.7 s of packing, across its two openings.
- **Going into cyberspace is the same**: 7.2 s, 5.9 of them that
  opening; coming out 2.5 s, 1.2 of them. S10's cause, measured.
- **Why the packing is that slow** (`ResPack` in
  `src/Libraries/RES/Source/resbuild.c`). A resource rewritten into the
  game file leaves the old one dead in it; the file is packed the next
  time it is opened for writing. Packing moved every resource that
  comes after the first dead one down the file, one at a time: a seek,
  a read, a seek and a write on the card for each. The game file has
  some 900 resources and a level's pieces sit near its start: nearly
  all of them moved, 3,600 card operations. (The reading under S10
  had it packing after each piece. It packs once an opening, the whole
  file.)

#### S8b: the game file's packing

Done; results below.

- **The packing is done in memory**: the part of the file from the
  first dead resource to the end of the data is read in one go, closed
  up there, and written back in one go; then the file is cut to its
  new length as before. Two card operations for thousands. With no
  memory for it, or a read that falls short, the old way is used.
- **The file that comes out is the old packing's, byte for byte**: the
  same resources at the same places, and between them the same
  leftover bytes, since closing up in memory moves what moving in the
  file moved.
- **Checked on the PC** (`tests/respack/run.sh`): the resource library
  as it was and as it is, each given the same life of a file: 700
  resources of a few bytes to 65 KB, a third of them compressed, then
  14 openings for writing with 20 to 70 resources rewritten in each, in
  a run as a level's are or here and there, and a new one added now
  and then. The file after each round: the same bytes from both, 16
  times of 16; every resource read back as last written. The test
  fails 14 times of 16 on a packing made wrong on purpose.
- **A timer on the compressing** (profile build): `reslzw=`, in a
  stutter's line, the part of `reswrite=` spent compressing. A level's
  34 pieces take 0.94 to 1.05 s to write: once the packing is cheap
  that is the largest piece of a save, and whether it is the
  compressing or the 34 writes isn't known.

Results (`docs/profiles-perf/profile-step-s8b.txt`): a new game, a
save, cyberspace in and out, two more saves, the three loaded.

| | before S8 | S8 | S8b |
|---|---|---|---|
| the file's packing, at the first save | | 6.35 s | 0.45 s |
| a save | 9.4 and 12.0 s | 8.1 s, then 3.5 s | 3.1 s the first, then 1.9 s |
| into cyberspace | 1.5 to 7.2 s | 7.2 s | 1.7 s |
| out of cyberspace | 2.5 to 7.2 s | 2.5 s | 1.3 s |
| a load | | 0.70 to 0.79 s | 0.85 to 0.88 s |

(The first save's figures leave out the old copy run beside it for the
comparison: 2.7 s in S8's capture, 3.5 s in this one.)

- **The packing is 14 times shorter**: 446 ms where it was 6,347; 103
  to 312 ms at the later openings.
- **The saves load right.** Each of the three loads puts the player
  back on the square, at the height and facing the way the log has for
  its save (`at=`).
- **A save is now its level's 34 pieces**: 1.0 to 1.1 s of the 1.9, with
  the copy to the slot 0.35, the player and the schedules 0.3 and the
  opening and closing 0.15. A level change is the same pieces: 0.92 to
  0.95 s.
- **Those pieces are not the compressing**: `reslzw=` is 17 to 25 ms.
  What is left is their writing, 28 ms a piece.

#### S8c: a level's pieces written together

Done; results below.

Read in `ResWrite` (`src/Libraries/RES/Source/resbuild.c`): each
resource written begins with a seek to the end of the data, which is
where the file already stands after the one before. A seek has the C
library hand what it holds to the disk, and its own buffer is a
kilobyte: so every piece of a level is a write on the card of its own,
34 of them for some 70 KB. That is a reading; this step times it and
takes it out together.

- **A file opened for writing gets a buffer of 256 KB**, where its
  resources collect.
- **A resource is written where the file stands, with no seek, when the
  last thing done to the file was the write before it.** After anything
  else, a read, the packing, the directory's writing, the opening, the
  seek is made as it was (`ResTailLost`). And the file is asked where
  it stands after each write: no seek is spared on a guess.
- The pieces reach the card when the file is closed, in one write or
  two. The file's bytes are the same.
- **`resio=`** (profile build), in a stutter's line: the part of
  `reswrite=` spent in the file's seeks and writes.
- **Checked on the PC** (`tests/respack/run.sh`, three seeds): the
  test of S8b, with resources now also read from the file between two
  writes while it is open for writing. The file after each round is
  still the reference library's, byte for byte, and every resource
  reads back as last written. The test fails 13 times of 16 on a
  library in which a read no longer brings the seek back, and 14 of 16
  on a packing made wrong.

Results (`docs/profiles-perf/profile-step-s8c.txt`): a new game, a
save, cyberspace in and out, a second save, the two loaded.

| | before S8 | S8 | S8b | S8c |
|---|---|---|---|---|
| a save | 9.4 and 12.0 s | 8.1 s, then 3.5 s | 3.1 s, then 1.9 s | 1.1 s, then 0.88 s |
| into cyberspace | 1.5 to 7.2 s | 7.2 s | 1.7 s | 0.78 s |
| out of cyberspace | 2.5 to 7.2 s | 2.5 s | 1.3 s | 0.47 s |
| a load | | 0.70 to 0.79 s | 0.85 to 0.88 s | 0.71 to 0.73 s |
| a new game | 2.9 to 3.1 s | 0.68 s | 0.87 s | 0.85 s |

(First-save figures without the old copy run beside it, 4.4 s here.)

- **The reading held, and the timer says so.** A level's 34 pieces: 22
  to 30 ms, from 0.92 to 1.67 s. Of that, the file's seeks and writes
  are 0.3 to 0.4 ms (`resio=`) and the compressing 17 to 25 ms: the
  pieces are no longer handed to the card one by one.
- **They go at the closing**: 95 to 139 ms, where it was 80 to 117.
- **The saves load right**: both loads put the player back on the
  square, at the height and facing the way the log has for their save.
  With the normal build: saved, the game closed and started again,
  loaded; the same after cyberspace and back; and a save from before
  S8 loaded. All as they should be.
- **A save's 0.88 s**: the copy to the slot 0.36, the player and the
  schedules 0.24, the closing 0.12, the opening with its packing 0.11,
  the pieces 0.03. Nothing stands out; the step stops here.
- **S10's question is answered for cyberspace** by S8b and S8c
  together: its seconds were the packing, then the pieces. The lift was
  not taken in any of these captures. It writes and reads a level the
  same way, and reopens the textures' file, which is in memory since
  S6.

#### S9: stepping onto a new map square

Done: measured here, fixed in S9b and S9c. `compare_locs`,
65.5 ms of a 66 ms `physics_run()`, in the same place in every capture.

- The timers that say which part are in the build since S7c
  (`fade_into_location`, `check_entrance_triggers`).
- Read in the code: where the music's zone changes, the new score is
  read from the card (`ReadXMI` in `src/MacSrc/Xmi.c`) and the music's
  threads are stopped and started with waits of a millisecond at a
  time; or it is the square's triggers.
- First: one capture, walking the first level's zones. No new code.
- Then, by what it names: the scores read ahead or kept in memory, and
  the threads handed over without the waits; or the trigger.
- Check: by ear, the music changes where and how it did.

As built, the first half: nothing of the game changed. If it is the
music, one timer on `fade_into_location` would only say so, and the
fix depends on which part of it is slow. So the profile build also
times what is inside (`src/GameSrc/musicai.c`, `MacTuneLoadTheme` in
`src/MacSrc/SDLSound.c`): the music's threads stopped, each waited for
a millisecond at a time (`musicai_shutdown`, `StopTheMusic`); the old
score let go (`FreeXMI`); the new one read from the card (`ReadXMI`)
and its tables after it (`theme_tables`); the track started
(`MacTuneStartCurrentTheme`). With `at=` in the stutter's line, the
capture gives the cause and the square.

Results (`docs/profiles-perf/profile-step-s9.txt`): 303 s in the game.
The walk to the lift, in and out of it some twenty times, up to level
2, in and out again.

- **The stutter is the music changing zone, every time**: 25 crossings
  of the lift's door, 25 stutters, `fade_into_location` 37 to 70 ms of
  each. On level 1 the door is between squares 15,36 and 15,37; on
  level 2 between 15,39 and 15,40 (`at=`).

| piece | time |
|---|---|
| the new score read and taken apart (`ReadXMI`) | 23 to 27 ms for the lift's, 35 to 37 for level 1's, 53 to 56 for level 2's |
| the old score let go (`FreeXMI`) | 6 to 12 ms |
| the music's threads stopped (`musicai_shutdown`) | 3 to 9 ms |
| the score's tables (`theme_tables`) | 2.4 ms; 10 to 24 the first time |
| the track started | under 2.4 ms |

- **The same score is read, taken apart and thrown away again at each
  crossing.** A score's time is the same to a millisecond or two each
  time and grows with the score. Neither of the two readings this step
  started from was it on its own: not the card alone, and the threads'
  waits are a tenth of it. Whether `ReadXMI`'s time is the card or the
  taking apart, its one timer doesn't say.
- **One other stutter, 8 s into the game: a trigger**, 25 ms in
  `check_entrance_triggers` on square 19,20 of level 1: the stutter
  the earlier captures have near the start. Which trigger, the log
  doesn't say yet.
- **The lift is 0.37 s** (level 1 to level 2): the level saved 0.19,
  the new one read 0.11, the rest setting it up. S10 is done for the
  lift too.

#### S9b: the music's scores kept

Done; results below.

- **A score read once is kept** (`src/MacSrc/Xmi.c`): `ReadXMI` takes
  it from those kept when its file was read before, and `FreeXMI`
  leaves a kept score where it is. Going back into a zone costs no
  reading, no taking apart and no letting go. The first time into
  each kind of zone costs what it did. Room for 24 scores.
- **What plays is as it was**: the same score, started the same way,
  the music device set up by the same call at every change. The
  player threads read a score and never write to it.
- **Timers** (profile build): in `ReadXMI`, the file's reading
  (`xmi_file#bytes`), the taking apart (`xmi_parse`) and the device's
  set-up (`xmi_setup_mode`), each on its own; and `trap#kind/object`
  on each trap a trigger runs, for the 25 ms one.
- No test on the PC: this is the music's player, which runs on the
  console only. The capture and the ear check it.

Results (`docs/profiles-perf/profile-step-s9b.txt`): 71 s in the game,
the walk to the lift, in and out of it some thirty times, up to
level 2. By ear, nothing has changed.

| a crossing of the lift's door | S9 | S9b |
|---|---|---|
| back into a zone whose score was read before | 37 to 70 ms | 10 to 18 ms |
| the first time into a zone | 37 to 70 ms | 44 ms (the lift), 70 ms (level 2) |

- **Two of the thirty crossings back still made a stutter**, frames of
  27 and 29 ms; the others are under its line.
- **A score's first reading is the card**: 23.6 ms for the lift's
  9.6 KB (`xmi_file`), 47.3 ms for level 2's 25 KB. Taking the larger
  apart is 7.7 ms (`xmi_parse`, 50 tracks).
- **The score's tables are read from the card at every crossing**, the
  score kept or not: 4 to 10 ms for a file of 400 bytes
  (`theme_tables`). With the music's threads stopped (3 to 7 ms) that
  is what a crossing still costs. The device's set-up doesn't show.
- **The trigger at 8 s is a preferences save**: `trap#4/713`, the trap
  that sets a game variable, here the one that switches the on-screen
  help off, which saves the preferences in the same breath
  (`trap_questbit_func` in `src/GameSrc/trigger.c`): 24 ms on the
  card, once in each new game.

#### S9c: the first time into a zone, and the trigger

Done; results below.

- **The scores and their tables are read ahead** (`XmiReadAhead` in
  `src/MacSrc/Xmi.c`): a thread below the game's, started once the
  music device is there, reads `thm0` to `thm15` of the music type in
  use, those that exist, and their tables. `ReadXMI` and the tables'
  reading take a file from there when it is in, and from the card as
  before when it is not: another music type chosen later, or the
  thread not there yet. What is played is the same bytes.
- **The help's switch-off saves the preferences later**
  (`SavePrefsLater`): at the next moment the game waits anyway, which
  is the wait cursor's (`begin_wait`: a save, a load, a level change),
  or when the options panel closes, which saves in any case. Closed
  before either, the game starts with the help on again, as a new game
  turns it on anyway.
- In the notes, a score taken from those read ahead is `xmi_ahead`,
  one from the card still `xmi_file`.

Results (`docs/profiles-perf/profile-step-s9c.txt`): two sessions,
111 s in the game: a new game with the help on, the walk to the lift,
in and out of it, up to level 2, a save and a load; then the game
started again. Nothing different to the ear, and the help's setting
kept.

| a crossing of the lift's door | S9 | S9b | S9c |
|---|---|---|---|
| back into a zone | 37 to 70 ms | 10 to 18 ms | 3 to 8 ms |
| the first time into a zone | 37 to 70 ms | 44 and 70 ms | 12 to 15 ms |

- **The stutter 8 s into a new game is gone**: passing square 19,20
  the game's part of the frame is 5.5 ms, where the trap took 24.
- **A crossing no longer touches the card.** In the 96 seconds at the
  lift's door the game's part of the worst frame is 3.1 ms at the
  median, 7.7 at the ninth decile.
- **The first time into a zone is the score taken apart, and no more**:
  9.2 ms of `ReadXMI` for level 2's score, 8.3 of them `xmi_parse`,
  the file coming from those read ahead.
- **Left as it is**: that first taking apart, once per score in a
  session. On a render of 10 ms it made a frame of 27 ms, twice in
  the capture, once in each session: just over the profiler's line for
  a stutter. Doing it ahead as well would mean reworking the score
  reader for a second thread to run it, which is not worth disturbing
  the music's code for 8 ms once per kind of zone.
- Along the way: the lift 0.48 s, a save 1.25 s, a load 0.66 s.

#### S10: changing level

Done by S8b and S8c: cyberspace in 0.78 s, out 0.47 s; the lift 0.37 s
(in S9's capture). Its cause was the game file's packing (5.9 of the
7.2 s of going into cyberspace), then the level's pieces written one by
one. What follows is the step as first planned. Into cyberspace 1.5 to 3.1 s, out
2.8 to 7.2 s. Since S6 the art's side of a level change costs nothing.

- Read in the code (`save_current_map` in `saveload.c`, `ResWrite` and
  `ResPack` in `src/Libraries/RES/Source/resbuild.c`): leaving a level
  rewrites its sixty or so pieces into the game file on the card, and
  each piece that takes the place of an old one has the library close
  the gap at once, moving everything behind it down the file 32 KB at
  a time. Then the new level is read.
- First: timers on the rewrite (the opening, the pieces, the packing,
  the closing) and on the read: in S8's capture, if a level is changed
  during it.
- Then, the smaller change first: the file packed once after the last
  piece and not after each; and only if that leaves it long, the game
  file kept in memory while playing and written to the card at a save.
- Checks: on the PC, a level written the new way reads back the same
  pieces, byte for byte, as written the old way; on the Vita, a level
  left and come back to is as it was left, and saves load.

#### S11: heavy views

Measured; what it found is S11b, below. `render_run()` at 16 to 24 ms in
some places, three frames in a row at one of them in S7c's capture,
and a look-up under the cursor at 7 ms there, two or three for a
double-click.

- Not known: where in the render those milliseconds are. The log has
  the phases, but no capture was made standing in such a place.
- Not measured either, as it turned out: the maps. The map in a side
  panel, in full screen, is only named in the log inside a stutter
  (`mfd@mfd_map_expose`), and no capture since S5 has one; what it
  costs each time it is redrawn while walking is nowhere. The map's
  own screen (mode 8) is 2 seconds of all the captures together, and
  its loop had no timer. Both draw through `amap_draw`
  (`src/GameSrc/amap.c`).
- First, with no change to the game: the map's drawing timed
  (`amap=avg/max`, `amaps`, `amapmax` in a second's line, and a note
  `amap_draw#zoom` in a stutter's), and the five draws of the map
  screen's loop counted in `ui2d` (`src/GameSrc/amaploop.c`). Nothing
  new for the heavy views: standing still, a second's averages give
  each part of the frame, and `at=` says where and facing what. The
  frame dumps first thought of are dropped: the checks flavour writes
  them when it decides to, not where the player stands.
- The capture (`profile-step-s11.txt`), in three parts: in each of the
  two or three heaviest places, five seconds standing still facing the
  heavy way, a slow full turn, a few double-clicks on something far
  off; half a minute's walk in full screen with a side panel on the
  map, then with both; twenty seconds on the map's own screen, moving
  it and zooming.
- Then it depends on what that shows. For the render this is the step
  most likely to end in "this is what the renderer costs here" and not
  in a fix; for the maps nothing is known yet.

The capture (`profile-step-s11.txt`, 12 minutes on levels 1 and 2, a
map in a side panel for nearly all of it):

- **The maps cost next to nothing.** In a side panel: 0.84 ms a
  drawing (1.5 at most), 15 a second for one panel and 30 for two,
  0.36 ms a frame on average. On its own screen: 0.8 to 2.8 ms a
  drawing, one to five a second, the frame at the screen's rate
  throughout. Nothing to fix.
- **Level 1's heavy places are the render at its ordinary price**: 48
  to 57 fps for a second here and there (square 18,23 facing north,
  157 to 161 calls a frame; 14,30), the traversal and the sending both
  long and nothing out of line. Left as it is.
- **A force bridge is the one real problem**, as the player had seen
  it: standing on level 2's (squares 23 to 26, 29), 13 to 20 fps,
  frames of 50 to 80 ms and one of 100; the same on level 1's (25,42)
  for the two seconds spent there. The log's counters name the cause:
  `gpuwhy=tlucpoly:4.0`, `gpucpu=4.0/61.31`, `gpuscenes=3.00`. The
  bridge is a translucent box (`_fr_draw_poly_cube` in `gameobj.c`),
  and a translucent polygon is the one kind of call that fell to the
  CPU in the whole capture. For each, the scene under way is ended
  and waited for, and `gri_poly_loop` then reads every pixel under
  the polygon from the canvas, a byte at a time, passes it through
  the colour's table and writes it back; the canvas is in the GPU's
  memory, which the CPU reads four times slower than its own
  (5.1 ms for half a megabyte against 1.3, the start-up line). Four
  polygons that fill the screen: 61 ms.

#### S11b: translucent polygons on the GPU

Built, and of no effect on the console: neither spelling of the shader
compiled, so the CPU drew the bridge as before (the result and what
followed are at the end of this section, and in S11c). Drawing the
same in ordinary memory would still cost some 15 ms a frame on three
cores, so the GPU draws these polygons itself.

- A translucent polygon is "the pixel becomes table[the pixel already
  there]". This GPU hands a fragment shader that asks for it the
  target's pixel as the calls before it in the scene left it (the
  `FRAGCOLOR` input, its "programmable blending"), and the renderer
  already looks indices up in a texture of tables. So: a fourth pair
  of shaders in `src/MacSrc/VitaGpu.c` (`DRAW_TLUC`, `vgpu_tluc`), the
  flat pair's vertex shader with the table's row where the colour is,
  and a `tluc` call in the queue's GPU interface (`rastq.h`).
- `gpu_classify` (`rastq.c`) follows `gri_tpoly_init` and
  `gri_clut_tpoly_init`: the table of the polygon's colour
  (`tluc8tab`) goes into the scene's tables as a colour table does;
  under the colour-table fill the row is the two tables one after the
  other (`gpu_table_row_then`); a colour without a table is the
  opaque polygon it always was. The shaded translucent kind
  (`FIX_TLUC8_SPOLY`) is not recorded and stays the CPU's.
- Nothing here had shown that the feature works on an 8-bit target,
  so the console decides at each start: `tluc_check` draws a ground
  whose column x holds x and translucent rectangles over it (one; two
  over one another; one in a second scene; one, a flat colour, then
  another) and counts the columns that aren't exactly the tables'
  answer. The shader is written two ways (the pixel as a colour from
  0 to 1, or as the target's bytes); the first that compiles and
  draws every column right is used, and with none the CPU draws those
  polygons as before. The session's second line says which:
  `tluc=on/2 tluc_check under=0 twice=0 kept=0 mixed=0` (`on, second
  way`, or `off (CPU)`; the number is how many compiled), and
  `gpu.txt` has the compiler's words.
- In the log `gpukinds=` gains `tluc`; `gpuwhy=tlucpoly` should stay
  at zero.
- On the PC (`tests/rastq`, `test_tluc`): 150 scenes of 40
  rectangles on whole pixels, opaque and translucent over one another
  in the three fill types, drawn by the stand-in GPU and by the
  mappers: no pixel differs, with the translucent call (1680 polygons
  through it) and without it.

The capture (`profile-step-s11b.txt`, with `gpu.txt`):

- **The shader did not compile**, either way: `tluc=off (CPU)/0`. The
  compiler's words, in `gpu.txt`: "using 'FRAGCOLOR' requires the entry
  function to be declared as '__nativecolor'", and of the second
  spelling "'__nativecolor' may only be used with function
  declarations". So the input is there; the keyword belongs on the
  shader's function and not on the pixel it reads.
- The fall-back did what it was for: the game ran as in S11, 13 to
  24 fps on the bridge.
- **Under the bridge looking up, 4 fps**: `gpucpu=2.0/262.02`, two
  polygons a frame for 262 ms. Two polygons can cover the screen
  twice at most, a million pixels: 250 ns a pixel, where reading the
  GPU's memory a word at a time costs 37 ns a word (the start-up
  line). `gri_poly_loop` reads a byte, writes it back and reads the
  next: on this memory, written through a buffer the processor has to
  empty before it reads again, a read after a write is the slow case.

#### S11c: the shader as the compiler wants it, and a cheaper fill

Done: the bridge at 60 fps, on it and under it (the capture is at the
end of this section). Two rounds had gone by without the bridge
improving, so this one carries two fixes: the GPU's, and the CPU's
for whatever it still draws.

- **The shader** (`src/MacSrc/VitaGpu.c`): `__nativecolor` on its
  entry function. In that mode the target's values come and go as
  they are, and whether that means colours from 0 to 1 or bytes from
  0 to 255, declared as colours or as the target's bytes
  (`__regformat unsigned char4`), nothing here says: the shader is
  written eight ways (`tluc_ways`; two since the capture, see below), the likeliest first, and each is
  checked as soon as it compiles, up to the first that draws every
  column of `tluc_check` right. A way that fails leaves its first
  wrong columns in `gpu.txt`, which show what the shader was handed.
  The second line: `tluc=on:way/tried`, or `off (CPU):-1/tried`.
- **The CPU's fill** (`gr_tluc8_span` in
  `src/Libraries/2D/Source/Flat8/fl8ply.c`): a stretch of a row is
  copied out, 256 pixels at a time, passed through the table in that
  copy and copied back, so that no byte of the canvas is read after a
  write. The same pixels by construction. For the bridge if no way of
  the shader passes, and for the shaded translucent polygons, which
  the GPU doesn't take.
- **Timed at each start**, since the bridge won't show the fill if the
  GPU takes the polygons: `64KB tluc_fill us: in_place= copied=` in
  the second line, 64 KB of GPU memory through a table the old way
  and the new.
- On the PC: `tests/rastq`, whose first part draws 4000 frames,
  translucent polygons among them, with these libraries and with
  those of before the row bands: identical.

The capture (`profile-step-s11c.txt`, a minute on and under level 2's
bridge; the player: "works great now"):

- **The shader works, its second way**: `tluc=on:1/1 tluc_check
  under=0 twice=0 kept=0 mixed=0`. Way 1 is the target's bytes both
  read and written (`__regformat unsigned char4`), from 0 to 255. It
  is the only one that compiled of the two tried. Of the first, the
  same as colours from 0 to 1, the compiler says (`gpu.txt`):
  "'FRAGCOLOR' varyings must be declared with the additional
  '__regformat' specifier" and "if __nativecolor is used, the 'COLOR'
  output must be declared with the addtional '__regformat'
  specifier". Six of the eight ways could therefore never compile:
  they are taken out since, and the one that draws right is tried
  first (`tluc=on:0/1` from now on), its shader's text unchanged.
- **The bridge is at the screen's rate**: 60.0 fps in each of the 44
  seconds it was drawn in, from above, on it and from underneath
  looking up, where S11b's capture had 4 to 20. `gpukinds=tluc` 1.4 to
  6.0 a frame, `gpuwhy` all zeros, `gpucpu=0.0/0.00`, one scene a
  frame, the wait for the GPU 3.1 to 4.6 ms as anywhere else. No
  stutter's line while playing.
- **The CPU's fill, timed at start-up**: `64KB tluc_fill us:
  in_place=36926 copied=1787`. In place 563 ns a pixel, more than the
  250 worked out from the bridge; through the copy 27 ns, 21 times
  less. A polygon that fills the screen would be 14 ms where it was
  294: twice my estimate of 7, and still more than the GPU's nothing,
  but it is what the shaded translucent polygons and the bitmaps'
  translucent kinds now cost at worst, should a level have them.

Outside the four: the options panel's 28 ms a key, which is a menu.

#### S12: frames shown a refresh late

Measured: the cause is found, and is S12b's to fix (the capture is at
the end of this section). What the roadmap left, looked at again in
the captures since S9 before choosing:

- **The options panel's 28 ms a key is gone since S7b**: it was the
  volume sliders' line in the game's log. No capture since has it. In
  S9c's the panel has a 14 ms read of its art when it opens, one key
  of 76 ms with nothing named, and the saves and loads.
- **A double-click in a heavy view**: once in S11's 12 minutes, a
  frame of 32 ms. Its look-ups are different questions (with and
  without what is translucent), so at most one of two or three could
  be kept from the click before.
- **The title screen's art**, 14 and 6 ms on a still screen.

None is worth a step. But S11's capture has, away from the bridges, a
stutter never looked at, and its most frequent one: a frame of about
25 ms whose work is short (6 to 9 ms) and whose hand-over to the
screen (`SDLDraw()`, `swapwait=`) takes 15 to 18 ms where it takes 6
to 8. The frame is shown one refresh late. 25 of the 683 seconds have
one (12 of them long enough for a stutter's line; S9's three captures,
485 seconds, have no stutter of the kind), and they follow the music:

| music's share of a core | seconds | with a late hand-over |
|---|---|---|
| under 12% | 160 | 0 |
| 12 to 15% | 341 | 1 |
| 15 to 18% | 77 | 3 |
| 18% and more | 105 | 21 (20%) |

By place: level 1 around squares 44 to 47, 27 to 40, and level 2
around 27 to 45, 41 to 50. The log's own writing is not it: its slow
seconds (`logio=` over 30 ms) are half of all seconds and 16 of the 25.

- Known: the music is synthesized in the audio thread's callback,
  2048 samples at a time (`Mix_OpenAudio(48000, ..., 2048)` in
  `src/MacSrc/SDLSound.c`), so every 43 ms that thread works in one
  go, some 8 ms of it when the music is at 20% of a core.
- Not known: that this is the cause, and how. The guess: when that
  stretch falls on the screen's refresh, what hands the finished
  frame to the screen (a thread of the screen library's, prebuilt, so
  not timed from inside) runs late and the frame waits for the next
  refresh. And all of it is from the measuring build: whether the
  normal one does the same is not known either.
- First, with no change to the game (profile build only,
  `src/GameSrc/vprof.c`):
  - a thread that sleeps on the screen's refresh and notes the time
    of each, at the highest priority, and how late it woke
    (`vbllag=`): itself a probe of whether something keeps a thread
    from a refresh;
  - each stretch of the music's synthesis, its start, length and
    core (`vprof_audio_begin`, `vprof_audio_add` from `MusicCallback`),
    and the longest of the second (`mcbmax=`);
  - when the hand-over was asked for and when it came back
    (`vprof_swap` from `SDLDraw`);
  - from those, for each frame whose work was short (under 14 ms):
    was it late (over 23 ms), and was the music at work on the
    refresh the hand-over was for (`onmcb=late/all`), in the 4 ms
    before it (`nearmcb=`), or on the refresh before the asking
    (`prevmcb=`), with `late=` and `light=` for the totals: the table
    that says whether the music's stretch and the late frame go
    together;
  - a `late` line for each late frame (8 a second at most), and the
    same fields in a stutter's line: `vbl=` refreshes spanned,
    `swapat=` how long after a refresh the hand-over was asked for,
    `mcb=start+length@core` the music's stretches about that refresh;
  - the two threads' priorities and cores (`aprio=`, `amask=`,
    `mprio=`, `mmask=`).
- The capture (`profile-step-s12.txt`): five minutes of ordinary play
  with the music on, through the parts of levels 1 and 2 where S11
  had them.
- Then: if the music's stretch is the cause, the candidates are the
  music made in smaller pieces, or its thread kept off the core or
  under the priority of what hands frames over. If it is not, the
  same lines say where the time went.

The capture (`profile-step-s12.txt`, 592 seconds in the game, on six
levels; the music at 12.6% of a core at the median):

- **Every late frame has the music at work on core 0.** 42 `late`
  lines (40 counted in the seconds' lines, in 38 seconds): in each,
  one stretch of the music's synthesis, on core 0, begun 0 to 1.7 ms
  before the refresh the hand-over was for (0.4 at the median) and 6
  to 10.7 ms long. None with the music on another core, none without.
- **The frame is late by what was left of that stretch**: its length,
  less one refresh, less the stretch's end after the refresh, is
  0.09 ms at the median and 1.05 at most.
- **The screen is not what is late** (`vbl=1`: the frame spans one
  refresh). The main thread is: it is pinned to core 0
  (`mmask=10000`, `rastq_threads_pin_main`, since the three cores'
  work) at priority 160; the audio thread has priority 64, well above
  it, and no core of its own (`amask=0`): last seen on core 0 in 71
  of the 592 seconds, on core 1 in 290, on core 2 in 231. When it
  works on core 0 as the hand-over ends, the main thread waits for it
  to finish, and starts its next frame with 6 to 10 ms gone. The
  guess of a held-up hand-over thread was wrong: the profiler's own
  refresh thread, at the audio thread's priority, was never more than
  4.8 ms late and mostly 0.03.
- **Of the frames with the music across their refresh on any core,
  1.1% are late** (40 of 3641), and none of the 27465 others: it
  takes core 0, and a stretch that begins just before the refresh.
  A stretch that begins earlier leaves less to wait for: the seconds
  with the audio thread last seen on core 0 have 1.6 frames over
  20 ms each, the others 0.8 to 1.0.
- Not answered here: what a stretch on core 0 does to a frame at
  work. A stutter's line had the music's stretches only when the
  hand-over waited (`mcb=?` on 19 of the 21 stutters).
- Besides: the build with the shader's ways cut to two runs
  (`tluc=on:0/1`, the check exact).

#### S12b: the audio thread off the game's core

Done; the capture is at the end of this section.

- **The audio thread is given cores 1 and 2**
  (`audio_thread_off_main_core`, at the top of `MusicCallback` in
  `src/MacSrc/Xmi.c`): it is SDL's thread, so it places itself, the
  first time it calls. The audio logs play through it too. The main
  thread stays on core 0 and the workers on 1 and 2: with the GPU
  renderer those two are mostly asleep; with the three cores' own
  (no shader compiler) the music was on them already in 521 of
  S12's 592 seconds.
- In the log (profile build): `mcb0=`, the music's stretches on the
  main thread's core in the second, which should stay at 0; `mcbf=`
  in a stutter's line, the music's stretches during the frame
  whatever the frame did; and S12's `late=` is `lateswap=` from now
  on, a second's line having had a `late=` already.
- To check: no `late` line, `mcb0=0`, `amask=60000`; the frames over
  20 ms; and the player's ears on the music and the effects, which
  must be as they were.

The capture (`profile-step-s12b.txt`, 377 seconds in the game, on six
levels; the player: "music was good"):

| | S12 | S12b |
|---|---|---|
| frames with short work that lasted over 23 ms | 40 of 31106 | 0 of 18086 |
| the audio thread | on core 0 in 71 of 592 seconds | on cores 1 and 2 (`amask=60000`), `mcb0=0` throughout |
| quiet seconds with a frame over 20 ms | 27% | 1% |
| frames over 20 ms in a quiet second | 0.35 | 0.01 |
| the longest frame of a quiet second, median | 17.2 ms | 16.7 ms |

- A quiet second is one with no render over 13 ms and nothing over
  5 ms in the input or in the game's logic: 179 of S12's seconds, 120
  of S12b's. The two captures are not the same play, and all their
  seconds together don't compare: S12b's has more of the heavy places
  and of the lifts.
- **The 20 to 23 ms frames that every capture has had in its light
  seconds were this**: the music's stretch on core 0 across the moment
  the main thread should wake, with less of it left than the 6 ms that
  make a late frame.
- **The stutters are not fewer here, and none is a late start**: 28
  in 377 seconds. Six are lift rides (0.4 to 0.7 s) and four the frame
  after one (the new level's music first read: the score taken apart
  in 11 ms, the old one shut down in 5). Twelve are in one place on
  level 3 (squares 27 to 29, 32 to 36): frames of 25 to 36 ms, with
  translucent bitmaps drawn by the CPU (`gpuwhy=tlucbm`, 1 to 2 a
  frame for 2 to 4 ms, up to three scenes a frame) and once 27 ms in
  the hand-over to the GPU with nothing in the log to say why. Three
  are a heavy render elsewhere, one a click, one 23 ms outside every
  timer (level 11), one the leaving of the game. `mcbf=` shows the
  music's stretches during each, on cores 1 and 2.

#### S13: translucent bitmaps on the GPU

Done; the capture is at the end of this section. The place on level 3 that S12b's
capture has (squares 27 to 29, 32 to 36; its seconds 138 to 170):

- **Measured**: `gpuwhy=tlucbm` in 25 of the 377 seconds, about one
  call a frame, and nowhere but there. A translucent bitmap
  (`BMT_TLUC8`) is the bridge's problem in its other form: the GPU
  path left it to the CPU, which draws it on the canvas in the GPU's
  memory, a pixel read after each pixel written. `gpucpu=` says 2 to
  8 ms a frame on average over a second (8.1 at second 152, 7.9 at
  165), and each call splits the frame in two or three scenes. The
  seconds run at 39 to 56 fps.
- **Read, not measured**: that the twelve stutters there, 19 to 27 ms
  in `gpu_run()`, are that same bitmap seen from close. The log had a
  second's average of the CPU's drawing and not a frame's; a
  stutter's line has it from now on (`gpucpu=calls/ms`).
- **What such a pixel is** (`gri_tluc8_lin_umap_loop` in
  `src/Libraries/2D/Source/Flat8/fl8nl.c`, and the scaler's in
  `fl8ns.c`): the bitmap's texel; if that colour has a translucency
  table, the table's entry for the pixel already there, otherwise the
  texel; then the call's colour table, if it has one. Texel 0 leaves
  the pixel alone in a transparent bitmap. The 2D library has only
  the linear mapper and its scaler for these, plain or through a
  colour table.
- **For the GPU** that is the textured shader with one lookup more,
  between the texel and the tables: a matrix of 256 rows of 256, the
  texel's row at the canvas's pixel (`gpu_matrix` in `rastq.c`, made
  from `tluc8tab` when it changes; a 256 by 256 texture in
  `src/MacSrc/VitaGpu.c`). The canvas's pixel is read as S11c's
  shader reads it. `gpu_classify` lets a `BMT_TLUC8` bitmap through
  the linear family as a `GPU_TMAP` with a mark (`gpu_tluc`), so the
  cutting, the repeats and the culling are the ordinary bitmaps', and
  `tluc_tmap` joins the queue's GPU interface.
- **Checked at start-up**: `tlucbm_check`, four bands over a ground
  whose column x holds x, the texels some with a row of their own in
  the matrix, some without, some 0: all of them (`opaque`), texel 0
  left out (`trans`), through a table (`table`), drawn twice
  (`twice`). A wrong column, or a shader that doesn't compile, and
  the CPU keeps these bitmaps: `tlucbm=off (CPU)` in the session's
  second line.
- **The CPU's two loops go through a copy of the row** as well, 256
  pixels at a time, as `gr_tluc8_span` since S11c: for whatever is
  left to it, and for all of these bitmaps if the shader's check
  fails. S11c's timing gives 27 ns a pixel for that way on the GPU's
  memory against 563 in place.
- On the PC (`tests/rastq`): the frames drawn directly are those of
  the libraries of before the row bands, the new loops included; the
  stand-in GPU has the new call, so the translucent bitmaps of the
  general scenes go through it; and `test_tluc` draws translucent
  bitmaps a pixel for a texel among its rectangles: no pixel differs
  from the mappers', with the call and without it.
- To expect: up to 8 ms a frame back there, and the long frames gone
  if they are the bitmap. The place is heavy besides (130 to 250
  calls a frame).

The capture (`profile-step-s13.txt`, 610 seconds, 592 of them on
level 3 around that place, the on-screen help on; the player asks where the
stutter comes from, the invisible creature or the cameras):

- **The shaders work**: `tlucbm=on tlucbm_check opaque=0 trans=0
  table=0 twice=0`. `gpukinds=tlucbm` in 257 seconds, 1 to 3 a frame;
  `gpuwhy=` all zeros in every second, `gpucpu=0.0/0.00`, one scene a
  frame. The slowest second there is 47.9 fps, from 38.6.
- **The place is the render at its price**: 250 to 310 calls a frame
  in 380 to 550 pieces, `render3d` 15 to 17.5 ms. Of the 180 seconds
  over 200 calls, 29% are under 57 fps. No part out of line: the
  traversal 10 to 12.5 ms with the hand-over to the GPU in it (5 to
  6: 2.2 to 2.9 to decide and cut, 2.0 to 2.4 to send), the view's
  end 6.
- **The cameras** (`view@render_hack_cameras`, a view more): 3.5 ms a
  frame with one in sight, 76 seconds, at 60 fps at the median.
- **The on-screen help's scan** (3.3 ms a frame, 4.4 to 7.5 in the
  long frames) runs while the GPU draws (`gpuoverlap=3.4`): the wait
  for the GPU is 0.4 to 0.6 ms with it and 3.5 without.
- **22 stutters, 2.2 a minute**:
  - 10 with `gpu_run()` at 8.4 to 11.1 ms. In 9 of them a stretch of
    the music's synthesis, 5.6 to 6.75 ms long on core 1 or 2, begins
    3.8 to 7.3 ms into the frame (`mcbf=`): while the list is being
    cut by the three threads. Read, not measured: the audio thread,
    on the workers' cores since S12b and far above them in priority,
    interrupts a worker that has taken its share of the list, and
    the main thread waits for that share (`gpu_cut_list`).
  - 4 the music changing zone, 6.4 to 8 ms (`fade_into_location`), in
    frames with nothing to spare; 4 clicks in the view; 2 keys in the
    options panel; the lift.
  - 2 with 23 ms in `other=`, outside every timer (t=150.29,
    t=456.51), as S11's capture has once (21.8) and S12b's (22.6).

#### S13b: a worker thread held up by the music

Done; the capture is at the end of this section. The suspect is timed
and the cure tried in the same capture: the profile build switches the cure off
and on every 20 seconds (`var=0`, `var=1`).

- **The timer** (`gpu_cut_list` in `rastq.c`): how long the main
  thread waits for the workers' share once its own is done.
  `cutwait=avg/max` in a second's line, the frame's in a stutter's.
- **The cure on trial** (`worker_boost` in `rastqthr.c`): a worker on
  a job that isn't waited for, which is that cutting and nothing
  else, takes the audio thread's priority (64, the highest a
  program's thread may have) for as long as the job lasts, about
  2 ms, and its own (161) again after. At the same priority the audio
  thread doesn't interrupt it: it waits those 2 ms of its 43, or takes
  the other core. A worker that the audio thread already keeps from
  its core when the job is handed out doesn't take a share at all,
  as before. On in the normal build.
- **The 23 ms**: the parts of the main loop and of the game's loop
  that are in no phase have a note each (`resimage_poll`,
  `MousePollProc`, `status_bio_update`, `ZoomDrawProc`,
  `message_clear_check`, the help scan's two ends, `AnimRecur`,
  `olh_overlay`, `loopmode_switch`). If the next one has no name
  either, it is not the game's code.
- The old pair of variants (the lists always shared, or by the rule
  of step S2) is taken out of `rastq_gpu_sharing`: the rule has been
  the game's since.
- After the capture `VPROF_ALTERNATE` goes back to 0, and the cure
  stays or goes by what the two sides show.

The capture (`profile-step-s13b.txt`, 188 seconds in the game, 167 on
level 3 in the heavy place; the player: "music & see-through are
good"), by `var=`:

| | `var=0`, as it was (99 s) | `var=1`, the cure (89 s) |
|---|---|---|
| seconds where the main thread waited over 1 ms for a worker (`cutwait=` max) | 76% | 0% |
| over 3 ms | 45% | 0% |
| the longest wait | 6.59 ms | 0.32 ms |
| `gpu_run()`'s longest of a second, median | 7.6 ms | 5.5 ms |
| seconds with a frame over 23 ms | 21% | 7% |
| the longest frame of a second, median | 20.8 ms | 18.9 ms |
| fps, median | 57.3 | 57.9 |

- **The suspect was real**: without the cure the main thread waits
  for a worker's share in three seconds of four, over 3 ms in nearly
  half, as long as a stretch of the music lasts at the worst. In the
  seconds whose lists are shared (153 of the 188) it is 89% and 54%.
- **With the cure the wait is gone**, 0.3 ms at most, and the frames
  over 23 ms with it for two thirds. The music is as it was to the
  ear.
- **The average speed there is the same**, 57 fps: the place's own
  weight, not this.
- 7 stutters, none of them a long hand-over: the lift and two frames
  after it, four clicks in the view. No frame with time outside the
  timers in these three minutes: the 23 ms has no name yet, and the
  notes stay for when it comes back.
- `VPROF_ALTERNATE` is back to 0: the cure is on in every build, all
  the time.
