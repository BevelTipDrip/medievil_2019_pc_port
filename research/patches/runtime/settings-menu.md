---
id: settings-menu
title: Settings menu in the game's Options menu look, usable with a pad alone (focus rows, L1 / R1 tabs, GOW3_MENU_PAD)
area: runtime
status: active
platforms: all
default_windows: on
default_deck: on
origin: ours
class: G
commits: 64fb18a, 7f6a98e, b914242
files: gpu/shim/gow3_overlay.cpp, gpu/shim/gow3_overlay.h, gpu/shim/gow3gpu.cpp, gpu/gow3gpu.h, src/runtime_pad.c, scripts/ue4_glyphs.py, tests/test_pad_map.c
symbols: MenuNav, NavRow, NavStartTab, NavEndFrame, MenuButtons, MenuPress, MenuPadCombo, MenuPad, StyledMenu, PlainMenu, BeginStyledRow, StyledRowHit, StyledSlider, DrawChoice, ToggleRow, ChoiceIndex, IntSliderRow, ButtonsRow, RestartPressed, SetMenuInput, gow3gpu_menu_set_input, menu_buttons, menu_key_buttons, menu_mouse_buttons, window_mouse_held, StyleImage, OVERLAY
switches: GOW3_MENU_PAD, GOW3_MENU_KEY
tests: T-SM1, T-SM2, T-SM3, T-SM4, T-SM5, T-SM6, T-SM7, T-SM8
docs: plan-settings-menu.md#2-design, plan-settings-menu.md#22-controls, plan-settings-menu.md#4-test-plan-for-the-user-each-launch-needs-the-users-go, plan-settings-menu.md#5-decisions-for-the-user
depends_on: load-list-details, button-prompts
related: key-capture, cursor-hide, live-render-scale
checked:
---
## What it does

The port's settings menu (Insert / `GOW3_MENU_KEY`, or R3+L2 / `GOW3_MENU_PAD`) drawn like the Load list when the
game UI cache is there: the Options frame, "-SETTINGS-", tabs between the L1 / R1 prompts, rows in the game's
font and colours, the focused row's help, SELECT / GO BACK / CHANGE TAB prompts (clickable); else the plain ImGui
window. In both looks every row is numbered (`MenuNav`): the focused one is highlighted, takes left / right and
Cross, shows its help and scrolls into view. Input is the runtime's mapped PS4 buttons polled every presented
frame (`menu_buttons`), as the Load list's.

## Invariants

- One input path for pad and keys: `menu_buttons` ORs pad and keys as mapped, the fixed menu keys (arrows, Enter,
  Esc, Backspace, Q, E; only while not bound to a button the menu uses) and the stick into one mask, so a key both
  bound and fixed is one press. ImGui's keyboard / gamepad navigation must stay off: with it, Space / Enter / A
  would act twice. A new menu key belongs in `menu_keys`, not in `HandleEvent` (pad-map-test checks the rules).
- Mouse binds count only for Circle / L1 / R1 and never from the left button (`menu_mouse_buttons`): a left click
  bound to Cross or Circle would act twice or close the menu at every click.
- Rows must call `NavRow` (through `RowEnd` / `StyledRowEnd`) exactly once each, in drawing order, and `TabBody`
  only from the two menus; `NavEndFrame` runs every drawn frame. A row drawn only some frames shifts the focus.
- Styled rows: controls with their own mouse handling (track, number box) are submitted before `StyledRowHit`
  (ImGui gives the hover to the first item; the row's hit box comes last).
- The style (all six images: frame, Cross, Circle, Square, L1, R1, and both fonts, or the plain look) is loaded
  once, at overlay Init on a thread, from `runtime_file_user_dir()/cache/game_ui` (the save list's folder);
  `StyledChoiceBox` uses only the first four images.
- Nothing reaches the game while the menu is open (`CapturesInput`, unchanged); the buttons that opened it wait
  for release (`menu_buttons_fresh`); nothing is read while a number box is typed in (`WantTextInput`).
- `OVERLAY` (ue4_glyphs.py) names the files `gow3gpu_choice_style` loads from `prompts/style/`.

## Interactions

- load-list-details: same frame, fonts, colours, prompts and input style; the list's look is unchanged.
- button-prompts: the menu's prompts follow the prompt style and binds as the list's (L1 / R1 added).
- key-capture: the menu key is still `GOW3_MENU_KEY`; the pad combination is `GOW3_MENU_PAD` (no launcher box).
- cursor-hide: no new modal; the menu is in `CapturesInput` as before.
- live-render-scale: the slider's pad steps keep the 0.3 s apply-after-last-step rule (`render_scale_edit`).

## Evidence

- Built, not run (2026-11-12): build clean (no new warnings), Python tests 189 OK (31 skipped), pad-map-test PASS
  with the menu key and mouse rules (built by hand, see below). No game launch: T-SM1-T-SM8 are the user's.
- Offline: every character of the menu's texts is in the game's text font (Pillow); the tab strip and prompt bar
  fit the frame at the chosen sizes.

## History and pitfalls

- Before: ImGui's gamepad navigation (D-pad, A; combos opened popups, sliders needed A first, no left stick), the
  pad seen through raw SDL events (binds ignored), the style loaded only when a save list first opened.
- Found: the `pad-map-test` CMake target does not link on Windows (compat_win.c needs winmm: `timeBeginPeriod`);
  built by hand with `-lwinmm`.
