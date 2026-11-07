---
id: drain-sleep
title: Stage A sleeps on stage B's progress instead of yield-spinning (GOW3_DRAIN_SLEEP, spacepepperoni 6ef7566)
area: stage-a
status: rejected
platforms: all
default_windows: off
default_deck: off
origin: spacepepperoni 6ef7566
class: G
commits: ea8f4c1, 64d998f
files: gpu/shadps4/video_core/renderer_vulkan/vk_draw_pipe.h
symbols: DrainSleeps, YieldWait, drainer_waiting, Consumed, consumed
switches: GOW3_DRAIN_SLEEP
tests:
docs: plan-linux-deck.md#drain-sleep-2026-11-04-appimage-ea8f4c1-fences-off-auto-clocks-log-datadeck-drain-sleep-1log, impl-cpu-waste.md#why-gow3_drain_sleep-lost-607-vs-640-fps-inferred-from-the-code, study-deck-rescore-rejected.md#a-switches-that-already-exist-no-build, study-fork-spacepepperoni.md#13-gpu-thread-synchronisation-memory-shadps4-video-core
depends_on: lineage-draw-pipe
related: drain-hybrid, E1-deck-drains
verified: 2026-11-07
---
## What it does

In `DrawPipe::YieldWait`, after 4,096 pauses and 32 yields, stage A sets `drainer_waiting` and sleeps on
`consumed` itself (`consumed.wait(seen)`), instead of calling `sched_yield` in a loop. Stage B's `Consumed()` calls
`consumed.notify_one()` after every packet while the flag is set. This is the second half of spacepepperoni `6ef7566`
(from the Uncharted Linux fork; upstream #30 profiled 15% in `sched_yield`). `ea8f4c1` made it the Linux default, and
`64d998f` made it opt-in (`GOW3_DRAIN_SLEEP=1`) on every platform.

## Invariants

- The flag store and the `consumed` load on stage A, and the `consumed` store and the flag load on stage B, are
  `seq_cst`. A wake between the check and the wait is not lost, because the wait returns when `consumed != seen`.
- Only in `YieldWait`: with drain-hybrid on (`GOW3_DRAIN_HYBRID=1`, bit 4096 clear) this code never runs. With both
  switches set, setting bit 4096 live brings this sleep back (not the plain yield loop).
- `DrainSleeps()` reads the variable once; no toggle bit. Stage B's `Consumed()` loads `drainer_waiting` after every
  packet whatever the switch (one load on `consumed`'s line).
- Each wake returns at every packet (the value changed), so stage A re-checks and sleeps again. That is the cost
  below, not a correctness issue.

## Interactions

- drain-hybrid replaced it. It keeps the sleep but registers one wake position (`drain_target`) and wakes once.
- E1-deck-drains: measured before E1, when stage A still drained at OnSubmit and the display labels. The bottleneck
  has since moved to stage B (rescore A8).

## Evidence

- Deck (2026-11-04, AppImage `ea8f4c1`, fences off, auto clocks, Graveyard): **60.7 FPS vs 64.0** (run A). Frame
  stddev 2.19 vs 2.25 ms, p99 21.0 vs 20.8 ms. Stage A waited 22.7% vs 22.6%, stage B 78% vs 80%. The user: CPU
  clocks boost higher (less spin power), FPS the same or lower.

## History and pitfalls

- Why it lost (inferred from the code, impl-cpu-waste.md): a drain of about 2 ms covers 100+ packets. Each one cost
  a futex wake on stage B (the bottleneck thread) and a context switch on stage A. "The design was right (sleep);
  the wake rule was the cost." The freed core did not become frames: the chain was stage B plus the game's
  per-frame EOP wait.
- The same run found the user's overlay showing 10 ms / 20 ms frames ping-ponging at about 62 FPS while our stats
  were even. That is presentation, not this patch.
- Retry: rescore A8 and the patch audit's restart test 7 suggest re-testing after E1 (-0.5 to +0.5 ms). With
  drain-hybrid measured (stage A CPU 112% to 76%, 78.7 vs 77.9 FPS), the per-packet wake rule has no reason to
  come back. Re-test only to compare against drain-hybrid, not as a default.
