---
id: bulk-descriptor-copy
title: Bulk descriptor copy per push: writes and info arrays copied into the chunk once, pointers rebased
area: stage-b
status: active
platforms: all
default_windows: on
default_deck: on
origin: bloodborne 4d4d208
class: G
commits: e0c756610b, ffd5d90fbc
files: gpu/shadps4/video_core/renderer_vulkan/vk_pipeline_common.cpp, gpu/shadps4/video_core/renderer_vulkan/vk_pipeline_common.h, gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp, gpu/shim/gow3_toggles.h, gpu/shim/gow3_settings.h, src/runtime_memory.c, scripts/graphics_options.py
symbols: BindResources, ReserveRecordData, RecordData, BulkDescriptorCopy, PerfLeanBinding, runtime_disabled_optimizations2
switches: GOW3_LEAN_BINDING, perf_lean_binding, toggle2:BulkDescriptorCopy
tests: N-30
docs: fix-inventory.md#2-draw-translation-cost-stage-b-per-draw, plan-frame-pacing.md#n-30-bulk-descriptor-copy-ab-inside-one-launch-2026-10-30-launched-by-claude-with-the-users-go-t-bulk-1log-build-ca5636f-lto
depends_on: lineage-draw-pipe, upstream-record-data-fix
conflicts_with:
supersedes:
related: sampler-memo, record-skip-identical, perf-patches-ini, lineage-toggle-file
verified: 2026-11-07
---
## What it does

With deferred recording, `Pipeline::BindResources` copied each descriptor write's info array into the recording
chunk with its own `RecordData()` (~16 calls a push). When every write's `pImageInfo` / `pBufferInfo` lies inside the
`image_infos` / `buffer_infos` spans the rasterizer passes (all four call sites: `DrawRecord`, `DrawIndirectRecord`,
`DispatchRecord`, `DispatchIndirectRecord`) and none has `pTexelBufferView`, it reserves room once, copies the writes
and the two arrays with three `RecordData()` calls and rebases each pointer by its offset in the source array. Other
pushes keep the per-write loop. `e0c7566` also added the toggle file's second word (`runtime_disabled_optimizations2`).

## Invariants

- The bulk path is taken only if the `within` check holds for every write; a write pointing elsewhere (a local
  array) or with texel buffer views must take the loop, or its rebased pointer is garbage.
- Rebasing uses `write.pImageInfo - image_infos.data()`: the spans passed must be the arrays the writes were built
  from, unreallocated since (`image_infos` / `buffer_infos` are rasterizer members filled per draw).
- Data and the command using it share a recording chunk (chunks are recycled once executed): `ReserveRecordData`
  (sum of the three arrays + 256 slack) runs before the copies and one `Record()` then carries only the span.
  `ReserveRecordData` itself is older (`454d543`); upstream-record-data-fix (1.3.5, `RecordDataFix`) moved the wait for
  the draw recorder before the data instead of into `Record()` (1.3.4's AMD crash, issue #18). With
  `GOW3_RECORD_DATA_FIX=0` this path is as exposed as the per-write loop. A push over `RecordChunk::Capacity` - 1 KiB
  is reserved as 0 and each array may land out of line (`AllocateOversized`).
- `RecordData` of an empty span returns the source span itself, not a copy; that is safe only because no write
  points into an empty array (`within` fails for it).
- Recording not deferred (`IsRecordingDeferred()` false): the push goes straight to `pushDescriptorSetKHR`.
- Off when `PerfOn(PerfLeanBinding)` is false or second-word bit 1 is set, live; both paths make the same push.

## Interactions

- Shares the "Cheaper draw binding" patch (`perf_lean_binding`, `GOW3_LEAN_BINDING=0`) with sampler-memo
  (`ffd5d90`); the toggle bits stay separate (second word 1 and 2).
- record-skip-identical (rescore C4: one `ChunkGuard` per block) is the unbuilt extension of the same cut.
- Pipelines without push descriptors take `updateDescriptorSets` and are unaffected.

## Evidence

- N-30 (PC, A/B in one launch, `toggle-ab.ps1` every 20 s, heavy frames ~1,550 draws, 99 / 179 frames): stage B
  descriptors 0.217 vs 0.337 us a draw median (-0.12 us, ~-0.18 ms of stage B a heavy frame); draw section 1.88 vs
  2.13 us. Kept on by default. No separate in-game confirmation recorded.
- Deck: on / on in study-deck-patch-audit.md (row 28); no Deck A/B of its own.

## History and pitfalls

- Study item 2 of study-frame-cost-2026-10-29.md, from the Bloodborne port 0.3 (`4d4d208`: the same `within` /
  rebase code in its `vk_pipeline_common.cpp`; 0.2 `bf95396` still had the per-write loop): credited in the commit
  and in the code ("From the Bloodborne port"). Keep the credit if the code moves.
- A descriptor-push counter once hashed each push byte by byte on stage B (the N-17 artifact, +0.4 us a draw); the
  counters here are only `descriptor_pushes` / `descriptor_writes` increments behind `Gow3Stats::enabled`.
