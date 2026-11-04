# Study: the Deck's APU and Linux platform, in depth, for stage B

Status: research only, 2026-11-04, branch `deck-lab`. Nothing built, launched or committed; no SSH. Parent:
[plan-linux-deck.md](plan-linux-deck.md). This page goes deeper than
[study-deck-platform-optimization.md](study-deck-platform-optimization.md) (called "the platform study" below; its items
M0-M4, R1-R6, P0-P6, G1-G5 and B1-B5 are not repeated, only qualified). Companions:
[study-stage-b-per-draw.md](study-stage-b-per-draw.md) (C1-C11),
[study-deck-other-threads.md](study-deck-other-threads.md), [study-fence-waits.md](study-fence-waits.md),
[study-vrr-present-pacing.md](study-vrr-present-pacing.md).

Marks: **[code]** read in this tree. **[log]** a number from `data/` or plan-linux-deck.md. **[web]** a source in the
Sources list (date given where it matters). **[inferred]** reasoning or estimate, not measured. Nothing here is
"verified" in the project's sense.

The yardstick (plan-linux-deck.md, E1 and run 3): graveyard heavy view, ~65 FPS, stage B (`gow3:DrawRec`) 91% busy,
~1,400 draws a frame, ~8.5 us a draw, every part 4-5x the PC's. CPU and GPU share one power budget. The GPU runs
800-1200 MHz on auto clocks and is ~90% busy. A pinned 1600 MHz GPU clock held the CPU at ~2 GHz.

## Answer

1. **Stage B is probably not running at 3.5 GHz.** This is the main new finding.
   - Data: on an OLED Deck in SteamOS's power mode (amd-pstate active, EPP `balance_performance`), a 98%-busy game
     thread sat at a **median 2.43 GHz**. David Vernet's July 2026 measurements also show that **any block longer
     than ~0.2 ms drops the core to a "sticky ~2.4 GHz plateau"** [web, LKML thread].
   - Stage B blocks several times a frame: `flip_present`, the recorder hand-offs, empty packet queues.
   - With EPP `performance` the same thread held 3.5 GHz: 1% lows +16%, p999 frame time -40%.
   - The arithmetic (section 1.2) puts stage B's 4.7x cost against the PC at either "clock 2.1x × IPC 1.3x × memory
     1.7x" (stage B at 2.4 GHz) or "clock 1.4x × IPC 1.3x × memory 2.5x" (at 3.5 GHz). **One `perf stat` of stage B's
     cycles per second decides which.** It needs no root, no build and no launch of its own.
2. **There may be a no-root route to the CPU governor.** SteamOS's `steamos-manager` user daemon exposes
   `steamosctl set-cpu-scaling-governor`, `set-cpu-boost-state`, `set-gpu-power-profile` and the TDP and GPU clock
   controls over the session D-Bus [web]. The platform study assumed EPP needs root or GameMode. Check it on 3.9.5.
3. **Microarchitecture:** the cheap code-level wins for a pointer-chasing thread on this core are:
   - fewer DRAM round trips per draw: prefetch the next packet's lookups while the current one runs;
   - hot/cold splits of the structures every draw touches;
   - `alignas(64)` on the scheduler fields that stage B, the recorder and the copy threads write (section 1.7: several
     cross-thread atomics share cache lines today [code]);
   - an SMT sibling that is idle while stage B runs. Zen 2 statically splits the reorder buffer, store queue and
     micro-op queue between busy siblings [web]. Measured here: stage B busy 79% → 70% with its sibling free [log].
     Linux **core scheduling** (`PR_SCHED_CORE`, unprivileged for our own threads) does this dynamically: no core
     taken away permanently.
4. **Huge pages, beyond the platform study:**
   - Zen 2's L1 DTLB **coalesces contiguous 4 KiB PTEs**: the kernel's Zen 2 PMU list has
     `tlb_reload_coalesced_page_hit/miss` [web].
   - Linux mTHP (16-64 KiB folios, per-size sysfs, including shmem) produces exactly that contiguity, with no 2 MiB
     alignment. A 64 KiB shmem mTHP also matches UE4's 64 KiB pool decommits, so a punch frees whole folios and splits
     nothing.
   - Zen 2 has no INVLPGB (Zen 3+): every unmap and write-revoke sends TLB-shootdown IPIs that interrupt stage B.
     `/proc/interrupts` (line `TLB`) counts them, no root.
5. **Pacing under gamescope** (from gamescope's WSI layer source, October 2026 master):
   - The layer **always gives the driver MAILBOX and does FIFO itself** over its own protocol. It forces at least 3
     images, sets `vk_xwayland_wait_ready=false` and `vk_khr_present_wait=true`, and now implements
     `VK_EXT_present_timing` with gamescope's real refresh cycle and past presentation times [web, code read].
   - So on the Deck, unlike NVIDIA on the PC, **present wait and timing report the real display**. The vblank tick can
     be phase-locked to it.
   - The OLED panel runs any integer refresh from 45 to 90 Hz [web, community], so "tick = cap = panel refresh" fits
     the Deck better than any fixed grid.
   - `VK_AMD_anti_lag`: Mesa 25.3 shipped it as an implicit layer; RADV-native in Mesa 26.3 (merged 2026-10-08) [web].
     It is the AMD counterpart of the port's Reflex path: it improves latency, not throughput.
6. **Ranked actions** are in section 5. The top three cost no build:
   - measure stage B's real clock, run-queue delay and TLB shootdowns (`perf stat`, `schedstat`, `/proc/interrupts`);
   - one launch with the governor at `performance` through `steamosctl`;
   - panel refresh = cap with VSync (FIFO) and the tick at the cap.

## 1. The core and memory, seen from stage B

### 1.1 Numbers [web]

| Item | Zen 2 / Van Gogh | Note for stage B |
|---|---|---|
| Clocks | 2.4-2.8 GHz base, **3.5 GHz max on every core** (no higher single-core boost) | per-core frequency through CPPC (amd-pstate). Idle cores give power to the busy ones and the GPU, but no core goes above 3.5 |
| L1D | 32 KiB, 8-way, 4-5 cycles; 2 loads + 1 store a cycle | |
| L2 | 512 KiB private, 8-way, 12 cycles, inclusive of L1 | the per-thread hot set should fit here |
| L3 | **4 MiB for the whole APU** (one CCX), 16-way, ~38-40 cycles at full clock; a **victim cache** of the L2s | Chips and Cheese found it "basically missing" from latency tests on default power settings (2023, schedutil) |
| DRAM | **~150 ns** (LCD, LPDDR5-5500, 88 GB/s peak); OLED LPDDR5-6400, 102.4 GB/s peak, latency not published | the CPU alone reached ~25 GB/s; the GPU over 70 GB/s. A 150 ns miss is ~360 cycles at 2.4 GHz and ~525 at 3.5 |
| Out of order | ROB 224, load queue 44, store queue 48, **up to 22 outstanding L1D misses** | memory-level parallelism is the lever: independent misses overlap, dependent ones (pointer chains) do not |
| L1 DTLB | 64 entries, fully associative, any page size; **coalesces contiguous 4 KiB pages** (PMU events `ls_l1_d_tlb_miss.tlb_reload_coalesced_page_*`) | 7-cpu: "behaves as 192 entries in 4 KiB mode" |
| L2 DTLB | 2,048 entries, 16-way, 4 KiB and 2 MiB | 8 MB of reach with 4 KiB pages |
| L2 ITLB | 512 entries | 2 MB of code with 4 KiB pages |
| Page walk | **2 walkers**, a 64-entry page-directory cache (PML4E/PDPE) | a walk whose PTE line misses the caches costs a DRAM trip |
| Branch prediction | L1 BTB 512, L2 BTB 7,168, 4,096-op op cache; **mispredict ~18 cycles**; return stack split 2×15 under SMT | indirect calls through closures (`UniqueFunction`) are what to watch |
| SMT | retire queue/ROB, store queue and micro-op queue **statically partitioned** between busy siblings; caches, TLBs and schedulers competitively shared (Mike Clark, AMD) | a busy sibling halves stage B's out-of-order window: fewer overlapping misses |
| TLB shootdown | **no INVLPGB** (Zen 3+; Linux uses it since 6.15) | every `munmap` or permission revoke on a shared mm sends IPIs to every CPU running the process |
| Power | APU capped near 15-16 W, shared; the GPU can take over 10 W with the CPU left 2-3 W (Chips and Cheese) | GPU work saved is CPU clock gained, and the reverse |

### 1.2 What 4.7x is made of [inferred, from [log] numbers]

Deck packet 11.04 us, PC 2.35 us (run 3, sections on). The 7800X3D runs at ~5.0 GHz in this load.

| Assumption | Deck cycles a packet | PC cycles | Cycle ratio | Clock share | IPC share (Zen 4 vs Zen 2, ~1.3x) | Left for caches and DRAM |
|---|---|---|---|---|---|---|
| Stage B at 2.43 GHz (Vernet's median for a busy thread) | ~26.8k | ~11.8k | 2.3x | 2.06x | 1.3x | **~1.7x** |
| Stage B at 3.5 GHz | ~38.6k | ~11.8k | 3.3x | 1.43x | 1.3x | **~2.5x** |

- The memory-heavy parts (texslow 6.7x, vertex 5.6x, obt_sync 6.3x) sit above the average in both cases. That fits a
  memory share either way.
- **Which row holds is the first measurement** (section 4.2, `perf stat`). It decides between "fix the clock" (row 1)
  and "fix the memory pattern" (row 2).
- Either way, ~1.7-2.5x of the gap is the 4 MiB L3 and 150 ns DRAM. A draw's 26-39k cycles hold room for ~50-75
  serialized DRAM misses at 2.4 GHz [inferred]. Cutting dependent misses is the code-side lever.

### 1.3 SMT and stage B

- **Measured** [log, plan-linux-deck.md "Decode pacing"]: with a dedicated core and a free sibling (Linux
  `DedicateCore`), stage B busy fell from 79% to 70%: ~11% less stage B time for the same work. FPS did not follow,
  because the game's threads were squeezed onto 6 of 8 CPUs.
- **Why** [web]: Zen 2 splits the ROB, store queue and micro-op queue in half while both siblings run. Stage B lives
  on overlapped cache misses, which need a large out-of-order window.
- **New route: core scheduling** [web, kernel doc].
  - What it does: `prctl(PR_SCHED_CORE, PR_SCHED_CORE_CREATE, <stage B tid>, PR_SCHED_CORE_SCOPE_THREAD, 0)` gives
    stage B its own cookie. While stage B runs, its sibling may run only tasks with the same cookie, so it is "forced
    idle". When stage B sleeps, the sibling runs anything.
  - Permission: ptrace read access to the target, which a process has to its own threads. No root.
  - Kernel: needs `CONFIG_SCHED_CORE` (check `zcat /proc/config.gz | grep SCHED_CORE` on the Deck).
  - Against the dedicated core: it takes the sibling only while stage B runs (~91% of the time today, less once it is
    faster), and it moves no other thread's affinity.
  - Caveats: the kernel doc warns of synchronization overhead and says core scheduling "isn't guaranteed to beat"
    SMT off. One forced idle CPU out of 8 is still expensive when the game wants 3+ cores.
  - Test: a toggle (apply or clear the cookie live), A/B in one launch. Report stage B us per draw, FPS, the game's
    ident-64 wait and per-thread run delay (section 4.2). Expected: stage B -5-10% per draw; FPS anywhere from -5% to
    +5% [inferred].
- **Draw-prep workers are SCHED_IDLE, which does not keep them off stage B's sibling** (study-deck-other-threads §3).
  With core scheduling on stage B they cannot share its core while it runs. That takes the place of "2 workers" as a
  sibling fix, but not as a CPU-budget fix.

### 1.4 TLB and page walks: what the platform study did not cover

1. **PTE coalescing plus mTHP** [web: kernel PMU list, transhuge doc; coalescing rules partly undocumented].
   - The L1 DTLB merges neighbouring 4 KiB PTEs into one entry when the pages are virtually and physically contiguous
     with equal attributes. The size is 16-32 KiB, depending on the source.
   - Linux's multi-size THP gives exactly that contiguity, with no 2 MiB alignment:
     - anonymous memory: `/sys/kernel/mm/transparent_hugepage/hugepages-{16,32,64}kB/enabled`;
     - shmem, our guest memfd: `.../hugepages-64kB/shmem_enabled`. The `advise` value makes it opt-in through
       `MADV_HUGEPAGE` on our mappings.
   - For the guest memory it fits unusually well:
     - guest pages are 16 KiB, so write tracking `mprotect`s whole 16 KiB runs, which leaves 4 KiB groups with equal
       permissions;
     - UE4 decommits in 64 KiB units, so a 64 KiB folio is freed whole by the hole punch, never split;
     - the 2 MiB route in the platform study (M1/M4) splits a PMD on every 16 KiB `mprotect` and every 64 KiB punch.
   - Root: the per-size sysfs switches, one launch, revert at reboot. `MADV_COLLAPSE` covers PMD sizes only.
   - Measure first (no root): `perf stat -e ls_l1_d_tlb_miss.all,ls_l1_d_tlb_miss.tlb_reload_coalesced_page_hit,ls_l1_d_tlb_miss.tlb_reload_4k_l2_miss,ls_tablewalker.dside -t <DrawRec tid>`.
     If 4 KiB L2 misses and D-side walks are a few per draw or more, mTHP is worth a launch.
2. **Shootdowns without INVLPGB.**
   - Zen 2 invalidates other CPUs' TLBs by IPI. Every pool decommit (`munmap` + `mmap` on Linux, L2 in
     plan-linux-deck), every write-revoking `mprotect` (~4 a frame, ~770 pages [log, run 8 stall lines]) and every
     `fallocate(PUNCH_HOLE)` of mapped pages sends one to each CPU currently running a thread of the process, stage B
     included.
   - Each IPI costs the receiver an interrupt and the flushed entries [inferred: a few us].
   - Test (no root): `grep TLB /proc/interrupts` before and after 10 s of the heavy view, per CPU. Stage B's CPU is
     known when it is dedicated.
   - At more than ~2k/s on stage B's CPU, L2 (keep-views on Linux) moves up the queue: it removes the decommit
     shootdowns.
3. **2 page walkers, 64-entry page-directory cache.** Hits in the upper levels are cheap. The PTE line itself misses
   when the working set is large. Huge pages remove that level, and mTHP coalescing removes many of the walks
   altogether.

### 1.5 Prefetching for a pointer-chasing thread

- **Hardware prefetchers** (L1 stride and next-line, L2 stream and up/down [web, AMD guides via 7-cpu/C&C]) see only
  regular address streams. Hash buckets, page-table slots and slot vectors indexed by IDs look random to them. The
  sequential copies (register blocks, record chunks) are already prefetched.
- **The L3 is a victim cache.** A line that stage A (or a DrawPrep worker) reads or writes lands in *its* L2, not in
  the L3. Stage B then pulls it core to core. A helper thread warming data for stage B therefore buys about an
  L3-class access, not an L2 hit [inferred from the victim design].
- **Software look-ahead in stage B** [inferred, code idea]:
  - Stage B receives packets from a ring that stage A fills ahead (`published` > `consumed`, `vk_draw_pipe.h:272`).
    When it starts packet N, it can issue `__builtin_prefetch` for packet N+1's lookup targets, once their addresses
    are known from the packet:
    - the buffer-cache direct-mapped table slots for the vertex, index and storage ranges;
    - the image-descriptor cache slots for the texture sharps;
    - the `SlotVector<Image>` entries the memoized set will touch.
  - Up to 22 misses can be in flight, so 5-10 independent prefetches overlap with packet N's ~10 us of work.
  - Zen 2 counts the result: `ls_sw_pf_dc_fill.*` (useful), `ls_inef_sw_pref.*` (wasted). These go in the perf list,
    so the gain can be checked without guessing.
  - Expected: 0.3-1 us a draw if a large share of the per-draw misses are these lookups; 0 if they already hit L2
    [inferred]. Effort S-M (the addresses come from packet fields stage A already decoded). Risk L: a prefetch never
    faults.
- **Batch independent lookups inside one draw.** `ObtainBuffer` runs 3-4 times a draw (study-stage-b-per-draw C5).
  Computing all keys first and probing after turns 3-4 dependent miss chains into overlapping ones. This pairs with
  C4 (one recorded command per draw).

### 1.6 Working set and data layout

- **What shares the 4 MiB L3:** the game's own threads (guest code and data), stage A, B and the recorder, the
  DrawPrep and copy workers, Mesa/RADV, Wwise. At ~65 FPS each frame's per-draw state is touched once per draw, so
  only what stays in stage B's 512 KiB L2, or in the L3 between draws, is cheap.
- **Rules that pay on this core** [inferred, standard]:
  - **Hot/cold split** of the per-draw objects. Keep the fields the memo-hit paths read (image ID, view, layout state,
    region stamp, last-use tick) in a dense parallel array indexed by slot ID, and the creation-time data in the
    `Image` object. The texset memo hit (0.85 us on the Deck, 5.7x) is a loop of image checks that each fetch an
    `Image`: one line each instead of several.
  - **32-bit IDs instead of pointers** in memo keys and tables: twice as many entries per line.
  - **Open-addressing tables that store the hash or key in the slot** (`tsl::robin_map` with `StoreHash`), so a probe
    does not chase into the value. The direct-mapped buffer table is already the right shape. Check its slot size
    with `pahole` (distrobox, no root).
  - **No allocation on the per-draw path**: the `std::function` per signal (study-stage-b-per-draw §3) touches the
    allocator's metadata as well as the object.
- Measure before changing (`perf stat`, no root):
  - `l2_latency.l2_cycles_waiting_on_fills` (×4 ÷ L2 misses = average L2 miss latency in core cycles, both threads);
  - `ls_refills_from_sys.ls_mabresp_lcl_dram` (demand fills from DRAM);
  - `ls_refills_from_sys.ls_mabresp_lcl_cache` (from the L3 or another core).

  These give stage B's DRAM misses a draw and the real latency under the GPU's bandwidth load. The 150 ns figure was
  measured with the GPU idle.

### 1.7 False sharing between stage A, stage B, the recorder and the copy threads [code]

`vk_draw_pipe.h:272-277` aligns `published`, `consumed` and `wake` to 64 bytes: good. `vk_scheduler.h` does not
separate fields written by different threads:

| Fields, in declaration order (`vk_scheduler.h`) | Writers | Readers |
|---|---|---|
| `num_pending_ops` (:1070), next to `pending_polls` and the pending-ops queue and mutex | stage B / A | every draw (`PopPendingOperations`, one atomic load per draw) |
| `chunk_lock` (:1089) | whoever records: stage B on every `Record` | — |
| `wake_deadline_ns` (:1162), `recorder_busy`, `queued_chunks` (:1170), `recorder_sleeping` | the recorder and stage B | stage B on every kick |
| `host_copies_done` (:1174) | the copy threads | stage B |
| `deferred_signals_issued` (:1189) | stage B | the recorder |

- Each write by one thread invalidates the line in the others' L1/L2. Inside one CCX the next access is a
  core-to-core transfer, of the order of an L3 hit or worse at a low fclk [inferred].
- **Fix:** group the fields by writing thread, one `alignas(64)` group per writer. Effort S, risk L.
- Expected: small but certain, 0.05-0.2 us a draw [inferred]. It also removes noise from every stage B measurement.
- **`Gow3Stats` counters** (`gow3_toggles.h:116-279`, ~64 `fetch_add` sites) are adjacent `inline std::atomic`
  globals. They are written from several threads whenever `GOW3_FRAME_STATS` is on, which is every Deck run.
  - Locked RMWs on shared lines inflate the stage B figures the Deck decisions rest on.
  - Per-thread counters summed at print time remove that. This matters for measurement, not for players.
- `perf c2c` would find false sharing directly, but on AMD it needs IBS. That is probably root or
  `perf_event_paranoid` ≤ 1 [inferred]. Use the table above instead.

### 1.8 Branch prediction

- Per draw, stage B makes ~6 `Record` calls (closures built for the recorder), dispatches its ordered tasks through
  `UniqueFunction`, and runs the memo lookups' data-dependent branches.
- The L2 BTB (7k) and the indirect predictor cover a loop this size. The recorder thread pays the closures'
  *indirect calls*, not stage B [code].
- Measure `ex_ret_brn_misp` and `ex_ret_brn_ind_misp` against `ex_ret_brn` on DrawRec. Below ~1 mispredict per
  100 instructions, leave it; PGO (platform study B3) is the general answer.

### 1.9 Bandwidth shared with the GPU

- The GPU is ~90% busy and can pull over 70 GB/s; the CPU alone ~25 GB/s [web].
- Under GPU load the CPU's DRAM latency rises (loaded latency), and on Van Gogh mclk follows fclk (platform study P0).
- So **GPU bandwidth saved shortens stage B's misses** as well as freeing power [inferred]. Examples: effects
  resolution, VRS (platform study R3), fewer full-screen passes.
- The loaded latency is what `l2_latency` measures in a real run (1.6).

## 2. Power and clocks

### 2.1 What is known about the Deck's CPU frequency behaviour [web]

| Source (date) | Setup | Finding |
|---|---|---|
| Vernet, RFC cover letter and replies (LKML, 2026-07-28/30) | **OLED Deck**, amd-pstate **active**, EPP **`balance_performance`** (corrected from "LCD" in the thread), Civilization VI benchmark, single busy thread, 6 interleaved A/B runs, Welch's t-test | Default: busy core **median 2.43 GHz at 98% utilization** |
| same | global EPP `performance` | median **3.5 GHz**, p999 frame time **-40%**, 1% lows **+16%** |
| same | `min_perf` floor at nominal on the busy core | median 3.5 GHz, but **p999 +13-21% worse**. Vernet suspects the SMU's shared CPU/GPU budget (not confirmed: no CPU contention in the traces) |
| same | `epp_boost` (per-core EPP `performance` while a core was ≥50% busy in the last 300 ms; RFC, not merged) | median 3.5 GHz, 1% lows **+31.8%**, p99 +4.1%, average FPS unchanged |
| same, reply | trace analysis | **"any block longer than ~0.2 ms resets the busy core to a sticky ~2.4 GHz plateau"**. EPP governs how fast the core recovers after a sleep, not whether it drops |
| Chips and Cheese (2023-03, LCD, schedutil era) | clock ramp | 1.4 → 1.7 GHz in 0.27 ms, hundreds of ms at 1.7, ~1 s to full clock; L3 "basically missing" in latency tests on default settings |

What it means for us [inferred]:
- **Stage B fits the regressing pattern exactly.** It is busy 91%, but it blocks every frame: in `flip_present` (~1
  ms of waits, run 3), for the recorder (`RunWriteData` ~250 us twice a frame), and when stage A has not published.
  Any of these over 0.2 ms puts it back on the 2.4 GHz plateau.
- The same holds for the game's submit thread (its ident-64 wait, ~6 ms a frame), the recorder (7.3k sleeps/s) and
  stage A.
- Vernet's case is one busy thread. Ours is 3-4 busy threads plus a GPU at ~90%. So the SMU has less spare budget,
  and the `performance` gain can come out smaller, or negative if the GPU loses clock. Measure the GPU clock in the
  same run.

### 2.2 Per-core boost and preferred cores

- All four cores have the same 3.5 GHz ceiling [web]. A single busy core does not boost above it when the others
  idle. Idle cores only leave budget to the busy core and the GPU.
- CPPC preferred-core rankings (amd-pstate `prefcore`, Linux 6.9+) only matter when the platform reports different
  `highest_perf` per core. On a 4-core monolithic APU they are probably equal [inferred]. Read
  `/sys/devices/system/cpu/cpu*/cpufreq/amd_pstate_highest_perf` and `amd_pstate_prefcore_ranking`. If they differ,
  `DedicateCore`'s choice of "the highest core ID" should take the top-ranked core instead (`gow3_threads.h:117+`
  [code]).
- **Per-core clock limits exist on Van Gogh** (amdgpu `pp_od_clk_voltage`, `p <core> <0=min|1=max> <MHz>`, after
  `power_dpm_force_performance_level=manual`) [web, kernel commits]. It is root only. A per-core *min* on stage B's
  core is a `min_perf` floor by another name, the thing that regressed p999 in Vernet's data. And the `manual` level
  also switches the GPU to manual clock ranges. Not recommended beyond one curiosity run.

### 2.3 The CPU/GPU power split

- The SMU balances one budget (~15 W socket on the Deck; the QAM TDP slider lowers the cap). A GPU-bound load leaves
  the CPU 2-3 W [web, C&C].
- Measured here [log]:
  - GPU pinned at 1600 MHz: the CPU fell to ~2 GHz;
  - auto clocks: the GPU sat at 800-1200 MHz at ~90% busy, so the SMU already limits the GPU in favour of the CPU.
- **GPU power profile.** Van Gogh has workload profiles `CAPPED` (8, power-focused) and `UNCAPPED` (9,
  performance-focused) in `pp_power_profile_mode`, added in Linux 6.4 [web]. `steamosctl set-gpu-power-profile`
  wraps the file [web].
  - Which profile Steam sets with and without a frame limit is unknown. Read `pp_power_profile_mode` (the `*` row)
    during a run, no root.
  - If it reads `CAPPED` while we run uncapped, one launch with `UNCAPPED` is a cheap test of the GPU leg. It may
    cost CPU clock: watch both.
- **TDP slider:** leave it at the default (off / 15 W). Lower caps only take from both sides.

### 2.4 SteamOS's power daemons and what a user can change [web]

| Tool | What | Root? |
|---|---|---|
| `steamos-manager` (user daemon on the session bus; a root daemon behind it on the system bus) via `steamosctl` | `get/set-cpu-scaling-governor`, `get/set-cpu-boost-state`, `get/set-tdp-limit`, `get/set-gpu-performance-level` (auto/low/high/manual/profile_peak), `get/set-manual-gpu-clock`, `get/set-gpu-power-profile`, `get/set-performance-profile` | **no** (session D-Bus, the privileged part is the daemon's). The list comes from a third-party man page and the project README: check with `steamosctl get-all-properties` |
| sysfs `energy_performance_preference`, `scaling_governor` | EPP / governor per CPU | yes |
| amd-pstate mode (`/sys/devices/system/amd_pstate/status`) | active / passive / guided | yes |
| `epp_boost` | not in any shipped kernel (RFC); SteamOS 3.8 was on Linux 6.16, and plan-linux-deck records 7.2.7 on this Deck | — |

- In **active mode** the `performance` governor pins EPP to 0 (performance) on every CPU. That is the "global
  `performance`" row above. In passive or guided mode it would pin the frequency request to max.
- So `steamosctl set-cpu-scaling-governor performance` should reach the measured +16% / -40% configuration without
  root. Confirm the driver first (section 0 of the platform study prints it).
- It reverts with `... powersave`, and probably at reboot or session restart [inferred].

### 2.5 What can favour stage B's clock, and what cannot

| Route | Mechanism | Root | Verdict |
|---|---|---|---|
| Governor `performance` (steamosctl) | EPP 0 on all cores: no sticky plateau after sleeps | no (if steamosctl works) | **test first** (rank 2) |
| Stage B never blocks > ~0.2 ms when the wait will be short | bounded spin (≤ 150-200 us, `_mm_pause`) before the futex sleep in stage B's own waits (packet ring empty, recorder hand-off, `flip_present`'s short waits) | no | test after the governor run. It works with any EPP. Costs power, which the platform study's P5 counts against spinning. Spin only on stage B, only when it is the bottleneck (91% busy) |
| Dedicated core + per-CPU PM QoS (`/sys/devices/system/cpu/cpuN/power/pm_qos_resume_latency_us` = 0 on stage B's CPU) | keeps that CPU out of deep C-states, the likely cause of the reset [inferred] | yes | measurement only; ship only through the spin route |
| uclamp (`sched_setattr` `SCHED_FLAG_UTIL_CLAMP_MIN`) | feeds schedutil | raising it needs `CAP_SYS_NICE` | **no effect in active mode**: the hardware picks the frequency (the LKML thread's point) |
| `min_perf` / per-core min clock | floor | yes | regressed p999 13-21% in Vernet's data. Avoid |
| Cutting spin elsewhere | stage A's yield-spin shows up as **kernel time** on the GPU thread: 3.4-9.6 ms per slow frame (run 8 stall lines, `getrusage(RUSAGE_THREAD)` at submission end, `liverpool.cpp:1981`) [log/code]; the recorder's 20 us spins; the DrawPrep workers | no | more budget for the busy cores and the GPU (platform study P5). Already in the work queue |

## 3. Frame pacing on the Deck

### 3.1 How gamescope presents a native Vulkan game (WSI layer source, `ValveSoftware/gamescope` master, commits to 2026-10-16) [web/code read]

- The layer (`VkLayer_FROG_gamescope_wsi`) wraps the game's surface. If the window can bypass Xwayland, the
  swapchain goes to gamescope directly over its `gamescope_swapchain` protocol. Otherwise it falls back to the X11
  surface.
- **The driver always gets `VK_PRESENT_MODE_MAILBOX_KHR`.** The code says: "Always send MAILBOX as the mode to the
  driver, as we implement FIFO ourselves -- using the Gamescope swapchain protocol." The app's chosen mode is passed
  to gamescope with `gamescope_swapchain_set_present_mode`. So FIFO, MAILBOX and IMMEDIATE are compositor policies on
  the Deck:
  - FIFO: gamescope shows each frame in order, at vblank;
  - MAILBOX: the newest frame at each vblank;
  - IMMEDIATE: tearing flips, only with "Allow tearing".
- **minImageCount is forced to 3** unless `GAMESCOPE_WSI_MIN_IMAGE_COUNT` / `vk_wsi_override_min_image_count` say
  otherwise.
- At instance creation it sets `vk_xwayland_wait_ready=false` and `vk_khr_present_wait=true`. Present wait is on for
  every game under gamescope. The platform study's G1 item (`vk_xwayland_wait_ready`) is therefore already handled
  wherever the layer loads.
- **`VK_EXT_present_timing`** is implemented by the layer since September-October 2026 (commits "implement
  VK_EXT_present_timing support", "keep monotonic time 64-bit"). It is fed by gamescope's `past_present_timing`
  events and `refresh_cycle`, with `PRESENT_AT_NEAREST_REFRESH_CYCLE`. `VK_GOOGLE_display_timing` data comes from the
  same source.
- **Steam's limiter:** gamescope announces its limiter state with the `gamescope_limiter` protocol.
  - When it is "forcing FIFO" and the client is *frame-limiter aware*, the layer offers only FIFO. Aware clients are
    DXVK ≥ 2.3, vkd3d ≥ 2.12, or anything with `GAMESCOPE_WSI_FRAME_LIMITER_AWARE=1`.
  - Our port is not aware by default, so Steam's limiter works on it through gamescope's older mechanism [inferred].
- Whether SteamOS 3.9.5's gamescope already has the present-timing commits is unknown: read
  `vulkaninfo | grep -i present_timing` inside Game Mode, or the layer's `[Gamescope WSI] Created swapchain ...
  presentTiming:` line in our log.

### 3.2 What the port does today [code]

- Present mode (`vk_swapchain.cpp:307-337`):
  - VSync on → FIFO;
  - VSync off → MAILBOX, else IMMEDIATE;
  - a named mode can override both.
- Present wait and present ID are enabled only with `GOW3_DISPLAY_TRACE=1` (`vk_instance.cpp:344-351`). They were
  turned off because on NVIDIA the wait returned at the present itself. **That reason does not hold under gamescope**
  (3.1).
- Reflex (`VK_NV_low_latency2`) is NVIDIA only. On AMD the Precise limiter is used.
- The vblank tick runs on `steady_clock` with no display feedback. On the PC it drifted ~25 ppm against the display
  (study-vrr-present-pacing).
- `Common::SetCurrentThreadRealtime` is a no-op on Linux (`common/thread.cpp:76-78`). `BoostPresentingThread` is
  Windows only (`driver.cpp:104+`). So the vblank and present threads run at normal priority on the Deck.
- `PreciseWait` on Linux is `sleep_until(slot - spin)` plus a `_mm_pause` spin (`driver.cpp`, 700 us by default with
  the precise tick). Linux's default timer slack is 50 us, so a 100 us spin would do. The Deck preset already runs
  with the precise tick off.

### 3.3 AMD Anti-Lag on Linux [web]

- Mesa 25.3 (2025) shipped `VK_AMD_anti_lag` as an implicit layer shared by Mesa's drivers.
- Valve's Daniel Schürmann replaced it with a RADV-native implementation on the common `vk_frame_pacer`, merged
  2026-10-08 for **Mesa 26.3**. The delay is computed as `avg_frame_time + queuing_delay - min(deviation, 1 ms)`,
  with the queuing delay measured from calibrated timestamps at submit and at GPU start. No latency numbers have been
  published.
- The Deck runs RADV 26.1.99 [log], so it has the layer, if Steam's runtime loads it. `vulkaninfo | grep anti_lag`
  settles that.
- For the port: `vkAntiLagUpdateAMD` (mode on, `maxFPS` = cap, the input stage) goes where the Reflex sleep goes, on
  the thread that fires the vblank event the game's frame waits for. It cuts the GPU queue (the ~15.6 ms
  `queued → gpu_ready` of run 4) and so the latency. It does not make stage B faster [inferred].

### 3.4 What the pacer should do differently on the Deck [inferred, from 3.1-3.3 and the logs]

1. **Panel refresh = cap = tick.**
   - The OLED panel accepts integer refresh rates from 45 to 90 Hz. The unified frame-limit slider sets both, or they
     can be set separately in developer settings [web, community].
   - The port's PC rule ("a cap holds when the tick is the cap") then holds without beating against a 90 Hz grid. That
     beating is the likely source of the user's 10 / 20 ms ping-pong at ~62 FPS on 90 Hz: MAILBOX picks the newest
     frame at 11.1 ms vblanks.
   - Use VSync on (FIFO, so gamescope's FIFO) with the tick at the refresh. Precise tick off: Proton run 4 was flat at
     60 [log].
   - The Deck preset picks the refresh from the content: 60 today, higher once stage B allows.
2. **Lock the tick to gamescope's vblank instead of `steady_clock`.**
   - With present timing (or present wait plus `refresh_cycle`), the vblank thread learns the real refresh period and
     phase. It then corrects its period (drift) and fires the guest's vblank a fixed margin before gamescope's latch.
   - On the PC this was impossible on NVIDIA; on the Deck the data is real.
   - Effort M: a feedback loop in `PresentThread`, behind a switch.
3. **Present wait instead of a fixed spin.** With FIFO under gamescope, `vkWaitForPresentKHR(id - 1)` on the present
   thread is a true back-pressure point. It can replace the tick's spin and the `GetRenderFrame` pool wait as the
   pacing source when the cap equals the refresh.
4. **Anti-Lag as the AMD limiter mode,** after 1-3: one switch, latency only. Measure with the flip trace's
   submit-to-presented time.
5. **Thread priority for the vblank and present threads on Linux** (section 4.1: rtkit, or EEVDF slices). Today they
   compete at nice 0 with ~77 threads on 8 CPUs, so a late tick shows as a long frame.
6. **Steam's limiter, tested once with `GAMESCOPE_WSI_FRAME_LIMITER_AWARE=1`.** `run.py` would need a
   `GAMESCOPE_` prefix, or a launch option.
   - Then gamescope's limiter forces our swapchain to FIFO and paces it, with no port tick.
   - It is the closest the Deck has to a driver-paced cap, as Reflex is on the PC. Compare the lows with the port's
     limiter at the same cap.
7. **Not on the Deck:** VRR modes and the 960 / 480 Hz ticks. The internal panel is fixed-refresh, and the spin costs
   power.

## 4. Linux specifics a user process can use

### 4.1 Scheduling

| Mechanism | What it gives | Unprivileged? | Fit |
|---|---|---|---|
| EEVDF custom slice (`sched_setattr`, `sched_runtime` for `SCHED_OTHER`, 6.12+) | an earlier virtual deadline on wake-up | yes | platform study P3. On stage B, the recorder and the vblank thread |
| **rtkit** (`org.freedesktop.RealtimeKit1`, system bus) `MakeThreadHighPriority(tid, nice)` | **negative nice down to -15** (rtkit default) for our own threads | **yes**, if rtkit runs (PipeWire setups usually ship it; check `busctl --system status org.freedesktop.RealtimeKit1`) [inferred] | stage B, the vblank thread. Nice -10 weighs ~9x a nice-0 guest thread (9548 vs 1024) on the same CPU |
| rtkit `MakeThreadRealtime(tid, prio ≤ 20)` | `SCHED_RR` | yes, but the process must set `RLIMIT_RTTIME` first; rtkit demotes everything if a canary starves | **vblank / present thread only** (short bursts). Never stage B (it runs ~91%) |
| `PR_SCHED_CORE` | sibling forced idle while stage B runs (1.3) | yes (own threads) | stage B |
| `PR_SET_TIMERSLACK` = 1 ns | precise `sleep_until` | yes | vblank thread (also item G of study-deck-other-threads) |
| `futex_waitv` (5.16+) | one wait on several futexes | yes | stage B waits on "packet published" and "flip request" separately today. A combined wait is tidy, not a speed-up |
| uclamp | — | raising it needs `CAP_SYS_NICE` | no effect in active EPP mode (2.5) |
| `scx_lavd` (sched_ext) | latency-criticality ranking from wake/wait chains; overrides nice/slice semantics | starting it is root (or a Steam developer option, if one exists) | SteamOS 3.8 notes list "support for the LAVD CPU scheduler" [web]; whether it is on by default is unconfirmed. Read `/sys/kernel/sched_ext/state` (platform study §0). If LAVD is active, slices and nice are re-interpreted by it: test the rtkit / slice items under both |

### 4.2 Measurements with no root and no build (they ride on a planned run)

```sh
pid=$(pgrep -o medievil-runti)
tid=$(grep -l DrawRec /proc/$pid/task/*/comm | cut -d/ -f5)
# 1. stage B's real clock, IPC, TLB and miss profile (perf from the distrobox; perf_event_paranoid <= 2)
perf stat -t $tid -e task-clock,cycles,instructions,ls_l1_d_tlb_miss.all,ls_l1_d_tlb_miss.tlb_reload_4k_l2_miss,\
ls_l1_d_tlb_miss.tlb_reload_coalesced_page_hit,ls_tablewalker.dside,ls_refills_from_sys.ls_mabresp_lcl_dram,\
l2_latency.l2_cycles_waiting_on_fills,ex_ret_brn_misp -- sleep 10
#    "cycles / task-clock" = GHz while running: 2.4 -> section 1.2 row 1 (clock), 3.5 -> row 2 (memory)
# 2. run-queue delay: field 2 of schedstat = ns spent runnable but waiting for a CPU
for t in /proc/$pid/task/*; do printf '%s %s\n' "$(cat $t/comm)" "$(cat $t/schedstat)"; done   # twice, 10 s apart
# 3. TLB shootdown IPIs per CPU
grep -E 'TLB' /proc/interrupts   # twice, 10 s apart
# 4. clocks and power profile (the platform study's P0 logger adds these)
cat /sys/devices/system/cpu/cpu*/cpufreq/scaling_cur_freq; d=$(ls -d /sys/class/drm/card*/device|head -1)
grep '\*' $d/pp_power_profile_mode; steamosctl get-all-properties 2>/dev/null
```

- `perf` is not on SteamOS's root image. The distrobox's `perf` works against the host kernel for hardware counters.
  If the Zen 2 event names are missing, use the raw codes from the kernel's `amdzen2/*.json`.
- Light: a few counters on one thread, plus `cat`s. Tell the user it runs (the no-background-load rule).

### 4.3 Transparent huge pages: additions to the platform study [web]

- Per-size mTHP for anonymous memory (`hugepages-<size>kB/enabled`) and shmem (`.../shmem_enabled`, values `always`,
  `inherit`, `never`, `within_size`, `advise`). Boot parameters exist (`thp_anon=`, `thp_shmem=`).
- `MADV_COLLAPSE` ignores the sysfs settings, but handles **PMD size only**. mTHP needs the sysfs switch, so root for a
  test. Shipping it needs SteamOS defaults or a startup check that only reports.
- `PR_SET_THP_DISABLE` / `PR_THP_DISABLE_EXCEPT_ADVISED` exist as per-process controls. Nothing in the port sets them
  [code: no `prctl(PR_SET_THP` in `src/`].
- Partial `mprotect` / `munmap` splits a PMD mapping into PTEs (the folio stays huge, deferred split): the cost the
  platform study noted for 2 MiB. mTHP folios are PTE-mapped from the start, so tracking does not split them.

### 4.4 I/O, cgroups, other

- **io_uring for pak reads:** low value.
  - The guest reads through synchronous HLE file calls on its own threads, not on stage B.
  - The 0.6-1.1 s first-visit hitches (native run 1) are more likely pipeline builds (platform study R6).
  - Check `/proc/<pid>/io` (`read_bytes`) across a hitch before any I/O work. `posix_fadvise(WILLNEED)` on a level's
    pak at load is the cheap step if reads show up.
- **cgroups:** gamescope's session runs games in systemd user scopes [inferred]. `cat /proc/<pid>/cgroup` shows
  which. CPU controller weights there do not limit a single game. There is nothing to tune, but it tells LAVD or
  systemd-oomd behaviour apart if that ever matters.
- **NUMA:** none (one CCX, one memory controller).
- **`perf_event_paranoid`:** read it (platform study §0). At 2, user-space counting and sampling of our own process
  works. Kernel samples and IBS (`perf mem`, `perf c2c`) need ≤ 1 or root.

## 5. Ranked actions

Impact is on stage B (us a draw) or the heavy-view frame, all [inferred] unless marked. "Launch" means a run with the
user's go, one change.

| # | Action | Kind | Expected impact | Test | Root |
|---|---|---|---|---|---|
| 1 | **Measure stage B's real clock, misses, TLB, run delay, shootdowns** (4.2) | measurement, rides on any planned launch | decides 2-9. If stage B runs ~2.4 GHz, clock is the first lever; if ~3.5, memory layout is | `perf stat` + `schedstat` + `/proc/interrupts`, 10 s each in the heavy view | no |
| 2 | **Governor `performance` via `steamosctl set-cpu-scaling-governor performance`** (or the sysfs EPP as root) | setting | if row 1 of 1.2 holds: stage B up to ~25-30% faster per draw (2.4 → 3.5 GHz on the clock-bound part); FPS +5-20% until the GPU or the power budget limits; lows better (Vernet: +16% 1% lows) | 1 launch, same spot; log FPS, lows, stage B us/draw, **GPU clock** (the trade) and socket power | no if steamosctl works |
| 3 | **Panel refresh = cap = tick, VSync on (gamescope FIFO), precise tick off** | settings (QAM + launcher) | pacing: removes the 11.1 / 22.2 ms alternation at 90 Hz; not FPS | 2 launches: 60 Hz / cap 60 vs 90 Hz / no cap; flip trace + the user's overlay | no |
| 4 | **Stage B: no block > 0.2 ms when the wait is short** (bounded spin before sleeping, stage B only) | code S, toggle | keeps stage B off the 2.4 GHz plateau under the default EPP; 0-15% of stage B; small if #2 already ships | toggle A/B in one launch, with the governor at default; count stage B sleeps > 0.2 ms | no |
| 5 | **Next-packet prefetch + batched lookups in stage B** (1.5) | code S-M, toggle | 0.3-1 us a draw if the lookups miss (decided by #1's DRAM fills per draw) | toggle A/B; `ls_sw_pf_dc_fill.*` vs `ls_inef_sw_pref.*` | no |
| 6 | **`alignas(64)` per-writer groups in `Scheduler`; per-thread `Gow3Stats`** (1.7) | code S | 0.05-0.2 us a draw; cleaner measurements | A-B-A builds | no |
| 7 | **Core scheduling cookie on stage B** (1.3) | code S, toggle | stage B -5-10% per draw; FPS -5% to +5% | toggle A/B; run delay of the game's threads | no (needs `CONFIG_SCHED_CORE`) |
| 8 | **rtkit nice -10 on stage B and the vblank thread; `PR_SET_TIMERSLACK` 1 ns on the vblank thread** | code S | run-queue delay off the chain (decided by #1's `schedstat`); lows | toggle; `schedstat` run delay before/after | no (rtkit) |
| 9 | **mTHP: anon 32/64 KiB `always`, shmem 64 KiB `always` (or `advise` + `MADV_HUGEPAGE` on the guest views)** | sysfs, then code | DTLB coalescing; 0-5% of stage B and the game's threads; only if #1 shows 4 KiB L2-TLB misses | 1 launch per setting, coalesced-hit counter | yes (sysfs) |
| 10 | **Present timing / present wait as the tick's phase source** (3.4 items 2-3) | code M | steadier frame starts at tick = refresh; fewer one-refresh misses | flip trace with display times (works under gamescope) | no |
| 11 | **GPU power profile read; `UNCAPPED` once if it reads `CAPPED`** | setting | GPU leg; may cost CPU clock | 1 launch; GPU and CPU clocks both | no (steamosctl) |
| 12 | **`VK_AMD_anti_lag`** (layer in 26.1, native in 26.3) behind the limiter switch | code S-M | latency (the ~15 ms GPU queue), not FPS | flip trace submit → presented | no |
| 13 | Steam limiter with `GAMESCOPE_WSI_FRAME_LIMITER_AWARE=1` (needs a `run.py` prefix) | setting | driver-paced cap; compare lows | 1 launch | no |
| 14 | Dedicated core + `pm_qos_resume_latency_us=0` on its CPU | sysfs, measurement only | shows whether C-state exit causes the 2.4 GHz plateau | 1 launch | yes |
| 15 | Not recommended: per-core min CCLK (`pp_od_clk_voltage`), `min_perf` floors, a pinned GPU clock, uclamp, SCHED_RR on stage B | — | measured or reported regressions | — | — |

Order: 1 on the next planned launch. 2 and 3 are one launch each. Then 4-8 as toggles: each is a bit in
`GOW3_TOGGLE_FILE`, so one launch carries several A/Bs. 9 and 14 are root measurements, reverting at reboot.

## 6. Where this qualifies the platform study and the plan

1. **P1 (EPP `performance`)** assumed root or GameMode. `steamosctl` may do it unprivileged (2.4). The evidence is
   stronger now:
   - Vernet's data is from an **OLED** Deck in SteamOS's own mode;
   - global `performance` gave +16% 1% lows and -40% p999 without the RFC.
2. **P5 ("spinning costs boost")** and the new "> 0.2 ms block → 2.4 GHz plateau" pull in opposite directions. Both
   hold:
   - spinning on non-critical threads (stage A, recorder, DrawPrep) wastes budget;
   - short bounded spinning on the **one** critical thread can keep its clock.

   The user's report that clocks rose with the drain sleep while FPS did not (plan-linux-deck "Drain sleep") fits:
   the other cores' clocks went up, stage B's chain did not change.
3. **M1/M4 (2 MiB THP for the guest memfd)** split on every 16 KiB `mprotect` and 64 KiB punch. mTHP at 64 KiB avoids
   both, and uses Zen 2's coalescing. Try mTHP first.
4. **G1 (`vk_xwayland_wait_ready=false`)** is already set by the gamescope layer for every game it loads into
   (3.1). Only the "is the layer loaded" check remains.
5. **The 2 helpers + dedicated core run** ("stage B 79 → 70%, FPS did not follow") is the SMT effect plus a thread
   budget squeeze. Core scheduling (#7) is the variant that does not take a core from the game.
6. **The present-wait rejection** (`vk_instance.cpp:344`, "on NVIDIA the wait returned at the present itself") does
   not carry over to the Deck. Re-enable present wait on Linux under gamescope for the flip trace's display times
   before designing #10.
7. **"GPU thread user/kernel" in the stall lines is stage A**, the thread that ends submissions, not stage B. Its
   kernel share (3.4-9.6 ms per slow frame) is the yield spin's `sched_yield` system calls, consistent with the
   wait-site run (8.4k voluntary switches/s). It should not be read as stage B kernel time.

## Sources

Hardware and microarchitecture:
- Chips and Cheese, "Van Gogh, AMD's Steam Deck APU" (2023-03-05): https://old.chipsandcheese.com/2023/03/05/van-gogh-amds-steam-deck-apu/
- 7-cpu, AMD Zen 2 (caches, TLBs, 2 page walkers, PDC 64, mispredict 18, ROB/LQ/SQ, 22 misses): https://7-cpu.com/cpu/Zen2.html
- Linux perf Zen 2 event list (coalesced-page DTLB events, table walker, L2 latency, software prefetch fills):
  https://github.com/torvalds/linux/tree/master/tools/perf/pmu-events/arch/x86/amdzen2
- SMT static partitioning (Mike Clark, AMD): Chips and Cheese interview (2024-07-15) https://chipsandcheese.com/2024/07/15/a-video-interview-with-mike-clark-chief-architect-of-zen-at-amd/ ;
  Hot Chips 28 slides https://old.hotchips.org/wp-content/uploads/hc_archives/hc28/HC28.23-Tuesday-Epub/HC28.23.90-High-Perform-Epub/HC28.23.930-X86-core-MikeClark-AMD-final_v2-28.pdf ;
  AnandTech Zen SMT https://at-web1.www.anandtech.com/show/10591/amd-zen-microarchiture-part-2-extracting-instructionlevel-parallelism/6
- PTE coalescing on Zen (academic, MEMSYS 2023; text not extracted here): https://www.memsys.io/wp-content/uploads/2023/09/19.pdf
- OLED LPDDR5-6400, 102.4 GB/s: https://www.pcgamer.com/steam-deck-memory-channels-lpdd5 ,
  https://adminnet.anandtech.com/print/21133/valves-steam-deck-oled-bigger-display-new-soc-faster-memory-more-storage-same-performance
- INVLPGB broadcast TLB invalidation, Zen 3+, Linux 6.15: https://www.phoronix.com/news/AMD-INVLPGB-Ready-For-Linux

Power and clocks:
- Vernet, "[RFC PATCH 0/4] cpufreq/amd-pstate: Per-core EPP boost for recently-busy CPUs" (2026-07-28): https://lkml.iu.edu/2607.3/07278.html ;
  reply with OLED correction, 0.2 ms plateau, uclamp discussion (2026-07-30): https://lkml.iu.edu/2607.3/12399.html ;
  doc patch: https://lkml.iu.edu/2607.3/07287.html ; LWN: https://lwn.net/Articles/1085871/
- Van Gogh per-core CCLK via `pp_od_clk_voltage`: https://mirror.iscas.ac.cn/git/github.com/kvm-riscv/linux/-/commit/0d90d0ddd10e576dbbad64486c263e5e86bbcdd7 ,
  https://lab.nexedi.cn/kirr/linux/-/commit/3dc8077fb00ba769b18d115b08935df388d33d2b
- Van Gogh CAPPED / UNCAPPED power profiles (Linux 6.4): https://lab.node.vifib.com/kirr/linux/-/commit/31865e96f9eb52ced6d5e23f9f3a5376f81c9410.patch ,
  https://www.phoronix.com/news/AMDGPU-Linux-6.4-Steam-Deck-PWR
- steamos-manager (README, interfaces): https://github.com/hhd-dev/steamos-manager-hhd (mirror; upstream gitlab.steamos.cloud/holo/steamos-manager);
  `steamosctl` commands (third-party man page): https://linuxcommandlibrary.com/man/steamosctl
- amd-pstate admin guide: https://docs.kernel.org/admin-guide/pm/amd-pstate.html

Pacing:
- gamescope WSI layer source (MAILBOX to the driver, min 3 images, `vk_khr_present_wait`, present timing, limiter protocol):
  https://github.com/ValveSoftware/gamescope/blob/master/layer/VkLayer_FROG_gamescope_wsi.cpp (read 2026-11-04)
- RADV-native `VK_AMD_anti_lag` (Mesa 26.3, merged 2026-10-08): https://mixed-news.com/en/?p=36345 ,
  https://phoronix.com/news/Mesa-26.3-RADV-VK_AMD_anti_lag ; Mesa 25.3 layer: https://www.phoronix.com/news/Mesa-Vulkan-AMD-Anti-Lag
- OLED refresh 45-90 Hz and the unified limiter slider (community): https://steamcommunity.com/app/1675200/discussions/0/4513255384646158768 ,
  https://lemm.ee/post/16871769

Linux:
- Core scheduling (`PR_SCHED_CORE`): https://docs.kernel.org/admin-guide/hw-vuln/core-scheduling.html
- Transparent huge pages incl. mTHP and shmem per size, `MADV_COLLAPSE`, `PR_THP_DISABLE_EXCEPT_ADVISED`: https://docs.kernel.org/admin-guide/mm/transhuge.html
- SteamOS 3.8.10 (Linux 6.16, LAVD support): https://phoronix.com/news/SteamOS-3.8.10-Stable ; scx_lavd: https://lwn.net/Articles/991205/

Repo: `gpu/shadps4/video_core/renderer_vulkan/vk_draw_pipe.h:167-277`, `vk_scheduler.h:1060-1191`,
`vk_swapchain.cpp:307-337`, `vk_instance.cpp:344-366`; `gpu/shadps4/core/libraries/videoout/driver.cpp:104-130`,
`:1022`, `:1180-1240`, `:1314-1412`; `gpu/shadps4/common/thread.cpp:40-80`; `gpu/shim/gow3_threads.h:95-140`;
`gpu/shim/gow3_toggles.h:116-279`; `gpu/shadps4/video_core/amdgpu/liverpool.cpp:1977-1990`;
`research/docs/data/deck-native-8.log` (stall lines ~1100-1115).
