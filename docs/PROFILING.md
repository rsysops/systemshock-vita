# Profiling the game on the Vita

How to measure what the game does on a real console: build the profile
package, take a capture, read it, and add a measurement of your own.

This is the how-to. What was measured with it, and what was done about
it, is in [PERFORMANCE.md](PERFORMANCE.md).

## The profile build

The profile build is the game with timers compiled in. It differs from
the normal build in three ways:

- it draws six lines of figures over the picture;
- it appends one line a second to a log on the memory card, and one
  for each stutter;
- in its second flavour, it checks its own drawing now and then (see
  [What it changes](#what-it-changes-in-the-game)).

It comes in two flavours:

| command | folder | for |
|---|---|---|
| `./build.sh profile` | `build-profile/` | measuring: the game with its timers and nothing else |
| `./build.sh profile checks` | `build-profile-checks/` | work on the drawing code: the same with the self-checks and the frame dumps, which hitch three times a second and so are kept out of the first |

In the normal build every timer compiles to nothing: `VPROF_RUN(phase,
code)` is just `code`. Nothing of the profiler ships in it.

## Quick start

1. **Build.** `./build.sh profile` gives
   `build-profile/systemshock.vpk` (`./build.sh profile checks`:
   `build-profile-checks/systemshock.vpk`). The normal build goes to
   `build/`, so none of them share anything.
2. **Install.** The package has the normal build's title
   (`SHOK00001`), so it replaces it. Saves and settings are in
   `ux0:data/systemshock/` and are kept.
3. **Clear the old capture.** In `ux0:data/systemshock/`, delete
   `profile.txt` and the `gpudumps` folder. The log is appended to,
   never cleared; without this the new capture follows the old one in
   the same file.
4. **Play.** Stay where you want to measure for a minute (see
   [Good practice](#good-practice)). When you move on to something else,
   note the second number of `check=` on the overlay's last line: it
   only goes up, and it is in every log line, so it marks the place in
   the log.
5. **Collect.** Copy from `ux0:data/systemshock/`:

   | file | what |
   |---|---|
   | `profile.txt` | the log: one line per second of play |
   | `gpu.txt` | how the GPU renderer's start-up went; rewritten at every start |
   | `gpudumps/` | pairs of frames the two renderers drew differently, if any |

Captures are kept in the repository next to the doc that uses them:
`docs/profiles-cpu/`, `docs/profiles-gpu/` and, for the steps after the
GPU renderer's (S1 and on), `docs/profiles-perf/`, as `profile-step-N.txt`,
`gpudumps-step-N/` and the latest `gpu.txt`.

## What it changes in the game

- **It draws as the game does**: on the GPU, or on three cores when
  the GPU can't be used. It can also switch between two ways of doing
  something under test, when `VPROF_ALTERNATE` is set to 1 in
  `src/Libraries/H/vprof.h` (it is 0): at present the GPU's lists
  always shared with the worker threads (`var=0`) against shared only
  while frames are short of time (`var=1`, what the game does), ten
  minutes each. The log says which was on (`var=`). That is what makes
  two ways comparable: same place, same moment, same capture.
- **It writes its log from a thread of its own**, a few times a second.
  The main thread only builds the lines (`logwrite=`); the card is the
  writer's business (`logio=`). A read of the game's that falls on a
  write can still wait for it.
- **With the checks flavour only, it checks pictures.** One 3D view in
  63 is drawn twice, by two paths, and compared (see
  [Self-checks](#self-checks-and-frame-dumps)). Such a frame is slow,
  about 68 ms, so it is left out of the timings (`skipped=`); with the
  HUD's checks that is about three a second. And it stops for about
  four seconds when it writes a frame dump to the card: at most five
  times in a session, 30 seconds apart. To find the game's stutters,
  use the first flavour, or go by the `spike` lines and not by the
  feel (see [Stutters](#stutters)).
- **It costs a little.** A timer is two clock reads; the overlay is
  six lines of text.

## The overlay

```
fps=62.8 frame=15.92/16.74ms music=14%
input=0.2 sim=0.5 render3d=8.1 ui2d=0.0 present=6.9
traverse=5.1 raster=2.4ms record=0.44ms cmds=131 batches=0 wait=0.0ms
mode=1 var=1 age=0s check=0/123 leaks=0 gpuwait=0.4ms gpucpu=0 gpudiff=59.9/1000
workers job=2/2% spin=98/98% sleep=0/0% lists=1.0 shared, 0.0 solo (0 calls)
spikes=3 last=212.4ms t=412 (sndload 205.1)
```

The figures of the first four lines are those of the second under way,
so they jump when it starts again. Line 1 is the frame: rate, average
and longest time, and the share of one core the music takes. Lines 2
and 3 are the phases of a frame, in ms. Line 4 says which screen is up
(`mode`), which path is on (`var`), and how the self-checks are going.
The fields are the log's; only `gpudiff` is given per thousand pixels
here.

Line 5 is the two worker threads in the last whole second: the share
of it each spent in jobs, awake without one, and asleep, and how many
of the GPU's lists per frame went to all the threads or stayed with the
main one (see [The recorder and the threads](#the-recorder-and-the-threads)).
Line 6 counts the session's stutters and gives the last one: how long
the frame was, when, and where most of it went (see
[Stutters](#stutters)).

## The log

`profile.txt` starts each session with two lines from the start-up
checks (`fixdiv_check ...` and `gpu: ready ...`), which also tell
sessions apart in a file that holds several. Then one line a second:

```
t=412 mode=1 var=1 fps=62.1 frame_avg=16.11 frame_max=17.98 input=0.17/0.24 sim=0.53/0.71 ...
```

How a line is made:

- It covers about one second: all the frames that ended in it.
- A time is in **ms per frame**. Where a field has two numbers,
  `a/b`, the first is the average over the second's frames and the
  second the most in one frame.
- A count is **per frame** too, so it is rarely a whole number.
- A few fields run on from the start of the session instead: they are
  marked "total" below.
- A second in which the screen changed (`mode`) is not logged.
- Frames left out of the timings (self-checks) are in none of the
  figures, except `skipped`, which counts them, and the workers' shares
  (`wjob`, `wspin`, `wsleep`), which are of the whole second.

### The frame

| field | what |
|---|---|
| `t` | seconds since the game started |
| `mode` | the screen: 0 game with panels, 1 game in full screen (cyberspace too), 4 menus, 6 cutscene, 8 map (`_current_loop`, `src/GameSrc/Headers/mainloop.h`) |
| `var` | the code path on in this second (`vprof_variant`) |
| `fps` | 1000 / `frame_avg` |
| `frame_avg`, `frame_max` | a whole turn of the main loop, average and longest |
| `input` | reading the controls (`input_chk`, `pump_events`) |
| `sim` | the game's rules: AI, physics, animations, sound and palette upkeep |
| `render3d` | the 3D view and what is drawn into it (`render_run`) |
| `ui2d` | the panels around the view, when there are any |
| `present` | putting the frame on screen (`SDLDraw`), its wait for the screen included |
| `swapwait` | of `present`: waiting to hand the frame to the screen. Time that isn't work |
| `music`, `acpu`, `mcpu` | music synthesis as a share of one core, and the core the audio and the main thread were last seen on |

`frame_avg` minus `swapwait` is what the frame costs. When that is under
16.67 ms the game is at the screen's 60 fps and `swapwait` is the time
to spare.

### The 3D view

These are inside `render3d`.

| field | what |
|---|---|
| `views` | 3D views drawn: the main one, the help's scan, each camera screen |
| `traverse` | from the start of a view to its last drawn call, `raster` included |
| `raster` | filling pixels: the CPU's replay, or all of the GPU's part |
| `calls_per_frame`, `raster_call_avg` | how many times `raster` ran, and its average length |
| `sendview` | after the drawing: `stars`, `hud`, `viewout` |
| `stars` | the stars seen through windows |
| `hud` | what is drawn over the view (see `hudparts`) |
| `viewout` | the cursor, and the view's way to the screen |
| `helpscan` | the on-screen help's search for the object in front of you |
| `helprend` | of `helpscan`: its small render |

### The recorder and the threads

The 3D library's calls are recorded, then drawn all at once
(`src/Libraries/3D/Source/rastq.c`).

| field | what |
|---|---|
| `record` | recording the calls |
| `cmds` | calls recorded |
| `copied` | texture pixels copied with them, KB |
| `flushes` | times the list was drawn; more than `views` means something interrupted it |
| `batches` | runs of calls handed to all the threads |
| `solo=a/b` | calls the main thread drew alone: `a` recorded, `b` not recordable |
| `wait` | the main thread waiting for the workers |
| `busy=a/b/c` | each thread drawing (main, worker 1, worker 2) |
| `late` | the longest a worker took to start in this second, in µs |
| `split=a/b/c` | the rows where the main view's three bands end |
| `wcpu=a/b/c` | the core each thread last drew on |
| `wjob=a/b%` | the share of the second each worker (1, 2) spent in jobs, the self-checks' drawing included |
| `wspin=a/b%` | awake without a job, watching for the next: the core is as busy as in a job |
| `wsleep=a/b%` | asleep |
| `wjobs=a/b` | jobs each worker took part in |
| `wsleeps=a/b` | times each went to sleep in this second (not per frame) |

A worker's core shows as busy for `wjob` + `wspin`. After a job a
worker watches for the next one before it sleeps (25 ms after a job the
main thread didn't wait for, 100 ms after one it did: `rastqthr.c`), so
one short job a frame keeps it awake all the time.

### The GPU renderer

All zero in the seconds the CPU draws.

| field | what |
|---|---|
| `gpuscenes` | GPU passes; 1 is the aim, more means the frame was interrupted |
| `gpupolys` | calls the GPU drew |
| `gpukinds=` | of those: `flat`, `plain` texture, through a colour table (`clut`), `lit`, `shaded` between colours, `line`, `point` |
| `gpupieces` | pieces the lit calls were cut into |
| `gpuculled` | calls dropped for facing away |
| `gpucpu=a/b` | calls of a GPU view the CPU drew, and the time it took |
| `gpuwhy=` | why: `tlucbm` translucent bitmap, `spoly` brightness-shaded polygon, `tlucpoly` translucent polygon, `poly` another kind of polygon, `fill` a fill type it doesn't do, `verts` too many vertices, `light` a light level outside the table, `clip` a clip rectangle, `size` a bitmap size, `other` |
| `gpuprepare` | deciding what each call is, and for a long list cutting it, on all the threads |
| `gpucut=a/b/c` | calls each thread decided and cut, when the list was shared |
| `cutlists=shared:a,solo:b,solocalls:c,rested:d` | lists decided and cut by all the threads (a job for the workers) / by the main thread alone, the calls in the latter, and how many of the latter had 64 calls or more: long enough to share, but frames had time to spare (`rastq_gpu_frame`) |
| `gpusubmit` | handing the pieces to the GPU |
| `gpuupload`, `gputex` | of `gpusubmit`: copying bitmaps into GPU memory, and how many KB |
| `gpuwait` | the main thread blocked until the GPU has drawn |
| `gpudraw`, `gpudraws` | of `gpuwait`: issuing the draws, and how many |
| `gpuoverlap` | time the main thread used for something else while the GPU drew |
| `gpufallbacks` | total: lists the GPU refused, drawn by the CPU instead |

### The HUD and loading

| field | what |
|---|---|
| `hudparts=` | of `hud`: weapon `hand`, help `label`, `text` (compass, messages), and in full screen `buttons`, `mfd` side panels, `inv`entory, `vitals`, `icons` |
| `hudkept=text:a/b,scaled:c/d` | texts and stretched bitmaps copied from a kept picture / drawn again (`src/GameSrc/hudkeep.c`). For a text, drawn again is a picture made of it, or the nine draws where it can't be kept |
| `outlined=a/b` | all the outlined text of a frame: copied from its picture, made into one, or drawn nine times (`draw_shadowed_string`). The HUD's messages and, in full screen, the side panels' text. At the end of the line |
| `sndload` | decoding a sound effect on the main thread: one played before the background thread got to it, or waited for while the thread was on it |
| `resload` | reading a resource: from the card, or from its file's copy in memory once that is there |
| `resimg=a/b` | total: art files read whole into memory, of those wanted (`src/GameSrc/resimage.c`). At the end of the line |
| `resimgmb`, `resimgms` | total: what those copies take up, in MB, and how long their thread has taken to read them, in ms |
| `applog=lines:ms` | the game's own log (`log_log`, warnings and errors): lines written in this second, by any thread, and what they took together. A line goes out to the console's debug output and holds its thread up meanwhile |
| `applogmax=address:ms` | the longest of them and the function it was written from. In a stutter's line the same is a note, `log@address` |
| `at=level:x,y,z@heading` | where the player is when the line is written: the level's number (a `c` after it in cyberspace), the map square across and along with how far into it (`23.50` is the middle of square 23), the height and the heading in degrees, both as the game counts them. `-` outside the game. At the end of the line; a stutter's line has the same, for where the player was at the end of that frame |
| `resmax=from#id:ms` | the slowest resource read of this second: from the card (`res`) or from memory (`resm`), its id (hex) and its time. `res#0:0.00` when nothing was read. At the end of the line |
| `alogload` | what `audiolog_play` does on the main thread to start an audio log: finding it, and starting the thread that reads it |
| `alogcpu` | the share of one core the audio thread spends on a log while it plays: converting it, a slice at each of its calls |
| `alogcbmax` | the longest of those calls in this second, in ms. A call has 43 ms before the sound runs dry |
| `alogdry` | calls of the audio thread in this second that gave silence because the log wasn't read from the card yet (not per frame). Expected at a log's start only |

### The battery

| field | what |
|---|---|
| `batt` | the battery's remaining capacity, mAh. There is no reading of the current: the drain is the slope of this over minutes. The gauge moves 3 to 5 mAh every 35 s, so two drains can only be told apart over stretches of 10 minutes or so (`VPROF_WINDOWS_PER_VARIANT`), unplugged for a few minutes beforehand |
| `battmv`, `batttemp` | its voltage in mV and its temperature in °C |
| `charging` | 1 while it is charging: such seconds say nothing about the drain |
| `sndready=a/b` | total: sound effects decoded, of those to decode ahead (`snd_preload` in `src/MacSrc/SDLSound.c`) |
| `snddecode` | total: what the background thread has spent reading and decoding them, in ms |
| `sndmem` | total: what its decoded effects take up, in MB |

A second with a large `frame_max` and a large `sndload`, `resload` or
`alogload` maximum is a stall from loading, not from drawing.

### Stutters

| field | what |
|---|---|
| `slow=20:a,34:b,50:c,100:d` | frames of this second longer than 20, 34, 50 and 100 ms: one, two, three and six screen refreshes (not per frame) |
| `skipped=a/b` | frames left out of the timings in this second, and the longest in ms. A self-check draws its frame twice: a hitch you can see, but the profiler's own |
| `logwrite` | how long the main thread took to build the previous second's lines and queue them, in ms. Between two frames, so in no frame's time, but the screen waits for it |
| `logio` | the longest write of the log's own thread since the line before, in ms. Nothing waits for it, unless a read or a write of the game's falls on it. 0.00 for seconds on end, then a large figure: the card held a write that long (30.8 s once, in S7c's capture), and anything of the game's that touched the card in that time waited with it |
| `logdrop` | total: times the lines found the queue full and were dropped. Must be 0 |

A frame is a stutter when it takes at least 25 ms and half again as
long as the average frame of the second before, so that a place that
runs steadily at 40 fps isn't one long stutter. The four worst since
the log was last written each get a line of their own, after that
second's line:

```
spike t=412.38 mode=1 frame=212.40 input=0.20 sim=0.61 render3d=209.10 ui2d=0.00 present=2.10 other=0.39 | traverse=3.10 sendview=205.80 raster=0.00 record=0.40 helpscan=1.20 hud=0.90 viewout=0.20 sndload=205.10 resload=0.00 alogload=0.00 outlined=0.30 reswrite=0.00 respack=0.00 reslzw=0.00 resio=0.00 | gpuwait=0.40 gpusubmit=0.30 gpuupload=0.20 gputex=12.0KB swapwait=0.00 cmds=140 views=1 at=1:23.50,41.25,96@180
```

`t` is when the frame ended, in seconds. The times are that one
frame's, in ms, under the names of the second's line; `other` is what
the five parts of a frame leave. `cmds` and `views` are the frame's
counts. `grep '^spike' profile.txt` lists a capture's stutters.

The line ends with `notes=`: the frame's ten longest pieces of 1 ms and
more, longest first, as `ms:name`, with `@address` and `#number/number`
where the piece has them:

```
notes=98.41:input_chk();97.02:uiPoll;96.80:ui@810a1234#4;61.04:res#4d2/52311;9.66:render_run()
```

| name | what |
|---|---|
| a statement, such as `ai_run()` or `inventory_draw()` | what a timer wraps: `VPROF_RUN` gives its statement's own text as the name, spaces taken out |
| `ui@address#type` | a handler of the interface library called for an input event of that type (hex): the function at that address |
| `hotkey@address#key` | a hotkey's function, and the key (hex) |
| `res#id/bytes` | a resource read from the card: its id (hex) and its size |
| `resm#id/bytes` | the same read from its file's copy in memory |
| `view@address` | a 3D view drawn (`fr_rend`), and where it was asked for from |
| `inv_select@address#row`, `inv_use@…`, `inv_drop@…` | a click in the inventory handed to the page's own function at that address, for that row (hex) |
| `mfd_handler@address#function/panel`, `mfd_simp@…` | a click on a side panel handed to the panel function's handler at that address |
| `copy_file#0/bytes` | a whole file copied: a save to its slot, a slot back for a load, the archive for a new game |
| `save_head`, `write_level_to_disk#level`, `level_save_open`, `level_save_pieces#count`, `level_save_close` | what a save is made of: the player and the schedules written, then the level, piece by piece |
| `empty_slate`, `closedown_game`, `load_head`, `load_level_from_file#level`, `free_dynamic_memory`, `level_load_open`, `level_load_pieces`, `level_load_close`, `load_dynamic_memory`, `load_level_data`, `obj_load_art` | what a load, a new game or a change of level is made of |
| `wait_cursor@address` | the frame `begin_wait()` presents to show the wait cursor, and who asked for it. A present waits for the screen: up to 16.7 ms |
| `audiolog_bark_play@address#bark` | a bark set off, and from where |
| `EDMS_soliton_vector`, `edms_delete_go`, `obj_move_to#object`, `compare_locs` | what `physics_run()` is made of |
| `fade_into_location#x/y`, `check_entrance_triggers#x/y`, `check_hazard_regions` | what `compare_locs` does when the player steps onto a new map square: the music's change, the square's triggers |
| `audiolog_stop`, `MacTuneUpdateVolume`, `snd_stop_music`, `Mix_HookMusic` | what `audiolog_play` does around reading and starting its log |
| `read_email`, `audiolog_play`, `inventory_draw_new_page`, `email_skip_lines`, `email_draw_text`, `make_email_cursor`, `inventory_clear`, `mfd_notify_func`, `play_vmail` and the like | what opening a mail or turning its page is made of (`src/GameSrc/email.c`) |
| `mfd@address#function/panel` | a side panel redrawn (`mfd_update_current_slot`): the panel function at that address, its number (hex, the `MFD_*_FUNC` of `mfdext.h`) and which panel, 0 left, 1 right |
| `uiPoll`, `poll_mouse`, `process_motion_keys` | the three parts of `input_chk()` |

In a stutter's line, `reswrite=` is the frame's time in `ResWrite`,
resources written to a file (a level saved), `reslzw=` the part of it
spent compressing them and `resio=` the part spent in the file's seeks
and writes. `respack=` is the time spent packing a file
whose resources were replaced: at its next opening for writing, not
when they are written (`src/Libraries/RES/Source/resbuild.c`).

The pieces sit inside one another: a handler that has a resource read
shows both, and that is how to read them. An address is looked up in
the build that made the capture:

```
arm-vita-eabi-nm -n build-profile/systemshock | awk '$1 <= "810a1234"' | tail -1
```

(in the build's Docker image; the last symbol at or under the address
is the function). The session's first lines have
`sym vprof_frame_end=`, that function's address on the console: if it
isn't the one `nm` gives, the game was loaded elsewhere and every
address is off by the difference. It is not the same from one capture
to the next (0, 0x3a000 and 0x79000 so far): take it from each
session's own line, and look the addresses up in the file of the
flavour that session ran. A
stutter in a second that isn't logged (the screen changed) still gets
its line, with the next second's.

### The self-checks' results

| field | what |
|---|---|
| `check=a/b` | total: rows that differed / CPU views checked. `a` must be 0 |
| `leaks` | total: rows a thread wrote outside its band. Must be 0 |
| `gpudiff=a/b` | total: pixels the GPU drew differently from the CPU / pixels compared. A few percent is normal: edges and light bands fall a pixel apart |
| `hudcheck=a/b` | total: kept HUD pictures that differed from a fresh drawing / compared. `a` must be 0 |

### Traps

- **`fps` above 60.** It is 1000 / `frame_avg`, over the frames that
  count. With the checks flavour it reads 61 to 63 at the screen's
  60: a check's long frame is left out of the average, and the short
  frames that catch up after it are not. The measuring flavour reads
  60.0. The same happens for a second after a switch to a faster path.
- **Fields that contain others.** `render3d` contains `traverse` and
  `sendview`; `traverse` contains `raster`; with the GPU renderer
  `render3d` and `sendview` also contain `helpscan`, which runs while
  the GPU draws. Don't add up a field and its parts.
- **Stalls.** One frame of 150 ms pulls a second's average far down.
  Look at `frame_max` and set such seconds aside; the stall itself has
  a `spike` line (see [Stutters](#stutters)).
- **The log keeps growing.** Delete it before a capture.

## Analysing a capture

Three rules have held up:

1. **Compare inside one capture.** Two captures taken at different
   spots, or at the same spot on different days, don't compare. With
   `VPROF_ALTERNATE` set, the alternating `var=` gives both paths in
   the same place.
2. **Take medians of the seconds**, not the mean: a stall or a check
   frame's neighbour doesn't move a median.
3. **Cut the capture** by the `check` numbers noted while playing and
   by `mode`, and look at each part and each `var` on its own.

A starting point, in Python. It reads a capture and prints medians for
the parts you name:

```python
import re, statistics, sys

rows = [dict(re.findall(r'(\w+)=([^\s|]+)', line))
        for line in open(sys.argv[1]) if line.startswith('t=')]

def num(row, field, part=0):          # part 0: average, 1: most in a frame
    return float(row[field].split('/')[part].rstrip('%KB'))

def checks(row):                      # the number noted from the overlay
    return int(row['check'].split('/')[1])

parts = [(0, 100, 'standing still'), (100, 200, 'walking')]   # by check number
fields = ['fps', 'frame_avg', 'swapwait', 'render3d', 'present']

for first, last, name in parts:
    for var in sorted({r['var'] for r in rows}):
        part = [r for r in rows
                if first <= checks(r) < last and r['var'] == var
                and r['mode'] in '01'             # in the game
                and num(r, 'frame_max') < 60]     # no stall in it
        if part:
            print(name, 'var', var, len(part), 'seconds:',
                  ' '.join('%s=%.2f' % (f, statistics.median(num(r, f) for r in part))
                           for f in fields))
```

## Self-checks and frame dumps

The first two are in every profile build. The others are only in the
checks flavour (`./build.sh profile checks`): without it `check=`,
`leaks=`, `gpudiff=` and `hudcheck=` stay at zero and no dump is
written.

| check | when | what it compares | where the result is |
|---|---|---|---|
| division | at start-up | the fast fixed-point division against the exact one, a million cases | first line of the session in `profile.txt` |
| GPU shaders | at start-up, in every build | known patterns drawn by each pair of shaders against what they must give | `gpu.txt`, and the session's second line |
| CPU replay | one view in 63, when the CPU draws | the recorded calls replayed against the same calls drawn directly; and each thread kept to its rows | `check=`, `leaks=` |
| GPU picture | one view in 63, when the GPU draws | the GPU's picture against the CPU's | `gpudiff=`, and the dumps |
| kept HUD pictures | one text in 64, one bitmap in 2,039 | the copy against a fresh drawing | `hudcheck=` |

A **frame dump** is written for the third GPU comparison of a session,
and then for each one that differs more than any before and by more
than 5% of its pixels. Each is four files in `gpudumps/`:

| file | what |
|---|---|
| `NN-gpu.raw`, `NN-cpu.raw` | the two pictures: one byte a pixel (a palette number), row after row, no header |
| `NN-palette.raw` | 256 colours, four bytes each: red, green, blue, alpha |
| `NN-info.txt` | `width`, `height`, which comparison it was, how many pixels differed |

To look at a pair on a PC, this writes `NN-gpu.ppm` and `NN-cpu.ppm`,
which any image viewer opens:

```python
import re, sys

stem = sys.argv[1]                     # e.g. docs/profiles-gpu/gpudumps-step-12/02
info = dict(re.findall(r'(\w+)=(\d+)', open(stem + '-info.txt').read()))
width, height = int(info['width']), int(info['height'])
palette = open(stem + '-palette.raw', 'rb').read()

for side in ('gpu', 'cpu'):
    pixels = open('%s-%s.raw' % (stem, side), 'rb').read()
    with open('%s-%s.ppm' % (stem, side), 'wb') as out:
        out.write(b'P6 %d %d 255\n' % (width, height))
        out.write(b''.join(palette[4 * p:4 * p + 3] for p in pixels))
```

## Adding a measurement

### A timer

1. Add a name to `vprof_phase_t` in `src/Libraries/H/vprof.h`, before
   `VPROF_PHASE_COUNT`. The header is in the libraries' include folder
   so that library code can use it too.
2. Wrap the code:

   ```c
   VPROF_RUN(VPROF_MYPHASE, my_function(a, b));
   ```

   or, around more than one statement:

   ```c
   VPROF_MARK_BEGIN(VPROF_MYPHASE);
   ...
   VPROF_MARK_END(VPROF_MYPHASE);
   ```

3. Print it in `vprof_window_flush` in `src/GameSrc/vprof.c`: a
   `name=%.2f/%.2f` at the **end** of the format string, and
   `frame_avg_ms(VPROF_MYPHASE, g_frame_samples),
   frame_max_ms(VPROF_MYPHASE)` at the end of the arguments. New fields
   go last so that scripts written for older captures still read the
   line.

A piece that should only be named when a frame stutters needs no
phase: `VPROF_NOTE("name", address_or_NULL, number, number, statement);`
puts it among the frame's notes (see [Stutters](#stutters)).
`VPROF_RUN_AS(phase, "name", number, number, statement)` is a phase's
timer under a name and numbers of its choosing.

Things to know:

- A phase run several times in a frame is summed over the frame.
- Phases may sit inside one another; nothing is subtracted, so the
  outer one contains the inner one.
- `VPROF_MARK_BEGIN`/`END` keep one start time per phase: don't use
  them where the same phase can begin again before it has ended.
  `VPROF_RUN` has no such limit.
- **Main thread only.** The timers aren't protected against two
  threads. The audio thread hands its one figure over with
  `vprof_audio_add`, which is atomic: do the same for another thread.

### A counter

Count into a global (the recorder's are in `rastq_stats`, the GPU
code's in `vgpu_counters`), set it back to zero in `vprof_window_reset`,
and print it divided by `g_frame_samples` in `vprof_window_flush`. A
counter that must survive frames left out of the timings, like a
check's result, also has to be carried over in `vprof_frame_end`, where
a discarded frame's counts are rolled back.

### Comparing two ways of doing something

With `VPROF_ALTERNATE` set to 1 (`src/Libraries/H/vprof.h`; it is 0
unless a comparison is under way), `vprof_variant` goes round
`VPROF_VARIANT_COUNT` values, one every `VPROF_WINDOWS_PER_VARIANT`
logged seconds, and only changes between two seconds. Read it where
the two ways part:

```c
#ifdef VITA_PROFILE
    use_new_way = vprof_variant != 0;
#endif
```

(in a file the PC tests also compile, such as `rastq.c`, test
`defined(__vita__) && defined(VITA_PROFILE)` instead)

and every log line says which was on. At present it is read in
`rastq_gpu_sharing` in `src/Libraries/3D/Source/rastq.c`: 0 shares the
GPU's lists with the workers always, 1 only while frames are short of
time. Each piece of work redefines what the values mean; raise
`VPROF_VARIANT_COUNT` to compare three. Without `VPROF_ALTERNATE` the
build stays on 1, so 1 should be what the game does.

### Leaving a frame out

A frame that does extra work for the profiler's own sake, a comparison
for instance, calls `vprof_frame_discard()`. Its times and counts are
dropped as if it hadn't happened.

## Good practice

- **Fix the conditions and write them down**: resolution (960x544),
  detail (Max), stock clocks, and the Midi Player on DOSBox OPL3. The
  music's synthesis takes a share of a core and the other players take
  more.
- **Stand still for a first figure.** Start a new game and don't touch
  the controls for a minute: the same view every time, in every
  capture. Then go and measure the places that matter.
- **A minute a place**, or, when two ways alternate, long enough for
  each to have had its stretches there (`VPROF_WINDOWS_PER_VARIANT`
  seconds each).
- **Note what you did, with the `check` number**, as you go. A capture
  without notes is hard to cut up afterwards.
- **Say what was not measured.** A capture only shows where you went.
- **The PC test is not a profiler.** `tests/rastq/run.sh` checks that
  the drawing paths give the same pictures, on the PC, in a few
  minutes. It says nothing about time on the Vita. Run it before you
  build for the console; it is what catches a wrong picture early.
