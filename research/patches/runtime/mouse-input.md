---
id: mouse-input
title: Mouse input through the game's own sceMouse path (step 1: the sceMouse stubs log the game's calls)
area: runtime
status: diagnostic
platforms: all
default_windows: on
default_deck: on
origin: ours
class: M
commits: f4f13b8
files: src/runtime_services.c
symbols: mouse_init, mouse_open, mouse_read
switches:
tests: T-MS1
docs: study-mouse-input.md#t-ms1-ps4enablemouse-from-the-paks-ini-2026-11-11-launched-by-claude-with-the-users-go-log-datamouse-step1-enablelog
depends_on:
related: cursor-hide
checked:
---
## What it does

Step 1 of study-mouse-input.md's plan. The runtime's `sceMouseInit` / `sceMouseOpen` / `sceMouseRead` stubs print
a `Mouse:` line with the guest caller: init and open every call, reads the first three and every 1000th with the
read rate. The stubs still answer "no mouse" (read zeroes 40 bytes and returns 1 entry, connected = 0), so the game
behaves as before. The game only calls them with `PS4.EnableMouse=1` (today only through `GOW3_UE4_CVARS`).

## Invariants

- `mouse_read` must keep filling `count` >= 1 entries of 40 bytes with `connected` (+0x08) = 0 until step 2 reports a
  real mouse: a non-zero byte there makes the game treat the entry as a mouse (study 2.2).
- The read counter is atomic; the log lines must stay rare (the read runs once per game-thread frame).

## Interactions

- cursor-hide: the launcher-side cursor rules; step 2's capture rules (study 4.4) must agree with it.
- `GOW3_UE4_CVARS` (ue4_override.py) is what turns the game's mouse code on; the pak's `[SystemSettings]` is early
  enough (T-MS1).

## Evidence

- T-MS1 (2026-11-11, PC, 01.02, run): init once; open right after the pad open for user 1 with `user 1 type 0
  index 0 param 1`; reads with count 8, 134-141 a second at a 141 cap (= once a frame). The user: nothing changed
  in game.

## History and pitfalls

- None yet. Next: step 2 (connected mouse from SDL relative motion), then the menu cursor and the yaw hook.
