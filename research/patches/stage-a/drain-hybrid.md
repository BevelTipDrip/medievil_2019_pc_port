---
id: drain-hybrid
title: Stage A hybrid drain wait: short pause, then a futex sleep stage B ends two packets early
area: stage-a
status: active
platforms: all
default_windows: off
default_deck: on
origin: ours
class: G
commits: 6f4be46
files: gpu/shadps4/video_core/renderer_vulkan/vk_draw_pipe.h, gpu/shim/gow3_drain_wait.h, gpu/shim/gow3_drain_wait.cpp, gpu/shim/gow3_toggles.h, tests/test_draw_pipe_drain.cpp
symbols: HybridWait, YieldWait, SpinUntil, Consumed, WakeDrainer, drain_target, drain_wake, commit_ends, CommitEnds, NoDrainTarget, Wanted, SpinNs, WakeAhead, FutexWait, FutexWake, DrainHybridOff
switches: GOW3_DRAIN_HYBRID, GOW3_DRAIN_SPIN_US, GOW3_DRAIN_WAKE_AHEAD, toggle2:DrainHybridOff
tests: Launch 15
docs: impl-cpu-waste.md#the-hybrid-wait-gow3_drain_hybrid1, impl-cpu-waste.md#unit-test, plan-linux-deck.md#stage-a-hybrid-drain-wait-2026-11-05-appimage-d6029de-gow3_drain_hybrid1-frame-stats-without-sections-log-datadeck-drain-hybrid-1log, plan-linux-deck.md#launch-15-results-2026-11-06-2050-2128-build-2a6efea-signal-a-new-arena-with-a-guaranteed-heavy-view-the-old-spot-cannot-be-found-under-a-cap-gpu-auto-log-datadeck-launch15log-traces-research-datadeckdeck-cap60-signalcap60-cutsuncap-cuts-
depends_on: lineage-draw-pipe
related: drain-sleep, drain-wake-ahead-6, L5, cacheline-groups, signal-sleep, core-sched, deck-preset, A3
verified: 2026-11-07
---
## What it does

`DrawPipe::Drain` (stage A waits until stage B has run every committed packet) used to run 4,096 pauses, then
`sched_yield` for as long as it took (`YieldWait`). With `GOW3_DRAIN_HYBRID=1`, `HybridWait` does this instead:
1. A pause spin (no system call) for up to `GOW3_DRAIN_SPIN_US` (30).
2. A futex sleep (`FutexWait` on `drain_wake`; Windows uses `WaitOnAddress`) with a wake position in
   `drain_target`: the commit end of the packet `GOW3_DRAIN_WAKE_AHEAD` (2) before the last one, from
   `commit_ends`.
3. A spin again. If the end is still not reached, it sleeps until the exact end.

Stage B's `Consumed(at)` reads `drain_target` once per packet and wakes only a registered sleeper.

## Invariants

- No lost wake. Stage A loads `drain_wake` (`seen`), stores `drain_target`, then loads `consumed`. Stage B stores
  `consumed`, then loads `drain_target`. Everything is `seq_cst`. Weakening any of these orders, or registering
  after the check, can leave stage A asleep with stage B idle: a hang (the unit test's 5 s watchdog catches it).
- `WakeDrainer` takes the target by compare-exchange (one wake per registration). It bumps `drain_wake` before
  `FutexWake`, so a sleeper whose `seen` is stale returns at once.
- Every exit resets `drain_target` to `NoDrainTarget`, and stage B wakes only when `target <= at`. A stale target
  would cost stage B a futex call per packet.
- `WakeAhead()` is clamped to 0-15, because `commit_ends` keeps the last `CommitEnds` (16) packet ends. Only stage A
  writes `commit_ends` and `head`. The early position is used only if it lies between `consumed` and the end.
- The wait always ends at the exact head (`SpinUntil(target)`). The early wake is a hint, never a return.
- `Enabled()` reads the toggle bit at each drain, so switching it live is safe: each drain uses one path.

## Interactions

- drain-sleep: the earlier opt-in sleep. It is still in `YieldWait` (bit 4096 set with `GOW3_DRAIN_SLEEP=1`). This
  card replaces its wake rule (a wake per packet) with one wake per sleeping drain.
- drain-wake-ahead-6: the knob tried at 6, neutral. `toggle2:DrainWakeAheadDefault` forces 2 live.
- L5, decode-pacing, E1-deck-drains: they decide how many drains there are. This card decides what a drain costs.
- cacheline-groups: `drain_target` sits on `consumed`'s cache line, so stage B's check costs no extra line.
- core-sched: with the core-scheduling cookie, stage A's drain waits rose from 2.1 to 6.9 ms a frame.
- signal-sleep: the same spin-then-futex pattern for the recorder waits. Rejected, because every Notify woke it. It
  reuses this card's `FutexWait` / `FutexWake` in `gow3_drain_wait.cpp` (and added `FutexWakeAll` there, `2a6efea`):
  a change to those helpers changes both waits.

## Evidence

- Deck (2026-11-05, `d6029de`, live A/B `0 2048` vs `0 6144`): **78.7 vs 77.9 FPS**, stddev 1.32 vs 1.44 ms, stage A
  CPU **76% vs 112%**, `sched_yield` **0 vs 4,540 a frame**, drain wait 3.26 vs 4.16 ms a frame. Verdict: keep.
- Launch 15 (2026-11-06, 60 cap): switching it off costs +4.8 points of a core in process CPU, but no measurable
  watts. Uncapped: stage A at 86.5% off vs 70.7% on (16 points), FPS equal (GPU-bound). Caveat: in both launches of
  Launch 15 draw preparation stopped for good at the first A1-A3 flip (A3's pitfall; `data/deck-launch15.log`,
  "Draw preparation: 0 prepared" from line 9198, and from 20691 in the uncapped launch), so the uncapped rows (rounds
  2-4) and the later 60 rounds ran without prepared draws in every state.
- `draw-pipe-drain-test`: 6,000 rounds, both paths flipped live, no lost wake on Windows and WSL.
- Timing only, no picture change, so there is no in-game visual check to make.

## History and pitfalls

- Wake latency is 51 us on average (1.5 ms max) against an early wake about 13 us of stage B work ahead. After a
  sleeping drain, stage B usually runs dry for about 40 us (impl-stage-a-decode.md section 2). Waking 6 ahead did
  not help (drain-wake-ahead-6).
- At a 60 cap the saved CPU became idle time at full clock and voltage, not watts (Launch 15).
