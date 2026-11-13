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
commits: 64fb18a, 7f6a98e, b914242, 33491a9, 0666928, 3c42639, 6ab5c19
files: gpu/shim/gow3_overlay.cpp, gpu/shim/gow3_overlay.h, gpu/shim/gow3gpu.cpp, gpu/gow3gpu.h, src/runtime_pad.c, scripts/ue4_glyphs.py, tests/test_pad_map.c, gpu/shim/gow3_settings.h, gpu/shim/gow3_settings.cpp
symbols: MenuNav, NavRow, NavStartTab, NavEndFrame, MenuButtons, MenuPress, MenuPadCombo, MenuPad, StyledMenu, PlainMenu, BeginStyledRow, StyledRowHit, StyledSlider, DrawChoice, ToggleRow, ChoiceIndex, IntSliderRow, ButtonsRow, SetMenuInput, DisplayTab, GraphicsTab, PerformanceTab, RestartTag, RecordItem, RowVisible, RestartNow, RestartHoldStep, RestartProgress, RestartCombo, OutputNeedsRestart, Note, gow3gpu_menu_set_input, menu_buttons, menu_key_buttons, menu_mouse_buttons, window_mouse_held, StyleImage, OVERLAY
switches: GOW3_MENU_PAD, GOW3_MENU_KEY, GOW3_PRESENT_MODE, GOW3_RESTART_COMMAND
tests: T-SM1, T-SM2, T-SM3, T-SM4, T-SM5, T-SM6, T-SM7, T-SM8, T-SM9, T-SM10, T-SM11, T-SM12, T-SM13, T-SM14, T-SM15, T-SM16
docs: plan-settings-menu.md#10-the-users-decisions-on-the-report-2026-11-13-fix-built-not-run, plan-settings-menu.md#9-the-reorganisation-2026-11-13-fix-built-not-run, plan-settings-menu.md#2-design, plan-settings-menu.md#22-controls, plan-settings-menu.md#4-test-plan-for-the-user-each-launch-needs-the-users-go, plan-settings-menu.md#5-decisions-for-the-user
depends_on: load-list-details, button-prompts
related: key-capture, cursor-hide, live-render-scale, deck-preset, particle-flip-guard, perf-patches-ini, upstream-ue4-setres, I1, vsync-control, button-prompts
checked: 2026-11-13
---
## What it does

The port's settings menu (Insert / `GOW3_MENU_KEY`, or R3+L2 / `GOW3_MENU_PAD`) drawn like the Load list when the
game UI cache is there: the Options frame, "-SETTINGS-", tabs between the L1 / R1 prompts, rows in the game's
font and colours, the focused row's help, SELECT / GO BACK / CHANGE TAB prompts (clickable); else the plain ImGui
window. In both looks every row is numbered (`MenuNav`): the focused one is highlighted, takes left / right and
Cross, shows its help and scrolls into view. Input is the runtime's mapped PS4 buttons polled every presented
frame (`menu_buttons`), as the Load list's.

Since 2026-11-13 (`0666928`, plan section 9): tabs CONTROLS, DISPLAY, GRAPHICS, PERFORMANCE, OVERLAY (God of War
III's cheats keep their code, no tab); the frame's middle stretched to 92% of the height, smaller rows, the prompt
bar inside the frame, a 3-line help under the list, whole rows only, an 80% dim behind; RESTART tags on restart-only
rows; "[Square]+[Touchpad] HOLD: APPLY & RESTART" in the prompt bar while changes wait (both held 5 s, or the mouse
held on it); the scroll bar takes the mouse; Display > Frame buffer size is restart-only (saved, applied at the next
start). Input is the launcher's binds only (plan section 10).

## Invariants

- One input path for pad and keys: `menu_buttons` ORs pad and keys as mapped, the fixed menu keys (arrows, Enter,
  Esc, Backspace, Q, E; only while not bound to a button the menu uses) and the stick into one mask, so a key both
  bound and fixed is one press. ImGui's keyboard / gamepad navigation must stay off: with it, Space / Enter / A
  would act twice. A new menu key belongs in `menu_keys`, not in `HandleEvent` (pad-map-test checks the rules).
- Keys and mouse buttons act only as bound (`menu_key_buttons`, `menu_mouse_buttons`), never from the left mouse
  button (the pointer's: a left click bound to Cross or Circle would act twice). The one fixed key is Esc (Circle,
  as in the Load list) while not bound to a menu button. A new fixed key would bypass the binds: don't.
- Tab is the touchpad's default bind in `key_map` (not a fixed line in `sample_host` since 2026-11-13), so a
  launcher rebind or "none" moves or removes it in the game and the menu alike.
- Rows must call `NavRow` (through `RowEnd` / `StyledRowEnd`) exactly once each, in drawing order, and `TabBody`
  only from the two menus; `NavEndFrame` runs every drawn frame. A row drawn only some frames shifts the focus.
- Whole rows: every item of the styled list records its screen band (`RecordItem`: `NavRow`, `Section`, `InfoRow`,
  `Note`, the preview); after `TabBody` the list's draw commands are clipped to the first and last whole item. A new
  kind of item drawn without `RecordItem` is never a cut point (it can vanish with its neighbour). Anything drawn into
  the list's draw list after the cut is clipped too: the scroll bar is drawn in the frame's list for that reason.
  Rows outside last frame's band take no clicks (`RowVisible`).
- The scroll bar's mouse area is submitted before the rows (first item wins the hover) and uses last frame's scroll
  range (`GetScrollMaxY` at the child's start).
- `Restart()` marks the next row only (`BeginStyledRow` / `Row` take it); a `Restart()` before a row that is then not
  drawn would tag the following one.
- APPLY & RESTART only by holding: Square + Touchpad (`Ps4::RestartCombo`) for 5 s (`RestartHoldStep`, every
  presented frame), or the left mouse button on its prompt; any release cancels; a combination held when the menu
  opened waits for release. Neither button does anything alone.
- The frame buffer size is restart-only (`Gow3Settings::OutputNeedsRestart`): the menu never calls r.SetRes.
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

- 2026-11-13 reorganisation (`0bff4d6`, `33491a9`, `0666928`): fix built, not run. Build clean, Python tests 201 run
  OK (31 skipped), graphics-settings-test and pad-map-test PASS. T-SM4-T-SM15 (plan 9.6) are the user's.

- Built, not run (2026-11-12): build clean (no new warnings), Python tests 189 OK (31 skipped), pad-map-test PASS
  with the menu key and mouse rules (built by hand, see below). No game launch: T-SM1-T-SM8 are the user's.
- Offline: every character of the menu's texts is in the game's text font (Pillow); the tab strip and prompt bar
  fit the frame at the chosen sizes.

## History and pitfalls

- Before: ImGui's gamepad navigation (D-pad, A; combos opened popups, sliders needed A first, no left stick), the
  pad seen through raw SDL events (binds ignored), the style loaded only when a save list first opened.
- Found: the `pad-map-test` CMake target does not link on Windows (compat_win.c needs winmm: `timeBeginPeriod`);
  built by hand with `-lwinmm`.
