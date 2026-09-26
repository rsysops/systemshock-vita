# Keymaps: PS Vita and Vita3K

The Vita build only reads the Vita's controller, touch panels and gyro — it
never reads a host keyboard directly. When running in Vita3K, your keyboard
drives Vita3K's *emulated* Vita pad, which the game then sees exactly as it
would real hardware. The Vita3K keys below are the emulator's defaults; they
can be rebound in Vita3K under **Controls**.

Button/stick handling lives in `HandleControllerButtonEvent()`,
`HandleControllerAxisEvent()`, `HandleTouchEvent()` and
`ProcessControllerAxisMotion()` in `src/Libraries/INPUT/Source/sdl_events.c`.
Each input is translated into the original game's keyboard hotkey or mouse
event, shown in the last column.

## Gameplay

| Action | Vita input | Vita3K default key | Emulated as (game hotkey/mouse) |
|---|---|---|---|
| Move forward / back | Left stick up / down | `W` / `S` | `w` / `s` |
| Run (forward only) | Left stick fully up | `W` | `W` (shift+w) |
| Strafe left / right | Left stick left / right | `A` / `D` | `a` / `d` |
| Aim / move cursor | Right stick | `I` / `J` / `K` / `L` | Mouse motion |
| Jump | × | `X` | `Space` |
| Cycle stance Stand → Crouch → Prone | ○ | `C` | `t` / `g` / `b` |
| Quick use / item pickup | □ | `Z` | Shift + LMB |
| Next weapon | △ | `V` | `Tab` |
| Grab / arm selected grenade | D-pad ↑ | `↑` | `u` |
| Toggle free look / cursor mode | D-pad ↓ | `↓` | `f` |
| Lean left / right | D-pad ← / → | `←` / `→` | `q` / `e` |
| Use | L | `Q` | LMB |
| Attack | R | `E` | RMB |
| Menu | START | `Enter` | `Esc` |
| Use selected drug | SELECT | `Right Shift` | `o` |
| Cursor + click | Front touch | Mouse click / drag (default touch mode) | Mouse motion + LMB |
| Previous / next page on left or right MFD | Rear touch: swipe up / down on the left or right half | Press `T` to switch to rear touch, then drag vertically | — |
| Aiming (on by default) | Gyro | Not available from the keyboard (needs a host controller with motion sensors) | Mouse-look delta |

Gyro aiming and analog/gyro look speed can be changed from the **Vita input**
option in the game menu.

Tuning values, all in `sdl_events.c`:

- Left stick deadzone: `CONTROLLER_L_DEADZONE = 15000`; forward run threshold:
  `CONTROLLER_L_RUN_DEADZONE = 32500` (of 32767).
- Right stick deadzone: `CONTROLLER_R_DEADZONE = 4000`.
- Rear touch swipe must cover more than `MFD_SWIPE_SIZE = 0.3` of the panel
  height.

## Unused inputs

These Vita3K bindings exist but do nothing in this port:

- `U` / `O` / `F` / `H` — L2 / R2 / L3 / R3 (not present on Vita hardware).
- `P` — PS button (handled by the system/emulator, not the game).
- `F11` — Vita3K fullscreen toggle (emulator only).

## Actions not reachable with the Vita controls

The Vita input code only ever emits plain keys — never Shift, Ctrl or Alt —
and only these: `w` `W` `s` `a` `d` `Space` `t` `g` `b` `Tab` `Esc` `o` `u`
`f` `q` `e`, plus LMB, RMB, Shift+LMB and mouse motion. Any game action bound
to another key or to a modifier combination therefore has no button. This
applies to Vita3K too, since keyboard presses there become Vita buttons.

Default bindings come from `HotKeyLookup[]` and `MoveKeybindsDefault[]` in
`src/MacSrc/Prefs.c`.

| Action | PC key | On-screen alternative on Vita |
|---|---|---|
| Show help overlay | `Alt+O` | None — only shown automatically at new game start, if on-line help is on |
| Toggle on-line help | `Ctrl+H` | Options menu (START) → on-line help setting |
| Save / load game | `Ctrl+S` / `Ctrl+L` | Options menu (START) |
| Quit | `Ctrl+Q` | Options menu (START) |
| Toggle music | `Ctrl+M` | Options menu (START) → Audio → music volume |
| Cycle detail level | `Ctrl+1` | Options menu (START) → detail setting |
| Pause | `P` | None (the options menu pauses the game while open) |
| Previous weapon | `Shift+Tab` | Tap the weapon in the inventory |
| Reload weapon (normal / swap ammo) | `Alt+Backspace` / `Ctrl+Backspace` | Tap the ammo buttons on the weapon MFD |
| Select grenade / select drug | `Ctrl+'` / `Ctrl+;` | Tap it in the inventory |
| Full / normal / map view | `Ctrl+F` / `Ctrl+D` / `Ctrl+A` | — |
| Clear fullscreen overlays | `Backspace` | — |
| Cycle HUD colour | `Alt+H` | — |
| Cancel audiolog | `Ctrl+.` | — |
| Hardware: bio scan, fullscreen, 360 view, lantern, shield, infrared, nav unit, data reader, booster, jumpjets | `1`–`9`, `0` (in that order) | Tap the side icon |
| MFD buttons | `F1`–`F10` | Tap the MFD button |
| Keypad digits | `0`–`9` | Tap the on-screen keypad |
| Lean up | `X` | — |
| Look up / down | `R` / `V` | Right stick / gyro |
| Turn / fast turn | `Z` / `C`, `←` / `→` (+ Shift) | Right stick / gyro |
| Cyberspace dive | `X` | — (climb, thrust, bank and roll still work via left stick and D-pad) |
| Cheats: give all / physics / level up / level down | `Ctrl+2` … `Ctrl+5` | — |
| Toggle OpenGL | `Ctrl+G` | Not applicable (Vita build has OpenGL off) |

### Rebinding via keybinds.txt

Hotkeys can be rebound in `ux0:data/systemshock/keybinds.txt`, which is
created with the defaults on first launch. An unreachable action can be moved
onto one of the plain keys above, but every one of them is already used, so
another action has to give up its key. Any action missing from the file gets
its default key back, so swap keys rather than deleting lines. For example, to
put the help overlay on SELECT:

```
bind  o                       "showhelp"
bind  alt+semicolon           "use_drug"
```

"Use selected drug" is then no longer reachable from the pad. Keeping both
needs a code change (e.g. a SELECT+START combination emitting `Alt+O` in
`HandleControllerButtonEvent()`).

## Menus and text entry

- On the main menu / setup screens (`_current_loop == SETUP_LOOP`), the
  D-pad, left stick and face buttons do not emit text, so they can't leak into
  the character-name field. START (`Esc`) and △ (`Tab`, cycles difficulty
  categories) still work there.
- In the in-game options menu, **Return** goes back one screen (e.g. Audio
  options → Audio → main); START closes the whole menu from any screen.
- The Vita on-screen keyboard opens when entering the New Game screen, when
  tapping the name field on that screen, and when selecting a save slot.

## Keyboard trap in Vita3K

Pressing a letter in Vita3K sends the *Vita input* bound to that key, not the
letter itself. For example, Vita3K `Q` is L1 → **Use**, not lean left, and
Vita3K `F` is L3 → nothing, not free-look toggle. Use the Vita3K column above,
not the PC Shockolate hotkeys.
