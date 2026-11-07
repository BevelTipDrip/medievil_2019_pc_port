---
id: frame-limit-parse
title: Any frame limit 20-1000 accepted (an unlisted value such as 119 had become unlimited); 119 / 237 in the menus
area: present
status: active
platforms: all
default_windows: on
default_deck: on
origin: ours
class: G
commits: fafe997a8f
files: gpu/shim/gow3_graphics.h, gpu/shim/gow3_settings.cpp, gpu/shim/gow3_overlay.cpp, launcher/medievil_launcher.py, tests/test_graphics_settings.cpp
symbols: ParseFpsLimit, FpsLimits, IndexOf, FPS_LIMITS
switches:
tests: T-F26, T-F27
docs: fix-inventory.md#6-frame-pacing-and-present, plan-stage-b-split.md#t-f2-which-barriers-break-the-passes-log-t-f2-barrier-breaks-trace-33, plan-stage-b-split.md#t-f27-the-capped-mode-that-works-and-the-launcher-consolidation-2026-10-25
depends_on:
related: vrr-auto-limits, reflex, vsync-control
verified: 2026-11-07
---
## What it does

`medievil.ini`'s `fps_limit` goes through `Gow3Graphics::ParseFpsLimit` (`gow3_graphics.h`, called from
`gow3_settings.cpp` when the ini is read). That function (upstream's) kept only the listed values (`FpsLimits`) and
turned anything else into 0, meaning unlimited. It now also keeps any value from 20 to 1000. `fafe997` also added 119
and 237 to the in-game menu and the launcher: one under half and three under the full refresh of a 240 Hz G-Sync
display. No switch: the parse runs on every start.

## Invariants

- Any value in `FpsLimits` (the negative auto values included), or 20-1000, is returned unchanged. Anything else
  (1-19, over 1000, any other negative) is 0, unlimited, silently. A new auto code (another negative value) must be
  added to `FpsLimits` and `ResolveFpsLimit` together, or the parse makes it unlimited.
- An unlisted value is kept and applied, but the in-game `ChoiceRow` has no label for it. Changing the choice in the
  menu replaces it.
- `current_limit` caps the result at 1000 only for the toggle-file cap. A parsed limit at or above the vblank tick
  still becomes 0 there.
- Unit test: `tests/test_graphics_settings.cpp` checks 240 and 75 are kept and 5 gives 0.

## Interactions

- vrr-auto-limits: the auto values are listed, so this change does not affect them. The fixed 119 is the T-F27
  setting.
- reflex: an unlimited parse means Reflex interval 0, the T-F26 log line.

## Evidence

- T-F26: a 119 cap was meant, but the log showed "Reflex interval 0", so the run was uncapped: 240 avg, 1% low 200,
  0.1% 180.
- T-F27 then ran the 119 cap: the user, "looks great, huge improvement". This confirms the value now reaches the
  limiter.

## History and pitfalls

- The bug was silent: no log line says a value was rejected. Check the log for "VideoOut: frame limit N FPS", or with Reflex
  "Swapchain: Reflex sleep mode, minimum interval N us" (with Reflex the VideoOut line always says 0: the driver
  paces), whenever an ini limit is changed by hand.
