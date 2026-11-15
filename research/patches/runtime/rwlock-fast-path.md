---
id: rwlock-fast-path
title: Guest rwlocks without a global lock or heap allocation per operation: lock-free handle lookup, per-thread ownership, SRW locks on Windows
area: runtime
status: active
platforms: all
default_windows: on
default_deck: on
origin: ours
class: G
commits:
files: src/runtime_rwlock.c, tests/test_rwlock.c, tests/test_rwlock_registry.c, gpu/CMakeLists.txt
symbols: lookup, resolve, acquire, rw_unlock, rw_destroy, held_find, held_add, held_remove, host_lock, host_try, host_timed, host_unlock, reserve_locked, create_locked, users, RW_LIVE, RW_DEAD
switches:
tests:
docs: plan-work-queue.md
depends_on: R2
related: wait-sites
checked:
---
## What it does

Before, every guest rwlock call took the process-wide `registry_lock` twice, walked the lock's holder list and
`calloc`'d / freed a holder record, then took a winpthreads `pthread_rwlock`. Now only create, static
initialization and destroy take `registry_lock`. `lookup` reads the R2 handle set without a lock; ownership
(EDEADLK, EPERM, a thread's read recursion) is a per-thread list (`held_*`: 16 inline, grown on the heap beyond);
the host lock is an SRW lock on Windows (`host_*`), glibc's `pthread_rwlock` elsewhere; destroy's EBUSY comes from
one atomic `users` count (an acquire in progress or a thread holding it). No switch: the same semantics, cheaper.

## Invariants

- Same error codes as before for every call (tests/test_rwlock.c repeats test_runtime.c's rw_lifecycle,
  rw_concurrency and rw_timeouts; test_rwlock_registry.c covers the set). Both must pass before a change ships.
- A read a thread already holds is counted, never taken from the host lock again (SRW locks are not recursive:
  a waiting writer would deadlock a recursive shared acquire; FreeBSD's libthr also grants it). Unlock releases
  the host lock only when the thread's count reaches zero; a writer releases exclusive.
- `users` is raised before an acquire checks `magic` and kept while that thread holds the lock (dropped by
  `held_remove` or on failure); `rw_destroy` writes `RW_DEAD` before reading `users`, both seq_cst, so a racing
  acquire either sees the lock dead (EINVAL) or destroy sees it in use (EBUSY).
- `lookup` never dereferences an unknown handle: only pointers found in the set are used (0xdead, 1, freed
  handles stay EINVAL). Destroyed locks and replaced tables are retired in rings (4096 locks, 8 tables) before
  being freed, because a lookup or racing call may still read them.
- Timed acquires on Windows poll (try, `SwitchToThread`, then `Sleep(1)`): fine for rare calls, not for hot ones.
- A thread's ownership list lives in TLS and is not freed at thread exit (a thread exiting with locks held kept
  them before too: destroy stays EBUSY).

## Interactions

- R2: the set and its rules stay (half full, `&deleted`, probes stop at NULL); lookups are now lock-free, the
  table is published with one release store and retired, slots are written with release stores.
- wait-sites: rwlock exports are not measured there (only mutex / cond / sema exports).

## Evidence

- Found (jitter-1, 2026-11-15, card tooling/wpr-capture): in frame-time streaks the game thread spent ~17% in
  guest lock calls, `acquire` at runtime_rwlock.c:123 (registry / calloc) and :130 (host lock) among them.
- Fix built, not run in the game: `tests/test_rwlock.c --bench` (Windows, CLANG64): read lock + unlock 90 -> 13.5 ns,
  write 90 -> 13 ns, 4 threads reading one lock 570 -> ~180 ns, 4 threads with 10% writes 1,050 -> ~190 ns,
  8 readers 1,900 -> ~425 ns. Linux (WSL gcc): 32 -> 14 ns, 10% writes 6,960 -> 875 ns, 8 readers 1,050 -> 557 ns.
  Both test files pass on Windows and Linux.

## History and pitfalls

- The per-operation counters (reads, writes, unlocks) were dropped: they were shared writes on every call;
  `runtime_rwlock_report` prints created and live only.
- A first version kept two counts (in flight, holders): six atomic operations per lock + unlock pair; one count
  made it four (8 readers ~600 -> ~425 ns).
