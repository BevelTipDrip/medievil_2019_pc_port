---
id: no-decal-prepass
title: r.DBuffer=0 + r.EarlyZPass=1: no forced full depth prepass for invisible DBuffer decals
area: ue4
status: active
platforms: all
default_windows: on
default_deck: on
origin: ours
class: M
commits: cd72da198a, 27ec51e36d, 1e657ca5ef
files: scripts/ue4_override.py, scripts/graphics_options.py, gpu/shim/gow3_settings.cpp
symbols: apply, PERF_SWITCHES
switches: perf_no_decal_prepass, GOW3_DECAL_PREPASS
tests: T-Z2-T-Z4b, T-Z5, T-Z1
docs: plan-stage-b-split.md#t-z2-t-z4b-dbuffer-decals-and-the-forced-prepass-2026-10-21-0200-0314-launched-by-claude-with-the-users-go, fix-inventory.md#7-medievil-specific-ue4-config-eboot-patches-hooks, study-live-switches.md#perf_no_decal_prepass-restart--easy-medium
depends_on:
supersedes: early-z-pass-off
related: hzb-remap, perf-patches-ini, ue4-cvars-tool, deck-preset
checked: 2026-11-07
---
## What it does

UE 4.21 forces a **full** depth prepass whenever DBuffer decals are on, whatever `r.EarlyZPass` says: in the heavy
view 628-739 draws (~28%), the G-buffer drawn twice, for 4 decal draws touching ~3,500 pixels of Dan's boots by
1-4/255 (invisible). `ue4_override.apply` writes `r.DBuffer=0` and `r.EarlyZPass=1` into the override pak: the
decals go, and the PS4's own partial prepass of large occluders (PS4Engine.ini's `r.EarlyZPass=1`) stays.
`perf_no_decal_prepass=0` -> `GOW3_DECAL_PREPASS=1`: the pak writes neither (the PS4's settings).

## Invariants

- Both values go together: `r.DBuffer=0` with `r.EarlyZPass=0` (T-Z2, under the then-default `fca0d85`) removes the
  prepass entirely and the GPU loses early depth; `r.EarlyZPass=1` alone keeps the forced full prepass. The pak's
  `r.EarlyZPass=1` is the value PS4Engine.ini already has, written explicitly.
- The switch is read by the Python pak writer at start (`os.environ.get('GOW3_DECAL_PREPASS') != '1'`); the C side
  only shows it in the in-game menu (`perf_off` table in gow3_settings.cpp). Restart only: `r.DBuffer` is ReadOnly.
- Assumption (class M): no DBuffer decal in this game is visible enough to matter. Checked in the heavy view only
  (RenderDoc decal shot); a level with visible decals (blood, scorch marks) would lose them. Booting with
  `r.DBuffer=0` uses the cooked base-pass shaders in both states (study-live-switches, [I]).
- `GOW3_UE4_CVARS` values win over it (applied after, same pak).

## Interactions

- perf-patches-ini: `27ec51e` put the pak write behind `GOW3_DECAL_PREPASS` (launcher toggle), `1e657ca` the ini key.
- supersedes early-z-pass-off (`r.EarlyZPass=0` alone, `fca0d85`): its gain came from DBuffer's forced prepass,
  which `r.EarlyZPass` cannot turn off; `cd72da1` replaced it.
- hzb-remap: HZB occlusion reads the depth the prepass / base pass leaves; the partial prepass keeps occluder depth
  early. Not measured separately.
- Deck preset: `perf_no_decal_prepass=1` (on).

## Evidence

- T-Z2 (`r.DBuffer=0`): ~1,650 draws (-600); the user, standing: the best yet (1% ~165, 0.1% ~145); panning spikes
  (worst 21-45 ms). T-Z3 (`r.DBuffer=0 r.EarlyZPass=1`): worst frames ~9-12 ms, more frequent; the user judged it
  worse that night.
- T-Z5 (+ `r.ScreenPercentage=50`): stage B never waited on the GPU yet panning still spiked: the spikes are stage B
  work, not GPU saturation. Default since 2026-10-21; the user: "we found a way to increase the 1% lows
  significantly in heavy areas". T-P5 (with the lock-free reads too): "quite good, the best it's been".

## History and pitfalls

- T-Z1's "~120 fewer draws" for `r.EarlyZPass=0` is not what the captures show: the forced prepass was 628-739 draws
  and stayed (study-heavy-frame-map.md).
- Live switching was studied (easy-medium: DBuffer targets allocated on the first frame with DBuffer=1) and not
  built: the user does not want the clear-win patches switched live.
