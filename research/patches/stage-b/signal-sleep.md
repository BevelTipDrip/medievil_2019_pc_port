---
id: signal-sleep
title: Recorder waits (deferred signals, host copies) as a short spin then a futex sleep
area: stage-b
status: rejected
platforms: all
default_windows: off
default_deck: off
origin: ours
class: G
commits: 2a6efea463
files: gpu/shim/gow3_signal_wait.h, gpu/shadps4/video_core/renderer_vulkan/vk_scheduler.cpp, gpu/shadps4/video_core/renderer_vulkan/vk_scheduler.h, gpu/shim/gow3_drain_wait.cpp, gpu/shim/gow3_toggles.h, tests/test_signal_wait.cpp
symbols: Gow3SignalWait, Wanted, Enabled, SpinNs, Notify, Wait, sleepers, generation, WaitDeferredSignals, WaitHostCopies, SignalSleepOff
switches: GOW3_SIGNAL_SLEEP, GOW3_SIGNAL_SPIN_US, toggle2:SignalSleepOff
tests: Launch 15
docs: plan-linux-deck.md#cpu-cuts-at-a-60-cap-2026-11-06-tooling-7379aa0-signal-waits-2a6efea-not-run, plan-linux-deck.md#launch-15-results-2026-11-06-2050-2128-build-2a6efea-signal-a-new-arena-with-a-guaranteed-heavy-view-the-old-spot-cannot-be-found-under-a-cap-gpu-auto-log-datadeck-launch15log-traces-research-datadeckdeck-cap60-signalcap60-cutsuncap-cuts-
depends_on: lineage-recorder-fences
conflicts_with:
supersedes:
related: recorder-lean, drain-hybrid, drain-sleep, stage-b-spin
verified: 2026-11-07
---
## What it does

`Scheduler::WaitDeferredSignals` (before WRITE_DATA, end-of-shader fences, the flip) and `WaitHostCopies` spin with
`sched_yield` until the recorder has run what was queued before them ("blocked: host copies" 7-8% of stage B's time
on the Deck, ~0.9 ms a frame at 80 FPS, launch 14). With `GOW3_SIGNAL_SLEEP=1` they pause-spin up to
`GOW3_SIGNAL_SPIN_US` (default 30, max 2000) and then sleep on a futex (`Gow3SignalWait::Wait`, Windows
`WaitOnAddress` via `Gow3DrainWait`). The recorder (deferred signal done) and the host-copy completion raise their
counters as before, then `Gow3SignalWait::Notify` wakes sleepers only if `sleepers` is non-zero.

## Invariants

- No lost wake: a sleeper registers (`sleepers` +1, reads `generation`, seq_cst fence) before rechecking `done()`;
  `Notify` fences then reads `sleepers`. Either the notifier sees the sleeper or the sleeper sees the new counter.
  `signal-wait-test` checks it (two waiters, producer gaps 2 / 80 / 400 us, a watchdog on lost wakes).
- Every producer that raises a counter these waits read must call `Notify` after the release store (today:
  the deferred-signal task in `SignalAfterHostCopies`, `host_copies_done` in vk_scheduler.h). A new done counter
  without it leaves a sleeper until some other wake.
- `WaitDeferredSignals` still helps the copy threads once (`Gow3Copy::WaitAsync`) before sleeping.
- Off (default, or second word 16777216 set): the `sched_yield` loop as before, live. Not byte-identical to before
  `2a6efea`: `Notify` (a seq_cst fence and a load of `sleepers`) runs after every deferred signal and every host copy
  whether the switch is on or not.
- `WaitHostCopies` on the sleep path does not help the copy threads before it sleeps (only `Gow3Copy::WaitAsync`
  after the wait), unlike the `WaitDeferredSignals` path.

## Interactions

- One global `generation` word: every `Notify` wakes every sleeper whatever its target, which is why each slept
  wait was woken ~12 times.
- Same pattern as drain-hybrid (stage A's drain: a short pause then a futex that stage B ends near the target, kept)
  and the opposite of stage-b-spin (spin longer before blocking).
- recorder-lean lets the recorder sleep longer; a stage B that sleeps here must not depend on a lazy label wake:
  the waits kick with `KickRecording(true)` first.

## Evidence

- Launch 15 (Deck, build `2a6efea-signal`, new arena, GPU auto), at the 60 cap (162 s each): 60 / 60 FPS, socket
  15.03 / 15.05 W, CPU 7.93 / 7.96 W, process CPU 362 / 363%, stage B 49.1 / 50.0%: neutral. It works (1.5 of 2.7
  waits a frame slept; wait time 0.38 vs 0.62 ms a frame) but ~42 wakes a frame.
- Uncapped: sleep off 78.8 vs on 78.3 FPS, presents > 15 ms 96 vs 120 a minute: the wake latency's sign, within
  noise. "Not for the preset."

## History and pitfalls

- Dropped because it saves no watts at the cap and costs slightly uncapped. Retry only with targeted wakes: wake a
  sleeper only when the counter reaches its own target (per-waiter target or a counter-indexed word), so a slept
  wait is woken once, not ~12 times.
- Wake latency on the Deck is ~70 us (stage A's drain line): a few such waits a frame cost stage B time whenever it is
  the limit.
