---
id: cursor-hide
title: Hide the mouse cursor in borderless and fullscreen and when idle in a window; shown while unfocused
area: launcher
status: active
platforms: all
default_windows: on
default_deck: on
origin: bloodborne 4d4d208
class: G
commits: 4bb55651ac, 638b6a0786
files: gpu/shim/window.cpp, gpu/shim/sdl_window.h, gpu/shim/gow3_overlay.cpp
symbols: UpdateCursor, last_mouse_motion_ms, cursor_hidden, CapturesInput, PollEvents, SetOpen
switches:
tests:
docs: plan-work-queue.md#the-users-quality-of-life-list-2026-11-03-after-the-v200-release
depends_on:
related: port-identity
verified: 2026-11-07
---
## What it does

`WindowSDL::UpdateCursor` runs at the end of every `PollEvents`. It hides the OS cursor (`SDL_HideCursor`) when the
window has input focus, the overlay does not capture input (`Gow3Overlay::CapturesInput`: menu, save list or text
entry closed), and either the window is fullscreen (borderless or exclusive; both set `SDL_WINDOW_FULLSCREEN`) or the
mouse has not moved for 3 s (`last_mouse_motion_ms`, set on `SDL_EVENT_MOUSE_MOTION`). Otherwise it shows the cursor.
`cursor_hidden` makes it call SDL only on a change. `4bb5565` also stopped ImGui from drawing its own cursor over the
OS one while the menu is open (`MouseDrawCursor` removed from `SetOpen`). `638b6a0` leaves the cursor shown while the
window has no input focus (alt-tab, another window clicked).

## Invariants

- Any overlay state that takes mouse input must be in `CapturesInput`, or the cursor hides under it in fullscreen.
  A new modal (a picker, a dialog) has to be added there.
- Without input focus the cursor must stay shown, so another window is usable (`638b6a0`, the user's test).
- Mouse motion must be recorded before `Gow3Overlay::HandleEvent` can swallow the event, as it is now
  (`last_mouse_motion_ms` is set first). Otherwise moving the mouse over the menu would not count as activity.
- `UpdateCursor` runs on the thread that polls SDL events (the `PollEvents` loop in gow3gpu.cpp). SDL wants cursor
  and window calls on that thread (SDL's rule, not checked in this code base); keep them there.
- The 3 s idle timer counts only `SDL_EVENT_MOUSE_MOTION`: mouse buttons and the wheel do not reset it [code].

## Interactions

- No performance interaction. The idle timer is one `SDL_GetTicks` compare per poll.
- A future mouse-look input (plan-work-queue.md items 3 / 13, `sceMouse` from SDL) will need a relative mouse mode,
  which changes this logic: in relative mode SDL hides the cursor itself.

## Evidence

- The user, after `4bb5565` + `638b6a0` (plan-work-queue.md, item 11): "exactly what we want".

## History and pitfalls

- Ported from the Bloodborne port (deadinside28/bloodborne_pc `4d4d208`, GPL-2.0). The focus rule is ours.
- The first version hid the cursor after alt-tab too. The user's test found it, and `638b6a0` fixed it.
- Before this the cursor was always shown, and the overlay only set ImGui's `MouseDrawCursor` (item 11 in the
  quality-of-life list).
