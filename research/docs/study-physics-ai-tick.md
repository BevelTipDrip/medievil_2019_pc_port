# Study: physics, AI and collision ticks at 237 FPS (2026-11-25)

Static and web research only: no launch, no build, no source edit, no commit. Game build analysed: 01.02
(`E:\MediEvil\CUSA11227-0102`). Eboot addresses are image offsets (vaddr 0) of `out/eboot.elf` /
`research-data/fname-research-20261122/image_0102.bin`; the patch XML adds 0x400000. Pak config is the 01.02 extract
in `research-data/debug-menu-research-20261124/config0102/`. Scripts and outputs:
`research-data/physics-ai-research-20261125/` (on D:, list at the end). Builds on
[study-npc-tick-rate.md](study-npc-tick-rate.md), [study-sv-hotspots.md](study-sv-hotspots.md) and
[study-sv-critical-path.md](study-sv-critical-path.md); it does not repeat their tables.

Status words: everything here is **found**. Nothing is fix built, run or verified. Confidence tags:
[eboot] = read from the 01.02 code or strings, [pak] = the game's cooked config / assets, [PhysX src] = checked in the
public PhysX 3.4 source, [UE 4.21] = engine behaviour from memory of the 4.21 source (not re-read here; medium-high
unless marked), [web] = linked page, [inferred] = my reasoning.

## Summary

1. **The physics simulation is not the problem; the scene queries the game thread makes are.** PhysX 3.4
   (`PhysX_3.4`, `APEX_1.4` source paths in the eboot) steps once per frame on the UE4 task-graph workers (0.6-0.9 ms
   per frame summed over workers in the waves). On the game thread the PhysX cost is **queries** (sweeps, overlaps,
   raycasts: ~1 ms per frame in waves), issued synchronously by CharacterMovement, Blueprint traces and overlap
   updates. Of that, our host mutex is only 0.03-0.10 ms; the rest is the traversal itself.
2. **UE 4.21 has no way to run physics or AI at a lower fixed rate with interpolation.** Substepping only splits
   large steps (at 4.2 ms it is one step), the async scene is deprecated and unused, `MaxPhysicsDeltaTime` clamps
   upwards only, network smoothing exists only for networked simulated proxies, and the game uses none of the
   engine's AI throttles (no Behavior Tree, no AI perception, no URO on NPC meshes). The game's own LOD
   (`BP_AIActiveList`) ticks every NPC within 2500 uu every frame.
3. **The right cut is "decide at 30-60 Hz, move every frame".** NPC controllers make the expensive decisions (two A*
   searches, a new `UNavigationPath` UObject, traces) every tick; their pawns' CharacterMovement and path following
   are separate component ticks. Throttling only the decisions needs **no interpolation** (movement stays per frame)
   and matches the PS4's 30 Hz decision rate. Two ways: a native hook that caches / throttles
   `FindPathToLocationSynchronously` and `MoveToLocation` per NPC (exec 0x2d3b4e0 / 0x2e46630 [eboot]), or
   controller tick intervals plus fixing the Blueprint timers that read `GetWorldDeltaSeconds`.
4. **Two no-code physics knobs exist that touch the task-graph cost**: `p.BatchPhysXTasksSize` (default 3, max 8
   [eboot]) and the command-line switch **`-physxDispatcher=N`**, which makes UE 4.21 use PhysX's own
   `PxDefaultCpuDispatcher` with N dedicated threads instead of the task graph (code 0x2452c23-0x2453323 [eboot]).
   Both only move or batch the simulation's tasks; neither touches the queries.
5. **GPU PhysX is not feasible and would not help.** PhysX 3.4's GPU path covers rigid-body dynamics and broadphase
   (CUDA), not scene queries; the game's sim is small; the PhysX in the eboot is statically linked SysV-ABI code
   with UE-side callbacks, and GPU contact generation needs meshes cooked with `buildGRBData`. Replacing it with a
   Windows PhysX build is months of ABI thunking for zero gain on the measured cost.
6. **P19 (Dan wedged in walls)**: no 4.21 setting fixes a corner wedge by itself. One frame-rate-dependent factor
   is real in the engine code: `ResolvePenetration`'s last fallback moves by the adjustment **plus this frame's
   attempted move**, which is 8x shorter at 237 than at 30 FPS. Candidate settings (`p.MoveIgnoreFirstBlockingOverlap`,
   `p.InitialOverlapTolerance`, Dan's `MaxStepHeight` 100 > capsule half height 96) need the Dan state trace first.

---

## 1. Physics in UE 4.21 and what this eboot has

### 1.1 Where PhysX runs (UE 4.21 frame) [UE 4.21]

- One **sync scene** per world (`bEnableAsyncScene=False` [pak]). The world tick runs the tick groups
  `TG_PrePhysics` -> `TG_StartPhysics` -> `TG_DuringPhysics` -> `TG_EndPhysics` -> `TG_PostPhysics`.
  - `FStartPhysicsTickFunction` (string 0x3e32a78 [eboot]) calls `FPhysScene::StartFrame` -> `PScene->simulate(dt)`.
    PhysX splits the step into tasks and hands them to its `PxCpuDispatcher`.
  - `TG_DuringPhysics` ticks run on the game thread while the step runs on workers.
  - `FEndPhysicsTickFunction` (0x3e32aac) waits for the step's completion event, then `fetchResults` on the game
    thread (results, scene-query tree update), syncs component transforms, dispatches collision events.
- **CharacterMovement, AI controllers, animation and almost all Blueprint ticks run in `TG_PrePhysics`**, before
  the step. Their sweeps and overlaps run synchronously on the game thread against the last frame's results.
- **The CPU dispatcher** is chosen at scene creation (function 0x2452be0 [eboot], disassembled here):
  - `-physxDispatcher=N` on the command line (or bare `-physxDispatcher` = 4): `PxDefaultCpuDispatcherCreate(N, NULL)`
    (call 0x2f19aa0 at 0x2453323): PhysX's own N worker threads.
  - else, if threading is off (0x70b660 false) or `-SingleThreadedPhysics` is given: `FPhysXCPUDispatcherSingleThread`
    (vtable 0x50ee028): every PhysX task runs inline on the submitting thread, i.e. the game thread.
  - else `FPhysXCPUDispatcher`: each PhysX task becomes a `TGraphTask<FPhysXTask>` on the task graph, batched by
    `p.BatchPhysXTasksSize` (global 0x5607b08 = **3** [eboot]), priority `TaskGraph.TaskPriorities.PhysXTask`.
- **Scene locks**: UE creates the scene with `PxSceneFlag::eREQUIRE_RW_LOCK` and wraps every query in
  `SCOPED_SCENE_READ_LOCK` (`NpScene::lockRead`: per-thread depth, the RW lock on the first level) [UE 4.21;
  PhysX src]. On PS4 / Unix the PhysX RW lock is a pthread mutex plus a reader counter
  ([PsUnixMutex.cpp](https://raw.githubusercontent.com/NVIDIAGameWorks/PhysX-3.4/master/PxShared/src/foundation/src/unix/PsUnixMutex.cpp)):
  - `lockReader`: `mutex.lock(); atomicIncrement(readerCounter); mutex.unlock()` = **0x319c9c0** [eboot, matches
    the source exactly: `pthread_mutex_lock`, `lock xadd` at 0x319c6e0, tail-jump to `pthread_mutex_unlock`];
  - `lockWriter`: `mutex.lock(); while (readerCounter);` = 0x319ca00 (spins holding the mutex);
  - `unlockReader`: `atomicDecrement` = 0x319ca30; `unlockWriter`: `mutex.unlock()` = 0x319ca40.
  - 0x319c9c0 has 153 call sites in 55 functions (PhysX and APEX internals too, not only scene queries)
    (`rwlock_callers.txt`). study-sv-hotspots.md's label "MutexImpl::lock (scene lock)" is this reader lock.
  - Per locked read: one guest `pthread_mutex_lock` + `pthread_mutex_unlock` -> our HLE (`runtime_mutex.c`
    `posix_mutex_lock` -> winpthreads). Sampled 0.03-0.10 ms per frame in waves.

### 1.2 Settings and switches in this build

PhysicsSettings keys (`[/Script/Engine.PhysicsSettings]`, property names 0x3eb38d8-0x3eb3bc8 [eboot]; values from
`OVERBITE/Config/DefaultEngine.ini` [pak]; BaseEngine.ini sets only gravity and `bEnable2DPhysics`):

| Key | Game value | What it does at 237 FPS [UE 4.21 unless marked] | Use? |
|---|---|---|---|
| `bEnableAsyncScene` | False | A second scene for bodies flagged `bUseAsyncScene` (destructibles by default). Deprecated in 4.21, removed in 4.22 ([forum](https://forums.unrealengine.com/t/physics-async-scene/455624)). Nothing in this game asks for it; turning it on adds a second simulate / fetch per frame | no |
| `bSubstepping`, `bSubsteppingAsync`, `MaxSubstepDeltaTime` 0.016667, `MaxSubsteps` 6 | False | Substeps = ceil(dt / MaxSubstepDeltaTime), capped: **1 at 4.2 ms**. On, the step goes through `FPhysSubstepTask` (task priority `TaskGraph.TaskPriorities.PhysXStepSimulation`, 0x3e3366e) with force / kinematic-target interpolation: extra work, same result. Only splits large frames ([substepping blog](https://www.unrealengine.com/en-US/blog/physics-sub-stepping)) | no |
| `MaxPhysicsDeltaTime` | 0.033333 | Clamps dt from above (also `p.MaxPhysicsDeltaTime` 0x3e32a4c) | irrelevant |
| `SyncSceneSmoothingFactor` / `AsyncSceneSmoothingFactor` / `InitialAverageFrameRate` | 0 / 0.99 / 0.016667 | Averages the physics dt over frames (0 = off). Smoothing, not cost | no |
| `PhysXTreeRebuildRate` | 10 | Frames over which PhysX rebuilds its dynamic scene-query tree (rebuild steps run in `fetchResults` on the game thread; meanwhile moved objects are refit). At 237 FPS a full rebuild every 42 ms vs 333 ms on the PS4: 8x the rebuild work per second, better trees. The `p.PhysXTreeRebuildRate` string (0x3e33574) is a **console command** (registered through vtable +0x68 with a delegate at 0x246ece0), not a cvar, so `[SystemSettings]` cannot set it; the ini key can | only if `fetchResults` / `SqAABBTreeBuild` shows in samples (check existing captures first) |
| `bEnablePCM` True, `bEnableStabilization` False, `bEnableEnhancedDeterminism` False, `bDisableCCD` False, `bDisableActiveActors` False, KS/KK pairs on | as listed | Simulation quality / broadphase; small sim | no |
| `MaxDepenetrationVelocity` 0, `ContactOffsetMultiplier` 0.02, `Min/MaxContactOffset` 2 / 8 | | Simulated bodies only (not CharacterMovement) | no |
| `DefaultShapeComplexity` | CTF_UseSimpleAndComplex, `bDefaultHasComplexCollision=True` | Complex (triangle-mesh) collision exists for queries that ask for it; GuBV4 triangle-mesh queries are in the game-thread samples | cook-time |
| `BaseEngine.ini [/Script/Engine.Engine] bDisablePhysXHardwareSupport=True` | | The legacy GPU PhysX switch (APEX / particles), off | - |

`p.*` cvars present (UTF-16 strings [eboot], 72 in all; the full list: `grep -a "	p\." eboot0102_utf16.txt` in
`research-data/debug-menu-research-20261124/`). The ones that matter here:

| Cvar / switch | Address | Default | Effect | Route |
|---|---|---|---|---|
| `p.BatchPhysXTasksSize` | 0x3e33472, global 0x5607b08 | **3** [eboot] | PhysX tasks per task-graph task (max 8). Higher = fewer task-graph tasks (fewer FIFO pushes / pops, fewer worker wakes) and less parallelism for a small sim | `GOW3_UE4_CVARS` (FAutoConsoleVariableRef, read per dispatch: also live-capable) |
| `-physxDispatcher=N` | strings 0x3e3380a / 0x3e3382c, code 0x2452c23 | off | PhysX's own `PxDefaultCpuDispatcher` threads instead of the task graph | `/app0/ue4commandline.txt` (study-debug-menu-automation.md; **whether the port reaches that read is open, test T1**) |
| `-SingleThreadedPhysics` | 0x3e33ae4 / 0x3e35ab4 | off | sim inline on the game thread | diagnostic only, would add the sim to the game thread |
| `TaskGraph.TaskPriorities.PhysXTask` / `PhysXStepSimulation` / `PhyXSceneCompletion` | 0x3e32b34 / 0x3e3366e / 0x3e33746 | | Which worker set (normal / high / background) runs PhysX tasks. Guest priorities are not applied on the host (`runtime_thread.c:4`: "recorded, not enforced"), so only the queue choice changes | cvar |
| `p.ForceNoKSPairs`, `p.ForceNoKKPairs`, `p.ForceMbpClient`, `p.OverrideMbpNumSubdivisionsClient` | 0x3e33138 ... | | Broadphase setup at scene creation | not useful (small sim) |
| `p.DisableQueryOnlyActors` | 0x3e2ede6 | 0 | Query-only bodies not added to the simulation (incompatible with origin shifting) | sim-only; low value |
| `p.EnableFastOverlapCheck`, `p.AllowCachedOverlaps`, `p.SkipUpdateOverlapsOptimEnabled` | 0x3df1bc6, 0x3df1946, 0x3df3e7c | on [UE 4.21] | Overlap-update shortcuts for moved components | leave on |
| `p.MoveIgnoreFirstBlockingOverlap` | 0x3df10dc, bool 0x5bffd20 | **0** [eboot] | Ignore an initial blocking overlap when moving out of it in `SafeMoveUpdatedComponent` | P19 candidate (section 5) |
| `p.PenetrationPullbackDistance` | 0x3df13a2, float 0x56078f0 | **0.125** [eboot] | Extra distance added to a depenetration MTD | P19 |
| `p.PenetrationOverlapCheckInflation`, `p.InitialOverlapTolerance`, `p.EncroachEpsilon` | 0x3df14aa, 0x3df1a0a, 0x3e15974 | 0.1 / 0.0 [UE 4.21]; 0.15 [eboot] | Penetration / move-out rules | P19 |
| `p.CharacterStuckWarningPeriod` | 0x3deec00 | | Log throttle for "stuck in geometry" warnings (logging is compiled out in Shipping) | no |

Task-graph and tick cvars present: `TaskGraph.NumWorkerThreadsToIgnore` (0x3c63188; only removes workers),
`TaskGraph.UseHiPriThreads` / `UseBackgroundThreads`, `TaskGraph.TaskThreadPriority`, `tick.AllowAsyncTickDispatch` /
`AllowConcurrentTickQueue` / `AllowAsyncComponentTicks` / `AllowAsyncTickCleanup` (experimental in 4.21; already
experiment 5 of study-npc-tick-rate.md), `tick.DoAsyncEndOfFrameTasks`, `TaskGraph.TaskPriorities.AsyncTraceTask`
(0x3dec836: the engine's async traces, used only by callers that ask for them). No `ai.*` or `nav.*` cvar controls a
rate: only `ai.crowd.Draw*` debug switches and `ai.NavCollisionAvailable`.

### 1.3 What changing each does at 237 FPS

- **Nothing in the engine lowers the physics rate.** Every knob above either splits large steps or moves the same
  step's tasks. The step itself is small: 0.6-0.9 ms per frame summed over workers in the SV waves.
- **The game thread waits on physics only at `TG_EndPhysics`.** In run 5 the game thread waited 0.13-0.46 ms per
  frame on high-priority workers (study-sv-critical-path.md 1.3); physics completion is one of the things in that,
  with animation. The game thread's own PhysX time is the queries made in `TG_PrePhysics`.
- So the physics knobs can at best take PhysX's share out of the **task-graph traffic** (0.65-1.29 ms per frame on
  the game thread: FIFO pushes, pops, worker wakes, completions). How much of that traffic is PhysX's is unknown:
  the samples give only exclusive time. `-physxDispatcher` answers it in one launch.

## 2. Lower tick rates with interpolation: what 4.21 offers, what the game uses

### 2.1 Engine features vs this game

| Feature [UE 4.21] | What it does | This game [pak / asset] | Usable here? |
|---|---|---|---|
| Actor / component `TickInterval` | Tick every N s; the tick's DeltaSeconds is the time since its last tick, `GetWorldDeltaSeconds` stays the frame dt. Cooling-down tick functions are not queued as tasks | Used by `BP_AIActiveList` for inactive NPCs only (0.25-3.4 s); 0 for NPCs within 2500 uu or seen in the last 0.2 s. No class default sets one (study-npc-tick-rate.md) | Yes, with the timer caveats in 2.3 |
| Physics substepping | Splits large steps only | off | No (section 1) |
| Async scene | Separate scene, its own frame lag | off, deprecated | No |
| CharacterMovement network smoothing (`NetworkSmoothingMode`, `bNetworkSmoothingComplete`) | Interpolates the **mesh** of `ROLE_SimulatedProxy` pawns on network clients | Standalone game: NPCs are authority pawns, the path never runs | No |
| Animation URO (`bEnableUpdateRateOptimizations`, `a.URO.*` 0x3df492c-0x3df4c54) | Skips anim updates by screen size / visibility, with pose interpolation | Not enabled on NPC meshes (`CH_MrMad` `CharacterMesh0` sets only AnimClass, mesh, shadows, transform) | Only by an eboot default change; anim is 0.08-0.09 ms per frame on the game thread (parallel update already on, `a.ParallelAnimUpdate`), so little to win |
| `VisibilityBasedAnimTickOption` | Skip pose ticks when not rendered | default (characters: always tick pose) | small |
| AI perception intervals | Sense update rates | `PerceptionSystemClassName=None` [pak]: no perception system | n/a |
| Behavior Tree / EQS intervals | Service / task tick rates | AI is Blueprint (`BP_EntBaseController` + species `OnUpdate`), no BT | n/a |
| CrowdManager / RVO | Crowd simulation; internals already time-based (`PathOptimizationInterval`, `NavmeshCheckInterval`) | `[/Script/AIModule.CrowdManager] bResolveCollisions=True, MaxAgents=100` [pak]; NPC CMCs do not set `bUseRVOAvoidance` | cheap (AI + Detour + movement 0.13-0.16 ms per frame): low value |
| Navigation repath | `UPathFollowingComponent` repaths on nav-data changes; `RuntimeGeneration=Static` [pak] so never | The Blueprint calls `MoveToLocation` every tick, which builds a new path every call [UE 4.21: `AAIController::MoveTo` -> `FindPathForMoveRequest` -> `RequestMove`, which aborts the active move] | The waste is self-inflicted, see 2.2 |
| Fixed-rate physics / game tick with render interpolation | **Not in 4.21** (Chaos async physics with interpolation is UE5) | - | No. Frame generation is the route to more displayed frames than game ticks |

### 2.2 Decide at 30-60 Hz, move every frame

- An NPC is an actor tick (pawn Blueprint), a controller tick (the AI behaviour: two A* searches, traces,
  timers), a `UCrowdFollowingComponent` tick (follows the current path), a CharacterMovement tick (sweeps, floor
  checks), an anim instance and its skeletal mesh. Each is its own tick function.
- The movement and path following produce the motion. The controller only chooses goals and actions. Throttling the
  **controller** therefore keeps motion smooth with **no interpolation**: the CMC still moves the pawn along the last
  path at the frame's dt, and turns towards the controller's rotation at `RotationRate` per frame
  (`bUseControllerDesiredRotation` on MrMad's CMC [pak]). This is how UE titles normally throttle AI.
- What would need interpolation: throttling the **CMC or the pawn** (movement in steps of 1/30 s). UE 4.21 has no
  mesh smoothing for authority pawns, so the port would have to offset the mesh per frame itself (a hook on the
  mesh's relative location), and weapon / attack collision components attached to bones would lag the capsule. Not
  recommended.

Two ways to throttle the decisions:

**(a) Native path-query cache / throttle (runtime hook).** Targets found in the native tables [eboot]
(`find_natives.py`; FNameNativePtrPair {name, exec} entries, `{exec, name}` pairs in the second table are the
`Z_Construct` link info):

| UFunction | Exec thunk | Native impl called | Table entry |
|---|---|---|---|
| `UNavigationSystemV1::FindPathToLocationSynchronously` | 0x2d3b4e0 | 0x2ca48c0 (static impl) | 0x5308aa0 |
| `AAIController::MoveToLocation` | 0x2e46630 | 0x2d5d3f0 [med: the only non-FFrame call in the thunk] | 0x5337460 |
| `AAIController::MoveToActor` | 0x2e46340 | | 0x5337450 |

- Cache per querier (the controller passed as WorldContext / PathfindingContext): if the start and goal moved less
  than ~20-30 cm and the cached result is younger than 1/30-1/60 s, return the same `UNavigationPath*`. For
  `MoveToLocation`: if the goal moved less than the acceptance radius and the current request is still moving,
  return "already requested" without a new path.
- What it removes per second at 237 FPS: ~(2 x NPCs x 237) A* searches and `NewObject<UNavigationPath>` calls down
  to (2 x NPCs x 30-60). The UObject part matters most now that the purge fences are gone: object index allocation
  and the UObject hash (guest `scePthreadMutexLock` per object, 0.3-0.7 ms per frame in the 112 s burst,
  study-sv-critical-path.md section 5), plus fewer objects for GC and `ActivePaths` (P18).
- **Hazard: object lifetime.** The cached path object is unreferenced and dies at the next GC. The cache must be
  dropped whenever a GC ran since it was filled (watch the engine's GC counter, or hook the GC start), else the
  Blueprint reads a destroyed object. This is the main implementation risk.
- Behaviour: an NPC decides on a path up to 17-33 ms old. The PS4 at 30 FPS always did.

**(b) Controller tick interval + Blueprint timer fixes.** `BP_AIActiveList` calls `UpdateTickInterval(0.0)` three
times for active NPCs; a same-length literal edit to 1/60 or 1/30 throttles pawn actor + controller ticks (the
function sets both through `SetActorTickInterval`, which leaves component ticks, the CMC and path following, alone
[UE 4.21]). It also removes those tick functions from the task graph on skipped frames. study-npc-tick-rate.md
experiment 7 lists why it is **not a fix alone**: timers read `GetWorldDeltaSeconds` (frame dt) instead of the tick's
accumulated DeltaSeconds: invulnerability 0.167 s, flee / wait turns, ragdoll blend would run 4-8x slow. Each such
site needs a same-length swap of the `GetWorldDeltaSeconds` call for the tick's DeltaSeconds where it is reachable
(the ubergraph's `K2Node_Event_DeltaSeconds`), per controller class (11 controller disassemblies are in
`research-data/npc-tick-research-20261125/dis_BP_*Controller.txt`). Larger audit, larger behaviour risk, larger win
(controller Blueprint VM time, the traces in `CheckFloor` / `UpdateWaterCollision`, tasks).

### 2.3 Gameplay risks of any tick change

| Item | At a throttled controller / actor tick |
|---|---|
| Timers via `GetWorldDeltaSeconds` | Run slow by the interval ratio unless fixed (b) |
| Timers via the tick's DeltaSeconds (`UpdateCount -= dt`) | Correct (accumulated) |
| Enemy invulnerability window (`UpdateInvulnerable`, 0.167 s) | Uses world dt today: would stretch under (b) without the fix. Under (a): unchanged |
| Hit detection | Weapon / attack collisions are component overlaps driven by anim notifies (`HandleAttackCollisions`, `EnableAttackCollisions` in `CH_MrMad`): anim and movement stay per frame under (a) and (b) |
| Grain Grinder, book pedestals, `Slide_Pusher` | Per-tick impulses: a **1/30 s tick interval on these actors reproduces the PS4 exactly** (one impulse per 1/30 s, Dan's slide decays per frame with dt). The class default cannot take a `TickInterval` by a same-length edit (`Default__BP_GrainGrinder_C` serialises only `PrimaryActorTick.bCanEverTick`); the dt-scaled impulse edit of study-npc-tick-rate.md 3b stays the practical fix |
| Pushables | `UpdateMovement` uses `GetWorldDeltaSeconds`: a tick interval would make them slower still. Only the 1/30 constant edit (3c) fixes them |

### 2.4 How the port can apply each lever

| Route | Reaches | Notes |
|---|---|---|
| `GOW3_UE4_CVARS` -> `[SystemSettings]` of the override pak (`scripts/ue4_override.py` `apply`) | any cvar (`p.BatchPhysXTasksSize`, `p.MoveIgnoreFirstBlockingOverlap`, `tick.*`, `gc.*`) | restart-only, exists |
| Live cvar (`runtime_ue4_request_cvar`, `src/runtime_extra.c:700`) | cvars with a known object + registration site per build | a table row per cvar (01.00 + 01.02) |
| Config-class keys (`[/Script/Engine.PhysicsSettings] PhysXTreeRebuildRate`, `[/Script/AIModule.CrowdManager]`) | `set_section` is generic, `set_values` writes only `[SystemSettings]` and `[/Script/Engine.Engine]` | small `ue4_override.py` extension (the engine-level platform ini loads after DefaultEngine.ini, so its keys win) |
| Command line (`/app0/ue4commandline.txt`, 255 chars, `" -pak"` appended) | `-physxDispatcher=N`, `-SingleThreadedPhysics` | open: does the port's launch reach the file read (T1 of study-debug-menu-automation.md)? A per-run file mounted over the game's (`runtime_file_mount`) keeps the game folder untouched |
| Same-length Blueprint edits (`apply_edits`) | literals (`BP_AIActiveList` intervals), call swaps with `EX_Nothing` padding | byte-checked per build |
| Eboot patches (`patches/MediEvil.xml`) / byte-checked runtime hooks (`src/*_hook.h`, `probe.c`) | native defaults (CMC ctor 0x1e9a8a0, `USkinnedMeshComponent` URO default), the RW lock, path caching | per build, two builds |

## 3. PhysX on the GPU

- **What PhysX 3.4 offers** ([GPU Rigid Bodies, 3.4 manual](https://archive.docs.nvidia.com/gameworks/content/gameworkslibrary/physx/guide/Manual/GPURigidBodies.html)) [web]:
  broadphase, contact generation, shape / body management and the constraint solver in CUDA
  (`PxSceneFlag::eENABLE_GPU_DYNAMICS`, `PxBroadPhaseType::eGPU`, SM 3.0+), immutable per scene. Spheres, capsules and
  planes get their contacts on the CPU; meshes need `PxCookingParam::buildGRBData`; articulations unsupported;
  CCD, triggers and joint projection on the CPU; fixed-size GPU buffers. Legacy GPU particles and cloth existed too
  (`PtContextCpu.cpp`, `PtParticleSystemSimCpu.cpp` are the CPU versions compiled into this eboot [eboot]).
- **Scene queries are CPU.** The 3.4 manual's GPU chapter does not cover them; PhysX 5's Direct GPU API docs list
  scene queries as CPU-based ([Direct GPU API](https://nvidia-omniverse.github.io/PhysX/physx/5.4.0/docs/DirectGPUAPI.html)) [web].
  CharacterMovement's queries are also a dependent chain (each sweep's hit decides the next), so a GPU round trip
  per query would be far slower than the ~µs CPU query even if it existed [inferred].
- **UE 4.21 never enabled GPU rigid bodies** [UE 4.21]; `bDisablePhysXHardwareSupport=True` (BaseEngine.ini) is the
  old APEX / particle switch. The PS4 build has no CUDA path at all.
- **Replacing the eboot's PhysX with a Windows build** ([PhysX-3.4 source](https://github.com/NVIDIAGameWorks/PhysX-3.4),
  BSD-3): the eboot links Epic's PhysX 3.4 / APEX 1.4 branch statically (`Engine/Source/ThirdParty/PhysX3/PhysX_3.4`
  paths [eboot]), compiled for the SysV ABI. A host PhysX would need:
  - every PhysX entry point UE calls (factory functions, ~hundreds of virtual methods on `PxScene`, `PxRigidActor`,
    `PxShape`, ...) redirected through SysV <-> Microsoft x64 thunks, or PhysX rebuilt with clang using `sysv_abi` on
    every exported and virtual function;
  - reverse thunks for every callback UE implements in guest code (allocator, error callback, `PxCpuDispatcher`,
    simulation-event and contact-modify callbacks, query filter callbacks, APEX render / resource callbacks);
  - all objects from one SDK (cooked meshes are created at load from UE's PS4-cooked streams; UE also reads public
    members such as `userData` directly, and APEX 1.4 must move along with it);
  - Epic's local PhysX changes reproduced.
  Weeks to months, high crash risk, and **no gain on the measured cost**: the queries would still be CPU code, of
  the same algorithm, on the same thread. The eboot's PhysX is already native x86-64 (AVX). **Not feasible / not
  worth it.**

## 4. PhysX on its own thread(s)

- **Simulation:** already off the game thread (task-graph workers). `-physxDispatcher=N` moves it to N dedicated
  PhysX threads (`PxDefaultCpuDispatcher`, lock-free job queue, created through `scePthreadCreate` -> our
  `runtime_thread.c` host threads). Expected: PhysX tasks leave the UE task-graph queues, so fewer FIFO pushes /
  pops and wakes on the game thread and workers; the sim itself costs the same. With N = 2 the dispatcher's
  threads also stop competing with render tasks for the 4 high-priority workers. [inferred]
- **Scene queries:** must stay on the caller's thread. CharacterMovement, `UPrimitiveComponent::UpdateOverlaps`, the
  Blueprint trace nodes and AkComponent occlusion traces call the synchronous API and use the hit at once. UE 4.21's
  async traces (`AsyncLineTraceByChannel` etc., one task per frame) serve only callers that ask for them. Parallel
  character movement does not exist in 4.21 (the CMC is not thread-safe; `tick.AllowAsyncComponentTicks` applies only
  to components that opt in with `bRunOnAnyThread`). [UE 4.21]
- **Our host mutex is not the cost.** The reader lock (0x319c9c0) was 0.03-0.10 ms of ~1 ms of PhysX game-thread
  time in the waves. A cheaper lock (section 6, S1) saves at most that.
- **Priorities / affinities:** the runtime records guest priority and affinity but does not apply them
  (`runtime_thread.c:4`); every guest thread is a normal-priority Windows thread on all 16 CPUs. Raising the game
  thread (`main`) above normal could help under contention, but run 5 showed the game thread rarely displaced
  (study-sv-critical-path.md 4.1). Not a physics lever.
- **What the game thread still waits on:** `TG_EndPhysics` (step completion + `fetchResults` on the game thread),
  parallel animation completion, and the frame-end sync with the render thread. None of these is the wave's cost;
  the wave is the game thread's own CPU (queries, Blueprint VM, task traffic).

## 5. Collision and character movement at small steps (P19)

What 4.21's CharacterMovement does that depends on the step size [UE 4.21]:

1. **`MaxSimulationTimeStep` (0.05) / `MaxSimulationIterations` (8)** only split large steps. There is no minimum step.
   Small steps are the CMC's design point; most movement code is dt-correct (study-npc-tick-rate.md: Dan's walking,
   slide and knockback are).
2. **`ResolvePenetration` is partly step-size dependent.** When a move starts in penetration: (i) teleport by the MTD
   if an inflated overlap test at the target is clear (`p.PenetrationOverlapCheckInflation`); (ii) else sweep by the
   MTD; (iii) if still stuck, sweep by the sum of two MTDs; (iv) last, sweep by **MTD + this frame's attempted move**.
   Fallback (iv) is the one that gets out of corners formed by two surfaces (a door frame and a drain pipe), and its
   move part is 812 cm/s x dt: 27 cm at 30 FPS, 3.4 cm at 237. That is a plausible frame-rate dependence for a
   two-surface wedge [inferred].
3. **Depenetration per call is capped** (`MaxDepenetrationWithGeometry` 500, `MaxDepenetrationWithPawn` 100 per
   call; not overridden for Dan [asset]). At high FPS there are more calls per second, which helps, not hurts.
4. **Step-up on every blocking hit.** Dan's `MaxStepHeight` is **100** with capsule half height **96** [asset]: every
   blocking hit against a wall feature tries a step-up (up 100, forward by this frame's delta, down). At 237 FPS the
   forward part is 3.4 cm, so step-ups land on ledge edges (perching) more often than at 30 FPS, and a failed
   step-up is undone each time [inferred, to confirm with the trace].
5. **Pawn vs pawn.** NPC capsules are blocking for Dan; NPCs move towards Dan every frame. A pawn MTD (from an NPC
   pressing in) pushes Dan towards the wall, the wall MTD back, inside the 100 cm per-call pawn cap. Knockback adds a
   slide velocity into the wall [eboot, study-npc-tick-rate.md].

Settings that could matter, only after the trace shows which branch fails (the user's rule: no toggle bisects):

| Setting | Default | Expected effect | Route |
|---|---|---|---|
| `p.MoveIgnoreFirstBlockingOverlap=1` | 0 [eboot] | Moves out of an initial overlap are not reported as blocking hits: fewer `ResolvePenetration` cycles, easier exit from a wedge | cvar |
| `p.InitialOverlapTolerance` (e.g. -0.1) | 0 | Moves slightly into a penetrated surface's plane count as "moving out" | cvar |
| `p.PenetrationPullbackDistance` (e.g. 0.5) | 0.125 [eboot] | Larger MTD margin, fewer re-penetrations next frame | cvar |
| Dan's `MaxStepHeight` 100 -> 45-55 | 100 [asset] | Fewer step-up attempts into wall features. **Changes traversal** (steps Dan can climb; level design may rely on 100) | same-length float edit in `BP_df_character` `CharMoveComp` |
| A minimum effective move in fallback (iv) | - | e.g. use max(frame move, 1/30 s of velocity) | eboot patch in `ResolvePenetrationImpl` (not located yet) |

The Dan state trace tool (P18 plan item 6) should record per frame: location, velocity, slide (+0x780), movement mode,
the blocking hit (normal, actor, component), whether the move started penetrating, which `ResolvePenetration`
branch ran, and step-up attempts.

## 6. Ranked experiments and changes

Each is one change per launch with the user's go, in the SV Trolls Head wave (or the P19 spot). Measure with the flip
trace (0.1% / 1% lows, frames over 6 ms) and a light sampling capture (game-thread ms per frame by area,
study-sv-hotspots.md method), with no periodic logging.

### No code (cvar / ini / command line)

| # | Change | Measure | Expected | Risk | Effort |
|---|---|---|---|---|---|
| N1 | `GOW3_UE4_CVARS=p.BatchPhysXTasksSize=8` | game-thread task-graph share (0x6e7bf0 / 0x6ea260 / 0x720480 samples), worker PhysX samples, `main <- HP worker` waits | fewer PhysX task-graph tasks; small gain (0.0-0.2 ms per frame) if PhysX is a real share of the traffic; the step may finish later (watch the EndPhysics wait) | low (engine knob, max 8) | none |
| N2 | `-physxDispatcher=2` in a mounted `ue4commandline.txt` (after T1 shows the file is read) | same as N1, plus the new PhysX threads' busy time | PhysX leaves the task graph entirely. Sizes PhysX's share of the 0.65-1.29 ms task traffic in one launch | low-medium (supported engine path; new host threads; PhysX's job queue) | small (file mount) |
| N3 | Data check, no launch: count `fetchResults` / `SqAABBTreeBuild` / `SqIncrementalAABB*` / `ScScene` samples on the game thread in the existing cap3 / cap4 / run 5 pickles | ms per frame | decides whether `PhysXTreeRebuildRate` (ini key, needs the section extension) is worth a launch | none | small |
| N4 | Reference: npc-tick-rate experiments 1 (GC interval), 3 (cap 120 vs 237: per-frame vs per-second scaling), 5 (`tick.AllowAsyncTickDispatch`) | as there | | | |
| N5 | P19, only after the trace shows the branch: `p.MoveIgnoreFirstBlockingOverlap=1` (then separately `p.PenetrationPullbackDistance`) | repro at the wedge spot | Dan exits wedges | low-medium (affects all moves: watch clipping through thin geometry) | none |

### Small patches

| # | Change | Measure | Expected | Risk | Effort |
|---|---|---|---|---|---|
| S1 | Dan state trace (dev build, ring buffer, hotkey dump) incl. the P19 fields of section 5 | a captured wedge | the failing branch; prerequisite for N5 / S5 | low (dev only) | small-medium |
| S2 | Tick census tool (dev build): tick functions queued per frame by class, Blueprint `ReceiveTick` calls by class, path queries per frame | one SV wave | where the task-graph and Blueprint VM time go; which actors could take an interval | low (dev only) | small-medium |
| S3 | PhysX reader lock without the host mutex: replace `lockReader` (0x319c9c0) / `lockWriter` (0x319ca00) / `unlockWriter` (0x319ca40) with an atomic protocol (writer flag in the counter's high bit; readers spin while it is set), or route their `pthread_mutex_*` to a light host lock | 0x319c9c0 (rt) samples | up to 0.03-0.10 ms per frame | medium (lock protocol; APEX and PhysX internals use the same lock) | small |
| S4 | Grain Grinder and pushables (P18 plan item 5, study-npc-tick-rate.md 3b / 3c) | SF ring / push speed at 60 vs 237 | PS4 behaviour at any FPS | low | small (same-length edits) |

### Medium projects

| # | Change | Measure | Expected | Risk | Effort |
|---|---|---|---|---|---|
| M1 | **Path-query cache / throttle hook** (2.2 a) at 0x2ca48c0 / 0x2d5d3f0 (01.00 addresses to find), 30-60 Hz per NPC, GC-safe | path queries and `UNavigationPath` creations per second (S2), game-thread ms in fights, the UObject-create bursts | A* and UObject churn down 4-8x; fewer GC objects; the 112 s burst class shrinks | medium (object lifetime across GC; NPC reaction 17-33 ms older, as on PS4) | medium (2-4 days with byte checks for both builds) |
| M2 | Controller tick interval 1/60 via `BP_AIActiveList` + the `GetWorldDeltaSeconds` timer fixes in each controller (2.2 b) | as M1 plus Blueprint VM share | larger cut (controller VM, traces, tasks) | medium-high (timers, invulnerability, AI feel) | medium-large (per-class bytecode audit) |
| M3 | Asset writer that rewrites export sizes (general) | - | unlocks class-default edits: tick intervals for the Grain Grinder and decor actors, CMC `MaxSimulationTimeStep`, URO on NPC meshes | medium | medium-large |

### Large or not feasible

| # | Idea | Verdict |
|---|---|---|
| L1 | GPU PhysX (CUDA) for this game | Not feasible and no benefit: queries are CPU, the sim is small, cooked meshes lack GRB data, UE 4.21 has no path |
| L2 | Replace the static PS4 PhysX with a Windows / host build | Not worth it: weeks to months of ABI thunks and callbacks (APEX too), same CPU algorithm afterwards |
| L3 | Physics / AI / movement at a fixed 30-60 Hz with render interpolation | Not in 4.21; needs an engine-level rework (tick split, transform interpolation of every moved component, collision lag). Frame generation is the practical route to more shown frames |
| L4 | Scene queries off the game thread | Would need rewriting CharacterMovement and Blueprint callers to async traces: no |
| L5 | NPCs in `NavWalking` mode (no floor sweeps) | Possible in principle (CMC default land mode), but changes NPC collision with the world and knockback / falling; class-default change (M3). Only if S2 shows NPC CMC queries dominate |

## Open questions

1. PhysX's share of the task-graph traffic (N2 answers it).
2. Whether the port's launch reaches `/app0/ue4commandline.txt` (T1); N2 depends on it.
3. Which hot callers take the PhysX reader lock (the samples have no caller frames); a frame-pointer walk in the dev
   sampler would show it (study-sv-hotspots.md 8.4).
4. The [UE 4.21] statements above (scene flag `eREQUIRE_RW_LOCK`, `MoveTo` re-pathing, tick-interval DeltaSeconds,
   `ResolvePenetrationImpl` fallbacks) were not re-read in the engine source; the eboot can confirm each.
5. 01.00 addresses for the hook targets of M1 and S3.

## Files (`research-data/physics-ai-research-20261125/`)

| File | What |
|---|---|
| `rwlock_callers.py` / `rwlock_callers.txt` | callers of PhysX `ReadWriteLock::lockReader` / `lockWriter` (uses the sv-hunt loader) |
| `cvar_defaults.py` / `cvar_defaults.txt` | code around cvar registrations and the globals they read |

Ad-hoc checks (commands in this study's session, results quoted above): `edis.py 0x2452be0` (dispatcher choice),
`edis.py 0x319c900 0x319cb40` (RW lock), `find_natives.py 0102 FindPathToLocationSynchronously MoveToLocation ...`,
global reads at 0x5607b08 / 0x5bffd20 / 0x56078f0.
