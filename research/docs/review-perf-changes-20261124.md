# Review: performance changes P1, P2, P4, P5, P6 (2026-11-24)

Status: static review only (no build, no launch). Branch `perf-hygiene`. Parent: [START-HERE.md](START-HERE.md).
Background: [study-perf-deepdive-20261123.md](study-perf-deepdive-20261123.md), [plan-work-queue.md](plan-work-queue.md)
rows P1-P17, card `research/patches/tooling/dev-tools-build.md`. Every diff was read against the code around it,
not against the commit messages. Run logs read: `user/logs/runs/run-20261124-*.log`.

Ratings: **confirmed bug** (the code does it, the scenario follows), **plausible** (the code allows it, not shown in a
run), **nit** (cost or tidiness, no wrong behaviour). [V] seen in code or log, [I] inferred.

## Findings, most severe first

### 1. P2: a tracked range is never released, and every later readback that starts at its address goes to the shadow (plausible, high impact if hit)

`texture_cache.cpp:398-411` (`FindTracked`, `WriteShadow`), `:552-560` (`shadowed`), `:590-591` (`Push(..., !shadowed)`),
`:291-294` (retire), `texture_cache.h:636` (4 slots), `SnapshotReadback` `:429-434` (a slot is taken, never freed);
the hook's `testers` (`medievil_hzb.cpp:90`) are never freed either.

Before P2 a tracked slot only numbered copies; every copy still landed in guest memory. Since P2 a copy whose start
address equals a tracked slot's address is (a) not page-watched (no first-copy wait, no CPU-read wait:
`RequiredTick` and `Spans` skip unwatched entries) and (b) written only into the slot's shadow, never into guest
memory. Nothing checks that the copy is still the HZB results image (size, format) or that the hook still uses the
slot. Syncs happen at loads (dev runs 11-24 01:13 / 01:58: 4 and 7 syncs, about one per load) [V], so the hook does
meet new testers or addresses over a session.

Failure scenario: a level load frees the old view's 256 KB HZB staging texture; the allocator reuses the address for
another CPU-read target (the sky light's 1x1 average brightness is the known one: card
`caches/readback-first-wait`). That copy goes to the shadow, guest memory keeps old bytes, and the first-copy wait
is skipped: the Hall of Heroes teal / white reflection bug class returns, only on some loads. A copy at the address
that is larger than the slot is also truncated (`std::min(size, t->size)`, `:409`): the tail never reaches guest
memory.

Fix proposal: shadow only when `copy->size == t->size` and the image matches the results image (256x256, 32 bpp);
free a slot (and its shadow) when the hook re-syncs a tester to another address, when the image at that address is
destroyed / `CancelReadbacks` hits it, or when `SnapshotReadback` has not been called for the slot for N frames.
Slot exhaustion (5th distinct address) is pre-existing: `Sync` then fails every frame and the game's own
`MapResults` waits for stage B every frame; worth the same release mechanism.

### 2. P1: under mouse motion the reader drains about every 4 ms instead of about every 1.5 ms (plausible, input feel)

`src/runtime_mouse.c:137-151`. The card's own run (mouse-input.md, "Run 2026-11-24 00:12"): fast movement 1,495
reports/s -> 245 wake-ups/s, ~6 reports a drain [V]; the old loop drained every ~1.5 ms (664/s). So the wait does
not return for each new report: `GetQueueStatus` right before `MsgWaitForMultipleObjectsEx(..., 0)` marks input that
already arrived as seen, so the wait blocks until the next new arrival is signalled [I mechanism, V rate]. Idle is
fixed (the goal), but in motion the reader now hands the game ~4 ms lumps. At the 237 cap (4.2 ms frames) the two
periods beat: some frames get no mouse delta, the next gets two lumps; the camera turn steps unevenly and lags
~2 ms more on average. Frame times do not show it. The P2 run's "small spike the first time the camera turns ...
only with fast mouse turns" may be fed by the bigger per-frame camera jumps (more new HZB keys per frame) [I].

No lost wake-up found: a report that arrives after the drain is still in the queue at the next `GetQueueStatus`
(HIWORD = present now), and WM_QUIT is a posted message (QS_POSTMESSAGE). Fix proposal: after a drain that found
reports, keep the 1 ms timer cadence and skip the message wait; block only after a drain that found nothing. Check:
reports per batch in the `Mouse:` line (want ~1-2 in fast movement), the 8000 Hz A/B on the card.

### 3. The DLSS report: nothing in P4 / P5 adds DLSS-only work; the DLSS part of each session carried the one-off costs (plausible explanation, pre-existing)

What the code and logs show [V]:
- P4 is the only DLSS-only change and it removes work: no `IsDisplayBuffer` per draw, no binding walk for
  compute dispatches that write no image. TAAU detection is unchanged: every run before and after P4 logs
  `UE4 motion: TAAU compute shader cs a080f599034de78e (samples velocity, writes one ...)` once, and DLSS runs
  (`DLSS context ... (TAAU)`). The bit-3 diagnostic is skipped in release only.
- In every P4+P5 session (09:15, 19:47) and every P2-only session (20:02, 20:05, 20:11), DLSS was on from the main
  menu through the Graveyard load, and FSR was switched in later with F9 (09:15 at View frame 2788, 20:11 at
  20681). The DLSS stretch is the cold one: the intro movie, first level load, first texture streaming and first
  views of the Graveyard (the off-cost A/B card already lists "a small series at the Graveyard's first long view" as
  the known release-build spike), and the HZB remap serves new keys visible for 3-4 frames after every camera cut
  (the intro is a run of cuts). FSR was judged warm, mostly in areas already seen.
- DLSS creates its context twice in that window, synchronously on stage B: `vk_ue4_upscaler.cpp:497-509`
  (`scheduler.WaitSubmitted()` then NGX `CreateFeature`): main menu `2859x1620 -> 3812x2160`, then the level
  `2880x1620 -> 3840x2160`. One feature is cached (`vk_dlss.cpp:198-215`), so any size change re-creates it (20:21:
  `2842x1599` then `2880x1620` 79 frames apart). FSR created one context at the F9 switch.
- At the 237 cap frames are GPU-bound (~4.0 of 4.4 ms waiting for the GPU, deep dive). If DLSS's evaluate costs
  more GPU time than FSR 3 at 2880x1620 -> 4K (likely with the transformer model), heavy views drop below the cap
  with DLSS and not with FSR: frame-time instability without any code change [I].
- P6 is not in the exe the report came from (P6 built 20:43; the runs used the 09:06 exe), so per-character log
  writes were still there; they hit DLSS and FSR alike (save menu, loads).

Not found: any periodic DLSS-only stall in the code touched by these commits. To settle it (one change per launch):
FSR from the start through the intro and the first Graveyard view, then DLSS from the start, same route, flip trace
on; optionally `GOW3_GPU_PROFILE=1` in the dev exe at the Graveyard long view for DLSS vs FSR GPU time.

### 4. P5 (8/n, P3): the early return in `ProcessDownloadImages` rarely fires in play (nit: no wrong behaviour, gain probably near zero)

`texture_cache.cpp:512-515` returns only when `download_pending` and `has_pending_readbacks` are both clear.
`has_pending_readbacks` is `pending_readbacks.Bytes() != 0` (`:313`) and counts every queued copy, P2's unwatched
HZB copies included. The HZB copy is queued every frame and retires 3-4 frames later (age 3-4 in every remap line),
plus ~5 tiny readbacks a frame, so in play some copy is nearly always queued and the full path (two mutexes) runs as
before [I]. The return helps in menus and loads. Correctness is fine: both flags are set under their mutex by the same
thread (stage B: `FindTexture` / `FindRenderTarget` and the end of pipe; the stage-A EOS path drains stage B first),
every `download_images.emplace` sets the flag (`:2005`, `:2046`), both `clear()` sites clear it. The queue row's
"~10-30 us a frame" should not be claimed without a measurement.

### 5. Missed by P5 (in P14): the warning-only `IsMapped` in `UpdatePageWatchers` (nit, off-cost and a log line in play)

`page_manager.cpp:194-199`: every watch / unwatch calls `rasterizer->IsMapped` (recursive lock and a heap allocation,
deep dive memory item 5) only to log a warning. The warning prints during play in release (19:47, 20:02, 20:11 logs:
"Tracking memory region ... which is not fully GPU mapped", up to 6 a session). P2 removed the HZB's two calls a frame;
the rest remain. Compile it out of release or move it behind `GOW3_DEV_TOOLS`.

### 6. P2: copies that lose their number on a restart are written to guest memory unwatched, while the shadow stays the snapshot source (nit)

`texture_cache.cpp:291-294` with `SnapshotReadback`'s restart (`:437-440`, `ClearSeq`): a queued shadowed copy whose
`seq` was cleared retires through `TryWriteBacking` although its pages were never watched, and `SnapshotReadback`
keeps serving the older shadow bytes (`shadow.size() >= n`). Harmless today: after a restart `published` is 0, so the
hook uses no source until the next numbered copy overwrites the shadow. Simpler: always shadow a copy of a tracked
address (seq or not), so guest memory is never written for it.

### 7. P4: work left in the observer (nit)

- `vk_rasterizer.cpp:5451-5454`: `written_desc.emplace(tsharp, desc)` (an `ImageDesc` construction) runs for every
  image-writing compute dispatch while an upscaler is on, before `taau` is known; only the TAAU dispatch uses it.
  Build it after the `taau` decision.
- `Ue4FrameMap::Pass` (~0.5 KB, zero-initialised) is built for every draw and dispatch; `HookTimer` calls the
  out-of-line `Ue4Upscaler::StatsClock` (`vk_ue4_upscaler.cpp:225`; folds only if LTO inlines it).
- Both upscalers: `Ue4Upscaler::Run` creates two `VkImageView`s a frame and defers their destruction
  (`vk_ue4_upscaler.cpp:478-489`); cache them per image id / uid.

### 8. P6: printf wrappers (nit; formats and varargs are correct)

`src/stdio_lines_win.c`. Checked: each family formats with its own formatter (UCRT `vsnprintf` with C99 return
semantics under `_UCRT`; `__mingw_vsnprintf`), so `%lld` / `%zu` / `%Lf` behave as before; `va_copy` is taken before
the first use and ended on every path; non-console streams go to the real `vfprintf`; the `--wrap` list covers all
eight entry points. Nits: lines over 2047 bytes are formatted twice and `malloc`ed (a crash line from a handler with a
damaged heap could fail, `:39`); the 2 KB stack buffer adds to a handler's stack use on a stack-overflow fault;
`__real_printf` / `__real_fprintf` / `__real_vprintf` are declared and unused.

### 9. Other release off-costs of the same kind still in the exe (nit)

- Shader compile path: `PhaseTimer` / `CompileTimer` clock reads and atomic adds, `g_gow3_pending_draws` per dropped
  draw (`vk_pipeline_cache.cpp:534-553, 567-620, 853, 896, 1394`): read only by the dev `Flip` stats. Only while
  compiling, so small.
- Runtime C first toggle word: `runtime_memory.c:1328, 1885` still do relaxed atomic loads of
  `runtime_disabled_optimizations` per call (LTO does not fold atomic loads; the card says they fold). A `mov` each.
- `LOG_WARNING` / `LOG_ERROR` are now written with no level check in release: `GOW3_GPU_LOG=error` can no longer
  silence warnings in release (the load-time `SanitizeCopyLayers` burst, 11 lines). Intended; noted for the card.

## Checked and found correct

- **P5 1/n `a1bb455`** (`gpu/shim/common/logging/log.h`): release `LOG_TRACE/DEBUG/INFO` in `if (false)`: arguments
  type-checked, never evaluated, which matches the old default level (warn), so no side effect moved. Nothing in the
  repo sets `GOW3_GPU_LOG`. `LOG_GENERIC_ON` without `do/while` is still a single expression statement.
- **2/n `7d7b848`**: `gpu_frames` is read only by `NoteHotUse` (GOW3_BUFFER_STATS, dev); `NoteFrameStart` has nothing
  functional after the profiler call. `Drain`'s release path keeps the same wait (hybrid or yield). Fence kind:
  `BeginFence` returns `~0` before consuming the TLS value, consistent with `NoteFenceKind`. `lock_armed_at` is only
  written with stats on (`gnmdriver.cpp:2436-2439`). `direct_recordings` and the staging byte counters have no
  other reader.
- **3/n `110be1b`**: the `Flip` frame-stats block (`driver.cpp:403-1165`) holds only stats and their resets; the
  counters it reset (`g_gow3_*`, `Gow3Stats::*`) are not used for decisions. `chunks` (`buffer_cache.cpp:557`) is
  only the log line's number. Marker labels: `ScopeMarkerBegin` still drains the pipe with an empty label.
  `scene_debug_frame` / `SceneTargets::debug` default false.
- **4/n `21f7fe2`**: `fault_log_enabled` constexpr false folds the scopes, `CopyFaultTrace` and the
  `FramesEnded` read (`buffer_cache.cpp:830`).
- **5/n `00358ca`**: `mouse_read` in release counts to 3 and stops, `n` is used only for the log; the gated
  `camera_look` summary resets only counters; pad record / replay and `read_inject`'s "replay" token are test tools;
  `runtime_toggle2()` is 0 only where nothing writes the second word (watcher dev only).
- **6/n `7c92669`**: `optimistic_spent` starts the 100 us budget at the first seen writer; the fallback to the locked
  lookup is unchanged. `PollChoiceInput`: `choice_active` is atomic; the early return is exactly the old "not active
  and not open" path. Load-before-exchange for the setres poll, cvar flag, screenshot and open requests is correct
  (the exchange still decides).
- **7/n `1904c41`**: the summary is quiet unless `GOW3_FPS_LOG=1`; a guard switch still prints.
- **P2's synchronisation**: `tracked_readbacks` and every shadow access are under `readback_mutex` (retire,
  `ProcessDownloadImages`, `SnapshotReadback`); `CanFit` fallback and queued path number and publish alike.
