# What the diagnostics cost when they are off (inventory, 2026-11-16, no launch)

Parent: [plan-work-queue.md](plan-work-queue.md) "Logging and trace code with a cost when off". Read-only
inventory by three research agents (runtime `src/`; GPU frame-path stats; capture / dump / debug), spot-checked
against the source and the binary (`out/medievil-runtime.exe` built 2026-11-16 06:57, `llvm-nm` / `llvm-objdump`).
Costs are estimates from instruction counts, **not measured**. Tags: **[B]** seen in the binary, **[S]** read in the
source, **[E]** estimate.

## The user's decisions (2026-11-16)

- A compile-time option `GOW3_DEV_TOOLS` gives a release build with the tooling compiled out. Until now release and
  dev were the same build: the user plays whatever the Start-menu icon points at.
- `GOW3_FPS_LOG` defaults **off**; the variable turns it on (the launcher sets it to 1 today).
- Shift+F12 frame dump stays in release (the launcher's bug-report capture); the F11 frame analyzer is dev only.
- Order: step 1 (the unconditional counters, no build option) first, one A/B launch against a copy of today's exe;
  then step 2 (`GOW3_DEV_TOOLS`), then step 3 (launcher). Small fixes any time.

## Main findings

1. **The switched-off checks are cheap.** Both the GPU library and the runtime are ThinLTO (`-flto=thin`,
   `gpu/CMakeLists.txt:27-31` sets IPO before `medievil-runtime` is defined at `:244`) **[B]**: the wait-site check
   is one inlined load and branch in all 15 exports (0 calls to `runtime_wait_sites_enabled`). Without LTO
   (`GOW3_LTO=OFF`, `out/gpu-pdb`; Linux `GOW3_STATIC_GPU=OFF`) these become real calls.
2. **The cost is unconditional work, not gated diagnostics.** Rough total on the GPU side ~100-300 us a frame across
   stage A and B, 2-6% of stage B **[E]**, almost all from the rows marked step 1 below.
3. **Shared cache lines** (placement accidental: the linker orders the globals by name, so any rename moves them):
   - `Gow3Stats::enabled` (0x1433baf20) shares a 64-byte line with `draws` (baf18), `constant_ring_reused_bytes`
     (baf08) and `hot_pages` (baf28) **[B]**: stage A writes the line every draw, stage B reads `enabled` many times
     a draw.
   - The runtime's mutex counters share lines with the memory-region table **[B]**: `unlocks` (0x1435dd718) with
     `vma_capacity`, `vma_count`, `vma_seq` (dd728-738); `waits`, `wakes`, `wakeups` (dd750-760) with
     `vma_seq_depth`, `vmas` (dd740, dd748). The GPU threads read `vma_seq` / `vma_count` / `vmas` on every lock-free
     region lookup (`runtime_memory.c:1990-2015, 2050-2070, 2177`); every guest unlock / wait / signal can
     invalidate them.

## Runtime (`src/`)

| Item | Hook | Runs when off | How often | Step |
|---|---|---|---|---|
| `memory_calls` | `runtime.c:165,170,175,180` | `lock incq` **[B]** | every guest memcpy / memset / memmove / memcmp, all threads | 1 |
| mutex / cond counters `created, locks, unlocks, conds, waits, wakeups` | `runtime_mutex.c:15,157`, `++` at `:64,87,101,108,150,166,311,325,340,347` | plain `incq` (racy) on lines shared with the region table | every guest lock / unlock / wait / signal | 1 |
| VirtualQuery counter `queries` | `runtime_memory.c:906,922` | locked add under the read lock | every query-protection / `sceKernelVirtualQuery` | 1 |
| getenv per call | `runtime_sema.c:75` (`GOW3_TRACE_SEMA`, **inside the global semaphore lock**); `runtime_file.c:258` (`GOW3_MOD_TRACE`, `GOW3_MODS_DIR`) | CRT environment lookup | every blocking semaphore wait; every file open | small fix |
| always-on log lines (stdout unbuffered, `probe.c:1330`) | `runtime_file.c:221,226,233` (missing files); `runtime_sema.c:59` (semaphore create, under the global lock); save trace (`runtime_savedata.c:210-354`, `runtime_services.c`); `runtime_thread.c:314` | a write call each | per event | keep save trace (user support); rest later |
| pool counters commented "GOW3_FRAME_STATS" but not gated | `runtime_memory.c:1196,1219,1302,1308` | locked add next to a ~60 us operation | per pool commit / decommit | 1 (cheap); keep the `pool_owner[]` stores (functional) |
| wait-sites / wait-trace | `runtime.h:147-150` | inlined load + branch | every guest wait | 2 |
| host-map stats, `GOW3_FRAME_STATS` gates in src, toggle words | `runtime_memory.c:433, 2227`; `runtime_extra.c:182`; toggle words `:1682` | load + branch | per region lookup, per thread-id call | 2 (toggle words become constant 0) |
| mouse read stats | `runtime_services.c:667` | seq_cst add | once a frame, mouse on | 2 |

Keep: `runtime_extra.c:805` `reads` (setres timing logic); `GOW3_CRASH_DUMP` (user crash feature). No diagnostic
thread starts in `src/` when off. Exit report (`runtime.c:70-87`) prints the counters only at guest exit / STOP.

## GPU frame path

| Item | Hook | Runs when off | How often | Step |
|---|---|---|---|---|
| `Gow3Stats` counters bumped unconditionally (~25: `draws`, `dispatches`, `constant_ring_bytes`, `constant_ring_reused_bytes`, `small_*`, `buffer_upload_bytes`, `render_pass_ends`, `stream_buffer_bytes`, ...) | `vk_rasterizer.cpp:1122,1552,652`; `vk_constant_ring.h:68`; `buffer_cache.cpp:1279-1335,1502,1514,1573-1579,1644-1659,1925-1928,1953`; `buffer.cpp:251-252`; `vk_staging_buffer_pool.cpp:62`; `vk_scheduler.cpp:192,446`; `texture_cache.cpp:1821` | `lock` RMW **[B]**; gow3_toggles.h:308 says "collected only with GOW3_FRAME_STATS=1" | ~3-6 a draw (stage A), ~2-4 a draw (stage B) | 1 |
| draw-pipe packet timing | `vk_draw_pipe.h:364-368` | 2x `__rdtsc` + 2 atomic adds **[S]** | every packet, stage B (~35-45 us a frame [E]) | 1 (keep for slow frames when stats on) |
| `Gow3Stats::WaitTimer` | `gow3_toggles.h:351-358`; `vk_scheduler.cpp:379,474,496,671,855` | 2 clock reads + locked add | tens a frame | 1 (LongWaits still needs `timer.start`) |
| stage A idle accounting | `liverpool.cpp:180-187` | 2 clock reads + 2 locked adds | per stage A wake | 1 |
| `GOW3_SECTIONS` scopes (55) | `gow3_sections.h:107-125` | guard + load + compare per scope | ~30-50 a draw, stage B (15-50 us [E]) | 2; keep `recording_thread` (functional) |
| draw classes | `vk_rasterizer.cpp:1168,1448` | out-of-line `~Scope` call per draw **[B]** | per draw | 2 (inline empty version) |
| perf-diag (`vk_wait_diagnostics.h:44-50`; `std::source_location` args, `first_image_desc.clear()` per flush `vk_runtime.cpp:1103`), upscaler stats (`vk_rasterizer.cpp:4661-4667`), texcache stats (`texture_cache.cpp:94-104`; one `steady_clock::now` per image create `:1763`), profiler `Get()` null checks, fence-stats residue (`liverpool.cpp:531,555,741,778` TLS stores; `gnmdriver.cpp:86` locked exchange), stage-b counters residue (`vk_rasterizer.cpp:2665,3247,3288`; `vk_draw_prep.cpp:505-511`), flip trace `Note`, FPS log (`gow3_overlay.cpp:3301-3326`), frame-stats lines (`driver.cpp:400-401`) | guard + branch, a few small stores | per draw / pass / flip | 2 |
| `LOG_*` | `log.h:33-45` | `ShouldLog` not inlined (794 calls **[B]**) | tens a frame | 2 (compile-time level) |
| copy-fault trace (`GOW3_COPY_FAULT_LOG`, **default on**) | `gow3_copy.cpp:242-245`, `page_manager.cpp:63-70` | on: ~6 TLS ops per guest copy | per guest copy | 2 (release default off) |

`GOW3_FRAME_STATS=0` half-enables stats: six sites test `getenv(...) != nullptr` (`vk_rasterizer.cpp:500,996,1897`,
`vk_draw_prep.cpp:508`, `vk_occlusion.cpp:258`, `texture_cache.cpp:2532`); the launcher only sets `1`. Small fix.
Tracy / MicroProfile: already compiled out (`gpu/shim/tracy/*.hpp`, `TRACY_GPU_ENABLED 0`).

Functional, keep in release: `Gow3Sections::recording_thread`, `download_reason` / `DownloadReasonScope`,
`Semaphore::Refresh` `refreshed_ns`, `last_game_flip`, `GpuOrderedWait::start`, `hot_since`, swapchain present ids
(Reflex), a stub for `gow3gpu_flip_trace_write`.

## Capture, dump, debug

| Item | Runs when off | Step |
|---|---|---|
| `FrameCapture::Active()` atomic loads, ~10-15 per draw (`vk_frame_capture.h:33-35`; `vk_rasterizer.cpp` 1394, 1783, 2065, ...) | load + branch, stage B (3-8 us a frame [E]) | 2; **keep `IsDisplayBuffer` / `AddDisplayBuffer` / `display_list`** (frame-start detection, upscaler) |
| Launcher always sets `GOW3_FRAME_DUMP_TRIGGER`, `GOW3_CAPTURE_TRIGGER`, `GOW3_TOGGLE_FILE` (`medievil_launcher.py:621-627`) **[S]** | a failing `remove()` every 250 ms on the draw path (`vk_presenter.cpp:507-515`); `filesystem::exists` every 250 ms on the present thread (`vk_frame_capture.cpp:164-176`); toggle thread `fopen` every 250 ms | 2: F12 / Shift+F12 through the key handler's atomic, no file polls |
| Vulkan object naming, no switch (`vk_platform.h:34-50`, 41 sites; extension always requested `vk_platform.cpp:297`) | `fmt::vformat` + driver call per image / view / buffer / pipeline created | 2 (load / streaming hitches, not per frame) |
| Debug-marker labels formatted before the gate (`vk_rasterizer.cpp:416,3940,3965`) | a string per op | small fix |
| Guest write log ring `std::array<Entry, 1<<20>` (`gow3_write_log.cpp:25`) **[S]** | 40 MiB BSS, untouched | 2 |
| `FrameCapture::state` beside `flips` / `last_presented` (written per flip) | possible one miss a frame | 2 |
| Crash-report recorders (`vk_runtime.cpp:1092-1094`, `image.cpp:110-118`) | TLS stores per barrier batch; ring per image create | **keep** (user crash reports) |
| AvPlayer trace map (`avplayer.cpp:60-90`) | mutex + map per movie frame | **keep** the map (movie suspend uses it); printfs could go |
| F12 PNG screenshot (`TakeScreenshotRequest`) | one atomic exchange per present | **keep** (user-facing) |
| RenderDoc API, Tracy | nothing | already out |

## Plan

1. **Step 1, the unconditional counters (no build option).** Run 2026-11-16 (`fa136ee`, card
   `tooling/stats-counters-gated`): under load frames > 6 ms 18.5 -> 3.6 a minute, 0.1% low 133.9 -> 152.8 FPS;
   unchanged at the cap. GPU: every `Gow3Stats` counter, the draw-pipe
   `rdtsc` timing, `WaitTimer` and stage A idle accounting only when `Gow3Stats::enabled`; the read-mostly switches
   on their own cache lines. Runtime: `memory_calls`, the mutex / cond counters, `queries` and the ungated pool
   counters only with frame stats on; the exit report leaves those lines out otherwise. A/B: a copy of today's exe
   against the new one, same settings, judged by lows (one change).
2. **Step 2, `GOW3_DEV_TOOLS`.** Fix built 2026-11-16 (card `tooling/dev-tools-build`; the user: flip trace kept in
   release, a separate dev exe chosen in the launcher, copy-fault trace off in release). Not compiled out after all:
   the runtime C's direct reads of the toggle words (a load of a word that stays 0), fence-stats residue (TLS stores,
   one exchange per submission). Planned: gates become `constexpr false` in release (`Gow3Stats::enabled`,
   `Gow3Sections::Enabled`, `WaitDiagnostics::Enabled`, `CacheStatsEnabled`, `StatsOn`, flip-trace `Get`, wait-sites,
   toggle words, `LOG_*` level); file polls replaced by key-handler atomics; object naming off; the 40 MiB ring out.
   Build script release variant; the run log's first line names the build type; LANDMINES: A/B only within one build
   type. `GOW3_FPS_LOG` default off; copy-fault trace default off in release.
3. **Step 3, launcher.** In a release build the diagnostic options are greyed out with the reason; the Start-menu
   icon points at the release build. **Fix built 2026-11-16** (card `tooling/dev-tools-build`, `3b05a90`): launcher
   greying, Linux `GOW3_BUILD`, the AppImage packs the release build (`GOW3_APPIMAGE_DEV=1` for a dev one). The
   Windows zip already shipped `out/medievil-runtime.exe` (release).
4. **Small fixes, any time:** semaphore `getenv` inside the global lock; `GOW3_FRAME_STATS=0` parsing; marker labels
   before the gate.
