# HDR: what the port and the game have, and what exposing it takes (research, 2026-11-17, no launch)

Parent: [plan-work-queue.md](plan-work-queue.md) "HDR output". The user (2026-11-17): the port has an HDR checkbox;
check statically what it does; UE4 has HDR support with many knobs, expose them all. Two read-only research agents
(the port's path and the game's request; the engine's variables). Tags: **[V]** verified in code, files or disassembly,
**[I]** inferred (UE 4.21 knowledge or not yet run). Game addresses are the 01.02 eboot (`out/eboot.elf` / `out/boot.bin`)
unless marked; cvar objects are mapped for 01.00 too. Agent scratch (not kept): `scratchpad/hdr/` (cvar scan
`regs2.py`, `regs0100.txt`, `regs0102.txt`, `sites.txt`, the 01.02 pak inis in `cfg0102/`, `kd.py` Kismet dump),
`scratchpad/sfo.py`, `xref.py`, `dis.py`.

## Summary

1. **The "Allow HDR output" checkbox does nothing for MediEvil today** [V]. The game's param.sfo declares no HDR
   (ATTRIBUTE `0x800020` in 01.00 and 01.02, bit 29 clear), and the port reports an HDR display (BT2020_PQ) only with
   `GOW3_HDR` **and** an HDR10 surface **and** that bit. The game always sees an SDR display. The checkbox's only
   effect is the instance extension `VK_EXT_swapchain_colorspace`.
2. **The game does contain UE4's PS4 HDR10 output path** [V]. At RHI init it reads the display's HDR capability
   (`sceVideoOutGetDeviceCapabilityInfo_(h, &cap, 8)`, `cap & 0x80` -> `GRHISupportsHDROutput`, byte `0x5b2f558`, code
   `0x688790`). The backbuffer setup `0x6893c0` (from `RHICreateViewport` `0x69e990` / `RHIResizeViewport` `0x69ea20`)
   uses `IsHDRAllowed() && IsHDREnabled() && GRHISupportsHDROutput`: then display buffers in `0x88740000`
   (A2R10G10B10 BT2020 PQ) instead of `0x80000000` (A8R8G8B8 sRGB), and `sceVideoOutConfigureOutputMode_` with
   colorimetry 12 (Bt2020PQ). `IsHDRAllowed` (`0xfa9af0`): command line `-hdr` / `-nohdr`, else `r.AllowHDR`;
   `IsHDREnabled` (`0xfa9a20`): `r.HDR.EnableHDROutput`. Both default 0 and **none of the game's 29 config inis sets
   any HDR, tonemapper, gamma or color cvar** [V]. PreInit forces both to 0 at SetByCode when the capability is missing
   (`0x874c-0x894c`) [V].
3. **The game has HDR plumbing but no HDR menu**, and a Brightness slider [V]. `OverbiteGameInstanceFunctions.
   ApplyOptions` calls `EnableHDRDisplayOutput(SupportsHDRDisplayOutput() AND SaveData.HDR, 1000)`; `SaveData.HDR`
   defaults false; `PMENU_HDR` exists only in `Game.locres`, no widget uses it. Brightness
   (`UI/OptionsMenu/WidgetBrightnessSettings`) runs `gamma <FClamp(B*4.5+0.5, 0.5, 5.0)>` (UE4's `gamma` command,
   `GEngine->DisplayGamma`, default 2.2 at the save default B = 0.3778).
4. **The port's HDR present path is unfinished** [V code]: the intermediate frame image and the post-process pipeline
   stay in the SDR surface format (`RecreateFrame` uses `swapchain.GetSurfaceFormat()`, `vk_presenter.cpp:234`,
   pipeline `:165`; the HDR switch `:1203-1206` recreates the frame in the same format), so PQ values would be cut to
   8 bits and then blitted into the 10-bit swapchain (banding [I]); the overlay pipeline is built for the SDR format
   but draws into the swapchain image (`vk_presenter.cpp:166`, `:975`, `gow3_overlay.cpp:2794-2811`): a format
   mismatch, and sRGB UI values read as PQ; FSR1's HDR mode squares the colour (`fsr.comp:67-75`), wrong for PQ;
   no paper-white / peak-nits handling, no scRGB; screenshots would save raw PQ. (One agent read the presenter as
   "already matching"; the other found these specifics. Treat it as needing work.)
5. **UE 4.21's HDR controls in this build are few** [V]: on/off, output device (1000 or 2000-nit ACES ST-2084, or
   scRGB which the PS4 RHI cannot make), gamut, UI level / composite mode. No `r.HDR.Aces.*`, no peak / mid luminance
   cvar, no `r.DisplayGamma`, no PS4 HDR cvar. A real paper-white or peak-nits slider has to be done by the port.

## The checkbox path today [V]

- Launcher `'hdr': False` (`launcher/medievil_launcher.py:181`), stored in settings.json; on -> `env['GOW3_HDR']='1'`
  (`:577-578`); UI `:1648-1650`. No HDR key in run.py, medievil.ini, extra-env.txt, the in-game menu
  (`gow3_overlay.cpp`, `gow3_settings.*`), `ue4_pc_settings.py` or `graphics_options.py`.
- `IsHdrAllowed()` = `Flag("GOW3_HDR", false)` (`gpu/shim/core/emulator_settings.h:60`) -> instance extension
  (`vk_platform.cpp:200-202`).
- Swapchain: the only HDR pair is `A2B10G10R10UnormPack32` + `eHdr10St2084EXT` (`vk_swapchain.cpp:20-23`);
  `FindPresentFormat` (`:273-302`) sets `supports_hdr` once (pair offered and `GOW3_HDR`); `Create` uses
  `needs_hdr ? HDR : surface_format` (`:86`, `:460`).
- `Presenter::SetHDR` (`vk_presenter.h:74-84`) returns unless `swapchain.HasHDR()`; `Swapchain::SetHDR` acts only on a
  change: `waitIdle`, recreate, `ImGui::Core::OnSurfaceFormatChange` (an empty stub, `imgui_core.h:8`). Called on every
  flip with `req.port->is_hdr` (`videoout/driver.cpp:385-386`).
- `port->is_hdr` comes from `sceVideoOutConfigureOutputMode_` (`video_out.cpp:432-458`): colorimetry Any (0xFF) ->
  false; Bt2020PQ (12) -> true only with `GetPSFAttributes().support_hdr` (bit 29, `elf_info.h:55`, filled in
  `src/probe.c:1542-1550`, passed at `gow3gpu.cpp:95`); anything else INVALID_VALUE.
- `sceVideoOutGetDeviceCapabilityInfo` (`video_out.cpp:370-380`): 0x80 only with `IsHDRSupported()` and `support_hdr`.
- The game's video-out imports: Open / Close, flip and vblank events, `ConfigureOutputMode_`,
  `GetDeviceCapabilityInfo_`, flip / resolution / vblank status, `ModeSetAny_`, Register / Unregister buffers,
  `SetBufferAttribute`, `SetFlipRate`, `SetWindowModeMargins`, `SubmitFlip`, `WaitVblank`. No SetGamma / AdjustColor.
  SystemService imports have no HDR query. A second path (`0x68a0e0-0x68a178`) re-registers SDR buffers with mode Any.
- The HDR pixel format maps to `A2B10G10R10Unorm` (`image_info.cpp:31`), viewed as `A2R10G10B10Unorm`
  (`vk_presenter.cpp:353`): accepted. `post_process.frag` passes HDR through unchanged (no PQ / Rec.2020 handling).
- UE4 upscalers (TAA, FSR 3, FSR 4, DLSS): their "HDR" flag means linear scene colour before the tonemapper, not HDR
  output. Native AA and every preset at 1080p run on scene colour; scaled presets run on the tonemapped frame
  (`vk_temporal_upscaler.h:8-10`, `.cpp:413-416`), which would become PQ-coded; format checks expect
  `eB8G8R8A8Srgb` (`vk_temporal_upscaler.cpp:1915`, `vk_ue4_motion.cpp:605`).

## Engine variables (both builds register the same set) [V]

Flags: `0x20` RenderThreadSafe, `0x40` Scalability, `0x04` ReadOnly. Object addresses: 01.02 / 01.00 (TAutoConsoleVariable,
`IConsoleVariable*` at +8 as in the port's `ue4_cvars[]`; the scan matches the existing `r.BloomQuality` row). "Live" [I].

| Cvar | Type, default, flags | What it does | Object | Live |
|---|---|---|---|---|
| r.AllowHDR | int 0, 0x04 | HDR-compatible swapchain if platform and display support it; `-hdr` / `-nohdr` override | 0x5ba21c0 / 0x5ba2180 | at viewport create / resize |
| r.HDR.EnableHDROutput | int 0, 0x20 | hardware HDR output | 0x5ba2208 / 0x5ba21c8 | sink next frame; buffers on resize |
| r.HDR.Display.OutputDevice | int 0, 0x60 | 0 sRGB, 1 Rec709, 2 gamma (r.TonemapperGamma), 3 / 4 ACES 1000 / 2000-nit ST-2084, 5 / 6 scRGB, 7 linear EXR | 0x5ba21f0 / 0x5ba21b0 | yes |
| r.HDR.Display.ColorGamut | int 0, 0x60 | 0 Rec709, 1 DCI-P3, 2 Rec2020, 3 ACES, 4 ACEScg | 0x5ba21d8 / 0x5ba2198 | yes |
| r.HDR.UI.Level | float 1.0, 0x20 | UI luminance when composited into HDR | 0x5be8c68 / 0x5be8c28 | yes |
| r.HDR.UI.CompositeMode | int 1, 0x20 | 0 standard, 1 shader pass for HDR blending | 0x5be8c80 / 0x5be8c40 | probably |
| r.TonemapperGamma | float 0, 0x60 | fixed gamma with OutputDevice 2 | 0x5ba2220 / 0x5ba21e0 | yes |
| r.Gamma | float 1.0, 0x20 | "gamma on output" (use not checked) | 0x5ba2238 / 0x5ba21f8 | ? |
| r.TonemapperFilm | int 1, 0x20 | film tonemapper | 0x5b92730 / 0x5b926f0 | yes |
| r.Tonemapper.Quality | int 5, 0x60 | 0 basic .. 5 + grain jitter (vignette 2, grain 4) | 0x5c1a0b8 / 0x5c1a078 | yes |
| r.Tonemapper.Sharpen | float 0, 0x60 | tonemapper sharpening (overlaps RCAS: leave out) | 0x5ba21a8 / 0x5ba2168 | yes |
| r.Tonemapper.GrainQuantization | int 1, 0x60 | anti-banding noise | 0x581a410 / 0x581a3f0 | yes |
| r.Tonemapper.MergeWithUpscale.Mode / .Threshold | int 0 / 0.49, 0x60 | leave alone (upscaler hooks) | 0x5b96550 / 0x5b96568 | yes |
| r.Color.Min / Mid / Max | float 0 / 0.5 / 1.0, 0x20 | black lift, mid-tones, highlights | 0x5b926d8 / 0x5b926f0 / 0x5b92708 (01.00 -0x40) | yes |
| r.EyeAdaptationQuality | int 2, 0x60 | eye adaptation | 0x581a3c8 / 0x581a3a8 | yes |
| r.EyeAdaptation.PreExposureOverride / .MethodOveride / .Focus | float 0 / int -1 / float 1 | overrides | 0x5b95040 / 0x5b95058 / 0x5b95070 | yes |
| r.SceneColorFormat / r.PostProcessingColorFormat | int 4 / 0, 0x60 | FloatRGBA scene colour | 0x581a008 / 0x581a038 | better at startup |

Project values: `r.DefaultFeature.AutoExposure=False`, `r.DefaultFeature.Bloom=True` (`OVERBITE/Config/DefaultEngine.ini`);
BaseScalability @3: `r.Tonemapper.Quality=5`, `r.Tonemapper.GrainQuantization=1`, `r.EyeAdaptationQuality=2`,
`r.BloomQuality=5`, `r.SceneColorFormat=4`; PS4 device profiles: nothing HDR. Not cvars: `GEngine->DisplayGamma` (the
`gamma` command only; the port has no console-command path), `UGameUserSettings` HDR nits (the game passes 1000).

What the engine does [V]: the sink `0x274b7b0` (on a change of `GRHIIsHDREnabled`, `0x5b2f557`) sets OutputDevice =
HDR ? (nits <= 1000 ? 3 : 4) : 0 and ColorGamut = HDR ? 2 : 0 at SetByDeviceProfile (`0x05`). `EnableHDRDisplayOutput`
(`0x20bca70`) rounds nits to 1000 / 2000 and sets `r.HDR.EnableHDROutput` at SetByGameSetting (`0x02`).

## How the port can set them

- **Override pak** (`scripts/ue4_override.py`, `[SystemSettings]` in a copy of `PS4Engine.ini` inside
  `pakchunk99-ps4_p.pak`): SetBySystemSettingsIni (`0x04`) at startup; `GOW3_UE4_CVARS="r.AllowHDR=1;..."`
  (`ue4_override.py:540-545`) already tests it. Read-only cvars can be set from the ini (`r.DBuffer=0` works). Nothing
  else in the ini hierarchy sets HDR keys. Caveat: OutputDevice from the pak loses to the sink's `0x05`.
- **Runtime** (mechanism A, `src/runtime_extra.c:572-664`, a `ue4_cvars[]` row per cvar): `IConsoleVariable::Set` at
  the cvar's current SetBy, so it always passes; from the main menu on, game thread, values up to 15 characters. Live:
  everything above except HDR availability (fixed at RHI init by the capability; buffer format and output mode change
  only on a viewport create / resize: live on / off might work through the existing r.SetRes resize [I]).
- SetBy (UE 4.21) [I, with V where seen]: Constructor 0, Scalability 1, GameSetting 2 [V], ProjectSetting 3,
  SystemSettingsIni 4 [V], DeviceProfile 5 [V], ConsoleVariablesIni 6, Commandline 7, Code 8 [V], Console 9; a lower
  SetBy is ignored. So once the pak enables HDR, the game's `ApplyOptions` (`0x02`, HDR false) cannot turn it off; the
  1000 / 2000-nit choice must come from the runtime Set (after the sink).
- Side finding: the plan-work-queue.md row on `ue4_override` High/Ultra shadow presets is not a SetBy problem but
  `ue4_override.py` itself (`LOCAL_SHADOWS` / `SUN_SHADOWS` overwrite same-named High/Ultra entries, `preset_values`
  lines 409-418); the real device-profile conflict is `r.Streaming.PoolSize` (2000 at `0x05` beats the pak's `0x04`).

## Two routes

- **A, the game's own HDR10** (real HDR highlights from the scene's linear colour through ACES):
  1. `video_out.cpp:375` / `:449`: report BT2020_PQ and accept Bt2020PQ when `GOW3_HDR` is on and the surface offers
     HDR10, ignoring the param.sfo bit (or set bit 29 in probe.c when HDR is asked for).
  2. Pak: `r.AllowHDR=1`, `r.HDR.EnableHDROutput=1` (startup); runtime rows for OutputDevice 3 / 4 and UI level.
  3. Presenter: frame image and post-process pipeline in a 10-bit format with HDR; the overlay pipeline for the HDR
     format with PQ encoding of the UI and a UI brightness; FSR1 / RCAS on PQ data (disable or convert); screenshots
     converted to SDR; check the scaled upscaler presets, FSR 3 frame generation and the frame map's `Tonemap` hash on
     PQ data (DLSS / FSR on scene colour are unaffected).
  4. Port-side paper-white / peak-nits: decode PQ, scale, re-encode in `post_process.frag` (live).
- **B, port-side SDR -> HDR** (inverse tonemap of the SDR frame to PQ / scRGB with paper-white): simpler, the overlay
  stays consistent, but highlights are only expanded. Windows Auto HDR / RTX HDR on the Vulkan swapchain: no code [I].

## Runs (2026-11-17, release build of the uncommitted step 1 + 2 code, 4K, Windows HDR on)

Card: [hdr-output.md](../patches/present/hdr-output.md). Screenshots: the user's NVIDIA captures (JXR, scRGB),
`Videos/NVIDIA/medievil-runtime.exe/* 2026.11.17 - 05.2*.jxr`.

- **H1, log lines only** ("Allow HDR output" on): `HDR10 surface offered`; 7 surface pairs, including
  `A2B10G10R10UnormPack32/Hdr10St2084EXT` and `R16G16B16A16Sfloat/ExtendedSrgbLinearEXT` (scRGB, an option for the
  port later). The game is told capability `0x0` (param.sfo HDR no), registers 2 buffers `A8R8G8B8Srgb (0x80000000)`
  3840x2160 and sets colorimetry 255 (Any). Open questions 1 and 2 answered. [V]
- **H2, `GOW3_HDR_GAME=1`** (two runs): capability `0x80`, buffers `A2R10G10B10Bt2020Pq (0x88740000)`, colorimetry 12,
  `Swapchain: HDR output on`. The game's own HDR10 path runs [V]. The 3D scene renders; **every UMG element is black**
  (options panel, title, button prompts; movies black too). RTSS's text is bright red (its SDR values read as PQ, ~10,000
  nits red: the capture's 2,690-nit peak). The user: the menu render looks washed out. Measured on the title capture:
  median 1.3 nits, 90% under 7, 99% under 74 [V]: a dim, flat picture.
- **H3, + `r.HDR.UI.CompositeMode=0`** (pak, `GOW3_UE4_CVARS`): movies and menus show up, oversaturated (sRGB UI values
  drawn straight onto the PQ frame), so the black UI is **confirmed** to come from the UI composite pass (mode 1). The
  user: legitimately brighter highlights, but raised blacks and poor contrast; the game's options menu has no HDR
  setting (open question 4 answered: none).
- **H4, 10-bit frame image** (`pp_pass` HDR pipeline, `RecreateFrame` in `A2B10G10R10` with HDR; mode 0 still): runs;
  a 600-px sky strip has 464 distinct values against 39 in an H2 capture of another view (a hint, not an A/B). Levels
  unchanged (median 1.7 nits, p90 9.6); the mode-0 UI peaks at 10,000 nits (its 0-1 values read as PQ).
- **H5, mode 1 + `r.HDR.UI.Level=4`**: UI still black. No eboot code writes `r.HDR.UI.Level` (xrefs: registration
  only, default 1.0) [V].
- **H6, frame map + shader dump** (`GOW3_UE4_FRAME_MAP=1 GOW3_DUMP_SHADERS=1 GOW3_PIPELINE_CACHE=0`; dumps in
  `user/shader/dumps`, the old SDR set moved to `dumps-sdr-20261029`): display-buffer draws in HDR are `fs
  12178a9314cb761e` (HDR tonemap) and, last, `fs c401ca35f9bceb8e`; neither is in the SDR dump [V]. `c401` is UE4's UI
  composite [V, IR]: scene (tex 0) PQ -> nits; UI (tex 1) rgba; where 0 < a < 1 the scene is compressed against
  `cb[0]`; UI colour * 1e-4 -> PQ -> **3D LUT (tex 2, `sampler3D`, 32^3)** -> x1.05 -> nits -> x `cb[0]`; out =
  PQ(scene * (1 - a) + UI term). Black where the UI is = alpha arrives, the UI term is 0. Both tonemaps (SDR `3632`,
  HDR `12178`) also sample a 32^3 `sampler3D` LUT and the scene is right, so 3D LUTs work in general. Lead [I]: the UI
  LUT is rendered once (when HDR turns on) where the scene LUT is re-rendered, and the texture cache's volume-slice
  path (`IsVolumeSliceTarget`, `texture_cache.cpp`) only serves a slice target that comes after its 3D image; a
  once-rendered LUT goes through `ExpandImage` or an upload from guest memory instead.
- **H7, image watch 32x32** (6,000 lines, cap reached at 27.9 s by a per-frame LUT): two 32-layer LUT targets, one
  re-rendered every frame through its 3D image, one rendered once. Which one the composite reads: unknown.
- **H8, frame map prints each display draw's sampled images** (new line `UE4 frame map: display draw ...`): `c401`
  reads `[2] 0x2001700000 32x32`; the HDR tonemap's LUT is another address, rendered every frame. The image watch:
  `0x2001700000` created at 0.23 s "for use as texture", uploaded from guest memory, **never rendered** [V].
- **H9, image watch by address** (`GOW3_IMAGE_WATCH_ADDR`, new; the watch lines now print depth and format): the only
  image ever over that memory is the 3D LUT `32x32x32 A2B10G10R10`, loaded once at boot [V]. Not a write in another
  shape.
- **H10, dropped-draw log + shader trace** (`GOW3_DROP_LOG=1`, `GOW3_UE4_FRAME_MAP_SHADERS`, new): the LUT builder `fs
  c401ca35ab81eb39` (no samplers, ~2,300 IR lines of PQ / ACES maths, in the H6 dump, HDR only) never drew; a draw
  with unnamed (still translating) stages was dropped right before the first composite. Precedent: the particle
  curve injection (`MustCompileSync`).
- **H11, the builder's pixel program in `MustCompileSync`**: still black. It compiled on the spot (3.3-4.1 ms), but
  the drop log shows `fs c401ca35ab81eb39, vs 0`: its vertex stage was still compiling in the background.
- **H12, RenderDoc** (`GOW3_RENDERDOC`, cold cache; captures `research-data/hdr-ui-20261117/menu_frame14681.rdc`,
  `16094`; script `research/tools/renderdoc/shader_draws.py` with `RD_EIDS`): composite eid 3569 reads the scene (PQ,
  max 0.75), the UI (RGBA8, full menu art) and the 32^3 LUT, **zero in every channel of every slice checked**; the
  scene LUT in the same frame is filled (0.002-0.72) [V]. Confirmed: the empty LUT is the cause.
- **H13, a listed pixel program compiles its whole draw on the spot** (`PipelineSelection::sync_all`): the builder
  and its stages (`f7457b588f171402`, `f7457b588a024e47`) compile at boot, it draws into `0x2001700000` (32 layers),
  the cache expands that into the 3D LUT with its contents. Menus, prompts and movies show with the right colours
  (the title yellow / orange; the blue in H12's PNG was RenderDoc's view of the 8-bit texture); the UI measures
  ~100 nits (p99 105). The user: "confirmed, that fixed it".
- **H14, SDR / HDR pair of the main menu** (SDR: game off HDR, NVIDIA PNG = the game's 8-bit pixels, 07.06.18;
  HDR: H13's JXR 06.55.01; 8x8 blocks, UI / RTSS masked; SDR modelled as gamma 2.2 at 200 nits): HDR p1 / p5 / p50 /
  p95 / p99 0.22 / 0.37 / 1.2 / 6.3 / 19 nits against SDR 0.05 / 0.21 / 1.8 / 14.6 / 37; p95/p5 17 against 71 (Windows'
  own sRGB decode of SDR: 25). Shadows lifted (floor ~0.2-0.4 nits), mid-tones where SDR at a 100-nit white puts
  them, highlights about half [V]. Scripts: scratchpad `hdr/curve.py`, `compare.py` (not kept).
- **LUT parameters** (`research-data/hdr-ui-20261117/lutbuf/eid3495_b0_*.bin`, menu capture, combine LUT builder `fs
  a4152b3be93c832a` buffer 0, 144 dwords) [V values, I names from UE 4.21's FCombineLUTsPS]: 40-44 film slope / toe /
  shoulder / black clip / white clip (0.88 / 0.55 / 0.26 / 0 / 0.04, UE4 defaults); 52-53 white temp / tint (6500 / 0);
  56-75 global saturation / contrast / gamma / gain / offset (float4 each, 1 / 1 / 1 / 1 / 0); 76-95 shadows; 96-115
  midtones; 116-135 highlights; 136 / 137 shadows max / highlights min (0.09 / 0.5); 138 output device (3); 139 gamut
  (2); 140 / 141 blue correction / expand gamut (0.6 / 1). The menu scene is not graded (all neutral). In the
  decompiled builder the HDR branch (`data[138] == 3 || 5`) takes the colour computed at line 735; the grading values
  are read before it (lines 498-709), the film values after it (890-1000, SDR path only) [V].
- **`GOW3_UE4_GRADE`** (runtime, `vk_rasterizer.cpp`): replaces named values of that block in the constant ring copy
  (and, outside the ring, in a patched stream copy); counters at 600 / 6,000 builder draws.
- **H15 / H16, `film.toe=1.0`**: no change at the dark end (H16: 6,000 of 6,000 draws patched; p1 0.211-0.228 against
  0.218). The film curve does not act in HDR [V: measurement and code]. The user saw black areas in the intro pan:
  not from the toe.
- **H17, `global.contrast=1.3`** (5,999 ring + 1 stream patched): p1 0.218 -> 0.082, p5 0.37 -> 0.143, p50 1.2 -> 0.57,
  p95 6.0 -> 5.3, p95/p5 17 -> 37. UE4 grading acts in HDR [V]; it pivots at mid-grey, so this scene (mostly below it)
  darkens overall: contrast plus a mid-tone gain.
- **H18, the overlay in HDR** (an ImGui pipeline for `A2B10G10R10`, `imgui_hdr.frag`: gamma 2.2 -> BT.2020 -> PQ, white
  203 nits): the user: "the menus look good, they're no longer over-saturated" [V]. The user keeps game UI at ~100 nits
  (OLED burn-in): one UI brightness for the game's UI and the overlay.
- **H19, `r.HDR.UI.Level=0.5`** (pak, `GOW3_UE4_CVARS`; JXRs 09.52.47 run 18 / 12.49.09 run 19, same gate view): health
  bar median / p99 55 / 105 -> 27 / 52 nits, mana bar 39 / 98 -> 19 / 49, icons p99 108-115 -> 55-58; the overlay's FPS
  box 203 both; sky median 4.1 / 4.9 [V]. The cvar scales the UI linearly, 1.0 = ~100-105 nits; nothing else moves.
  Built on it: Display > HDR (UI brightness, curve) and the Color tab (card hdr-output).
- **H20, the HDR section and the Color tab** (release 13:23): the overlay at 100 nits (log `white 100 nits`), 6,000 of 6,000
  combine LUT draws patched in the ring. White balance, Global and Ranges and gamut work; Shadows / Mid-tones / Highlights
  showed ImGui's "2 visible items with conflicting ID" and a change in one range moved all four: the styled rows' IDs are
  their names, which the four ranges share.
- **H21, an ID scope per range** (`ImGui::PushID(range)`, release 13:51): the user: "all the sliders appear to work" [V].
- **H22, the Color panel and the release build without the HDR diagnostics** (release 15:29; the user's run 21 grading
  active): the user: "everything functions as expected" [V].
- **H23, the UI composite's view formats** (RenderDoc replay of `menu_frame14681.rdc` eid 3569, `draw_state.py`, no
  launch): scene `R10G10B10A2_UNORM`, UI `R8G8B8A8_UNORM` (no sRGB view), LUT `R10G10B10A2_UNORM` 3D [V]. The UI's
  gamma-encoded values reach the UI LUT as linear: the washed-out UI and movies. Design: plan-hdr-sdr-match.md.
- **H24, our grading LUT pass (plan-hdr-sdr-match.md step 1)**: no effect, no log line: `Wanted` read dword 138 as a
  float; the game tests it as an integer (`data[138] >= 3u`) [V]. Fixed; the HDR flag now set every present.
- **H25, the pass running** (log `UE4 HDR grading: ... 32x32x32, A2B10G10R10`): the user: grey, lacking colour in the
  shadows / mid-tones. Menu JXR against the SDR menu frame: mid-tones 5-20x darker than SDR^2.2 x 203, darker than the
  ACES look (skull 6.5 nits, ACES 9.2, SDR-predicted 49). Offline, our table is brighter than ACES in the mid-tones
  (grey texel 3: 73 against 33 nits) [V]: the table or its input was off.
- **H26, RenderDoc** (`research-data/hdr-sdr-match-20261117`, `research/tools/renderdoc/lut_probe.py`, `draw_buffers.py`):
  our pass (eid 3589, 3 x 32) writes the 3D LUT the HDR tonemapper (eid 3614) samples; the diagonal is neutral; low
  end crushed (texel 1 0.01 against 0.63 nits, top 214 as expected). The block our pass received: dword 4 / 5
  (InverseGamma.x / .y) 0.615 / 1.354 against 0.428 / 0.942 in the H12 capture, everything else equal [V]: the game's
  Brightness setting (UE display gamma 1.625 now, 2.335 then) as a power on the graded colour. The ACES path never
  applied it, so it had no effect in HDR. Fix: dword 5 = 1 in our pass (Game brightness and Gamma take its place).
  The same slider in SDR bends the mid-tones, not the black level: the user's "Brightness looks wrong" (issue #1) [I].
- **H27, the game's Brightness neutral in our pass** (RenderDoc `run27_frame14247.rdc`, `save_target.py` of the composite's
  output, PQ decoded; gamma 2.2, 203 nits): our table matches the offline maths (grey texels 0.71 / 17 / 69 / 124 nits);
  the displayed menu re-encoded as an SDR screen would is close to the user's Nov 17 SDR screenshot, but that shot was
  taken with Windows HDR on (the user had flagged it) and is lighter than their Nov 11 SDR screenshot by about a power
  of 1.3 in linear. The user: run 27 looks much better than the morning's HDR, but the black floor is raised (their
  settings then: gamma Off (sRGB), game brightness 250) and the SDR re-encode is "extremely blown out" on a calibrated
  SDR screen.
- **H28, an SDR reference from the pre-HDR build** (worktree `../medievil_cft`, copy-fault-trace `22744f1`; RenderDoc
  `cft_frame12411.rdc`): the game's Brightness at the user's calibration (the skull just visible, slider ~30%) is UE
  display gamma 1.985 (dword 4 0.504, dword 5 1.108); the SDR LUT is built by the game (eid 3422, device 0). The SDR
  tonemapper writes `R8G8B8A8_SRGB`; its stored image equals sRGB(G^1.108) at static points (BGR bytes; sky 0 / 63 / 123
  against 14 / 64 / 123 predicted): encoded once, not twice [V]. The user's screenshot of the same menu equals the
  stored image within a few levels (slightly darker) [V]: the port's SDR presentation adds no brightening. Textures:
  colour textures are BC1 / BC3 `_SRGB` and every sampled view has its image's format (81 G-buffer draws) [V]. A
  washed-out default would then come from the game (scene exposure, its tuning) or a setting before its grading, not
  from the output. Next: test patterns through the whole pipeline, measured against the desktop with the user's meter.
- **H29, meter runs (test patterns, research/tools/meter/measure_patches.py, ArgyllCMS 3.5.0 spotread, display type `o`,
  no correction; CSVs in research-data/meter, 2026-11-18)** [V meter]:
  - Desktop reference (full-screen window, 10% window on black): white 102 cd/m2 (HCFR 100); effective gamma 2.01 (10%)
    to 2.14; neutral grey.
  - Game raw patches on black (written before presentation): white 99-101; within 1-3% of the desktop from 30% up, a
    little darker near black (10%: 0.81-0.96 against 1.03). The port's SDR output path is clean. Earlier raw runs over
    the game's own image read 50 and up to 83 at white: the screen's brightness limiting reacting to the surroundings.
  - Game patches through the game's grading at neutral Brightness (power 1, `--neutral-brightness`): measured =
    predicted (the offline port through the measured display response) within 1-4% from scene 0.05 up, in absolute
    cd/m2 (mid-grey 0.18: 19.4 against 19.1; scene 1.0: 74.0 against 73.2).
  - The same with the game's Brightness slider centred: a power of 0.758 on the graded colour (0.73-0.77 at every level),
    UE display gamma 2.90: mid-grey 29.4 cd/m2 (1.5x neutral), scene 0.05 2.6x. Neutral (power 1, display gamma 2.2)
    sits near 35% of the slider (30% read 1.985 in H28; linear in display gamma between the two points). The user's
    calibration (skull just visible) is ~25-30%, a little darker than neutral, on a screen measured at ~2.1.
  - So the over-bright default is the game's own slider mapping (the centre brightens), not the port: the port's output
    and its reproduction of the game's grading are both verified.
- **H30, HDR meter runs** (HDR 400 True Black, Windows HDR on; game: SDR grading kept, 203 nits, gamma 2.2) [V meter]:
  - Raw PQ codes (after the fix below): 20-400 nits within +-4%, peak 435 cd/m2; under ~10 nits the screen shows more
    than asked (10: 1.08x, 1: 1.42x, 0.1: 2.6x, 0.05: 3.2x); code 0 stays black. The first try packed the SDR surface
    format (B8G8R8A8) into the HDR10 image (white / magenta patches): `GetSurfaceFormat` stays the SDR format while the
    swapchain is HDR10; the presenter now passes the HDR format.
  - Scene greys through our HDR pass: measured = target through the screen's measured PQ response within 0.5-6% from
    scene 0.01 up; and = the SDR neutral run scaled to a 203-nit white within ~2-4% from 0.05 up (mid-grey 37.9 against
    39.4, scene 1.0 145.5 against 150.3). The SDR look is kept, measured.
  - The raised HDR black floor is the screen: the AW3225QF's near-black rises after pixel-refresh cycles (a known issue,
    the user, 2026-11-18); it also lifted SDR near-black (H29), so the user needed the game's Brightness lower on this
    monitor than on their TV. A fix belongs in an opt-in calibration from meter data, not in defaults.
- **H31, HDR highlights (step 2)** (2026-11-19; HDR 400 True Black, the user's own HDR ICC profile, near-black
  correction off, meter with the AW3225QF i1d3 ccmx; game at 203 nits, gamma 2.2, Peak brightness 450) [V meter]:
  - `game-highlights` with Highlights 0 (`20261119-214244`): scene 0.18 / 0.5 / 1 / 2 / 16 / 256 at 35.8 / 102.2 /
    141.9 / 167.3 / 195.4 / 203.2 nits, within 1-4% of the 2026-11-18 game-grey run: step 1's curve unchanged by the
    move to the shared include. The highlights-low reading of 2026-11-18 11:30 (white 173) did not come back.
  - Highlights 100 (`20261119-214748`): up to the knee within 0.5% of Highlights 0; above it 132 / 159 / 243 / 306 /
    391 / 416 nits at scene 0.75 / 1 / 2 / 4 / 16 / 32, flat at 420-422 from 64 to 256. Local log-log slope 0.66 just
    above the knee and 0.57-0.64 up to scene 2 (no kink), 0.38 from 2 to 3 where the roll-off starts (225 nits). The top
    is 0.94 of 450: the screen's tracking under PQ in this range, as white (190-195 against 203) in earlier runs.
  - Grey x/y 0.312 / 0.331 from 36 to 416 nits (the user's new per-channel calibration; slightly green of D65). At the
    peak (scene 64 and up) y drops to 0.326 while luminance barely moves: the user's monitor LUT (the correction for
    the screen's raised blacks and colour shifts) at the top of its range, not the port (the user, 2026-11-19).
  - The user, on screen: "it all looks good to me".
- **H32, the UI composite (step 3) and the black menu background** (2026-11-19):
  - Captures (replay only): `menu_frame19708.rdc` (11-17) is the weapons tab. The composite (eid 9318) gets an intact
    scene, but the UI image is opaque everywhere (alpha min 1.0). Pixel history of a background pixel: the clear, then
    eid 8746, a full-screen copy (no blend) of a black image writing (0, 0, 0, 1). Its chain: eid 8726 and 8736, two
    passes of shader `8c6fbe78cd6698bb` (a blur), whose source is **the UI image itself** (just cleared). UE's
    background-blur widget blurs what is already drawn; in SDR the UI goes onto the finished frame (the scene), in HDR
    into its own empty image, so the blur is black and covers the screen [V]. The other menu captures have no blur.
  - The game's composite squashes the scene under translucent UI to about (1 - a)^2, close to SDR's (1 - a)^2.2: not
    the black background (the old lead in plan-hdr-sdr-match.md) [V maths].
  - Channels: `draw_io.py` (now printing view swizzles) shows the composite's scene and UI views as `bgra`; the images
    store blue first, RenderDoc's values and PNGs show that stored order (LANDMINES.md 28).
  - Our composite (`ue4_ui_composite.frag`, release 22:45): first build through identity views: the pass-through right,
    the BT.709 -> 2020 matrix on swapped channels, greens mint (the user's screenshot). Fixed (release 23:05): the
    game's views, maths in true RGB, written `.bgr`. The user: "The intro movies look correct now ... the menu looks
    like the correct color scheme" [V the user].
- **H33, the blur backdrop and the game's Brightness on the UI** (2026-11-19 / 20):
  - Backdrop (release 23:45): before the blur's first pass samples the HDR UI image, our pass (the composite shader,
    flag 2) writes the UI so far over the scene in the SDR signal into a 4K RGBA8 image, and only that draw samples it
    (`PrepareHdrBlur`, patched into `BindTextures`, that draw kept off the bind helper). Log: `background blur reads
    our backdrop`. The user: the weapons tab fixed [V the user].
  - The map screen's level-select and back-to-main-menu popups stayed black. RenderDoc (`research-data/
    hdr-blur-menus-20261120/mapmenu_frame20618.rdc`, `26047`): a second first pass, `8c6fbe7824203398` (eid 7690),
    downsamples the UI image (1093) to 1920x1080, then the blur `8c6fbe78cd6698bb` runs twice at half size (7700,
    7710) and eid 7722 copies the result back [V]. Fix (release 00:27): both first passes recognised. Run
    00:44-00:47 (`user/logs/runs/run-20261120-004741`, `background blur reads our backdrop`): the user: both popups
    worked [V the user].
  - The game's Brightness reaches the UI too: every Slate pixel shader runs `if (cb[1] != 1) sRGB_encode(pow(c,
    cb[0]))` (fs `4fe20af683865713` IR); cb = (1.35385, 0.615385) in `menu_frame19708` = 2.2 / display gamma and its
    inverse, the grading block's dword 4 / 5 at the same setting [V]. The user's choice: no shader change; the
    launcher's Display > Game brightness > Set to neutral writes the default 0.3778 (display gamma 2.2) into every
    save slot (`scripts/save_brightness.py`: backup first, 4 bytes in place, file times kept; the slider steps 0.02
    from that default; slots without the property are at the default), and the UI brightness help says to leave the
    slider at its default.
- **H34, regression A/B against copy-fault-trace `22744f1`** (2026-11-20; release builds of both, saved in
  `C:\Users\Ty\MediEvil\baselines`; the user's graveyard run, Windows HDR on, flip trace on, periodic FPS line off;
  traces and logs in `research-data/regression-hdr-20261120`, figure `regression-4runs.png`). 105 s of gameplay from
  5 s after the level load:

  | Run | avg FPS | 1% | 0.1% | >6 ms / min | worst ms |
  |---|---|---|---|---|---|
  | A baseline, SDR | 219.6 | 152.7 | 128.1 | 114 | 13.5, 7.8, 7.7 |
  | B hdr, SDR | 218.1 | 148.0 | 127.0 | 279 | 12.4, 12.3, 7.9 |
  | B2 hdr, SDR (repeat) | 219.1 | 147.2 | 82.1 | 75 | 14.2, 14.1, 14.0 |
  | C hdr, HDR on | 215.5 | 170.3 | 149.7 | 13 | 12.1, 11.9, 6.5 |

  The frames over 6 ms are smooth 1-2 s rises in the fight; how high depends on the camera (the heavy background in
  view) and on how many enemies die at once (the user), so they swing 2-4x between runs of one build. B2's 0.1% low
  is one burst at 115 s (~30 frames 6-14 ms in half a second; `flip_slow_stages.py`: game + stage A +3.8 ms each,
  recording and present unchanged; nothing in the log): cause not found, seen once. Read: SDR within 0.2-0.7% of
  the baseline, HDR output ~1.7% [V run; the user: run-to-run variance].
- **H35, the SDR path made equal to the baseline by construction; three installs** (2026-11-20):
  - Review of every change since `22744f1` (two independent reviewers, read-only, plus mine): with HDR off, nothing
    above a few microseconds a frame; nothing that should be dev-only reaching the release build. Cleanups:
    `post_process.frag` back to its `22744f1` bytes and 12-byte push range, the near-black correction in
    `post_process_hdr.frag` with its own layout (HDR pipeline only); `Ue4HdrGrading` created only with "Allow HDR
    output" (or in the dev build), so the per-draw HDR test is one null check; the HDR / pattern members after the
    rasterizer's per-draw members and the HDR / grade fields after the older settings (their old offsets back); the
    test patterns dev-build only (compile-time constants in release); the live cvar poll checks one pending flag and
    its dev-only rows are compiled out; `hdr_active` stored on a change only. Fixes from the review: the UI level no
    longer requested in SDR at start, the saved menu tab 5, the overlay's HDR pipeline made outside the submit lock
    and not drawn with the SDR pipeline if it fails, `GOW3_HDR_GAME` parsed one way, non-finite grade values
    rejected, the composite's render-pass split after its checks, the launcher's save edit on a cut-off save.
  - Three side-by-side installs, same ini / saves / shader cache (`C:\Users\Ty\MediEvil`): `medievil_bestrun`
    (`21886b6` release: the best run's code `e12ffaa` plus off-cost steps 1-2), `medievil_cft` (`22744f1` release),
    `medievil_2019_pc_port` (this branch, release). The best run (screenshot 2026-11-15 19:28: 1% 233.8, 0.1% 227.9)
    was `e12ffaa`'s code in a static view with every log off, not `deck-lab`. The user's graveyard runs of all three
    plus repeatable heavy moments (particle bursts from hitting metal): about the same, within run-to-run variance;
    HDR on: works, looks as before; "significantly better than the 2.4.0 release" [V the user]. A finer A/B needs
    the repeatable stress test (work queue).
- **H36, FSR1 / RCAS in HDR, the near-black correction removed, Hilltop Mausoleum's grading builder** (2026-11-20 /
  21; captures `research-data/hdr-grading-20261121/cap_frame{7814,16057,22288,24343,82572}.rdc`, not in git):
  - FSR1 / RCAS: the presenter passed `frame->is_hdr` as the pass's `hdr` flag; that mode expects linear input and
    squares its output, which crushed PQ frames. PQ is a perceptual encoding like gamma, so the pass now always runs
    in its SDR mode. Run 2026-11-21: Sharpening and FSR1 in HDR look as without them, only sharper (the user).
  - The near-black correction (`hdr_black_correction`, `hdr-black-correction.txt`, `post_process_hdr.frag`,
    `measure_patches.py --write-correction`) removed: the user calibrates the display outside the game, the right
    place for a per-display fix. `pp_pass` is back to one shader (`post_process.frag`, `22744f1`'s bytes) for both
    pipelines.
  - Hilltop Mausoleum: the HDR rows and the Colour tab did nothing there (the user, 2026-11-20). Capture 82572: the
    level's grading draw (eid 15063) runs `fs 9f6bac6cba984956`, UE 4.21's builder with a level grading LUT
    (BLENDCOUNT 2) [V]: its block is 164 dwords, a415's with 20 inserted at 45 (LUTWeights; [0] at 48, [1] at 52,
    captured 0.65 / 0.35), plus a 256x16 RGBA8 texture (a 16^3 LUT unwrapped, `bgra` view) sampled linear / clamp to
    edge. Between the film curve and MappingPolynomial it clamps the colour to 0-1, sRGB-encodes it, reads two LUT
    slices lerped by fract(15 b), mixes `w0 * colour + w1 * lut`, floors at 6.1e-5 and decodes sRGB. Its HDR devices
    (OutputDevice >= 1) read the colour before the film curve [V, `lut_9f6b.glsl` 1358 on reads `_1572` / `_1575` /
    `_1583`]: the game's ACES output never shows the level LUT.
  - Port: `ue4_grade.glsl` `GRADE_LUT_BLEND` (`ue4_hdr_lut_blend_frag`); offline harness
    `research-data/hdr-grading-20261121/port9f6b` (the game's shader decompiled to C++ against the port, all 32^3
    texels, 12 cases: as captured, LUT only, no LUT, half and half, contrast, shadows gain, mid-tones offset, white
    temperature, film toe, blue correction, scale / overlay, legacy curve): all match, max |d| 9e-7, no NaN
    mismatches. Runtime: `ReplaceHdrGrading` and the Colour tab accept both builders; the 9f6b block is remapped to
    a415's layout (0-44 as is, 65-161 -> 45-141, the weights -> 142 / 143) and the level LUT bound through the
    game's view; the Colour tab writes the 9f6b block at the shifted dwords. Another 32^3 builder is logged once per
    shader (`unknown 32^3 LUT builder`). Run 2026-11-21: in Hilltop the Colour tab (SDR and HDR) and the HDR settings
    work; the Graveyard and the map look as before (the user).
  - Hilltop's HDR looks brighter than its ACES output, the reverse of other levels (the user). Offline
    (`port9f6b/highlights9f6b.cpp`, our HDR maths on the captured block): the level LUT lifts dark tones (scene 0.02
    grey: 0.006 of SDR white with it, 0.002 without) and mid-grey ~5%, and leaves the highlight slope at the knee as
    it is (0.62 with, 0.63 without; extension ~4% brighter, the mid-tone lift). So that is the level's own SDR haze,
    which ACES skips [V above]. Open: Hilltop HDR against Hilltop SDR, Highlights 0 and 100 (the user).
- **H37, the levels' grading tables, the Graveyard's "blue push", gamma by brightness only; HDR rows in the Color
  tab** (2026-11-21):
  - The game's tables [V, pak index + map files, read-only scans in the session scratchpad]: three from the Amplify
    LUT Pack (`AdditionalPackages/AmplifyLUTPack`: Base/BleachAlternative, PopularLooks/80sPost-ApocalypticAction
    and -Drama) and `FX/LUTs/LUT_Fall`. Post-process volumes with the table and its intensity switched on
    (`bOverride_ColorGradingLUT` / `_Intensity`; intensity = LUTWeights[1]): BleachAlternative in 00 DC 0.40 (a
    local volume, no `bUnbound`), 03 HM 0.35, 08 SV 0.30, 09 AG 0.28, 07 PS 0.25, 21 ZL 0.25, 10 IA 0.10, 19 EH 0.10
    (all unbound); LUT_Fall 0.30 in the Battlefield cinematic map (`Sequences/.../Level_00_AV/Level_Battlefield`).
    Named but off: 80sPost-ApocalypticDrama in 01 GY1 / 02 CH / 04 GY2 (no override, or the volume disabled /
    BlendWeight 0), BleachAlternative in 06 PG (intensity only), 20 TD and `IN_MM` (volume disabled). Not scanned:
    blueprints or cameras that might set a table at runtime.
  - What BleachAlternative does (the captured texture; true RGB = stored bytes 2, 1, 0, checked by each output
    channel following its own input axis): in the sRGB-encoded domain after the film curve, black to 16% signal,
    mid-grey +21%, white -4%, saturation x0.78 (shadows x0.62, blues x0.64), dark greys slightly pink, yellows and
    cyans turned ~15 degrees. At Hilltop's 0.35: black 5.6% signal, mid-grey +7%, saturation x0.93 (shadows x0.89,
    blues x0.87), hue shifts under 5 degrees.
  - The live grading blocks (menu `eid3495`, Graveyard capture 16057 our pass eid 13215, Hilltop 82572 remapped) are
    all at the engine defaults (white balance 6500 / 0, every saturation / contrast / gamma / gain 1, offsets 0, film
    0.88 / 0.55 / 0.26 / 0 / 0.04, blue correction 0.6, expand gamut 1): the levels' only grading is the table. The
    Graveyard's volume stores a full grade (WhiteTemp 7414, saturation 1.52, warm gains) with only its exposure
    overrides on. MapScreen's volume grades for real (WhiteTemp 10829, contrast 1.22, gain 0.74 / 0.58 / 0.56 x 1.39,
    shadow gain 0.56 / 0.16 / 0.23, film slope 0.92 / toe 0.52; LUT override with no texture) [V file; live block
    not captured].
  - The Graveyard's "blue push" in dark tones (the user): not the level's grading (neutral, above). Gamma 2.2
    emulated per channel darkens a colour's dim channels more: for a dark bluish signal (0.10, 0.12, 0.18) at 203
    nits blue's share is 0.537 through the sRGB curve, 0.594 per channel at 2.2; the same 2.2 luminance with the sRGB
    curve's colour keeps 0.537; above ~10 nits all three are within ~1%. The user: Gamma Off lessens the blue.
  - Fix: "Gamma applies to" (`hdr_gamma_mode=brightness|channel`, default brightness): `HdrSdrLook` scales the sRGB
    colour to the gamma screen's luminance (push flag bit 1 = per channel, as before). Run 2026-11-21, main menu
    screenshots 09.37.41 (per channel) / 09.37.54 (brightness only), same view: blue share 0.476 -> 0.441 at
    1.5-3.5 nits, 0.488 -> 0.472 at 0.5-1.5, 0.379 -> 0.361 below 0.5; mean 7.46 / 7.38 nits; above 3.5 nits
    unchanged. The user: "the shadows no longer have the blue push", closer to SDR.
  - The HDR rows moved to the top of the Color tab (`HdrRows`), so the "Adjust while playing" panel shows them; the
    Display tab keeps a pointer. Run: the rows work in the panel (the screenshots above).
- **H38, the purple shift in dark tones: the grading LUT's resolution near black** (2026-11-21; found, checked
  offline, plan agreed, not built):
  - The user's comparison (`Medievil comparison/`, SDR 09.02.38 against HDR "brightness only" 09.37.54, main menu):
    the dark bands' red share 0.09 -> 0.22, a hue shift of ~+12 degrees towards purple, changing the grade across the
    image.
  - Cause [V, IR + simulation]: the HDR tonemapper (`fs 12178a9314cb761e`, IR lines 159-161 / 198-202) indexes the
    32^3 LUT at `PQ(scene * 0.0001) * 0.96875 + 0.015625` (texels at scene 0, 0.023, 0.108, 0.297, ...) and samples
    trilinearly; everything darker than scene ~0.02 lands in the first cell, where trilinear blending mixes in texels
    of other hues, and PQ's steepness near black amplifies the error. The SDR path indexes by log2
    (`LutDecodeSdrTexel`, ~10 texels below 0.02), so SDR does not show it. Simulation
    (`research-data/hdr-grading-20261121/port9f6b/lutres.cpp`, menu block, 222 nits, 2.2, peak 435): grey at scene
    0.01 comes out 6-8x too bright with red share 0.40; dark blue-teal at 0.02 x5.2 with red share 0.188 (exact 0.010).
  - Candidates simulated (`lutres_v.cpp`, `lutres_shaper.cpp`, `lutres_pt.cpp`, `lutres_x.cpp`):
    - BT.709 storage only: less red, blacks still lifted. BT.709 plus a shaper: brightness right, teal too blue.
    - Pass-through LUT plus our own 48^3 log LUT in the composite: colours exact from scene 0.02, but one 10-bit code
      changes darks by 10-37% and the tonemapper's grain (+-1/512) becomes +-20% at 3 nits (3-6x SDR's); would need a
      16F target. A bent pass-through (codes per cell 182, 283, 175, 84, ...) keeps steps at 1.2-8%, grain as SDR.
    - **x100 input scale** (as UE5 and later 4.x, per the research below): the tonemapper's three `#0.0001` -> `0.01`
      and our pass decoding its texels x100 (`LutDecodeHdrTexel`, `ue4_grade.glsl`): dark blue-teal at 0.02 x1.22
      with red share 0.015, at 0.04 x1.05 / 0.021; grey at 0.01 x1.36 / 0.345, at 0.02 x1.05; from 0.08 up within 1%.
      Output precision and grain as today, no per-frame cost. Clips above scene 100: no change at peak 435; at peak
      1000 highlights past scene 100 stop at 977 nits (exact reaches 1000 at 200); at 2000, 1734 (exact 1872 at 200).
  - Research (three agents, 2026-11-21) [docs and public mods; Unreal source not read]:
    - Epic's docs: the film curve is off for HDR output and grading LUTs are SDR-only, by design in every version; the
      game's HDR (ACES look) drops the level tables for that reason. Our "SDR grading kept" is not overbuilt: an
      extended-filmic route still needs the grading chain and the 2.2 handling and is less faithful; its one gain is
      highlight saturation. The quoted `r.LUT.Size=64` line is not in Epic's docs; 32 is baked into both of this
      game's shaders (builder dispatch and the tonemapper's 0.96875 / 0.015625).
    - A larger LUT (64^3, the user's question): possible only by patching the tonemapper's constants, allocating our
      own 64^3 image and rebinding it, and an 8x larger build of our pass; its first texel moves from scene 0.023 to
      ~0.006 (about 4x finer near black). The x100 scale moves it to ~0.0002 for no cost, so 64^3 is not planned.
    - RenoDX keeps UE's sampling and upgrades the LUT to 16F, with no fix for the x0.0001 era; Luma lists the issue as
      a TODO. Later UE versions index `PQ(scene * 0.01)` (UE5 takes LUTScale / LUTOffset from a cbuffer).
    - The UE 4.21 SDK: its licence forbids combining engine code with GPL code; reading is allowed but the user chose
      to skip it (2026-11-21): no Unreal source in this work.
    - SDR by measurement on the user's calibrated display: ~100 patches, per-channel curves plus a 3x3 matrix,
      Windows HDR off, dark room, ABL settling, meter unreliable below ~0.05 nits. A later option for choosing between
      "brightness only" and "each colour" gamma, not needed for this fix.
  - Plan (the user: "sounds like a good plan", 2026-11-21; the exact change proposed before coding): a tonemapper variant through the shader recompiler, as `RefractionScalePass`
    does for the distortion shaders: an IR pass marks `12178a9314cb761e` (`Info` flag), and in the variant whose
    `HwFragmentRuntimeInfo` field is set, multiplies the three `FPMul32 #0.0001` (each one use, straight into PQ, no
    clamp before the sample) by 100. The pipeline cache picks the variant while HDR look is "SDR grading kept"; the
    ACES look keeps the game's shader, since the game's builder assumes 0.0001. Our pass decodes x100 under a push
    flag tied to the same setting, so a live HDR look switch changes both (a one-frame mismatch at the switch at
    most). Patching the shader files from the launcher would also work but is fragile and not switchable live; the
    recompiler pass is reversible and per setting.
  - Fix built (2026-11-21, not run): `HdrLutScalePass` (`shader_recompiler/ir/passes/hdr_lut_scale_pass.cpp`): in
    the variant with `HwFragmentRuntimeInfo::hdr_lut_x100`, the HDR tonemapper's three `FPMul32 #0.0001` become
    `#0.01` (exactly three found, else the game's index kept and logged). `PipelineCache::hdr_lut_x100` is set at
    every scene LUT build by `ReplaceHdrGrading`: true when our pass built it (`Ue4HdrGrading::ScaledIndex()`),
    false when any path kept the game's draw (ACES look, SDR output, declines, an unknown 32^3 builder); the
    tonemapper draw picks the variant from it, prepared draws re-check it. Our pass: push flag bit 2, texel scene
    x 0.01 (`ue4_hdr_lut.frag`). The tonemapper compiles synchronously (`MustCompileSync`), so a variant switch drops
    no frame. Shader cache versions bumped (binary 15, meta 15, pipeline key 13): the first start recompiles.
    `GOW3_HDR_LUT_X100=0`: the game's index (A/B, read once). Log: "UE4 HDR grading: LUT index ..." and "HDR
    tonemapper: LUT index at PQ(scene x 0.01) (3 constants)".
  - Run 2026-11-21 (launched by the user; build 12:26:15; log: "LUT index PQ(scene x 0.01) (HDR tonemapper
    variant)", "HDR tonemapper: LUT index at PQ(scene x 0.01) (3 constants)", the tonemapper compiled synchronously).
    Title screen, 174 nits, gamma 2.2, peak 434: 12.39.48 (brightness only) and 12.42.45 (each colour) against
    09.37.54 (before, brightness only) and SDR 09.02.38 (png by 2.2), banded by the SDR shot's luminance, 4x4 block
    means, the centre column and the right quarter left out (animated: they differ between shots of one view;
    scratchpad `jxrtool/bands_keep.py`, `grid.py`). Darkest 28% (SDR Y < 0.003; SDR red share 0.03-0.05, blue
    0.77-0.85, ~0.17-0.5 nits at 174): before 0.55-1.1 nits, red 0.20-0.21, blue 0.51-0.52; brightness only
    0.15-0.35 nits, red 0.07-0.09, blue 0.64-0.69; each colour 0.3-0.7 nits, red 0.14-0.16. Dark areas typically
    ~0.5x as bright as before (cell medians 0.24-0.8). Contact sheet (`jxrtool/sheet.py`): the "Color" overlay
    panel in the right quarter of 09.37.54 / 12.42.45, and the menu selection differs (SDR "New Game", the new shots
    "Load Game"); with the menu text also left out (`bands_keep2.py`): 0.8-1.6 nits (SDR Y 0.003-0.012) red share
    0.11-0.14 against SDR 0.09-0.12 (before 0.21-0.24); one band (SDR Y 0.025-0.05, ~6 nits) red 0.16 against
    before 0.11 / SDR 0.06, open (the rain streaks differ between shots). ACES look, same view (13.01.36): shadows
    0.46-0.68 nits red 0.11; mid-darks darker (7.8 against 9.6 nits) and redder (0.20 against SDR 0.08);
    highlights brighter (86 against 56 nits) and whiter (blue share 0.22 against SDR 0.14). The user: "shadows a lot more depth,
    they're no longer blown out and elevated ... higher contrast look without destroying the brightness or altering
    gamma"; brightness only still looks clearly better than each colour and closer to SDR.
  - **The map screen black with x100 (found, 2026-11-21):** the map draws its scene with a second HDR tonemapper,
    `fs 52770387e0d6363a` (10 samples instead of 4; the same three `FPMul32 #0.0001` (IR %400 / %404 / %406), the
    0.96875 / 0.015625 index (%439-441), the 3D LUT sample (%443) and the 493013 grain). `HdrLutScalePass` and the
    pipeline cache match `12178a9314cb761e` only, so on the map our x100 LUT was read at x 0.0001: ~100x too dark,
    black under the map's grading (screenshot 13.02.43: 97.5% of pixels exactly 0). Captures
    (`research-data/hdr-map-20261121/`): `x100off_frame21943.rdc` (`GOW3_HDR_LUT_X100=0`, the map works) and
    `x100on_frame10168.rdc` (black): one LUT build per frame, ours (eid 7717: our push constants; the map's grading
    block, WhiteTemp 10828.6, contrast 1.22, gain 0.74 / 0.58 / 0.555 x 1.39, shadow gain 0.56 / 0.16 / 0.23), then
    the tonemapper (eid 7742, 10 samples, `0.0001` in its SPIR-V), no draw between. The pre-patch map capture
    (`hdr-blur-menus-20261120/mapmenu_frame20618.rdc`, eids 7500 / 7525) has the same pair. The map's grading through
    our pass at x100 is valid (lutres_x with that block: no NaN, within 0-18% from scene 0.006).
    Fix built (2026-11-21 15:13, not run): `HdrLutScalePass` recognises HDR tonemappers by structure (exactly three
    `FPMul32 #0.0001`, three `FPFma32 #0.96875, x, #0.015625`, the grain's `FPMul32 #493013`; the UI composite
    `c401ca35f9bceb8e` has six and no grain) and marks them `Info::uses_hdr_lut_index`; the pipeline cache picks the
    x100 variant by that flag; flagged shaders and both known hashes compile synchronously; near misses are logged
    ("HDR tonemapper-like shader not patched"); "HDR tonemapper recognised: fs ..." once per program and variant.
    Shader cache versions: binary / meta 16, pipeline key 14. Run 2026-11-21 ~15:15-15:25 (build 15:13:04): log
    "HDR tonemapper recognised" for `12178a9314cb761e` and `52770387e0d6363a`, each with its x 0.01 variant; no
    near-miss line. Map screenshot 15.22.56: 0.05% of pixels exactly 0 (13.02.43: 97.5%), median 2.9 nits. The
    user: the map is back; other areas and a level not visited before look right.
  - The user (2026-11-21): if the ACES look cannot be made good, drop it; any alternative look becomes our own HDR
    grading, not the game's ACES path.
  - Tonemapper cvars (two research agents, 2026-11-21; eboot strings and IR [V], gains [I]):
    - The grain is `r.Tonemapper.GrainQuantization` (eboot help: "high frequency pixel pattern to fight 8 bit color
      quantization"): both tonemappers add `fract(sin(...) * 493013) / 256 - 1/512` per channel after LUT x 1.05
      (HDR IR %131, %209-217; SDR `user/shader/dumps-sdr-20261029/fs_0x3632d4c4faaa85d2`, %131, %182-190), a literal
      amplitude, no cbuffer gate: a cooked permutation. In HDR it is ~+-2 10-bit PQ codes; Epic's profiling page:
      "not needed when we output in 10:10:10". Setting the cvar picks another cooked variant (if any; else likely a
      missing-shader failure) with another hash, which the x100 variant would miss. Removing it in HDR belongs in the
      same recompiler pass (a separate change and run after x100).
    - No sharpen path in either tonemapper (four samples: one at a FragCoord UV, one at the interpolated UV, one at it clamped to cb0 dwords 20-23, the LUT; no neighbour taps;
      a vignette term is present): `r.Tonemapper.Sharpen` has nothing to drive here. Film grain proper is compiled
      out. TAA cvars in the eboot (`r.TemporalAACatmullRom`, `r.TemporalAAFilterSize`,
      `r.TemporalAACurrentFrameWeight`) are the leads for a sharpness setting.
    - Keep tonemapper-variant cvars at their defaults (`r.Tonemapper.Quality`, `GrainQuantization`, `Sharpen`,
      `ConfigIndexOverride`, `MergeWithUpscale.*`, `r.TonemapperFilm`): each can change the hash the HDR work keys on.
      Keep `r.EyeAdaptation.PreExposureOverride` 0 (it would shift every LUT index).
    - FXAA: the game runs TAA (no FXAA pass); the alpha luma is baked into the tonemapper. In HDR the target's alpha
      has 2 bits, so `ue4_aa=fxaa` with HDR would feed FXAA a useless luma.
    - Saved outside the repo (third-party pages): `C:\Users\Ty\MediEvil\reference-pages\` (framedsc UE4 4.27 cvar
      list as HTML and markdown, Epic's 4.27 "feature levels" profiling page, and the agent's findings with an
      eboot presence check: 1,420 of 4,594 entries present); summary in `notes-ue4-cvars-framedsc.md`.
- **H39, the curve's shape: highlights start, highlight contrast, roll-off start** (2026-11-21; fix built, not run):
  - The user: volumetric fog flattens the scene and brightens it (SDR too); after a level with milder fog, more about
    our curve: keep the game's dark night look (game brightness 125 looks like night; 203 does not, screenshots
    16.15.17 / 16.16.15) but spend the headroom on speculars, lit areas, fire and bright saturated colours; "not much
    qualifies as a highlight outside of the brightest torches"; the shadow clip point looks right.
  - The curve (`research-data/hdr-grading-20261121/port9f6b/curve.cpp`, grey ramp, engine-default grading, gamma 2.2
    brightness only, peak 435): below Y(U) 0.5 HDR is exactly the SDR look x game brightness (mid grey 0.18 -> 31.9
    nits at 175, 22.8 at 125; black below scene ~0.004; the toe's log-log slope 2-3, mid-tones below 1 from ~0.3);
    above, the SDR slope at the knee (0.63) continues to the roll-off (from half the peak), the peak at scene ~50.
  - Where the pixels are (the 125 shot mapped back to scene values through the current curve): p50 scene 0.017, p90
    0.079 (6 nits), p99 0.21 (28), p99.9 0.46 (62); 0.019% above 100 nits (fire, max 313). The local slope at p90-p99
    is 1.3-1.7: the SDR curve does not compress this scene's lit areas; they are dim in the game's grading. The knee
    at 0.5 sees ~0.1% of the pixels.
  - Variants plotted (scratchpad `curve_variants.py`; at 125, nits at p50 / p90 / p99 / p99.9 / scene 1 / 2 / 5):
    now 0.18 / 6.1 / 28 / 62 / 102 / 158 / 267; knee 0.25 slope 1.0: 63 / 128 / 250 / 367 above p99; knee 0.1 slope
    1.25: 75 / 198 / 337 / 425; knee 0.1 slope 1.5: 94 / 278 / 389 / 435; p99 and below unchanged in all. A softer
    toe (lift below scene 0.1, clip kept) brightened the shadows (scene 0.05 2.6 -> 3.9 nits): dropped. The roll-off
    is abrupt in every variant (slope from 1.25-1.5 to ~0.3 within a stop near 220 nits).
  - Built: three live rows under Highlights (Color tab > HDR): **Highlights start** (`hdr_highlight_start`, 10-50% of
    SDR white in Y(U), default 50), **Highlight contrast** (`hdr_highlight_contrast`, 0-150 = log-log slope x 100;
    default 0 = the code before H39, the knee's SDR slope continued; > 0: `HdrHighlightBoost` walks the colour's own
    SDR curve up from the knee in half stops (8 steps, four stops; beyond, the last slope continues) and lifts it
    wherever its slope falls below the target, eased in over the first stop: up to 9 more GradeScene calls per LUT
    texel above the knee, none at the default), **Roll-off start** (`hdr_rolloff_start`, 20-50% of the peak, default
    50; W = (4 peak - knee) / (peak - knee), slope 1 at the knee, the peak at 4x peak as before). Defaults reproduce
    the curve before H39 exactly. Push constants: LutPush and PatchPush (the pattern shader's pads) carry knee,
    target, roll-off start. The harness re-states the shader's walk: knee 0.1 / 1.25 -> 74.6 / 197 / 335 / 416 (the
    plotted design 75.0 / 198 / 337 / 425); 1.5 -> 93.7 / 278 / 388 / 435; 1.25 with roll-off from 30% -> 74.6 / 185
    / 295 / 404.
  - Run 1 (2026-11-21 ~17:10, build 17:05:17): the user: the sliders seem to do nothing; some change on very bright
    things near white only. The build's shader has the new fields (SPIR-V names checked). As the numbers predicted: at
    125 nits the p90-p99 pixels sit where the SDR slope is 1.3-1.7, so targets up to 1.5 lift only the top ~0.5%
    (start 10%, contrast 150: p99 x1.03, p99.9 x1.5). Wider: start 8% / contrast 200 / roll-off 40%: p99 x1.37,
    p99.9 x3.0, scene 1 x3.7; contrast 250: p99 x1.87, p99.9 x4.6; p90 and below unchanged. Ranges widened (build
    17:28): start 5-50%, contrast 0-300.
  - Build 17:37:11: the row renamed "Highlights begin at" (the Color tab's grading row "Highlights start",
    `GradeHighlightsMin`, had the same label; `grade_highlights_min` in the ini stayed at its stored default 0).
  - Run 2 (2026-11-21, build 17:37): the user: the rows do work; their effect depends strongly on game brightness,
    peak and where in the level you stand, "Highlights begin at" and "Roll-off start" most. Why: the start picks
    which pixels are lifted (scene-dependent: a torch-lit room has many above it, a moonlit field few); the roll-off
    start is in nits (a fraction of the peak), so it only acts on pixels the lift pushes past it, which depends on
    game brightness x lift (at 125 / 435 only fire reaches 217 nits without a strong lift).
    Later: the saturation-weighted boost (the 3D LUT can give bright saturated colours more headroom than greys).
- **H40, a piecewise HDR curve in place of the game's film curve** (2026-11-21; fix built, not run):
  - The user: the H39 rows depend on each other and on game brightness / peak / the area; wants a toe / straight /
    shoulder curve with a control per part (their reference plot: toe length / strength, shoulder length / strength,
    gamma) plus a fine **toe clamp**; agreed: HDR only, the game's colour grading kept, its film curve replaced, the
    current curve kept as a mode, game brightness pins mid grey (scene 0.18 -> 18% of it).
  - Reference: Hable, "Filmic Tonemapping with Piecewise Power Curves" (2017), saved in
    `C:\Users\Ty\MediEvil\reference-pages\filmicworlds-piecewise-power-curves.{html,md}`: toe and shoulder are power
    curves matched in value and slope to the middle (`B = m x0 / y0`), the shoulder flipped; overshoot (shoulder
    angle) because the plain flipped shoulder is nearly flat for its last stretch; toe length in a 2.2 power space for
    fine control; he prefers the curve per channel (bright colours fade to white) over luminance only. The Uncharted 2
    curve (A-F, W 11.2; the user's note) is the older rational form he replaced (all-or-nothing, odd when pushed).
  - Offline fit (scratchpad `h40_fit.py`, chart `h40_fit.png`): s' = max(s - clamp, 0); middle G 0.18 ((s' - b) /
    (0.18 - b))^gamma, b = toe strength x toe length; power toe below toe length (B = gamma / (1 - strength));
    Hable's flipped shoulder on those nits from r x peak, reaching the peak L stops later. Fit to the current curve at
    125 / 435 over scene 0.002-0.35: clamp 0.0032, toe length 0.033, strength 0.45, gamma 1.28 (toe power 2.35);
    rms 0.29% of the PQ range. Nits at p50 / p90 / p99 / p99.9 / scene 1 / 2 / 5: now 0.18 / 6.1 / 28.7 / 62 / 102
    / 158 / 267; fitted 0.17 / 6.2 / 27.9 / 80 / 222 / 395 / 435. The shoulder without overshoot (B 7 at r 0.5,
    L 2) is within 1% of the peak from 2x peak unrolled (scene ~2.3): needs Hable's shoulder angle.
  - Found while building: the level's grading LUT (9f6b builder, LUTWeights 0.65 / 0.35 in the captured block), the
    mapping polynomial and colour scale / overlay apply after the film curve, so U (before it) lacks them. The colour
    is therefore G's, re-toned: G x curve(Y(U)) / Y(G of a grey of the same Y(U)) (a grey gets the curve exactly, a
    colour keeps its luminance relative to grey as the game grades it; one more GradeScene per LUT texel).
  - Built (2026-11-21 21:20, fix built, not run): **Tone curve** row (`hdr_tone_curve=sdr_look|piecewise`, default the
    SDR look) and, in Piecewise, Mid contrast (`hdr_pw_gamma` x100, 0.80-2.00, 1.28), Toe clamp (`hdr_pw_toe_clamp`
    x1e-4, 0-0.0100, 0.0032), Toe length (`hdr_pw_toe_length` x1e-3, 0.005-0.150, 0.033), Toe strength
    (`hdr_pw_toe_strength` x100, 0-0.90, 0.45), Shoulder start (`hdr_pw_shoulder_start`, 20-90% of peak, 50), Shoulder
    length (`hdr_pw_shoulder_length` x10 stops, 0.5-5.0, 2.0), Shoulder angle (`hdr_pw_shoulder_angle`, 0-100%, 50:
    a blend of Hable's flipped power shoulder (0, flat end) and a Reinhard shoulder (100, rises to the peak); both slope
    1 at the start and reach the peak at the end, so no overshoot solve), Highlight saturation (`hdr_pw_saturation`,
    0-100%, 0: U's colour blended in by smoothstep(0.4, 0.9, Y(G grey))). Gamma / Gamma applies to / Highlights / H39
    rows are hidden in Piecewise. Constants derived on the CPU (vk_ue4_hdr.cpp PiecewiseFromSettings), pushed as three
    vec4s (LutPush and PatchPush 96 bytes); flag bit 3.
  - Peak limit: a saturated colour could put one channel far above the peak (orange (4, 1.2, 0.2) at saturation 100:
    R 918 nits at peak 435). Now no channel exceeds the peak: saturation 0 pulls towards the colour's luminance (fades
    to white), 100 scales the colour down whole (stays orange: 435 / 92 / 7), blended by the saturation.
  - Checked offline (`port9f6b/h40check.cpp`, the shader's H40 section via `gen_h40.py` over the port's GradeScene):
    grey ramp within 0.00075 PQ of `h40_fit.py`'s model below scene 2 (angle 0); GLSL compiles (lut, blend, pattern).
  - Blue push lead: a dark blue (scene 0.02 / 0.03 / 0.08) has U 0.020 / 0.030 / 0.080 but G 0.0052 / 0.0100 /
    0.0496 (B/R 4 -> 9.5): the film curve's per-channel toe adds saturation in the shadows (Hable notes it), in SDR too.
    Piecewise keeps G's colour, so it keeps this; a "Shadow saturation" row (U's colour in the toe) would remove it.
  - Run 1 (2026-11-21 ~21:25, build 21:20:24; log: "piecewise tone curve"; ini: game brightness 175, toe clamp
    0.0038, the old curve's gamma mode "channel"): the user: looks better already; the asymmetrical chroma push
    appears reduced or gone; looks more like SDR. Baseline pair old / new: screenshots 21.27.39 and 21.27.58 (.jxr),
    not yet analysed. (A stray `find /` of ours ran during this run: not a performance run.)
  - The user, after several levels: looks quite good when adjusted for the level; the settings that look good vary
    strongly per level (the per-level grading, its LUT likely among the causes; every implementation so far has been a
    compromise), but the new rows make a good result possible on every level played. Next: per-level settings, or
    find what differs per level and compensate it (work queue "level grading strength").
- **H41, what the game varies per level / area: the mechanisms first** (2026-11-21; found [V, static], live check
  open). The user: find every mechanism before tuning any more settings; settings data only once the mechanisms are
  known. Plan: (1) static scan of the game's maps + existing captures (no launch), (2) a dev-build event log of the
  grading block, the tonemapper's constants and exposure, (3) offline analysis per area, (4) then decide on
  compensation.
  - Static scan (`research-data/hdr-grading-20261121/pp-scan/`: `pp_scan_all.py` reads every map in the paks, 266, read
    only; `pp_active.py` keeps the settings whose `bOverride_` flag is on; reports `pp_all.txt`, `pp_active.txt`): 71
    maps carry post-process or fog settings. Active per level (main unbound volume unless noted; LUT = BleachAlternative
    intensity):
    - 00 DC: WhiteTemp 7000, saturation 1.01 (shadows 1.15), gamma 1.1, film slope 0.94 / toe 0.45, bloom 3 (threshold
      0.1, wide), LUT 0.40 (a local volume, earlier note).
    - 01 GY1 / 04 GY2: bloom 20 (threshold -0.06 / 0.11), exposure min = max 1.0, vignette 0.71; no grading (the live
      blocks: engine defaults, matches).
    - 02 CH: bloom 5 (threshold 0.015), exposure 0.88-1.25 (the only adaptive range), vignette 0.71, shadow contrast 1.02.
    - 03 HM: LUT 0.35 only (matches the live block).
    - 06 PG: film 0.9 / 0.6, LUT intensity 0.1 without a table. 07 PS: saturation 0.95, film 1.0 / 0.62, LUT 0.25.
    - 08 SV: WhiteTemp 5986, contrast 1.1, gamma 0.94, LUT 0.30. 09 AG: film 0.9 / 0.6, LUT 0.28.
    - 10 IA: an unbound volume (contrast 1.3, exposure 1.0, LUT 0.10) plus seven local volumes, priorities 2-3: each its
      own white balance (4700-5521 K), gain (0.65-1.05; one warm 0.8 / 0.57 / 0.49), film slope 0.82-0.98 / toe
      0.55-0.66, bloom 3.
    - 11 EE: two full grades disabled; active: WhiteTemp 5500, warm gain 1.19 / 1.16 / 1.29, film slope 0.95.
    - 12 AC: film 0.9 / 0.565. 14 LA: film 0.84 / 0.76 (a volume without bUnbound: local) plus a local volume (blend radius 850): 7580 K,
      gain 0.64 / 0.77 / 0.95 x 1.55, shadow gain 1.34 / 1.60 / 1.79, shadow offset -0.30 red, film 1.0 / 0.60, bloom 4.1.
    - 15 CC, 05 SF, 13 PD: no grading (fog only). 16 GG: 5350 K, shadow gain 1.37 / 0.85 / 0.98.
    - 17 HR: two local volumes (11386 K, gain 0.63, offset -0.01, film slope 1.0; 7800 K, shadow gain 0.69 / 0.82 / 0.71,
      film 0.886, bloom 1.2 threshold -0.31) and an art volume (8500 K, saturation 0.95, contrast 1.1, gamma 1.05,
      gain 0.89, bloom 2 threshold 8, vignette 0.5).
    - 18 GS: 5086 K. 19 EH: 7400 K, contrast 1.05, shadow gain 0.74 / 0.82 / 0.95, film toe 0.43, black clip 0.0002,
      bloom 4.1 (threshold -0.15, wide), LUT 0.10.
    - 20 TD: two volumes disabled; active: 5500 K, film 0.95 / 0.52, bloom 7.3. 21 ZL: contrast 1.05, exposure 1.0,
      LUT 0.25. 22 HH: 7657 K, saturation 1.0 / 0.61 / 0.62 x 1.06, gamma 1.01, gain 1.5 / 1.47 / 1.43, film 0.775 /
      0.579.
    - MapScreen: manual exposure (bias 10.35, shutter 140; the method's enum misread by the scanner), the full grade
      noted earlier, grain 0.1. Main menu (IN_MM): its volume disabled; vignette 1.0 in the persistent map.
      Battlefield cinematic: bloom 8, grain 0.2, LUT_Fall 0.3.
    - Fog: every level has its own height fog and most volumetric fog (density, inscattering colour, albedo,
      extinction), e.g. 14 LA density 2.5 x 10, 08 SV 0.25, 21 ZL 0.25.
  - Where each acts (ue4_grade.glsl and the tonemapper): white balance, saturation / contrast / gamma / gain / offset
    (U, before the film curve), bloom (added in the tonemapper before the LUT), fog, vignette and exposure all reach Y(U):
    both our curves see them. The film curve (slope / toe / black clip) and the table's effect on greys act after: the
    H40 piecewise curve re-tones greys to its own curve, so in Piecewise **each area's film curve and its table's tone
    are dropped** (only their colour relative to grey is kept). The levels' film slopes run 0.775-1.0 and toes
    0.43-0.76 (engine 0.88 / 0.55): one likely reason one set of Piecewise settings does not fit all levels.
  - Exposure: fixed at 1.0 almost everywhere (project `r.DefaultFeature.AutoExposure=False`; GY / IA / TD / ZL pin min
    = max 1.0); only CH adapts (0.88-1.25), MapScreen is manual. Likely not a main factor outside CH.
  - Not covered by the scan: blueprints, cameras and sequences that change post-process at runtime; how volumes blend
    (local volumes in IA, LA, HR). Next: the live check from existing captures (DC crypt, SV, HH, HM, GY, map): the
    grading blocks against this table, and the tonemapper's constants (bloom, vignette, exposure) as the GPU sees them.
  - Live check (2026-11-22, confirmed; `research-data/hdr-grading-20261121/survey/`: scratchpad
    `rd_survey/grading_survey.py` replays a capture and dumps every grading LUT builder's and tonemapper's buffers and
    1x1 textures (by pixel shader hash, else any draw reading a 32^3 colour LUT); `decode_survey.py` decodes them; 14
    captures). Tonemapper inputs per area (SDR tonemapper IR, fs 3632d4c4faaa85d2): scene x scene scale (cb0 dw 8-10)
    + bloom x (bloom scale dw 12-14 + dirt-mask tint cb1 x dirt mask), x exposure (vertex shader, the 1x1 R32G32
    eye-adaptation texture) x vignette (dw 16 intensity), then the LUT; the map's variant has the same fields 4 dwords
    later plus grain (0.1, jitter 0.0005).
    - The grading blocks equal the static table: DC (LUT 0.40, film 0.94 / 0.45, 7000 K, gamma 1.1, saturation 1.01,
      shadows 1.15), HH (film 0.7752 / 0.5786, 7657 K / -0.2857, saturation 1.0 / 0.61 / 0.6165 x 1.06, gamma 1.01, gain
      1.5 / 1.4675 / 1.425), SV in two areas (LUT 0.30, 5986 K, contrast 1.1, gamma 0.94), the Mausoleum and Hilltop
      (LUT 0.35 only); the Graveyard's known defaults.
    - The tonemapper: bloom scale = the volume's BloomIntensity (DC 3, Graveyard 20 in three captures, else UE4's
      0.675), vignette 0.4 (Graveyard 0.7143); scene scale 1 and dirt-mask tint 0 everywhere.
    - Exposure: 1.0 in every level capture; the map screen 0.991 (its manual exposure). The calibration / menu
      captures (cft 12411, run28 26066) are defaults.
    - DC also builds a LUT and tonemaps once more early in the frame (InverseGamma 2.2, bloom 0; 1024^2 targets, also
      in other captures: a scene capture, not the screen).
  - Found: everything the game varies per area that reaches the picture is in the grading block (which our pass
    already reads at every LUT build) plus bloom intensity and vignette (which act on the scene before the LUT, so
    any curve of ours sees them already); no runtime exposure changes in the levels captured. Not seen yet: CH (the
    adaptive exposure 0.88-1.25), IA / LA / HR's local volumes blending.
  - Direction (the user, 2026-11-22): keep our controls (toe length / clamp / strength, mid contrast, game
    brightness, peak, ...); the game's grading blocks are not applied as they are but fed into our curve per level /
    area, with the user's adjustments kept on top: the game's raised blacks get exaggerated in HDR, so the toe and mid
    contrast stay tunable. Bloom and volumetric fog controls wanted too (work queue rows). Adjustments global first;
    if more granularity is needed, adjustments keyed by grading block (levels sharing a block should look right with
    the same settings). Bloom / fog after this; fog settings are level data, not cvars (likely memory patches).
- **H42, our curve against each level's SDR look** (2026-11-22; found, offline; `research-data/hdr-grading-20261121/`
  `port9f6b/sdrcurve.cpp` (a level's SDR look on a grey ramp from its captured block, the level LUT blended, game
  brightness 125, gamma 2.2 brightness only), `h42/fit_levels.py`, chart `h42/fit_levels.png`):
  - The levels' SDR black floor (scene 0) is the level LUT's lift: Dan's Crypt 0.26 nits, Hilltop 0.19, Sleeping
    Village 0.13 at 125 (0.1-0.2% of SDR white); Graveyard and Hall of Heroes (no LUT) 0. Mid grey (Y(U) 0.18):
    22.8 nits without a LUT, 24.5-24.9 with (the LUT lifts mid-tones ~8%).
  - Fit below the SDR shoulder (SDR nits < half of white), as a function of Y(U): our H40 curve as built matches the
    two levels without a LUT within 0.35-0.38% PQ (Graveyard clamp 0.0042, length 0.052, strength 0.47, gamma 1.10;
    Hall of Heroes 0.0035 / 0.053 / 0.49 / 1.00: the film slope 0.775 vs 0.88 shows as mid contrast) but the LUT
    levels only within 1.6-1.8% (it has no black floor). With a black floor added (nits fading out by mid grey) every
    level fits within 0.30-0.36% PQ: Dan's Crypt floor 0.254, gamma 1.11, strength 0.27, length 0.074; Hilltop 0.189 /
    1.12 / 0.36 / 0.083; Sleeping Village 0.133 / 1.10 / 0.39 / 0.079.
  - The fit is not unique: the game's smooth SDR curve does not pin toe length, strength and gamma down separately
    below the shoulder (the Graveyard fitted gamma 1.28 / length 0.033 in H40 over a shorter range, 1.10 / 0.052 here,
    both within 0.35%; they differ above grey, where only gamma acts). The controls themselves act differently (the
    user: toe length / strength add contrast in the shadows without lowering the mid-tones; grey is pinned, while a
    change to the game's film toe / slope moves the mid-tones too): the derivation needs a convention (e.g. gamma from
    the curve's slope at and above grey, toe from the shadows with the length fixed) so each slider keeps its meaning.
  - So our curve's shape can carry every captured level's SDR look below the shoulder; the per-area inputs it needs:
    black floor, mid-grey level, mid contrast, toe (strength, clamp).
  - Corrections (the user, 2026-11-22): the clamp is a black point (crushes the deepest tones without lifting
    anything), the floor a lift (the LUT's); a long relaxed toe (strength 0) is a plain gamma curve. The user's tuning in
    Piecewise was absolute (it replaced the game's tone; only the colour was the game's), hence different values per
    level. The LUT rebuild happens every frame (every level capture holds one; Dan's Crypt two): the derivation must
    be cheap, GPU only (no CPU readback / sync), recomputed only when the block changes (a CPU hash of the 144 dwords).
  - Derivation by fixed rules, no fit (`h42/derive.py`, chart `h42/derive.png`), anchors on the game's grey curve as
    a function of Y(U) (at runtime: about six GradeScene calls on greys, the LUT included): floor = the curve at black;
    grey level = the curve at 0.18 / (0.18 x game brightness); gamma = log-log slope of (curve - floor) from 0.18 to
    0.36; toe length, strength and clamp = the curve through the game's at Y(U) 0.07, 0.02 and 0.006 (a fixed toe
    length cannot serve both kinds of level: without a LUT 0.05 fits, with one 0.08-0.10). Result: Graveyard floor 0 /
    grey x1.01 / gamma 1.17 / toe 0.060 / 0.40 / clamp 0.0043; Hall of Heroes 0 / 1.01 / 1.09 / 0.060 / 0.38 / 0.0043;
    Dan's Crypt 0.26 nits / 1.11 / 1.08 / 0.128 / 0.27 / 0.0009; Hilltop 0.19 / 1.10 / 1.14 / 0.125 / 0.32 / 0.0009;
    Sleeping Village 0.13 / 1.09 / 1.14 / 0.102 / 0.34 / 0.0010. Below grey within 0.14-0.41% PQ rms, at most 1.05%;
    from grey to half of SDR white 0.7-1.0% rms (our curve keeps climbing where the SDR shoulder starts: intended).
    Fixed (first trial) toe length 0.05: the LUT levels off by up to 1.9% PQ around Y(U) 0.06.
  - The same adjustments on every level (floor x0.5, gamma x1.1, toe strength +0.1): floor halves everywhere (0.46-
    0.50), mid grey stays (pinned), Y(U) 0.4 +13-20%, Y(U) 1 +27-38%, Y(U) 0.08 x0.69-0.81: the same direction and
    similar size per level.
  - Raised blacks (the user: unpleasant, more visible in HDR on high-contrast screens; removing them is a point of the
    new curve). Floor 0 removes the black point but not the LUT's lift of the lowest shadows, which the derived long
    toe carries (Y(U) 0.003 / 0.01, nits at 125: Dan's Crypt with LUT 0.293 / 0.548, ours floor 0 0.036 / 0.311,
    without the LUT 0.002 / 0.094; Hilltop 0.204 / 0.328, 0.015 / 0.173, 0.000 / 0.027; Sleeping Village 0.148 /
    0.262, 0.012 / 0.156, 0.000 / 0.027). Derived from the curve without the LUT (`sdrcurve.exe ... 1`: weights 1 / 0)
    Hilltop and Sleeping Village get the Graveyard's values exactly (floor 0, grey x1.013, gamma 1.17, toe 0.060 /
    0.40, clamp 0.0043); Dan's Crypt its own film (gamma 1.11, toe 0.057 / 0.31). Proposed: a "level LUT tone" amount
    (the anchors from a blend of the curve with and without the LUT; the LUT's colour kept): 0 removes the floor and
    the haze together; costs a second set of anchors, on block changes only.
  - The user's Hilltop settings (2026-11-22 03:32, screenshot 03.32.24.58; Piecewise, absolute values): game
    brightness 223, peak 434, mid contrast 1.30, toe clamp 0.0018, toe length 0.028, toe strength 0.57, shoulder start
    20% of peak, shoulder length 5.0 stops (the row's maximum), shoulder angle 100%, highlight saturation 51%. The
    lifted shadows resolved; mid contrast could be a little lower, deep blacks / contrast partly the user's taste;
    "would have been unachievable without the piecewise curve". The shoulder at its range's end: widen it.
  - To fix when built: grey is pinned at s' = Y - clamp, so the clamp darkens grey ~3% (22.06 vs 22.80 nits): pin it at
    Y(U) instead. Solver at runtime: a few Newton steps on the analytic curve (no GradeScene) instead of the offline
    bisections.
- **H43, frame-time spikes in "SDR grading kept"** (2026-11-22; found by reading the code, fix built 04:47:21,
  verified by the user the same day: "that was it, the issue is gone"): the user's live switch in the Graveyard (build 21:20:24): SDR grading kept 215.0 avg, 1% / 0.1% lows 171.6 /
  157.9, a comb of spikes; ACES 214.4, 212.9 / 210.7, flat (screenshots 03.38.41 / 03.38.15). The user suspected the
  x100 work (043a1ce, the only change since the last verified regression run). Not its shader recognition (compile
  time only): its prepared-draw check. `TryPreparedPipeline` compared each prepared draw's `hdr_lut_x100` (the
  variant picked for it: true only for the HDR tonemapper) with the global flag (true whenever our LUT pass built the
  LUT, i.e. SDR grading kept): every other draw failed and took the full selection and unprepared binding on the GPU
  thread. ACES: the flag false, all match. Fix: `PreparedDraw::hdr_lut_index` (the fragment shader indexes the HDR LUT),
  the flag compared only then (not "the flag at prep time": frames with two LUT builds, one the game's, e.g. Dan's
  Crypt's scene capture, flip it twice a frame). Previous exe kept in `baselines/hdr-piecewise-20261121-2120/`.
  To confirm: the same live switch on the fixed build (lows and graph equal in both looks).
  - Decision (the user, 2026-11-22): the final HDR replaces the game's tonemapper draw with our own pass (the LUT read
    at our own index: no x100 variant, no recognition pass, no prepared-draw check; room for bloom strength, grain,
    FXAA luma), built on copy-fault-trace and regression-tested there. This fix only confirms the mechanism early.
- Contrast, a lead [I]: in UE 4.21 the HDR output devices (3 / 4) go through the ACES ODT instead of the SDR film curve,
  so the film toe that crushes SDR blacks is not applied; the 8-bit frame image adds banding but should not lift blacks.
  Needs a same-view SDR / HDR capture pair to measure.

## RenoDX's Unreal Engine approach (research agent, 2026-11-17; MIT licence)

Read from source: clshortfuse/renodx `bfdb52d` (`src/games/unrealengine`, `src/shaders/{draw,tonemap,colorcorrect}.hlsl`,
`tonemap/reno_drt.hlsl`, `games/halocampaignevolved`, `games/robocop`) and marat569/renodx `11089c9`
(`src/games/ue-extended`). [V] read in source, [I] inference.

- **What it replaces [V]:** only UE's LUT builder (CombineLUTs; ~217 hash-specific copies) plus one swap-chain pass;
  the generic mod leaves the tonemapper and the UI composite alone (per-game mods replace the composite). The 32^3
  R10G10B10A2 LUT is upgraded to RGBA16F.
- **Keeping the SDR look [V]:** inside the builder it captures the untonemapped colour U (after white balance and
  ColorCorrectAll, before blue correction and the film curve), lets the whole SDR chain run to the graded SDR colour G,
  then skips UE's output-device switch (no ACES ODT) and outputs an upgrade: N = neutral SDR tonemap of U (100-nit
  peak, 0.18 -> 0.18); ratio = Y(U)/Y(N) below the curve, else (Y(G) + Y(U) - Y(N)) / Y(G); faded in by
  saturate(Y(U)); out = hue-corrected (OKLab) G * ratio. So the SDR grade holds up to SDR white and only the luminance
  the SDR curve removed comes back above it. Newer UE mods instead extend UE's own film curve linearly above 0.18.
- **Then [V]:** a luminance-based Reinhard roll-off (RenoDRT: mid-grey kept, peak reached at a white clip of 65,
  OKLab hue correction, gamut clamp), gamma 2.2 emulation (`decode2.2(encode_sRGB(c))`, the default "Gamma correction
  2.2"), x game nits, PQ BT.2020, / 1.05 for UE's LUT scaling. No BT.2390 EETF in the UE paths.
- **Defaults [V]:** peak 1000 (replaced by the detected display peak), game brightness 203, UI brightness 203, gamma
  correction 2.2, hue processor OKLab, scene grading strength 100. UE mods override FilmBlackClip to 0 ("prevents
  crushing or black raise").
- **UI and video [V]:** the UI is decoded sRGB (or 2.2 with UI gamma correction), the scene PQ normalised by UI nits,
  both re-encoded 2.2 and blended premultiplied (ui + scene * (1 - a)), then x UI nits, clamp to peak, PQ. Videos drawn
  as UI land at UI nits with 2.2 decoding; startup videos as sRGB at paper white, no expansion.
- **For MediEvil [I]:** the raised blacks / low contrast (H3, H14) match the ACES ODT path plus sRGB-vs-2.2; the washed
  out UI and movies (the user, 2026-11-17) match an sRGB UI decode where the SDR display used 2.2. Both point at our own
  versions of the combine LUT builder (`a4152b3be93c832a`) and the UI composite (`c401ca35f9bceb8e`).

## Open questions (each needs a run with the user's go)

1. With `GOW3_HDR=1`, Windows HDR on and the 4090: is the HDR10 surface pair offered? (needs a log line)
2. Which buffer format / colorimetry the game uses at boot (the RegisterBuffers `LOG_INFO` is filtered: needs a line).
3. With the capability forced and the pak cvars: does the game switch to `0x88740000` / colorimetry 12, are the cvars
   in time for RHI init, and how do the overlay and the upscaler presets look?
4. Does the game's options menu show anything HDR once the capability is reported (`PMENU_HDR` has no widget)?
5. Does an r.SetRes resize switch HDR on / off live? (`0x6893c0` waits for flips and re-registers buffers)
6. Is `r.Gamma` read anywhere? Does OutputDevice 3 / 4 change the Tonemap shader hash the frame map keys on?
7. Art: the game shipped SDR-only; how its ACES PQ output looks is the user's call.
8. Blue blotches (queue row): not HDR output (the game never enters it); likely an inf / NaN or out-of-range value in
   the linear scene colour at 1080p (reconstruction runs there), same family as the RenderDoc warm-cache row [I].
