---
id: micro-mip-targets
title: HZB small mips rendered: a 1D-thin render target for a micro-tiled mip is a view of the parent (N-22)
area: caches
status: active
platforms: all
default_windows: on
default_deck: on
origin: ours
class: G
commits: 464d9258d6
files: gpu/shadps4/video_core/texture_cache/image_info.cpp, gpu/shadps4/video_core/texture_cache/texture_cache.cpp
symbols: ImageInfo::MipOf, micro_mip_of_macro, GetMacroTileExtents, NoteMipTarget, TextureCache::ResolveOverlap
switches: GOW3_MICRO_MIP_TARGETS
tests: N-22, N-23
docs: fix-inventory.md#5-correctness-and-stability, plan-frame-pacing.md#n-22-the-hzbs-small-mips-were-never-rendered-2026-10-30-launched-by-claude-with-the-users-go-t-hzb1-mip-targetslog-t-hzb2-micro-mipslog
related: hzb-remap, finer-occlusion-culling, park-unmapped, texcache-stats, perf-diag
checked: 2026-11-07
---
## What it does

UE4 4.21 builds its HZB (2048x2048 R16F at 4K, 11 mips, 2D-thin tile mode) one mip per render pass. shadPS4 lays out
mips smaller than a macro tile as micro-tiled (`ImageSizeMacroTiled`), and the guest's colour buffer for such a mip
has the 1D-thin array mode. `ImageInfo::MipOf` rejected any array-mode difference, so mips 5-10 (64x64 down) became
images of their own. The HZB's own small mips held nothing rendered, and every large box read visible. Now `MipOf`
accepts that one case (`micro_mip_of_macro`) as a view of the parent's mip.

## Invariants

- Accepted only when all of these hold: the formats are compatible with equal bits and samples (`IsCompatible`),
  the parent is macro-tiled with thickness 1 and not a block format, the target is `Array1DTiledThin1` with one
  level, and the mip is one the parent's own layout made micro-tiled (`mip != 0` and the mip's width or height,
  pow2-rounded when the parent is pow2, is below `GetMacroTileExtents`). Anything else is rejected as before.
- The mip-size rule here must match the layout rule that downgrades mips (`UpdateSize` ->
  `ImageSizeMacroTiled`: `max(pitch >> mip, 1)`, `bit_ceil` when pow2, micro when `mip > 0` and either side is
  below the macro tile). Checked equal on 2026-11-07. If one changes without the other, targets alias the wrong
  bytes, or the small mips drift apart again and culling turns lenient.
- `GOW3_MICRO_MIP_TARGETS=0` (read once) brings back the strict array-mode match.

## Interactions

- It makes the HZB occlusion (hzb-remap) cull for real, and finer-occlusion-culling's scene-size HZB uses the same
  path. Before it, HZB culled ~17% of draws where hardware queries culled 63-68%.
- With real culling, view switches showed spikes from targets re-created through guest unmaps (N-23). park-unmapped
  (N-24) fixed those.
- The same commit added the `GOW3_TEXCACHE_STATS` "Mip target" lines (`NoteMipTarget`, texcache-stats), the HZB remap
  line's hidden counts, and `GOW3_LONG_WAITS` (perf-diag).

## Evidence

- N-22 t-hzb1 [V]: mips 1-4 were views, mips 5-10 never resolved. t-hzb2 [V]: all "view of the mip". 30-67% of
  tested keys hidden in gameplay. Same route: ~850-1,460 draws vs ~1,150-2,120, 186-237 FPS vs 162-215, p99 4.4-7.1
  vs 5.6-10.0 ms. Overlay 217.7 / 184.5 / 164.9 (avg / 1% / 0.1%) vs 207.6 / 170.6 / 94.3. Screenshots: no missing
  geometry.
- t-hzb3-clean, the user: "the 1% and .1% lows are higher in the fight, and significantly higher almost
  everywhere else."
- Deck: on, never A/B'd alone. The audit lists lows "significantly higher" (PC).

## History and pitfalls

- Found because the user saw culling "very gentle" behind the alley fight. The remap lost under 1% of keys, so the
  leniency had to be in the results texture.
- Not checked on other levels at the time (N-22 "not checked yet"). Any game that renders into its own mips one by
  one gains from it (class G).
