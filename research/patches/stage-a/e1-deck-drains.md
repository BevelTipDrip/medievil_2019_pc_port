---
id: E1-deck-drains
title: Stage A's last two drains off the frame path: met display-label waits without a drain, OnSubmit on stage B
area: stage-a
status: active
platforms: all
default_windows: mixed
default_deck: on
origin: spacepepperoni 6ef7566
class: G
commits: c4a0147
files: gpu/shadps4/video_core/amdgpu/liverpool.cpp, gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp, gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.h, gpu/shim/gow3_decode_pacing.cpp
symbols: OnSubmitInOrder, OnSubmitWork, OnSubmitOnStageB, PendingFenceValue, NotePendingWrite, IsVoLabel, NoteVoLabelWait, ProcessGraphics
switches: GOW3_ONSUBMIT_STAGE_B, GOW3_VO_LABEL_DRAIN
tests:
docs: impl-decode-pacing.md#display-buffer-label-waits-spacepepperonimedievil_ps4_linux-6ef7566-ported-2026-11-04, impl-decode-pacing.md#onsubmit-on-stage-b-gow3_onsubmit_stage_b-ported-2026-11-04, impl-decode-pacing.md#thread-safety-argument, plan-linux-deck.md#e1-and-the-fix-hit-rates-2026-11-04, study-deck-patch-audit.md#32-restart-only-one-launch-each-against-l1s-a-windows-in-the-same-spot
depends_on: lineage-draw-pipe
related: decode-pacing, early-frame-eop, compute-drains-pipelined, upstream-texture-gc, lineage-draw-preparation, lineage-texture-set-memo
checked: 2026-11-07
---
## What it does

1. **Display buffer label waits** (`ProcessGraphics`, WAIT_REG_MEM, the `IsVoLabel` branch). Before, stage A drained
   stage B before every such wait. Now a wait whose label in memory already meets the test does not drain. This
   half is from spacepepperoni `6ef7566`. Ours adds one condition: the newest write to that label still queued for
   stage B (`PendingFenceValue`), if any, must pass the test too. `GOW3_VO_LABEL_DRAIN=1` restores the drain every
   time.
2. **OnSubmit on stage B** (ours). At `sceGnmSubmitDone`'s flush, `Rasterizer::OnSubmitInOrder` hands one
   `RunInOrder` task to stage B instead of `OnSubmit()` (a drain) plus `Flush()`. The task runs `OnSubmitWork`
   (`ProcessFaultBuffer` if pending, `ProcessDownloadImages`, `RunGarbageCollector`, `runtime.TickFrame`), then
   `Flush`. Called "E1" in the Deck docs, together with decode pacing lead 2. Not the end-of-shader patch E1.

## Invariants

- The label fast path is taken only if all of these hold. (a) `IsVoLabel` and no other submission is queued
  (`num_submits == ...submits.size()`). (b) `wait_reg_mem->Test` passes on memory. (c) The newest unreached entry
  of `pending_fences` covering the label (`PendingFenceValue` scans from the back) passes the test, or there is none.
  (c) relies on `NotePendingWrite` being called for every pipelined WRITE_DATA (only when `RunInOrder` queued it, and
  only writes of 8 bytes or less), which includes the flip's "in use" write from `PatchFlipRequest`. A pipelined
  path that skips it lets the game render into a display buffer whose flip has not been shown: tearing, or a frame
  shown twice or out of order. It matters only when stage B lags
  a full display-buffer cycle, which decode-pacing lead 2 allows.
- `CloseRingReuse()` stays before the wait (the CPU may write memory meanwhile). An unmet wait still drains and sleeps
  (`WaitVoLabel`).
- The OnSubmit task enters the pipe at the stream position the drain had: after every packet before
  `sceGnmSubmitDone`, before anything decoded after it.
- Stage B owns the texture cache, buffer cache, scheduler and staging pool whenever the pipe has work
  (`IsGpuSideThreadId`). The GC may run while stage A is a frame ahead only because prepared draws (`PreparedStage`)
  hold sharps, hashes and pipelines, never an `ImageId`, `BufferId` or view. Stage B memos that hold `ImageId`s are
  checked against `RegistryGeneration` / `RegionStamp`, and destruction goes through `DeferOperation`. A prep-side
  cache object breaks this.
- `OnSubmitOnStageB()`: unset follows `GOW3_DECODE_PACING`, `1` / `0` force it. Never on with `GOW3_GPU_FENCES=1`.
  Read once.

## Interactions

- `default_windows` is mixed. The label fast path is on in every configuration, Windows included. OnSubmit on stage
  B is off there (it follows decode pacing). Deck preset: both on.
- decode-pacing: the reason both drains mattered. In D2 the idle drain's time had moved into OnSubmit.
- compute-drains-pipelined: with these gone, the compute-queue drain is stage A's last gate (30% waiting).
- upstream-texture-gc / lineage-texture-set-memo: the GC and its memos now run on stage B between two packets.

## Evidence

- Deck E1 (2026-11-04, `c4a0147`, D2 + this): FPS 64.5-64.9, stddev 1.42 / p99 18.9 ms, **stage B 91% busy** (D2:
  83%). Stage A waits 30%, all of it the compute drain. OnSubmit drains avoided: 320 per 5 s (294 ms/s of stage A
  waiting saved). 975 label waits met without a drain. Ident-64 wait 6.5 ms, lead 1. No visual issues (the user).
- FPS did not rise: stage B became the wall. That is what the later per-draw work (R1/R2/T1/T2, A1-A3) aimed at.

## History and pitfalls

- E0 (E1 without OnSubmit on stage B) was planned to split the two halves, but never run. The patch audit lists
  `GOW3_ONSUBMIT_STAGE_B=0` as restart test 4 (expected stage B -1 to -5 points). OnSubmit's GC and staging trim now
  count in stage B's busy time.
- Never soaked under a pressured GC (write-backs) with stage A ahead. If flicker or missing textures appear after
  streaming, rerun with `GOW3_ONSUBMIT_STAGE_B=0`. For a display issue, rerun with `GOW3_VO_LABEL_DRAIN=1`.
- The label fast path changes Windows behaviour on `deck-lab` too. No PC measurement of it is written up (the impl
  doc says "built ... not launched"). PC launches of `deck-lab` with the Deck switches did happen for the Mausoleum
  repro (plan-linux-deck.md: "the Deck preset on the PC", the one PC glitch, put down to the Windows pool leak; logs
  `data/t-mausoleum-pc-*.log`, untracked, no frame stats). Regression-test it on the PC before any merge into `lab`.
