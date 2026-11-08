---
id: thread-nice
title: Deck #8: nice -10 for stage B and the vblank thread (setpriority or rtkit), live off bit
area: deck
status: active
platforms: linux
default_windows: n/a
default_deck: on
origin: ours
class: G
commits: 24e461b, 5ab1a24
files: gpu/shim/gow3_linux_sched.cpp, gpu/shim/gow3_linux_sched.h, gpu/shim/gow3_toggles.h, gpu/shadps4/core/libraries/videoout/driver.cpp, gpu/shadps4/video_core/renderer_vulkan/vk_draw_pipe.h
symbols: WantedNice, RaisePriority, RtkitHighPriority, StageBThread, VBlankThread, PollLive, stage_b_tid, vblank_tid, live_busy, ThreadNiceOff
switches: GOW3_THREAD_NICE, toggle2:ThreadNiceOff
tests:
docs: impl-deck-build-sched-pool.md#8-nice-for-stage-b-and-the-vblank-thread-gow3_thread_nice, plan-linux-deck.md#thread-nice-live-2026-11-06-build-5ab1a24-nicelive-logs-datadeck-nicelive-1log-datadeck-nicelive-1400log, plan-linux-deck.md#round-1-results-gpu-clock-auto-2026-11-05-ini-logging-67-uncapped-launched-by-the-user
depends_on:
related: core-sched, deck-preset, inline-present, precise-tick, lineage-toggle-file
checked: 2026-11-07
---
## What it does

`GOW3_THREAD_NICE` at start (`WantedNice`: exactly `1` = nice -10, `-N` = nice -N down to -20, anything else off):
stage B (`StageBThread`, from `DrawPipe::Run`) and the vblank thread `shadPS4:PresentThread` (`VBlankThread`, after
`BoostPresentingThread`) get the nice level by `setpriority`, else through rtkit's `MakeThreadHighPriority` over the
system bus (`RtkitHighPriority`: host `libdbus-1.so.3` dlopen'd, private connection). The vblank thread also gets
1 ns timer slack. Live (`5ab1a24`): the vblank loop calls `PollLive()` every tick; toggle file second word 32768
(`ThreadNiceOff`) sets both back to nice 0, clearing it restores the start's nice, from a detached helper thread
(rtkit calls can take up to a second). One "Thread priority (GOW3_THREAD_NICE)" line per thread and switch.

## Invariants

- Never realtime scheduling; never root. Failure only logs ("unchanged, not nice -10: ...").
- `PollLive` returns at once without the env and makes no `setpriority` / rtkit call on the vblank thread (a change
  starts a detached helper thread, which inherits the vblank thread's nice and slack); `live_busy`
  allows one switch in flight, a change seen meanwhile is retried on a later tick.
- Live off resets only the two recorded tids (`stage_b_tid`, `vblank_tid`); threads they created after the start
  inherited -10 and keep it. It resets the nice level only: the vblank thread's 1 ns timer slack stays.
- A tid is recorded when its thread starts. `PollLive` runs on the vblank thread, so the vblank tid is always
  there first; if the off bit is already set before stage B starts, the switch is made without stage B
  (`!tid`: skipped), `live_off` is then true, and stage B, starting later, takes nice -10 and keeps it until the bit
  is cleared and set again [code reading, not seen].
- A set bit turns the fix OFF (toggle-file convention); with the env unset the bit does nothing. 32768 is the
  second word's bit; the first word's 32768 is `HotPages`, a different switch.
- Windows: every function is empty (`#ifdef _WIN32`).

## Interactions

- `inline-present` (default): presents run on the vblank thread, so nice covers the present call too.
- `precise-tick`: the 1 ns timer slack makes the tick's `sleep_until` wake on time; the Deck preset runs the tick
  at 900 Hz, not precise.
- `deck-preset`: `GOW3_THREAD_NICE=1` is one of its switches (`c4158d1`).
- `core-sched` was built in the same commit and file and rejected; both run from `StageBThread`, each with its own
  env.

## Evidence

- Round 1, auto clock, between launches (`data/deck-nice-1.log`): 80.15 FPS, worst 17.0 ms, p99 15.82 (the plan:
  "+0.8, sd 1.21"): inside the ~1.35-1.5 FPS within-launch drift (round 2), so it was made live.
- Live A/B (`data/deck-nicelive-1.log`, `-1400.log`; rtkit path: "setpriority: Permission denied, RLIMIT_NICE 0"):
  auto clock neutral (81.7 vs 82.0 FPS at nice 0), although stage B busy fell 2-3 points and stage A waits ~7
  points. **GPU fixed at 1,400 MHz: nice -10 wins every block**, 77.8 vs 77.4 FPS (mean +0.5), worst median 18.4
  vs 19.1 ms, sd 1.82 vs 1.98, p99 17.38 vs 17.64. "The gain is mostly in frame pacing." Not verified in motion.

## History and pitfalls

- From study-deck-apu-deep-dive.md ranked action 8. rtkit refuses without an active local session and rate-limits
  callers; repeated live switches may be refused (logged). `scx_lavd`, if active, reinterprets nice.
- The Deck's RLIMIT_NICE is 0, so only the rtkit path has been seen working; rtkit's default floor is -15.
