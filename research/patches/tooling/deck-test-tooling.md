---
id: deck-test-tooling
title: Clock push-pull tooling: APU trace beside the flip trace, frame alignment, live frame cap as the toggle file's third word
area: tooling
status: diagnostic
platforms: all
default_windows: off
default_deck: off
origin: ours
class: G
commits: 5aac2092b7, 0e54155d74, 7b0eb94654, 1afffd9d20, e3d9f92bed, cd1b1a8644, 6dbd601acf, dba69a23ae, 5ead84222d, ba3ba76b21, 7379aa005b
files: research/tools/apu_trace.py, research/tools/frame_clocks.py, research/tools/deck.sh, research/tools/deck_summary.py, research/tools/state_power.py, research/tools/deck-cpu-clocks.sh, src/runtime_memory.c, gpu/shim/gow3_toggles.h, gpu/shadps4/core/libraries/videoout/driver.cpp, tests/test_gpu_runtime_stubs.cpp
symbols: runtime_toggle_frame_cap, toggle_watcher, current_limit, read_flip, NAMES
switches: GOW3_TOGGLE_FILE, GOW3_FLIP_TRACE
tests: Launch 14
docs: plan-linux-deck.md#clock-push-pull-at-gpu-auto-2026-11-06-the-users-model-tooling-5ead842-not-run, plan-linux-deck.md#launch-14-results-2026-11-06-1945-2022-build-5ead842-trace-2-helpers-logging-ini-67-uncapped-log-datadeck-pushpull-1log-traces-research-datadeckdeck-pushpull-auto1fixed1100capsweep-csv, plan-linux-deck.md#deck-test-tooling-2026-11-04-researchtoolsdecksh--deck_summarypy, plan-linux-deck.md#cpu-cuts-at-a-60-cap-2026-11-06-tooling-7379aa0-signal-waits-2a6efea-not-run
depends_on: lineage-toggle-file, flip-trace
related: precise-limiter, reflex, upstream-vsync-limit-rule, frames-queued-2, deck-preset, prep-workers-2, signal-sleep
verified: 2026-11-07
---
## What it does

`apu_trace.py` (run on the Deck by `deck.sh trace`) samples the amdgpu `gpu_metrics` table (GPU clock and busy,
socket / CPU / SoC / GPU power, core clocks) and the key threads' schedstat run time on `CLOCK_MONOTONIC`, the
flip trace's clock. `frame_clocks.py` (`deck.sh frames`) lines the two up per presented frame: autocorrelation,
alternation, means by frame-time band, lagged correlations. The toggle file takes an optional third word, a frame
cap in FPS (`runtime_toggle_frame_cap`) that replaces the limit live in the present thread's `current_limit` (the
menu's, or a fixed `GOW3_FPS_LIMIT`); `deck.sh steps PERIOD CYCLES "S1" "S2" ...` cycles states (e.g. `"0 0 70"`) for
a cap sweep in one launch. Base: `deck.sh` / `deck_summary.py` (`5aac209`: deploy, env, toggle, `ab`, summary per
toggle state; then `clocks`, `mem`, `cpu-script` (deck-cpu-clocks.sh), render-scale labels, `--perf` windows);
`7379aa0`: the sampler keeps its files open (pread), `deck.sh power` (`state_power.py`) per state.

## Invariants

- The cap is read per present-thread pass; 0 (or a file with fewer than three words) means "no cap word", the
  menu's limit applies. It is clamped to 1000 and then goes through the same rules as the menu's: `limit >=
  vblank_hz` becomes 0 (unlimited), VSync with `limit + 1 >= refresh` becomes 0, and with Reflex it sets the
  latency interval instead of the port's limiter.
- Every writer of the toggle file must write all three words to keep a cap: a `deck.sh ab` / `steps` state with
  two words, and `toggle-ab.ps1` ("N 0" or "0 N", "0" at exit), drop it, so the limit goes back to the menu's there.
- `deck_summary.py` labels windows by the "Runtime: toggle file frame cap=N" log line; the line is printed only on
  a change (`last_cap` starts at 0), so a log that never set a cap has no such line.
- The trace and the flip trace must share `CLOCK_MONOTONIC`; `frame_clocks.py` reads the flip trace's "start" row
  for that origin. Linux only (sysfs, schedstat); the cap word works on both platforms.

## Interactions

- lineage-toggle-file: the watcher this extends (third `fscanf` word); flip-trace: the per-frame side
  (`GOW3_FLIP_TRACE` must be set at start in extra-env.txt).
- precise-limiter, reflex, upstream-vsync-limit-rule: the cap feeds the same limiter and rules as the menu.
- frames-queued-2: proposed as a push-pull fix in the same plan; the cap sweep made it unnecessary.
- deck-preset, prep-workers-2: Launch 14 ran the preset's switches with 2 prep helpers.
- signal-sleep: its 60-cap power A/Bs (Launch 15) run on this tooling (`"W1 W2 60"`, `deck.sh trace 420 50`).

## Evidence

- Launch 14 (Deck, build `5ead842-trace`): cap sweep at GPU auto (present intervals): none 80.5 FPS SD 0.96 ms,
  p99.9 16.67; 75: SD 0.22; 70: SD 0.10, p99.9 14.66; 65: SD 0.06. "A cap a few FPS under the uncapped rate makes
  the pacing flat at auto." The auto trace: socket power 14.8-15.0 W in every frame-time band (the 15 W cap
  binds); lag-1 autocorrelation -0.23.
- The user's decision (as plan-linux-deck.md records it): no finer sweep, a 60 cap is fine on this screen; CPU cuts
  are judged at a 60 cap by power and per-thread CPU from then on. Tooling, not verified in game as a fix.

## History and pitfalls

- The sampler cost 23.7% of a core at 4 ms, 13.2% at 8 ms (it reopened the sysfs files each row); the SMU table
  changes only every ~8 ms. `7379aa0` keeps the files open (pread); sample at 8 ms or 50 ms for averages.
- schedstat run time counts spin-waits, and each traced thread runs a near-fixed share of every frame, so
  per-thread ms correlates with frame time by construction.
- `tests/test_gpu_runtime_stubs.cpp` must define `runtime_toggle_frame_cap` (and the other toggle words) or the GPU
  tests do not link.
