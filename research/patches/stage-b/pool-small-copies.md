---
id: pool-small-copies
title: Small host copies off the recorder to the copy threads in batches (toggle bit 19 PoolSmallCopies)
area: stage-b
status: rejected
platforms: all
default_windows: off
default_deck: off
origin: bloodborne 9120756
class: G
commits: 9120756
files: gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp, gpu/shim/gow3_copy.cpp, gpu/shim/gow3_toggles.h, gpu/shadps4/video_core/renderer_vulkan/vk_scheduler.h
symbols: SmallGuestCopy, RecordHostCopy, QueueCopy, FlushBatch, NoteHostCopySource, PoolSmallCopies
switches: toggle:PoolSmallCopies
tests: T-S1, T-S2
docs: plan-stage-b-split.md#t-s2-small-host-copies-to-the-copy-threads-bit-19-2026-10-19-2343-2349-launched-by-claude-with-the-users-go-build-c4bf2dc-log-datat-s2-small-copieslog, study-deck-rescore-rejected.md#a-switches-that-already-exist-no-build, study-deck-patch-audit.md#11-draw-pipeline-and-helper-threads-inherited-lineage-docsparallel_gpumd-measured-on-the-base-ports-game-not-medievil
depends_on:
related: lineage-recorder-fences, lineage-dropped-stage-b, async-fences, recorder-lean, B1, S2, E1, W2
verified: 2026-11-07
---
## What it does

Small guest copies (stream-buffer reserves in `ObtainBuffer`, small image staging) go through
`BufferCache::SmallGuestCopy`. Default: `scheduler.RecordHostCopy` replays them on `gow3:VkRecorder` in command
order. Toggle word 1 bit 19 (`524288`) is the inverse of a normal toggle bit: **set** it and the copies go to
`Gow3Copy::QueueCopy` instead, batched per thread (flushed at 256 items / 512 KiB, or by `FlushBatch` before a fence)
and run by the copy threads. This card is the bit-set path: tried live, not kept.

## Invariants

- Every copy must be done before the guest sees a fence past it and before the submission that reads it. Both paths
  are covered: `WaitHostCopies` waits for `host_copies_done` (recorder) and `SignalAfterHostCopies` calls
  `Gow3Copy::FlushBatch` and then signals through `Gow3Copy::AfterCopies` (copy-thread epochs, in order).
- `NoteHostCopySource` runs before either path, so B1's targeted `WRITE_DATA` waits see the source whichever thread
  copies it.
- Only the recording owner may `RecordHostCopy`: with the recorder thread on, a draw-preparation worker
  (`!scheduler.OwnsRecording()`) copies at once (`item.run`), before the bit is looked at. The thread-local copy batch
  of a worker would otherwise stay unflushed (the null-chunk crash noted in `SmallGuestCopy`).
- The recorder path needs `IsRecordingDeferred()`: without the Vulkan recording thread (`GOW3_VK_RECORD_THREAD=0`,
  toggle `ThreadedRecording`) every small copy goes to `QueueCopy`, bit set or not. `QueueCopy` itself copies inline
  when the copy pool is off (`GOW3_COPY_THREADS=0`, toggle bit 17 `ParallelCopies`).
- The bit is read per copy, so a live flip only changes where later copies run; each path keeps its own order.

## Interactions

- lineage-recorder-fences: the default exists because the recorder spun for work, so a recorded copy cost no wakeup
  (`9120756` message, comment in `SmallGuestCopy`). recorder-lean (<= 8 threads, the Deck; `ff54428`, after the
  rescore's numbers) removed that spin [inferred: the default's premise no longer holds there]. The rescore calls the
  question open for another reason: T-S2 predates E1 and S2.
- async-fences: its `AfterCopies` path waits only for copy-thread copies; it is consistent only with this bit set
  (see that card).
- S2 (small read-only buffers from the arena) and W2 shrank the copy count and the end-of-pipe wait after T-S2:
  59% of small buffers come from the arena on the Deck (plan-linux-deck.md:1046).
- lineage-dropped-stage-b B1: the lineage tried the same lever twice (queue run by the idle recorder; one command
  per 32 copies): neutral.

## Evidence

- T-S2 (PC, build `c4bf2dc`; bit set live mid-run, compared with T-S1, the run before with the bit clear): FPS 126
  (~130), tasks 0.56 us/packet (0.64-1.17), end of pipe 0.37 (0.48-0.62), **draw 1.61 (1.48-1.55)**, stage A blocked
  on copy threads 15.8% (11.6%). The user: "Seems about the same, maybe slightly worse panning through the heavy area." **Bit 19 stays off.**
- Deck: never run. Rescore A2 estimates -0.3 to +0.5 ms a frame (direction unknown): the recorder is 63% busy
  (`0771a23`, before recorder-lean) and stage B waits for its catch-ups, but the Deck has only 2 copy threads
  (`Gow3Threads::Available() / 4`). Ranked 2 on the Deck shortlist (live, no build). Not verified in game.

## History and pitfalls

- The lineage first put every small copy on the copy threads, then moved them back to the recorder ("no wakeups",
  `9120756`), leaving this bit as the way back.
- T-S2's lesson: the end-of-pipe and fence waits are ordering cost (every copy before a guest-visible write must be
  done), not where the copies run; moving the copies moved the queueing into the draw.
- Retry only on the Deck (recorder lean mode, 2 copy threads, E1, S2 all changed since T-S2): word 1 `524288` <-> `0`
  every 20-30 s in one launch. Judge by lows and stage B busy, not only mean FPS.
