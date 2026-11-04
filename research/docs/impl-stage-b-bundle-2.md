# Implemented: stage B bundle 2 (C5, D4, D5, #4, #6) (2026-11-04)

Status: built (Windows and Linux), not run. Branch `deck-lab`: `50b965d` (C5), `b7e947d` (D4), `0b32271` (D5),
`4158db0` (#4), `8dd5881` (#6). Nothing was skipped.
Inputs: [study-stage-b-per-draw.md](study-stage-b-per-draw.md) (C5),
[study-deck-rescore-rejected.md](study-deck-rescore-rejected.md) (D4, D5; spacepepperoni `a3f4c1d`, `95e295e`),
[study-deck-apu-deep-dive.md](study-deck-apu-deep-dive.md) (ranked actions 4 and 6, sections 1.7, 2.1, 2.5).

| # | Change | Where | Switch | Default |
|---|---|---|---|---|
| C5 | The buffer cache's `ResolveReadbacks` calls skip `readback_mutex` when the range is on no page of a pending readback copy | `readback_queue.h` (`Spans`), `texture_cache.cpp/.h` | **second toggle word 128** (set: always the lock, live) | on |
| D4 | Linux: pool commit / decommit make their `mmap` outside the region table lock | `src/runtime_memory.c` | **second toggle word 256** (set: under the lock again, live) | on (Linux); Windows unchanged |
| D5 | Linux: the region table rwlock is tried up to 1000 times (pause) before blocking; both: `flush_hooks` does not take the lock exclusively to find an empty queue | `src/runtime_memory.c` | **second toggle word 512** (set: as before, live) | on |
| #4 | Stage B spins up to 200 us before it blocks (packet ring, recorder sync, GPU ticks, present frame / fence) | `gpu/shim/gow3_stage_b_wait.h`, `vk_draw_pipe.h`, `vk_scheduler.cpp/.h`, `vk_semaphore.cpp`, `vk_presenter.cpp` | **`GOW3_STAGE_B_SPIN=1`** at start (opt-in), `GOW3_STAGE_B_SPIN_US` (default 200, max 2000); **second toggle word 1024** switches it off live | **off** (costs power) |
| #4 counter | Stage B's blocking waits by site, those over 0.2 ms, and the spin | `gow3_stage_b_wait.h`, `driver.cpp` | `GOW3_FRAME_STATS=1` | - |
| #6 | `alignas(64)` groups for cross-thread fields of `Scheduler` and `DrawPipe` | `gpu/shim/gow3_cacheline.h`, `vk_scheduler.h`, `vk_draw_pipe.h` | build: **`GOW3_CACHELINE_PADDING=OFF`** (env for `build.sh`, or the CMake option) | on |

Second toggle word, all bits now: 1 bulk descriptor copy, 2 sampler memo, 4 ring reuse, 8 direct arena writes, 16
semaphore poll interval (C1), 32 recorder lean mode, **64 reserved** (particle-sim skip), **128 C5, 256 D4, 512 D5,
1024 #4** (documented in `gpu/shim/gow3_toggles.h` `Gow3Toggle2` and the comment above
`runtime_disabled_optimizations` in `src/runtime_memory.c`). Every bit only switches its item off, as the others do.

New `GOW3_FRAME_STATS=1` lines (5 s block; added to `research/tools/deck_summary.py`'s extra lines):
- `Readback pre-check per frame (C5 on|off): without the lock N, locked N`
- `Region table per frame (D4 on|off|n/a, D5 on|off): pool maps outside the lock N (ms); lock busy (Linux): reads N
  (spin got it N, blocked ms), writes N (spin got it N, blocked ms); hook flushes without the lock N`
- `Stage B sleeps per frame (spin on 200 us|off|toggled off): over 0.2 ms N (packet ring, recorder, GPU waits,
  present); all N (ms); spins N (ms, N ended the wait)`

## C5: lock-free readback pre-check

`ObtainBuffer`, `ObtainBufferForImage` and the upload paths ask `TextureCache::ResolveReadbacks` 3-4 times a draw.
With readback copies always queued (UE4's HZB and exposure readbacks), each ask took `readback_mutex` and scanned the
queue (`obt_readbacks` 0.131 us a packet on the Deck). Now the pending copies' pages are also published as up to 4
merged ranges (`ReadbackQueue::Spans`, closest neighbours merged first) through a seqlock; those three callers return
"nothing to wait for" without the lock when their range overlaps none of them.

Why it is safe (also in the commit message):
- The spans are a superset of what `RequiredTick` matches (page-rounded like `OverlapsPage`; an empty copy still
  covers a byte; merging only adds pages; more than 64 copies publish "everything").
- They are republished under the mutex after every `Push` (the only change that adds pages), before
  `has_pending_readbacks` is set; `Retire` / `Cancel` republish only to tighten.
- Seqlock: odd seq while written, release fence, relaxed stores, even seq with release; the reader re-checks seq after
  an acquire fence. A torn or concurrent read sends the caller to the lock, never past it.
- Only the buffer cache's calls skip the lock: they run on the thread that owns the caches and queues the copies, or
  after a draw-pipe drain. The CPU fault paths (`CpuRead`, `CpuWrite`) keep the lock: a page is watched before its
  copy is pushed, so a fault can come before the spans include it. The stale-mode `ReleaseOutside` path is untouched.

Expected on the Deck (study): 0.08-0.1 us a draw, ~0.1-0.14 ms a frame at ~1,400 draws. The counter line shows how
many asks skip the lock; if "locked" stays high, the copies sit near the buffers and the spans need to be finer.

## D4: pool maps outside the table lock (Linux)

Each pool commit / decommit (~6,000 a second in MediEvil) made its `mmap` / `munmap` with the table lock held
exclusively; on Linux the locked table readers include the GPU threads' backing-view copies
(`CopySparseSerial` -> `runtime_memory_read_backing`), labels and page protection. Now: commit takes the pages
(locked), maps them (unlocked), enters the range and queues the map hook (locked); decommit enters the range as
reserved and queues the unmap hook (locked), replaces the pages with a reservation in one `mmap` (unlocked; was
`munmap` + `mmap`), then gives the pages back (locked).

Safety: the table never says mapped where the host is not (commit: reserved until entered; decommit: reserved before
the host view goes); L2's rule holds (the decommit's table edit bumps the region-cache buckets before the host
change); pages return to the pool only after their old view is gone (kept taken if the reservation `mmap` fails); a
new `pool_map_mutex` serializes pool commits and decommits among themselves as the table lock did (lock order:
`pool_map_mutex`, then the table lock). Non-pool kinds and Windows (shared host views table) keep the old path.

Expected (re-score): game side -0.1 to -0.5 ms, stage B ~0.05 ms directly; more if stage B's Linux backing-view
copies wait on the lock today (the "lock busy" reads on the new line answer that).

## D5: table lock spin (Linux) and the empty hook flush

1. `read_lock` / `write_lock` try the rwlock; when busy they retry up to 1000 times with a pause before the blocking
   call. Reentrancy is unchanged (glibc's reader-preferring rwlock grants a nested `tryrdlock`).
2. `flush_hooks` took the table lock exclusively once more per operation only to find the GPU hook queue empty. It
   now reads `pending_count` (stored atomically, still only under the lock) and returns. A caller that reads 0 had
   its hooks taken by another flush, which runs them; `direct_unmap`'s count restore stays under the lock.

The spacepepperoni commit's separate hook and pool spin locks were not taken: they change who may run a hook while an
operation still holds the table lock (ordering of map/unmap hooks), for a gain the re-score puts at ~0 for stage B;
`pool_map_mutex` (D4) covers the pool side. Expected: ~0 for stage B unless the new line shows blocked reads.

## #4: stage B never blocks for a short wait (opt-in)

On the Deck (amd-pstate active, default EPP) a block over ~0.2 ms puts a busy core on a sticky ~2.4 GHz plateau.
With `GOW3_STAGE_B_SPIN=1`, stage B (only; `Gow3Sections::recording_thread`) spins with pauses up to
`GOW3_STAGE_B_SPIN_US` before it blocks in:
- the packet ring (`DrawPipe::Run`): the existing spin (50 us at <= 8 threads) lasts the longer of the two;
- `Scheduler::SyncRecording` (submissions, the flip): polls `queued_chunks` and a new hint `recorder_running`; the
  check under the recorder mutex still decides;
- `Semaphore::Wait` (frames-ahead wait in `flip_present`, readback resolves, `Finish`): asks the timeline semaphore
  every 20 us (an ioctl on RADV);
- `Presenter::GetRenderFrame` (`flip_present`): a free presentation frame (every 2 us under `free_mutex`) and its
  present fence (`vkGetFenceStatus` every 20 us).
Each spin is only a delay before the unchanged wait. Not covered: `WaitDeferredSignals` / `WaitHostCopies` /
`Gow3Copy::WaitAsync` already yield-spin (no sleep).

Expected (deep dive): 0-15% of stage B if the plateau is real (check with `deck.sh clocks`); small if the
`performance` governor ships. Cost: up to 200 us of a busy core per wait that turns out long (between frames when
stage A waits on the game, GPU-bound frames-ahead waits): watch FPS against the GPU clock and power.
The counter runs with the spin on or off, so the baseline already shows how many sleeps over 0.2 ms stage B takes.

## #6: cache-line groups

`GOW3_CACHE_LINE` (`alignas(64)`) starts each group, no field moved: in `Scheduler`, `pending_polls` /
`num_pending_ops` (read every draw) apart from the pending-ops mutexes and the priority queue; the recording thread's
`record_chunk` / `chunk_lock` / `recording_owner`; `wake_deadline_ns`; the recorder's `recorder_mutex` group (queue,
`recorder_busy`, `queued_chunks`, `recorder_running`, `recorder_sleeping`); `direct_mode` / `host_copies_issued`
(written by stage B at every Kick: they shared a line with the recorder's per-chunk writes); `host_copies_done` and
`host_copy_sources_mutex`. In `DrawPipe`: `busy_cycles` (stage B per packet) shared a line with `packets` (stage A per
Commit); `handler` / `ring` and `head` now each start a line. Expected 0.05-0.2 us a draw. Not done: per-thread
`Gow3Stats` counters (measurement only, ~64 sites). `GOW3_CACHELINE_PADDING=OFF` gives the layout before #6 exactly.

## Build and test

- Windows: `research/tools/build-clang64.sh` (LTO) clean at `8dd5881` (only the existing `gow3_overlay.cpp:546`
  format warning).
- Linux: WSL Debian `/root/src/medievil` fast-forwarded to `8dd5881`, `CC=clang CXX=clang++ bash build.sh` (static GPU,
  LTO) + `packaging/linux/appimage.sh`: **`dist/MediEvil-x86_64.AppImage`** (131,222,008 bytes, sha256 `b814acee...`);
  `--help` works. Also **`dist/MediEvil-x86_64-nopad.AppImage`** (same commit, `GOW3_CACHELINE_PADDING=OFF`,
  131,209,720 bytes, sha256 `06cd158f...`) for #6. The WSL `out/` tree was built last with padding OFF; `build.sh`
  passes the option every time, so the next default build reconfigures it ON.
- `readback-queue-test` (new `Spans` cases), `draw-classes-test`: pass on both; `pipeline-binary-cache-test` passes
  (WSL). `runtime_memory.c` compiles with `-Wall -Wextra -Werror` on both.
- Unit tests (`py -3.14 -m unittest discover -s tests`, from Git Bash): 127 run, the 8 known failures (2 failures,
  6 errors). From PowerShell `test_mods ... cleans_view` also fails (`bash run.sh` resolves to WSL's bash there);
  no Python or script changed.
- Not run in game.

## The Deck test (one launch, the user's go)

Settings as the previous Deck runs (720p, 67%, Low, graveyard heavy view, stand still), plus:
`deck.sh deploy`; `deck.sh env "bundle 2 A/B" GOW3_FRAME_STATS=1 GOW3_STAGE_B_SPIN=1` (carry over any keys the
current runs use; `deck.sh env` replaces the file). Start with `deck.sh toggle "0 1024"`: everything on except the
#4 spin, i.e. this build's defaults. If the current runs keep other bits set in either word, add them to every word
below.

In the same launch, one pair at a time (each `ab` alternates B, A, ... and ends on A; 20 s x 4 cycles = ~3 min):

| Item | Command | A (default) | B (item switched) |
|---|---|---|---|
| #4 spin | `deck.sh ab "0 1024" "0 0" 20 4` | spin off | spin on (200 us) |
| C5 | `deck.sh ab "0 1024" "0 1152" 20 4` | pre-check on | always the lock |
| D4 | `deck.sh ab "0 1024" "0 1280" 20 4` | maps outside the lock | under the lock |
| D5 | `deck.sh ab "0 1024" "0 1536" 20 4` | spin + empty-flush skip | plain lock |

Optionally run `deck.sh clocks 20` in a second terminal once in each state of the #4 pair (`gow3:DrawRec`'s clock
with the spin on vs off: the point of #4). Then `deck.sh pull bundle2-ab` and `deck.sh summary bundle2-ab --min-draws 1000`: per toggle state, FPS, stage B
busy, us per draw, and the new lines. Drop windows that straddle a switch (the summary marks them mixed).

What to read:
- **#4**: "Stage B sleeps per frame": over 0.2 ms should fall to ~0 with the spin on (the spins "ended the wait"
  count is the waits it caught); FPS / stage B busy / stage B clock vs the cost (spin ms per frame). Keep it only if
  FPS rises; otherwise leave it off (and try after the `performance` governor test, where it should matter less).
- **C5**: "Readback pre-check": with A most asks go without the lock; stage B us per draw should drop ~0.1 us.
- **D4 / D5**: "Region table per frame": pool maps outside the lock (A, D4) / 0 (B); lock busy reads and blocked ms
  (expect fewer with A); stage B busy, and "Guest event waits" / the game thread's frame time (the game side).
- Nothing else may get worse in A: fence lag, guest event waits, frame pacing p99.

#6 needs two builds (layout): one launch with `MediEvil-x86_64-nopad.AppImage`
(`deck.sh deploy /root/src/medievil MediEvil-x86_64-nopad.AppImage`) and one with the default, same spot and toggles
`"0 1024"`; compare stage B us per draw (expect -0.05 to -0.2 us with padding). One launch each is within noise for
this size: call it no difference unless the 5 s windows separate, or ask for an A-B-A.

Pass: each default-on item (C5, D4, D5, #6) at least not worse in FPS / stage B busy / pacing; #4 kept off unless it
wins clearly.

## Review (2026-11-04)

An independent read of C5, D4 and D5 found no real race (C5's seqlock and its callers, D4's lock order and backing
views, D5's empty-queue check and spin all hold). Two low D4 edge cases were fixed in `1b3c28b`: the locked pool
paths now take `pool_map_mutex` too (a flip of bit 256 between two calls could otherwise let a locked commit lose its
mapping to an unlocked decommit's reservation), and a failed decommit table edit keeps the reservation mmap under the
table lock. Older and unchanged by these commits: `flush_hooks` returns before hooks another thread dequeued have run,
so a decommit-then-recommit of one range can run MAP while UNMAP is still running elsewhere.
