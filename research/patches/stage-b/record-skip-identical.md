---
id: record-skip-identical
title: Skip identical setVertexInputEXT / pushConstants records; one ChunkGuard per block (not built)
area: stage-b
status: rejected
platforms: all
default_windows: n/a
default_deck: n/a
origin: ours
class: G
commits:
files: gpu/shadps4/video_core/renderer_vulkan/vk_pipeline_common.cpp, gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp, gpu/shadps4/video_core/renderer_vulkan/vk_scheduler.h, gpu/shadps4/video_core/renderer_vulkan/host_passes/fsr_pass.cpp, gpu/shadps4/video_core/renderer_vulkan/host_passes/pp_pass.cpp, gpu/shadps4/video_core/renderer_vulkan/vk_camera_motion.cpp, gpu/shadps4/video_core/buffer_cache/fault_manager.cpp
symbols: pushConstants, setVertexInputEXT, EmitVertexBuffers, ChunkGuard, RecordData
switches:
tests: T-S3, N-30
docs: study-frame-cost-2026-10-29.md#2-bulk-descriptor-copy-per-push-bloodborne-03-port-plus-a-single-guard--goal-3, study-stage-b-per-draw.md#4-ranked-cuts, study-stage-b-native.md#2-hot-patterns-and-redundancy-measured, study-deck-rescore-rejected.md#c-analysed-gated-or-ranked-never-built
depends_on: bulk-descriptor-copy
related: S1-delta-draw, recorder-lean, parallel-recording, upstream-record-data-fix
checked: 2026-11-07
---
## What it does

Not built. Two record-side cuts left over from the bulk descriptor copy (rescore C4 + C5):
- **C4, one guard per block:** `Pipeline::BindResources` still makes three `RecordData` calls and
  `EmitVertexBuffers` its own; each takes the `ChunkGuard` spin lock, a reserve check, allocation and prefetch.
  Taking the guard once per block would turn ~7 guarded record operations a draw into 2.
- **C5, skip identical records:** do not record `setVertexInputEXT` / `pushConstants` when their bytes equal the
  last ones recorded into the same command buffer.

## Invariants

- "Last recorded" state is per command buffer: it must reset at every new command buffer (submission, `Flush`),
  every direct-mode `CommandBuffer()` use, and after every host pass that sets its own pipeline, push constants or
  vertex input: about 12 sites (`fsr_pass.cpp`, `pp_pass.cpp`, `vk_camera_motion.cpp`, `fault_manager.cpp`). One
  missed site leaves a draw with the host pass's push constants or vertex layout (garbage geometry, no error).
- Push constants are only comparable within a compatible pipeline layout; a layout change must also reset.
- One guard per block must still keep the guarded data and the command that reads it in the same chunk
  (`record_data_chunk`, upstream-record-data-fix), or the recorder replays a pointer into a chunk it already
  recycled.

## Interactions

- bulk-descriptor-copy (shipped, N-30): did the descriptor part; this is its "extension" and "later and separate"
  items from study-frame-cost item 2.
- S1-delta-draw: the record skips are part of S1 (per-draw study C4 + C7); built together or one after the other.
- recorder-lean: fewer recorder commands shorten the recorder's catch-ups, which on the Deck sit inside stage B's
  tasks (x7.6).

## Evidence

- T-S3 (PC): 7.5 `Record` calls per packet, 0.094 us in all (~12.5 ns each): merging a draw's records saves
  <= ~0.09 us. Native study: `setVertexInputEXT` / `pushConstants` repeat 96% / 86.5%.
- Study-frame-cost: C4 ~0.03-0.05 ms a frame; C5 "~0.02 ms of stage B, and the recorder isn't the limit ... Build it
  only if (a) moves FPS."
- Rescore (Deck): C4 0.12-0.2 ms, C5 0.1-0.2 ms on stage B plus less recorder work ("Underrated for the Deck").
  Nothing built or measured; not verified in game.

## History and pitfalls

- Deferred behind the bulk copy on the PC as too small; the Deck's slower per-call costs make it worth a toggle A/B.
- Build C5 with the invalidation list first and a toggle bit that disables the skip live; check host passes
  (FSR, post-processing, camera motion) visually in the same run.
