---
id: flip-trace
title: GOW3_FLIP_TRACE: per-flip timestamps from submit to the game's flip event, gpu_ready, present_start, WPR clock origin
area: tooling
status: diagnostic
platforms: all
default_windows: off
default_deck: off
origin: ours
class: G
commits: 608f0ec5c6, d07cc4b2d3, 8f6718072c, daeebdb3b9, d032cd4
files: gpu/shim/gow3_flip_trace.h, gpu/shim/gow3gpu.cpp, gpu/gow3gpu.h, src/probe.c, tests/bench_flip_trace.cpp, gpu/CMakeLists.txt, gpu/shadps4/core/libraries/videoout/driver.cpp, gpu/shadps4/core/libraries/videoout/video_out.cpp, gpu/shadps4/video_core/renderer_vulkan/vk_presenter.cpp, scripts/graphics_options.py, launcher/medievil_launcher.py, run.py
symbols: Gow3FlipTrace, Note, Write, GpuReadyEnabled, gow3gpu_flip_trace_write, Start, TraceGpuReady, GpuReadyThread, present_environment, FRAME_TRACE
switches: GOW3_FLIP_TRACE, GOW3_FLIP_TRACE_GPU_READY, present_frame_trace
tests: T-L2, T-L3, T-L4, N-27, N-32, Native run 4, flip-trace-bench
docs: fix-inventory.md#9-diagnostics-worth-taking, plan-stage-b-split.md#t-l2-flip-trace-uncapped-vs-120-cap-2026-10-22-0020-0029-launched-by-claude-with-the-users-go-build-1e657ca--nostats-gow3_flip_trace-trace-research-dataflip-trace-1csv, plan-linux-deck.md#native-run-4-flip-trace-2026-11-03-gow3_frame_stats1-gow3_flip_trace-log-datadeck-native-4log-trace-research-dataflipdeck-native-4csv, plan-frame-pacing.md#n-32-ring-reuse-retest-ab-inside-one-launch-2026-10-30-launched-by-claude-with-the-users-go-t-ring-1-build-9fb2d6c--nostats-flip-trace-research-dataflipt-ring-1csv
depends_on:
related: wpr-capture, precise-limiter, present-side-limiter, reflex, display-trace, deck-test-tooling, ring-reuse, async-log-writer, presentation-settings
checked: 2026-11-15
---
## What it does

`GOW3_FLIP_TRACE=<file.csv>` writes one row `stage,flip_arg,ns` per stage of every flip (`Gow3FlipTrace::Note`).
Stages: `submit` (video_out.cpp, the game registers the flip), `irq` / `submit_direct` (stage B reaches it),
`queued` (present recorded), `release` (present thread / limiter), `reflex_wake`, `presented`, `flipdone` (the
game's flip event), `dropped`. `d07cc4b` adds `gpu_ready`: `Presenter::TraceGpuReady` queues the frame's ready
semaphore and tick for a `gow3:GpuReady` thread that waits on it and notes the time. It is started only when the
trace is on (since 2026-11-16 only with `GOW3_FLIP_TRACE_GPU_READY=1`). `8f67180` adds `present_start` right before `vkQueuePresentKHR`. `daeebdb` writes a first row `start`
holding the trace origin in steady_clock ns (QueryPerformanceCounter on Windows), to line up with WPR. `d032cd4` adds a `wall` row (`wall,<Unix time ns>,<ns since start>`, read between two steady clock reads): an ETL's start time is wall clock, so a frame maps onto ETW to the microsecond (the start row alone: +-0.5 s, the ETL's boot time is printed to 0.1 s). The
launcher's Advanced > Diagnostics "Frame timing trace" (`present_frame_trace`, `FRAME_TRACE`, added with the
Presentation settings in `44994b9`, presentation-settings) sets the variable to `user/logs/flip-trace.csv` through
`present_environment` (`run.py`, unless the variable is already set). Tools: `research/tools/flip_*.py`.

Since 2026-11-15 the rows are kept in memory (24-byte rows in 65,536-row chunks) and `Write()` writes the file once:
at the window-close exit (`gow3gpu.cpp`, before `std::_Exit`, which skips atexit), at atexit (the game's own
`exit()`), and from the Windows crash handler (`probe.c` -> `gow3gpu_flip_trace_write`). The log says
"Flip trace: N rows written to <file>". `gpu_ready` rows and their thread are opt-in since 2026-11-16
(`GOW3_FLIP_TRACE_GPU_READY=1`): the thread cost small overshoots at 237 FPS (Evidence). Without them
`flip_slow_stages.py` leaves out its GPU column and `etw_common.flip_frames` stops requiring the stage;
`frame_clocks.py` and `flip_stages.py` show NaN for it. Deck GPU studies need `=1` (deck.sh's trace note). `flip-trace-bench <dir>` (tests/bench_flip_trace.cpp) times the old writer against the new `Note`.

## Invariants

- Off (`Get()` null): `Note` returns after one static load; `TraceGpuReady` returns without a thread.
- On, every `Note` reads the clock, takes one global mutex and stores a row; every 65,536 rows it allocates a new
  1.5 MB chunk. No file I/O on the frame threads. Memory: ~80 MB an hour at 237 FPS, never freed.
- The file is written once, at exit; rows noted after `Write()` are dropped. A process killed from outside (Task
  Manager, debugger) writes nothing. From the crash handler `Write()` gives up after ~200 ms if the lock is held
  ("Flip trace: not written (busy)") rather than hang the exit.
- `flip_arg` is the game's frame tag: tools join stages by it (`flip_stages.py` keys rows by `flip_arg`). A new
  stage must note the same tag as the frame's other stages.
- `gpu_ready` (only with `GOW3_FLIP_TRACE_GPU_READY=1`) waits at most 1 s per frame, in queue order; a frame not finished by then has no `gpu_ready` row.
- The `start` row's ns must stay the absolute origin (not 0), and later rows relative to it: `flip_toggle_ab.py`
  (and `frame_clocks.py`, `state_power.py`) and WPR alignment read it. It carries `flip_arg` 0, so tools that key by
  `flip_arg` (`flip_stages.py`) file it under frame tag 0. It is captured when `Get()` first runs.
- The launcher's file is opened with `"w"`: each start overwrites `user/logs/flip-trace.csv`.

## Interactions

- T-L2's trace found the present-thread limiter's early / late releases that precise-limiter fixed. `present_start`
  came with present-side-limiter (`8f67180`, rejected).
- deck-test-tooling puts an APU trace beside this one. `displayed` (vk_swapchain.cpp) belongs to display-trace
  (rejected).
- Not routed through async-log-writer: rows stay in memory until exit.

## Evidence

- T-L2 (120 cap): each frame waited ~8 ms for its slot, yet releases came 6.3-10.4 ms apart (p99 9.86, p99.9
  10.43 = 8.33 + one 480 Hz tick). T-L4 after the fix: presented p99 9.05, p99.9 9.39 ms.
- flip-trace-bench (3 threads x ~650 rows/s, 20 s, PC): old writer avg 4.7 us, p99 79 us, p99.9 160 us, max 240 us,
  203 calls > 100 us; in memory avg 0.58 us, p99 4.3 us, p99.9 49 us, max 114 us, 2 calls > 100 us. Writing 39k rows
  at exit: 7.2 ms.
- 2026-11-15 in game (4K 75% DLSS, cond-precise-timeout B, trace on, 586 s, 1,087,211 rows written at window close):
  steady stretches 237.0 / 236.6 FPS, p99.9 4.50 / 4.87 ms, worst 5.6 / 5.9 ms, 0 frames > 6 ms (old trace, same
  view: 228.5 FPS, p99.9 4.95, worst 14.2, 16 > 6 ms). RTSS at the end: 1% low 229.2, 0.1% low 217.5 (the screenshot
  dips it; logs off: 233.2 / 227.7). The other stretches were first-time asset loading (stalls to 1.19 s) and heavy
  combat (~224 FPS, nothing > 8 ms), per the user.
- 2026-11-16, the same with `GOW3_FLIP_TRACE_GPU_READY=0` (one change, 555 s): steady 300-550 s 236.9 FPS, p99.9
  4.50 ms, worst 4.66 ms, 0 frames > 4.7 ms (gpu_ready on: 7 and 96 in its two stretches, worst 5.6 / 5.9). The
  user: stutters less frequent and smaller, spikes only on loading and first-time asset streaming. One run each,
  scenes not identical; enough to make gpu_ready opt-in.
- Deck native run 4: submit -> irq 12.53 ms, queued -> gpu_ready 15.62 ms (PC 0.26 / 0.44): the Deck's GPU is busy
  almost the whole frame.

## History and pitfalls

- N-32: the user, after a run without any logging or trace, "without logging, I didn't see the spikes". ~1,800 lines
  a second under one mutex, with `fflush` on whichever thread notes: a suspect for N-27's isolated spikes. The doc's
  next step, buffer in memory and write at exit, was done 2026-11-15 (below).
- 2026-11-15 (PC, 4K 75% DLSS, static view, cond-precise-timeout B): with the trace on, ~228 FPS and isolated
  spikes every few seconds; with every log off, RTSS 236.6 current, 1% low 233.2, 0.1% low 227.7 (cap 237), flat
  graph (same build and switches, only `present_frame_trace` changed). The trace's cost is large: A/B lows taken
  with it on are its spikes (LANDMINES 8b). Same day: rows kept in memory, written at exit (fix built, run;
  Evidence). Traces from earlier builds still carry the old cost. 2026-11-16: gpu_ready opt-in (its thread's
  overshoots); traces from 2026-11-15 and earlier have gpu_ready rows unless it was set to 0.
- "T-L2" also names the L2 region-bucket run (plan-stage-b-split.md); this card's is the flip-trace run.
- Not verified in game: diagnostics only.
