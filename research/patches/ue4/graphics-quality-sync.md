---
id: graphics-quality-sync
title: Graphics quality and its 18 settings in sync; Console is the PS4's exactly (shadows included)
area: ue4
status: active
platforms: all
default_windows: on
default_deck: on
origin: ours
class: M
commits: a75f7a0
files: scripts/graphics_options.py, scripts/ue4_override.py, run.py, launcher/medievil_launcher.py, gpu/shim/gow3_settings.h, gpu/shim/gow3_settings.cpp, gpu/shim/gow3_overlay.cpp, tests/test_launcher_controls.py, tests/test_graphics_settings.cpp
symbols: GRAPHICS_SETTINGS, QUALITY_LEVELS, LEGACY_TOKENS, ALWAYS_CVARS, level_choices, graphics_choices, graphics_migration, matched_level, graphics_cvars, UE4ApplyLevel, UE4MatchedLevel, UE4LevelChoice, UE4DeckLevel, quality_choice_var
switches:
tests:
docs: plan-graphics-settings.md#plan-graphics-quality-and-the-individual-graphics-settings-in-sync
depends_on: live-graphics-options
related: deck-preset, launcher-graphics-page, ue4-cvars-tool
---
## What it does

Graphics quality is the only preset (the user's design, 2026-11-08). Picking a level sets all 18 settings
(`level_choices`); the quality shows the level they all match, else Custom (`matched_level`, `UE4MatchedLevel`),
in the launcher and the in-game menu. Settings hold real values, the PS4's marked "(PS4 default)". run.py writes
every setting's console variables into the override pak (`graphics_cvars`); 13 also switch live in the game.

## Invariants

- One table: scripts/graphics_options.py `GRAPHICS_SETTINGS`; the game's `UE4LiveOptions` is generated from it and
  `test_graphics_options_match_the_game` compares every field. Change the Python table, then regenerate.
- Every difference between quality levels is a setting: no cvar may be set by the level alone (diag_* presets
  apart), or Custom and the level would hide it. Every level's column differs from every other (test).
- Column order is `QUALITY_LEVELS` = the launcher's `QUALITY_CHOICES` = the game's `UE4QualityKeys`, then Deck.
- Old inis are translated (`graphics_migration` in run.py's prepare and the launcher's load; `Set` in C++ for a game
  started without run.py): "preset" and missing keys from ue4_quality, `LEGACY_TOKENS`.
- The launcher's `load_ini` must not fill graphics keys from `INI_DEFAULTS` (Console's): a filled key looks set and
  the migration keeps it. `test_loaded_ini_takes_missing_settings_from_its_level` goes through `load_ini`.
- Shadow caching stays on for every level (`ALWAYS_CVARS`, as upstream's presets did).
- The 5 settings that are not live apply at the next start; `RestartChanges` counts them, not ue4_quality.
- `GOW3_UE4_CVARS` (extra-env) still wins over the settings in the pak: remove test lines after a test.

## Interactions

- live-graphics-options: its table and live requests are this card's (Preset removed).
- deck-preset: Deck Optimized is the table's deck column (LOD Low, the measured flat-60 values); it shows while
  every setting matches it, else Custom; picking another level ends it and its switches, as before.
- ue4-cvars-tool: `GOW3_UE4_CVARS` overrides the settings (the PS4 shadow test used it).

## Evidence

- PS4 shadow test (PC, 2026-11-08, `data/t-ps4-shadows-1.log`): the PS4's shadow values cost nothing measurable in
  the heavy view; the user: "It looks like their patch wasn't necessary". The Deck is not tested.
- Python tests 57 OK; C++ `graphics-settings-test` (legacy tokens, matched level, apply, Deck) passes; launcher smoke
  test in a hidden window (old ini read as Custom, pick High, change one: Custom, back: High, Deck, Low).
- Fix built `a75f7a0`, not run in the game.

## History and pitfalls

- Upstream `c2ad62e` (2026-10-13) had fixed shadow sizes, 2 cascades and small-object culling for every level
  (flicker, ~4,000 draws); Console now uses the PS4's values again.
- The launcher's Quick settings box reset an unknown quality to its first entry (Ultra): it now shows Custom.
- The first Deck column had LOD "PS4"; the measured Deck value is Low (fixed before commit).
- Deck, 2026-11-08, build `e3112f4` (confirmed in its log and a launcher replay of the Deck's ini): the launcher
  gave a Low ini the five new keys at Console's values (`load_ini` started from `INI_DEFAULTS`); small object
  shadows 0.01 and refraction 2 reached the game (sun shadows and foliage were changed by the user). The tests had
  checked `graphics_migration` alone. Fix built (this card's next commit), not run.
