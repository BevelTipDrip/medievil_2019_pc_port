# Study: which restart-only settings can become live

Status: study, 2026-11-04, branch `deck-lab`. Read-only: code reading and the existing static eboot scans. Nothing was
built or run. Parent: [START-HERE.md](START-HERE.md). This study adds to
[study-live-switches.md](study-live-switches.md) (2026-10-30: cvar objects, per-option notes) and
[plan-live-switches.md](plan-live-switches.md) (what has been built). It does not repeat their tables. New here: the
renderer-recreate paths, binary patches at run time, a full VkDevice re-creation, output resolution, AA and the
current settings list.
Tags: **[code: file:line]** read in this tree; **[measured: source]** from a logged run; **[inferred]** reasoning or
UE 4.21 / driver knowledge that was not checked in this binary. `r.ScreenPercentage` (live render scale) is being
built by another agent. Only its interactions are noted here.

## Short answer

- **Most settings players tune are live already or need little work.** Already live: display mode, VSync, frame
  rate limit, VRR, frames queued, async shaders, FSR 1 / RCAS, preshadows, effects resolution, the 13 per-effect
  graphics options, 5 performance patches, the toggle-file bits. Small work: the rest of the quality preset, AA,
  present mode and the frame limiter. All of these are an atomic, a `ue4_cvars[]` row or the existing swapchain
  recreate.
- **Output resolution can probably be made live without a restart.** The boot size is one cached rectangle in the
  game's **bss** (data, not code): `{0,0,W,H}` at 0x5b1d2cc, with its "computed" flag at 0x5b1d2c8 (01.00)
  [code: study-boot-resolution.md:43-77]. To switch: write that rectangle, then do the `r.SetRes` the menu already
  sends. This may also fix I1 for the existing "Game resolution now" buttons. The guest memory size (`GOW3_DMEM_MB`)
  limits it: it is fixed at boot.
- **Binary patches.** "Finer occlusion culling" can be switched while the game runs. The two same-length instruction
  rewrites in `BuildHZB` would be done from the HZB remap's MapResults hook, which runs on the render thread outside
  `BuildHZB`. That is safe self-modifying code on the thread that runs it. The 4K framebuffer pool patch cannot be
  live: the allocator is built once at boot. The boot display size patch does not need to be live: write the cached
  value instead (above).
- **Re-creating the renderer while the game keeps running.** This is cheap for the swapchain, presenter frames and
  FSR / post-processing. They already rebuild on resize, fullscreen and VSync changes, with a `waitIdle`. A new
  VkDevice is a different matter: every texture, buffer, pipeline and descriptor would have to be rebuilt. GPU-written
  tiled render targets cannot be downloaded to guest memory today [code: texture_cache.cpp:2093-2097], so their
  contents would be lost: cached shadow maps, TAA history, GPU particle state. That is several weeks of work with a
  high risk. **Not recommended.** For device-level choices (Reflex, readback queue, VRS), turn the feature on at
  device creation and switch it each frame.
- **True restarts:** the 4K memory size, the framebuffer pool, edits to the game's packaged files (debug menu,
  unlimited ammo, voice language, 01.02's `t.MaxFPS`), the game language, safe GPU mode and driver environment
  variables. Turning on the two-stage pipeline, the present thread and the readback modes also stay restarts: little
  value for the work.

### Top 5 conversions (user value × effort × risk)

| # | Conversion | Value | Effort | Risk | Mechanism |
|---|---|---|---|---|---|
| 1 | **Whole quality preset live** (Low / Medium / Console / High / Ultra as one switch) | high | S-M | L | (b): 4 more `ue4_cvars[]` rows (`r.Shadow.MaxCSMResolution`, `r.Shadow.RadiusThreshold`, `r.Shadow.CacheWholeSceneShadows`, `r.RefractionQuality`; 01.00 objects known, 01.02 to find), plus every preset's values from `run.py`; apply as a batch |
| 2 | **Output resolution live** | high | M | M | (c)+(d): write the cached display-metrics rect (bss), `r.SetRes`, refit the window; limited by the boot's guest memory |
| 3 | **Present mode + frame limiter live** (VSync special modes, Reflex / Precise / Standard, precise tick) | medium-high | S-M | L | (c): `GOW3_PRESENT_MODE` → a setting plus the existing `UpdateVsync` recreate; Reflex extension enabled whenever offered; limiter statics → atomics |
| 4 | **Anti-aliasing live** (TAA / FXAA / off) | medium | S | L | (b): `r.PostProcessAAQuality`, registration found (object 0x5819fb8, 01.00 [inferred from the scan]) |
| 5 | **Finer occlusion culling following the (live) render scale** | medium (Deck, low-end) | S | L-M | (e): patch on the render thread from the HZB remap hook |

Next after these: HDR (medium, low value until it is known whether MediEvil asks for HDR output), and the engine
frame rate (boot at the highest rate and cap live instead). Device re-creation: no.

## 1. Inventory: what needs a restart today

### 1.1 medievil.ini keys (`gpu/shim/gow3_settings.{h,cpp}`, `scripts/graphics_options.py`)

How a value reaches the game. **Live** = read every frame or applied at once. **Boot cvar** = the override pak
writes it at start. **Env** = `run.py` turns it into a `GOW3_*` variable that is read once.

| Key(s) | Today | Read where |
|---|---|---|
| display_mode | live | atomic → window thread `ApplyDisplayMode` [code: gpu/shim/window.cpp:159-161, 105-119] |
| vsync | live | `Swapchain::UpdateVsync` each present [code: vk_swapchain.cpp:346-356; vk_presenter.cpp:845] |
| fps_limit, display_vrr | live | atomics in the vblank / limiter loop |
| frames_queued | live | each frame [code: vk_presenter.cpp:681] |
| async_shaders | live | [code: study-live-switches.md:54] |
| fsr1, rcas, rcas_strength | live | each frame [code: vk_presenter.cpp:638-640] |
| preshadows, effects_resolution, 13 `UE4LiveOptions` | live (UE4 cvar) | `runtime_ue4_request_cvar`, also requested once at start [code: gow3_settings.cpp:284-294] |
| perf_small_buffers, perf_lockfree_memory (half), perf_cheap_frees, perf_lean_binding, perf_ring_reuse | live | `PerfLive` [code: gow3_settings.h:160] |
| perf_hzb_occlusion, perf_no_decal_prepass, perf_particle_single_pass, perf_texture_memo, perf_gpu_data_no_waits, perf_4k_memory, perf_lean_barriers | restart | env [code: graphics_options.py:273-290]; boot cvars / hooks at load |
| ue4_quality | restart (boot cvars) | override pak `[SystemSettings]` [code: ue4_override.py:459-470] |
| output_resolution | restart | patches + `GOW3_INTERNAL_*` + `GOW3_DMEM_MB` [code: graphics_options.py:129-162] |
| render_scale | restart (the other agent is making it live) | boot cvar `r.ScreenPercentage` (+ `r.TemporalAA.Upsampling` below 100) [code: ue4_override.py:466-469] |
| ue4_aa | restart | boot cvar `r.PostProcessAAQuality` [code: ue4_override.py:481-483] |
| engine_fps | restart | pak `SmoothedFrameRateRange` + `GOW3_FPS_CAP` |
| deferred_readback, stale_readback | restart | const members [code: study-live-switches.md:258-265] |
| present_limiter, present_tick_hz, present_precise_tick, present_inline, present_no_frame_caps, present_mode, present_frame_trace | restart | env [code: graphics_options.py:295-336]; statics [code: driver.cpp:1354-1410]; `GOW3_PRESENT_THREAD` in the ctor [code: driver.cpp:96-100] |
| ue4_voice, cheat_menu, infinite ammo | restart | Blueprint / ini edits in the override pak [code: ue4_override.py:263-390] |
| render_resolution, upscaler / preset (GoW3 / FSR keys) | restart; not used by MediEvil (`GOW3_UPSCALER=none`) | - |

### 1.2 Launcher-only ("app") options (`launcher/medievil_launcher.py:470-500`)

`hdr` → `GOW3_HDR`; `draw_pipe`; `readbacks`; `vulkan_compat`; `background_warmup`, `parallel_warmup`,
`pipeline_binary_cache` (start-up only); `prep_workers`; `stage_b_core`; `language` (`GOW3_LANGUAGE`); patch
selection (`GOW3_PATCHES`); diagnostics (`frame_stats`, `gpu_profile`, `vk_validation`, `perf_diag`). All are read
once. The launcher marks `hdr` as live, but it is not (see 2.3).

### 1.3 `GOW3_*` environment variables

`src/`, `gpu/shim/`, `gpu/shadps4/` and `gpu/dlss_bridge/` read **213 distinct `GOW3_*` variables** in 67 files
[code: grep of getenv / Flag / Number / EnvOn]. Almost all are read once (`static const ... = getenv`). Except for the
user-facing ones above, they are diagnostics or A/B switches. The toggle file already covers the A/B need while the
game runs (50+ bits, switch-off only, [code: study-live-switches.md:60-79]). No new live path is proposed for them.
The ones that matter here:

- **At device / instance creation:** `GOW3_REFLEX` (adds `VK_NV_low_latency2` [code: vk_instance.cpp:357-365]),
  `GOW3_READBACK_QUEUE` (a second queue [code: vk_instance.cpp:520]), `GOW3_VULKAN_COMPAT`, `GOW3_NO_MAINTENANCE5/8`,
  `GOW3_NO_2D_VIEW_OF_3D` [code: vk_instance.cpp:226-233, 336-343, 455], `GOW3_PAGEABLE_MEMORY`, `GOW3_GPU_ID`,
  `GOW3_HDR` (instance extension `VK_EXT_swapchain_colorspace` [code: vk_platform.cpp:200]), `GOW3_VK_VALIDATION`.
- **VideoOut at construction:** `GOW3_INTERNAL_WIDTH/HEIGHT` [code: emulator_settings.h:25-26; video_out.cpp:476],
  `GOW3_VBLANK_HZ` (a refresh of 119.88 Hz reported to the guest when ≥ 120 [code: driver.cpp:93-95]),
  `GOW3_PRESENT_THREAD` (swap thread [code: driver.cpp:96-100]).
- **Guest memory:** `GOW3_DMEM_MB` [code: src/runtime_memory.c:29-37].
- **At load (patch / hook):** `GOW3_PATCHES` → `apply_patches` [code: src/probe.c:677-709, 1003], `GOW3_HZB_REMAP`
  [code: probe.c:584-607], `GOW3_MOVIE_SUSPEND`.

### 1.4 UE4 cvars: boot vs live

- **Boot:** `scripts/ue4_override.py apply()` writes `[SystemSettings]` into a replacement
  `Engine/Config/PS4/PS4Engine.ini` inside an override pak (priority SetBySystemSettingsIni) and
  `[/Script/Engine.Engine] SmoothedFrameRateRange` [code: ue4_override.py:459-540]. Today that covers the presets,
  `r.ScreenPercentage`, `r.TemporalAA.Upsampling`, `r.setres`, `r.SeparateTranslucencyScreenPercentage`,
  `r.PostProcessAAQuality`, HZB (`r.AllowOcclusionQueries`, `r.HZBOcclusion`, `r.NumBufferedOcclusionQueries`),
  `r.DBuffer` / `r.EarlyZPass`, `r.GPUParticle.FixDeltaSeconds`, plus `GOW3_UE4_CVARS` extras.
- **Live:** `runtime_ue4_request_cvar(name, value)` queues a value (at most 7 characters). The game's input thread,
  inside `scePadReadState`, sets it through `IConsoleVariable::Set` (vtable +0x68) with the cvar's current SetBy
  priority, after 600 polls (the main menu). Before that it checks 7 bytes at the registration site for 01.00 / 01.02
  [code: src/runtime_extra.c:610-630, 641-664, 689-719]. The log line is `UE4: <name> <value> (live)`
  [code: runtime_extra.c:628]. The table has 18 rows today. `r.SetRes` has its own slot (`GOW3_UE4_SETRES_CVAR`).

### 1.5 Binary patches (`patches/MediEvil.xml`, applied once by `probe.c apply_patches`)

The patches are written into the image after relocation and before the segments are protected (code ends up
PAGE_EXECUTE_READ) [code: probe.c:1003, 1023-1030]. For 01.00 / 01.02: "Native 4K framebuffer pool (512 MB)" (on),
"4K display metrics at boot" (`run.py`), "Boot display size WxH" (`run.py`), "GPU particle bounds" (on), "Finer
occlusion culling" (`run.py` auto ≤ 1280x800 scene, or `GOW3_HZB_FULL_SIZE`) [code: MediEvil.xml:20-136;
graphics_options.py:173-200].

### 1.6 Vulkan-level choices

Present mode (`GOW3_PRESENT_MODE`, re-read at every swapchain build [code: vk_swapchain.cpp:323]); VSync (live);
HDR (instance extension + the swapchain format from `FindPresentFormat`, once [code: vk_swapchain.cpp:277-282]; the
per-flip `SetHDR` already recreates the swapchain [code: vk_swapchain.cpp:176-191; driver.cpp:382]); swapchain image
count (`minImageCount + 1`, recomputed each `Create` [code: vk_swapchain.cpp:373]); frames queued (live); FSR 1 /
RCAS (live); output and VideoOut size (`GOW3_INTERNAL_*`, static); internal framebuffer and DMEM sizes (boot); RADV
variables (`RADV_FORCE_VRS`, `RADV_DEBUG`, through `extra-env.txt` on Linux [code: plan-linux-deck.md:742-744]),
read by the driver when the device is created.

## 2. Per setting: how it could become live

### 2.1 (a) Already live, or a value read each frame

Covered in 1.1. Small leftovers:
- Launcher text: "Effects resolution" and the 13 graphics options say "Applied at start" with `restart=True`, but they
  are live in game [code: medievil_launcher.py:1272-1274, 1285-1289]. Text only.
- `hdr` has no restart mark in the launcher, but `IsHdrAllowed` is a static and also gates an instance extension
  [code: emulator_settings.h:60; vk_platform.cpp:200]: it really is a restart (2.3).
- `perf_lockfree_memory` is shown as live but switches only `OPTIMISTIC_CLAMP`; `REGION_BUCKETS` stays at its boot
  value [code: study-live-switches.md:296-301].

### 2.2 (b) Live through the UE4 cvar path

Does UE 4.21 honour a change made while the game runs? Per cvar [inferred, UE 4.21 source knowledge, as in
study-live-switches.md]:

| Setting | Cvars | Honoured live? | Notes |
|---|---|---|---|
| Rest of the quality preset | `r.Shadow.MaxCSMResolution` (0x581a228), `r.Shadow.RadiusThreshold` (0x5bcfea0), `r.Shadow.CacheWholeSceneShadows` (0x5bcfec0), `r.RefractionQuality` (0x5bc81a0) (01.00 objects [code: study-live-switches.md:128-151]) | yes, read per frame | CSM targets reallocated; a cache toggle drops / rebuilds cached shadow maps (one heavy frame). 01.02 objects: run `cvars_0102.py` (session scratchpad) as for the other rows |
| Preset as one switch | union of `preset_values()` keys | yes, as above | `run.py` passes only the **boot** preset's values today (`GOW3_UE4_PRESET_CVARS` [code: gow3_settings.cpp:211]). A live preset needs all five tables, e.g. one variable per preset. Leave out `r.MaterialQualityLevel` (cooked shader maps: restart; Ultra's 1 is the default), `r.DetailMode` (the sink recreates every component's render state: a hitch; Ultra's 2 is the default), and `foliage.DensityScale` / `grass.DensityScale` (Low only, apply when foliage is rebuilt: "next area") |
| Anti-aliasing | `r.PostProcessAAQuality`: registration lea at 0x731eac; IConsoleVariable* stored to 0x5819fc0, so the object is 0x5819fb8 (01.00) [code: scratchpad `cvars3.txt:1350-1357`; the object is inferred from the same layout as `r.MotionBlurQuality` 0x5819fa0] | yes: the view's AA method is decided each frame [inferred] | Re-enabling TAA resets its history (one soft frame). **Interaction with live render scale:** below 100% `r.TemporalAA.Upsampling=1` needs TAA, so the AA row must force TAA while the live scale is below 100 (`ue4_override.py` already does this at boot [code: ue4_override.py:481]). 01.02 object to find |
| Render scale (other agent) | `r.ScreenPercentage`, `r.TemporalAA.Upsampling` | yes; reallocates every scene target | Interactions: (1) the framebuffer pool and guest memory are sized at boot (2.4), so live values above the boot scale × output can run out (the menu already refuses larger outputs when supersampling [code: gow3_overlay.cpp:529-544]); (2) AA above; (3) the finer-culling patch's auto rule uses the **start-up** scene size [code: graphics_options.py:176-200], see 2.5; (4) effects resolution is a percent of the render size, so it follows automatically |
| Engine frame rate | `GEngine->SmoothedFrameRateRange` (a UObject member, not a cvar) | read every tick [inferred] | Needs the GEngine pointer and the member offset (RE not done); `t.MaxFPS` does not cap in this build (START-HERE). Product answer: boot at the highest rate and use the live limiter. Open: game timing above 120 [code: emulator_settings.h:31-35 comment] |
| HZB, decal prepass, particle pass | `r.AllowOcclusionQueries`, `r.DBuffer` / `r.EarlyZPass`, `r.GPUParticle.FixDeltaSeconds` | yes [code: study-live-switches.md:267-288] | The user does not want these clear wins switchable (plan-live-switches.md "Scope") |

Method cost: one row per cvar in `ue4_cvars[]` (object, site, 7 bytes, two builds). A batch lands between two game
frames. Render-thread-safe cvars switch together, the others at once: at most one frame of mixed state [inferred].

### 2.3 (c) Re-create port-side renderer objects at a frame boundary

**The existing recreate paths:**
- **Window resize / fullscreen toggle:** the menu sets `display_mode`; the window thread calls SDL
  [code: window.cpp:105-119, 159-161]; the next `Present` sees the window size differ from the swapchain and calls
  `Swapchain::Recreate` [code: vk_presenter.cpp:844-858]. `Recreate` → `Create` → `Destroy` does a
  **`device.waitIdle()`** under `wait_mutex` and rebuilds the swapchain, views and semaphores
  [code: vk_swapchain.cpp:68-116, 391-418]. The presenter's frame images are re-made when the expected size changes
  (`RecreateFrame`, [code: vk_presenter.cpp:226-288, 1187]).
- **VSync:** `UpdateVsync` at each present: `waitIdle`, `FindPresentMode`, `Recreate`
  [code: vk_swapchain.cpp:346-356].
- **HDR:** `SetHDR` per flip with the game's colorimetry: `waitIdle`, `Recreate`, ImGui format change
  [code: vk_swapchain.cpp:176-191; driver.cpp:382].

The state these paths keep: everything outside the swapchain. Texture, buffer and pipeline caches, the schedulers and
the guest's frames are untouched; the present queue just drains. The cost is one `waitIdle` hitch, which the user
already accepts for VSync. Built at presenter construction from the swapchain's image count, and **not** rebuilt:
`present_frames`, `fsr_pass.Create(..., num_images)` and `Gow3Overlay::Init(..., num_images)`
[code: vk_presenter.cpp:150-166]. A recreate that changes the image count would mismatch them. Today the count
depends only on surface capabilities, not on the present mode [inferred; it changes per mode only with
`VK_EXT_surface_maintenance1`, not used]. Any new live recreate should assert that the count is unchanged.

**Conversions that use these paths:**
- **Present mode** (launcher "Off, allow tearing" / "Adaptive"): make `GOW3_PRESENT_MODE` a setting atomic. Have
  `UpdateVsync` also compare the mode, then call `FindPresentMode` + `Recreate` (they already re-read the mode
  [code: vk_swapchain.cpp:323-338]). Effort S, risk L.
- **Frame limiter** (Reflex / Precise / Standard): Reflex needs `VK_NV_low_latency2` at device creation
  [code: vk_instance.cpp:357-365] and `SwapchainLatencyCreateInfoNV` at swapchain creation
  [code: vk_swapchain.cpp:87-89]. Enable the extension whenever the driver offers it; keep the "use Reflex" decision
  per frame (`LatencySleep` / markers return early when disabled [code: vk_swapchain.cpp:123-168]). With the latency
  mode set but no sleep calls, the driver should not pace frames [inferred: NVIDIA docs; one A/B launch to confirm no
  cost]. `GOW3_PRECISE_LIMIT`, `GOW3_LIMIT_HOLD` and `GOW3_PRESENT_PRIORITY` are statics read in the limiter
  [code: driver.cpp:1354-1380]: make them atomics, and re-apply the thread priority from inside the thread.
  Effort S-M, risk L (one mis-paced frame at the switch).
- **Precise tick / tick rate:** statics [code: driver.cpp:1383-1390; emulator_settings.h:36-42]. Atomics, plus
  re-anchoring the tick grid. A tick rate crossing 120 changes the 119.88 Hz the game read at boot
  [code: driver.cpp:93-95]: allow changes only on the same side of 120. Effort S-M.
- **HDR:** two blockers. `VK_EXT_swapchain_colorspace` is an **instance** extension, added only with `GOW3_HDR`
  [code: vk_platform.cpp:200], and `supports_hdr` is computed once [code: vk_swapchain.cpp:277-282]. Fix: always add
  the extension when present, and make "HDR allowed" an atomic that `SetHDR` checks. The game decides HDR itself
  through `sceVideoOutConfigureOutputMode` colorimetry and the param.sfo HDR attribute
  [code: video_out.cpp:444-452]. Whether MediEvil ever asks for Bt2020PQ is unknown: check a log or a breakpoint
  first. If it never does, the HDR option is moot and needs no live path. Effort S-M, value unknown.
- **FSR 1 / RCAS / post-processing passes:** already per frame. The passes are created once and take their settings
  per call [code: vk_presenter.cpp:638-650].

### 2.4 (d) Needs the game's own render resources re-created

**Why guest-side sizes cannot change while the game runs:** the game builds its memory pools once, from what the PS4
APIs report at boot, and keeps the allocator state (free lists, buddy pages, which pool an address belongs to) in its
own memory. The port cannot resize them without re-running the game's own initialisation.
- **Framebuffer pool (512 MB patch):** the manager at 0x8f4a00 allocates and maps its direct memory and sets its
  buddy page count at init. The free paths decide the pool from the address range (`addr >> 29`)
  [code: MediEvil.xml:10-50]. Patching after boot would not grow the pool, and patching the free paths alone would
  misroute frees (the crash the patch comment describes). **Restart only.** It is on by default for every boot size
  [code: MediEvil.xml:25; graphics_options.py:144], so live sizes up to 4K are covered by it.
- **Guest direct memory (`GOW3_DMEM_MB`, 5056 or 7104 MB):** read once [code: runtime_memory.c:29-37]. The game takes
  all of it into its pool at boot ([code: graphics_options.py:139-146 comment]; the 4K run's pool was +6,592 MiB,
  [measured: plan-work-queue.md, "Pak values inert" row]). **Restart only.**
- **Output resolution:** a hybrid, and it can be live:
  1. The boot size is the cached rect at bss 0x5b1d2cc ("computed" byte 0x5b1d2c8), written once by 0xb68fd0 /
     0xb6d640 and read by their 9 callers on demand [code: study-boot-resolution.md:43-77;
     plan-baseline-issues.md:41-43]. Writing `{0,0,W,H}` there while the game runs is a **data write**, no code
     patch. Do it on the game thread in `runtime_ue4_setres_poll`, then send the existing `r.SetRes WxH`
     [code: runtime_extra.c:631-634, 689-698]. For 01.02 the bss addresses are not recorded (the functions moved
     +0x2c80 [code: MediEvil.xml:172-174 comment]): find them in 0xb6bc50.
  2. I1 (menus not navigable after a live `r.SetRes`) came from that cache staying at 1080p while the viewport grew
     [code: plan-baseline-issues.md:52-57]. Refreshing the cache first should make a live change behave like a boot
     at that size. **Hypothesis**: UE4 / Slate may hold its own copy (SWindow size, hit-test grid) taken from the
     metrics when the window was made [inferred]. One launch decides: switch 1080p → 4K in the main menu, then
     navigate.
  3. VideoOut: the game never reads its size (Neo mode 0, cvar 0) [code: study-boot-resolution.md:88-90]. Its
     buffers re-register on a mode change (0x687630), as the live path already does. The presenter handles any frame
     size. In windowed mode, call `FitWindowedSize(W, H)` on the window thread [code: window.cpp:81-101] so the
     window follows.
  4. Limit: the boot's DMEM. A ≤ 1080p boot has 5056 MB. A live jump to 1800p / 4K then has the PS4's memory budget:
     texture reloads on small camera moves (the hint for `perf_4k_memory` [code: gow3_overlay.cpp:469]), possibly
     worse [inferred]. Options: allow live outputs only up to the boot's class (≤ 1440p for a ≤ 1080p boot: the same
     framebuffer blocks as 1080p [code: study-boot-resolution.md:170-180]), or always give 7104 MB on PCs with RAM to
     spare (about +2 GB of host RAM, not for the Deck).
  5. With live render scale, check the pool budget against output × scale (the menu's supersampling rule).
- **Graphics memory pools inside the game** (texture streaming pool 2000 MB, set by the PS4 device profile): fixed
  at boot [code: study-live-switches.md:161].

### 2.5 (e) Binary patches at run time

General mechanism: code pages are PAGE_EXECUTE_READ after load [code: probe.c:82-94, 1023-1030]. A run-time patch is
`VirtualProtect(RWX)` → write → `VirtualProtect(RX)` → `FlushInstructionCache` (`mprotect` on Linux). Cross-modifying
code (one thread writes code another may be executing) is only defined on x86 with a serialising step on the
executing thread [inferred: Intel SDM 8.1.3]. Safe cases: (1) **the thread that runs the code patches it itself**
while it is outside the function (x86 detects self-modifying code); (2) every thread that could run it is parked;
(3) the patch is in data, not code.

| Patch | Who runs the code | Live? | How / why |
|---|---|---|---|
| **Finer occlusion culling** (01.00 image 0x15357ba `41 ff ce`→`0f 1f 00`, 0x1535812 `ff c8`→`66 90`; 01.02 +0x2c80) [code: MediEvil.xml:120-136] | `BuildHZB` (0x1535740), render thread, once per view per frame | **yes, safely** | Same-length rewrites of single instructions on instruction boundaries; each line stays within one cache line (offsets 0x3a-0x3c and 0x12-0x13 in their 64-byte lines). Patch from `medievil_hzb_map` (the MapResults hook, [code: gpu/shim/medievil_hzb.cpp:302]): it runs on the render thread, which calls MapResults in InitViews before `BuildHZB` in the same frame [inferred, UE 4.21 order], so that thread is not inside `BuildHZB`: case (1). The hook exists only with `perf_hzb_occlusion` (default on; with it off the patch matters little). Effect: the HZB render target is reallocated at the new size on the next frame; the occlusion test reads the pyramid size from the view, so a frame stays consistent [code: MediEvil.xml:112-119 comment]; the remap's 3-frames-late results are only visibility bits [inferred]. Check the `Original` bytes before writing (as `patches.py` does). Use: let the live render scale toggle it at the 1280x800 scene limit, so the patch follows the scale as `hzb_full_size_environment` does at boot. Effort S, risk L-M |
| **GPU particle bounds** (two 4-byte RIP displacements in `UpdateBoundingBox` 0x23828b0) | game thread / particle tasks (several threads possible) | possible, no value | A bug fix, on by default; nobody toggles it. Patching it live would need case (2) |
| **Native 4K framebuffer pool** | init code, runs once | **no** | Patching init code after it ran changes nothing; the free-path halves must match the pool for the whole process (2.4) |
| **Boot display size / 4K display metrics** | the two metrics functions, which return the cache | not as a patch | After the first call the rodata default and the `je` are never read again (computed flag). Write the bss cache instead (2.4) |
| HZB remap, movie suspend hooks (`install_hzb_remap`, `install_level_hook`) | call-site rel32 rewrites | possible | Not settings. To switch the HZB remap, install it always and switch the cvar (study-live-switches.md:267-276) |

### 2.6 (f) Driver-level choices: a new VkDevice

**What holds VkDevice-owned objects.** The `Presenter` owns the `Instance` (VkInstance, VkDevice, VMA allocator,
queues), three schedulers, the swapchain, `Runtime` (staging pool, barrier tracking) and the `Rasterizer`
[code: vk_presenter.cpp:142-149; vk_presenter.h:163-198]. The `Rasterizer` owns the page manager, the buffer cache
(VkBuffers, arena, readback queue, GPU write twins), the texture cache (images, views, samplers, parked images), the
pipeline cache (pipelines, layouts, descriptor pools, async compiler and warm-up threads), draw preparation, scene
targets, motion / upscaler objects, the constant ring and the draw pipe threads. It registers itself with Liverpool
and the memory manager and hooks image access [code: vk_rasterizer.cpp:47-110]. Also tied to the device: the GPU
profiler's query pools, occlusion queries, the GPU signal threads, the FSR / PP passes, the ImGui backend
(`Gow3Overlay::Init`) and the DLSS bridge. In all, about 31k lines under `video_core/{texture_cache, buffer_cache,
renderer_vulkan, amdgpu/liverpool.cpp, page_manager.cpp}` [code: wc], with raw handles cached in many of them
(texture-set memo, sampler-handle cache, descriptor memos, thread-local stage B caches). The presenter is created
once in `GnmDriver::RegisterLib` [code: gnmdriver.cpp:2997-2999]. There is no device-lost recovery: every
`eErrorDeviceLost` asserts [code: vk_scheduler.cpp:964; buffer_cache.cpp:2107; vk_presenter.cpp:306].

**What a re-creation with the game kept would need:**
1. Quiesce: stop Liverpool after a flip, drain stages A / B and the recorder, finish every pending readback, EOP /
   label write and fence the guest may wait on, then `waitIdle`.
2. Save GPU-only contents. CPU-written data can be uploaded again from guest memory (the caches copy it in; no host
   memory import is used). **GPU-written data cannot**: the texture cache cannot download tiled images ("We can't
   handle non-linear image downloads" [code: texture_cache.cpp:2093-2097]), and most of UE4's render targets are
   tiled. Lost would be: cached whole-scene shadow maps (the presets set `r.Shadow.CacheWholeSceneShadows=1`
   [code: ue4_override.py:404]), TAA history, eye adaptation, GPU particle state, HZB and occlusion history, a
   captured sky light [inferred]. The workaround is a port-private snapshot: copy every image to a host buffer in
   the host's packed layout and copy it back into an identical image on the new device. At 4K that is ~6 GB of host
   memory [measured: plan-work-queue.md, ~6 GB VRAM at 4K] and about a second each way over PCIe [inferred].
3. Destroy the whole Presenter and build it again; re-register the VideoOut surfaces (`vo_buffers_addr`); rebuild
   pipelines from the disk / binary cache (seconds to minutes; `async_shaders` hides some as missing effects).
4. Re-bind every pointer other threads hold (Liverpool, memory manager, the image access hook, the overlay).

**Assessment:** several weeks of work with a high risk (any handle missed is a crash in the driver), a pause of
several seconds, and visible artefacts. The only settings it would serve are niche: safe GPU mode, maintenance5/8,
GPU choice, RADV variables, validation. **Not recommended.** Cheaper ways for the device-level settings that matter:
- **Reflex, readback queue:** enable at creation whenever supported, switch each frame (2.3; the readback queue is
  in study-live-switches.md:303-311).
- **VRS:** `RADV_FORCE_VRS` is read at device creation. Steam's QAM "Half rate shading" toggle uses
  `RADV_FORCE_VRS_CONFIG_FILE` [code: study-deck-platform-optimization.md:213-216]. Mesa's docs only say the file
  supersedes the variable. That RADV watches the file and applies changes while the game runs is **[inferred]**
  (Mesa's inotify notifier, and the QAM toggle working without a restart): check on the Deck before relying on it.
  A port-side alternative that works on any vendor and is fully live: `VK_KHR_fragment_shading_rate`. Enable the
  feature at device creation, add the dynamic state to pipelines (pipeline keys change once), and call
  `vkCmdSetFragmentShadingRateKHR` per draw only for scene-size targets, not the UI or post-processing. Effort M;
  value mostly on the Deck / weak GPUs.
- **Safe GPU mode, GPU choice, validation:** keep as restarts. They are fallbacks, chosen once.

## 3. Ranking

Value is weighted by how often people tune a setting: resolution, quality, shadows, view distance, effects, AA,
present mode, frame limit.

| Rank | Setting | Today | Class | Value | Effort | Risk | Note |
|---|---|---|---|---|---|---|---|
| 1 | Quality preset (whole) | restart | b | high | S-M | L | per-effect options already live; finish the preset |
| 2 | Output resolution | restart (live `r.SetRes` buttons with I1) | c+d | high | M | M | bss cache write; DMEM class rule; I1 retest |
| 3 | Present mode, frame limiter, precise tick | restart | c | med-high | S-M | L | existing recreate path; Reflex extension always on |
| 4 | Anti-aliasing | restart | b | medium | S | L | one row; forced TAA below 100% scale |
| 5 | Finer occlusion culling ↔ render scale | restart (patch) | e | medium (Deck) | S | L-M | render-thread self-patch from the HZB hook |
| 6 | HDR | restart | c | unknown | S-M | L | first check whether MediEvil requests HDR |
| 7 | Engine frame rate | restart | b (non-cvar) | low-med | M (RE) | M | boot high, cap live instead |
| 8 | Vblank tick rate | restart | c | low | S-M | L-M | same side of 120 only |
| 9 | Render scale | restart → **in progress (other agent)** | b | high | - | - | interactions: pool budget, AA, finer-culling threshold |
| - | 4K memory, framebuffer pool, packaged-file edits (debug menu, ammo, voice, `t.MaxFPS`), language, safe GPU mode, RADV variables, draw pipe on, present thread, readback modes | restart | d / f | - | - | - | keep as restarts |

## 4. Open checks (each one launch, with the user's go)

1. Output resolution: write the cache rect + `r.SetRes` in the main menu (1080p → 4K and back), then navigate the
   menus with the pad (I1). Log: `UE4: r.SetRes ... (live)`.
2. HDR: does the log or the VideoOut path ever see `Bt2020PQ` from MediEvil?
3. Reflex extension enabled with the Standard limiter: no change in frame times compared with today's Standard.
4. Finer occlusion culling switched at the render thread: HZB-hidden counts change (Deck stats), no crash, no
   pop-in.
5. Steam Deck: does a QAM half-rate shading toggle change the GPU time without a restart?
