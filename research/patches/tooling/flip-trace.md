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
commits: 608f0ec5c6, d07cc4b2d3, 8f6718072c, daeebdb3b9
files: gpu/shim/gow3_flip_trace.h, gpu/shadps4/core/libraries/videoout/driver.cpp, gpu/shadps4/core/libraries/videoout/video_out.cpp, gpu/shadps4/video_core/renderer_vulkan/vk_presenter.cpp, scripts/graphics_options.py, launcher/medievil_launcher.py
symbols: Gow3FlipTrace, Note, File, Start, TraceGpuReady, GpuReadyThread, present_environment, FRAME_TRACE
switches: GOW3_FLIP_TRACE, present_frame_trace
tests: T-L2, T-L3, T-L4, N-27, N-32, Native run 4
docs: fix-inventory.md#9-diagnostics-worth-taking, plan-stage-b-split.md#t-l2-flip-trace-uncapped-vs-120-cap-2026-10-22-0020-0029-launched-by-claude-with-the-users-go-build-1e657ca--nostats-gow3_flip_trace-trace-research-dataflip-trace-1csv, plan-linux-deck.md#native-run-4-flip-trace-2026-11-03-gow3_frame_stats1-gow3_flip_trace-log-datadeck-native-4log-trace-research-dataflipdeck-native-4csv, plan-frame-pacing.md#n-32-ring-reuse-retest-ab-inside-one-launch-2026-10-30-launched-by-claude-with-the-users-go-t-ring-1-build-9fb2d6c--nostats-flip-trace-research-dataflipt-ring-1csv
depends_on:
related: precise-limiter, present-side-limiter, reflex, display-trace, deck-test-tooling, ring-reuse, async-log-writer
---
## What it does

`GOW3_FLIP_TRACE=<file.csv>` writes one row `stage,flip_arg,ns` per stage of every flip (`Gow3FlipTrace::Note`).
Stages: `submit` (video_out.cpp, the game registers the flip), `irq` / `submit_direct` (stage B reaches it),
`queued` (present recorded), `release` (present thread / limiter), `reflex_wake`, `presented`, `flipdone` (the
game's flip event), `dropped`. `d07cc4b` adds `gpu_ready`: `Presenter::TraceGpuReady` queues the frame's ready
semaphore and tick for a `gow3:GpuReady` thread that waits on it and notes the time. It is started only when the
trace is on. `8f67180` adds `present_start` right before `vkQueuePresentKHR`. `daeebdb` writes a first row `start`
holding the trace origin in steady_clock ns (QueryPerformanceCounter on Windows), to line up with WPR. The
launcher's Advanced > Diagnostics "Frame timing trace" (`present_frame_trace`, `FRAME_TRACE`) sets the variable to
`user/logs/flip-trace.csv` through `present_environment`. Tools: `research/tools/flip_*.py`.

## Invariants

- Off (`File()` null): `Note` returns after one static load; `TraceGpuReady` returns without a thread.
- On, every `Note` takes one global mutex and `fprintf`s on the calling thread (game, stage A, stage B, present,
  vblank threads), with `fflush` every 2,048 lines on whichever thread hits it. The trace is part of the timing it
  measures.
- `flip_arg` is the game's frame tag: tools join stages by it (`flip_stages.py` keys rows by `flip_arg`). A new
  stage must note the same tag as the frame's other stages.
- `gpu_ready` waits at most 1 s per frame, in queue order; a frame not finished by then has no `gpu_ready` row.
- The `start` row's ns must stay the absolute origin (not 0), and later rows relative to it: `flip_stages.py` and
  WPR alignment rely on that.

## Interactions

- T-L2's trace found the present-thread limiter's early / late releases that precise-limiter fixed. `present_start`
  came with present-side-limiter (`8f67180`, rejected).
- deck-test-tooling puts an APU trace beside this one. `displayed` (vk_swapchain.cpp) belongs to display-trace
  (rejected).
- Not routed through async-log-writer: it writes synchronously.

## Evidence

- T-L2 (120 cap): each frame waited ~8 ms for its slot, yet releases came 6.3-10.4 ms apart (p99 9.86, p99.9
  10.43 = 8.33 + one 480 Hz tick). T-L4 after the fix: presented p99 9.05, p99.9 9.39 ms.
- Deck native run 4: submit -> irq 12.53 ms, queued -> gpu_ready 15.62 ms (PC 0.26 / 0.44): the Deck's GPU is busy
  almost the whole frame.

## History and pitfalls

- N-32: the user, after a run without any logging or trace, "without logging, I didn't see the spikes". ~1,800 lines
  a second under one mutex, with `fflush` on whichever thread notes: a suspect for N-27's isolated spikes. The doc's
  next step, buffer in memory and write at exit, is not done (the code still writes as it goes).
- Not verified in game: diagnostics only.
