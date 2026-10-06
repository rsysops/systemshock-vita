# Vita performance investigation notes

Findings from an investigation into codebase health and native-resolution
performance on PS Vita, and from a **first attempt at a multicore
software rasterizer**. That attempt's code was discarded so that it could
be redone cleanly, with profiling done only at 960×544 and always from
the same spot in the game.

This file records everything that attempt learned, so it can be rebuilt
without the old code:
- how to measure
- the speed-ups that don't need threads
- the design that worked, and the pitfalls hit along the way
- the measured results
- a recommended order for the retry

## Summary

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

The GPU work that followed the multicore retry took another route, a
palette-indexed renderer fed by the recorder's draw list: see
`docs/PERFORMANCE-GPU.md`.

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

## How to measure

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
    - A `VITA_PROFILE` CMake option adds `-DVITA_PROFILE` and links
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

## Baseline

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

## Speed-ups that don't need threads

Each of these can be done and measured on its own, before any threading.

### FPU `fix_div`

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

### Music: DOSBox OPL3 instead of Nuked

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

### Pre-existing engine bugs worth knowing

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

## Multicore rasterizer: the design that worked

### Why split the rasterizer, and where

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

### Record, then replay

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

### Row bands, bit-exact

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

### Threads

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

### Verification

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

## Pitfalls found, in the order they were hit

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

## Results of the first attempt

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

## Results of the retry

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

## Recommended order for the retry

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

## Ranked recommendations

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
   than wiring `OpenGL.cc` to vitaGL: see `docs/PERFORMANCE-GPU.md`.
5. **Do not touch AI frame-coupling** (`newai.c`). It is
   original/upstream behaviour, and changing it would diverge from the
   original game's feel.
