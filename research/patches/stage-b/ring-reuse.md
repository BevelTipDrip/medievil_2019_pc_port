---
id: ring-reuse
title: Constant-ring reuse within one guest command buffer; ring 96 MiB; stamp fix; live switch
area: stage-b
status: active
platforms: all
default_windows: on
default_deck: on
origin: ours
class: C
commits: d6a12857a9, 40385642bf, 31a6c1ad19, 32586bcb44, 9fb2d6c1d1, b46ff97030
files: gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp, gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.h, gpu/shadps4/video_core/renderer_vulkan/vk_constant_ring.h, gpu/shadps4/video_core/amdgpu/liverpool.cpp, gpu/shim/gow3_settings.h, gpu/shim/gow3_toggles.h, scripts/graphics_options.py
symbols: CollectRingBindings, CloseRingReuse, RingReuse, ring_reuse, ring_reuse_epoch, Hold, CloseReuseEpoch, Stamp, Publish, Retire, Capacity, ReportPressure, PerfRingReuse
switches: GOW3_RING_REUSE, GOW3_CONSTANT_RING_MB, toggle2:RingReuse, perf_ring_reuse
tests: N-7, N-14, N-15, N-16, N-32
docs: fix-inventory.md#2-draw-translation-cost-stage-b-per-draw, plan-frame-pacing.md#n-7-constant-ring-reuse-2026-10-28-launched-by-the-user, plan-frame-pacing.md#n-14-bumps-with-stats-off-the-constant-ring-ran-full-2026-10-28-launched-by-the-user, plan-frame-pacing.md#n-15-heavy-area-9-10-ms-frames-with-the-gpu-partly-idle-ring-reuse-2026-10-2829-launched-by-the-user, plan-frame-pacing.md#n-16-research-where-heavy-frames-lose-time-constant-ring-stamp-fix-2026-10-29, plan-frame-pacing.md#n-32-ring-reuse-retest-ab-inside-one-launch-2026-10-30-launched-by-claude-with-the-users-go-t-ring-1-build-9fb2d6c--nostats-flip-trace-research-dataflipt-ring-1csv
depends_on: lineage-constant-ring
conflicts_with:
supersedes:
related: decode-pacing, early-frame-eop, A1, A2, C1-semaphore-poll, stream-class-table, S2, perf-patches-ini
verified: 2026-11-07
---
## What it does

Stage A copies small read-only constant buffers into the constant ring (`Rasterizer::CollectRingBindings`, from
`PostDraw`); stage B only binds them. With reuse on, a 4,096-entry direct-mapped table (`ring_reuse`, keyed by guest
address and size, tagged with `ring_reuse_epoch`) lets a range bound again in the same epoch bind its first copy. An
epoch is one top-level guest command buffer (`ProcessGraphics` with a sequence number; nested IBs do not close it),
also closed at `WRITE_DATA`, `DMA_DATA`, `WAIT_REG_MEM`, `MEM_SEMAPHORE` waits and
`REWIND` (`CloseRingReuse`, liverpool.cpp). `ConstantRing::Hold` keeps every reusable copy from being overwritten
until the GPU has completed every packet recorded before its epoch closed (`CloseReuseEpoch` queues
`{floor, until}`; `Retire` releases them in order). Ring 96 MiB (`GOW3_CONSTANT_RING_MB`, clamped 8-512).

## Invariants

- **Class C:** a guest range's bytes do not change within one epoch (the CPU wrote them before submitting). Any
  packet that can change memory mid-command-buffer must call `CloseRingReuse` first; a new such packet type without
  it reuses stale constants.
- GPU-written ranges never reach the copy: `desc.is_written`, `IsRegionGpuModified` and `PendingWriteOverlaps`
  `continue` before `copy()`, for hits and misses alike. Graphics queue only (`liverpool->OnGfxQueue()`); the
  compute queue never closes an epoch, so a compute write to a graphics constant range relies on those GPU-write
  checks. The flattened user-data buffer (address 0) is copied every time, never reused.
- A hit is valid only while its region is held: `Hold(absolute)` at the copy (not at the reuse, whose first packet
  may already be complete), and `retired = min(completed, reuse_floor, held_floor)`. Merging closed epochs into one
  floor never released it (t-rr1); keep the queue of `held` entries (1,024, the last extended when full).
- `Stamp` (stage B) must publish the pending stamp when the same end arrives on a later tick, or a full ring whose
  position stopped never retires and stays full (N-16).
- Live off (second word bit 4, or `perf_ring_reuse=0`): copies made while on keep their hold until their epoch
  closes, so switching mid-epoch is safe; `GOW3_RING_REUSE=0` starts off.
- `ring_reuse_epoch` is a `u32`: after 2^32 epochs an entry untouched since exactly one wrap would match again (an
  estimate puts that at hours of play; not measured, not seen).

## Interactions

- Ring use grows with stage A's lead (N-14: stats off, stage A further ahead, 32 MiB full); decode-pacing and
  early-frame-eop change that lead. `Retire` runs only when an allocation finds the ring in use; its
  `Semaphore::Refresh` (a GPU query on stage A) then also restarts C1-semaphore-poll's interval.
- The copy reads guest memory through `CopySparseMemory` (A1 changes how, Linux); A2's pending-write filter answers
  `PendingWriteOverlaps`. A full ring leaves the binding to stage B's stream path (stream-class-table, S2).

## Evidence

- N-7 (`t-rr2`, ~1,160 draws): 1.93 MB copied + 8.13 MB reused a frame (81%); BAR writes ~17.8 -> ~4.0 MB a frame
  [V]; "no visual problems (the user)".
- N-15: 9-10 ms GPU-partly-idle frames while panning the heavy area went away with reuse off; mechanism never found;
  reuse off by default (`31a6c1a`). N-16 found and fixed the stamp bug (`32586bc`), not shown to be N-15's cause.
- N-32 (PC, A/B in one launch, no stats): 1% low ~188 vs ~179, 0.1% ~153 vs ~145, no full ring. `t-ring-2`: the user,
  "the .1% lows in the heavy area are all the way up in the 185 range. So they're legitimately higher." On by
  default since `b46ff97`.
- Deck: on / on (audit row 32: 8.75 MB a frame reused in the D2 run; on UMA no PCIe saving, FPS effect inferred ~0).
  The audit's live pair `0 0` / `0 4` (its launch L2) never ran: no Deck A/B.

## History and pitfalls

- `d6a1285` on (N-7) -> 32 MiB ran full with stats off (N-14; `4038564`: 96 MiB and the "Constant ring (5 s): N
  allocations found it full" report: the ring fell back to stage B) -> off (N-15, `31a6c1a`) -> stamp fix
  (`32586bc`) -> live bit (`9fb2d6c`) -> on again (`b46ff97`, N-32).
