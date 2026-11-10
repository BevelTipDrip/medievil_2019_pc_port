---
id: debug-menu-0102
title: "Debug menu on 01.02: L1 + R1 + R3 (the game's own combo); no input edit; GetAllowDebugMenu trace probe"
area: ue4
status: active
platforms: all
default_windows: off
default_deck: off
origin: ours
class: M
commits: 4ba06ad, 90255b8, d8d5d5a, 9597d52
files: scripts/ue4_override.py, src/probe.c, launcher/medievil_launcher.py, README.md
symbols: install_debug_menu_trace, debug_menu_trace, medievil_debug_menu_sites
switches: GOW3_DEBUG_MENU_TRACE
tests:
docs: plan-work-queue.md, plan-0102.md
depends_on:
related: game-update-0102
---
## What it does

The developers' debug menu (launcher > Game > Cheats, ini `cheat_menu=1`: `AllowDebugMenu=true` and the `DebugHUD`
edits in the override pak) opens from Dan's Blueprint (`Characters/Dan/BP_df_character`), in a level only:
- 01.00: R3. The event goes straight to `DebugMenu()`, which calls the native `GetAllowDebugMenu`.
- 01.02: hold L1 + R1, then R3. The R3 event's first statements are
  `if (RightShoulderDown AND LeftShoulderDown) DebugMenu()`. Dan's own `Gamepad_RightShoulder` /
  `Gamepad_LeftShoulder` key events set and clear those flags, so the control scheme remapping does not matter.

The port changes nothing for 01.02's combo. The launcher tooltip, the log line and the README name it.

`GOW3_DEBUG_MENU_TRACE=1` (research probe, off by default) wraps `GetAllowDebugMenu`'s exec function through its
`FNameNativePtrPair` slot (0x4ed1818 in both builds; exec 0x6639c0 / 0x665ca0). Each call is logged with the calling
Blueprint function and object (FFrame Node 0x10, Object 0x18) and the result.

## Invariants

- No `DefaultInput.ini` edit. 01.02's R3 `RecenterCamera` action binding does not consume the key
  (`bConsumeInput` 0), so the key event still runs.
- The main menu's `MainMenuPawn` R3 event has no debug menu (identical in both builds). Test in a level.
- The trace checks the exec function's first 13 bytes and that the slot holds the relocated exec address before
  replacing it. Anything else: "unavailable" and nothing is patched.

## Interactions

- game-update-0102: the other 01.02 differences.
- 01.02: R3 also recenters the camera when it opens the menu (harmless).

## Evidence

- `debug-menu-trace-0102.log`: R3 alone, three presses in a level, gave no `GetAllowDebugMenu` call.
- `debug-menu-trace-0100.log`: R3 in a level opened the menu, with 4 calls returning 1 from Dan's Blueprint.
- Disassembly of `ExecuteUbergraph_BP_df_character` (scratch Kismet disassembler): the R3 entry is 0x17eb on 01.00
  (`DebugMenu()`) and 0xacd on 01.02 (the shoulder check, then `DebugMenu()`).
- In-game check of L1 + R1 + R3 on 01.02: pending.

## History and pitfalls

- 4ba06ad dropped 01.02's R3 RecenterCamera line, assuming the action binding took the button. The run showed R3
  alone still did nothing. Removed.
- 90255b8 renamed the R3 key to the touchpad (`Gamepad_Special_Left`) in Dan's and the main menu's name maps. It
  failed on both builds and was reverted (d8d5d5a). The 01.00 test was in the main menu, which has no debug menu.
- Lesson: trace the native call first. Two blind input edits were spent on a combo that is in the Blueprint
  bytecode.
