# Testing System Shock on the Vita

Project facts for the `vita-testing` skill.

## Folder on ux0:

`data/systemshock/` (`ux0:data/systemshock/`).

## Flavours

Default: **profile**.

| flavour | build command | folder | package | title ID | app |
|---|---|---|---|---|---|
| game | `./build.sh` | `build/` | `systemshock.vpk` | `SHOK00001` | System Shock |
| profile | `./build.sh profile` | `build-profile/` | `systemshock-profile.vpk` | `SHOK00002` | System Shock Profile |

The profile build's Profiler page (in-game menu, the main list's third
row) picks what it profiles (kept in `prefs.txt`, so say which to set):
- **Profile:** Timings (measuring) or Checks (self-checks and frame dumps,
  what proves a drawing change);
- **Variants:** 1 only, or Alternate (variants 0 and 1 take turns, 20 s
  each, `var=` in the log).

`./build.sh profile checks` no longer exists. The old "System Shock Checks"
app (`SHOK00003`) can be deleted from the Vita.

Builds are incremental (a minute or so); the last line says
"Build complete: ...". Each build folder also has `systemshock.self`: an
app's `eboot.bin`.

## Results

What a run writes to `data/systemshock/`:

- `gpu.txt`: this run's GPU start-up, written again each run;
- `profile.txt` (profile flavour): appended to, run after
  run. Each session's second line is the GPU start-up's report;
- `gpudumps/` (profile flavour, Checks on): up to five frame dumps a session,
  `01-` to `05-`, overwritten by the next session.

`RESULTS="gpu.txt profile.txt gpudumps"`, `DEST=/tmp/systemshock-...`

## Never touched

`CurrentGame.dat` and any other save, `prefs.txt`, `keybinds.txt`, `res/`.
The user's settings and progress live there.

## What to test from

- A step's "What a capture should show" in `docs/SHADOWS.md`, or the
  step's write-up in `docs/PERFORMANCE.md`.
- What to set: Profiler → Profile on Checks for a drawing change, on
  Timings to measure; Variants on Alternate to compare two ways. Each
  log line says which was on (`checks=`, `var=`).
- What's normal: with Checks on, it hitches about once a second (a
  checked frame). Frame dumps are written in the background since
  2026-11-19 (`dumpms=`); a long freeze when one is taken is a bug.

## Reading results

- `gpu.txt`, and each session's report line in `profile.txt`: the
  start-up checks;
- `profile.txt`: see `docs/PROFILING.md`, "Analysing a capture";
- frame dumps with an `NN-side.raw`:
  `python3 -I docs/shadows_preview.py "$DEST/gpudumps/NN"` makes pictures
  to look at (convert the `.ppm` to `.png` to show them).

## Keeping captures

Never committed. To keep one beside the docs, it goes in the git-ignored
`docs/profiles-*/` folders.
