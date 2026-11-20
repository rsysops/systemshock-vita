# Smooth shadows

The 3D view's light, shaded smoothly in full colour, in the manner of
System Shock: Enhanced Edition, where the original shades it in 16 bands.
What was found, what was decided, and each step's results, in the order
they were done. Like [PERFORMANCE.md](PERFORMANCE.md), file names and line
numbers are the tree's when a step was written; how to build the profiler
and read its captures is in [PROFILING.md](PROFILING.md).

## Contents

- [Starting point](#starting-point)
- [Decisions](#decisions)
- [The approach](#the-approach)
- [Roadmap](#roadmap)
- [How it is checked](#how-it-is-checked)
- [Step 1: the side pass](#step-1-the-side-pass)

## Starting point

**Light in the game.** A tile has a light level for its floor and one for
its ceiling, 0 (bright) to 15 (dark). The lantern and the level's light
changes take some away, by distance, which gives the level a fraction:
the renderer has it as 8.8 fixed point per vertex (`_fr_do_light_val`,
`src/GameSrc/frterr.c`). The mappers carry it across a polygon, then drop
the fraction: a pixel is `ltab[texel + 256 * floor(level)]`
(`fix_light`, `src/Libraries/2D/Source/tmapint.h`), one of the 16 rows of
the light table `shadtabl.dat`. The GPU does the same by reading its copy
of the table with point sampling. Objects and sprites get one row for the
whole object.

**What the bands come from.** Where the level crosses a whole number the
row changes, and every texel changes colour along the same line: the
bands and their creases, sharp at 960×544. In the Enhanced Edition the
same spots fade smoothly, without the creases where the light changes
from one map square to the next, and its darkest places are dim rather
than black. (Screenshots of the same two spots in both, which the user
provided, were compared for this.)

**8 bits can't do it.** Measured on `shadtabl.dat` and `gamepal.res`:

| | distinct colours a texel goes through, over its rows (mean of the 255) |
|---|---|
| the table's 16 rows | 9.0 |
| 61 rows, three more between each pair, the colour blended and taken back to the nearest of the palette | 10.8 |

Neighbouring rows often give a texel the same palette colour (the teal
0x6C goes 108, 108, 109, 109, 110, 110, 110, 111, 111, ...), and the colours
between two rows mostly fall back on one of them. A finer table would cost
the same work as the rest of this and leave the bands where they are. So
the light has to become colour after the palette: on the GPU, at the last
moment.

**What the table does to brightness.** The median, over the texels, of a
row's brightness against row 0's (`docs/shadows_preview.py` measures it
the same way):

| row | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 | 12 | 13 | 14 | 15 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| ratio | 1.00 | 1.00 | .866 | .802 | .737 | .666 | .601 | .533 | .469 | .400 | .338 | .273 | .204 | .134 | .068 | .008 |

From row 1 on it falls by about a fifteenth a row, to nearly nothing.
Row 15 sends 229 of the 255 colours to index 1, (0, 0, 7): black. Some
texels are the same in every row (the first 24 indices but three, and
others: fullbright colours), and the infrared table `bwtabl.dat` is the
same in all 16 rows.

## Decisions

The user's, on 2026-11-18:

- **Smooth shadows are the default.** The original ones stay, as a
  hidden setting: `shadows = original` in the prefs file
  (`src/MacSrc/Prefs.c`), with no menu. It is read at start-up.
- **Full colour, not dithering.** An ordered dither between two rows
  would stay in the palette, but its pattern shows at 960×544 and doesn't
  look like the Enhanced Edition.
- **The GPU only.** When the game runs on three cores (no
  `libshacccg.suprx`), the shadows are the original ones whatever the
  setting.
- **The 8-bit picture stays as it is.** The canvas, the HUD drawn into it,
  the comparisons with the CPU (`gpudiff`, the frame dumps) and the
  start-up checks don't change.

## The approach

**A side pass.** The GPU draws each scene of a view twice: into the
canvas, as it always has, and again, with the same list, into a side
buffer of the canvas, 32 bits a pixel:

| byte | what |
|---|---|
| red | the palette index the canvas gets |
| green | the texel |
| blue | the light level in sixteenths of a row, 0 to 255 |
| alpha | `SIDE_LIT` (0x5A) where the pixel is a lit texture, else not |

A GXM scene has one colour surface, hence a second scene, on a render
target of its own: a target takes at most 8 scenes a frame
(`SCE_GXM_MAX_SCENES_PER_RENDERTARGET`), and the canvas's keeps the scenes
it had. Only the side
pass's textured shader writes the mark; every other shader is used as it
is and writes 255 in alpha. Objects and sprites, whose colour table is a
row of the light table, come out as lit at the middle of their row.

**The screen.** In place of the view's paletted texture, a shader of its
own draws the view: where the side buffer is marked and its red is the
canvas's index, the colour of the texel at that light, from a 256×16
table of colours filtered between its rows; anywhere else the palette's
colour, as now. What the CPU drew over the view (the HUD, voxel objects,
the screen's effects) no longer matches the index under it, and keeps the
original look by itself; so do translucent surfaces.

**The table of colours.** "Fitted": a texel's colour at row 0 times the
brightness ratio above, the ratios joined between the rows' middles, the
gamma after. Row 15 is replaced by a dim floor, to be tuned on the Vita,
in place of black. Texels the same in every row stay the same. Built
again when the palette changes (gamma, fades, the palette's cycling) or
the light table does (infrared). A "table" way, the colours of two rows
blended, is kept to compare.

## Roadmap

| Step | What | Result |
|---|---|---|
| 1 | The prefs setting; the side buffers and the side pass; a start-up check; counters, and the side buffer in the frame dumps; a preview of the look on a PC from a dump | |
| 2 | The main thread waits for the canvas only, not for the side pass too | |
| 3 | The table of colours, the screen's shader, its start-up check, a PC test of the table: smooth shadows on the screen | |
| 4 | Where the view is copied to the screen buffer: paused, the options panel and its gamma slider, the zoom rectangle | |
| 5 | Tuning: the dark floor and the curve, a light fraction for objects, translucent surfaces over lit ones | |

## How it is checked

GPU code doesn't run on the PC: each step is built there (`./build.sh`,
`./build.sh profile`), tried on the Vita, and read from what the Vita
writes.

- `gpu.txt`: the side pass's shaders, its buffers, `side check drawn: a b
  c d wrong` (all 0, or the shadows are the original ones), and
  `side=on|off` in the report line.
- `profile.txt`: `side=a/b`, the scenes drawn again a frame and the time
  to send them; `sidecheck=a/b/c` with the profile build's Checks on (see
  PROFILING.md).
- The frame dumps, with Checks on, hold the side buffer and the
  light table. `docs/shadows_preview.py` turns one into pictures of the
  smooth look, before the Vita shows it.
- Timing: the profile build's Variants on Alternate turns the side pass
  off in variant 0. Comparing the variants' `gpuwait` gives its
  cost to the GPU.

## Step 1: the side pass

What it does: the side buffers, the side pass and the checks, without
anything on the screen yet. The game looks as before.

- `src/MacSrc/Prefs.c`: `shadows = smooth|original`, smooth by default.
- `src/MacSrc/VitaGpu.c`:
  - the side pass's shaders, `side_fragment_source`, built by
    `make_side_programs` after the translucent bitmaps' (they share the
    textured shaders' vertex program);
  - three side buffers next to the canvases (`side_make`, 6.3 MB at
    960×544);
  - each scene into a canvas drawn again by `side_scene`, on a render
    target of its own (`target_for`, with its depth buffer 2 MB more), the
    frame's first one clearing the side buffer (`side_clear`);
  - `side_check` at start-up;
  - the mark and the index counted against the GPU's picture in each
    comparison, and in each dump `NN-side.raw` and `NN-ltab.raw`.
- `src/GameSrc/frsetup.c`: no side pass in cyberspace.
- `src/GameSrc/vprof.c`: `side=` and `sidecheck=`.
- The profiler's variant, which compared the worker threads' priorities
  in step S13b of PERFORMANCE.md, now turns the side pass off and on; the
  workers keep the priority S13b chose.

**First try (2026-11-19): the GPU stayed off.** The side scenes were
first drawn on the canvas's render target, with its `scenesPerFrame`
raised from 8 to 16 for them. GXM takes 8 at most, and refuses the target
outright (`render target 256x32 failed: 0x805b0003`,
`SCE_GXM_ERROR_INVALID_VALUE`, from the first start-up check). With no
target the GPU turned itself off, and the game ran on three cores: 28 to
48 fps in the station, `gpuscenes=0`, no frame dumps. The side scenes now
have a target of their own, with the canvas's back at 8. A side pass that
can't get one leaves the frame's shadows original and the GPU at work.

What a capture should show:

| where | expected |
|---|---|
| `gpu.txt` | the side shaders compiled, the side buffers made, `side check drawn: 0 0 0 0 wrong`, `side=on` |
| `profile.txt` | `side=` scenes as many as `gpuscenes` in the station; 0 in cyberspace, and with `shadows = original` |
| Checks on | `sidecheck=` with no mismatch, and lit pixels most of the view's |
| `gpudiff=` | as before |
| `gpuwait`, variant 1 against 0 | the side pass's cost to the GPU: a scene's, about 3 ms |
| the game | as before |
