---
id: pool-prefill
title: Deck memory #1: guest pool prefill at boot and populate per pool commit (Linux, opt-in)
area: memory
status: rejected
platforms: linux
default_windows: n/a
default_deck: off
origin: ours
class: G
commits: a863482
files: src/runtime_memory.c
symbols: pool_prefill_start, pool_prefill_thread, pool_prefill_init, pool_prefill_left, pool_prefilled_bytes, pool_populate, pool_populate_init, pool_populate_on, pool_populates, runtime_memory_lock_report
switches: GOW3_POOL_PREFILL_MB, GOW3_POOL_POPULATE
tests:
docs: impl-deck-build-sched-pool.md#pool-prefill-and-populate-gow3_pool_prefill_mb-gow3_pool_populate, study-deck-memory-budget.md#25-guest-memory-first-touches-and-recommits-the-ram-item-that-pays-code-log, plan-linux-deck.md#round-1-results-gpu-clock-auto-2026-11-05-ini-logging-67-uncapped-launched-by-the-user
depends_on:
related: R3, D4, pool-kept-pages, A1
checked: 2026-11-07
---
## What it does

Each Linux pool commit maps a new view of the memfd, so every 4 KiB page the game then touches faults once, and a
page the memfd never held is allocated and zeroed in that fault (area loads on the Deck: 68-86k faults in ~1 s).
- `GOW3_POOL_PREFILL_MB=N`: after `pool_expand`, a detached `gow3:PoolFill` thread `fallocate`s (mode 0) the lowest
  N MiB (total budget, max 16384) of the pool's physical pages in 16 MiB steps.
- `GOW3_POOL_POPULATE=1`: each commit run gets one `madvise(MADV_POPULATE_WRITE)` (`_READ` for read-only commits;
  no-access ones skipped) after the commit leaves its locks and before `flush_hooks`. Probed once (Linux 5.14+).
Both off by default; the "Guest pool faults (window)" line (`GOW3_FRAME_STATS`) shows faults, populates, MiB prefilled.

## Invariants

- Nothing the guest can see changes: prefill only allocates holes (which read as zero anyway), populate follows the
  view's current protection and fails harmlessly (counted) where a hook or a racing decommit took access first.
- `pool_populate` must stay outside `pool_map_mutex` and the table lock (it costs ~2 ms per 4 MiB in WSL) and before
  the GPU hooks, in both the locked and the D4 paths.
- Prefill only helps because commits take the lowest free pages first (`free_hint`, R3); kept pages (pool-kept-pages)
  now make most recommits reuse pages the memfd already holds, so prefill's share shrank further [inferred].
- The kernel counts each populated page as a minor fault: judge populate by hitch frames and populate ms, not faults.

## Interactions

- R3: the lowest-first order it relies on. D4: populate sits after D4's unlocked steps.
- pool-kept-pages: `pool_zero_foreign_locked` / failed commits punch holes, which undo prefill for those pages.
- A1: prefill's `fallocate` runs without the table lock; safe for lock-free copies because it only fills holes,
  which read as zero either way. Both switches are read once (`pthread_once`): restart only.

## Evidence

- WSL scratch test (4 MiB commit, 1,024 touches): off ~2.0 us per page, prefill ~1.1 us, populate moves the cost into
  the commit (2,044-2,383 us), prefill + populate ~0.8 us.
- Deck round 1, launch 6 (`GOW3_POOL_POPULATE=1`, GPU auto, static heavy view): 78.4 FPS, worst 17.95 ms, p99 16.54 ms
  (launches 1-3 of the round: 79.3-79.4); **faults 9k -> 437k per 5 s, 537 ms per 5 s populating**: rejected in the static view.
- Prefill: never launched; dropped because demand faults were only ~1.8k/s in play.
- Round 2 note: between-launch differences under ~1.5 FPS (populate -1) are inside one launch's drift.

## History and pitfalls

- Populate fills pages UE4 never touches (64 KiB commits ~6,000 a second), so in a still view it is pure cost.
- Not tested in motion or during area loads, where the 68-86k-fault hitches are (the user: still-camera rejects may
  help in panning / combat). Worth retrying only there: prefill (2.5-3 GiB, the study's N) with populate, judged by
  the area-load hitch length and the 0.1% lows while turning, not by a static view's FPS.
- Windows: not done (the pagefile-backed section has no cheap, clearly safe equivalent).
