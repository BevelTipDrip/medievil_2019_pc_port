---
id: upstream-hitch-work
title: Upstream hitch work: 1 ms timer, no power throttling, spare GPU block, pageable memory, futex condvars, merged barrier ranges
area: runtime
status: active
platforms: all
default_windows: on
default_deck: mixed
origin: upstream 1.3.8
class: G
commits:
files: src/compat_win.c, src/runtime_mutex.c, gpu/shadps4/video_core/texture_cache/image.cpp, gpu/shadps4/video_core/renderer_vulkan/vk_instance.cpp, gpu/shadps4/video_core/renderer_vulkan/vk_barrier_tracker.cpp
symbols: runtime_host_scheduling_init, env_switch, futex_cond_enabled, futex_cond_wait, futex_cond_wake, SpareBlockKeeper, pageable_device_memory, memory_priority, InsertRange, merge_gap
switches: GOW3_TIMER_RES, GOW3_POWER_THROTTLE, GOW3_SPARE_IMAGE_BLOCK, GOW3_PAGEABLE_MEMORY, GOW3_FUTEX_COND, GOW3_TRACKER_MERGE_GAP
tests: N-27
docs: fix-inventory.md#from-upstream-medievil-133-138-bc160201983-and-contributors-merged-into-lab, plan-work-queue.md#the-users-order-2026-10-30, plan-frame-pacing.md#n-27-single-frame-stalls-and-the-fight-dips-measured-per-frame-2026-10-30-merge-branch-merge-138-launched-by-claude-with-the-users-go-t-m138-flip-1-t-m138-flip-2-host-off-t-lab-flip-1-t-m138-stall-1--wpr-stall-1etl
depends_on:
related: barrier-merge, upstream-barrier-batch-limit, upstream-texture-gc, driver-memory-off-stage-b, crash-dump
verified: 2026-11-07
---
## What it does

Upstream 1.3.8 `424f3ea` (#20), ported from the Uncharted port and merged on `merge-1.3.8` (`e308c4c`). Our branch
leaves these sections as upstream has them. Six switches, all on by default:
- `runtime_host_scheduling_init` (Windows): `timeBeginPeriod(1)` (`GOW3_TIMER_RES`), and power throttling off for
  the process, both execution speed and timer resolution (`GOW3_POWER_THROTTLE=1` keeps it).
- Guest condition variables (Windows): a sequence word waited on with `WaitOnAddress` (`futex_cond_wait`) instead of
  winpthreads' `pthread_cond` (`GOW3_FUTEX_COND`).
- `SpareBlockKeeper` (`GOW3_SPARE_IMAGE_BLOCK`): when an image memory type has no free range of 64 MiB left, its
  own thread allocates a 64 MiB spare there (VMA opens a new block) and frees the previous spare, so the new-block
  allocation (up to ~170 ms in the driver) happens off the GPU thread.
- Pageable video memory (`GOW3_PAGEABLE_MEMORY`): `VK_EXT_memory_priority` + `VK_EXT_pageable_device_local_memory`
  when offered, so a full card spills to system RAM instead of returning out-of-memory (#58).
- `BarrierTracker::InsertRange`: a range starting up to `merge_gap` bytes (64) after an existing one extends it
  (`GOW3_TRACKER_MERGE_GAP`, 0 turns it off).

## Invariants

- Futex condvars: a waiter reads `seq` while it still holds the mutex, and every wake increments `seq` before
  `WakeByAddress*`. A wake between the unlock and the wait then makes `WaitOnAddress` return at once, so no wake-up is
  lost. A wait can return 0 early (spurious), so guests must loop on their predicate, as POSIX allows.
  `futex_cond_enabled` caches its answer in a static, so signal, broadcast and every wait use one mechanism for the
  whole process. A new wait or wake path must check it too.
- The timer request is process-long (no `timeEndPeriod`). Turning power throttling off includes
  `IGNORE_TIMER_RESOLUTION`, so the 1 ms tick holds while the window is hidden.
- `SpareBlockKeeper` allocates with `VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT`, waits until a burst of image creation
  is over (150 ms quiet, at most one check per 20 ms), and frees the old spare only once the new one has its block.
  It holds up to 64 MiB per memory type for the whole run.
- The tracker merge only grows a node when nothing lies between it and the new range (`FindNode(between)`): the
  tree is a search tree of disjoint ranges, and a grown node overlapping another would break `FindNode`. Same root
  only (same access type). The gap counts as accessed: that may add barriers, but never removes one.
- `SpareBlockKeeper` is created once, for the first `VmaAllocator` that `Note` sees, and its detached thread runs for
  the process: that allocator must never be destroyed or replaced while the game runs.
- Pageable memory needs `memory_priority` first. Without either, the device chain unlinks the feature structs.

## Interactions

- barrier-merge: the opt-in layer-range merge from the same bundle (`GOW3_BARRIER_MERGE`), off, has its own card.
- upstream-texture-gc (budget-based GC limits): the spares count against the same driver budget.
  driver-memory-off-stage-b uses the same allocate-ahead idea for buffer residency.
- Deck (mixed): timer resolution, power throttling and futex condvars are Windows-only code (n/a). The spare block,
  the merge gap and pageable memory are on. Deck logs print `GPU: pageable video memory on` and spare blocks of
  19-36 ms on type 0 (`data/deck-baseline-1-ab.log`). On the Deck's shared memory each spare is 64 MiB of RAM.
- crash-dump / S1 (winpthreads `pthread_mutex_lock` on a garbage mutex): the futex condvars still take that mutex.
- The wait-sites diagnostic (`GOW3_WAIT_SITES`) wraps `cond_wait` / `cond_timedwait` around both mechanisms
  (`e308c4c` put the futex path inside our wrappers).

## Evidence

- Upstream: on 4 cores, worst frame 31-32 -> 18 ms [msg]. The Uncharted port measured +9..+22% FPS from the 1 ms
  timer and up to +31% 1% lows from futex condvars (code comments).
- N-27 (merge build, flip trace): with the host switches off (`GOW3_TIMER_RES=0`, `GOW3_POWER_THROTTLE=1`,
  `GOW3_PAGEABLE_MEMORY=0`, `GOW3_FUTEX_COND=0`), isolated single-frame stalls came 5.3/min at the cap against 2.8/min
  with them on, so they stay on. The worst 15-s fight 1% low was 122 FPS with them off against 103 on, a GPU-bound
  burst. Not verified in game as a separate item.
- Merge test (plan-work-queue.md item 1): `t-m138-3-upstream-off` vs `t-m138-1-check`, heavy normal frames 4.48 vs
  4.45 ms (pre-merge `lab` 4.55), lowest 5-s window 184 vs 186 FPS. Its switch set is not written down; its log
  shows pageable memory off and no spare-block line.
- No A/B of ours for `GOW3_SPARE_IMAGE_BLOCK` or `GOW3_TRACKER_MERGE_GAP` alone.

## History and pitfalls

- The single-frame stalls are older than the merge (`t-lab-flip-1`: 2.0/min on `lab`). One of three was TabTip.exe
  holding a win32k lock during the present (N-27 WPR). Do not blame these switches for them. The fight-low
  difference above is one run each; the per-switch A/B of plan-work-queue.md item 1 was not done.
- `GOW3_TIMER_RES=0` is not a clean 15.6 ms run while the sampler is on (`GOW3_SAMPLE_FILE`): its thread calls
  `timeBeginPeriod(1)` itself (`gpu/shim/gow3_sampler.cpp`).
- `e308c4c` resolved conflicts in `vk_instance.cpp` (pageable features beside Reflex's present id / wait), `image.cpp`
  and `runtime_mutex.c`. Re-check these sections after the next upstream merge.
