# Shockolate (System Shock) port for PS Vita

## Install

Data files from System Shock are required. This port was only tested with `System Shock - Classic Edition` from GoG, so I have no idea if Mac data from Enhanced Edition would work properly with it.

To install the data files, you'll have to create the `ux0:data/systemshock/res/` folder on your PS Vita and copy `DATA` and `SOUND` folders from the installed System Shock folder there.

The GPU renderer, which is the default, needs the shader compiler module `libshacccg.suprx` in `ur0:data/` (the same file many Vita ports ask for). Without it the game runs on its CPU renderers.

## Building

### Prerequisites
- VitaSDK
- SDL2
- SDL2-mixer

### Build
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

Gyro aiming is active by default. You can turn it off or adjust analog/gyro look speed by selecting `Vita Options` in the game menu.

The cursor is hidden in menus by default (they're navigated with the D-pad; touch still works). Enable `Cursor` in `Vita Options` to show it there again.

`Renderer` in `Vita Options` chooses what draws the 3D view: `GPU` (the default; offered when `libshacccg.suprx` is installed), `3 cores` (the software renderer on three CPU cores) or `1 core` (the original software renderer).

In the main menu and the in-game menu, move with the D-Pad or left stick, select with ×, go back with ○ and switch the in-game menu page with L1/R1. In the in-game menu, △ shows the help / controls screen and □ toggles the music. START starts the game from the New Game screen and closes the in-game menu. See [docs/KEYMAPS.md](docs/KEYMAPS.md) for details.

## Additional info

### Tip for the new players

System Shock is an old-school game. It can be hard, confusing and even obtuse. It does not hold your hand and invites you to explore and improvise. It is advisable to start with normal (or if prefered lowered) difficulties settings. Take things slow, read, listen to audiologs, pay attention, make notes if needed. Pay attention to security levels on each deck, lower the better and also pay attention to your current objective. If needed, refer to manual (included for example with GOG release or can be found on internet) or ask other players.

### Known issues

Audiologs can freeze the game for a few seconds before starting the playback. Cinematics can take some time to load too.

### Special thanks

- Taffer from Discord for the inspiration, control ideas, testing and help with the README.

## Credits

This port is built on [Shockolate](https://github.com/Interrupt/systemshock), the cross-platform source port of System Shock, which is based on the Macintosh source code released by Night Dive Studios. This repository only builds for the PS Vita: for Windows, Linux or macOS, use Shockolate itself.
