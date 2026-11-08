---
id: hot-pages
title: Hot pages: pages written again and again stay writable and count as always CPU-modified (GOW3_HOT_PAGES)
area: caches
status: rejected
platforms: all
default_windows: off
default_deck: off
origin: bloodborne b90a781
class: G
commits: 1366635, 98aaa22, b90a781
files: gpu/shadps4/video_core/buffer_cache/region_manager.h, gpu/shim/gow3_toggles.h
symbols: HotPagesEnabled, CountWriteFaults, KeepHotPagesModified, HotFaults, HotPeriod, write_faults, num_hot, HotPages
switches: GOW3_HOT_PAGES, toggle:HotPages
tests: Native run 6, Native run 7, Native run 8
docs: plan-linux-deck.md#native-run-6-gow3_uffd1-2026-11-03-appimage-a15b5cb-settings-by-extra-envtxt-log-datadeck-native-6log-trace-research-dataflipdeck-native-6csv, plan-linux-deck.md#native-run-7-gate-2-stats-2026-11-03-appimage-3efee7a-gow3_frame_stats1-by-file-log-datadeck-native-7-crashlog, study-deck-rescore-rejected.md#a-switches-that-already-exist-no-build, plan-work-queue.md#found-on-the-way-2026-10-30
depends_on:
related: uffd, D2, gpu-write-twins, fault-page-download, S2, U5
checked: 2026-11-07
---
## What it does

`RegionManager::CountWriteFaults` counts, per tracked page, each time a write-protected page is marked
CPU-modified. With `GOW3_HOT_PAGES=1` a page marked more than `HotFaults` (2) times becomes hot (`hot` bits).
`KeepHotPagesModified`, run on every upload (`ForEachModifiedRange` with clear), ORs the hot pages back into
`cpu`, so they stay writable (no fault) and are re-uploaded on every use. The set is per `RegionManager`: it is
cleared on the first upload in that region more than `HotPeriod` (20 s) after its first hot page, and re-protected
then.

## Invariants

- Off unless `GOW3_HOT_PAGES=1`. Toggle bit 32768 (`HotPages`) switches it off live: the next upload clears the
  set and re-protects the pages. A hot page must never lose its CPU bit while hot: it is unprotected, so its
  writes are not seen any other way.
- What it counts is not faults: `CountWriteFaults` runs from `ChangeRegionState<CPU, true>` for every protected
  page marked CPU-modified from any source: the faulting page itself (`OnWriteFault` invalidates 8 bytes), guest
  unmaps (pool decommits: `MarkRegionAsCpuModified` over the whole range), invalidations, precise writes. The rest
  of a fault window is not counted: `RegionManager::ExtendWriteFault` sets those `cpu` bits directly. So pages the
  runtime invalidates or unmaps also become "hot".
- `HotPeriod` must stay long: a 2 s period re-protected thousands of pages at once, each faulting again before it
  was hot (~20k faults/s and frame spikes, code comment).
- `Gow3Stats::tracker_faults` / `hot_pages` are the counters; the fault statistic counts pages, not signals.

## Interactions

- uffd: the other inherited attempt to make write tracking cheaper; same premise, also rejected.
- S2 binds a small buffer from the arena only if `!IsRegionCpuModified` (`ObtainBuffer`); hot pages are always
  CPU-modified, so such buffers end in `stream_class` as dynamic and keep the stream copy.
- D2, gpu-write-twins, fault-page-download: the active ways write tracking was made cheaper instead.
- U5: the Deck direction that removes write tracking for buffers read in place.

## Evidence

- Lineage test scene (Bloodborne port, `b90a781`, code comment): the set grew to ~14k pages (56 MB), each
  re-uploaded on every binding. Result 33 FPS and a GPU ring timeout. Never measured in MediEvil.
- Deck Native run 8: real write faults ~1 a frame (Garlic-WC only, 25-81 distinct pages per 5 s, none every
  frame). The "~760 faults a frame" were pages marked (Native run 7 correction). Rescore A9: "~0 gain, large cost".

## History and pitfalls

- Built (`1366635`), tuned to 2 faults / 20 s (`98aaa22`), made opt-in (`b90a781`) in the Bloodborne lineage.
  Inherited here as `GOW3_HOT_PAGES` after the `bb` -> `gow3` rename.
- The open PC idea (plan-work-queue.md): pages that fault every frame stay writable but are uploaded once a
  frame, not per binding. It needs a fault map first. Its premise (~760 faults a frame on the PC, 530-760 in PC
  logs) came from the same per-page counter. Count real signals on the PC before building it.
