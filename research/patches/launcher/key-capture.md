---
id: key-capture
title: Launcher key capture: a Controls key box takes the next key pressed (Tk keysym to SDL scancode name)
area: launcher
status: active
platforms: all
default_windows: on
default_deck: on
origin: ours
class: G
commits:
files: launcher/medievil_launcher.py, tests/test_launcher_controls.py
symbols: sdl_key_name, TK_KEYS, key_capture
switches:
tests: T-G4c
docs: study-glyphs.md#t-g4c-square-tab-boxes-key-capture-2026-11-11-launched-by-the-user-from-the-launcher-with-claudes-build-log-dataglyphs-g4c-square-tabs-0102log
depends_on:
related: button-prompts
checked:
---
## What it does

The keyboard column of Launcher > Controls is a set of buttons: click one ("Press a key..."), press a key, and its
SDL name is stored for that PS4 button (passed to the game as `GOW3_KEY_MAP`). Clicking elsewhere cancels. Asked
for by the user (2026-11-11): typing "Left Shift" was the old way.

## Invariants

- The stored names must be ones the runtime's `SDL_GetScancodeFromName` knows (`runtime_pad.c`): `sdl_key_name`
  turns Tk keysyms into SDL scancode names (letters upper case, digits, F1-F11, `Keypad N`, `TK_KEYS`).
- Refused: Insert (the port's menu), F12 (screenshots), `,` and `=` (`control_map`'s separators), unknown keys.
- The capture's key binding returns "break", so Tab, Space and Return go to the capture, not to Tk's focus
  traversal or the button's own Space binding.
- Every default key of `CONTROLS` is one the capture can produce (a test checks).

## Interactions

- button-prompts: the keyboard prompts show the key's cap (`ue4_glyphs.key_icon`; keypad digits use the digit's).

## Evidence

- T-G4c (PC): the user: "The Key capture works, and it properly differentiates between left and right alt."

## History and pitfalls

- Before: free-text entries with SDL names; a misspelt name kept the default (the start log said which).
