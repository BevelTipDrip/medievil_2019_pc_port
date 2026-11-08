---
id: deck-epp-governor
title: EPP performance on every CPU (Deck; proposed via steamosctl, run via deck-cpu-clocks.sh perf)
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
docs: plan-linux-deck.md#cpu-clocks-and-the-power-cap-2026-11-0405-log-datadeck-epp-perf-1log-datadeck-dedicated-1log, plan-linux-deck.md#baseline-1-and-live-abs-2026-11-04-appimage-a863482-67-helpers-auto-no-dedicated-core-auto-clocks-mailbox-log-datadeck-baseline-1-ablog, study-deck-apu-deep-dive.md#24-steamoss-power-daemons-and-what-a-user-can-change-web, study-deck-apu-deep-dive.md#25-what-can-favour-stage-bs-clock-and-what-cannot
depends_on:
related: deck-cpu-clock-split, stage-b-spin, deck-test-tooling, deck-preset
checked: 2026-11-07
---
## What it does

A system setting, not code: every CPU at EPP `performance` (amd-pstate-epp; in active mode the `performance`
governor pins EPP 0 on every core). The APU study's rank 2 proposed it through `steamosctl
set-cpu-scaling-governor performance` (no root) on the guess that stage B sat on EPP's sticky ~2.4 GHz plateau
after sleeps. It was applied with `research/tools/deck-cpu-clocks.sh perf` (the user, with sudo; reset at reboot):
`energy_performance_preference` = `performance` and `scaling_max_freq` = the hardware maximum on every CPU; the
governor itself was not changed. `steamosctl` was never tried.

## Invariants

- Nothing in the port sets clocks, EPP or the governor; the launcher only advises a manual GPU clock (`DECK_HINT`).
  Any future clock control must stay opt-in and reversible (`deck-cpu-clocks.sh save` / `restore`).
- Class assumption behind the idea: stage B clock-bound below its boost clock. Measured false (2.98 GHz).

## Interactions

- `deck-cpu-clock-split` is the per-core variant, tried right after; also rejected.
- `stage-b-spin` was the study's alternative for the same plateau; under EPP performance spin off / on was 75.6 /
  75.9 FPS (neutral).
- `deck-test-tooling`: `deck.sh clocks` gave the stage B clock that decided this.

## Evidence

- Baseline 1 (`deck.sh clocks`): stage B at ~2.98 GHz mean (the plan's range reads 2.88-2.97), "not ~2.4 (the APU
  study's guess)"; the governor's headroom is "~15% at most".
- EPP performance on every CPU (`data/deck-epp-perf-1.log`): stage B 2,963 MHz (2,976 before), **74-76 FPS vs
  76.5: no gain**.
- Why: the APU is at its power cap: `power1_cap` 15 W, 14-15 W used, CPU 7.9 W and GPU 3.2 W of 14.9 W
  (`gpu_metrics`), GPU 860-1,090 MHz at ~100% busy. More CPU clock only takes power from the GPU.

## History and pitfalls

- The user (decisions after the culling run) kept "the performance governor ... on the list despite stage B's ~2.98
  GHz"; this run closed it. The user's direction after it: "cut total CPU work on every thread (less CPU power = GPU
  headroom)".
- Worth retrying only with a higher APU power limit (TDP above 15 W) or a much lighter GPU load, where the CPU,
  not the shared 15 W budget, sets the frame (this card's inference; the docs give no retry condition).
