---
id: particle-single-fixed
title: GPU particle fixed step in the single pass (eboot patch, GOW3_PARTICLE_SINGLE_FIXED)
area: ue4
status: rejected
platforms: all
default_windows: off
default_deck: off
origin: ours
class: M
commits: b3c01fd965
files: patches/MediEvil.xml, scripts/graphics_options.py, run.py
symbols: PARTICLE_SINGLE_FIXED_PATCH, particle_single_fixed_environment
switches:
tests:
docs: plan-linux-deck.md#particle-fix-1-2026-11-05-2110-2128-build-49291f3-particlefix-log-datadeck-particlefix-1log, plan-linux-deck.md#particle-fix-2-the-guard-alone-2026-11-06-build-49291f3-particlefix, study-particle-fixed-step.md#the-change, study-particle-fixed-step.md#the-emitters-per-frame-data-game-thread-code
depends_on: single-particle-pass
related: particle-flip-guard
---
## What it does

Eboot patch "GPU particle fixed step in the single pass", 5 writes per build. The game thread's per-frame data
(0x2383a36 / 0x2383a5c, 01.00) always plans with the registered 1/30 constant (0x3e2461c) instead of the cvar. The
three first `ExecuteSimulationCommands` calls get `bUseFixDT = 1` (`seta al` -> `mov al, 1; nop`). With the single
pass on, the 12 flag-8 emitters (`LocalVectorField.bUseFixDT`: `PS_air_sparkles`, leaves, fluff) keep 1/30 s steps
in one pass. Switch: `GOW3_PARTICLE_SINGLE_FIXED=1` / `0` (run.py, extra-env.txt). It is not in switches.tsv
because it is a Python-only key. Unset, the launcher's Patches page decides (`isEnabled="false"`).

## Invariants

- With the cvar at its 1/30 default every patched value equals the stock one, so the live A/B (menu, toggle file
  8192) still compares with the stock game.
- Each line has `Original` bytes. `patches.py` (`original_mismatches`) skips the patch on another build. 01.02 is the
  same at +0x2dd0, with the constant at 0x3e276dc.
- All three collision variants' `seta` sites must be patched together, or some emitters take the variable pass.
- Without the single pass on it changes nothing.

## Interactions

- single-particle-pass: needed for any effect.
- particle-flip-guard: the real fix for the torch and the crypt floor. With the guard, this patch adds nothing.

## Evidence

- Particle fix 1 (Deck, `data/deck-particlefix-1.log`): applied together with the guard ("Patches: ... 'GPU
  particle fixed step in the single pass'"), sim runs 1.00. The torch and the floor were right, but that is the
  guard's doing (guard off brought both back).
- Particle fix 2 (same build, without this patch): the user, "the torches still worked", "the crypt floor looks
  fine". So the patch is not needed.

## History and pitfalls

- Built from the hypothesis that the crypt ember was `PS_air_sparkles` (flag 8) losing its 1/30 s steps. The
  ember turned out to be the same lost update as the torch, a flip without a simulation.
- Dropped because it costs the flag-8 emitters their look-ahead step (they move in 1/30 s steps, a possible judder
  above 30 FPS on fast sparkles [inferred]) and saves nothing measurable over the guard.
- Worth retrying only if a flag-8 emitter (sparkles, leaves, fluff) is seen moving at the wrong speed with the single
  pass and the guard on. Compare at second word `0` vs `8192`.
