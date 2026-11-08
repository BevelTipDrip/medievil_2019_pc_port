---
id: findimage-generation-misses
title: FindImage generation-only misses served by region stamps (study item 10, not built)
area: caches
status: rejected
platforms: all
default_windows: n/a
default_deck: n/a
origin: ours
class: G
commits:
files: gpu/shadps4/video_core/texture_cache/texture_cache.cpp, gpu/shadps4/video_core/texture_cache/texture_cache.h
symbols: TextureCache::FindImage, find_image_cache, registry_generation, RegionStamp, BumpRegions, still_exact
switches:
tests: N-28
docs: plan-frame-pacing.md#n-28-stage-b-counters-the-studys-step-0-2026-10-30-launched-by-claude-with-the-users-go-t-sb0-counterslog-build-ae09fcc-gow3_sections1, fix-inventory.md#10-tried-and-dropped, study-deck-rescore-rejected.md#c-analysed-gated-or-ranked-never-built
depends_on:
related: texset-region-stamps, stage-b-counters
checked: 2026-11-07
---
## What it does

Never built. `TextureCache::FindImage` memoizes lookups in `find_image_cache` (1,024 entries). An entry is valid
while `registry_generation` is unchanged, which any image registration or unregistration bumps, or, for an exact
image (no view mip / slice), while `still_exact()` holds. A "generation-only miss" has a matching key and would
return the same image, but fails only because the generation moved. The idea: validate entries by
`RegionStamp` (4 MiB region sums, as the texture-set memo does) instead of the global generation.

## Invariants

- A stamp-validated hit is correct only if every registration or unregistration that could change the lookup's
  result bumps a region the lookup's range covers. Today that holds: the slow path's candidates are exactly the
  images overlapping the lookup range (`ForEachImageInRegion(info.guest_address, info.guest_size)`), and
  `RegisterImage` / `UnregisterImage` call `BumpRegions` over the whole image, which covers that range for any
  candidate, a view's parent included. A slow path that ever looks beyond the range breaks this.
- The stamp must be taken under the cache mutex after the slow path (which may itself register images), where the
  entry and its generation are stored today. The 4 MiB regions wrap (`RegionCount`): only extra misses, never
  a wrong hit.
- Exact render targets already survive generation bumps through `still_exact()`. Study item 10: "the target-memo
  half is worthless".

## Interactions

- texset-region-stamps: the same mechanism (`RegionStamp`, `region_gens`) built for texture-set memos (T-S1a),
  where one streamed texture had invalidated every set.
- stage-b-counters: N-28's counter (generation-only misses per frame) is what dropped it.

## Evidence

- Study estimate (study-frame-cost item 10): 0.05-0.15 ms on the 6.9-8 ms frames, lows +1-3 FPS. The direct
  slope is 1.4-1.5 us per lookup. Gate: build only at >= 50 generation-only misses per slow frame.
- N-28 (2026-10-30, PC): 3 per ordinary heavy frame, 9 per slow one: "Dropped". Render-state spikes were 0.8% of
  heavy frames, all in target creation, with cache lock waits of 0.00-0.06 ms.
- Deck rescore C7: 0.03-0.09 ms estimated, "Holds: too few on the Deck too". Not measured on the Deck.

## History and pitfalls

- Dropped by its own gate. Worth retrying only if a counter shows >= 50 generation-only misses per frame (a scene
  with heavy texture streaming, or after a change that bumps the generation more often).
