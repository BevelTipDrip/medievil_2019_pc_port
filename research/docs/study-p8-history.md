# Study: P8 history (guest write faults that need no GPU work drain the draw pipe)

Status: research only (static: code, git history on all branches, patch cards, the Bloodborne port), 2026-11-24.
Nothing built, nothing launched. Parent: [plan-work-queue.md](plan-work-queue.md) row P8,
[study-perf-deepdive-20261123.md](study-perf-deepdive-20261123.md) "Memory, transfers, synchronisation" item 1.
[V] seen in code, git or data; [I] inferred.

## Verdict in short

- **Nobody has been down this path.** No commit, branch, card, doc or stash moves any part of the write-fault decision
  off the `SendCommand<true>` path, here or in the Bloodborne port (`bloodborne_pc` 0.51 still sends twins through
  the command, `buffer_cache.cpp:703-785`). Nothing to "un-abandon" [V].
- **The command path for these branches was never chosen; it is inherited.** Upstream shadPS4's `ReadMemory` posts
  every non-GPU-thread flush as a command (`6849d33`). D1 (`057afc6`), twins (`2a40666`, Bloodborne 0.4) and
  fault-page-download (`94b31c3`) were each added *inside* that lambda. Their evidence counted GPU waits
  (`download_ns`, measured inside `DownloadMemory`), so the drain the guest thread pays before the lambda runs was never
  measured [V code, I for "never looked at"].
- **The drain itself was chosen, for a different reason**: L5 (`bc0e1d6`, Bloodborne `4d4d208`). Commands record into
  the scheduler, and a command running beside stage B caused H1 (hang) and C1 (driver crash). The twin, no-GPU-bytes
  and unmap-discard branches record nothing. They get a side benefit from the drain, though: stage B is the only
  thread that touches `gpu_modified_ranges` and `twins` while the pipe has work. That is the invariant P8 has to
  replace.
- **The direction is sound, but the proposal as written has gaps** (section 5): four mutation sites are missing, the
  stated lock order cannot be met on stage B's written-binding path, a thread re-entering the lock would deadlock
  itself, and the queue row lists "finished-page" as inlinable, which it is not. With those fixed it is worth
  building, behind a switch.

## 1. Timeline: everything related

| Date | Commit / card | What | Outcome |
|---|---|---|---|
| 2026-09-25 | `6849d33` (import) | Upstream shadPS4 fault path: `GuestFaultSignalHandler` → `ReadMemory(…, assume_locks = IsGpuSideThread())`; non-GPU threads `SendCommand<true>`; the lambda only downloads a 512 KiB window | Base design. The tracker's own comment (`memory_tracker.h:83-86`) already expects guest threads racing the GPU thread on region state |
| 2026-09-25 | `eab5c93` / `758b560` | Fault window: unprotect 64 then 256 KiB around a write fault (`ExtendWriteFault`, guest thread, region locks, no command) | Active. First guest-thread mutation of tracker state outside the command |
| 2026-09-30 | `13486e5` (BB lineage) | Two-stage draw pipe; `IsGpuSideThread` = stage A or stage B; stage B handles its own faults inline | Active |
| 2026-09-30 / 10-01 | `3b818cd`, BB `542f2d4` | Guest heap corruption with the draw pipe; "buffer downloads on page faults" and "fault handling when stage A is not treated as GPU-side" were suspects | Cause was deferred fences (GPU idle after deferred signals), not faults |
| 2026-10-06 | BB `4d4d208` (0.3) | Drain stage B before **every** queued command ("null chunk crashes …, lost chunks, hangs") | Kept in BB |
| 2026-10-07+ | BB 0.4 (`daa7165`) / CHANGES_after_0.4 | Twins ported into the flush lambda. Precise-readbacks freeze: stage A, snapshotting twins under `twins_mutex`, hit a read-protected page; its own fault handler waited for the same mutex | Fixed by reading guest bytes through the backing view (`ReadBacking`); twins off with Precise readbacks |
| 2026-10-18 | `bc0e1d6` L5 ([card](../patches/stage-a/l5.md)), [plan-baseline-issues.md](plan-baseline-issues.md) "Root cause found" | H1 (stage A stuck in `WaitHostCopies`, stage B's chunk never handed over) and C1 (`vkCmdDrawIndexed` null read, draw split across command buffers): a guest fault's command ran on stage A beside stage B | Fixed. T-race / T-L5 evidence; L4 re-kick reverted (`a07f709`) |
| 2026-10-19 | `057afc6` D1 ([card](../patches/caches/d1.md)) | Unmap discard inside the lambda | Active; 0/s in T-D1b and on the Deck, now measured up to ~17/s |
| 2026-10-19 | `1bc2dda` / `42a4943` D2 + D2b | Libc writes into GPU-written pages queued, no fault; `ProcessCpuWritesLocked` on stage B (`BeforeRecord`) and in `DownloadMemory` | Active |
| 2026-10-19 | `5a5d73f` D3 | Downloads on a readback queue, wait only for the newest write tick (`WriteTicks`, lock-free atomics) | Active |
| 2026-10-19 | `2a40666` twins ([card](../patches/caches/gpu-write-twins.md)) | Twin decision and snapshot inside the lambda, on stage A after the drain | Active; T-T1 verified in game |
| 2026-10-28 | `94b31c3` fault-page-download ([card](../patches/caches/fault-page-download.md)) | Finished-page download, no-GPU-bytes branch, twin limit 1 → 16 KiB, all in the lambda | Active; t-map3 "0.1-0.6 ms of waits a second" = GPU waits only |
| 2026-11-04 | `c4a0147` E1-deck-drains | OnSubmit work as a stage-B task instead of after two drains; impl-decode-pacing.md writes down the ownership rule: "stage B owns the texture cache, buffer cache, scheduler and staging pool whenever the pipe has work. Stage A touches them only after a drain … guest threads take the texture cache mutex" | Active. The closest precedent: drains removed by moving work to the owner, not by locking |
| 2026-11-08 | `7af0988`, `data/u1-hang-recorder-fault-20261108.txt` | Hang at quit to main menu: `gow3:VkRecorder` read-faults in `RunGuestCopy`, `SendCommand<true>` → stage A `Drain` → stage B `WaitDeferredSignals` → recorder | Work-queue row still "found; fix to propose"; same stack as the 200% hang below [I: covered by `3a84125` on Windows] |
| 2026-11-15 | `3a84125` deferred-copy-backing ([card](../patches/stage-b/deferred-copy-backing.md)), copy-fault-trace | 200% hang, same cycle. Fix: deferred copies read the backing view so they never fault. Rejected before building: a stage A wait when protecting a page with pending copy sources | Active (Windows), fix built, run |

Rejected neighbours (same "cheaper write tracking" family, different idea): **hot-pages** (pages stay writable:
re-upload cost, 14k pages, 33 FPS in the BB lineage) and **uffd** (one handler thread, worse lows on the Deck).
Neither touched the command or the drain.

## 2. Why the command and the drain exist

1. **Commands record into the scheduler.** `DownloadMemory` records copies and either `Finish()`es or uses the
   readback queue. `ProcessCpuWritesLocked`, which it runs first, records staged `CopyBuffer`s for D2's queued writes
   (`buffer_cache.cpp:1085-1089`, `1668-1672`). `ResolveReadbacks` waits and flushes. Two recording threads is the
   H1/C1 race. That is the L5 reason, and it applies only to branches that record.
2. **Single-owner cache state.** `gpu_modified_ranges` is a plain `RangeSet` with no lock (`buffer_cache.h:230`). Its
   writers are stage B (`ObtainBuffer` Add, `SynchronizeMemory` twin Subtract, `ProcessCpuWritesLocked` Subtract)
   and stage A after a drain (commands, stage A's own inline faults). The drain serialises the fault lambda with all
   of them for free. `twins` has `twins_mutex`, but the twin *decision* reads `gpu_modified_ranges`.
3. **Atomic "GPU mark + range + tick".** On a written binding, stage B sets the tracker's GPU bit inside
   `ForEachUploadRange`'s second loop, releases the region locks, and only then does `gpu_modified_ranges.Add` +
   `WriteTicks().Note` (`memory_tracker.h:101-133`, `buffer_cache.cpp:1364-1366`). A fault decided in that window
   sees the GPU bit but no ranges. It takes the no-GPU-bytes branch and unmarks the page, so the shader output is lost
   on the next upload. Today the drain closes the window.
4. **Stage-A-owned helpers with no lock of their own.** `FastReadback` (D3) is serialised only by `cpu_writes_mutex`
   inside `DownloadMemory`. The staging pool and the `resident_ranges` / residency bitmap follow the owner-thread
   rule.

What does **not** need the drain [V]: `WriteTicks` (atomics, leaf creation under its own mutex), `scheduler.IsFree`
(atomic CAS in `Semaphore::Refresh`), `ForgetGpuWritten` / `runtime_memory_mark_gpu_written` (atomic bitmap), the
tracker (region locks; guest threads already mutate it in `InvalidateRegion`'s non-flush path and in
`ExtendWriteFault`), `ReadBacking` (runtime lock or lock-free A1 copy), and `Gow3Stats`.

## 3. Concrete hazards hit so far (what a change here must not repeat)

| # | Hazard | Where | Lesson for P8 |
|---|---|---|---|
| H1 | A command recorded beside stage B: host copies in a chunk nobody hands over, endless `WaitHostCopies` | L5, 2026-10-18 | Inline branches must record nothing: no `DownloadMemory`, no `ProcessCpuWritesLocked`, no `CopyBuffer` |
| C1 | Stage A submitted in the middle of stage B's draw: null index buffer, driver crash | L5 | Same |
| BB-twin | A twin snapshot read a read-protected page under `twins_mutex`; the nested fault waited for the same mutex | BB 0.4 | No ordinary guest-memory read, and no fault, while the new lock or `twins_mutex` is held (keep `ReadBacking`) |
| Q1 | A non-GPU-side thread that stage B waits on (the recorder) faulted. `SendCommand<true>` → drain → stage B → recorder: a deadlock by construction | quit-to-menu 11-08, 200% 11-15 | Any lock stage B holds while it waits on the recorder, copy threads or draw-prep workers must never be taken by those threads |
| L5-open | Stage B faults inline between its buffer binds and the draw (motion-vector hashing) | L5 card | Stage B's inline faults now also need the new lock: no re-entry while it holds it |
| D1-C | Class C assumption: the guest never reads back GPU data from memory it unmapped | D1 card | Unchanged by inlining |

## 4. Reference ports

- **Bloodborne port** (`bloodborne_pc` `3c57da4`, 0.51): same structure. `ReadMemory`'s lambda holds the twin
  decision and the download (`buffer_cache.cpp:703-785`); `SendCommand<true>` unless `assume_locks`;
  `ProcessCommands` drains per command (`liverpool.cpp:102-122`). Its newer trap handlers (`OnVramDataAccess`,
  `LayerReadTrapHit`) also go through the command after a drain ("as ReadMemory: the recording thread idle while the
  copy back runs"). No comment says why twins could not be inline: the code was written inside the existing lambda
  [V]. The only fault-locking lesson there is BB-twin above.
- **Upstream (bc160201983) main**: our fork's code (twins, fault-page-download, L5 all present); nothing newer.
- **Upstream shadPS4**: not in the tree (the GPU code is a copy, not a submodule). Its tracker comment shows
  guest-thread concurrency on region state was intended. Its command path predates any draw pipe; upstream has no
  drain.

## 5. Critique of the proposed design

Proposal: a new mutex over `gpu_modified_ranges` + `WriteTicks`. Stage B holds it from the written binding's tracker
GPU mark through `Add` / `Note`, and the download paths hold it too. The fault handler resolves unmap-discard, twin
and no-GPU-bytes inline under it, and real downloads keep the command. Lock order: new → region lock; `twins_mutex`
only after releasing the new mutex.

### 5.1 What is right

- It removes exactly the race in section 2 item 3, the one that matters.
- Real downloads stay commands, so H1/C1 cannot come back through the inline branches.
- Guest threads already mutate the tracker in `ExtendWriteFault` and in `InvalidateRegion`'s non-flush path. The
  texture cache side already does what P8 proposes: `ResolveReadbacks` decides on the guest thread under
  `readback_mutex` and sends a command only when there is something to wait for
  (`texture_cache.cpp:316-387`).
- Inline branches also leave the Q1 deadlock class: a twin or unmap discard no longer waits on stage A.

### 5.2 Problems

1. **"Finished-page" is listed as inlinable in the queue row and the deep dive; it is not.** It calls
   `DownloadMemory`. That runs `ProcessCpuWritesLocked` (staged `CopyBuffer` into the scheduler) and uses the
   lock-free `FastReadback`. It must keep the command. The task statement gets this right ("real downloads keep the
   stage A command"); the row should be corrected.
2. **The stated lock order cannot hold on stage B.** For `is_written`, `ForEachUploadRange` takes each region
   manager's lock in its first loop and keeps it across `func`, `on_upload` (`UploadCopies`, `twins_mutex`, the twin
   `Subtract`) and the second loop's GPU mark (`memory_tracker.h:101-133`). To keep "new → region", stage B must
   take the new mutex **before** `ForEachUploadRange` starts, so it covers the whole written `SynchronizeMemory` up
   to `Add` / `Note`. That span includes `staging_pool.Request` and `ResolveReadbacks(…, true)`, both of which can
   wait for the GPU. It is bounded and still cheaper than a drain, but it is not "around the mark".
3. **"`twins_mutex` only after releasing it" breaks the twin branch.** The snapshot loop reads
   `gpu_modified_ranges` *inside* `twins_mutex` (`buffer_cache.cpp:977`). Stage B already nests
   region → `twins_mutex` (in `on_upload`). The consistent order is
   **`cpu_writes_mutex` → new → region → `twins_mutex` → runtime backing lock**, and nothing takes the new mutex while
   holding `twins_mutex`. The twin branch should keep decision, snapshot and `MarkRegionAsCpuModified` under the new
   mutex, with `twins_mutex` nested inside. Releasing it between decision and snapshot lets a download or a new GPU
   mark slip in. That leaves twins over bytes that are no longer GPU-modified: mostly harmless, but a new state to
   reason about.
4. **Missing mutation sites.** Every access to `gpu_modified_ranges` must be under the new mutex
   (`buffer_cache.cpp`, current `perf-hygiene`):
   - `ReadMemory` lambda: 953, 967, 977, 1014 (inline and command paths alike, since the command now runs beside
     inline faults);
   - `DownloadMemory`: 1107-1108, inside `ForEachDownloadRange`'s region lock, so take the new mutex at the top of
     `DownloadMemory`, after `cpu_writes_mutex`;
   - `ObtainBuffer`: 1364 (Add) and 1366 (Note);
   - `ProcessCpuWritesLocked`: 1677 (Note), 1689 (Subtract), 1693 (ForEachInRange) plus the per-page unmark. This runs
     on stage B per draw (`BeforeRecord`) and is **not in the proposal**;
   - `SynchronizeMemory` twin path: 1903 (Subtract of changed runs). This applies to **read-only** bindings too (no
     region lock held then) and to `SynchronizeDmaBuffers`. Also **not in the proposal**.

   `WriteTicks` needs no lock of its own (atomics). It belongs inside the critical section only for the
   mark-to-Note window.
5. **Re-entry.** Stage B and stage A resolve their own faults inline (`assume_locks = true`) and today take no lock.
   Under P8 they must take the new mutex, because guest threads now decide concurrently. A fault on a thread that
   already holds it (stage B inside `SynchronizeMemory`, stage A inside a download) deadlocks on a `std::mutex`. You
   need either a thread-local "held" flag (resolve without re-locking) or a proof that no guest-memory access can
   fault inside the held span. The region locks have the same constraint today; `UploadCopies` resolves image
   readbacks first for that reason (`buffer_cache.cpp:1955-1958`).
6. **Who may take it.** Stage B holds the new mutex while it may wait for the recorder (`Wait` → `SyncRecording`), the
   copy threads (`WaitHostCopies`), or a draw-prep / binding-helper worker joined inside a draw. `ObtainBuffer` is
   also reached from draw-preparation workers (`SmallGuestCopy` comment, `ad7169e`), so the lock must live inside
   `ObtainBuffer` / `SynchronizeMemory`, not at stage B's call sites. If any of those threads faults into an inline
   branch, the cycle is Q1 again with a mutex in place of the drain. Today on Windows they do not fault
   (deferred-copy-backing; prep workers' guest reads recover by longjmp, though `gow3gpu_handle_fault` runs first in
   `vectored_fault`, `probe.c:270-286`). Write this down as an invariant on the card. A defensive `try_lock` that
   falls back to the command path on those threads gains nothing, because the command path deadlocks too.
7. **Ordering semantics change (class C, small).** Today a fault decision sees every *decoded* draw recorded, because
   the drain runs first. Inline sees only what stage B has recorded so far. For a page that a decoded but unrecorded
   draw writes, the guest's write now reaches the arena before that draw's GPU write instead of after it. That is
   arguably closer to the PS4, where the GPU runs behind the CPU, but it is a behaviour change. Likewise an inline
   unmap discard no longer forces pending draws to record their uploads from the range before the decommit. Unmaps
   of ranges with no GPU bit already work that way today (no flush, no drain), and deferred copies read through
   decommit-keep-access (LANDMINES 24) or the backing view, so the risk is low [I].
8. **The lock is global.** It serialises every inline fault with every written binding anywhere. The region locks it
   replaces in effect are per 4 MiB manager. This is fine at ~100 faults/s. Sharding needs alternative B.
9. **Measure what it saves before trusting the fit.** The guest-side cost is in `t_write_faults`; the stage-A-side
   drain is in `drain_sites` (`ReasonCommands`). The fitted 0.45-0.6 ms per fault should be checked against those two
   counters in one frame-stats run before building.

### 5.3 Alternatives

- **A. A "no-drain" command** (stage A runs the decision between packets without draining): it still needs the same
  mutex, because stage B keeps running. It adds the wait for stage A to reach a packet boundary, and stage A itself
  often sits in drains (compute packets, label waits, OnSubmit with E1 off). It is worse than inline. Not
  recommended.
- **B. `gpu_modified_ranges` per region manager, under `manager->lock`.** `Add` would move into
  `ForEachUploadRange`'s second loop (atomic with the GPU mark by construction), and the fault decision would run
  inside `InvalidateRegion`'s locked check instead of in `on_flush`. There is no new lock, the existing order
  (region → twins) holds, and it is sharded. It is a larger refactor (`DownloadMemory`, D2, twin `Subtract`, D1 all
  per manager), but it is the clean end state if a global mutex shows contention.
- **C. Partial inlining.** The mutex is the whole cost and risk of the change; once it exists, twins are the payoff
  (~100/s). No-GPU-bytes is 0/s measured, so it comes free with the same code. Unmap discard (~17/s) is the simplest
  branch (no twins snapshot, just `EraseTwins` + clears) and is a good first step to prove the lock sites. Order:
  unmap discard + lock sites → twins.
- **D. Fewer twin faults instead of cheaper ones.** The perf-diag write-fault report names the guest RIP and page.
  If ~100/s is a few pages a frame (as in N-12: a 20-byte GPU block on a page of bone matrices), a targeted
  keep-writable page with a standing twin is possible. Hot-pages failed on volume (14k pages), not on the idea. Look
  at the sites before choosing.

## 6. Recommended shape if built

- Switch `GOW3_FAULT_INLINE` (live toggle bit if cheap), default off for the first run, per the "old path behind a
  switch" rule. Counters: inline twin / unmap / no-GPU-bytes, commands sent, lock wait time on guest threads and on
  stage B.
- Lock order `cpu_writes_mutex` → new → region → `twins_mutex` → backing. The new mutex is taken at the top of
  `DownloadMemory`, `ProcessCpuWritesLocked`, the written path of `SynchronizeMemory` (before `ForEachUploadRange`,
  through `Add` / `Note` in `ObtainBuffer`), the read-only twin `Subtract`, and the whole `ReadMemory` decision
  (inline and command).
- A thread-local re-entry guard for stage A / B inline faults.
- Card invariants: inline branches record nothing; no guest-memory read and no fault under the lock; threads stage B
  waits on never take it.
- Tests: frame-stats run first to confirm the fitted cost (section 5.2 item 9); heavy combat lows A/B (inline off /
  on, one change); t-map3 map (twins at ~35/s, no oscillation); T-T1 light view; quit to main menu and a 200% load (Q1
  class); `stale_readback=0` / Precise readbacks (twins off path); a Deck run (Linux `SIGSEGV` handler context, 4
  cores).

## 7. Open points found on the way

- Work-queue row "Hang at quit to main menu" (11-08) has the same `RunGuestCopy` → `memcpy` stack that `3a84125`
  fixed on Windows on 11-15. It is probably covered; check before proposing another fix [I].
- `Rasterizer::ReadMemory` sends a buffer-cache command for every read fault without a pending image readback, even
  when the range is not GPU-modified (`vk_rasterizer.cpp:4355-4368`). Both hang traces were such faults ("GPU-modified:
  no, image readback: no"). An inline "nothing to do" return would not fix them: nothing lifts that protection, so the
  access would fault again. What protects those pages is still unknown.
- Unmap discards were 0/s in T-D1b (10-19) and on the Deck, and are now up to ~17/s. Which unmaps hit GPU-written
  ranges now is worth one perf-diag look before inlining D1.

## Sources

Code (`perf-hygiene` `52d33b3`): `gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp` 935-1059, 1061-1160,
1249-1370, 1616-1700, 1872-1945; `buffer_cache/memory_tracker.h` 58-133; `amdgpu/liverpool.cpp` 119-140,
`liverpool.h` 141-160; `renderer_vulkan/vk_rasterizer.cpp` 517-545, 4294-4420; `page_manager.cpp` 144-163;
`texture_cache/texture_cache.cpp` 316-387; `src/probe.c` 266-286. Cards: stage-a/l5, caches/{d1,d2,d3,
gpu-write-twins,fault-page-download,hot-pages,uffd}, stage-b/deferred-copy-backing, tooling/copy-fault-trace,
stage-a/e1-deck-drains. Docs: plan-baseline-issues.md (H1, C1, root cause), impl-decode-pacing.md (ownership),
plan-work-queue.md rows P8 / 200% / quit hang, `data/u1-hang-recorder-fault-20261108.txt`,
`data/hang-200pct-stacks-20261115.txt`. Bloodborne: `bloodborne_pc` `buffer_cache.cpp` 645-800, `liverpool.cpp`
102-122, `vk_rasterizer.cpp` 851-863 / 4244-4261, `docs/parallel_gpu.md` 350-370 / 555-580,
`docs/CHANGES_after_0.4.md` 266-274. Git searches (all branches): `-S InvalidateRegion`, `-S assume_locks`,
`-S "SendCommand<true>"`, `-S twins_mutex`, `-S IsGpuSideThread`, `-G assume_locks`, message greps for
fault / twin / drain / deadlock / hang / revert.
