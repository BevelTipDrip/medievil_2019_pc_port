---
id: D2-gettid
title: Linux: scePthreadGetthreadid caches the thread id (Deck D2)
area: deck
status: active
platforms: linux
default_windows: n/a
default_deck: on
origin: spacepepperoni 1232650
class: C
commits: 471913a
files: src/runtime_extra.c
symbols: thread_id, runtime_thread_id_calls, runtime_thread_id_lookups, runtime_fault_stats_enabled
switches:
tests:
docs: impl-deck-cpu-bundle.md#d2-cached-thread-id, impl-deck-cpu-bundle.md#s-counters-gow3_frame_stats1, study-deck-rescore-rejected.md#d-linux-only-items-from-the-spacepepperoni-review-verdict-adopt-not-ported
depends_on:
related: frame-stats-lines
---
## What it does

`thread_id()` (the `scePthreadGetthreadid` export) on Linux keeps `syscall(SYS_gettid)`'s result in a
`static __thread int32_t id` after the first call, instead of a system call every time; UE4 asks for it on every
`IsInRenderingThread()`-style check. With `GOW3_FRAME_STATS=1` it counts calls (`runtime_thread_id_calls`) and
kernel lookups (`runtime_thread_id_lookups`) for the "Thread ids per frame" line. Windows already read the TEB
(`GetCurrentThreadId`). Not to be confused with `D2` (precise CPU writes, caches).

## Invariants

- Class C, assumptions: a thread's id never changes, and the process never forks (a forked child would keep the
  parent thread's cached id). Nothing in the runtime forks.
- The host TLS seen while guest code runs is the thread's own (the runtime's other `__thread` state relies on the
  same), so the cache is per real thread.
- `id == 0` means "not looked up yet"; a real tid is never 0.
- The counters are touched only when `runtime_fault_stats_enabled()`; without stats the call is one TLS read.

## Interactions

- `frame-stats-lines`: "Thread ids per frame: scePthreadGetthreadid N, cache hits N, kernel lookups N".
- Independent of every other Deck switch; always on in Linux builds.

## Evidence

- The fork's measurement: ~150,000 calls in 4 s on UE4's render thread (spacepepperoni commit message).
- Deck log `data/deck-baseline-1-ab.log`: "Thread ids per frame: scePthreadGetthreadid" 640-2,792, all cache hits,
  kernel lookups 0.00-0.08: the system calls are gone.
- Rescore D2 estimate: ~1-2% of a game thread's core, 0 for stage B [inferred]. No FPS A/B; not verified in game on
  its own.

## History and pitfalls

- Credit: Porticus (upstream PR #57, `25e312c`); `1232650` is spacepepperoni's reapply.
