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
- **Shaders are compiled on a Vita** with Sony's compiler module
  (`ur0:data/libshacccg.suprx`, through vitaShaRK) while the work is under
  way. The compiled programs are shipped at the end, so players don't
  need the module.

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
| G4 | Light interpolated the way the mappers do it; the shader's cost; which calls still go to the CPU and split the frame into scenes | the same picture as the CPU's, and a GPU frame clearly faster than three cores |
| G5 | What needs the picture already drawn: translucent surfaces, shaded polygons, lines, voxels; cyberspace policy | no holes left, or clean CPU fallbacks |
| G6 | A "Renderer" setting in Vita Options, compiled shaders shipped, the paneled view | players need no extra module |

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
