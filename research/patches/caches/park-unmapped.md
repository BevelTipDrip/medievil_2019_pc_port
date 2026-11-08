---
id: park-unmapped
title: Park render / depth targets a guest unmap frees; the next image with the same address and geometry takes them back (N-24)
area: caches
status: active
platforms: all
default_windows: on
default_deck: on
origin: ours
class: C
commits: fdd12e8
files: gpu/shadps4/video_core/texture_cache/texture_cache.cpp, gpu/shadps4/video_core/texture_cache/texture_cache.h
symbols: ParkImage, TakeParkedImage, EvictParkedImages, ParkUnmappedEnabled, ParkedBudget, ParkedMaxImage, ParkedMaxAge, parked_images, parked_count, UnmapMemory, RunGarbageCollector
switches: GOW3_PARK_UNMAPPED
tests: N-23, N-24
docs: fix-inventory.md#3-memory-sync-uploads-and-readbacks, plan-frame-pacing.md#n-24-targets-a-guest-unmap-frees-are-parked-for-reuse-2026-10-30-launched-by-claude-with-the-users-go-t-park1-checklog-t-park2-cleanlog, plan-frame-pacing.md#n-23-the-view-switch-spikes-after-the-hzb-fix-2026-10-30-launched-by-claude-with-the-users-go-t-hzb5-switch-waitslog-t-hzb6-long-waitslog-t-hzb7-switch-profilelog
depends_on: C3
related: new-images-undefined, pool-kept-pages, preshadows-live, upstream-texture-gc, texcache-stats
checked: 2026-11-07
---
## What it does

UE4's render-target pool decommits the torch shadow maps (512x512 D16) and the 4K RGBA16F + RGBA8 pair while unused;
each guest unmap freed the images and the next use made them again (C3 only stopped the re-upload). Now
`TextureCache::UnmapMemory` calls `ParkImage` first: a render / depth target that is not a video-out surface, not a
scene proxy, not `Aliased` and at most `ParkedMaxImage` (128 MiB) is unregistered and untracked, its readbacks
resolved / cancelled, metas and pending download dropped, but the Vulkan image and its views kept. `FindImage`, before
creating an image, asks `TakeParkedImage` for one with the same address and geometry. `EvictParkedImages` deletes the
oldest past `ParkedMaxAge` (10 s, each GC tick) or over `ParkedBudget` (768 MiB, at each park).

## Invariants

- A parked image is invisible to lookups and page tracking: `UntrackImage` + `UnregisterImage` before it enters
  `parked_images`. A parked image still found by an overlap search would hand back freed-memory contents.
- `TakeParkedImage` matches every geometry field (address, size, format, type, tile mode, pitch, resources, samples,
  bits, tiled / depth / stencil / block / pow2 / volume props, `alt_tile`); a looser match would bind a target of a
  different layout.
- Taken back, the image starts as a new one: `flags = Dirty`, binding, usage and depth link reset, metas and stencil
  address from the new info; `FindImage` then registers it and flags it `Undefined`, so only a target first use skips
  the upload. `UnmapMemory` records the address in C3's `unmapped_addresses` before parking, so the flag is set even
  with `GOW3_NEW_IMAGES_UNDEFINED=0` (only bit 61 stops it). Its Vulkan layout state is kept (it is the image's).
- `parked_images`, `parked_bytes` and eviction are under `mutex` (`UnmapMemory`, `FindImage`,
  `RunGarbageCollector`); `parked_count` is the lock-free "anything parked" check.
- Assumption (class C): UE4's pool returns a target at the same address with the same geometry, and memory a guest
  unmap freed holds nothing a target needs (as C3). Limits tuned for MediEvil at 4K.

## Interactions

- depends_on C3: without the `Undefined` flag (toggle bit 61 set) a taken-back target is `Dirty` and uploads guest
  memory like a new one; parking still saves the creation.
- pool-kept-pages: non-target first uses of a taken-back image upload guest memory, so they need the recommit's own
  bytes (or zeros).
- upstream-texture-gc: parked images are not in `lru_cache` (only `EvictParkedImages` frees them) and leave the summed
  `total_used_memory`, but count where the driver reports usage (`GetDeviceMemoryUsage`): up to 768 MiB parked can push
  the GC to evict registered images first [from the code, not measured].

## Evidence

- N-23: with the horizon in view the game freed and re-created the 512x512 D16 targets 12-38 times a second through
  guest unmaps, now and then the 4K pair; the GPU was starved between submissions (+3.1 to +4.7 ms idle).
- N-24 t-park1 (stats): 95-100% of parked targets taken back (99/99, 248/240), up to ~200 MiB parked, few evicted;
  the user: torches, shadows, fire, glow and haze normal. t-park2 (clean): the user, "the big spikes there are gone,
  it looked about the same as the logging run. The .1% lows in the heavy area are the best they've been compared to
  all previous clean runs." Verified in game.

## History and pitfalls

- `fdd12e8` also carries preshadows-live (gow3_overlay / gow3_settings / runtime_extra.c).
- Creating the remaining new images cost only 0-4.4 ms per 5 s (max 0.87 ms), so the gain lies elsewhere in the free /
  re-create cycle [?, the write-up's mark].
- Open: some 4K pairs were still made new while one sat parked (likely a texture lookup first, whose geometry fields
  differ) [inferred in N-24].
- study-bloodborne-07-memory.md item 13 asks to check parking against Bloodborne's "dead images deleted before the next
  submission" rule (not done).
