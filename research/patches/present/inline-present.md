---
id: inline-present
title: Present on the vblank thread instead of a swap thread (default)
area: present
status: active
platforms: all
default_windows: on
default_deck: on
origin: ours
class: G
commits: 44994b9bff, 0c357bc1cb
files: gpu/shadps4/core/libraries/videoout/driver.cpp, gpu/shadps4/core/libraries/videoout/driver.h, scripts/graphics_options.py, launcher/medievil_launcher.py
symbols: separate_swap, RunPresenter, SwapThread, swap_thread, PRESENT_FLAGS
switches: present_inline, GOW3_PRESENT_THREAD
tests: T-N120, T-V120h, T-P120
docs: fix-inventory.md#6-frame-pacing-and-present, plan-stage-b-split.md#t-p120-the-configuration-that-holds-the-cap-2026-10-23-0300-the-user-launcher-settings, plan-stage-b-split.md#ablation-of-the-t-p120-settings-2026-10-23-evening-traces-research-dataflip-trace-18csv-to--24csv
depends_on:
related: precise-tick, reflex, precise-limiter, menu-idle-redraw, newest-frame, one-clock, present-mode-select, presentation-settings, deck-preset
verified: 2026-11-07
---
## What it does

`present_inline=1` sets `GOW3_PRESENT_THREAD=0` (`PRESENT_FLAGS`). The `VideoOutDriver` constructor then leaves
`separate_swap` false and starts no swap thread, so `RunPresenter` runs the present work (`SetHDR`, `Present`, the
Reflex sleep) on the calling thread: the vblank thread, or the newest-frame pacer. The default otherwise queues the work
onto the `gow3:Present` swap thread. The switch is inherited (`switches.tsv`: upstream). The finding, its launcher
setting (`44994b9`) and making it the default (`0c357bc`) are ours.

## Invariants

- Inline, each vblank-loop pass includes the present's cost: acquire, blit submit, `vkQueuePresentKHR`. That is
  0.3-0.5 ms on the PC (T-F14); on the Deck the `vkQueuePresentKHR` call alone took 0.94 ms (native run 4, PC 0.15).
  The tick and slot waits must tolerate a pass longer than one tick: the precise tick's re-anchor, the precise
  limiter's `next_flip = max(...)`.
- `RunPresenter`'s `if_idle` skip (no redraw while frames are queued or the swap thread is busy) exists only for the
  swap thread. Inline, every idle redraw presents, so the callers must limit the rate (the 16 ms rule, see
  menu-idle-redraw).
- Anything that blocks inside `Present` (the Reflex sleep, `GOW3_LIMIT_IN_PRESENT`, a FIFO present waiting for the
  display) blocks the vblank thread, and with it the guest's vblank events. That is intended for Reflex (as with
  RTSS). With FIFO at a 165 tick it showed as missed ticks of 12.1 ms (ablation run 5).
- `swap_thread` is declared before `present_thread` in `driver.h`, so it is destroyed after it. This only matters
  with the swap thread.

## Interactions

- precise-tick: the user found these two essential together (T-P120). Not measured separately with the precise tick.
- reflex: the sleep runs on the vblank thread (no ticks during it). Every Reflex measurement (T-F17-T-F27) was inline.
- precise-limiter: the hold holds the presenting thread itself.
- newest-frame: the pacer thread calls `Flip`, so inline it presents on the pacer thread while the vblank thread
  can still present idle redraws: two threads in `Presenter::Present` with no swap queue between them. Never run
  (T-L5 used the swap thread) [code reading].
- present-mode-select: a Fifo mode blocks the vblank thread on the display (ablation run 5).
- deck-preset: on (`present_inline=1`).

## Evidence

- T-N120 (inline, coarse 120 tick, `GOW3_FRAMES_AHEAD=0`): present calls 1% 7.0 / 99% 9.7 ms, no 12.5 / 4.17 ms waves.
  The user: lows better than T-V120p (swap thread), but not T-V120's flat line.
- T-V120h (G-Sync off, inline): "that isn't it", so inline alone did not bring back T-V120.
- T-P120: the user's read was that the precise vblank tick and the present on the vblank thread contribute most.
  Ablation 0c (120): 98% of frames 8.12-8.54 ms. 0d (165): 165.0 avg, 1% 158.1, 0.1% 151.8, 0 frames over 9 ms in 75 s.

## History and pitfalls

- The user found it in ~2 h of their own testing before the ablation. No run since then has the swap thread, so the
  swap-thread path (the `if_idle` skip, priority on the `swap` thread) is not exercised by the defaults.
- One-clock v1 broke partly because the inline present plus the vblank events overran a 1.04 ms tick (T-F14).
