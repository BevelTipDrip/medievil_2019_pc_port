---
id: gpu-profiler-fixes
title: GOW3_GPU_PROFILE fixes: frames end at the display pass, one frame per display pass, timestamps read without waiting, slow passes by growth
area: tooling
status: diagnostic
platforms: all
default_windows: off
default_deck: off
origin: ours
class: G
commits: 0a038d334a, 121952a72e, 10683445b0, 32586bcb44
files: gpu/shadps4/video_core/renderer_vulkan/vk_gpu_profiler.cpp, gpu/shadps4/video_core/renderer_vulkan/vk_gpu_profiler.h, gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp
symbols: GpuProfiler, BeginFrame, Collect, skip_frame, WriteTimestamp, NumSlices, in_display_pass, NoteFrameStart, display_target
switches: GOW3_GPU_PROFILE
tests: T-F1, N-19, N-20, N-23
docs: fix-inventory.md#9-diagnostics-worth-taking, plan-stage-b-split.md#t-f1-what-makes-the-fights-frames-slow-2026-10-25-logs-t-p960-gpu-profile-5-t-p960-slow-frames, study-upstream-commits.md#3-conflicts-with-ours, plan-frame-pacing.md#n-19-what-the-fights-slow-frames-are-per-pass-2026-10-29-launched-by-claude-with-the-users-go-t-prof1-fight-burstslog
depends_on:
related: effects-resolution, ring-reuse, slow-frames, park-unmapped
---
## What it does

The inherited GPU profiler (timestamps per render pass, dispatch and upscaler run in a ring of `NumSlices` = 4
query slices) only closed a frame where the upscaler's motion vectors start one, so with the upscaler off
(MediEvil) it printed nothing. `0a038d3`: `Rasterizer` calls `GpuProfiler::BeginFrame` at a display-buffer draw
when `camera_motion` is off. `121952a`: only at the first draw of a display pass (`in_display_pass`); the game
draws into the display buffer about four times a frame. `1068344`: `Collect` reads the oldest slice without
`eWait`. When it is not ready, the current frame is skipped (`skip_frame`: `WriteTimestamp` and the close do
nothing) and the slice is retried at the next frame. `32586bc`: each 5 s window splits scene parts (frames >= 1 ms)
into slow (>= 1.35x the median of the last 128) and other, and prints the labels by growth.

## Invariants

- `BeginFrame` must run once per game frame on the thread that records draws. Two calls per frame advance the slice
  ring twice (the second call finds `used == 0` and only steps on). The 4-frame-old read then becomes ~2 frames old:
  more frames skipped when the GPU lags.
- `Collect` must never block. The draw recording thread can be behind the caller, and the blocking read stalled the
  boot (T-F1, twice).
- `WriteTimestamp` ends the open render pass (`scheduler.EndRendering`): radv crashed on timestamps inside passes.
  So the profiler adds pass breaks. Its GPU and CPU numbers are not the unprofiled frame's.
- Frames are counted only for collected slices (`++frames` in `Collect`); a skipped frame is simply missing.

## Interactions

- Upstream 1.3.6 (#30) added its own frame start: `NoteFrameStart()` from the `else if (!camera_motion->Enabled())`
  branch, when a draw's display target changes (`display_target`). study-upstream-commits H2 said to keep ours and
  take only the `gpu_frames` increment. `lab` and `deck-lab` both have both: our `in_display_pass` block and upstream's
  branch calling `NoteFrameStart` -> `profiler->BeginFrame()`. Read from the code (not run): two `BeginFrame`
  calls at the first display draw of each frame (see Invariants). fix-inventory says "ours kept over theirs"; the
  code does not match. Check before trusting per-frame coverage.
- `32586bc` also carries effects-resolution and the ring-reuse stamp fix (`ConstantRing::Stamp`).
- slow-frames tells CPU-late frames from GPU frames on the CPU side; this profile does it on the GPU side
  ("between submissions: GPU idle" growth = stage B late).

## Evidence

- T-F1: averaged over 5 s the GPU was not busier in the fight (~370-475 ms of GPU a second vs ~500 calm): the cost
  sits in single frames.
- N-19: two kinds of slow frames. CPU-late ones ("GPU idle" +1.8 ms), and GPU effect frames (7.95 vs 4.94 ms with
  the same draws) whose growth sits in 4K RGBA16F translucency passes (+2.33 ms). That led to effects-resolution
  (N-20: half resolution removed the growth).
- N-23 (t-hzb7): slow GPU frames' growth almost all "GPU idle" (+3.1 to +4.7 ms): the GPU starved.

## History and pitfalls

- study-frame-cost-2026-10-29: "The 2x comes from two display passes per frame in today's code, not from merge
  hazard H2" (written before the 1.3.5-1.3.8 merges). Count frames by flips if per-frame times look halved.
- With the profiler on, the N-23 view-switch spikes did not appear (timing): not for pacing judgements.
- Not verified in game: diagnostics only.
