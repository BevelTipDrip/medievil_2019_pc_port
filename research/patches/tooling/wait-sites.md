---
id: wait-sites
title: GOW3_WAIT_SITES: every guest wait by call site, requested and actual time (pthread exports, event queues)
area: tooling
status: diagnostic
platforms: all
default_windows: off
default_deck: off
origin: ours
class: G
commits: 9f088f144a, 9380965734, a2cb3a324c, ff78bdf92a
files: src/runtime_waitsites.c, src/runtime.h, src/runtime_mutex.c, src/runtime_kernel.c, src/runtime_sema.c, src/runtime_extra.c, src/runtime_thread.c, gpu/shadps4/core/libraries/kernel/equeue.cpp
symbols: runtime_wait_site, runtime_wait_sites_enabled, runtime_wait_now, RUNTIME_WAIT_BEGIN, RUNTIME_WAIT_END, kind_names, mutex_lock_impl, sceKernelWaitEqueue
switches: GOW3_WAIT_SITES
tests: T-W1, T-W2, T-W3
docs: plan-frame-pacing.md#wait-sites-2026-10-18-1340-gow3_wait_sites1-lab-diagnostic-9f088f1-run-wait-sites, plan-frame-pacing.md#the-panning-stall-is-a-wait-chain-not-work-2026-10-19-pan-1etl--ghidra-no-launch, plan-linux-deck.md#wait-sites-and-per-thread-cpu-2026-11-04-appimage-0771a23-fences-off-auto-clocks-except-the-last-30-s-log-datadeck-waitsites-1log
depends_on:
related: fence-stats, early-frame-eop, drain-sleep, hzb-remap, occlusion-queries-off, U5
verified: 2026-11-07
---
## What it does

`RUNTIME_WAIT_BEGIN()` / `RUNTIME_WAIT_END(kind, requested_ns)` (`runtime.h`) wrap each guest-callable wait. They
record `__builtin_return_address(0)` (the guest call site), the kind, the requested time and the actual time in a
lock-free open-addressing table (`runtime_wait_site`, 4,096 slots, 64 probes, then counted as "lost").
Kinds: yield, pthread_yield, usleep(0), usleep, nanosleep (`9f088f1`); cond_wait / timedwait, sema_wait,
eventflag_wait, mutex_lock (`9380965`); equeue_wait (`ff78bdf`, `sceKernelWaitEqueue`, which calls
`runtime_wait_site` directly instead of the macros). Every 5 s the thread that
crosses the boundary prints the 16 sites with the most time as `guest+offset` (eboot image offset; add 0x400000 in
Ghidra), with calls/s and average requested / actual time. `a2cb3a3`: each POSIX export measures itself around a
shared `_impl`, and absolute-deadline waits record the time left to the deadline as requested.

## Invariants

- The begin / end pair must sit in the function the guest calls, not in a helper it reaches through another
  export. Otherwise the return address is the port's wrapper, not the guest site (T-W1: `posix_cond_wait` /
  `posix_mutex_lock` hid the busiest sites). Exports that share code call an unmeasured `_impl`
  (`mutex_lock_impl`), never another measured export.
- Off: one cached load per call (`runtime_wait_sites_enabled`), no clock read.
- The export itself must stay a real function (address handed to the guest, not inlined into another export), or
  `__builtin_return_address(0)` names the wrong caller.
- On: three clock reads (begin, end, the report check in `runtime_wait_site`) and three atomic adds per wait, plus
  the report's `printf` on a guest thread every 5 s (the first call only arms the window). On
  Windows the stacks trace showed `runtime_wait_site` at 4.9% of the render thread: the spin it measures got
  slower.
- Totals sum over all threads, so idle waiters (timers, task workers) dominate them. Read per call site and per
  call, not by total.
- `runtime_wait_now()` never returns 0 (0 means "not measured").

## Interactions

- fence-stats instruments the same `sceKernelWaitEqueue` (by event filter / ident and thread, `GOW3_FRAME_STATS`).
  The two are independent.
- The Deck's per-frame EOP wait at guest+0x68af47 is what early-frame-eop and the U5 gate study target. The Deck
  run also led to drain-sleep (ported, rejected).
- The 1.3 M/s `usleep(0)` spin is UE4's render thread (stacks trace), called the "render thread's query spin" in
  T-W2. Tied to occlusion results (R4a's two-frame-old queries cut it to ~100k/s), but the docs leave open whether it
  waits on occlusion or timestamp queries (T-O3c) and call the loop an inferred UE4 wait-for-task loop. The
  occlusion path is hzb-remap / occlusion-queries-off.

## Evidence

- Graveyard standing (`9f088f1`): ~1,300,000 `usleep(0)`/s at guest+0x689c89, a spin-wait loop.
- T-W2: timed waits end before their deadlines (no timer rounding); in stall windows the same `cond_wait` sites
  wait longer (guest+0x70b46c 0.8-3.8 ms -> 9.6 / 21 ms): "what they wait for is late".
- T-W3 (equeue): guest+0x6885e7 once a frame at 3.8-4.1 ms (calm ~1.3 ms). The game waits about half of each
  ~8.2 ms frame for the port's frame-completion events.
- Deck (2026-11-04): guest+0x68af47 `equeue_wait` ~68/s, 6.4 ms average; the game's own waits block, not spin.

## History and pitfalls

- T-W1 / T-W2 / T-W3 also name other runs (frames queued, W2, W3): this card's are the wait-site runs in
  plan-frame-pacing.md.
- Not verified in game: diagnostics only.
