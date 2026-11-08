---
id: upstream-audio-input
title: Upstream audio and input: 7.1 -> stereo downmix, every controller merged, background pad events, stick range
area: runtime
status: active
platforms: all
default_windows: on
default_deck: on
origin: upstream 1.3.4 / 1.3.6
class: G
commits:
files: src/runtime_audio.c, src/runtime_pad.c, gpu/shim/window.cpp, launcher/medievil_launcher.py
symbols: audio_open, audio_state, downmix_stereo, soft_limit, output_port, out_channels, current_gamepads, gamepad_stick, gamepad_trigger, stick_range, SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, STICK_RANGES
switches: GOW3_AUDIO_SDL_DOWNMIX, GOW3_STICK_RANGE
tests:
docs: fix-inventory.md#from-upstream-medievil-133-138-bc160201983-and-contributors-merged-into-lab, study-wwise-stereo-mix.md#answer
depends_on:
related: pad-hotplug
checked: 2026-11-07
---
## What it does

These came with the upstream merges, so no commit of ours: 1.3.4 `4d72031` (downmix, #14) and 1.3.6 `8cf5cc5`
(pads, #25, #36). `runtime_audio.c` and `runtime_pad.c` are still upstream's code (`git diff upstream/main HEAD`
on them is empty); `window.cpp` and the launcher carry other changes of ours, but not in the hint or Stick range.
- Audio: `audio_open` gives SDL a stereo stream (`out_channels=2`) for an 8-channel port when the default device has
  1-2 channels. `downmix_stereo` folds 7.1 in SDL order: L = FL + 0.707 FC + 0.707 (SL + BL), likewise R, LFE
  dropped, peaks above 0.8 softly limited (`soft_limit`). SDL's own downmix scaled every channel down, so the game was
  quiet.
- Pads: `current_gamepads` opens every pad SDL lists and the reads merge them (buttons OR'd, the stick pushed
  furthest, the highest trigger). The window sets `SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS`, so pads work while
  another window has the focus. `GOW3_STICK_RANGE=<50-100>` (launcher "Stick range") makes that much tilt a full push.

## Invariants

- `downmix_stereo` works in place. Frame f's output (2 samples) lands at or before its input (8 samples), so it must
  read all eight channels before it writes. It assumes SDL's 7.1 order: `output_port` remaps the PS4 8-channel
  layout, and skips the remap for the `_STD` formats 6 and 7 (`std_layout`; Wwise's port is format 7), taken to be
  in SDL order already.
- The queue control in `output_port` counts bytes in the stream's channels (`out_channels`). A size computed from
  `p->channels` would aim the stereo queue at four times its level (latency) [inferred].
- The device's channel count is read when each port opens (`audio_open`). A device changed later keeps the open
  ports' choice. The downmix runs only for `out_channels==2 && channels==8`.
- Wwise reads the port's reported `channel` (2 for any port with more than 2 channels, `audio_state`) once at
  sink init and mixes in stereo. Reporting 3, 4, 5 or 7 makes its sink init fail (silence); 1 keeps a 7.1 mix
  (study-wwise-stereo-mix).
- `stick_range` reads `GOW3_STICK_RANGE` once into a static and accepts 50-100; anything else (or unset) becomes
  100, not the nearest bound.
  `gamepad_stick` picks the pad whose stick is pushed furthest by its raw values, then scales that one, so an idle
  pad's drift never beats a pushed stick.
- Pads are opened only in `current_gamepads`, rescanned once a second (at once while none is open).

## Interactions

- pad-hotplug (branch `fork-ports`): adds SDL hotplug hints at the same two init sites. Background events are what
  it must not break.
- Every platform, the Deck included: Wwise already mixes stereo, so the downmix only folds zero channels (FL FR then six zeros) plus the limiter.
  No CPU saving is left in the output layout. Do not port spacepepperoni's `58a7e18` (report 8 channels): Wwise would
  mix 7.1, estimated at +8-18% of a CPU [inferred] (study-wwise-stereo-mix.md).
- 5.1 / 7.1 PC devices get the stereo mix on the front pair only. Real surround means reporting 6 or 8 channels, with
  its own A/B.

## Evidence

- Upstream issue reports (#14 quiet audio, #25 pad unread, #36 sticks short of the edge). No measurement of ours.
  `runtime_audio.c` / `runtime_pad.c` merged without conflicts; the launcher's Stick range row came through the
  `e308c4c` conflict resolution (its message). The 1.3.8 merge test (N-27) covers the 1.3.6 pad code (the 1.3.4
  downmix came with `merge-1.3.5`) and found no regression, but it did not check audio or input. Not verified in
  game as a separate item.

## History and pitfalls

- `GOW3_AUDIO_SDL_DOWNMIX=1` goes back to SDL's downmix. `GOW3_AUDIO_DUMP` writes the 8-channel data before the
  downmix.
- The 1.3.4 note "louder audio" is this downmix (fronts at full level), not a volume change.
