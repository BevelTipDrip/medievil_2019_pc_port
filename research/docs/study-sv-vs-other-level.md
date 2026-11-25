# Study: Sleeping Village game-thread cost against another level (2026-11-25)

Static analysis of existing captures only: nothing was launched, built, edited in the source or committed. Status
words: everything here is **found** (from ETW samples and context switches). Nothing is fixed, run or verified.
Parent: [study-sv-gpu-gaps.md](study-sv-gpu-gaps.md) (run 6). Related: [study-sv-hotspots.md](study-sv-hotspots.md),
[study-physics-ai-tick.md](study-physics-ai-tick.md), [study-npc-tick-rate.md](study-npc-tick-rate.md).
Scripts and outputs: `research-data/sv-hunt-20261125/analysis-crosslevel/` (list at the end).

**Question.** Is Sleeping Village's (SV) game-thread cost specific to that level? The user's hypothesis: SV's heavy
AI pathing / collision checks starve the GPU. In SV, swinging Dan's weapon while walking into a wall drops GPU usage
sharply and frames spike; in other levels the same action raises GPU usage (or power at 99% usage).

## Summary

1. **SV's game thread is heavier than the other level's in calm and in fights, by about the same margin.**
   - Calm: SV 2.98 ms per frame (run 6 CAP frames: 3.12) against 2.07-2.39 in the other level: **+0.6 to +0.9 ms**.
   - Fight: SV 4.38-4.62 against 3.33-3.59 (with the unpatched nav purge) or 2.77-3.35 without it: **+0.8 to +1.3 ms**.
   - Calm to fight, the game thread grows by a similar amount in both levels: SV +1.4 to +1.6 ms, the other level
     +0.7 to +1.3 ms (without the purge). SV starts its fights from a higher base.
2. **Both levels' fights run at the same frame rate (205-218 FPS), but something different limits them.**
   - SV fight: the game thread runs 95-96% of the frame. It waits only on its own task-graph workers
     (0.09-0.14 ms per frame). It sets the frame period, and the GPU starves (run 6: ~0.75 ms net GPU idle per frame).
   - Other-level fight: the game thread runs 74% of the frame. It still waits ~0.6 ms per frame on runtime threads
     and outside wakers (the flip / GPU side). So the period is set downstream, most likely by the GPU.
   - That fits the user's observation: GPU usage falls in SV fights and rises in other levels' fights. **[inferred]**
     GPU busy time was not measured in any of these captures.
3. **Collision (PhysX scene queries on the game thread) is the largest SV-only cost.**
   - Totals: **+0.20-0.23 ms calm, +0.33-0.44 ms fight.**
   - The PhysX GeomUtils top-level sweep / overlap tests cost **3-6x** more in SV: 0.105 vs 0.017-0.026 calm (4-6x),
     0.154-0.159 vs 0.030-0.054 fight (3-5x).
   - The scene read lock (host mutex HLE, same code in both builds) costs **~3x** more in SV: 0.039-0.046 vs
     0.011-0.017.
   - Both point to more sweep / overlap queries per frame in SV, not slower ones. **[inferred: samples give time,
     not counts]**
4. **AI / navigation / pathing code itself is not measurably larger in SV.**
   - AI / navigation / movement: SV 0.14-0.15 against 0.09-0.13 ms per frame (+0.01 to +0.05).
   - The unpatched nav-purge loop in the other level grows as paths accumulate between garbage collections:
     2.80 ms/s per second. SV run 2 (also unpatched, trolls): 2.71 ± 1.39.
   - Purge cost grows with the number of registered paths, so the path-search rate looks similar in both levels.
     **[inferred, low confidence]**: a 7-second SV fit, and it assumes every path search registers one path.
5. **Other SV-only costs in fights:**
   - task-graph queues +0.17 to +0.43 (a lower bound: the other level's build lacks the queue-read fence patch);
   - Cascade particles +0.18 to +0.27;
   - engine other +0.12 to +0.20;
   - Blueprint VM +0.04 to +0.14;
   - tick dispatch +0.03 to +0.07;
   - Core / CoreUObject +0.02 to +0.11.
6. **Answer to the hypothesis:**
   - "Collision checks starve the GPU": **supported**. The game thread sets SV's fight period, and collision
     queries are its largest SV-specific cost.
   - "AI pathing": **not supported** by these data.
   - The rest of SV's surplus is spread over task dispatch, particles, engine and Blueprint code: about as much
     again as the collision surplus.
   - Whether the **wall** swing specifically adds collision queries cannot be told: no capture marks a wall swing
     (section 6).

## 1. Captures and windows

**The other level.** The 11-23 runs loaded save `@MediEvil0006`, labelled "The Graveyard", and rewrote it soon
after loading with that label (`Save trace: set param 2 ... 'The Graveyard'`, comb run 19:46:49, HDR run 20:24:43).
**Which level the user was in is pending the user's confirmation.** The label alone does not prove it:
study-perf-deepdive-20261123.md places the 21:43 run of the same day, which loaded the same save, "near the start of
Cemetery Hill". Either way it is not SV.

| Capture | Run (log) | Build / settings | Flip trace | Windows used |
|---|---|---|---|---|
| `comb-2.etl` (19:49:34.5-19:51:48.3, whole span in the ring) | `comb-run-c254378/last_run-c254378.log` | c254378 release, 01.02, 4K DLSS 75%, Reflex 237, SDR (`GOW3_HDR off`) | **none**: `flip-trace-c254378.csv` in that folder is from 2026-11-22 08:39 (wall row, file date), another session | calm 19:51:25-40 (3,453 frames); fightA 19:50:38-50 (2,457); fightB 19:51:04-16 (2,620) |
| `comb-1.etl` (19:48:14.5-19:49:11.3) | same run | same | none | steady 19:48:32-47 (3,214 frames) |
| `cur-c254378-3.etl` (20:33:39.3-20:36:15.6; exported 20:34:39-20:36:16) | `cur-run-c254378-hdr/` | c254378, HDR on | yes, no `gpu_ready` rows | calm 20:35:45-36:00 (3,321 frames); busy 20:34:40-52 (2,519; GPU-side slow frames, game thread unchanged) |

Not used:
- `cur-c254378-2.etl`: 29 s, all calm, the same run as cur-3.
- `ref-e32c55f-1.etl` (20:14:34-20:15:11): no run log covers it. The ref run ended 20:05:51; the HDR run started
  20:22:14.
- The 11-15 captures: older build, level not recorded.

**SV (run 6, unchanged data).**
- `np-c4`: calm 12:22:34-38 (948 frames), fight 12:22:40-46 (1,238).
- `np-c5`: fight 12:23:56-12:24:08 (2,619).
- Build 655ad2a: the three fence patches on (nav purge, RHI pending delete, task-queue reads).

**Build differences.**
- c254378 has none of the three fence patches (they came in 09e5878, c2b0d17, 56ecb95, after it).
  - So the nav purge and the task-graph queue categories are not on equal footing; they are reported separately.
  - The rest (PhysX, AI, Blueprint, particles, engine, core) is the same guest code: the same 01.02 eboot, run
    natively in both builds.
- The host mutex code did not change between c254378 and 655ad2a (`git log c254378..655ad2a`: no runtime mutex
  or thread commit).
- c254378's runtime exe (138,678,272 bytes) is no longer on disk, so host functions cannot be named there. Only
  guest-side categories are used. Thread roles come from guest profiles; host threads (stage A / B, recorder) stay
  unnamed in the 11-23 captures.

## 2. Method

- **Exports.** xperf dumper exports (no `-symbols`) were parsed with run 6's `parse6.py` unchanged (PIDs 23560 and
  6988). The CSVs (17 GB) were deleted after parsing.
- **Roles.** `rolesx.py` is `roles6.py` without the host part: the same cosine match to run 2's named guest threads.
  - main: 0.87 / 0.52 / 0.50 (next best 0.28-0.32).
  - RenderThread 0.87-0.91; AudioThread 0.99.
- **Categories.** The hotspot study's `classify.category`, unchanged. `gamecatx.py` is `gamecat6.py` as a window
  average: samples (1 kHz, 1 sample ≈ 1 ms) divided by frames.
  - Re-run on run 6 it gives c4 calm 2.98 / fight 4.62 / c5 4.38 ms per frame.
  - `gamecat-c4/c5.txt` gave, by frame class: CAP 3.12, LATE 4.69 / 4.68.
- **Frames without a flip trace.** Counted by the per-frame marker "main readies AudioThread". Against the flip
  trace it is within 0.4%:
  - cur3: 3,324 vs 3,321 and 2,526 vs 2,519;
  - SV c4: 951 vs 948 and 1,242 vs 1,238;
  - SV c5: 2,628 vs 2,619.
- **Frame classes.** No CAP / GPU / LATE classes in the other level: no `gpu_ready`, and no flip trace for comb. The
  windows are averages. LATE frames (guest flip after the previous present) are 0% in cur3: there the guest flip
  comes 3.8-4.7 ms before the previous present, so the CPU chain is ahead and the frames are GPU / present bound.
- **Fights are inferred, not seen.** From 19:49:54 to 19:51:23, comb-2's game thread runs 2.5-4 ms per frame
  (`persec-comb2.txt`), and fightA shows SV's fight signature at a smaller scale (ms per frame, calm to fightA):

  | Thread: area | other level, calm to fightA | SV, calm to fight |
  |---|---|---|
  | render thread: particles | 0.03 to 0.39 | 0.16 to 0.65 |
  | task-graph workers: PhysX | 0.14 to 0.80 | 0.38 to 1.52 |
  | task-graph workers: particles | 0.06 to 0.40 | 0.27 to 1.08 |
  | game thread: particles | 0.03 to 0.11 | 0.10 to 0.35 |

  What Dan did (wall or no wall) is not known.

## 3. Game thread, ms per frame (`compare.txt`, `gamecatx-*-main.txt`)

| Category | other calm: cur3 / comb2 / comb1 | other fight: comb2 A / B | SV calm (c4) | SV fight: c4 / c5 | SV minus other, calm | SV minus other, fight |
|---|---|---|---|---|---|---|
| **frames per second** | 221 / 230 / 214 | 205 / 218 | 237 | 206 / 218 | | |
| **total** | **2.10 / 2.07 / 2.39** | **3.59 / 3.33** | **2.98** | **4.62 / 4.38** | +0.6 to +0.9 | +0.8 to +1.3 |
| running, % of the frame | 47% / 48% / 52% | 74% / 74% | 72% | 95% / 96% | | |
| **PhysX (scene queries, scene lock)** | 0.38 / 0.35 / 0.36 | 0.51 / 0.41 | 0.58 | 0.85 / 0.84 | **+0.20 to +0.23** | **+0.33 to +0.44** |
| Blueprint VM | 0.53 / 0.55 / 0.55 | 0.72 / 0.64 | 0.64 | 0.76 / 0.78 | +0.09 to +0.11 | +0.04 to +0.14 |
| Engine other | 0.24 / 0.23 / 0.28 | 0.36 / 0.34 | 0.33 | 0.54 / 0.48 | +0.05 to +0.10 | +0.12 to +0.20 |
| Particles (Cascade) | 0.02 / 0.03 / 0.04 | 0.11 / 0.08 | 0.10 | 0.35 / 0.29 | +0.06 to +0.08 | +0.18 to +0.27 |
| Core / CoreUObject other (UObject create / destroy, hash, host mutex calls) | 0.17 / 0.17 / 0.18 | 0.26 / 0.21 | 0.20 | 0.32 / 0.28 | +0.02 to +0.03 | +0.02 to +0.11 |
| **AI / navigation / movement** | **0.10 / 0.09 / 0.10** | **0.13 / 0.10** | **0.15** | **0.14 / 0.14** | **+0.05 to +0.06** | **+0.01 to +0.04** |
| Tick dispatch (TickTaskManager) | 0.05 / 0.05 / 0.05 | 0.08 / 0.05 | 0.08 | 0.11 / 0.12 | +0.03 | +0.03 to +0.07 |
| Animation / skeletal mesh | 0.05 / 0.05 / 0.06 | 0.10 / 0.07 | 0.05 | 0.09 / 0.06 | 0 | 0 |
| Audio (AkAudio) | 0.03 / 0.03 / 0.06 | 0.06 / 0.04 | 0.07 | 0.08 / 0.09 | +0.01 to +0.04 | +0.02 to +0.05 |
| UI (Slate / UMG) | 0.07 / 0.07 / 0.08 | 0.10 / 0.10 | 0.07 | 0.10 / 0.10 | 0 | 0 |
| host only (no guest frame) | 0.03 / 0.02 / 0.03 | 0.05 / 0.08 | 0.07 | 0.06 / 0.07 | +0.04 to +0.05 | about 0 |
| *host code called from guest code (all categories: mutex, HLE)* | 0.22 / 0.23 / 0.27 | 0.39 / 0.31 | 0.31 | 0.40 / 0.39 | +0.04 to +0.09 | 0 to +0.09 |
| **Not on equal footing (fence patches off in c254378):** | | | | | | |
| task-graph queues / events | 0.34 / 0.35 / 0.49 | 0.72 / 0.54 | 0.48 | 0.97 / 0.89 | 0 to +0.14 | +0.17 to +0.43 (SV patched, so a lower bound) |
| nav path purge (0x2c94b30, loop 0x2c94b90) | 0.00 / 0.00 / 0.00 | 0.24 / 0.56 | 0.05 | 0.07 / 0.07 | | (unpatched SV, run 2: 0.72) |
| **total without those two** | 1.76 / 1.72 / 1.89 | 2.62 / 2.23 | 2.45 | 3.58 / 3.42 | **+0.56 to +0.73** | **+0.80 to +1.35** |

- The other level's calm windows are within 0.3 ms of each other across two runs (SDR and HDR), so the level's
  calm baseline is stable. comb2 calm comes right after a garbage collection: its purge is 0.00.
- **The task-graph FIFO** (`Pop` 0x6e7bf0, the 0x6e7c22 loop): 0.10-0.23 ms per frame in the other level, 0.04-0.10
  in SV. That is the fence patch's effect, and it is why the queue category is not compared directly.

### 3.1 PhysX on the game thread by sub-area (ms per frame)

| Sub-area (eboot range) | other calm (3 windows) | other fight A / B | SV calm | SV fight c4 / c5 |
|---|---|---|---|---|
| GeomUtils top-level sweep / overlap tests (0x2fe0000-0x3000000: 0x2ff14d0, 0x2ff9c80, 0x2ff0c00) | 0.017-0.026 | 0.054 / 0.030 | **0.105** | **0.159 / 0.154** |
| Sq AABB-tree ray / sweep traversal (0x3128000-0x3130000: 0x312ad40 ...) | 0.098-0.111 | 0.151 / 0.122 | 0.148 | 0.210 / 0.228 |
| GuBV4 triangle-mesh queries (0x3060000-0x3090000) | 0.051-0.058 | 0.044 / 0.044 | 0.075 | 0.099 / 0.083 |
| scene read / write lock (0x319c9c0 / 0x319ca00; almost all of it host mutex HLE) | 0.011-0.017 | 0.016 / 0.016 | **0.039** | **0.046 / 0.045** |
| other PhysX | 0.157-0.180 | 0.246 / 0.200 | 0.213 | 0.334 / 0.331 |

- The tree traversal (finding candidate shapes) is +40-80% in SV; the exact shape tests (sweep / overlap against
  convex / primitive shapes) are 3-6x.
- So SV issues more sweeps / overlaps against more nearby convex shapes per frame. Candidates **[inferred]**:
  - more CharacterMovement sweeps / floor checks (more moving NPCs);
  - more Blueprint overlap tests (weapon, damage volumes, `CheckFloor` / `UpdateWaterCollision` traces of the NPC
    controllers, study-physics-ai-tick.md 2.2);
  - SV's geometry (houses, fences, props as convex hulls).
- The lock costs the same per call in both builds, so ~3x the lock time is ~3x the locked reads. 0x319c9c0 has 153
  call sites, PhysX internals included, so this counts locked reads, not queries exactly.

### 3.2 Pathing: the nav purge as a path counter

- In c254378 (patch off) and in SV run 2 (patch off), the purge loop walks the nav data's `ActivePaths` every frame.
  `ActivePaths` grows with every `FindPathToLocationSynchronously` (0x2ca48c0) until the next garbage collection,
  every 61.1 s (study-npc-tick-rate.md).
- comb2 shows the sawtooth: the purge drops to 0 at ~19:50:23 and 19:51:24, and in between grows linearly,
  **2.80 ms/s per second** at ~210 FPS, to 0.62 ms per frame.
- SV run 2 (cap1, trolls, unpatched): the purge level is ~154 ms/s (0.71 ms per frame), growing **2.71 ± 1.39 ms/s
  per second** over 5-11 s.
- The purge loop is the same eboot code on the same CPU, so the growth rate is proportional to new paths per second.
  **No sign that SV issues more path searches per second than the other level's fights.**
  - **[inferred, low confidence]**: the SV fit covers 7 s, and the GC phase is unknown there.
  - "Every path search registers one path" is UE 4.21 behaviour from memory.
- What makes the purge expensive is the frame rate and the GC period, not the level. The patch (on since 09e5878)
  removes it in both.

## 4. Other threads (ms per frame; `gamecatx-*-RenderThread.txt`, `-TaskGraph.txt`, `waits-main.txt`)

| | other calm (cur3 / comb2 / comb1) | other fight A / B | SV calm | SV fight c4 / c5 |
|---|---|---|---|---|
| render thread, sampled | 1.13 / 1.13 / 1.62 | 2.25 / 1.62 | 1.59 | 4.34 / 3.67 |
| task-graph workers, sampled (sum) | 1.24 / 1.26 / 2.68 | 3.75 / 2.52 | 2.32 | 7.67 / 6.62 |
| of it PhysX (the simulation step and its tasks) | 0.19 / 0.14 / 0.36 | 0.80 / 0.47 | 0.38 | 1.52 / 1.29 |
| game thread waiting on runtime threads / other processes | 1.68 / 1.42 / 1.33 | 0.64 / 0.54 | 0.48 | 0.02 / 0.02 |
| game thread waiting on its task-graph workers | 0.55-0.60 | 0.47 / 0.49 | 0.51 | 0.14 / 0.09 |

- In SV's fight the render thread (85-90% busy, run 6) and the workers grow 2-3x more than in the other level's
  fight. Stage B follows the draw count (run 6: 92% busy). That is the render-side half of SV's fight load.
- Stage B cannot be named in the 11-23 captures (no runtime symbols), so it is not compared.

## 5. The user's hypothesis, tested

| Reading | Verdict | Evidence |
|---|---|---|
| SV's game-thread cost is specific to the level | **found true**, calm and fight | +0.6-0.9 ms per frame calm, +0.8-1.3 fight against the other level at the same settings; the other level's calm is stable across two runs |
| SV's heavy AI pathing starves the GPU | **not supported** | AI / navigation / movement +0.01-0.05 ms; path-registration growth rate similar (2.71 ± 1.39 vs 2.80 ms/s per second) |
| SV's heavy collision checks starve the GPU | **supported** (largest single SV-only cost) | PhysX queries +0.20-0.23 calm, +0.33-0.44 fight; sweep / overlap tests 3-6x, locked reads ~3x |
| In SV fights the GPU starves; elsewhere it stays busy | **consistent, GPU not measured** | SV fight: game thread 95-96% and sets the period (run 6: GPU ~85% busy). Other fight: game thread 74%, still waits ~0.6 ms per frame downstream; same 205-218 FPS, so another stage (most likely the GPU) is the limit [inferred] |
| The wall swing is what adds the SV collision cost | **open** | no capture marks a wall swing; SV's surplus is already there in calm (+0.2 ms PhysX) |

## 6. What these captures cannot answer

- **What Dan was doing.** Neither SV run 6 nor the 11-23 captures mark "swing into a wall" vs "swing in the air";
  the other level's fights are inferred from the thread signature.
- **GPU busy time.** Every capture used the `Jitter` profile (no DxgKrnl GPU queue packets). The 11-23 flip traces
  have no `gpu_ready`, and the comb run has no flip trace at all. The GPU side of the other level is inferred.
- **Counts.** Path searches, PhysX queries and NPC controller ticks per frame: 1 kHz sampling gives time per function,
  not calls. The lock time and the purge slope are proxies.
- **Inclusive time.** Guest stacks are one frame deep: 68,357 of 70,549 comb-2 game-thread samples have exactly one
  guest frame, the rest none. So there is no inclusive time under the AI controllers' tick, under weapon-hit /
  overlap handling, or under `FindPathToLocationSynchronously` (0x2ca48c0 has no exclusive samples either).
- **Equal builds.** The nav purge and task-graph queue categories differ by build (fence patches).
- **NPC count and geometry.** Not recorded. SV's calm window is at the wall-fight spot, possibly with idle NPCs near.
- **The level identity** (pending the user).

## 7. A targeted capture that would answer it (proposal; each launch needs the user's go)

- One build for both levels: the current release with the fence patches. Settings as run 6. Flip trace with
  `GOW3_FLIP_TRACE_GPU_READY=1`, and `wpr-session.ps1 -Gpu` (the `JitterGpu` profile, GPU queue packets for GPU busy
  and idle per frame).
- Per level (SV at the run-6 wall spot; the Graveyard or a level the user names), ~15 s each, with the time of each
  segment written down (F8 after each):
  1. standing still;
  2. swinging in the air, no wall, no NPC near;
  3. walking into a wall while swinging, no NPC near;
  4. the same wall swing with NPCs around (SV only, if it can be arranged).

  Segments 2 vs 3 isolate Dan's own wall collision; 3 vs 4 isolates the NPCs.
- For counts and inclusive time (a dev build, `GOW3_DEV_TOOLS`): per-frame counters of
  - `FindPathToLocationSynchronously` (0x2ca48c0) and `MoveToLocation` (0x2d5d3f0) calls;
  - PhysX locked reads (0x319c9c0) by return site, i.e. which query kinds;
  - AI controller ticks.

  Or the guest frame-pointer walk of study-sv-hotspots.md 8.4 for inclusive costs per tick group. This follows the
  "trace data, not toggles" rule. Each tool would need a proposal and the user's go before it is built.

## Where this left off (2026-11-25, after the v2.5.0 release)

Paused here for the v2.5.0 "game does not load" reports. Pick up from this list.

**The user's readings (2026-11-25):**
- Motion blur is not worth testing: more headroom does not fix the unevenness, and the SV instability looks unlike
  other levels already at 100% GPU (rapid spikes, not a lower frame rate).
- In SV, swinging at a wall while walking into it drops GPU usage sharply; standing back (similar particles) does
  not. Elsewhere the same action raises GPU usage, or power (+50 W seen in the Graveyard at 99% usage).
- Reflex markers and letting stage A run ahead are still worth investigating.

**Found since (2026-11-25):**
- This study: SV's game thread is +0.6-0.9 ms (calm) / +0.8-1.3 ms (fight) per frame heavier than the other level;
  the largest SV-only part is collision (PhysX sweep / overlap tests 3-6x); AI / pathing code is not larger.
  In SV fights the game thread runs 95-96% of the frame (it sets the frame time, the GPU starves); in the other
  level 74%, waiting on the GPU side.
- **Reflex gets no per-frame GPU attribution** (code): our `vkQueueSubmit`s carry no
  `VkLatencySubmissionPresentIdNV`, and the markers sit at one instant before the present
  (`vk_swapchain.cpp` Present: SimulationEnd, RenderSubmitStart / End, PresentStart; SimulationStart after
  `LatencySleep`). Earlier: slow presenter-paced frames had all their extra time in Reflex's sleep
  (+0.8-1.0 ms, study-sv-critical-path.md).
- **Occlusion does not wait** (hzb-remap card, run logs): the newest finished results are served each frame,
  older ones when the GPU is behind, uncovered keys read visible; the remaining queries are read 3 frames late
  without a spin. 0 resyncs in the SV runs. Ruled out as a wait.
- Stage A run-ahead (early display release): run 6 measured a 0.00 ms label wait in the fight frames
  (study-sv-gpu-gaps.md section 4), so it has nothing to remove there; cheap to build behind a switch if wanted.

**Proposed, waiting for the user:**
1. **Reflex R1** (port only, `GOW3_REFLEX_MARKERS`, default off, A/B): a frame ID given at stage A's frame start
   and carried to the present as its present ID; every queue submission of the frame tagged with it;
   RenderSubmitStart at stage A's first packet, RenderSubmitEnd at `queued`. **R2**: SimulationStart / End at the
   game thread's frame start / end (needs the eboot's frame tick found and hooked), maybe the sleep moved there.
2. **The targeted capture** above (SV wall spot and a second level: still, swing in the air, wall swing, wall swing
   with NPCs; flip trace with gpu_ready, `wpr-session.ps1 -Gpu`).
3. Then, by what it shows: a dev-build counter of PhysX queries per frame by calling game code (Dan's movement,
   weapon trace, NPC movement, overlaps); if NPC movement dominates, NPC decisions at 30-60 Hz
   (study-physics-ai-tick.md).

**Questions open with the user:** go for R1; which level the 11-23 comb captures are; go for the capture run and
which second level.

## Files (`research-data/sv-hunt-20261125/analysis-crosslevel/`)

| File | What |
|---|---|
| `rx.py` | loader for the 11-23 windows on run 5's `r5.py` (PIDs, ETL starts; no runtime symbols) |
| `rolesx.py` / `roles-comb1/comb2/cur3.*` | guest thread roles (roles6.py's cosine match, no host part) |
| `flipx.py` / `frames-hdr.npz`, `flip-hdr-2032-2036.txt` | HDR run flip trace to numpy; per second around cur-3 (LATE share, guest flip lead) |
| `persec.py` / `persec-comb1/comb2/cur3.txt` | per second: game thread running ms, categories, per-frame marker candidates (found main -> AudioThread) |
| `gamecatx.py` / `gamecatx-<cap>-{main,RenderThread,TaskGraph}.txt`, `fn-*-main.tsv` | window averages by category (gamecat6.py logic), host-call share, watched functions; per-function dumps |
| `waitsx.py` / `waits-main.txt` | game thread waits per frame by waker role |
| `compare.py` / `compare.txt` | the side-by-side tables, PhysX sub-areas, nav-purge growth slopes |
| `np-comb1/`, `np-comb2/`, `np-cur3/`, `parse-*.log` | parsed windows (`parse6.py`, unchanged); CSV exports deleted |
