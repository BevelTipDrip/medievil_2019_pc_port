---
id: display-trace
title: Display times through VK_KHR_present_wait (GOW3_DISPLAY_TRACE)
area: present
status: rejected
platforms: all
default_windows: off
default_deck: off
origin: ours
class: G
commits: d18cfdf040, 8a96bc7ea8
files: gpu/shadps4/video_core/renderer_vulkan/vk_instance.cpp, gpu/shadps4/video_core/renderer_vulkan/vk_instance.h, gpu/shadps4/video_core/renderer_vulkan/vk_swapchain.cpp, gpu/shadps4/video_core/renderer_vulkan/vk_swapchain.h
symbols: IsPresentWaitEnabled, IsPresentIdEnabled, DisplayWaitThread, waitForPresentKHR, SetPresentTag, present_wait
switches: GOW3_DISPLAY_TRACE
tests: T-V120b, T-V120c
docs: plan-stage-b-split.md#t-v120-120-hz-tick-vsync-fifo-no-limiter-50-ue4_upscaleperformance-2026-10-23-launched-by-claude-with-the-users-go-f4ffc01-gow3_vblank_hz120-trace-flip-trace-8csv, plan-stage-b-split.md#baseline-recheck-of-t-v120-2026-10-23-0130-0210-launched-by-claude-with-the-users-go-each-time, fix-inventory.md#10-tried-and-dropped
depends_on:
related: flip-trace, reflex, vblank-sync
verified: 2026-11-07
---
## What it does

A `displayed` stage for the flip trace. With `GOW3_DISPLAY_TRACE=1` (`8a96bc7`; in `d18cfdf` it came with any
`GOW3_FLIP_TRACE`) the device enables `VK_KHR_present_id` + `VK_KHR_present_wait` (`present_wait`). `Swapchain::Present`
attaches a present id to each present, and for game presents (a `SetPresentTag` tag >= 0) queues the id for
`DisplayWaitThread`. That thread calls `waitForPresentKHR` for each, in order (2 ms timeout, up to 100 tries), and
notes `displayed` with the guest's flip argument. Off by default; diagnostic only.

## Invariants

- `present_wait` needs both features and both extensions. Without them, `IsPresentWaitEnabled` is false and nothing
  runs.
- The switch no longer checks `GOW3_FLIP_TRACE`. With `GOW3_DISPLAY_TRACE=1` alone the wait thread still runs and
  waits for every game present, with no trace to record it. Harmless, but it is a present-path change without a trace.
- Present ids are shared with Reflex: `present_id = present_id || present_wait` in `vk_instance.cpp`. Reflex adds
  `VK_KHR_present_id` only if the display trace did not end with `present_wait` true (`present_wait ||` in the Reflex
  block). A third user must keep that single-add order. The trace thread also holds `wait_mutex` during each 2 ms
  wait, which the swapchain's teardown takes too.

## Interactions

- flip-trace: the trace this stage belongs to. `present_start` (present-side-limiter) and `gpu_ready` are the
  stages that remained useful.
- reflex: since T-F27, `VK_KHR_present_id` is on in every default Windows run whose driver has `presentId` (the
  Reflex block adds it before checking for `VK_NV_low_latency2`, so AMD / Intel too), which is the state this card's
  move behind a switch tried to avoid in traces.
- vblank-sync: the other attempt to see the display's own timing.

## Evidence

- T-V120b (`d18cfdf`, T-V120's settings plus display times): `displayed` came 0.00 ms after `presented` for every
  frame. NVIDIA's present wait returns at the present, so it carries no display information. Present calls spread
  1% 7.06 / 99% 9.67 ms, like the native run, not T-V120's 8.42 ms. The user: unstable lows again.
- T-V120c (`8a96bc7`, display times off): 14% locked, "completely different". So T-V120's state did not come back
  without present ids either: the baseline recheck ruled out the code, G-Sync and the NVCP present method.
- PresentMon (T-F18 / T-F19, `research/tools/presentmon-capture.ps1`) became the display-side measure instead.

## History and pitfalls

- "Present ids change the present path" was a suspicion from one run (T-V120b), never isolated, and Reflex has kept
  them on since T-F27. If the PC's present spread is ever studied again, A/B present ids alone with PresentMon.
- Could be worth retrying on a driver whose present wait reports the real display time (Mesa / RADV on the Deck
  [inferred, not tested]).
