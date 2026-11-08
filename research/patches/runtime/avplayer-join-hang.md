---
id: avplayer-join-hang
title: Fix the game hanging after the first movie (AvPlayer thread joins, upstream PR #68)
area: runtime
status: active
platforms: all
default_windows: on
default_deck: on
origin: upstream PR #68 (dvilelaf 42b2afb)
class: G
commits: b182a97325
files: gpu/shim/core/libraries/kernel/threads.h
symbols: Thread, Join, Stop, Joinable, Run
switches:
tests:
docs: study-forks-survey.md#4-dvilelaf-prs-68-69-70, study-forks-survey.md#9-worth-porting-shortlist, plan-linux-deck.md#54-linux-and-4-core-items-not-uma-likely-bigger-on-the-deck
depends_on:
related: movie-suspend, avplayer-trace
checked: 2026-11-07
---
## What it does

Only on branch `fork-ports` (from `lab` `63b0c76`): not in `lab` or `deck-lab` yet, so their `threads.h` is still
the old one. The write-up is `research/docs/impl-fork-ports.md` §1 on that branch. The game stops each movie with
`sceAvPlayerStop`, which joins AvPlayer's demuxer and decoder threads. Those threads also joined themselves when they
finished, and a self-join detached them. A join on a thread that has just detached hangs on Linux (black screen,
sound goes on) and fails on Windows ("HLE thread: join failed"). The new `Kernel::Thread`: a self-join returns at
once, one joiner at a time (`join_mutex`), `Stop()` requests the stop through a saved `stop_source` before it
queues for `join_mutex` (so the request lands even while another joiner holds it), and only a thread destroying its
own object detaches.

## Invariants

- The self check in `Join()` reads the atomic `id` before taking `join_mutex`: the owner may hold the lock while it
  joins this very thread. Moving the check under the lock deadlocks.
- No thread detaches while another may join it. The only `detach()` calls left are in `~Thread` (a thread destroying
  its own object) and after a failed join, once its function has returned or 5 s have passed.
- `Joinable()` means "started and its function has not returned" (`finished`). It never waits for a joiner.
  `AvPlayerSource::Stop`'s "already stopped" path depends on a finished thread counting as stopped.
- `Stop()` calls `stop_source.request_stop()` before `Join()`. The demuxer may be joining a decoder that only ends
  once it is asked to stop.
- `Run()` first calls `Stop()` (joins a previous thread), then starts the new one and saves its `stop_source` and
  `id` under `join_mutex`. `Stop()` reads `stop_source` and `Joinable()` reads `finished` without the lock, so
  `Run()` must never overlap another thread's `Stop()` / `Joinable()` on the same object. For the source's three
  threads `AvPlayerSource::Start` and `Stop` both hold `m_state_mutex`, and the threads' own self-joins call only
  `Join()`.
- Paused decoders (movie-suspend) wait on stop-aware condition variables, so a stop request still ends them.

## Interactions

- movie-suspend (`f0ced8c`, level-change pause): it only pauses the source. It was checked against the new join
  rules and the paused threads still end on Stop (impl-fork-ports.md §1.3).
- avplayer-trace (`6fd8e15`): log lines only, no thread logic.
- Deck plan item L5 (plan-linux-deck.md §5.4). This is not stage A's drain patch, which is also called L5.
- spacepepperoni's `0ddcabc` fixes the same race but keeps the self-detach and reads `joinable()` / `get_id()`
  outside the lock (a data race). Do not take both.

## Evidence

- Offline (impl-fork-ports.md §1.4): a standalone test of the AvPlayer pattern under CLANG64 / libc++, 400 cycles.
  With `lab`'s header it printed 905 `HLE thread: join failed` lines; with this one, none. It shows the race and its
  fix in the header alone, not the game's own timings.
- Upstream PR #68: seven Linux starts in a row on llvmpipe and several on RADV got past the intro [msg].
- Built on Windows (2026-11-05). **Not run in game anywhere.** Not verified in game.
- Our native Deck runs got past the logo movie without the fix (`data/deck-native-2.log`). The hang depends on
  timing, so those runs do not show the bug is absent.

## History and pitfalls

- PC logs show `HLE thread: join failed (... No such device or address)`, then a wait of up to 5 s: one at the
  logo's end (impl-fork-ports.md §1.1), and in `research/docs/data` 260 of 337 logs carry 1-7 such lines (the rest
  are Deck runs, excerpts and runs that never got through the logo movie). It should be gone with this patch: that line is the cheap check on Windows.
- No switch: the old path is the race itself. The A/B is a build without `b182a97`.
- Risk: a join that now waits where the old code detached. Before merging, run regression items R1, R4, R5 and R8
  of impl-fork-ports.md §4: logo, intro, level ends, world map, restart, a second start, five Deck level ends.
- The intro's player is never closed by the game, so its finished threads stay un-joined until exit. That is as
  harmless as the old detach was.
