---
id: debug-menu-0102
title: Debug menu on 01.02: the override pak drops 01.02's R3 RecenterCamera mapping while the debug menu is on
area: ue4
status: active
platforms: all
default_windows: off
default_deck: off
origin: ours
class: M
commits: 4ba06ad
files: scripts/ue4_override.py
symbols: debug_menu_input, INPUT_CONFIG
switches:
tests:
docs: plan-work-queue.md, plan-0102.md
depends_on:
related: game-update-0102
---
## What it does

The developers' debug menu (launcher > Game > Cheats, ini `cheat_menu=1`: `AllowDebugMenu=true` and the `DebugHUD`
edits in the override pak) opens with R3 on 01.00 but not on 01.02. 01.02's `DefaultInput.ini` adds one line,
`+ActionMappings=(ActionName="RecenterCamera",...,Key=Gamepad_RightThumbstick)`. No Blueprint of either build names
R3, so the debug menu's R3 check is native (`DebugHUD` has `OpenDebug`). 01.02's `BP_InputDelegateActor` and the
camera areas listen for RecenterCamera, whose action binding takes the button.

With the debug menu on, `debug_menu_input` writes the build's own `DefaultInput.ini` without that line into the
override pak. Nothing is written for 01.00 (no such line).

## Invariants

- Only the R3 RecenterCamera line is removed; the rest of the file is the build's own, CRLF kept (dry run: 13,647 ->
  13,525 bytes, 135 -> 134 lines).
- The file is written only when the debug menu is on: without it, R3 recenters the camera on 01.02 as shipped.

## Interactions

- game-update-0102: the other 01.02 differences; the pak's config edits already applied there (log "debug menu (R3)").
- While the debug menu is on, R3 no longer recenters the camera on 01.02 (the player's trade).

## Evidence

- Both builds' paks compared (2026-11-09): `DefaultInput.ini` differs by that line only; `PS4Game.ini` by the build
  string and `AllowDebugMenu=False` (capital F); Blueprints naming RecenterCamera / the debug menu listed. Python tests
  OK. Not run.

## History and pitfalls

- First version.
