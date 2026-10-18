# Shockolate (System Shock) port for PS Vita

## Install

Data files from System Shock are required. This port was only tested with `System Shock - Classic Edition` from GoG, so I have no idea if Mac data from Enhanced Edition would work properly with it.

To install the data files, you'll have to create the `ux0:data/systemshock/res/` folder on your PS Vita and copy `DATA` and `SOUND` folders from the installed System Shock folder there.

The game checks for these files when it starts: if some are missing, it says which ones in a message and goes back to the LiveArea.

The game draws its 3D view with the GPU, which needs the shader compiler module `libshacccg.suprx` in `ur0:data/` (the same file many Vita ports ask for). Without it the game draws on the CPU, which is slower, and says so in a message when it starts.

## Building

### With Docker (nothing else to install)
```
./build.sh
```
This builds `build/systemshock.vpk` inside the `vitasdk/vitasdk` image (`vita/Dockerfile`). `./build.sh clean` starts the build folder afresh.

`./build.sh profile` and `./build.sh profile checks` build the profile flavours, each an app of its own, into `build-profile/` and `build-profile-checks/`: see [docs/PROFILING.md](docs/PROFILING.md).

### With a local VitaSDK
The SDK with the libraries the game links: SDL2, SDL2_mixer, vita2d, libjpeg, libpng, libwebp, zlib, libvorbis/libogg, libmikmod, libmodplug, libxmp-lite, opus/opusfile, FLAC, mpg123 and taihen.
```
mkdir build && cd build
cmake .. -DCMAKE_TOOLCHAIN_FILE=$VITASDK/share/vita.toolchain.cmake -DCMAKE_BUILD_TYPE=None
make
```

## Port info

### Controls

- Left analog stick - Movement
- Right analog stick - Aiming/Cursor movement
- × - Jump
- ○ - Cycle between Stand/Crouch/Prone
- □ - Quick action/item pickup (instantly picks up the item, opens the door, etc)
- △ - Next weapon
- D-Pad Up - Grab/Arm _selected_ grenade
- D-Pad Down - Toggle between free look and mouse movement
- D-Pad Left/Right - Lean left/right
- L1 - LMB (Use)
- R1 - RMB (Attack)
- START - Esc
- SELECT - Use _selected_ drug
- Rear touchpad - Next/previous MFD (you can switch them by swiping up or down on the left/right side of the touchpad)
- Front touchpad - Mouse emulation

Gyro aiming is active by default. You can turn it off or adjust analog/gyro look speed on the `Input` page of the game menu.

Once you have the data reader, new e-mails and audio logs play as soon as they arrive. Set `Log autoplay` to Off on the `Options` page of the game menu for the original behaviour, where they wait until you tap the sender's face in the MFD (or pick them in the log list).

When you save, the keyboard opens on a name such as `Save 3 (Medical)`: the next number and the floor you're on. Press Enter to keep it, or edit it. Set `Save Templ.` to Off on the `Options` page to have it open on the slot's own name instead. Closing the keyboard without a change leaves the slot's name as it was.

The cursor is hidden in menus by default (they're navigated with the D-pad; touch still works). Enable `Menu Cursor` on the `Input` page to show it there again.

In the main menu and the in-game menu, move with the D-Pad or left stick, select with ×, go back with ○ and switch the in-game menu page with L1/R1. In the in-game menu, △ shows the help / controls screen and □ toggles the music. START starts the game from the New Game screen and closes the in-game menu. See [docs/KEYMAPS.md](docs/KEYMAPS.md) for details.

## Additional info

### Tip for the new players

System Shock is an old-school game. It can be hard, confusing and even obtuse. It does not hold your hand and invites you to explore and improvise. It is advisable to start with normal (or if prefered lowered) difficulties settings. Take things slow, read, listen to audiologs, pay attention, make notes if needed. Pay attention to security levels on each deck, lower the better and also pay attention to your current objective. If needed, refer to manual (included for example with GOG release or can be found on internet) or ask other players.

### Known issues

Cinematics can take some time to load.

### Special thanks

- Taffer from Discord for the inspiration, control ideas, testing and help with the README.

## Credits

This port is built on [Shockolate](https://github.com/Interrupt/systemshock), the cross-platform source port of System Shock, which is based on the Macintosh source code released by Night Dive Studios. This repository only builds for the PS Vita: for Windows, Linux or macOS, use Shockolate itself.
