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
- [Step 2: one pass](#step-2-one-pass)
- [Step 2b: what the full-screen work costs](#step-2b-what-the-full-screen-work-costs)
- [Step 2c: a lighter side shader](#step-2c-a-lighter-side-shader)
- [Step 3: on the screen](#step-3-on-the-screen)
- [Step 3b: the table of colours, only what changed](#step-3b-the-table-of-colours-only-what-changed)
- [Step 4: the view through the screen buffer](#step-4-the-view-through-the-screen-buffer)

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

**One pass, into 32 bits.** The GPU draws each scene of a view into a
side buffer of the canvas, 32 bits a pixel, in place of the canvas itself:

| byte | what |
|---|---|
| red | the palette index the canvas gets |
| green | the texel |
| blue | the light level in sixteenths of a row, 0 to 255 |
| alpha | `SIDE_LIT` (0x5A) where the pixel is a lit texture, else not |

Only the smooth scenes' textured shader writes the mark; every other
shader is used as it is and writes 255 in alpha, and the translucent ones
read the red as the index under them. Objects and sprites, whose colour
table is a row of the light table, come out as lit at the middle of their
row.

Two full-screen passes keep the 8-bit canvas exactly as the CPU, the HUD,
the checks and the screen expect it:

- **the import**, a scene's first draw, brings the canvas into the
  buffer. The CPU writes to the canvas before a view's first GPU scene (a
  clear it was left) and between scenes (a call it draws itself). In the
  frame's first scene every pixel becomes the canvas's, unmarked; in a later
  one, only those where the canvas isn't the buffer's red, which are the
  CPU's, and the rest keeps its mark.
- **the extract**, a scene of its own on the canvas's render target,
  copies the buffer's red into the canvas after each scene.

The smooth scenes are on a render target of their own: a target takes at
most 8 scenes a frame (`SCE_GXM_MAX_SCENES_PER_RENDERTARGET`), and each
keeps one per scene of the view. Step 1 drew the scene twice instead, into
the canvas and into the buffer, and that cost as much as the view itself.

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
the light table does (infrared). A "table" way, the light table's own
colours blended between two rows, was tried in step 3 and dropped in 3b:
the user found the fitted way better.

## Roadmap

| Step | What | Result |
|---|---|---|
| 1 | The prefs setting; the side buffers and the side pass; a start-up check; counters, and the side buffer in the frame dumps; a preview of the look on a PC from a dump | correct (0 mismatches in 84.8 M pixels), the smooth look in the previews; the GPU's work doubled: `gpuwait` 8.6 ms against 0.3-3.9, 53 s of 233 under 50 fps |
| 2 | One pass: each scene drawn once, into the 32-bit buffer, with an import and an extract (replaces "the main thread waits for the canvas only") | correct (0 mismatches in 83.2 M pixels); `gpuwait` +4 ms against the 8-bit way at the same spot, whatever the scene: half of step 1's worst, still a frame rate of 52 in heavy places. 2b times its parts at start-up: the side shader is most of it; 2c makes it lighter: **+1.4 ms**, at a frame rate 3 to 4 lower where the frame is already short of time |
| 3 | The table of colours, the screen's shader, its start-up check, a PC test of the table: smooth shadows on the screen | on the screen and right (0 mismatches in 172 M pixels), dark rooms a dim teal as in the Enhanced Edition; the table made again every frame, 1.0 ms. 3b: made again only where the palette changed, 0.04 ms; `table` dropped |
| 4 | Where the view is copied to the screen buffer: paused, the options panel and its gamma slider, the zoom rectangle | smooth there too, the panel, the rectangle and the overlays as before; nothing smooth on the menus or in a cutscene |
| 5 | Tuning: the dark floor and the curve, a light fraction for objects, translucent surfaces over lit ones | |

## How it is checked

GPU code doesn't run on the PC: each step is built there (`./build.sh`,
`./build.sh profile`), tried on the Vita, and read from what the Vita
writes.

- `gpu.txt`: the smooth shadows' shaders, the side buffers, `side check
  drawn: a b c d wrong` (all 0, or the shadows are the original ones), and
  `side=on|off` in the report line.
- `profile.txt`: `side=a/b`, the smooth scenes a frame and the time to
  send their extracts; `sidecheck=a/b/c` with the profile build's Checks on
  (see PROFILING.md).
- The frame dumps, with Checks on, hold the side buffer and the
  light table. `docs/shadows_preview.py` turns one into pictures of the
  smooth look, before the Vita shows it.
- Timing: the profile build's Variants on Alternate draws the 8-bit way in
  variant 0. Comparing the variants' `gpuwait` gives the smooth way's
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

**Results (2026-11-19, the old checks build, 233 s in the station).**

- Every start-up check passed; `side check drawn: 0 0 0 0 wrong`, `side=on`.
- `sidecheck=` 0 mismatches in 84.8 M pixels; the side buffer marked
  99.99% of each dump's view lit.
- `shadows_preview.py` on the five dumps: the bands gone on walls, floors
  and objects in the fitted way; the table way smooth too, with its hue
  shifts.
- `gpudiff=` 10%, against 4.6 and 6.3% in G11's and G12's captures: the
  dumps show the usual difference, the texels the GPU and the mappers pick
  on floors and ceilings seen at a grazing angle, which this route had
  more of. The side pass draws into its own buffer and can't change the
  picture.
- **The cost**: the side pass costs the GPU as much as the view.

  | | `gpuwait`, median | worst | seconds under 50 fps |
  |---|---|---|---|
  | earlier captures (G11, G12, S12, S13b) | 0.3 to 3.9 ms | 5 to 6 ms | 1 to 5 |
  | step 1 | 8.6 ms | 19 ms | 53 of 233 |

  In heavy places the GPU's own work outgrows a 16.7 ms frame, which no
  waiting less for it can mend. Hence step 2.

## Step 2: one pass

What it does: each smooth scene drawn once, into the 32-bit buffer, as
[the approach](#the-approach) has it. Still nothing new on the screen.

- `src/MacSrc/VitaGpu.c`:
  - `import_clear_fragment_source`, `import_keep_fragment_source` and
    `extract_fragment_source`, built by `make_single_pass_programs` after
    the side shaders, with the textured vertex program;
  - `smooth_set`: per canvas, the 32-bit buffer as a target and as a
    texture, and the canvas as a texture (`smooth_set_init`);
  - `vgpu_begin` begins a smooth scene on the buffer, the import its first
    draw (`full_quad`); `vgpu_end` follows it with `extract_scene`, its
    quad in room kept past the scenes' vertices;
  - `side_check` redone: two scenes with a CPU write between them, the
    canvas and the buffer checked band by band.
- The second scene, `side_scene`, and the side clear are gone.

What a capture should show:

| where | expected |
|---|---|
| `gpu.txt` | the import and extract shaders compiled, `side check drawn: 0 0 0 0 wrong`, `side=on` |
| Checks on | `sidecheck=` with no mismatch, and lit pixels most of the view's; `gpudiff=` as before for the places played |
| `gpuwait`, Variants on Alternate | variant 1 within 1 to 2 ms of variant 0, and no second under 50 fps that variant 0 doesn't have |
| the game | as before |

**Results (2026-11-20, the profile build: 159 s with Checks on, then 202 s
with Variants on Alternate, in the station, on levels 0 to 2).**

- Every start-up check passed, the new side check too (`side check
  drawn: 0 0 0 0 wrong`): the clear and keep imports, the extract, and a
  CPU write between two scenes.
- `sidecheck=` 0 mismatches in 83.2 M pixels; four dumps, one of the
  paneled view (804×293), 0 mismatches each. `gpudiff=` 9.2%, as in
  step 1's route.
- `side=1.00` a frame in the station, the extract sent in 0.10 ms.
- **The cost**, variants 0 (the 8-bit way) and 1 (smooth) in turn, 20 s
  each:

  | window | spot | `gpuwait`, 8-bit | smooth | fps, 8-bit | smooth |
  |---|---|---|---|---|---|
  | t=358-418 | level 1, 17.55,23.28, still | 3.62 | 7.57 / 7.62 | 60.0 | 52.0 / 51.9 |
  | t=539-620 | level 1, the start | 3.18 / 3.64 | 7.57 / 7.57 | 60.0 | 59.5 / 60.0 |
  | all 102 / 100 s | | 3.63 median, 6.0 worst | 7.57 median, 11.9 worst | 1 s under 50 | 6 s under 50 |

  About +4 ms wherever the player stands, a heavy view or a light one:
  the cost is in the full-screen work, not in the polygons. Each scene
  now loads and stores a 32-bit buffer of 2 MB, laid out linearly, the
  import reads the canvas as a texture, the extract reads the 2 MB
  buffer as a texture and is a scene of its own. Which of these the
  4 ms are is not known yet: GXM can't skip loading a colour surface,
  but has tiled surfaces and textures, usually faster than linear ones.

## Step 2b: what the full-screen work costs

What it does: in the profile build only, once the side buffers are made,
`smooth_timing` (`src/MacSrc/VitaGpu.c`) times each part of a smooth
frame's full-screen work on canvas 0 and its side buffer, before the game
draws anything, and writes one line to `gpu.txt`:
`smooth timing us min/median: ...`. A case is one scene, begun, drawn,
ended and waited for, ten times over; the minimum and the median, in µs.
The game itself doesn't change.

| case | target | drawn | tells |
|---|---|---|---|
| `empty8` | the 8-bit canvas | one pixel | a scene's fixed cost, loading and storing 0.5 MB |
| `empty32` | the 32-bit side buffer, linear | one pixel | the same with 2 MB |
| `empty32t` | a 32-bit buffer, tiled | one pixel | the same, tiled |
| `clear`, `keep` | the side buffer | the clear or the keep import | the imports |
| `extract` | the canvas | the extract, from the side buffer | the extract scene |
| `extractt` | the canvas | the extract, from the tiled buffer | the same, tiled |
| `tmap8` | the canvas | a full-screen textured polygon, the 8-bit shader | a pixel's work, 8 bits |
| `side32`, `side32t` | the side buffer, the tiled one | the same, the side shader | a pixel's work, 32 bits |

Read as differences: `empty32 - empty8` is what the 32-bit buffer's load
and store add; `clear` or `keep - empty32` the import's; `extract -
empty8` the extract's; `side32 - tmap8` what a pixel costs more; the tiled
cases against the linear ones. Their sum, for the parts a smooth frame
has, should come near step 2's +4 ms.

What a capture should show: start "System Shock Profile", wait for the
main menu, quit; `gpu.txt` has the line, every case a number (or `-1/-1`
with the error logged if a tiled surface or texture is refused).

**Results (2026-11-20, one start-up).** In µs, minimum / median of ten:

```
empty8=802/827 empty32=798/807 empty32t=802/811 clear=1443/1459 keep=2988/3016
extract=1444/1456 extractt=1452/1465 tmap8=2745/2764 side32=5145/5179 side32t=5157/5587
```

| part | cost |
|---|---|
| a scene, begun, ended and waited for, whatever its target | 0.80 ms, the same for the 8-bit canvas, the 32-bit buffer and a tiled one |
| loading and storing 2 MB in place of 0.5 MB | nothing measurable |
| tiled in place of linear | nothing, for the buffer or for the extract's reading |
| the clear import | 0.65 ms |
| the keep import (reads the target) | 2.19 ms, in a frame's later scenes only, which are rare |
| the extract | 0.64 ms, and a scene of its own |
| a screen of textured pixels, the 8-bit shader | 1.94 ms |
| the same, the side shader | 4.35 ms: **2.24 times**, +2.4 ms a screen |

A smooth frame pays the clear import (0.65), the extract (0.64 and a
scene) and its pixels' extra, a screen or a little more of them (+2.4 or
more): about the +4 ms step 2 measured. **The side shader is most of
it.** It does the textured shader's work and then a floor, a clamp, a
compare for the mark and four outputs (652 bytes compiled against 452).
The buffer's size and layout cost nothing. Most of the rest is the clear
import, which a view whose first draw is a clear of the whole canvas
doesn't need.

## Step 2c: a lighter side shader

What it does, from 2b's findings (`src/MacSrc/VitaGpu.c`):

- **The side shader does no more than the textured one, but for its four
  outputs.** It used to work out the level's sixteenths with a `floor` and
  a `clamp`, and the mark with a compare on the level. Now the level goes
  out as it is, `tab * 16 / 255`, rounded by the output, and the mark is
  a constant: the shader is only given lit polygons, whose level is
  always one of the light table's rows.
- **Lit and unlit textured polygons are told apart** when they are handed
  over (`vgpu_tmap`, `DRAW_LIT` and `DRAW_LIT_TRANS`): lit when the first
  vertex's level is under 16. A polygon of one row of another table (the
  row that leaves texels as they are, the colour tables) keeps the
  textured shader in a smooth scene too, and writes 255 in alpha:
  unmarked, as the compare made it.
- **No clear import when the view clears itself.** A view's first draw is
  usually its clear, a flat polygon over all of it (`gpu_scene_begin` in
  `rastq.c`). `vgpu_flat` sees it come right after the clear import, and
  takes the import back: everything it would have brought in is drawn
  over. What the view doesn't cover isn't shown. With Checks on, the CPU
  does the clear and the import stays.
- **The start-up check**: its first scene begins with a full clear, as a
  view does; the lit bands' level is `(x + 0.25) / 16`, which gives `x`
  sixteenths whether the output rounds or truncates.
- **`smooth_timing`**: `side32` times the lighter shader; `tmap32`, the
  textured shader into the 32-bit buffer, tells the shader's cost from
  the 32-bit write's; `flat32`, a full clear into it, is what a frame's
  start now costs in place of `clear`.

What a capture should show:

| where | expected |
|---|---|
| `gpu.txt` | every check 0 wrong, `side=on`; `smooth timing`: `side32` near `tmap32`, `flat32` well under `clear` |
| Checks on | `sidecheck=` with no mismatch; dumps whose preview is right |
| `gpuwait`, Variants on Alternate | variant 1 within 1 to 2 ms of variant 0 at the same spots |

**Results (2026-11-20, the profile build: start-up, 179 s with Checks on,
then 212 s with Variants on Alternate, levels 1 and 2).**

- Every start-up check passed, the redone side check too. `sidecheck=` 0
  mismatches in 91.2 M pixels; five dumps, one of the paneled view, 0
  mismatches each. `gpudiff=` 9.3%, as on the earlier routes.
- **The start-up timing**, in µs, minimum / median of ten:

  ```
  empty8=799/815 empty32=804/809 empty32t=802/817 clear=1447/1455 flat32=1215/1230
  keep=3001/3009 extract=1452/1463 extractt=1448/1462 tmap8=2749/2767 tmap32=2755/2776
  side32=3188/3210 side32t=3189/3221
  ```

  | part | 2b | 2c |
  |---|---|---|
  | a screen of lit pixels, the side shader | 4.35 ms | **2.39 ms** |
  | the same with the textured shader, into 8 bits / into 32 | 1.94 | 1.95 / 1.95 |
  | what the side shader adds | +2.40 | **+0.44** |
  | a frame's start: the clear import / the view's clear in its place | 0.65 | 0.41, the clear the 8-bit way draws as well |

  Writing 32 bits costs no more than 8 (`tmap32` against `tmap8`); what
  the side shader still adds is its other outputs. The extract is
  unchanged: 0.65 ms and a scene.
- **In the game**, the two ways in turn, 20 s each:

  | window | spot | `gpuwait`, 8-bit | smooth | fps, 8-bit | smooth |
  |---|---|---|---|---|---|
  | t=296-331 | level 1, the start | 0.38 | 1.79 | 60.0 | 60.0 |
  | t=371-412 | level 1, 17.77,23.29, still | 3.59 / 3.61 | 4.94 | 53.8 | 50.1 |
  | t=526-574 | level 1, 16.41,30.41, still | 3.62 | 5.00 / 5.02 | 46.2 | 43.3 / 43.2 |
  | all 99 / 113 s | | 3.59 median, 5.4 worst | 4.94 median, 6.7 worst | | |

  **+1.35 to 1.4 ms** wherever the player stands, against step 2's +4.
  At these heavy spots the frame is already over its 16.7 ms the 8-bit
  way (46 to 54 fps), and the smooth way takes 3 to 4 fps more; where the
  8-bit way holds 60 fps, the smooth way does too. What is left: the
  extract (0.65 ms and a scene) and the side shader's other outputs.

## Step 3: on the screen

What it does: the smooth shadows on the screen.

- **The table of colours**, `src/MacSrc/ShadeLut.c` (plain C, tested on a
  PC by `tests/shadelut/run.sh [DATA folder]`):
  - `shade_lut_ratios`, once per light table: each row's brightness
    against row 0's, the median over the texels that change; the last
    row carried on from the two before it; none under
    `SHADE_DARK_FLOOR` (0.07, dim rather than black, as the Enhanced
    Edition) and none brighter than the row above. On the game's table:
    1.00 1.00 .861 .798 .733 .662 .595 .528 .464 .394 .332 .266 .200
    .131 .070 .070. A texel the same index in every row (fullbright
    colours, the whole infrared table) keeps its colour.
  - `shade_lut_fill`, at each change of the palette: 16 rows of 256
    colours, row r the colour at level r + 0.5. **Fitted** (the default):
    the texel's colour at row 0 times row r's ratio, then the gamma.
    **Table** (`shadows = table`, to compare, dropped in 3b): the light
    table's own colours. Index 255 black, as the game shows it.
- **The screen's shader** (`display_fragment_source`, `vgpu_draw_view`
  in `src/MacSrc/VitaGpu.c`), called by `SDLDraw` (`src/MacSrc/Shock.c`)
  for the full-screen view and the paneled view's window, inside vita2d's
  drawing. Where the side buffer marks a pixel lit and holds the canvas's
  index, the table read at the texel's column and the level's place
  between two rows; elsewhere the view's P8 texture, its palette's colour.
  A view without smooth shadows (cyberspace, a frame that went the 8-bit
  way, `shadows = original`) is drawn by vita2d as before.
- **The tables**: three in turn in GPU memory, one made while the screen
  may still be drawing from the others. `SetSDLPalette` gives
  `vgpu_set_palette` the palette before its gamma and the gamma row; the
  light table is the one a smooth view's scene was given.
- **A start-up check** of the screen's shader (`display_check`): a lit
  band against the table read between rows as the CPU works it out, a
  marked band with another index and an unmarked one against the
  palette. If it fails, the shadows stay original.
- `shown=` and `lut=` in the log; a `show` case in the start-up timing,
  the screen's shader over the whole canvas.
- Not yet: the view while the game is paused (the options panel, its
  gamma slider) and under the zoom rectangle is copied into the screen
  buffer and shown with its palette (step 4 does them).

What a capture should show:

| where | expected |
|---|---|
| the screen | walls, floors and objects shaded smoothly, the darkest dim; the HUD, the paneled view's frame, translucent surfaces as before; cyberspace and the options panel as before |
| `gpu.txt` | every check 0 wrong, `display check drawn: 0 0 0 wrong`, `side=on`; `show` in the timing line |
| Checks on | `sidecheck=` with no mismatch; `shown=1.00` in the station |
| `lut=` | about 0 a frame but at fades and in places whose palette cycles |
| `gpuwait`, Variants on Alternate | variant 1 within about 1.5 ms of variant 0, as in 2c |

**Results (2026-11-20, the profile build: one session in the smooth way,
366 s with Checks on, then 411 s with Variants on Alternate, levels 1 and
2; one short session with `shadows = table`).**

- Every start-up check passed, `display check drawn: 0 0 0 0 wrong` in
  both sessions; `sidecheck=` 0 mismatches in 172 M pixels; five dumps,
  0 mismatches each; `gpudiff=` 8.5%.
- **On the screen** (the user's screenshots, at the start of level 1,
  lights on and off): the smooth shadows show, the HUD and the help
  labels as before. With the lights off, the dark room on the right is a
  dim teal in the fitted way, close to the Enhanced Edition's, and darker
  in the table way.
- The start-up timing: `show=1286/1320` µs, the screen's shader over the
  whole canvas, 0.48 ms over a scene's fixed cost.
- `shown=1.00` a frame in the station.
- **`lut=1.00/1.0`: the table of colours is made again every frame, at
  1.0 ms of the main thread's time**, 0.9 in the table way. The game
  cycles part of its palette every frame, and each change made the whole
  table again.
- **The cost**, the two ways in turn, 20 s each:

  | window | spot | `gpuwait`, 8-bit | smooth | fps, 8-bit | smooth |
  |---|---|---|---|---|---|
  | t=580-640 | level 1, the start | 0.36 | 1.75 | 60.0 | 60.0 |
  | t=680-741 | level 1, 17.46,23.26, still | 3.72 / 3.69 | 5.40 / 5.09 | 53.6 / 53.7 | 47.4 / 47.4 |
  | t=903-943 | level 2, 25.68,29.58, still | 4.71 / 4.72 | 6.35 | 60.0 | 60.0 |

  The GPU's share is 2c's, +1.4 ms. At the heavy spot the smooth way
  loses 6 fps where 2c lost 3.7: the table's 1.0 ms every frame on the
  main thread.

## Step 3b: the table of colours, only what changed

Why: step 3's log had the table of colours made again every frame, 1.0 ms
of the main thread's time, because the game cycles part of its palette
every frame; and the user, from the screenshots of both ways (lights on
and off at the start of level 1), found the fitted way better than the
table way.

What it does:

- **`table` is gone**: `shadows = smooth` or `original`; any other value,
  `table` included, is smooth, and the next save writes `smooth`.
  `shade_lut_fill` has no mode any more.
- **`vgpu_set_palette`** marks the colours stale only when the palette,
  the gamma or index 255's black differ from the last ones given.
- **The table is made in RAM** and copied to the next texture in one go,
  in place of byte by byte into GPU memory. All of it when the light
  table, the gamma or index 255 changed; else only the texels whose row-0
  palette entry changed (`shade_lut_affected`); no copy when none did.
- `tests/shadelut/` checks that the texels made again after a change give
  the whole table made from the new palette.
- `lut=a/b/c` in the log: tables made, ms, texels made again, a frame.

What a capture should show: `lut=` near 0 ms, a few texels a frame where
the palette cycles; at the heavy spot, the smooth way 3 to 4 fps under
the 8-bit way, as in 2c, not 6; the look unchanged.

**Results (2026-11-20, the profile build, 320 s with Variants on
Alternate, level 1).**

- Every start-up check passed. The user's `prefs.txt` said `table`, and
  the game wrote it back as `smooth`.
- **`lut=`** in the smooth seconds: 0.33 to 0.49 tables a frame,
  **0.036 to 0.054 ms**, 2.1 to 2.5 texels: a few palette entries
  cycling, a few texels made again, where step 3 made the whole table
  every frame in 1.0 ms. Nothing in the 8-bit seconds.
- **The cost**, the two ways in turn, 20 s each:

  | window | spot | `gpuwait`, 8-bit | smooth | fps, 8-bit | smooth |
  |---|---|---|---|---|---|
  | t=61-92 | level 1, the start | 0.36 / 0.36 | 1.69 | 60.0 | 60.0 |
  | t=172-212 | level 1, 17.95,23.35, still | 3.59 / 3.59 | 4.95 | 60.0 | 60.0 |
  | t=289-294 | level 1, 17.50,23.30, still | 3.59 | 5.00 | 60.0 | 58.6 |

  The GPU's share stays +1.4 ms; the main thread no longer pays the
  table. At the heavy spot both ways held the screen's rate this time,
  the smooth way down 1.4 fps at most.
- One smooth window at the start (t=112) had `gpuwait` 4.6 ms where the
  one before it had 1.7 at the same place: not explained, not seen
  again.

## Step 4: the view through the screen buffer

Why: the view reaches the screen through the 8-bit screen buffer, not
from its GPU canvas, when `VitaShowView` turns it down (the game paused:
the options panel and its gamma slider; the zoom rectangle on its way in
the paneled view; `frsetup.c` then copies the canvas there) and when
`VitaSyncView` copies it before the game draws over it (the pause, the
cursor, a video mail, the help overlay). The screen then showed it
through the palette, the original look.

What it does:

- **The copy is remembered** (`vgpu_view_copied`, from `frsetup.c` and
  `VitaSyncView`): which canvas, its hand-out serial (`vgpu_canvas`
  counts them), and where it went. It holds while that canvas isn't
  handed out again and was drawn the smooth way; it is forgotten when a
  view is shown from its canvas again (`VitaShowView`) and at a loop
  change (`mainloop.c`), whose other screens follow.
- **`vgpu_draw_copied_view`**, in `SDLDraw` right after the screen buffer
  is drawn: the screen's shader over that rectangle, reading the screen
  buffer's colours and indices at the screen's coordinates
  (`vgpu_set_screen`) and the canvas's side buffer at the view's. A pixel
  the game drew over the view (the panel, the text, the cursor, the zoom
  rectangle, a video mail) holds another index, and shows its palette's
  colour. One that holds the same index by chance shows that texel at its
  light: the same colour within a light step.
- The shader reads the side buffer at a second pair of coordinates
  (`vLight.xy`); for a view shown from its canvas both pairs are the same.
- `shownover=` in the log: views shown smooth over the screen buffer, a
  frame.

What a capture should show:

| where | expected |
|---|---|
| the options panel, full screen and paneled | the view behind it smooth; the panel as before; the gamma slider's changes on the smooth view |
| the zoom rectangle (an object picked up in the paneled view) | the view smooth, the rectangle drawn right |
| a video mail, the help overlay, the cursor | drawn right over a smooth view |
| the map, the main menu, a cutscene, cyberspace | no smooth pixels anywhere |
| the log | `shownover=` about 1 a frame while paused, 0 in play; every check 0 wrong |

**Results (2026-11-20, the profile build, 561 s: the paneled and the full
screen views, the options panel, the gamma slider, the zoom rectangle, the
main menu, a cutscene).**

- The user: everything looks right: the view smooth behind the panel and
  under the zoom rectangle, the gamma slider followed, the panel, the
  rectangle and the overlays as before, nothing stray on the other
  screens.
- Every start-up check passed.
- `shownover=`: about 1 a frame in 105 s, 39 in the paneled view and 66
  in full screen, the seconds paused or with the rectangle flying (25 of
  them shared with `shown=`, the game going back and forth). 0 in the
  15 s on the main menu and in the cutscene.
