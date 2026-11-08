---
id: upstream-barrier-batch-limit
title: Upstream: at most 128 image barriers per call
area: stage-b
status: active
platforms: all
default_windows: on
default_deck: on
origin: upstream 1.3.8
class: G
files: gpu/shadps4/video_core/renderer_vulkan/vk_runtime.cpp
symbols: FlushBarriers, MaxImageBarriers, recording_barriers, largest_batch
tests: N-27
docs: fix-inventory.md#from-upstream-medievil-133-138-bc160201983-and-contributors-merged-into-lab, study-deck-patch-audit.md#13-barriers-sync-and-readbacks
depends_on: upstream-record-data-fix
related: barrier-merge, upstream-safe-gpu-mode, lean-barriers, narrow-image-stages, upstream-hitch-work
checked: 2026-11-07
---
## What it does

In `Runtime::FlushBarriers` the recorded closure splits the batch's image barriers into calls of at most
`MaxImageBarriers` (128) `pipelineBarrier2` each. The batch's one memory barrier goes with the first call only.
The batch itself is still built and handed to the recorder whole (`scheduler.RecordData` of the array). A
`Barriers: N image barriers in one batch (largest so far)` line prints each new record size above 64
(`largest_batch`). There is no switch.

## Invariants

- Splitting is valid because each `pipelineBarrier2` call is a full dependency on every earlier command, and the
  calls of one batch are recorded back to back in one closure on one command buffer (after `EndRendering`). A
  change that records a command between the calls that touches an image of a later sub-batch would see it before
  its transition.
- The memory barrier must go with the first call (`done == 0`). Repeating it is harmless but redundant; dropping
  it loses the buffer / global dependency.
- The array is `RecordData` output, so it must stay in the command's chunk (upstream-record-data-fix). A stale
  chunk gave the "garbage barriers" crash on AMD (#18).
- `recording_barriers` is set per call to the current sub-batch, so the crash dump names the 128-sized piece
  being recorded, not the whole batch.

## Interactions

- barrier-merge (opt-in, off) attacked the same crash by merging per-layer barriers. With it off, layered images
  still produce one barrier per layer, which this split keeps under the limit.
- lean-barriers and narrow-image-stages change how many barriers a batch has and their stages. Neither changes
  this split.
- upstream-safe-gpu-mode: its crash-report dump reads the same `recording_barriers`.
- upstream-hitch-work: merged barrier *ranges* (tracker gap 64) is a different, buffer-side merge.

## Evidence

- Upstream 1.3.8 (#45, #49): AMD's older Windows driver (RX 400/500, Vega) crashed recording 3,696 image barriers
  in one call (issues #18, #34, #41, #43). Our hardware never showed the crash. N-27 found no regression after the
  merge. Not separately measured; not verified on AMD Windows by us.

## History and pitfalls

- MediEvil reaches the crash size every run. In 69 logs under `research/docs/data/` the first "largest so far"
  line is already `3696`, and the largest is `4758` (`deck-mausoleum-1.log`). Upstream's comment in
  `Image::GetBarriers` says it is one layered image (the per-layer loop: "A layered image gave 3696 barriers in
  one batch"), so the limit splits it into about 29-38 calls. Otherwise
  batches are small and the split costs nothing.
- Do not raise the limit without an AMD Windows test. The driver bug was in recording, not in the GPU.
