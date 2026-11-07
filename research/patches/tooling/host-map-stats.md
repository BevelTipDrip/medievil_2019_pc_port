---
id: host-map-stats
title: GOW3_HOST_MAP_STATS: host mapping counts, table edits (bytes moved, time), lock hold time, pool commit / decommit time
area: tooling
status: diagnostic
platforms: windows
default_windows: off
default_deck: n/a
origin: ours
class: G
commits: 2618b9c79c, c7e128e58b
files: src/runtime_memory.c, research/tools/etw_waits.py
symbols: host_stats, host_stats_enabled, host_stats_record, host_stats_take, host_now_ns, HOST_STAT_ADD, write_lock_held_ns, write_lock_acquires
switches: GOW3_HOST_MAP_STATS
tests: T-H1, T-M1, T-M2
docs: plan-frame-pacing.md#t-h1-result-2026-10-18-1750-1800-launched-by-claude-with-the-users-go-build-2618b9c-log-datat-h1-hostmaplog--times, plan-stage-b-split.md#t-m1-table-stats-what-the-region-table-costs-2026-10-21-2125-launched-by-claude-with-the-users-go-build-c7e128e--nostats-50-gow3_host_map_stats1-log-datat-m1-table-statslog
depends_on:
related: M1, decommit-keep-access, lockfree-region-reads, L2, R3, linux-build-lab
---
## What it does

Windows only. `host_stats_enabled()` (`GOW3_HOST_MAP_STATS=1`, read once) turns on counters in `host_stats`:
host map / unmap / protect calls with bytes, time and max, and the views `carve_host` unmapped and mapped again.
It also counts pool views kept and reused (M1). `c7e128e` adds sorted-array edits (guest map entries and host view
regions: count, bytes moved, time), pool commit / decommit count and time, and the exclusive hold time of the
region table lock (`write_lock_held_ns`, `write_lock_acquires`, taken in `write_lock` / `write_unlock`).
`host_stats_record` prints "Host map stats: slow ..." for any call over 1 ms when it happens. Every 5 s it prints
two "Host map stats (5 s)" lines. `2618b9c` also added `research/tools/etw_procs.py`, `etw_sysfn.py` and
`etw_waits.py` for WPR traces of the pacing episodes.

## Invariants

- Measuring only. Off, it costs one cached load per host call and no clock read (`write_lock` reads the clock only
  when enabled).
- The 5 s report is printed by whichever thread makes the host call that crosses the boundary (a CAS on
  `host_stats_next`). That can be a game thread inside a pool commit, possibly under the table lock. Its `printf`
  cost lands there: keep it off for pacing runs.
- With no host calls no report is printed (the window only ends on a call).
- The lock hold time covers only the outermost exclusive acquire (`exclusive_depth`); nested writes count once.
- On Linux (since `8072f54`, linux-build-lab) `host_stats_enabled()` returns 0 and `HOST_STAT_ADD` compiles away:
  the switch does nothing there.

## Interactions

- T-H1 is the measurement behind M1. T-M1 (table stats) behind decommit-keep-access (`GOW3_DECOMMIT_KEEP_ACCESS`).
- Its lock-hold numbers sized the region table work (L2, lockfree-region-reads, R3).

## Evidence

- T-H1 (steady play, per 5 s): ~60,000 host maps, ~30,000 unmaps, ~30,000 protects, ~300 ms of unmap time,
  regions 4,734 at boot growing to ~14,900. In the episode the same 64 KiB maps / unmaps took 1-17 ms each (~150
  slow calls, unmap time 1,452 ms per 5 s). The 2:1 map:unmap ratio pointed at pool commit / decommit (M1).
- T-M1 (table stats): decommits ~5,420 per 5 s (~320 ms, ~60 us each); guest-map edits ~2.2 us each ("the sorted
  array is not the cost"); table lock held exclusively ~33,000 times for ~330 ms (6.6% of the time).
- T-M2 (`GOW3_DECOMMIT_KEEP_ACCESS=1`): decommits ~320 -> ~41 ms per 5 s, lock held ~330 -> ~105 ms. Then T-M3:
  the user, "better with it on".

## History and pitfalls

- The episodes were a System worker trimming the guest memory section (`MiTrimSection` / `MiViewMayContainPage` in
  WPR) while the render thread waited on a kernel push lock. The ETW tools in this commit read those traces.
- Not verified in game: diagnostics only.
