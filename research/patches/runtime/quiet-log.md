---
id: quiet-log
title: GOW3_FPS_LOG gates the periodic log lines (FPS line, particle guard summary); default off, =1 on
area: runtime
status: active
platforms: all
default_windows: off
default_deck: off
origin: ours
class: S
commits: f53f76b, 1904c41
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

- Since 2026-11-16 (dev-tools-build) the FPS line is off unless `GOW3_FPS_LOG=1` (`FpsLogOn`); the launcher no
  longer sets it. Since P5 (2026-11-24) the particle guard summary follows the same rule: printed only with
  `GOW3_FPS_LOG=1`, so a default launch has no periodic line (until then it printed unless the value was `0`).
- A particle guard switch (toggle file) still prints its line when quiet.

## Interactions

- particle-flip-guard: only its periodic summary is skipped; the guard itself is unchanged.

## Evidence

- Fix built `f53f76b`, not run.

## History and pitfalls

- `getenv(...) != nullptr` made `GOW3_FPS_LOG=0` mean on.
- 2026-11-16 to P5: the FPS line defaulted off but the particle summary still printed every ~5 s in a default launch
  (it was quiet only with `0`; offcost/runtime-src.md item 2).
