---
id: recorder-lean
title: Recorder lean mode on <= 8 threads: no spin, polled labels do not wake it
area: stage-b
status: active
platforms: all
default_windows: mixed
default_deck: on
origin: ours
class: C
commits: ff5442892a
files: gpu/shadps4/video_core/renderer_vulkan/vk_scheduler.cpp, gpu/shadps4/video_core/renderer_vulkan/vk_scheduler.h, gpu/shadps4/video_core/renderer_vulkan/vk_draw_pipe.h, gpu/shadps4/video_core/amdgpu/liverpool.cpp, gpu/shim/gow3_toggles.h
symbols: RecorderLeanWanted, RecorderLean, RecorderWakeDelayNs, HandoffBytes, RecorderSpin, RecorderBackstop, Kick, KickKind, wake_deadline_ns, WakeDeferredRecorder, WakeDeferredRecorderSlow, SignalAfterHostCopies, recorder_lazy_kicks
switches: GOW3_RECORDER_LEAN, GOW3_RECORDER_WAKE_US, GOW3_RECORDER_HANDOFF_KB, GOW3_RECORDER_SPIN_US, toggle2:RecorderLean
tests:
docs: impl-deck-cpu-bundle.md#r-recorder-lean-mode, study-deck-other-threads.md#31-gow3vkrecorder-63-73k-switchess-code--log, plan-linux-deck.md#baseline-1-and-live-abs-2026-11-04-appimage-a863482-67-helpers-auto-no-dedicated-core-auto-clocks-mailbox-log-datadeck-baseline-1-ablog
depends_on: lineage-recorder-fences, lineage-draw-pipe
conflicts_with:
supersedes:
related: signal-sleep, E1, U5-2, C1-semaphore-poll, cacheline-groups, stage-b-core
checked: 2026-11-07
---
## What it does

On by default when the machine has <= 8 hardware threads (`RecorderLeanWanted`: `Gow3Threads::Available() <= 8`, or
`GOW3_RECORDER_LEAN=1`; `=0` never; read once): the Steam Deck yes, a desktop with more than 8 threads no. In lean
mode the Vulkan recorder sleeps without its spin (20 us below 12 threads, 200 at 12+; `RecorderSpin`;
`GOW3_RECORDER_SPIN_US` sets one), and labels the guest only polls
(end-of-pipe without interrupt, end-of-shader: `SignalAfterHostCopies(..., urgent = false)`) still hand their chunk
over at once but leave a sleeping recorder asleep: `Kick(KickKind::Label)` stores `wake_deadline_ns` instead of
`notify_one`. The owed wake is made by the next waking kick, any stage B kick after `GOW3_RECORDER_WAKE_US` (200),
stage B running dry or `Scheduler::Wait` (`WakeDeferredRecorder`), or the recorder's timed sleep
(`RecorderBackstop`, max(1 ms, 2x the delay)). `GOW3_RECORDER_HANDOFF_KB` (4-96, default 32) sizes the hand-off.

## Invariants

- Order is unchanged: signals still run on the recorder in recording order, then `Gow3Copy::AfterCopies`.
- Every wait that needs a deferred signal (`WaitDeferredSignals`, `WaitHostCopies`, `SyncRecording`) forces a wake
  first (`KickRecording(true)` = `KickKind::Now`); a new such wait without it could stall until the backstop.
- Only non-urgent labels may be lazy: interrupt EOPs, GPU idle (`sceGnmSubmitDone`) and decode-pacing frame retire
  wake at once. Marking a label the game blocks on as `urgent = false` costs up to the wake delay a frame. Only a
  kick made on stage B (`Gow3Sections::recording_thread`) can be a `Label` kick: stage B is the only thread whose
  next kick or idle hook is certain to come.
- An awake recorder needs no wake: `Kick` clears `wake_deadline_ns` when `!recorder_sleeping` (under
  `recorder_mutex`); stage A's `WAIT_REG_MEM` drains stage B, whose idle hook makes the owed wake.
- Live off (second word bit 32) makes any owed wake due at the next kick (`!lean` in `due`).
- **Class C:** assumes the guest does not wait on a polled label on its critical path; a polled label may reach
  the guest ~200 us later while stage B is busy.

## Interactions

- Changes when lineage-recorder-fences' labels run, not how; E1 (deferred end-of-shader fences) labels are the
  non-urgent kind. `GOW3_GPU_FENCES` (U5-2) labels go through the signal thread and are not affected.
- signal-sleep changes how stage B waits for the recorder; lean mode changes how the recorder waits for stage B.
- cacheline-groups gave `wake_deadline_ns` its own cache line.
- Windows: `mixed` = on for CPUs with <= 8 hardware threads (4c/8t PCs; `Available()` counts the process affinity
  mask), off above. The impl doc's "The PC is unaffected unless `GOW3_RECORDER_LEAN=1`" holds only for PCs with
  more than 8 threads.
- stage-b-core on Linux: `Available()` reads the calling thread's affinity. `RecorderLeanWanted` is a static,
  normally first read by the recorder thread as it starts (Scheduler constructor, before stage B exists); if stage B
  read it first after `DedicateCore` it would see 1 thread and turn lean mode on even on a large PC.

## Evidence

- Cause (study-deck-other-threads.md 3.1): `gow3:VkRecorder` 63% of a CPU, 7.3k switches/s; the 20 us spin almost
  never caught a chunk: ~7.3k x 20 us, ~15% of a CPU, also taking issue slots from the sibling hyperthread
  (often stage B's) [inferred there].
- Deck baseline 1 (`a863482`), live pair `0 32` (lean off): 74.3 vs 74.2 FPS, stage B busy 93.4%: "neutral in the
  heavy view". No visual issues reported. Recorder CPU per state not recorded in the docs.

## History and pitfalls

- If FPS drops with lean on while recorder CPU falls, the impl doc says to try `GOW3_RECORDER_WAKE_US=50` first;
  `GOW3_RECORDER_WAKE_US=0` keeps only the no-spin part.
- A larger `GOW3_RECORDER_HANDOFF_KB` means fewer wakes but longer `SyncRecording` waits at submissions and the flip.
