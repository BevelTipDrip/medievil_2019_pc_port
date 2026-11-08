---
id: gate2-ordered-fences
title: GOW3_GPU_ORDERED_FENCES: EOP / EOS reach the guest only once the GPU finished (reverted)
area: stage-b
status: reverted
platforms: all
default_windows: n/a
default_deck: n/a
origin: ours
class: G
commits: f97479f8f6, ba9590709d
files: gpu/shadps4/video_core/renderer_vulkan/vk_scheduler.cpp
symbols: GpuOrderedFences, GpuOrderedQueue, OrderedQueue, SignalAfterHostCopies
switches: GOW3_GPU_ORDERED_FENCES
tests: Native run 9
docs: plan-linux-deck.md#gate-2-ab-attempt-2026-11-03-f97479f-reverted-in-ba95907, plan-linux-deck.md#native-run-9-fence-lag-2026-11-03-appimage-579c9d2-67-log-datadeck-native-9log, impl-gpu-ordered-fences.md
depends_on: lineage-recorder-fences
related: U5-2, U5, E1, async-fences, early-frame-eop
superseded_by: U5-2
checked: 2026-11-07
---
## What it does

Gate 2's "measure directly" A/B (`f97479f`, reverted 9 minutes later in `ba95907`). In
`Scheduler::SignalAfterHostCopies`, an EOP or EOS (the only fences that set `Gow3FenceStats::pending_fence_kind`)
did not run its signal after the host copies. It was pushed instead onto one detached thread
(`GpuOrderedQueue`) that waited on the work semaphore for the fence's timeline tick (`CurrentTick()` at record
time) and then wrote the label and raised the interrupt, in order. The copy model, GPU idle and frame retirement
were left as they were. Off by default (`GOW3_GPU_ORDERED_FENCES=1`). The code is gone.

## Invariants

These are what a retry must hold. U5-2 (`GOW3_GPU_FENCES`) is the version that holds them:
- Every wait on a GPU-ordered signal needs something that submits the tick it waits for. `f97479f` waited on the
  tick of a command buffer that is submitted only at the end of the guest's submission, while the guest waited on
  that fence first: a deadlock. U5-2 adds the decoder idle flush, batching at submission ends, the frame-end and
  WAIT_REG_MEM flushes and a 50 ms request on the signal thread (impl-gpu-ordered-fences.md, risk 2).
- GPU idle (`IrqC GpuIdle`, raised at a submission's end in `Liverpool::Process`) may fire only after every
  label before it is written. The guest frees the objects that hold its labels at GPU idle. The attempt left GPU
  idle at the old time, so delayed labels would have landed in freed memory. That is the corrupted-heap class
  (gpu-threading-reference.md invariant 4, the 2026-10-01 heap fix in docs/parallel_gpu.md).
- Labels keep their order. The attempt held this only among EOP / EOS: one queue thread, in submission order.
  Every other deferred signal (WRITE_DATA, `pending_fence_kind` = -1) still ran at the old time and could
  overtake a queued EOP label, and `deferred_signals_done` was counted before the queued label was written, so
  `WaitDeferredSignals` did not wait for it (the commit's own comment: "the port's own ordering waits ... do not
  wait for the GPU"). A retry must order every guest-visible write, and a second queue thread breaks it again.

## Interactions

- lineage-recorder-fences: the attempt hooked into its deferred path (`SignalAfterHostCopies`), which EOP / EOS
  take only when `DrawPipe::OnStageB()` and `RecorderFences` (toggle bit 41) is not disabled (`liverpool.cpp`).
  With the draw pipe off, the bit disabled or `GOW3_ASYNC_FENCES=1`, the switch did nothing.
- U5-2 replaced it: it submits at the fence, retires GPU idle and frames after the labels, and uses Bloodborne
  0.7's pacing (the first submissions of a frame go ahead).
- E1: its EOS labels took the same path and were delayed too. async-fences, W3 and W3b are other label
  deferrals; none can be combined with GPU-ordered labels (impl-gpu-ordered-fences.md, Switches).

## Evidence

- Motivation, Native run 9 (Deck): the EOP with IRQ that the game's `SubmitGPUCommandsAsyncTaskThread` waits on
  lags GPU completion by p50 14.2-14.7 ms, and the game waits on it about 5.6 ms a frame, never already
  signalled. The estimate was a drop to the high 30s FPS from 68.
- The attempt, one Deck launch: **black screen at boot**. The log stops after RHIThread creation, with no GPU
  error. Not verified in game beyond that.
- U5-2 later made the measurement correctly: 64.0 FPS with fences off against 41.2 at ahead 2 (-36%). The
  estimate held.

## History and pitfalls

- The commit called it "copy model unchanged, so safe". It was not: a fence's safety also depends on GPU-idle
  ordering. The deployed AppImage kept the inert switch until the next build.
- Do not retry the naive form. Delaying guest-visible signals past the CPU recording point needs U5-2's
  submitters and GPU-idle ordering.
