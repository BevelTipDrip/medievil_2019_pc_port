# Study: native / HLE work to unload stage B (2026-10-20)

Status: read-only study. Nothing built, launched or committed. Parent: [START-HERE.md](START-HERE.md); context:
[plan-stage-b-split.md](plan-stage-b-split.md) (T-S1/S3/E1), [fix-inventory.md](fix-inventory.md).
Question (the user): reduce stage B's load by reverse engineering MediEvil and using native calls where possible
instead of translating generic PM4 per draw.

**Short answer.** Hooking the game higher up does not address stage B's cost. Stage B never decodes PM4: stage A
does, and stage A already waits on B 25-38% of the time. Stage B's ~1.7 us per draw goes into Vulkan-side
resource and state work (cache lookups, descriptors, buffer copies, render-state checks). A native RHI would still
have to do that work. The evidence points to three cheaper levers: (1) a "same as the previous draw" fast path
inside stage B, (2) stop copying static index and vertex buffers into the stream buffer on every draw, (3) fewer
draws on the game side (UE4 cvars, after a heavy-view frame map).

Tools and outputs (outside git): `research-data\ghidra\stageb\` (`py\eimg.py`, `gnmlib.py`, `callers.py`,
`relocs.py`: capstone scans of the eboot; `gnm-scan.txt`, `gnmlib-map.txt`, `rhi-vtable.txt`; `rd_*.py` +
`rd-*.txt`: RenderDoc analysis of the existing captures). Offsets are guest+0x (image offset). The Ghidra copy
`proj-stageb` was not needed in the end (the stock project is unanalysed, imports appear as NIDs only).

## 1. How the game builds GPU work

| Layer | Where | Evidence |
|---|---|---|
| UE4 GnmRHI command context (vtable, 97 slots) | vtable at guest+0x4ed6388; code guest+0x66c000-0x6a2000 | slots 76-84 = RHIDrawPrimitive 0x672880, DrawPrimitiveIndirect 0x672db0, DrawIndexedIndirect 0x6732f0, **DrawIndexedPrimitive 0x672fb0**, DrawIndexedPrimitiveIndirect 0x673560, Begin/EndDrawPrimitiveUP 0x6737d0/0x673960, Begin/EndDrawIndexedPrimitiveUP 0x673c70/0x673d20 (UE4 4.21 order; each calls the LCUE pre-draw, then the Gnm draw wrapper) |
| Gnmx LightweightConstantUpdateEngine | guest+0x2e88000-0x2e9b000 | **pre-draw guest+0x2e949d0** (0x10e0 bytes, called by all 8 draw entries): for each dirty stage it memcpys the resource tables into a fresh ring chunk (0x2e92650) and points user-data SGPRs at it (45 calls to setPointerInUserData, 16 to setUserDataRegion). The full CUE (CE RAM, WRITE_CONST_RAM, 0x2e8b5c0 etc.) is linked but has no direct callers |
| libSceGnm (static) | guest+0x3be5000-0x3bf3800 | all SET_SH_REG / SET_CONTEXT_REG / DMA_DATA / EVENT_WRITE_EOP packets are written here, inline: setPointerInUserData 0x3be9e70 (SET_SH_REG n=3, 65 call sites), setUserDataRegion 0x3be9f90 (NOP marker + SET_SH_REG, 27), setContextReg-style 0x3be7ee0/0x3be8040/0x3be80c0 (69/2), writeEventEop 0x3bead80 (45), waitRegMem 0x3beaf50 (30), CP-RAM write 0x3be5f30 (52, CUE only). About 100 functions hold PM4 header immediates (`gnmlib-map.txt`) |
| libSceGnmDriver imports | GOT guest+0x5450800-0x5450b60, PLT 0x3bf30b0-0x3bf3770 | 39 of 112 imports are called, **each from exactly one libSceGnm wrapper**: DrawIndex (wrapper 0x3be8d40), DrawIndexAuto (0x3be8bf0, 0x3be8c50), DrawIndexIndirect 0x3be8e60, DrawIndirect 0x3be8dc0, DispatchDirect 0x3be8f00, Set{Vs,Ps350,Es,Gs,Hs,Ls,CsWithModifier}Shader, Update{Vs,Ps350,Gs,Hs}Shader, SetEmbedded{Vs,Ps}Shader, Insert{Push,Pop,PushColor}Marker, InsertWaitFlipDone, Submit[AndFlip]CommandBuffers[ForWorkload], SubmitDone, DingDong, MapComputeQueue, ValidateDrawCommandBuffers (`gnm-scan.txt`) |

The port's HLE of those imports is shadPS4's `gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp`. It writes the
same PM4 into the game's buffer (sceGnmDrawIndex :455, DrawIndexAuto :476, SetPsShader :1690, SetVsShader :1825,
Submit :2348). Per draw, the PM4 the game hands over is: the LCUE's SET_SH_REG pointers to freshly copied
resource tables, the constant-buffer pointers (UE4 commits changed constants into new ring memory), context
registers for changed state, and the driver's DRAW_INDEX_2 / DRAW_INDEX_AUTO. Stage A decodes it (liverpool.cpp),
flattens the tables (DrawPrep workers) and copies small read-only constants into the constant ring
(`CollectRingBindings`, vk_rasterizer.cpp:508). **Stage B receives a register delta plus prepared sharps, not PM4**
(`RunDrawPacket` :708).

Conclusion for item 1: hooking the driver imports gains nothing (one wrapper each, and they only emit draws and
shaders). Hooking the LCUE or the RHI could replace stage A's decode, but stage A is not the limit (T-E1: "stage A
waited 29.1%", stage B 92.9% busy).

## 2. Hot patterns and redundancy (measured)

Heavy view, T-E1 log (`data/t-e1-eos-deferred.log`:1416-1430): 2,568-2,811 draws, 82 dispatches, 50 submits,
**~485 render passes ended per frame**, 429,588 packets/s at 138 FPS (~3,100 packets a frame). Stage B per packet:
2.14 us, of which draw 1.78: bind 0.61 (textures 0.35, buffers 0.18), vertex 0.33, descriptors 0.22, begin render
0.12, dynamic 0.08, index 0.075, render state 0.066, emit vertex 0.064, kick 0.035, 7.8 Record calls (0.093).
Memo rates: render state 82% hits, texture sets ~88-90% (misses ~18k per 262k draws, nearly all "other key", i.e.
new sets, not conflicts: the table has 32,768 slots, vk_rasterizer.h:309).

Two light-view captures (RenderDoc, `rd-v4-shadow_frame17750.txt`, `rd-v1-shadow_frame24679.txt`; 702 / 919
draws). These are the state calls stage B recorded in front of each draw, and how often each one exactly repeats
the previous call of its kind:

| call | per draw | repeats previous |
|---|---|---|
| vkCmdSetVertexInputEXT | ~1 | **96%** |
| vkCmdPushConstants | ~1 | **86.5%** |
| vkCmdBindPipeline | 1 | **73-74%** |
| vkCmdPushDescriptorSetKHR | ~1 | 0%, but **79-83% identical apart from buffer offsets/ranges** (constant-ring offsets) |
| vkCmdBindVertexBuffers | ~1 | 10-14% |
| vkCmdBindIndexBuffer | ~1 | 1-5% |

The vertex and index binds almost never repeat because **both point into the stream buffer at a new offset every
draw** (diff dumps: `pBuffers=R219`, offsets +640 bytes per draw). `BufferCache::ObtainBuffer` copies every
read-only buffer of 16 KiB or less that the GPU has not modified into the stream buffer, on every use, whether or
not its pages are CPU-dirty (buffer_cache.cpp:776, `STREAM_THRESHOLD = 16_KB` buffer_cache.h:51). That covers small
static meshes and the shared GPU-particle index buffer. T-U1 (`data/t-u1-uploads.log`, "Buffer stats") measured
**~640k stream copies/s, ~2.0 GB/s** at ~400k draws/s: ~1.6 copies and ~5 KB per draw. Each one is a
`SmallGuestCopy` → `RecordHostCopy` run by the recorder in command order, and each one counts in the host-copy
waits.

Pass mix (light captures): **388 of 702-919 draws (42-55%) are UE4 GPU-particle simulation**: 1024x1024
R32G32B32A32 x2 targets, 48 indices (8 tiles of 2 triangles), one pipeline (VS ResourceId 11020 / PS 11010), in
**two identical runs of 194 draws**. The per-draw constants of run 2 equal run 1's (draws 108/1469, 115/1476:
`rd-24679-consts*.txt`). Inside a run only the tile-offset vertex buffer, the stream index copy and the ring
constants change. Next come the depth-only 3840x2160 draws (89-195, the prepass), the G-buffer (37-128, 6 targets)
and 512x512 shadow depth (25-30). The heavy view's mix is unknown: the existing captures are light views.

What is redundant and what each needs:

| Redundancy | Needs game knowledge? |
|---|---|
| bindPipeline / pushConstants / setVertexInput identical to the previous draw (74-96%) | no: compare in stage B |
| same textures, samplers, targets and render state as the previous draw (pipeline equal in 74%; descriptors equal except buffer offsets in ~80%) | no: `prepared->image_hashes`, `BeginSignature` and the regs delta already identify it |
| static small index/vertex buffers re-copied every draw | no: "pages not CPU-dirty since the last sync" is already in the memory tracker. Game side would only confirm (`RHICreate*Buffer` BUF_Static) |
| second GPU-particle run (194 draws/frame) | **yes**: why UE4 simulates twice per frame (see H3) |
| depth prepass draws | game cvar (`r.EarlyZPass`) |

## 3. Candidates

### Inside stage B (no game hooks)

**S1. Delta-draw fast path.** If a draw's pipeline, prepared image hashes of every stage, `BeginSignature`
(targets, generation) and the dirty register blocks outside user data match the previous draw: reuse the previous
`image_infos`, sampler writes, `RenderState`, vertex attribute/binding descriptions and dynamic state. Rebuild only
the buffer bindings (ring offsets, ObtainBuffer for vertex/index) and push. Skip recording bindPipeline,
pushConstants and setVertexInputEXT when their bytes equal the last ones (vk_rasterizer.cpp:1197,
vk_pipeline_common.cpp:26-33, :1760). This saves part of textures (memo-hit cost ~0.1-0.15), render state + begin
render (~0.1), dynamic (~0.06), vertex input copies (~0.03-0.05) and ~2.5 Record calls (~0.03) on ~75% of draws:
**~0.25-0.3 us per packet**. Correctness rests on the existing memo invalidation (registry generation, image
`needs_rebind`/`is_target`, scene generation). It is the T-S1 "no dominant part" cost removed for repeated state,
and it is what UE4 4.21's drawing policies produce (SetSharedState once, then per-mesh constants: the 744x5,
8232x3, 23814x3 index-count runs in `frame24679-actions-5227-6465.txt`).

**S2. Stream only what needs uploading.** In `ObtainBuffer` (buffer_cache.cpp:776): if the range is not
CPU-modified in the tracker, the arena already holds it, so bind the arena with no copy and no render-pass break.
Stream only CPU-dirty ranges (UE4 dynamic vertex buffers: UI, sprites). Add an (addr, size) → (arena, offset)
memo validated by the L2 region-bucket generation, so the arena path costs about as little as the stream reserve.
Stage B saves ~0.05-0.1 us per packet (1.6 fewer SmallGuestCopy/RecordHostCopy per draw). The recorder saves ~2 GB/s
of memcpy (~0.4-0.5 us of recorder time per draw), fewer host copies are pending at fences, and vertex/index binds
start repeating (feeds S1). It matters more once option (c) moves work to the recorder. Risk: pages written by the
CPU without the tracker seeing it (the same assumption every arena binding already makes).

**S3. Split descriptors: cached image set + pushed buffers.** Images and samplers go into a regular descriptor set
cached per texture-set memo entry (bound with one bindDescriptorSets); only buffers are pushed. Descriptors
0.22 → ~0.08 us, and the recorder's push work shrinks. This needs a two-set pipeline layout in the shader
recompiler's binding scheme. The alternative, pushing only changed bindings, needs the incremental push semantics
checked against the Vulkan spec. `PushData::buf_offsets` is u8 (resource.h:212), so ring offsets cannot move into
push constants.

**S4. ObtainBuffer memo for large static vertex buffers** (arena path: ResolveReadbacks, GetArena,
EnsureResident, SynchronizeMemory per call): covered by S2's memo. About 0.05-0.1 us per packet on its own.

### Game side (HLE / UE4 knowledge)

**H1. Native RHI** (replace the GnmRHI vtable at 0x4ed6388 with Vulkan calls). It would remove stage A and the
PM4 round trip, but stage A is not the limit. Stage B's work (resources, descriptors, passes) stays: UE4's own PC
Vulkan RHI spends ~1-2 us per draw. It would also need UE4's FGnm* object layouts reverse engineered and the
port's whole memory/texture model rebuilt. **Not recommended**: huge effort, high risk, gain unclear.

**H2. LCUE hook** (guest+0x2e949d0: per-stage dirty masks): tells stage B which stages' resources changed. S1
derives the same from the prepared hashes. Adds nothing.

**H3. GPU-particle double run** (194 draws per frame in the light captures, 21% of draws there). It is not the
fixed-step loop: `r.GPUParticle.FixDeltaSeconds` (default 1/30, read at guest+0x2383a36, iteration code
0x2383b93-0x2383c58, `MaxNumIterations` default 3) gives floor(accumulated dt / (1/30)) iterations, so at most one
per frame above 30 FPS, and only for emitters with flag 8. The runs are identical, so the likely cause is a second
scene render (scene capture / reflection view family) or a second simulation phase. Next: a heavy-view capture,
then find what renders between the runs (eid 1450 in frame24679: one 4-target 1024x1024 draw). If it is a second
view family, a cvar or a one-byte patch could skip the second simulation. Possible gain is 194 draws per frame
(~7% of the heavy view). Visual risk: particles may tick differently.

**H4. Draw-count cvars.** `r.EarlyZPass` (default 3, registered at guest+0x119a778; strings at guest+0x3d045f4) and
`r.EarlyZPassMovable`. The prepass is 89-195 draws in the light captures. Setting 0 or 1 removes most of it with no
visual change, but raises GPU base-pass overdraw at 4K (GPU already at 70-90%). `r.DBuffer` forces a full prepass,
so check the game's value first (guest+0x3c6b784). Shadow cvars (`r.Shadow.*`) are a quality trade the port already
exposes. The gain depends on the heavy-view mix: 5-20% if the prepass share holds.

**H5. Shader recognition / pre-translated pipelines / skipping fixed fullscreen passes.** Pipelines are already
cached and picked on stage A and the DrawPrep workers. There are ~50-100 fullscreen and compute passes per frame
against ~2,700 draws: **<3% of stage B**, so not worth it.

## 4. Ranking

Conversion: the heavy view is ~3,100 packets a frame at ~130 FPS (7.4-7.7 ms), stage B 92-94% busy. Saving x us per
packet takes ~3.1·x ms off the frame while stage B stays the limit. **GPU ceiling:** the GPU runs 70-90% in heavy
spots, so it caps at ~145-150 FPS there, and gains above ~12-15% show up as better 1% lows, not higher averages.

| # | Item | us/packet saved | Heavy-view FPS (from ~130) | Effort / risk | Confidence |
|---|---|---|---|---|---|
| 1 | S1 delta-draw fast path (+ skip redundant pipeline / push-constant / vertex-input records) | 0.25-0.3 | +11-14% (~145, GPU cap) | medium / medium (memo invalidation) | medium |
| 2 | S2 stream only CPU-dirty buffers + ObtainBuffer memo (covers S4) | 0.1-0.15 (+ recorder −2 GB/s) | +4-7% | low-medium / low-medium | medium |
| 3 | H4 `r.EarlyZPass` A/B (cvar only, one launch) | draw count −5-20% (depends on heavy-view mix) | +0-15%, GPU cost up | trivial / low (revertible) | low until the heavy view is mapped |
| 4 | S3 split descriptor sets | ~0.1-0.14 | +4-6% | medium-high / medium (layout change in the recompiler) | medium |
| 5 | H3 second GPU-particle run | up to −194 draws per frame | up to +7% | unknown until the cause is found | low |
| 6 | H5 known passes / shader hashes | <0.05 | <2% | medium | high (that it is small) |
| 7 | H1 native RHI / H2 LCUE hook | stage A only | ~0 now | very high / high | high (that it does not pay) |

Suggested order: one heavy-view capture for a frame map (`GOW3_CAPTURE_TRIGGER` or RenderDoc; a launch, needs the
user's go), then run `rd_redundancy.py` on it to confirm the heavy-view repeat rates and pass mix. Then S2 (smallest
change, fewer stream copies feed S1), then S1, measured with `GOW3_SECTIONS=1` against T-E1. H4 is a one-launch A/B
whenever convenient. S1 + S2 together (~0.35-0.45 us of ~2.05) are close to what option (c) expected, without a
second thread.
