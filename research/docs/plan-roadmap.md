# Plan: faster MediEvil with real motion vectors, FSR 3 FG and DLSS SR/FG

Status: planned 2026-10-18, nothing built. Parent: [START-HERE.md](START-HERE.md).
Read the area's reference doc ([gpu-threading-reference.md](gpu-threading-reference.md) or
[upscaler-reference.md](upscaler-reference.md)) before changing anything there.

## The rule

One change per launch, each behind a switch (environment variable or `medievil.ini` setting) that defaults to the
current behaviour. Measure before and after with the same scene and the same settings. A stage is **verified** only
when the user confirms it in game.

## Order (proposed 2026-10-18; the user to confirm)

0. **The user, 2026-10-18: fix the baseline issues first** (menu navigation, frame pacing):
   [plan-baseline-issues.md](plan-baseline-issues.md). DLSS / FG / other enhancements wait.
1. Build the port and run it with the user's 01.00 copy (baseline).
2. Profile the baseline: which GPU-side thread is the limit (`GOW3_FRAME_STATS`, `GOW3_SAMPLE_FILE`).
3. GPU threading / Bloodborne performance ports, chosen by step 2 ([plan-gpu-threading.md](plan-gpu-threading.md)).
   Can run alongside 4-6.
4. Map a UE4 frame, build real motion vectors, validate with FSR 3 / DLSS SR upscaling
   ([plan-ue4-motion-vectors.md](plan-ue4-motion-vectors.md)).
5. FSR 3 frame generation ([plan-fsr-frame-generation.md](plan-fsr-frame-generation.md)).
6. DLSS SR on correct inputs, then DLSS FG via NGX Vulkan ([plan-dlss.md](plan-dlss.md)).

## Progress

State values: `not started` / `built` / `verified` (the user confirmed in game). PR column: record file and PR
number once a stage is proposed upstream ([pr-checklist.md](pr-checklist.md)).

| Stage | State | Commit | PR | Notes |
|---|---|---|---|---|
| B0 Toolchain | done | | | MSYS2 CLANG64 + packages, Python 3.14 pillow/cryptography (2026-10-18) |
| B1 Build | built | `783b25a` | | 2026-10-18, `research/tools/build-clang64.sh`, ~1 min, 3 harmless warnings; gpu-check OK on RTX 4090 |
| B1b Game | done | | | CUSA11227 01.00 at `Game Files\CUSA11227`, eboot sha256 matches the port's patch build |
| B1c Fork / remotes | done | | | `origin` = BevelTipDrip fork, `upstream` fetch only, local `lab` branch (2026-10-18) |
| B1d Research tools | done | | | Ghidra 12.1.4 + GhidraOrbis (eboot imports), WinDbg attach script + PDB lab build (`research/tools/`), 2026-10-18 |
| B2 Baseline run | done | `783b25a` | | Run 1 by the user 2026-10-18 01:42-02:07 (4K_native, ultra, 240). Found I1 menus, I2 frame pacing: [plan-baseline-issues.md](plan-baseline-issues.md) |
| C1 Crash in vkCmdDrawIndexed (recorder thread) | not reproduced | | | Run 2 cutscene / level load: NVIDIA driver null read under `DrawRecord` lambda. Run 3 same place: no crash (non-deterministic). [plan-baseline-issues.md](plan-baseline-issues.md) |
| L1 Minidump on fatal fault | built, tested (injected fault) | `b01661e` | draft [pr-crash-dump.md](pr-crash-dump.md) | `user\crashdumps\`, `GOW3_CRASH_DUMP=0/full`. Installed in `out/` 2026-10-18 02:51 |
| L2 Sample the VkRecorder thread | built | `151dadb` | no (diagnostic) | `GOW3_SAMPLE_FILE` now also writes `<prefix>-VkRecorder.txt` |
| I1 Menu navigation | cause confirmed; fix attempt 1 built | `ebf85f4` | | The user, run 3: menus work at `native` → live r.SetRes (4K_native) breaks them. L3: boot at 4K like a PS4 Pro on a 4K TV (`GOW3_UE4_BOOT_4K=1`): patch + 4K VideoOut, no live change. Test T-I1b. |
| I2 Frame pacing (stage B bound) | not started | | | Stage B 93-97% busy, FPS follows draws/frame. Tests T-I2a profile, T-I2b texture helper |
| P1 Baseline profile | not started | | | Scene(s) to be chosen with the user |
| V1 Pattern scan for version-specific addresses | not started | | | The user's idea, 2026-10-18; upstream candidate. r.SetRes (`runtime_extra.c:560-583`), 4K pool patch |
| T* GPU threading | not started | | | Stages in plan-gpu-threading.md |
| M* Motion vectors | not started | | | Stages in plan-ue4-motion-vectors.md |
| F* FSR 3 FG | not started | | | Stages in plan-fsr-frame-generation.md |
| D* DLSS | not started | | | Stages in plan-dlss.md |
| DLSS FG backend - decision | **NGX directly (Vulkan)** (proposed 2026-10-18) | | | DLSS SDK 310.9.1 has `NGX_VK_CREATE_DLSSG`; matches MGS2; the port already has an NGX bridge. User to confirm. |

## Test notes

(Append dated entries: scene, settings, numbers, what the user saw.)
