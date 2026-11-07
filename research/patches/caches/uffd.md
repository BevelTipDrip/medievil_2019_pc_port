---
id: uffd
title: userfaultfd write protection instead of signals (GOW3_UFFD)
area: caches
status: rejected
platforms: linux
default_windows: n/a
default_deck: off
origin: bloodborne 9ab7429
class: G
commits: 9ab7429
files: gpu/shadps4/video_core/page_manager.cpp, src/runtime_file.c, src/runtime_memory.c
symbols: UffdImpl, UffdHandler, SignalImpl, PageManager, read_revoked_pages, UFFDIO_WRITEPROTECT, UFFDIO_WAKE, UFFD_USER_MODE_ONLY, touch_for_write, record_mapped, flush_hooks
switches: GOW3_UFFD
tests: Native run 5, Native run 6, Native run 7, Native run 8
docs: plan-linux-deck.md#native-run-6-gow3_uffd1-2026-11-03-appimage-a15b5cb-settings-by-extra-envtxt-log-datadeck-native-6log-trace-research-dataflipdeck-native-6csv, plan-linux-deck.md#native-run-7-gate-2-stats-2026-11-03-appimage-3efee7a-gow3_frame_stats1-by-file-log-datadeck-native-7-crashlog, study-deck-rescore-rejected.md#a-switches-that-already-exist-no-build
depends_on:
related: hot-pages, lineage-deferred-readbacks, pool-kept-pages, U5
verified: 2026-11-07
---
## What it does

With `GOW3_UFFD=1` on Linux, `PageManager` uses `UffdImpl` instead of `SignalImpl`. Guest GPU mappings are
registered with userfaultfd in write-protect mode (`OnMap`). Protection changes use `UFFDIO_WRITEPROTECT`, which
edits page-table bits under the address-space lock for reading; mprotect takes it for writing and splits mappings.
A write fault suspends the writing thread. The `gow3:Uffd` handler thread (`UffdHandler`) reads the event, runs
`rasterizer->OnWriteFault`, then wakes the thread (`UFFDIO_WAKE`). Read protection (readbacks) cannot be done
with userfaultfd: it stays mprotect (`read_revoked_pages`) and its faults still arrive as signals.

## Invariants

- Fallback: if the syscall or `UFFDIO_API` fails (no `UFFD_FEATURE_WP_HUGETLBFS_SHMEM` / `WP_UNPOPULATED` for the
  memfd guest mapping), the constructor throws and `PageManager` uses `SignalImpl`. The log line "Memory tracking
  method" says which one runs.
- Protect clears write protection with `DONTWAKE`, so every handled fault must end in an explicit `UFFDIO_WAKE`
  for its page, or the faulting thread sleeps forever.
- A file read into a write-protected page fails with EFAULT (the descriptor is `UFFD_USER_MODE_ONLY`: kernel
  copies do not reach the handler): `touch_for_write` (`src/runtime_file.c`) writes each destination page from
  user mode first (`9ab7429`). Keep it for any new kernel-side write into guest memory.
- Every new guest mapping must be registered again: `mmap(MAP_FIXED)` (a pool commit, `host_map`) replaces the
  registered VMA. `record_mapped` queues `HOOK_MAP`, and `flush_hooks` runs `RegisterMemory` -> `OnGpuMap` ->
  `UFFDIO_REGISTER`; a decommit queues `HOOK_UNMAP` (unregister). A mapping made without that hook, or a failed
  register (logged only, no fallback), leaves its pages unprotectable ("Uffdio writeprotect ... failed").

## Interactions

- hot-pages: the other inherited attempt at cheaper write tracking; both rest on the same wrong premise.
- lineage-deferred-readbacks: non-stale readbacks watch reads, which still use mprotect here. In stale mode only
  write watches remain.
- pool-kept-pages: every recommit is a new mapping, so a uffd run also pays one register per commit (cost not
  measured).
- U5 (guest memory read in place) is the Deck direction that removes the need for most write tracking.

## Evidence

- Deck Native run 6 vs run 5 (signals), Graveyard, last ~60 s, same 67% scale: avg 69.0 -> 62.4 FPS, 1% low
  47.0 -> 38.0, 0.1% low 43.2 -> 26.3, frame stddev 2.46 -> 2.84 ms. The slow frames' game excess went
  +5.32 -> +8.93 ms. "Faults" unchanged (~47.5k/s); handler time 2.0-2.2 ms/s. Reverted to signals by extra-env.txt.
- Native runs 7-8 corrected the premise: the frame stats counted pages, not signals. Real write faults are 59-233/s
  (~1 a frame, Garlic-WC only). Rescore A10: "with ~1 real fault a frame there is nothing to win".

## History and pitfalls

- Built in the Bloodborne lineage (`9ab7429`, `BB_UFFD`), inherited as `GOW3_UFFD`; Deck plan item U4.
- Run 6 was read as "a faulting thread sleeps until the handler thread wakes it" and "the ~760 faults a frame
  stretch the game's frames". Run 7 showed those were pages; why uffd made frames worse is still unexplained.
- Worth retrying only if a workload has many real write faults per frame (count signals, not the page counter)
  and the wake latency of a single handler thread is solved.
