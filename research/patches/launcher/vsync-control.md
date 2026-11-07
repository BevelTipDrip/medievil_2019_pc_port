---
id: vsync-control
title: Launcher: one VSync / present mode control, Frame limiter choice (Reflex default), Engine frame rate with Unlimited
area: launcher
status: active
platforms: all
default_windows: on
default_deck: on
origin: ours
class: G
commits: ebe42c7616, 1b2c5af028, 49040f1e51, 0c357bc1cb
files: launcher/medievil_launcher.py, scripts/graphics_options.py, gpu/shim/gow3_settings.h, gpu/shadps4/video_core/renderer_vulkan/vk_swapchain.cpp, tests/test_launcher_controls.py, tests/test_graphics_options.py
symbols: VSYNC_CHOICES, VSYNC_HINT, vsync_choice_var, PRESENT_LIMITERS, ENGINE_CAP_CHOICES, engine_cap_var, apply_deck_preset, PRESENT_DEFAULTS, FindPresentMode, UpdateVsync, fps_limit
switches: present_mode, present_limiter, present_no_frame_caps
tests: T-F27
docs: plan-stage-b-split.md#t-f27-the-capped-mode-that-works-and-the-launcher-consolidation-2026-10-25
depends_on: presentation-settings
related: present-mode-select, reflex, precise-limiter, vrr-auto-limits, frame-limit-parse, launcher-graphics-page, deck-preset
---
## What it does

A single "VSync" list (`VSYNC_CHOICES`: On / Off / Off, allow tearing / Adaptive) on Home and Display replaces the
VSync checkbox and the separate present-mode list. `vsync_choice_var` is a derived variable. On and Off set
`present_mode=auto` and the `vsync` key, which the in-game menu also switches live. "Off, allow tearing" and
"Adaptive" set `present_mode=immediate` / `fifo_relaxed`, which fixes the Vulkan mode. "Frame limiter"
(`present_limiter`: reflex / precise / standard, default reflex) picks a `PRESENT_LIMITERS` bundle. "Engine frame
rate" (`engine_cap_var`) adds Unlimited, which writes `present_no_frame_caps=1` and `engine_fps=240`. `0c357bc` set
the T-F27 defaults: Reflex, tick 960, precise tick, inline present, and `fps_limit=-3` (Auto: below half refresh,
also `Values::fps_limit` in gow3_settings.h). In the same commit Reflex steps aside on a driver without
`VK_NV_low_latency2` (reflex card).

## Invariants

- A fixed `present_mode` wins over the in-game VSync switch: `FindPresentMode` returns early on
  `GOW3_PRESENT_MODE`. `UpdateVsync` re-runs it but gets the same mode, so with tearing or Adaptive the menu's switch
  does nothing. `VSYNC_HINT` says so, and must keep saying so while the swapchain works this way.
- On and Off must leave `present_mode=auto`, or the in-game switch stops working. `vsync_choice_var` converts the
  older fixed `fifo` / `mailbox` values on load (they only duplicated the switch).
- Derived variables (`store = 'derived'`) are skipped by `collect`. Their state must always be carried by the real
  keys (`vsync` + `present_mode`, `engine_fps` + `present_no_frame_caps`), which their `write` traces set.
- `PRESENT_LIMITERS['reflex']` also carries the precise limiter's variables. Without the driver extension
  `PresentThread` sees `ReflexEnabled()` false and the precise limiter paces instead. Removing them would leave AMD
  and Intel unpaced by the port.
- `apply_deck_preset` sets the two derived lists from `DECK_PRESET` after the keys, so the UI shows the preset.

## Interactions

- presentation-settings turns these keys into variables. present-mode-select is the swapchain side, and its
  Immediate-to-Mailbox fallback exists for gamescope.
- vrr-auto-limits / frame-limit-parse: the Frame rate limit list beside it (`display_vrr`, the 119 / 237 entries).
- deck-preset: VSync off (Mailbox under gamescope), Standard limiter, Unlimited.

## Evidence

- T-F27: Reflex (sleep before the present), VSync on and a 119 cap on the 240 Hz OLED. The user: "looks great, huge
  improvement", with no flicker, confirmed in a second run with RTSS's limiter off. That configuration is these
  defaults (the 119 comes from `fps_limit -3` with `display_vrr=1`).
- Not verified separately: the launcher consolidation itself (labels, conversion of old values).

## History and pitfalls

- `ebe42c7` first offered the raw present modes. `1b2c5af` dropped Fifo and Mailbox because they duplicated the
  in-game switch, and `49040f1` shortened the labels.
- Removed from the launcher in `0c357bc` (environment only): newest-frame, the display-vblank tick, the separate
  precise-limiter switch and the present-wait modes (their cards are rejected).
- The user: the offsets below the refresh are for VRR only, and a fixed-refresh display wants the refresh or a
  fraction of it. That is `display_vrr` (vrr-auto-limits), not this list.
