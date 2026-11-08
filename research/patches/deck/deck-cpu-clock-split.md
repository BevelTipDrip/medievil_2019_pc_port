---
id: deck-cpu-clock-split
title: Boost stage B's core, cap the other cores (Deck)
area: deck
status: rejected
platforms: linux
default_windows: n/a
default_deck: off
origin: ours
class: G
commits:
files:
symbols:
switches:
tests:
docs: plan-linux-deck.md#cpu-clocks-and-the-power-cap-2026-11-0405-log-datadeck-epp-perf-1log-datadeck-dedicated-1log, plan-linux-deck.md#launch-15-results-2026-11-06-2050-2128-build-2a6efea-signal-a-new-arena-with-a-guaranteed-heavy-view-the-old-spot-cannot-be-found-under-a-cap-gpu-auto-log-datadeck-launch15log-traces-research-datadeckdeck-cap60-signalcap60-cutsuncap-cuts-, study-deck-apu-deep-dive.md#22-per-core-boost-and-preferred-cores, study-deck-apu-deep-dive.md#23-the-cpugpu-power-split
depends_on:
related: deck-epp-governor, stage-b-core, deck-test-tooling
verified: 2026-11-07
---
## What it does

A system setting, not code: `research/tools/deck-cpu-clocks.sh split` (the user, with sudo) put stage B's CPU 6 (the
launcher's dedicated core for stage B, `stage-b-core`) at EPP performance and full clock, and capped every other CPU
at 2.0 GHz with its EPP unchanged. The idea (the user): boost stage B, cap the rest, and leave power to the GPU under
the 15 W cap.

## Invariants

- Nothing in the port sets per-core clocks; the script saves and restores the previous values and resets at reboot.
- Class assumption behind the idea: capping a core's frequency saves its power. On this APU it does not (below).

## Interactions

- `stage-b-core`: the split needs stage B on a known CPU (`deck-cpu-clocks.sh split` defaults to CPU 6). It ran with
  the dedicated core on: `data/deck-dedicated-1.log` is one launch, with one "Stage B: dedicated core 3 (CPU 6;
  sibling CPU 7 kept free), 25 other threads moved" line at start, and its last ~20 windows are the split's ~62-68
  FPS (checked in the raw log). The same launch holds the dedicated-core-alone run (74.1 FPS, stage B busy 96 -> 89%,
  GPU 98% busy at 1,024 MHz), the split's real baseline.
- `deck-epp-governor`: the all-core variant, measured just before; no gain either.

## Evidence

- Split (`data/deck-dedicated-1.log`, same launch as the dedicated core alone): **~66 FPS**. Stage B ran at
  3,344 MHz, but stage A at ~1.9 GHz starved it (packet-ring waits 0.2 -> 1.8 ms a frame), and the GPU fell to
  863 MHz (GPU waits 0.7 -> 1.7 ms a frame).
- Capped cores still drew ~1.7 W each, like stage B's core [inferred: one shared CPU voltage set by the fastest
  core, so capping frequency saves little]. The doc's verdict: "Boost-one / slow-the-rest does not work on this APU."

## History and pitfalls

- The user afterwards: a fixed GPU clock already gives the GPU more power (+1-2 FPS at a cost in frame stability);
  not worth chasing clocks unless CPU use drops seriously. Direction: cut CPU work on every thread.
- The next step the plan proposes (Launch 15 results): a CPU clock ceiling on all cores at the 60 cap (3.0 / 2.6 /
  2.2 GHz, a 60 s trace each): "Capping every core lowers the shared voltage (capping some did not, 2026-11-05)."
  Not run yet. A true boost-one split would need per-core voltage, which the APU does not expose [inferred].
