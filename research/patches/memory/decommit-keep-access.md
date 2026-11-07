---
id: decommit-keep-access
title: Decommit keeps the kept view's access instead of flipping it to PAGE_NOACCESS (perf_cheap_frees)
area: memory
status: active
platforms: windows
default_windows: on
default_deck: n/a
origin: ours
class: C
commits: 6158dad, a9f6ac8
files: src/runtime_memory.c
symbols: decommit_keep_access, perf_keep_access, runtime_memory_set_perf, host_keep_view
switches: GOW3_DECOMMIT_KEEP_ACCESS, perf_cheap_frees
tests: T-M1, T-M2, T-M3
docs: fix-inventory.md#4-runtime-loader-hle-guest-memory, plan-stage-b-split.md#t-m1-table-stats-what-the-region-table-costs-2026-10-21-2125-launched-by-claude-with-the-users-go-build-c7e128e--nostats-50-gow3_host_map_stats1-log-datat-m1-table-statslog
depends_on: M1
related: L2, lockfree-region-reads, perf-patches-ini, linux-build-lab
verified: 2026-11-07
---
## What it does

M1 made a pool decommit keep its view but flip it to `PAGE_NOACCESS` (`host_keep_view`), and revoking access rewrites
page entries and flushes the TLB on every core, under the table lock, ~1,100 times a second. With
`decommit_keep_access()` on, `host_keep_view` returns after bumping the range's buckets: the view keeps the access it
had. The recommit at that address sets the new protection as before (`host_view_reusable` -> `VirtualProtect`).

## Invariants

- **Assumption (class C):** the game never touches pool memory it has decommitted. On the PS4 (and with the switch off)
  such an access faults; here a read returns stale bytes and a write succeeds silently. Worse, kept pages can be
  handed to another address (second loop of `pool_take_locked`, M1's open leak): a stray write through the old,
  still-writable view would then corrupt the other allocation with no fault to show it.
- `region_changed` in `host_keep_view` must stay before the early `return 0`: the GPU threads' region caches (L2)
  must see the decommit even though the host mapping does not change.
- The decommit still queues `HOOK_UNMAP` and enters the range as reserved; only the host protection is left alone.
- Live: `perf_keep_access` is read on every decommit; the in-game menu writes it through `runtime_memory_set_perf`
  (`gow3_overlay.cpp`, each time the Performance section is drawn; `1e657ca`, perf-patches-ini), the launcher / ini
  through `GOW3_DECOMMIT_KEEP_ACCESS=0` at start (`PERF_SWITCHES`). Any value but a leading `0` means on.
- Only reached while M1 is on (`pool_keep_views()`); on Linux `decommit_keep_access` is compiled but unused
  (`__attribute__((unused))`, `8072f54`, linux-build-lab).

## Interactions

- M1: required (the kept view exists only there); toggle bit 31 makes this switch moot.
- lockfree-region-reads / Windows `CopySparseSerial`: GPU-side copies read the guest mapping after an unlocked lookup;
  with the access kept, a copy that races a decommit reads stale bytes instead of faulting on the `PAGE_NOACCESS` view
  [inferred, not tested]. This, not M1 alone, is what the `runtime_memory_copy_backing` stub's "kept views" needs.
- perf-patches-ini: `perf_cheap_frees` is one of the launcher's performance patches (Deck preset sets it, no effect on
  Linux).

## Evidence

- T-M1 table stats (stage-b-split, build `c7e128e`, 50%, `GOW3_HOST_MAP_STATS=1`): per 5 s pool commits ~5,430
  (~69 ms), **decommits ~5,420 (~320 ms, ~60 us each)**, table lock held exclusively ~33,000 times for ~330 ms;
  kept ~= reused views (~5,500): UE4 decommits and recommits the same pages ~5-6 times a frame (~1,100 decommits a
  second, the figure in the code comment).
- T-M2 (`GOW3_DECOMMIT_KEEP_ACCESS=1`): decommits ~320 -> ~41 ms per 5 s (~60 -> ~8 us each), commits ~69 -> ~104 ms,
  both ~390 -> ~145 ms, lock held exclusively ~330 -> ~105 ms; no faults.
- T-M3 (4K, `-NoStats`, on vs off at the same spot): the user, "better with it on"; smaller high-amplitude spikes
  remained. Default since 2026-10-21 (`a9f6ac8`).

## History and pitfalls

- Opt-in in `6158dad`, default on in `a9f6ac8` the same evening. Never tried before T-M2 because M1 deliberately kept
  the fault.
- If a game is found to read decommitted memory (wrong data instead of a crash), this is the first switch to turn off.
- T-M1 / T-M2 / T-M3 here are the stage-b-split tests of 2026-10-21, not the frame-pacing T-M1 of M1.
