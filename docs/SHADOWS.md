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
the light table does (infrared). A "table" way, the colours of two rows
blended, is kept to compare.

## Roadmap

| Step | What | Result |
|---|---|---|
| 1 | The prefs setting; the side buffers and the side pass; a start-up check; counters, and the side buffer in the frame dumps; a preview of the look on a PC from a dump | correct (0 mismatches in 84.8 M pixels), the smooth look in the previews; the GPU's work doubled: `gpuwait` 8.6 ms against 0.3-3.9, 53 s of 233 under 50 fps |
| 2 | One pass: each scene drawn once, into the 32-bit buffer, with an import and an extract (replaces "the main thread waits for the canvas only") | correct (0 mismatches in 83.2 M pixels); `gpuwait` +4 ms against the 8-bit way at the same spot, whatever the scene: half of step 1's worst, still a frame rate of 52 in heavy places |
| 3 | The table of colours, the screen's shader, its start-up check, a PC test of the table: smooth shadows on the screen | |
| 4 | Where the view is copied to the screen buffer: paused, the options panel and its gamma slider, the zoom rectangle | |
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
