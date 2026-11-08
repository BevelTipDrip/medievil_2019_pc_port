---
id: lineage-recorder-fences
title: Recorder fences: end-of-pipe labels written by the Vulkan recording thread after queued copies (inherited)
area: stage-b
status: active
platforms: all
default_windows: on
default_deck: on
origin: upstream (inherited lineage, docs/parallel_gpu.md)
class: G
files: gpu/shadps4/video_core/amdgpu/liverpool.cpp, gpu/shadps4/video_core/renderer_vulkan/vk_scheduler.cpp, gpu/shadps4/video_core/renderer_vulkan/vk_scheduler.h
symbols: RunEventWriteEop, SignalEop, SignalAfterHostCopies, WaitDeferredSignals, deferred_signals_issued, deferred_signals_done, AfterCopies, WaitHostCopies, IsRecordingDeferred
switches: toggle:RecorderFences
docs: fix-inventory.md#1-gpu-sync-barriers-and-render-pass-breaks, gpu-threading-reference.md#2-invariants-must-hold-after-every-change, study-deck-patch-audit.md#11-draw-pipeline-and-helper-threads-inherited-lineage-docsparallel_gpumd-measured-on-the-base-ports-game-not-medievil, docs/parallel_gpu.md
depends_on: lineage-draw-pipe
related: E1, W3, W3b, async-fences, gate2-ordered-fences, U5-2, early-frame-eop, decode-pacing, recorder-lean, signal-sleep, W2, B1
checked: 2026-11-07
---
## What it does

On stage B, `RunEventWriteEop` (`liverpool.cpp`) does not wait for the host copies. It calls
`Scheduler::SignalAfterHostCopies` with the label write and interrupt (`SignalEop`), unless GPU-ordered fences
(`GOW3_GPU_FENCES`) or `GOW3_ASYNC_FENCES` took the fence first. That call `Record`s a closure into the command
stream, counted in `deferred_signals_issued`. When the Vulkan recorder reaches it, the closure registers the signal
with `Gow3Copy::AfterCopies`, so it runs once the guest-memory copies queued before it are done, and then counts
`deferred_signals_done`. Stage B goes on recording. With toggle word 1 bit 41 (`RecorderFences`) set, or off stage B (draw pipe off),
`RunEventWriteEop` writes the label synchronously after `WaitHostCopies` and `WaitDeferredSignals`. With recording
not deferred (`IsRecordingDeferred` false), `SignalAfterHostCopies` itself waits for the host copies and signals at
once. E1 extends the same path to end-of-shader labels (`GOW3_EOS_DEFERRED`).

## Invariants

- A label is written only after the host copies queued before it (the guest reads the copied data once it sees
  the label) and after every earlier deferred label. Labels keep stream order because one recorder replays them
  in order and `AfterCopies` runs them in order.
- Every path that writes a guest-visible value synchronously must first `WaitDeferredSignals`: the synchronous
  EOP / EOS fallbacks, `SignalFlip` (the flip interrupt), `RunWriteData`, stage A's non-pipelined packets that may
  write guest memory (`ScanDcb`, after its drain) and the compute queue's WRITE_DATA / RELEASE_MEM
  (`ProcessCompute`). Otherwise a later value overtakes an earlier label. W3 / W3b tried deferring those too
  instead.
- GPU idle (`Liverpool::Process`, submission end) is raised only after `DrainDrawPipe` + `WaitDeferredSignals`
  (or under decode pacing, as a step stage B defers after the signals). The guest frees label holders at GPU
  idle. Breaking this was the 2026-10-01 guest heap corruption (gpu-threading-reference.md invariant 4).
- Toggle bit 49 (`OrderedGuestWrites`) makes `WaitDeferredSignals` return at once. That breaks the two rules above
  and exists only for A/B. Do not set it in play.
- The labels are not GPU-ordered: they fire when the CPU-side recording and copies pass them, about 14 ms before
  the GPU on the Deck (Native run 9). Anything that frees or reuses memory on a label must not need GPU
  completion. Readbacks use their own waits (D3, deferred readbacks).

## Interactions

- E1 (EOS labels), W3 (WRITE_DATA flags, rejected) and W3b (flip, rejected: boot deadlock) reuse this path.
- async-fences writes the EOP from the copy threads instead (rejected; ordering gaps). gate2-ordered-fences
  (reverted) and U5-2 (rejected) delay labels to GPU completion.
- early-frame-eop signals the frame EOP at decode, ahead of this path. decode-pacing defers GPU idle through it.
- recorder-lean: labels without an interrupt do not wake a sleeping recorder (`KickKind::Label`). signal-sleep
  changed how `WaitDeferredSignals` waits.
- W2 / B1: the targeted host-copy waits use the same deferred-copy source table.

## Evidence

- Inherited, base port's game (docs/parallel_gpu.md): "fences signalled by the recording thread" 103.5 vs 94.9 FPS
  (A/B of that step alone).
- MediEvil: E1 on top (T-E1 with W2): stage B's ordered tasks 0.52 -> 0.23 us per packet. The game's own EOP wait
  on the Deck is 5.6-6.8 ms a frame (Native runs 8-9).

## History and pitfalls

- The 2026-10-01 heap corruption came from this path plus an early GPU idle. It was found with `GOW3_WRITE_LOG=2`.
  Any new early signal (GPU idle, flip, WRITE_DATA) must be checked against it.
