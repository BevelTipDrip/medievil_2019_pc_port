---
id: key-capture
title: Launcher key capture: a Controls key box takes the next key pressed (Tk keysym to SDL scancode name); menu key and mouse buttons
area: launcher
status: active
platforms: all
default_windows: on
default_deck: on
origin: ours
class: G
commits: 29d3782e05, 8f381ab, 7449317
files: launcher/medievil_launcher.py, tests/test_launcher_controls.py, gpu/shim/gow3_overlay.cpp, src/runtime_pad.c, src/runtime_mouse.c
symbols: sdl_key_name, TK_KEYS, key_capture, key_taken, menu_key_name, menu_key, MOUSE_BUTTONS, MenuKey, mouse_map_buttons, runtime_mouse_buttons
switches: GOW3_MENU_KEY, GOW3_MOUSE_MAP
tests: T-G4c, T-MS10
docs: study-glyphs.md#t-g4c-square-tab-boxes-key-capture-2026-11-11-launched-by-the-user-from-the-launcher-with-claudes-build-log-dataglyphs-g4c-square-tabs-0102log
depends_on:
related: button-prompts
checked:
---
## What it does

The keyboard column of Launcher > Controls is a set of buttons: click one ("Press a key..."), press a key, and its
SDL name is stored for that PS4 button (passed to the game as `GOW3_KEY_MAP`). Clicking elsewhere cancels. Asked
for by the user (2026-11-11): typing "Left Shift" was the old way.

Added 2026-11-11 (the user: "my main keyboard has no insert key"; mouse buttons bindable next to keys):
- **Menu key** box: the port menu's keyboard key (`GOW3_MENU_KEY`, default Insert; the overlay's `MenuKey` reads it
  with `SDL_GetKeyFromName`, unknown names fall back to Insert). The menu title, its "ready" log line and the
  launcher's sidebar note name the chosen key.
- **Mouse** section: "Mouse look and buttons" (`GOW3_MOUSE=1`, mouse-input card) and "Mouse look speed"
  (`GOW3_MOUSE_YAW`, 0.011-0.044 degrees per count).
- **MOUSE column**: left / right / middle / side (x1, x2) button per game button (`GOW3_MOUSE_MAP`, e.g.
  `square=left,cross=x2`). `runtime_pad.c` ORs the PS4 buttons of the held mouse buttons
  (`runtime_mouse_buttons`: only while the game has the mouse, without the click that took it) into the pad, next
  to the keys; no defaults.
- Wheel (7449317): `wheel_up`, `wheel_down`, `wheel` (either). One 60 ms press per scroll gesture; a gesture ends after
  200 ms without a notch, so a fast scroll is one press (the user: no queue, no back and forth). Verified in
  T-MS10 (the user: everything works as expected).

## Invariants

- The stored names must be ones the runtime's `SDL_GetScancodeFromName` knows (`runtime_pad.c`): `sdl_key_name`
  turns Tk keysyms into SDL scancode names (letters upper case, digits, F1-F11, `Keypad N`, `TK_KEYS`).
- Refused for a game button: the menu key (Insert by default), F12 (screenshots), `,` and `=` (`control_map`'s
  separators), unknown keys. The menu key may be Insert or any key a game button could take, but not one a game
  button has (`key_taken`: "Used by ...").
- Mouse buttons reach the game only as pad buttons, never as sceMouse buttons (a click the game received broke
  the main menu's focus: mouse-input T-MS2).
- The capture's key binding returns "break", so Tab, Space and Return go to the capture, not to Tk's focus
  traversal or the button's own Space binding.
- Every default key of `CONTROLS` is one the capture can produce (a test checks).

## Interactions

- button-prompts: the keyboard prompts show the key's cap (`ue4_glyphs.key_icon`; keypad digits use the digit's).

## Evidence

- T-G4c (PC): the user: "The Key capture works, and it properly differentiates between left and right alt."
- Menu key, Mouse section and MOUSE column (2026-11-11, `8f381ab`, run, log `data/camera-moves-t-cm2.log`): the menu
  opened on Home ("Overlay: menu ready (Home or R3+L2)"); "pad map: ... 3 mouse buttons". The user:
  "everything feels great".

## History and pitfalls

- Before: free-text entries with SDL names; a misspelt name kept the default (the start log said which).
