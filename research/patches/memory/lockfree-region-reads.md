---
id: lockfree-region-reads
title: Lock-free runtime_memory_clamp / runtime_memory_region (seqcount; spin through an edit before locking)
area: memory
status: active
platforms: all
default_windows: on
default_deck: on
origin: ours
class: G
commits: e15a859, 45eca39, 76de0f5
files: src/runtime_memory.c
symbols: vma_seq, vma_begin, vma_end, vma_optimistic, VMA_FIXED_CAPACITY, OPTIMISTIC_SPIN_CYCLES, optimistic_enabled, clamp_optimistic, region_optimistic, runtime_memory_clamp, runtime_memory_region, vma_insert, vma_erase, split_at, perf_optimistic
switches: GOW3_OPTIMISTIC_CLAMP, perf_lockfree_memory
tests: T-P2, T-P3, T-P4, T-P5
docs: fix-inventory.md#4-runtime-loader-hle-guest-memory, plan-stage-b-split.md#t-p1--t-p2-why-panning-costs-2026-10-21-0335-1940-launched-by-claude-with-the-users-go-build-166557c-logs-datat-p1-panninglog-datat-p2-panning-sampleslog-samples-research-datasamplest-p2-panning-samples
depends_on:
related: L2, R1-region-index, A1, D4, D5, M1, decommit-keep-access
verified: 2026-11-07
---
## What it does

`runtime_memory_clamp` (`e15a859`) and `runtime_memory_region` (`45eca39`) search the region table without the table
lock when their region cache misses. `vma_seq` is odd only while entries change (`vma_begin` / `vma_end` around
`vma_insert`, `vma_erase`, `split_at`), not across the host mapping calls; a reader keeps its result when `vma_seq`
was even and unchanged. The table is allocated once at `VMA_FIXED_CAPACITY` (131,072) entries and never moves; a
growth past it turns lock-free reads off for good (`vma_optimistic=0`, old table left allocated). `76de0f5`: readers
spin through an edit for up to `OPTIMISTIC_SPIN_CYCLES` (400,000 TSC cycles, ~100 us) before taking the read lock.

## Invariants

- Every in-place change of an entry's `start`, `end`, `kind` or `phys` must sit inside `vma_begin` / `vma_end`. Today
  only `split_at` edits in place (inside); `protect_locked` writes `prot` / `type` outside, which is fine because no
  lock-free reader reads them. A reader that starts using another field needs that field covered.
- The `vmas` pointer a reader loaded must stay valid: the table never shrinks or frees; growth copies to a new table
  and leaves the old one allocated. Readers clamp `vma_count` to `VMA_FIXED_CAPACITY`.
- `optimistic_enabled()` is false on a thread holding the lock exclusively (`exclusive_depth`): a writer's own nested
  lookups take the locked path.
- Torn reads are harmless only because they are discarded: nothing read in the window may be acted on (dereferenced,
  copied) before the `vma_seq` re-check.
- A lock-free answer can say "mapped" while a writer's host call is changing that mapping (the seqcount is not held
  across host calls; with the lock, the lookup waited). The copy after the lookup was never under the lock, so the
  window only widened. Callers must tolerate it: Linux reads guest memory through the backing view
  (copy-thread-backing-view); on Windows a pool decommit keeps its view (M1) with its access (decommit-keep-access),
  so a racing copy reads stale bytes; with `GOW3_DECOMMIT_KEEP_ACCESS=0` the view is `PAGE_NOACCESS` [inferred].
- Live: `perf_optimistic` is re-read on every call; the menu sets it (`runtime_memory_set_perf`); the environment is
  read only while it is still -1. A table growth past `VMA_FIXED_CAPACITY` (`vma_optimistic=0`) cannot be undone.

## Interactions

- L2: same launcher switch (`perf_lockfree_memory=0` also sets `GOW3_REGION_BUCKETS=0`, restart-only); the menu
  toggles only this part.
- R1-region-index is filled by these searches (`region_slot_fill`); A1 needs `optimistic_enabled()`, so
  `perf_lockfree_memory=0` / `GOW3_OPTIMISTIC_CLAMP=0` also turns A1 off.
- D4 moves the window where table and host differ; D5's spin matters less because most readers skip the lock.

## Evidence

- T-P2 (stage-b-split 2026-10-21, `-Samples`, 50%, panning): stage A 13% in `CollectRingBindings` ->
  `runtime_memory_clamp`, 5% of stage A blocked in `RtlAcquireSRWLockShared` behind pool commits / decommits.
- T-P3 (`e15a859`): clamp 0.46% self, no lock wait; the wait moved to `runtime_memory_region` via `CopySparseMemory`
  (4.7% of stage A blocked).
- T-P4 (`45eca39`): `CollectRingBindings` 13.4% -> **7.1%**, lock waits 6.4% -> 3.0%, stage A waiting on stage B
  26.9% -> 41.8%. Left: clamp falling back to the lock (stage A 2.2%, stage B 2.1%), edits outlasting 4 retries.
- T-P5 (`76de0f5`, 4K, `-NoStats`): the user, "quite good, the best it's been", panning included.

## History and pitfalls

- First version retried only 4 times with a pause; a table edit is a `memmove` of about half the ~13,000-entry array
  (~260 KB, tens of us), so readers fell back to the lock: hence the ~100 us spin.
- T-P1..T-P5 here are the stage-b-split tests of 2026-10-21; T-P1 / T-P2 also name frame-pacing tests (P1 dirty
  registers, P2 end-of-pipe skip), and stage-b-split's "Profile T-P3" (2026-10-19, the one tests.tsv lists) is an
  earlier stage B profile that L2 cites.
