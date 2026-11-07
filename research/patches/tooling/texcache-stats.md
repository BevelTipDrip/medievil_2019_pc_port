---
id: texcache-stats
title: GOW3_TEXCACHE_STATS: per 5 s images created / freed, lookups, overlaps, expands, uploads, re-created and re-uploaded images
area: tooling
status: diagnostic
platforms: all
default_windows: off
default_deck: off
origin: ours
class: G
commits: c4c74f608c, 403555681a, 9a3127a548, e385484834, 40a7825cab
files: gpu/shadps4/video_core/texture_cache/texture_cache.cpp, gpu/shadps4/video_core/texture_cache/texture_cache.h
symbols: CacheStats, cache_stats, CacheStatsEnabled, CACHE_STAT, RecreateKey, NoteMipTarget, RunGarbageCollector, expand_sites, create_sites, refresh_sites, refresh_sites_mutex, unmapped_recently, stats_reports
switches: GOW3_TEXCACHE_STATS
tests: T-C0, T-C1, T-C2, T-P2, N-10, N-11, N-22, N-23
docs: plan-frame-pacing.md#r1-reverted-performance-degrades-with-new-content-2026-10-18-1440-1510, plan-frame-pacing.md#t-c0-result-2026-10-18-1510-1515-the-cache-re-creates-and-re-uploads-20-images-per-frame, plan-frame-pacing.md#t-c2-result-2026-10-18-1850-1853-launched-by-claude-with-the-users-go-build-9a3127a-log-datat-c2-recreatelog--times-ring-trace-pan-1etl-saved-1851416, plan-frame-pacing.md#n-10-what-the-image-upload-bursts-are-2026-10-28-launched-by-the-user-t-up1-refresh-siteslog-gow3_texcache_stats1
depends_on:
related: V3, C3, new-images-undefined, clear-skips-upload, park-unmapped, micro-mip-targets, P2, upstream-texture-gc, mausoleum-toggle-bisect, pool-kept-pages
---
## What it does

`CacheStatsEnabled()` (`GOW3_TEXCACHE_STATS=1`, read once) turns on counters (`CACHE_STAT`) and lists that
`TextureCache::RunGarbageCollector` prints every 5 s after its collector run:
- `c4c74f6`: "Texture cache stats (5 s)": live / registered / unregistered images, fast and slow lookups, overlaps,
  expands, uploads, alias syncs / copies, volume views, GPU memory against the collector's trigger.
- `4035556`: the top 8 expanded images with the reason (`expand_sites`).
- `9a3127a`: images freed by guest unmaps and created again within ~30 s (`RecreateKey`, `unmapped_recently`, 6
  reports), and the busiest created images by description and binding (`create_sites`).
- `e385484`: images uploaded again (`RefreshImage`) by description and dirty flag (`refresh_sites`).
- `40a7825`: the refresh list names the first use's caller and whether the image was Undefined.
Later counters: undefined / cleared targets, parked images (park-unmapped), image creation time, end-of-pipe waits
and skips, and "Mip target" lines (`NoteMipTarget`, micro-mip-targets).

## Invariants

- Off, each `CACHE_STAT` is one load of a static bool. On, the lists build a `std::string` key per event
  (`refresh_sites` under its own `refresh_sites_mutex`): real work on the GPU threads.
- The report prints with `printf`, its lists under `mutex`, on the thread that runs the collector
  (`Rasterizer::OnSubmit`: stage A, or stage B with E1-deck-drains).
- `unmapped_recently` grows while on and is pruned only by the report (entries older than 6 reports).
- `NoteMipTarget` prints each kind once and keeps at most 256 keys.
- Must not change cache behaviour. `unmapped_addresses` (C3) is filled whether stats are on or not; only
  `unmapped_recently` is stats-only.

## Interactions

- T-C0 / T-C1 found the 2D-array / 3D expand ping-pong that V3 fixed. T-C2's re-created shadow maps and 4K targets
  led to C3, and later new-images-undefined and park-unmapped. N-10 / N-11's refresh list chose
  new-images-undefined over clear-skips-upload (0 hits).
- `e385484` also made P2 opt-in (`GOW3_IDLE_EOP_SKIP=1`).
- The "Texture cache: memory pressure / collecting" line (upstream-texture-gc) is separate and also prints without
  this switch.

## Evidence

- T-C0 (after progressing, start area): ~850 live images, ~9.2k created = expanded per 5 s (~20 a frame), ~29.6 GB
  uploaded per 5 s (~5.9 GB/s), ~91 FPS. The user: "GPU sitting at 52C, but usage is 80%".
- T-C1: every in-game expand was a volume flipping between 2D array and 3D. The fog grid alone was 976 expands per
  5 s (15.6 GB).
- T-C2: 512x512 D16 shadow maps re-created 67 times in one window; 4K targets ~860 MiB per 5 s re-created.
- N-10: the 33-100 MB frames were 4K render-target-pool images first used as a non-target (~446 MB), not mainly
  texture streaming (~150 MB).

## History and pitfalls

- Hilltop Mausoleum (Deck): one run "repaired itself" with `GOW3_TEXCACHE_STATS=1`, not reproducible (memory reuse
  luck; the cause was pool-kept-pages). Different allocation timing with the switch on can hide memory-content bugs.
- N-23: with `GOW3_PERF_DIAG` + `GOW3_TEXCACHE_STATS` the 8-9 ms view-switch frames did not appear (timing).
- Not verified in game: diagnostics only.
