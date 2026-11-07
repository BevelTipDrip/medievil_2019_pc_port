---
id: present-mode-select
title: GOW3_PRESENT_MODE read by Swapchain::FindPresentMode; a forced Immediate the surface lacks falls back to Mailbox
area: present
status: active
platforms: all
default_windows: off
default_deck: off
origin: ours
class: G
commits: 44994b9bff, a4250a2bad
files: gpu/shadps4/video_core/renderer_vulkan/vk_swapchain.cpp, scripts/graphics_options.py
symbols: FindPresentMode, UpdateVsync, PRESENT_MODES, present_environment
switches: GOW3_PRESENT_MODE, present_mode
tests: T-P120
docs: fix-inventory.md#6-frame-pacing-and-present, plan-linux-deck.md#baseline-1-and-live-abs-2026-11-04-appimage-a863482-67-helpers-auto-no-dedicated-core-auto-clocks-mailbox-log-datadeck-baseline-1-ablog, plan-stage-b-split.md#ablation-of-the-t-p120-settings-2026-10-23-evening-traces-research-dataflip-trace-18csv-to--24csv
depends_on:
related: vsync-control, presentation-settings, inline-present, reflex, deck-preset, upstream-vsync-limit-rule
verified: 2026-11-07
---
## What it does

`present_mode` (ini: auto / fifo / fifo_relaxed / mailbox / immediate; the launcher's "VSync" choice writes
`immediate` for "Off, allow tearing", `fifo_relaxed` for "Adaptive", `auto` for On / Off) becomes
`GOW3_PRESENT_MODE` (`present_environment`, nothing for `auto`). `Swapchain::FindPresentMode` uses that mode instead of
following the in-game VSync switch. The variable existed upstream (`EmulatorSettings::GetPresentMode`) but nothing read
it before `44994b9`. A forced mode the surface does not offer falls back to Fifo. Since `a4250a2`, a forced Immediate
falls back to Mailbox when the surface has Mailbox, and the log line adds ", not offered by the surface". The default
`auto` follows VSync: on is Fifo; off is Mailbox, else Immediate, else Fifo.

## Invariants

- Only the lower-case names are recognised. Any other value (upstream's "Mailbox", a typo) is forced Fifo, which
  means VSync on and a frame rate capped at the refresh. `PRESENT_MODES` in `graphics_options.py` filters the ini
  value, but a hand-set `GOW3_PRESENT_MODE` is not filtered.
- While a mode is forced, the in-game VSync switch does not change the mode: `UpdateVsync` recreates the swapchain
  and `FindPresentMode` returns the forced mode again. The launcher's hint says so ("These two fix the mode"). The
  `vsync` key still feeds `current_limit`'s VSync rule, and the launcher leaves it as it was when it writes a forced
  mode (default 1): with "Off, allow tearing" and `vsync=1`, a limit at refresh - 1 or above is dropped although
  nothing waits for the display.
- The fallback for a missing Immediate must not wait for the display. Mailbox does not; Fifo would cap the frame rate
  at the refresh (the 2026-11-04 Deck run).
- An empty value is the same as unset (`forced[0]`).

## Interactions

- vsync-control, presentation-settings: the launcher controls that write `present_mode`.
- inline-present: with presents on the vblank thread, a Fifo mode blocks that thread on the display. A 165 tick with
  FIFO missed ticks (ablation run 5).
- reflex: T-F24 used Immediate. T-F27 (the working mode) used VSync on (auto -> Fifo).
- upstream-vsync-limit-rule: reads `Gow3Settings::vsync`, not the forced mode (see Invariants).
- deck-preset: `present_mode=auto`, `vsync=0`, so Mailbox under gamescope. The fallback only matters when the user
  picks "Off, allow tearing".

## Evidence

- T-P120: Immediate was one of the user's settings (the user: Immediate possibly helps). Ablation 0d (165, Immediate):
  165.0 avg, 1% 158.1, 0.1% 151.8. Run 5 (FIFO, 165): 1% low 126.3, 0.1% 81.9.
- Deck, 2026-11-04: with `GOW3_PRESENT_MODE=immediate` the log said "Swapchain: present mode Fifo
  (GOW3_PRESENT_MODE=immediate)". The gamescope surface offers no Immediate, even with "allow tearing" on, and the user
  saw the frame rate capped. That run led to `a4250a2`. The Mailbox fallback itself was not run on the Deck afterwards
  (not verified in game).

## History and pitfalls

- True tearing on the Deck would need gamescope to offer Immediate. Mailbox is the uncapped mode there.
- The fallback is in `deck-lab` only (`a4250a2`). `lab` still falls back to Fifo.
