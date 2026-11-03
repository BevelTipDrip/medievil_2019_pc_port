# Study: what the MGS2 temporal work gives MediEvil (UE4), and a revised DLSS / FSR / FG plan

Status: research 2026-11-03, read only. Nothing built, launched or committed. Parent: [START-HERE.md](START-HERE.md).
Read with [upscaler-reference.md](upscaler-reference.md), [plan-ue4-motion-vectors.md](plan-ue4-motion-vectors.md),
[plan-dlss.md](plan-dlss.md), [plan-fsr-frame-generation.md](plan-fsr-frame-generation.md). MGS2 sources:
`C:\Users\Ty\MGS2-Lab\MGSHDFix` (read only; called `MGS2:` below, paths under `src\features\mgs2_lab\` unless given).

Labels used here (the repo's "verified" stays reserved for the user in game):
- **checked**: read in this port's code, the game's own files (01.02 paks, `out\eboot.elf`) or the existing RenderDoc
  capture `research-data\renderdoc\heavy_frame41595.rdc` (2026-10-21, 4K, render scale 100, heavy view).
- **inferred**: from Unreal Engine 4.21 behaviour or from MGS2 by analogy, not checked against this game.

## 1. The answer in short

- **MediEvil does not need MGS2's hardest work.** MGS2 had no motion vectors and no jitter, so the team built camera
  reprojection, a per-draw object matcher with engine object keys, a DXBC shader patcher for in-draw vectors, and their own
  projection jitter. UE 4.21 already does all of it: a velocity pass for moving and skinned objects, `ClipToPrevClip` for
  everything else, and its own jitter in the View uniform buffer. The port only has to **read** these (one small compute
  pass to combine them), not generate them.
- **What transfers from MGS2 is the back half**: the DLSS / DLSS-G / FSR integration rules (zero-initialised NGX
  options, UI recomposition flag at creation, matrix layout, invalid-vector value, jitter sign test), the HUD-less + UI
  input and HUD paste, the frame generation presenter thread and pacing lessons, and the test method (debug DLL
  visualizer, PresentMon displayed times, A/B switches).
- **Not needed:** the D3D11 to D3D12 bridge, DXBC patching, object matching and keys, MGS2's jitter, MSAA switch, the
  HUD Redirect (UE4's `r.ScreenPercentage` + `r.TemporalAA.Upsampling` already render the 3D below the output and the UI
  at the output).
- **What remains hard:** frame generation presentation inside the port's own present path (Reflex, vblank thread, the
  FSR 1 / RCAS and overlay passes), and the one object-motion gap UE4 leaves: translucent effects and particles write no
  velocity.

## 2. What MGS2 built (question 1)

| Piece | How MGS2 does it | Code / doc | Lessons, dead ends |
|---|---|---|---|
| Camera motion vectors | Full-screen pass: unproject depth with the inverse of this frame's `raise_eye_pers`, project with last frame's; UV offsets current -> previous | `MGS2: lab_motion_vectors.cpp:26-110` (shader), `:494-560` (`EnsureInputs`, `DrawInputs`) | Keep the previous matrix yourself (`g_prevM`, `:164`, `:792`); cut detection by camera travel / turn (`IsCut`, `:180-205`): 2000 units / 20 deg fired on ordinary pans, now 8000 / 45 |
| Object motion vectors, old path | A second draw of each moving piece into UAVs; matrices captured at the game's Unmap; previous piece chosen by mesh + draw order | `MGS2: lab_object_motion.cpp` (2,437 lines) | ~40 D3D calls a draw, 3-4 ms a frame; slot table overflow (8,192) turned fights blue |
| Object motion vectors, in-draw | DXBC patcher appends a copy of the game's vertex shader reading last frame's constants (`cb13`) and a pixel shader output `o1 = prevPixel - SV_Position`; an RG16F second render target; blend variants mask motion when depth write is off | `MGS2: lab_dxbc_patch.cpp:83` (`PatchVertexShaderPrev`), `:199` (`PatchPixelShaderMotion`), `lab_mv_rt.*`, `lab_state_shadow.*`; `MGS2: research\docs\plan-in-draw-motion-vectors.md` | 0.15-0.28 us a draw after "Lean State"; translucent rule: a blended draw writes motion only if the same piece wrote depth this or last frame (hair); 183/183 VS and 58/58 PS bit-exact offline first |
| Object identity | Mid hook in the engine's object setup command: the live `DG_OBJS*` goes into the piece key; hand-off rule for meshes passing between objects | `MGS2: lab_object_motion.cpp:234-240, 1106`; `MGS2: research\docs\plan-object-keys.md` | Dead ends: D3D-level identity (per-object vertex buffers are not stable), "closest matrix" matching (the matrix carries the camera, so neighbours win under a pan) |
| Jitter | 8-phase Halton 2/3 added to channel 0's projection in an inline hook on `DG_SetCamera`; unjittered copies kept for the vectors | `MGS2: lab_temporal.cpp:37-68, 162-180`; `MGS2: research\docs\plan-upscaling.md` S0-S1 | DLSS jitter sign settled only below native (+x +y at 0.667, all four looked alike at native); an inline hook moved a stack slot another mid hook read (focal length trap) |
| Below native, HUD at full size | Internal resolution = render size, then at the engine's "first HUD pack" rule upscale into output-size twins and re-bind the pass | `MGS2: lab_redirect.cpp:210` (`Split`), `lab_final_viewport.cpp` | Shadow map taken for scene depth below native (size rule); the game rewrote its output fields at 2560x1440 |
| DLSS SR | NGX D3D11 on the game's device; flags MVLowRes, DepthInverted, AutoExposure; MV scale = render size; preset hint | `MGS2: lab_dlaa.cpp:103-142, 182` | +~10% GPU at native on the 4090; jitter sign switch (`'`+`\`) |
| DLSS-G | NGX **D3D12** directly (not Streamline), on the bridge; `clipToPrevClip` / `prevClipToClip` in DLSS-G's `clip x M` layout, no jitter in them; real projection and camera basis; `motionVectorsInvalidValue`; `UserInterfaceRecompositionEnabled = 1` at creation | `MGS2: lab_dlssg.cpp:91-110` (create), `:127-135` (layout), `:141-200` (evaluate); `lab_motion_vectors.cpp:444-480` (`DlssgMatrices`) | Traps 12-16 in `MGS2: research\docs\handoff-2026-10-01-dlss.md`: transposed matrices made static walls "move" (inch-wide trails); `eye_inv` is world -> view, not the camera; zero-initialised options (0.0 = invalid vector); the HUD-less / UI inputs are ignored without the creation flag; check every input with NVIDIA's debug `nvngx_dlssg.dll` visualizer |
| HUD-less colour | The scene at the HUD split through MGSHDFix's gamma pass into a shared texture; must equal the real frame bit for bit outside the HUD | `MGS2: lab_hudless.cpp:905` (`OnEndOf3D`); `MGS2: research\docs\plan-free-hudless.md` | "120 that looks like 60": any pass after the capture point missing from the HUD-less copy makes FSR show the real frame |
| UI input and HUD paste | Alpha of the target zeroed at the split, HUD draws' alpha blend swapped to build coverage; `UI = (F - (1-a) H, a)`; after DLSS-G the real HUD pasted over the generated frame | `MGS2: lab_hudless.cpp:739-790` (`LayerBefore/After`), `lab_bridge.cpp:1086` (`UiPass`), `:1273` (`HudPaste`); `MGS2: research\docs\hud-dlssg.md` | Trap: the game starts the next frame before presenting this one, so per-frame flags must travel with the frame; thin HUD lines still smeared under DLSS-G's own recomposition: the paste fixed it |
| FG presentation and pacing | DLSS-G: the game thread presents the generated frame at the pacer's time, a presenter thread presents the real frame half a period later (timer + spin), timed from the generated present's *call*; FSR FG: FSR's own swap chain | `MGS2: lab_bridge.cpp:835-900` (`PresenterLoop`), `:902-915` (`StartPresenter`), `lab_present_pacer.cpp` | A detached thread is not joinable (17,327 presenter threads); judge by PresentMon displayed times and the eye, never Present times or 1% lows; FSR 4.0.1 presented with sync interval 0 (driver V-Sync needed); phase steering and present slots failed (`MGS2: research\docs\fsr-reference.md` section 5) |
| Backends | One shared input set, upscaler and frame generator chosen separately (NVIDIA / AMD paths never run together) | `MGS2: lab_upscale.*`; `MGS2: research\docs\plan-temporal-paths.md`, `MGS2: research\docs\plan-framegen-backends.md` | NGX over Streamline to keep their own swap chain and pacer; FSR version structs at context creation |

## 3. What UE4 gives in MediEvil (question 2)

### 3.1 Engine and config

| Finding | Label | Evidence |
|---|---|---|
| Unreal Engine **4.21** | checked | UTF-16 string `++UE4+Release-4.21-CL-0` at file offset `0x3c660c6` of `out\eboot.elf` (the 01.02 build the port runs; 107.5 MB, 2026-11-03) |
| Default AA is temporal: `r.DefaultFeature.AntiAliasing=2`; motion blur off by default (`r.DefaultFeature.MotionBlur=False`); auto exposure off by default (`r.DefaultFeature.AutoExposure=False`); `r.SeparateTranslucency=True`; `r.CustomDepthTemporalAAJitter=False` | checked | `OVERBITE/Config/DefaultEngine.ini` `[/Script/Engine.RendererSettings]` (01.02 pakchunk0, extracted with `scripts/ue4_override.py` `read_file`) |
| `r.BasePassOutputsVelocity` is not set anywhere in the configs: the engine default (0) applies, so velocity comes from a **separate velocity pass**. It is a read-only cvar that selects shader permutations; turning it on would need re-cooked shaders | checked (not set) / inferred (meaning) | grep of all 29 inis in the 01.02 paks; the cvar name exists in the eboot (`0x3cef752`) |
| Cvars present in the eboot: `r.TemporalAA.Upsampling`, `r.TemporalAASamples`, `r.TemporalAAFilterSize`, `r.TemporalAACurrentFrameWeight`, `r.TemporalAA.AllowDownsampling`, `r.UsePreExposure`, `r.EyeAdaptation.*`, `r.SecondaryScreenPercentage.GameViewport`, `r.ParallelVelocity`, `r.VelocityTest` | checked | UTF-16 strings in `out\eboot.elf` |
| The port writes `r.ScreenPercentage` and, below 100%, `r.TemporalAA.Upsampling=1` (TAAU) into the override pak | checked | `scripts/ue4_override.py:464-469`; AA choice `:392-394, 480-482` |

### 3.2 The frame (from the existing capture; frame map in [study-heavy-frame-map.md](study-heavy-frame-map.md) section 1)

| Pass | Targets | eids (capture A) | Label |
|---|---|---|---|
| GBuffer | 6 RT + D32S8 3840x2160 | 9892-14335 (630 draws) | checked |
| **Velocity** | `R16G16_UNORM` + the scene D32S8; cleared at 14346 | 14348-14771 (**59 draws** vs 630 in the GBuffer: moving / skinned only) | checked |
| Readers of velocity | 16789 (P6576, also reads GBuffer, depth and last frame's TAA output: SSR-like) and **17630 / 17641** | | checked (usage) |
| Separate translucency composite | reads SceneColor + the translucency target, writes RGBA16F 6429 | 17616 (P6007) | checked (usage) / inferred (role) |
| **TAA** | two full-screen draws (P6575, P6574) with the scene depth-stencil bound; read colour 6429, velocity, depth, last frame's output 6647; write RGBA16F 147863. Two draws with a stencil test = UE4's normal + responsive-AA split. Its 176-byte pass buffer holds 0.04 = UE4's default `r.TemporalAACurrentFrameWeight` | 17630, 17641 | checked (bindings, usage) / inferred (that it is TAA, the stencil split) |
| Bloom, eye adaptation (1x1 `R32G32_FLOAT` + readback), LUT | from the TAA output | 17654-17889 | checked |
| **Tonemap** -> the guest display buffer 1179 (3840x2160 `R8G8B8A8_SRGB`) | | 17901 | checked |
| UMG off-screen widgets (894x52, 1370x70, 1340x35) | own targets | 17917-17963 | checked |
| **UI into the display buffer** | 28 draws into 1179 | **17976**-18173 | checked |
| Port presenter: FSR 1 / RCAS compute (240x135 groups), blit to the B8G8R8A8 swap chain, ImGui overlay | | 18181-18403 | checked (`vk_presenter.cpp:635-649, 956`) |

So the HUD-less picture is simply **the display buffer at the end of the tonemap pass**, before the first UI draw:
UE4 draws its UI straight into that buffer, with no post pass in between in this frame (checked for this frame; menus,
pause and movies not captured yet).

### 3.3 The View uniform buffer (checked)

One 3,296-byte buffer with the same contents (md5 `d0b850687f30`) is bound to the velocity draw (binding 0 and 4), the
SSR-like pass and both TAA draws (binding 0): UE4's `FViewUniformShaderParameters`. The member order comes from the
eboot (the member names are stored in reverse declaration order at `0x3e48618`-`0x3e4a8e8`); offsets follow UE4's
uniform buffer alignment (matrix / float4 / float3 16 bytes, float2 8, float 4, a float packs after a float3). Values
are from eid 17630 (`rd_bufdump.py` on capture A); they confirm the offsets.

| Offset | Member | Value in capture A | Use |
|---|---|---|---|
| 384 | `ViewToClip` (jittered) | row 2 = (-7.67e-5, 4.96e-5, 0, 1), row 3 = (0, 0, 10, 0) | jitter is in [2][0], [2][1] |
| 448 | `ViewToClipNoAA` | same without the jitter | DLSS-G `cameraViewToClip` |
| 832 / 848 / 864 | `ViewForward` / `ViewUp` / `ViewRight` | unit vectors | DLSS-G / FSR camera basis (no inverse needed, unlike MGS2 trap 14) |
| 912 | `InvDeviceZToWorldZTransform` | (0, 0, 0.1, -1e-8) | scene depth = 10 / deviceZ |
| 928 | `ScreenPositionScaleBias` | (0.5, -0.5, 0.5, 0.5) | NDC y up, texture y down |
| 944 | `WorldCameraOrigin` | (-984.9, 18808, 706.4) | camera position (UE4 units = cm) |
| 1200 | `PrevViewToClip` | last frame's jitter in row 2 | |
| **1824** | **`ClipToPrevClip`** | ~identity (camera almost still); row 2 does not carry the jitter difference | camera reprojection; DLSS-G `clipToPrevClip` |
| **1888** | **`TemporalAAJitter`** | (-7.666e-5, 4.960e-5, 3.373e-5, -3.401e-4) = this frame xy, last frame zw, in NDC | jitter: x px = j.x * W / 2, y px = -j.y * H / 2 |
| 1920 | `FieldOfViewWideAngles` | (1.2217, 0.7504) rad = 70 deg horizontal, 43 deg vertical | FG vertical FOV |
| 1952 / 1968 | `ViewSizeAndInvSize` / `BufferSizeAndInvSize` | (3840, 2160, 1/3840, 1/2160) twice | **identification signature**, render size |
| 2004 | `PreExposure` | 1 | scene colour is not pre-exposed |
| 2112 | `NearPlane` | 10 | |
| 2128 | `MaterialTextureMipBias` | 0 (render scale 100) | UE4's own mip bias with TAAU (inferred): the port must not add another |
| 2140 / 2144 | `FrameNumber` / `StateFrameIndexMod8` | 663 / 6 | jitter phase (8 samples, inferred) |
| **2148** | **`CameraCut`** | 0 | reset for DLSS / FSR / FG |

**Depth (checked):** reversed Z with an infinite far plane: `ViewToClip` has [2][2] = 0, [2][3] = 1, [3][2] = near
(10), [3][3] = 0; D32S8 at the render size. Invariant 2 of upscaler-reference.md is now confirmed from data.

### 3.4 Velocity and jitter semantics (inferred from UE 4.21, to confirm at M2)

- Encoding (`DecodeVelocityFromTexture`): `v = enc / (0.499 * 0.5) - (32767/65535) / (0.499 * 0.5)`; `enc = (0, 0)`
  (the clear) means "not written": take the camera's motion. `v` is current minus previous NDC position, with the jitter
  of both frames removed in the velocity shader.
- Static geometry: UE4's TAA rebuilds it as `prev = (ndc.x, ndc.y, deviceZ, 1) x ClipToPrevClip`, `v = ndc - prev.xy / prev.w`.
- To pixels, current -> previous (what DLSS and FSR want): `mv = (-v.x * W / 2, +v.y * H / 2)`.
- Not covered by velocity: translucency and particles (the 83 draws of phase 10, Cascade GPU particles), static meshes
  animated by world position offset (wind, flags), anything drawn after TAA (UI). UE4's own TAA handles translucency
  with the responsive-AA stencil.
- Jitter pattern: 8 samples (`r.TemporalAASamples` default); 4.21 shapes the Halton points into a Gaussian with
  `r.TemporalAAFilterSize`, and with TAAU raises the count by the upscale ratio squared. DLSS and FSR prefer uniform
  Halton points; the values handed to them are exact either way.

## 4. Mapping MGS2 to MediEvil (question 3)

| MGS2 component | MediEvil needs it? | Source in MediEvil | Action |
|---|---|---|---|
| D3D11 -> D3D12 bridge, shared textures, fences | no | Vulkan already; NGX VK and FSR-Vulkan run on the port's device | none |
| Camera reprojection pass (`lab_motion_vectors.cpp:26-110`) | yes, smaller | UE4's `ClipToPrevClip` (no inverse, no stored previous matrix) | one compute pass that also decodes velocity (section 3.4); adapt the push-descriptor compute scaffolding of `vk_camera_motion.cpp:180-219`, **not** its Bloodborne camera match (`:221-241`) |
| Object vectors (second draw, DXBC in-draw, keys, slot table) | **no** for opaque objects | the velocity pass (moving and skinned, 59 draws in capture A) | none; drop the skinning replay `motion_history.h` for MediEvil |
| Translucent / particle vectors (MGS2's "Cut-out Edges" rule) | later, if FG shows smearing on effects | none (UE4 writes none) | first: camera motion + DLSS bias mask / FSR reactive mask from the TAA responsive stencil or the translucency pass; only then consider an in-draw port in shadPS4's shader recompiler (it owns the SPIR-V, so it is easier than DXBC, but particles need previous positions from the simulation targets) |
| Cut detection (`IsCut`) | no | `CameraCut` (offset 2148); add resets for level loads, movies, `r.SetRes` | read the flag |
| Jitter injection (`lab_temporal.cpp`) | **no** | `TemporalAAJitter` (1888) | port viewport jitter off for MediEvil (`vk_rasterizer.cpp:1244-1246, 1388-1389`, `vk_temporal_upscaler.cpp:346-356`) |
| MSAA off | no | UE4 runs no MSAA (`NumSceneColorMSAASamples` = 1) | none |
| HUD Redirect, render below native | no | `r.ScreenPercentage` + `r.TemporalAA.Upsampling` (`ue4_override.py:464-469`): TAA's output and everything after it at the output size | replace TAAU's output instead of redirecting targets; the port's `TemporalUpscaler` redirect machinery (`RedirectColor`, `RasterScaling`, `SceneMipBias`, `vk_rasterizer.cpp:1720-1726`) stays off |
| DLSS SR settings (`lab_dlaa.cpp:127`) | yes | the port's bridge already has MVLowRes, depth inverted flag, HDR + auto exposure, jitter, reset (`gow3_dlss_bridge.cpp:178-181, 217-218`) | pass `depth_inverted = 1` (today 0: `vk_dlss.cpp:207-209`), `hdr = 1` (TAA's input is linear RGBA16F before the tonemap); later the 1x1 eye adaptation texture as exposure |
| DLSS-G inputs (`lab_dlssg.cpp:141-200`) | yes | View UB: `ClipToPrevClip`, `ViewToClipNoAA`, camera basis and origin, `NearPlane`, `FieldOfViewWideAngles.y`, `CameraCut`, jitter | new bridge calls (`NGX_VK_CREATE_DLSSG` / `NGX_VK_EVALUATE_DLSSG`, `MGS2-Lab\MGSHDFix\external\DLSS\include\nvsdk_ngx_helpers_dlssg_vk.h:45-77`); copy MGS2's option list line by line |
| HUD-less picture | yes, much simpler | copy of the display buffer after the tonemap draw, before the first UI draw (3.2) | one image copy at a role-identified point |
| UI input / HUD layer / HUD paste (`lab_bridge.cpp:1086, 1273`, `lab_hudless.cpp:739-790`) | yes | the UI draws into the display buffer after that point | start with MGS2's legacy rule (`F != H` -> opaque UI), then the alpha layer (in shadPS4 the blend state is part of the pipeline key: a pipeline variant per UI pipeline), then the paste |
| Presenter thread + pacing (`lab_bridge.cpp:835-915`, `lab_present_pacer.cpp`) | concepts, not code | the port's own pacing: Reflex (`vk_instance.cpp:353-364`), vblank-thread presents, "below half refresh" cap (START-HERE state 2026-10-25) | two presents per flip, real half a period after generated; the existing "Auto: below half refresh" cap (119 on 240 Hz) is already the right base rate for 2x FG |
| Test tools | yes | | NVIDIA debug DLSS-G DLL visualizer; MV debug view (Bloodborne's `CameraMotion::Overlay` pattern); PresentMon displayed times; a live A/B switch per change |

**Jitter rules.** One jitter source, the game's. With UE4 TAA on, the port does nothing. With DLSS or FSR replacing TAA,
the port reads `TemporalAAJitter` from the View UB **of the same frame** it upscales (store it with that frame's work,
not in a global: MGS2 trap "the game starts its next frame before it presents this one"). Optional later: raise
`r.TemporalAASamples` in the override pak for low render scales; check the sample spread in the log before.

## 5. Revised step plan (question 4)

Each step: one change per launch, behind a switch, the user's go for every launch.

| Step | What | Reuse from MGS2 | New | Test | Risks |
|---|---|---|---|---|---|
| **U0 Frame map, finish** | Most of M0 is already done (3.2, 3.3). Missing: a RenderDoc capture at render scale 67 (TAAU) and one in a menu / pause / after a death effect; the shadPS4 `pgm_hash` of the velocity PS, both TAA PS, the tonemap PS | `rd_bufdump.py` method | none in code | capture analysis, offline | TAAU may use other TAA permutations or a compute TAA (`FPostProcessTemporalAACS` exists in the eboot) |
| **U1 Identify, log only** | In `vk_rasterizer.cpp` (buffer binding, `:2280-2283, 2341-2343`): the View UB = range 3296 + `ViewSizeAndInvSize` matching the bound depth; the velocity pass (`R16G16_UNORM` target + scene DS); TAA (full-screen draw reading velocity + scene depth, writing RGBA16F, after velocity); the tonemap draw into the display buffer; the first UI draw | MGS2 invariant 13b (identify by role, not pointer) | `Ue4Frame` record per frame: images, View UB snapshot (jitter, matrices, cut) | log every frame: found / not found per role, render size, jitter values, cut flag; across 4K, 1440p and a render-scale change, menus | the guest may rewrite the View UB before stage B reads it (copy it at bind time) |
| **U2 Motion vectors + debug view** | Compute pass: decode velocity, reconstruct the rest from depth + `ClipToPrevClip`, write RG16F pixels | `lab_motion_vectors.cpp:26-110` shape; the colour source map idea (green own / blue camera) | the shader (section 3.4 math) | the user: static walls ~0 with the camera still, characters coloured, pans uniform; static walls must not change when an enemy moves | y sign; velocity scale (0.499 constant) |
| **U3 DLAA (DLSS at render scale 100)** | Skip both TAA draws, evaluate DLSS into TAA's output image (147863 in A; next frame's history and the SSR input keep working) | `lab_dlaa.cpp` settings, jitter sign switch (`lab_dlaa.cpp:182`) | bridge D0 (build), `depth_inverted = 1`, `hdr = 1`, MediEvil profile in place of the Bloodborne identifiers | A/B against UE4 TAA live; jitter sign cycled; static shimmer | the responsive-AA pixels (stencil) lose their special handling: feed a bias mask later |
| **U4 DLSS below native** | Same hook with TAAU (render scale 50-77%): DLSS writes the output-size image TAAU would | | quality from the ratio, preset setting | compare with UE4 TAAU at the same scale | TAAU permutation from U0 |
| **U5 FSR 3.1 / 4 SR** | Same inputs into the FSR-Vulkan upscaler the port already links | | flags: depth inverted + infinite, HDR | A/B with DLSS | FSR exposure / HDR range |
| **G1 HUD-less + UI inputs** | Copy the display buffer at the first UI draw; UI = legacy rule first | `hud-dlssg.md` pipeline steps 1-3; `lab_bridge.cpp:1086` | the copy, the UI pass | HUD-less diff against the real frame outside the UI = 0 pixels (MGS2 invariant 1); debug view | menus with a blurred background widget (a UI draw reading the scene) |
| **G2 DLSS-G evaluate, shown alone** | Bridge: query the FrameGeneration feature's instance / device extensions too (the bridge asks for SuperSampling only, `gow3_dlss_bridge.cpp:69, 79-110`), create / evaluate DLSSG on the display buffer | `lab_dlssg.cpp:91-200` option by option: UI recomposition at creation, `motionVectorsInvalidValue`, `clip x M` layout untransposed (UE4's matrices are row-vector already), `depthInverted`, `cameraMotionIncluded`, `mvecScale` | extension list, `NVSDK_NGX_Resource_VK` wrappers | NVIDIA debug DLL visualizer: "Motion and Camera Vectors" green on static geometry, "ClipToPrevClip" masks only moving things | device extensions must be requested at device creation (restart switch) |
| **G3 Present both** | Generated then real per flip; the port's FSR 1 / RCAS and ImGui overlay applied to both images after FG, not before | `PresenterLoop` timing (real from the generated present's call), start-once rule | the second present in the vblank thread, swap chain image count, Reflex markers around two presents | PresentMon displayed gaps (two buckets of half the base period), the user's eye on a slow pan | interaction with Reflex sleep and the 960 tick; tearing in Immediate mode (FG wants FIFO / VRR) |
| **G4 HUD paste, alpha layer** | Paste the real UI over generated frames; then true alpha from the UI draws | `HudPaste`, `LayerBefore` | pipeline variants with alpha-coverage blend for UI pipelines | captures of generated frames: UI pixel-exact, scene behind translucent panels generated | |
| **G5 FSR 3 FG** | `ffxVkPortableFrameGenerationContextPrepare/RecordDispatch` (`ffx_vk_portable.h:202-260, 339-376`) on the same inputs; host owns presentation and UI (`ffx_vk_framegeneration_presenter.h`) | `fsr-reference.md` invariants 1, 2, 4, 13b; HUD paste (FSR's Vulkan API takes no UI texture) | camera fields: near 10, far infinite, vertical FOV, `viewSpaceToMeters` 0.01 (UE4 cm, inferred) | same as G3 | FSR pacing differs from DLSS-G |
| **G6 Effects polish** | Reactive / bias masks from the translucency pass or the responsive stencil; then decide on translucent motion | MGS2 translucent rule | | fire, death effects, smoke in a fight, under FG | the biggest image-quality unknown |

Suggested order: U0, U1, U2, U3, then **G1-G3 before U4-U5**. MediEvil is CPU-bound (stage B, START-HERE "I2"), so
sub-native rendering buys little FPS on the user's 4090 while frame generation doubles the displayed rate; DLAA is the
cheapest hook that proves the inputs.

## 6. Where the existing MediEvil plans are wrong or outdated (not edited)

| Doc | Says | Now |
|---|---|---|
| upscaler-reference.md section 1 | pass identification at `vk_rasterizer.cpp:265-295, 959-970, 1101-1107`; present path `vk_presenter.cpp:689-860`; pacing `driver.cpp:654-720` | moved: `vk_rasterizer.cpp:289-314` (G-buffer / scene colour), `:1085-1093` (display pass), `:1115-1117` (UI); `vk_presenter.cpp:825` (`Present`), `:635-649` (FSR 1 / RCAS), `:956` (overlay); `driver.cpp:358` (`Flip`), `:898` (`PacerThread`) |
| upscaler-reference.md invariant 2 | reversed-Z infinite "to confirm at M0" | confirmed from capture A (3.3) |
| plan-ue4-motion-vectors.md "Design" 1 | map the frame with `GOW3_CAPTURE_TRIGGER` | the RenderDoc map of 2026-10-21 already has velocity, TAA, tonemap, UI; `user\captures` is empty. Remaining: TAAU, menus, hashes (U0) |
| plan-ue4-motion-vectors.md 3 | MGS2's `lab_motion_vectors.cpp:25-150` is the reprojection reference | UE4's own `ClipToPrevClip` path is simpler (no inverse, no stored previous matrix) |
| plan-ue4-motion-vectors.md 6 | feed UE4's eye adaptation later | `PreExposure` = 1 and a 1x1 `R32G32_FLOAT` eye adaptation target exists; the TAA input is linear HDR: DLSS needs `hdr = 1` |
| plan-ue4-motion-vectors.md (missing) | | UE4 applies its own mip bias with TAAU (`MaterialTextureMipBias`): keep the port's `SceneMipBias` off; the TAA hook is two draws (normal + responsive stencil); TAA's output is also next frame's history and the SSR input |
| plan-dlss.md D2 | extend the ABI for DLSSG | also the extension query: the bridge's feature discovery is SuperSampling only (`gow3_dlss_bridge.cpp:69`) |
| plan-fsr-frame-generation.md | pacing with "480 Hz virtual vblank, slot-based limits" | the default is now Reflex + vblank-thread presents + "below half refresh" cap, tick 960 (START-HERE 2026-10-25); FG must fit that path |
| plan-fsr-frame-generation.md 2 | redraw the ImGui overlay on both presents | also the FSR 1 / RCAS pass: run FG on the display buffer and the presenter chain on each image (or the HUD-less copy differs) |
| plan-fsr-frame-generation.md | HUD-less from Dispatch | FSR-Vulkan's Dispatch has no UI input: the host composes the UI (README of the submodule, line 62); plan the HUD paste for FSR too |
| plan-roadmap.md order 4-6 | MVs, then FSR FG, then DLSS | see section 5: DLAA first as the input check, FG before sub-native for this CPU-bound game |

## 7. Open questions

1. TAAU (render scale < 100): the same PS pair or another permutation / compute shader? (U0 capture.)
2. Menus, pause, movies, loading: is the UI still drawn after the tonemap into the display buffer, and does any UI draw
   read the scene (background blur)? Set `notRenderingGameFrames` / reset there.
3. How bad are fire, smoke and death effects under FG with camera motion only? (Decides G6.)
4. Does `VK_NV_low_latency2` pacing work with two presents per flip, or does FG need Reflex off and the port's own limiter?
5. NGX DLSS-G on Vulkan outside Streamline: which device extensions and queue capabilities does the FrameGeneration
   feature require on the user's driver? (Query at D0, before any launch.)
6. Jitter: keep UE4's Gaussian 8-sample set, or raise `r.TemporalAASamples` for DLSS below native?

## How this was checked

- Configs: every `.ini` in the 01.02 paks extracted with `ue4_override.read_file` into a temp folder and grepped.
- Eboot: UTF-16 strings of `out\eboot.elf` (engine version, cvar names, View UB member names and order).
- View UB values: `research-data\renderdoc\heavy-analysis\rd_bufdump.py` run through `qrenderdoc.exe --python` on
  `heavy_frame41595.rdc` with `RD_EIDS=17630,17641,14348,17901,16789`, the 3,296-byte binding decoded with the offsets
  above. Usage (which pass reads velocity, depth, the TAA output) from `dump-41595.json`.
- No game launch, build or git command.
