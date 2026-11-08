---
id: nav-purge-nofence
title: Navigation path purge without the two mfences per refcount read (GOW3_NAV_PURGE_NOFENCE)
area: ue4
status: rejected
platforms: all
default_windows: off
default_deck: off
origin: ours
class: M
commits: b98df73397
files: patches/MediEvil.xml, scripts/graphics_options.py, run.py, tests/test_graphics_options.py, tests/test_medievil_patches.py
symbols: NAV_PURGE_PATCH, nav_purge_environment
switches:
tests:
docs: impl-cpu-waste.md#the-patch-item-2-gow3_nav_purge_nofence1, impl-cpu-waste.md#what-0x802c94ba2--ba5--ba8-is-not-a-wait-static-analysis-0102-high-confidence, plan-linux-deck.md#round-1-results-gpu-clock-auto-2026-11-05-ini-logging-67-uncapped-launched-by-the-user
depends_on:
related: deck-preset
checked: 2026-11-07
---
## What it does

The Deck profile's top game-thread samples (guest 0x2c94ba2/ba5/ba8, ~6% of the thread) are UE 4.21's
`ANavigationData::TickActor`, which purges `ActivePaths` every frame. Each entry's TWeakPtr reference count is read
through the PS4 build's AtomicRead, `mfence; mov eax, [rax+8]; mfence`, which serialises the cache misses. The eboot
patch "Navigation path purge without fences (CPU)" keeps the load and turns the fences into one 9-byte nop (01.00
0x2c91ddf, 01.02 0x2c94b9f). Opt-in and restart-only: `GOW3_NAV_PURGE_NOFENCE=1` / `0` (run.py, extra-env.txt;
Python-only, not in switches.tsv) or the launcher's Patches page (`isEnabled="false"`).

## Invariants

- Safe only because an aligned 32-bit load is atomic on x86, the loop stores nothing the fences order against, the
  count never rises from 0 (`Pin()` refuses 0), and the weak reference keeps the controller allocated. A path that
  dies during the read is dropped a frame later, as before.
- Only this one site. The image has 138 `mfence; mov [r+8]; mfence` sites and ~6,300 mfences, and some may need
  sequential consistency. Do not generalise the patch.
- `Original` bytes (`0faef08b40080faef0`) are checked per line. No branch lands inside the 9 bytes.

## Interactions

- deck-preset: left out of `DECK_SWITCHES` (neutral). With the switch unset the launcher's Patches page can still
  tick it (`isEnabled="false"` only sets the page's default).
- Game thread only. Stage B is the Deck's bottleneck, so FPS was expected unchanged.

## Evidence

- Deck round 1 (GPU auto, ini logging 67% uncapped, `data/deck-nav-1.log`, patch applied, 18 writes): 79.4 FPS,
  worst 17.3 ms, p99 16.14 ms. Neutral in FPS. Game-thread CPU and power were not measured.
- Two segments of the same state within one launch differed by 1.35 FPS (round 2), so between-launch results under
  ~1.5 FPS cannot be judged; this restart-only result is inside that.

## History and pitfalls

- Tried: one launch with the patch against the round's other launches. Measured: no FPS change. Dropped because
  stage B, not the game thread, gates the Deck's frame, and the patch could not be A/B'd live.
- Worth retrying only with a measure that can see it: `deck.sh threads` (game-thread CPU%) and a `perf record`
  at the same spot (the three samples should shrink), or APU power at a fixed GPU clock. A frame-time gain would
  need a game-thread-bound scene. A larger fix (purge every N frames through a host helper) needs a code cave and is
  less clearly safe.
