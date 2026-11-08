# Plan: graphics quality and the individual graphics settings in sync

Parent: [START-HERE.md](START-HERE.md). The user's design (2026-11-07/08); nothing built yet except where marked.

## The model (agreed)

- **Graphics quality is the only preset:** Ultra, High, Console (PS4), Medium, Low, Deck Optimized. Picking one sets
  every setting below to that level's value. **Custom** is shown (not picked) when any setting differs from the
  picked level; Custom keeps nothing hidden, because every difference between levels is a setting in the list.
- **Settings show real values**; no "Preset" and no "Console" entries. The value the PS4 itself uses is marked
  ("PS4 default" beside it when selected). Plain words where it reads well; a High setting under the Ultra preset is
  normal (nothing goes higher yet).
- **A value picked by hand that equals the level's counts as matching** (no Custom).
- **The in-game menu follows the same rules** (C++, `gpu/shim/gow3_settings.h` UE4LiveOptions,
  `gow3_overlay.cpp`); picking a quality there sets the settings, the new level arrives with the restart.
- **Ambient occlusion stays "Auto"** at Console (r.AmbientOcclusionLevels -1, the PS4's), unmeasured.
- Storage: settings store values; existing inis with `preset` are resolved once from their ue4_quality. The five new
  settings are written into the override pak (next start); making them live would need their cvar objects in both
  eboots (separate work).

## Where the values come from (checked 2026-11-07, 01.00 and 01.02 identical)

Console's values come from the game's `PS4Engine.ini` [SystemSettings] and `BaseScalability.ini` at level 3 (what the
PS4 runs), except two shadow settings that **upstream** (`c2ad62e`, bilal, 2026-10-13, before this work: shadow
flicker while moving and draw count) replaced for every level: fixed 512 point/spot shadow maps (PS4: sized on screen
up to 2048), sun distance 0.8 with 2 cascades (PS4: 1.0, up to 10), small objects (radius threshold 0.1; PS4 0.01),
shadow caching on (the engine default, so the same as the PS4). Settings only quality changed before (now in the list):
anisotropy (PS4 8; High/Ultra 16), sun shadow resolution r.Shadow.MaxCSMResolution (PS4 2048), radius threshold,
foliage / grass density (Low 0.6), refraction (Low off). Dropped: Ultra's r.DetailMode 2 (= the PS4's).

## The settings (to build after the shadow test)

\* = decided by the PS4 shadow test: if the PS4's values work, they are the highest choice and High / Ultra select them
(the fixed 2048 and the Far / Farthest sun distances leave the lists for now); if not, upstream's values stay, labelled
honestly, with the reason.

| # | Setting | Choices | Low | Medium | Console | High | Ultra | Applies |
|---|---|---|---|---|---|---|---|---|
| 1 | Shadow resolution | 256, 512, 1024, Dynamic up to 2048 (PS4)* | 256 | 512 | PS4* | PS4* | PS4* | live |
| 2 | Small-object shadows (new) | 0.15, 0.12, 0.10, All 0.01 (PS4)* | 0.15 | 0.12 | PS4* | PS4* | PS4* | next start |
| 3 | Sun / moon shadow distance | Short 0.6 / 2, Medium 0.8 / 2 (upstream's Console, merged with 0.75), 1.0 up to 10 (PS4)* | Short | Medium | PS4* | PS4* | PS4* | live |
| 4 | Sun / moon shadow resolution (new) | 1024, 1536, 2048 (PS4), 4096 | 1024 | 1536 | 2048 | 2048 | 4096 | next start |
| 5 | Shadow filtering | 1, 2, 3 (PS4), 4, 5 | 1 | 2 | 3 | 4 | 5 | live |
| 6 | View distance | 0.7, 0.85, 1.0 (PS4), 1.5, 2.0 | 0.7 | 0.85 | 1.0 | 1.5 | 2.0 | live |
| 7 | Level of detail | Low, PS4, High, Ultra | PS4 | PS4 | PS4 | High | Ultra | live |
| 8 | Foliage and grass density (new) | 60%, 100% (PS4) | 60% | 100% | 100% | 100% | 100% | next start |
| 9 | Texture filtering (new) | 8x (PS4), 16x | 8x | 8x | 8x | 16x | 16x | next start |
| 10 | Ambient occlusion | Off, 1, 2, 3, Auto (PS4) | Off | 1 | Auto | 3 | 3 | live |
| 11 | Reflections | Off, 1, 2, 3 (PS4), 4 | Off | 1 | 3 | 3 | 4 | live |
| 12 | Refraction (new) | Off, On (PS4) | Off | On | On | On | On | next start |
| 13 | Volumetric fog | Off, On (PS4) | Off | On | On | On | On | live |
| 14 | Motion blur | Off, Low, PS4 | Off | Low | PS4 | PS4 | PS4 | live |
| 15 | Depth of field | Off, Low, PS4 | Off | Low | PS4 | PS4 | PS4 | live |
| 16 | Bloom | Off, Low, PS4 | Low | PS4 | PS4 | PS4 | PS4 | live |
| 17 | Lens flares | Off, Low, PS4 | Off | PS4 | PS4 | PS4 | PS4 | live |
| 18 | Chromatic aberration | Off, On (PS4) | Off | On | On | On | On | live |

Deck Optimized: 256, 0.15, Short, 1024, 1, 0.7, PS4 LOD, 60%, 8x, AO 2, reflections 1, refraction off, fog on, motion
blur off, DoF off, bloom Low, flares off, chromatic aberration off.

## PS4 shadow test (prepared 2026-11-08, not run)

PC, current build, Console quality. `extra-env.txt`: `GOW3_UE4_CVARS=r.Shadow.MinResolution=32;r.Shadow.MaxResolution=
2048;r.Shadow.RadiusThreshold=0.01;r.Shadow.CSM.MaxCascades=10;r.Shadow.DistanceScale=1.0` (written into the override
pak and into the in-game "Preset" choice) and `GOW3_FRAME_STATS=1`; medievil.ini shadow_resolution and shadow_distance
set to `preset` (were 512 / console; backup `medievil.ini.bak-before-ps4-shadows-20261108`). Same-launch A/B in the
in-game menu: shadow resolution Preset (PS4, dynamic) vs 512; shadow distance Preset (PS4) vs Console (0.8, 2).
Look for: flicker of torch / light shadows while walking; draws a frame, FPS and lows in a heavy outdoor view
(graveyard); small-object shadows are PS4 for the whole launch (not live).
