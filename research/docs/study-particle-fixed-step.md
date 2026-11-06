# Study: the single GPU particle pass without the torch / crypt floor bug (2026-11-05)

Offline (no launch). Game builds 01.00 and 01.02 (every address below is 01.00; 01.02 is the same code at +0x2dd0,
its data at +0x40, its constant pool at +0x30c0, each site checked byte by byte). Tools and listings:
scratchpad `particle\` (Capstone `dis.py` / `ripref.py` / `cmp.py`, Ghidra headless `ghidra.sh` on `proj-hzb`
read-only, `survey_fixdt.py` over the paks). Follows [study-heavy-frame-map.md](study-heavy-frame-map.md) §2 (H3) and
[plan-0102.md](plan-0102.md) T-0102-4; Deck numbers from [plan-linux-deck.md](plan-linux-deck.md) "Particle 1".

## Answer

1. **What `r.GPUParticle.FixDeltaSeconds` does** [code]. The emitter's per-frame data (game thread) carries two
   (step, count) pairs, "fixed" and "variable". With the default 1/30 s, emitters whose *local vector field* asks for
   a fixed step (`bUseFixDT`, flag 8; 12 of 174 GPU systems: ambient sparkles, leaves, fluff) are stepped in 1/30 s
   steps, the leftover time carried to the next frame and shown by a look-ahead step; every other emitter gets one
   step of the frame time in the "fixed" pair and nothing in the "variable" one. The render thread then runs the
   fixed pass (run 1: previous pair -> current pair) and the variable pass (run 2: current -> previous, the look-ahead,
   a plain copy for everyone else), and draws, sorts and bounds from the previous pair (run 2's output). With 0 the
   branch is skipped: every emitter gets one step of the frame time, one pass writes the current pair, and everything
   reads the current pair. **Both are consistent in the stock code.**
2. **The torches are not flag-8 emitters** [code + assets]: the torch blueprint's three systems (`vfx_fireTorch`,
   `vfx_smokeTorch`, `PS_Torch_Flame`) have no vector field module. For them the single pass changes neither the
   step (frame time), nor the step count (1), nor the parameters (the same fields, moved from one slot to the other):
   **only which buffer holds the state and that the copy (run 2) is gone.** A slow torch therefore cannot come from
   time-step maths; it is a **lost update in the ping-pong** [inferred]: with one pass the simulation reads the pair
   written one simulation ago, so a state flip without a simulation (or a simulation without a flip) makes it read
   an older state and lose a step; the default's copy leaves both pairs equal after every frame and is immune.
   Frame-rate independence (same at a 60 FPS lock and at 79 FPS on the Deck, 120-240 on the PC) fits this and
   argues against a dt effect. Which event loses the step is **not identified**: candidates are a scene render that
   flips without simulating (planar reflections / scene captures exist in Dan's crypt and the graveyards), or a port
   side read of a stale pair. The view dependence ("slow while the other torch is far, normal as Dan nears it") fits
   an extra scene render that comes and goes with the camera.
3. **The crypt floor ember / golden stream** is in `PS_air_sparkles` (placed in Dan's crypt; a flag-8 emitter with a
   5000-intensity local vector field) [assets; the link is inferred]: with 0 it also loses its 1/30 s steps.
4. **The change, three parts, all default off** (section "The change"):
   - patch **"GPU particle fixed step in the single pass"** (`GOW3_PARTICLE_SINGLE_FIXED=1`): with the single pass
     on, the flag-8 emitters keep their 1/30 s steps (no look-ahead: they move in 1/30 s steps), everyone else as
     before. With the cvar at 1/30 the game is exactly stock, so the live switch (menu, toggle file 8192) still A/Bs
     against the stock game;
   - **flip guard** (`GOW3_PARTICLE_FLIP_GUARD=1`, live off: toggle file second word **16384**): exactly one state
     flip per main-phase simulation, which removes the lost-update hazard if it is a flip/simulation mismatch, and
     counts the mismatches either way ("GPU particle flips" log line);
   - **diagnostic** (`GOW3_FRAME_STATS=1`): "Draw classes" now also gives the simulation runs per frame that write
     the same target as the run before ("on the previous run's target": 0.00 with a healthy single pass).
   Whether the guard fixes the torch is **unverified**: if the next test shows mismatches at the slow torch and the
   torch at normal speed with the guard on, the saving is kept with correct torches; if it shows none, the lost
   update is on the port side and the next step is a capture (below).

## Evidence

### The emitter's per-frame data (game thread) [code]

`FGPUSpriteParticleEmitterInstance` per-frame data (`GetDynamicData` in UE 4.21 terms, 0x2382e50, vtable slot
0x50db860; called with the instance in r14, the new dynamic data in r13). Tick (0x2381fa0) stores the tick's
DeltaSeconds at instance+0x270 (a store, not a sum); the per-frame data consumes it and zeroes it (0x2383d7f).

| Site | What |
|---|---|
| 0x2383a36-0x2383af5 | xmm5 = FixDeltaSeconds (`[ptr + 4*(not game thread)]`, cvar value pointer 0x5c14d28), xmm6 = FixTolerance (0x5c14d40, 0.1), eax = MaxNumIterations (0x5c14d58, 3) |
| 0x2383af9-0x2383b37 | +0x108 DeltaSecondsInFix = FixDT, +0x10c NumIterationsInFix = 0, +0x110 DeltaSecondsInVar = tick dt + carry (+0x270 + +0x274), +0x114 NumIterationsInVar = 1, carry = 0 |
| 0x2383b42 | FixDT <= 0: done (one variable step of the frame time) |
| 0x2383b4c-0x2383b8e | flag 8 clear (`[Simulation(+0x1d8) + 0x1cc] & 8`): swap the pairs: fixed = (frame time, 1), variable = (1/30, 0) |
| 0x2383b93-0x2383c58 | flag 8 set: n = floor(T / FixDT); near a whole step (tolerance) the variable pair is 0 and nothing is carried; else the remainder is the variable (look-ahead) step **and** carried to +0x274 for the next frame; n capped by MaxNumIterations, the fixed step stretched to T / n |

Flag 8 is `LocalVectorField.bUseFixDT`: the render thread copies the emitter info's local vector field into the
simulation at 0x23880e0 (intensity -> +0x1c4, tightness -> +0x1c0, flags bits 1/2/4/8 -> +0x1cc, the first three
being bTileX/Y/Z, tested as such by the command builder at 0x2367b34-0x2367b92).

### The simulation (render thread) [code]

| Function | Site | What |
|---|---|---|
| FFXSystem::PreInitViews 0x230dda0 | 0x230de02 | tail jump to AdvanceGPUParticleFrame 0x23667e0: `FrameIndex ^= 1` (ParticleSimulationResources+0x1f8); its only caller |
| FFXSystem::PreRender 0x230de80 | 0x230df42 | SimulateGPUParticles(main phase) (+ a distance-field phase at 0x230dfb1); PostRenderOpaque 0x230dfe0: depth-buffer phase 0x230e0aa, then sorting 0x2366830 |
| SimulateGPUParticles 0x2366cb0 | 0x2366d6d | the render thread's value read once (kept at [rsp+0x34]) |
| | command loop, reset at 0x23680b3 | one command per simulation of the phase with tiles (+0x38) and enabled (+0x22b); it copies the simulation's per-frame data (+0x50..+0x8c) and then **resets its step fields** (+0x78..+0x88): a simulation without new data this frame gets dt 0 (the dt=0 tiles of the captures) |
| | 0x23684da / 0x236851f / 0x236855b | first ExecuteSimulationCommands (three collision variants 0x2361e30 / 0x2362270 / 0x23626b0) with bUseFixDT = (FixDT > 0), into the current pair; then tile clears and new particles into the current pair |
| | 0x2368e19 | second pass only if FixDT > 0 and any command: bUseFixDT 0, into the previous pair |
| ExecuteSimulationCommands | 0x2361ed8-0x2361efc | input pair = ((FixDT <= 0) or bUseFixDT) xor FrameIndex: run 1 reads the previous pair, run 2 the current |
| per-command parameters 0x238b540 (and 0x238cd70, 0x238de30) | 0x238b6ef-0x238b764 | DeltaSeconds / NumIterations from the fixed or the variable pair by bUseFixDT; the position offset zeroed in the variable pass when FixDT > 0 |
| readers of the result | 0x236686e (sort), 0x2366bad (FinalizeGPUSimulation), 0x236b27a, 0x2388708 (sprite vertex factory) | pair = (FixDT > 0) xor FrameIndex: the previous pair (run 2's output) with 1/30, the current with 0 |

So run 2 with 0 iterations is the identity the captures saw (bit-exact copy, dt 1/30 = the swapped variable step
with count 0), and every reader agrees with the writer in both modes. Twelve code sites read the cvar's value
pointer (0x5c14d28; `ripref.py`), all in the tables above: the per-frame data, SimulateGPUParticles, the three
ExecuteSimulationCommands variants, the three per-command setters and the four readers.

### Which emitters have flag 8 [assets]

`survey_fixdt.py` (the paks' cooked properties): 174 GPU sprite systems, 12 with a local vector field whose
`bUseFixDT` is set (`EmitterInfo.LocalVectorField.bUseFixDT = True`; default false): `PS_air_sparkles`,
`PS_air_sparkles_02`, `PS_HM_TombSparkles`, `PS_air_fluff`, `PS_SF_air_fluff`, `PS_GY1_leaves`, `PS_GY1_leaves_2`,
`PS_GY2_fluff`, `PS_GY2_leaves`, `PS_MM_fluff`, `PS_MM_leaves`, `PS_MM_leaves_float`. Dan's crypt
(`00_DC_MAIN_Lighting`) places `PS_air_sparkles` (also 7 other levels); GY1 and GY2 the leaves. The torch
(`Torchlight_Blueprint`: `vfx_fireTorch`, `vfx_smokeTorch`, `PS_Torch_Flame`; GY2's `PS_Torch_Flame_Blueprint`):
no vector field module in any emitter.

### Measurements [log]

- Deck, `data/deck-particle-1.log`, uncapped 67%: single pass 79.4 vs 77.1 FPS median, 1,122 vs 1,316 draws, sim
  runs 1.00 vs 1.98 a frame, stage B 86.3 vs 89.1%. The torch and the floor glitch with the single pass only.
- Deck, the user (2026-11-05, after `f10bac9`): **at a 60 FPS lock the same** as uncapped ~79 FPS; on the PC seen at
  120-240 FPS. The bug does not grow with the frame rate: the flag-8 step change (1/30 -> 1/60..1/240) would; a lost
  update does not.
- `data/t-0102-4.log` (PC): 1/30 back, torch and floor right.

### Why the copy pass makes the default immune [inferred from the code above]

After each frame with 1/30 both pairs hold the newest state (run 1 writes it, run 2 copies it). An extra flip only
changes which equal pair is read; an extra simulation (dt 0 after the reset) copies the newest state forward. With 0
the pairs hold the last two states: an extra flip, or an extra simulation without its flip, makes the next simulation
start from the older one, and the step in between is lost: motion at a fraction of its speed, the more such events,
the slower, whatever the frame rate.

## The change

| Part | Switch | Files | Log |
|---|---|---|---|
| Patch "GPU particle fixed step in the single pass" | `GOW3_PARTICLE_SINGLE_FIXED=1` at start (run.py, extra-env.txt) or the launcher's Patches page; off by default | `patches/MediEvil.xml`, `scripts/graphics_options.py`, `run.py` | "Patches: ... 'GPU particle fixed step in the single pass'" |
| Flip guard | `GOW3_PARTICLE_FLIP_GUARD=1` at start; toggle file second word **16384**: guard off, live | `src/particle_hook.h`, `src/probe.c`, `src/runtime_extra.c`, `gpu/shim/gow3_toggles.h` | "GPU particle flip guard: PreInitViews and PreRender hooks ready (game build ...)", then every 1,200 simulations and at each switch "GPU particle flips: N flips, M simulations; X flips without a simulation (skipped / kept), Y simulations without a flip (flip added / none added); guard on / off" |
| Diagnostic | `GOW3_FRAME_STATS=1` | `gpu/shim/gow3_draw_classes.h`, `vk_rasterizer.cpp` | "particle sim runs R (I after an inject draw, S on the previous run's target)" |

**The patch** (5 writes per build, `Original` bytes checked, both builds verified by applying them and
disassembling):

| 01.00 / 01.02 | Bytes | Effect |
|---|---|---|
| 0x2383a36 / 0x2386806 | `mov rbx, [rip -> cvar value]` -> `lea rbx, [rip -> 0x3e2461c / 0x3e276dc]` (the registered 1/30) | the game thread always plans 1/30 s steps |
| 0x2383a5c / 0x238682c | `vmovss xmm5, [rbx + rcx*4]` -> `vmovss xmm5, [rbx]; nop` | the same constant on either thread slot |
| 0x23684da, 0x236851f, 0x236855b / 0x236b2aa, 0x236b2ef, 0x236b32b | `seta al` -> `mov al, 1; nop` | the first pass is the fixed-step pass whatever the cvar |

With the cvar at 0 (single pass): the fixed pair is used (flag-8 emitters keep their 1/30 s steps, everyone else
gets the frame time as before), there is no second pass, and the readers take the current pair, which that pass
wrote. With the cvar at 1/30 every patched value equals the stock one: stock behaviour. What is lost: the look-ahead
of the 12 flag-8 systems (they move in 1/30 s steps, visible as a slight judder above 30 FPS on fast sparkles).
The patch does not touch the torches (no flag 8): for them it is the single pass as before.

**The flip guard.** Two rel32 redirections to host stubs (signatures and targets checked per build; the stubs keep
the argument registers):
- PreInitViews' `jmp AdvanceGPUParticleFrame` (0x230de02 / 0x2310bd2): the flip happens only if a main-phase
  simulation ran since the previous flip (else skipped and counted);
- PreRender's `call UpdateMultiGPUResources` (0x230df0a / 0x2310cda, before PrepareGPUSimulation and the main
  phase): if a simulation already ran since the last flip, AdvanceGPUParticleFrame is called first (counted).
When flips and simulations alternate (the normal case) both hooks change nothing; with the default 1/30 they change
nothing that matters (both pairs are equal). Live off (16384): the game's flips as they come, still counted. One
FX system per slot, 8 slots (more: no guard for the extra ones). Risk: a flip skipped where the game needed one would
show as the same symptom (frozen particles), and the log counts it; the hooks are on the render thread only.

**Risk / failure signs.** Patch: particles frozen or exploding with the single pass (a wrong pair or step), or any
change with the cvar at 1/30 (should be none). Guard: particles that freeze or jump right after a level load or
around scene captures; "skipped"/"flip added" counts that keep growing in plain gameplay.

**Not done:** a host-side skip of run 2 (option b): it would have to prove the copy per draw (dt and count are in
the per-emitter constants, not mapped on stage B: impl-draw-reduction.md section 3) and keep the parity; the guest
side above needs none of it.

## Test (Deck or PC, needs the user's go)

One launch: `GOW3_FRAME_STATS=1 GOW3_PARTICLE_FLIP_GUARD=1 GOW3_PARTICLE_SINGLE_FIXED=1`, in-game menu "Single GPU
particle pass" on. Check the start lines: "GPU particle flip guard: ... hooks ready", "Patches: ... 'GPU particle
fixed step in the single pass'". At a lone slow torch (graveyard) and the crypt floor spot, toggle file second word,
30 s each:

| Second word | Expect |
|---|---|
| `16384` (single pass, guard off) | the bug as before; "GPU particle flips" gives the unpaired counts; Draw classes "on the previous run's target" > 0 if a flip went missing or doubled |
| `0` (single pass, guard on) | torch at normal speed and floor right if the counts above were not 0; sim runs 1.00, ~193 fewer draws than `8192` |
| `8192` (the stock 1/30 step) | reference: right, sim runs ~2, "on the previous run's target" ~1.00 |

Reading it: unpaired flips/simulations at the slow torch and a normal torch at `0` -> fixed (then the flag-8
emitters' look-ahead loss is the only visual cost; compare the crypt sparkles at `0` and `8192`). No unpaired counts
and "on the previous run's target" 0.00 with a slow torch -> the lost update is on the port side (a stale pair read):
next, a RenderDoc capture at the slow torch with the single pass, checking that the simulation's input textures are
the previous run's output (the frame map's H3 method).

## Unverified

- Which event loses the torch's steps (no launch). The scene-render explanation is a hypothesis; the tests above
  separate it from a port-side cause.
- That the floor ember / golden stream is `PS_air_sparkles` (its flag 8 and placement are certain; the visual link is
  not).
- UE 4.21 names (`DeltaSecondsInFix`, `GetVisualizeStateTextures`, `AdvanceGPUParticleFrame`, ...) are recalled and
  matched to the code's behaviour, not checked against the 4.21 source text.
- The patch and the guard are checked statically only (bytes, targets, stub encodings, unit tests); not run.
