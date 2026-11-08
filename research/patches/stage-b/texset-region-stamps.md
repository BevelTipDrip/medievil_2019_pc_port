---
id: texset-region-stamps
title: Texture-set memo valid by 4 MiB region stamps, checked only after a registry change, two slots per key
area: stage-b
status: active
platforms: all
default_windows: on
default_deck: on
origin: ours
class: G
commits: 641134c, 2492874, 61440d7, c44a640
files: gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp, gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.h, gpu/shadps4/video_core/texture_cache/texture_cache.cpp, gpu/shadps4/video_core/texture_cache/texture_cache.h, scripts/graphics_options.py
symbols: TextureSetRegions, TextureSetTwoWay, TextureSetStamp, BindTexturesFromSetIn, KeepTextureSet, RegionStamp, BumpRegions, region_gens, RegistryGeneration, PERF_SWITCHES
switches: GOW3_TEXSET_REGIONS, GOW3_TEXSET_TWO_WAY, perf_texture_memo
tests: T-S1a, T-S1b, T-S1c, T-W3d
docs: fix-inventory.md#2-draw-translation-cost-stage-b-per-draw, plan-stage-b-split.md#t-s1a-texture-binding-split-2026-10-20-2312-launched-by-claude-with-the-users-go-build-60d27d4-log-datat-s1a-texture-sectionslog, plan-stage-b-split.md#t-s1b-region-stamps-2026-10-20-2322-2327-launched-by-claude-with-the-users-go-build-641134c-log-datat-s1b-region-stampslog, plan-stage-b-split.md#t-s1c--t-w3-texture-memo-two-way-the-write_data-wait-2026-10-20-2331-2345-launched-by-claude-with-the-users-go-builds-2492874-d67ac70-logs-datat-s1c-two-waylog-datat-w3-writedata-targetslog, plan-stage-b-split.md#t-w3d-w3-ab-without-diagnostics-2026-10-21-0012-0028-launched-by-claude-with-the-users-go-build-807d4c8--nostats
depends_on: lineage-texture-set-memo
related: texset-compact, T2, lineage-draw-preparation, findimage-generation-misses
checked: 2026-11-07
---
## What it does

The inherited texture-set memo voided every set whenever any image anywhere was (un)registered
(`RegistryGeneration`). The texture cache now also counts (un)registrations per 4 MiB region (`BumpRegions`,
`region_gens`, 65,536 regions wrapping). A kept set stores the sum of its entries' `RegionStamp`s, over each bound
image's range and each T#'s range, plus the generation. In `BindTexturesFromSetIn` an unchanged generation needs no
stamp. A changed one sums the stamps again; if they match, the set takes the new generation. Two ways: a key lives
in slot `key % slots` or its neighbour `^ 1`, and the less recently used one is replaced.

## Invariants

- Every registration and unregistration of an image calls `BumpRegions` over its guest range and changes
  `RegistryGeneration` (`texture_cache.cpp`, both sites). A new path that adds or removes an image without both lets
  a stale set bind a freed or replaced image's view.
- Region counters only grow (`fetch_add` on `u32`s, summed into a `u64`), so an unchanged sum means no region of
  the set changed. A decrement, or a reset of `region_gens`, would break that.
- `RegisterImage` / `UnregisterImage` raise `registry_generation` before they call `BumpRegions`, and
  `BindTexturesFromSetIn` reads the generation before it sums the stamps. A lookup running beside a
  (un)registration could see the new generation with the old stamps and confirm a stale set for good. That is safe
  only while images are (un)registered on the thread that binds (or with it idle); swapping the two lines in both
  functions would make it safe regardless [from the code; concurrency of every path not checked].
- A set is kept only if no image was (un)registered during its own bind (`RegistryGeneration() == bind_generation`
  in `BindTextures` when regions are on). Otherwise the stamp could be taken after a change it missed.
- The stamps cover the T#'s guest range as well as the bound image's. A new image appearing under a T# that resolved
  to an older, larger image must still miss.
- A hit still runs the per-image checks (backing, `needs_rebind`, `is_target`, `IsUpToDate`, proxy, upscaler
  redirect). Stamps only replace the generation test.

## Interactions

- lineage-texture-set-memo is the memo itself, inherited. lineage-draw-preparation supplies the prepared T# hashes
  the key is built from.
- texset-compact keeps the same decisions in another layout; the stamp there is `TextureSetStamp(u32)`.
- T2 (wide sets, unprepared stages) uses the same validity rules.
- `perf_texture_memo=0` sets both switches to 0. The in-game menu shows the patch from `GOW3_TEXSET_REGIONS` only.
- The memo is off entirely with first word 2^45 (`TextureSetMemo`).

## Evidence

- T-S1a: 47% of misses were "generation", one streamed texture voiding every set (the panning link).
- T-S1b (`641134c`): hit rate 83.6 -> 92.3%, generation misses ~15,300 -> ~48 a window. But the hit path rose
  0.109 -> 0.137 us, because every hit summed stamps. Hence "stamps only after a registry change".
- T-S1c (`2492874`): textures 0.396 -> 0.369 us a packet, hit rate 92.4%. The user: no texture problems (T-S1b,
  T-S1c).
- T-texset-both-off (in the T-W3d write-up, build `61440d7`, `-NoStats`, both switches 0): the user, "too much run
  to run variance at this level, but it looks like the memo work is worth it"; 1% and 0.1% lows worse without it
  (single feel runs, no numbers logged). Kept on.
- Deck (patch audit): 99.3% hits; a miss costs ~15 us.

## History and pitfalls

- **Ordering fix built `c44a640` (2026-11-07, LANDMINES item 11, found in code), not run:** `RegisterImage` / `UnregisterImage` bumped
  the generation before the region stamps; a lookup in between could confirm a set at the new generation with the
  old stamps, and later lookups at that generation skip the stamp check. Stamps now first. Keep that order.
- Two-way did not reduce the "other key" misses (~14,500 a window). They are new sets (~5.5% of draws), not slot
  conflicts. It is kept as harmless.
- The lesson of T-W3d: at ~175-185 FPS single feel runs differ by about as much as such changes. Judge small stage
  B changes by logged same-route or same-launch A/Bs.
