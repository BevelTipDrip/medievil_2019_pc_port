---
id: prep-priority
title: GOW3_PREP_PRIORITY=below: draw-preparation scanner and workers below normal instead of idle (Windows)
area: stage-b
status: active
platforms: windows
default_windows: off
default_deck: n/a
origin: ours
class: G
commits: f9e5d01272
files: gpu/shadps4/video_core/renderer_vulkan/vk_draw_prep.cpp, gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp, gpu/shadps4/core/libraries/videoout/driver.cpp, gpu/shim/gow3_toggles.h
symbols: PrepThreadPriority, ScannerLoop, WorkerLoop, MakeBackground, unprep_pending, unprep_unavailable, unprep_checksum
switches: GOW3_PREP_PRIORITY
tests: T-S5
docs: plan-stage-b-split.md#t-s5--t-q2-the-remaining-spikes-frames-queued-2-2026-10-21-2210-2222-launched-by-claude-with-the-users-go-50-slow-frames--sections-logs-datat-s5-spikeslog-datat-q2-frames-queued-2log, study-deck-other-threads.md#32-gow3drawprep-4-19-each-17k-involuntary-switchess-each-code--log
depends_on: lineage-draw-preparation
related: prep-workers-2, slow-frames, lineage-texture-set-memo, T2, thread-nice, stage-b-core
verified: 2026-11-07
---
## What it does

`PrepThreadPriority()` (`vk_draw_prep.cpp`) runs at the start of `DrawPreparation::ScannerLoop` and `WorkerLoop`.
With `GOW3_PREP_PRIORITY=below` (read once, Windows only) the scanner and every worker get
`THREAD_PRIORITY_BELOW_NORMAL`. Otherwise they get `Gow3Threads::MakeBackground()`: `THREAD_PRIORITY_IDLE` on
Windows, `SCHED_IDLE` (or nice 19) on Linux. The same commit (`f9e5d01`) splits the slow-frame line's
unprepared draws by cause in `Rasterizer::Draw`: `unprep_pending` (no worker reached the draw), `unprep_unavailable`
(it could not be prepared, for example no pipeline yet) and `unprep_checksum` (prepared from other registers).
`driver.cpp` prints them. That part belongs with the slow-frames card.

## Invariants

- Only the priority changes. Workers still claim whole buffers, and a prepared draw is used only when
  `state == Ready` and `reg_checksum == liverpool->gfx_reg_checksum` (`TryPreparedPipeline`). A late or starved
  worker therefore costs speed (an unprepared draw), never correctness.
- Below normal must stay below the critical-path threads (stage A, stage B, the recorder, copy threads: normal
  priority; stage B above normal under `GOW3_STAGE_B_CORE=1`, `DedicateCore`). Normal or above would let 8 workers
  on a 16-thread PC take cores from stage B. Below normal is the same level as the async shader compile workers
  (`ThreadPriority::Low` in `vk_pipeline_cache.cpp`), so with the switch the prep threads compete with compiles on
  equal terms instead of yielding to them.
- Applied once per thread at start (`static const` read). It does not change live. The read sits inside
  `#ifdef _WIN32`, so Linux never reads the variable and always takes `MakeBackground()`.

## Interactions

- lineage-draw-preparation: the threads it tunes. prep-workers-2 tunes their count, which is the Deck's lever
  because this switch does nothing on Linux.
- lineage-texture-set-memo / T2: an unprepared draw cannot use the texture-set memo unless T2's wide memo is on
  (`GOW3_TEXSET_WIDE=1`, the Deck preset), so starved workers cost slow texture binds on stage B.
- thread-nice (Deck) is the Linux-side priority work for stage B and the vblank thread. It does not touch the
  prep threads.
- slow-frames: the unprepared-by-cause counters tell whether a burst is `pending` (this switch can help) or
  `unavailable` (it cannot: async compiles).

## Evidence

- Motivation, T-S5 (PC, defaults): two "new content" bursts of about 8 frames each at 9-23 ms, with 150-2,250
  unprepared draws, about 100-250 FindImage page-table lookups and 150-330 texture-set misses. "Next: the
  new-content bursts (draw preparation falling behind, FindImage page-table lookups)".
- **No measurement of `GOW3_PREP_PRIORITY=below` found in the docs.** Not verified in game.

## History and pitfalls

- Status is uncertain: the switch is in the build but was never A/B'd. The launcher and `extra-env.txt` defaults
  do not set it.
- Before trying it, read the cause split: if the bursts are mostly `unavailable`, priority cannot help.
- study-deck-other-threads.md 3.2 gives the Linux view: there, idle-class workers are preempted about 1.7k
  times/s each. One mutex is shared with stage A and the submit thread, so lock-holder preemption is possible
  (inferred). A Linux equivalent (nice 0 or 5 instead of `SCHED_IDLE`) was never built.
