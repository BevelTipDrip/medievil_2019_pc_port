---
id: barrier-merge
title: Upstream opt-in layer-range barrier merging (GOW3_BARRIER_MERGE)
area: stage-b
status: rejected
platforms: all
default_windows: off
default_deck: off
origin: upstream 1.3.8
class: G
files: gpu/shadps4/video_core/texture_cache/image.cpp
symbols: GetBarriers, first_barrier, SkipReadBarriers, ReadCovered
switches: GOW3_BARRIER_MERGE
docs: fix-inventory.md#from-upstream-medievil-133-138-bc160201983-and-contributors-merged-into-lab, study-deck-rescore-rejected.md#a-switches-that-already-exist-no-build, study-deck-patch-audit.md#13-barriers-sync-and-readbacks
related: upstream-barrier-batch-limit, upstream-safe-gpu-mode, lean-barriers, narrow-image-stages, upstream-hitch-work, particle-curve-sync-compile
verified: 2026-11-07
---
## What it does

In `Image::GetBarriers`, on the partial-transition path (per mip and layer, `subresource_states`), the switch
`GOW3_BARRIER_MERGE=1` (read once) extends the previous barrier's `layerCount` instead of adding a barrier. It
does so when the new layer directly follows it on the same mip with the same old layout, source stages and
source access. Only barriers added in this call qualify (`first_barrier`). The destination fields are the call's
own, so they always match. Off by default, by upstream's own choice in 1.3.8.

## Invariants

These matter if the switch is retried:
- A merged barrier must describe exactly the per-layer barriers it replaces: same image, aspect, mip, old and new
  layout, and same src/dst stages and access. The code compares old layout, `srcStageMask` and `srcAccessMask` and
  relies on the rest being constant within one call. A caller that changes the aspect or destination inside one
  `GetBarriers` call would break that.
- The per-layer state update (`state.layout`, `access_mask`, `pl_stage = keep_stages | dst_stage`) must still run
  for every merged layer. It does (the `continue` comes after the update). `keep_stages` can differ per layer
  without affecting the merge, since only the source side is compared.
- Layers skipped by lean-barriers' read-after-read rule (`SkipReadBarriers` with `ReadCovered`), or that need no
  barrier at all, end a run: the next layer is no longer adjacent (`baseArrayLayer + layerCount == layer`), so
  it always starts a new barrier. A new mip always starts one too (`levelCount == 1`, same `baseMipLevel`).

## Interactions

- upstream-barrier-batch-limit is the fix that shipped for the same AMD crash. It splits the batch, so merging is
  not needed for the crash.
- lean-barriers changes which layers get a barrier at all. It was never tested together with the merge.
- narrow-image-stages: `ReadCovered` and the narrowed per-layer stages make `srcStageMask` differ more often, so
  fewer layers merge (`lab` code; upstream 1.3.8 had neither, so its glow observation predates both).
- The glow bug appeared "with the background pipeline warm-up". The warm-up and particle-curve-sync-compile both
  concern background pipeline compilation; no link between them and the outlines is shown.

## Evidence

- Upstream's comment (`image.cpp`, present in `upstream/main` 1.3.8): "OFF by default: with the background
  pipeline warm-up it broke glowing effects (red outlines on the crypt gargoyle, 29 Oct 2026). To be fixed with
  the AMD crash work." That is upstream's observation. We never ran it.
- Rescore A14 (Deck): an AMD crash fix, not a speed item. Expected gain about 0 (few layered transitions a frame,
  inferred). Visual risk.

## History and pitfalls

- Nothing found in the merge logic explains the red outlines (review 2026-11-07, not tested). Possible causes are
  a transition order the warm-up changes, or a mismatch on a field the merge does not compare.
- Worth retrying only for AMD's older driver and only after finding the cause: compare per-layer and merged batches
  for the gargoyle frame (RenderDoc) with the warm-up on. MediEvil's 3,696-layer batch would shrink to a few
  barriers, but the 128 split already makes it safe.
