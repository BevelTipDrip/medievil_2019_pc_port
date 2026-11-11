---
id: load-list-details
title: Load list: per-save details, level picture and level list (Square), in the game's Options menu look
area: runtime
status: active
platforms: all
default_windows: on
default_deck: on
origin: ours
class: M
commits:
files: src/runtime_saveinfo.c, src/runtime_services.c, src/runtime_savedata.c, src/runtime.h, gpu/shim/gow3_overlay.cpp, gpu/shim/gow3_overlay.h, gpu/shim/gow3gpu.cpp, gpu/gow3gpu.h, tests/test_services.c
symbols: runtime_saveinfo_read, runtime_saveinfo_text, runtime_saveinfo_level_name, runtime_saveinfo_picture, runtime_savedata_dir, save_entry, gow3gpu_choice_style, SetChoiceStyle, StyledChoiceBox, DetailedChoiceBox, ChoiceStep
switches:
tests: T-SV5
docs: study-save-slots.md#t-sv5-details-level-pictures-and-the-games-menu-look-2026-11-10-launched-by-claude-with-the-users-go-logs-dataload-details-1-0102log-dataload-style-1-0102log-dataload-style-2-0102log
depends_on: load-list-continue, game-ui-cache
related: save-date-last-write
checked:
---
## What it does

Each save in our SaveDataDialog list gets a summary line (chalices, levels completed, play time); beside the list,
the chosen save's level picture and details (saved in, last level, chalices, levels completed and open, life
bottles, gold, bestiary entries, play time); Square / Tab switches to every level in the save with its state and
chalice. With the game UI cache present the list is drawn in the Options menu's look (its frame, fonts, colours and
button prompts); otherwise in the plain overlay style.

## Invariants

- runtime_saveinfo.c scans GVAS tagged properties in file order and enters containers; map / array inner type
  names ("EnumProperty", "StructProperty") are skipped. `HealthVials` is the life bottle count; `LastLevelPlayed`
  is a ByteProperty with an enum name.
- The overlay parses the texts runtime_saveinfo_text writes: an entry's first line is its name ("Continue" is
  drawn green); details are "Label: value" lines; the level list is "Name: state[, chalice]" under a heading line.
  Changing one side's format changes the other.
- The styled list keeps the frame's proportions (1181x870), 76% of the height from 6% down, so the prompts end
  above the title screen's copyright line. The fill inside the frame is opaque. Details start under the picture's
  left edge, values from its middle.
- Style files load once (gow3gpu_choice_style, game thread); fonts are added to the ImGui atlas under imgui_mutex
  (1.92 dynamic fonts), textures made on the present thread. All six files or the plain look.
- Square is PS4 bit 0x8000 in the list's input; Tab and the gamepad's west button when SDL events drive it.

## Interactions

- load-list-continue: Continue is the first entry. game-ui-cache: names, pictures and the look.
- tests/test_services.c models OrbisSaveDataDialogNewItem as a struct (title first); the trace line reads it.

## Evidence

- T-SV5 (PC, 01.02): verified. Slot 7: 6 chalices, 7 levels (3 open), 5 life bottles, 1471 gold, 27 bestiary,
  2h 26m. The user: "Yes, everything looks correct". services-test passes (built by hand on Windows: build.sh runs
  the C tests only on Linux).

## History and pitfalls

- First showed "Life bottles" from LifeBottleCollected (0) and "Health vials" from HealthVials (5): the user
  corrected it.
- The services test had not linked since save-trace (no runtime_save_trace_caller stub); fixed here.
