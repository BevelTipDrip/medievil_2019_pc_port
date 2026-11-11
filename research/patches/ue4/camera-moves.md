---
id: camera-moves
title: The free camera's automatic moves, each switchable off (GOW3_CAMERA_OFF, override pak)
area: ue4
status: active
platforms: all
default_windows: off
default_deck: off
origin: ours
class: M
commits: e1a220b, 36deef0
files: scripts/ue4_camera.py, scripts/ue4_override.py, tests/test_ue4_camera.py
symbols: SWITCH, MOVES, FOLLOW, RUN_ZOOM, PITCH_RETURN, parse, files
switches: GOW3_CAMERA_OFF
tests:
docs: study-camera-moves.md
depends_on:
related: mouse-input
checked:
---
## What it does

`GOW3_CAMERA_OFF` lists camera moves to turn off, comma-separated: `follow` (the camera chasing Dan's direction while
he moves, and the extra InputRotation turn that came with it), `swing` (Dan's sideways speed added to the shown yaw),
`suggest` (the turn toward a suggest-direction area's arrow on entry), `keepout` (yaw pushed around keep-out posts),
`run_zoom` (boom and pitch changes at run start / end), `pitch_return` (vertical look easing back to the middle while
Dan moves or attacks). Each move is one or two same-size edits of a camera Blueprint `.uexp` (bytecode jump or float
constant) in the override pak (`scripts/ue4_camera.py`, called from `ue4_override.apply`). Only the free areas
(Free, FreeSuggestDirection, FreeSimple) and the camera manager's velocity swing (weight 0 outside free areas) are
touched: fixed, spline, boss and aim cameras keep the game's behaviour. The research and offsets:
`study-camera-moves.md`.

## Invariants

- Same-size edits only, each pattern found exactly once (`apply_edits`); a list entry is one variant per build.
- A move is all or nothing; a move whose bytes are missing is skipped with "Camera: ... not available" and the
  others still apply.
- FreeSimple is read under its pak name `Blueprints/Camera/...` (pakchunk6, mounted at OVERBITE/Content/) and
  written under the full path.
- `follow` must keep both edits together: 400 -> 1e30 alone made moving turn 4.5x the stick (control yaw 2.5x plus
  InputRotation x 2) against 2.5x standing.

## Interactions

- mouse-input: with `follow` off the mouse hook's InputRotation half adds nothing in free areas; the control-yaw half
  turns the camera at the set degrees per count, standing and moving.
- Movement follows the shown camera rotation, so `swing` off changes steering slightly [inferred].

## Evidence

- Every move applied to both builds' files (01.00, 01.02); changed offsets match the research's 01.02 offsets.
- Fix built `e1a220b`, not run.

## History and pitfalls

- The research's FreeSimple uexp offsets were wrong (they pointed into other code); found again by pattern.
