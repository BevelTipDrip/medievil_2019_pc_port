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
commits: f4f13b8, e27103e, 870f584, 88bf535, a7c91f7, b4d9e6f, 5bb45d0, 7449317, 64da6dd, d76d3d0
files: src/runtime_mouse.c, src/mouse_hook.h, src/camera_hook.h, src/probe.c, src/runtime_services.c, gpu/shim/window.cpp, gpu/shim/sdl_window.h, scripts/ue4_override.py
symbols: runtime_mouse_enabled, runtime_mouse_start, runtime_mouse_capture, runtime_mouse_take, drain, reader_thread, mouse_init, mouse_read, SetMouseCapture, mouse_captured, runtime_mouse_take_camera, medievil_mouse_yaw_find, medievil_mouse_yaw_script_ok, mouse_yaw, install_mouse_yaw, camera_look, camera_look_class, camera_dancam, medievil_camera_pitch, medievil_camera_dancam
switches: GOW3_MOUSE, GOW3_MOUSE_BUTTONS, GOW3_MOUSE_INPUT_YAW
tests: T-MS1, T-MS2, T-MS3, T-MS4, T-MS5, T-MS6, T-MS7, T-MS8, T-MS9, T-MS10
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
wheel notches since the last read (buttons only with `GOW3_MOUSE_BUTTONS=1`). The window thread decides capture: borderless and fullscreen take the mouse with focus and no
menu, a window takes it on a click; focus loss / the Insert menu give it back (F1 dropped, T-MS6). Mouse look (7449317): on the camera tick, in a free area, the
ComputeCameraSimple wrapper (`camera_look`, probe.c; camera-moves card) adds x counts x `mouse_sensitivity` to this
frame's rotation's yaw and y counts x sensitivity x `mouse_vertical_ratio` (forward looks up; `mouse_invert_y`) to its
pitch, clamped like the stick's and kept in the area's Target / Smoothed pitch; both axes read at the same moment
(medievil.ini keys, live from the menu's Controls tab). `AddControllerYawInput`'s wrapper (src/mouse_hook.h) now
records Dan's handler's AllowUserCameraControl each frame (the lock); `GOW3_MOUSE_INPUT_YAW=1` keeps the first yaw
path there (T-MS5/6: the stick-turn local and InputRotation).

## Invariants

- `sceMouseRead` (game thread, once a frame) must not call into Windows: only atomics (MGS2 3.11: every win32k
  call slowed 20-30x with an 8000 Hz mouse).
- No SDL relative mode while this reader runs: Windows keeps one raw mouse registration per process, SDL's would
  replace ours. The capture log line reports who holds the registration.
- Counts and buttons reach the game only while captured; a release drops what was not read.
- Buttons held at a capture stay blocked until let go (the click that took the mouse is never half seen).
- No mouse buttons to the game by default: a click the game received left the main menu deaf to keyboard and pad
  (T-MS2). `GOW3_MOUSE_BUTTONS=1` is for the menu-focus research only.
- No release key: alt-tab, the Windows key and the Insert menu release it (the user, 2026-11-11, after T-MS6).
- Launcher (8f381ab, 7449317): Controls > Mouse sets `GOW3_MOUSE=1` and the medievil.ini mouse keys; ticking it sets
  the user's mouse camera (camera-moves). Mouse buttons and the wheel press game buttons
  through the pad (`GOW3_MOUSE_MAP`, card launcher/key-capture), not through sceMouse.
- A window only takes the mouse on a click, so its title bar stays usable.
- T-MS7 (2026-11-11, PC, `5bb45d0`, run): auto capture in borderless; the user: "Yup, it works". Log: captured at start,
  released and captured again on focus changes (data/mouse-auto-capture-t-ms7.log).
- Capture changes run on the window thread on events, never per frame.
- The yaw wrapper must call the original exec first (it consumes the FFrame argument), act only when Stack.Code
  is Dan's handler's end offset and the script size matches, and stay off after any failed bytecode or
  property-owner check. Mouse yaw goes to InputRotation (degrees / 2), not control yaw: the camera areas
  overwrite control yaw (study, step 5 research). No yaw while AllowUserCameraControl is false.
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
  overlay has the cursor.
- T-MS5 (2026-11-11, PC, `a7c91f7`, run): the user: the mouse turns the camera (x only) only while Dan walks.
  Log: the hook ran once a frame (705 calls / 5 s) and added the mouse at rest too. Confirmed in the bytecode:
  with Dan still (`IsPlayerControlActive` = movement input < 0.22), BP_CameraAreaFree keeps control yaw, which
  only AddControllerYawInput turns. Fix built `b4d9e6f`: the mouse also goes into the stick-turn local
  the AddControllerYawInput call reads (/ InputYawScale 2.5), put back after the call.
- T-MS6 (2026-11-11, PC, `b4d9e6f`, run): the user: "Everything appears to work" (standing and walking; menus
  work after a click). Wanted next: no click needed to take the mouse; no automatic camera moves while Dan
  moves, for mouse and stick (work queue item 4); the near-camera speed was not found in play.
- T-MS4 (2026-11-11, PC, `88bf535`, run): the user: "it all works as expected" (no snap on the Load list, a click
  in the game hides the cursor). Log: no capture / release pairs while the list was open.

- Mouse look on the camera tick, wheel buttons, sensitivity settings: fix built `7449317`, run in T-MS8: no mouse
  look on either axis (research/docs/data/mouse-look-t-ms8.log). Found: the pitch finder saw two three-statement
  runs (Add..FMax and FMin..Subtract) and gave up ("Mouse look: no pitch"), and a class without pitch dropped yaw
  too. Fix built `64da6dd`: the finder anchors on the whole Add, FMin, FMax, Subtract chain and `Target = L4` (a
  byte test of the Free layout fails on the old finder and passes on the new); yaw no longer needs the pitch.
- T-MS9 (`64da6dd`, research/docs/data/mouse-look-t-ms9.log): both axes in free areas, the user: "it works quite
  well". Found: in Dan-cam the camera "fights" (shakes, does not turn); the ~30 s of "vertical without the area's
  pitch" in the log. Dan-cam's ComputeCamera rebuilds its rotation after ComputeCameraSimple from PreviousYaw and
  stick x 40 + Pitch, so the mouse only moved the boom for a frame. Fix built `d76d3d0`: in Dan-cam the counts also go into
  PreviousYaw (a local of that frame) and Pitch, clamped to the first Pitch seen +-40 (the stick's reach); yaw
  waits while Dan-cam still follows Dan's facing (its first 1.5 x CameraBlendTime). Found from its bytecode
  (`medievil_camera_dancam`: SelectFloat, x 40, + Pitch, twice and equal; byte test of that layout passes).
- T-MS10 (`d76d3d0`, research/docs/data/mouse-look-t-ms10.log): "Mouse look: Dan-cam found"; Dan-cam turned by the
  mouse on up to ~1170 of ~1180 ticks per 5 s, 0 vertical without pitch, each new Dan-cam waited out its 0.3 s
  of Dan's facing. The user: "everything is working as expected": mouse look on both axes (free areas and
  Dan-cam), wheel, sensitivity / ratio / invert verified.

## History and pitfalls

- Found, low priority, left as is: the reader polls ~700 times a second with the mouse still (MGS2's verified
  "Every 1 ms" wait, line for line). Change only with an 8000 Hz A/B (study T-MS2).

- Planned first with SDL relative mode; changed before coding because SDL's reader wakes per report at
  TIME_CRITICAL, the pattern that made MGS2's frame starts wobble with an 8000 Hz mouse (study 4.2, revised).
