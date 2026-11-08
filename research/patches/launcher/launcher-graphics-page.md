---
id: launcher-graphics-page
title: Launcher > Performance: the 13 graphics settings above Frame rate; frame rate on the Display page; new default patches start on
area: launcher
status: active
platforms: all
default_windows: on
default_deck: on
origin: ours
class: G
commits: 3cea271054, bc93780e3c
files: launcher/medievil_launcher.py, tests/test_launcher_controls.py, gpu/shim/gow3_settings.h, gpu/shim/gow3_settings.cpp
symbols: GRAPHICS_OPTIONS, build_performance, build_display, FRAME_TRACE, game_patch_selection, chosen_by_run_py, PATCHES_AFTER_SEEN_LIST, store_game_patches, game_patches_seen, UE4LiveOptions, RequestUE4Live
switches: present_frame_trace
tests: T-0102-6
docs: plan-live-switches.md#done, plan-0102.md#t-0102-6-2026-11-01-2338-2352-launched-by-claude-with-the-users-go-build-548db4c-log-datat-0102-6log
depends_on: live-graphics-options
related: particle-bounds, vsync-control, presentation-settings, perf-patches-ini, deck-preset, flip-trace
verified: 2026-11-07
---
## What it does

`3cea271`: launcher > Performance opens with "Graphics settings", one list per `GRAPHICS_OPTIONS` entry (shadow
resolution / distance / filtering, view distance, LOD, AO, reflections, volumetric fog, motion blur, depth of field,
bloom, lens flares, chromatic aberration). They use the same `medievil.ini` keys, ini tokens, titles and labels as
the in-game `UE4LiveOptions`. "Preset" (the default) follows Graphics quality. The game applies a non-preset value
once it reaches its main menu (`Load` -> `RequestUE4Live`). `bc93780`: Frame rate and Advanced presentation move to
the Display page, and the frame timing trace (`FRAME_TRACE`) moves to Advanced > Diagnostics. The Patches page's
saved selection now also keeps `game_patches_seen`, and `game_patch_selection` turns on a default patch the page
never showed.

## Invariants

- `GRAPHICS_OPTIONS` must match `UE4LiveOptions` in gow3_settings.h entry for entry: key, title, labels and tokens,
  with an `INI_DEFAULTS` value of `preset`. `test_graphics_options_match_the_game` parses the header to check this.
  A launcher token the game does not know is ignored at load (the row reads as Preset in game).
- `game_patch_selection`: a saved selection is the whole list (`GOW3_PATCHES_ONLY=1`). A default patch is added
  only when it is not in `game_patches_seen` for that title. A default the user switched off after the page showed
  it stays off. `store_game_patches` must keep adding every shown patch to the seen set.
- With no `game_patches_seen` (selections saved before 2026-11-02), every patch except `PATCHES_AFTER_SEEN_LIST`
  counts as seen. A future default patch has to be added there, or it stays off for those installs.
- `chosen_by_run_py` patches (resolution, frame rate, boot size, 4K boot) never appear on the page and never enter
  the selection. run.py picks them from medievil.ini.
- Selections are keyed by title id, not by app version (01.00 and 01.02 share `CUSA11227`). A name that exists in
  only one version is filtered by `AppVer` when the page builds. `store_game_patches` replaces the title's list with
  the shown version's patches but unions the seen set. Once the page was shown for both versions, a patch that only
  the other version has is neither chosen nor new there: it starts off even if it is a default (read from the code,
  not seen).

## Interactions

- live-graphics-options is the game side (cvars, Preset restore through `GOW3_UE4_PRESET_CVARS`). This card is only
  the launcher rows.
- particle-bounds: the default patch the seen list was made for ("GPU particle bounds" stayed off for launcher
  starts until `bc93780`).
- deck-preset writes these keys (shadows 256 / short / 1, view 0.7, ...) from `DECK_PRESET`.
- vsync-control / presentation-settings: the controls that moved to the Display page.

## Evidence

- T-0102-6 (build `548db4c`, run before `bc93780`, with the patch selected): the user confirmed "the dust no
  longer vanishes with the camera". `bc93780` makes launcher starts select it. plan-0102.md: "The user's settings
  now give both default patches."
- The 13 options: plan-live-switches.md says "Not yet verified in game: the user tests them one by one".

## History and pitfalls

- Before `bc93780` the Performance page held the frame-rate controls, and the frame trace was a Presentation
  checkbox. It is not the 5 s frame statistics.
- Tests: `test_new_default_patches_start_on` covers the old-selection, seen and switched-off cases on the 01.02
  profile.
