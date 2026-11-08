---
id: fence-stats
title: Deck gate 2 stats: guest fence waits, submission lock, cache actions, faults by PS4 type, fence lag, invalidations
area: tooling
status: diagnostic
platforms: all
default_windows: off
default_deck: off
origin: ours
class: G
commits: 3efee7ab94, 579c9d297c, aec1592c24
files: gpu/shim/gow3_fence_stats.h, gpu/shadps4/core/libraries/kernel/equeue.cpp, gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp, gpu/shadps4/video_core/amdgpu/liverpool.cpp, gpu/shadps4/video_core/renderer_vulkan/vk_scheduler.cpp, gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp, src/runtime_memory.c, gpu/shadps4/core/libraries/videoout/driver.cpp
symbols: Gow3FenceStats, NoteEqueueWait, NoteInvalidate, NoteAcquireMem, FenceProbeLoop, SetGpuWaiter, BeginFence, FenceRecorded, FenceSignaled, pending_fence_kind, lock_armed_at, Report, SignalAfterHostCopies, runtime_fault_stats_enabled, runtime_memory_note_gpu_fault, runtime_memory_fault_report
switches: GOW3_FRAME_STATS
tests: Native run 7, Native run 8, Native run 9
docs: study-fence-waits.md#5-instrumentation-plan-stats-only, plan-linux-deck.md#native-run-7-gate-2-stats-2026-11-03-appimage-3efee7a-gow3_frame_stats1-by-file-log-datadeck-native-7-crashlog, plan-linux-deck.md#native-run-8-gate-2-in-the-graveyard-2026-11-03-appimage-aec1592-67-log-datadeck-native-8log, plan-linux-deck.md#native-run-9-fence-lag-2026-11-03-appimage-579c9d2-67-log-datadeck-native-9log
depends_on: frame-stats-lines
related: U5, U5-2, gate2-ordered-fences, wait-sites, early-frame-eop, uffd, copy-thread-backing-view, E1
verified: 2026-11-07
---
## What it does

Gate 2 of the Deck's U5 plan (study-fence-waits.md section 5), all under `GOW3_FRAME_STATS=1`, printed every 5 s
by `Gow3FenceStats::Report` from the frame-stats block (`3efee7a`):
- Guest event waits: `sceKernelWaitEqueue` by filter / ident and waiting thread: calls, waits, wait time, timeouts,
  and for immediate returns the slack since the event fired (`NoteEqueueWait`, `triggered_ns` in equeue).
- Submission lock (gnmdriver): `SubmitDone` calls, armed / held time until the GPU idle IRQ, submit waits.
- Cache actions: EOP / EOS / EVENT_WRITE event types, RELEASE_MEM bits, ACQUIRE_MEM coherence (`NoteAcquireMem`).
- Runtime fault map (`runtime_memory_note_gpu_fault`, `runtime_memory_fault_report`): real GPU write faults by PS4
  memory type (Onion, Garlic-WC / WB, direct other, flexible), distinct 16 KiB pages, every-frame pages, top sites.
- `579c9d2` fence lag: the packet handler sets `pending_fence_kind`, `Scheduler::SignalAfterHostCopies` takes the
  fence's timeline tick (`BeginFence` / `FenceRecorded`) and stamps our signal (`FenceSignaled`). A detached probe
  thread (`FenceProbeLoop`) waits for the tick. Lag = GPU done - our signal, per EOP with IRQ, EOP label, EOS.
- `aec1592`: `Rasterizer::InvalidateMemory` calls by thread with the draw-pipe drain each one starts with.

## Invariants

- Stats only: nothing may change a command stream or a signal's order. `BeginFence` returns `~0` without
  `Gow3Stats::enabled`, and every later call is then a no-op.
- `pending_fence_kind` is thread-local and consumed by the next `BeginFence` (`std::exchange`): it must be set right
  before the handler's `SignalAfterHostCopies`, or another signal takes its kind.
- The lag is an upper bound: the tick is the command buffer's, so it includes submission granularity
  (study 5.1 level 1).
- The probe thread is started by the first fence with stats on (`SetGpuWaiter`, from `SignalAfterHostCopies` or
  `SignalAfterGpu`); the `!wait_gpu_tick` check is not atomic, so two first fences at once could start two probes
  [code]. It waits through `Semaphore::Wait` (a timed wait in a loop until the tick, no overall timeout), polls every
  500 us when idle, and is detached and never joined.
- Its ring holds 4,096 fences. A probe that falls behind skips ahead and loses samples.
- `pending_fence_kind` is set in the EOP / EOS handlers whether stats are on or not (a thread-local store, harmless).
- The runtime's fault map has its own `GOW3_FRAME_STATS` check (`runtime_fault_stats_enabled`, `1` only).

## Interactions

- The gate for U5 (rejected). The direct measurement that followed was gate2-ordered-fences (reverted). U5-2 /
  `GOW3_GPU_FENCES` also feed `BeginFence` (vk_gpu_signals.cpp).
- wait-sites times the same `sceKernelWaitEqueue` calls by guest call site. This card adds filter / ident / thread.
- Run 7 (with these stats) crashed in `CopySparseMemory` (a pool page read while unmapped, a Linux decommit bug, not
  caused by the stats), which led to copy-thread-backing-view. Run 7's fault map corrected the uffd reading ("write
  faults/s" are pages).
- The equeue timing in `sceKernelWaitEqueue` sits next to wait-sites' own clock reads; both on at once adds both
  costs to every guest event wait.

## Evidence

- Native run 8 (graveyard, 64-67 FPS): real write faults ~1 a frame, Garlic-WC only. Invalidations ~80/s with a
  drain of ~0 ms (ruled out). `SubmitGPUCommandsAsyncTaskThread` waits on the EOP (filter -14, ident 64) once a
  frame, 5.8-6.8 ms average, never already signalled.
- Native run 9: EOP with IRQ lag p50 14.2-14.7 ms, p90 16.2-17.5 ms. "GPU done first" 0 of every kind. By the gate's
  rule (> +4 ms, newest frame, no slack): no-go for U5 as designed.

## History and pitfalls

- Native run 7 crashed after the main menu (fixed separately, `8e7bb25`), so its readings come from the menu and the
  early game only.
- Turning on frame stats also starts the probe thread and the extra timestamps on the equeue and submit paths: these
  runs are for counts, not for pacing.
- Not verified in game: diagnostics only.
