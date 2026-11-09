# Study: frame map under temporal upsampling (step U0 of the DLSS / FSR plan)

Status: offline capture and cache analysis, 2026-11-08. Nothing launched, built or committed. Parent:
[study-mgs2-temporal-for-ue4.md](study-mgs2-temporal-for-ue4.md) (sections 3.2-3.4, 5 step U0, 7 questions 1-2);
method from [study-heavy-frame-map.md](study-heavy-frame-map.md).

Captures:
- **A** `research-data\renderdoc\heavy_frame41595.rdc` (2026-10-21, 4K, render scale 100; the map of the parent study).
- **C** `medievil_2019_pc_port\research-data\renderdoc\mausoleum-pc_frame21929.rdc` (PC, NVIDIA, 2026-11-06 23:59:15).

Labels: **checked** = seen in a capture, the port's code or the port's shader cache files; **inferred** = from UE 4.21
behaviour or from indirect evidence, not seen in a capture.

## Answer in short

- **Capture C is not a TAAU frame.** It is a second native 4K frame (checked): the View uniform buffer has
  `ViewSizeAndInvSize` = `BufferSizeAndInvSize` = 3840x2160 and `MaterialTextureMipBias` 0, every scene target,
  viewport, the display buffer and the swap chain are 3840x2160, and TAA is the same pair of full-screen pixel draws
  as in A. The capture was written at 23:59:15, during the run of `t-mausoleum-pc-clean.log` (23:56:56-00:00:43,
  3840x2160 output, render scale 100%, console quality), not the run of `t-mausoleum-pc-ref.log` (1280x720, 80%,
  ended 23:55:19). Both runs used the same `GOW3_RENDERDOC` prefix `mausoleum-pc`. The separate translucency target
  is 2880x1620 = 75% of 4K, the clean run's `effects_resolution=75` (the ref run had 50). No TAAU capture exists
  (`mausoleum-pc-a2off*` was never written; the dust captures of 2026-11-01 are native 4K too).
- **Under TAAU, TAA is most likely one compute dispatch, not the two draws** (inferred, strong). The ref run (1280x720,
  80%) compiled two compute shaders 11 s after it started that no native run had needed. One of them,
  **`cs 0xb373f39908ae979f`** (8x8 groups), has the TAA pixel shaders' velocity decoding constants
  (`4.008016 = 1/(0.499*0.5)`, `2.0039775`), their 0.2 / 0.025 / 0.01 constants, a 1x1 texel fetch (eye
  adaptation), **a stencil read as a uint texture testing bit 3 (`8u & stencil ? 0.25 : ...`)**, the same bit the
  native draws' stencil test uses, and **one image store** (its output). So the responsive-AA split moves into the
  shader and no depth-stencil is bound. Only a TAAU capture can show its eids, sizes and bindings.
- **Native TAA (checked, A and C):** draw 1 `fs 0xe88c8fcbe08ea0b6` (stencil bit 3 clear), draw 2
  `fs 0xe5588ed466d8f2f3` (bit 3 set), both with `vs 0xe76703c6ddfc5f06`; **same SPIR-V bytes in both captures**.
  Inputs: scene depth-stencil (bound and sampled), velocity, scene colour after the separate translucency composite,
  last frame's TAA output; output RGBA16F at the render size.
- **View UB offsets hold** in C (checked): jitter in `ViewToClip[2][0..1]` = `TemporalAAJitter.xy`, last frame's
  in `PrevViewToClip` = `TemporalAAJitter.zw`; C's previous jitter equals A's current jitter, and
  `StateFrameIndexMod8` is 7 in C and 6 in A: one fixed 8-entry jitter table indexed by that field.
- **Tonemap and UI:** C tonemap 16260 (`fs 0x3632d4c4faaa85d2`, same SPIR-V as A 17901), first UI draw 16344
  (`fs 0x4fe20af683865713`, same as A 17976); nothing touches the display buffer in between (checked).
- **Shader identity:** shadPS4's `pgm_hash` is the guest binary's own `shader_hash` (checked,
  `regs_shader.h:249`). It is visible in every capture in the pipeline debug names (`Graphics Pipeline
  fs_<hash>,vs_<hash>`), and the SPIR-V of each stage matches the cache file `<hash>_<perm>.spv` byte for byte.
  The velocity pass has **no single shader**: 7 (C) / 10 (A) material-dependent pixel shaders. Identify it by target.

## 1. TAA: draws or compute, inputs, outputs

| Item | A (native 4K) | C (native 4K) | TAAU (1280x720 at 80%) | Label |
|---|---|---|---|---|
| Form | 2 full-screen draws, 3 indices each: eid 17630 (P6575), 17641 (P6574) | 2 draws: eid **16004** (P8875), **16012** (P8719) | 1 compute dispatch, `cs 0xb373f39908ae979f`, local size 8x8x1, one `imageStore` | checked (A, C); inferred (TAAU, from the cache: see "TAAU evidence") |
| Pixel shaders | `fs 0xe88c8fcbe08ea0b6` (26,700 B SPIR-V, md5 `9e8d6ed8`), `fs 0xe5588ed466d8f2f3` (25,800 B, md5 `75ca2239`) | same hashes, **same SPIR-V md5** | n/a | checked |
| Vertex shader | `vs 0xe76703c6ddfc5f06` (md5 `1bca5dbe`); reads a 1x1 `R32G32_FLOAT` image (A 6653, C 8725; eye adaptation, inferred) at binding 13 | same | a 1x1 fetch in the CS (`texelFetch(cs_img1, (0,0))`) | checked (binding) / inferred (role, TAAU) |
| Stencil split | draw 1: stencil test Equal 0, compare mask 8 (bit 3 clear = normal pixels); draw 2: NotEqual 0, mask 8 (responsive); depth test off; write mask 8, ops Keep (no writes) | same | in the shader: `0 != (8u & stencil)` picks blend 0.25 | checked (A, C); inferred (TAAU) |
| Depth-stencil bound | A 6411 D32S8 3840x2160 | C 8812 D32S8 3840x2160 | none (compute) | checked / inferred |
| PS binding 2 | scene depth 6411 | 8812 | | checked |
| PS binding 3 | velocity 6395 `R16G16_UNORM` | velocity 8587 | | checked |
| PS binding 4 / 5 | draw 1: colour 6429 / history 6647; draw 2: history 6647 / colour 6429 (swapped) | draw 1: colour 8707 / history 8811; draw 2: 8811 / 8707 | CS: 5 `texture2D` + 1 `utexture2D` (stencil) + 1 storage image | checked / inferred |
| Colour input | 6429 RGBA16F 3840x2160 = output of the separate translucency composite 17616 (`fs 0xcf069898bf1f5514`) | 8707 RGBA16F 3840x2160, composite 15990 (same shader and SPIR-V) | render size (1024x576), composite before TAA (inferred) | checked / inferred |
| History (last frame's TAA output) | 6647 RGBA16F | 8811 RGBA16F (read only at 15541, 16004, 16012) | output size (inferred) | checked / inferred |
| Output | 147863 RGBA16F 3840x2160 | **46579** RGBA16F 3840x2160; read by bloom setup 16025 and tonemap 16260 | output size 1280x720, written by `imageStore` | checked / inferred |
| Viewport / render area | 3840x2160 | 3840x2160 (flipped viewport, y = 2160, h = -2160) | | checked |
| Bindings 0 / 1 | View UB 3,296 B / TAA parameters 176 B | same | | checked |
| TAA parameters (176 B, binding 1) | float [25] = 0.04 (`r.TemporalAACurrentFrameWeight`); floats 0-7 = (3840, 2160, 1/3840, 1/2160) twice; [8-24] change with the jitter (sample weights) | same layout, [25] = 0.04 | | checked (values) / inferred (names) |

The other reader of velocity and of last frame's TAA output: A 16789, C **15541** (`fs 0xf8cfb0056f2c2e12`, same
SPIR-V in A and C; SSR-like, also reads GBuffer, depth, HZB 2048² R16F); writes RGBA16F 8795 (checked).

### TAAU evidence (inferred: shader cache, not a capture)

| Fact | Where |
|---|---|
| The ref run (1280x720, 80%, low) started 23:53:18. Cache files written at 23:53:29: 2 compute shaders and 11 graphics shader binaries with their `.meta` and pipeline keys; 14 more graphics binaries by 23:54:38. The a2off run (same settings, 00:10) compiled no new compute shader | `user\cache\CUSA11227` file times (`spv_cache_scan.py`); a file's time is its last compile; runs that find it in the cache do not rewrite it (the boot-time compute shaders were recompiled on 2026-11-08, these two have not been rewritten since 2026-11-06 23:53) |
| `0xb373f39908ae979f_0.spv`: CS 8x8x1, 35,592 B. Same velocity decode as the TAA PS (`fma(4.008016, v, -2.0039775)`), same 0.2 / 0.025 / 0.01 constants, stencil test `8u &` on a `utexture2D`, 1x1 `texelFetch`, one `imageStore` to a `writeonly image2D` | `spirv-cross` output `taau-analysis\glsl\0xb373f39908ae979f_0.glsl` lines 570-571, 789, 826 |
| `0xa080f599034de78e_0.spv`: CS 8x8x1, 12,180 B, 3 textures, 1 image store, no velocity decoding; shares the constants 10, 6.4, 64, 11, 0.1 with the TAA CS | unidentified (open question 2) |
| `0x2b1ad246bf1f5514_0`: same decompiled code as the native translucency composite `0xcf069898bf1f5514`, another guest hash | the same pass can arrive under a second `pgm_hash` under other settings: match hashes as a set, or by role |
| UE 4.21 has `FPostProcessTemporalAACS` (in the eboot) with screen-percentage permutations; its pixel shader pair cannot write an output larger than its input with the scene depth-stencil bound | inferred (engine behaviour; study-mgs2-temporal section 7 question 1) |

What is not settled without a TAAU capture: that `0xb373f39908ae979f` is dispatched as TAA (not, say, a low-quality
variant used at any scale), its dispatch size, input and output sizes, whether the scene targets are exactly the
render size or larger with a sub-rectangle, and the output image's id and format.

## 2. Velocity pass

| Item | A | C | Label |
|---|---|---|---|
| Target | 6395 `R16G16_UNORM` 3840x2160 + scene D32S8 6411 | **8587** `R16G16_UNORM` 3840x2160 + D32S8 8812 | checked |
| Clear | 14346 | load-op clear at BeginRendering 8576 | checked |
| Draws | 59 (14348-14771), 10 pipelines | **24** (8578-8745), 7 pipelines; vs 236 GBuffer draws (3267-4928) and 103 prepass draws | checked |
| Pixel shaders | `0x6335e3e86c740313` (14), `0x257a18527403ceeb` (10), `0x9f82a9d96c740313` (9), `0x3ee8851c198ba15d` (8), `0x6defe6d46e64ec96` (8), `0xaa4852617403ceeb` (4), `0x7daa8c97ac3f5401` (2), `0x8ffe3ccf1216218f` (2), `0x0bf093136c740313` (1), `0x27cdb350e9b37f46` (1) | `0xd485041188566aa2` (14), `0x8ffe3ccf1216218f`, `0x6335e3e86c740313`, `0x7daa8c97ac3f5401`, `0x257a18527403ceeb` (2 each), `0x125028b8198ba15d`, `0x27cdb350e9b37f46` (1 each) | checked |
| State | depth test GreaterEqual (reversed Z), depth write off, no blend | same | checked |
| Readers | 16789 (SSR-like), 17630, 17641 | 15541, 16004, 16012 | checked |
| Encoding | `v = enc * 4.008016 - 2.0039775` (= `(enc - 32767/65535) / (0.499 * 0.5)`), in both TAA PS and the TAAU CS | | checked (shader code), confirms section 3.4 of the parent study |

Under TAAU the velocity target should be at the render size (inferred). The velocity shaders are per material and
vertex factory, so **the velocity pass must be found by its target** (an `R16G16_UNORM` colour target with the scene
D32S8 bound, after the GBuffer), not by shader hash.

## 3. View uniform buffer at the TAA draws (C 16004 / 16012; A 17630)

One 3,296-byte content per frame (C md5 `09e3c7298ef7` at the velocity draw, 15541, both TAA draws; A `d0b850687f30`),
bound to the TAA pixel shader at binding 0 and to compute work (checked).

| Offset | Member | C value | A value | Label |
|---|---|---|---|---|
| 384 | `ViewToClip` | rows (1.428148, 0, 0, 0), (0, 2.53893, 0, 0), (**1.911252e-4, 2.851072e-4**, 0, 1), (0, 0, 10, 0) | row 2 (-7.665879e-5, 4.960274e-5, 0, 1) | checked |
| 448 | `ViewToClipNoAA` | same, row 2 (0, 0, 0, 1) | same | checked |
| 1200 | `PrevViewToClip` | row 2 (-7.665879e-5, 4.960274e-5) | (3.372992e-5, -3.400749e-4) | checked |
| 1824 | `ClipToPrevClip` | ~identity (camera still) | ~identity, row 2 small | checked |
| 1888 | `TemporalAAJitter` | (1.911252e-4, 2.851072e-4, -7.665879e-5, 4.960274e-5) = 0.367 px, -0.308 px at 3840x2160 (`x * W/2`, `-y * H/2`) | (-7.666e-5, 4.960e-5, 3.373e-5, -3.401e-4) | checked |
| 1920 | `FieldOfViewWideAngles` | (1.22173, 0.7504159) | same | checked |
| 1952 | `ViewSizeAndInvSize` | (3840, 2160, 1/3840, 1/2160) | same | checked |
| 1968 | `BufferSizeAndInvSize` | (3840, 2160, ...) | same | checked |
| 2004 | `PreExposure` | 1 | 1 | checked |
| 2112 | `NearPlane` | 10 | 10 | checked |
| 2128 | `MaterialTextureMipBias` | 0 | 0 | checked |
| 2132 | `MaterialTextureDerivativeMultiply` | 1 | 1 | checked |
| 2140 | `FrameNumber` | 1567 | 663 | checked |
| 2144 | `StateFrameIndexMod8` | 7 | 6 | checked |
| 2148 | `CameraCut` | 0 | 0 | checked |

Differences from A: only the camera and the jitter phase. C's previous jitter is A's current one and C's index is one
higher, so the jitter comes from one fixed table indexed by `StateFrameIndexMod8` (checked for indices 5, 6, 7:
A's zw, A's xy, C's xy). `InvDeviceZToWorldZTransform` (0, 0, 0.1, -1e-8) and `ScreenPositionScaleBias`
(0.5, -0.5, 0.5, 0.5) are the same (checked).

Expected under TAAU at 1280x720 / 80% (inferred, to check in the TAAU capture): `ViewSizeAndInvSize` = 1024x576,
`BufferSizeAndInvSize` = the scene target size (equal or larger), `MaterialTextureMipBias` negative,
`TemporalAAJitter` in render-size NDC, a jitter table of more than 8 entries (UE 4.21 raises the sample count with the
upscale ratio), and the View UB bound to the TAA compute dispatch.

## 4. Tonemap, display buffer, first UI draw

| Item | A | C | Label |
|---|---|---|---|
| Bloom / eye adaptation / LUT | 17654-17889 | 16025-16248 (eye adaptation 1x1 `R32G32_FLOAT` 8818 at 16037 + readback copy 16040; LUT 32x32 `R10G10B10A2` at 16248) | checked |
| Tonemap | 17901, `fs 0x3632d4c4faaa85d2` / `vs 0xe76703c6ddfc5f06`, into 1179 | **16260**, same shaders and SPIR-V (md5 `f0ecd320`); reads TAA output 46579, bloom 8712 1920x1080, LUT 8763, 1x1 1671; writes **1229** `R8G8B8A8_SRGB` 3840x2160 (view `R8G8B8A8_UNORM`) after a load-op clear at 16258 | checked |
| Between tonemap and the first UI draw | UMG widgets into their own targets | 16263-16267 three `vkCmdClearColorImage` of 102518 (a 400x400 UI texture), 102521, 104918; widget draws into 102523 894x52, 102537 1370x70, 102541 1340x35 (16278-16331); `vkCmdCopyBuffer` 16333. **No use of 1229** (its usage list goes 16260 ColorTarget -> 16344 ColorTarget) | checked |
| First UI draw | 17976 `fs 0x4fe20af683865713` / `vs 0x05bcd744ac08b2b2`, 28 draws into 1179 | **16344**, same shaders and SPIR-V; **33 draws** into 1229 (16344-16582), alpha blend | checked |
| After the UI | port presenter | 16586 `vkCmdCopyImageToBuffer` from 1229 (port side; role not checked), FSR 1 EASU / RCAS dispatches 16592 / 16597 (240x135 groups), blit into the swap chain 1893 / 194 `B8G8R8A8` 3840x2160, ImGui 16701-16725 | checked (order) |

So the HUD-less image is still "the display buffer after the tonemap draw, before the first UI draw" in a second
frame and a second scene (checked for gameplay frames only; menus, pause, movies, loading still open).

## 5. Shader identity the port can match at run time

How the identifier works (checked in code):
- `pgm_hash` = the guest shader binary's `shader_hash` from its binary info (`gpu/shadps4/video_core/amdgpu/regs_shader.h:249`
  `.hash = bininfo.shader_hash`), the game's own value: the same across runs and port builds (A and C have the same
  TAA and tonemap hashes, 16 days and many port commits apart; which game version each run used was not checked).
- Pipelines are named `Graphics Pipeline <stage>_<pgm_hash>,...` / `Compute Pipeline cs_<pgm_hash>`
  (`vk_pipeline_common.cpp:142-155` `GetDebugString`, `vk_graphics_pipeline.cpp:439`, `vk_compute_pipeline.cpp:114`;
  `PipelineCache::GetShaderName`, `vk_pipeline_cache.cpp:1353-1358`). The pixel stage is spelled `fs`.
- Shader modules are named `<stage>_<pgm_hash>_<perm>` only when compiled in that run (`vk_pipeline_cache.cpp:572-573,
  1168-1169`); modules restored from the cache at warm-up are not named (`vk_pipeline_serialization.cpp:~1145`
  `CompileSPV` without `SetObjectName`), so in captures most show as "Shader Module N".
- The cache stores each stage's SPIR-V as `user\cache\CUSA11227\<pgm_hash>_<perm>.spv` (`RegisterShaderBinary`,
  `vk_pipeline_serialization.cpp:611-619`); `draw_io.py` matches a capture's SPIR-V md5 against those files.
- At run time the rasterizer has the hash in each bound pipeline's stage info (`stage->pgm_hash`), the same value
  `GetDebugString` prints.

| Role | A | C | TAAU | How obtained | Label |
|---|---|---|---|---|---|
| Velocity PS | 10 shaders (section 2) | 7 shaders (section 2) | | pipeline names of every draw into the velocity target (`dump-*.json`) | checked; identify by target, not hash |
| TAA PS, normal pixels | `fs 0xe88c8fcbe08ea0b6` (P6575) | same (P8875) | | pipeline name + cache SPIR-V match `0xe88c8fcbe08ea0b6_0` | checked |
| TAA PS, responsive pixels | `fs 0xe5588ed466d8f2f3` (P6574) | same (P8719) | | same, `_0` | checked |
| TAA VS | `vs 0xe76703c6ddfc5f06` (also the tonemap's) | same | | same | checked |
| TAA CS | not present | not present | **`cs 0xb373f39908ae979f`** | cache file time + decompiled code | inferred |
| Separate translucency composite PS | `fs 0xcf069898bf1f5514` | same | possibly `fs 0x2b1ad246bf1f5514` (identical code) | pipeline name / cache | checked / inferred |
| Tonemap PS | `fs 0x3632d4c4faaa85d2` | same | | pipeline name + cache | checked |
| First UI draw PS | `fs 0x4fe20af683865713` | same | | module name (compiled in the run) + cache | checked |
| SSR-like reader of velocity and history | `fs 0xf8cfb0056f2c2e12` | same | | pipeline name + cache | checked |

The same 11-12 compute pipelines exist in A, C and the dust capture `dust_frame73172.rdc` (`names-3caps.txt`); none is
`0xb373f39908ae979f`. All three hold the TAA PS pair's pipelines and a 3840x2160 display buffer: none of the captures
checked is a TAAU frame (`dust_frame74230.rdc` not opened).

## 6. What would break the plan's hook under TAAU

The hook of step U3 / U4: "skip both TAA draws, write the upscaler output into TAA's output image".

| Issue | Why | What U1 / U4 must do | Label |
|---|---|---|---|
| There are no TAA draws to skip | TAAU runs as one compute dispatch (`cs 0xb373f39908ae979f`) | U1 role detection must accept a compute dispatch that reads velocity + depth + the stencil view and stores one image; U4 skips that dispatch | inferred |
| The output is a storage image written by the CS, at the output size | TAAU writes the upscaled result in place of the two draws' colour target | write DLSS / FSR's output into that storage image (it is also next frame's history, the SSR-like pass's input and bloom / tonemap's input) | inferred |
| No depth-stencil binding to identify the pass by | the stencil is sampled as a `utexture2D` | match by the CS hash or by its bound images (velocity `R16G16_UNORM` + D32S8 depth / stencil views) | inferred |
| Responsive-AA pixels lose their 0.25 blend | the CS reads stencil bit 3 | build DLSS's bias / FSR's reactive mask from stencil bit 3 of the scene depth-stencil (the same bit in native and TAAU) | checked (bit), inferred (use) |
| Sub-rectangle | `BufferSizeAndInvSize` may exceed `ViewSizeAndInvSize` (UE4 keeps larger scene targets after a live render-scale change; the port reallocates on change, impl-live-render-scale.md) | pass DLSS / FSR the input sub-rectangle and render size from the View UB of the same frame, not the image size | inferred |
| Jitter scale | the jitter is in render-size NDC; UE 4.21 uses more samples under TAAU | pixels = `j.x * ViewSize.x / 2`, `-j.y * ViewSize.y / 2`, read from the View UB bound to the TAA dispatch | inferred |
| Mip bias | `MaterialTextureMipBias` should be negative under TAAU | keep the port's own `SceneMipBias` off | inferred |
| Hash sets | a pass can arrive under two guest hashes with identical code (translucency composite) | U1 logs every hash per role at each setting; do not hard-code one | checked (composite), inferred (TAA) |
| History and SSR at different sizes | under TAAU the history is output size while the scene is render size | none if the upscaler writes the same image; check that the SSR-like pass still reads it | inferred |

Nothing here blocks the native-resolution hook of U3 (DLAA): in both native captures it is the same two draws, the same
SPIR-V, the same bindings and the same output role (checked).

## Open questions

1. **A TAAU capture** (needs a launch: the user's go). Settings of `t-mausoleum-pc-ref` (1280x720, 80%) with a
   `GOW3_RENDERDOC` prefix of its own; check: the dispatch of `cs 0xb373f39908ae979f`, its group count, input and
   output images (ids, sizes, formats), the View UB values of section 3's "expected under TAAU", the jitter table length.
2. What `cs 0xa080f599034de78e` is (compiled at the same moment, no velocity input).
3. Whether the low quality preset alone (at 100%) also selects the CS; a TAAU capture at console quality or a native
   capture at low quality would separate the two.
4. Menus, pause, movies, loading (parent study question 2): still not captured.
5. The `t-mausoleum-pc-ref.log` / capture pairing in the run records: the capture belongs to `t-mausoleum-pc-clean.log`.

## How this was checked

RenderDoc 1.46 (`C:\Program Files\RenderDoc\qrenderdoc.exe --python`). Raw outputs in
`C:\Users\Ty\MediEvil\research-data\renderdoc\taau-analysis\` (junction to `D:\MediEvil-archive\research-data`).
New reusable scripts: `research/tools/renderdoc/draw_io.py` (per draw: pipeline name, shader SPIR-V md5 + cache file
match, viewport, targets, stencil state, every bound image / buffer), `view_ub.py` (the 3,296-byte View UB decoded),
`spv_cache_scan.py` (shaders in the cache folder by write time). One-off helpers copied next to the outputs:
`groups.py` (draw groups from a dump), `quick_ids.py` (pipeline / image names in several captures).

```sh
cd /c/Users/Ty/MediEvil/research-data/renderdoc/taau-analysis
QRD="/c/Program Files/RenderDoc/qrenderdoc.exe"; T='C:\Users\Ty\MediEvil\medievil_2019_pc_port\research\tools\renderdoc'
C='C:\Users\Ty\MediEvil\medievil_2019_pc_port\research-data\renderdoc\mausoleum-pc_frame21929.rdc'
A='C:\Users\Ty\MediEvil\research-data\renderdoc\heavy_frame41595.rdc'
CACHE='C:\Users\Ty\MediEvil\medievil_2019_pc_port\user\cache\CUSA11227'
# whole frame: actions, state calls, textures, usage
RD_CAPTURE=$C RD_OUT=dump-21929.json "$QRD" --python 'C:\Users\Ty\MediEvil\research-data\renderdoc\heavy-analysis\rd_dump.py'
python groups.py dump-21929.json 0 99999 > groups-21929.txt
# per-draw bindings, shader hashes (C, then A)
RD_CAPTURE=$C RD_OUT=io-21929.txt RD_SPV_DIR=$CACHE RD_SPV_SAVE=spv-21929 \
  RD_EIDS=8578,8745,15491,15541,15551,15616,15632,15649,15663,15676,15976,15990,16025,16037,16248,16260,16344,16605 \
  "$QRD" --python "$T\\draw_io.py"
RD_CAPTURE=$C RD_OUT=io-21929-taa.txt RD_SPV_DIR=$CACHE RD_SPV_SAVE=spv-21929 RD_EIDS=16004,16012 "$QRD" --python "$T\\draw_io.py"
RD_CAPTURE=$A RD_OUT=io-41595.txt RD_SPV_DIR=$CACHE RD_SPV_SAVE=spv-41595 RD_EIDS=14348,16789,17616,17630,17641,17901,17976 \
  "$QRD" --python "$T\\draw_io.py"
# View UB
RD_CAPTURE=$C RD_OUT=view-21929.txt RD_EIDS=8578,15541,16004,16012 RD_RAW_DIR=ub-21929 "$QRD" --python "$T\\view_ub.py"
RD_CAPTURE=$A RD_OUT=view-41595.txt RD_EIDS=17630 "$QRD" --python "$T\\view_ub.py"
# TAA parameter buffers (176 B)
RD_CAPTURE=$C RD_OUT=buf-21929-taa.txt RD_EIDS=16004,16012 "$QRD" --python 'C:\Users\Ty\MediEvil\research-data\renderdoc\heavy-analysis\rd_bufdump.py'
RD_CAPTURE=$A RD_OUT=buf-41595-taa.txt RD_EIDS=17630,17641 "$QRD" --python 'C:\Users\Ty\MediEvil\research-data\renderdoc\heavy-analysis\rd_bufdump.py'
# TAA / compute pipeline names in A, C and the dust capture
RD_CAPS="C:\\Users\\Ty\\MediEvil\\research-data\\renderdoc\\dust_frame73172.rdc;$A;$C" RD_OUT=names-3caps.txt "$QRD" --python quick_ids.py
# compute shaders in the cache by write time; decompile the candidates
python "$T\\spv_cache_scan.py" "$CACHE" > cache-compute.txt
/c/msys64/clang64/bin/spirv-cross.exe "$CACHE\\0xb373f39908ae979f_0.spv" --vulkan-semantics --output glsl/0xb373f39908ae979f_0.glsl
```

Velocity counts, readers of 8587 / 46579 / 8811 / 1229 and the pass contents of section 4 come from `dump-21929.json`
(`usage`, `actions`). Run attribution: file times of the capture (23:59:15), of `research/docs/data/t-mausoleum-pc-*.log`
/ `.times`, and the `medievil.ini.bak-t-mausoleum-pc-*` backups (`output_resolution`, `render_scale`,
`effects_resolution`). RenderDoc's stencil function numbers: 6 = Equal, 7 = NotEqual; depth 5 = GreaterEqual. No game
launch, build or git command.
