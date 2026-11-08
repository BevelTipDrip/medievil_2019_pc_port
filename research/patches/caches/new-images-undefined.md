---
id: new-images-undefined
title: Every new image is Undefined until first use; first use as a render / depth target skips the upload (N-11)
area: caches
status: active
platforms: all
default_windows: on
default_deck: on
origin: ours
class: C
commits: 40a7825
files: gpu/shadps4/video_core/texture_cache/texture_cache.cpp, gpu/shadps4/video_core/texture_cache/texture_cache.h, gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp
symbols: new_undefined, FindImage, RefreshImage, UpdateImage, DiscardUndefinedContents, gpu_freed_reused
switches: GOW3_NEW_IMAGES_UNDEFINED, toggle:UndefinedTargets
tests: N-10, N-11, N-14
docs: fix-inventory.md#3-memory-sync-uploads-and-readbacks, plan-frame-pacing.md#n-11-render-targets-uploaded-before-being-rendered-into-the-map-oscillations-writer-2026-10-28-launched-by-the-user, plan-frame-pacing.md#n-10-what-the-image-upload-bursts-are-2026-10-28-launched-by-the-user-t-up1-refresh-siteslog-gow3_texcache_stats1, plan-linux-deck.md#hilltop-mausoleum-lighting-pool-pages-leaked-on-linux-2026-11-0607-fixed-7246f88--f74e1a1
depends_on: C3
related: clear-skips-upload, park-unmapped, pool-kept-pages, mausoleum-toggle-bisect, texcache-stats
checked: 2026-11-07
---
## What it does

C3's `Undefined` flag set on every image `FindImage` creates, not only at addresses a guest unmap freed
(`new_undefined`, read once from `GOW3_NEW_IMAGES_UNDEFINED`). A first use as a render / depth target then skips the
upload through C3's `DiscardUndefinedContents`; any other first use uploads guest memory in `RefreshImage` as before,
so textures the CPU loads are unchanged. The same commit made `UpdateImage` / `RefreshImage` take a
`std::source_location` so the `GOW3_TEXCACHE_STATS` refresh list names each upload's caller and marks undefined
images.

## Invariants

- As C3: only target first uses skip. `RefreshImage` clears `Undefined` before it looks at `Dirty`; a sampled,
  storage or copy first use of a new image always uploads.
- The flag is set only on the creation path of `FindImage` (`slot_images.insert` or `TakeParkedImage`), after
  `RegisterImage`; an existing image never becomes Undefined again. Images made elsewhere (`ExpandImage`, the
  depth-overlap rebuild, the stencil placeholder) start `Dirty`, not Undefined, and upload as before.
- Assumption (class C): a new image first bound as a render / depth target has no CPU-written contents the pass needs
  (no load of CPU data under the first draw). A guest that fills memory on the CPU and then renders over it with
  blending into a fresh target would lose its bytes.
- Toggle bit 61 (`UndefinedTargets`) also switches this off, live, together with C3 and the GPU-freed case (new
  flags only: an image flagged before the switch still skips at its first target binding).

## Interactions

- depends_on C3: the binding-side discard and the `RefreshImage` rule are C3's.
- With this on, C3's `unmapped_addresses` and clear-skips-upload's `gpu_freed_addresses` only change the stats line
  ("other new images flagged undefined").
- pool-kept-pages: the Mausoleum fix relies on the non-target first uses still uploading guest memory; this card
  must not be extended to texture first uses without proof the memory holds nothing (N-10: streamed textures also
  land in recycled memory).
- park-unmapped: a parked target taken back is "new" here and gets the flag.

## Evidence

- N-10 (`t-up1`): the large gameplay uploads were 4K render-target-pool images (3840x2160 RGBA16F at 64 MB, RGBA8 at
  32 MB, ~446 MB in all), the 33-100 MB frames.
- N-11 `t-up5`: no 4K render-target uploads in the refresh list; gameplay image upload peaks 6.5-8.1 MB/s vs
  12.5-14.1 in `t-up3b`; no visual problems (the user). The "not uploaded first" counter counts every target first
  use of an Undefined image, dirty or not: it overstates the saving.
- N-14 `t-nostats4` bisect: this switch among five off still showed the bumps; the cause was ring reuse.

## History and pitfalls

- Built after `GOW3_CLEAR_SKIPS_UPLOAD` and `GOW3_UNDEFINED_GPU_FREED` had 0 hits (clear-skips-upload): the 4K targets
  sat in fresh pool memory where no image had been.
- Hilltop Mausoleum (Deck, 2026-11-06/07): undefined-target uploads were a suspect, ruled out by a
  `GOW3_NEW_IMAGES_UNDEFINED=0` run; the cause was leaked pool pages (pool-kept-pages).
