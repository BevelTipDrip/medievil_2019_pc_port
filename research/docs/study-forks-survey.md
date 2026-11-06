# Study: every fork of the upstream port, and what `lab` / `deck-lab` should take from them

Status: study, 2026-11-05. Read only: nothing was built, launched, committed or pushed. No GitHub write of any kind
(no comment, star, issue or PR). Parent: [START-HERE.md](START-HERE.md). Related:
[study-fork-spacepepperoni.md](study-fork-spacepepperoni.md) (one fork in depth; not redone here),
[plan-linux-deck.md](plan-linux-deck.md), [plan-ue4-motion-vectors.md](plan-ue4-motion-vectors.md),
[plan-fsr-frame-generation.md](plan-fsr-frame-generation.md), [study-glyphs.md](study-glyphs.md),
[study-wwise-stereo-mix.md](study-wwise-stereo-mix.md).

How it was read:
- `gh api` (forks, forks of forks, branches, releases), `gh pr list/view`, `gh issue list/view` on upstream, all
  read-only.
- One scratch clone of upstream with every fork fetched as a remote, outside this repo:
  `C:\Users\Ty\AppData\Local\Temp\claude\c--Users-Ty-MediEvil\78f7734e-4522-4580-95d1-94046d666589\scratchpad\forks-survey\repo`.
  PR and issue texts are dumped beside it in `gh\`. Nothing of theirs was run or built.
- Ours: `lab` = `63b0c76`, `deck-lab` = `e57dd80`, read with `git show` / `git grep` / `git log --grep`.

Marks:
- **[code]**: read in the code (theirs at the commit named, ours on `lab` / `deck-lab`).
- **[msg]**: their commit message or PR text. Their measurements, on their hardware.
- **[log]**: a line from one of our logs in `data/`.
- **[inferred]**: reasoning, not checked.

Nothing here is verified in game in the project's sense.

Licence: upstream and every fork are GPL-2.0(-or-later). Code can be ported with the author named and the commit hash
in our commit message.

## Answer

Ranked by value to us. Tags: **PC** / **Deck** / **both**, and **new** (we lack it) / **have** / **partly**.

1. **Porticus's FSR on the UE4 upscaler (upstream PR #61, open) is our motion-vector frame map, done.** [PC + both] **new**
   - It hooks UE4's own TAAU compute pass and feeds its inputs to FSR 3.1.5 or FSR 4.1.1, and to FSR 3.1.6 frame
     generation with a HUD-less frame and paced presents. Commits `82cb58e`, `9620962`, `d3a3e45` [code].
   - It already has what [plan-ue4-motion-vectors.md](plan-ue4-motion-vectors.md) M0 ("map a UE4 frame") and
     [plan-fsr-frame-generation.md](plan-fsr-frame-generation.md) F0 ask for. Neither is in our docs or code. It gives:
     - the TAAU shader hashes;
     - the View uniform buffer's size and float offsets (`ClipToPrevClip`, `TemporalAAJitter`, view rect, projection);
     - the velocity decode;
     - the HUD-less capture point (section 2.1).
   - Tested only on Linux with an RX 9070 XT (RADV): 60 FPS at Quality 1281x721 to 1080p, frame generation reaching
     120 at a real 120 Hz [msg]. Windows: CI build only. NVIDIA: never run.
2. **AvPlayer thread joins (dvilelaf, upstream PR #68, open): the better of two fixes for one hang. Neither of our
   branches has it.** [both] **new**
   - The hang: after the first movie on Linux (black screen, sound goes on), and on the world map after a level
     (spacepepperoni's `0ddcabc`).
   - It also fits three upstream reports [inferred]: #50 (map softlock, Windows build under Proton on a Deck), #60
     (freeze on "restart level" / "quit to map") and #59 (black screen on the second start).
   - On Windows the same race is the "HLE thread: join failed" line in our PC logs [log]. There the fallback can wait
     up to 5 s (`threads.h` `Join()`).
   - PR #68 is more complete than spacepepperoni's version [code]. Section 4 compares them.
3. **Torch flames missing on a cold pipeline cache (Porticus, upstream PR #52, open).** [both] **new**
   - UE4 writes its particle curve texture once, at level load. With async compiles, 80 of the 84 injection draws
     were skipped, so the flames got a zero size [msg].
   - The fix: two programs (vs `0x113f0c20873a7659`, ps `0x1b1e7c76d6a281d3`) always compile synchronously. It is
     ~15 lines in `vk_pipeline_cache.cpp`.
   - Our `lab` has the same async path with no exception [code]. On the Deck every new build starts with a cold
     cache [inferred].
   - spacepepperoni took it (`c758207`), then reverted it (`7f69812`, 10-28) with no reason given. It was not
     re-applied with the six runtime commits reverted the same day (section 3).
4. **Credit fix for `deck-lab`: Porticus wrote D1, D2, D4 and D5, not spacepepperoni.** [Deck] **have**
   - `deck-lab`'s `02c15bf` (D1), `471913a` (D2), `b7e947d` (D4) and `0b32271` (D5) credit spacepepperoni hashes.
   - Those hashes are cherry-picks of Porticus's upstream PR #57 (branch `perf-frame-rate`). The `git patch-id`s are
     identical [code], and the author field of `029ee2a` and the five others is Porticus.
   - PR #57 has the original numbers: Linux RDNA3, 1440p Ultra, graveyard heavy 56-58 to 86-88 FPS, light 75-83 to
     118 [msg]. Two parts of #57 are not on `deck-lab`: the address-indexed region cache (`e06072e`) and the
     per-chunk first-free index (`f9d0bb8`). We have R1 and R3 for those.
5. **spacepepperoni's newest commit (after our study): the Deck preset must keep ambient occlusion and SSR.**
   [Deck] **mostly have**
   - `ef5d3cf`: with `low`'s `r.AmbientOcclusionLevels=0` / `r.SSR.Quality=0`, the Asylum rooms came out shiny
     and over-bright, and the lava room was "lit like daylight" [msg].
   - With UE4's defaults (`-1`, `3`) the rooms look right and the Deck held 30 FPS [msg].
   - Our "Deck Optimized" preset (`aec4e33`) boots `low`, but its live options set `ambient_occlusion=2`,
     `reflections=1` and `volumetric_fog=1` (`launcher/medievil_launcher.py:675`, `gow3_settings.h:243-251`) [code].
     So AO and SSR are on, at lower levels than theirs. Whether SSR at 1 is enough for the Asylum is untested.
   - Two of their Low-preset fixes still apply to our preset [inferred]:
     - view distance 0.7: props vanish under the top-down camera (`fbef777`);
     - sun shadows at 1024 / filter 1: shadows jump while the camera follows Dan (`b136400`).

     See section 8.
6. **Pads plugged in after start (Porticus, upstream PR #64, open).** [both] **new** for hotplug, **have** cursor hide
   - `SDL_HINT_JOYSTICK_THREAD` gives SDL its own thread for device messages on Windows, because the thread that
     started the gamepad subsystem may never pump messages.
   - In a container it also sets the udev-less hints.
   - Our pointer hiding is Bloodborne's (`window.cpp:218-231`), so we have that part.
7. **Trophies (Porticus, branch `trophies`, no PR).** [both] **new**, feature: an unlock notice, an in-game list on
   F8 / R3+R2, a launcher page, and `trophies.dat`. Port of gow3_pc's system. Its texts are "copied from trophy sites"
   [msg]: check where they come from before taking them.
8. **Smaller items**: each is either **have**, **partly**, or low value:
   - Porticus's `t.MaxFPS` lift (`78234ca`, PR #57). We **have** it as a pak edit (`3cf6d69`).
   - TheRhysWyrill's AppImage CI with a 5-distro smoke test. **Partly**.
   - nydal91's exact depth bias through `VK_EXT_depth_bias_control`. Unproven by its own author.
   - nydal91's shadow-flicker cvar tests for AMD. A diagnostic list for #40.
   - Porticus's Xbox glyphs (PR #31). The maintainer's review is a useful checklist for our runtime-switch plan.
   - dvilelaf's Mesa 25.0 note. Our Deck runs RADV 26.2, so it does not apply.

**Why the spacepepperoni fork fixes effects we don't see** (section 8 reviews all 8 merged PRs). There are three
causes, none of them the 30 FPS rate itself:
- **Upstream's `low` preset.** Their Deck mode runs it: AO, SSR and fog off, view distance 0.7, unfiltered 1024 sun
  shadows. The PC never uses `low`.
- **Missing renderer fixes.** Their base lacks `lab`'s V4 stencil-plane copy, which fixed our box shadows (their
  "dark square around Dan"), and our data-path fixes (D2, twins, page downloads) for stale light colours.
- **Their own experiments.** Frame-wait serving and mapped arenas broke lighting until later PRs undid them.

The 30 FPS target explains only pacing workarounds: the fixed game clock, movies stepped to 60, physics substeps.
The user's PC playthroughs (several 3-4 hour sessions through those levels, the items acquired) show none of the
fork's effect glitches on our build (the user, 2026-11-05); see the caveat in 8.2.

**Conflicts with our findings:**
- **Audio.** No fork other than spacepepperoni touches audio. spacepepperoni reports 8 channels
  (`58a7e18`), which [study-wwise-stereo-mix.md](study-wwise-stereo-mix.md) says would make Wwise mix 7.1 and cost
  Deck CPU. Keep reporting 2.
- **FSR 1.** PR #61 removes the "FSR 1" option in `d3a3e45`, saying temporal upscaling does better. Our FSR 1 + RCAS
  path is used, so do not take that part.
- **Depth layout.** Nothing new on spacepepperoni's depth-layout revert warning (study-fork-spacepepperoni.md
  "warning").

## 1. All forks

The API reports `forks_count = 8`; the fork list returns 7 (ours included). The eighth may be private or deleted
(**unverified**). One fork of a fork exists (RottingSoul, from Porticus). A repository search for "medievil" found
no other related fork. Upstream's latest is 1.3.8 = `424f3ea` (2026-10-30).

| Fork | Default branch (tip) | Based on | Unique commits (not in upstream main) | Last push | What |
|---|---|---|---|---|---|
| [spacepepperoni/medievil_ps4_Linux](https://github.com/spacepepperoni/medievil_ps4_Linux) | `main` `51b75d4` | 1.3.8, 0 behind | 122 (11 branches, all in `main`) | 2026-11-05 | Linux / Steam Deck fork; studied in [study-fork-spacepepperoni.md](study-fork-spacepepperoni.md) at `4ae7198`. **New since: `ef5d3cf`** (+ merge `51b75d4`) |
| [Porticus/medievil_2019_pc_port](https://github.com/Porticus/medievil_2019_pc_port) | `main` `d4c9cf4` (= upstream 1.3.5, 0 unique) | 1.3.5-1.3.8 per branch | 16 across 8 branches | 2026-10-29 | The most active contributor upstream: PRs #26, #28, #29 merged; #31, #52, #57, #61, #64 open; `trophies` unsent |
| [dvilelaf/medievil_2019_pc_port](https://github.com/dvilelaf/medievil_2019_pc_port) | `main` `424f3ea` (0 unique) | 1.3.8 | 3 (one per PR branch) | 2026-11-01 | PRs #68, #69, #70 (open) |
| [TheRhysWyrill/medievil_2019_pc_port-linux](https://github.com/TheRhysWyrill/medievil_2019_pc_port-linux) | `main` `fc3d30f` | 1.3.8, 0 behind | 5 (3 versions of one change + 2 merges) | 2026-11-01 | Linux AppImage CI and smoke test; no PR |
| [nydal91/medievil_2019_pc_port](https://github.com/nydal91/medievil_2019_pc_port) | `main` `d0e1a2a` (0 unique) | 1.3.3 | 3 on `depth-bias-shadow-tests` | 2026-10-23 | Author of upstream #13 (Linux build, merged); pre-release "linux-0.1" (2026-10-18) |
| [prima1403/medievil_2019_pc_port](https://github.com/prima1403/medievil_2019_pc_port) | `main` `d4c9cf4` | 1.3.5 | 0 | 2026-10-19 | Untouched copy |
| [RottingSoul/medievil_2019_pc_port](https://github.com/RottingSoul/medievil_2019_pc_port) (fork of Porticus) | `main` `d4c9cf4` | 1.3.5 | 0 (4 branches, all merged upstream) | 2026-10-20 | Untouched copy |
| BevelTipDrip/medievil_2019_pc_port | — | — | — | — | Ours, excluded |

Upstream PRs from forks: #13 (nydal91, merged), #26, #28, #29 (Porticus, merged), #31, #52, #57 (#56 closed as
its duplicate), #61, #64 (Porticus, open), #68, #69, #70 (dvilelaf, open). The open ones have no maintainer comments,
except #31.

## 2. Porticus

Author `Porticus <flagguso@gmail.com>`. Every commit message ends with a Claude Co-Authored-By line, and the PR texts
are careful about what was and was not tested. Porticus tests on Linux: Bazzite and distrobox, RADV, an RX 9070 XT
and an RDNA3 desktop. On Windows they only check that it compiles.

### 2.1 `fsr-upscalers` (PR #61): FSR 3.1 / FSR 4.1.1 / FSR 3.1.6 frame generation on UE4's TAAU

Commits:
- [`82cb58e`](https://github.com/Porticus/medievil_2019_pc_port/commit/82cb58e125ab253e1101644fd9a1880d8c7243eb):
  30 files, +15,346 / -151. 12,538 of those lines are one fsr-vulkan patch with regenerated SPIR-V.
- [`9620962`](https://github.com/Porticus/medievil_2019_pc_port/commit/96209627a2aa5ae4452a21b7754195d2ea7cd2d6):
  Windows CRLF fix for the patches.
- [`d3a3e45`](https://github.com/Porticus/medievil_2019_pc_port/commit/d3a3e4520d05f34980a943563ae7e76d96979e3a):
  launcher texts; removes FSR 1 and the old Graphics page.

Based on 1.3.7 (`bb28370`).

**What it found about the UE4 frame** [code: `vk_neural_render.h/.cpp`, `ue4_motion.comp`]:

| Item | Value |
|---|---|
| TAAU compute shader (`r.TemporalAA.Upsampling=1`) | `0x9c89e98d32a43b49`; above ~71% screen percentage UE4 picks another permutation, `0xb373f39908ae979f` |
| View uniform buffer | 3,296 bytes, bound to the TAAU dispatch; "UE 4.21 FViewUniformShaderParameters" |
| Float offsets in it | `ViewToClip` 96 (row-major; `[1][1]` = 1/tan(fovY/2), `[3][2]` = near, reversed infinite Z); `ClipToPrevClip` 456; `TemporalAAJitter` 472 (current x, y then previous x, y, clip space, y up; pixels = v × size / 2); `ViewRectMin` 484; `ViewSizeAndInvSize` 488 |
| Inputs found among the TAAU's sampled images | velocity = the `R16G16Unorm` one; depth = the depth image or its `R32Sfloat` copy; scene colour = the first `R16G16B16A16Sfloat` |
| Velocity decode | encoded.x > 0: `(enc - 32767/65535) / (0.499 × 0.5)` = clip-space current − previous. 0 = static: reproject from depth with `ClipToPrevClip` (UE4's own TAA does the same) |
| Motion for FSR | `(-0.5 vx, 0.5 vy)` in view uv, × view size for pixels; depth `1 - z` for FSR 4 |
| HUD-less frame | the first full-screen draw (3 indices, 1 instance) into a display buffer after the TAAU is UE4's tonemapper. It is copied right after that draw is recorded (`vk_rasterizer.cpp` `DrawRecord`) |
| HUD over generated frames | `fg_hud_compose.comp`: where the game frame differs from its HUD-less copy by ≥ 2/255, it is UI and is copied over the generated frame |

**How it runs:**
- FSR runs right after the TAAU is recorded and **overwrites the TAAU's output** (`AfterDispatch`). The TAAU itself
  still runs, so its cost stays.
  - Our plan's design step 2 ("skip it and write the upscaler output into TAA's output") would save that cost.
  - Theirs is simpler and keeps every later pass unchanged.
- Frame generation is presented first, then the game frame, half a game-frame interval later.
  - It pauses when game frames come faster than 1.9× the refresh interval (hysteresis to 2.2×).
  - The refresh interval is measured from present waits, because GNOME paced a 120 Hz window at 60.
- Switches: `ue4_upscaler=taau|fsr3|fsr4` and `frame_gen` in medievil.ini, both live in the in-game menu;
  `GOW3_UE4_FSR3`, `GOW3_UE4_FSR4`, `GOW3_FRAMEGEN`, `GOW3_FG_STATS`, `GOW3_NR_DEBUG` (motion vectors drawn into the
  colour).
- fsr-vulkan patches 0003 and 0004:
  - 0003: retire bridge frames, reuse mapped constant buffers;
  - 0004: interpolation updated to SDK 2.4.0 sources with a separate colour rect, and regenerated SPIR-V.
- `vk_neural_render` also carries a "DLSSNR-AMD" hookup that is never compiled (`GOW3_HAVE_DLSSNR` undefined).

**Evidence**: 60 FPS with FSR 3.1 and FSR 4.1.1 at Quality on an RX 9070 XT; frame generation "works", pauses and
resumes against a 60 cap, and reaches 120 at a real 120 Hz [msg]. No image comparisons, no NVIDIA run, no Windows run.

**Ours**:
- `lab` and `deck-lab` have no TAAU hook, `vk_frame_gen`, `ue4_motion` or View-buffer offsets [code].
- Our motion-vector plan lists exactly these as unknowns ("Frame map: fill in at M0").
- Both branches have `fsr411/` and `vk_fsr4.cpp` from upstream, plus fsr-vulkan patches 0001 and 0002 only [code].

**Fit**:
- On the PC, M0-M3 and F0-F2 become "port and check" instead of "research". DLSS SR/FG ([plan-dlss.md](plan-dlss.md))
  can take the same prepared inputs.
- On the Deck, FSR 3 in place of TAAU at our 80% scale costs GPU time on a power-capped APU [inferred]. Not a Deck
  priority.
- Our own pacing (Reflex, tick = cap, the vblank thread) differs from upstream 1.3.7's present path. The present-side
  half needs the most rework; the TAAU hook and `ue4_motion.comp` port nearly as-is [inferred].

### 2.2 Other Porticus branches

| Branch / PR | Commits | What | Evidence | Ours | Tag |
|---|---|---|---|---|---|
| `fix-particle-curve-injection` / #52 | [`40d105e`](https://github.com/Porticus/medievil_2019_pc_port/commit/40d105e8cf517c0d6c6e5e54aa0f632da85b6f11) | `MustCompileSync()`: the curve-injection vs/ps compile on the spot; everything else stays async | Linux RADV with the pipeline cache off; before/after screenshots of the crypt torch [msg]. Not tested on Windows | **New**: `lab` `vk_pipeline_cache.cpp:1216` has no exception [code] | both |
| `perf-frame-rate` / #57 | `78234ca`, `92b4d51`, `e06072e`, `f9d0bb8`, `25e312c`, `a61aab3`, `ef597f0` | (1) raise MediEvil's own `t.MaxFPS 60` to the engine rate at run time (finds the cvar from `GetMaxTickRate`'s code, like `r.SetRes`); (2) initial-exec TLS; (3) address-indexed region cache; (4) per-chunk first-free index; (5) `gettid` cache; (6) pool maps outside the table lock; (7) lock spin + own locks for the hook queue and pool | Linux RDNA3 1440p Ultra graveyard heavy 56-58 to 86-88, light 75-83 to 118; noise 3-6 FPS; each lock change A/B'd over ≥ 3 runs [msg]. Windows compile-checked only; the hook and pool spinlocks also run on Windows | (1) **have** differently: `3cf6d69` edits the Blueprint's `t.MaxFPS 30/60` strings to `00` (01.02). (2), (5), (6), (7): **have on `deck-lab`** via spacepepperoni's cherry-picks (D1, D2, D4, D5); credit Porticus. (3), (4): **have equivalents** (R1, R3) | Deck (2, 5-7), both (1) |
| `pad-hotplug-cursor` / #64 | [`ffa6a72`](https://github.com/Porticus/medievil_2019_pc_port/commit/ffa6a72d693515d84cabae0f74b0f471626778bb) | `SDL_HINT_JOYSTICK_THREAD`, set in `window.cpp` and in `runtime_pad.c`, whichever starts SDL first. In a container: `SDL_JOYSTICK_DISABLE_UDEV`, `SDL_HIDAPI_UDEV=0`. Logs "SDL gamepad added". Hides the pointer on pad input | Bazzite + distrobox, 8BitDo Ultimate: three reconnect cycles [msg]. Windows: CI only | Hotplug **new**. Pointer hiding **have** (Bloodborne's, `window.cpp:218-231`) | both |
| `trophies` (no PR) | [`f1b1f54`](https://github.com/Porticus/medievil_2019_pc_port/commit/f1b1f54209944f5e4b45d2b2ae88e4a8d14b1525) | `sceNpTrophy*` answered (`runtime_services.c` +235); `trophies.dat` (4 words of bits); overlay notice card + list (F8, R3+R2); launcher page with reset; icons from the player's `sce_sys/trophy/trophy00.trp`; texts in 9 languages "copied from trophy sites"; CJK system font; unit tests | Unit tests only (`tests/test_services.c`, `tests/test_trophies.py`) | **New**: we only have the import names (`src/import_names.inc:626-631`) | both |
| `xbox-glyphs` / #31 | `808e89b`, `f207c6b` | Reads back the 1024x1024 `InventoryButtonAtlas` when bound, matches it by XXH3, pastes a shipped 1024x64 Xbox strip over the top rows, rebuilds the mips | Options menu on 01.00 and 01.02 [msg] | Not wanted as is. The maintainer's review (2026-10-23) listed four problems: (1) the strip looks cut from the game's own atlas (game data in the repo); (2) a `scheduler.Finish()` from the bind-helper thread; (3) a blocking readback of every 1024² image twice a second; (4) a lost edit when the cache recreates the image. Our [study-glyphs.md](study-glyphs.md) plan (CC0 art, pak-level) avoids all four. Its G6 runtime switch should keep (2)-(4) in mind | PC |
| `0102-compat`, `frame-analyzer-flatbuf-fix`, `native-output-4k-fixes` | — | Merged upstream (#26, #28, #29) | — | **have** | — |
| `pr-curve-images`, `pr31-images` | orphan branches | PR screenshots only | — | — | — |

## 3. spacepepperoni: what is new since our study

The study read `4ae7198` (2026-11-03). `main` is now `51b75d4`. The only new content is
[`ef5d3cf`](https://github.com/spacepepperoni/medievil_ps4_Linux/commit/ef5d3cf26eda46b3185163d7b643f52c983ccae2)
(their PR #9, 2026-11-05):
- What it changes: the Deck preset keeps `r.AmbientOcclusionLevels=-1` and `r.SSR.Quality=3` (Answer item 5).
- Their own issue #7, still open: after the Mausoleum fix (`4c63bf4`, already in the study), a user still sees purple
  reflections in the Hall of Heroes. The author suspects dust or fog [msg]. This is a place to check on our Deck runs.

Corrections to the study's credits [code]:
- **Commit authors**: 104 of the 122 commits are by "ngin <ngin@ngin.lan>", 11 by spacepepperoni, and **7 by Porticus**.
- **Porticus's seven** are `c758207` (curve injection) and the six runtime commits `029ee2a`, `ad7123c`, `def86f9`,
  `954df48`, `bb9841f`, `45390a5`. Those six are byte-identical to PR #57.
- **The six "reverted together, reapplied together" commits** (study 1.4) are Porticus's PR #57. The seventh revert
  of that day, `7f69812` (curve injection), was never re-applied.
- **Effect on `deck-lab`**: its commit messages for D1/D2/D4/D5 should also name Porticus and upstream PR #57.

No other spacepepperoni branch holds commits outside `main`.

## 4. dvilelaf (PRs #68, #69, #70)

| Branch / PR | Commit | What | Ours | Tag |
|---|---|---|---|---|
| `fix-avplayer-stop-hang` / #68 | [`42b2afb`](https://github.com/dvilelaf/medievil_2019_pc_port/commit/42b2afbe694b8013e4b91dfb4e9c4b5973b8171d) | Rewrites `Kernel::Thread` (`gpu/shim/core/libraries/kernel/threads.h`); details below | **New** on both branches: `threads.h` unchanged since upstream (`git log deck-lab -- threads.h`: last `a7cf186`) [code] | both |
| `linux-mesa-radv-note` / #69 | `57e9b99` | README: Mesa 25.0.7 RADV presents only black (all-zero display buffer); 25.3 works | Already noted in plan-linux-deck.md 1.4. The user's Deck is SteamOS 3.9.5 / RADV 26.2 (`a82287f`): does not apply | Deck (info) |
| `linux-readme-accuracy` / #70 | `1f0d38b` | README: GCC 14 fails on `actor_hook.h:26`, Vulkan headers 1.4.363 needed | Already in plan-linux-deck.md 1.1 | — |

PR #68 in detail [code]:
- A thread joining itself returns at once and its owner joins it later; it never detaches.
- One joiner at a time (`join_mutex`), with the self check made on an atomic id before the lock.
- `Stop()` requests the stop through a saved `stop_source` without waiting for another joiner.
- `Joinable()` means "started and not finished".
- The destructor detaches only a thread destroying its own object.

Evidence: tested on Linux, CUSA12982 01.00: seven starts in a row on llvmpipe, and several on RADV (RX 6800 XT, a
Ryzen 7000 iGPU), every one past the intro and into the crypt; later movies stop fine [msg].

**Compared with spacepepperoni's `0ddcabc`** [code]:
- `0ddcabc` keeps the self-detach (behind `try_lock`).
- `0ddcabc` reads `thread.joinable()` and `get_id()` outside the lock while another thread may be joining the same
  `std::jthread`, which is a data race.
- `0ddcabc` adds no stop-without-join.
- PR #68 removes all three.
- **Port #68**, not `0ddcabc`. Keep spacepepperoni's `0deb475` (notify queues before join) and `e88bf70` (never
  0 channels) as separate commits; they touch other code.

Our native Deck runs played the logo movie and went on into gameplay (`data/deck-native-2.log:389` onwards) [log].
The hang depends on timing, so those runs do not show the bug is absent [inferred].

## 5. TheRhysWyrill: Linux AppImage CI

[`507265e`](https://github.com/TheRhysWyrill/medievil_2019_pc_port-linux/commit/507265e7c347d7f9be4a6360a3f70e2ceea9c054)
(final version; `b6167d3` and `a693ef4` are earlier versions, plus 2 merges), 2026-11-01, no PR [code]:
- **Workflow**: `.github/workflows/linux-appimage.yml` builds on Ubuntu 24.04 (glibc 2.39) with clang + lld (LTO
  stays on), then opens the launcher from the AppImage under Xvfb in five containers: ubuntu:24.04, debian:13,
  fedora:42, archlinux, opensuse/tumbleweed (`smoke-test.sh`). A `linux-v*` tag publishes it.
- **`appimage.sh` `host_lib`**: also leaves `libasound`, `libpulse*`, `libpipewire-*`, `libudev` and `libwayland-*`
  to the host.
- **Tk text stack**: bundles fontconfig, freetype, libexpat and libXft for the launcher's Tk (`collect_launcher_libs`),
  so `import tkinter` works on non-desktop systems.

Ours:
- **Partly.** `deck-lab`'s `appimage.sh:172-184` leaves `libwayland-*` and `libgbm` to the host (spacepepperoni
  `22d6f92`), but not the sound and udev libraries. It also has no launcher library collection and no smoke test.
- The glibc 2.39 base matters less now that the Deck runs SteamOS 3.9.5.

Value: low to medium, and only if we publish Linux builds. Tag: Deck.

## 6. nydal91: depth bias and shadow-flicker tests

Branch `depth-bias-shadow-tests`. The commit messages carry a Claude Co-Authored-By line.
- [`2f0c18b`](https://github.com/nydal91/medievil_2019_pc_port/commit/2f0c18b1f5587141d854c95c99c56e61a68cc88a):
  `VK_EXT_depth_bias_control` with float representation.
  - What it does: the game's fixed-point bias (Z16 units) goes in as an absolute depth, N × 2^-bits, instead of being
    rescaled to the D32S8 host format. `GOW3_DEPTH_BIAS_CONTROL=0` is the A/B switch. It also logs each depth / bias
    format pair (lock-free after 16).
  - Evidence: the message says **"Unproven"**. On an RX 6950 XT the level tested made no depth-bias draws, and the
    flicker stayed with `r.ShadowQuality=0` [msg].
  - Ours: **new**. Upstream's rescale is in `UpdateDepthStencilState`.
  - Value: low until a flicker is traced to a biased draw.
- `5a8b555`, `29bc18e`: a launcher "shadow test" with one UE4 change per start.
  - The tests: `r.ShadowQuality=0`, no cached whole-scene shadows, no point-light cubemap shadows, one CSM cascade,
    more bias, no contact shadows, no distance-field shadows / AO, `r.AllowDepthBoundsTest=0`, no SSAO, no SSR.
  - Why: a dark wedge on AMD/RADV that flickers even with shadows off [msg].
  - Ours: we have `diag_ao` / `diag_ssr` (`scripts/ue4_override.py:182-183`). The others are a ready list for
    upstream #40 (Deck / Ally X black boxes) on our Deck.
  - Tag: Deck.

## 7. Upstream issues that touch fork work

| Issue | Link to fork work | Note |
|---|---|---|
| #30 Linux 2.5x slower (RADV) | Per-pass GPU profile Windows vs Linux; already in plan-linux-deck.md 1.6 | Nothing new |
| #40 Deck / Ally X flicker with black boxes | spacepepperoni's stencil clear (study), nydal91's tests (section 6) | Ally X under Windows too, so AMD, not Linux |
| #46 locked at 60 FPS (01.02, EU) | Porticus `78234ca` | We **have** `3cf6d69` |
| #50 map softlock on the Deck (Proton) | Fits the AvPlayer join race (section 4) [inferred] | Port #68 |
| #59 black at the second start (Intel Arc), #60 freeze on restart level / quit to map | PR #68 names both as possible matches [msg] | — |
| #74 crash in Scarecrow Fields at the second Iron Giant, 1.3.8, `Fault: read access to 0x44 at guest offset 0xad5c20` (2026-11-03) | none; not in our docs | Worth one check on `lab` when the user plays level 3 |

## 8. spacepepperoni's merged PRs, reviewed one by one

The fork has 8 merged PRs (#1-#6, #8, #9; #7 is an issue). It sent no PR to upstream. Each PR was read with its
commits (`gh pr view`, `git show`), and its symptom was checked against our docs and code.

Their base is upstream 1.3.8 (`424f3ea`), the same release `lab` merged (`e308c4c`). So their base is **not older
than ours in version**. It lacks ~370 `lab` commits, including several renderer fixes for the same symptoms (8.2).

Their test machines:
- "pyro": a Steam Deck LCD APU [msg];
- "medic": not identified;
- the user's Steam Deck OLED.

Verdict keys:
- **sound**: correct fix, cause addressed;
- **workaround**: hides a cause;
- **harmful on ours**: would make our build worse;
- **n/a**: not applicable to us.

Column (c) asks whether the change is tied to their 30 FPS target.

### 8.1 Per PR

**PR #1 `linux-steamdeck` (2026-10-27, 4 commits: `cc2e434`, `9d7fe24`, `1803166`, `2eddec6`)**
- (a) **Changes**: AppImage CI (Debian 13), a `sync-upstream` workflow, a Linux launcher (self-update, Add to Steam,
  Steam Deck mode: fullscreen, 720p native, Low, 60/30 FPS, locked settings), `install.sh`.
- (b) **Fixes**: no visual effect.
- (c) **30 FPS**: Deck mode offers 30 FPS; the default became 30 in PR #2.
- (d) **Our approach**: our "Deck Optimized" preset with Deck detection (`aec4e33`) targets a flat 60. We build
  through `deck.sh` / the AppImage, with no self-updater.
- (e) **Verdict**:
  - packaging: n/a (only if we publish Linux builds);
  - Add to Steam and the Deck-mode mechanism: sound (study 1.1);
  - updater and sync workflow: n/a.

**PR #2 `steam-deck-wayland` (2026-10-30, 87 commits).** Everything from 1.3.7-linux.5 to 1.3.8. The study's tables
1.1-1.5 rate each commit. Here it is grouped by the effect each commit claims to fix:

| Theme (commits) | (a) what / why | (b) effect; broken in ours? | (c) 30 FPS? | (d) ours | (e) verdict |
|---|---|---|---|---|---|
| Libwayland on the host (`22d6f92`) | No Vulkan driver loaded on SteamOS | Startup failure; we hit it too | no | **have** (`8072f54`) | sound |
| Deck mode 30 FPS, 720p scene in 1080p output (`c6cc0b0`, `2e0c6c2`) | Deck "does not hold 60" (upstream #16, #21). At 720p output, upstream's live `r.SetRes` showed the 1080p loading screens cropped | Cropped loading screens: an upstream live-`r.SetRes` artefact. We boot at the output size (eboot boot-rect patch), a different path; no cropping is recorded in our docs, and it was not looked for [inferred] | **yes** | 60 cap at 80% of 1280x720 output | Their target, n/a. The crop is a workaround for upstream's path |
| Steady game clock (`cfc9b39`) | `bUseFixedFrameRate` at the Deck rate: "a slow frame moved everything by the whole delay at once" (the title pan jumped, a spider popped in) | Symptom of frame-time spikes. Ours removes spikes (stage B, pacing, tick = cap) | **yes**: hides slow frames by slowing the game | none | **workaround**; harmful on ours (game speed drops under load) |
| 60 Hz physics substeps (`4519594`, off in `50ee1fa`) | The wisp zigzagged under the fixed 30 FPS clock | Self-caused: only with their fixed clock | **yes** | — | n/a (they turned it off) |
| Movies stepped to 60 under a 30 cap, early frames, catch-up, audio lead (`6503421` … `ba89450`, `934abc4`, `a7be15f`, `3f9c739`) | 30 FPS movies beat against the 30 cap; sound ~1 s behind | Stutter at a cap at or below the movie rate; sound lag reported on the Deck | **yes** (step); the sound lead is not | our decode pacing (`GOW3_DECODE_PACING`, impl-decode-pacing.md); our Deck runs at 60 | Step: sound for caps < 60, n/a at our 60. Audio lead: unverified on ours (study item 11) |
| Audio 8 channels + surround fold (`58a7e18`, `03209a3`, `401762b`) | "Doubled, hollow" effects on headphones | Not reported on ours; [study-wwise-stereo-mix.md](study-wwise-stereo-mix.md): with 2 reported, Wwise mixes stereo itself | no | report 2 | **harmful on ours** (makes Wwise mix 7.1: CPU on the Deck) |
| Audio NaN guard (`371f153`), never 0 channels (`e88bf70`), wake movie threads before join (`0deb475`), join lock (`0ddcabc`) | Silence after setting an enemy on fire; SIGFPE after a level; hang on the world map | Not seen on ours, but the code paths are the same [code] | no | none (PR #68 is the better join fix, section 4) | sound (`0ddcabc` superseded by #68) |
| GPU-thread waits (`6ef7566`, `d82d86c`, `a08d0b0`, `094b727`, `81442ce`) | Deck CPU time in drains and host-copy waits | Performance only | no | **have** E1, W2, drain sleep, label waits (`ea8f4c1`, `c4a0147`); compute queues not yet | sound |
| Mapped arenas + deferred merges (`c7ebc03`, `7401069`, `2488a68`, `81c7289`, `f9a74de`, `05682ea`, `2ff4265`, `433cd1a`) | Download waits (18-19 FPS in the Mausoleum). `433cd1a`: "the old copy-back wrote a 512 KiB window of older GPU data over bytes the CPU had rewritten (wrong light colours in the Hilltop Mausoleum)" | Two causes. **Upstream's base data path** (the 512 KiB copy-back), which `lab` replaced with page-only downloads (N-13), precise writes (D2) and twins (keep the CPU's bytes). And **their own experiments** (the default flipped four times in a day; "stale GPU data: white, pink and washed-out lighting"; a hang; an illegal shader instruction). The Mausoleum is not recorded as tested on ours | no | D2, D3, twins, N-13 | Final state: plausible, but high-risk. **Harmful on ours** as a replacement for D2/D3/twins (study item 14) |
| File reads invalidate their range (`5892ff9`, reverted `cc9e5b4`) | "Random colours per run" | Self-diagnosed stale level data; reverted with no reason | no | — | n/a |
| Volumes as 2D arrays with `image2DViewOf3D` (`717338d`) | ~37 MB of copies a frame on SteamOS RADV (no `sampler2DViewOf3D`) | Performance, RADV-specific | no | **have** V3 for render targets | sound; partly have |
| Bigger ring and stream buffer (`d0884ba`) | Pumpkin Gorge filled 64 MiB: ~72% of the GPU thread in an ioctl | Performance | no | ring 96 MiB + ring reuse | unclear (study item 6) |
| Low-preset values (`f6a90fe` fog on with a coarse grid; `4519594` / `56496d5` local-light threshold 0.1, then back to 0.15; `433cd1a` tiled deferred off) | Fog off: floors glowed pink, the Mausoleum washed out. Threshold 0.1: "a level turned bright pink". Tiled deferred: 21-23 to 30 FPS | Fog: an effect of **upstream's `low`** (`r.VolumetricFog=0`), never used on the PC; our Deck preset turns fog on [code]. Threshold 0.1: **self-caused**, reverted | indirectly: Low was chosen to hold 30 | our Deck preset: fog on, threshold 0.15 (upstream's) | fog: sound for Low; threshold: n/a |
| Lean barriers (a) and (b) (`64454ab`, reverted (b) `ec81d34`); depth stays writable (`8d6bf40`, reverted `ba0d35f`) | Pass breaks; (b) "hung the render thread on pyro" | — | no | **have** (a) and (b), on | (a) sound. (b) is our open Deck risk: `GOW3_DEPTH_SAMPLE_IN_PLACE=0` is the test |
| Porticus's runtime six + curve injection (`029ee2a` … `45390a5`, `c758207`; reverted, then reapplied except `c758207`) | See sections 2.2 and 3 | — | no | D1/D2/D4/D5 on `deck-lab` | sound |
| Launcher hides unsafe options (`55086ba`, `335ffcb`) | Settings that broke things for players are hidden and reset before every start | — | no | we keep switches for A/B | n/a for us |

**PR #3 `stencil-draw-clear` (2026-10-30, `59387dc`, `aea2165`, `e87679c`).** Reverted on their main, then re-applied
in PR #4.
- **Stencil clear by draw:**
  - (a) A GNM clear with `STENCIL_CLEAR_ENABLE` is a draw. It now writes the clear value only into the pixels it
    covers (stencil always / replace, mask 0xff), and the depth/stencil buffer is attached even when the game's
    stencil test is off. The port had made it a render-pass load clear of the whole buffer, and dropped clears
    without an attachment.
  - (b) Shadows cut along straight edges and flickering black shapes in Pumpkin Gorge (upstream #40). The square
    edges came from their own first version (`2cfb243`, the "b46" scissor clear), which `59387dc` replaced.
  - Ours: the same whole-buffer clear as upstream (`vk_rasterizer.cpp:3228`, `:3332-3334`) [code].
  - Our "box shadows" (V1) had a different cause, which we fixed: UE4 clears the stencil with a **compute fill of
    GCN's separate stencil plane**, which never reached the image. V4 (`769de12`, `423efc7`) copies the plane
    (patches-and-pr-proposal.md V4, RenderDoc-checked).
  - **The fork has no V4** [code: no `CopyBufferToStencil` in their tree]. It has only the Uncharted "stencil proxy"
    check.
  - Pumpkin Gorge is not recorded as tested on ours.
  - (c) Not tied to 30 FPS.
  - (d) We fixed where the stencil data comes from (V4); they fixed how clear draws are applied. Both can be true.
  - (e) **Sound in principle**: it matches the GNM semantics as they describe them; we did not check the register
    meaning against AMD docs here. But it is unproven as the cause of the boxes, because their build lacks V4. Port
    it only behind its switch and A/B it on top of V4.
- **Frame-wait serving (`aea2165`):**
  - (a) While the GPU thread waits for the GPU, it serves guest write faults and readbacks from a separate queue
    taken first.
  - (b) Game threads stuck 20-49 ms a frame in Pumpkin Gorge on the Deck (19-28 FPS) [msg].
  - (c) Indirectly: a GPU-bound Deck.
  - (d) We cut the faults and waits instead: D2, twins, W2, early frame EOP.
  - (e) **Harmful**: it reorders data against queued commands. It caused the pink Mausoleum (fixed by their own
    PR #8). Now opt-in.

**PR #4 `release-halloween` (2026-10-31, 6 commits)**
- **Stencil clear (re-apply of PR #3)**: as above.
- **`b136400`, Deck Low preset sun shadows at 2048 px and filtered (`r.ShadowQuality=3`):**
  - (a)-(b) Shadows "jump while the camera follows Dan" in Pumpkin Gorge at Low's 1024 unfiltered sun maps.
  - Not broken on the PC (console and up use 2048+ / filter 3+).
  - Our Deck preset: Low's sun cascade size (1024) with `shadow_quality=1` → probably the same jumping [inferred].
  - (c) Indirectly: Low was chosen for 30.
  - (e) **Sound** for a Low preset (quality vs GPU).
- **`05bda5c`, preshadows off in every preset ("a dark square around Dan that came and went with his idle animation"):**
  - (b) This matches our V1/V4 symptom closely: the per-object shadow projections "darkened their whole box", fixed
    on our PC with V4. Our PC runs preshadows **on** (default, live switch N-25) with no box since V4 [doc:
    patches-and-pr-proposal.md V4]. Our Deck preset turns them off for draw count, not for a box.
  - (c) No.
  - (e) **Workaround that hides a cause** (most likely the missing V4) [inferred, strong].
- **`e8fa1ad`, depth clears by draw, made opt-in in `05bda5c`:**
  - (b) It was aimed at the same square, which "turned out to be the preshadows".
  - (e) Unproven: opt-in only, as the study said.
- **`37aec40`, Gaming Mode update prompt**: n/a.

**PR #5 `steam-art` (2026-10-31, `57fec3d`)**
- (a) Steam library art from SteamGridDB and the game's `pic0.png`.
- (b), (c) None.
- (e) Sound, if we ship Add to Steam.

**PR #6 `skinning-fix` (2026-11-02, 8 commits)**
- **`6dcb901`, `r.SkinCache.Mode=0`, `r.MorphTarget.Mode=0` in every preset:**
  - (b) "A long white spike on Dan's silver shield" on the Deck.
  - `b182492` withdraws half the evidence: "the Enchanted Earth plank was the original game's" (their first diagnosis
    blamed a feature of the original).
  - Not in our docs or the upstream issues. Unknown on our PC.
  - (c) No.
  - (d) We set neither cvar.
  - (e) **Workaround that hides a cause**: compute-written vertex buffers read as vertex input point to a port
    synchronisation bug, on RADV or on all GPUs. One PC check first (`GOW3_UE4_CVARS=r.SkinCache.Mode=0;...`, the
    shield after collecting it).
- **`5e7b0e5`, firefly: a 4-byte edit in `BP_Firefly`** (the Lerp alpha 0.4 `cdcccc3e` → 0):
  - (b) It jittered left and right "every frame" beside Dan.
  - The claimed cause is a per-frame turn not scaled by the frame time [msg]. That is a game Blueprint issue,
    present at any frame rate, possibly on the PS4 too. Not in our docs.
  - (c) Not specific to 30; frame-rate dependent.
  - (e) **Plausible, but it changes original behaviour.** We did not read the bytecode. Check on the PC at 120 FPS
    whether the jitter is there at all before taking it.
- **`fbef777`, Low view distance 0.7 → 1:**
  - (b) Walls and barrels vanish under the top-down camera in the Sleeping Village.
  - Upstream's `low` has 0.7; the PC presets don't. **Our Deck preset sets `view_distance=0.7`** → likely the same
    [inferred].
  - (c) Indirectly: Low for 30.
  - (e) **Sound**. Test it on our Deck preset.
- **`c409fcc`, `3ad9f59`, D-pad debounce**: reverted in the same PR (`7215678`, `46a26fc`). n/a.

**PR #8 `mausoleum-order` (2026-11-03, `4c63bf4`)**
- (a) Write faults and readbacks go back into the command queue in order unless frame-wait serving is on.
- (b) Random pink / washed-out Mausoleum lights (their issue #7, Deck and ROG Ally).
  - **Self-caused**: their own `aea2165` (PR #3, from build .77) made those commands jump the queue even with
    serving off.
  - Ours never reordered.
- (c) No.
- (e) **Sound** as a fix of their own regression. n/a for us.

**PR #9 `asylum-lighting` (2026-11-05, `ef5d3cf`)**
- (a) The Deck preset keeps AO (`-1`) and SSR (`3`).
- (b) Asylum rooms shiny and over-bright, the lava room lit like daylight. This is a consequence of **upstream's
  `low`** values (AO 0, SSR 0), not a port bug.
  - PC presets never use them.
  - Our Deck preset sets AO 2 / SSR 1 live [code]. Probably fine, untested in the Asylum.
- (c) Indirectly.
- (e) **Sound**. The Asylum lighting depends on SSR/AO being on.

### 8.2 Why they see broken effects that we do not

The evidence supports one explanation with three parts. In order of how many of their fixes each explains:

1. **Their Deck mode runs upstream's `low` preset, and the PC never does.**
   - Upstream's `low` (`scripts/ue4_override.py:229-235` at `424f3ea`) turns off AO, SSR and volumetric fog, and
     sets view distance 0.7, unfiltered 1024 sun shadows and a 0.15 local-light threshold.
   - MediEvil's lighting was authored with those on. With them off, the levels look wrong: pink floors, a washed-out
     Mausoleum, a daylight Asylum, missing props, jumping sun shadows.
   - That explains `f6a90fe`, `ef5d3cf`, `fbef777`, `b136400` and the local-light threshold back-and-forth.
   - This is tied to the 30 FPS target only in that Low was picked to hold 30 on the Deck.
   - Our PC uses console and up. Our Deck preset re-enables fog, AO and SSR through live options, but keeps view 0.7
     and Low's sun shadows.
2. **Their base lacks `lab`'s renderer and data-path fixes for the same symptoms.**
   - V4, the stencil plane copy, fixed our box shadows. Their "dark square around Dan" and black shapes are most
     likely that bug; they hid it with preshadows off and chased it through clear semantics.
   - D2 / twins / page-only downloads replace the upstream copy-back that `433cd1a` blames for the wrong Mausoleum
     light colours.
   - V3 replaces volume re-uploads.
   - Upstream #40 shows the same black boxes on a Windows Ally X with upstream's build, so this is not a Linux or
     RADV problem.
3. **Their own experiments broke things, and later PRs fixed them:**
   - frame-wait serving reordered commands (pink Mausoleum, PR #8);
   - mapped arenas / deferred merges: stale data (white / pink / washed out), a hang, an illegal instruction;
   - local-light threshold 0.1 (bright pink);
   - the scissor stencil clear (square edges);
   - fixed clock + physics substeps (wisp, dragons);
   - file-read invalidation (reverted).

   At least four of their "fixes" undo their own earlier changes.

What is **not** the explanation:
- **The 30 FPS rate itself.** It accounts only for pacing symptoms: movies against a 30 cap, the title pan jumping on
  slow frames, the wisp under the fixed clock. Those are not broken effects.
- **RADV differences.** They account for performance (`717338d`, the depth-layout hang) and possibly the skin-cache
  spike, but none of the colour or shadow reports.

**Caveat on "we don't see them":** our docs record PC and Deck tests in Dan's Crypt, the graveyard and a little of
the Scarecrow Fields and the Sleeping Village. **Pumpkin Gorge, the Hilltop Mausoleum, the Asylum, the Hall of Heroes
and the Enchanted Earth have no test record.**
- Three symptoms could be on our build and simply not looked for [inferred]:
  - the stencil-clear edges;
  - the shield spike;
  - the firefly jitter.
- Their issue #7's "purple reflections in the Hall of Heroes" is still open on their side.
- One PC pass through those five levels with screenshots would settle it. Each check is a game launch and needs the
  user's go.

**Answered (the user, 2026-11-05):** "I have played through a lot of the game on PC, and I am fairly confident those
glitches do not exist. I have played a few 3-4 hour sessions, including those levels, and acquiring those items."
So on the PC (`lab`, 4090) none of the three shows; the docs simply never recorded those sessions. Still open: the
Deck at the Deck preset's view distance 0.7 and Low sun shadows (the vanishing props and jumping shadows), which is
a settings question, not a renderer bug. The Deck's manual settings do apply (log `data/deck-perdraw-1.log`: `r.ViewDistanceScale 0.7`, `r.Shadow.Min/MaxResolution 256`, `r.Shadow.DistanceScale 0.6`, `CSM.MaxCascades 2`, set live at the menu), and the user has seen no pop-in or sun-shadow jumps on the Deck (2026-11-05; play so far mostly the graveyard). Not a planned check: if it shows, view distance 1 is a live change.

## 9. Worth porting: shortlist

One commit on `lab` (or `deck-lab`) per row, with the fork author and hash in the message, and one launch with the
user's go per test.

| # | Port | From | Branch | Effort | Risk | Test |
|---|---|---|---|---|---|---|
| 1 | AvPlayer `Kernel::Thread` rewrite | dvilelaf `42b2afb` (PR #68) | `lab`, then `deck-lab` | S (one header) | L-M (thread lifetime; a mistake hangs movie ends) | PC: logo, intro, finish a level (exit movie), world map, restart level; the "HLE thread: join failed" line should be gone. Deck: five level ends |
| 2 | Curve injection compiled synchronously | Porticus `40d105e` (PR #52) | both | S (~15 lines) | L (two programs compile synchronously at level load) | Clear the pipeline cache, load the crypt: torch on the right wall has a flame. Compare compile ms at that load |
| 3 | Deck preset checks: the Asylum with our AO 2 / SSR 1; view distance 1 (`fbef777`); sun shadows 2048 + filter 3 (`b136400`) | spacepepperoni | `deck-lab` | S (live options, no build) | L (GPU cost at our 60 target; they held 30) | Deck, one live switch per check: the Asylum and the lava room; the Sleeping Village walls and barrels; Pumpkin Gorge sun shadows while walking |
| 4 | Credit lines for D1/D2/D4/D5 | Porticus PR #57 | `deck-lab` | S (docs / a note; no history rewrite) | none | — |
| 5 | Pad hotplug hints | Porticus `ffa6a72` (hotplug part only) | both | S | L | PC: start without a pad, plug it in, play; then unplug and replug |
| 6 | UE4 frame map + `ue4_motion.comp` + TAAU hook, debug view first | Porticus `82cb58e` (`vk_neural_render`'s Prepare / `ue4_motion.comp` only) | `lab` | M | M (hook on the TAAU dispatch in our reworked rasterizer; live render scale switches between the two TAAU hashes) | M1/M2 of plan-ue4-motion-vectors: `GOW3_NR_DEBUG`-style motion view; the user sees static walls ~0 and moving enemies coloured |
| 7 | FSR 3.1 / FSR 4 replacing the TAAU output | Porticus `82cb58e` | `lab` | M | M (NVIDIA never tested; the TAAU still runs) | Same scale, TAAU vs FSR 3 vs FSR 4 screenshots and GPU ms |
| 8 | FSR 3.1.6 frame generation + HUD compose + fsr-vulkan patches 0003/0004 | Porticus `82cb58e`, `9620962` | `lab` | L | H (our present path, Reflex and tick = cap differ from 1.3.7; 12.5k-line SPIR-V patch) | plan-fsr-frame-generation F0-F3 |
| 9 | Trophies | Porticus `f1b1f54` | both | M (1.7k lines, overlay + launcher) | L for the game, M for content (check the text sources) | Unlock one trophy; list; reset |
| 10 | AppImage: host sound / udev libraries, launcher Tk libraries, smoke test | TheRhysWyrill `507265e` | `deck-lab` | S-M | L | Only if Linux builds are published |

Not to take:
- FSR 1 removal (`d3a3e45`);
- the Xbox glyph strip (#31): game-art provenance and the readback cost;
- the `t.MaxFPS` runtime poll: we have a pak edit;
- the region-cache and free-index parts of PR #57: we have R1 and R3;
- spacepepperoni's `0ddcabc`: take #68 instead.

## Unverified / open

- **The eighth fork**: `forks_count` 8, 7 listed.
- **PR #61 never ran on NVIDIA or Windows.** The TAAU hashes are from their own dump. The PR does not say which game
  build (Porticus's other PRs used CUSA12982 01.00 and 01.02), or whether CUSA11227 shares the hashes. Check M1 (log
  the hash and the View-buffer size each frame) on our 01.02 before building on it.
- **Is the View-buffer layout per game version?** It is said to be UE 4.21's [msg]. Both of our eboots should be the
  same engine build [inferred].
- **Does the curve-injection bug show on our Windows `lab`?** The Windows pipeline binary cache may hide it on the
  user's PC. It would show on a fresh Deck build [inferred].
- **Is #74 (Iron Giant crash) on `lab`?** Unknown.
- **Why spacepepperoni dropped `c758207`**: no reason given.
