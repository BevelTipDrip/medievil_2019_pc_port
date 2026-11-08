---
id: movie-suspend
title: Intro movie still playing at a level change is paused (OpenLevel hook, AvPlayer pause)
area: ue4
status: active
platforms: all
default_windows: on
default_deck: on
origin: ours
class: M
commits: f0ced8c2c4, 1e8150f109
files: src/level_hook.h, src/probe.c, gpu/shadps4/core/libraries/avplayer/avplayer.cpp, gpu/shadps4/core/libraries/avplayer/avplayer_state.cpp, gpu/shadps4/core/libraries/avplayer/avplayer_impl.cpp
symbols: medievil_level_builds, medievil_level_find, medievil_level_build_stub, install_level_hook, medievil_level_change, SuspendPlayersForLevelChange, AvPlayerState::Suspend, TraceForget
switches: GOW3_MOVIE_SUSPEND
tests: T-M1, T-U1
docs: plan-frame-pacing.md#t-m1-intro-movie-suspended-at-the-level-change-2026-10-26-launched-by-claude-with-the-users-go-log-datat-m1-movie-suspendlog, study-intro-movie.md#summary, study-intro-movie.md#5-recommended-fix, fix-inventory.md#7-medievil-specific-ue4-config-eboot-patches-hooks
depends_on:
related: avplayer-trace, avplayer-join-hang, game-update-0102
checked: 2026-11-07
---
## What it does

MediEvil plays its intro on one persistent UE4 MediaPlayer and never stops it after the skip, so the hidden movie
kept decoding and uploading 1080p frames for ~200 s. `install_level_hook` (probe.c) redirects the rel32 call to
`UGameplayStatics::OpenLevel` inside the Blueprint native `execOpenLevel` (01.00 0x290eeda, 01.02 0x2911c9a) to a
48-byte stub that saves the argument registers, calls `medievil_level_change` -> `SuspendPlayersForLevelChange`,
restores them and jumps to `OpenLevel`. Every player still in Play / Buffering gets only its source's pause flag
(`AvPlayerState::Suspend` -> `AvPlayerSource::Pause`): no state change, no event, no buffer freed.

## Invariants

- The hook goes in only when `medievil_level_find` matches: the 12 bytes before the call and its `e8`, the call's target equal to
  `open_level`, and OpenLevel's 20-byte prologue. Another eboot gets no hook ("level-change suspend unavailable").
- The stub must preserve rdi, rsi, rdx, rcx, r8, r9 and keep rsp 16-byte aligned at the helper call (6 pushes +
  `sub rsp, 8`); `medievil_level_change` is `sysv_abi` because guest code calls it. Changing the stub length or the
  push count breaks OpenLevel's arguments or the alignment.
- `Suspend` must not call `SetState` or emit an event: the game's own clock then still reaches the end (197 s), finds
  `IsActive` true and calls `sceAvPlayerStop` itself; a `Stop()` in the hook would free the frame buffer the render
  thread may still be converting (study section 5).
- The player list is the AvPlayer trace map (`traces`, under `trace_mutex`); `sceAvPlayerClose` must call
  `TraceForget` before `delete handle`, or the suspend walks a freed player. Already-paused players are skipped
  (`IsPaused`), so later level changes log nothing for them.
- Assumption (class M): every level change of this game goes through `execOpenLevel`, and no movie has to keep
  playing across it (every cinematic goes through `UI_VideoPlayer`, which ends the movie before loading).
- Installed only with the GPU enabled (`if (gpu_enabled) install_level_hook`). `GOW3_MOVIE_SUSPEND=0`: off.

## Interactions

- avplayer-trace: the trace map it walks is that diagnostic's (always on, no switch: `Trace` at Init / InitEx adds
  every player); removing the trace, or putting it behind a switch, removes the player registry.
- avplayer-join-hang (branch `fork-ports` only): the same AvPlayer threads; a suspended player's decoder threads block
  on their buffers, so the join fix must still let them exit at the game's own Stop (206.9 s in T-M1) and Close.
- game-update-0102: the 01.02 sites (`medievil_level_builds[1]`) came with `1e8150f`.

## Evidence

- T-M1 (2026-10-26, third use of the ID): the skip opened a level at 15.9 s and suspended `gameintro.mp4` at 187
  frames; its count never moved again; the game called `Stop -> 0` at 206.9 s itself, as predicted [V]. Image
  uploads ~90 MiB/s for ~200 s before (T-U1) -> 0-5 MB/s from ~5 s after the skip, the whole session [V].
- Not verified in game by the user as a visual check; the effect is in the logs.

## History and pitfalls

- Found with the avplayer-trace diagnostic (T-U1: the intro never stopped or closed, 41 windows of uploads).
- Rejected alternatives (study): a pak Blueprint edit (`EndMovie` -> `VideoPlayer.Close`) needs Kismet bytecode
  insertion and a new import; making `IsActive` false or sending StateStop has no AvPlayer-level skip signal.
- The ID T-M1 is used three times (pool views, table stats, this); cite the anchor, not the ID alone.
