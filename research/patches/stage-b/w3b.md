---
id: W3b
title: Flip interrupt raised after the deferred fences, not waited for (do not use: boot deadlock)
area: stage-b
status: rejected
platforms: all
default_windows: off
default_deck: off
origin: ours
class: G
commits: 6006996, 807d4c8
files: gpu/shadps4/video_core/amdgpu/liverpool.cpp
symbols: SignalFlip, SignalAfterHostCopies, WaitDeferredSignals
switches: GOW3_FLIP_DEFERRED
tests: T-W3b, T-W3c
docs: plan-stage-b-split.md#t-s1c--t-w3-texture-memo-two-way-the-write_data-wait-2026-10-20-2331-2345-launched-by-claude-with-the-users-go-builds-2492874-d67ac70-logs-datat-s1c-two-waylog-datat-w3-writedata-targetslog, plan-stage-b-split.md#t-w3d-w3-ab-without-diagnostics-2026-10-21-0012-0028-launched-by-claude-with-the-users-go-build-807d4c8--nostats, fix-inventory.md#10-tried-and-dropped
depends_on:
conflicts_with:
related: W3, lineage-recorder-fences, frames-queued-2
checked: 2026-11-07
---
## What it does

With `GOW3_FLIP_DEFERRED=1`, `SignalFlip` on stage B does not call `WaitDeferredSignals`. It hands the GfxFlip
interrupt to `SignalAfterHostCopies`, so the Vulkan recording thread raises it after the deferred fences. It was
meant to take the ~1 ms flip wait that W3 created off stage B. It deadlocks at boot, and `807d4c8` made it opt-in
and kept it only as a reference. Never set it.

## Invariants

- The guest's flip handler runs inside the interrupt and presents: `sceVideoOutSubmitEopFlip` -> `SubmitFlip` ->
  `Presenter::PrepareFrame` -> `Scheduler::CommandBuffer` -> `SyncRecording`. Anything that raises GfxFlip on the
  recording thread (`gow3:VkRecorder`) makes that thread wait for itself.
- So the flip interrupt must be raised on a thread other than the recorder, after `WaitDeferredSignals`. That is
  the default path in `SignalFlip`.
- More generally: no deferred signal (`SignalAfterHostCopies`) may run guest code that can record or present. A
  deferred signal runs on the recorder, or on the copy thread that finishes the last earlier copy
  (`Gow3Copy::AfterCopies`: "on whichever thread finishes last"). T-W3c caught the recorder case; the copy-thread
  case would run the guest's handler and its present on a copy thread [from the code, not seen]. The label and
  memcpy signals of E1 / W3 are safe; an interrupt is not.

## Interactions

- W3: W3b existed only to rescue W3. Without W3, `SignalFlip` cost 0.038 us a packet (T-W3b's baseline). Both are
  off.
- frames-queued-2 (rejected) is another attempt at the flip's waits on stage B (the GPU frame wait, not this one).

## Evidence

- T-W3c (build `6006996`, two launches): both froze at boot after the save-data mount, where a good boot connects
  the gamepad and creates `RenderThread 1`. Stacks (`data/t-w3c-freeze-stacks.txt`): the interrupt ran on
  `gow3:VkRecorder` and the handler waited in `SyncRecording`; `GpuCommand` waited in `WaitDeferredSignals`.
  Not verified in game beyond the freezes.

## History and pitfalls

- Lesson from T-W3c: most of the flip's stage B time is the present synchronising with the recording thread, and
  the present needs that (both record into the same command stream).
- Retry only after a redesign that records the present in stream order on stage B, so that no catch-up is needed.
  The interrupt still must not run on the recorder.
