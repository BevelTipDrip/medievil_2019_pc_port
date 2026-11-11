---
id: game-ui-cache
title: Game UI cache: level names, loading screens, menu frame, button prompts and fonts from the player's paks
area: launcher
status: active
platforms: all
default_windows: on
default_deck: on
origin: ours
class: M
commits:
files: scripts/game_ui_assets.py, run.py
symbols: build, decode, levels, Paks
switches:
tests: T-SV5
docs: study-save-slots.md#t-sv5-details-level-pictures-and-the-games-menu-look-2026-11-10-launched-by-claude-with-the-users-go-logs-dataload-details-1-0102log-dataload-style-1-0102log-dataload-style-2-0102log
depends_on:
related: load-list-details
checked:
---
## What it does

At each start (run.py, after the save backup) scripts/game_ui_assets.py fills `<user>/cache/game_ui/` from the
installed paks, once per game folder and VERSION (`source.txt`): `levels.tsv`, `levels/level_NN.png` (480x270
loading screens), `menu/frame_options.png`, `prompts/PS4_*.png` (every button prompt) and `fonts/title.ttf`,
`fonts/text.ttf`. One cache for any overlay part (the Load list; next, controller and keyboard glyphs). Nothing of
the game is in the repo or the release.

## Invariants

- Textures decode only as single-mip DXT1 / DXT5 / B8G8R8A8 in PS4 1D thin micro tiling, padded to whole 8x8-element
  tiles; anything else is skipped, never guessed. Width and height are read 16 / 12 bytes before the pixel format
  name.
- Needs Pillow (both packages ship it). Any failure prints "Game UI files skipped" and the game starts; the overlay
  then uses its plain look.
- Bump VERSION when the cache's contents change, or old caches are kept.

## Interactions

- load-list-details reads levels.tsv, the pictures, the frame, three prompts and both fonts.

## Evidence

- 2026-11-10: 23 level names, 23 pictures, 22 of 22 menu, prompt and font files in about 2 s; the pictures match a
  numpy reference decode. T-SV5 verified the look in game.

## History and pitfalls

- Started as `cache/load_menu` (Load list only); generalised the same day at the user's suggestion, for the glyph
  work.
- A Python heredoc turned `b'\0'` into a NUL byte in the source once; edit such scripts from files.
