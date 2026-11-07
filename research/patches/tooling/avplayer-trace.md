---
id: avplayer-trace
title: AvPlayer trace: player calls and the frames each player delivered
area: tooling
status: diagnostic
platforms: all
default_windows: on
default_deck: on
origin: ours
class: G
commits: 6fd8e15ea7
files: gpu/shadps4/core/libraries/avplayer/avplayer.cpp
symbols: PlayerTrace, traces, trace_mutex, Trace, TraceSource, TraceFrame, TraceForget, TraceSeconds, SuspendPlayersForLevelChange, sceAvPlayerClose, sceAvPlayerGetVideoDataEx, sceAvPlayerInit, sceAvPlayerInitEx
switches:
tests: T-D2d
docs: plan-frame-pacing.md#upload-audit-what-is-copied-from-system-ram-to-the-gpu-2026-10-19-logs-only-no-launch, study-intro-movie.md#summary
depends_on:
related: movie-suspend, avplayer-join-hang, texcache-stats
---
## What it does

Always on (no switch): every AvPlayer call except the per-frame polls prints one line, "AvPlayer T s: <handle>
<call> <detail> -> <result> (<source>, N frames delivered)", flushed at once: Init / InitEx (auto start),
AddSource(Ex), Start, Stop, Pause, Resume, SetLooping, the JumpToTime / SetTrickSpeed stubs, Close.
`sceAvPlayerGetVideoDataEx` counts the frames each player handed to the game (`TraceFrame`). The per-handle state
lives in `traces` under `trace_mutex`.

## Invariants

- `traces` is not only a log: movie-suspend's `SuspendPlayersForLevelChange` walks it as the list of live players.
  So every player must enter it at creation (`Trace` in `sceAvPlayerInit` / `InitEx` inserts through `operator[]`)
  and `sceAvPlayerClose` must call `TraceForget` before `delete handle`, under the same mutex. Gating the trace
  behind a switch, or dropping the Init line, silently breaks the movie suspend (the intro decodes into gameplay
  again); deleting before forgetting lets the suspend touch a freed player.
- Lines are few (a handful per movie); the frame count is a map lookup under a mutex once per delivered frame
  (~30 a second while a movie plays), not per game frame.

## Interactions

- movie-suspend: depends on this card's `traces` map (above). Its suspend line uses the same format.
- avplayer-join-hang: the other AvPlayer fix (thread joins); both edit `avplayer.cpp`.
- texcache-stats: the upload audit that pointed here (six 1920x1088 R8 + two 960x544 R8G8 planes re-uploaded 30
  times a second, ~90 MiB/s, for exactly 200 s per run).

## Evidence

- Upload audit (2026-10-19, logs only): the intro was ~85% of all texture re-upload bytes (11.7 GiB of Y planes a
  run); `6fd8e15` was added to see what the game asks the player.
- T-D2d (2026-10-19): "The intro starts at 11.6 s (AvPlayer trace)". study-intro-movie.md: the log shows the logo's
  Stop and Close at 7.243 s and the intro's Init at 7.246 s, and no Close for the intro even at exit, so the game
  never stops it. Diagnostic; the fix (movie-suspend) is the part verified in game.

## History and pitfalls

- The trace proved the game, not the port, leaves the intro running (the logo is closed only because the intro
  reuses its UMediaPlayer), which led to a level-change hook instead of a Stop fix.
- `sceAvPlayerStop` traces inside its try block: a Stop that throws logs an error, not a trace line.
