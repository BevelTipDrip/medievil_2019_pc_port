---
id: gpu-write-subresources
title: GPU writes inside an image (not at its base) mark the image GPU dirty (opt-in)
area: caches
status: rejected
platforms: all
default_windows: off
default_deck: off
origin: ours
class: G
commits: 856169cfd4
files: gpu/shadps4/video_core/texture_cache/texture_cache.cpp
symbols: TextureCache::InvalidateMemoryFromGPU, ForEachImageInRegion, InvalidateAlias, WatchNote, Watched
switches: GOW3_GPU_WRITE_SUBRESOURCES
tests:
docs: plan-linux-deck.md#hilltop-mausoleum-lighting-pool-pages-leaked-on-linux-2026-11-0607-fixed-7246f88--f74e1a1
depends_on:
related: pool-kept-pages, mausoleum-diagnostics, mausoleum-toggle-bisect, tiny-target-readback, readback-first-wait, lineage-deferred-readbacks
checked: 2026-11-07
---
## What it does

`TextureCache::InvalidateMemoryFromGPU` (called for GPU buffer writes: `Rasterizer::CopyBuffer` and storage-buffer
bindings with `desc.is_written` in `vk_rasterizer.cpp`) marks an image `GpuDirty` (re-uploaded from the buffer on its
next use) only when the write starts at the image's `guest_address`; upstream's `TODO: Maybe also consider
subresources`. With `GOW3_GPU_WRITE_SUBRESOURCES=1` a write anywhere inside the image (`image.Overlaps`) marks it too.
Either way, a watched image (`GOW3_IMAGE_WATCH`) logs the write ("GPU write inside" / "GPU write inside, ignored").

## Invariants

- Off (default): behaviour is identical to before the commit apart from the `WatchNote` logging, which runs
  only for `Watched(image.info)` images. `ForEachImageInRegion` already passes only images that overlap the
  write (`image.Overlaps`), so `inside` is always true there and the real gate is `subresources` alone.
- "Inside" means any overlap not at the base, including a write that starts below the image and runs into it
  (the logged offset `address - guest_address` then wraps to a huge value).
- On: the whole image becomes `GpuDirty` for a write into any part of it, so its next use re-uploads all of it
  from the buffer cache. A frequent small GPU write inside a big image (an array, a mip chain) turns into a full
  re-upload per write: the cost is unmeasured.
- `ResolveReadbacks(..., GpuPartialWrite)` runs before the lock in both modes: a partial GPU write still needs the
  image's untouched bytes published to guest memory first.
- Assumption (C) behind the idea: UE4 copies each reflection capture into its slice of the capture array with a
  buffer write that starts inside the array. Never confirmed in a capture. The Mausoleum write-up lists the capture
  arrays among the leak's victims instead, "uploaded once from guest memory" (plan-linux-deck.md, root cause).

## Interactions

- pool-kept-pages: the real Mausoleum fix (leaked pool pages); this switch was one of the dead ends ruled out
  before it.
- mausoleum-diagnostics: shares the image watch (`Watched`, `WatchNote`); the "inside" notes are part of that tool
  and stay useful with the switch off.
- tiny-target-readback, readback-first-wait: the other opt-in Mausoleum suspects left behind switches, same status.
- lineage-deferred-readbacks: the `GpuPartialWrite` resolve at the top of the function.

## Evidence

- Deck, 2026-11-07: listed among the dead ends "each ruled out by a run" (plan-linux-deck.md, Mausoleum section).
  No numbers for the run were written up; not verified in game as a fix (it did not fix the room).

## History and pitfalls

- Written on 2026-11-07 while the Mausoleum lighting was blamed on the texture cache; the root cause was the
  Linux pool leaking another allocation's bytes (pool-kept-pages, `7246f88`), which no cache change could fix.
- Left in as opt-in ("Opt-in until measured", commit message): its frame cost and any other image it would fix
  were never measured. Worth retrying only with a capture showing a GPU write inside an image the cache then
  serves stale, and a frame-cost run with the switch on.
