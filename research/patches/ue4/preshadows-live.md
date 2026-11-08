---
id: preshadows-live
title: Preshadows (r.Shadow.Preshadows) as a live switch through the game's console variable
area: ue4
status: active
platforms: all
default_windows: off
default_deck: on
origin: ours
class: M
commits: fdd12e8269
files: src/runtime_extra.c, src/runtime.h, gpu/shim/gow3_settings.cpp, gpu/shim/gow3_settings.h, gpu/shim/gow3_overlay.cpp, launcher/medievil_launcher.py
symbols: runtime_ue4_request_cvar, ue4_cvars, ue4_set_cvar, Ue4CvarSite, runtime_ue4_setres_poll, preshadows, INI_OPTIONAL_KEYS
switches:
tests: N-18, N-25
docs: plan-frame-pacing.md#n-18-preshadows-off-2026-10-29-launched-by-claude-with-the-users-go-t-pre1-preshadows-offlog-research-datasmi-pre1csv, plan-frame-pacing.md#n-25-preshadows-as-a-live-switch-2026-10-30-launched-by-claude-with-the-users-go-t-park3-clean-preshadows-offlog-t-pre2-live-switchlog, plan-live-switches.md#mechanism-built, fix-inventory.md#7-medievil-specific-ue4-config-eboot-patches-hooks
depends_on:
related: live-graphics-options, effects-resolution, park-unmapped, deck-preset, upstream-ue4-setres, single-particle-pass, live-render-scale
checked: 2026-11-07
---
## What it does

The first live UE4 console variable besides r.SetRes: `runtime_ue4_request_cvar(name, value)` queues a value; the
game's input thread (`runtime_ue4_setres_poll`, called at every pad read) sets it through the variable's own
`IConsoleVariable::Set` (vtable +0x68). `ue4_cvars[]` holds per game build the TAutoConsoleVariable object
(`r.Shadow.Preshadows`: 01.00 0x5bcff28, 01.02 0x5bcff68) and its registration site, whose 7 bytes
(`lea rsi, [rip+name]`) are checked first. Ini key `preshadows` (default 1; written only when set,
`INI_OPTIONAL_KEYS`); menu Game > "Applied now" > Preshadows; an ini 0 is requested by `Load()` at start.
Default here means the patch (preshadows off): off on Windows, on with the Deck preset (`preshadows=0`).

## Invariants

- A site whose bytes or image size do not match is skipped with "unavailable"; the object pointer at +8 must be
  non-null and its vtable, `GetFlags` (+0x20) and `Set` (+0x68) inside the image, else nothing is written.
- `Set` is called with the variable's own SetBy (`GetFlags() & 0xff000000`): the live value does not outrank a
  higher-priority setter, and a later device-profile / ini apply could override it. Hence the wait: queued values
  are applied only from 600 pad reads on ("once the game reaches its menu", after the engine read its settings).
- Only the game thread calls `Set` (pad poll); the menu (window thread) only queues (`ue4_cvar_pending` atomics).
- New entries need both builds' object and site, found as r.SetRes's was (`cvar_find*.py`, study-live-switches);
  the object comes from the registration's own store, not from a lookup site that only caches the data pointer.

## Interactions

- live-graphics-options and effects-resolution reuse this table and poll; the single particle pass
  (single-particle-pass) and live-render-scale too. `ue4_set_cvar` itself is upstream's r.SetRes setter
  (upstream-ue4-setres), shared with the live resolution path.
- park-unmapped shipped in the same commit (`fdd12e8`); its t-park3 run is also the preshadows-off clean run.
- deck-preset: `preshadows=0`.

## Evidence

- N-18 (`GOW3_UE4_CVARS`): passes per draw roughly halve in heavy views [V]; the GPU-bound bursts are not preshadows.
- N-25: t-park3 (off, parking on) held the cap at the Graveyard horizon view (1% low 233-235, 0.1% 202-219 vs
  t-hzb3-clean 168-235 / 106-208). t-pre2: 34 live switches, all applied; ~130-150 fewer draws off (~12%, ~0.4 ms of
  stage B). The user: "The difference is small, and I only noticed it in one place, the shadows cast on dan's shoes
  by himself."

## History and pitfalls

- Found on the way (N-25): the in-game menu, while open, costs a frame ~60 times a second (open item,
  plan-work-queue).
- The t-park3 lows mix parking and preshadows off; the preshadows share is the t-pre2 draw count only.
