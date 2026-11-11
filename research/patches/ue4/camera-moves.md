---
id: camera-moves
title: The free camera's automatic moves, each switchable live (menu Controls tab, medievil.ini camera_*)
area: ue4
status: active
platforms: all
default_windows: off
default_deck: off
origin: ours
class: M
commits: e1a220b, 36deef0, 10beddc
files: src/camera_hook.h, src/probe.c, gpu/shim/gow3_settings.h, gpu/shim/gow3_settings.cpp, gpu/shim/gow3_overlay.cpp
symbols: medievil_camera_edits, medievil_camera_input_rotation, medievil_camera_find, camera_search, camera_update, camera_simple, install_camera_moves, gow3_camera_off_mask, gow3_camera_status, ControlsTab, CameraMoveKeys
switches:
tests: T-CM1
docs: study-camera-moves.md
depends_on:
related: mouse-input
checked:
---
## What it does

The menu's Controls tab ("Advanced camera") turns each automatic move of the free camera off or on, live, saved as
`medievil.ini` `camera_follow`, `camera_swing`, `camera_suggest`, `camera_keepout`, `camera_run_zoom`,
`camera_pitch_return` (1 = the game's move, the default):

- follow: the camera chasing Dan's direction while he moves, with the extra InputRotation x 2 turn that came with it;
- swing: Dan's sideways speed added to the shown yaw;
- suggest: the turn toward a suggest-direction area's arrow on entry;
- keepout: yaw pushed around keep-out posts;
- run_zoom: boom and pitch changes at run start / end;
- pitch_return: vertical look easing back to the middle while Dan moves or attacks.

`src/camera_hook.h` / `probe.c` wrap `UMedievilSpringArm::ComputeCameraSimple`'s exec (slot 0x4ecc7f8 in 01.00 and
01.02), called once a tick by the free areas' ComputeCamera. On that call (game thread) the FFrame's Node (ComputeCamera,
its Outer = the area class) and Object (the area) lead to: the area class's own functions, its parents
(ProcessKeepOuts), the area's subclass (FreeSuggestDirection), and the camera manager class (owner of the
InputRotation property the x 2.0 statement names). Each is searched once for every edit's original bytes; every edit
found is written or put back whenever `gow3_camera_off_mask()` (the menu's choice) changes. Only free areas and the
manager's velocity swing (weight 0 outside free areas) are touched: fixed, spline, boss and aim cameras keep the game's
behaviour. The research and offsets: `study-camera-moves.md`.

## Invariants

- Same-size edits in the loaded bytecode, each found exactly once in its class; the original bytes are kept per site
  and put back when the move is switched on again.
- The follow edits apply only where both are found ("together"): SplineConstraint (a rail camera that also calls
  ComputeCameraSimple) has the InputRotation x 2.0 statement but no chase test.
- The x 2.0 edit must be the InputRotation statement (EX_Context / EX_LocalVariable / skip 9 / the same property twice).
- No override-pak camera edits: they would hide the original bytes from the hook (the pak route was removed for that).

## Interactions

- mouse-input: with follow off the mouse hook's InputRotation half adds nothing in free areas; the control-yaw half
  turns the camera at the set degrees per count, standing and moving.
- Movement follows the shown camera rotation, so swing off changes steering slightly [inferred].

## Evidence

- T-CM1 (2026-11-11, PC, 01.02, override-pak version `36deef0`, all six off): the user: "everything seems to work
  great" (log `data/camera-moves-t-cm1.log`).
- Live version: fix built `10beddc`, not run.

## History and pitfalls

- First built as override-pak edits (`GOW3_CAMERA_OFF`, `scripts/ue4_camera.py`), fixed at launch; replaced by the live
  hook the same day (the user wanted to switch in game).
- The research's FreeSimple uexp offsets were wrong (they pointed into other code); found again by pattern.
