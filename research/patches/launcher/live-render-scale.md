---
id: live-render-scale
title: Render scale live in the in-game menu, any whole percent from 50 to 200
area: launcher
status: active
platforms: all
default_windows: on
default_deck: on
origin: ours
class: M
commits: 7ab4310696
files: gpu/shim/gow3_settings.cpp, gpu/shim/gow3_settings.h, gpu/shim/gow3_overlay.cpp, src/runtime_extra.c, scripts/graphics_options.py, launcher/medievil_launcher.py, tests/test_graphics_settings.cpp, tests/test_launcher_controls.py, tests/test_graphics_options.py
symbols: RequestRenderScale, LiveRenderScaleMax, LiveRenderScaleMin, RenderScaleNeedsRestart, ParseRenderScale, RenderScaleMin, RenderScaleMax, RenderScaleIniMin, live_render_scale, startup_render_scale, ApplyRenderScale, RenderScaleSection, ue4_cvars, RENDER_SCALE_RANGE, live_render_scale_range, parse_render_scale, render_scale_control
switches:
tests:
docs: impl-live-render-scale.md#how-it-works, impl-live-render-scale.md#limits, impl-live-render-scale.md#what-follows-a-live-change-what-stays-as-started, plan-linux-deck.md#spin-compact-texture-memo-and-the-task-split-2026-11-04, plan-live-switches.md#done
depends_on: live-graphics-options
related: finer-occlusion-culling, effects-resolution, 4k-memory, upstream-ue4-setres, I1, park-unmapped, deck-preset, launcher-graphics-page
checked: 2026-11-07
---
## What it does

In game, Performance > Applied now has a 1% render-scale slider with -/+ buttons (`RenderScaleSection`).
`ApplyRenderScale` sends the value once the slider has been still for 0.3 s or the menu closes.
`RequestRenderScale` clamps it to the live range and requests `r.TemporalAA.Upsampling` 1 / 0 when the scale crosses
100%, then `r.ScreenPercentage`. Both go through `runtime_ue4_request_cvar` on the game thread's next pad poll
(objects and 7-byte sites for 01.00 and 01.02 in `ue4_cvars`), and the log says `Render scale: A% -> B% (live)`. The
launcher shows the presets plus a 50-200 percent box (`render_scale_control`). The ini keeps whole percents; 25-49
still start. On `deck-lab` only (not on `lab`).

## Invariants

- Live range: `LiveRenderScaleMin` = min(start, 50) to `LiveRenderScaleMax` = max(start, 100). The game's memory
  (framebuffer pool, `GOW3_DMEM_MB`) is sized at start from the output size. A larger scene was never tried; upstream
  `783b25a` saw 1800p at 200% run out of memory and crash. A value above the range is saved, counted by
  `RenderScaleNeedsRestart`, and drawn at the top of the range until the next start.
- `RenderScaleMin` / `RenderScaleMax` / `RenderScaleIniMin` (gow3_settings.h) must equal `RENDER_SCALE_RANGE` and
  `parse_render_scale` (`test_render_scale_range_matches_the_game` reads the header). `ParseRenderScale` must parse
  as `int(float(v))` does in Python, or launcher and game disagree on an ini value. It nearly does: `strtod` also
  reads hex (`0x50` is 80 in game, 100 in Python) [code].
- The upsampling cvar must be set before `r.ScreenPercentage` (table order = poll order). Below 100% without it,
  UE4 falls back to its spatial upscale.
- With `ue4_aa` FXAA / off, crossing 100% live keeps the start's anti-aliasing, so it is a restart change.
- Each change reallocates every scene target in UE4. Do not apply per slider step.
- `live_render_scale` is what the game draws at. The ini `render_scale` written on close is the chosen value, and
  the next start boots at it through `ue4_override.apply`.

## Interactions

- finer-occlusion-culling: a binary patch decided at start by `hzb_full_size_environment` from the start scene (on up
  to 1280x800). A live change keeps that decision. On the Deck's 720p / 800p output every live value up to 100% stays
  at most 1280x800, so the decision still holds there. On a PC at 1080p, a start at 50% (960x540, patch on) followed
  by a move to 100% keeps a full-size HZB at 1920x1080. The reverse, a 1080p start at 100% (patch off unless the
  Patches page selects it) moved live to 50%, keeps the stock coarse HZB for the small scene. Neither has been
  measured.
- The HZB pyramid size follows the scene every frame. The HZB remap is unaffected (fixed 256x256 results).
- effects-resolution follows, as a percent of the render size. 4k-memory / I1 stay as started (output size).
- park-unmapped: freed guest targets are parked up to 10 s / 768 MiB (N-24), so a live increase peaks above a start
  at the same scale (inferred).
- upstream-ue4-setres's "Game resolution now" buttons stay disabled after a start above 100%.

## Evidence

- Deck (2026-11-04, plan-linux-deck.md): "Live render scale and the menu redraw fix: verified by the user (works;
  pacing with the menu up normal)". The per-scale numbers of the planned test (impl-live-render-scale.md, "Deck
  test") are not written up. plan-live-switches.md ("Done", render scale row) still says "Not yet verified in game":
  out of date against plan-linux-deck.md.
- Unit tests: graphics-settings-test (parse, live requests, clamp, restart rule, save) and the two Python range tests.

## History and pitfalls

- Inferred, not checked: UE4 4.21 re-reads `r.TemporalAA.Upsampling` every frame. If the game re-applied its
  scalability, a 100% start would snap back, and "Scene now" would not show it because it reports the request.
- Before this commit the render scale was 7 presets, applied only at start. A Deck middle ground between 67 and
  100% cost a restart per try.
