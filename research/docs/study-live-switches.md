# Study: which options can be switched live

Status: study, 2026-10-30. Read-only: code reading, a static scan of `out\eboot.elf` (01.00) and of the game's own
configs (`ue4_override.read_file`). Nothing built or run. Parent: [START-HERE.md](START-HERE.md). Background:
[plan-frame-pacing.md](plan-frame-pacing.md) N-24 (parking), N-25 (preshadows live). Evidence scripts and raw outputs
(scratchpad, this session): `cvar_find3.py` (one-pass index of every rip-relative `lea`, then the UTF-16 cvar names;
`cvars3.txt`), `cvar_objects.py` (object per registration), `dis_at.py` (disassembly; `regs.txt`: registration flags
and defaults), all under `C:\Users\Ty\AppData\Local\Temp\claude\c--Users-Ty-MediEvil\78f7734e-4522-4580-95d1-94046d666589\scratchpad\`.
**Addresses are eboot image offsets** (as `ue4_cvars[]` uses them). **[V]** verified in code or the binary, **[I]**
inferred (UE 4.21 source knowledge or reasoning), **[?]** guess. Nothing here was tested in game.

## Summary

- **Most of "Graphics quality" can be live with the existing mechanism.** Every cvar the presets write is a plain
  `TAutoConsoleVariable` / `FAutoConsoleVariableRef` with the IConsoleVariable* at +8 (same layout as r.SetRes and
  r.Shadow.Preshadows) **[V]**; all their objects are in the table below, with the 7-byte site to check. The renderer
  reads nearly all of them every frame **[I]**. Exceptions: `r.MaterialQualityLevel` (restart, and Ultra's value 1 is
  the default anyway), `r.DetailMode` (live but recreates every component's render state: a hitch), `foliage.DensityScale`
  / `grass.densityScale` (only on rebuild), `r.MaxAnisotropy` (partial, new samplers only).
- **`sg.*` scalability groups are not a shortcut** for the port's presets: they apply the game's `BaseScalability.ini`
  sections at `SetByScalability`, the lowest priority, so every cvar the override pak or `PS4Engine.ini` sets
  (`SetBySystemSettingsIni`) ignores them **[I]**, and the presets' own values (fixed local shadow sizes, the flicker fix)
  are not in those sections **[V]**.
- **ECVF_ReadOnly does not block the port.** Only `r.DBuffer` (flags 0x24) and `r.PS4Allow4kOutput` (4) of the scanned
  cvars are read-only **[V]**; 4.21 checks ReadOnly in the console command path, not in `IConsoleVariable::Set`
  **[I]**, and `ue4_set_cvar` passes the cvar's current SetBy (`GetFlags() & 0xff000000`), so the priority check passes
  too **[V]** (`src/runtime_extra.c:590`).
- **Render scale is live in UE4** (`r.ScreenPercentage` is read every frame) but reallocates every scene target
  (`r.SceneRenderTargetResizeMethod` = 0 here **[V]** default): medium, and bounded by the boot-time framebuffer pool.
  **Output resolution, engine frame rate, the GPU-readback modes and 4K memory stay restarts** (patches at load, VideoOut
  size, guest memory size, page-watch state).
- Of the 7 restart-only performance patches, **3 are UE4 cvars** (decal prepass: `r.DBuffer` + `r.EarlyZPass`; particle
  pass: `r.GPUParticle.FixDeltaSeconds`; HZB: `r.AllowOcclusionQueries` + always-installed hooks) and **1 is a pure
  stage-B decision** (lean barriers): all medium or easy. 4K memory is a true restart.
- **The toggle file is already live for 50+ port optimizations** (switch-off only; the launcher always sets
  `GOW3_TOGGLE_FILE`), but it covers only parts of three user options (texture memo, GPU data without waits, draw pipe).
- Found on the way: the pak's `r.GarlicHeapSizeMB=4600` is probably inert (the name is not in the eboot) and its
  `r.Streaming.PoolSize=1600` is probably overridden by the PS4 device profile's 2000; toggle bit 31 means two different
  things; "Lock-free memory map lookups" live only switches half of what its restart switch does. Details at the end.

## How "live" works today (three mechanisms)

**A. UE4 console variable** (`src/runtime_extra.c:572-664`) **[V]**: the menu calls `runtime_ue4_request_cvar(name,
value)` (value <= 7 chars, `ue4_cvar_values[][8]`); the game thread's `scePadReadState` poll (`src/runtime_pad.c:420`)
sets it through `IConsoleVariable::Set` (vtable +0x68) with `GetFlags()` (+0x20) & 0xff000000 as SetBy, after checking 7
bytes at the registration site, and only after 600 polls (the main menu). Adding a cvar = one `ue4_cvars[]` row.
UE4 side **[I]**: for `ECVF_RenderThreadSafe` (0x20) cvars `Set` on the game thread propagates the value to the render
thread at the next render command (consistent at a frame boundary); cvars without it are read racily (one aligned
int/float: benign, as UE4's own console). `OnChanged` callbacks and the console-variable sinks run (sinks next game
frame), so cached scalability values (`GetCachedScalabilityCVars`) update as with the in-game console.

**B. Port setting atomics** (`gpu/shim/gow3_settings.h`): read every use by the renderer. Live today **[V]**: frame rate
limit, VRR, frames queued (`vk_presenter.cpp:551`), async shaders (`vk_pipeline_cache.cpp:637`), VSync
(`Swapchain::UpdateVsync`, `vk_swapchain.cpp:341`), FSR 1 / RCAS (`vk_presenter.cpp:508-510`), and three perf patches,
`PerfLive = {small buffers, lock-free memory, cheap frees}` (`gow3_settings.h:156`): small buffers read
`perf[PerfSmallBuffers]` per buffer bind (`buffer_cache.cpp:983`); the other two through `runtime_memory_set_perf`,
called while the menu's Performance tab draws (`gow3_overlay.cpp:467`, `runtime_memory.c:142`).
The rest of the port's switches are `static const bool x = []{ getenv(...) }()`: read once, at first use.

**C. Toggle file** (`GOW3_TOGGLE_FILE`, `src/runtime_memory.c:1135-1174`) **[V]**: a watcher thread re-reads a number
every 250 ms into `runtime_disabled_optimizations`; `Gow3Toggle::Disabled(bit)` (`gow3_toggles.h:84`) is a relaxed load
per use. Started only when the variable is set (`runtime_memory.c:1215`); **the launcher always sets it**
(`launcher/medievil_launcher.py:407`, `user/screenshots/toggles.txt`). Bits only switch an optimization **off** (set =
off); a path whose environment switch was off at start cannot be switched on. Honoured live **[V]** (grep of
`Disabled(`): 1 region cache (`runtime_memory.c:1329/1375/1405`), 2 fetch shader cache, 4 page-tracking early exit, 8
pending poll limit, 16 threaded recording, 32 image descriptor cache, 64 lock-free upload check, 128 FindImage cache,
256 deferred uploads, 512 access memo, 1024 texture binding memo, 2048 coarse read tracking, 4096 prepared resources,
8192 draw preparation, 16384 deferred stream copies, 32768 hot pages, 65536 fault window, 131072 parallel copies, 262144
async fences, 524288 pool small copies, bit 30 UpdateImage fast path, **bit 31 two meanings** (narrow image stages,
`vk_rasterizer.cpp:2413`, and pool views kept across a decommit, `runtime_memory.c:997`), bits 20-29 motion /
upscaler debug, 32 record prefetch, 33 texture view memo, 34 texture bind helper, 35 early draw inputs, 36 constant
ring, 37 draw pipeline (`vk_rasterizer.cpp:394`), 38 pipelined tasks, 39 pending fence waits, 40 pipelined dispatch, 41
recorder fences (also deferred EOS, `liverpool.cpp:448`), 42 pipelined memory writes, 43 multi-copy shader, 44 render
state memo, 45 texture set memo (`vk_rasterizer.cpp:2794`), 46 pipelined indirect draws, 47 scene attachments only, 48
sample scene proxies, 49 ordered guest writes, 50 scene half res, 51-55 TAA, 56 unmap discard, 57 scene mip bias, 58
volume slice views, 59 stencil plane writes, 60 idle-EOP skip **and** targeted EOP wait (W2, `texture_cache.cpp:313/338`),
61 undefined targets, 62 targeted host-copy waits, 63 precise CPU writes. User options it touches: perf_texture_memo
(bit 45: the memo off altogether, stronger than the patch's "off"), perf_gpu_data_no_waits (bit 60: W2 off; bit 41:
deferred EOS off, plus recorder fences), launcher "Two-stage GPU pipeline" (bit 37: off only).

## Summary table

Live = **done** (already live), **easy** (a `ue4_cvars[]` row or an atomic, no state), **medium** (state to reset or
reallocation), **hard**, **restart**.

| Option (where) | What it changes | Live? | How | Risk |
|---|---|---|---|---|
| Preshadows (menu, now) | `r.Shadow.Preshadows` | done | A (`0x5bcff28`) | verified N-25 |
| Effects resolution (menu now; launcher marks restart) | `r.SeparateTranslucencyScreenPercentage` | done | A (`0x5bd2d68`) | separate translucency target reallocated **[I]**; not verified in game |
| Game resolution now (menu) | `r.SetRes` | done | A-like (`0x5c28320`) | I1: menus' navigation at non-boot sizes |
| Graphics quality (menu restart, launcher) | pak `[SystemSettings]`: ~25 cvars (below) | **easy** for ~20, medium for 3, restart for 1 | A: per-cvar rows, a preset = a batch | shadow / AO / fog targets reallocated (spike, one frame mixed); `r.DetailMode` hitch |
| Render scale (menu restart, launcher) | pak `r.ScreenPercentage` (+ `r.TemporalAA.Upsampling` below 100) | **medium** | A (`0x581a048`, `0x5c1a120`) | all scene targets reallocated (VRAM / guest spike, TAA history reset); above the boot scale can exhaust the 512 MB framebuffer pool (crash) |
| Output resolution (menu restart, launcher) | boot rect / 4K branch eboot patch at load, VideoOut size (`GOW3_INTERNAL_WIDTH/HEIGHT`), DMEM, framebuffer pool patch, `r.setres` | **restart** (live alternative exists: r.SetRes) | - | - |
| Engine frame rate (menu restart, launcher) | pak `[/Script/Engine.Engine] SmoothedFrameRateRange`; `GOW3_FPS_CAP`, `GOW3_VBLANK_HZ`, `GOW3_NO_FRAME_CAP` | **hard** (moot: see notes) | write `GEngine->SmoothedFrameRateRange` (address and offset unknown) | game timing above 120 **[?]** |
| Deferred GPU readbacks | `GOW3_DEFERRED_READBACK` (const member, `texture_cache.h:512`) | **hard** | drain readbacks, switch, keep watchers balanced | lost or stale readbacks if not drained |
| Game reads GPU data without waiting | `GOW3_STALE_READBACK` (`texture_cache.h:521`) | **medium-hard** | drain pending readbacks, then flip (watch mode per entry) | unbalanced page watchers |
| perf_hzb_occlusion | hooks at load (`probe.c:549`) + pak `r.AllowOcclusionQueries`/`r.HZBOcclusion`/`r.NumBufferedOcclusionQueries` | **medium** | always install hooks (analysed eboot), switch `r.AllowOcclusionQueries` live | first frames all-visible (no pop-in) **[I]** |
| perf_no_decal_prepass | pak `r.DBuffer=0`, `r.EarlyZPass=1` | **easy-medium** | A (`0x581a438` ReadOnly, `0x5b59770`) | DBuffer targets allocated on first use; prepass doubles draws (the point) |
| perf_particle_single_pass | pak `r.GPUParticle.FixDeltaSeconds=0` | **easy** | A (`0x5c14d18`) | none expected **[I]** |
| perf_small_buffers | `perf[PerfSmallBuffers]` | done | B | - |
| perf_texture_memo | `GOW3_TEXSET_REGIONS`, `GOW3_TEXSET_TWO_WAY` (statics, `vk_rasterizer.cpp:2770/2809`) | **easy-medium** | atomics + clear `texture_sets` on stage B | stamps of the other mode: clear first |
| perf_lockfree_memory | `GOW3_OPTIMISTIC_CLAMP` (live), `GOW3_REGION_BUCKETS` (static) | done (half) | B; buckets still static (`runtime_memory.c:1259`) | buckets: per-thread caches hold stamps of one mode |
| perf_cheap_frees | `GOW3_DECOMMIT_KEEP_ACCESS` | done | B | - |
| perf_gpu_data_no_waits | readback queue (device queue at creation), twins, deferred EOS, targeted EOP | **medium** (twins hard) | always create the queue; atomics; twins: resolve all on disable | twins: guest bytes kept beside GPU bytes |
| perf_4k_memory | `GOW3_DMEM_MB` (guest direct memory, `runtime_memory.c:28`), pak heap / pool sizes | **restart** | - | game sizes its pools at boot |
| perf_lean_barriers | `GOW3_SKIP_READ_BARRIERS`, `GOW3_DEPTH_SAMPLE_IN_PLACE` (statics, `image.cpp:402`, `vk_rasterizer.cpp:3202/3216`, `vk_runtime.cpp:217`) | **easy** | atomic per use (stage B) | none: each decision is locally correct **[I]** |
| Frame limiter (launcher) | `GOW3_REFLEX` (device extension, `vk_instance.cpp:341`), `GOW3_PRECISE_LIMIT`, `GOW3_LIMIT_HOLD`, `GOW3_PRESENT_PRIORITY`, `GOW3_REFLEX_SLEEP_BEFORE_PRESENT` | **medium** | enable VK_NV_low_latency2 when present; atomics in the vblank loop | one paced frame off |
| Vblank tick rate (launcher) | `GOW3_VBLANK_HZ` (static, `emulator_settings.h:36`; refresh reported to the guest in the VideoOut ctor) | **medium** | re-anchor the tick grid on change | the guest's 119.88 Hz report is read at boot |
| Present on the vblank thread | `GOW3_PRESENT_THREAD` (swap thread made in the ctor, `driver.cpp:88`) | **hard** | start/stop the swap thread | queue hand-over |
| Precise vblank tick | `GOW3_PRECISE_TICK` (static, `driver.cpp:1184`) | **easy** | atomic, re-anchor grid | one tick off |
| VSync special modes (launcher, restart) | `GOW3_PRESENT_MODE` (re-read at every swapchain build, `vk_swapchain.cpp:323`) | **easy** | a setting + `UpdateVsync`-style recreate | `waitIdle` hitch (as VSync today) |
| Frame timing trace | `GOW3_FLIP_TRACE` (static path) | easy | open on demand | diagnostics only |
| Two-stage GPU pipeline (launcher, app) | `GOW3_DRAW_PIPE` (thread at init) | off: done (bit 37); on: restart | C | - |
| GPU readbacks mode (launcher, app) | `GOW3_READBACKS` (static; also gates twins, `buffer_cache.cpp:243`) | restart | - | - |
| HDR (launcher, app) | `GOW3_HDR` (static), swapchain format | medium | swapchain recreate + presenter path | - |
| Shader cache / warm-up (launcher, app) | load-time work | n/a | - | - |

## UE4 console variables: objects for `ue4_cvars[]` (CUSA11227 01.00)

All **[V]** from `regs.txt`: the registration `lea rsi, [rip+name]` (site, the 7 bytes to check), the call, and the
stores of the result at object+8 and a vtable at object. Flags: 0x20 RenderThreadSafe, 0x40 Scalability, 0x80
ScalabilityGroup, 0x04 ReadOnly. "Ref" = `FAutoConsoleVariableRef` (same layout, the value lives in the referenced
global). Default = the constructor argument.

| Cvar | Object | Site | Site bytes | Type, default, flags | Per frame? **[I]** |
|---|---|---|---|---|---|
| r.ShadowQuality | 0x5819f88 | 0x731dda | 48 8d 35 fd 46 53 03 | int 5 (PS4Engine.ini: 3), 0x60 | yes |
| r.Shadow.MaxCSMResolution | 0x581a228 | 0x73294f | 48 8d 35 78 6a 53 03 | int 2048, 0x60 | yes (CSM targets realloc) |
| r.Shadow.MaxResolution | 0x581a210 | 0x7328e6 | 48 8d 35 79 69 53 03 | int 2048, 0x60 | yes (targets realloc) |
| r.Shadow.MinResolution | 0x5bd0068 | 0x1611cac | 48 8d 35 4f 7b 74 02 | int 32, 0x20 | yes |
| r.Shadow.RadiusThreshold | 0x5bcfea0 (Ref, float at 0x5606b18) | 0x1611445 | 48 8d 35 9c 6e 74 02 | float, 0x60 | yes |
| r.Shadow.CacheWholeSceneShadows | 0x5bcfec0 (Ref, int at 0x5606b20) | 0x1611507 | 48 8d 35 0a 70 74 02 | int, 0x60 | yes (caches dropped / rebuilt) |
| r.Shadow.CSM.MaxCascades | 0x5bc82a8 | 0x1586344 | 48 8d 35 c7 88 7c 02 | int 10, 0x60 | yes |
| r.Shadow.DistanceScale | 0x581a3c0 | 0x733053 | 48 8d 35 fc 7e 53 03 | float, 0x60 | yes |
| r.AmbientOcclusionLevels | 0x5b533c8 | 0x114b2cd | 48 8d 35 84 38 bb 02 | int -1, 0x60 | yes |
| r.SSR.Quality | 0x5bc0228 | 0x14f84d3 | 48 8d 35 0c f2 84 02 | int 3, 0x60 | yes (new pipelines compile) |
| r.MaxAnisotropy | 0x581a1f8 | 0x73287d | 48 8d 35 84 68 53 03 | int 4, 0x60 | partial: samplers made later **[?]** |
| r.ViewDistanceScale | 0x581a2d0 | 0x732c2d | 48 8d 35 7e 73 53 03 | float, 0x60 | yes (cached scalability, sink) |
| r.StaticMeshLODDistanceScale | 0x5bc8ef8 | 0x160d78a | 48 8d 35 e1 60 74 02 | float, 0x60 | yes |
| foliage.LODDistanceScale | 0x5c0a180 | 0x2119693 | 48 8d 35 aa e7 ce 01 | float, 0 | yes |
| foliage.DensityScale | 0x5c0a210 | 0x21198f1 | 48 8d 35 d2 ea ce 01 | float, 0x40 | no: on component rebuild |
| grass.densityScale | registration not resolved (name at 0x3d8c81e; the pak's `grass.DensityScale` matches it case-insensitively) | | | | as grass regenerates **[I]** |
| r.DetailMode | 0x581a408 | 0x733186 | 48 8d 35 eb 81 53 03 | int 2, 0x60 | sink recreates all render state (hitch) |
| r.MaterialQualityLevel | 0x581a078 | 0x7321ee | 48 8d 35 f7 59 53 03 | int 1, 0x60 | no: shader maps per level (cooked) |
| r.MotionBlurQuality | 0x5819fa0 | 0x731e43 | 48 8d 35 b6 47 53 03 | int 4, 0x60 | yes |
| r.DepthOfFieldQuality | 0x581a030 | 0x7320b3 | 48 8d 35 7c 53 53 03 | int 2, 0x60 | yes |
| r.VolumetricFog | 0x5bd3bb0 (Ref, int at 0x5606b74) | 0x1665ee2 | 48 8d 35 0b 9f 6f 02 | int, 0x60 | yes (3D volumes allocated) |
| r.LensFlareQuality | 0x5819f10 | 0x731bca | 48 8d 35 43 41 53 03 | int 2, 0x60 | yes |
| r.SceneColorFringeQuality | 0x5819f40 | 0x731c9c | 48 8d 35 3d 44 53 03 | int 1, 0x60 | yes |
| r.BloomQuality | 0x5819f28 | 0x731c33 | 48 8d 35 18 42 53 03 | int 5, 0x60 | yes |
| r.RefractionQuality | 0x5bc81a0 | 0x1585ec6 | 48 8d 35 eb 81 7c 02 | int 2, 0x60 | yes |
| r.ScreenPercentage | 0x581a048 | 0x732124 | 48 8d 35 27 56 53 03 | float, 0x40 | yes (scene targets realloc) |
| r.TemporalAA.Upsampling | 0x5c1a120 | 0x25662e6 | 48 8d 35 bd 63 8d 01 | int 0, 0 | yes |
| r.SceneRenderTargetResizeMethod | 0x5bbfc90 | 0x14f7ec7 | 48 8d 35 d4 e1 84 02 | int 0, 0x20 | yes |
| r.AllowOcclusionQueries | 0x5bc8170 | 0x1585de6 | 48 8d 35 79 80 7c 02 | int 1, 0x20 | yes |
| r.HZBOcclusion | 0x5bc8eb8 (Ref, int at 0x5bc8ec8) | 0x160d671 | 48 8d 35 f6 5d 74 02 | int, 0x20 | yes |
| r.NumBufferedOcclusionQueries | 0x581a0f0 | 0x7323f2 | 48 8d 35 23 5f 53 03 | int 1, 0x20 | yes; keep fixed (ring index) |
| r.DBuffer | 0x581a438 | 0x733255 | 48 8d 35 28 85 53 03 | int 1, **0x24 ReadOnly** | yes (cached lookup at 0xfb204d) |
| r.EarlyZPass | 0x5b59770 | 0x119a778 | 48 8d 35 75 9e b6 02 | int 3, 0x40 | yes |
| r.GPUParticle.FixDeltaSeconds | 0x5c14d18 | 0x23933b3 | 48 8d 35 c0 16 a9 01 | float, 0 | yes |
| r.Streaming.PoolSize | 0x5c27590 | 0x272128e | 48 8d 35 19 3b 73 01 | int -1, 0x40 | budget per update; pool memory fixed at boot |
| t.MaxFPS | 0x5c285a0 | 0x278ee1f | 48 8d 35 0a fd 6c 01 | float, 0 | does not cap here (START-HERE) |
| sg.ResolutionQuality / ViewDistance / AntiAliasing / Shadow / PostProcess / Texture / Effects / Foliage | 0x5c198c8 / 0x5c198e0 / 0x5c198f8 / 0x5c19910 / 0x5c19928 / 0x5c19940 / 0x5c19958 / 0x5c19970 | 0x2564d57 ... 0x2565055 | | float / int 3, 0x80 | see sg.* below |

Calibration **[V]**: the same scan gives r.Shadow.Preshadows 0x5bcff28 (site 0x1611700, int 1, flags 0x20) and
r.SeparateTranslucencyScreenPercentage 0x5bd2d68 (site 0x16651ee, float, 0x40), as in `ue4_cvars[]`; r.SetRes
0x5c28320 is built by a constructor call (0x221cf60), the object `lea rbx` before the name. Sites calling
`[rax+0x90]` then `[rcx+0x40/0x48]` are cached lookups, not registrations (e.g. r.DBuffer 0xfb204d,
r.SeparateTranslucencyScreenPercentage 0x145d993). `r.GarlicHeapSizeMB` / `r.OnionHeapSizeMB`: **no string in the
eboot** (UTF-16 or ASCII, case-insensitive) **[V]**.

## Per-option notes

### Graphics quality (ue4_quality; `scripts/ue4_override.py:153-217, 300-307`)

What it writes **[V]**: `QUALITY[preset]`, then for every preset (console included) `LOCAL_SHADOWS` (`r.Shadow.Min/
MaxResolution`, `r.Shadow.RadiusThreshold`, `r.Shadow.CacheWholeSceneShadows=1`) and `SUN_SHADOWS`
(`r.Shadow.CSM.MaxCascades`, `r.Shadow.DistanceScale`), which overwrite the QUALITY entries of the same names (so
`high`/`ultra`'s `r.Shadow.MaxResolution=2048`, ultra's `r.Shadow.DistanceScale=2` / `MaxCascades=4` and high's
`DistanceScale=1.5` are dead entries). All at `SetBySystemSettingsIni` priority (the pak replaces
`Engine/Config/PS4/PS4Engine.ini`, whose own `[SystemSettings]` already has `r.ShadowQuality=3`, `r.HZBOcclusion=0`,
`r.EarlyZPass=1`) **[V]**.

Live: a preset becomes a batch of `runtime_ue4_request_cvar` calls, the union of the keys of all presets. **Every key
has an object above except `grass.densityScale`.** Needs: the "console" baseline value of each key (what the game had
before the pak: engine default, `PS4Engine.ini`, or the startup scalability level). Two ways: read each cvar once at the
main menu through the IConsoleVariable vtable (GetString / GetInt / GetFloat; slot offsets not verified **[?]**), or let
`run.py` compute them from the game's configs (`BaseScalability.ini` @3 sections, `PS4Engine.ini`, the registration
defaults above) and pass them in. The second needs no new RE.
Per cvar **[I]** (UE 4.21): shadows (quality, resolutions, cascades, distance, radius threshold, cache), AO levels, SSR
quality, view distance, static mesh LOD scale, foliage LOD scale, motion blur, depth of field, volumetric fog, lens
flare, fringe, bloom, refraction are read every frame on the render thread or from the scalability cache the sink
refreshes. Exceptions:
- `r.MaterialQualityLevel` (Ultra only, value 1): the default is 1 **[V]**, so drop it from Ultra; other values need the
  cooked shader maps of that level: restart at best **[I]**.
- `r.DetailMode` (Ultra 2 = default 2 **[V]**: a no-op too; other values): the scalability sink recreates every
  component's render state (`FGlobalComponentRecreateRenderStateContext`) **[I]**: a hitch, components appear / vanish.
- `foliage.DensityScale`, `grass.densityScale` (Low, `diag_lod`): only when foliage / grass instances are rebuilt
  (level load, or grass regeneration as the camera moves) **[I]**: live value is partial; say "on next level".
- `r.MaxAnisotropy` (High/Ultra 16): texture samplers carry the value from their creation **[I]**; the guest's sampler
  sharps then reach the port's Vulkan samplers. Partial live (new textures). A port-side anisotropy override in the
  sampler translation would be fully live and engine-independent **[I]**.
Risk: shadow-map, AO, SSR, bloom and fog targets change size or appear: UE4's render-target pool allocates, the port
creates images (N-24 parking keeps the old ones 10 s / 768 MB, `texture_cache.cpp:755-757` **[V]**, so switching back
is cheap); new shader variants (SSR quality, shadow filter quality) compile (async shaders: an effect may show a moment
late). A batch lands between two game frames; render-thread-safe cvars switch together at the frame boundary, the
others (flags 0) at once: at most one frame of mixed state **[I]**. The 7-char value limit fits every preset value
**[V]**.

### sg.* scalability groups (shortcut?)

Objects found (flags 0x80) **[V]**. In 4.21 each `sg.X` change calls `SetGroupQualityLevel` → applies the
`[XQuality@N]` section of the game's Scalability ini at `ECVF_SetByScalability` **[I]**. The game ships only
`Engine/Config/BaseScalability.ini` (no PS4 or project scalability ini; PS4 device profile sets only
`r.Streaming.PoolSize=2000`, `r.VolumetricFog.GridPixelSize=16`, `GridSizeZ=64`) **[V]**. Consequences: (1)
`SetByScalability` is below `SetBySystemSettingsIni` and `SetByDeviceProfile`, so every cvar the pak or `PS4Engine.ini`
sets keeps its value **[I]**: e.g. `sg.ShadowQuality` would change only the shadow-group cvars nobody set
(`r.LightFunctionQuality`, `r.DistanceFieldShadowing`, `r.AOQuality`, `r.Shadow.PreShadowResolutionFactor`,
`r.Shadow.CSM.TransitionScale`, `r.CapsuleShadows`...), not the ones the presets tune; (2) the engine's @0-@3 values
are not the port's presets (fixed local shadow sizes against flicker, sun cascade counts). **Not a shortcut.** Per-cvar
`Set` (mechanism A, current SetBy) works whatever the priority.

### Render scale (render_scale; `ue4_override.py:308-311`)

`r.ScreenPercentage` (flags 0x40, game thread) and `r.TemporalAA.Upsampling=1` below 100 **[V]**. UE4 4.21 reads the
screen percentage every frame (`FLegacyScreenPercentageDriver`) **[I]**; with `r.SceneRenderTargetResizeMethod=0`
(registration default **[V]**, not set by the game's configs) the scene targets are reallocated to the new size on the
next frame **[I]**. Live = two rows (upsampling first, then the percentage). Risks: one reallocation of every scene
target (VRAM and guest heap spike, a long frame), TAA history reset (one soft frame), and memory: the menu already
refuses larger outputs when the boot scale is above 100 ("1800p at 200% needs 6400x3600 targets: crash",
`gow3_overlay.cpp:515-520` **[V]**). Safe rule: live values at or below the boot scale (or <= 100 when the output is
above 1080p); larger values on restart. The menu's "Game resolution now" buttons must use the same rule.

### Effects resolution (already live)

`r.SeparateTranslucencyScreenPercentage` 0x5bd2d68 (float, flags 0x40) **[V]**; the renderer reads it through a cached
data pointer (lookup site 0x145d993) every frame and resizes the separate translucency target **[I]**. The launcher's
row still says "Applied at start" with `restart=True` (`medievil_launcher.py:1103-1106`) although it is live in game
**[V]**: text only. Not yet verified in game.

### Output resolution (restart)

Boot size: rodata rect or the 4K branch patched at load (`graphics_options.output_environment`, study-boot-resolution),
VideoOut size from `GOW3_INTERNAL_WIDTH/HEIGHT` (static, `emulator_settings.h:25-26`), the framebuffer-pool patch,
`GOW3_DMEM_MB` (`runtime_memory.c:28`, read once) **[V]**. All at load. The live alternative exists (r.SetRes buttons)
with the I1 menu-navigation caveat. Stays a restart.

### Engine frame rate (restart; moot)

The pak's `SmoothedFrameRateRange` upper bound (`ue4_override.py:143-148`), read from config into the `UEngine` object
at init; `UGameEngine::GetMaxTickRate` reads the member every tick **[I]**. Live would mean writing the float in
`GEngine` (address and member offset not found: RE work); `t.MaxFPS` does not cap in this build (START-HERE).
Port side: `GOW3_FPS_CAP` / `GOW3_NO_FRAME_CAP` only matter with `GOW3_VBLANK_HZ=0` (`emulator_settings.h:44-55`);
the default tick is 960 **[V]**. Since the in-game frame rate limit is live, the simpler product answer is to boot at
the highest engine rate and cap live; the open question is game timing above 120 (an old note says running slowed,
`emulator_settings.h:34`) **[?]**.

### Deferred GPU readbacks / game reads GPU data without waiting (restart; low value live)

`deferred_readbacks` is a `const bool` member, `stale_readbacks` derived from it (`texture_cache.h:512-524`) **[V]**.
Stale mode watches pending copies for writes only, normal mode for reads and writes, and each entry is unwatched with
the mode it was watched with (`WatchReadback<track, read>`, `texture_cache.cpp:176-183`) **[V]**. A live switch must
first drain (`scheduler.Finish()`, `RetireReadbacks()` under `readback_mutex`, cancel the rest) on the GPU thread, then
flip; turning deferred off also has to wait out `pending_readbacks`. Both are fallback switches ("switch off if...");
keep them restarts.

### perf_hzb_occlusion (restart → medium)

Off today: no hooks (`install_hzb_remap` returns unless `GOW3_HZB_REMAP=1`, `probe.c:549-551`) and the pak sets
`r.AllowOcclusionQueries=0`; on: hooks + `r.AllowOcclusionQueries=1`, `r.HZBOcclusion=1`,
`r.NumBufferedOcclusionQueries=3` (`ue4_override.py:330-334`) **[V]**. Live: install the hooks whenever the eboot
matches (they only act when the game calls MapResults / Submit, i.e. with HZB on **[I]**), boot with HZB=1 and
NumBuffered=3 always, and switch only `r.AllowOcclusionQueries` (0x5bc8170, render thread safe). Keep
`r.NumBufferedOcclusionQueries` fixed (ring indexing; the remap reads 3 frames late, T-HZ4e). On enabling, results start
invalid → primitives read visible (study-hzb-double-buffer: `IsValidFrame` false = visible) **[V]** for the game,
**[I]** for the switch: no pop-in, a few frames without culling.

### perf_no_decal_prepass (restart → easy-medium)

`r.DBuffer` 0x581a438 (ReadOnly; read per call through a cached lookup, `IsUsingDBuffers`) and `r.EarlyZPass` 0x5b59770
(per frame in the renderer constructor) **[I]**. The base pass shaders are the cooked ones in both states (booting with
`r.DBuffer=0` already works, T-Z2-T-Z5), so the switch only changes passes and targets **[I]**. Risk: DBuffer A/B/C
targets (render size) allocated on the first frame with DBuffer=1; the full prepass doubles draws (the measured cost).

### perf_particle_single_pass (restart → easy)

`r.GPUParticle.FixDeltaSeconds` 0x5c14d18 (float, flags 0): read every frame by the GPU particle simulation **[I]**.
One row. Risk: none expected; the fixed-step accumulator restarts **[?]**.

### perf_texture_memo (restart → easy-medium)

Two statics (`TextureSetRegions`, `two_way`, `vk_rasterizer.cpp:2770-2812`) **[V]**; memo slots store a stamp of one
mode. Make them atomics read once per `BindTexturesFromSet` and clear `texture_sets` (stage B owns it) when either
changes. Today's toggle bit 45 switches the whole memo off live (stronger than the patch's "off").

### perf_lockfree_memory (live, half)

The menu calls `runtime_memory_set_perf` for `perf_optimistic` only; `GOW3_REGION_BUCKETS` stays the boot value
(`static int on`, `runtime_memory.c:1259-1262`) **[V]**. Restart still needed for the bucket half. Live buckets: the
per-thread region caches (`__thread region_cache[8]`) hold entries validated by one mode; switching needs each thread
to drop its cache (a global epoch compared on lookup) **[I]**: medium.

### perf_gpu_data_no_waits (restart → medium, twins hard)

Four parts **[V]**: readback queue (a second device queue chosen at device creation, `vk_instance.cpp:480`; use gated by
a static in `FastReadback::Available`, `buffer_cache.cpp:131-137`), GPU write twins (static `TwinsEnabled`,
`buffer_cache.cpp:239`), deferred EOS (static, `liverpool.cpp:443`; toggle bit 41 already disables it live), targeted
EOP wait (static, `texture_cache.cpp:334`; toggle bit 60 already disables it live). Live: always create the queue when
the family exists and make `Available` an atomic (easy); EOS/EOP atomics (easy; turning them off falls back to the
synchronous waits, always correct **[I]**); twins: on is easy, off must resolve every twin under `twins_mutex` after the
GPU is idle, since uploads and downloads consult them (`buffer_cache.cpp:228-236`) (hard).

### perf_4k_memory (restart)

`GOW3_DMEM_MB` is read once and fixes the guest's direct memory size (`runtime_memory.c:28-36`) **[V]**; the game
sizes its pools from it at boot **[I]**. Restart by nature. See "Found on the way" on the pak half.

### perf_lean_barriers (restart → easy)

Three statics, all consulted on stage B per draw / per image access: `SkipReadBarriers` (`image.cpp:402`),
`skip_read_barriers` and `depth_in_place` (`vk_rasterizer.cpp:3202-3219`), `SampleDepthInPlace` (`vk_runtime.cpp:217`)
**[V]**. Replace them with one atomic read per use. No reset needed: with skipping on, reads are merged into the tracked
state, so a later full barrier (after switching off) still waits for them; switching on mid-frame skips only read-after-
read **[I]**. A diagnostic fallback, so a live A/B is useful.

### Presentation (launcher only)

- Present mode (the VSync row's "Off, allow tearing" / "Adaptive", `restart=True`, `medievil_launcher.py:1035`):
  `FindPresentMode` re-reads `GOW3_PRESENT_MODE` at every swapchain build (`vk_swapchain.cpp:323`) **[V]**: a setting
  plus the existing `UpdateVsync` recreate path makes it live. Easy.
- Precise vblank tick, frame limiter (precise / hold / priority), Reflex sleep placement: statics read at vblank-thread
  start (`driver.cpp:1119-1187`) and used per iteration **[V]**: atomics, plus a grid re-anchor (`next_tick`,
  `grid_start`). Reflex needs `VK_NV_low_latency2` enabled at device creation (`vk_instance.cpp:341-349`): enable it
  whenever present, then switch per frame. Thread priority: re-apply from inside the thread. Medium.
- Vblank tick rate: `GetVblankFrequency` static (`emulator_settings.h:36-42`); the VideoOut ctor reports 119.88 Hz to
  the guest when the tick is >= 120 (`driver.cpp:85-87`) **[V]**, read by the game at boot **[I]**: live only within the
  same side of 120. Medium, low value.
- Present on the vblank thread: the swap thread exists or not from the ctor (`driver.cpp:88-92`) **[V]**: hard, keep
  restart.

## Found on the way

- **`r.GarlicHeapSizeMB` / `r.OnionHeapSizeMB` do not exist in the 01.00 eboot** (no string) **[V]**, yet the pak sets
  `r.GarlicHeapSizeMB=4600` for outputs above 1080p (`ue4_override.py:321-323`) and the shipped `PS4Engine.ini` has 2600.
  Probably inert **[I]**; the effective 4K memory change is `GOW3_DMEM_MB` (+ framebuffer pool). Worth one check before
  crediting the heap value in docs.
- **`r.Streaming.PoolSize`**: the PS4 device profile sets 2000 (`OVERBITE/Config/PS4/PS4DeviceProfiles.ini`) **[V]** at
  `SetByDeviceProfile`, above the pak's `SetBySystemSettingsIni`, so the pak's 1600 is probably ignored **[I]** (the
  PS4 game already had 2000, not "400-800 MB").
- **Toggle bit 31 has two meanings** (`gow3_toggles.h:65` narrow image stages; `runtime_memory.c:997` pool views across
  a decommit) **[V]**: writing 2147483648 switches both off.
- **"Lock-free memory map lookups" live switches only `OPTIMISTIC_CLAMP`**; `REGION_BUCKETS` needs a restart
  (above) **[V]**; the menu presents it as fully live.
- The "Small read-only buffers/s" stats line is printed whenever `GOW3_STREAM_CLEAN` is set at all, whatever the live
  state (`driver.cpp:715`) **[V]**.
- Ultra's `r.MaterialQualityLevel=1` and `r.DetailMode=2` equal the registration defaults **[V]**: no-ops unless
  something lowered them.

## Recommended order (user value per effort)

1. **Graphics quality live** (mechanism A, ~25 rows from the table, a preset = batch, baseline values computed by
   `run.py` from the game's configs). Biggest value: the most-changed option, today a full restart. Drop
   `r.MaterialQualityLevel` from Ultra; label foliage/grass density "next level"; keep `r.DetailMode` out of the live
   batch (or accept the hitch). One launch: switch presets in the heavy Graveyard view, watch the overlay.
2. **Particle single pass and decal prepass live** (3 rows: `r.GPUParticle.FixDeltaSeconds`, `r.DBuffer`,
   `r.EarlyZPass`). Tiny effort; makes two A/B patches instant (the decal prepass is ~600 draws).
3. **Lean barriers live** (three statics → one atomic). Small; turns the fights' fallback switch into a live A/B.
4. **Render scale live within the boot budget** (2 rows + a menu rule). Medium; high value for finding a scale, with
   the memory rule.
5. **Present mode live** (VSync special modes) and **precise tick** atomics. Small; removes a launcher-only restart.
6. **HZB live** (hooks always installed, `r.AllowOcclusionQueries` row). Medium; an A/B more than a user choice.
7. **Texture memo, GPU data without waits (minus twins), lock-free buckets.** Medium, diagnostic value.
8. Leave as restarts: output resolution (live r.SetRes exists), 4K memory, deferred / stale readbacks, engine frame
   rate (boot high, cap live instead), present thread, draw pipe on, readbacks mode, HDR.
