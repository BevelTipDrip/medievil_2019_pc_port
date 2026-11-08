---
id: deck-preset
title: Launcher Deck Optimized preset (the user's flat 60) with its Steam Deck performance switches and Deck detection
area: deck
status: active
platforms: all
default_windows: off
default_deck: on
origin: ours
class: M
commits: aec4e33, c4158d1
files: launcher/medievil_launcher.py, tests/test_deck_preset.py
symbols: DECK_PRESET, DECK_SWITCHES, DECK_QUALITY, DECK_HINT, steam_deck, deck_preset_offer, apply_deck_preset, offer_deck_preset, quality_choice_var, game_environment, deck_switches, deck_offered
switches:
tests:
docs: plan-linux-deck.md#a-flat-60-on-the-deck-the-user-2026-11-05, plan-linux-deck.md#deck-optimized-preset-2026-11-05-launcher-built-not-run, plan-linux-deck.md#thread-nice-live-2026-11-06-build-5ab1a24-nicelive-logs-datadeck-nicelive-1log-datadeck-nicelive-1400log
depends_on: early-frame-eop, decode-pacing, drain-hybrid, T2, R1-region-index, A1, A2, A3, particle-flip-guard, thread-nice, single-particle-pass
related: U2, 4k-memory, pool-kept-pages, M1, perf-patches-ini, presentation-settings, vsync-control, extra-env-file, prep-workers-2, core-sched, S1-v3-build, nav-purge-nofence, stage-b-spin
verified: 2026-11-07
---
## What it does

Graphics quality > "Deck Optimized" (Home and Performance) writes `DECK_PRESET` into medievil.ini
(`apply_deck_preset`: 1280x720 borderless, render scale 80, Low with effects 50, volumetric fog on, fps_limit 60,
VSync off (Mailbox), engine 240 + `present_no_frame_caps=1`, tick 900 Hz not precise, Standard limiter, inline
present, frames queued 1, every perf patch on but `perf_4k_memory=0`, `perf_particle_single_pass=1`) and turns on
`deck_switches`, which `game_environment` passes with `env.setdefault`: `DECK_SWITCHES` = `GOW3_EARLY_FRAME_EOP=1`,
`GOW3_DECODE_PACING=1` + `_LEAD=2`, `GOW3_ARENA_HOST_VISIBLE=0`, `GOW3_DRAIN_HYBRID=1`, `GOW3_TEXSET_WIDE=1`,
`GOW3_REGION_INDEX=1`, and since `c4158d1` `GOW3_BACKING_COPY=1`, `GOW3_PENDING_FILTER=1`, `GOW3_REG_HASH_PAIRED=1`,
`GOW3_PARTICLE_FLIP_GUARD=1`, `GOW3_THREAD_NICE=1`. `ue4_quality` stays `low` for the game; "deck" lives only in
`graphics_preset`. `steam_deck()`: `SteamDeck=1`, or DMI vendor Valve with product Jupiter / Galileo;
`deck_preset_offer` selects it on a first run on a Deck, asks once on an existing setup (`deck_offered`), never on
`--play`. `DECK_HINT` advises a manual GPU clock of about 1,200 MHz.

## Invariants

- fps_limit 60 must be the only clock: the engine unbounded (no_frame_caps -> 1000 FPS bound), the tick (900) above
  the cap so `driver.cpp` `current_limit` keeps it, VSync off so it is not zeroed, Standard limiter so no Reflex.
  `engine_fps=60` would add UE4's own bound (two limiters at one rate fight, T-F13); a 60 Hz tick turns a missed
  tick into 33 ms on the Deck. `test_the_60_limit_is_the_only_clock` checks it.
- An environment variable already set (or extra-env.txt, applied last by `run.py` `load_extra_env`) wins over a Deck
  switch. `--play` (Steam shortcuts) passes the saved `deck_switches` too (`play_without_window` ->
  `game_environment`), but never makes the offer.
- Not Linux defaults: off unless `deck_switches` (default False); choosing another quality in the launcher ends the
  preset and its switches (other values stay).
- `test_switches_come_with_the_preset_only` pins `DECK_SWITCHES` exactly; a change needs the test changed with it.

## Interactions

- Each switch is its own card (depends_on). `U2` is turned off (`GOW3_ARENA_HOST_VISIBLE=0`: no gain on the Deck).
  The switches are platform-neutral env: `deck_switches` on Windows sets them too, where `GOW3_THREAD_NICE` and A1's
  `GOW3_BACKING_COPY` do nothing (Linux-only code).
- `single-particle-pass` is on only with `particle-flip-guard` (the guard fixes the torch / crypt floor bug).
- `perf_4k_memory=0` -> `GOW3_PS4_MEMORY=1` (stock pools): more pool reuse, so `pool-kept-pages` (Linux) and `M1`'s
  un-zeroed kept pages (Windows: "the PC with the Deck preset did once") show sooner.
- Left out (plan): `nav-purge-nofence` (neutral), `stage-b-spin`, `S1-v3-build`, `core-sched` (rejected), pool
  prefill; `prep-workers-2` was never added.

## Evidence

- The user (2026-11-05, Deck OLED): a flat 60 FPS in the heaviest graveyard view with these settings and the GPU at
  1,200 MHz, from a hand-edited ini and seven extra-env variables before the preset existed. "cutting CPU usage is
  still the big win here, because there's a lot of GPU headroom, we just can't use it".
- `c4158d1`'s additions: stage A cuts +0.6 FPS (auto) / +0.65 (1,200 MHz); nice better pacing at 1,400 MHz; the
  flip guard alone fixes the crypt floor. The preset as written by the launcher on a fresh data folder is **not
  verified on the Deck** (plan: "built, not run"); `tests/test_deck_preset.py` and an off-screen Tk smoke test pass.

## History and pitfalls

- The checkbox text "Steam Deck performance switches" still lists only the first six switches (no A1-A3, flip
  guard, nice).
- `quality_choice_var`: a quality changed in the in-game menu clears `graphics_preset` but leaves `deck_switches` on
  (only a launcher choice clears both). Code reading; not seen as a problem yet.
