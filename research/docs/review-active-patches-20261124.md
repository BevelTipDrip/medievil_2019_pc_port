# Review: the active patches the user can toggle (2026-11-24)

Status: static review only (no launch, no build, no source edit). Branch `perf-hygiene` at `52d33b3`; release exe
`out/medievil-runtime.exe` (built 2026-11-24 20:42, P5 / P6 included) disassembled with `llvm-nm -n --demangle` and
`llvm-objdump -d` (CLANG64). Parent: [START-HERE.md](START-HERE.md). Related: [INDEX.md](../patches/INDEX.md),
[LANDMINES.md](../patches/LANDMINES.md), [study-perf-deepdive-20261123.md](study-perf-deepdive-20261123.md),
[study-tooling-off-cost.md](study-tooling-off-cost.md), [plan-work-queue.md](plan-work-queue.md) (P1-P17).
Markers: [V] seen in code, disassembly or a log; [I] inferred. Status words as in the patch cards: verified = the
user confirmed it in game; run = ran and measured, no separate in-game confirmation; built = not run.

## Summary

1. **The Windows defaults and the user's Windows setup differ.** The port folder's `medievil.ini` has every `deck_*`
   key at 1 [V] (written by the 2026-11-13 migration of the old launcher Deck switches), so every PC session and
   measurement since then ran 7 Deck switches that the patch map lists as "off" on Windows and run.py logs as
   "Windows: untested". A fresh install runs none of them. Settle it with one PC A/B, then make the CPU-only,
   same-answer cuts (A3, R1, A2, T2, drain hybrid) Windows defaults and keep early frame EOP / decode pacing opt-in
   (they reorder work; LANDMINES 29 with HDR grading).
2. **What a switch costs to check in the release exe is small, and in places nothing.** Toggle-file bits, the
   second toggle word and `Gow3Stats::enabled` compile to constants in release [V]. The rest are one or two
   L1-resident loads and a predicted branch per check (function-local static guards, C `-1` sentinels, one relaxed
   atomic load for the live patches). About 80-90 checks a draw across stage A and B: at most ~0.5-1% of the per-draw
   CPU work [I], no visible FPS change on the GPU-bound PC, inside the Deck's run-to-run drift. Making the internal
   default-on switches compile-time constants in release is worth doing for tidiness (one source of truth, no stray
   variable changing a release run), not for frame rate.
3. **Three launcher fallbacks are worse than "as before"** (LANDMINES 18-20): `perf_gpu_data_no_waits=0` keeps
   fault-page downloads without D3's queue (a full GPU wait each), `perf_lockfree_memory=0` silently turns off R1 /
   A1, HZB with deferred readbacks off falls back every frame. Fix the bundles or the launcher hints.
4. **Rejected or superseded code that still runs or is offered:** clear-skips-upload (status rejected, default on,
   runs per render target bind), the launcher's "Dedicated core for the GPU thread" (stage-b-core, rejected: "a lot of
   large stutters"), P2's dead branch in `ProcessDownloadImages`. No superseded patch is on by default.
5. **Worth a run:** finer occlusion culling at the PC's scene size (never run above 1280x800; Deck +4.6% FPS) and
   a collision count for the 4,096-entry ring-reuse table.

## 1. Inventory

### 1a. Performance patches (launcher > Performance, in-game menu > Performance; `medievil.ini perf_*`)

All default on, Windows and Deck (graphics_options.PERF_DEFAULT_OFF is empty; the Deck preset sets all of them to 1
except `perf_4k_memory`). run.py sets the "off" variables only when a key is 0 (`perf_environment`); the
runtime's own default is on when the variable is missing.

| Launcher key (live?) | Cards | W / D | Status | When the key is 0 | How the switch is read at runtime |
|---|---|---|---|---|---|
| `perf_hzb_occlusion` (restart) | hzb-remap, occlusion-queries-off | on / on | verified (T-HZ4: culling correct, the user "average 175-180, 1% lows ~145") | `GOW3_HZB_REMAP=0` | Once at start: `install_hzb_remap` (probe.c:592) installs the hook or not; run.py writes the pak's query cvars. No per-frame check. |
| `perf_no_decal_prepass` (restart) | no-decal-prepass | on / on | verified (the user: "increase the 1% lows significantly in heavy areas") | `GOW3_DECAL_PREPASS=1` | Pak cvars at start (ue4_override.py:536). No runtime check. |
| `perf_particle_single_pass` (live) | single-particle-pass | on / on | verified with the guard (PC test 2026-11-13, Deck particle runs) | `GOW3_PARTICLE_FIXED_STEP=1` | Start value once (C `static int start=-1`, runtime_extra.c:729), then the menu's atomic `particle_wanted` in `ue4_particle_pass_poll` (per poll, not per draw). |
| `perf_particle_flip_guard` (restart) | particle-flip-guard | on / on | verified (Deck quotes; the user's PC test 2026-11-13, no log) | nothing set (guard needs `GOW3_PARTICLE_FLIP_GUARD=1`, run.py sets it when on) | Hook installed at start (probe.c:1192); per simulation `particle_guard_on()` = `!(runtime_toggle2() & ...)`, a constant in release. |
| `perf_small_buffers` (live) | S2 | on / on | verified (T-S2b, "the best so far") | `GOW3_STREAM_CLEAN=0` | Relaxed atomic load `Gow3Settings::Get().perf[PerfSmallBuffers]` per `ObtainBuffer` (buffer_cache.cpp:1270; per buffer binding). |
| `perf_texture_memo` (restart) | texset-region-stamps | on / on | verified (no texture problems T-S1b/c; the user: "worth it") | `GOW3_TEXSET_REGIONS=0`, `GOW3_TEXSET_TWO_WAY=0` | Function-local statics with guards, `TextureSetRegions()` / `TextureSetTwoWay()` (vk_rasterizer.cpp:3320, 3355), per texture-set lookup. |
| `perf_lockfree_memory` (live) | lockfree-region-reads, L2 | on / on | lock-free reads verified in T-P5 (bundle: "the best it's been"); L2 run only | `GOW3_OPTIMISTIC_CLAMP=0`, `GOW3_REGION_BUCKETS=0` | `perf_optimistic` C int with `-1` sentinel, set live by `runtime_memory_set_perf` (runtime_memory.c:183); `region_buckets_on()` plain static int (restart). Per region lookup (~12 a draw). |
| `perf_cheap_frees` (live) | decommit-keep-access | on / n/a | verified (T-M3, "better with it on") | `GOW3_DECOMMIT_KEEP_ACCESS=0` | `perf_keep_access` C int, `-1` sentinel, live (runtime_memory.c:173). Per decommit (~1,100 a second, T-M1). |
| `perf_gpu_data_no_waits` (restart) | D3, gpu-write-twins, E1, W2 | on / on | twins verified (T-T1, "Much better"); D3 / E1 / W2 run (T-E1 quote is for the pair) | `GOW3_READBACK_QUEUE=0`, `GOW3_GPU_WRITE_TWINS=0`, `GOW3_EOS_DEFERRED=0`, `GOW3_EOP_TARGETED=0` | Readback queue: created or not at device creation (vk_instance.cpp:520), plus a guarded static in `FastReadback::Available`. Twins: guarded static per write fault. E1: guarded static per end-of-shader event (liverpool.cpp:775). W2: guarded static per end of pipe (texture_cache.cpp:502; 220-330 a frame). |
| `perf_4k_memory` (restart) | 4k-memory | on / off | run (T-R3, "everything seemed to work as expected") | `GOW3_PS4_MEMORY=1` | Memory sizes chosen at start (run.py, DMEM). No runtime check. |
| `perf_lean_barriers` (restart) | lean-barriers | on / on | verified (T-F7: 1% / 0.1% 119.7 / 117.4 at 120, "no issues") | `GOW3_SKIP_READ_BARRIERS=0`, `GOW3_DEPTH_SAMPLE_IN_PLACE=0` | Guarded statics: `SkipReadBarriers()` per image barrier (image.cpp:554), and in `BeginRenderingFull` (vk_rasterizer.cpp:4023, 4037) and `Runtime::SampleDepthInPlace` (vk_runtime.cpp:222). |
| `perf_lean_binding` (live) | bulk-descriptor-copy, sampler-memo | on / on | run (N-30 -0.12 us, N-31 -0.085 us a draw; no in-game item) | `GOW3_LEAN_BINDING=0` | `PerfOn(PerfLeanBinding)`: relaxed atomic load, per descriptor push (`Pipeline::BindResources`) and per stage (`BindSamplers`). |
| `perf_ring_reuse` (live) | ring-reuse | on / on | verified (N-32: "the .1% lows ... legitimately higher") | `GOW3_RING_REUSE=0` | `PerfOn(PerfRingReuse)` per stage in `CollectRingBindings` (stage A). |

### 1b. Steam Deck switches (`medievil.ini deck_*`; launcher Performance > Deck tuned; Deck Optimized preset)

Default 0 everywhere; 1 in the Deck preset. **The port folder's `medievil.ini` has all ten at 1** [V]; on Windows
run.py applies 7 and leaves out `backing_copy` and `thread_nice` (Linux code) and `arena_writes_off` (U2 is off on
Windows anyway), logging "Windows: untested". All restart-only (A3 made restart-only by `adfeb67`).

| Key | Card | W / D | Status | Variable | How read |
|---|---|---|---|---|---|
| `early_frame_eop` | early-frame-eop | off / on | run (Deck: pacing stddev 2.2 -> 1.3-1.5 ms; the user "a small gain") | `GOW3_EARLY_FRAME_EOP=1` | Guarded static `EarlyFrameEop()::on` (liverpool.cpp), per frame EOP. |
| `decode_pacing` | decode-pacing | off / on | run (Deck D0-D2, FPS unchanged until E1-deck-drains) | `GOW3_DECODE_PACING=1`, `_LEAD=2` | Guarded static `Gow3DecodePacing::Enabled()` (gow3_decode_pacing.cpp:149), per submission / label. |
| `drain_hybrid` | drain-hybrid | off / on | run (Deck: stage A CPU 112% -> 76%, keep) | `GOW3_DRAIN_HYBRID=1` | Guarded static in gow3_drain_wait.h, per drain (~300-1,000 a second); the live toggle2 bit is a constant in release. |
| `texset_wide` | T2 | off / on | run (Deck, in the R1/R2/T1/T2 group) | `GOW3_TEXSET_WIDE=1` | Guarded static `TextureSetWideWanted()` per bind. |
| `region_index` | R1-region-index | off / on | run (Deck: 14.3k index hits a frame) | `GOW3_REGION_INDEX=1` | C `static int wanted=-1` (runtime_memory.c:1875), per region lookup. |
| `pending_filter` | A2 | off / on | run (Deck: scans 1,225 -> 0.4 a frame) | `GOW3_PENDING_FILTER=1` | Guarded static `PendingFilterOn()` (vk_rasterizer.cpp:574), ~1,400 checks a frame. |
| `reg_hash_paired` | A3 | off / on | run (Deck, A1-A3 group) + unit test (0.63 vs 1.03 ns a word on the 7800X3D) | `GOW3_REG_HASH_PAIRED=1` | Guarded static `PairedRegisterHash()::wanted` (liverpool.cpp:1030) per register packet on stage A, the scanner and the workers. |
| `backing_copy` | A1 | n/a / on | run (Deck) | `GOW3_BACKING_COPY=1` | C sentinel (Linux only). |
| `thread_nice` | thread-nice | n/a / on | run (Deck, wins at a fixed 1,400 MHz) | `GOW3_THREAD_NICE=1` | At thread start (Linux only). |
| `arena_writes_off` | U2 | off / off (on Deck preset = U2 off) | run ("correct, no gain") | `GOW3_ARENA_HOST_VISIBLE=0` | At buffer cache creation (memory type), not per draw. |

### 1c. Presentation and frame rate (launcher > Performance / Display; `present_*`, `fps_limit`, `vsync`)

| Setting | Cards | W / D default | Status | How read |
|---|---|---|---|---|
| `present_inline=1` (`GOW3_PRESENT_THREAD=0`) | inline-present | on / on | run / feel (T-P120) | Once at driver start (thread layout; driver.cpp:100). |
| `present_precise_tick=1` (`GOW3_PRECISE_TICK=1`) | precise-tick | on / off | verified in T-V120 / T-P120; **a no-op under the Windows defaults** (LANDMINES 5, confirmed in code) | Guarded static in the vblank loop (driver.cpp:1564). The user's ini has 0. |
| `present_limiter=reflex` (Reflex + precise-limiter fallback vars) | reflex, precise-limiter | mixed / off (Deck: standard) | verified (T-F27, "looks great, huge improvement") | Extension at instance creation; guarded statics in the present path (`ReflexSleepBeforePresent`, `precise_limit`). Per flip. |
| `present_no_frame_caps` (`GOW3_NO_FRAME_CAP`) | presentation-settings | off / on | run | Emulator settings at start. |
| `present_tick_hz=960` (`GOW3_VBLANK_HZ`) | precise-tick | 960 / 900 | run | At start. |
| `present_mode` (`GOW3_PRESENT_MODE`) | present-mode-select | auto | run | At swapchain creation. |
| `fps_limit=-3`, `display_vrr=1`, `vsync=1` | vrr-auto-limits, vsync-control, frame-limit-parse | on / off | verified (T-F27) | Settings atomics, per flip; VSync live. |

### 1d. Other launcher settings that gate patch code

| Setting | Card | Default | Status | How read |
|---|---|---|---|---|
| `stale_readback` / `deferred_readback` (GPU data page) | lineage-deferred-readbacks | 1 / 1 (settings_version 2 turns stale on once) | run (GoW3 40.9 -> 66.5 FPS; Deck "seemed to help a lot") | `TextureCache` members initialised once from the variables (texture_cache.h:560, 569): a plain member load, no guard. |
| `draw_pipe`, `prep_workers`, `readbacks` (Advanced) | lineage-draw-pipe, lineage-draw-preparation | auto | lineage | At start (thread creation). |
| `stage_b_core` "Dedicated core for the GPU thread (Experimental)" | stage-b-core | off | **rejected** (T-HZ4h: "a lot of large stutters") yet offered | At start (vk_draw_pipe.h:312). |
| `frames_queued` | frames-queued-2 (value 2 rejected) | 1 | 1 is the tested default | Live setting. |
| `async_shaders`, `pipeline_binary_cache`, `background_warmup`, `parallel_warmup`, `vulkan_compat` | lineage / upstream | on / on / on / on / off | lineage, upstream | At start. |

### 1e. Default-on switches with no launcher control (environment only; relevant to section 2)

`GOW3_RECORD_DATA_FIX`, `GOW3_NARROW_IMAGE_STAGES`, `GOW3_DEPTH_WRITE_EFFECTIVE`, `GOW3_READBACK_FIRST_WAIT`,
`GOW3_FAULT_PAGE_DOWNLOAD`, `GOW3_NEW_IMAGES_UNDEFINED`, `GOW3_PARK_UNMAPPED`, `GOW3_MICRO_MIP_TARGETS`,
`GOW3_DEFERRED_COPY_BACKING`, `GOW3_READBACK_SUBMIT`, `GOW3_VOLUME_VIEWS`, `GOW3_POOL_ZERO_FOREIGN`,
`GOW3_FORCE_ANISO`, `GOW3_CLEAR_SKIPS_UPLOAD`, `GOW3_UNDEFINED_GPU_FREED`, `GOW3_IMAGE_RELEASE_THREAD`,
`GOW3_MOVIE_SUSPEND`, `GOW3_COND_PRECISE` (Windows); values `GOW3_GPU_WRITE_TWINS_MAX` (16 KiB),
`GOW3_SEMAPHORE_POLL_US` (100), `GOW3_CONSTANT_RING_MB` (96). Almost all are guarded function-local statics; a few are
C sentinels or `InitOnceExecuteOnce` (cond precise, once per timed wait). Patches whose only switch is a toggle-file
bit (B1, V4, C3, D1, D2, D5, T1, R2, C5, texset-compact, C1's poll interval, the inherited memos) are already
unconditionally on in release: the bit tests fold away (section 2a).

No getenv runs per call on a hot path: every per-call `getenv` left in `gpu/` and `src/` is in start-up, a
once-only initialiser, a 5 s report, or a dev-only branch [V, grep of all getenv sites outside static
initialisers].

## 2. What checking a switch costs

### 2a. How each kind compiles in the release exe [V]

- **`Gow3Toggle::Disabled`, `Gow3Toggle2::Disabled`, `runtime_toggle2()`**: `return false` / `0` under
  `GOW3_DEV_TOOLS=0`; the tests vanish. `BindSamplers`' `memo_on` is just the perf byte, with no toggle load.
  `runtime_disabled_optimizations` is **absent from the release symbol table**: ThinLTO saw no writer (the watcher is
  compiled out) and folded the direct `runtime_disabled_optimizations & 1` tests in `runtime_memory_clamp` /
  `_region` too (the function starts with the `cached_region` call, no load).
- **`Gow3Stats::enabled`**: `inline constexpr Switch{false}`: every stats test folds.
- **`PerfOn()` (live patches)**: `Gow3Settings::Get()` is inlined by ThinLTO into the callers:
  `movzbl guard(%rip); test; je cold; movzbl values+0xe0(%rip)` (BindSamplers 0x1400b8001). Two byte loads and a
  branch.
- **Function-local `static const bool x = [] { getenv... }()`**: the same guard pattern (load guard byte, branch,
  load value). Each inlined site also carries its cold path (`getenv`, `__cxa_guard_acquire` / `_release`) inline.
- **C sentinels** (`perf_optimistic`, `region_buckets_on.on`, `region_index_wanted.wanted`): `mov; test; jns` and
  the value: one load and a branch.

Switch references in the hottest release functions (from the disassembly; a guarded site references its guard
about 3 times: the check, plus the cold acquire / release):

| Function (per what) | Switch statics referenced |
|---|---|
| `Pipeline::BindResources` (per descriptor push) | `Scheduler::RecordDataFix()::enabled` ~5 sites (15 guard refs); `Get()` perf byte 1 |
| `Rasterizer::BindTextures` (per stage) | `ImageReadStages::narrow` ~5 sites; `TextureSetRegions` ~5; `TextureSetTwoWay` 2; `TextureSetWideWanted` 1 |
| `Rasterizer::DrawRecord` (per draw) | `RecordDataFix` 1, `Ue4Motion::Enabled` 1, `all_motion` 1, `Get()` 1 |
| `BufferCache::ObtainBuffer` (per buffer) | `Get()` perf byte (small buffers) 1 |
| `Rasterizer::BindSamplers` (per stage) | `Get()` perf byte (lean binding) 1 |
| `runtime_memory_clamp`, `runtime_memory_region` (per lookup) | `perf_optimistic`, `region_index_wanted`, `region_buckets_on`, `vma_optimistic`: 2 refs each |
| `cached_region` (per lookup, not inlined) | `region_buckets_on` 4, `region_index_wanted` 2 |
| `Image::GetBarriers` (per transition) | `SkipReadBarriers` 2 sites |

`RecordDataFix` is the most-read switch: `Record()` and `RecordData()` check it on every deferred command
(vk_scheduler.h:779, 818, 845), several per draw.

### 2b. Cache lines [V layout, I cost]

- The guard bytes (0x140b42740 on) and the static values (0x140b76xxx-0x140b77xxx) sit among other
  initialise-once statics and report counters that change rarely (`frame_dump_*`, `reported`, `warned`): read-mostly
  lines, no false sharing found.
- The C switch line 0x140b0f340-37f (`perf_optimistic`, `perf_keep_access`, `vma_optimistic`,
  `region_buckets_on.on`, `region_index_wanted.wanted`, `pool_zero_foreign.v`, `futex_cond_enabled.value`) shares
  its line with `key_lock`, `low_next` and `static_init`: written only at pthread key creation / deletion, boot-time
  low allocations and static mutex initialisation. Read-mostly in play.
- **One shared line:** `Gow3Settings::Values::perf[]` (+0xd6 to +0xe2; the line 0x140b761c0) shares its line with
  `image_width / image_height / window_width / window_height` (+0xac to +0xb8), which `Presenter::PrepareFrame`
  writes **with `xchg` every frame** (vk_presenter.cpp:662-665, seq_cst stores of usually the same value). Stage B's
  first `PerfOn` after each flip misses: ~240 misses a second at a 237 cap, ~100 ns each, ~24 us a second
  (0.002%). Negligible; fixable for free (`alignas(64)` on `perf`, or store only when the value changed, which also
  drops the presenter's five locked exchanges a frame).

### 2c. Frequencies and an honest estimate

Frequencies from the cards: heavy PC frames ~1,490-1,550 draws (N-30, N-31), stage B's draw section 1.86-2.13 us a
draw; ~372k packets a second (T-E1); ~12 region lookups a draw (R1: 14.3k a frame at ~1,150 draws, Deck);
~1,400 pending-write checks a frame (A2); 220-330 end-of-pipe events a frame (P3); ~1,100 decommits a second (T-M1);
530-760 game-thread write faults a frame (work queue).

That is roughly 80-90 switch checks a draw across stage A and B (about half on stage B) [I]. Each is an L1 hit and
a well-predicted branch, well under a cycle to ~1 cycle in an out-of-order core: about 8-18 ns a draw, at most
~0.5-1% of the ~2 us per-draw work, and 12-28 us of a heavy 4.2 ms frame spread over two threads [I]. The PC is
GPU-bound at the user's settings (deep dive flip traces: the presenter waits ~4.0 ms of ~4.4 ms for the GPU), so
**no FPS change is expected**. On the Deck, stage B is 76-86% busy: 0.5-1% of it is at most ~0.1 ms a frame, inside
the 1.35-1.5 FPS run-to-run drift. Write faults, decommits and lock waits cost microseconds each, so a check
there is noise.

### 2d. Recommendation: compile-time constants in release

- **A. Make constant in release (dev keeps the variable for A/Bs):** the section 1e list. Verified or
  upstream fixes with no launcher control; `RecordDataFix` (an upstream crash fix, issue #18) and
  `ImageReadStages::narrow` first, as the most-read. Pattern: `#if GOW3_DEV_TOOLS` static from the variable,
  `#else inline constexpr bool ... = true` (as `Gow3Stats::enabled`). Gain: the checks and their inline cold paths
  go; dead fallback code (the non-bulk descriptor loop, `eAllCommands` stages, the full-window fault download)
  becomes removable. Expected: not measurable in FPS; do it for tidiness and so a stray variable (extra-env.txt)
  cannot silently change a release run.
- **B. Launcher perf bundles: keep switchable in release.** They are the user-facing fallback ("if one misbehaves
  on another PC"). If wanted, read each once at start into one `const`-after-init struct on its own cache line,
  filled by `Gow3Settings::Load()` before the GPU threads start (no guards; the live ones stay atomics). Three
  already cost nothing at runtime (HZB remap, decal prepass, 4K memory: start-time only). Dropping the release toggle
  for long-verified restart-only bundles (texture memo, lean barriers) is the user's call; it gains the same
  nothing.
- **C. Deck switches: keep switchable** (they are the Deck A/B surface and untested on Windows), except A3 (3.2).

## 3. Improvements, ranked by expected value and risk

| # | Item | Value | Risk | Evidence | Next step |
|---|---|---|---|---|---|
| 1 | **Settle the Deck switches on Windows.** The user's ini runs 7 on Windows; fresh installs run none, and the patch map says "off". Baselines and other players get a configuration the user does not play. | medium | low | ini [V]; LANDMINES 9 ("every Start-menu session on the PC runs the 8 Deck switches"); Deck runs on each card | One PC launch pair (`deck_*` 0 vs 1, release, no logs, same spot, judged on 1% / 0.1% lows). Promote A3, R1, A2, T2, drain hybrid to Windows defaults; keep early frame EOP and decode pacing opt-in until P14's grade-block ring copy (LANDMINES 29: the HDR grading pass reads guest memory at record time, and early EOP lets the game run further ahead). Alternatively fold T2 into `perf_texture_memo` and R1 into `perf_lockfree_memory` (R1 already depends on it). |
| 2 | **A3 as the only register hash** (compile-time) | low-medium | low | 0.63 vs 1.03 ns a word on the 7800X3D (-38%), on stage A, the scanner and 4 workers; `stage-a-hash-test` 0 collisions in 2 million variants | Make the paired hash unconditional. This also removes the LANDMINES 1 class (stage A and the workers hashing differently). |
| 3 | **Fallback bundles that are worse than before** | medium (support) | low | LANDMINES 18-20 | `perf_gpu_data_no_waits=0` should also set `GOW3_FAULT_PAGE_DOWNLOAD=0` (else every finished-page download waits for the whole GPU); the launcher hint for `perf_lockfree_memory` should say it also stops R1 / A1; grey out HZB occlusion (or warn) with deferred readbacks off. |
| 4 | Release constants for the internal switches (2d A) | low (hygiene) | low | section 2 | One commit per area; check with `llvm-objdump` that the guards are gone. |
| 5 | **clear-skips-upload: rejected, yet on and running** | low | low | card: "the measured effect in MediEvil is nil"; `IsMetaCleared` per colour target in `BeginRenderingFull`; `gpu_freed_addresses` insert per freed GPU-written image and erase per new image, redundant while `NEW_IMAGES_UNDEFINED` is on | Default off (or dev only), and skip the GPU-freed bookkeeping when new images are undefined anyway. |
| 6 | **Finer occlusion culling at the PC's scene size** | medium, uncertain | medium (pop-in) | Deck +4.6% FPS, 1,241 -> ~1,151 draws; "PC: not run at small scenes. At 1080p and above the patch is not added" | One A/B at the user's 3840x2160 x 75% (2880x1620 scene), `GOW3_HZB_FULL_SIZE=1`, with a pop-in check. The GPU-bound PC saves geometry work on culled draws but pays a bigger HZB build. |
| 7 | **Ring-reuse table collisions** | low-medium | low | 4,096 direct-mapped entries, 81% of constant bytes reused (N-7); the texture-set memo found half its misses were slot conflicts (T-S1b) and gained from two ways | Count "slot held another range in this epoch" in frame stats (dev); if it matters, make it two-way or 8,192 entries (fewer PCIe BAR writes: 1.93 MB a frame still copied). |
| 8 | `present_precise_tick` default | low | low | LANDMINES 5: a no-op with a cap or Reflex; its 700 us spin per tick runs only uncapped with Standard or Precise (also Reflex's fallback on AMD / Intel): ~0.65 of a core, computed. The user's ini has 0; the Deck preset 0 | Default 0, or on only when the tick rate is the cap (the T-P120 "tick = cap" mode it was made for). |
| 9 | Thresholds set long ago, checked with counters that exist | low | none (measure only) | `GOW3_GPU_WRITE_TWINS_MAX` 16 KiB (`fault_too_big_for_twin`); `OPTIMISTIC_SPIN_CYCLES` 400,000 (~100 us; region counter `RC_LOCKED`, T-P4's fallbacks 2.1-2.2%); `GOW3_SEMAPHORE_POLL_US` 100 (tuned for the Deck's ioctl); constant ring 96 MiB ("found it full" report) | One dev frame-stats run in the heavy area and a fight answers all four; change a value only where its counter shows misses. |
| 10 | Rejected or superseded code still reachable | low | low | stage-b-core rejected but offered as "Experimental"; P2's `idle_eop_skip` branch in `ProcessDownloadImages` (superseded by W2 and P5's lock-free return, `aa3d304`); O2-O3 occlusion (`GOW3_OCCLUSION`) off | Hide "Dedicated core" in the release launcher (or label it rejected); delete P2's branch; O2-O3 dev only. **No superseded patch is on by default** (W2 > P2, L2 > L1, hzb-remap > O2-O3, no-decal-prepass > early-z-pass-off: all off) [V card scan]. |
| 11 | Presenter's per-frame `xchg` on the settings line | negligible | none | section 2b | `alignas(64)` on `perf[]`, store-if-changed. Fold into 4. |
| 12 | Stale keys in the user's ini (`output_res`, `present_precise_limit`, `present_newest_frame`, `present_vblank_sync`) | none (tidiness) | none | read by nothing [V grep] | Optional `settings_version` step to drop them (after the user's say; ini is CRLF, byte-safe edits). |

Already queued elsewhere, not repeated here: P8 (guest faults that need no GPU work drain the draw pipe), P9 (dynamic
read-only buffers over 16 KB: S2's sibling case), P14 (grade / tonemapper blocks from the stage-A ring copy;
copy-thread backing reads), the region table's writer cost (memmove per edit; the real fix for item 9's spin).

**Defaults that later fixes made questionable:** `present_precise_tick` (item 8). Early frame EOP in the user's own
Windows setup with HDR grading on (item 1, LANDMINES 29): correct in every run so far, but it widens the window the
grading pass's direct guest read depends on. Everything else on by default still has its reason: stale readbacks
(their glitches patched: readback-first-wait, hzb-remap, W2), the single particle pass (with the guard).

## Method

`llvm-nm -n --demangle out/medievil-runtime.exe` (38,006 symbols; 225 guard variables), then `llvm-objdump -d` over
each hot function's range, counting references to `_ZGV*` guards, `_ZZ*` statics and the C switch symbols; cache
lines from the sorted symbol addresses. Cards via `INDEX.md` and their front matter / Evidence sections; the
launcher (`PERF_PATCHES`, `deck_switch_rows`, `game_environment`), `scripts/graphics_options.py` (`PERF_SWITCHES`,
`DECK_SWITCH_ITEMS`, `present_environment`), run.py, `gow3_settings.cpp` `Load()`; a grep of every `getenv` in
`gpu/` and `src/` outside static initialisers. The game was not running at any point (checked before the symbol
dump).
