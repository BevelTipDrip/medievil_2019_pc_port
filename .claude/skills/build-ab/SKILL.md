---
name: build-ab
description: Build the MediEvil runtime and run A/B comparisons between builds or settings - saving a baseline exe, swapping only out\medievil-runtime.exe, byte-safe ini / extra-env edits with backups, launch etiquette, and how to judge the result. Use for any regression check, performance comparison or "does the fix still work" run.
---

# Build and A/B runs

## Build (never while the game runs)

```
$env:MSYSTEM='CLANG64'; & C:\msys64\usr\bin\bash.exe -l /c/Users/Ty/MediEvil/medievil_2019_pc_port/research/tools/build-clang64.sh
```
Output goes to `out\build.log` (look at its last lines: `build.sh exit: 0`) and `out\medievil-runtime.exe`.
Check the game is closed first (`Get-Process medievil-runtime`). CMake changes: always this script (bare ninja
from Git Bash reconfigures with a broken compiler). A header such as `texture_cache.h` rebuilds most of the GPU
library (minutes).

## Baselines

- Before changing code for a comparison, branch from the pushed commit and copy the current exe:
  `cp -p out/medievil-runtime.exe /c/Users/Ty/MediEvil/baselines/<branch>-<commit>/`. Copy each build you will
  compare again (e.g. `baselines/copy-fault-trace-fix-103704/`).
- Swapping builds = copying only `out\medievil-runtime.exe` (`cp -p`, then `cmp` against the saved copy).
  Everything else in `out\` (eboot.elf, boot*.bin, json) is regenerated at each start. The release runtime is
  `dist\medievil-windows\bin\medievil-runtime.exe` (stripped, same code as its tag).
- Confirm in the run log which exe ran: the `Build:` line's mtime and size (the `(branch)` there is only git's
  current branch).
- **Ask which build is meant when it is ambiguous** ("the previous release" meant the A/B baseline, not v2.4.0).

## Settings for a run

- Back up first: `cp -p medievil.ini extra-env.txt <scratchpad>/ini-backup-<time>/`.
- `medievil.ini` is CRLF: Python bytes replace asserting one match (`b'\r\nrender_scale=75\r\n'`), never
  `sed -i`; then `diff <(tr -d '\r' < backup) <(tr -d '\r' < medievil.ini)` and `file medievil.ini`.
- If an assert fails, the user changed the file: re-read it, do not force.
- The launcher writes its copy back on Play / close: edit while it is closed, or close and reopen it
  (`Start-Process -FilePath pyw -ArgumentList '-3','launcher\medievil_launcher.py' -WorkingDirectory 'C:\Users\Ty\MediEvil\medievil_2019_pc_port'`).

## Launch etiquette

- Every launch needs the user's explicit go; one change per launch; the user plays, Claude stays idle (no builds,
  exports or other heavy work while the game runs).
- Keep the log quiet (`GOW3_FPS_LOG=0`, no periodic diagnostics) for performance runs.

## Judging

- Static-view screenshots and free movement cannot settle 0.1% lows (2026-11-15): use the flip trace
  (`present_frame_trace=1`) and compare stretches by `flip_plot.py` and per-phase statistics (average, 1% / 0.1%
  lows, frames over 5 / 6 ms per minute, game-frame sd and lag-1 correlation), or the frame-jitter-capture skill.
- Report lows and worst frames, not only averages; a still view can hide what panning shows.
- A repeatable route (pad record / replay + flip trace) is still missing: work queue "A repeatable stress test".
- Status words in notes and commits: found / confirmed / fix built / run / verified (the user's word).
