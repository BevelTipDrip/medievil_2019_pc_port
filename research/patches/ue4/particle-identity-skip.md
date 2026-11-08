---
id: particle-identity-skip
title: Skip identity GPU-particle sim draws (dt = 0, output equals input; not built)
area: ue4
status: rejected
platforms: all
default_windows: n/a
default_deck: n/a
origin: ours
class: M
commits:
files: gpu/shim/gow3_toggles.h
symbols:
switches:
tests:
docs: impl-draw-reduction.md#3-gpu-particle-dt0-copy-skip-not-built, study-frame-cost-2026-10-29.md#8-skip-identity-gpu-particle-sim-draws-dt0-for-2-frames--goal-3, study-deck-rescore-rejected.md#c-analysed-gated-or-ranked-never-built, study-heavy-frame-map.md#2-h3-gpu-particle-double-run
depends_on:
related: single-particle-pass, particle-flip-guard, draw-classes
verified: 2026-11-07
---
## What it does

Not built. The idea: on stage B, skip a GPU particle simulation draw whose step dt is 0 and whose input equals its
output (both ping-pong buffers already hold the same data), behind `GOW3_PARTICLE_SKIP_IDENTITY=1` with toggle2 bit
64 to switch it off live. Bit 64 is still marked reserved in `gow3_toggles.h` and used by nothing.

## Invariants

- Exact only if (a) the draw has dt = 0 (the shader is then an identity copy), (b) the previous run drew the same
  tiles with dt = 0, and (c) nothing else (the inject draw, tile clears) wrote either buffer on those tiles in
  between. By induction, a chain of skips stays exact.
- dt lives in the per-emitter constants (RenderDoc "binding 1 float [8]"), which are not mapped to the guest V# stage
  B reads. A wrong buffer or offset reads the wrong float, and a false "dt = 0" freezes or corrupts particles.
- The key must be the simulation PS hash, because shapes alone also match tile clears. No doc records the hash.

## Interactions

- single-particle-pass with particle-flip-guard removes the whole second run in the engine, so this skip has nearly
  nothing left to save where the pass is on (Deck). On the PC (pass off) the run-2 copies remain.
- draw-classes (N0) counts sim runs, runs after an inject draw and the sim PS hashes: the inputs a build would need.

## Evidence

- Captures (study-heavy-frame-map H3): two runs of 194 draws. Run 2 is a bit-exact copy. 155 / 127 of 194 sim draws
  are dt = 0 copies (study-frame-cost item 8) [verified in captures, not in game].
- Estimated gain: 0.15-0.28 ms of stage B on the PC, ~0 if a global inject reset is needed. Deck rescore C6: -0.3 to
  -0.8 ms [inferred].

## History and pitfalls

- Analysed, never built: plan-work-queue item 8 "open, risky (frozen particles)". Safety could not be shown without a
  capture that maps dt and more reverse engineering of `SimulateGPUParticles` and the inject order.
- Worth building only for the PC with the single pass off, and only after (1) a capture that maps binding 1 to the PS
  buffer index and records the sim and clear PS hashes, and (2) a stats-only dry run that counts would-be skips. The
  N1 run replay (study-native-render-paths) may be the better first pilot.
