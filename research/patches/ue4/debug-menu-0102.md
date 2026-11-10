---
id: debug-menu-0102
title: Debug menu on the touchpad click (01.00 and 01.02): its key renamed in Dan's and the main menu's Blueprints; R3 keeps recentering the camera
area: ue4
status: active
platforms: all
default_windows: off
default_deck: off
origin: ours
class: M
commits: 4ba06ad, 90255b8
files: scripts/ue4_override.py, tests/test_ue4_package.py, launcher/medievil_launcher.py, gpu/shim/gow3_overlay.cpp
symbols: DEBUG_MENU_KEY, DEBUG_MENU_KEY_ASSETS, name_hashes, package_header, rename_package_name, debug_menu_key_assets
switches:
tests:
docs: plan-work-queue.md, plan-0102.md
depends_on:
related: game-update-0102
---
## What it does

The developers' debug menu (launcher > Game > Cheats, ini `cheat_menu=1`: `AllowDebugMenu=true` and the `DebugHUD`
edits in the override pak) opened with R3 on 01.00 but never on 01.02.

- 01.02's `DefaultInput.ini` adds one line: `RecenterCamera` on `Gamepad_RightThumbstick`. 01.02's
  `BP_InputDelegateActor` and the camera areas listen for that action, and its binding takes the button.
- The R3 that opens the menu is a key event in two Blueprints, which check `AllowDebugMenu` themselves:
  - `Characters/Dan/BP_df_character` (gameplay);
  - `UI/MainMenu/MainMenuPawn` (main menu).
  Found by scanning every asset of 01.00 (19,155) for the key name. "OpenDebug" is not in the eboot.

With the debug menu on, the override pak carries both assets with their key's name map entry renamed from
`Gamepad_RightThumbstick` to `Gamepad_Special_Left` (`rename_package_name`):
- the name is renamed and both name hashes are recomputed;
- every summary offset behind it and every export SerialOffset shift by the size difference (-3 bytes);
- the edited header is read back before it is used.

The touchpad click then opens the menu on 01.00 and 01.02. Through the port it is Back / View on Xbox pads and Tab on
the keyboard. R3 keeps recentering the camera on 01.02. The user chose the touchpad (2026-11-09): "The touchpad just
mimicks the select/start button".

## Invariants

- Name hashes are UE 4.21's: `Strihash_DEPRECATED` on the ANSI name with the MSB-first CRC table, and `StrCrc32` at
  four bytes per character. They reproduce every entry of both assets (3,851 of 3,851) and are pinned in
  `test_ue4_package`.
- Only offsets behind the renamed entry shift (the summary's TotalHeaderSize, Export / Import / Depends / AssetRegistry
  / Preload offsets, BulkDataStartOffset, each export's SerialOffset). The `.uexp` is unchanged: exports keep their
  positions relative to the header's end.
- In `package_header`, a count read and its skip are separate statements: `pos += take('<i') * n` reads `pos` before
  `take` moves it (the first version mis-parsed every package that way).
- L3 is not free (`ToggleForcedWalk`). The touchpad has no action in DefaultInput.ini; the game's control scheme
  tables (`KeyRemapperTable_Classic` / `_Modern`) list it.

## Interactions

- game-update-0102: the other 01.02 differences; the pak's config edits already applied there (log line "debug menu").
- The `DebugHUD` edits (closing the menu resumes the AI) are unchanged. The menu's toggle is the same key event, so
  the touchpad closes it too [inferred].

## Evidence

- Dry run on both builds' assets: one name changed per asset; 666,024 -> 666,021 and 30,721 -> 30,718 bytes (01.00),
  674,557 -> 674,554 and 30,721 -> 30,718 (01.02); the header after the name map byte-identical apart from shifted
  offsets; self-check passed. Python tests OK (`test_ue4_package` 3, launcher and settings suites). Not run in game.

## History and pitfalls

- First version (`4ba06ad`, replaced before any run): dropped 01.02's R3 RecenterCamera mapping so R3 opened the menu
  again and no longer recentered the camera. The user preferred another button.
