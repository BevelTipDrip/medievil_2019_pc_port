# Study: NPC / AI / movement tick rate at 237 FPS (2026-11-25)

Static research only: no launch, no build, no source edit. Raw data, scripts and disassemblies:
`research-data/npc-tick-research-20261125/` (on D:). Game build analysed: 01.02 (`E:\MediEvil\CUSA11227-0102`);
eboot addresses are vaddr-0 image offsets of `research-data/fname-research-20261122/image_0102.bin` (the patch XML
adds 0x400000), cvar / property name strings are from `research-data/debug-menu-research-20261124/eboot0102_*.txt`.
Tools reused: `dump_asset.py` (class defaults), `kismet.py` (Blueprint bytecode), `name_grep.py` (name maps).
New here: `find_names.py`, `find_natives.py`, `scan_offsets.py`, `involuntary_audit.py`, `flip_period.py`.

Status words: everything below is **found** (static). Nothing is fix built, run or verified. Confidence tags:
[asset] = read from the cooked Blueprints / maps, [eboot] = read from the eboot, [UE 4.21] = engine source behaviour
recalled from memory (not checked against source here), [inferred] = my reasoning.

## Summary

1. **The user's hypothesis is right for the AI, and worse than linear for navigation.** MediEvil's AI is Blueprint,
   not BehaviorTree. Every active NPC's controller tick (every frame, tick interval 0) runs its behaviour, and the
   chase / wait / wander behaviours call `FindPathToLocationSynchronously` (via `CanNavToLocation`) **and**
   `MoveToLocation` every tick: two A* searches per NPC per frame [asset]. Each `FindPathToLocationSynchronously`
   returns a `UNavigationPath` UObject that keeps its path alive until the next garbage collection (61.1 s,
   BaseEngine.ini), and every path is registered in the nav-data's `ActivePaths` [UE 4.21]. So `ActivePaths` grows
   by (calls per frame x FPS) until GC: its per-frame purge (the 3 ms loop at guest+0x2c94b90) costs ~FPS per frame
   and ~FPS^2 per second, with a sawtooth of the GC period [inferred, fits the measured 3 ms]. At the PS4's 30 FPS the
   same scene had ~1/8 the entries and 1/64 the purge work per second.
2. **The Scarecrow Fields obstacle is very probably the Grain Grinder** (`Maps/Level_05_SF/BP/BP_GrainGrinder`,
   placed as `SF_GrainGrinder_Rotor_01_2` with 8 grain cannons). It pulls Dan towards its blades by calling
   `SetInvoluntaryMotion` **every tick with an impulse not scaled by delta time** [asset]. The native slide
   (UOBCharacterMovementComponent) adds each impulse to a slide velocity and decelerates it by 2571.75 cm/s^2 x dt
   [eboot]. The outer ring's pull adds 32.7 cm/s per tick; deceleration removes 2571.75/FPS per tick. Below
   **~79 FPS** the pull never builds up; above it the slide accelerates to the 1828.8 cm/s cap, more than Dan's
   812 cm/s walk, so he cannot leave the ring [inferred from both]. That matches "fine at 60, impassable at 144".
3. **All pushable objects move slower as FPS rises** above 66.7 FPS (`BP_SplinePushableObjectComponent`: velocity
   = -velocity x clamp(4 x dt) + 20000 x dt, clamped to 300 cm/s) [asset]. They move at 300 cm/s up to 66.7 FPS,
   135 at 144 and 83 at 237. This covers the SF hay bale, 10 pushables in the Sleeping Village, and others in
   Hilltop Mausoleum and Asylum Grounds.
4. **Dan's own movement and knockback are frame-rate safe** in the parts checked: CharacterMovement sub-steps only
   above 50 ms, and the native slide is dt-correct [eboot]. Dan's jump uses a 30 Hz accumulator
   (`Run30FPSTick`) [asset]. I found **no frame-rate-dependent cause for P19 (stuck in wall)**. It needs a trace
   (experiment 6).
5. **UE 4.21 has no separate game tick.** One engine loop iteration is one world tick plus one rendered frame.
   `t.MaxFPS`, the engine frame-rate range and `bUseFixedFrameRate` all pace the whole game. `-UseFixedTimeStep`
   fixes dt per frame, which speeds the game up. The only per-actor levers are tick intervals (Blueprint / asset),
   and they have side effects in this game's Blueprints. To show more frames than the game ticks, the route is
   frame generation (FSR3 FG / DLSS FG on the roadmap).

---

## 1. What runs every frame and how it depends on dt

| System | How it runs in this build | Cost per second | Behaviour with small dt | Controls present in the 01.02 eboot / paks |
|---|---|---|---|---|
| Actor / component ticks | every frame unless `TickInterval > 0` [UE 4.21]. With an interval the tick function's DeltaSeconds is the time since its last tick [UE 4.21, medium confidence] | ~FPS | engine-side fine. Blueprints that use `GetWorldDeltaSeconds` under an interval get the frame dt, not the accumulated one [UE 4.21] | `SetActorTickInterval` / `SetComponentTickInterval` (ASCII names 0x3e6ac7c / 0x3e6c316), `TickInterval` property names 0x3e6b2d5 / 0x3e6c41a / 0x3e86a80. No global cvar. |
| The game's AI LOD | `Blueprints/AI/BP_AIActiveList` (component, ticks every frame) walks every registered NPC every frame. NPCs within `ActiveRange` 2500 uu of the camera, or rendered in the last 0.2 s, get `UpdateTickInterval(0.0)`. Inactive ones get 0.1 x (i%10) + ((i%10)+1)/4 = 0.25-3.4 s, staggered [asset] | ~FPS x active NPCs | n/a | Blueprint literals (same-length editable) |
| AI controllers | `BP_EntBaseController` ReceiveTick -> (not paused, Active) `UpdateCount -= dt`, `UpdateInvulnerable`, `UpdateAudioEvents`, `OnUpdate(Behavior)` (per species: `StandardChase` / `StandardWander` / `StandardWaiting` / `StandardFlee` ...), `CheckFloor`, `UpdateWaterCollision`, `UpdateStateFlags` [asset] | ~FPS x active NPCs | timers use the tick's DeltaSeconds or `GetWorldDeltaSeconds` (fine at interval 0) | none (Blueprint) |
| Path queries | `StandardChase`: `CanNavToLocation` (= `FindPathToLocationSynchronously`, a `UNavigationPath` UObject per call) then `NavigateToLocation` (= `MoveToLocation`), **every tick, no throttle**. `StandardWaiting`: up to two `CanNavToLocation`. `StandardWander`: `CanNavToLocation`, `EnemyMagnetGetPath` / `FindPathToLocationSynchronously`, `NavigateToLocation` [asset] | ~FPS x NPCs x 2 A* | n/a | none |
| Path following | controllers use `UCrowdFollowingComponent` (`BP_EntBaseController` export 865 `CrowdFollowingComponent PathFollowingComponent`) [asset] -> detour crowd | ~FPS | time-based intervals inside | `[/Script/AIModule.CrowdManager]` keys, below |
| Navigation data | `ANavigationData::TickActor` (0x2c94b30, impl-cpu-waste.md): `PurgeUnusedPaths` over `ActivePaths` (+0x3e8) every frame, observed paths every `ObservedPathsTickInterval` (name 0x3ee7968), repath requests [eboot + UE 4.21] | per frame ~ size of ActivePaths ~ FPS x seconds since GC -> **~FPS^2** | n/a | `gc.TimeBetweenPurgingPendingKillObjects` (UTF-16 0x3e609d2; 01.00 0x3e5d980) bounds the growth |
| Detour crowd | `UCrowdManager::Tick(dt)` from the navigation system every frame: neighbours, velocity sampling, `bResolveCollisions` iterations, integrate [UE 4.21] | ~FPS x agents | dt-consistent (acceleration x dt) | config keys in the eboot: `MaxAgents` 0x3ef85d0, `MaxAgentRadius` 0x3ef85c1, `MaxAvoidedAgents` 0x3ef85b0, `MaxAvoidedWalls` 0x3ef85a0, `NavmeshCheckInterval` 0x3ef858b, `PathOptimizationInterval` 0x3ef8572, `SeparationDirClamp` 0x3ef855f, `PathOffsetRadiusMultiplier` 0x3ef8544, `bResolveCollisions` 0x3ef8531, `AvoidanceConfig` 0x3ef85eb (`AdaptiveDivisions` / `Rings` / `Depth` 0x3ef8490 / 82 / 74). The game sets `bResolveCollisions=True`, `MaxAgents=100` (DefaultEngine.ini) |
| CharacterMovement | component tick every frame. `PhysWalking` / `PhysFalling` split dt only when it exceeds `MaxSimulationTimeStep` (0.05) up to `MaxSimulationIterations` (8) [UE 4.21]. Floor sweep each frame, step-up when blocked, `ResolvePenetration` when a move starts penetrating (up to `MaxDepenetrationWithGeometry` / `WithPawn` per call) | ~FPS x characters | designed for variable dt; small dt is safe. **There is no minimum or fixed step in 4.21's CMC** | property names (ASCII): `MaxSimulationTimeStep` 0x3e7ca58, `MaxSimulationIterations` 0x3e7ca40, `bForceMaxAccel` 0x3e7cd95, `MaxDepenetrationWithGeometry` 0x3e7ca23, `MaxDepenetrationWithPawn` 0x3e7c9e6, `bUseRVOAvoidance` 0x3e7c5d3, `bEnablePhysicsInteraction` 0x3e7cc94, `bAlwaysCheckFloor` 0x3e7c62a, `NavMeshProjectionInterval` 0x3e7c44a, `bEnableScopedMovementUpdates` 0x3e7cda4. Not config properties: only asset or eboot edits reach them |
| Dan's native slide (UOBCharacterMovementComponent) | `SetInvoluntaryMove` (exec 0x660ea0, impl 0x650ac0) stores a pending impulse. The tick (0x653b10) adds it once to the slide vector (+0x780), scaled by 16 / CustomMass. 0x6510e0 decelerates the slide by GroundDecel (+0x75c, 2571.75) or AirDecel (+0x760, 1714.5) **x dt** and clamps it to MaxSlideSpeed (+0x758, 1828.8). Walking adds slide + velocity (0x653774). Defaults from the constructor 0x6616f0 [eboot] | ~FPS | **dt-correct for one-shot impulses. Per-tick callers add one impulse per frame, so their push grows with FPS** | Blueprint `ScaleGlobalConstants`, `SetInvoluntaryImpulseScale` (`BP_EntBase` calls them with 1.0) |
| Physics (PhysX) | one scene step per frame. dt clamped to `MaxPhysicsDeltaTime` 0.033333, `bSubstepping=False`, `MaxSubstepDeltaTime` 0.016667, `MaxSubsteps` 6 (DefaultEngine.ini) [asset] | ~FPS | sub-steps only split large dt: irrelevant at 4 ms | p.* cvars below; `[/Script/Engine.PhysicsSettings]` keys (names 0x3eb3944-0x3eb3977) |
| Animation | skeletal meshes tick every frame (Character meshes `AlwaysTickPoseAndRefreshBones` [UE 4.21]), AnimBP event graph on the game thread | ~FPS x meshes | fine | `bEnableUpdateRateOptimizations` (0x3ec5b5e) is per component and off by default. `a.URO.Enable` / `a.URO.ForceAnimRate` (0x3df492c / 0x3df4a9c) act only on URO-enabled meshes. `a.ParallelAnimUpdate`, `a.ForceParallelAnimUpdate` (0x3df40aa / 0x3df41c6) |
| Tick dispatch | one TaskGraph task per tick function per frame (the lock-free list / link allocator samples, study-sv-hotspots.md) [UE 4.21] | ~FPS x tick functions | n/a | `tick.AllowAsyncTickDispatch` 0x3e5dd82, `tick.AllowConcurrentTickQueue` 0x3e5dd00, `tick.AllowAsyncComponentTicks` 0x3e5dc76, `tick.AllowAsyncTickCleanup` 0x3e5de1a (all default 0, experimental in 4.21) |
| Game-side fixed steps | Dan: `Run30FPSTick` accumulates `GetWorldDeltaSeconds` and runs `Update30FPS` (jump impulse, tilt, upper-body orientation) once when 1/30 s has accumulated (`30FPSTickDuration` 0.03333) [asset]. `BP_EntBase.FPS = 30` is only a unit conversion in `SetInvoluntaryMotion` (vector x 0.9525 x 30) | - | fine | - |

### Engine-wide frame controls (none of them separate the game tick from rendering)

- `t.MaxFPS` (UTF-16 0x3e61b82): works when set at run time (01.02's own command, card engine-cap-cvars), not from
  config. It caps the whole loop.
- `SmoothedFrameRateRange` / `bSmoothFrameRate` / `bUseFixedFrameRate` / `FixedFrameRate` (ASCII 0x3e83b57 /
  0x3e83b90 / 0x3e83b7d / 0x3e83b6e): the project sets `bSmoothFrameRate=False`, `bUseFixedFrameRate=False`,
  `MinDesiredFrameRate=30` (DefaultEngine.ini `[/Script/Engine.Engine]`). The port's `frame_rate_range`
  (ue4_override.py) already lifts the 60 bound. A fixed frame rate would pace the whole game [UE 4.21].
- `FApp::bUseFixedTimeStep` / `FixedDeltaTime` (01.02 0x581ed32 / 0x5601638, study-debug-menu-automation.md): fixed
  game dt per frame. Set to 1/30 at 237 FPS the game runs 8x too fast. Useful for determinism only.

### What the paks set (01.02)

- `OVERBITE/Config/DefaultEngine.ini`: `[/Script/Engine.Engine] bSmoothFrameRate=False, bUseFixedFrameRate=False,
  MinDesiredFrameRate=30`. `[/Script/NavigationSystem.RecastNavMesh] RuntimeGeneration=Static` (agent radius 20,
  height 100, step 55). `[/Script/NavigationSystem.NavigationSystemV1] CrowdManagerClass=CrowdManager,
  DirtyAreasUpdateFreq=60, bTickWhilePaused=False, DataGatheringMode=Instant, ActiveTilesUpdateInterval=1`.
  `[/Script/AIModule.CrowdManager] bResolveCollisions=True, MaxAgents=100`. `[/Script/AIModule.AISystem]
  PerceptionSystemClassName=None`. `[/Script/Engine.PhysicsSettings]` as in the table, plus
  `MaxDepenetrationVelocity=0`, `bEnablePCM=True`, `bEnableStabilization=False`,
  `DefaultShapeComplexity=CTF_UseSimpleAndComplex`, `PhysXTreeRebuildRate=10`.
- `Engine/Config/BaseEngine.ini`: `gc.TimeBetweenPurgingPendingKillObjects=61.1`, incremental BeginDestroy on. The
  project does not override GC.
- `Engine/Config/PS4/PS4Engine.ini` (the file the override pak replaces): `[SystemSettings]` render cvars only;
  `bSmoothFrameRate=false`.
- Dan, `Characters/Dan/BP_df_character` (`OBCharacterMovementComponent CharMoveComp`): MaxStepHeight 100 (capsule
  half height 96, radius 50), GroundFriction 30, MaxWalkSpeed 812, MaxAcceleration 15000 (the Blueprint switches
  between `DefaultMaxAcceleration` 5120 and `ShoulderChargeMaxAcceleration` 10000), BrakingFrictionFactor 10,
  BrakingDecelerationFalling 15000, AirControl 1, RotationRate yaw 395.5, Mass 1, InitialPushForceFactor 50, nav
  agent 42 / 192, `CustomMass` 14. **No tick interval and no simulation-step overrides.**
- NPCs, e.g. `Characters/Mr_Mad/CH_MrMad` (Sleeping Village): CharMoveComp overrides only
  `bUseControllerDesiredRotation`. Capsule 100 / 44. Controller `BP_MrMadController_C` (child of
  `BP_EntBaseController`, which has no tick interval in its defaults). The Sleeping Village gameplay map spawns
  BoilerGuard, MrMad, MsMad, NellieMad, PuffyFish, Rat_V2 and ToadDemon.
- 32 assets name a tick interval or time dilation (`name_grep_tick.txt`). Among them: `BP_AIActiveList` (above),
  `BP_EntBase` / `BP_EntBaseController` (`UpdateTickInterval` -> `SetActorTickInterval` on pawn and controller),
  triggers, audio regions, `Interactive_Parent`. This is the game's own LOD scheme.

## 2. How the port can apply each lever

| Lever | Route today | Needs |
|---|---|---|
| Any startup cvar (`gc.*`, `tick.*`, `a.*`, `p.*`, `t.*`) | `GOW3_UE4_CVARS="name=value;..."` -> `[SystemSettings]` of `Engine/Config/PS4/PS4Engine.ini` in the override pak (ue4_override.py `apply`). `[SystemSettings]` outranks the project-setting priority that `[/Script/Engine.GarbageCollectionSettings]` uses [UE 4.21] | nothing (restart-only) |
| Live cvar | `runtime_ue4_request_cvar` (src/runtime_extra.c) | a row per cvar: TAutoConsoleVariable object + registration site per build, as for `r.GPUParticle.FixDeltaSeconds` (code change) |
| Config-class properties (`[/Script/AIModule.CrowdManager]`, `[/Script/NavigationSystem.*]`, `[/Script/Engine.PhysicsSettings]`, `[/Script/Engine.Engine]`) | `set_section` is generic, but `apply` only writes `[SystemSettings]` and `[/Script/Engine.Engine]`. The engine-level platform ini loads after DefaultEngine.ini, so keys written there win | a small ue4_override.py extension (e.g. a `GOW3_UE4_INI` "section|key=value" list) |
| Blueprint literals / operands (tick-interval literals in `BP_AIActiveList`, the Grain Grinder, pushables) | `apply_edits` / `edited_asset` (same-length byte edits, checked per build, EX_Nothing padding as in the debug-menu edit) | an edit list per asset |
| CMC properties (`MaxSimulationTimeStep`, `bForceMaxAccel`, depenetration limits), tick intervals in class defaults | not same-length (adding a tagged property). An eboot patch of the native constructor is possible (UOBCharacterMovementComponent ctor 0x6616f0 calls the UCharacterMovementComponent ctor 0x1e9a8a0) | asset writer that rewrites export sizes, or an eboot patch |
| Engine frame rate | `engine_fps` (60 / 120 / 240) -> `frame_rate_range`, present limiter `GOW3_FPS_CAP`, Reflex | exists |

Earlier fixed-step work, for context:
- `r.GPUParticle.FixDeltaSeconds` (card single-particle-pass, study-particle-fixed-step.md): the default 1/30 makes a
  second GPU particle pass above 30 FPS. The port sets 0 (one pass at the real dt). The torch / ember artefact came
  from the single pass's ping-pong state, not from dt maths. `PARTICLE_FIXED_STEP` "0.033333335" restores stock.
- study-debug-menu-automation.md: `-UseFixedTimeStep` / the `FApp` globals give a fixed dt per frame (1/30 by
  default; 1/cap through the port for real-time speed). That is for determinism, not a separate simulation rate.

## 3. Frame-rate-dependent bugs found

### 3a. Navigation paths kept alive until GC (P18, the 3 ms purge) [asset + UE 4.21 + inferred]

- `BP_EntBaseController.CanNavToLocation` (`dis_BP_EntBaseController.txt`, function at line ~1240):
  `FindPathToLocationSynchronously(self, pawn location, Location)`, then reads `PathPoints`. Callers:
  `StandardChase` (every tick while chasing, followed by `NavigateToLocation` = `MoveToLocation`),
  `StandardWaiting` (two calls), `StandardWander`, `StandardDistracted`, `StandardFlee`, `Jump`, `JumpToLocation`.
  The tick chain: ReceiveTick -> ubergraph 0xcb8 -> `OnUpdate(this.Behavior)` (`ug_ctrl.txt`).
- [UE 4.21] `FindPathToLocationSynchronously` creates `NewObject<UNavigationPath>(NavSys)`, and `SetPath` keeps the
  `FNavPathSharedPtr`. The nav data's `CreatePathInstance` calls `RegisterActivePath` (01.02 0x2c958c0 appends to
  +0x3e8, impl-cpu-waste.md). The UObject is unreferenced at once but lives until GC, and its `BeginDestroy`
  releases the path. So every call leaves one valid weak entry in `ActivePaths` for up to 61.1 s, and the purge
  walks them all every frame. `MoveToLocation` paths are released when the next request replaces them (the purge
  drops them a frame later).
- Scale: N calls per frame at F FPS leave N x F x t entries t seconds after a GC, N x F x 30 s on average. With
  ~10 calls per frame at 237 FPS that is ~70k entries on average and ~145k before GC. At ~40 ns per fenced,
  cache-missing entry that is ~3 ms, the measured cost. The per-frame cost grows with FPS, so a higher frame rate
  slows the game thread, and the sawtooth resets at each GC. Each GC then also has ~10^5 extra UObjects to mark and
  destroy, a frame-rate-scaled hitch every 61 s. Memory grows by roughly 0.5-1 KB per entry [inferred].
- The nav-purge-nofence patch removes the fences, not the growth.

### 3b. Grain Grinder pull (the Scarecrow Fields obstacle, probably) [asset + eboot]

- `Maps/Level_05_SF/BP/BP_GrainGrinder` `Update(DeltaTime)`, called from ReceiveTick: `Multiplier = clamp(Multiplier
  - 2 x dt, 0.5, 1.25)` (dt-correct). Then, while Dan is in the inner / outer capsule:
  `Dan.SetInvoluntaryMotion(ToBlades x InnerAttractionForce x Multiplier, 0.0)` (or the outer force). **No dt in
  the impulse.** Instance in `Level_05_SF_Gameplay.umap` (export 1100): InnerAttractionForce 4, OuterAttractionForce
  2, rotor 140 deg/s. Each of the 8 `BP_GrainGrinderCannon` instances sets the grinder's `Multiplier = 1.0` every tick
  while Dan is in its push area and the cannon is firing.
- Chain: `BP_EntBase.SetInvoluntaryMotion` multiplies by 0.9525 x 30 and calls `SetInvoluntaryMove(..., duration
  0)`. The native tick adds the impulse x 16/14 (Dan's CustomMass 14) to the slide velocity once [eboot 0x653b10].
  Per tick: outer ring at Multiplier 0.5 adds +32.7 cm/s, the inner ring +65.3 cm/s. Deceleration removes
  2571.75 / FPS cm/s per tick on the ground [eboot 0x6510e0, constant from ctor 0x6616f0].

  | FPS | decel per tick | outer ring net per tick | inner ring net per tick |
  |---|---|---|---|
  | 30 (PS4) | 85.7 | none (no build-up) | none |
  | 60 | 42.9 | none | +22 (builds) |
  | 79 | 32.6 | threshold | builds |
  | 144 | 17.9 | +14.8 -> 2130 cm/s^2 | +47 |
  | 237 | 10.9 | +21.8 -> 5170 cm/s^2 | +54 |

  Once it builds up, the slide reaches MaxSlideSpeed 1828.8 cm/s, more than Dan's 812 cm/s walk. The design
  (30 FPS) only tugs at Dan [inferred].
- Other per-tick callers of `SetInvoluntaryMotion` with duration 0 (`involuntary_audit.txt`; non-forced calls
  during a duration > 0 are refused natively, so the 0.5 s callers are safe): `Interactive_Book_Pedestal.RepelDan`
  (every tick while Dan stands on a book pedestal; RepelForce 1.5; 28 pedestal references in the Sleeping Village
  gameplay map), `Level_10_IA/Slide_Pusher.RepelDan`, and probably `Level_20_TD/BP_RotatingWheels` and
  `BP_TD_SpinningDevice` (call sites in their ubergraphs, not traced to the tick).

### 3c. Pushables slow down above 66.7 FPS [asset]

`BP_SplinePushableObjectComponent.UpdateMovement` (01.02 bytecode m0x0068-m0x02d6): `Velocity *= (0.0 - clamp(Friction x dt,
0, 1))`, then `Velocity += input x Acceleration x dt`, clamped to MaxSpeed, then the object moves by
Velocity x dt. The literal is a real 0.0 (`EX_FloatConst`), so the old velocity is almost discarded each frame and
the speed is about Acceleration x dt. Defaults (`BP_PushableComponentBase`, no overrides in SF / SV / HM / AG
instances): Friction 4, MaxSpeed 300, Acceleration 20000. Speed: 300 cm/s up to 66.7 FPS, 161 at 120, 135 at 144,
83 at 237. Timers (`MinTimeToEngagePush`, `MaxTimeBetweenHits`, `MaxNoInputTime`) are dt-correct.

### 3d. Dan stuck in walls (P19): no frame-rate cause found

Checked and frame-rate safe: Dan's knockback (`ProcessDamageInternal` -> `SetInvoluntaryMotion` once per hit, swept
through the native walking code); NPC locomotion (`AddMovementInput` / `MoveToLocation` / crowd, all through the
swept CMC); NPC `LaunchCharacter` jumps. Unswept teleports of Dan exist only in `UpdateDeathLift`,
`AttachToComponent`, `ScriptedMovement` (sweep on) and SF's `BP_MovingBricks` (it moves Dan to the brick face
without a sweep while the bricks advance). Not analysed: the native handling of Dan's extra capsules
(`RegisterCustomColliders`, `UOBCapsuleComponent`), and engine-side penetration resolution between an NPC capsule
and a wall. Relevant cvars exist (`p.PenetrationPullbackDistance` 0x3df13a2, `p.PenetrationOverlapCheckInflation`
0x3df14aa, `p.InitialOverlapTolerance` 0x3df1a0a, `p.MoveIgnoreFirstBlockingOverlap` 0x3df10dc,
`p.EncroachEpsilon` 0x3e15974, `p.CharacterStuckWarningPeriod` 0x3deec00), but toggling them without a repro
is the toggle bisect the user's rules reject.

[UE 4.21] Known CMC frame-rate traps, none confirmed here: per-frame impulses to physics bodies on contact
(`InitialPushForceFactor`, Dan 50, applied each frame while the body is at rest, so physics props get pushed
harder); `MaxSimulationTimeStep` only splits large steps (it does not help small ones); per-call depenetration
limits act more often per second at high FPS (harmless in itself).

## 4. Ranked experiments (one change per launch, each needs the user's go)

| # | Change | Where / what to measure | Prediction | Risk |
|---|---|---|---|---|
| 1 | `GOW3_UE4_CVARS=gc.TimeBetweenPurgingPendingKillObjects=15`, nav-purge-nofence **off**, the Trolls Head spot at rest, then a fight | dev frame stats + a light sampling capture (game-thread ms/frame, samples at guest+0x2c94b9x), flip trace worst frames, wave count. First a baseline launch of 3+ min at rest without the change: does the purge share rise for ~61 s and drop at GC? | the purge share falls ~4x and the long waves shrink. A small GC hitch every 15 s instead of a large one every 61 s | low (restart-only cvar; GC cost per pass drops too) |
| 2 | Scarecrow Fields: ask the user which obstacle. If it is the Grain Grinder, cap at **72** and at **90** (two launches) | can Dan walk out of the outer ring? Time from the outer ring's edge to the blades without input | 72: like 60 (escapes). 90: pulled in. Threshold ~79 FPS | none |
| 3 | Sleeping Village fight, cap **120** vs the usual 237 (one launch each) | game-thread **ms per frame** (not only per second), stage A / B, 1% / 0.1% lows | if the costs scale with FPS (3a), game-thread ms per frame is lower at 120 than at 237, not equal. Equal per-frame time would mean per-tick work without FPS-proportional state | none |
| 4 | Pushable speed: push the SF hay bale (or a Sleeping Village pushable) a fixed distance at caps 60 and 237 | stopwatch / frame count | 300 vs ~83 cm/s (3.6x slower at 237) | none |
| 5 | `GOW3_UE4_CVARS=tick.AllowAsyncTickDispatch=1` (then `tick.AllowConcurrentTickQueue=1` separately), SV fight | game-thread ms/frame, crashes | less game-thread time in tick queueing (the lock-free list samples) | medium: experimental in 4.21, may crash or change tick order |
| 6 | Tooling for P19 (code, dev build only): a ring buffer of Dan's per-frame location, velocity (+0x104), slide (+0x780), pending impulse (+0x774 / +0x74e) and movement mode, dumped on a hotkey when stuck | the frames before he gets wedged: what moved him (slide, step-up, depenetration) | n/a: data | low (dev only, no periodic log) |
| 7 | Measurement only: `BP_AIActiveList`'s three `UpdateTickInterval(0.0f)` -> 1/60 (same-length edit) | game-thread ms/frame in a fight, to size the AI Blueprint share | AI share drops ~4x. **Not a fix**: `GetWorldDeltaSeconds` timers (invulnerability 0.167 s, flee / wait turns, ragdoll blend) would run ~4x slow at 237, and `MoveForward` input would arrive only on tick frames | medium (gameplay changes) |
| 8 | Crowd settings (needs the ue4_override.py section extension): `MaxAvoidedAgents=4`, `AvoidanceConfig` lower adaptive rings | game-thread / worker time in fights | small: detour crowd is cheap for ~10 agents [inferred] | low |

Fix candidates once confirmed (to propose and approve separately, not built):
- 3a: a shorter GC interval in the override pak (from experiment 1), plus the existing nofence patch. Larger
  fixes (throttling `CanNavToLocation` in the Blueprint, or purging every N frames) are bigger edits.
- 3b: a same-length bytecode edit in `BP_GrainGrinder.Update` that makes the impulse dt x 30-scaled. For example,
  swap `this.Multiplier` for the local `CallFunc_Multiply_FloatFloat_ReturnValue` (= DeltaTime x Decay, same
  operand size) and retune the `Decay` literal. That drops the cannon boost, so a proper rewrite is the better
  fix. The book pedestal / Slide_Pusher / TD devices get the same treatment after their tick paths are checked.
- 3c: a same-length edit in `UpdateMovement`: replace the `GetWorldDeltaSeconds` call that feeds the acceleration
  (m0x0270) with the constant 1/30 plus EX_Nothing padding. The pushable then moves at the PS4's 30 FPS speed
  (300 cm/s) at any frame rate.

## Open questions

1. Which Scarecrow Fields obstacle the user meant (the Grain Grinder is my inference from the code).
2. The real `ActivePaths` size over time (a dev probe reading the nav-data actor's +0x3e8 TArray count once per
   second in a diagnostic run, not a performance run), and whether level streaming in the Sleeping Village triggers
   GC more often than every 61.1 s.
3. The [UE 4.21] statements here were not checked against the engine source: `UNavigationPath` keeping its path
   until `BeginDestroy`, `CreatePathInstance` registering every path, `AAIController::MoveTo` re-pathing on every
   call, the tick-interval DeltaSeconds rule, and `UCrowdFollowingComponent::ApplyCrowdAgentPosition` being empty.
   The eboot could confirm each (the functions near 0x2c958c0 for paths).
4. P19: the native extra-capsule code (`RegisterCustomColliders`, UOBCharacterMovementComponent around
   0x650c30 / 0x6514d0) is not analysed.
5. 01.00 addresses for the native slide functions were not taken; the name strings are at different offsets in 01.00
   (e.g. `gc.TimeBetweenPurgingPendingKillObjects` 0x3e5d980, `MaxSimulationTimeStep` 0x3e79a18).
