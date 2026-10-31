# Plan: graphics settings switched live

Status: started 2026-10-30. Parent: [plan-work-queue.md](plan-work-queue.md) item 4. Study (what can be live, what
needs a restart, addresses): [study-live-switches.md](study-live-switches.md).

## Scope (the user, 2026-10-30)

- **Graphics settings** are the target: the quality preset and the options a player trades for frame rate. Effects
  resolution and preshadows count as graphics settings.
- **Not** HZB occlusion or the other clear-win performance patches: no reason to switch them; most games don't.
- Also wanted: settings the launcher does not have yet, so a player can min-max (highest frame rate, keep what they
  care about most).

## Mechanism (built)

`src/runtime_extra.c`: `ue4_cvars[]` (name, TAutoConsoleVariable image offset, registration site, 7 bytes checked) and
`runtime_ue4_request_cvar(name, value)`; the game's input thread (`runtime_ue4_setres_poll`, from `runtime_pad.c`)
applies queued values through IConsoleVariable::Set (vtable +0x68) once the game reaches its menu. Settings: a
`gow3_settings` value saved in `medievil.ini`, a row in the in-game menu's "Applied now" section that requests the
cvar on change, and `Load()` requesting non-default values at start. New addresses: `cvar_find3.py` (session
scratchpad, see the study) or `cvar_find2.py`.

## Done

| Setting | cvar | Object | Notes |
|---|---|---|---|
| Effects resolution | `r.SeparateTranslucencyScreenPercentage` | 0x5bd2d68 | N-26, `9cc915e`; default 75%; verified live (target re-sized next window) |
| Preshadows | `r.Shadow.Preshadows` | 0x5bcff28 | N-25, `fdd12e8`; default on; ~12% fewer draws off |
| (Game resolution now) | `r.SetRes` | 0x5c28320 | the output size buttons (native output) |
| 13 graphics options (2026-10-30) | 16 cvars, `ue4_cvars[]` | study table | `gow3_settings.h` `UE4LiveOptions`: shadow resolution, shadow distance (+ cascades), shadow filtering, view distance, level of detail (static mesh + foliage), ambient occlusion, reflections, volumetric fog, motion blur, depth of field, bloom, lens flares, chromatic aberration. Choice 0 "Preset" restores the boot preset's values (`run.py` -> `GOW3_UE4_PRESET_CVARS`, `ue4_override.preset_cvars`: pak preset > PS4Engine.ini > BaseScalability@3 > engine default). Sites checked against the eboot (lea loads the name). Not yet verified in game: the user tests them one by one |
| Anisotropic filtering | port side, not a cvar | `sampler.cpp` | 16x for every filtered, linearly mipmapped, non-depth sampler (`GOW3_FORCE_ANISO=0`: the game's S#). No setting: the user wants 16x always |

## Next, in order

1. **Done (2026-10-30): presets fixed:** the dead High/Ultra/Medium/Low shadow entries removed (no applied value changed; checked per preset), `preset_values()` shared. Was: in `scripts/ue4_override.py`, High/Ultra's `r.Shadow.MaxResolution`,
   `r.Shadow.DistanceScale`, `r.Shadow.CSM.MaxCascades` are overwritten later by the local / sun shadow tables
   (study). A live preset must apply what the boot preset means.
2. **Quality preset live:** one batch of Set calls from one table shared with `ue4_override.py` (the study's cvar table
   has the object and the 7 bytes for every preset cvar except `grass.densityScale`). The "console" values come from
   the game's own configs (run.py can work them out). Caveats from the study: `r.MaterialQualityLevel` restart-only
   (Ultra's 1 is the default anyway); `r.DetailMode` live but rebuilds render state (one hitch); foliage / grass
   density only on rebuild; `r.MaxAnisotropy` only for textures made after the change. `sg.*` scalability groups are
   **not** a shortcut (lowest priority: the pak and PS4Engine.ini win).
3. **Individual options for min-maxing** (item 4's second half): the preset's components as their own live rows
   (shadows, AO, SSR, volumetric fog incl. the PS4 fog grid density, motion blur, view distance / LOD...). Which ones,
   and their visible effect against cost on this machine: to measure, one at a time.
4. **Render scale live** (`r.ScreenPercentage`, 0x581a048, read every frame): re-allocates every scene target, so live
   values must stay at or below the boot scale (framebuffer pool); needs a check in game.
5. Presentation: the launcher's special VSync modes and the precise vblank tick (the swapchain re-reads its mode when
   it is re-made).

Each new live setting: one launch to verify it applies (log line `UE4: <cvar> <value> (live)` and an effect in the
stats or the GPU profile), the user judges the look.

## Stays a restart (study)

Output resolution (eboot patches at load, VideoOut, guest memory; the live r.SetRes buttons cover it), 4K memory sizes,
deferred / stale GPU readbacks, engine frame rate (boot at the highest, cap with the live limiter), present thread,
turning the two-stage pipeline on, HDR.
