---
id: ue4-cvars-tool
title: UE4 override pak: GOW3_UE4_CVARS adds console variables (opt-in test tool)
area: tooling
status: diagnostic
platforms: all
default_windows: off
default_deck: off
origin: ours
class: M
commits: b2f9d2612a
files: scripts/ue4_override.py
symbols: apply, preset_cvars, set_values
switches:
tests: T-O1, T-O2
docs: plan-frame-pacing.md#r4a-result-2026-10-18-1405-1415-launched-by-claude-with-the-users-go-rejected, patches-and-pr-proposal.md
depends_on:
related: R4a, occlusion-queries-off, S1-streaming, preshadows-live, effects-resolution, live-graphics-options, extra-env-file, 4k-memory, engine-cap-cvars
checked: 2026-11-07
---
## What it does

`GOW3_UE4_CVARS="name=value;name=value"` (read by `scripts/ue4_override.py`, so not in the C switch list) adds
console variables to the override pak's `[SystemSettings]`. In `apply` they are merged after the presets, the
render scale, the particle step and the other port cvars, so they win on the same name; the startup line ends
"; extra cvars ...". `preset_cvars` (the live options' boot values, the menu's "Preset" restore) applies them too.
Unset: no change. `ue4_override.py` reads `os.environ`; run.py's `env` is `os.environ` itself, so the value
`extra-env.txt` sets (run.py `load_extra_env`, applied before the pak is written) reaches it: the Deck route
without editing Steam's launch options.

## Invariants

- Boot-time only: the pak is written before the game starts, so a change needs a restart. A live option the
  menu sets later (live-graphics-options, preshadows-live, effects-resolution) overrides the boot value of the
  same cvar while the game runs.
- `values.update(extra)` must stay after every port-set value in `apply`; moving it earlier lets a preset win and
  the test silently measures the default.
- Nothing checks the names: a misspelt cvar, or one this build does not have, is written to the ini as is. Keys are
  case-sensitive (`values`, `set_section`): a name in another case than the port's or the game's line adds a second
  line instead of replacing it; which one UE4 uses is untested.
  `[SystemSettings]` is also outranked by the PS4 device profile: `ue4_override.py` notes "this build has no
  r.GarlicHeapSizeMB, and the PS4 device profile's r.Streaming.PoolSize=2000 outranks [SystemSettings]".
- Assumption (class M): the cvar exists in this UE 4.21 build and is read from `[SystemSettings]` at boot. Check
  its effect in the log or in game before trusting a null result (engine-cap-cvars: no effect in this build).

## Interactions

- R4a: `r.NumBufferedOcclusionQueries=2`, rejected. occlusion-queries-off: T-O1 / T-O2 (`r.AllowOcclusionQueries`).
- S1-streaming: `r.Streaming.AmortizeCPUToGPUCopy`, rejected. preshadows-live, effects-resolution: first tested
  as cvars here (`r.Shadow.Preshadows=0`, `r.SeparateTranslucencyScreenPercentage=50`) before they became options.
- 4k-memory: `run-game.ps1` once passed `r.GarlicHeapSizeMB=4600;r.Streaming.PoolSize=1600` through this
  variable (plan-frame-pacing, 2026-10-19); neither can act (see Invariants). The port now grows the game's
  graphics memory through `GOW3_DMEM_MB` (and the 512 MB framebuffer pool patch) instead.
- extra-env-file: the Deck route for setting it.

## Evidence

- R4a (2026-10-18, launched by Claude with the user's go): the spin at guest+0x689c89 fell from ~1.3 M to ~100 k
  calls/s, but the heavy view fell from 94.3 to 75.3 FPS median, p99 14.6 -> 16.2 ms, GPU-tick waits 1.3% -> 21.9%.
  The tool did its job; the cvar was rejected.

## History and pitfalls

- One cvar set per launch keeps results attributable (plan-frame-pacing S1 / S2 runs). `run-game.ps1 -Env
  @{GOW3_UE4_CVARS='...'}` passes it on the PC (T-O2).
- A cvar test that "did nothing" may not have been applied at all: the memory cvars above are the example.
