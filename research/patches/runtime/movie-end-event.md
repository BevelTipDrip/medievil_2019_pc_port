---
id: movie-end-event
title: A movie that plays to its end also sends WAR_LOOPING_BACK (UE4 PlaybackEndReached; the Best ending's second movie)
area: runtime
status: active
platforms: all
default_windows: on
default_deck: on
origin: ours
class: C
commits: e7694d9
files: gpu/shadps4/core/libraries/avplayer/avplayer_state.cpp
symbols: UpdateEndOfFileState, EmitEvent, ORBIS_AVPLAYER_ERROR_WAR_LOOPING_BACK
switches: GOW3_MOVIE_END_EVENT
tests:
docs: plan-work-queue.md
depends_on:
related: avplayer-trace, movie-suspend, avplayer-join-hang
checked:
---
## What it does

With all 20 chalices (`GetChalliceCount == 20` in `Level_AV_Outro`'s Blueprint, `IsBest`), MediEvil's ending opens the
UE4 media playlist `GameOutro_Best` (`VIDEO_GameOutro_01`, `VIDEO_GameOutro_02`) under the Best level sequence, whose
Wwise audio covers both movies. UE4 opens a playlist's next item on `EMediaEvent::PlaybackEndReached`. The game's PS4
media player event handler (`0x45c4b0` in the 01.02 eboot, the AvPlayer event callback) queues:

| AvPlayer event | UE4 event |
|---|---|
| StateStop (1) | PlaybackSuspended (7) |
| StateReady (2) | TracksChanged (9) + MediaOpened (3) |
| StatePlay (3) | PlaybackResumed (6) |
| StatePause (4) | PlaybackSuspended (7) |
| StateBuffering (5) | MediaBuffering (0) |
| WarningId 0x806A00A1 (WAR_LOOPING_BACK) | **PlaybackEndReached (5)** |
| WarningId 0x806A00A3 (WAR_JUMP_COMPLETE) | SeekCompleted (8) |
| WarningId 0x806A0004, DrmError | MediaOpenFailed (4) |

The port sent WAR_LOOPING_BACK only for looping sources, so a movie ending with looping off (every MediEvil movie)
became "suspended": the game stopped pulling frames from `gameoutro1.mp4` and never opened `gameoutro2.mp4`; the
Best sequence played its audio over a black screen to its end (205 s) and the credits started. Now
`UpdateEndOfFileState` sends WarningId WAR_LOOPING_BACK, then StateStop, when a source has run out. A stop the game
asks for (`sceAvPlayerStop`) still sends StateStop alone. `GOW3_MOVIE_END_EVENT=0`: StateStop alone, as before.

## Invariants

- Only the natural end (`UpdateEndOfFileState`: end of file, source no longer active, once per source through
  `m_eof_stop_event_sent`) sends the warning; the payload is the 32-bit code at `event_data` (the handler reads
  `dword [rcx]`).
- The warning goes before StateStop: the game's queue then holds EndReached, then Suspended.
- A real PS4 must report the end of a non-looping source this way for this UE4 build: otherwise no MediEvil movie
  would ever report PlaybackEndReached on console and the Best ending's playlist could not advance there either.

## Interactions

- Every movie that plays to its end (logo, intro, credits, in-level movies) now also reports PlaybackEndReached at
  that moment. Movies the game ends by its own timer or skip are unchanged.
- movie-suspend: a paused movie never reaches its end, so it never sends either event.
- avplayer-trace: the movie log shows `event WarningId` and `event StateStop` at each natural end.

## Evidence

- Found with the movie log (avplayer-trace, 2026-11-15): `gameoutro1.mp4` end of file, StateStop sent, then no player
  call from the game for 79 s until the Best sequence ended; no `gameoutro2.mp4` open. The handler's switch and its
  queued types decoded from `out/eboot.elf` (capstone, jump table 0x3c38914).
- Tester's 100% save (all 22 levels completed): the Best branch needs `ChaliceCount == 20` exactly; the debug menu's
  "give level chalice" on Zarok's Lair made it 21 (Regular ending). Reproduced with Zarok's Lair's Completed flag
  cleared (`@MediEvil0013`-`0016`, one byte) and the debug menu's complete level.
- Verified (the user, 2026-11-15, 01.02): "Yup! Looks like it played!"; log: WarningId + StateStop at 1276.48 s,
  `gameoutro1` closed and `gameoutro2.mp4` opened 3-4 ms later, 2299 frames (76.6 s), then the credits.
- Start-up (the user): the logos, the Book of Gallowmere and the intro advanced normally, no extra gaps. The starting
  logo flashed for a couple of frames between videos; not known whether that is new (work queue).

## History and pitfalls

- First suspected: the movie-suspend patch (two movies in succession) and `credits.mp4`'s 1980-pixel width. The
  movie log ruled both out: nothing was suspended, and the credits delivered frames.
- The black screen was `gameoutro2`'s place in the Best sequence; `gameoutro1` itself played (3802 frames).
