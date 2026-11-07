---
id: effects-resolution
title: Effects resolution: r.SeparateTranslucencyScreenPercentage 100 / 75 / 50, live, 75% default
area: ue4
status: active
platforms: all
default_windows: on
default_deck: on
origin: ours
class: M
commits: 32586bcb44, 9cc915e076
files: scripts/ue4_override.py, run.py, src/runtime_extra.c, gpu/shim/gow3_settings.cpp, gpu/shim/gow3_settings.h, gpu/shim/gow3_overlay.cpp, launcher/medievil_launcher.py
symbols: UE4EffectsKeys, effects_resolution, startup_effects_resolution, runtime_ue4_request_cvar, ue4_cvars, ue4_set_cvar
switches:
tests: N-19, N-20, N-21, N-26
docs: plan-frame-pacing.md#n-19-what-the-fights-slow-frames-are-per-pass-2026-10-29-launched-by-claude-with-the-users-go-t-prof1-fight-burstslog, plan-frame-pacing.md#n-20-the-fights-effects-at-half-resolution-2026-10-29-launched-by-claude-with-the-users-go-t-prof2-translucency-50log, plan-frame-pacing.md#n-21-why-the-death-effects-cost-2-3-ms-2026-10-29-launched-by-claude-with-the-users-go-t-diag1-death-effectslog-shader-dumps-in-usershaderdumps, plan-frame-pacing.md#n-26-effects-resolution-as-a-live-switch-2026-10-30-launched-by-claude-with-the-users-go-t-fx1-live-effectslog-gow3_gpu_profile1, study-live-switches.md#effects-resolution-already-live, fix-inventory.md#7-medievil-specific-ue4-config-eboot-patches-hooks
depends_on:
related: live-graphics-options, preshadows-live, deck-preset, gpu-profiler-fixes, upstream-ue4-setres
verified: 2026-11-07
---
## What it does

UE4's separate translucency (where the enemy death effects, smoke and fire draw) renders into its own target,
blended in by depth. The ini key `effects_resolution` (100 / 75 / 50; an ini key, not a `perf_*` / `GOW3_*`
switch) sets `r.SeparateTranslucencyScreenPercentage`: the override pak writes it at boot for 75 / 50
(`ue4_override.apply`, `effects` from run.py, default `'75'`), and the in-game menu's "Applied now" row requests it
live through `runtime_ue4_request_cvar` (the game's own `IConsoleVariable::Set`). `Load()` also requests a 75 / 50
value at start, applied once the game reaches its menu.

## Invariants

- The live path writes only to the 01.00 / 01.02 objects in `ue4_cvars[]` (0x5bd2d68 / 0x5bd2da8) after checking the
  7 registration bytes (`lea rsi, [rip+name]` at 0x16651ee / 0x1667e6e); another eboot logs "unavailable" and keeps
  the boot value.
- `ue4_set_cvar` keeps the variable's SetBy priority (`GetFlags() & 0xff000000`) and checks the vtable and both
  methods lie in the image; values longer than 15 characters are dropped (`ue4_cvar_values[16]`).
- Values are applied from the game thread's pad poll (`runtime_ue4_setres_poll`, from 600 pad reads on), never from
  the menu's window thread, which only queues (`ue4_cvar_pending`). UE4 re-sizes the target live (N-26).
- `UE4EffectsKeys` = {"100", "75", "50"}: 100% is neither written to the pak (`apply` writes only '75' / '50') nor
  requested at start (`Load()` only for index > 0); an unknown ini value (index -1) gets neither, so it runs at
  100%, and `effects_resolution_raw` writes it back unchanged. run.py's default when the key is missing: `'75'`.
- Separate translucency is one target for all such materials: the death effects cannot be lowered alone.

## Interactions

- live-graphics-options / preshadows-live: the same `ue4_cvars[]` queue and poll.
- deck-preset: `effects_resolution=50`.
- The effects write no depth and draw after the HZB is built from opaque depth, so they cannot disturb occlusion
  (N-21).
- gpu-profiler-fixes: `GOW3_GPU_PROFILE` slow-pass split found it (N-19).

## Evidence

- N-19 / N-20 (4K): death-effect windows +2.36 / +2.91 ms of GPU at 100%; at 50% the effect-pass growth <= 0.09 ms in
  every window, passes at 1920x1080 [V]. N-21: the shaders are light (6 samples, no loops) [V]; the cost is overdraw
  of large soft particles into an `R16G16B16A16Sfloat` 4K target (inferred, "[?, consistent]").
- N-26: switched to 50 live, the next profile window's passes ran at 1920x1080 [V]. The user: "Slightly less sharp at
  50%, but it's hard to notice on most things ... I imagine 75% would be very difficult to tell".

## History and pitfalls

- study-frame-cost-2026-10-29 section 4 listed separate translucency at 50% as dropped (N-16, on narrow counters);
  N-19 reopened it and N-20 / N-26 reversed it.
- `32586bc` was a restart setting with default 100 until judged; `9cc915e` made it live and 75% the default. 75% was
  not measured on its own (the user's judgement of 50% and a guess for 75%).
