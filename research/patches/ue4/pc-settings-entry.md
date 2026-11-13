---
id: pc-settings-entry
title: PC Settings in the game's pause menu (its hidden Button_Controls), opening the port's settings menu (medievil.ini pc_settings_entry)
area: ue4
status: active
platforms: all
default_windows: on
default_deck: on
origin: ours
class: M
commits: bb7409b, f8fadec
files: scripts/ue4_pc_settings.py, scripts/ue4_override.py, run.py, launcher/medievil_launcher.py, src/pc_settings_hook.h, src/probe.c, gpu/shim/gow3_overlay.cpp, gpu/shim/gow3_overlay.h, gpu/shim/gow3gpu.cpp, gpu/gow3gpu.h, tests/test_ue4_pc_settings.py, tests/test_pc_settings_hook.c
symbols: pause_menu, string_table, Locres, str_crc32, LABELS, CLICK_JUMPS, pak_names, medievil_pc_settings_find, medievil_pc_settings_call, install_pc_settings, pc_settings_sound, gow3gpu_menu_open, RequestOpen, open_requested
switches: GOW3_PC_SETTINGS_OPEN
tests: T-PC1
docs: study-pc-settings-entry.md
depends_on: settings-menu
related: button-prompts, camera-moves, mouse-input
checked:
---
## What it does

The pause menu (`UI/PauseMenu`) keeps a hidden, fully wired development button, `Button_Controls` ("Switch to
Classic Controls"). The override pak shows it as "PC Settings", fifth after Options. The edits, all same size in
`PauseMenu.uexp` and computed from the player's files: its Visibility tag is renamed (skipped on load), the label's
keys become new keys, the click handler's cheat-mode jump goes to its `pop` (the confirm sound only), and the slot
order and highlight arrays change. `STR_GENERAL` gains the two keys, and the 25 `Game.locres` files their
translations. The runtime wraps `PostEventAtLocationWithMotion`; the call from that handler opens the settings menu
(`gow3gpu_menu_open`).

## Invariants

- `KEYS`: each new key has its old key's length (the bytecode keeps its size). The old keys stay in `STR_GENERAL`:
  Options > Button Layout uses them.
- `pause_menu` raises unless every edit is found exactly once: a build whose files differ loses the row only
  (`ue4_override.apply` logs it and turns `pc_settings` off). A partial edit would show a row that switches the
  controls.
- The runtime opens the menu only when the jump in memory points at the `pop` (`MEDIEVIL_PC_SETTINGS_CLICK`). The
  game's handler (no pak edit, `pc_settings_entry=0`) would also switch the controls, so it is only logged.
- `Locres` keeps each string's ANSI / UTF-16 choice and the key order, so an unchanged file writes back identical
  bytes. New strings follow UE4's `IsPureAnsi` rule. The source hash is `FCrc::StrCrc32` (four byte steps per
  character), checked against the game's own hashes.
- `string_table` grows the last (only) export and updates its `SerialSize` and the summary's
  `BulkDataStartOffset`; nothing else in the header changes (tested).
- `RequestOpen` is taken in `Render`, which runs only while `Visible()`, so `Visible()` includes `open_requested`.
- Every label's characters occur in that culture's own strings (the menu font has them); a new culture without
  a label gets the English source.

## Interactions

- settings-menu: opens the same menu as the menu key. Its input rules give the game back a clean pad, because buttons
  held at open and at close wait for release (`menu_buttons_fresh`, `hold_after_capture`).
- mouse-input: the row takes a mouse click like the other buttons. The game mouse is let go under the menu.
- button-prompts: the same override pak. No file in common.

## Evidence

- T-PC1 (2026-11-12, 01.02, English): run; the row shows and opens the settings menu (the user, log, screenshots). German label and 01.00 not run. Found: the pause menu stays drawn under ours (prompt bars overlap).
- Offline: `tests/test_ue4_pc_settings.py` on the unpacked 01.00 (`Game Files/CUSA11227`) and 01.02
  (`E:/MediEvil/CUSA11227-0102`). Every locres and the string table round-trip byte-identical. Every edit applies
  exactly once and no other byte changes. The edited bytecode decodes as intended (scratch Kismet walker: arrays
  reordered, jump `0x143d` / `0x1518`, new keys).
- In game: none yet. T-PC1 is written in study-pc-settings-entry.md (section 4).

## History and pitfalls

- Found, designed and built 2026-11-12 (study-pc-settings-entry.md). The user chose translated labels and the place
  after Options. The Options-menu row (a structural package edit reusing the removed Screen Adjust plumbing) waits.
- Unconfirmed engine behaviour [I]: a tag named after a property UButton does not have is skipped on load. If not,
  add an `ESlateVisibility::Visible` name (a header edit) instead.
