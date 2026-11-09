---
id: quiet-log
title: GOW3_FPS_LOG=0 turns off the periodic log lines (FPS line, particle guard summary)
area: runtime
status: active
platforms: all
default_windows: off
default_deck: off
origin: ours
class: S
commits: f53f76b
files: gpu/shim/gow3_overlay.cpp, src/runtime_extra.c
symbols: LogPresent, particle_guard_report
switches: GOW3_FPS_LOG
tests:
docs: plan-linux-deck.md#cemetery-hill-heavier-than-before-2026-11-08-the-user-build-e3112f4
depends_on:
related: particle-flip-guard
---
## What it does

Upstream's `GOW3_FPS_LOG` printed "Perf: N FPS, worst frame N ms" every 5 s whenever the variable existed, and the
launcher always sets it (`env.setdefault('GOW3_FPS_LOG', '1')`), so no value turned it off. Now `0` turns it off, and
the particle flip guard's summary (every 1,200 simulations) is skipped with it. For a run with no periodic log
lines at all (the user, 2026-11-08: Deck pacing judged without any measurement on): `GOW3_FPS_LOG=0` in
extra-env.txt (run.py's extra-env overrides the launcher's value). Frame stats stay off by their own switch.

## Invariants

- Unset or any value but `0`: as before (the launcher's 1 keeps the Perf lines `deck_summary.py --perf` reads).
- A particle guard switch (toggle file) still prints its line when quiet.

## Interactions

- particle-flip-guard: only its periodic summary is skipped; the guard itself is unchanged.

## Evidence

- Fix built `f53f76b`, not run.

## History and pitfalls

- `getenv(...) != nullptr` made `GOW3_FPS_LOG=0` mean on.
