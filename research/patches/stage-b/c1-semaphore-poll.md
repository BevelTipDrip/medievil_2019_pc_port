---
id: C1-semaphore-poll
title: Stage B polls the GPU's timeline semaphore at most every 100 us for end-of-pipe readback retirement
area: stage-b
status: active
platforms: all
default_windows: on
default_deck: on
origin: ours
class: G
commits: f03ee526b8
files: gpu/shadps4/video_core/renderer_vulkan/vk_scheduler.h, gpu/shadps4/video_core/renderer_vulkan/vk_semaphore.cpp, gpu/shadps4/video_core/renderer_vulkan/vk_semaphore.h, gpu/shadps4/video_core/texture_cache/texture_cache.cpp, gpu/shadps4/video_core/texture_cache/texture_cache.h, gpu/shim/gow3_toggles.h
symbols: IsFreePolled, RefreshedWithin, refreshed_ns, Refresh, RetireReadbacks, ProcessDownloadImages, SemaphorePollInterval, semaphore_polls_skipped
switches: GOW3_SEMAPHORE_POLL_US, toggle2:SemaphorePollInterval
tests:
docs: study-stage-b-per-draw.md#5-implemented-c1-f03ee52, plan-linux-deck.md#stage-b-per-draw-2026-11-04-study-stage-b-per-drawmd-c1-built-in-f03ee52-not-run, plan-linux-deck.md#baseline-1-and-live-abs-2026-11-04-appimage-a863482-67-helpers-auto-no-dedicated-core-auto-clocks-mailbox-log-datadeck-baseline-1-ablog
depends_on: lineage-deferred-readbacks
conflicts_with:
supersedes:
related: ring-reuse, P2, W2, R1-readback, C5, stage-b-spin, recorder-lean
checked: 2026-11-07
---
## What it does

C1 of study-stage-b-per-draw.md (not the C1 driver crash, not rescore C1). Each end-of-pipe / end-of-shader event
(~200 a heavy frame) runs `TextureCache::ProcessDownloadImages`, whose `RetireReadbacks(true)` asked the timeline
semaphore for the GPU's tick (on RADV an ioctl, `DRM_IOCTL_SYNCOBJ_QUERY`). `Scheduler::IsFreePolled(tick)` answers
from the last known tick and queries again only when no thread has queried within `GOW3_SEMAPHORE_POLL_US` (default
100; 0 disables). `Semaphore::Refresh` stamps `refreshed_ns` after every query; `RefreshedWithin` reads it.

## Invariants

- Never early: `IsFreePolled` returns true only when the known tick already covers `tick` (or after a fresh
  `Refresh`). A skipped query returns false, so a copy retires at a later event, at most one interval late.
- Only polls whose answer just decides "now or at a later poll" may use it: `RetireReadbacks(true)` at the end of
  pipe and the opt-in idle-skip check (`FrontReady` under `GOW3_IDLE_EOP_SKIP`; there a stale "not ready" makes
  the event count as idle and skip its host-copy wait, safe only because `download_images` must also be empty).
  Every wait that needs the work to be
  done (fault paths, `ResolveReadbacks`, `SnapshotReadback`, staging, the `Finish` fallbacks) keeps `IsFree` /
  `Refresh`. Switching one of those to `IsFreePolled` would make a waiter spin or act on stale data.
- `refreshed_ns` is shared by every caller of `Refresh`: any thread's query restarts the interval. Adding a
  `Refresh` that does not actually query the GPU would starve the poll.
- Second toggle word bit 16 set: a query at every event again (live); the interval is read once at start.

## Interactions

- Retirement publishes readback data later by up to the interval (lineage-deferred-readbacks, R1-readback); stale
  readbacks already tolerate later data. W2 / P2 decide whether the end-of-pipe waits at all; C1 only changes the
  poll inside it.
- Other threads' `Refresh` calls (ring-reuse's `ConstantRing::Retire` on stage A, `Scheduler::IsFree`,
  `Semaphore::Wait`, which refreshes before and after it blocks, and stage-b-spin's 20 us polls) count for the
  interval.
- The inherited pending-op poll limit (one semaphore query in 32 polls, audit row 15) is the same idea elsewhere.
  study-deck-platform-optimization.md R2 (a GPU-written tick instead of queries) would replace both.

## Evidence

- The cause (Deck, study-stage-b-per-draw.md): ordered tasks on stage B cost 17-26x the PC's per event while draws
  cost 4-5x; `ProcessDownloadImages` 3.3 us there vs 0.13 us on the PC.
- Deck baseline 1 (`a863482`), live pair `0 16` (C1 off) vs base: 73.7 vs 74.2 FPS, stage B busy 94.1% vs ~94%:
  "+0.5 for C1 (noise level)". No visual issues reported. Not verified in game beyond that run.
- The "Stage B syscalls per frame" counters (queries by thread, skipped, time saved) came with it; no doc quotes
  their numbers.

## History and pitfalls

- Default on everywhere because it can only delay a retirement and never makes one early. If a readback-dependent
  effect (torches, HZB, luminance) looks a frame late on the Deck, compare with `0 16`.
- On the PC the whole texture-cache part of an event is 0.13 us (`ProcessDownloadImages`), so the query is cheap
  there; the patch matters on RADV only, but runs on both.
