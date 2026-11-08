---
id: clear-skips-upload
title: Skip the upload when the pass fast-clears the target; new images where a GPU-written image was freed
area: caches
status: rejected
platforms: all
default_windows: on
default_deck: on
origin: ours
class: C
commits: 40a7825
files: gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp, gpu/shadps4/video_core/texture_cache/texture_cache.cpp, gpu/shadps4/video_core/texture_cache/texture_cache.h
symbols: clear_skips_upload, DiscardClearedContents, IsMetaCleared, gpu_freed_addresses, FreeImage
switches: GOW3_CLEAR_SKIPS_UPLOAD, GOW3_UNDEFINED_GPU_FREED
tests: N-11
docs: fix-inventory.md#10-tried-and-dropped, plan-frame-pacing.md#n-11-render-targets-uploaded-before-being-rendered-into-the-map-oscillations-writer-2026-10-28-launched-by-the-user
depends_on:
related: new-images-undefined, C3
checked: 2026-11-07
---
## What it does

Two attempts at the 4K render-target uploads of N-10, kept as default-on switches with no effect in MediEvil:
- `GOW3_CLEAR_SKIPS_UPLOAD`: in `BeginRenderingFull`, a colour target with one mip and one layer whose CMASK has a
  pending fast clear for the bound slice (`IsMetaCleared`) loses its `Dirty` flag (`DiscardClearedContents`) before
  `UpdateImage`: the clear overwrites it anyway.
- `GOW3_UNDEFINED_GPU_FREED`: `FreeImage` remembers the addresses of freed `GpuModified` images
  (`gpu_freed_addresses`, cleared at 16,384); a new image there is flagged Undefined as after an unmap (C3).

## Invariants

- `DiscardClearedContents` is only correct when the pending clear covers the whole image: keep the
  `levels == 1 && layers == 1` and `IsMetaCleared(CmaskAddress, base layer)` conditions. It drops `Dirty` on any
  image (new or not), so a wrong "cleared" answer would lose CPU writes.
- Same locking as `DiscardUndefinedContents`: atomic `fetch_and` on the flags, no `mutex`.
- The GPU-freed rule assumes guest memory under a GPU-written image never got the GPU's data (no write-back).
  `FreeImage` resolves pending readbacks into guest memory first (`ResolveReadbacks(..., ImageFree)`), so with
  deferred readbacks that memory can hold the GPU's bytes; harmless only because target first uses do not load it
  and texture first uses still upload [inferred].

## Interactions

- new-images-undefined flags every new image anyway, so `GOW3_UNDEFINED_GPU_FREED` changes only the stats while it
  is on; toggle bit 61 turns the GPU-freed flag off with C3.
- `GOW3_CLEAR_SKIPS_UPLOAD` is independent of the Undefined flag and of bit 61.

## Evidence

- N-11 `t-up3b`: `GOW3_CLEAR_SKIPS_UPLOAD` 0 hits ("dirty targets cleared by their pass" 0): those passes do not
  fast-clear. `t-up4`: `GOW3_UNDEFINED_GPU_FREED` 0 hits in the menu: the 4K targets were in fresh memory.
- Not verified in game as a gain (nothing to see). Fix-inventory lists both under "Tried and dropped".

## History and pitfalls

- Status rejected although the code runs by default: both are harmless, kept for other games or scenes whose targets
  are fast-cleared or reuse GPU-freed memory. Could be argued active; the measured effect in MediEvil is nil.
- Worth retrying only where the refresh list (`GOW3_TEXCACHE_STATS`) shows uploads of targets that are cleared first
  or made where a GPU-written image was freed, with `GOW3_NEW_IMAGES_UNDEFINED=0`: with it on, the stats line "other
  new images flagged undefined (GPU-freed or new)" counts every new image not at an unmapped address, so it cannot
  show GPU-freed hits.
