---
id: live-graphics-options
title: Live graphics options: 13 options through 16 more console variables; Preset restores the boot preset
area: ue4
status: active
platforms: all
default_windows: on
default_deck: on
origin: ours
class: M
commits: 6fa68196f5, 3cea271054
files: gpu/shim/gow3_settings.h, gpu/shim/gow3_settings.cpp, gpu/shim/gow3_overlay.cpp, src/runtime_extra.c, scripts/ue4_override.py, run.py, launcher/medievil_launcher.py
symbols: UE4LiveOption, UE4LiveOptions, UE4LiveCount, RequestUE4Live, ue4_live, ue4_cvars, preset_cvars, preset_values, LIVE_CVARS, LIVE_ENGINE_DEFAULTS
switches: GOW3_UE4_PRESET_CVARS
tests:
docs: plan-live-switches.md#mechanism-built, plan-live-switches.md#done, study-live-switches.md#summary, study-live-switches.md#ue4-console-variables-objects-for-ue4_cvars-cusa11227-0100, fix-inventory.md#7-medievil-specific-ue4-config-eboot-patches-hooks
depends_on: preshadows-live
related: effects-resolution, launcher-graphics-page, deck-preset, live-render-scale, force-aniso, single-particle-pass
checked: 2026-11-07
---
## What it does

`UE4LiveOptions` (gow3_settings.h): 13 options, each 1-2 cvars (16 in all, rows in `ue4_cvars[]` for 01.00 and
01.02): shadow resolution (Min/MaxResolution), shadow distance (DistanceScale + CSM.MaxCascades), shadow filtering,
view distance, level of detail (static mesh + foliage), AO, SSR, volumetric fog, motion blur, depth of field, bloom,
lens flares, chromatic aberration. A menu change calls `RequestUE4Live` -> `runtime_ue4_request_cvar`; `Load()`
requests every non-Preset ini value at start. Choice 0 "Preset" restores the boot preset: run.py passes
`GOW3_UE4_PRESET_CVARS` ("name=value;..." from `ue4_override.preset_cvars`: pak preset with `GOW3_UE4_CVARS` on
top > PS4Engine.ini `[SystemSettings]` > BaseScalability.ini @3 > `LIVE_ENGINE_DEFAULTS`; set only when the pak
write succeeds). `3cea271` put the 13 on the launcher's
Performance page.

## Invariants

- `LIVE_CVARS` (ue4_override.py) must list every cvar of `UE4LiveOptions`, or Preset prints "no preset value for X
  (restart for the preset)" and leaves the live value. `static_assert(UE4LiveCount == ... ue4_live)` ties the table
  to `Values::ue4_live`.
- Values fit `ue4_cvar_values[16]` (the header says 7 characters); a longer value is silently dropped.
- Each cvar's object and 7-byte registration site per build; a mismatch logs "unavailable" and the boot value stays
  (the same checks as preshadows-live).
- The ini keys use the option `tokens` ("preset", "256", ...); an unknown token reads as Preset (`ue4_live` 0).
- Live switching keeps each variable's SetBy (`ue4_set_cvar`); a later scalability / device-profile apply by the
  game could override a live value (not seen; not tested one by one).
- Assumption (class M): each of the 16 cvars takes effect when set at run time (study-live-switches, per option,
  mostly [I]); not checked in game one by one.

## Interactions

- preshadows-live: the mechanism (`ue4_cvars[]`, pad-poll apply) it extends.
- effects-resolution, live-render-scale, single-particle-pass: other users of the same queue.
- deck-preset: sets 12 of the 13 (shadows 256 / short / 1, view 0.7, LOD low, AO 2, SSR 1, fog 1, motion blur 0,
  DoF 0, bloom 2, flares 0; chromatic aberration "preset"); they apply at start through `Load()`.
- launcher-graphics-page: the launcher side of `3cea271` (labels and keys must match `UE4LiveOptions`).
- force-aniso (16x anisotropic filtering) shipped in `6fa6819` too; it is port-side, not a cvar.

## Evidence

- None measured: a feature. Not yet verified one by one in game (plan-live-switches.md "Done": "the user tests
  them one by one"). Sites checked against both eboots (each lea loads the cvar's name).

## History and pitfalls

- The High / Ultra presets' `r.Shadow.MaxResolution` / `DistanceScale` / `CSM.MaxCascades` were overwritten by the
  local / sun shadow tables (`LOCAL_SHADOWS`, `SUN_SHADOWS` applied last in `preset_values`). The code agrees with
  plan-live-switches.md: `6fa6819` ("presets cleaned up") removed the dead entries (`QUALITY['high' / 'ultra']` no
  longer set them; no applied value changed). plan-work-queue.md's "open" row for it is stale.
- `sg.*` scalability groups are not a shortcut: the pak and PS4Engine.ini outrank them (study).
