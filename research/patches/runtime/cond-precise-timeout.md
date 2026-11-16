---
id: cond-precise-timeout
title: Timed guest condition waits end at their deadline (100 ns timeouts, 0.5 ms timer), not rounded up to whole ms; optional spin for short waits
area: runtime
status: active
platforms: windows
default_windows: on
default_deck: n/a
origin: ours
class: G
commits: e12ffaa
files: src/runtime_mutex.c, tests/test_mutex.c, gpu/CMakeLists.txt
symbols: futex_cond_wait, precise_init, cond_precise, spin_wait, ns_until, rtl_wait_on_address, cond_spin_ns
switches: GOW3_COND_PRECISE, GOW3_COND_SPIN_US
tests:
docs: plan-work-queue.md
depends_on: upstream-hitch-work
related: wait-sites, wait-trace, flip-trace
checked:
---
## What it does

The futex condition variables (upstream-hitch-work, `GOW3_FUTEX_COND`) waited with `WaitOnAddress`, which takes
whole milliseconds: a timed wait was rounded up to the next ms, then ended on the 1 ms timer tick. An early timeout
came back as a spurious wakeup. Now (default, `GOW3_COND_PRECISE` unset or 1) a timed wait calls ntdll's
`RtlWaitOnAddress` with a 100 ns relative timeout, the process asks for a 0.5 ms timer resolution
(`NtSetTimerResolution`, once, at the first timed wait), and an early timeout waits again until the deadline.
`GOW3_COND_SPIN_US=N` (default 0) spins on the sequence word (`YieldProcessor`, QPC every 16 rounds) instead of
sleeping when N us or less are left. Untimed waits, signals, broadcasts and mutexes are unchanged.
`GOW3_COND_PRECISE=0` is the old code path and also leaves the timer resolution at 1 ms.

## Invariants

- The futex rules of upstream-hitch-work hold on every path: `seen` is read while the mutex is held, wakes increment
  `seq` before `WakeByAddress*`; the spin path watches the same word, so a wake is never lost (a spinning waiter
  is not registered with `WakeByAddressSingle`: a single wake can reach a sleeper too, a spurious wakeup POSIX
  allows).
- A precise timed wait returns ETIMEDOUT only at or after its CLOCK_REALTIME deadline, 0 when the word moved.
- The 0.5 ms timer resolution is process-wide (Sleep, other kernel waits) while precise mode is on.
- Spinning keeps the waiting thread's core busy. `GOW3_COND_SPIN_US` applies only to waits with that little time
  left, so the pools' 9-10 ms polls never spin; the game thread's 0-1 ms polls all spin with 1000.
- Windows only: Linux / the Deck use glibc's `pthread_cond_timedwait` (no ms rounding).
- `tests/test_mutex.c` (`ninja -C out/gpu mutex-test`): semantics pass in every mode and with `GOW3_FUTEX_COND=0`;
  `--timing` shows requested vs actual for expiring waits.

## Interactions

- UE4's `FPThreadEvent::Wait(ms)` builds its deadline from the current time cut to whole ms, so `Wait(1)` asks
  for 0-1 ms (requested uniform 0-1 ms; the pools' `Wait(10)` 9-10 ms).
- wait-sites' T-W2 note "no timer rounding" predates this measurement; the wait trace (wait-trace) shows the
  rounding (below).

## Evidence

- Found (wait trace, 2026-11-15, 4K 75% DLSS): the game thread (`main`) waits at guest+0x70df3a ~450 times a second,
  requested median 0.50 ms, actual 1.01 ms; 91% time out; 63,307 expired waits were followed by work against
  7,857 signalled ones, so the game thread mostly learns its work is done when a poll expires. Pool threads:
  requested ~9.48, actual ~10.28 ms (idle polls).
- `mutex-test --timing` (overshoot past the deadline, median / p90): UE4 1 ms old 0.85 / 1.85 ms, precise
  0.50 / 0.50, spin 1000 0.001 / 0.001 (core busy); 9.5 ms old 0.78 / 1.02, precise 0.32 / 0.49. The old code
  also returned 51 of 200 2 ms waits before their deadline.
- Run 2026-11-15 (PC, 4K 75% DLSS, static view, flip trace on, one launch each): average FPS A (old) 224.8-225.8,
  B (precise) 228.1-228.5, C (spin 1000) 231.1-231.3; median frame 4.43 / 4.38 / 4.33 ms; presented jitter about the
  same; C's game-frame (submit) spread 0.18 -> 0.27 ms; lows decided by the flip trace's own spikes. The user chose
  B as the default (no CPU cost). Then B with every log off: RTSS 236.6 current, 1% low 233.2, 0.1% low 227.7 FPS
  (cap 237), flat graph.
- Run with every log off, one change (2026-11-15, same area and direction, RTSS after 5-7 min): A (old,
  `GOW3_COND_PRECISE=0`, log has no precise line) 236.5 current, 1% low 220.4, 0.1% low 193.7 FPS, small spikes in
  the graph; B (precise) 236.6 / 233.2 / 227.7, flat. 1% low 4.54 -> 4.29 ms, 0.1% low 5.16 -> 4.39 ms. The user:
  "B is legitimately better".

## History and pitfalls

- The A/B runs above carried the flip trace's cost (flip-trace, LANDMINES): compare only within the same logging.
