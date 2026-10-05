# Vita GPU renderer notes

The multicore work (`docs/PERFORMANCE-CPU.md`) took the Vita build from
27.0 to 44.8 fps at 960×544 by filling the 3D view's pixels on three
cores. This document follows the next step: having the GPU fill them.

Captures for this work are in `docs/profiles-gpu/`, named
`profile-step-N.txt`; frame dumps are under `docs/profiles-gpu/dumps/`.

## Starting point

Standing still in the first area, full 3D view, 960×544, three cores
(`docs/profiles-cpu/profile-step-6.txt`):

| part of the frame | time |
|---|---|
| whole frame | 22.3 ms (44.8 fps) |
| pixel filling, wall time on three cores | 9.6 ms |
| the rest of drawing the 3D views (traversal, recording, the help scan) | 2.9 ms |
| stars, HUD overlay and copy of the view to the screen (`sendview`) | 3.4 ms |
| showing the frame (`present`) | 3.2 ms |
| game simulation and input | 0.8 ms |
| everything else | 2.4 ms |

If the GPU took over most of the 9.6 ms the frame would be about 13 ms,
which is the display's 60 fps limit, with cores 1 and 2 left idle. That
is an estimate; nothing here is measured yet.

## Decisions

- **Same look as the software renderer.** 8-bit palette, the game's own
  light tables, sharp texels. A GPU fills polygons by slightly different
  rules, so the result can't be pixel-identical, but colours and shading
  are the same data.
- **The CPU renderers stay, selectable**, as the reference and as the
  fallback for anything the GPU path doesn't draw.
- **Shaders are compiled on the Vita, each time the game starts**, with
  Sony's compiler module (`ur0:data/libshacccg.suprx`, through
  vitaShaRK). The module is a requirement of the GPU renderer; no
  compiled programs are shipped. (The first plan was to ship them at the
  end; dropped at step G6.) Without the module the game runs on its CPU
  renderers.
- **The GPU renderer is the default** (step G6), with "3 cores" and
  "1 core" selectable in Vita Options.

## The approach

- **The recorder is the GPU's draw list.** `src/Libraries/3D/Source/rastq.c`
  already turns a view into a list of 2D polygons in drawing order, with
  their textures and drawing state. The CPU paths fill that list on one
  or three threads; the GPU path sends it to the GPU.
- **The GPU draws palette indices.** Its render target is an 8-bit
  surface (`SCE_GXM_COLOR_FORMAT_U8_R`) laid over the view's own canvas.
  Textures stay 8-bit, and lighting is a lookup in the game's light table
  (`grd_screen->ltab`), held as a second texture. What lands in the
  canvas is the same kind of data the software mappers write, so the HUD
  overlay, the stars, palette effects and the final palette-to-screen
  step (already done by the GPU through vita2d) are untouched.
- **No depth buffer.** The list is in back-to-front order, as the
  software renderer needs it.
- **On top of what is linked already.** vita2d owns the GXM context and
  shader patcher (`vita2d_get_context`, `vita2d_get_shader_patcher`), and
  SDL2's Vita build already pulls vitaShaRK into the binary.

### Why not the existing OpenGL renderer

`src/MacSrc/OpenGL.cc` is the PC port's GPU renderer, and vitaGL could
run something like it. It was not chosen:

- It draws in true colour with its own lighting (a brightness factor per
  vertex), so it looks like the PC's OpenGL mode, not like the software
  renderer.
- It takes over before projection, with 3D points, so it can't share the
  recorder or its checks.
- It leaves cyberspace, the help scan and the security cameras to the
  CPU.
- vitaGL and vita2d each want to own the GPU context.

## Roadmap

| step | what | what it proves |
|---|---|---|
| G1 | Feasibility: GPU set-up, a shader compiled on the Vita, the list drawn as flat-coloured polygons into an 8-bit canvas | **done**: the pieces work, coverage matches, a view costs about 1.4 ms |
| G2 | Buffers: keep the CPU out of the memory the GPU draws into, and show the GPU's canvas without copying it | **done**: a GPU frame is faster overall in flat colours (45 fps → the screen's 60), with two faults to fix |
| G3 | G2's two faults (the canvas shown while it is wiped; polygons the CPU skips), then textures, light-table and CLUT lookups, perspective, sprites, transparency | **done**: textures and geometry are right; light bands are angular where the CPU's are round; a textured scene costs the GPU 5.5 ms, so the frame is no faster than three cores yet |
| G4 | Light interpolated the way the mappers do it; the shader's cost; which calls still go to the CPU and split the frame into scenes | **done**: round shadows again, 95% of pixels identical; shader B takes 3.2 ms a scene against 6.0; 53 fps standing still against 44.5 on three cores |
| G5 | Light worked out per pixel in place of slabs; the colour-table fill type on the GPU, so that a frame is one scene; shader A dropped | **done**: shadow edges as fine as the CPU's; the screen's rate at the test spot (a 14.6 ms frame against 22.4 on three cores) and while walking (62.5 fps against 36.3) |
| G6 | The GPU renderer in the normal build behind a "Renderer" setting, default on; the paneled view; the screen buffer when the view stops being drawn; cyberspace and low resolution left to the CPU | **done**: played through both views, pause, panels, video mail and cyberspace without a glitch; 62.9 fps against 35.9 while walking and fighting |
| G7 | Holding the screen's rate: the help scan's repeated questions, the queue told per view whether it is the GPU's, and measurements of where a GPU frame's CPU time, `sendview` and the input stalls go | **done**: the door is back at the screen's rate; the stalls are sound effects decoded for their first use; what is left under 60 fps (58.5 at one angle) is the HUD and the help scan on the CPU, next to 3.4 ms of the main thread waiting for the GPU |
| G8 | The help scan run while the GPU draws, in place of the main thread waiting; the HUD measured part by part | **done**: where the scan runs the main thread waits 0.4 ms for the GPU in place of 3.4; G7's heavy angle and the paneled view with labels are at the screen's rate with 1.8 ms to spare. A heavier angle (132 polygons in 354 pieces, no scan) is at 53 to 57 fps |
| G9 | A cheaper HUD: outlined text drawn once and copied; the vitals' arrows and the inventory's buttons stretched once and copied; transparent copies four pixels at a time | **done**: the full-screen HUD from 2.2-3.3 ms to 1.35-1.9, from 4.4 to 1.5 with a help label, on every renderer, no pixel changed in 438 checks. The hallway angle is at 59 fps in full screen, 55 paneled |
| G10 | The sending shared between the three cores: deciding what each call is and cutting it up, the hand-over staying in order | about 2 ms off the frames with many pieces. **Not started** |

G2 was not in the first roadmap. G1 showed that the memory question
decides whether the GPU path is worth anything, so it comes before
textures.

Open questions, and the step that answers each:

- ~~Does an 8-bit render target give exact palette indices?~~ Yes (G1).
- ~~How fast can the CPU read and write memory the GPU renders into?~~
  Writing is 3 times slower than in ordinary memory, reading 4 times
  (G1). G2 is about that.
- ~~How long does the GPU take per view, and what does waiting for it
  cost?~~ About 1.4 ms for the main view in flat colours (G1).
- ~~Is a GPU frame faster overall once the CPU stays out of GPU
  memory?~~ Yes, in flat colours: it reaches the screen's refresh rate
  where three cores give 45 fps (G2).
- ~~What do textures and the table lookups cost the GPU?~~ 5.5 ms for a
  full-screen scene, against 1.3 ms in flat colours, whatever the number
  of polygons: it is the shader's work per pixel (G3). G4 is about that.
- Can a shader read the pixel already drawn? Translucent surfaces need
  it; if not, those calls fall back to the CPU. (G4)

## How it is checked

GPU code only runs on the Vita, and its output isn't identical to the
CPU's, so the multicore work's "0 differing rows" can't be the test.
Instead the profile build:

- compares, every 63rd view, the GPU's result with the CPU's rendering
  of the same list (since G3 the real one, with textures), and logs the
  share of pixels that differ;
- writes some of those pairs to `ux0:data/systemshock/gpudumps/`, to be
  looked at on a PC: the third comparison, and each later one that
  differs by more pixels than any before it and by more than 5% of them,
  five in all. Writing a pair stops the game for a second or two;
- draws known patterns at start-up and reads them back (`gpu.txt`);
- logs the time spent sending the list and waiting for the GPU.

The PC harness (`tests/rastq/run.sh`) keeps guarding the recorder and the
CPU paths. Since G3 it also checks what the queue hands the GPU, with a
stand-in that fills polygons in the shader's arithmetic: see step G3.

## Step G1: feasibility

Profile builds only; the normal build has no GPU code.

### What was built

- **`src/MacSrc/VitaGpu.c`**, the GPU module.
  - At start-up it loads the shader compiler, compiles one vertex and one
    fragment program from Cg source in the file, and registers them with
    vita2d's shader patcher. Each stage is written to
    `ux0:data/systemshock/gpu.txt` before it starts, so a crash shows
    where it happened.
  - `vgpu_alloc` hands out CDRAM mapped for the GPU. The main 3D view
    draws into the pixels of an SDL surface, `offscreenDrawSurface`
    (`SetupOffscreenBitmaps` in `src/MacSrc/ShockBitmap.c`); in profile
    builds those pixels come from `vgpu_alloc`. The GPU is therefore set
    up before that surface is first created.
  - A render target is made over a canvas the first time a scene is
    begun on it: an 8-bit colour surface with the canvas's own stride,
    and a depth buffer that is never tested.
  - A scene is one indexed triangle list. Polygons are fanned into
    triangles on the CPU, with positions already in clip space.
  - After `sceGxmEndScene` it calls `sceGxmFinish`, because the CPU goes
    on to draw the stars and the HUD into the same canvas.
- **The recorder's GPU path** (`rastq.c`): `rastq_set_gpu` takes a set of
  hooks (`begin`, `flat_poly`, `end`, `compared`), and a view that goes
  to the GPU is flushed through them instead of being replayed. If
  `begin` refuses, the CPU replays as before.
- **Flat colours for now.** Each call is drawn as one polygon in one
  palette index: a polygon's own colour, or the texel in the middle of a
  bitmap. Vertices are moved by half a pixel, because the software
  mappers fill the pixels whose integer coordinates are inside a polygon
  and a GPU those whose centres are.
- **The index sent is `(index + 0.25) / 255`,** which gives the index
  whether the 8-bit target rounds or truncates.

### What the capture will show

- **In `gpu.txt` and the first lines of the log:**
  - whether the shaders compiled;
  - `index_check mismatches=N/256`: one column per palette index, drawn
    and read back;
  - `undrawn_kept=`: whether a scene keeps the pixels it doesn't draw,
    which a view drawn in several scenes needs;
  - how long the CPU takes to write and to read 512 KB of GPU memory and
    of ordinary memory.
- **Per window, when variant 1 (GPU) is active:**
  - `gpusubmit=` and `gpuwait=`: time per frame to send the list, and to
    wait for the GPU;
  - `gpufallbacks=`: lists the GPU refused;
  - `gpudiff=a/b`: over the comparisons so far, `a` pixels of `b`
    differed between the GPU's flat fill and the CPU's.
- **Variant 0 (three cores) now draws into GPU memory too,** so its fps
  against step 6's 44.8 says what that memory costs the CPU.
- **Frame pairs** in `gpudumps/`: the GPU's and the CPU's flat rendering
  of the same view, with the palette.

The PC harness exercises the recorder's GPU path with a stand-in that
fills the flat polygons on the CPU; its comparison must then find no
difference.

### First run (`docs/profiles-gpu/profile-step-1.txt`, `gpu.txt`)

The set-up worked; no game view reached the GPU.

- **Shaders compile on the Vita:** a 284-byte vertex program and a
  228-byte fragment program.
- **The 8-bit target stores exact indices:**
  `index_check mismatches=0/256`.
- **A scene keeps the pixels it doesn't draw** (`undrawn_kept=1`).
- **The CPU writes GPU memory about 3 times slower** than ordinary
  memory: 737 µs against 257 µs for 512 KB. The read timing of this run
  is void: the compiler had removed the loop.
- **Every view was refused.** `gpufallbacks` reached 7084 and
  `gpuscenes` stayed 0, so both variants were the three-core replay
  (44.8 and 45.1 fps). The GPU module only draws into canvases in GPU
  memory, and this build allocated that memory in `fr_place_view`, where
  the main view never gets its canvas: it is handed the pixels of
  `offscreenDrawSurface`. The second build allocates those instead, and
  logs a refusal with its reason.

### Second run (`docs/profiles-gpu/profile-step-1b.txt`, `gpudumps-step-1b/`)

The GPU drew the main view, in flat colours, in the GPU variant.
Standing still in the first area, medians of 203 and 205 windows:

| | three cores | GPU |
|---|---|---|
| fps | 19.5 | 28.4 |
| frame | 51.2 ms | 35.3 ms |
| pixel filling (`raster`) | 17.8 ms | 1.8 ms |
| of which sending the list / waiting for the GPU | – | 0.09 / 1.28 ms |
| stars, HUD, copy to the screen (`sendview`) | 8.1 ms | 8.1 ms |
| help scan | 18.8 ms | 19.0 ms |

- **The GPU is fast.** 87 flat polygons covering the 960×544 view cost
  0.09 ms to send and 1.28 ms to wait for. Three cores take 9.5 ms in
  ordinary memory.
- **Coverage matches the CPU's.** With the half-pixel shift, 16 pixels of
  522,240 differ between the GPU's flat fill and the CPU's, all on
  polygon edges (`gpudumps-step-1b/01` to `03`). No list was refused.
- **Everything the CPU does in the canvas got slower**, in both variants,
  because this build put the main view's canvas in GPU memory (CDRAM)
  for both. The start-up timing shows why: 512 KB takes 0.71 ms to write
  and 4.9 ms to read there, against 0.25 ms and 1.2 ms in ordinary
  memory.
  - *Three-core filling:* 17.8 ms instead of 9.5.
  - *`sendview`:* 8.1 ms instead of 3.2. It ends with a copy of the whole
    canvas to the screen buffer, which reads all of it.
  - *The help scan:* 18.8 ms instead of 3.5. `olh_init_single_scan`
    gives the scan the main view's own pixels as its canvas (it draws in
    the top-left corner, after the main view has been sent), so it was
    rasterizing on one core in GPU memory.
- **So the GPU frame was slower than today's 44.8 fps,** although its
  pixel filling is five times faster.
- **Still open:**
  - While walking, a few comparisons differed by tens of thousands of
    pixels (the running total jumps from 6,775 to 53,965 within a few
    checks). None of the five dumped pairs is one of them; G2 dumps
    exactly those.
  - The dumps were written every 30th comparison in this build, and each
    one froze the game for about two seconds.

### Go for G2

The approach holds: exact indices, matching coverage, a cheap GPU. What
has to change is where the pixels live.

## Step G2: keep the CPU out of GPU memory

Profile builds only.

### What was built

- **Two canvases.** The view keeps its ordinary-memory canvas
  (`offscreenDrawSurface`, as upstream). The GPU has its own: a second
  vita2d texture, 8-bit paletted like the screen's and given the same
  palette (`MakeViewTexture` in `src/MacSrc/Shock.c`).
- **A view picks its canvas per frame.** `fr_start_view`
  (`src/GameSrc/frsetup.c`) points the main view's draw canvas at the
  GPU's when `rastq_gpu_next()` says the frame goes to the GPU, and back
  at its own memory otherwise. Stars, HUD overlay and cursor follow the
  draw canvas. The help scan, the security cameras and the 360 view
  never use the GPU canvas, so the scan is back in ordinary memory.
- **Present from the canvas.** In the full-screen 3D view, `fr_send_view`
  asks `VitaShowView` before copying the view to the screen buffer. When
  it agrees, the copy is skipped and the next `SDLDraw` draws the GPU
  canvas texture instead of the screen texture, which also skips the copy
  of the screen buffer to its texture. Anything drawn straight to the
  screen buffer during such a frame doesn't show; the paneled view keeps
  the copy.
- **Variants:** 0 = three cores, 1 = GPU with the view copied to the
  screen as before, 2 = GPU with a full-screen view shown from its
  canvas.

### Results (`docs/profiles-gpu/profile-step-2.txt`, `gpudumps-step-2/`)

Standing still in the first area, 960x544, medians of the 1 s windows.
The GPU variants are in flat colours.

| ms per frame | 0: three cores | 1: GPU, view copied | 2: GPU, shown from its canvas |
|---|---|---|---|
| pixel filling (`raster`) | 9.49 | 1.73 | 1.72 |
| of which waiting for the GPU | | 1.29 | 1.28 |
| 3D pass (`traverse`) | 12.32 | 4.50 | 4.40 |
| stars, HUD, copy to screen (`sendview`) | 3.19 | 7.71 | 2.30 |
| help scan | 3.53 | 3.54 | 3.49 |
| `present` | 3.17 | 3.02 | 5.04 |
| whole frame | 22.21 | 19.35 | 15.92 |
| fps | 45.0 | 51.7 | 62.8 |

- **The memory fix worked.** The three-core path and the help scan are
  back at their step-6 speed: neither touches GPU memory any more.
- **Copying the view out of GPU memory costs what the GPU saves.**
  Variant 1's `sendview` is 4.5 ms longer than the three cores': the
  copy to the screen buffer reads the whole canvas, in the slow
  direction. Showing the canvas itself (variant 2) removes it.
- **Variant 2 is at the screen's refresh rate.** The menus, which do no
  work, log 61 fps with about 15 ms of every frame spent in `present`:
  that is the display's 60 Hz by the profiler's clock. Variant 2's
  `present` is 2 ms longer than the others' although it copies less,
  which is the same wait. Its real work is about 11 to 13 ms a frame, so
  62.8 fps is a floor for the flat-coloured GPU path, not its speed. The
  next capture logs the wait on its own.
- **Exactness unchanged**: `check=0/377`, `leaks=0`.
- **`gpufallbacks=136`**: the paneled (non-full-screen) view, whose
  804-pixel-wide canvas the GPU module refuses (`gpu.txt`). Expected;
  that view is on the roadmap for G6.

Two faults:

- **Variant 2 glitches and flickers** (seen on the Vita: blocks of the
  picture black, HUD text cut where the blocks meet). Every view starts
  by wiping its canvas on the CPU (`gr_clear` in `fr_start_view`,
  `src/GameSrc/frsetup.c`). vita2d draws the screen a moment after
  `SDLDraw` asks for it, tile by tile, from the canvas texture; the next
  frame's wipe lands in the middle of that. Variant 1 is immune: it
  copies the finished view before the next frame starts. So a canvas
  that is on its way to the screen must not be written, by the wipe or
  by the next frame's drawing.
- **The GPU draws polygons the CPU skips.** Standing still, 47 pixels of
  522,240 differ per comparison. While walking, some comparisons differ
  by thousands (dumps 02 to 05: 2,709 to 15,800 pixels): a whole
  polygon present on the GPU's side only. The software fillers stop at
  the first row whose right edge is left of its left edge, which for a
  polygon wound the other way round is the first row; the GPU scene has
  culling off and draws both windings.

### Go for G3

A GPU frame is faster overall once nothing on the CPU reads or wipes the
GPU's memory. G3 starts with the two faults:

- three GPU canvases used in turn, as vita2d does with the screen's own
  buffers, so that a canvas is reused only after two newer frames have
  been handed to the screen; and the wipe done by the GPU, as the first
  rectangle of its scene;
- polygons wound the way the software fillers reject are dropped before
  they reach the GPU.

Then textures: every software mapper ends by picking a texel and
optionally passing it through a 256-entry table (a CLUT, a row of the
light table, a solid colour). One shader can do that with the call's
bitmap as one texture and the tables as rows of a second one.

## Step G3: two fixes, then textures

Profile builds only.

### The two fixes

- **Three GPU canvases, used in turn** (`MakeViewTextures` in
  `src/MacSrc/Shock.c`, `vgpu_canvas` in `src/MacSrc/VitaGpu.c`). Each
  frame takes the canvas used longest ago. vita2d lets at most two
  frames wait for the screen, so by the time a canvas comes round again
  two newer frames have been handed over and its own has been drawn. One
  render target serves the three; each scene gets a colour surface over
  the canvas it draws into.
- **The clear is the GPU's.** `fr_start_view` asks `rastq_gpu_clear`
  instead of clearing; the clear becomes the first rectangle of the
  view's first scene. If the GPU doesn't take the view after all, or a
  call has to be drawn straight to the canvas first, the queue clears on
  the CPU.
- **Winding.** A call wound anticlockwise on screen (`gpu_reversed` in
  `rastq.c`) is dropped before it reaches the GPU: the mappers draw
  nothing for it.

### Textures

What a mapper does for a pixel, and what the shader does:

| mapper | texel | then |
|---|---|---|
| plain | `bits[...]` | as is |
| colour table | `bits[...]` | `clut[texel]` |
| lit | `bits[...]` | `ltab[texel + 256 * light level]` |
| transparent | any of the above | skipped when the texel is 0 |

- **Tables.** The scene's tables are one texture, 256 wide: rows 0 to 15
  are the light table (`grd_screen->ltab`), row 16 leaves a texel as it
  is, the rest are the colour tables its calls use (a table inside the
  light table, as the 3D library builds for evenly lit polygons, is one
  of rows 0 to 15). The shader looks the texel up in the call's bitmap,
  then (texel, row) in the tables.
- **Texel addressing**, as the mappers have it and not as a GPU would by
  itself (`rastq.h`):
  - the row, floor and wall mappers read
    `bits[(v * width + u) mod (width * height)]`: u beyond the width
    moves on to the next row. The shader carries u over the same way;
  - the perspective mapper wraps u and v each on their own;
  - sprites, the only bitmaps whose sides aren't powers of two, don't
    wrap: clamped.
- **Perspective.** Vertices carry (u·q, v·q, q) and the shader divides.
  q is the vertex's w (1/z) for the floor, wall and perspective mappers,
  and 1 for the linear mapper, which ignores w (the 3D library doesn't
  set it then).
- **Light level.** The floor and wall mappers carry it through the
  perspective division with u and v; the perspective mapper steps it
  along its scanlines, and the linear mapper has no division. So the
  table row is `row / q + flat_row`, each mapper using one of the two.
- **Bitmaps** are copied into GPU memory once per scene
  (`texture_for`), as 8-bit textures sampled without filtering.
- **One vertex format and one shader pair** (plus the pair's variant
  that discards texel 0). A flat polygon goes through it too, as a
  "texture map" of the tables texture's unchanged row at its colour, so
  a scene is a run of draws that only change texture.

What the GPU path covers, decided per call by `gpu_classify`:

- texture maps on `BMT_FLAT8` bitmaps with the normal fill type, in the
  families the 3D library asks for (linear, floor, wall, perspective;
  plain, lit, colour table; opaque or transparent), sprites included;
- flat polygons (`FIX_UPOLY`), in the normal, colour-table and solid
  fill types.

Everything else (translucent bitmaps and polygons, shaded polygons, more
than 16 vertices, a light level outside the table, the perspective
mapper under a clip rectangle) is drawn by the CPU, in its place in the
list: the scene under way is ended and waited for, the CPU draws, and
the next GPU call opens a new scene. The picture stays right; the cost
is logged (`gpucpu=calls/ms`, `gpuscenes`).

If the textured shaders don't compile on the Vita, the build draws in
flat colours as in G2 and says so (`textures=off` in the report line).

### Checked on the PC

`tests/rastq/rastq_test.c` has a stand-in for the GPU: it implements the
queue's GPU interface and fills polygons as triangle fans with the
shader's arithmetic in single precision. Scenes made for it are drawn by
the mappers and by the stand-in, and the two compared.

They can't be equal, for three reasons that are not mistakes:

- the mappers don't sample exactly where a GPU does. The floor and wall
  mappers take the slope of 1/z from spans in whole pixels, which moves
  texel boundaries by a pixel or two in the middle of a long wall; the
  perspective mapper works in 16.16 fixed point with a w of 12 bits or
  so;
- a GPU interpolates a light level over triangles, the mappers along a
  polygon's edges and then across each row. The two agree only when the
  level is one linear function over the whole polygon. The comparison
  scenes make it one; the game's quads often aren't, so on the Vita the
  light bands of a wall will sit a little differently;
- where one pixel covers several texels, the two pick different ones.

So the test counts the pixels of the stand-in whose value the mappers
have neither there nor within two pixels, on scenes with noise-free
textures over a flat background. Over 900 frames:

| | share of pixels |
|---|---|
| differ from the mappers' | 2.32% |
| and have no match within two pixels | 0.157% |
| limit on the second, per run / per frame | 0.5% / 6% |

Flat polygons come out identical to the mappers', pixel for pixel.

Deliberate mistakes in the translation, and the second figure with each:

| mistake | no match within two pixels |
|---|---|
| none | 0.16% |
| no culling / the other winding culled | 5.5% / 44% |
| colour table ignored | 23% |
| light table not sent | 36% |
| light one row off (through the division / not) | 10.9% / 1.5% |
| the two ways of carrying light swapped | 1.0% |
| transparency dropped | 9.9% |
| no perspective / perspective for the linear mapper | 10.4% / 8.2% |
| u and v swapped | 38% |
| u three texels off | 25% |
| u carried over as two separate wraps | 0.71% |
| clear dropped / in the wrong colour | 28% |
| the CPU's calls skipped | 13% |
| two separate wraps as u carried over (perspective mapper) | 0.34%, **under the limit** |
| colour-table fill of a flat polygon not applied | 0.31%, **under the limit** |
| solid fill of a flat polygon not applied | 0.19%, **under the limit** |

The last three move the figure the right way but stay under the limit:
few pixels are concerned, or the shift is within the two pixels. The
queue's own comparison (as on the Vita) is exercised too: it must leave
the mappers' picture on the canvas.

One upstream bug met on the way: the lit floor mapper doesn't step along
a row of a bitmap whose sides aren't powers of two
(`gri_lit_floor_umap_loop`, the `GRL_OPAQUE` case). The game never gets
there (the 3D library refuses such bitmaps for everything but sprites),
and the GPU path leaves those calls to the CPU.

### What the capture will show

Variants, 5 s each: 0 = three cores, 1 = GPU with the view copied to the
screen, 2 = GPU with the view shown from its canvas.

- `gpu.txt`: the shaders' sizes, the texture units, and
  `texture_check plain= table= light= wrap=`: wrong columns out of 256
  for four patterns drawn with the textured shaders at start-up. All
  four should be 0;
- `gpudiff=`: pixels differing from the CPU's rendering in the periodic
  comparisons. A few percent is expected, for the reasons above;
- `gpuwait=`, `gpusubmit=`: what textures cost the GPU and the sending;
- `gputex=`: KB of bitmaps copied to GPU memory per frame;
- `gpuscenes=`, `gpucpu=calls/ms`: how often the CPU has to step in, and
  what that costs: what G4 and G5 are worth;
- `gpuculled=`: calls dropped for their winding;
- `swapwait=`: ms per frame spent waiting to hand a frame to the screen,
  so that the work in a frame is its time minus this.

### Results (`docs/profiles-gpu/profile-step-3.txt`, `gpu.txt`, `gpudumps-step-3/`)

What works:

- **The shaders compile on the Vita** (356, 596 and 696 bytes) and the
  start-up patterns come back exact: `texture_check plain=0 table=0
  light=0 wrap=0`. Table lookups through an 8-bit texture are exact for
  all 256 indices.
- **The glitch of G2's third phase is gone**: no black blocks and no
  flicker on the Vita with the three canvases and the GPU's clear.
- **Textures and geometry are right.** In the dumped comparisons, 76 to
  81% of the pixels are identical to the CPU's; of the rest, all but
  0.6 to 1.0% of the frame are the same texel shown one light level
  brighter or darker (found by looking each differing pair up in
  `SHADTABL.DAT`). Edges, texel positions and winding account for that
  last percent.
- `check=0/292`, `leaks=0`; 10 calls a frame culled for their winding.

What doesn't yet:

- **Light bands are angular where the CPU's are round**, and whole
  stretches of wall sit one level off. `gpudiff` is 16.6% standing
  still. The cause is the one named in "Checked on the PC": a quad's
  four corners rarely have light levels that fit one plane, the mappers
  interpolate along the edges and then along each row (or column), and
  the GPU interpolates over the two triangles of the quad. Confirmed on
  the PC by giving the comparison scenes a light level of their own at
  each corner:

  | stand-in against the mappers | no match within two pixels |
  |---|---|
  | light level one linear function per polygon (G3's scenes) | 0.16% |
  | a level of its own at each corner | 3.7% |
  | the same, lit polygons cut into slabs 8 pixels thick along the mapper's scan lines | 0.5% |

  A slab's two long sides are lines the mapper draws (rows for the
  linear and floor mappers, columns for the wall mapper, lines of one
  depth for the perspective mapper), with the values at their ends taken
  along the polygon's edges. Between them the GPU's interpolation has
  little room to differ. What is left of the 0.5% is in floors a few
  rows high whose texture is squeezed several texels to the pixel.
- **Once a second the picture shows the CPU's rendering for one frame**:
  the periodic comparison leaves the CPU's result on the canvas. It
  should put the GPU's back.
- **The GPU path is not faster yet.** Standing still in the first area,
  medians of the 1 s windows:

  | ms per frame | 0: three cores | 1: GPU, view copied | 2: GPU, shown from its canvas | G2's variant 2 (flat) |
  |---|---|---|---|---|
  | pixel filling (`raster`) | 9.57 | 12.23 | 12.10 | 1.72 |
  | of which waiting for the GPU | | 10.80 | 10.71 | 1.28 |
  | of which sending (`gpusubmit`) | | 1.00 | 0.96 | 0.08 |
  | `sendview` | 3.20 | 7.65 | 2.32 | 2.30 |
  | `present` | 3.23 | 3.15 | 1.07 | 5.04 |
  | whole frame | 22.45 | 29.06 | 21.38 | 15.92 |
  | fps | 44.5 | 34.4 | 46.8 | 62.8 |

  - **A textured scene takes the GPU about 5.5 ms**, and the number of
    polygons has nothing to do with it: 5.0 to 6.6 ms in the windows
    with one scene a frame, for 5 polygons as for 123. It is the work
    per pixel: two texture reads at computed coordinates, a division
    and three roundings, against a constant in G2.
  - **Each further scene adds about 2.5 ms**: 5.8 ms with one scene a
    frame, 8.7 with two, 10.75 with three (medians over all windows).
    The test spot has three, because 9 calls a frame are drawn by the
    CPU (`gpucpu=9.0/0.07`): the CPU's own time for them is nothing,
    what costs is ending a scene and starting another.
  - Sending is 1 ms, of which the copy of 158 KB of bitmaps a frame.
  - `swapwait` is 0.08 ms: no variant waits for the screen any more
    (the menus do, 5.8 ms).
  - Variant 1 pays 4.4 ms for copying the view out of GPU memory, as in
    G2. It has told what it could and can go.

### Go for G4

The look is one fix away and the speed two:

- lit polygons cut into slabs along the mapper's scan lines, as tried
  on the PC;
- a cheaper shader. The GPU can do the perspective division and the
  wrap itself if the vertices carry a real w and the bitmap is read at
  interpolated coordinates; what stays computed is the table lookup.
  The row, floor and wall mappers' carrying of u into the next row is
  then not reproduced (a texture repeated along u would sit one texel
  lower at each repeat on the CPU, not on the GPU);
- fewer scenes: a count of the CPU-drawn calls by kind, to know which
  kinds to move to the GPU first.

## Step G4: light like the CPU's, and a cheaper shader

Profile builds only.

### Light in slabs

A lit polygon with four or more corners is cut, before it reaches the
GPU, into slabs 8 pixels thick along the lines its mapper draws
(`gpu_emit_slabs` in `src/Libraries/3D/Source/rastq.c`):

| mapper | slabs along |
|---|---|
| linear, floor | rows |
| wall | columns |
| perspective | lines of one depth (equal w) |

"Mapper" is the one that really draws the call. `per_umap` hands a
polygon that is flat enough, or lies like a floor or a wall, to the
linear, floor or wall mapper, and each carries light its own way; the
queue asks it which (`gr_per_umap_family` in
`src/Libraries/2D/Source/permap.c`) and treats the call as one of that
mapper's.

The cut is a convex clip that takes every vertex value along the
polygon's edges (`slab_clip`). A slab's two long sides are then lines
the mapper draws, with the mapper's values at their ends; in between the
GPU has 8 pixels to differ in. A triangle is left whole: three levels
always fit one plane.

Two smaller things:

- a lit level gets 1/256 of a level added, as the wall mapper adds it,
  so that one sitting exactly on a row of the table doesn't fall on
  either side of it from pixel to pixel;
- the periodic comparison puts the GPU's picture back on the canvas
  (it left the CPU's, which showed for one frame a second). When a view
  is drawn in several parts, each part is now compared on top of the
  GPU's earlier parts.

### The floor and wall mappers' depth

`h_umap` and `v_umap` don't take a floor's or a wall's depth as its
vertices have it. They replace each vertex's w by a straight line across
the rows (or columns), from the w of the first to the w of the last,
over the whole number of rows between them, and the texture and the
light follow that line. For a wall seen at an angle it lies a pixel or
two off the true perspective in the middle, which is the drift G3 put
down to "spans in whole pixels". `gpu_scan_w` computes the same w in the
same fixed-point arithmetic, and the GPU's vertices carry it.

### Repeats cut on the CPU

The row, floor and wall mappers read `bits[(v * width + u) mod size]`,
so a texture repeated along u sits one texel row lower at each repeat.
G3's shader reproduced that per pixel. It is now done before the GPU:
the polygon (or each slab of it) is cut where u passes a multiple of the
width, a straight line on screen, and each part's v is moved by its
number of repeats (`gpu_emit_repeats`). The GPU then only ever wraps u
and v each on their own, which a texture's repeat mode can do, and the
shader loses that work. Slabs first, repeats within each: the cut
between two repeats runs through the polygon's inside, where only a
slab's long sides have the mapper's light.

### Shader B

Shader A (G3's, minus the carry) works the texel out per pixel: a
division, the rounding, the wrap, then two texture reads at coordinates
it computed.

Shader B leaves that to the hardware:

- the vertices carry a real w (1/q), so the GPU interpolates u, v and
  the table row with the perspective itself;
- the bitmap is read at the coordinates as they arrive, its texture set
  to repeat (or to clamp, for sprites). Nearest-texel sampling is the
  rounding down;
- the tables texture is read at (texel, row / 64): nearest-texel
  sampling picks the row, so there is no rounding to compute.

What it changes in the picture: the perspective mapper's light level,
which that mapper steps along its scan lines without the perspective
division, goes through the division like everything else. Inside a slab
cut along those very lines the two are the same to well under a pixel.

Both shaders read the same vertex layout, each its own fields
(`vgpu_vertex` in `src/MacSrc/VitaGpu.c`); `fill` writes a vertex for
the shader of the scene under way. The start-up check now draws its four
patterns with each shader. Shader B counts on linear textures repeating:
if any of its columns is wrong, it is switched off and shader A takes
its place (`shader_b=off` in the report line).

### Checked on the PC

The comparison scenes now have a light level of their own at each
corner, as the game does, and the stand-in has shader B's arithmetic as
a second mode. Over 900 frames per shader:

| | shader A | shader B |
|---|---|---|
| pixels that differ from the mappers' | 2.39% | 2.39% |
| and have no match within two pixels | 0.203% | 0.203% |
| limit on the second, per run | 0.5% | 0.5% |

The same figure step by step, over 100 frames per canvas size (shader
A; B within 0.01 of it each time):

| | no match within two pixels |
|---|---|
| G3's translation on these scenes (no slabs) | 3.7% |
| slabs | 0.39% |
| repeats cut on the CPU, within each slab | 0.32% |
| the dispatcher asked which mapper draws | 0.28% |
| the floor and wall mappers' depth | 0.20% |
| G3's figure, on scenes whose light fits a plane | 0.16% |

Deliberate mistakes, and the second figure with each (60 frames;
0.18% without any):

| mistake | no match within two pixels |
|---|---|
| no slabs | 2.7% |
| a cut vertex's light, u or position not interpolated | 6.6%, 9.9%, 12.9% |
| the second slab of every polygon dropped | 0.76% |
| row slabs cut along columns | 0.64% |
| repeats not cut / v not moved / moved the other way | 0.75% / 0.70% / 1.7% |
| the two ways of carrying light swapped | 0.89% |
| no culling, colour table ignored, transparency dropped, no perspective, light one row up | 5.6% to 25% |
| the comparison leaving the CPU's picture | caught by the test of the comparison itself |
| wall slabs cut along rows | 0.34%, **under the limit** |
| perspective slabs cut along rows | 0.30%, **under the limit** |
| the floor and wall mappers' depth not used | 0.24%, **under the limit** |
| the dispatcher not asked | 0.19%, **under the limit** |

The last four move the figure the right way but not past the limit: with
slabs this thin their direction matters little for those two mappers,
and the last two concern few polygons, though each of those is then
visibly off (one frame of the 900 was at 17% before them).

### What the capture will show

Variants, 5 s each, both GPU ones shown from the canvases: 0 = three
cores, 1 = GPU with shader A, 2 = GPU with shader B.

- `gpu.txt`: `shader_a=` and `shader_b=` with the wrong columns of each
  one's four patterns; all eight figures should be 0;
- `gpudiff=`: 16.6% in G3; a few percent expected with slabs;
- `gpuwait=` in variant 1 against G3's 10.7 ms: what slabs cost (more
  triangles) and what dropping the carry saves; in variant 2 against
  variant 1: what shader B saves;
- `gpukinds=flat:,plain:,clut:,lit:` and `gpuslabs=`: the GPU's calls
  per frame by kind, and the pieces lit ones were cut into;
- `gpuwhy=...`: the CPU-drawn calls per frame by reason, which is what
  splits a frame into scenes and what G5 has to move to the GPU.

### Results (`docs/profiles-gpu/profile-step-4.txt`, `gpu.txt`, `gpudumps-step-4/`)

Standing still in the first area, medians of the 1 s windows:

| ms per frame | 0: three cores | 1: GPU, shader A | 2: GPU, shader B | G3's variant 2 |
|---|---|---|---|---|
| pixel filling (`raster`) | 9.54 | 13.88 | 9.54 | 12.10 |
| of which waiting for the GPU | | 9.78 | 5.54 | 10.71 |
| of which sending (`gpusubmit`) | | 3.68 | 3.58 | 0.96 |
| `sendview` | 3.25 | 2.36 | 2.35 | 2.32 |
| `present` | 3.21 | 1.04 | 1.03 | 1.07 |
| whole frame | 22.45 | 23.14 | 18.78 | 21.38 |
| fps | 44.5 | 43.2 | 53.3 | 46.8 |

While walking (medians over the walk): 37.3 fps on three cores, 55.8
with shader A, 61.5 with shader B, which is the screen's rate.

- **Both shaders compile and their start-up patterns are exact**
  (`shader_a=on ... shader_b=on`, all eight counts 0). Linear textures
  do repeat.
- **Shader B is the cheaper by a wide margin.** Waiting for the GPU,
  over all windows, by scenes per frame:

  | scenes a frame | shader A | shader B |
  |---|---|---|
  | 1 | 5.97 ms | 3.15 ms |
  | 2 | 8.03 ms | 4.38 ms |
  | 3 | 9.78 ms | 5.54 ms |

  A scene costs 3.2 ms with B, a further one 1.2 ms.
- **The light is right in shape.** Round bands again on the Vita, and
  the once-a-second frame is gone. In the dumped comparison at the test
  spot 95.1% of the pixels are identical to the CPU's (81% in G3);
  `gpudiff` standing still is 4.7% (16.6% in G3). What differs is the
  same texel one light level off, along the edges of the bands.
- **But the bands' edges are coarser than the CPU's.** Inside a slab
  the GPU still interpolates over two triangles, so an edge moves in
  steps of the slab's 8 pixels where the mappers move it pixel by
  pixel.
- **Slabs cost the CPU.** 499 slabs a frame at the test spot, and
  sending went from 0.96 to 3.6 ms. Nearly everything is lit:
  `gpukinds=flat:1,plain:0,clut:2,lit:65`.
- **One reason for every CPU-drawn call**: `gpuwhy=...fill:9.0`, and
  over the whole run 7.4 a frame, all `fill`. No translucent bitmap,
  shaded or translucent polygon came up. Those calls are texture maps
  under `FILL_CLUT`, which the game sets for lit 3D objects
  (`gameobj.c`, `interp.c`). Under that fill type every mapper is its
  colour-table one with the fill's table (`fl8ft.c`), so they are calls
  the GPU path already knows how to draw.
- `check=0/360`, `leaks=0`; `gpufallbacks=69` is the paneled view.

### Go for G5

- **Light per pixel in place of slabs.** A mapper's light at a pixel is
  the value on the left edge at that row, the value on the right edge,
  and how far along the row the pixel is, divided by the row's depth
  for the floor and wall mappers. Left value, right minus left, distance
  from the left edge, row width and row depth are each a linear function
  of the screen position between two corner levels of the polygon, and a
  GPU interpolates those exactly. The shader then computes
  `(left + (right - left) * distance / width) / depth`. A polygon is
  cut only at its corners' levels, a few pieces in place of its slabs.
- **`FILL_CLUT` on the GPU**: one scene a frame.
- **Shader A goes.**

## Step G5: light per pixel, one scene a frame

Profile builds only.

### Light per pixel

What the queue hands the GPU for a vertex's table row is no longer a
row but five values (`rastq_gpu_vertex` in
`src/Libraries/3D/Source/rastq.h`), each a linear function of the screen
position, and the row is

    (left + span * along / width) / depth

- `left`: the mapper's value where its line (row, column, line of one
  depth) meets the polygon's left edge;
- `span`: what the right edge has more;
- `along`, `width`: the pixel's distance from the left edge along the
  line, and the line's length inside the polygon;
- `depth`: the w the floor and wall mappers divide by; 1 for the others,
  which step the level itself.

For the floor and wall mappers `left` and `span` are the level times w,
as those mappers step it along the edges. One row for a whole polygon is
(row + 0.5, 0, 0, 1, 1).

A lit polygon is cut at the level of each of its corners
(`gpu_emit_cut` in `rastq.c`). A piece then lies between two of the
mapper's lines with one edge on either side, and on each of its two
lines `gpu_light_piece` gives every vertex the value at the line's first
end, the difference to its other end, the distance between the two and
the vertex's own distance from the first. All four are linear over the
piece, so the GPU interpolates them exactly whatever triangles it draws,
and they survive the cuts at a texture's repeats unchanged.

The shader (`src/MacSrc/VitaGpu.c`) gets the five multiplied by the
vertex's w. The hardware's perspective interpolation then yields each
of them times the pixel's w, which cancels in the quotients. Per pixel
it is two divisions more than G4's shader B.

A lit triangle stays whole: its level (times w, for floors and walls)
and its w, each linear, already give the mapper's quotient.

### The colour-table fill

Under `FILL_CLUT` the 2D library's table (`fl8ft.c`, "clut fill type")
has, for every texture mapper, its colour-table variant, and `h_umap`,
`v_umap` and `per_umap` take the table from the fill: the call's light
level and its own table count for nothing. `gpu_classify` now takes such
calls as colour-table ones with the fill's table. They were G4's only
reason for a second and third scene.

### One shader

Shader A is gone. The textured shader is G4's B with the light formula;
the flat-colour pair stays as the fallback when the textured one doesn't
compile.

### Checked on the PC

The stand-in computes the row with the same formula, and the comparison
runs twice: light per pixel, and light in slabs as in G4 (kept for one
more capture: `rastq_test_gpu_slabs`, variant 1 on the Vita). Over 900
frames each:

| | light per pixel | light in slabs |
|---|---|---|
| pieces lit polygons are cut into | 5,781 | 58,887 |
| pixels that differ from the mappers' | 2.52% | 2.71% |
| and have no match within two pixels | 0.099% | 0.211% |
| limit on the second, per run | 0.15% | 0.5% |

Light per pixel leaves less to chance than G3's scenes whose light fit
one plane (0.16%), with a tenth of the slabs' pieces.

Deliberate mistakes, with the second figure for light per pixel
(60 frames; 0.086% without any):

| mistake | no match within two pixels |
|---|---|
| left and right values swapped | 6.4% |
| the distance not taken from the left edge | 6.7% |
| both lines of a piece given the first one's values | 5.9% |
| the width doubled | 5.0% |
| columns taken as rows | 3.1% |
| the depth not applied | 2.8% |
| a cut vertex's light not interpolated | 0.81% |
| not cut at the corners' levels | 0.43% |
| the fill's colour table ignored / the light kept under that fill | 7.5% / 7.3% |
| repeats not cut, or v not moved | 0.67% |
| the two ways of carrying light swapped | 0.82% |
| no culling, colour table ignored, transparency dropped, no perspective, light one row up | 6.9% to 25% |
| lines of one depth taken the wrong way | 0.20% |
| the floor and wall mappers' depth not used | 0.18% |
| the dispatcher not asked | 0.094%, **under the limit** |

The test fails on all but the last, which concerns too few polygons to
move the figure.

### What the capture will show

Variants, 5 s each, both GPU ones shown from the canvases: 0 = three
cores, 1 = GPU with light in slabs, 2 = GPU with light per pixel.

- `gpu.txt`: `textures=on texture_check plain=0 table=0 light=0 wrap=0`:
  the light band now goes through the formula, the wrap band through a
  depth;
- `gpuscenes=` should be 1 at the test spot, `gpuwhy=` all zeros there;
- `gpuwait=` in variant 2 against variant 1: what the two divisions per
  pixel cost; `gpusubmit=` and `gpuslabs=`: what not cutting slabs
  saves;
- `gpudiff=`: 4.7% in G4.

### Results (`docs/profiles-gpu/profile-step-5.txt`, `gpu.txt`, `gpudumps-step-5/`)

Standing still in the first area, medians of the 1 s windows:

| ms per frame | 0: three cores | 1: GPU, light in slabs | 2: GPU, light per pixel |
|---|---|---|---|
| pixel filling (`raster`) | 9.51 | 8.36 | 5.44 |
| of which waiting for the GPU | | 3.80 | 3.38 |
| of which sending (`gpusubmit`) | | 4.20 | 1.70 |
| 3D pass (`traverse`) | 12.36 | 11.01 | 8.11 |
| `sendview` | 3.23 | 2.33 | 2.31 |
| help scan | 3.55 | 3.54 | 3.52 |
| `present` | 3.22 | 1.05 | 2.32 |
| of which waiting for the screen | 0.09 | 0.07 | 1.35 |
| whole frame | 22.42 | 17.57 | 15.95 |
| fps | 44.6 | 56.9 | 62.7 |

While walking: 36.3 fps on three cores, 61.5 with slabs, 62.5 with light
per pixel.

- **Variant 2 is at the screen's rate**, with 1.35 ms of each frame
  spent waiting for it: its work is 14.6 ms a frame, against 22.4 on
  three cores.
- **Shadow edges are as fine as the CPU's** on the Vita.
- **Light per pixel costs the GPU less than slabs did**: 3.38 ms against
  3.80. The two divisions a pixel weigh less than the triangles of 499
  slabs; the polygons now go over in 76 pieces, and sending takes
  1.70 ms against 4.20.
- **One scene a frame**, and nothing drawn by the CPU: `gpuwhy=` is all
  zeros at the test spot, and over the walk too but for a translucent
  polygon now and then. The colour-table fills are 9 of the
  `clut:11` calls.
- **The shader compiles and its start-up patterns are exact**
  (`texture_check plain=0 table=0 light=0 wrap=0`).
- **The picture**: 96.2% of the pixels identical to the CPU's in the
  dumped comparison at the test spot. What differs is thin lines where a
  texel boundary or a band's edge falls one pixel to the side. `gpudiff`
  is 4.3% standing still, both GPU variants taken together.
- `check=0/288`, `leaks=0`; `gpufallbacks=65` is the paneled view.

Where the 14.6 ms of a GPU frame go now: the 3D pass without the GPU
about 2.7 ms, sending 1.7 ms, the GPU 3.4 ms, stars, HUD and cursor
2.3 ms, the help scan 3.5 ms, presenting 1 ms, the game itself 0.7 ms.

### Go for G6

The GPU path draws the game's picture, as fast as the screen shows it
where it was measured. What is left is making it the game's renderer
and not a variant of the profile build:

- the slab path goes (light per pixel is better on every count);
- the GPU code into the normal build, behind a setting in Vita Options,
  with the CPU renderers as the other choice and as the fallback;
- the shaders shipped compiled, so that `libshacccg.suprx` is no longer
  needed;
- the paneled (non-full-screen) view, whose 804-pixel-wide canvas the
  GPU module refuses today;
- a frame without a 3D view after frames shown from a GPU canvas (pause,
  full-screen map): the screen buffer doesn't hold the last view;
- more of the game than the first area: the kinds of call that still
  fall to the CPU elsewhere (translucent and shaded polygons, lines,
  voxels), and cyberspace.

## Step G6: the GPU renderer in the game

In every Vita build from here on.

### The Renderer setting

Vita Options' "Multicore" button is now "Renderer", with three choices
(`gShockPrefs.renderer`, `vita-renderer` in the prefs file; an older
file's `vita-multicore = 0` is read as one core):

| choice | what draws a 3D view |
|---|---|
| GPU (default) | the queue records the pass and the GPU draws it; three cores for the views the GPU isn't given |
| 3 cores | the queue records the pass and three cores fill it (docs/PERFORMANCE-CPU.md) |
| 1 core | the original drawing, call by call |

`VitaApplyRenderer` (`src/MacSrc/Prefs.c`) sets the queue's mode, its
threads and `rastq_use_gpu`. When the GPU path couldn't be set up
(`vgpu_available`: no compiler module, or a shader that doesn't
compile), the button offers the two CPU choices and a "GPU" setting
behaves as "3 cores".

The profile build ignores the setting and alternates, 5 s each:
0 = three cores, 1 = GPU. The periodic comparison, its frame dumps and
the timings are the profile build's only; `gpu.txt` is written anew at
each start by both.

The slab path of G4 is gone (`gpu_emit_cut` cuts at the corners' levels
only), and so is drawing in flat colours when the textured shader
fails: without it the GPU isn't used.

### The paneled view

The view of the paneled screen is 804 x 293, in a canvas of its own
whose rows are 804 bytes apart, which the GPU module refused. Now:

- `fr_start_view` gives the draw canvas a GPU canvas's pixels and its
  row length (960), and puts the view's own back for a CPU frame. The
  view draws into the canvas's top left;
- `vgpu_begin` takes any view up to the canvas's size, with the
  canvas-sized render target and colour surface. One render target
  serves every view;
- `fr_send_view` copies the finished view to the screen buffer as
  before (`Fast_Slot_Copy`): the game draws on the screen over this view
  (the help overlay, messages), so the screen buffer has to hold it. The
  copy reads GPU memory.

### Where the GPU is not used

- **Cyberspace** (`_frp.faces.cyber`): its walls are flat and shaded
  polygons and its objects wireframes; shaded polygons and lines are the
  CPU's in the GPU path, drawn into GPU memory between two scenes. It
  keeps the three-core renderer. In the profile build the queue counts
  what the GPU could draw of such a view and why not the rest
  (`rastq_gpu_survey`, into `gpukinds=` and `gpuwhy=`).
- **Low resolution** (`DoubleSize`): the view is doubled out of its
  canvas, which reads all of it.
- Views of fewer than 272 rows (`rastq_gpu_next` now takes the view's
  rows), the help scan, the security cameras, the 360 view: as before.

### The screen buffer when the view stops being drawn

A full-screen GPU view is shown from its canvas and not copied to the
screen buffer. Three rules keep the two in step (`src/MacSrc/Shock.c`):

- `SDLDraw` shows the view's canvas for a frame that drew one, and goes
  on showing the last one while nothing says the screen buffer is to be
  shown (a frame without a view, a palette fade);
- `VitaShowView` refuses while the game is paused, and whenever the
  canvas isn't the full screen: `fr_send_view` then copies the view to
  the screen buffer as on the CPU, and the screen buffer is shown. That
  is how the options panel gets the view under it (`wrapper_start`
  pauses, then renders);
- `VitaSyncView` copies the last shown canvas into the screen buffer,
  once. It is called before the game draws on the screen without having
  drawn the view: at the top of `game_loop`'s paused branch, in
  `loopmode_switch`, before a video mail plays, before the help overlay
  and the wait cursor.

The cursor needs nothing: `rend_mouse_hide` saves what is under it from
the canvas before drawing it there, so the cursor code's save-under is
the view without the cursor, in step with the canvas.

A place that draws on the screen without one of these would show as a
picture that stays frozen on the last view.

### Checked on the PC

The stand-in comparison has a fourth canvas, 804 x 293 with rows 960
bytes apart, for the paneled view. Over 1,200 frames: 2.42% of pixels
differ from the mappers', 0.093% without a match within two pixels
(limit 0.15%). `tests/rastq/run.sh` passes; both Vita builds are free of
warnings.

### Results (`docs/profiles-gpu/profile-step-6.txt`, `gpu.txt`, `gpudumps-step-6/`)

Played on the Vita with the normal build: the three renderers switched
in game, the full-screen and the paneled view, pause, the options panel,
a video mail, cyberspace. No glitch and no frozen picture.

The capture (profile build, full-screen view throughout), medians of the
1 s windows:

| | 0: three cores | 1: GPU |
|---|---|---|
| standing still | 44.6 fps (22.4 ms) | 62.7 fps (15.96 ms, of which 1.3 waiting for the screen) |
| walking and fighting | 35.9 fps | 62.9 fps |
| cyberspace (three cores in both) | 62.5 fps | 61.6 fps |

- One scene a frame everywhere outside cyberspace, nothing drawn by the
  CPU; `check=0`, `leaks=0`; `texture_check` all zeros.
- The paneled view wasn't in the capture: its copy is still unmeasured.
- Dumped comparisons: 96.4% of pixels identical at the test spot; 79 to
  86% facing a wall from close, where the light changes slowly across
  the wall and a band's edge a few hundredths of a level off moves far.
  The pictures look the same.

What the capture explains:

- **Drops to about 50 fps at some angles.** 9 of the 255 GPU windows are
  under 57 fps without a stall. At the test spot a GPU frame has 2 ms to
  spare, and four things grow:

  | | usual | in the slow windows |
  |---|---|---|
  | help scan | 3.5 ms | 6 to 7 ms, once 15.6 ms |
  | sending to the GPU (`gpusubmit`) | 1.7 ms | up to 5.0 ms (135 polygons in 362 pieces) |
  | stars, HUD and cursor (`sendview`) | 2.3 ms | 4 to 5 ms |
  | waiting for the GPU | 3.4 ms | up to 5.8 ms |

  The help scan's case is the door seen from close: after its render,
  `olh_scan_objs` goes over its 58,000 pixels and calls `olh_candidate`
  for every one that is on an object, a distance computation each time,
  for the same object again and again.
- **2-second freezes in the profile build, three in a row**: the frame
  dumps. Dumps 02 to 04 were written within 15 seconds, each a new
  record of differing pixels on entering a room.
- **An fps counter above 60**: the counter is 1000 / average frame time.
  Three 1 s windows, each the first of a GPU phase, are at 66.8, 68.1
  and 108.3: frames that the screen didn't hold back (it holds a frame
  back only when two finished ones are waiting). Why a whole second can
  pass like that is not understood.
- **Stutters when fighting**: eight stalls of 130 to 260 ms, all in the
  input phase, in the three-core windows as in the GPU ones. Not the
  renderer. Unmeasured suspects: `snd_sample_play` decodes and converts
  each sound effect the first time it is played (`Mix_LoadWAV_RW`, on
  the main thread; the result is kept), and resource loads from the
  memory card.

One thing wrong in the queue: in cyberspace `fr_start_view` keeps the
view off the GPU, but the queue takes any large view for the GPU's while
the GPU renderer is on. On every flush it prepared the list, was refused
the canvas and fell back (`gpufallbacks` from 28 to 55,618 over the
cyberspace part). The picture is right and cyberspace runs at the
screen's rate, but the work is wasted, and the count of what the GPU
could draw of cyberspace (`rastq_gpu_survey`) never ran.

### Go for G7

- The help scan asks `olh_candidate` once per object and scan.
- `fr_start_view` tells the queue whether a view is the GPU's.
- Frame dumps no closer than 30 seconds.
- Measurements, in the profile build, of what else eats the 2 ms: the
  sending split into its parts, the wait into issuing the draws and the
  GPU working, `sendview` into stars, HUD and cursor, the help scan into
  its render and its look at the pixels; and of sound and resource
  loading, for the stalls.

Cyberspace on the GPU is dropped from the roadmap: it is at the screen's
rate on three cores.

## Step G7: holding the screen's rate

Built and run on a Vita twice. Results at the end of this section.

### Fixes

- **The help scan asks once per object** (`olh_scan_objs` in
  `src/GameSrc/olhscan.c`). It keeps `olh_candidate`'s answer for each
  object of a scan, by the object's colour in the scan's picture, where
  it asked again for every pixel on the object. The answer depends on
  the object alone, so the scan finds what it found before. In all
  builds and for every renderer.
- **The queue is told whether a view is the GPU's**
  (`rastq_gpu_view`, called by `fr_start_view` for every view). The
  queue used to take any large view for the GPU's while the GPU
  renderer was on, and to be refused the canvas when `fr_start_view`
  had kept the view for the CPU (cyberspace, low resolution). Such a
  view is now replayed on three cores without the attempt, and the
  profile build's count of what the GPU could draw of it runs.
- **Frame dumps at least 30 seconds apart** (`vgpu_compared`), so that
  walking into a room doesn't freeze the profile build three times.

### New in the log (profile build)

| field | what |
|---|---|
| `gpuprepare=` | of the sending: deciding what each call is, and the tables (ms a frame) |
| `gpusubmit=` | the rest of the sending: cutting calls up and handing them over |
| `gpuupload=` | of `gpusubmit`: copying bitmaps into GPU memory |
| `gpudraw=`, `gpudraws=` | of `gpuwait`: issuing the scene's draws, and how many |
| `helprend=` | of `helpscan`: its render; the rest is its look at the pixels |
| `stars=`, `hud=`, `viewout=` | of `sendview`: the stars, the overlays drawn into the view, and the cursor with the view's way to the screen |
| `sndload=` | a sound effect decoded for its first use (`snd_sample_play`) |
| `resload=` | a resource read from the card (`ResLoadResource`) |

Each is the average per frame and, after the slash where there is one,
the most in one frame of the window.

### Results (`docs/profiles-gpu/profile-step-7.txt`, `profile-step-7b.txt`, `gpu.txt`, `gpudumps-step-7/`, `gpudumps-step-7b/`)

Two captures with the profile build. The first has the door of step 6
and a long stay in cyberspace. The second has what the first lacked: a
long stay at an angle that costs more, and the paneled view with the
help labels on. They agree where they overlap. The table is the second
one, medians of the 1 s windows, cut by the log's `mode=` (1 full
screen, 0 paneled) and the check numbers noted while playing:

| | 0: three cores | 1: GPU | GPU: waiting for the screen |
|---|---|---|---|
| standing still | 46.4 fps (21.6 ms) | 62.1 fps | 2.0 ms |
| moving | 34.5 fps | 62.1 fps | 1.0 ms |
| the heavy angle | 32.1 fps | 58.5 fps (17.1 ms) | none: 0.5 ms too much |
| paneled view, help labels on | 53.9 fps | 61.7 fps | 0.1 ms |
| paneled view, no help scan | 60.1 fps | 62.7 fps | 5.2 ms |
| cyberspace (three cores in both) | 62.4 fps | 62.6 fps | 4.6 ms |

- **The door of step 6** is at 60 to 62 fps (first capture): the help
  scan no longer asks the same question for every pixel. Its most in one
  frame is still 17 to 19 ms, once a capture, and that frame has a
  resource load inside the scan's render.
- **Cyberspace**: `gpufallbacks=0`, the queue no longer tries. The count
  of what the GPU could draw of it ran: 43 flat polygons a frame, and
  2.5 of a kind it doesn't draw.
- **The stalls** are measured now. 10 in the second capture: 8 have a
  sound effect decoded for its first use in them (`sndload` 124 to
  237 ms in one frame), one a resource load alone (54 ms), one is the
  load of the cyberspace level (1.9 s). In the three-core windows as in
  the GPU ones. Not the renderer, and not this branch's work: noted in
  `docs/TODO.md`.
- **Dumps**: 4 and 3, none closer than 30 seconds, no freeze. 6.5% and
  5.9% of the compared pixels differ. `check=0`, `leaks=0`,
  `texture_check` all zeros.
- **The paneled view** is in both captures. The first has it without the
  help scan, which had stopped (help off, an object on the cursor or a
  panel open: the log doesn't say which).

Where a GPU frame goes, in ms a frame:

| | standing still | the heavy angle | paneled, labels on |
|---|---|---|---|
| waiting for the GPU (`gpuwait`) | 3.4 | 3.5 | 2.2 |
| help scan (`helpscan`), of which its render | 2.9, 1.2 | 3.6, 1.7 | 2.3, 1.4 |
| HUD drawn over the view (`hud`) | 2.2 | 4.5 | 2.3 |
| sending to the GPU (`gpuprepare` + `gpusubmit`) | 1.8 | 2.2 | 1.8 |
| the view's way to the screen (`viewout`) | 0.1 | 0.1 | 2.9 |
| polygons | 77 | 48 | 45 |
| whole frame, without the wait for the screen | 14.1 | 17.1 | 16.1 |

- **The heavy angle isn't heavy for the GPU.** It has fewer polygons
  than the test spot, and the GPU takes the same time. What grows is on
  the CPU, and it grows the same on three cores: the HUD by 2.3 ms, the
  help scan by 0.7 ms, the sending by 0.4 ms.
- **The HUD moves in steps**: 2.2, 2.7 and 4.5 ms in full screen, 0.5
  and 2.2 ms in the paneled view. One step is 1.75 ms in both views, and
  in the paneled view it comes and goes with the help scan. A lead, not
  measured: the help label's text. `draw_shadowed_string` draws a string
  nine times, eight shifted copies for the outline and then the text.
  The rest of the full-screen HUD is `fullscreen_overlay`, which redraws
  the button panels, the two side panels, the inventory, the vitals and
  the icons over the view every frame.
- **The paneled view on the GPU** pays 2.9 ms to copy the view out of
  GPU memory onto the screen (0.4 ms from ordinary memory on three
  cores). Without the help scan it has 5 ms to spare; with it, none.
- **The wait is the largest fixed part**, and the main thread does
  nothing during it. `gpudraw` is 0.1 ms of it: the rest is the GPU
  working.

### Go for G8

- The queue hands a scene to the GPU without waiting for it; a separate
  call waits. It still waits at once where the CPU draws on the canvas
  within the frame.
- `fr_send_view` waits before the HUD, and before that runs the help
  scan when the view is the GPU's: the scan never touches the picture,
  and it takes about as long as the GPU does. A view that shows stars
  waits first, as today: they read the picture.
- The game loop skips its own scan when the render ran it.
- The HUD measured part by part (profile build): hand, help label, other
  texts, button panels, side panels, inventory, vitals, icons.

Expected, not measured: the heavy angle from 17.1 ms to about 14, the
paneled view with labels from 16.1 to about 14. How much of the wait
needs the CPU is not known.

Left out: the stalls (their own task, later); the paneled view's copy
(G8 gives it the time).

## Step G8: use the GPU's time

Built and run on a Vita. Results at the end of this section.

### The queue hands the last scene over and doesn't wait

`rastq_gpu` has a `finish` next to `end`: `end` gives the scene to the
GPU (`sceGxmEndScene`), `finish` returns once the canvas holds it
(`sceGxmFinish`). The queue still waits at once wherever the CPU touches
the canvas within the view: before a call left to the CPU, between two
scenes, in the profile build's comparison, and after a flush in the
middle of a view (voxels and direct calls are drawn straight after it).
Only the last scene of `rastq_end` is left to the GPU. Until
`rastq_gpu_finish` the canvas is the GPU's; `rastq_gpu_busy` says so.

### The view scans while the GPU draws

`fr_send_view`, for a view the GPU is still drawing:

1. If the view shows a star field (`star_field_seen`, which is
   `star_render`'s own test), it waits first: the stars go into the
   pixels the field left, so they read the picture.
2. Otherwise the stars' block draws nothing and closes the view's 3D
   frame, as it always did.
3. It runs the help scan (`olh_scan_in_render`), a render of its own
   into a canvas of its own, on the three cores.
4. It waits for the GPU (`rastq_gpu_finish`), then draws the HUD and the
   cursor and sends the view, as before.

The scan is the game loop's: `game_loop` marks its `render_run` as the
one the scan may run in, `olh_scan_in_render` runs `olh_scan_objects`
under the loop's own conditions, and the loop skips its scan if the
render ran it. Other renders (video mail, the wait cursor) never scan,
as before. With the CPU renderers, or stars in view, the scan stays
where it was.

What the game sees: the same scan, a game step earlier (before
`physics_run` in place of after), so the label is the one of the picture
it is drawn on and not of the frame before.

A second render between a view's render and its HUD changes four things
the HUD and the send use, and `fr_send_view` puts them back: `_fr`,
`_fr_curflags`, the current canvas, and `current_num_hudobjs`. The last
one is a trap: the scan's render stores the rectangles of HUD objects
(the target box, the beam's end) in its own small coordinates
(`SET_HUDOBJ_RECT`). The next frame used to clear them before anything
read them; run before the HUD, they would give a second, misplaced
target box. The HUD's path makes no 3D calls, so the scan's 3D frame
changes nothing for it.

### Checked on the PC

The test's stand-in GPU now draws aside and copies its picture to the
canvas only at `finish`, so a canvas read or written before the wait
shows as a wrong picture. `draw_scene` does what `fr_send_view` does:
when the GPU still has the view's last scene, the CPU renders the same
calls as a second view into another canvas, which must come out as the
mappers draw it and leave the GPU's scene out, and only then waits. The
comparison's GPU side must be the finished picture.

`tests/rastq/run.sh 300`: passes; the GPU is left its last scene in 682
of 1,200 frames (the others end on a call the CPU draws). Each of these,
put into `rastq.c` on purpose, fails the test: no wait before a call
left to the CPU, none after a flush in mid-view, none in the comparison,
and waiting at the end after all.

Not checkable on the PC: the game's side (the scan inside the render,
the HUD after it), and how much of the GPU's time the CPU really gets.

### New in the log (profile build)

| field | what |
|---|---|
| `gpuwait=` | now: issuing the scene's draws, and the time the main thread is really blocked on the GPU |
| `gpuoverlap=` | the time between handing the last scene over and starting to wait for it: what the CPU did meanwhile |
| `hudparts=` | of `hud`: `hand` the weapon in hand, `label` the help label, `text` compass and messages, and in full screen `buttons` the two button panels, `mfd` the two side panels, `inv` the inventory, `vitals` vitals and meters, `icons` the side icons |

`render3d` and `sendview` now contain the help scan when it runs in the
render; `helpscan` is still its own time.

### What the capture will show

- `gpuoverlap` about the help scan's time and `gpuwait` near zero with
  help on: the wait is hidden. If `gpuwait` stays at 3 ms, the GPU needs
  a core the scan is using and the gain is smaller.
- The heavy angle and the paneled view with labels at the screen's rate
  (`swapwait` above 2 ms).
- `hudparts`: whether `label` is the 1.75 ms step, and which part of the
  full-screen HUD is the rest.
- To look at in the normal build: the help labels follow objects as
  before; one target box, in its place; a window onto space; pause,
  video mail, the paneled view.

### Results (`docs/profiles-gpu/profile-step-8.txt`, `gpu.txt`, `gpudumps-step-8/`)

Medians of the 1 s windows, cut by the check numbers noted while
playing:

| | 0: three cores | 1: GPU | GPU: waiting for the screen |
|---|---|---|---|
| standing still | 46.2 fps | 63.3 fps | 4.7 ms (2.0 in G7) |
| moving | 33.1 fps | 62.8 fps | 3.2 ms |
| G7's heavy angle, labels on | 31.5 fps | 63.3 fps (58.5 in G7) | 1.8 ms |
| the same, paneled | 51.0 fps | 63.1 fps | 1.8 ms (0.1 in G7) |
| the hallway angle, full screen, no scan | 37.0 fps | 55.4 fps (18.0 ms) | none |
| the same, paneled | 58.3 fps | 57.0 fps (17.6 ms) | none |
| the hallway angle again, help off | 35.1 fps | 53.2 fps (18.8 ms) | none |
| turning on the spot, help on | 40.7 fps | 62.8 fps | 3.3 ms |
| the same spot, help off | 48.9 fps | 62.7 fps | 5.4 ms |
| cyberspace (three cores in both) | 62.8 fps | 62.6 fps | 3.4 ms |

- **The wait is hidden where the scan runs.** `gpuwait` is 0.2 to
  0.45 ms (3.4 in G7) and `gpuoverlap` 2.4 to 3.8 ms, the scan's time:
  the GPU doesn't need the cores the scan uses. No glitch was reported
  from the play-through.
- **Without the scan nothing changes**: `gpuwait` 3.2 to 3.7 ms,
  `gpuoverlap` 0.02 ms.
- `check=0`, `leaks=0`, `gpufallbacks=0`, 6.2% of the compared pixels
  differ, 3 dumps.
- **Stalls**: 14. Seven have a sound effect's first decoding in them,
  three a resource load alone, two are the cyberspace level loading
  (2.5 s in, 7.7 s out), and two (129 and 67 ms) show nothing in the
  timers there are.

The HUD's parts (`hudparts`, ms a frame, the same on three cores):

| | full screen | paneled |
|---|---|---|
| help label, when one shows | 1.6 to 1.9 | 1.75 |
| inventory | 1.2 | |
| vitals and meters | 0.9 | |
| a side panel, when one is open | 0.6 | |
| weapon in hand | 0.4 | 0.4 |
| messages, when there is one | 1.0 | |
| button panels, side icons | 0.1 | |

The label is G7's 1.75 ms step: `draw_shadowed_string`'s nine draws.

**The hallway angle** (past the large door of the first rooms, looking
down the main hallway) is a different case from G7's:

| ms a frame, GPU renderer | standing still | the hallway angle |
|---|---|---|
| polygons, and the pieces they are cut into | 77, 76 | 132 to 139, 354 to 365 |
| sending (`gpuprepare` + `gpusubmit`) | 1.8 | 5.0 |
| blocked on the GPU (`gpuwait`) | 0.45 | 3.7 |
| help scan | 2.9 | none |
| HUD | 2.2 | 3.3 |
| whole frame, without the wait for the screen | 11.1 | 18.0 to 18.8 |

- It is heavy for the sending: lit polygons there are cut into 2.7
  pieces each, on one core, while the two worker cores do nothing.
- The wait has nothing to hide behind: the scan doesn't run there.
- In the paneled view the GPU is slower than three cores there (57.0
  against 58.3 fps): the sending, plus the 2.9 ms copy of the view out
  of GPU memory.

**Why the scan doesn't run there**: not the port. The level has a
trigger past that door that switches on-screen help off and saves the
option as off (`trap_questbit_func` in `src/GameSrc/trigger.c`, quest
bit 0x2091, "special hack for auto shutoff of on-line help"); a new
game switches it on again. That fits both G7 captures, where the scan
also ended for good while leaving the first rooms. Kept as the original
has it.

### Go for G9

A cheaper HUD, for every renderer:

- the help label drawn into a small bitmap when its text changes and
  copied each frame, in place of nine string draws a frame;
- the copies of the inventory, the vitals and the side panels over the
  view: large bitmaps, mostly see-through. How much there is to save is
  to be read from the code first.

Expected, not measured: 1.5 ms off a full-screen frame, 3 ms with a
label. The hallway angle would be at 16.5 to 17 ms, near the screen's
rate and not safely at it.

Then G10: the sending shared between the three cores (deciding what
each call is and cutting it up, the hand-over staying in order), about
2 ms off the frames with many pieces.

Left out: the wait when the scan doesn't run (nothing else in the frame
is free to run in that time); the paneled view's copy; the stalls.

## Step G9: a cheaper HUD

Built and run on a Vita. Results at the end of this section.

The HUD is redrawn identically frame after frame, on every renderer.
Three changes make that drawing cheaper without changing a pixel of it.

### Outlined text is drawn once and kept

`draw_shadowed_string` (`src/GameSrc/tools.c`) draws a string nine
times: eight copies in the outline's colour around its place, then the
text. `hudkeep_outlined` (`src/GameSrc/hudkeep.c`) makes those nine
draws, with the same calls, into a canvas of its own, keeps the part
they cover and copies it each frame, transparent where nothing was
drawn. It covers the help label and the HUD's messages, which both end
in that function.

- A text is known by its string, font, colours and screen mode. Eight
  are kept; the one unused the longest goes.
- A text is made into a picture the second time it is asked for, so one
  that changes every frame (a counter) is drawn as before and costs
  nothing more.
- Drawn as before whenever the copy couldn't be the same: a colour that
  is palette index 0 (the picture's "nothing here"), a text that
  reaches the edge of the 1024x128 canvas it is made in, a fill type
  other than the normal one.

### Small stretched bitmaps are kept

`ss_bitmap` stretches a 320x200 bitmap to the screen's size at every
call. The vitals are up to 46 arrows of a few pixels and two icons a
frame, each a stretch of its own; the inventory's page buttons are a
292x10 bitmap stretched to 876x27 every frame. `ss_kept_bitmap`
(`src/GameSrc/gr2ss.c`, `hudkeep_scaled`) keeps the stretched result
and copies it.

- A bitmap is known by its pixels, compared byte for byte at every
  call, not by its address: one that changed is stretched again.
- Only uncompressed 8-bit bitmaps of at most 4,096 pixels, stretched to
  at most 32,768, landing whole inside the clip rectangle. Anything else
  is stretched as before (the vitals' icons, if they are compressed).
- It rests on the stretch giving the same pixels wherever the bitmap
  goes, which the PC test checks.

Asked for where the game draws the same small bitmap frame after frame:
`draw_status_arrow` and the icons in `src/GameSrc/vitals.c`, the page
buttons in `inv_update_fullscreen`. `ss_bitmap` itself is unchanged.

### Transparent copies go four pixels at a time

`flat8_flat8_ubitmap` (`src/Libraries/2D/Source/Flat8/fl8fl8.c`) copied
a transparent bitmap one pixel at a time, testing each. It now reads
four: all transparent, it skips them; none transparent, it copies them;
otherwise one by one as before. A bitmap copied onto itself goes the
old way. This is the copy of the inventory and the side panels over the
view, and of the kept pictures above.

### Checked on the PC

`tests/rastq/rastq_test.c` (`test_overlays`), in `tests/rastq/run.sh`:

- 4,000 transparent copies of random bitmaps, at random places and
  partly off the canvas, against one pixel at a time: none differs. A
  wrong test of the four pixels, put in on purpose, fails 3,230 of them.
- 2,000 random bitmaps stretched at random places against the same
  bitmap stretched once at the corner and copied: none differs.

Not checkable on the PC: the game's texts and bitmaps themselves. The
profile build checks them as it runs.

### New in the log (profile build)

| field | what |
|---|---|
| `hudkept=text:a/b,scaled:c/d` | per frame: outlined texts copied / drawn, stretched bitmaps copied / stretched |
| `hudcheck=bad/runs` | kept pictures also drawn the old way and compared (one text in 64, one bitmap in 2,039), and how many differed. `bad` must stay 0. A checked frame is left out of the timings |

### The build

`vita/segment-gap.ld`, passed to the linker from `CMakeLists.txt`: the
profile package stopped building with "Cannot allocate 4584 bytes for
SCE data at end of segment 0; segment 1 overlaps". `vita-elf-create`
puts the module's data after the code segment and needs room there; the
default linker script starts the data segment at the next 64 KB
boundary, so the build failed whenever the code happened to end within
4.5 KB of one, which this step's code did. The fragment keeps 8 KB free
after the code. Both packages start on the Vita with it.

### What the capture will show

- `hudparts`: `label` from 1.75 ms to a few tenths, `vitals` and `inv`
  down; `text` when a message shows.
- `hudcheck` with `bad` at 0, and `hudkept` showing the copies are used
  (about one text and fifty bitmaps a frame).
- The full-screen HUD from 3.3 ms to about 2, and from 5 to about 2.2
  with a label. Estimates: how the vitals' 0.9 ms splits between arrows,
  icons and meters isn't measured.
- To look at in the normal build: the outline and the text of labels
  and messages, the health and energy bars as they change, the
  inventory's pages and buttons, a side panel, in both views.

### Results (`docs/profiles-gpu/profile-step-9.txt`, `gpu.txt`, `gpudumps-step-9/`)

Medians of the 1 s windows, cut by the check numbers noted while
playing:

| | 0: three cores | 1: GPU | GPU: waiting for the screen |
|---|---|---|---|
| standing still | 46.8 fps | 63.2 fps | 5.6 ms (4.7 in G8) |
| G7's heavy angle, labels on | 29.8 fps | 63.5 fps | 5.9 ms (1.8 in G8) |
| a fight | 37.7 fps | 63.2 fps | 4.6 ms |
| the hallway angle, full screen | 39.1 fps | 59.0 fps (16.95 ms; 55.4 fps in G8) | none |
| the hallway angle, paneled | 61.4 fps | 55.3 fps (18.1 ms) | none |
| fights, paneled | 61.9 fps | 63.0 fps | 3.4 ms |

The HUD (`hud`, `hudparts`), ms a frame on the GPU renderer; three
cores gain the same:

| | G8 | G9 |
|---|---|---|
| full screen, no label | 2.2 to 3.3 | 1.35 to 1.9 |
| full screen with a help label | 4.4 | 1.5 |
| `label` | 1.76 | 0.20 |
| `inv` | 1.2 | 0.6 |
| `vitals` | 0.9 | 0.65 |
| `hand`, when a weapon is out | 0.4 | 0.4 |

- **`hudcheck=0/438`**: no kept picture differed from the old drawing.
  No glitch was reported from the play-through.
- `hudkept`: one text and 28 to 30 stretched bitmaps copied a frame,
  none made again once the first frames are past.
- **The vitals gained little.** The arrows are the 29 copies; what is
  left is the two icons and the meters, which this step didn't touch.
- `check=0`, `leaks=0`, `gpufallbacks=0`, 6.6% of the compared pixels
  differ, 5 dumps.
- **Stalls**: 24, most of them in the fights. 19 have a sound effect's
  first decoding in them (20 to 190 ms), three a resource load alone,
  two show nothing in the timers there are.

What is left at the hallway angle, GPU renderer, ms a frame:

| | full screen | paneled |
|---|---|---|
| sending (`gpuprepare` + `gpusubmit`), 133 polygons in 354 pieces | 5.1 | 4.8 |
| blocked on the GPU, no scan to run meanwhile | 3.8 | 2.4 |
| the view's way to the screen (`viewout`) | 0.1 | 2.9 |
| HUD | 1.8 | 0.5 |
| whole frame | 16.95 | 18.1 |

In the paneled view three cores draw that angle in 16.3 ms: the GPU
loses there by the 2.9 ms copy of the view out of GPU memory.

### Go for G10

The sending shared between the three cores: deciding what each call is
and cutting it into pieces is done by the main thread alone while the
two worker cores do nothing. The pieces are still handed to the GPU in
the order the game drew them.

Expected, not measured: about 2 ms off the frames with many pieces, the
hallway angle near 14.7 ms in full screen and 15.8 ms paneled. How the
5 ms split between the cutting and the hand-over, which stays on one
core, is not known.

After that, to choose from: the paneled view shown from GPU memory in
place of copied (2.9 ms; everything the game draws over the view on the
screen has to be looked at); the wait when the help scan doesn't run
(2.4 to 3.8 ms); the stalls.
