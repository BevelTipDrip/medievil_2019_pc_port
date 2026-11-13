---
id: single-particle-pass
title: Single GPU particle pass (r.GPUParticle.FixDeltaSeconds=0): on by default with the flip guard, live switch
area: ue4
status: active
platforms: all
default_windows: on
default_deck: on
origin: ours
class: M
commits: 27ec51e36d, 8d85929db0, d20f010416, 0bff4d6
files: scripts/ue4_override.py, scripts/graphics_options.py, launcher/medievil_launcher.py, src/runtime_extra.c, gpu/shim/gow3_overlay.cpp, gpu/shim/gow3_settings.h, gpu/shim/gow3_toggles.h
symbols: ue4_particle_pass_poll, runtime_ue4_particle_single_pass, particle_wanted, PARTICLE_FIXED_STEP, TOGGLE2_PARTICLE_FIXED_STEP, runtime_ue4_request_cvar, ue4_cvars, PERF_SWITCHES, PERF_DEFAULT_OFF, DECK_PRESET, PerfLive, ParticleFixedStep
switches: perf_particle_single_pass, GOW3_PARTICLE_FIXED_STEP, toggle2:ParticleFixedStep
tests: T-H3b, T-0102-1, T-0102-2, T-0102-3, T-0102-4
docs: plan-stage-b-split.md#t-h3b-one-gpu-particle-pass-2026-10-21-2310-rgpuparticlefixdeltaseconds0-4k-frame-stats-log-datat-h3b-particles-fixdelta0log, plan-0102.md#t-0102-4-2026-11-01-1955-launched-by-claude-with-the-users-go-build-723a893-log-datat-0102-4log, plan-linux-deck.md#single-gpu-particle-pass-live-2026-11-05-d20f010-built-not-run, plan-linux-deck.md#particle-1-results-2026-11-05-1946-build-d20f010-test1105-log-datadeck-particle-1log, study-particle-fixed-step.md#answer, study-heavy-frame-map.md#2-h3-gpu-particle-double-run
depends_on: particle-flip-guard
related: particle-single-fixed, particle-identity-skip, deck-preset, perf-patches-ini, live-graphics-options, draw-classes, lineage-toggle-file, mausoleum-toggle-bisect
checked: 2026-11-13
---
## What it does

With UE 4.21's default `r.GPUParticle.FixDeltaSeconds` (1/30 s) the GPU particle simulation runs twice a frame:
run 1 (previous pair -> current pair) and run 2 (current -> previous: a look-ahead for flag-8 emitters, a plain
copy for all others); readers draw from run 2's output. With 0 there is one run of the frame time into the
current pair and everything reads it: ~193 draws a frame fewer. The override pak sets 0 at boot unless
`GOW3_PARTICLE_FIXED_STEP=1`; since `d20f010` the runtime switches it live (`ue4_particle_pass_poll`).

## Invariants

- One pass only works if every simulation reads the pair the previous one wrote: a state flip without a
  simulation loses a step (slow / frozen torches, the crypt floor ember). The stock 1/30 s copy hides this. The pass
  may be on by default only while particle-flip-guard is on by default too: since 2026-11-13 both are
  (`PERF_DEFAULT_OFF` empty, `perf_particle_flip_guard` on without a key). Turning the guard's default off again
  means turning this default off with it. An explicit `perf_particle_single_pass=0` stays off.
- The off value is the string `PARTICLE_FIXED_STEP` "0.033333335" (float 0x3d088889, the registered default).
  "0.0333333" is 9 ulps off, so it would not be stock. Values must fit `ue4_cvar_values` (at most 15 chars):
  `runtime_ue4_request_cvar` drops longer ones without a word.
- Effective value: toggle2 bit 8192 set -> fixed step; else the menu's `particle_wanted`, or before the menu was
  drawn the start value `!(GOW3_PARTICLE_FIXED_STEP=="1")`, which must match what `ue4_override.apply` put in the pak
  (a `GOW3_UE4_CVARS` entry for this cvar wins in the pak and breaks that). The cvar is written only when the
  effective value changes (`applied`, which starts at the start value).
- Live writes happen only from `runtime_ue4_setres_poll` once `reads>=600` (the main menu: earlier, the engine's own
  settings win), on the game's pad thread, and only when the `ue4_cvars` row's registration bytes match (01.00
  0x5c14d18 / 0x23933b3, 01.02 0x5c14d58 / 0x2396183). Otherwise it logs "unavailable".
- Three places read the same default and must agree: `PERF_SWITCHES` / `PERF_DEFAULT_OFF` (graphics_options.py, used
  by run.py), the launcher's `INI_DEFAULTS` (built from `PERF_DEFAULT_OFF`), and `gow3_settings.cpp` (the ini key, then
  `GOW3_PARTICLE_FIXED_STEP`). `gow3_settings.h` starts the pass at true: the menu shows it off only because run.py
  exports `GOW3_PARTICLE_FIXED_STEP=1` when the ini key is 0 (a missing key is on since 2026-11-13, so all three
  agree on on).

## Interactions

- particle-flip-guard makes it correct; both on by default since 2026-11-13 (before, the guard was a Deck switch,
  opt-in on Windows, and the PC kept the pass off). The menu notes when the pass is on but this start has no guard. particle-single-fixed: tried with it, not needed.
  particle-identity-skip: would have cut the same run-2 draws on stage B.
- lineage-toggle-file: second word 8192; `deck.sh ab "0 0" "0 8192"` compares only with the ini's pass on.
- draw-classes: "particle sim runs" per frame (1.00 vs ~2). live-graphics-options: same `ue4_cvars` path.
  mausoleum-toggle-bisect ruled it out.

## Evidence

- T-H3b (PC 4K, 2026-10-21): per the user, particles, fire and fog looked fine and the heavy area slightly better
  (paraphrased in the doc). Draw counts inconclusive.
- T-0102-1: lone torch slow, ember / golden stream in Dan's crypt. T-0102-2 (toggle groups) and T-0102-3 (HZB off):
  no change. T-0102-4 (pass off): per the user, the slow torch was gone and so was the crypt ember (paraphrased).
- Deck particle 1 (`data/deck-particle-1.log`, GPU auto, 67% uncapped): 79.4 vs 77.1 FPS, 1,122 vs 1,316 draws,
  sim runs 1.00 vs 1.98, stage B 86.3 vs 89.1%, p99 16.30 vs 16.74 ms. The user: "the torch and the small glitch on
  the floor do return with it on"; at a 60 lock "made no difference" (the bug is there at 60 too).
- Deck round 2 (GPU 1,200 MHz): 80.60 vs 77.90 FPS, worst window max 18.8 vs 36.0 ms.

## History and pitfalls

- On by default in `27ec51e` (2026-10-21), off in `8d85929` (2026-11-01), live in `d20f010`. Why the bug showed only
  after hours of play is open. Wrong guesses: a dt effect (same at 60 and 79-240 FPS); "run 2 fires less near 60 FPS"
  (rescore A6: it fires 1.98 times a frame at ~78 FPS). Particles alive at a switch can outlast it: turn away first.
- On by default again since 2026-11-13 (`0bff4d6`, the user's decision once the guard was on by default). Launcher
  and menu hints now say it needs Particle flip guard.
