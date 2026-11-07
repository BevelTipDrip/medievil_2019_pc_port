---
id: L1-region-buckets
title: Region caches checked per 16 MiB bucket, not table-wide (reverted)
area: memory
status: reverted
platforms: all
default_windows: n/a
default_deck: n/a
origin: ours
class: G
commits: 0705e49, bc60b8b
files: src/runtime_memory.c
symbols: region_buckets, region_changed, region_stamp, cached_region
switches:
tests: T-L1
docs: fix-inventory.md#10-tried-and-dropped, plan-frame-pacing.md#step-1-heavy-view-stage-b-2026-10-19-the-user-1-then-2
depends_on:
related: L2
verified: 2026-11-07
---
## What it does

Tried 2026-10-19 (`0705e49`), reverted the same day (`bc60b8b`). The GPU threads' region caches were valid for one
table-wide generation bumped at every exclusive table lock, so UE4's ~6,000 pool commits / decommits a second emptied
them. L1 kept 65,536 hashed generations of 16 MiB buckets, bumped in `vma_insert` / `vma_erase` / `split_at`; a cached
region stayed valid while its buckets were unchanged. It still gave no cached answer while a writer held the lock.
Toggle bit 62 (4611686018427387904) turned it off.

## Invariants

- (Code gone; the same rules now live in L2.) The buckets had to be bumped by every table edit of a range, and no
  cached answer could be given while a writer held the lock, because writers changed host mappings before the table.
  The bucket count, the 16 MiB shift and `region_stamp`'s 64-bucket limit came back unchanged in L2.

## Interactions

- L2 re-applied the same buckets (`03c363b`) and added the rule that made them pay: buckets bumped before the host
  mapping changes, so caches answer while a writer holds the lock.
- Bit 62 was later reused by B1; do not read old notes about bit 62 as this patch.

## Evidence

- T-L1 (frame-pacing 2026-10-19 04:08-04:14, build `0705e49`, live A/B at a heavy spot, ~2,900 draws a frame): on
  123-125 FPS, median 7.9-8.2 ms, p99 10.2-10.9 ms; off 120-126 FPS, 7.8-8.1 ms, 10.0-10.4 ms; stage B ~91% busy
  both. The user: roughly the same, 1% lows the same.

## History and pitfalls

- Why it gave nothing: in a steady view the 8-entry cache already hit; `vma_index`'s share (~5% of both GPU threads
  in P2a) came from lookups the cache did not serve. Which ones was not measured then; L2's answers during writes
  later halved the lock waits, so part of them were lookups made while a writer held the lock [inferred].
- Worth retrying only in the form L2 took (answers during writes); the lock waits then halved (T-L2), still with no
  visible FPS change.
- "L1" is also the minidump item of plan-roadmap.md (crash-dump card); "T-L1" also names other tests on other days.
