# Plan: port the Bloodborne port's performance work (threading, waits, memory)

Status: researched 2026-10-18 (read only, nothing measured in this port), nothing built.
Parent: [plan-roadmap.md](plan-roadmap.md). Read [gpu-threading-reference.md](gpu-threading-reference.md) before
changing anything here.

## Answer

This port forked from Bloodborne v0.2 (`0afaaa2`, 2026-10-04) and still uses Bloodborne 0.2's memory model and
single Vulkan recorder. Bloodborne's 0.3-0.51 commits (`4d4d208` 0.3, `daa7165` 0.4, `39940f2`, `af9e66d`,
`f2cd4ca`, `10baf9d` 0.5-pre4, `c7e3e55` 0.5, `3c57da4` 0.51) are not in. Neither port parallelises PM4 decoding or
draw translation (stage A / stage B); Bloodborne's notes say heavy scenes stay draw-recording bound
(`bloodborne_pc/docs/CHANGES_2026-10-06.md:15-22`). **Profile first:** the cheap wait/memory fixes below target stage
A and stalls; parallel recording only helps if `gow3:VkRecorder` or CPU/GPU serialization is the limit.

## Threads today (both ports)

`GpuCommandProcessor` (stage A, PM4, `liverpool.cpp:81,106`) → SPSC ring → `DrawRec` (stage B, `vk_draw_pipe.h`)
with `DrawScan` / `DrawPrep0..N` (`vk_draw_prep.cpp`), `TexBind`, `Copy0..N` (`gow3_copy.cpp:35`),
`PipelineCacheIO`, present/swap (`driver.cpp`). This port only: one `gow3:VkRecorder` (`vk_scheduler.cpp:30-45,276`),
async shader recompile + pipeline creation (`vk_async_compiler.h`, `adde71e`), parallel cache warm-up.

## Candidates (ranked; claimed gains are Bloodborne's own numbers)

| # | Item | Bloodborne source | Claimed gain | Here today | Windows | Risk |
|---|---|---|---|---|---|---|
| 1 | Targeted host-copy waits: `WRITE_DATA`/DMA wait only for copies reading that range | `daa7165` `vk_scheduler.cpp:389-440`, `liverpool.cpp:524,586,633` | waits 37→2/frame, 158→167 FPS (CHANGES_0.4) | No: `liverpool.cpp:329` `WaitHostCopies()` | Low | Low-med; keep a wait-all fallback switch |
| 2 | Buffer readback on its own queue + GPU-write twins | `4d4d208` `buffer_cache.cpp:167-289, 703-785, 853-940`, `vk_instance.cpp:436-455` | removes ~frame-long (20-30 ms) stalls | No: `buffer_cache.cpp:184-222` `scheduler.Finish()` | Low-med (`ReadBacking` → section backing view) | Med (deadlock with Precise readbacks, CHANGES_after_0.4:266) |
| 3 | Residency in 64 MiB blocks, background pre-allocation, idle release, OOM spill | `4d4d208`/`39940f2`/`3c57da4` `buffer_cache.cpp:1464-1920` | ~8 ms burst allocations gone; VRAM 7.3→4-5.5 GB | No: `buffer_cache.cpp:469-523` per-burst, never freed, aborts on OOM | Med (WDDM allocation cost, 4096 allocation limit) | Med (eager release → 90→55 FPS without re-promotion) |
| 4 | Texture collector fixes + 64 MiB VMA blocks | `4d4d208`/`c7e3e55`/`3c57da4` `texture_cache.cpp:1175-1300`, `vk_instance.cpp:792-805` | stops collector stalling, ~10 GB growth, re-upload storms | No: `texture_cache.cpp:1438-1470` old bug | Low | Low. **Do not take** ≥4 MiB dedicated allocations (this port's NVIDIA sub-allocation `image.cpp:120-170` is faster for UE4) |
| 5 | Targeted ConstRam dump waits | `4d4d208`+`daa7165` `vk_rasterizer.cpp:870-900`, `liverpool.cpp:784-795` | stage A waits 30%→1.5% (16 threads) | No: `liverpool.cpp:428-436` drains every dump | Med | **High** (Bloodborne DoF bug #44: list vertex/index ranges too) |
| 6 | Lock micro-fixes on recording threads | `39940f2`/`c7e3e55` `memory_tracker.h`, `region_manager.h`, `texture_cache.cpp:89-100, 244-269` | part of copy-shader +6% | Partly (`memory_tracker.h:104` read path only) | Low | Low |
| 7 | Batched submissions | `4d4d208` `vk_rasterizer.cpp:380-416` | 95→100 FPS, half the submits | No | Med (needs honest labels) | Low-med |
| 8 | Parallel Vulkan recording, async submit, direct segments, presenter on recorders, GpuSignals | `4d4d208`/`daa7165` `vk_scheduler.cpp:97-805`, `vk_presenter.cpp:344` | heavy scene 95.5→115-124 | No (one recorder; `783b25a` spin lock vs Bloodborne's never-null chunk design) | Med (`gettid`, `dladdr`, `__rdtsc` shims) | Med |
| 9 | Submit-ahead | `gnmdriver.cpp:92-196` | 101→154 FPS (Yahar'gul) | No | Med, needs honest labels | High; Bloodborne-tuned |
| 10 | "PC memory model" (`BB_GUEST_IN_PLACE`) | `4d4d208`→`c7e3e55` | not clearly faster in steady scenes | No | High (dma-buf/memfd → section views + host-pointer import, unverified) | High; Bloodborne hooks |
| 11 | Memory module (non-sparse layer) | `af9e66d`/`f2cd4ca`/`10baf9d` | fixes its own regressions | No | High (needs 10) | High |
| 12 | Full-screen/indirect draws wait for their pipeline | `10baf9d`/`c7e3e55` `vk_pipeline_cache.cpp` | fewer grey frames | Check the async compiler follows it | Low | Low |
| 13 | Swapchain recreate with `oldSwapchain` | `3c57da4` `vk_swapchain.cpp` | smoother recreate | No (`vk_swapchain.cpp:72` passes `nullptr`) | Low | Low |
| 14 | (this port) per-thread memcpy counter | `src/runtime.c:165-174` global atomic on every guest memcpy/memset/memmove/memcmp | not measured | — | Low | Low |

No gain / correctness only (do not port for speed): constants in place, native copy shader, occlusion queries,
indirect guard, GPU timestamps, `MEM_SEMAPHORE`, small-mip detiling, shader-cache atomic writes, `cp_decoder`,
Linux driver cache env vars, forced 16x aniso.

Diagnostics worth porting: `BB_PASS_BREAK_TRACE` (found 92-109k render-pass breaks / 5 s), `BB_SECTIONS` (per-function
cycles on stage B), `BB_CE_STATS` / `BB_RES_STATS`, fault-site table (Windows: `CONTEXT` in the VEH +
`RtlLookupFunctionEntry` / `SymFromAddr`).

## Order (proposed; decided by P1)

1. P1 baseline profile (below).
2. T1 = #1, T2 = #2, T3 = #3, T4 = #4 + #6, T5 = #14. Each behind a `GOW3_` switch defaulting to the old path.
3. T6 = #5 only if `drains_by_reason[ReasonConstRam]` matters for UE4.
4. T7 = #8 (parallel recording) if P1 shows the recorder or serialization limits.
5. Later / maybe never: #7, #9-11.

## What to measure (before and after every stage)

Fixed route, busiest level, Engine frame rate 120; also with a 4c/8t affinity. Average FPS, p99, worst frame time;
count only gains outside run-to-run noise (Bloodborne saw ±3-10%).
- `GOW3_FRAME_STATS`: `host_copy_waits` and wait ms (`driver.cpp:516-517`); `drains_by_reason` and drain cycles
  (`vk_draw_pipe.h:102-122`); `t_resident`, `t_gc`, gc evictions/downloads; VRAM over a 15-minute run; frames over 40 ms.
- `GOW3_SAMPLE_FILE` + `scripts/symbolize_samples.py`: share in `WaitHostCopies`, `DrainDrawPipe`,
  `DownloadMemory` / `Scheduler::Finish`, `vkAllocateMemory`, region-manager locks, per thread.

## Stages (one change per launch)

| Stage | What | Verify |
|---|---|---|
| P1 | Baseline profile, no code change | Numbers in `data/`, bottleneck thread named |
| T1 | Targeted host-copy waits | `host_copy_waits` drops; FPS/p99 vs P1; no visual change (user) |
| T2 | Readback queue + twins | Stutters gone where they were; user confirms no stale/corrupt image |
| T3 | Residency blocks + release + OOM spill | No ~8 ms alloc spikes; VRAM flat over 15 min |
| T4 | Collector fixes + 64 MiB VMA blocks + lock micro-fixes | VRAM / re-upload counts; FPS over a long run |
| T5 | Thread-local memcpy counter | Sample share of the atomic |
| T6 | ConstRam targeted waits (if needed) | Drain share drops; user checks DoF / effects |
| T7 | Parallel recording + async submit + direct segments | Recorder thread no longer the limit; no GPU hangs |
