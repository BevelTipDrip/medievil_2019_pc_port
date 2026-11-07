---
id: lineage-constant-ring
title: Constant ring for small constant buffers (inherited)
area: stage-b
status: active
platforms: all
default_windows: on
default_deck: on
origin: upstream (inherited lineage, docs/parallel_gpu.md)
class: G
files: gpu/shadps4/video_core/renderer_vulkan/vk_constant_ring.h, gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp
symbols: ConstantRing, Allocate, Stamp, Publish, Retire, CollectRingBindings, NotePendingGpuWrite, PendingWriteOverlaps, PostDraw
switches: toggle:ConstantRing
tests: N-14, N-16
docs: fix-inventory.md#2-draw-translation-cost-stage-b-per-draw, study-deck-patch-audit.md#11-draw-pipeline-and-helper-threads-inherited-lineage-docsparallel_gpumd-measured-on-the-base-ports-game-not-medievil, docs/parallel_gpu.md
depends_on: lineage-draw-pipe
related: ring-reuse, S2, lineage-draw-preparation, D2, U2
verified: 2026-11-07
---
## What it does

Stage A, in `Rasterizer::PostDraw`, calls `CollectRingBindings` for each stage of a draw it hands to stage B. Small
read-only guest buffers (the stream path of `ObtainBuffer`, `size <= STREAM_THRESHOLD`) and the flattened user data
are copied into `ConstantRing`, a host-visible stream buffer (96 MiB now, 32 inherited; `GOW3_CONSTANT_RING_MB`,
8-512). The packet then carries (index, size, offset) and stage B only binds them. Positions are monotonic. After
recording a packet, stage B `Stamp`s it with the submission tick, and stage A's `Retire` frees regions whose tick
the GPU passed (work semaphore `IsFree`). When the ring is full, `Allocate` returns nothing and stage B binds the
buffer itself. Toggle word 1 bit 36 (`ConstantRing`) turns the ring off live.

## Invariants

- Only ranges whose bytes cannot change before the GPU reads them are copied at decode time, and these are
  skipped:
  - written descriptors (`desc.is_written`, noted with `NotePendingGpuWrite`);
  - GPU-modified ranges (`IsRegionGpuModified`);
  - ranges that queued work will write (`PendingWriteOverlaps`: storage buffers, DMA, WRITE_DATA, fences).

  Copying one of these early binds stale constants (wrong lighting or geometry).
- A region may be reused only after the GPU finished every packet using it. `Stamp` merges stamps per tick and
  publishes the last end of a tick. A ring that runs full must still publish its last stamp (N-16's fix: the end
  that no longer moves is published on a later tick). Without it the ring stays full and every constant falls
  back to stage B.
- The copy reads guest memory on stage A (`CopySparseMemory` or the flat buffer). It must run before stage A's
  later commands can change those bytes, which holds because it happens at decode, in stream order.

## Interactions

- ring-reuse rebinds the first copy of a range within one guest command buffer (`ring_reuse` table, epochs). It is
  what made 96 MiB necessary, and it adds the hold or floor logic to `Retire`.
- S2 binds small clean read-only buffers from the arena on stage B. This ring takes them first on stage A. Ranges
  the ring skips go to S2 or the stream copy.
- D2 / U2: CPU writes into shader-written pages change `IsRegionGpuModified`, which decides eligibility.
- lineage-draw-preparation: `CollectRingBindings` uses the prepared V#s (`buffer_sharps`) when the prepared draw
  matches the stage.

## Evidence

- Inherited, base port's game (docs/parallel_gpu.md): constant ring 114.2 vs 102.5 FPS (+11%).
- MediEvil: N-14 found the ring full at 32 MiB under ring reuse, and N-16 fixed the stamp. N-7 (ring reuse) took
  BAR writes from ~17.8 to ~4.0 MB a frame. Ring numbers belong with ring-reuse.

## History and pitfalls

- Before the ring, "per-epoch constant chunks" (one snapshot per chunk and epoch) was tried and removed in the
  lineage: 32 KiB chunks 82/82/79 vs 85/84/84 FPS without (docs/parallel_gpu.md).
- Ring pressure is reported at most every 5 s and only when full (`ReportPressure`). With stats off, stage A runs
  further ahead, so a size that looks fine with stats can run full without them (the t-nostats bisect).
