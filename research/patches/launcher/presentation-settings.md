---
id: presentation-settings
title: Launcher Presentation settings: tick rate, present mode, no built-in frame caps, present switches
area: launcher
status: active
platforms: all
default_windows: on
default_deck: on
origin: ours
class: G
commits: 44994b9bff
files: scripts/graphics_options.py, run.py, gpu/shim/core/emulator_settings.h, launcher/medievil_launcher.py, tests/test_graphics_options.py
symbols: PRESENT_DEFAULTS, PRESENT_FLAGS, PRESENT_LIMITERS, present_environment, no_frame_caps, GetFrameLimit, GetVblankFrequency, GOW3_DERIVED, PRESENT_SWITCHES, engine_cap_var
switches: GOW3_NO_FRAME_CAP, present_no_frame_caps, present_tick_hz, present_frame_trace
tests: T-P120
docs: plan-stage-b-split.md#presentation-settings-in-the-launcher-2026-10-23, plan-stage-b-split.md#t-p120-the-configuration-that-holds-the-cap-2026-10-23-0300-the-user-launcher-settings, plan-linux-deck.md#deck-optimized-preset-2026-11-05-launcher-built-not-run
depends_on:
related: vsync-control, precise-tick, inline-present, present-mode-select, reflex, precise-limiter, flip-trace, deck-preset, launcher-graphics-page, upstream-vsync-limit-rule, engine-cap-cvars
---
## What it does

`medievil.ini` `present_*` keys become variables in `run.py` through `present_environment`, using the defaults in
`PRESENT_DEFAULTS`. The flags are in `PRESENT_FLAGS`, the limiter bundles in `PRESENT_LIMITERS`,
`present_tick_hz` 1-2000 becomes `GOW3_VBLANK_HZ`, `present_mode` (not `auto`) becomes `GOW3_PRESENT_MODE`, and
`present_frame_trace` becomes `GOW3_FLIP_TRACE`. Each one is set only if absent, and its name goes into
`GOW3_DERIVED`, so an in-game "Apply and restart" derives it again. `present_no_frame_caps=1` ("No built-in frame
caps", since 0c357bc the "Unlimited" engine frame rate) does two things. It sets `GOW3_NO_FRAME_CAP=1`
(`GetFrameLimit` returns 0), and run.py raises the engine frame rate to 1000, so UE4's `SmoothedFrameRateRange`
bound becomes 1002 and `GOW3_FPS_CAP` becomes 1000. This card is the plumbing and `present_no_frame_caps`. The other
`present_*` keys are on their own cards.

## Invariants

- `present_environment` must not overwrite a variable already in the environment (`if key not in env`). That lets
  `run-game.ps1 -Env` and extra-env.txt tests win. Derived keys must be listed in `GOW3_DERIVED`, or a restart from
  the menu keeps the old value.
- The defaults live in two places, `PRESENT_DEFAULTS` and the launcher's `INI_DEFAULTS`
  (`test_graphics_defaults_match_the_game` and `test_present_environment` check the main ones). A missing key must
  mean the tested best (T-F27).
- `GOW3_NO_FRAME_CAP` matters only with an automatic tick: `GetFrameLimit` already returns 0 when `GOW3_VBLANK_HZ`
  is above 0, as it is with the default 960 or the Deck's 900. With any explicit tick, the engine bound of 1000 is
  the part of "Unlimited" that has an effect.
- In run.py the present variables are set before the `engine_fps > 60` block, so that block's
  `setdefault('GOW3_VBLANK_HZ', '0')` never replaces the configured tick.
- `fps_limit` (the in-game Frame rate limit) stays in force with no built-in caps. The Deck preset relies on that:
  it is the only clock there.

## Interactions

- deck-preset: `present_no_frame_caps=1`, `engine_fps=240`, tick 900, not precise, Standard limiter. The 1000 bound
  and a 900 Hz tick leave `fps_limit=60` as the one clock. With `engine_fps=60`, UE4's bound would be a second clock at
  the same rate, and two limiters at one rate fight (T-F13).
- The in-game menu has no `present_no_frame_caps` row, but it does have Engine frame rate. Choosing 120 there while
  the key is 1 still starts at 1000 (`no_frame_caps` wins in run.py). This was read from the code, not reported.
- precise-tick, inline-present, reflex / precise-limiter, present-mode-select and flip-trace own the other keys.
  vsync-control and launcher-graphics-page are the launcher UI around them.

## Evidence

- T-P120 (the user, launcher settings): "flat frame-time graph, 1% lows 118-119 at a 120 tick, no cyclical waves,
  with G-Sync on or off". That was the configuration these settings made reachable without variables.
- Deck: the user's flat 60 in the heaviest graveyard view used `present_no_frame_caps=1`
  (plan-linux-deck.md, "A flat 60 on the Deck").

## History and pitfalls

- At first every key defaulted to off, and the newest-frame, vblank-sync and separate precise-limiter keys were in
  the launcher. `0c357bc` removed those (environment only now) and set the T-F27 defaults (vsync-control).
- Before `44994b9`, `GOW3_PRESENT_MODE` existed upstream (`GetPresentMode`) but nothing read it
  (present-mode-select).
- A tick at or below the frame limit paces the game by itself and rounds slow frames up to whole ticks. That is
  why the Deck preset uses 900, not 60.
