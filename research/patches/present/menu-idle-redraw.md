---
id: menu-idle-redraw
title: VideoOut: the in-game menu's idle redraws count from the game's last flip
area: present
status: active
platforms: all
default_windows: on
default_deck: on
origin: ours
class: G
commits: b838a236ad
files: gpu/shadps4/core/libraries/videoout/driver.cpp
symbols: last_game_flip, LastGameFlip, DrawLastFrame, redraw_idle, CapturesInput
switches:
tests: N-25
docs: plan-linux-deck.md#spin-compact-texture-memo-and-the-task-split-2026-11-04, plan-work-queue.md#found-on-the-way-2026-10-30, plan-frame-pacing.md#n-25-preshadows-as-a-live-switch-2026-10-30-launched-by-claude-with-the-users-go-t-park3-clean-preshadows-offlog-t-pre2-live-switchlog
depends_on:
related: inline-present, reflex, precise-limiter, one-clock
verified: 2026-11-07
---
## What it does

While the in-game menu captures input (`Gow3Overlay::CapturesInput()`), the vblank loop redraws the last frame
(`DrawLastFrame`) so the menu stays live when the game draws nothing. Before this change it redrew every 16 ms even
while the game kept flipping under the menu. `Flip` now stores the time of the game's last flip in `last_game_flip`
(a relaxed atomic). The redraw waits for 16 ms from `max(last_redraw, LastGameFlip())`, so it only fills gaps of 16 ms
with no game frame. A paused game's menu redraws as before. The change is in both idle-redraw sites in
`PresentThread`: the `redraw_idle` lambda, used by the one-clock branch, and its copy in the main branch.

## Invariants

- `last_game_flip` is written only in `VideoOutDriver::Flip`, which is reached by real guest flips (vblank thread or
  newest-frame pacer). `DrawLastFrame` and `DrawBlankFrame` must not write it. If they did, the menu would stop
  redrawing whenever the game is paused.
- Every idle-redraw site must use the same `std::max(last_redraw, LastGameFlip())` test. The redraw code is
  duplicated, and a new copy that tests `last_redraw` alone brings back the extra presents.
- With presents on the vblank thread (inline-present) there is no `if_idle` skip, so this 16 ms test is the only
  thing that keeps redraws from adding presents beside the game's own.
- The 16 ms is wall time, not the frame limit. A game running under 62.5 FPS with the menu open still gets a redraw
  in each gap longer than 16 ms.

## Interactions

- inline-present: see Invariants. With a swap thread, `RunPresenter(..., true)` also drops redraws while frames are
  queued.
- reflex: `DrawLastFrame` calls `Present(frame, true)` with `is_game_frame` true, so with the sleep before the present
  each remaining redraw also takes a Reflex sleep. Found by reading the code, not measured.
- precise-limiter: a redraw is not a release and does not move `next_flip`.

## Evidence

- N-25 (PC, 2026-10-30, found on the way): with the menu open ~177 FPS, almost exactly 60 frames a second at ~8.7 ms
  (296 per 5 s). The same views with the menu closed ran at 211-237 FPS.
- Commit message (Deck): the game under the menu at ~79 FPS got up to ~60 extra presents a second (gamescope showed
  120+), and its frame pacing went from 1.4 to 4.8 ms stddev.
- Verified by the user on the Deck (2026-11-04): the menu redraw fix works, pacing with the menu up is normal.
  Not re-measured on the PC.

## History and pitfalls

- The cause sat open in plan-work-queue.md from N-25 (2026-10-30) to `b838a23` (2026-11-04); that row still says
  "open".
- Not covered: with the guest threads paused (`DebugState.IsGuestThreadsPaused()`), the loop calls `DrawLastFrame`
  on every tick, outside both sites.
- The change is in `deck-lab` only, not yet in `lab`.
- The redraw code exists twice in `PresentThread`. Folding it into one helper would make the invariant
  above automatic.
