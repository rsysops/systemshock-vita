# Architecture

## Overview

This project is a **PS Vita port of Shockolate** ([Interrupt/systemshock](https://github.com/Interrupt/systemshock)), the SDL2-based cross-platform source port of *System Shock*, built from the original PowerPC Mac source Night Dive Studios released publicly. The codebase is primarily C (C99) with a handful of C++ (C++11) files, and it still carries a lot of the shape of the original Mac application — the port adapts it to Vita mainly by adding `#ifdef VITA` branches into the shared code rather than maintaining a separate platform tree.

At runtime the game requires the original System Shock data files (`DATA`/`SOUND`), which are not part of the repo and must be copied onto the device under `ux0:data/systemshock/res/`.

## Repository layout

| Path | Purpose |
|---|---|
| `CMakeLists.txt`, `CMakeLists.32bit.txt` | Main build definitions (Vita + desktop targets) |
| `build.sh`, `build.bat`, `build_deps.sh`, `build_win32.sh`, `build_win64.sh` | Platform build entry points |
| `src/` | All engine and game source (see below) |
| `vita/` | Vita packaging/build glue: `Dockerfile`, `vita.cmake`, `sce_sys/` (icon/LiveArea assets) |
| `externals/sanitizers/` | Vendored CMake sanitizers module |
| `shaders/`, `windows/`, `osx-linux/` | Desktop-only shader and packaging assets |
| `build/` | Out-of-tree CMake build output (git-ignored) |

## Source tree (`src/`)

### `src/MacSrc/` — application/platform layer
Inherited from the original Mac codebase; hosts process entry and OS-facing glue: `Shock.c` (`main()`, SDL/vita2d init), `InitMac.c`, `Prefs.c` (settings/keybinds), `SDLSound.c`, `Modding.c` (fan-mission/mod loading), `OpenGL.cc`, `Xmi.c`, `ShockBitmap.c`, `MacTune.c`.

### `src/GameSrc/` — game logic (~100 files, built as `GAME_LIB`)
Grouped by concern:
- **Rendering** — the `fr*` frame-renderer family (`frmain.c`, `frcamera.c`, `frobj.c`, `frclip.c`, `frterr.c`, …), plus `render.c`, `rendtool.c`, `gamerend.c`.
- **AI** — `ai.c`, `newai.c`, `pathfind.c`, `schedule.c`.
- **Physics/collision** — `physics.c`.
- **HUD / MFDs** — `hud.c`, `mfd*.c`, `newmfd.c`, `cybermfd.c`, `cardmfd.c`, `gearmfd.c`, `ammomfd.c`.
- **Player/inventory/combat** — `player.c`, `invent.c`, `combat.c`, `weapons.c`, `damage.c`, `grenades.c`.
- **World/objects** — `objects.c`, `objsim.c`, `objuse.c`, `objload.c`, `gameobj.c`, `trigger.c`.
- **Automap / cyberspace** — `amap.c`, `automap.c`, `fullamap.c`, `cyber.c`, `cybrnd.c`.
- **Game loop / lifecycle** — `mainloop.c`, `gameloop.c`, `gamesys.c`, `gametime.c`, `setup.c`, `cutsloop.c`, `saveload.c`.

### `src/MusicSrc/`
`MusicDevice.c` — MIDI/music device abstraction.

### `src/Libraries/` — reusable engine libraries
Each has its own CMake target, added via `add_subdirectory(src/Libraries/)`:

| Library | Purpose |
|---|---|
| `2D` | Bitmap/blit primitives |
| `3D` | 3D math and texture mapping |
| `GR` | Graphics abstraction |
| `RES` | Reads the original `.res` game data files (`resacc.c`, `lzw.c`) |
| `LG` | "Looking Glass" core utilities: memory allocator, logging (`LG/Source/LOG`) |
| `INPUT` | Input abstraction; `sdl_events.c` is the primary cross-platform input layer |
| `UI` | UI widgets |
| `RND` | Random number generation |
| `VOX` | Voxel-ish rendering |
| `FIX` / `FIXPP` | Fixed-point math |
| `EDMS` | Collision detection (C++) |
| `DSTRUCT` | Data structures |
| `PALETTE` | Palette handling |
| `AFILE` | Movie/AFILE playback |
| `adlmidi` | FM-synth MIDI |

## Vita porting layer

There is **no dedicated `src/vita/` platform tree** — the root `CMakeLists.txt` references `include_directories(src/vita)` and a `vita_SRCS` list, but that list is never populated, so it's effectively vestigial. Instead, Vita support is implemented as `#ifdef VITA` / `#ifdef VITA2D` blocks woven directly into shared files (~25 of them). The main concentrations are:

- **`src/MacSrc/Shock.c`** — Vita-specific `main()` prologue (`chdir` into `VITA_PATH`); `InitVita2D()` / `ClearVita2D()`, which create a paletted texture (`SCE_GXM_TEXTURE_FORMAT_P8_ABGR`) via **vita2d** (SceGxm-based) that SDL2's software renderer writes into; controller/gyro init (`OpenController()`, `OpenGyro()`); aspect-ratio letterboxing (`SetRenderRect()`).
- **`src/GameSrc/gameloop.c`** — Vita heap size override and `sceClibMem*`-based `memcpy`/`memset`/`memmove`/`memcmp` for performance.
- **`src/Libraries/INPUT/Source/sdl_events.c`** — the largest concentration of Vita logic: analog-stick movement/aim, rear/front touchpad-to-mouse emulation, gyro-based look, and a virtual-keyboard text input buffer.
- Other touched files: `Prefs.c/h`, `ShockBitmap.c`, `wrapper.c`, `fullscrn.c`, `screen.c`, `setup.c`, `gr2ss.c`, `newmfd.c`, `amaploop.c`, `LG/Source/LOG/src/log.c`, `2D/Source/mode.c`.
- **`vita/vita.cmake`** — defines `VITA_APP_NAME` and `VITA_TITLEID` (`SHOK00001`), calls `vita_create_self` / `vita_create_vpk`, and bundles the `vita/sce_sys/` icon and LiveArea assets into the package.

## Build system

The root `CMakeLists.txt` sets C99/C++11 and exposes options for OpenGL/SDL2/SDL2_mixer/FluidSynth (`ON`/`BUNDLED`/`OFF`). When `VITA` is set, it:
1. Adds `-DVITA -DVITA2D` and aggressive Cortex-A9/NEON compile flags (`-Ofast -mcpu=cortex-a9 -mfpu=neon`).
2. Sets `VITA_LIBS`, linking SDL2, vita2d, libjpeg/png/webp/z, vorbis/ogg, mikmod/modplug/xmp-lite, opus(file), FLAC, mpg123, and Vita system stub libraries (`SceCtrl`, `SceTouch`, `SceMotion`, `SceGxm`, `taihen`, etc.).
3. Includes `vita/vita.cmake` to produce the `.vpk`.

It then adds `src/Libraries/`, defines the `MAC_SRC`/`GAME_SRC` file lists, and links the final `systemshock` executable against `GAME_LIB` and every library target.

**Docker build**: `build.sh` builds a `systemshock-vita-vitasdk` image from `vita/Dockerfile` (`FROM vitasdk/vitasdk:2026.08`, which ships all required prebuilt libraries), then runs `cmake` + `make` inside the container against a bind-mounted workspace, producing `build/systemshock.vpk`. No local VitaSDK install is required for this path (a local VitaSDK setup is also possible but needs `VITASDK` set and a pacman symlink for `vdpm install`).

## Runtime data flow

1. **Entry** — `main()` in `src/MacSrc/Shock.c` runs the Vita-specific prologue (`chdir`), then initializes SDL and vita2d (`InitSDL()` → `InitVita2D()`).
2. **Main loop dispatch** — control passes to `mainloop()` in `src/GameSrc/mainloop.c`, which selects the active loop via the `citadel_loops[]` function-pointer table (game / setup / cutscene / automap modes).
3. **Per-frame update** — the active mode's function (e.g. `game_loop()` in `gameloop.c`) advances game time, runs AI (`ai_run`) and game systems (`gamesys_run`), updates animations, then calls into rendering (`render_run`) when `localChanges` is set.
4. **Presentation** — rendering writes into an SDL software surface, which on Vita is blitted into the vita2d paletted texture (`palettedTexturePointer`) and presented to the screen through vita2d/SceGxm.
