---
id: trim-vertex-bindings
title: Trim vertex bindings to the draw's index range (not built)
area: caches
status: rejected
platforms: all
default_windows: n/a
default_deck: n/a
origin: ours
class: G
commits:
files: gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp, gpu/shadps4/video_core/renderer_vulkan/motion_history.h
symbols: Rasterizer::ResolveVertexBuffers, BindVertexBuffers, ranges_merged, ObtainBuffer, IndexRangeCache
switches:
tests:
docs: study-deck-rescore-rejected.md#c-analysed-gated-or-ranked-never-built, docs/parallel_gpu.md
depends_on:
related: obtainbuffer-memo, lineage-draw-preparation, S2, hot-pages
checked: 2026-11-07
---
## What it does

Never built. `Rasterizer::ResolveVertexBuffers` binds each vertex V# over its whole guest size
(`base_address .. base_address + GetSize()`), merges adjacent ranges (`ranges_merged`), and obtains and
synchronizes each range in the buffer cache. UE4's V#s cover whole vertex pools, so each binding walks ~950
tracked pages. The idea: bind and sync only the vertices the draw's indices reach (min / max index times the
stride, plus the base vertex), not the whole pool.

## Invariants

- Correct only if the trimmed range covers every vertex the draw can fetch: the index buffer's min / max (or
  the vertex count for non-indexed draws), the base vertex / instance offsets, every stream's stride, and
  instanced streams (step rates) over their own range. A miss reads stale or unsynchronized vertex data.
- The index range itself costs a scan of the index buffer (or a cache of it per index buffer and range). Object
  motion's `Motion::IndexRangeCache` (`motion_history.h`, used from `vk_rasterizer.cpp`) reuses a result and
  re-verifies it only every 32 frames: "a stale result only costs camera-quality vectors". For a binding a stale
  range drops vertices, so it cannot be reused as is: it would need a check on every use (an index-buffer write
  generation or the hash).
- The draw-preparation path (`prepared->vertex`, `ready->ranges`) builds the same ranges ahead of stage B; a
  trim must be applied there too, or prepared and unprepared draws bind different ranges.

## Interactions

- obtainbuffer-memo: the other idea for cutting per-binding buffer cache work; both target `ObtainBuffer`'s
  page walks.
- lineage-draw-preparation: prepares the vertex ranges this would change.
- S2 / hot-pages: fewer pages walked would also mean fewer pages synchronized or re-uploaded per binding
  (inferred).

## Evidence

- Named in docs/parallel_gpu.md as a remaining candidate ("the V#s cover whole vertex pools, so each binding
  walks ~950 tracked pages"); never taken up on the PC.
- Deck rescore C9: -0.3 to -0.8 ms a frame estimated (vertex part 5.4x slower than on the PC, the worst draw
  part, 1.46 us/draw), "Never judged". No measurement; not verified in game.

## History and pitfalls

- Never built. The rescore asks for `perf` of `gow3:DrawRec` first: the share of page walks under
  `ResolveVertexBuffers`. Build it only if that share is large on the Deck.
