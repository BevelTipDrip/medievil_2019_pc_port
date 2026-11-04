# Study: every performance patch, audited for the Steam Deck and slower hardware (2026-11-04)

Status: read-only research (docs, code on `deck-lab` `0bb5a01`, git log of `lab` / `deck-lab`, Deck logs in `data/`).
Nothing built, launched or changed. Parent: [plan-linux-deck.md](plan-linux-deck.md) ("Direction (the user,
2026-11-03)": the "Deck audit of the PC fixes"). This page builds on, and does not redo:
[study-deck-rescore-rejected.md](study-deck-rescore-rejected.md) (the **unshipped** ideas: its A1-A14, B, C, D rows),
[study-stage-b-per-draw.md](study-stage-b-per-draw.md) (C1-C11), [study-deck-other-threads.md](study-deck-other-threads.md),
[study-deck-platform-optimization.md](study-deck-platform-optimization.md), [study-deck-apu-deep-dive.md](study-deck-apu-deep-dive.md),
[study-deck-memory-budget.md](study-deck-memory-budget.md), [design-u5-deck-memory.md](design-u5-deck-memory.md) section 13,
[fix-inventory.md](fix-inventory.md) (the PC evidence for every shipped fix).

Marks: **[measured: source]**, **[code: file:line]** (paths under `gpu/shadps4/video_core/` unless they start with
`src/`, `gpu/shim/`, `scripts/`, `patches/`, `gpu/shadps4/core/`, `gpu/shadps4/common/`), **[inferred]** (reasoning or
estimate). "Deck" numbers are the heavy graveyard view at 720p / 67%, Low, ~1,400 draws, ~65 FPS, stage B ~91% busy
(E1, plan-linux-deck.md:1035-1038) unless named.

## Short answer

- **85 distinct performance patches** found (section 1, some rows group small items): 67 on by default on the PC,
  13 designed for the Deck on `deck-lab` (9 on by default on Linux, 4 opt-in) and 5 rejected opt-ins. **Of the 67 PC
  defaults, only 2 were ever switched on the Deck**: the HZB remap, and the precise tick under Proton. About 14 more
  show a hit rate or a counter in a Deck log; the rest have no Deck data. Of the 13 Deck items, only U2 and the E1
  bundle have run.
- **(c) likely costs more than it saves on the Deck / slow hardware (5):**
  1. **Precise vblank tick, on by default** (`present_precise_tick=1`, tick 960). It spins 700 us of every ~1.1 ms
     tick: ~2/3 of a logical CPU, and on Windows at TIME_CRITICAL + MMCSS. The user's Deck ini already has it off
     (no "precise vblank tick" line in any native log). It is still the default for a fresh install, a handheld or any
     4-6 core PC.
  2. **U2 direct arena writes, default on for Linux integrated GPUs.** It measured no gain on the Deck. It adds a use-tick
     table walk to every `ObtainBuffer` on stage B (3-4 a draw). The live switch (word 2 bit 8) leaves that bookkeeping on,
     so the U2 A/B could not see its cost [code: `buffer_cache.cpp:1322-1325`, `:633-640`]. Every run since U2
     (D0-E1) had `GOW3_ARENA_HOST_VISIBLE=0`, so **the next default deploy changes the baseline** unless that key is
     kept.
  3. **Forced 16x anisotropic filtering** (`GOW3_FORCE_ANISO`, on, `6fa6819`). It is a quality item that saves
     nothing and was never measured. It adds texture bandwidth on a GPU that is 85-95% busy at 0.8-1.2 GHz and shares
     88 GB/s and ~15 W with the CPU.
  4. **The texture-set memo's 32,768-slot, ~52 MB layout** (the memo itself is essential). The 32,768 slots were a
     +1.3% PC tweak. On a 4 MB L3 / 8 MB-reach TLB, a hit costs 5.7x the PC's against 4-5x for other parts.
     Estimate 0.15-0.5 ms a frame of stage B.
  5. **Precise limiter / hold spin** (the Reflex fallback on AMD, Intel and RADV). It spins 0.7 ms per frame when capped,
     at TIME_CRITICAL on Windows. Small; zero when uncapped.
- **Most important (d), needs a measurement:**
  - the draw-prep worker count and priority. The workers feed stage B's texture memo: an unprepared draw cannot use
    the memo and binds on the ~15 us slow path. So fewer workers can cost more than the SMT relief they give;
  - the Deck-designed defaults that never ran: C1, recorder lean, C5, D4, D5, the static GPU library, #6 padding;
  - OnSubmit on stage B (E1 moved per-frame work onto the wall; its E0 split was never run);
  - lean barriers (depth sampled in place) and narrow image stages on RADV;
  - W2 / B1 targeted waits, which take a mutex per readback range inside the Deck's 17-26x events.
- **Recommended first Deck test:** the launch already planned (plan-linux-deck.md:1103-1106, the word-2 pairs 16 / 32
  / 128 / 256 / 512), with **`GOW3_ARENA_HOST_VISIBLE=0` kept in extra-env** so it matches the E1 baseline. Add the
  three word-1 screening pairs of section 3.1 in the same or the next launch. The first restart-only test after
  it: `GOW3_PREP_WORKERS=3` while watching the "unprepared draw" slow binds. Next, U2's default (the key removed), then
  `GOW3_FORCE_ANISO=0`.

## 0. Cost model used

From the yardstick of study-deck-rescore-rejected.md: 1 us a draw on stage B = ~1.4 ms a frame; every part of a draw
is 4-5x the PC's, memory-bound lookups 5.4-6.7x, ordered event tasks 17-26x (a timeline-semaphore ioctl per event on
RADV) [measured: plan-linux-deck.md:679-695, study-stage-b-per-draw.md section 1]. There is no idle core: stage A,
stage B and the game's main thread each use ~1 core, the recorder 63%, Wwise 58%, and four SCHED_IDLE prep workers ~19% each
with ~1.7k preemptions/s. SCHED_IDLE workers still land on stage B's SMT sibling [measured:
plan-linux-deck.md:914-919, at a capped clock]. CPU and GPU share one power budget: a pinned GPU clock held the CPU near
2 GHz, and any GPU work saved is CPU clock gained [measured: plan-linux-deck.md:904-905; inferred: platform study 8.5].
Stage B probably runs at a ~2.4 GHz plateau after short sleeps [inferred: apu deep dive 1]. The Deck has 8 logical
CPUs, so every `Available()` threshold resolves to its 8-thread value: draw pipe on (>= 8), prep workers 4 (/2),
copy threads 2 (/4), compiler threads 4 + 2 capture, recorder lean on (<= 8), pipe spin 50 us, old recorder spin
20 us [code: `renderer_vulkan/vk_rasterizer.cpp:454`, `vk_draw_prep.cpp:56`, `gpu/shim/gow3_copy.cpp:29`,
`vk_pipeline_cache.cpp:399-400`, `vk_scheduler.cpp:506`, `vk_draw_pipe.h:218`, `vk_scheduler.cpp:531`;
measured: "draw preparation workers: 4 (+1 scanner, 8 hardware threads)", every `deck-*.log`].

## 1. Inventory

Sources swept: both toggle words (`gpu/shim/gow3_toggles.h:25-124`, the comment at `src/runtime_memory.c:1440-1464`);
every `GOW3_*` read in `src/`, `gpu/shim/`, `gpu/shadps4/`, `run.py`, `scripts/`, `launcher/` (~230 names, of which the
performance ones are below; diagnostics, cheats, input, upscaler and TAA switches are left out); `patches/MediEvil.xml`;
`scripts/ue4_override.py` and `scripts/graphics_options.py` (`PERF_SWITCHES`, `PRESENT_DEFAULTS`, the presets);
fix-inventory.md (`lab` to `8187eb0`); `git log lab..deck-lab` (60 commits) and `8187eb0..lab` (docs and UI only).

Columns: default **PC** / **Deck** (Linux, 8 threads, integrated GPU; "user" = the user's Deck ini or extra-env).
Switch: `w1:N` / `w2:N` = toggle file first / second word, value N (a set bit switches the item **off**), `env`,
`ini`, `xml` (eboot patch), `build`. PC gain: the docs' own number, [measured] in the cited doc. "Deck?":
**A/B** = switched on the Deck; **rate** = only its hit rate or counter seen in a Deck log; **no** = nothing.

### 1.1 Draw pipeline and helper threads (inherited lineage: `docs/parallel_gpu.md`, measured on the base port's game, not MediEvil)

| # | Patch | What | Thread / resource | PC / Deck | Switch | PC gain | Deck? |
|---|---|---|---|---|---|---|---|
| 1 | Two-stage draw pipe (+ pipelined tasks, pending fence waits, dispatches, memory writes, indirect draws) | stage A decodes PM4, stage B (`gow3:DrawRec`) records draws | +1 thread that spins 50 us then sleeps | on (>= 8 threads) / on | w1: bits 37-42, 46; `GOW3_DRAW_PIPE` | +19% (16 thr), **+18% at 4c/8t taskset**, +32% whole pipe | no (always on) |
| 2 | Vulkan recorder thread | `vkCmd*` replayed on `gow3:VkRecorder` from 32 KiB chunks | +1 thread, 63% of a CPU on the Deck | on / on | w1:16; `GOW3_VK_RECORD_THREAD` | 103.5 vs 94.9 FPS (fences by recorder step) | rate (63%) |
| 3 | Draw preparation (pipelines, sharps, vertex inputs) | scanner + workers prepare draws ahead of stage A / B | 4 SCHED_IDLE workers + scanner, ~0.76 CPU on the Deck | on, workers = threads/2 (8) / 4 | w1:8192 (use), w1:4096 (sharps, vertex); `GOW3_PREP_WORKERS` | +11.5%, +3%, +5.5%; at 4c/8t +28% | rate (97.8% prepared) |
| 4 | Constant ring on stage A | small read-only buffers copied by stage A, stage B only binds | stage A memcpy; ring 96 MiB | on / on | w1:2^36; `GOW3_CONSTANT_RING_MB` | +11% | no |
| 5 | Recorder fences / E1 end-of-shader deferred | EOP / EOS labels written by the recorder after the host copies | recorder | on / on | w1:2^41; `GOW3_EOS_DEFERRED` | tasks 0.52 -> 0.23 us/packet (E1) | no |
| 6 | Copy threads: deferred uploads, deferred stream copies, parallel large copies | guest -> staging memcpy on copy threads, fences wait | 2 threads (normal prio) on the Deck | on / on | w1:256, w1:16384, w1:131072; `GOW3_COPY_THREADS` | streaming stalls 60-170 -> 40-50 ms | rate ("copy threads" blocked 0.4-2%) |
| 7 | Small host copies replayed by the recorder (default) vs copy threads | in command order on `gow3:VkRecorder` | recorder | recorder / recorder | w1:524288 = copy threads | rescore A2 | no |
| 8 | Async shader / pipeline compile | background compile, capture threads | 4 + 2 threads (6 + 2 PC) | on / on | `async_shaders`; `GOW3_ASYNC_SHADERS` | hitch removal | no |
| 9 | Driver memory calls off stage B | image frees on a worker, 32 MB residency chunks allocated ahead | 1-2 workers | on / on | `GOW3_IMAGE_RELEASE_THREAD`, `GOW3_RESIDENCY_CHUNK_MB` | NVIDIA lock waits 33 ms -> 0 | no |
| 10 | D3 downloads on a readback queue | per-page write ticks, copy on a compute-only queue | 2nd queue + semaphore | on / on | `GOW3_READBACK_QUEUE` | each download waits for its write only | rate (downloads ~0/s) |
| 11 | Texture binding helper (opt-in) | forks texture binds to a spinning helper | +1 spinning thread | off / off | `GOW3_TEXTURE_HELPER`, w1:2^34 | neutral (Bloodborne) | rate (0 binds) |

### 1.2 Stage B per-draw caches and memos

| # | Patch | What | Thread / resource | PC / Deck | Switch | PC gain | Deck? |
|---|---|---|---|---|---|---|---|
| 12 | Region cache | 8 entries per thread in front of `runtime_memory_clamp` | per-thread | on / on | w1:1 | (inherited) | no |
| 13 | Fetch shader cache | | stage A | on / on | w1:2 | (inherited) | no |
| 14 | Page tracking early exit | no range mask when nothing is CPU-modified | stage B | on / on | w1:4 | (inherited) | no |
| 15 | Pending-op poll limit | semaphore query 1 in 32 polls | stage B | on / on | w1:8 | (inherited; matters more where the query is an ioctl) | no |
| 16 | Image description cache (2-way, 4,096 x ~300 B) | | stage B, ~1.2 MB | on / on | w1:32 | 65.6 -> 93.9 FPS with #19 | no |
| 17 | Lock-free upload check | | stage B | on / on | w1:64 | (inherited) | no |
| 18 | FindImage cache (1,024) | | stage B | on / on | w1:128 | (inherited) | no |
| 19 | Texture binding memo, texture view memo | | stage B | on / on | w1:1024, w1:2^33 | 81.5 vs 80.8 FPS (view memo + #22) | no |
| 20 | Barrier tracker access memo | | stage B | on / on | w1:512 | (inherited) | no |
| 21 | Coarse (4 KiB) read tracking for barriers | wider read ranges: fewer inserts, may add barriers | stage B; GPU barriers | on / on | w1:2048 | (inherited) | no |
| 22 | Record-chunk write prefetch | `prefetchw` a few lines ahead in the record chunk | stage B | on / on | w1:2^32 | with #19 | no |
| 23 | Early draw inputs | vertex / index resolved inside BindResources | stage B | on / on | w1:2^35 | (inherited) | no |
| 24 | Render-state memo | reuse the open pass's state | stage B | on / on | w1:2^44 | +9% | rate (93.7% vs 81.5%) |
| 25 | Texture-set memo, region stamps, two-way slots, 32,768 slots (~52 MB) | a stage's resolved textures by prepared T# hashes | stage B; 52 MB table | on / on | w1:2^45; `perf_texture_memo` (`GOW3_TEXSET_REGIONS`, `GOW3_TEXSET_TWO_WAY`); size: build | +5.6%; stamps 83.6 -> 92.3% hits; slots 8,192 -> 32,768 +1.3% | rate (99.3% vs 92.1%) |
| 26 | UpdateImage lock-free fast path | | stage B | on / on | w1:2^30 | +2% | no |
| 27 | P1 dirty registers by word | | stage A / B | on / on | none | 114-123 -> 124-131 FPS (+8%) | no |
| 28 | Bulk descriptor copy per push | 3 `RecordData` instead of ~16 | stage B | on / on | w2:1; `perf_lean_binding` | -0.12 us/draw (N-30) | rate |
| 29 | Sampler handle memo | per-thread direct-mapped | stage B | on / on | w2:2; `perf_lean_binding` | -0.085 us/draw (N-31) | no |
| 30 | Stream-class table (65,536 x 16 B, 1 MB) | replaced an `unordered_map` cleared at 262k | stage B | on / on | none | hitch ObtainBuffer 10-12 -> 0.5 ms | no |
| 31 | S2 small read-only buffers from the arena | bind clean ranges in place, upload once | stage B; page protection | on (MediEvil) / on | `perf_small_buffers`; `GOW3_STREAM_CLEAN` | best lows then (T-S2b) | rate (59% vs 51%) |
| 32 | Constant-ring reuse | rebind the first copy of a range within a command buffer | stage A table (4,096); ring 96 MiB | on / on | w2:4; `perf_ring_reuse` | 1% / 0.1% lows +9 / +8 FPS; BAR writes -81% | rate (8.75 MB/frame reused) |
| 33 | HLE copy shader as one dispatch (+ 64 KiB merge distance) | | GPU compute | on / on | w1:2^43; `GOW3_COPY_MERGE_KB` | GPU 2.1 -> 0.6 ms (base port's game) | no (likely unused by MediEvil) |
| 34 | ThinLTO | | build | on / on | `GOW3_LTO` | per draw -4-7% median, mean +3% (noise) | no |
| 35 | **C1** semaphore poll interval | end-of-pipe retirement queries the tick at most every 100 us | stage B ioctl | on / on | w2:16; `GOW3_SEMAPHORE_POLL_US` | Deck-designed | **no** (built `f03ee52`) |
| 36 | **Recorder lean mode** | no recorder spin; polled labels do not wake it | recorder, stage B futex | off (> 8 thr) / on | w2:32; `GOW3_RECORDER_LEAN`, `_WAKE_US`, `_HANDOFF_KB` | Deck-designed | **no** |
| 37 | **C5** readback pre-check | seqlock spans skip `readback_mutex` | stage B | on / on | w2:128 | Deck-designed | **no** |
| 38 | **#6** cache-line groups | `alignas(64)` for cross-thread fields | all GPU threads | on / on | build `GOW3_CACHELINE_PADDING` | Deck-designed | **no** |
| 39 | **Static GPU library + D1 initial-exec TLS** (Linux) | no PLT / `__tls_get_addr` | build | n/a / on | build `GOW3_STATIC_GPU` | Deck-designed | **no** |
| 40 | **#4** stage B bounded spin (opt-in) | spin up to 200 us before blocking | stage B, power | off / off | `GOW3_STAGE_B_SPIN`; w2:1024 | Deck-designed | no |
| 41 | x86-64-v3 build flavour (opt-in) | | build | off / off | `GOW3_CPU_LEVEL=v3` | Deck-designed | no |

### 1.3 Barriers, sync and readbacks

| # | Patch | What | Thread / resource | PC / Deck | Switch | PC gain | Deck? |
|---|---|---|---|---|---|---|---|
| 42 | **Fewer render pass breaks** (a) read-after-read, (b) depth sampled in place in `DepthReadOnlyStencilAttachment`, (c) stencil rebind | | stage B; GPU (driver drains) | on / on | `perf_lean_barriers` (`GOW3_SKIP_READ_BARRIERS`, `GOW3_DEPTH_SAMPLE_IN_PLACE`) | fight lows 107.6 / 100.5 -> 119.7 / 117.4 | no |
| 43 | Narrow image stages | transitions wait for the reading stages | GPU | on / on | w1:2^31 (Linux: this only); `GOW3_NARROW_IMAGE_STAGES` | GPU active -0.8% (N-16) | no |
| 44 | Depth writes only with the depth test | | GPU layouts | on / on | `GOW3_DEPTH_WRITE_EFFECTIVE` | none (correctness) | no |
| 45 | W2 targeted end-of-pipe wait | wait only for copies reading the readback ranges | stage B; `host_copy_sources_mutex` per range | on / on | w1:2^60; `GOW3_EOP_TARGETED` | the full wait was ~21% of stage B | no |
| 46 | B1 targeted WRITE_DATA waits | 64 KiB-granule source table | stage B | on / on | w1:2^62 | 1 of ~120 waits skipped | no |
| 47 | Ordered guest writes (WRITE_DATA waits for deferred signals) | correctness wait | stage B | on / on | w1:2^49 | (correctness) | no |
| 48 | R1 readback submitted when recorded | | stage B | on / on | `GOW3_READBACK_SUBMIT` | none measured | no |
| 49 | Deferred image readbacks (64 copies) + stale readbacks | CPU reads the previous finished copy, no wait | stage B; per-event retirement | on / on (user) | ini `deferred_readback`, `stale_readback` | (pacing) | the user, Proton run 3: "seemed to help a lot" (mixed) |
| 50 | L5 drain before every queued command | | stage A | on / on | none | correctness (~37 drains/s) | no |
| 51 | Upstream `SyncWithDrawRecorder` / RecordData waits first | | stage A / B | on / on | `GOW3_RECORD_DATA_FIX` | correctness | no |
| 52 | Upstream barrier bundle: merged barrier ranges (gap 64), <= 128 image barriers per call, layer merge (opt-in) | | stage B; GPU | on / on (merge off) | `GOW3_TRACKER_MERGE_GAP`, `GOW3_BARRIER_MERGE` | hitch work (4 cores: worst 31 -> 18 ms, upstream) | no |

### 1.4 Guest memory sync, uploads, faults

| # | Patch | What | Thread / resource | PC / Deck | Switch | PC gain | Deck? |
|---|---|---|---|---|---|---|---|
| 53 | D2 + D2b precise CPU writes | bitmap check in every guest `memcpy` / `memset` / `memmove`; hits go to the GPU side | game threads; 32 MB sparse bitmap | on / on | w1:2^63; `GOW3_PRECISE_WRITE_RELEASE` | downloads 27/s -> 0; stddev 1.1-1.5 -> 0.3-0.5 ms | rate (0-3.8/s) |
| 54 | GPU write twins (16 KiB) | | fault path | on / on | `GOW3_GPU_WRITE_TWINS(_MAX)` | light-view stddev 0.56 -> 0.06 ms | rate (2 entries) |
| 55 | N-13 write fault without unrelated waits | | fault path | on / on | `GOW3_FAULT_PAGE_DOWNLOAD` | 127-158 -> 0.1-0.6 ms/s of waits | rate (0) |
| 56 | Fault window (256 KiB unprotected around a write fault) | fewer faults, more pages marked for upload | game threads; stage B uploads | on / on | w1:65536; `GOW3_FAULT_WINDOW` | +22% (base port) | rate (~1 real fault a frame) |
| 57 | C3 undefined targets after an unmap | | stage B; uploads | on / on | w1:2^61 | spikes 19 -> 7-12 ms | no |
| 58 | N-11 new images undefined | | uploads | on / on | `GOW3_NEW_IMAGES_UNDEFINED` | 4K target uploads gone | no |
| 59 | N-24 park targets an unmap frees (<= 768 MiB) | | memory | on / on | `GOW3_PARK_UNMAPPED` | "big spikes gone" (user) | no |
| 60 | V3 volumes through 2D array views | | GPU; uploads | on / on | w1:2^58; `GOW3_VOLUME_VIEWS` | lows ~60 -> ~100 | no |
| 61 | V4 stencil plane writes | | GPU copy | on / on | w1:2^59 | (correctness) | no |
| 62 | D1 unmap discard | | | on / on | w1:2^56 | never fires | no |
| 63 | HZB small mips rendered (N-22) | | GPU; culling | on / on | `GOW3_MICRO_MIP_TARGETS` | lows "significantly higher" | no |
| 64 | **U2** direct arena writes | arena in `DEVICE_LOCAL\|HOST_VISIBLE`; use ticks noted per `ObtainBuffer` | stage B; arena memory type | off / **on (Linux iGPU)**, user runs 0 | `GOW3_ARENA_HOST_VISIBLE` (start), w2:8 (direct writes only) | Deck-designed | **A/B: 62-63 FPS both ways** [deck-u2-ab-1] |
| 65 | Hot pages (rejected), userfaultfd (rejected) | | | off / off | `GOW3_HOT_PAGES`, w1:32768; `GOW3_UFFD` | 33 FPS / no change | uffd: **A/B worse** (69.0 -> 62.4) |
| 66 | Async fences (rejected for the guest) | | | off / off | `GOW3_ASYNC_FENCES`, w1:262144 | 79 vs 83 (not MediEvil) | no |

### 1.5 Runtime (HLE, guest memory)

| # | Patch | What | Thread / resource | PC / Deck | Switch | PC gain | Deck? |
|---|---|---|---|---|---|---|---|
| 67 | R2 + R3 rwlock hash set, pool free-page hint | | game threads | on / on | none | +17% median FPS | no |
| 68 | Lock-free region reads + L2 bucket caches | seqlock reads; spin up to 400k TSC cycles through an edit | stage A / B | on / on | `perf_lockfree_memory` (`GOW3_OPTIMISTIC_CLAMP` live, `GOW3_REGION_BUCKETS` restart) | stage A 13.4 -> 7.1% in CollectRingBindings; "best it's been" | no |
| 69 | M1 / M1b kept pool views, decommit keeps access | | | **Windows only** / n/a | w1:2^31 (Windows), `perf_cheap_frees` | trim episodes gone; decommits 320 -> 41 ms / 5 s | n/a |
| 70 | **D4** pool maps outside the table lock (Linux) | | game threads; mmap_lock | n/a / on | w2:256 | Deck-designed | **no** |
| 71 | **D5** table rwlock spin (1,000 pauses), empty hook flush | | all | n/a / on (spin); both (flush) | w2:512 | Deck-designed | **no** |
| 72 | **D2 (Deck)** cached `scePthreadGetthreadid` (Linux) | | game threads | n/a / on | none | Deck-designed | no |
| 73 | Deck opt-ins: pool prefill / populate, core-scheduling cookie, nice (`impl-deck-build-sched-pool.md`) | | | off / off | `GOW3_POOL_PREFILL_MB`, `GOW3_POOL_POPULATE`, `GOW3_STAGE_B_CORE_SCHED`, `GOW3_THREAD_NICE` | Deck-designed | no |
| 74 | Dedicated stage B core (rejected on PC) | | | off / off | `GOW3_STAGE_B_CORE` | slow frames unchanged | confounded (rescore A3) |

### 1.6 Presentation and pacing

| # | Patch | What | Thread / resource | PC / Deck | Switch | PC gain | Deck? |
|---|---|---|---|---|---|---|---|
| 75 | **Precise vblank tick at 960 Hz** | fixed grid; `sleep_until(slot - 700 us)` then a pause spin | vblank thread spins ~0.63-0.67 of a CPU; Windows: TIME_CRITICAL + MMCSS | **on** / on by default, **off in the user's ini** | ini `present_precise_tick`, `present_tick_hz`; `GOW3_PRECISE_TICK`, `GOW3_TICK_SPIN_US` | 1% lows 118-119 at a 120 cap (T-P120) | Proton run 4: off gave "completely flat 60" (user) |
| 76 | Present on the vblank thread | | vblank thread | on / on | ini `present_inline` | the two essential settings with #75 | no |
| 77 | Reflex limiter; **precise limiter + hold + present priority** as its fallback | AMD / Intel / RADV get the precise limiter: slot wait spins 700 us | vblank thread (Windows TIME_CRITICAL) | Reflex (NVIDIA) / precise fallback, limit 0 in the user's runs | ini `present_limiter`; `GOW3_PRECISE_LIMIT`, `GOW3_LIMIT_HOLD`, `GOW3_PRESENT_PRIORITY`, `GOW3_PACER_SPIN_US` | lows up to 118 at 120 | no |
| 78 | Auto frame limits for VRR | | | on / on | ini `fps_limit`, `display_vrr` | flicker gone | no |
| 79 | frames_queued 1 (default) | | stage B flip wait | 1 / 1 | ini, `GOW3_FRAMES_AHEAD` | 2 hurt the lows | no (rescore A1) |
| 80 | **E1 bundle** (opt-in env, used on the Deck): early frame EOP, decode pacing, OnSubmit on stage B, display-label fast path | | stage A -> stage B | off / user on | `GOW3_EARLY_FRAME_EOP`, `GOW3_DECODE_PACING(_LEAD)`, `GOW3_ONSUBMIT_STAGE_B`, `GOW3_VO_LABEL_DRAIN` | Deck-designed | **A/B**: D0-D2 and E1 (FPS 64-65 all) |
| 81 | Drain sleep, GPU-ordered fences (opt-in, rejected) | | | off / off | `GOW3_DRAIN_SLEEP`; `GOW3_GPU_FENCES` | n/a | **A/B**: 60.7 vs 64.0; 41.2 vs 64.0 |

### 1.7 MediEvil-specific (UE4 config, eboot patches, hooks)

| # | Patch | What | Resource | PC / Deck | Switch | PC gain | Deck? |
|---|---|---|---|---|---|---|---|
| 82 | HZB occlusion through the remap hook + buffered queries 3 | culls; readback copies every frame | GPU readbacks; game thread ~100 us a map (Deck); stage B retirement | on / on | `perf_hzb_occlusion`; `GOW3_HZB_REMAP` | 130 -> 160-178 FPS | **A/B**: "worse without it" (plan-linux-deck.md:1041-1042) |
| 83 | No forced depth prepass (`r.DBuffer=0`, `r.EarlyZPass=1`); occlusion queries off where the remap cannot run | ~600 / ~150 fewer draws | stage B, GPU | on / on | `perf_no_decal_prepass`; `GOW3_DECAL_PREPASS` | best lows then; +5% | no |
| 84 | Effects resolution 75 (separate translucency), fixed local / sun shadow sizes per preset, preshadows live (on), intro movie suspended, GPU particle bounds (xml, on), single particle pass (off), finer occlusion culling (xml, off, Deck), 512 MB framebuffer pool (xml, on: applied at 720p too) | | GPU; draws; memory | as listed | ini `effects_resolution`, `preshadows`, `perf_particle_single_pass`; xml | death effects 2.4-2.9 ms GPU -> <= 0.09; preshadows off ~130-150 draws; movie uploads 90 MiB/s -> 0 | no (finer HZB: planned) |
| 85 | **Forced 16x anisotropic filtering** | every trilinear sampler gets 16x | GPU texture bandwidth | on / on | `GOW3_FORCE_ANISO` (restart) | none measured (a quality choice) | no |

Rows grouped (#84 holds several small MediEvil items; #65, #66, #81 hold rejected ones). Counting each switchable item
once gives the 85 of the short answer; the remaining `GOW3_*` names are diagnostics, cheats, input, the upscaler / TAA
(off for MediEvil: `GOW3_UPSCALER=none`, so word-1 bits 20-29, 47-50, 51-55, 57 are inert here [code:
`run.py`; measured: "reduced-size draws 0/frame" in every Deck log]) or the Windows hitch bundle (timer resolution,
power throttling, futex condition variables: Windows-only paths [code: `src/compat_win.c:123-128`,
study-deck-other-threads.md 2.1]).

## 2. Scores

Class: **(a)** likely helps on the Deck too; **(b)** neutral; **(c)** likely costs more than it saves on the Deck or
slower hardware; **(d)** unknown, needs a measurement. "GPU" / "low-core" flag effects for a slow GPU or a 4-6 core CPU
apart from the Deck.

| # | Class | Deck cost model and estimate | Slow GPU | Low-core CPU |
|---|---|---|---|---|
| 1 | a | the only measured 4c/8t number is +18% [measured: parallel_gpu.md:377]. Stage A's yield-spin drain costs power and stage B's sibling (see #81, rescore A8) | - | **below 8 threads the pipe is off** (`>= 8`, `vk_rasterizer.cpp:454`): a 6c/6t or 4c/4t CPU never gets the +18%; never measured there (d) |
| 2 | a | 63% of a CPU is real RADV recording; recording on stage B would put it on the wall (other-threads 3.1) | - | - |
| 3 | **d** | 4 workers ~0.76 CPU (~8 us of worker CPU per prepared draw) [measured, capped clock: other-threads 3.2]. They share stage B's SMT sibling and are preempted 1.7k/s. One mutex is shared with stage A and the submit thread. **But an unprepared draw cannot use the texture-set memo** (`BindTexturesFromSet` returns at `!prepared`, `vk_rasterizer.cpp:2985-2987`) and binds on the slow path, ~15 us a call on the Deck [measured: per-draw study C3, ~70 a frame, 1.1 ms]. Each 1% of draws unprepared is ~14 draws x ~14 us = **~0.2 ms a frame** [inferred]. 97.8% prepared on the Deck vs 99.6% on the PC [measured: E1]. Worker 2: 90% prepared (confounded run) [measured: deck-helpers-run1] = up to ~1.5 ms more slow binds [inferred]. The "stage B 8.4-8.8 us/draw, the same as with 4" reading in other-threads 3.2 does not settle it: that figure is the Frame stats "GPU thread us/draw", which is **stage A's** CPU time (`getrusage(RUSAGE_THREAD)` on the GPU command thread, `amdgpu/liverpool.cpp:1980-1985`; printed at `gpu/shadps4/core/libraries/videoout/driver.cpp:662-669`). The toggle w1:8192 only stops consumption, not the workers (`vk_draw_prep.cpp:301`) | - | Windows: THREAD_PRIORITY_IDLE workers = threads/2; same trade |
| 4 | a | moves memcpy to stage A, which waits 25-30% [measured: E1, D2] | - | - |
| 5 | a | keeps label writes off stage B | - | - |
| 6 | a/b | 3.15 MB of uploads a frame [measured: D2 "staging uploads"]; done inline they would land on stage B. 2 normal-priority threads compete only in bursts | - | 1 copy thread at < 8 threads |
| 7 | d | rescore A2 | - | - |
| 8 | a (steady), d (hitches) | idle in steady play [measured: "compile queue 0"]. 4 + 2 threads compete with stage B in first-visit hitches; `High` priority is a no-op on Linux (`SetCurrentThreadPriority` passes a priority to SCHED_OTHER) [code: `gpu/shadps4/common/thread.cpp`] | - | Windows: capture threads ABOVE_NORMAL |
| 9 | b | it fixed an NVIDIA / Afterburner driver lock; on RADV the frees are ioctls with no monitor lock; workers idle | - | - |
| 10 | b | downloads ~0/s on the Deck [measured: D2] | - | - |
| 11 | c (off: keep off) | rescore A5 | - | - |
| 12-20, 22-24, 26-27 | a | each removes stage B work; the Deck's 4-5x per-part cost scales their savings up [inferred]. Tables are small (<= ~1.2 MB) | - | - |
| 15 | a+ | the query is an ioctl on RADV, so the 1-in-32 limit saves more than on NVIDIA [code: per-draw study 3] | - | - |
| 21 | d (small) | wider read ranges "only add barriers, never drop one" [code: `vk_runtime.cpp:960-964`]; barriers are full cache flushes on RADV [inferred: plan L6]. Likely < 0.1 ms either way | barrier cost on AMD | - |
| 25 | a (memo) / **c (layout)** | the memo is essential (99.3% hits; a miss ~15 us). The table is 32,768 x ~1.55 KiB = ~52 MB, hashed, so ~2,800 lookups a frame land on different pages. A hit costs **0.851 us/packet, 5.7x the PC** (other parts 4-5x) [measured: per-draw study section 1; size: memory-budget 2.9]. The 32,768 slots (from 8,192) were +1.3% on the PC's 96 MB L3 [measured: parallel_gpu.md:504-505]. The Deck's misses are all "other key" (1,683 per 5 s) [measured: D2]. **Fix: a dense index** (key + slot, ~0.5 MB) in front of the entries, not fewer slots: 4,096 slots would add conflict misses at ~15 us each [inferred]. Estimate **0.1-0.35 us/draw = 0.15-0.5 ms a frame** [inferred, memory-budget 2.9] | - | any small-L3 CPU (non-X3D desktops) pays the same TLB / DRAM misses |
| 28, 29 | a | Deck: 1,277 pushes at 17.7 writes a frame [measured: D2] | - | - |
| 30 | a/b | 1 MB direct-mapped, ~2,300 lookups a frame: likely L2 misses on Zen 2 (~0.1-0.3 ms) but far cheaper than the hash-map clears it replaced [inferred]. Could pack to 8 B entries | - | small-L3 |
| 31 | a | 59% of small buffers from the arena on the Deck [measured: E1]; real write faults ~1 a frame [measured: run 8], so the "upload once, fault on rewrite" premise holds | - | - |
| 32 | a/b | on UMA the PCIe saving is gone, but 8.75 MB a frame of stage A memcpy and LPDDR5 traffic is still saved [measured: D2 "8.75 MB more reused"]. Stage A has slack, so FPS ~0 [inferred] | - | - |
| 33 | b | MediEvil's frames show no copy-shader dispatches in the PC profiles [inferred: fix-inventory has none]; `< 8` copies take the old path | GPU-side by design | - |
| 34 | a | build only | - | - |
| 35-38 | **d** | Deck-designed, never run: the planned launch (section 3.1). C1 risk: readbacks retire up to 100 us later (HZB age) | - | lean (#36) also turns on for any <= 8-thread Windows PC: never run there (d) |
| 39 | d | planned static vs shared A/B (impl-deck-cpu-bundle.md G) | - | - |
| 40, 41 | d (opt-in) | planned (#4 with `deck.sh clocks`) | - | - |
| 42 | a (a, c parts) / **d (b part) on RADV** | (a) fewer barriers helps any driver. (b) samples depth in `DepthReadOnlyStencilAttachment`: on RADV that layout's HTILE handling is the RADV-slow-pass suspect (plan L6). spacepepperoni reverted their (b) for a hang, while ours keeps (b) on [doc: study-fork-spacepepperoni.md section 2 row 9]. The Proton run 1 GPU hang went with a driver update, cause unknown [measured: plan-linux-deck.md:598-617] | **AMD drivers: d** | - |
| 43 | d (small) | measured -0.8% GPU on NVIDIA only; RADV unknown | AMD: d | - |
| 44 | b | correct and free | - | - |
| 45, 46 | a likely / **d** | the alternative waits for every host copy (~21% of stage B on the PC) [measured: T-P3]. But the targeted check takes `host_copy_sources_mutex` once per queued readback range inside each of ~200 events a frame, and those events cost 17-26x on the Deck [code + measured: per-draw study 3]. Probably still a net win; a cheap live check | - | - |
| 47, 48, 50-52 | b | correctness or small | - | 52: upstream measured its gain on 4 cores |
| 49 | a | stale readbacks remove waits; the per-event semaphore query they cause is C1's job (#35) | - | - |
| 53 | b | precise writes ~0-4/s on the Deck [measured: D2]. The steady cost is one bitmap word load per guest `memcpy` on game threads (`src/runtime_memory.c:1529-1540`): a likely L1 / L2 hit [inferred]. (Adjacent, not a patch: `memory_calls` is a shared atomic incremented by every guest memcpy, `src/runtime.c:164-180`, rescore C16) | - | - |
| 54, 55, 62 | b | rarely fire on the Deck (twins 2 entries, downloads 0) [measured: D2] | - | - |
| 56 | **d** (low) | the "write faults" counter (~760 pages a frame) counts pages marked, and the uploads (3.15 MB a frame ~ 770 pages) match it [measured: D2, run 7 correction]. A 256 KiB window marks 64 pages per real fault. Real faults are ~1 a frame, so the window adds at most ~64 pages of possible re-upload; most marked pages come from pool commit / decommit invalidations [inferred]. A Linux fault (signal + mprotect) costs 7-57 us [measured: faultprobe, run 3]. Probably still right; check live | - | - |
| 57-61, 63 | a | each removes uploads, rebuilds or draws; at 720p the targets are 1/9 of 4K, so the byte savings shrink, but stage B work per event does not [inferred] | 60, 63: fewer GPU passes / draws | - |
| 64 | **c** | **zero measured gain** (62-63 FPS both ways, stage B ~80.7% both; uploads are not where stage B's time goes) [measured: deck-u2-ab-1]. With the arena mapped, every `ObtainBuffer` (3-4 a draw, ~5,000 a frame) runs `NoteArenaUse`: a walk of a 3-level use-tick table with an atomic max per page (4 KiB pages up to 64 KiB, 64 KiB pages for larger ranges such as whole vertex pools) [code: `buffer_cache.cpp:114-173`, `:1322-1325`]. **Toggle w2:8 does not turn this off** (it gates only the direct write); only `GOW3_ARENA_HOST_VISIBLE=0` at start does. Estimate 0-0.3 us/draw (mostly cache-hit loads once a page holds the current tick) [inferred]. Default on for Linux integrated GPUs (`buffer_cache.cpp:633-640`) | - | - |
| 65, 66 | c (off: keep off) | measured worse or no use (rescore A9-A11) | - | - |
| 67 | a | game threads; the per-call saving grows on a slower core [inferred] | - | - |
| 68 | a / d (tail) | readers skip the lock. During a table edit a stage B reader spins up to 400k TSC cycles (~140 us at the Deck's ~2.8 GHz TSC [inferred]) before locking (`src/runtime_memory.c:1640-1660`). Edits are ~4,400/s at 2.2 us each on the PC [measured: T-M1], slower on Zen 2 [inferred]. Region caches answer most calls, so the cost is a tail | - | - |
| 69 | n/a on Linux | the Linux analogue is D4 / L2 (rescore D4) | - | - |
| 70-72 | d | planned (impl-stage-b-bundle-2.md); the D5 spin is 1,000 pauses (~20-40 us) per contended lock on a busy 4-core part [inferred] | - | - |
| 73, 74 | d (opt-in) | planned / rescore A3 | - | - |
| 75 | **c** | 700 us spin of every ~1.11 ms tick at 900 Hz = **~0.63 of a logical CPU**, often a sibling of stage B. On a shared ~15 W budget the spin also costs clock (plan L1; apu deep dive 6.2) [code: `gpu/shadps4/core/libraries/videoout/driver.cpp:1368-1383`, `:1465-1467`, `:1256-1273`]. Under Proton, turning it off gave a flat 60 [measured: Proton run 4, the user]. The user's native runs have it off ("VideoOut: vblank 900 Hz", no "precise vblank tick" line). The default is on (`scripts/graphics_options.py:285-287`). Even off, the 900 Hz plain tick wakes the vblank thread 900/s and the game's `RHIFrameOffsetThread` ~844/s [measured: D2 "ident 7: 4,221 calls / 5 s"]. Small, but it preempts SCHED_IDLE workers; "tick = cap = panel refresh" (apu deep dive #3) removes both | spin costs GPU clock on an APU | **Windows: the vblank thread is TIME_CRITICAL + MMCSS** (`GOW3_PRESENT_PRIORITY=1` from the Reflex / precise preset, `driver.cpp:107-130`): a ~2/3-duty time-critical spinner on a 4-6 core CPU |
| 76 | b | presents on the vblank thread; the present is 0.94 ms on the Deck [measured: run 4] | - | - |
| 77 | **c (capped), b (uncapped)** | with a cap, each frame's slot wait ends in a pause spin of `GOW3_PACER_SPIN_US` (700) after a `sleep_until`. At 60 FPS that is ~4% of a CPU, and on Windows the thread is time-critical [code: `driver.cpp:1361-1366`, `:1588-1603`]. On Linux `sleep_until` is precise to ~50-100 us [inferred: plan L1], so 700 us is ~7x too long. Zero with limit 0 (the user's runs) | - | the same spinner on 4-6 cores |
| 78, 79 | b | - | - | - |
| 80 | **d** (OnSubmit part) | E1 moved `OnSubmit` (texture GC, downloads, `TickFrame`) onto stage B, the wall: stage B 83 -> 91% busy, FPS unchanged (64.5-64.9 vs 64-65) [measured: plan-linux-deck.md:1035-1038]. The rest of E1 removed stage A gates. E0 (E1 with `GOW3_ONSUBMIT_STAGE_B=0`) was planned and never run (impl-decode-pacing.md:347) | - | - |
| 81 | c (opt-in: keep off) | measured (rescore A8 asks for a post-E1 re-test of drain sleep) | - | - |
| 82 | a | measured on the Deck. Its readbacks drive #49 / #35 | fewer draws | - |
| 83 | **a+** | ~600 + ~150 draws a frame on the PC [measured] are 4-5 ms of Deck stage B at ~8.5 us a draw [inferred] | fewer passes | - |
| 84 | a (most) / b | effects 75 and shadows per preset cut GPU work. Preshadows on is a setting (the Deck preset turns it off). GPU particle bounds adds draws for 8 of 180 emitters [measured: T-0102-5 survey], small, unmeasured on the Deck. The 512 MB framebuffer pool at 720p is reserved guest memory, touched lazily on Linux [inferred] | - | - |
| 85 | **c** | **it saves nothing**: it is a quality item ("Cheap on PC GPUs", `texture_cache/sampler.cpp:15-22`). It costs extra texture fetches on oblique surfaces (the graveyard's ground) on an 8 CU RDNA2 at 0.8-1.2 GHz, 85-95% busy, sharing LPDDR5 with the CPU. Estimate **2-8% of GPU time** where the GPU limits [inferred]. At 67% render scale, where stage B is the wall, it still matters: GPU work saved returns power budget to the CPU [inferred: platform study 8.5]. At 100% / with the finer-HZB patch, the GPU is the wall | **c** for any low-end GPU (the Low preset does not turn it off) | - |

**(a) or (b) everywhere, with no Deck-specific mechanism found:** #2, 4, 5, 6, 9, 10, 12-20, 22-24, 26-34, 44, 47-52, 54,
55, 57-63, 67, 76, 78, 79, 82-84. **(c):** #25 (layout), 64, 75, 77 (capped), 85; keep-off: #11, 65, 66, 81. **(d):** #1
(low-core), 3, 7, 8 (hitches), 21, 35-41, 42 (b part), 43, 45-46, 56, 68 (tail), 70-74, 80.

## 3. Test plan

Rules: one change per launch, each launch with the user's go, re-read `medievil.ini` before it. Toggle words are
decimal; the file is "W1 W2". `deck.sh env` replaces extra-env.txt, so every `env` line below carries the baseline keys.
**Baseline keys (E1 + stats):** `GOW3_FRAME_STATS=1 GOW3_TOGGLE_FILE=/home/deck/.local/share/medievil-pc/toggles.txt
GOW3_EARLY_FRAME_EOP=1 GOW3_DECODE_PACING=1 GOW3_DECODE_PACING_LEAD=2 GOW3_ARENA_HOST_VISIBLE=0`.

Taken bits: **word 1: all 64 are assigned** (`gow3_toggles.h:25-87`; bits 20-29 are raw motion / upscaler debug
toggles: never set them; bit 31 on Linux switches narrow stages only, `src/runtime_memory.c:1175-1180`). **Word 2:
bits 1-1024 are assigned** (64 reserved); 2048 and up are free. Every bit of both words is an **off-switch** for a
default-on item, except w2:1024 (switches off an opt-in spin, only with `GOW3_STAGE_B_SPIN=1`), w1:524288 (turns on
an alternative path, rescore A2) and w1:32768 / w1:262144 / w1:2^34 (only act when their opt-in env is set).

### 3.1 Live A/Bs (no rebuild; `deck.sh ab "A" "B" 20 4`, ~3 min a pair; summary by state with `deck.sh summary NAME --min-draws 1000`)

| Order | Items (#) | A | B | Read |
|---|---|---|---|---|
| L1 (planned launch, plan-linux-deck.md:1103-1106) | C1 (#35) | `0 0` | `0 16` | "Stage B syscalls" (queries a frame, us each), FPS, stage B busy, HZB age |
| L1 | lean (#36) | `0 0` | `0 32` | "Recorder per frame", wakes, ident-64 wait, fence lag |
| L1 | C5 (#37) | `0 0` | `0 128` | "Readback pre-check" |
| L1 | D4 (#70) | `0 0` | `0 256` | "Region table per frame", game-thread waits |
| L1 | D5 (#71) | `0 0` | `0 512` | the same line, blocked ms |
| L2 (same launch if time allows, else the next) | **screen: barrier / GPU items** #43 + #21 | `0 0` | `2147485696 0` (2^31 + 2048) | FPS (the GPU is near its limit at 67%), "all pass ends", barrier batches. If it moves, split: `2147483648 0`, `2048 0` |
| L2 | **screen: targeted waits** #45 + #46 | `0 0` | `5764607523034234880 0` (2^60 + 2^62) | stage B busy, "blocked: host copies", "WriteData targets ... waited". If B wins, split `1152921504606846976 0` / `4611686018427387904 0` |
| L2 | **screen: copy / record helpers** #22 + #6 (parallel copies) | `0 0` | `4295098368 0` (2^32 + 131072) | stage B busy, "copy threads" blocked % |
| L2 | ring reuse #32 (confirm (a/b) on UMA) | `0 0` | `0 4` | stage A waited %, "Mapped writes" |
| L2, last and short (`20 2`) | fault window #56 | `0 0` | `65536 0` | "Fault handlers ... ms/s", "write faults/s", uploads MB/s; stop if the fault time climbs past ~50 ms/s |

If the user runs with the #4 spin (`GOW3_STAGE_B_SPIN=1`), add 1024 to every second word (A `0 1024`, ...).
Not worth a live test: word-1 bits that only gate stage B memos and caches (#12-20, 23, 24, 26: removing any of them
can only add stage B work); 8192 / 4096 (they disable consumption but leave the workers running, so they do not
measure #3's cost); 2^63 (precise writes ~0/s on the Deck; off would bring back downloads with full waits).

### 3.2 Restart-only (one launch each, against L1's A windows in the same spot)

| Rank | Item (#) | `deck.sh env` (baseline keys + ...) | Expected | Read |
|---|---|---|---|---|
| 1 | **Prep workers** (#3; rescore A4, with the slow-bind caveat) | `GOW3_PREP_WORKERS=3`, then `=2` | -1.5 to +0.7 ms of stage B [inferred] | "Draw preparation" prepared share; **"slow texture binds per 1000 draws": unprepared draw** (the cost side); FPS, stage B busy; `deck.sh threads` (DrawPrep CPU, involuntary switches). If 3 loses, the next idea is the opposite: workers off SCHED_IDLE (nice 10-19, other-threads cut D), which needs a build |
| 2 | **U2 default** (#64) | the baseline keys **without** `GOW3_ARENA_HOST_VISIBLE=0` (= the shipped default), and inside the launch `ab "0 0" "0 8"` (direct writes on / off; bookkeeping stays on in both) | stage B 0 to -0.3 us/draw better with the key = 0 [inferred] | stage B busy vs L1, "Arena writes per frame"; the in-launch pair separates direct writes from bookkeeping. If not better than L1: make the default 0 |
| 3 | **16x AF off** (#85) | `GOW3_FORCE_ANISO=0` | GPU 2-8% [inferred]; FPS gain only where the GPU or the power budget limits | FPS; the user's overlay GPU % and clock, CPU clock (`deck.sh clocks 20`); a screenshot of the graveyard ground. Repeat at 100% render scale (GPU-bound) for the clean number |
| 4 | **E0: OnSubmit back on stage A** (#80) | `GOW3_ONSUBMIT_STAGE_B=0` | stage B busy -1 to -5 points; FPS -2 to +2% [inferred] | stage B busy, "stage A waits ... OnSubmit", ident-64 wait, frame lead |
| 5 | **Lean barriers (b) on RADV** (#42) | `GOW3_DEPTH_SAMPLE_IN_PLACE=0`, then `GOW3_SKIP_READ_BARRIERS=0` | GPU-side, unknown sign | FPS at 100% render scale (GPU-bound), "all pass ends"; `GOW3_GPU_PROFILE=1` per-pass GPU ms if it moves |
| 6 | Copy threads (#6) | `GOW3_COPY_THREADS=1` | +-0.2 ms [inferred] | "blocked: copy threads", stage B busy |
| 7 | Drain sleep re-test after E1 (#81, rescore A8) | `GOW3_DRAIN_SLEEP=1` | -0.5 to +0.5 ms | as rescore A8 |
| 8 | Precise tick / tick rate (#75), only to size the default's cost | ini `present_precise_tick=1` (back it up first) | -2 to -10% FPS [inferred] | FPS, `deck.sh threads` (the vblank thread's CPU). Better: settle "tick = cap = panel refresh" (apu deep dive #3) and make the precise tick default off at <= 8 threads |

Needs a build first: the **texture-set memo layout** (#25): a restart switch for the slot count and, better, a dense key
index (`GOW3_TEXSET_SLOTS`, say). Read "texset" us per draw with `GOW3_SECTIONS=1 GOW3_SLOW_FRAMES=1.4` and the miss
split; pair it with the platform study's M0 dTLB counters on `gow3:DrawRec`.

Expected payoff order (Deck): L1 (planned, Deck-designed defaults) > rank 1 (largest two-sided swing) > the memo
layout build > rank 2 (free if it confirms) > rank 3 > L2 screens > rank 4 > ranks 5-8.

### 3.3 Also worth a PC A/B on the slower machine (5800X3D + 6900XT, Windows, AMD driver)

- **#75 + #77 (spinning, time-critical present threads):** no Reflex on AMD, so the preset falls back to the precise
  limiter with `GOW3_PRESENT_PRIORITY=1`. A/B `present_precise_tick` 1 vs 0 at a cap. Then repeat with the process
  restricted to 8 logical CPUs (`start /affinity FF`). That gives the Deck's thread policy (draw pipe on, 4 prep workers,
  2 copy threads, recorder lean on), but not its L3 or clocks. A run below 8 CPUs switches the draw pipe off: set
  `GOW3_DRAW_PIPE=1` for any affinity test below 8.
- **#42 (b), #43, #21 on an AMD Windows driver:** the same live / env pairs as 3.1 L2 and 3.2 rank 5; a GPU-bound spot
  at 4K.
- **#35 C1 on AMD Windows:** `vkGetSemaphoreCounterValue` may be a kernel call there too [inferred]; the "Stage B
  syscalls" line answers it in one live pair.
- **#36 recorder lean** at 8-CPU affinity (on there automatically); never run on Windows.
- **#85 16x AF** at 4K native on the 6900XT: the clean GPU-cost number for the AF question.
- **Not testable there:** #25 (the memo layout) and #30. The 5800X3D's 96 MB L3 hides them as the 7800X3D does. A
  non-X3D CPU (16-32 MB L3) would show them.
- **Low-core flag #1:** a 6c/6t or 4c/4t CPU runs without the draw pipe (threshold 8). Affinity to 6 CPUs with
  `GOW3_DRAW_PIPE=0` vs `=1` would tell whether the threshold should be 6.

## 4. Notes

- **Baseline hygiene:** the E1 baseline had `GOW3_ARENA_HOST_VISIBLE=0` (U2 off) and the E1 env keys; the deployed
  default differs in U2 (on) and in everything built since (C1, lean, C5, D4, D5, static library, #6). A first launch of
  the new build without `GOW3_ARENA_HOST_VISIBLE=0` would mix U2's bookkeeping into "the new baseline".
- **"GPU thread us/draw" in the Frame stats line is stage A's CPU time** (the GPU command thread's rusage, `amdgpu/liverpool.cpp:1980-1985`,
  `driver.cpp:662-669`), not stage B's. The drain-sleep run's lower value (7.6 vs 8.8) is stage A sleeping instead of
  yield-spinning, not cheaper draws. Use "stage B busy" and sections for stage B.
- **All Deck numbers so far carry `GOW3_FRAME_STATS=1`** (shared counters, timers on several paths). Smoothness
  judgements need a run without it (fix-inventory "Judge smoothness only from runs without the diagnostics").
- Everything with "Deck-designed" in section 1 came from earlier Deck research and is listed for completeness. Its
  tests are in the impl-* docs; this page adds only the ordering.
