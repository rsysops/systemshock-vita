# Reading guide

A path through the source, for someone who knows C/C++ and wants to understand how the game works. Tick the boxes as you go.

The repository holds about 203,000 lines of C and C++ (175,000 without blank lines). Reading them front to back is not the goal: most of them are leaf code, such as pixel mappers, the vendored MIDI synthesizer and MFD mini-games, which you only need to know exists. The guide starts at `main()`, goes through one frame of the game, and then takes one subsystem at a time, each read in the order its data flows.

## Contents

- [How to read this code](#how-to-read-this-code)
- [Phase 1: Orientation](#phase-1-orientation)
- [Phase 2: Start-up](#phase-2-start-up)
- [Phase 3: One frame](#phase-3-one-frame)
- [Phase 4: Foundations](#phase-4-foundations)
- [Phase 5: The world model](#phase-5-the-world-model)
- [Phase 6: Simulation](#phase-6-simulation)
- [Phase 7: The 3D renderer](#phase-7-the-3d-renderer)
- [Phase 8: Input, UI and HUD](#phase-8-input-ui-and-hud)
- [Phase 9: The other modes](#phase-9-the-other-modes)
- [Phase 10: Sound and music](#phase-10-sound-and-music)
- [Phase 11: The Vita layer and the tooling (optional)](#phase-11-the-vita-layer-and-the-tooling-optional)
- [Final check](#final-check)
- [Glossary](#glossary)
- [Where each directory is read](#where-each-directory-is-read)

## How to read this code

**Top-down, then one thread at a time.** Start from `main()` and one call of `game_loop()`. When a function calls into something new, note its name and keep going; go down only along the path the current phase is about. Come back to the notes in the phase that covers them.

**Headers before bodies.** The structs in `src/GameSrc/Headers/` and in each library's `.h` say more per line than the `.c` files do. For every new subsystem, read its header first and write down its main struct and its five or six main functions.

**Skip on purpose.** Some code is worth knowing about but not worth reading line by line:
- vendored code: `src/Libraries/adlmidi/` (the OPL3 FM synthesizer) and `src/Libraries/LG/Source/LOG/` (the logger);
- most of `src/Libraries/2D/Source/Flat8/`, `Gen/` and `Clip/`: dozens of near-identical pixel loops; read one of each kind;
- `mfdgames.c` (the MFD mini-games), unless you are curious;
- commented-out code and `// KLC - does nothing!` blocks: the original authors left a lot of it.

**Know the code's age.** This is 1994 Looking Glass code, ported from DOS to the Mac, then to SDL (Shockolate), then to the Vita. Expect fixed-point maths instead of floats, global state everywhere, `errtype` returns that are rarely checked, and RCS `$Log` headers at the top of files (skip them). Comments signed `KLC` are from the Mac port.

**Keep two notes files.**
1. A glossary: every unknown term, with the file it came from (see the [Glossary](#glossary) below for a head start).
2. A call map: short call chains such as `main → mainloop → game_loop → render_run → fr_rend`. A dozen of these are the skeleton of the whole program.

**Tools.**
- **Finding a definition:** the code puts a function's return type at the start of its line, so `git grep -n "^errtype render_run"` or `git grep -n "^[a-z].* render_run("` finds a definition; `git grep -n "render_run("` finds every caller.
- **Editor navigation:** the VS Code C/C++ extension's "Go to Definition" and "Find All References" work without any setup on most symbols. The Docker build writes no `compile_commands.json`, so don't expect exact results on macros.
- **History:** `git log --follow -p <file>` shows why a Vita change was made; the commit messages are written to be read.
- **Running code:** the PC tests in `tests/` compile parts of the libraries natively. Each has a `run.sh`, and stepping through one in a debugger is the fastest way to see a library working.
- **The docs:** [ARCHITECTURE.md](ARCHITECTURE.md) is the map; [PERFORMANCE.md](PERFORMANCE.md) explains the renderer changes in depth.

**Pace.** The estimates below are for careful reading with notes. Phases 1 to 5 build on each other; do them in order. After that, phases 6 to 10 can be read in any order, though 7 is the largest and the most rewarding.

## Phase 1: Orientation

*About 2 hours. No code yet.*

- [ ] [README.md](../README.md): what the port is and how it is controlled
- [ ] [ARCHITECTURE.md](ARCHITECTURE.md), in full: the map you'll keep coming back to
- [ ] [KEYMAPS.md](KEYMAPS.md): what the input layer has to produce
- [ ] [CMakeLists.txt](../CMakeLists.txt) and [src/Libraries/CMakeLists.txt](../src/Libraries/CMakeLists.txt): which libraries are built, which files go into `GAME_LIB`, what is linked
- [ ] [build.sh](../build.sh) and [vita/vita.cmake](../vita/vita.cmake): how the `.vpk` is made
- [ ] Play the game for an hour if you haven't lately. MFDs, cyberspace, the automap, audio logs, hacking cameras: the game's vocabulary is the code's vocabulary, and most file names come from it

## Phase 2: Start-up

*About 3 hours.*

- [ ] [src/MacSrc/Shock.c](../src/MacSrc/Shock.c), `main()`: the clocks, the data check (`VitaCheckData()`), `InitSDL()` → `InitVita2D()`, the rasterizer queue's set-up (`rastq_set_mode()`, `rastq_use_gpu()`), then the call into the game
- [ ] [src/GameSrc/init.c](../src/GameSrc/init.c), `init_all()`: write down each subsystem it initializes, in order. This list is a table of contents for the rest of the guide
- [ ] [src/GameSrc/mainloop.c](../src/GameSrc/mainloop.c): `mainloop()` and its three tables, `citadel_loops[]`, `enter_modes[]` and `exit_modes[]`. A "loop mode" is the game's top-level state: game, full-screen game, setup (the main menu), cutscene, automap
- [ ] [src/GameSrc/Headers/mainloop.h](../src/GameSrc/Headers/mainloop.h): the `_current_loop` / `_new_mode` switch, and `loopLine()`, a macro that now does nothing but mark each step of a loop
- [ ] **Exercise:** write the call chain from `main()` to the first call of `setup_loop()`, and from there to the first call of `game_loop()` once a game is started

## Phase 3: One frame

*About 3 hours. The most important phase: everything later hangs off this function.*

- [ ] [src/GameSrc/gameloop.c](../src/GameSrc/gameloop.c), `game_loop()`: each `loopLine(...)` is one subsystem. Ignore the `VPROF_*` wrappers for now; they only time the call inside them
- [ ] The dirty flags: `_change_flag`, `chg_set_flg()` and `chg_unset_flg()` in `mainloop.h`, and `localChanges`. Find who sets `INVENTORY_UPDATE` and `MFD_UPDATE`, and when the 3D view is redrawn
- [ ] [src/GameSrc/gametime.c](../src/GameSrc/gametime.c), `update_state()`: how game time moves
- [ ] [src/GameSrc/gamesys.c](../src/GameSrc/gamesys.c), `gamesys_run()`: skim it; it is a list of per-frame chores (screen effects, SHODAN's takeover, fatigue, drugs, continuous traps, radiation zones)
- [ ] `SDLDraw()` in [Shock.c](../src/MacSrc/Shock.c): how the 8-bit screen buffer, with its palette (`SetSDLPalette()`), reaches the screen
- [ ] **Exercise:** list the steps of one frame in order, with the file each lives in. Keep the list: phases 6 to 10 each take one line of it

## Phase 4: Foundations

*About 5 hours. Short readings of the libraries that every other file uses, so that later phases read smoothly.*

- [ ] [src/Libraries/H/lg_types.h](../src/Libraries/H/lg_types.h) and [error.h](../src/Libraries/H/error.h): `uchar`, `ushort`, `errtype` and friends
- [ ] [src/Libraries/FIX/Source/fix.h](../src/Libraries/FIX/Source/fix.h): 16.16 fixed point (`fix`, `fix_make()`, `fix_int()`, `fix_mul()`, `fix_div()`) and `fixang` angles. Run [tests/fix_div/run.sh](../tests/fix_div/run.sh) and read the test
- [ ] [src/Libraries/LG/Source/](../src/Libraries/LG/Source/): `dbg.h` (the `TRACE`/`DEBUG`/`ERROR` macros you see everywhere), `tmpalloc.c` and `stack.c` (temporary memory)
- [ ] [src/Libraries/RES/Source/res.h](../src/Libraries/RES/Source/res.h): the resource system that holds every piece of game data. Learn `Id` (a resource), `Ref` (an item inside a compound resource), `ResLock()` / `ResUnlock()` / `ResGet()`, and how a `.res` file is opened
- [ ] The rest of `RES`: `resfile.c` (opening files), `resacc.c` and `refacc.c` (getting at a resource or a ref), `lzw.c` (decompression). Then [src/GameSrc/resimage.c](../src/GameSrc/resimage.c), which reads the art files into memory on a thread
- [ ] Run [tests/respack/run.sh](../tests/respack/run.sh) and [tests/resimage/run.sh](../tests/resimage/run.sh), and read what they check
- [ ] [src/Libraries/DSTRUCT/Source/](../src/Libraries/DSTRUCT/Source/): skim `array.h`, `hash.h`, `pqueue.h` and `rect.h`, to recognize them when they come up
- [ ] **Exercise:** pick one `Id` used in `init.c` (a palette or a font), and follow it from `ResLock()` to the bytes in memory

## Phase 5: The world model

*About 6 hours. The data every simulation step reads and writes.*

- [ ] [src/GameSrc/Headers/map.h](../src/GameSrc/Headers/map.h): `MapElem` (one tile: type, heights, textures, flags) and `FullMap`. The level is a grid of these, and `global_fullmap` is the current one
- [ ] [src/GameSrc/Headers/objects.h](../src/GameSrc/Headers/objects.h): `Obj`, `ObjSpec` and `ObjRef`. Every thing in the world is an `Obj` in the `objs[]` array, with class-specific data in an `ObjSpec` and one `ObjRef` per map tile it touches
- [ ] [objapp.h](../src/GameSrc/Headers/objapp.h) (`ObjClass`, `ObjLoc`) and [objclass.h](../src/GameSrc/Headers/objclass.h): classes, subclasses and types, packed into a "triple" by `MAKETRIP()`
- [ ] [objprop.h](../src/GameSrc/Headers/objprop.h): the per-type property tables, read from the game data
- [ ] [src/GameSrc/objects.c](../src/GameSrc/objects.c): `ObjsInit()`, `ObjAndSpecGrab()`, `ObjPlace()`, `ObjDel()`, `ObjUpdateLocs()`: how objects are linked into the map
- [ ] [src/GameSrc/objsim.c](../src/GameSrc/objsim.c): `obj_create_base()` and the code around it; and [objload.c](../src/GameSrc/objload.c)
- [ ] [src/GameSrc/Headers/player.h](../src/GameSrc/Headers/player.h): the `Player` struct (`player_struct`), including the quest bits (`QUESTBIT_GET()`) that drive the story
- [ ] [saveload.c](../src/GameSrc/saveload.c) and [archiveformat.c](../src/GameSrc/archiveformat.c): what a saved game holds. If this reads clearly, the world model is understood
- [ ] **Exercise:** follow `obj_create_base()` to the point where the new object appears in its tile's `ObjRef` list

## Phase 6: Simulation

*About 8 hours.*

- [ ] [src/GameSrc/physics.c](../src/GameSrc/physics.c), `physics_run()`: the game side of physics
- [ ] The EDMS collision library (C++), [src/Libraries/EDMS/Source/](../src/Libraries/EDMS/Source/): `edms.h`, then `interfac.cc`, then `collide.cc`. Skip the models' maths in `MODELS/` on a first pass
- [ ] AI: [newai.c](../src/GameSrc/newai.c) (`ai_run()`), [ai.c](../src/GameSrc/ai.c), [pathfind.c](../src/GameSrc/pathfind.c), [schedule.c](../src/GameSrc/schedule.c)
- [ ] Combat: [combat.c](../src/GameSrc/combat.c), [weapons.c](../src/GameSrc/weapons.c), [damage.c](../src/GameSrc/damage.c), [grenades.c](../src/GameSrc/grenades.c)
- [ ] Interaction: [objuse.c](../src/GameSrc/objuse.c) (using an object: doors, switches, pick-ups) and [trigger.c](../src/GameSrc/trigger.c) (traps and triggers, which run most of the level scripting)
- [ ] [effect.c](../src/GameSrc/effect.c) and [drugs.c](../src/GameSrc/drugs.c): animated effects, and drugs as an example of timed player state
- [ ] **Exercise:** follow one shot of the pistol from the button press to `damage.c` and the target's hit points

## Phase 7: The 3D renderer

*About 12 hours. The largest phase; three renderers share one pipeline.*

- [ ] [src/GameSrc/render.c](../src/GameSrc/render.c), `render_run()`, then [frmain.c](../src/GameSrc/frmain.c), `fr_rend()`: where a frame's view begins
- [ ] The `fr*` pipeline, in this order: [frsetup.c](../src/GameSrc/frsetup.c) (the view's context, `fauxrend_context` in `frintern.h`), [frcamera.c](../src/GameSrc/frcamera.c), [frclip.c](../src/GameSrc/frclip.c) (which tiles are visible), [frterr.c](../src/GameSrc/frterr.c) (walls, floors and ceilings), [frobj.c](../src/GameSrc/frobj.c) (objects)
- [ ] The 3D library, [src/Libraries/3D/Source/3d.h](../src/Libraries/3D/Source/3d.h) first (`g3s_point`, `g3s_phandle`), then `matrix.c`, `points.c`, `polygon.c`, `tmap.c`
- [ ] The 2D library, [src/Libraries/2D/Source/2d.h](../src/Libraries/2D/Source/2d.h), `grs_canvas` and `grs_bitmap` in `canvas.c` and `bitmap.c`. Then read two mappers in `Flat8/` in full: `fl8ply.c` (flat polygons, short) and `fl8lf.c` (the floor texture mapper). Skim the others' names only
- [ ] The rasterizer queue: [rastq.c](../src/Libraries/3D/Source/rastq.c), [rastqthr.c](../src/Libraries/3D/Source/rastqthr.c) and [lgslot.c](../src/Libraries/LG/Source/lgslot.c); with [PERFORMANCE.md, "Multicore rasterizer: the design that worked"](PERFORMANCE.md#multicore-rasterizer-the-design-that-worked). Run [tests/rastq/run.sh](../tests/rastq/run.sh)
- [ ] The GPU renderer: [src/MacSrc/VitaGpu.c](../src/MacSrc/VitaGpu.c), with [PERFORMANCE.md Part 2](PERFORMANCE.md#part-2-the-gpu-renderer); and `VitaShowView()` / `VitaSyncView()` in `Shock.c`
- [ ] [gamerend.c](../src/GameSrc/gamerend.c): what is drawn around and over the view
- [ ] **Exercise:** draw the path of one textured wall from `frterr.c` to a pixel on the screen, on each of the three paths (one core, three cores, GPU)

## Phase 8: Input, UI and HUD

*About 8 hours.*

- [ ] [src/Libraries/INPUT/Source/sdl_events.c](../src/Libraries/INPUT/Source/sdl_events.c): the Vita controls turned into keyboard and mouse events (`ProcessControllerAxisMotion()`, `PushKeyEvent()`, touch and gyro); then `kbcook.c` and `mouse.c`
- [ ] The UI library, [src/Libraries/UI/Source/](../src/Libraries/UI/Source/): `region.h` (`LGRegion`, a tree of screen rectangles), `event.h` (`uiEvent` and how events reach a region's handler), `hotkey.c`, `cursors.c`
- [ ] [src/GameSrc/input.c](../src/GameSrc/input.c): the 3D view's handlers (`view3d_mouse_handler()`, `use_object_in_3d()`)
- [ ] The HUD: [hud.c](../src/GameSrc/hud.c) and [hudkeep.c](../src/GameSrc/hudkeep.c); run [tests/hudkeep/run.sh](../tests/hudkeep/run.sh)
- [ ] The MFDs: [newmfd.c](../src/GameSrc/newmfd.c) (`mfd_update()`) first, then [mfdfunc.c](../src/GameSrc/mfdfunc.c) and [mfdpanel.c](../src/GameSrc/mfdpanel.c); one or two of the small ones (`ammomfd.c`, `cardmfd.c`); skim [mfdgames.c](../src/GameSrc/mfdgames.c)
- [ ] [invent.c](../src/GameSrc/invent.c) (`inventory_draw()`) and [status.c](../src/GameSrc/status.c) (the vitals bars)
- [ ] The in-game menu, [wrapper.c](../src/GameSrc/wrapper.c), and the settings it writes, [src/MacSrc/Prefs.c](../src/MacSrc/Prefs.c)
- [ ] [olh.c](../src/GameSrc/olh.c) and [olhscan.c](../src/GameSrc/olhscan.c): the on-line help shown over the objects the cursor points at
- [ ] **Exercise:** follow a press of △ (next weapon) from `sdl_events.c` to the weapon MFD being redrawn

## Phase 9: The other modes

*About 5 hours. Each is one more entry in `citadel_loops[]`.*

- [ ] [setup.c](../src/GameSrc/setup.c): the main menu and the start of a new game (`setup_loop()`)
- [ ] The automap: [amap.c](../src/GameSrc/amap.c), [automap.c](../src/GameSrc/automap.c), [fullamap.c](../src/GameSrc/fullamap.c), [amaploop.c](../src/GameSrc/amaploop.c) (`automap_loop()`)
- [ ] Cyberspace: [cyber.c](../src/GameSrc/cyber.c) and [cybrnd.c](../src/GameSrc/cybrnd.c): same engine, different rules
- [ ] Cutscenes: [cutsloop.c](../src/GameSrc/cutsloop.c) (`cutscene_loop()`) and the movie player in [src/Libraries/AFILE/Source/](../src/Libraries/AFILE/Source/) (`movie.c`, `afile.c`)

## Phase 10: Sound and music

*About 4 hours.*

- [ ] [src/MacSrc/SDLSound.c](../src/MacSrc/SDLSound.c): sound effects through SDL_mixer, and the thread that decodes them ahead of time (`snd_preload()`, called from `digifx.c`)
- [ ] Music: [musicai.c](../src/GameSrc/musicai.c) (picks the music for the situation), [src/MacSrc/MacTune.c](../src/MacSrc/MacTune.c), [src/MacSrc/Xmi.c](../src/MacSrc/Xmi.c), [src/MusicSrc/MusicDevice.c](../src/MusicSrc/MusicDevice.c)
- [ ] `adlmidi`: learn its interface only (how `MusicDevice.c` calls it); [PERFORMANCE.md, "Music: DOSBox OPL3 instead of Nuked"](PERFORMANCE.md#music-dosbox-opl3-instead-of-nuked) explains the choice of emulator
- [ ] [email.c](../src/GameSrc/email.c): e-mails and audio logs. Run [tests/alog/run.sh](../tests/alog/run.sh)

## Phase 11: The Vita layer and the tooling (optional)

*About 6 hours.*

- [ ] `git grep -n "sce[A-Z]" -- src` lists every Vita system call. Look at each one in its context
- [ ] The Vita parts listed in [ARCHITECTURE.md, "Vita porting layer"](ARCHITECTURE.md#vita-porting-layer), each in its file
- [ ] The profiler: [src/GameSrc/vprof.c](../src/GameSrc/vprof.c) and [src/Libraries/H/vprof.h](../src/Libraries/H/vprof.h), with [PROFILING.md](PROFILING.md). The `VPROF_*` wrappers from phase 3 now make sense
- [ ] [PERFORMANCE.md Part 3](PERFORMANCE.md#part-3-smoothness): the smoothness steps. Each is a worked case study: a problem measured on the console, the engine code behind it, and the fix
- [ ] [src/MacSrc/Modding.c](../src/MacSrc/Modding.c): loading fan missions
- [ ] Run [tests/filecopy/run.sh](../tests/filecopy/run.sh) and read it with `saveload.c`

## Final check

Answer these without opening the code first, then check your answers in it. If they all come easily, the reading is done.

- [ ] Which variable decides whether the 3D view is drawn again this frame, and who sets it?
- [ ] Where does the pixel data of a wall texture come from, from the `.res` file to the mapper?
- [ ] How does an object know which tiles it is in, and how does a tile know which objects are in it?
- [ ] What happens, in order, when the player opens a door with □?
- [ ] How are the three rendering paths chosen, and how does the game fall back to the CPU when `libshacccg.suprx` is missing?
- [ ] What does a saved game contain?
- [ ] How does a touch on the rear touchpad become a change of MFD?
- [ ] How does the game go from the main menu to the game, and from the game to the automap?
- [ ] Why does the code use `fix` instead of `float`, and where does it still use floats?

## Glossary

A head start; add your own.

| Term | Meaning | Where |
|---|---|---|
| Citadel | The space station the game is set in; the original code's project name (`cit`, `citadel_loops`) | everywhere |
| LG | Looking Glass Technologies, the original developer; the prefix of its shared libraries | `src/Libraries/LG` |
| `errtype` | A `short` error code returned by most functions, rarely checked | `H/error.h` |
| `fix` | 16.16 fixed-point number; `fixang` is an angle on 16 bits | `FIX/Source/fix.h` |
| `Id`, `Ref` | A resource in a `.res` file; an item inside a compound resource (high word `Id`, low word index) | `RES/Source/res.h` |
| `ObjID` | Index of an object in `objs[]` | `Headers/objects.h` |
| `ObjSpec` | The class-specific data of an object | `Headers/objects.h` |
| `ObjRef` | Link between an object and one map tile it touches | `Headers/objects.h` |
| triple | An object's class, subclass and type packed into one number (`MAKETRIP()`) | `Headers/objclass.h` |
| `MapElem` | One tile of the level map | `Headers/map.h` |
| quest bits / quest vars | The story's flags and counters, kept in the player struct | `Headers/player.h` |
| MFD | Multi-Function Display: the two small screens on each side of the view | `newmfd.c`, `mfd*.c` |
| OLH | On-line help: the labels shown over objects | `olh.c`, `olhscan.c` |
| HUD | What is drawn over the 3D view | `hud.c` |
| `frc`, `fauxrend_context` | The 3D view's rendering context | `Headers/frprotox.h`, `frintern.h` |
| tmap | Texture map, a textured polygon | `3D/Source/tmap.c`, `2D/Source/Flat8` |
| `grs_canvas`, `grs_bitmap` | A drawing surface, a bitmap (2D library) | `2D/Source/` |
| `g3s_point` | A point transformed by the 3D library | `3D/Source/3d.h` |
| `LGRegion`, `uiEvent` | A screen rectangle with event handlers; an input event | `UI/Source/region.h`, `event.h` |
| rastq | The rasterizer queue that records drawing calls and replays them on threads or hands them to the GPU | `3D/Source/rastq.c` |
| EDMS | The physics and collision library (C++) | `Libraries/EDMS` |
| loop mode | The game's top-level state (game, setup, cutscene, automap) | `mainloop.c` |
| `KLC` | Initials in comments from the Mac port | everywhere |

## Where each directory is read

| Directory | Phase |
|---|---|
| `src/MacSrc/` | 2, 3, 7 (`VitaGpu.c`), 8 (`Prefs.c`), 10, 11 |
| `src/GameSrc/` | 2 to 10, by subject |
| `src/GameSrc/Headers/` | with the `.c` file of the same name |
| `src/MusicSrc/` | 10 |
| `src/Libraries/H`, `FIX`, `FIXPP`, `LG`, `RES`, `DSTRUCT` | 4 |
| `src/Libraries/EDMS` | 6 |
| `src/Libraries/3D`, `2D` (including `GR`) | 7 |
| `src/Libraries/INPUT`, `UI` | 8 |
| `src/Libraries/AFILE` | 9 |
| `src/Libraries/adlmidi`, `SND` | 10 |
| `src/Libraries/PALETTE`, `RND`, `VOX` | when you meet them; all three are small |
| `tests/` | with the library each one tests |
| `vita/` | 1 |
