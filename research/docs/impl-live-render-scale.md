# Implementation: render scale live, in 1% steps

Status: built 2026-11-04 (`7ab4310`, `cd1b1a8`), Windows and Linux AppImage. **Not run in game yet**: nothing
here is verified (verified = the user confirmed it in game). Parent: [plan-live-switches.md](plan-live-switches.md)
item 4, [study-live-switches.md](study-live-switches.md) "Render scale". **[V]** checked in code or the binary,
**[I]** inferred (UE 4.21 source knowledge or reasoning).

## What the user gets

| Where | Before | Now |
|---|---|---|
| In-game menu, Performance > **Applied now** | "Render scale" under Applied on restart: 7 presets | **Render scale** slider, 1% steps, `-` / `+` buttons; **Scene now** row (`921 x 518 at 72% of 1280 x 720`); applied while playing |
| Launcher, Performance > Resolution and quality | 7 presets (other ini values shown as "Custom (N%)") | the presets **plus a percent box** (50-200, type or arrows) and the scene size it gives (`scene 921x518`); the Home page's list shows a box value as "Custom (N%)" |
| `medievil.ini render_scale` | 25-200, presets in the lists | unchanged format (whole percent); the menu writes the live value back on close, as every live setting |

Range: **50 to 200%, whole percents**, in both the menu and the launcher (`RenderScaleMin` / `RenderScaleMax` in
`gow3_settings.h` = `graphics_options.RENDER_SCALE_RANGE`; a launcher test checks they match). 50-200 is UE 4.21's
temporal upsampling range (resolution fraction 0.5-2) **[I]**. Ini values 25-49 keep starting as before (the slider
then starts at that value); anything unparsable is 100, as `parse_render_scale` always did. Default unchanged (100).

## How it works

1. The slider edits `Values::render_scale` (the chosen value). Each change reallocates every scene target in UE4, so
   the overlay applies it only when the slider is let go and no step came for 0.3 s, or when the menu closes
   (`ApplyRenderScale`, `gow3_overlay.cpp`), not at every step of a drag.
2. `Gow3Settings::RequestRenderScale(percent)` (`gow3_settings.cpp`) clamps to the live range (below), and if the
   value differs from what the game draws at (`live_render_scale`): requests `r.TemporalAA.Upsampling` 1 / 0 when the
   scale crosses 100% (a start below 100% sets it to 1 in the override pak, one at or above does not), then
   `r.ScreenPercentage`, and logs **`Render scale: 67% -> 72% (live)`**.
3. The requests go through mechanism A of the study (`runtime_ue4_request_cvar`, `src/runtime_extra.c`): on the game
   thread's next pad poll (once the game reached its main menu), `IConsoleVariable::Set` with the cvar's current
   SetBy; the runtime logs `UE4: r.ScreenPercentage 72 (live)`. Same table order as the request: upsampling first.
4. UE4 reads `r.ScreenPercentage` every frame (`FLegacyScreenPercentageDriver`) and, with
   `r.SceneRenderTargetResizeMethod=0` (registration default, not set by the game), reallocates the scene targets at the
   new size on the next frame **[I]**; the temporal AA history restarts (one soft frame) **[I]**.
5. On menu close `Save()` writes `render_scale=72` (only changed keys, in place). The next start boots at it through
   `run.py` / `ue4_override.apply` as before.

Console variables (eboot image offsets; the object's IConsoleVariable* is at +8; site = the registration's
`lea rsi, [name]`, 7 bytes checked before any write) **[V]**:

| cvar | 01.00 object / site / bytes | 01.02 object / site / bytes | flags |
|---|---|---|---|
| `r.TemporalAA.Upsampling` | 0x5c1a120 / 0x25662e6 / `48 8d 35 bd 63 8d 01` | 0x5c1a160 / 0x25690b6 / `48 8d 35 6d 66 8d 01` | int, 0 |
| `r.ScreenPercentage` | 0x581a048 / 0x732124 / `48 8d 35 27 56 53 03` | 0x581a068 / 0x734ce4 / `48 8d 35 c7 5f 53 03` | float, 0x40 (scalability) |

Found with the session's `cvars_0102.py` (name -> `lea rsi`) and checked by disassembly: each registration stores
the returned IConsoleVariable* with `mov [object+8], rax` right after the register call (01.00 `0x73213a` ->
0x581a050, `0x25662fb` -> 0x5c1a128; 01.02 `0x734cfa` -> 0x581a070, `0x25690cb` -> 0x5c1a168). The finder's own
"first `lea rbx` after the site" heuristic picks the next cvar for `r.TemporalAA.Upsampling` (0x5c1a138 / 0x5c1a178):
do not use it for this one.

## Limits

**Live range: from 50% (or a lower start value) up to the start value, or 100% if the start was lower.** A value above
it is saved, shown with a note ("applies at the next start ... live changes go up to N%"), counted as a restart change
(the menu's "Apply and restart" appears), and the scene stays at the top of the live range until then. Why:

- The game's memory is sized at start by the **output** size, not the scene: above 1080p (by area) the 512 MB
  framebuffer pool patch and `GOW3_DMEM_MB=7104` (`graphics_options.output_environment`); at or below 1080p the
  Patches tab's pool (on by default) and the PS4's direct memory. A scene at 100% of the output, or at the start's own
  scale, is a configuration a start sets up; a larger one at that output is not, and the study's per-size pool
  blocks double at 2x the area (study-boot-resolution.md, "Framebuffer pool per size") **[I]**.
- Upstream already saw the failure mode: 1800p output at 200% (6400x3600 targets) ran out of PS4 memory and crashed
  (`783b25a`; the "Game resolution now" buttons stay disabled after a start above 100% for that reason).
- During the switch UE4's render-target pool can still hold the old targets beside the new ones for a few frames, and
  the port parks freed guest targets for up to 10 s / 768 MiB (N-24) **[I]**: a live increase peaks above a start at
  the same scale. Going down or back up to the start value stays within what the start itself allocated.

**Anti-aliasing:** with `ue4_aa=fxaa` / `off` a start at or above 100% has no temporal AA, and one below 100% forces it
(`ue4_override.apply`). Live, the anti-aliasing stays as the game started: below 100% after an FXAA start, UE4 falls
back to its spatial upscale **[I]**. Crossing 100% that way counts as a restart change, with a note in the menu.

## What follows a live change, what stays as started

| Thing | Live change |
|---|---|
| Scene targets, TAA history, post-process sizes | follow (UE4 reallocates) **[I]** |
| Effects resolution (`r.SeparateTranslucencyScreenPercentage`) | follows: a percentage of the render size |
| HZB pyramid size | follows: UE4 builds it from the scene size every frame (with or without the patch) |
| "Finer occlusion culling" patch | **stays as decided at start** (binary patch; `hzb_full_size_environment` from the start scene: on up to 1280x800). Restart to re-decide. On the Deck (720p / 800p output) every live value up to 100% is still at most 1280x800, so the start's choice matches the one a start at the new value would make |
| HZB remap (`medievil_hzb.cpp`) | unaffected: its results texture is a fixed 256x256, remapped per primitive |
| Port texture cache | new images for the new sizes; old guest targets parked and evicted (N-24) |
| VideoOut, `GOW3_INTERNAL_WIDTH/HEIGHT`, boot display size, framebuffer pool, `GOW3_DMEM_MB` | **stay** (output size; restart-only) |
| FSR 1 / RCAS | unaffected: they act on the game's output image against the window |
| Developer debug menu HUD (`ue4_override.debug_hud(..., scale > 100)`) | **stays** as started |
| "Game resolution now" buttons | rule unchanged: disabled after a start above 100% (a live scale can only exceed 100% then) |

Not verified in this build **[I]**: that UE4 4.21 re-reads `r.TemporalAA.Upsampling` every frame (the scan shows flags
0: read without render-thread copy), and that nothing re-applies scalability afterwards. At a 100% start the pak does
not set `r.ScreenPercentage`, so its SetBy is the scalability level's (`sg.ResolutionQuality`); if the game ever
re-applies its scalability settings, the scene would snap back to 100% (the menu's "Scene now" would not show it: it
reports the requested value). The game has no PC graphics menu, so this is not expected.

## Tests and builds (2026-11-04)

- `py -3.14 -m unittest discover -s tests`: 129 run, the 8 known failures (2 failures, 6 errors), nothing new.
  New: `test_graphics_options.test_render_scale_any_percent`, `test_launcher_controls.test_render_scale_range_matches_the_game`.
- C++ `graphics-settings-test` (`ninja -C out/gpu graphics-settings-test`): PASS. It now stubs
  `runtime_ue4_request_cvar` (it no longer linked without it) and checks parsing, the live requests (upsampling only
  when crossing 100%), the clamp, the restart rule (above the range, FXAA across 100%) and the saved line.
- Launcher built off-screen (Tk, scratch ini): box -> ini value, both lists show "Custom (81%)", out-of-range typing
  ignored, the scene label follows the output.
- Windows `research/tools/build-clang64.sh`: OK, no warnings in the changed files (a `%` in an old menu text that
  clang flagged is fixed). WSL Debian (`/root/src/medievil` at `cd1b1a8`): `build.sh` OK (LTO, static, baseline
  CPU level), `packaging/linux/appimage.sh` OK: `dist/MediEvil-x86_64.AppImage` (131,242,488 bytes, sha256
  `c97864e2...878a6d`). Not deployed to the Deck.

## Deck test (one launch, with the user's go)

Settings: `deck.sh ini logging`, render scale **67** at the 1280x720 (or 800) output, `GOW3_FRAME_STATS=1`
(`deck.sh env`), the usual heavy graveyard view. Expect at start `Render resolution: 1280x720 output, render scale
67% (temporal upsampling)` and `Finer occlusion culling: on`.

1. Stand still in the heavy view ~30 s (6 windows) at 67%.
2. Menu > Performance > Applied now > Render scale: **80** (slider or `+`), close the menu. Log: `Render scale: 67% ->
   80% (live)`, `UE4: r.ScreenPercentage 80 (live)`. Same view, ~30 s.
3. **100** (log also `UE4: r.TemporalAA.Upsampling 0 (live)`), ~30 s.
4. Back to **67** (`r.TemporalAA.Upsampling 1`), ~30 s: same numbers as step 1 means the change is clean.
5. Try **150**: the menu should say it applies at the next start, the scene stays 100%; set it back to 67 before
   closing (the ini keeps what is set on close).

Read: `deck.sh pull NAME`, then `deck.sh summary NAME --windows` (or
`py -3.14 research/tools/deck_summary.py <log> --windows`): windows are labelled `0 0 67%`, `0 0 80%`, `0 0 100%`
and the window each change falls in is left out (`*`). Compare per scale: **FPS, draws/frame, HZB hidden**, pacing
p99 / worst, stage B busy. Reference (runs 4/5 and the culling run): 67% with the finer HZB ~119 hidden, ~1,151
draws, ~77.6 FPS; 100% without it ~122 hidden. With the patch on for both, hidden should move less between 67 and
100% than in runs 4/5. Also note: the hitch at each switch (worst frame of the `*` window), the picture after the
switch (soft for a frame, then sharp; no stretched or offset image, no wrong HUD), and GPU use in the overlay.
If the scene does not change size: the log's `UE4: ... unavailable` line (not 01.00 / 01.02) or a missing
`UE4: r.ScreenPercentage` line.
