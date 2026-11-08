---
id: R1-region-index
title: Region cache index of 1,024 entries by page, filled by the lock-free searches too (Deck)
area: memory
status: active
platforms: all
default_windows: off
default_deck: on
origin: ours
class: G
commits: ccfa08a
files: src/runtime_memory.c, gpu/shim/gow3_toggles.h
symbols: region_slots, REGION_SLOTS, region_slot, region_slot_fill, region_index_wanted, region_index_on, TOGGLE2_REGION_INDEX_OFF, cached_region, region_cache_count, region_slots_free, clamp_optimistic, region_optimistic, RegionIndex
switches: GOW3_REGION_INDEX, toggle2:RegionIndex
tests: Launch 15
docs: impl-stage-b-per-draw-2.md#r1-region-cache-index-ccfa08a-gow3_region_index1-second-word-524288-off, plan-linux-deck.md#per-draw-cuts-r1--r2--t1--t2-on-the-deck-2026-11-05-appimage-c71e87c-drain-hybrid-on-gow3_texset_wide1-gow3_region_index1-frame-stats-without-sections-log-datadeck-perdraw-1log, plan-linux-deck.md#launch-15-results-2026-11-06-2050-2128-build-2a6efea-signal-a-new-arena-with-a-guaranteed-heavy-view-the-old-spot-cannot-be-found-under-a-cap-gpu-auto-log-datadeck-launch15log-traces-research-datadeckdeck-cap60-signalcap60-cutsuncap-cuts-
depends_on: L2, lockfree-region-reads
related: A1, deck-preset, R2-residency-bitmap, T1, T2
verified: 2026-11-07
---
## What it does

The 8-entry region cache is filled only on the locked path, and since the lock-free searches almost every lookup
takes one of those, so it stayed nearly empty: each vertex range and buffer binary-searched ~13,000 regions. With
`GOW3_REGION_INDEX=1` (opt-in) `cached_region` first tries one of `REGION_SLOTS` (1,024) per-thread entries picked by
the address's 64 KiB page (`region_slot`). `clamp_optimistic` / `region_optimistic` fill it (`region_slot_fill`) with
the region they found, its bucket stamp and (since A1, `be214c8`) its backing offset `phys`. Second toggle word
524288 switches it off live.

## Invariants

- A fill is kept only when `table_generation` was the same even value before and after the search
  (`region_slot_fill`): no writer held the lock meanwhile, so the entry is one `cache_region` could have made under the
  read lock. The stamp is read inside the `vma_seq` window. Breaking either lets the index hold a region a writer
  changed during the search.
- An entry answers only while `stamp == region_stamp(start, end)` and `stamp != 0` (the L2 rule: buckets bumped
  before any host or table change).
- `region_index_wanted` requires `region_buckets_on()`: `GOW3_REGION_BUCKETS=0` (or `perf_lockfree_memory=0`) turns
  R1 off; first-word bit 1 (no region cache) does too. With `GOW3_OPTIMISTIC_CLAMP=0` nothing fills it.
- Per thread, allocated on first fill, freed at thread exit (`region_slots_free` via a pthread key, pointer cleared).
- Counters are per-thread single-writer (`region_cache_count`), summed in the "Region cache per frame (R1 ...)" line.

## Interactions

- L2 / lockfree-region-reads: required (stamps; the searches that fill it).
- A1: its fast path reads the backing offset from this index; without R1 every A1 copy does its own lock-free search.
- deck-preset: `GOW3_REGION_INDEX=1` is in `DECK_SWITCHES`, which the launcher applies on any platform
  (`deck_switches`, `setdefault`): Deck Optimized on a Windows PC turns R1 on there too, untested on Windows. Without
  the preset it is off everywhere.
- R2-residency-bitmap, T1, T2: the other per-draw cuts, measured together (bits 65536 + 131072 + 262144 + 524288).
- Not the readback-submit R1 (R1-readback) nor the reverted stale-EOP skip (R1-stale-eop).

## Evidence

- Cuts redo (2026-11-07, draw preparation alive): R1/R2/T1/T2 off (983040) +3.8 points of a core at the 60 cap (stage B 50 -> 53%), +0.17 W CPU; uncapped 82.8 -> 81.7 FPS, presents > 15 ms 54 -> 89 a minute, worst 20.4 ms. Launch 15's +14.6 was inflated: with no prepared draws every draw took T2's path.
- Deck per-draw 1 (AppImage `c71e87c`, R1/R2/T1/T2 all on vs off, `0 0` vs `0 983040`, 30 s x 4): 78.8 vs 78.2 FPS,
  p99 16.4 vs 16.5 ms, stage B busy 86.7 vs 92.6% (~0.75 ms a frame freed); **R1 14.3k index hits a frame** (13.6k
  lock-free searches with it off). Single bits off (20 s x 3): 78.7-79.0 FPS, within noise.
- Launch 15 (60 cap, R1/R2/T1/T2 off): process CPU +14.6 points of a core, CPU power +0.00-0.03 W; uncapped 77.6 vs
  78.3 FPS, stage B 81.2 vs 76.0%.
- No unit test of its own (impl doc); A1's `runtime-test --backing-copies` compares copies before and after the clamp
  filled the index. Not verified in game on its own (no visual check of it is recorded).

## History and pitfalls

- Opt-in because it changes the runtime's memory core used by stage A, stage B and the copy threads alike; measured
  on the Deck only, so it ships in the preset, not as a Linux or Windows default.
- The ID R1 is used for three different patches (this, R1-readback, R1-stale-eop).
