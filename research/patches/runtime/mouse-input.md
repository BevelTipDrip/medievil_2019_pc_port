---
id: mouse-input
title: Mouse input through the game's own sceMouse path, from a batched Raw Input reader (GOW3_MOUSE=1, Windows)
area: runtime
status: active
platforms: windows
default_windows: off
default_deck: n/a
origin: ours
class: M
commits: f4f13b8, e27103e, 870f584, 88bf535
files: src/runtime_mouse.c, src/runtime_services.c, gpu/shim/window.cpp, gpu/shim/sdl_window.h, scripts/ue4_override.py
symbols: runtime_mouse_enabled, runtime_mouse_start, runtime_mouse_capture, runtime_mouse_take, drain, reader_thread, mouse_init, mouse_read, SetMouseCapture, mouse_captured
switches: GOW3_MOUSE, GOW3_MOUSE_BUTTONS
tests: T-MS1, T-MS2, T-MS3
docs: study-mouse-input.md#t-ms1-ps4enablemouse-from-the-paks-ini-2026-11-11-launched-by-claude-with-the-users-go-log-datamouse-step1-enablelog
depends_on:
related: cursor-hide
checked:
---
## What it does

`GOW3_MOUSE=1` writes `PS4.EnableMouse=1` into the override pak, so UE4 opens and reads `sceMouse` once a frame
(T-MS1). `src/runtime_mouse.c` reads the mouse on its own thread with MGS2's method: a message-only window
(`RIDEV_INPUTSINK`), normal priority, at most one wake-up a millisecond, every queued report in one
`GetRawInputBuffer` batch, totals in atomics. `mouse_read` reports a connected mouse with the counts, buttons and
wheel notches since the last read (buttons only with `GOW3_MOUSE_BUTTONS=1`). The window thread decides capture: a click takes the mouse, plain F1 / focus loss
/ the Insert menu give it back. Step 2 of the study; no camera or menu use yet (steps 3-7).

## Invariants

- `sceMouseRead` (game thread, once a frame) must not call into Windows: only atomics (MGS2 3.11: every win32k
  call slowed 20-30x with an 8000 Hz mouse).
- No SDL relative mode while this reader runs: Windows keeps one raw mouse registration per process, SDL's would
  replace ours. The capture log line reports who holds the registration.
- Counts and buttons reach the game only while captured; a release drops what was not read.
- Buttons held at a capture stay blocked until let go (the click that took the mouse is never half seen).
- No mouse buttons to the game by default: a click the game received left the main menu deaf to keyboard and pad
  (T-MS2). `GOW3_MOUSE_BUTTONS=1` is for the menu-focus research only.
- F1 releases only without Alt / Shift / Ctrl / Win (Alt+F1 is NVIDIA's overlay; the user, 2026-11-11).
- Capture changes run on the window thread on events, never per frame.
- A click takes the mouse only while `Gow3Overlay::CapturesInput()` is false; otherwise the capture is undone at
  once and the cursor is left at the window's centre (T-MS3).

## Interactions

- cursor-hide: `UpdateCursor` also hides the cursor while captured and releases the capture without focus or under
  the overlay (`CapturesInput`).
- Linux / Deck: off (`runtime_mouse_enabled` is 0); the stubs keep answering "no mouse".

## Evidence

- T-MS1 (2026-11-11, PC, 01.02, run): init once; open right after the pad open with `user 1 type 0 index 0
  param 1`; reads with count 8, 134-141 a second at a 141 cap (= once a frame).
- T-MS2 (2026-11-11, PC, `e27103e`, run): registration kept; ~930 reports/s moving at 1000 Hz, ~7100 at 8000 Hz,
  700 reader wake-ups a second either way; the user: no frame-time spikes at either rate. Found: a click the game
  received stopped the main menu taking keyboard and pad input. Buttons withheld since (`870f584`).
- T-MS3 (2026-11-11, PC, `870f584`, run): the user: "all the inputs work" (main menu, Load list, level). Found: a
  click on the Load list snapped the cursor to the centre (capture then release). Fix built `88bf535`: no capture while the
  overlay has the cursor; not run.

## History and pitfalls

- Found, low priority, left as is: the reader polls ~700 times a second with the mouse still (MGS2's verified
  "Every 1 ms" wait, line for line). Change only with an 8000 Hz A/B (study T-MS2).

- Planned first with SDL relative mode; changed before coding because SDL's reader wakes per report at
  TIME_CRITICAL, the pattern that made MGS2's frame starts wobble with an 8000 Hz mouse (study 4.2, revised).
