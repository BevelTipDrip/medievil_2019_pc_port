---
id: upstream-vsync-limit-rule
title: Upstream VSync frame-limit rule: VSync on and limit >= refresh - 1 leaves the cap to VSync
area: present
status: active
platforms: all
default_windows: on
default_deck: on
origin: upstream 1.3.3
class: G
commits: 8a70032, b7b78fb990, e6bf93dab4
files: gpu/shadps4/core/libraries/videoout/driver.cpp, gpu/shim/gow3gpu.cpp
symbols: current_limit, Gow3DisplayRefreshHz, reflex_interval, SetLatencyInterval
switches:
tests:
docs: fix-inventory.md#from-upstream-medievil-133-138-bc160201983-and-contributors-merged-into-lab, study-upstream-commits.md#3-conflicts-with-ours, plan-stage-b-split.md#t-l1b-precise-frame-limiter-at-233-2026-10-21-0140-launched-by-claude-with-the-users-go-build-a297c6b-log-datat-l1b-precise-233log
depends_on:
related: reflex, precise-limiter, vrr-auto-limits, frame-limit-parse, vsync-control, presentation-settings, present-mode-select, one-clock, lineage-toggle-file
checked: 2026-11-07
---
## What it does

In `current_limit` (the vblank thread's frame limit, re-read every pass): when the `vsync` setting is on and
`limit + 1 >= Gow3DisplayRefreshHz()`, the limit becomes 0 and the display alone paces the presents. A limit at the
refresh is a second clock beside the display's own (often 59.94 Hz) that repeats or drops a frame now and then
(upstream issue #3, 4K TV at 60 Hz). Upstream returned 0 at once, and so did our first merge (`b7b78fb`, branch `merge-1.3.5`, before Reflex existed);
the merge into `merge-1.3.8` (`e6bf93d`) set `limit = 0u` and falls through, so the Reflex branch after it runs.

## Invariants

- Order in `current_limit`: user limit (`ResolveFpsLimit`), fixed / env limit, toggle-file cap, `limit >= vblank_hz`
  -> 0, then this rule, then Reflex. The rule must stay before the Reflex branch and must not `return` early:
  Reflex keeps its driver interval in `reflex_interval` and updates it only when it runs, so an early return would
  leave the old `SetLatencyInterval` cap in force with VSync on.
- It reads only `Gow3Settings::vsync` (live, in-game menu), not the swapchain's present mode. `GOW3_PRESENT_MODE`
  (launcher "Off, allow tearing" = immediate, "Adaptive" = fifo_relaxed) fixes the mode without changing `vsync`;
  see History for the consequence.
- `Gow3DisplayRefreshHz` is the primary display's mode, rounded (59.94 -> 60), at least 60, cached at first call:
  a window on another monitor or a refresh change after start uses the old value; a 50 Hz display counts as 60.
- The toggle file's third-word cap passes through the rule too: with VSync on, a cap >= refresh - 1 is dropped.

## Interactions

- reflex: with the rule hit, Reflex gets interval 0 (no driver cap); VSync paces. The default Windows set
  (`vsync=1`, Reflex) therefore never runs two clocks at the refresh.
- vrr-auto-limits: Auto refresh without VRR resolves to the refresh, so the rule hands it to VSync; with VRR the
  Reflex-rule values (116 at 120 Hz, 138 at 144) and the fixed 141 / 237 stay below refresh - 1 and are kept.
- frame-limit-parse: a fixed 119 on a 120 Hz display with VSync on is dropped (119 + 1 >= 120); 119 is labelled for
  240 Hz displays. Under VRR this hands the cap to VSync at the refresh (not tested).
- precise-limiter, one-clock: both need a non-zero limit, so neither runs when the rule fires.
- deck-preset: `vsync=0`, so the rule never fires with the Deck Optimized preset (code present, inert) unless
  VSync is turned on in the in-game menu.

## Evidence

- Upstream's fix for issue #3 (stutter with a 60 FPS limit at 60 Hz); no run in this port's docs tested it.
- Merge test of 1.3.3-1.3.8 into `lab` (N-27, fix-inventory): no regression against the pre-merge `lab`; not a test
  of this rule. T-L1c (VSync alone at 240) was skipped. Not verified in game.

## History and pitfalls

- Upstream's first issue #3 fix (1.3.2, `783b25a`) is the tick-side twin: `limit >= vblank_hz` -> 0 (moved by
  `966d23d`, still just before this rule).
- study-upstream-commits.md predicted an auto-merge with no conflict marker and suggested gating it
  (only without `GOW3_PRECISE_LIMIT` / `GOW3_NEWEST_FRAME`, automatic tick). Not done: it applies to every limiter.
- Found in code (not tested): with "Off, allow tearing" chosen while the `vsync` key is still 1 (the launcher's
  `vsync_choice_var` leaves it as it was), the swapchain is Immediate, yet a limit >= refresh - 1 is dropped: only
  the engine frame rate caps the game. A fix would check the effective present mode (Fifo / FifoRelaxed). The same
  holds for the in-game VSync switch under a fixed mode: the launcher says it has no effect, yet it still turns this
  rule on or off.
