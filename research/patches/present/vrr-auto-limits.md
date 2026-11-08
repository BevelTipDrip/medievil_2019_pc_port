---
id: vrr-auto-limits
title: Auto frame limits for VRR: below refresh (Reflex rule) and below half refresh; refresh / refresh/2 without VRR
area: present
status: active
platforms: all
default_windows: on
default_deck: off
origin: ours
class: G
commits: 9b92c4a707, 0c357bc1cb, 50290a5781
files: gpu/shim/gow3_graphics.h, gpu/shadps4/core/libraries/videoout/driver.cpp, gpu/shim/gow3_overlay.cpp, gpu/shim/gow3_settings.h, gpu/shim/gow3_settings.cpp, launcher/medievil_launcher.py
symbols: ResolveFpsLimit, FpsLimits, fps_limit, display_vrr, current_limit, Gow3DisplayRefreshHz, FPS_LIMITS
switches:
tests: T-F19, T-F26, T-F27
docs: fix-inventory.md#6-frame-pacing-and-present, plan-stage-b-split.md#t-f27-the-capped-mode-that-works-and-the-launcher-consolidation-2026-10-25, study-vrr-present-pacing.md
depends_on: frame-limit-parse
related: reflex, upstream-vsync-limit-rule, vsync-control, precise-limiter, deck-preset
checked: 2026-11-07
---
## What it does

`fps_limit` values below 0 are resolved against the display's refresh by `Gow3Graphics::ResolveFpsLimit(value,
refresh, vrr)`, called from `current_limit` in `PresentThread` and from the in-game menu. -2, "Auto: below refresh",
gives refresh - refresh^2 / 3600 (NVIDIA Reflex's rule) with VRR, and the refresh itself without. -3, "Auto: below
half refresh", the default (`Gow3Settings::fps_limit{-3}`, launcher `fps_limit=-3`), gives refresh / 2 - 1 with VRR
and refresh / 2 without. -1 is always refresh / 2. `display_vrr` ("G-Sync / FreeSync display", default on) chooses
between the two. The menus also list fixed values (119, 141, 237). `fps_limit` and `display_vrr` are `medievil.ini`
keys and live settings, not `GOW3_*` switches.

## Invariants

- `refresh` is `Gow3DisplayRefreshHz()`: the primary display's mode (SDL), rounded, at least 60, and read once.
  Assumption: the game runs on the primary display and its refresh does not change during the run. Otherwise the auto
  limits follow the wrong refresh, and at exactly half of the real one the VRR flicker returns.
- The in-game `fps_labels` and `Gow3Graphics::FpsLimits` (11 values) must keep the same order and length:
  `ChoiceRow` maps a label index to a value. The launcher's `FPS_LIMITS` pairs each value with its own label.
- The resolved limit then passes through `current_limit`'s other rules: it becomes 0 at or above the tick, and with
  VSync on at refresh - 1 or above (upstream-vsync-limit-rule). So "below refresh" on a fixed-refresh display with
  VSync on leaves the cap to VSync, as intended. With VRR, 116 at 120 Hz stays a cap, but at 60 Hz "below refresh"
  gives 59, which the VSync rule also zeroes.
- `ResolveFpsLimit` returns 0 (unlimited) for any other negative value.

## Interactions

- frame-limit-parse: the ini values only reach this code if `ParseFpsLimit` accepts them (119 did not before `fafe997`).
- reflex: the resolved limit becomes Reflex's `minimumIntervalUs`. The T-F27 mode is Reflex + VSync + 119.
- vsync-control: the launcher's Display / Performance pages that write these keys.
- deck-preset: `fps_limit=60`, `display_vrr=0`, so the auto values are not used there (the Deck LCD has no VRR).

## Evidence

- T-F19: RTSS at 119.98 flickered visibly, at 119 barely. T-F26: the 119 test was lost to the parse bug.
- T-F27 (Reflex + VSync on + 119 on the 240 Hz OLED): the user, "looks great, huge improvement", flicker gone. At
  exactly 120 (half the refresh) the VRR display flickered with any limiter, RTSS at 119.98 included.
- The user on fixed-refresh displays: the offsets below the refresh are only for VRR; a fixed refresh wants exactly
  the refresh or a fraction of it. That led to `display_vrr` (`50290a5`).
- Not measured: the auto values on 144 / 165 Hz displays, FreeSync, and the "below refresh" choice in game.

## History and pitfalls

- Before `9b92c4a`, any negative value meant refresh / 2. `9b92c4a` added -2 / -3 and 141, `0c357bc` made -3 the
  default (`Gow3Settings`, launcher), `50290a5` made the offsets depend on VRR.
- The comments do not match the integer arithmetic. The comment in `gow3_graphics.h` and T-F27 list 138 at 144 Hz and
  225 at 240 Hz. `int(refresh * refresh / 3600)` truncates, so the code gives 144 - 5 = 139 and 240 - 16 = 224
  (116 / 171 / 189 at 120 / 180 / 200 match). Both are still below the refresh. Decide which is meant before
  quoting the numbers.
- The hypothesis behind the half-refresh rule (the VRR controller switching modes at exactly half its refresh) was
  never measured on the display side.
