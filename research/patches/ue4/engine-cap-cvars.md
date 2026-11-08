---
id: engine-cap-cvars
title: rhi.SyncInterval=0; t.MaxFPS / the engine frame-rate range as a cap (no effect in this build)
area: ue4
status: rejected
platforms: all
default_windows: off
default_deck: off
origin: ours
class: M
commits:
files: scripts/ue4_override.py, run.py
symbols: frame_rate_range
switches:
tests: T-L0, T-Z4, T-Z4c, T-Z4d, T-F13
docs: plan-stage-b-split.md#t-l0-rhisyncinterval0-2026-10-21-0128-0133-launched-by-claude-with-the-users-go-build-5231983-log-datat-l0-syncinterval0log, plan-stage-b-split.md#t-z2-t-z4b-dbuffer-decals-and-the-forced-prepass-2026-10-21-0200-0314-launched-by-claude-with-the-users-go, fix-inventory.md#10-tried-and-dropped, plan-linux-deck.md#deck-optimized-preset-2026-11-05-launcher-built-not-run
depends_on:
related: game-update-0102, precise-limiter, reflex, vsync-control, deck-preset, ue4-cvars-tool
checked: 2026-11-07
---
## What it does

Three UE4-side frame controls were tried as config cvars through the override pak (`GOW3_UE4_CVARS`) or the ini:
`rhi.SyncInterval=0` (T-L0: did the vblank quantise frame times?), `t.MaxFPS=120` (T-Z4), and the engine frame-rate
range at 120 as a cap (`engine_fps=120` -> `SmoothedFrameRateRange`, T-Z4c, plus `r.VSync=1` in T-Z4d). None
engaged. Every working cap above 60 is the port's present-thread limiter. Nothing of them is in the build.
`frame_rate_range` is not one of them: it is upstream 1.2.0's unlock (`86fce58`), listed here because T-Z4c used it
as a cap; it only lifts the stock 60 FPS bound when `engine_fps > 60`.

## Invariants

- Do not rely on UE4's config cvars for a cap above 60 in this build. The cap is the port's limiter
  (precise-limiter, reflex). `run.py` gives the engine a 1000 FPS bound when `present_no_frame_caps` is on.
- `frame_rate_range(fps)` sets the upper bound to `fps + 2` and is only written when `fps > 60`. Its docstring says
  the stock range (below 62) bounds the game at 60 (UGameEngine::GetMaxTickRate with VSync on). T-Z4c (bound 122)
  ran at 175-182, so above 60 the range is no ceiling. The docs do not explain why the stock bound holds 60 while
  a 122 bound holds nothing; do not build a cap on either reading.
- `rhi.SyncInterval` stays the game's (1). At the 480 Hz tick the vblank did not quantise frames (T-L0), but a
  120 Hz vblank did (run.py comment: 9 ms frames became 17 ms), so above 60 run.py sets `GOW3_VBLANK_HZ=0` and
  `GOW3_FPS_CAP` unless the present settings already set the tick (`present_tick_hz`, 960 by default).

## Interactions

- game-update-0102: on 01.02 the game's own `t.MaxFPS 60` console command did cap at 60, so `t.MaxFPS` does work
  when issued at run time. Only the config value had no effect (T-Z4, 01.00). The cause is not known [inferred:
  SetBy priority or when the value is read].
- deck-preset: avoids `engine_fps=60` as a second clock beside the present limit (two limiters at one rate fight,
  T-F13).
- ue4-cvars-tool carried the test cvars.

## Evidence

- T-L0 (build `5231983`): window medians continuous (5.08, 5.16, 5.58, 5.85 ms...), heavy windows ~160 FPS at ~2,300
  draws, stddev 0.29-0.4 ms. The user: no timing issues, "quite possible that setting has no effect".
- T-Z4: `t.MaxFPS` ignored (ran to 240). The first launch crashed at boot ("unmapped host pthread error -1"), a rare
  fault unrelated to the cvar. T-Z4b (engine_fps=120) stuck on the intro loading screen (intermittent, cause unknown).
- T-Z4c: engine_fps=120 ran at 175-182 FPS. T-Z4d: with `r.VSync=1` the user, "still 240".
- T-F13: RTSS at 120 beside the port's limiter flickers. Two limiters at one nominal rate with independent phases
  fight (the user).

## History and pitfalls

- Tried to rule out vblank quantisation (T-L0) and to find an engine-side cap for the T-Z2/T-Z3 panning spikes.
  Dropped: no effect.
- Worth retrying only through a live set at run time (`runtime_ue4_request_cvar` with a `t.MaxFPS` row, as the game
  itself does on 01.02), and only if an engine-side cap is wanted over the present limiter. Expect T-F13's
  two-clock problem if both run at one rate.
