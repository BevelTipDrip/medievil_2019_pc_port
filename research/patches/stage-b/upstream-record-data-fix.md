---
id: upstream-record-data-fix
title: Upstream SyncWithDrawRecorder: stage A waits for stage B before recording; RecordData waits before copying
area: stage-b
status: active
platforms: all
default_windows: on
default_deck: on
origin: upstream 1.3.4 / 1.3.5
class: G
files: gpu/shadps4/video_core/renderer_vulkan/vk_scheduler.h, gpu/shadps4/video_core/renderer_vulkan/vk_scheduler.cpp, gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp, gpu/shadps4/video_core/texture_cache/texture_cache.cpp
symbols: SyncWithDrawRecorder, SetDrawPipeSync, RecordDataFix, RecordData, ReserveRecordData, ReportInterleave, ReportStaleRecordData, record_data_chunk, EndRendering, CommandBuffer
switches: GOW3_RECORD_DATA_FIX
tests: N-27
docs: fix-inventory.md#from-upstream-medievil-133-138-bc160201983-and-contributors-merged-into-lab, plan-upstream-merge.md#1-where-upstream-and-lab-overlap, plan-upstream-merge.md#2-the-five-conflicts-and-how-to-resolve-them, plan-work-queue.md#pending-fixes-research-2026-10-29-ranked-there-state-now
depends_on: lineage-draw-pipe
related: L5, upstream-barrier-batch-limit, bulk-descriptor-copy, U2, D2
verified: 2026-11-07
---
## What it does

1.3.4: `Scheduler::SyncWithDrawRecorder` runs before stage A records anything: `Record`, `CommandBuffer`,
`EndRendering`, and the texture cache's `UpdateImage` / `FindImage` before their lock. It calls the rasterizer's
`SetDrawPipeSync` callback. That callback returns at once on stage B or when the pipe is idle. On stage A it
drains stage B (`ReasonForeignRecord`). Under the texture cache lock, or on any thread other than stage A (copy
threads, helpers), it cannot drain, so it reports the interleave instead (`ReportInterleave`: "recorded while the draw recorder had queued draws"). 1.3.5: the wait
moves into `RecordData` / `ReserveRecordData`, before the data is copied. The data then lands in the same chunk
as the command that uses it (`record_data_chunk`, checked by `ReportStaleRecordData`).
`GOW3_RECORD_DATA_FIX=0` restores the 1.3.4 order (wait in `Record`).

## Invariants

- Commands stage A records while stage B has queued draws must not land between those draws. Breaking this let a
  buffer copy's `EndRendering` close the render pass stage B was drawing in: an NVIDIA crash in level 3, issues
  #2 and #12.
- `RecordData` output must stay in the chunk of the command that reads it, because chunks are recycled once
  executed. If the wait happens after the copy, the recorder can hand the chunk over in between, leaving the
  command in the next chunk with its data in a recycled one. That gave garbage barriers and an AMD driver crash
  in 1.3.4 (#18).
- No caller may hold a lock that stage B takes and then wait here. Texture-cache-lock holders skip the drain
  (`LockHeldByCurrentThread`), and `UpdateImage` / `FindImage` sync before locking. The merge check: D2's
  `DownloadMemory` holds `cpu_writes_mutex` while it records a copy. Today stage B is already idle there because
  L5 drains first.
- On stage B the callback returns at once (`DrawPipe::OnStageB()`), so per-draw cost is unchanged even with
  1 + N calls per descriptor push.

## Interactions

- L5 handles the same race from the other side: stage A drains stage B before every queued command, removing the
  cause. This card guards the recording calls. Keep both (plan-upstream-merge.md 1).
- upstream-barrier-batch-limit: `FlushBarriers` passes its barrier array through `RecordData`, which is exactly
  the data whose chunk #18 lost.
- bulk-descriptor-copy: 3 `RecordData` calls per push instead of about 16, so fewer syncs on stage A paths.
- U2: `DrawRecorderQuiet` (same callback, `drain=false`) lets direct arena writes act without a command.

## Evidence

- Upstream: the level 3 crash on NVIDIA (1.3.4) and the AMD crash (#18, fixed by the 1.3.5 order).
- N-27 merge test (1.3.3-1.3.8 merged): no regression against the pre-merge `lab` (heavy normal frames 4.45-4.48
  vs 4.55 ms). This card was not measured alone. No user report of the level 3 crash on `lab` before or after.

## History and pitfalls

- A "Scheduler: recorded while the draw recorder had queued draws" line in a log means a stage A caller under the
  texture cache lock recorded before draining. Look for a new lock-holding caller.
- Merge conflict 2 (`EndRendering`): our `std::source_location caller` signature with their sync call first.
- `GOW3_RECORD_DATA_FIX=0` is for tests only. It brings back the #18 window on AMD.
