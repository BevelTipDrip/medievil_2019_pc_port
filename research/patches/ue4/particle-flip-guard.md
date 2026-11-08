---
id: particle-flip-guard
title: GPU particle flip guard: one state flip per main-phase simulation (fixes the single pass's torch / crypt floor bug)
area: ue4
status: active
platforms: all
default_windows: off
default_deck: on
origin: ours
class: M
commits: fbb8c1ad3d
files: src/particle_hook.h, src/probe.c, src/runtime_extra.c, gpu/shim/gow3_toggles.h, launcher/medievil_launcher.py, tests/test_particle_fixed_step.py
symbols: medievil_particle_builds, medievil_particle_find, medievil_particle_build_advance_stub, medievil_particle_build_pre_render_stub, install_particle_flip_guard, medievil_particle_advance, medievil_particle_pre_render, particle_slot, particle_guard_on, particle_guard_report, PARTICLE_FX_SLOTS, TOGGLE2_PARTICLE_FLIP_GUARD_OFF, ParticleFlipGuardOff, DECK_SWITCHES
switches: GOW3_PARTICLE_FLIP_GUARD, toggle2:ParticleFlipGuardOff
tests:
docs: plan-linux-deck.md#particle-fix-1-2026-11-05-2110-2128-build-49291f3-particlefix-log-datadeck-particlefix-1log, plan-linux-deck.md#particle-fix-2-the-guard-alone-2026-11-06-build-49291f3-particlefix, study-particle-fixed-step.md#the-change, study-particle-fixed-step.md#why-the-copy-pass-makes-the-default-immune-inferred-from-the-code-above
depends_on:
related: single-particle-pass, particle-single-fixed, deck-preset, lineage-toggle-file, draw-classes, game-update-0102
checked: 2026-11-07
---
## What it does

UE 4.21 keeps GPU particle state in two texture pairs. `FFXSystem::PreInitViews` flips the current pair (its tail
`jmp AdvanceGPUParticleFrame`, 01.00 0x230de02: `FrameIndex ^= 1`). `PreRender` then simulates the main phase into
it. Two rel32 redirections send these to host stubs (`particle_hook.h`). `medievil_particle_advance` skips a flip
when no simulation ran since the last flip. `medievil_particle_pre_render` (at the `call UpdateMultiGPUResources`,
0x230df0a) flips first when a simulation already used the current pair. The result is one flip per simulation.

## Invariants

- Per FX system: `simulated` is set by every main-phase simulation and cleared only by a flip that happens. Break
  this and the single pass reads a pair one simulation old again (frozen torch, crypt floor ember).
- `medievil_particle_find` accepts a build only if both sites' bytes, their rel32 targets and
  AdvanceGPUParticleFrame's first 20 bytes match. The 01.02 row is 01.00 + 0x2dd0. An unknown build gets no hook
  ("unavailable").
- The stubs keep rdi, rsi, rdx, rcx, r8 and r9 and align rsp to 16 at the helper call (6 pushes + `sub rsp, 8`).
  The pre-render stub reloads rdi from `[rsp+0x30]` before calling Advance, then jumps to UpdateMultiGPUResources
  with PreRender's return address. Stubs outside rel32 range: no hook (`rel32_to`).
- The helpers run on the render thread only. `particle_fx` and the counters have no locks.
- 8 slots by FFXSystem pointer (`PARTICLE_FX_SLOTS`), never freed. A 9th distinct FX system gets no guard,
  silently and uncounted. A new system at a freed one's address inherits its `simulated` bit (one wrong decision).
  Assumption (class C): a session never has more than 8 FX systems. FX systems are per scene, and level loads may
  create new ones. Not checked.
- With the stock 1/30 s step both pairs are equal after each frame, so the guard changes nothing visible there
  [inferred, study]. Toggle2 bit 16384 lets the game's flips through live, and the hooks keep counting. The bit is
  defined twice (`TOGGLE2_PARTICLE_FLIP_GUARD_OFF` in runtime_extra.c, `ParticleFlipGuardOff` in gow3_toggles.h).
- Installed only with `GOW3_PARTICLE_FLIP_GUARD=1` at start, the GPU enabled and both sites inside a mapped
  segment (`install_particle_flip_guard`); otherwise one "unavailable" line and no hooks.

## Interactions

- single-particle-pass: the only mode where it matters; Deck Optimized turns on both (`DECK_SWITCHES`). Opt-in on
  Windows, so the PC keeps the pass off until a `lab` regression run. particle-single-fixed: tested together in
  fix 1, not needed (fix 2). lineage-toggle-file: second word 16384, next to 8192. draw-classes: "on the previous
  run's target" 0.00 with the guard on. game-update-0102: the 01.02 rows follow its mapping rule.

## Evidence

- Particle fix 1 (Deck, 01.02, `data/deck-particlefix-1.log`): most windows of 1,200 simulations have 0-8 flips
  without a simulation. At the test spots there are 966-1,200 (once 1,140 of 1,140), about one extra flip per
  simulation. With the guard off there, 2,400 flips per 1,200 simulations. The user: lone torch "it's animating now", frozen again with the guard off.
  Crypt floor: "That glitch is also gone"; guard off: "Yup, it's back". With the fixed-step patch also on, vs stock:
  81.2 vs 79.5 FPS, 1,102 vs 1,295 draws, stage B 85.9 vs 91.8%, p99 15.64 vs 15.94 ms.
- Particle fix 2 (guard alone): "the torches still worked", "the crypt floor looks fine".
- PC: not verified in game.

## History and pitfalls

- The game event behind the extra flip is not identified (the study guesses a scene render that flips without
  simulating). Failure sign: particles that freeze or jump after a level load, with growing "skipped" counts.
