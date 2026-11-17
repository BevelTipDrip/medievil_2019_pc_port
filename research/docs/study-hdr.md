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
