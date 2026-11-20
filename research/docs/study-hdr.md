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
    7710) and eid 7722 copies the result back [V]. Fix built (release 00:27): both first passes recognised. The
    user's 00:47 run logged no backdrop line (no blurred menu opened, or the fix did not fire): not yet run.
  - The game's Brightness reaches the UI too: every Slate pixel shader runs `if (cb[1] != 1) sRGB_encode(pow(c,
    cb[0]))` (fs `4fe20af683865713` IR); cb = (1.35385, 0.615385) in `menu_frame19708` = 2.2 / display gamma and its
    inverse, the grading block's dword 4 / 5 at the same setting [V]. The user's choice: no shader change; the
    launcher's Display > Game brightness > Set to neutral writes the default 0.3778 (display gamma 2.2) into every
    save slot (`scripts/save_brightness.py`: backup first, 4 bytes in place, file times kept; the slider steps 0.02
    from that default; slots without the property are at the default), and the UI brightness help says to leave the
    slider at its default.
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
