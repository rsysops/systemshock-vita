# Architecture

## Overview

This project is a **PS Vita port of Shockolate** ([Interrupt/systemshock](https://github.com/Interrupt/systemshock)), the SDL2-based cross-platform source port of *System Shock*, built from the original PowerPC Mac source Night Dive Studios released publicly. The codebase is primarily C (C99) with a handful of C++ (C++11) files, and it still carries a lot of the shape of the original Mac application — the Vita code is written directly into the shared files rather than kept in a separate platform tree. The desktop branches, switches and build options it stood beside have been removed: the tree only builds for the Vita.

At runtime the game requires the original System Shock data files (`DATA`/`SOUND`), which are not part of the repo and must be copied onto the device under `ux0:data/systemshock/res/`.

## Repository layout

| Path | Purpose |
|---|---|
| `CMakeLists.txt` | Build definition (Vita only) |
| `build.sh` | Vita build entry point (Docker) |
| `src/` | All engine and game source (see below) |
| `vita/` | Vita packaging/build glue: `Dockerfile`, `vita.cmake`, `segment-gap.ld` (linker script), `sce_sys/` (icon/LiveArea assets) |
| `tests/` | PC harnesses that compile parts of the libraries natively: `fix_div/` (fixed-point division) and `rastq/` (rasterizer queue) |
| `docs/` | This file, the key mappings, the profiling guide and the performance findings |
| `build/`, `build-profile/` | Out-of-tree CMake build output of the default and profile builds (git-ignored) |

## Source tree (`src/`)

### `src/MacSrc/` — application/platform layer
Inherited from the original Mac codebase; hosts process entry and OS-facing glue: `Shock.c` (`main()`, SDL/vita2d init, presentation), `VitaGpu.c` (the GPU renderer, see [Rendering paths](#rendering-paths)), `InitMac.c`, `Prefs.c` (settings/keybinds), `SDLSound.c`, `Modding.c` (fan-mission/mod loading), `Xmi.c`, `ShockBitmap.c`, `MacTune.c`. These files, with `src/MusicSrc/MusicDevice.c`, are compiled straight into the `systemshock` executable.

### `src/GameSrc/` — game logic (~120 files, built as `GAME_LIB`)
Grouped by concern:
- **Rendering** — the `fr*` frame-renderer family (`frmain.c`, `frcamera.c`, `frobj.c`, `frclip.c`, `frterr.c`, …), plus `render.c`, `rendtool.c`, `gamerend.c`.
- **AI** — `ai.c`, `newai.c`, `pathfind.c`, `schedule.c`.
- **Physics/collision** — `physics.c`.
- **HUD / MFDs** — `hud.c`, `mfd*.c`, `newmfd.c`, `cybermfd.c`, `cardmfd.c`, `gearmfd.c`, `ammomfd.c`; `hudkeep.c` (keeps pictures of what the HUD redraws identically each frame); `olh.c`, `olhscan.c` (on-screen help).
- **Player/inventory/combat** — `player.c`, `invent.c`, `combat.c`, `weapons.c`, `damage.c`, `grenades.c`.
- **World/objects** — `objects.c`, `objsim.c`, `objuse.c`, `objload.c`, `gameobj.c`, `trigger.c`.
- **Automap / cyberspace** — `amap.c`, `automap.c`, `fullamap.c`, `cyber.c`, `cybrnd.c`.
- **Game loop / lifecycle** — `mainloop.c`, `gameloop.c`, `gamesys.c`, `gametime.c`, `setup.c`, `cutsloop.c`, `saveload.c`.
- **Profiling** — `vprof.c`, the on-device profiler of the profile build (see [PROFILING.md](PROFILING.md)).

### `src/MusicSrc/`
`MusicDevice.c` — MIDI/music device abstraction.

### `src/Libraries/` — reusable engine libraries
Each row with sources is its own CMake target, defined in `src/Libraries/CMakeLists.txt`:

| Library | Purpose |
|---|---|
| `2D` | Bitmap/blit primitives |
| `3D` | 3D math and texture mapping; the rasterizer queue and its worker threads (`rastq.c`, `rastqthr.c`) |
| `GR` | Graphics abstraction (its sources live in `2D/Source/GR`) |
| `RES` | Reads the original `.res` game data files (`resacc.c`, `lzw.c`) |
| `LG` | "Looking Glass" core utilities: temporary memory (`tmpalloc.c`, `stack.c`), per-thread slots for the rasterizer's workers (`lgslot.c`), logging (vendored, `LG/Source/LOG`) |
| `INPUT` | Input; `sdl_events.c` turns SDL controller, touch and gyro events into the game's keyboard and mouse events |
| `UI` | UI widgets |
| `RND` | Random number generation |
| `VOX` | Voxel-ish rendering |
| `FIX` | Fixed-point math |
| `FIXPP` | C++ fixed-point class, header only (`fixpp.h`), used by `EDMS` |
| `EDMS` | Collision detection (C++) |
| `DSTRUCT` | Data structures |
| `PALETTE` | Palette handling |
| `AFILE` | Movie/AFILE playback |
| `adlmidi` | FM-synth MIDI (vendored libADLMIDI) |
| `H`, `SND` | Shared headers only, no target |

## Vita porting layer

There is **no dedicated platform tree**: the Vita code sits directly in the shared files, unconditionally, with no `VITA` switch. The only exception is the handful of files the PC tests in `tests/` also compile (`lgslot.c`, `fix.c`, `mode.c`, `rastq.c`, `rastqthr.c`, `vprof.h`, and the vendored `log.c`): they keep a PC branch, chosen with the compiler's built-in `__vita__`. The main concentrations of Vita code are:

- **`src/MacSrc/Shock.c`** — `main()` prologue (`chdir` into `VITA_PATH`, CPU/GPU clock settings); the heap size (`_newlib_heap_size_user`) and `sceClibMem*`-based `memcpy`/`memset`/`memmove`/`memcmp`; `InitVita2D()` / `ResizeVita2D()`, which create the screen's paletted texture (`SCE_GXM_TEXTURE_FORMAT_P8_ABGR`) and the GPU's canvases through **vita2d** (SceGxm-based); controller/gyro init (`OpenController()`, `OpenGyro()`); aspect-ratio letterboxing (`SetRenderRect()`); presentation (`SDLDraw()`).
- **`src/MacSrc/VitaGpu.c`** — the GPU renderer: SceGxm draw calls and shaders compiled at start-up.
- **`src/Libraries/INPUT/Source/sdl_events.c`** — the largest concentration of Vita logic: analog-stick movement/aim, rear/front touchpad-to-mouse emulation, gyro-based look, and a virtual-keyboard text input buffer.
- **`src/Libraries/3D/Source/rastqthr.c`**, **`src/Libraries/LG/Source/lgslot.c`** — the rasterizer's worker threads and their per-thread slots, on SceKernel threads and semaphores.
- **`src/GameSrc/vprof.c`** — the profiler's overlay and log, in the profile build only.
- **`src/MacSrc/Prefs.c`**, **`src/GameSrc/wrapper.c`** — the Vita settings (gyro, look speeds, cursor) and their `Vita Options` menu page.
- **`vita/vita.cmake`** — defines `VITA_APP_NAME` and `VITA_TITLEID` (`SHOK00001`), calls `vita_create_self` / `vita_create_vpk`, and bundles the `vita/sce_sys/` icon and LiveArea assets into the package.

## Rendering paths

Three paths can fill the 3D view's pixels. All three look the same; they differ in who does the work:

| Path | What happens |
|---|---|
| GPU | The 3D pass records its calls in the rasterizer queue (`3D/Source/rastq.c`) instead of drawing, and the queue hands its list to `VitaGpu.c`, which draws flat polygons, texture maps and shaded polygons into a canvas of its own; the CPU still draws what the GPU does not take. |
| Three cores | The queue replays the recorded calls in the same order on three threads, each filling a band of rows. |
| One core | The original software renderer: each finished 2D polygon goes straight to the pixel-filling mappers of the `2D` library. |

The game uses the GPU, and three cores for the views the GPU isn't given or when it can't be used; there is no setting. `main()` in `Shock.c` sets the queue up that way once, at start-up. The one-core path is what the queue's tests (`tests/rastq`) compare the others against.

The GPU's shaders are compiled when the game starts, which needs `ur0:data/libshacccg.suprx`; without it the game says so in a message box and stays on the CPU. The design and measurements are in [PERFORMANCE-CPU.md](PERFORMANCE-CPU.md) ("Multicore rasterizer") and [PERFORMANCE-GPU.md](PERFORMANCE-GPU.md).

## Build system

The root `CMakeLists.txt` only builds for the Vita and expects the VitaSDK toolchain file. It sets C99/C++11, finds SDL2 and SDL2_mixer in the SDK, and:
1. Sets aggressive Cortex-A9/NEON compile flags (`-Ofast -mcpu=cortex-a9 -mfpu=neon`).
2. Sets `VITA_LIBS`, linking SDL2, vita2d, libjpeg/png/webp/z, vorbis/ogg, mikmod/modplug/xmp-lite, opus(file), FLAC, mpg123, and Vita system stub libraries (`SceCtrl`, `SceTouch`, `SceMotion`, `SceGxm`, `taihen`, etc.).
3. Includes `vita/vita.cmake` to produce the `.vpk`.

Its one option is `ENABLE_VITA_PROFILE`, which defines `VITA_PROFILE` for the profile build (`./build.sh profile`).

It then adds `src/Libraries/`, defines the `MAC_SRC`/`GAME_SRC` file lists, and links the final `systemshock` executable against `GAME_LIB` and every library target.

**Docker build**: `build.sh` builds a `systemshock-vita-vitasdk` image from `vita/Dockerfile` (`FROM vitasdk/vitasdk:2026.08`, which ships all required prebuilt libraries), then runs `cmake` + `make` inside the container against a bind-mounted workspace, producing `build/systemshock.vpk`. No local VitaSDK install is required for this path (a local VitaSDK setup is also possible but needs `VITASDK` set and a pacman symlink for `vdpm install`).

## Runtime data flow

1. **Entry** — `main()` in `src/MacSrc/Shock.c` runs the Vita-specific prologue (`chdir`), then initializes SDL and vita2d (`InitSDL()` → `InitVita2D()`).
2. **Main loop dispatch** — control passes to `mainloop()` in `src/GameSrc/mainloop.c`, which selects the active loop via the `citadel_loops[]` function-pointer table (game / setup / cutscene / automap modes).
3. **Per-frame update** — the active mode's function (e.g. `game_loop()` in `gameloop.c`) advances game time, runs AI (`ai_run`) and game systems (`gamesys_run`), updates animations, then calls into rendering (`render_run`) when `localChanges` is set.
4. **Presentation** — everything the game draws in software (HUD, panels, menus, and the 3D view on the CPU renderers) lands in an 8-bit screen buffer. `SDLDraw()` in `Shock.c` copies it into the screen's paletted vita2d texture (`palettedTexturePointer`) and draws that, scaled, through vita2d/SceGxm. A 3D view the GPU drew is shown straight from its canvas instead, without a copy: alone if it fills the screen, or over the screen buffer's picture if it is the paneled view's window.

See also [KEYMAPS.md](KEYMAPS.md) for the controls and [PROFILING.md](PROFILING.md) for measuring the game on the console.
