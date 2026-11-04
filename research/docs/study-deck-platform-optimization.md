# Study: Steam Deck platform optimisation (kernel, RADV, power, gamescope, build)

Status: research only, 2026-11-04, branch `deck-lab` (`119b471`). Nothing built, launched or committed; no SSH.
Parent: [plan-linux-deck.md](plan-linux-deck.md). Companions, whose items are not repeated here:
[study-stage-b-per-draw.md](study-stage-b-per-draw.md) (C1-C11), [study-deck-other-threads.md](study-deck-other-threads.md)
(A-I), [study-deck-rescore-rejected.md](study-deck-rescore-rejected.md) (A1-D5, including `-march`, PGO and the TLS model).

Marks: **[code]** read in this tree. **[log]** a number from `data/` or plan-linux-deck.md. **[web]** a source in the
Sources list (date given where it matters). **[inferred]** reasoning or estimate, not measured. Nothing here is
"verified" in the project's sense.

The yardstick (plan-linux-deck.md, E1 and run 3): graveyard heavy view, ~65 FPS, **stage B 91% busy**, ~1,414 draws a
frame, 8.5 us a draw on stage B, every part 4-5x the PC's. 1 us per draw = ~1.4 ms a frame. CPU and GPU share one
power budget: pinning the GPU clock held the CPU at ~2 GHz. GPU at auto clocks: 800-1200 MHz, ~90% busy.

## Answer

1. **Huge pages are the biggest untested platform lever, and the first test needs no build.** Stage B's cost is
   memory latency on a CPU with a 4 MB L3, 150 ns DRAM and a 2,048-entry L2 DTLB (8 MB reach with 4 KiB pages).
   On the 7800X3D the page-table entries stay in a 96 MB L3; on the Deck each TLB miss is a page walk to DRAM.
   - The Deck almost certainly ships shmem THP off (`never`), so today's guest memory memfd is all 4 KiB pages.
   - Anonymous THP is probably `madvise`. The Gate 1 probe hints at both: memfd first-touch writes ran at 1.0 GB/s
     against 7.7 GB/s for anonymous memory.
   - Tests: measure the TLB misses first (perf, no root). Then one launch with THP forced on by sysfs (root, reverts
     at reboot). Code work after that: `MADV_COLLAPSE` on the eboot image, `glibc.malloc.hugetlb=1`.
   - Expected: 0-10% of stage B time [inferred]. This is the widest range here, so measure before building.
2. **CPU frequency policy is the second lever.** Valve-adjacent kernel work (David Vernet's `epp_boost`, July 2026,
   an RFC) measured +31.8% 1% lows on a Deck. The cause it names: threads that sleep briefly every frame wake at a
   low operating point. That is exactly our relay of stage A, stage B, the recorder and the game's submit thread.
   - Test: one launch with EPP `performance` (root, or `gamemoderun` if GameMode works). A read-only clock and power
     logger first decides whether it applies.
3. **Half-rate shading instead of render scale.** Render scale 67% costs culling: about 145 more draws a frame,
   measured. `RADV_FORCE_VRS=2x2` (the QAM "Half rate shading" toggle) cuts pixel-shading cost and keeps the depth
   and HZB at full resolution. A lighter GPU also leaves the shared power budget to the CPU. One launch, no build.
4. **RADV:** two cheap items for stage B:
   - `MESA_VK_ENABLE_SUBMIT_THREAD=1` moves the submit ioctl off stage B;
   - a GPU-written tick in host memory would remove every timeline-semaphore ioctl, which C1 only throttles.
   `VK_EXT_descriptor_heap` (default in Mesa 26.2; the Deck runs 26.1.99) is the long-term descriptor path.
5. **Build:** the Linux GPU library is a shared library without hidden visibility. Every call from it into the
   runtime (`runtime_memory_clamp` on the `ObtainBuffer` path) goes through the PLT and cannot be inlined, and the
   runtime C code is built without LTO. Linking statically as on Windows (or hidden visibility plus
   `-Bsymbolic-functions`) supersedes D1. PGO on clang, then BOLT (with `--hugify` for the code's huge pages),
   come after.
6. **Contradictions with the plan:** listed in section 8. The main ones:
   - gamescope FSR at a lower output resolution has the same culling cost as render scale;
   - "native Wayland" may not exist as a separate present path in Game Mode;
   - the 2022 frame-limiter latency figure predates Steam's Mesa-integrated limiter;
   - SMT and affinity tests silently switch the draw pipe off.

## 0. Facts to read on the Deck first (no launch, no root, over SSH)

One script, about 1 s of work. Sections 1-4 depend on its answers. Lines marked "during a run" read
`/proc/<pid>`: run them while the game is already running, light (a few `cat`s).

```sh
cd /sys/kernel/mm/transparent_hugepage && grep . enabled defrag shmem_enabled khugepaged/defrag 2>/dev/null
ls /sys/kernel/mm/transparent_hugepage/ | grep hugepages-       # mTHP sizes present
cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_driver /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor
cat /sys/devices/system/amd_pstate/status /sys/devices/system/cpu/cpu0/cpufreq/energy_performance_preference 2>/dev/null
cat /sys/kernel/sched_ext/state /sys/kernel/sched_ext/root/ops 2>/dev/null; systemctl list-units 'scx*' --no-pager
cat /proc/sys/kernel/perf_event_paranoid /proc/sys/vm/max_map_count; ulimit -e   # RLIMIT_NICE
cat /sys/devices/system/cpu/smt/control; cat /sys/devices/system/cpu/cpu*/topology/thread_siblings_list | sort -u
which gamemoderun gamemoded scx_lavd perf 2>/dev/null; pacman -Q gamemode scx-scheds 2>/dev/null
d=$(ls -d /sys/class/drm/card*/device | head -1); cat $d/pp_dpm_fclk $d/pp_dpm_sclk $d/power_dpm_force_performance_level
# during a run:
pid=$(pgrep -o medievil-runti)
tr '\0' '\n' < /proc/$pid/environ | grep -i -E 'gamescope|wsi|vk_|mesa|radv|display|glibc|ld_preload'
grep -i -E 'gamescope|wsi' /proc/$pid/maps | awk '{print $6}' | sort -u     # is the gamescope WSI layer loaded?
grep -E 'AnonHugePages|ShmemPmdMapped|FilePmdMapped|Rss' /proc/$pid/smaps_rollup; wc -l < /proc/$pid/maps
```

## 1. Memory and TLB

### 1.1 Why the Deck is different here [web] / [log]

| | Deck (Zen 2, Van Gogh) | PC (7800X3D, Zen 4) |
|---|---|---|
| L1 DTLB / L2 DTLB | 64 / 2,048 entries (4 KiB and 2 MiB) [web 7-cpu] | 72 / 3,072 [inferred from AMD guides] |
| L2 DTLB reach, 4 KiB pages | 8 MB | 12 MB |
| L2 DTLB reach, 2 MiB pages | 4 GB | 6 GB |
| L2 ITLB | 512 entries (2 MB reach with 4 KiB pages) [web] | 512 |
| L3 | 4 MB, one CCX | 96 MB |
| DRAM latency | ~150 ns LPDDR5 (LCD model) [web, Chips and Cheese 2023] | ~70-80 ns DDR5 [inferred] |

- A page walk reads up to 4 levels of page tables. On the PC those lines live in the 96 MB L3. On the Deck, with
  the game, the port and RADV sharing 4 MB, most walks go to DRAM at ~150 ns.
- Chips and Cheese also found the Deck's L3 "nearly absent" in latency tests on default Linux power settings, and
  better with the performance governor. That ties into section 3.
- Stage B's per-draw work is pointer chasing across several regions:
  - the port's C++ heaps: buffer, texture, memo and region tables;
  - guest memory: PM4 command buffers, V#/T# sharps, UE4's transient pool;
  - the 128 KiB record chunks.
- The working set is well above 8 MB [inferred], so a share of the 4-5x per-draw ratio is page walks rather than
  data misses.
- The 4.7-6.7x parts are the most memory-heavy ones: texslow, vertex, obt_sync ([log], study-stage-b-per-draw §1).
  That fits the picture but does not prove it.

**The Gate 1 probe hint** [log, design-u5-deck-memory.md §2]:
- CPU first-touch writes ran at 7.7 GB/s into anonymous memory and 1.0 GB/s into a memfd.
- `cpu_speed()` memsets a fresh mapping (`bloodborne_pc/tools/host_import_probe.c:240-260` [code]), so both figures
  include page faults.
- A 7.7x gap fits 4 KiB shmem faults against cheaper anonymous faults (or anonymous THP) [inferred]. Section 0's
  sysfs read settles it.

### 1.2 Where the port's memory is, and which huge-page mechanism reaches it [code]

| Memory | How it is made | Huge-page route | Notes |
|---|---|---|---|
| **eboot image** (~100 MB code + data, guest code runs natively) | `allocate()` → `runtime_low_map()`: anonymous private `mmap(MAP_FIXED_NOREPLACE)` at `0x0800000000` and up, 16 KiB aligned, then `fread`, patches, `mprotect` per segment (`src/probe.c:69-93`, `:938`; `src/runtime_memory.c:1120-1140`) | anonymous THP: `MADV_HUGEPAGE` before the `fread`, or `MADV_COLLAPSE` after patching. The mapping must be 2 MiB aligned | iTLB: 512 L2 ITLB entries cover 2 MB of code with 4 KiB pages. The segment `mprotect`s split VMAs, so only 2 MiB-aligned ranges inside each segment can be huge |
| **Guest direct + flexible memory** (5,056 MiB pool + 1 GiB flexible, sparse) | one `memfd_create` (no flags), backing view `mmap(NULL, MAP_SHARED\|MAP_NORESERVE)`, guest views `mmap(MAP_SHARED\|MAP_FIXED, phys)`; decommit = `fallocate(PUNCH_HOLE)` (`runtime_memory.c:443-466`) | shmem THP: `shmem_enabled` = `advise` + `MADV_HUGEPAGE` on the views, or `always` / `within_size`. `MFD_HUGETLB` needs a root-reserved hugetlb pool and only punches holes in 2 MiB units | 16 KiB guest pages; write tracking `mprotect`s GPU-read ranges, which splits a PMD mapping into PTEs (the folio stays huge; [web] kernel doc); UE4 decommits 64 KiB pool pages thousands of times a second (T-H1), and each punch inside a huge folio splits it |
| **Port heaps** (C++ caches, `std::function` per signal, maps), SDL, FFmpeg, Mesa | glibc malloc | `GLIBC_TUNABLES=glibc.malloc.hugetlb=1` (`madvise(MADV_HUGEPAGE)` on malloc's mmaps and heap) [web glibc manual], or system THP `always` | no build |
| **`libgow3gpu.so` text** | file mapping from the AppImage's squashfs over FUSE | not THP-eligible in practice. Routes: remap text to anonymous huge pages at start (BOLT `--hugify`, or a small remap routine), or link statically (section 5 B1) | probably a few MB of hot code [inferred] |
| **Vulkan memory** (arena, staging, rings) | amdgpu GTT / VRAM BOs | the kernel's TTM pool already allocates higher-order pages [inferred]; not ours to change | U5 would move guest memory here, making 1.3's shmem work moot for those ranges |

### 1.3 Items

**M0. Measure first** (no build, no root, rides along on any planned launch).
- `perf stat -e dTLB-load-misses,iTLB-load-misses,cycles,instructions -t <DrawRec tid> -- sleep 10` from the
  distrobox. Also run `perf list | grep -i -E 'tlb|tablewalk'` for Zen 2's page-walker events, if the generic ones
  do not map.
- `perf_event_paranoid` ≤ 2 lets a user count user-space events of their own process. Check it in section 0.
- Find stage B's thread id with `grep -l DrawRec /proc/$pid/task/*/comm`.
- Decision: if page walks are > ~5% of stage B's cycles, M1-M3 are worth a build; if < 2%, drop this section.
- Effort: none. Risk: none. perf is light, but it is still a tool running during a run.

**M1. System-wide THP for one launch** (root, no build; the sysfs writes revert at reboot):

```sh
echo always | sudo tee /sys/kernel/mm/transparent_hugepage/enabled
echo within_size | sudo tee /sys/kernel/mm/transparent_hugepage/shmem_enabled   # or advise / always
echo defer+madvise | sudo tee /sys/kernel/mm/transparent_hugepage/defrag       # no compaction stalls at fault
```

- Applies to the eboot (anonymous), every heap, and the guest memfd (shmem), wherever the mapping is 2 MiB aligned
  in both address and file offset.
- Read `AnonHugePages`, `ShmemPmdMapped` and `wc -l maps` (section 0) during the run to confirm it took effect.
- Expected: 0-10% stage B time, plus game-thread time [inferred].
- Risks:
  - more memory used (the pool is sparse and holes are punched at 64 KiB);
  - folio splits on every decommit punch, which cost kernel time on the game's threads;
  - with `defrag` set to `always`, compaction stalls. Keep `defer+madvise`.
- Test: one launch, graveyard heavy view. Compare stage B us per draw, FPS, p99 and the game's ident-64 wait with
  the E1 baseline. If it helps, do the same with `shmem_enabled` reset to `never`: that separates the guest memfd
  from the eboot and the heaps.

**M2. The eboot image in huge pages, from the code** (no root):
- Align the image allocation to 2 MiB: over-reserve in `runtime_low_map`, or a dedicated `low_next` alignment for
  the image.
- Call `madvise(MADV_HUGEPAGE)` before `fread`. Then, after patching and before the segment `mprotect`s, call
  `madvise(MADV_COLLAPSE)` (Linux 6.1+). `MADV_COLLAPSE` "is independent of the sysfs THP settings" [web man
  madvise; kernel doc]. It is refused only under `PR_SET_THP_DISABLE` or `MADV_NOHUGEPAGE`.
- RPCS3 does the same for its anonymous reservations: `MADV_HUGEPAGE` when the size is a 2 MiB multiple
  (`rpcs3/util/vm_native.cpp`) [web].
- Expected: guest code iTLB misses; 0-3% of the game's own threads [inferred]. Our stage B does not run guest code,
  so the gain is on the game's frame (the long frames came from the game's own frame, run 5).
- Effort: S. Risk: L. It needs the `low_next` alignment and must leave the code-patch paths alone.
- Test: two AppImages (or an env switch), A-B-A launches.

**M3. `GLIBC_TUNABLES=glibc.malloc.hugetlb=1`** (no build, no root):
- The port's heaps get `MADV_HUGEPAGE`. Under `enabled=madvise` that is the only way they become huge without M1.
- `extra-env.txt` accepts only `GOW3_`, `RADV_`, `MESA_`, `AMD_`, `VK_` and `SDL_` keys (`run.py:72-93`, case
  sensitive) [code]. So this needs one prefix added to `run.py` (`GLIBC_`), or a Steam launch option. The user keeps
  launch options plain, so the `run.py` prefix is the clean route.
- Expected: 0-3% [inferred].

**M4. Shmem huge pages for the guest memfd, in code**: after M1 shows a gain from the shmem part only.
- Map the views 2 MiB aligned with matching file offsets, and `madvise(MADV_HUGEPAGE)` the backing view (needs
  `shmem_enabled=advise` or above: root).
- `MADV_COLLAPSE` works without the sysfs setting. Apply it at boot to large, long-lived direct allocations, not to
  the UE4 pool pages that commit and decommit.
- Effort: M. Risk: M (split costs on decommit, the interaction with write tracking).
- Overlap: U5 (guest memory as Vulkan dma-buf) would replace the memfd for direct memory. Do M4 only if U5 stays
  closed.

**Not recommended: `MFD_HUGETLB` / hugetlbfs.** It needs `vm.nr_hugepages` reserved by root, it punches holes only in
2 MiB units (the pool decommits 64 KiB), and allocation fails hard when the pool runs out. RPCS3 tries it and falls
back to a plain memfd [web].

## 2. RADV / Mesa

What matters first: **stage B makes few driver calls.** The `vkCmd*` calls run on `gow3:VkRecorder` (63% busy, not
the wall). The driver calls on stage B are:
- timeline semaphore queries (ioctls; C1 throttles them);
- `vkUpdateDescriptorSets` for pipelines over 32 bindings (163 a frame; C6);
- `SubmitExecution`, which ends the command buffer and calls `vkQueueSubmit` on the thread that flushes
  (`vk_scheduler.cpp:877-945`; 2 submits a frame [log]);
- `flip_present`'s waits.

RADV per-draw CPU tuning therefore mostly helps the recorder and the power budget, not the wall.

**R1. `MESA_VK_ENABLE_SUBMIT_THREAD=1`** (no build, `extra-env.txt`):
- Mesa's common queue code then submits from its own thread, from the start. By default it spawns that thread only
  for wait-before-signal [web, Mesa commit "vulkan/queue: Rework submit thread enabling", 2022]. RADV uses the
  common queue.
- What it saves: the CS ioctl, and the syncobj work for the wait and signal semaphores, on stage B. Bloodborne
  measured ~65 us a submit (design-u5-deck-memory.md, Bloodborne table), so 2 a frame ≈ 0.1-0.2 ms of stage B
  [inferred].
- Risk: L. The submission moves to one more thread on 8, and fence timing shifts slightly.
- Test: one launch. Watch stage B busy, FPS and the "Fence lag" line.

**R2. A GPU-written tick instead of semaphore queries** (code, M):
- After each submission's work, the GPU writes the submission's tick into a small host-visible coherent buffer:
  `vkCmdWriteBufferMarker2AMD` (RADV has `VK_AMD_buffer_marker`), or a `vkCmdFillBuffer` after a barrier.
- `Semaphore::Refresh` (`vk_semaphore.cpp:39`) then reads it with an atomic load, and queries the semaphore only
  when a caller must sleep.
- On RADV each `vkGetSemaphoreCounterValue` is a `DRM_IOCTL_SYNCOBJ_QUERY` (study-stage-b-per-draw §3). This
  removes all of them, from every caller: `PopPendingOperations`, the fault paths, staging, `IsFree`.
- Bloodborne already writes labels this way (`WriteLabelOnGpu`, design-u5-deck-memory.md).
- Expected: C1's measured saving rises to the full ~0.5-0.8 ms a frame "not CPU speed" excess, minus C1
  [inferred]. Read C1's "Stage B syscalls" line first: if queries cost < 0.5 us, drop R2.
- Risk: L-M. The marker write must come after all of the submission's work.

**R3. Half-rate shading: `RADV_FORCE_VRS=2x2`** (no build; `RADV_` is an allowed extra-env prefix; or the QAM
"Half rate shading" toggle):
- RADV forces 2x2 variable-rate shading on GFX10.3. The `RADV_FORCE_VRS_CONFIG_FILE` form is what Steam's toggle
  uses, and it affects only GFX10.3 APUs [web Mesa envvars]. Steam's session exports `STEAM_USE_DYNAMIC_VRS=1`
  [web gamescope-session].
- Why it suits this port:
  - render scale 67% → 100% doubled HZB culling (~63 → ~122 hidden a frame, ~145 fewer draws, ~10%), but lost FPS
    at auto clocks because of GPU cost [log, "HZB resolution and culling"];
  - VRS cuts fragment cost while keeping the depth buffer (and so the HZB) at full resolution;
  - a less loaded GPU also lets the SoC give power to the CPU (section 3).
- Expected: at 100% + VRS 2x2, the ~10% fewer draws ≈ ~1.2 ms of stage B, if the GPU stops being the limit
  [inferred]. Image quality: blurrier shading, with edges kept.
- Risk: L. Unknown interactions with UE4's post-processing, UI and temporal AA (the port's upscaler is off).
- Test: three launches, all at 100% render scale: VRS on, VRS off, and 67% without VRS (the reference). Compare FPS,
  draws a frame, HZB hidden, and GPU busy and clock (MangoHud level 2+).

**R4. `VK_EXT_descriptor_heap`** (long term, L):
- RADV merged it in Mesa 26.1 behind `RADV_EXPERIMENTAL=heap`, and Mesa 26.2-devel exposes it by default
  [web Phoronix, April and June 2026]. The Deck runs RADV 26.1.99 (26.2-devel), so check `vulkaninfo`.
- Stage B could write image and sampler descriptors straight into a GPU-visible heap. That replaces:
  - the push-descriptor bulk copy (17.6 writes a draw, "descriptors" 0.76 us a packet);
  - the recorder's `vkCmdPushDescriptorSetKHR`;
  - the over-32-binding `vkUpdateDescriptorSets` path (C6).
- It needs the shader recompiler's binding model changed (study-stage-b-native.md S3 hit the same wall). DXVK 3.0
  uses heaps by default [web].
- Expected: 0.3-0.8 us a draw on stage B, plus recorder time [inferred]. Not before C3-C6.

**R5. GPU-side shader options** (no build, GPU leg and power only):
- `RADV_PERFTEST=pswave32`, `cswave32`, `gewave32` [web Mesa envvars]. One launch each, after R3.
- The pipeline cache is keyed on these options, so the first run recompiles everything: expect hitches.
- Not for the Deck: `nggc` (GFX11+), `sam` / `dmashaders` (discrete GPUs), `RADV_EXPERIMENTAL=transfer_queue`.
- `RADV_DEBUG=nohiz` / `nodcc` are diagnostics only (S7).

**R6. Shader disk cache.**
- Defaults: `MESA_SHADER_CACHE_MAX_SIZE` 1 GB, multi-file cache in `~/.cache/mesa_shader_cache` [web].
- Steam's Fossilize pre-caching does not cover a non-Steam AppImage shortcut [inferred].
- The 0.6-1.1 s first-visit hitches (native run 1) are partly pipeline builds. Check: `du -sh ~/.cache/mesa_shader_cache*`
  and the environ for `MESA_SHADER_CACHE_DIR`.
- If the cache is near 1 GB, set `MESA_SHADER_CACHE_MAX_SIZE=4G`. No cost.

## 3. CPU scheduling and power

**P0. Clock and power logger** (no root, no build). A 200 ms loop over SSH, in the background of a run:
- per-CPU `scaling_cur_freq`;
- `pp_dpm_fclk` and `pp_dpm_sclk` (the `*` level);
- `gpu_busy_percent`;
- the amdgpu hwmon `power1_average` (socket power).

What it answers:
- Does stage B's core run below 3.5 GHz in the heavy view?
- Does fclk drop when the GPU is at 800 MHz? On Van Gogh, mclk is bonded to fclk [web amd-gfx patch], so a low
  fclk raises the CPU's DRAM latency.
- How much of the budget does the GPU take?

It decides P1 and P2. Cost: a few `cat`s every 200 ms. That is lighter than MangoHud, but tell the user it runs
(the no-background-load rule).

**P1. EPP / governor `performance` for one launch.**
- What:
  - with `amd-pstate` in active mode: `echo performance | sudo tee /sys/devices/system/cpu/cpu*/cpufreq/energy_performance_preference`;
  - with passive or guided mode: `scaling_governor` = `performance`;
  - both revert at reboot.
  - Or `gamemoderun` through the run script, if GameMode is installed and its polkit helper works in Game Mode.
    It is reported present on SteamOS but not active in Game Mode [web forum].
- Evidence:
  - Vernet's `epp_boost` (RFC, July 2026, not merged) gave +31.8% 1% lows and +4.1% p99 in Civilization VI on a
    Deck. Mechanism, from the coverage: "Each sleep decays the hardware's performance signal, causing post-wakeup
    bursts to start at a low operating point" [web].
  - Chips and Cheese (2023): 1.7 GHz held for hundreds of ms and ~1 s to full boost; the L3 looked better under the
    performance governor [web].
- Applies: stage A, stage B, the recorder (7.3k sleeps/s) and the game's submit thread hand off every frame, and
  the problem is the lows (run 5).
- Expected: lows and p99 better, average 0-5% [inferred]. Risk: the CPU takes power from the GPU, which is ~90% busy
  at auto clocks. That is the trade P0 measures. Effort: none. Root: yes (or GameMode).
- Test: one launch, same spot. Report FPS, 1% / 0.1% lows, p99, stage B us per draw and GPU clock.

**P2. SMT and affinity.**
- `taskset -c <one logical CPU per core>` emulates 4C/4T without root. Note that the port sizes itself from the
  affinity mask (`gow3_threads.h:97-105`):
  - **the draw pipe turns off below 8** (`vk_rasterizer.cpp:412-419`);
  - prep workers drop to 2 (`vk_draw_prep.cpp:56`);
  - copy threads drop to 1 (`gow3_copy.cpp:29`).

  So `GOW3_DRAW_PIPE=1` must go with it.
- True SMT off (`/sys/devices/system/cpu/smt/control`) needs root. No QAM toggle was found [web].
- Evidence: the dedicated core with its sibling free took stage B busy from 79% to 70%, but FPS did not follow
  [log, helpers run]. The game runs 67 guest threads plus ~10 of ours.
- Expected: -10% to +5% [inferred]. Low priority after A3 (dedicated core), which already covers the sibling
  question.

**P3. EEVDF custom slices for the critical chain** (code, S, no root).
- What: `sched_setattr(SCHED_OTHER, sched_runtime = 0.5-1 ms)` on stage B, stage A, the recorder and the vblank
  thread.
- Since Linux 6.12 a shorter requested slice gives an earlier virtual deadline and lets the waking task preempt.
  "Any task can request shorter time slices", clamped to 0.1-100 ms [web LWN, heise].
- Applies on the Deck: 8 hardware threads, ~77 runnable-capable threads, and guest wake-ups at 10-15k/s preempting
  everything (study-deck-other-threads §3.2).
- Expected: fewer run-queue delays on the chain; better lows, average 0-3% [inferred].
- Risk: L. Too short a slice preempts the task itself more often, so do not go under 0.5 ms.
- Unprivileged routes that do **not** work:
  - negative nice needs `CAP_SYS_NICE` or `RLIMIT_NICE` (read `ulimit -e`);
  - raising `uclamp.min` needs `CAP_SYS_NICE` since 2021 [web LKML].
- Test: A/B in one launch, if the setting is applied per thread from a toggle bit (re-applied when it changes).

**P4. `scx_lavd`, if SteamOS ships it.**
- Igalia wrote this sched_ext scheduler under contract for Valve, for the Deck. It ranks tasks by latency
  criticality (wake and wait patterns) [web LWN 2024, Phoronix 2025].
- Whether SteamOS 3.9 enables it by default was **not** confirmed by any primary source [web]. Section 0 reads
  `/sys/kernel/sched_ext/state`.
- If it exists but is off: one launch with `sudo scx_lavd` (or `--performance`) running. It stops cleanly and falls
  back to EEVDF.
- Applies: our chain is the waker-wakee pattern LAVD targets. Expected: lows [inferred]. Risk: L (it unloads on
  error). Root: yes.

**P5. Spinning costs boost.**
- On a shared 15 W budget, a spinning thread does more than take a hardware thread: it heats the SoC and lowers
  everyone's boost. The user saw "CPU clocks boost higher" with the drain sleep (plan-linux-deck.md).
- So the recorder spin (study-deck-other-threads B), stage A's yield-spin (A8) and the prep workers (A) are worth
  more on the Deck than their CPU % says [inferred].
- Measure each with P0's socket power and per-core clock alongside FPS.

**P6. Not recommended:**
- pinning the GPU clock (measured: the CPU drops to ~2 GHz);
- forcing fclk or `profile_peak` (root; Van Gogh clocks have stuck after the profile modes [web drm/amd #1726]);
- deep C-state limits through `/dev/cpu_dma_latency` (root; at 70-80% load the cores rarely go deep [inferred]).

## 4. gamescope / SteamOS

**G1. Which present path does the game use in Game Mode?** (no launch beyond a planned one; section 0)
- Gamescope's Vulkan WSI layer (`VkLayer_FROG_gamescope_wsi`) intercepts swapchain creation and presents to
  gamescope directly [web].
- Game Mode clients get an X11 `DISPLAY`. A Wayland client socket is not offered by default [inferred].
- If the layer `.so` shows in `/proc/<pid>/maps`, our "X11 / XWayland present path" suspect for `flip_present`
  (plan-linux-deck.md run 3) is already bypassed. The "native Wayland first" item would then change nothing in Game
  Mode: see section 8.
- Also read `vk_xwayland_wait_ready` in the environ. Gamescope setups set it to `false` ("don't wait for buffers to
  idle on the client side") [web, community configs].
- If it is unset and the layer is not loaded, test `vk_xwayland_wait_ready=false`. Lower-case keys need a `run.py`
  prefix (M3).

**G2. Frame limiter: the plan's evidence is dated.**
- The plan rejects Steam's limiter on 2022 measurements (~110 ms at 30 FPS).
- Valve's session now exports `STEAM_GAMESCOPE_DYNAMIC_FPSLIMITER=1` "We have the Mesa integration for the
  fifo-based dynamic fps-limiter" [web gamescope-session]. That is a different mechanism from 2022. The gamescope
  latency issue #474 still had complaints at SteamOS 3.5.5 [web].
- Keep the port's own limiter as the default, but retest Steam's at 60 once. It needs VSync on (FIFO), and costs
  the port no CPU [inferred].

**G3. OLED refresh and pacing** (QAM, no build):
- At 90 Hz with no cap, ~65 FPS shows as 11.1 / 22.2 ms steps. That may be the user's "10 ms and 20 ms frames
  ping-ponging".
- Options, one launch each:
  - panel 60 Hz + cap 60 (the PC rule, tick = cap);
  - panel 90 Hz + "Allow tearing" (immediate flips, lowest latency, tears);
  - 90 Hz + cap 45 (even 2-refresh pacing).
- The internal panel has no VRR; HDR adds composition [inferred], so leave it off for measurements.

**G4. Gamescope upscaling (FSR 1 / NIS) at a lower output resolution has the same culling cost as render scale.**
- The game renders and builds its HZB at the lower resolution, so it draws more (section 2, R3). The plan's
  "960x600 + gamescope FSR 1" option (§5.5) is not free for stage B.
- Prefer full-resolution depth with R3. If the GPU must be cut, use the port's render scale (UE4 temporal) over
  gamescope's: same draw cost, better image.

**G5. Composition.**
- The scaling filters, the Steam performance overlay (mangoapp) and HDR make gamescope composite every frame (GPU
  work) instead of flipping the game's image directly [inferred from gamescope's design; not measured].
- For the final Deck numbers, take one run with the overlay off and timings from the port's own log.

## 5. Build level

**B1. Link the GPU library statically on Linux too, or at least stop interposition.** [code]
- Today:
  - `libgow3gpu.so` is `SHARED` off Windows (`gpu/CMakeLists.txt:145-151`), with no `-fvisibility=hidden` on the
    library: the flag appears only on test sources (`:264`, `:314`, `:320`);
  - the runtime C is compiled with `-O2` and no LTO (`build.sh:91`);
  - calls from the library into the runtime go through the PLT into the `-rdynamic` executable, and cannot be
    inlined across the boundary. `MemoryManager::ClampRangeSize` → `runtime_memory_clamp` is one, on
    `ObtainBuffer`'s path (`gow3gpu.cpp:115-117`);
  - every thread-local in the library uses the general-dynamic TLS model (D1).
- Options, smallest first:
  - `-fvisibility=hidden` plus explicit exports of the `gow3gpu_*` API, or `-Wl,-Bsymbolic-functions` with
    `-fno-semantic-interposition`;
  - D1 (`-ftls-model=initial-exec`);
  - **a static library on Linux as on Windows**: local-exec TLS, direct calls, and one LTO unit if the runtime C
    joins it (`-flto=thin` on the `$CC` line).
- Expected: 1-4% of stage B (PLT, GOT loads, `__tls_get_addr` at ~3% of stage B in spacepepperoni's profile),
  [inferred]. It also helps the game's threads, which call runtime HLE functions.
- Effort: S (flags) to M (static link: the `--allow-shlib-undefined` model changes). Risk: L.
- Test: two AppImages, A-B-A. Confirm first with `perf report` on DrawRec: count `@plt` and `__tls_get_addr`
  samples (no build).

**B2. `-march`.** Both machines support x86-64-v3 (AVX2, BMI2, FMA, MOVBE), so one `-march=x86-64-v3 -mtune=znver2`
artifact could serve both if pre-Haswell CPUs are dropped. Otherwise ship a Deck-only artifact (C15). 0-3%
[inferred]: stage B is pointer chasing, not SIMD.

**B3. PGO on clang, then BOLT.**
- PGO: the PGO path is GCC-style (`.gcda`, `__gcov_dump`; `build.sh:56-62`) and does not work with clang (C14).
  Clang IR PGO needs `-fprofile-generate` → `llvm-profdata merge` → `-fprofile-use=x.profdata`, with
  `__llvm_profile_write_file` in place of `__gcov_dump`. **Train on the Deck**: its layout and hot set are what
  matter.
- BOLT:
  - Zen 2 has no LBR, so use instrumentation (`llvm-bolt -instrument`) or sample-only profiles, "performance gains
    ... expected to be lower" [web BOLT README];
  - it needs `--emit-relocs` at link; shared-library support is "added recently" (one more reason for B1's static
    link);
  - `--hugify` puts the hot text in huge pages (M-row "libgow3gpu.so text").
- Expected: PGO ~3-5% per draw (Bloodborne GCC: LTO + PGO -4.8% per draw [log, docs/parallel_gpu.md]). BOLT on top
  is usually a few % on large front-end-bound C++ [inferred]; Zen 2's 512-entry L2 ITLB and 4 MB L3 favour it.
- Effort: M (PGO), M-L (BOLT pipeline). Risk: L (build only).

**B4. Allocator.**
- Guest `malloc` is the game's own libc (guest code in guest memory) [code: `runtime.c` binds only a few libc
  entries], so a host allocator affects only the port, SDL, FFmpeg and Mesa.
- No build needed: `LD_PRELOAD` a `libmimalloc.so` shipped in the AppDir, plus the `run.py` prefix change (M3).
- Hot host allocations: a `std::function` per signal on stage B (study-stage-b-per-draw §3), the texture and
  buffer cache maps.
- Expected: 0-2% [inferred]. M3 first (it is free). Risk: L-M (allocator mixing if any library frees across a
  `dlopen` boundary).

**B5. Not checked:** what the RPCS3, PCSX2 and Dolphin AppImages ship (compiler, LTO, `-march`). Only RPCS3's huge-page
code (M2) was read.

## 6. Other Deck-specific items

| # | Item | Applies? | Test |
|---|---|---|---|
| O1 | **VMA count.** Write tracking `mprotect`s 16 KiB-granular ranges, and every partial `mprotect` splits a VMA. SteamOS sets `vm.max_map_count` to 2,147,483,642 [web Phoronix], so there is no failure on the Deck. On stock Fedora / Ubuntu (65,530 → 1,048,576 recently) a long session could hit the limit [inferred]. Many VMAs also lengthen `mprotect` / `mmap` under the address-space write lock | the count is unknown | `wc -l /proc/<pid>/maps` (section 0) over a 10-minute run |
| O2 | Split-lock detection (the kernel slows tasks that use split-locked atomics) | **no**: Intel and Zen 5 bus-lock detection only; Zen 2 has none [inferred] | — |
| O3 | OLED LPDDR5-6400 vs LCD 5500 | the latency figure above is the LCD's; the OLED is somewhat better [inferred] | — |
| O4 | Timer slack for guest threads (`PR_SET_TIMERSLACK`) | already item G in study-deck-other-threads | — |
| O5 | `hw.ncpu` = 7 sizes UE4's pools (16 PoolThreads, 8 FImgMedia, 12 task-graph threads) | it is the PS4's value; changing it changes guest behaviour. Not proposed | — |

## 7. Ranked shortlist (cheapest and most likely first)

| Rank | Item | Cost | Root? | Expected (all [inferred]) |
|---|---|---|---|---|
| 1 | **Section 0 facts + P0 clock/power logger + M0 perf TLB counters** | no launch of its own; read-only on a planned run | no | decides 2-6 |
| 2 | **R3 half-rate shading at 100% render scale** (`RADV_FORCE_VRS=2x2` or QAM) vs 67% | 2-3 launches, no build | no | ~10% fewer draws kept (≈1.2 ms of stage B) if the GPU allows; better power split |
| 3 | **P1 EPP / governor `performance`** | 1 launch, no build | yes (or GameMode) | lows / p99 better; average 0-5% |
| 4 | **M1 THP `always` + shmem `within_size`** (then shmem `never` to split) | 1-2 launches, no build | yes | 0-10% stage B, decided by M0 |
| 5 | **R1 `MESA_VK_ENABLE_SUBMIT_THREAD=1`** | 1 launch, no build | no | 0.1-0.2 ms of stage B |
| 6 | G2 / G3: Steam's limiter at 60 once; 60 Hz + cap 60 vs 90 Hz + tearing | 1 launch each, QAM | no | pacing, not FPS |
| 7 | M3 `glibc.malloc.hugetlb=1` (with the `run.py` prefix change) | 1-line change + 1 launch | no | 0-3% |
| 8 | P4 `scx_lavd`, if present | 1 launch | yes | lows |
| 9 | B1 hidden visibility / static GPU library (includes D1) | build, A-B-A | no | 1-4% |
| 10 | R2 GPU-written tick (after C1's counter) | code M | no | up to ~0.5 ms a frame |
| 11 | P3 EEVDF custom slices on the critical chain | code S, toggle | no | lows |
| 12 | M2 eboot `MADV_COLLAPSE`; M4 guest memfd huge pages (only if M1's shmem half wins and U5 stays closed) | code S / M | no / yes for M4's sysfs | 0-3% / part of M1 |
| 13 | B3 clang PGO trained on the Deck, then BOLT with `--hugify` | build pipeline M-L | no | 3-5% + a few % |
| 14 | R4 descriptor heap | L | no | 0.3-0.8 us a draw |

Order of launches: 1 rides along on the next planned launch (C1's toggle test); then 2, 3, 4, 5. Each is one change.
Root items (3, 4, 8) use `sudo` in an SSH session before the user presses Play. They revert at reboot. They are
measurements, not settings to ship: shipping needs the code routes (M2-M4, GameMode / P3).

## 8. Where this contradicts or qualifies the current plan

1. **"Output 960x600 + gamescope FSR 1" and render scale 67-75% as the Deck's GPU levers** (plan §5.5, Deck runs).
   Both coarsen the HZB and add ~10% draws to stage B, the wall. Half-rate shading (R3) cuts the GPU without that
   cost. Test it before choosing the Deck preset's resolution.
2. **"Native Wayland first" for `flip_present`** (run 3's order).
   - In Game Mode the gamescope WSI layer may already present over Wayland, and clients may not get a Wayland socket
     [inferred]. Run section 0's maps/environ check before any work on it.
   - The present itself is 0.94 ms; the rest of `flip_present` is waits (run 4).
3. **"Gamescope's limiter off: ~110 ms latency"** (plan §5.5) cites 2022 tests. Steam now uses a Mesa-integrated FIFO
   limiter (G2). The conclusion may still hold, but the evidence is dated. Retest once.
4. **SMT or affinity experiments** must set `GOW3_DRAW_PIPE=1`. Below 8 available CPUs the port switches the draw pipe
   off, along with prep and copy thread counts (P2). A `taskset` run would otherwise compare a different pipeline.
5. **"No pinned GPU clock"** stands, but the reason generalises: on this SoC any GPU work saved is CPU clock gained.
   So GPU-side cuts (R3, R5, effects resolution) can raise stage B's speed, not only the GPU's. Rank GPU savings by
   that too.
6. **D1 (`-ftls-model=initial-exec`)** is a subset of B1. If a Linux build change is made, make B1's.

## Sources

Kernel and libc:
- Transparent huge pages admin guide (shmem_enabled, per-size mTHP, MADV_COLLAPSE ignores sysfs, PMD split on partial
  mprotect): https://docs.kernel.org/admin-guide/mm/transhuge.html
- madvise(2) (MADV_COLLAPSE since 6.1; MADV_HUGEPAGE on shmem since 5.4): https://man7.org/linux/man-pages/man2/madvise.2.html
- glibc memory allocation tunables (`glibc.malloc.hugetlb`): https://sourceware.org/glibc/manual/latest/html_node/Memory-Allocation-Tunables.html
- EEVDF custom slices (LWN, April 2024; merged 6.12 per heise): https://lwn.net/Articles/969062 , https://heise.de/-9949941 ,
  commit "sched/eevdf: Use sched_attr::sched_runtime to set request/slice suggestion": https://upd.dev/kernel/linux/commit/857b158dc5e8
- uclamp increases need CAP_SYS_NICE (2021): https://lkml.iu.edu/hypermail/linux/kernel/2106.1/04306.html
- Van Gogh mclk bonded to fclk: https://yhbt.net/lore/all/BN6PR12MB112431A20309BCCCBC4BD7F8F14E9@BN6PR12MB1124.namprd12.prod.outlook.com/T ;
  stuck clocks after profile modes: https://gitlab.freedesktop.org/drm/amd/-/issues/1726

Hardware:
- Chips and Cheese, "Van Gogh, AMD's Steam Deck APU" (2023-03; 4 MB L3, ~150 ns LPDDR5, clock ramp, power sharing):
  https://old.chipsandcheese.com/2023/03/05/van-gogh-amds-steam-deck-apu/
- Zen 2 TLBs: https://7-cpu.com/cpu/Zen2.html ; AMD-guide summary: https://courses.physics.illinois.edu/cs433/fa2022/projects/AMD-Zen.pdf

Power and scheduling:
- `epp_boost` RFC, Steam Deck Civilization VI +31.8% 1% lows (2026-07): https://steamdeckhq.com/linux-patches-boost-1-low-framerates-steam-deck/ ,
  https://phoronix.com/news/AMD-P-State-Better-1p-Lows
- scx_lavd: https://github.com/sched-ext/scx (scheds/rust/scx_lavd), https://lwn.net/Articles/991205/ ,
  https://www.phoronix.com/news/Meta-SCX-LAVD-Steam-Deck-Server
- GameMode: https://github.com/FeralInteractive/gamemode ; presence on SteamOS (forum, unverified):
  https://www.gamingonlinux.com/2023/12/feral-gamemode-v18-out-now-with-cpu-core-pinning-and-parking/page=1/

Mesa / RADV:
- Environment variables (RADV_DEBUG, RADV_PERFTEST, RADV_FORCE_VRS(_CONFIG_FILE), shader cache): https://docs.mesa3d.org/envvars.html
- Submit thread (`MESA_VK_ENABLE_SUBMIT_THREAD`, 2022): https://lists.freedesktop.org/archives/mesa-commit/2022-March/131411.html
- VK_EXT_descriptor_heap in RADV (Mesa 26.1, default in 26.2-devel): https://www.phoronix.com/news/RADV-Merges-Descriptor-Heap ,
  https://phoronix.com/news/RADV-Descriptor-Heap-Default

gamescope / SteamOS:
- gamescope session environment (dynamic FIFO limiter, dynamic VRS, tearing): https://github.com/ChimeraOS/gamescope-session-steam
  (`usr/share/gamescope-session-plus/sessions.d/steam`)
- Frame limiter latency issue: https://github.com/ValveSoftware/gamescope/issues/474
- `vk_xwayland_wait_ready=false` in gamescope setups (community): https://gist.github.com/matte-schwartz/c9a0735105eb63af16c64e16c3dd640f
- THP defaults on SteamOS as reported by CryoUtilities (community, 2023; read sysfs instead):
  https://deepwiki.com/CryoByte33/steam-deck-utilities/3.1-memory-management
- `vm.max_map_count` on SteamOS: https://www.phoronix.com/news/Fedora-39-VM-Max-Map-Count

Emulators and tools:
- RPCS3 `MADV_HUGEPAGE` / `MFD_HUGETLB` with fallback: https://github.com/RPCS3/rpcs3/blob/master/rpcs3/util/vm_native.cpp
- yuzu "host_memory: Use transparent huge pages where available" (2023-01, commit fd1831b, diff seen only in part)
- LLVM BOLT (no-LBR profiles, instrumentation, `--emit-relocs`, .so support): https://github.com/llvm/llvm-project/blob/main/bolt/README.md

Repo: `src/runtime_memory.c:436-466`, `:1120-1140`; `src/probe.c:69-93`, `:938`; `run.py:72-93`; `build.sh:56-91`;
`gpu/CMakeLists.txt:11-40`, `:145-174`; `gpu/shim/gow3_threads.h:97-113`; `gpu/shim/gow3gpu.cpp:115-130`;
`vk_rasterizer.cpp:412-419`; `vk_scheduler.cpp:530`, `:728-945`; `vk_semaphore.cpp:39`; `vk_draw_prep.cpp:56`;
`gow3_copy.cpp:29`; `bloodborne_pc/tools/host_import_probe.c:240-326`.
