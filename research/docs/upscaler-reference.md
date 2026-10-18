# Upscaler reference: motion vectors, FSR, DLSS, frame generation

Read it **before changing the upscaler / MV / FG path and before every test launch** of that path. **Keep it
current:** a new rule, symptom or dead end goes in here the same day, in one line with a pointer to the findings.
Parent: [START-HERE.md](START-HERE.md).

## 1. Directory

| What | Where |
|---|---|
| Temporal upscaler (FSR 3.1/4, TAA, DLSS caller) | `gpu/shadps4/video_core/renderer_vulkan/vk_temporal_upscaler.{h,cpp}` |
| Camera motion / MV reconstruction | `vk_camera_motion.{h,cpp}`, `motion_history.h` |
| Pass identification (depth, colour, UI) | `vk_rasterizer.cpp:265-295, 959-970, 1101-1107` |
| DLSS loader / bridge | `vk_dlss.{h,cpp}`, `gpu/dlss_bridge/` |
| FSR-Vulkan (upscaler + FG API) | `gpu/third_party/fsr-vulkan`, patches `gpu/patches/fsr-vulkan/` |
| Present path / swapchain | `vk_presenter.cpp:689-860`, `vk_swapchain.cpp` |
| Pacing / virtual vblank | `driver.cpp:654-720`, `emulator_settings.h:31-52` |
| Per-game upscaler setting | `scripts/game_profiles.py`, `run.py:177-180` |
| UE4 override pak (TAAU, screen percentage) | `scripts/ue4_override.py` |

## 2. Invariants: must hold after every change

| # | Rule | What broke when it didn't | How to verify |
|---|---|---|---|
| 1 | **Never reuse the Bloodborne identifiers for MediEvil** (shader hashes, 864-byte camera buffer, far 3000, 5-MRT depth rule). Put MediEvil's in its own profile. | Inherited; why MediEvil ships with `GOW3_UPSCALER=none` (read 2026-10-18). | Grep for the hash in the MediEvil profile path. |
| 2 | **Depth is reversed-Z with infinite far** (to confirm at M0): FFX inverted + infinite flags, DLSS `depth_inverted = 1`. | Not yet seen. | Frame map; DLSS-G visualizer. |
| 3 | **One jitter source: the game's `TemporalAAJitter`.** Port viewport jitter off for MediEvil. | Not yet seen (double jitter expected otherwise). | Static scene shimmer; log jitter values. |
| 4 | **Identify targets by role, not pointer or size** (`r.SetRes`, 1440p/1800p/4K, TAAU scale change them; with `GOW3_UE4_BOOT_4K=1` the game boots at 3840x2160). | MGS2 invariant 13b (alternating targets). | Log identification across a resolution change. |
| 5 | **HUD-less colour matches the real frame bit for bit outside the HUD.** | MGS2 invariant 1 (output looks like 60 fps). | Diff the two images outside UI. |
| 6 | **DLSS-G matrices in `clip x M` layout; no extra transpose.** | MGS2 trap 13 (static walls treated as moving). | NVIDIA debug visualizer. |
| 7 | **Set `UserInterfaceRecompositionEnabled = 1` at DLSSG creation; set every NGX option explicitly** (zero-initialised). | MGS2 traps 15 and creation-time flag. | Log the parameter block. |
| 8 | **Judge FG by PresentMon displayed times and the user's eye**, not Present times or 1% lows. | MGS2 invariant 4. | Displayed-gap buckets. |

## 3. Before every test launch

1. The user has said go for this launch.
2. One change since the last launch, behind a switch; say which.
3. `medievil.ini` backed up and diffed against the backup.
4. Build output is new (exe timestamp, a new log string).
5. Say in advance what each outcome would mean, and what the user should look at.

## 4. Symptom → first place to look

| Symptom | Most likely cause | Look at |
|---|---|---|
| (fill in as found) | | |

## 5. Things not to retry

(None yet.)
