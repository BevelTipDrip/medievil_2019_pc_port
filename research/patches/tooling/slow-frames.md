---
id: slow-frames
title: GOW3_SLOW_FRAMES: what slow frames did, against samples; stage A idle, stage B sections, misses and unprepared draws per frame
area: tooling
status: diagnostic
platforms: all
default_windows: off
default_deck: off
origin: ours
class: G
commits: 7d3d288833, 53ade79730, 615d583565, 8061698f3a, f9e5d01272, 166557ccc5
files: gpu/shadps4/core/libraries/videoout/driver.cpp, gpu/shim/gow3_toggles.h, gpu/shadps4/video_core/renderer_vulkan/vk_draw_pipe.h, gpu/shadps4/video_core/amdgpu/liverpool.cpp, gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp
symbols: slow_ratio, slow_shown, window_reset, pacing_median, last_sections, stage_b_busy_cycles, occlusion_dumps, gpu_idle_total_ns, stage_a_drain_cycles, texset_misses, unprepared_draws, image_generation_misses, unprep_pending
switches: GOW3_SLOW_FRAMES, GOW3_FRAME_STATS
tests: T-SF1, T-D1b, T-HZ4f, T-HZ4g, T-F1, N-8, Native run 3
docs: fix-inventory.md#9-diagnostics-worth-taking, plan-frame-pacing.md#t-sf1-what-the-slow-frames-do-2026-10-19-1735-1744-launched-by-claude-with-the-users-go-build-7d3d288-log-datat-sf1-slow-frameslog, plan-stage-b-split.md#t-hz4f--t-hz4g--t-hz4h-where-the-remaining-spikes-come-from-2026-10-20-2157-2225-launched-by-claude-with-the-users-go, plan-stage-b-split.md#t-f1-what-makes-the-fights-frames-slow-2026-10-25-logs-t-p960-gpu-profile-5-t-p960-slow-frames
depends_on: frame-stats-lines
related: sections, async-log-writer, prep-priority, D1, stage-b-core, lean-barriers, perf-diag
---
## What it does

`GOW3_SLOW_FRAMES=<ratio>` (e.g. `1.2`, read by `atof`) prints one line for every frame longer than ratio x the
previous window's median, at most 600 per 5 s window (`slow_shown`). Every 100th frame is printed as a "Frame
sample" to compare against. Each line has draws, dispatches, submits, occlusion queries, passes ended, stage B
busy %, stage A CPU time, stage A idle for submits (`gpu_idle_total_ns`) and draining (`stage_a_drain_cycles`), and
waits (readback, ticks, recorder, host copies). It also has uploads, protection faults, and misses: texture sets,
render states, unprepared draws by cause (`unprep_pending` / `unavailable` / `checksum`), image lookups and
generation-only lookups. With `GOW3_SECTIONS=1` it adds a second line, "stage B ms:" per section for that frame
(`8061698`). Lines are formatted into one string and queued to the background writer (async-log-writer).

## Invariants

- The code sits inside `if (frame_stats)` in `VideoOutDriver::Flip`, and most counters it reads (misses, fault
  timers) count only under `Gow3Stats::enabled`. So it needs `GOW3_FRAME_STATS=1` as well; alone it prints nothing.
- The threshold is `pacing_median` from the previous 5 s pacing report. No line is printed in the first window, or
  in the frame after a window reset (`window_reset`): those deltas span the reset.
- Per-frame values are deltas of cumulative counters taken at each flip. A counter that is reset elsewhere breaks
  them (the window-reset counters are why `window_reset` exists).
- `stage_b_busy_cycles` is added in `DrawPipe` around each packet handler (`vk_draw_pipe.h`). Without the draw
  pipe, stage B busy reads 0.
- Only measuring: nothing in the line may wait or take a lock on the flip path except the queue push.

## Interactions

- frame-stats-lines provides the median and the enclosing block. sections provides the per-frame breakdown.
- async-log-writer (`de30a39`) moved the formatting off `printf`. Before it, the ~30 unbuffered printf a slow frame
  were themselves the stutter (N-8).
- prep-priority shares `f9e5d01` (unprepared draws by cause were added with it).
- T-SF1's GPU tick waits led to D1 and the download counters (`057afc6`). T-HZ4h (stage-b-core) was judged on these
  lines. T-F1 pointed at render-pass breaks (lean-barriers, perf-diag's pass-break report).

## Evidence

- T-SF1 (`1.2`): 3,115 slow frames against 878 samples. Slow frames had a median of 2.5 ms of GPU tick waits (90%
  over 1 ms), samples 0. The waits were `BufferCache::DownloadMemory`, ~40 a second, up to 5.8 ms each. The draws
  and stage A columns read 0 in that build (fixed in `057afc6`).
- T-HZ4f: stage A waited ~0.03 ms a frame for the game's submits ("the game is never late"). 966 slow frames had
  stage B busy against 50 starved. T-HZ4g: per draw, slow frames cost +22-27% per packet, textures +39-43%.
- T-F1 (`1.15`): the kills had 560-660 passes ended against 326, stage B 92-97% busy against 44%.

## History and pitfalls

- The cap was 60 per window, now 600 (`53ade79`). The commit says "a run"; the code resets it each window.
- The lines change timing: never judge smoothness from a run with them on (N-8). Deck native run 3 took the
  per-part breakdown only through this line.
- Not verified in game: diagnostics only.
