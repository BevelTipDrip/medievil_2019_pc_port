# Implemented: draw reduction for the Deck (HZB size patch, N0 draw classes) (2026-11-04)

Status: built (Windows and Linux), not run. Branch `deck-lab`: `93d1a58` (1), `853877d` (2); item 3 not built
(section 3). Inputs: [study-ue4-draw-reduction.md](study-ue4-draw-reduction.md) (sections 3, 4, 6),
[study-native-render-paths.md](study-native-render-paths.md) (N0, N1), [study-heavy-frame-map.md](study-heavy-frame-map.md)
(pass shapes, the particle double run). Target: stage B (`gow3:DrawRec`) at ~91% with ~1,400 draws a frame in the heavy
graveyard view, 720p at 67%.

| # | Change | Switch | Default |
|---|---|---|---|
| 1 | Built-in patch **"Finer occlusion culling (low render resolutions, Steam Deck)"**: UE4's HZB at the scene's size | launcher > Patches, or `GOW3_HZB_FULL_SIZE=1` / `0` (extra-env.txt) | off |
| 1b | Patch lines may carry `Original="..."`: `patches.py` drops a built-in patch whose original bytes are not in the eboot | always | on |
| 1c | `scripts/ue4_override.py` comment: `r.Shadow.RadiusThreshold` culls small shadow casters, not lights | - | - |
| 2 | N0: "Draw classes per frame" line in the 5 s frame stats | `GOW3_FRAME_STATS=1` | with frame stats |
| 3 | Particle-sim dt=0 copy skip | not built | - |

## 1. Finer occlusion culling (`patches/MediEvil.xml`)

**What.** UE 4.21's `BuildHZB` makes the HZB's mip 0 `2^(ceil(log2 W) - 1)` by `2^(ceil(log2 H) - 1)` and stretches
the whole view over it: 512x256 at 858x482 (720p at 67%), 1024x512 at 1280x720. The patch turns the two `- 1` into
nops, so mip 0 is `2^ceil(log2)`: 1024x512 at 67% of 720p, the 100% run's pyramid. The occlusion test reads the
pyramid size from the view, so it stays consistent (study section 4). Restart only (a code patch).

| Build | width: `dec r14d` `41 ff ce` -> `nop dword [rax]` `0f 1f 00` | height: `dec eax` `ff c8` -> `xchg ax, ax` `66 90` |
|---|---|---|
| 01.00 | 0x15357ba (XML 0x019357ba) | 0x1535812 (XML 0x01935812) |
| 01.02 | 0x153843a (XML 0x0193843a) | 0x1538492 (XML 0x01938492) |

Checked read-only in both eboots (`prepare.parse_self`, session scratchpad `hzbcheck.py`): 01.00 sha256 `a7e976d1...`
(`Game Files\CUSA11227`) and 01.02 `fe2f88ba...` (`E:\MediEvil\CUSA11227-0102`) hold the original bytes at those
offsets, in `vroundss ..., 0xa; vcvttss2si r14d / eax; dec; test; cmovle` (the 16-byte windows around each site are
unique), and the 01.02 `out/eboot.elf` passes `original_mismatches`. The patch system already gates every built-in
patch on the eboot's sha256 (`patches.py` `GAMES`); `Original` adds a per-line check that also holds under
`GOW3_FORCE_PATCHES`, which skips the sha256 gate.

**Turn on.**
- Launcher: Patches > Game patches > "Finer occlusion culling (low render resolutions, Steam Deck)" (the patch's
  note is its tooltip). Off by default, and a saved selection keeps it off (not in `PATCHES_AFTER_SEEN_LIST`).
- Test settings (Deck): `GOW3_HZB_FULL_SIZE=1` in `~/.local/share/medievil-pc/extra-env.txt` adds it to the patch
  list whatever the launcher says; `GOW3_HZB_FULL_SIZE=0` removes it; unset, the launcher decides
  (`graphics_options.hzb_full_size_environment`, called by `run.py` after `output_environment`).
- The log shows it in `Patches: CUSA11227; N writes from [..., 'Finer occlusion culling ...']` (2 writes more than
  without it). A mismatch prints `Patches: <name>: the eboot has other bytes at its addresses: not applied`.

**Expected** (study 4, [I]): at 67% of 720p culling near the 100% run's: HZB hidden ~63 -> ~120 a frame (the "HZB
remap" line's `hidden`), draws ~1,430 -> ~1,290-1,310 (~120-145 fewer: base pass, prepass, velocity, shadows of hidden
objects), ~1-1.5 ms less stage B. GPU: the HZB build gets one more mip and 4x the mip-0 texels (small). Not for 1080p
and above: at 4K it would build a 4096x4096 pyramid for nothing. Risk: more culling means more exposure to the latent
(3 frames late) result pop-in already accepted; look for objects missing or popping when turning.

No-code cross-check of the mechanism: render scale 81% already gets the 1024x512 pyramid (1037x583).

## 2. N0: draw classes (`gpu/shim/gow3_draw_classes.h`)

With `GOW3_FRAME_STATS=1`, `DrawRecord` and `DrawIndirectRecord` time each recorded draw (rdtsc, the whole record
including its tasks) and class it by the render targets `PrepareRenderState` bound. Without frame stats a draw costs an
untaken branch and an inline call that returns at once. Printed after "Thread ids per frame":

```
Draw classes per frame: particle sim 194.0 (3.10 us), particle inject 1.0 (6.00 us), depth prepass 120.0 (...), ...,
total 1430.0; particle sim runs 1.40 (1.00 after an inject draw), sim ps 1234abcd5678ef00 193.0, sim ps ... 1.0
```
(numbers illustrative). Per class: draws per frame (flips) and stage B us per draw. Classes (first match):

| Class | Targets | Heavy-frame pass (frame map section 1) |
|---|---|---|
| no attachment | no colour, no depth | 5a: the 3-vertex draws before shadow maps |
| shadow depth | no colour, D16 | 5: per-object / light shadow maps |
| depth prepass | no colour, 32-bit depth, depth writes | 2: prepass |
| depth/stencil only | no colour, depth without depth writes | 3, 9b: occlusion boxes, projection stencil |
| display | colour target 0 is a display buffer | 11-12: tonemap, UI, present |
| particle sim | 2 colour, first RGBA32F, no depth | 1: simulation (and any tile clear into the same targets) |
| particle inject | 3+ colour, first RGBA32F, no depth | 1: inject |
| post | colour without depth | 0, 4, 11: post, HZB build, bloom, off-screen UI |
| base pass | 5+ colour with depth | 7: G-buffer |
| velocity | R16G16 with depth | 8 |
| scene colour | RGBA16F with depth | 9, 10: light volumes, translucency, particles, fog |
| other with depth | any other colour with depth | 9a, 6: shadow projection, decals |

Also: particle sim runs (consecutive sim draws) per frame, how many came after an inject draw since the previous run
(the gate for item 3), and the sim class's pixel shaders (first four guest hashes, draws per frame each: the
simulation shader, and any other shader drawing into the same targets). `research/tools/deck_summary.py` prints the
line whole per toggle state. Test: `tests/test_draw_classes.cpp` (`ninja -C out/gpu draw-classes-test`).

Limits: the shapes come from the PC 4K captures; at 67% on Low the formats are the same, but a class may also catch a
pass the frame map does not list (look at "other with depth" and "post" if they are large). Indirect draws are classed
the same way; dispatches are not counted.

## 3. GPU-particle dt=0 copy skip: not built

Asked: skip a sim draw when it is dt=0 with the same tile set two runs running and nothing else wrote its tiles (both
ping-pong buffers then hold the same data), default off, live switch on a free bit of the toggle file's second word.
Not shipped: its safety cannot be shown without a capture or more reverse engineering.

**The rule that would be safe** (run R reads A, writes B; run R-1 read B, wrote A). Skipping draw D (tiles T) of run R
is exact iff `A_T == B_T` before D. That holds when (a) D has dt = 0 (the shader is then an identity copy: the captures'
dt=0 tiles are bit-exact copies, frame map section 2), (b) run R-1 drew exactly T with dt = 0 (copied or itself
skipped: either way `A_T == B_T` after it), and (c) nothing else wrote A or B on T between them. By induction a skip
chain stays exact.

**What is missing:**
1. **dt on stage B.** The captures put the step dt in "binding 1 (per emitter) float [8]" (RenderDoc's binding numbers
   of the port's SPIR-V), not mapped to the guest V# the stage reads (`Shader::Info::buffers` index, byte offset, and
   whether it is the vertex or pixel stage). Guessing the buffer or offset reads the wrong float: a wrong "dt = 0"
   freezes or corrupts particles. Needs one capture (or a constant dump of the sim PS's buffers next to the RenderDoc
   view) on 01.02, and the same check on 01.00.
2. **Other writers per tile.** The inject draw writes the latest state targets for new particles' tiles (frame map:
   between run 1 and run 2), and UE 4.21 clears freed / newly allocated tiles. Inject tiles are only knowable by
   decoding the new-particle buffer (UE4's `FNewParticle` layout in this build), so (c) can only be checked globally:
   "no inject or other draw into the state targets since the previous run". If that resets every run, the skip never
   fires. The per-emitter argument that inject only hits emitters with dt > 0 in the adjacent run (spawning and dt both
   come from the emitter's tick) is plausible from the engine source but unchecked in the eboot
   (`SimulateGPUParticles` / the emitter tick at guest+0x2382e50, frame-cost study item 8: "a game hook needs Ghidra
   work").
3. **The sim shader's identity.** Shapes alone also catch tile clears; the skip must key on the simulation PS hash,
   which no doc records (P146812 is a RenderDoc pipeline id).

**What N0 measures for it** (no launch needed beyond the baseline): sim draws per frame and their stage B us (the
ceiling of the saving), runs per frame (1 or 2: `perf_particle_single_pass` off by default), runs after an inject
(if this equals the runs, a global inject reset leaves nothing to skip), and the sim class's PS hashes (the key for
item 3 or the N1 run replay).

**Next steps, if pursued:** (1) one RenderDoc capture on the PC of the graveyard at 01.02: map binding 1 to the PS
buffer index and confirm float [8] = byte 32; record the sim and clear PS hashes; (2) Ghidra on the emitter tick and
`SimulateGPUParticles` / inject ordering, or build it with the global inject reset first and count would-be skips in
a stats-only dry run (no draw skipped) before any real skip; (3) then the skip behind `GOW3_PARTICLE_SKIP_IDENTITY=1`
with second-word bit **64** to switch it off live (bits 1-32 are taken: bulk descriptor copy 1, sampler memo 2, ring
reuse 4, arena direct writes 8, semaphore poll interval 16, recorder lean 32; 64 is not consumed by anything yet).
The N1 run replay (study-native-render-paths) keeps the semantics and needs only item 3's PS hash: it may be the
better first pilot.

## Build and test

- Windows: `research/tools/build-clang64.sh` (LTO) clean, no warnings from the change.
- Linux: WSL Debian `/root/src/medievil` fast-forwarded to `853877d`, `CC=clang CXX=clang++ bash build.sh` (static GPU,
  LTO) + `packaging/linux/appimage.sh`: `dist/MediEvil-x86_64.AppImage` (126 MB, sha256 `7d4042de...`), `--help` works.
- `draw-classes-test` passes on both; `pipeline-binary-cache-test` passes (WSL).
- Unit tests (`py -3.14 -m unittest discover -s tests`): 127 run (3 new: the patch's writes for both builds, the
  `Original` check on a synthetic ELF, `hzb_full_size_environment`), the 8 known failures (2 failures, 6 errors).
- `patches.py` dry run against the 01.02 game folder with the patch selected: 15 writes (13 + 2).
- Not run in game.

## The Deck test (one change per launch, each with the user's go)

Settings as the previous Deck runs: 720p, 67%, Low preset, the same graveyard heavy spot, stand still ~60 s, drop the
first 5 s window. `deck.sh env` replaces the whole `extra-env.txt`: carry over any keys the current runs use.

1. **Deploy and baseline with N0** (this AppImage, patch off). `deck.sh deploy`; `deck.sh env "N0 baseline"
   GOW3_FRAME_STATS=1`; run; `deck.sh pull draw-red-0`; `deck.sh summary draw-red-0 --min-draws 1000`. Read the
   "Draw classes per frame" line: the class sizes replace study-ue4-draw-reduction section 1's estimates; note "HZB
   remap ... hidden" and draws/frame. This launch can also carry the existing toggle A/Bs (`deck.sh ab "0 0" "0 32"`):
   the patch is not involved.
2. **Patch on.** `deck.sh env "HZB full size" GOW3_FRAME_STATS=1 GOW3_HZB_FULL_SIZE=1`; check the log's `Patches:`
   line names the patch; same spot; `deck.sh pull draw-red-1`; `deck.sh summary draw-red-1 --min-draws 1000`.
   Compare with run 1: hidden (expect ~63 -> ~120), draws/frame (expect -120 to -145), which classes shrank (base
   pass, depth prepass, velocity, shadow depth), stage B busy, FPS, frame pacing p99. Screenshots or a look for missing
   or popping objects while turning (the patch is restart-only: no live toggle A/B).
3. If the change is within the spread of the 5 s windows: one more launch of 1 (A-B-A), or `render_scale=81`
   without the patch as the no-code cross-check (same pyramid).

Pass: fewer draws and lower stage B busy (or higher FPS) with no missing geometry. Item 3 has no test until it is built.
