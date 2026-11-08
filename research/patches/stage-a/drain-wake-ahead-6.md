---
id: drain-wake-ahead-6
title: Drain wake-ahead count 6 instead of 2 (live A/B bit)
area: stage-a
status: rejected
platforms: all
default_windows: off
default_deck: off
origin: ours
class: G
commits: d2f145f
files: gpu/shim/gow3_drain_wait.h, gpu/shim/gow3_toggles.h
symbols: WakeAhead, DrainWakeAheadDefault
switches: GOW3_DRAIN_WAKE_AHEAD, toggle2:DrainWakeAheadDefault
tests:
docs: plan-linux-deck.md#drain-wake-ahead-6-vs-2-live-2026-11-06-build-d2f145f-lows-log-datadeck-wakeahead-1log, impl-stage-a-decode.md#2-why-stage-b-waits-1-ms-a-frame-on-stage-a
depends_on: drain-hybrid
related: deck-preset
checked: 2026-11-07
---
## What it does

Under drain-hybrid, stage B wakes a sleeping stage A `GOW3_DRAIN_WAKE_AHEAD` packets before the end of the drain
(default 2). The idea was to wake it 6 packets (about 40 us of stage B work) early, more than the measured 51 us
average wake latency minus the 30 us spin. Then stage A would be running again before stage B runs dry. `d2f145f`
only adds the live A/B: with `toggle2:DrainWakeAheadDefault` (second word 8388608) set, `WakeAhead()` returns 2
whatever the start's count.

## Invariants

- `WakeAhead()` clamps the start's value to 0-15 (16 commit ends are kept). With the bit set it returns 2.
- The count only moves where a drain sleeps until. The drain still ends at the exact head (drain-hybrid's
  invariants), so any value is correct. Only the cost changes.
- Nothing sets `GOW3_DRAIN_WAKE_AHEAD` by default, and the Deck preset leaves it at 2.

## Interactions

- drain-hybrid: the knob belongs to its wait (`WakeAhead()` is called only from `HybridWait`). The toggle bit does
  nothing unless `GOW3_DRAIN_HYBRID=1` (and second-word bit 4096 clear) and `GOW3_DRAIN_WAKE_AHEAD` set to something
  other than 2 at start.

## Evidence

- Deck (2026-11-06, build `d2f145f-lows`, launched by the user, preset switches + `GOW3_DRAIN_WAKE_AHEAD=6`, GPU at
  1,400 MHz, heavy view, `ab "0 0" "0 8388608" 30 5`):

  | state | FPS median | 1% / 0.1% low FPS | p99 ms | drain wait ms/frame | sleeps/frame |
  |---|---|---|---|---|---|
  | 6 ahead | 76.8 | 56.5 / 51.8 | 17.74 | 0.63 | 1.66 |
  | 2 ahead | 76.5 | 56.6 / 51.0 | 17.64 | 0.62 | 1.50 |

  Neutral. +0.3 FPS is within the noise, and the lows and the drain wait are equal.

## History and pitfalls

- Why it did not help: stage A woke before the end more often and went back to sleep (1.66 vs 1.50 sleeps a
  frame). The 51 us wake latency is not what stage A waits on. After a drain, stage A simply makes packets more
  slowly than stage B records them (impl-stage-a-decode.md section 2).
- Worth retrying only if the wake latency itself becomes the gap: stage B running dry right after each sleeping
  drain, seen in the "wake latency" and stage B packet-ring sleep counters. Raising the count will not shorten a
  wait that is set by stage A's per-draw cost.
- The `d2f145f-lows` build also carried the 1% / 0.1% lows in the "Frame pacing" line (`0ba768b`, not part of this
  patch; `deck_summary.py` pools them).
