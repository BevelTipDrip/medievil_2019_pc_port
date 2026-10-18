# START HERE: MediEvil port work

Entry point for every session. **Keep it current:** when a stage lands, update
[plan-roadmap.md](plan-roadmap.md)'s Progress table and, if the state below changes, this page.

## Read in this order

1. This page.
2. [plan-roadmap.md](plan-roadmap.md): the working plan (order of work, Progress table).
3. The latest handoff: [handoff-2026-10-18.md](handoff-2026-10-18.md).
4. [building-and-tools.md](building-and-tools.md): toolchain, build, run, profiling. **Do not re-install the
   toolchain or re-derive the build: it is written down there.**
5. Before touching an area, its reference doc:
   [gpu-threading-reference.md](gpu-threading-reference.md) (scheduler, recording, submit) or
   [upscaler-reference.md](upscaler-reference.md) (motion vectors, FSR, DLSS, frame generation).

## Rules (the user's; also in Claude's memory)

- **Every game launch needs the user's explicit go for that launch.** Ask before closing the game.
  One change per launch.
- **Never push or tag without asking.** `origin` is the public upstream (bc160201983/medievil_2019_pc_port).
- Never roll back or delete work without asking.
- Never touch the save folder. Back up `medievil.ini` before editing it.
- `C:\Users\Ty\MGS2-Lab` is **read only** (reference for the FSR FG / DLSS / DLSS FG work).
- `bloodborne_pc\` is a reference repo. It is GPL-2.0 like this port, so its code may be ported, with the
  Bloodborne commit hash in the commit message.
- Check in instead of assuming. Confirm screenshots with the user before concluding.
- **"Verified" means the user confirmed it in game** (quote them). A log line or number goes with it.

- **Anything for upstream goes through [pr-checklist.md](pr-checklist.md)** and its `pr-<topic>.md` record: one
  change per PR, clean coherent code, proof of what it does, how, and how it fits. The user reads it before any push.

- **Commit freely on `lab`** (the user, 2026-10-18): one commit per change, message says why, ends with the
  attribution line. `lab` stays local. Pushing anything still needs the user's go.

Open: a private remote for `lab` is not decided yet.

## Current state (2026-10-18)

| Area | State |
|---|---|
| Repo | `783b25a` (MediEvil 1.3.2), 6 submodules. Remotes: `origin` = the user's fork BevelTipDrip/medievil_2019_pc_port (public), `upstream` = bc160201983 (fetch only). Working branch `lab` (local only); `main` = upstream. `research/` not committed yet. See [repo-boundaries.md](repo-boundaries.md). |
| Toolchain | MSYS2 CLANG64 installed with the README package list (clang 23.1.3, cmake 4.4.4, ninja 1.13.2). Python 3.14 has pillow + cryptography. |
| Build | Built 2026-10-18 at `783b25a` with `research/tools/build-clang64.sh` (not run in game yet). GPU: RTX 4090. |
| Game | **CUSA11227 01.00, unpacked at `C:\Users\Ty\MediEvil\Game Files\CUSA11227`** (from `Game Files\MediEvil-CUSA11227_V0100.pkg`, 2026-10-18). `eboot.bin` sha256 `a7e976d1…3b828` = **exactly the build the port's patches and addresses were checked against.** Not launched yet. (Earlier, wrong file: `MediEvil [BASE][UPDATE 1.02][CUSA42759].pkg`, unpacked then deleted at the user's request, is the **1998 PS1 MediEvil** in Sony's PS1 emulator (PS Plus Classics: `data\*.bin` disc images, `config-title.txt` emulator flags, 4.4 MB `eboot.bin`), not the 2019 UE4 remake.) Version notes for other builds: 4K pool patch is 01.00 + sha256 gated (`scripts/patches.py:72`); **native output modes use a fixed 01.00 address with no check** (`src/runtime_extra.c:560-583`); Blueprint edits are byte-pattern checked (`scripts/ue4_override.py:241-295`); profiles keyed by title ID (`scripts/game_profiles.py`); SSE4a sites irrelevant (Ryzen 7 7800X3D). |
| GPU threading / perf | Researched: this port forked from Bloodborne v0.2 (`0afaaa2`); Bloodborne 0.3-0.51 not in. 14 candidates ranked (top: targeted host-copy waits, readback queue, residency blocks, texture collector). See [plan-gpu-threading.md](plan-gpu-threading.md). **Open:** profile first (P1). |
| Motion vectors | Researched: the port's temporal upscalers are off for MediEvil (`GOW3_UPSCALER=none`), hooks are Bloodborne's. See [plan-ue4-motion-vectors.md](plan-ue4-motion-vectors.md). **Open:** map a UE4 frame. |
| FSR 3 FG | Not started. FSR-Vulkan already links a FG API with no caller. See [plan-fsr-frame-generation.md](plan-fsr-frame-generation.md). |
| DLSS SR / FG | SR bridge exists (NGX, inputs wrong for UE4). FG: NGX Vulkan (`NGX_VK_CREATE_DLSSG`), not Streamline. See [plan-dlss.md](plan-dlss.md). |
| Baseline issues | **First priority (the user, 2026-10-18).** I1 menu navigation at 4K: **fixed and verified by the user** with `GOW3_UE4_BOOT_4K=1` (boot at 4K like a PS4 Pro on a 4K TV, `ebf85f4`; opt-in, log still to collect). I2 frame pacing 50↔120 FPS: stage B (draw recording) 84-97% busy, FPS follows draws/frame, CPU-side (resolution/quality barely matter). C1 rare crash in the NVIDIA driver under a recorded draw: dumps now written on any fault. See [plan-baseline-issues.md](plan-baseline-issues.md). |
| Next | See the handoff's "Start here next time": WPR trace with stacks of the busiest game thread's waits ([plan-frame-pacing.md](plan-frame-pacing.md)). Done today: I1 fix, H1/C1 race fix, R1-R3 (+17% heavy-view FPS). Pending idea (the user, 2026-10-18): replace fixed 01.00 addresses (r.SetRes, 4K pool patch) with pattern scans, worth offering upstream; needs a remake eboot to test against, so not started. |

## Knobs and triggers

Environment variables and settings already in the port that this work uses (full lists in the port's own docs):

| Knob | What |
|---|---|
| `GOW3_GAME_DIR` | Game folder or .pkg for `run.py` |
| `GOW3_FRAME_STATS` | Per-frame stage timings |
| `GOW3_SAMPLE_FILE=<prefix>` | Sampling profiler for GPU threads; `scripts/symbolize_samples.py` |
| `GOW3_CAPTURE_TRIGGER` | Frame capture (for mapping the UE4 frame) |
| `GOW3_FRAME_DUMP_TRIGGER` / `GOW3_DUMP_DIR` | Dump guest display buffer and presented image |
| `GOW3_DRAW_PIPE` | Two-stage draw pipeline (auto at 8+ threads) |
| `GOW3_VK_RECORD_THREAD` | The single Vulkan recorder thread (0 = off) |
| `GOW3_ASYNC_SHADERS` | Async shader recompile / pipeline creation |
| `GOW3_UPSCALER` | Temporal upscaler; `none` for MediEvil today (`scripts/game_profiles.py:9-16`) |
| `GOW3_DLSS` | DLSS bridge on/off |

## Document index

| Doc | What |
|---|---|
| [plan-roadmap.md](plan-roadmap.md) | Working plan: rule, order, Progress table |
| [plan-baseline-issues.md](plan-baseline-issues.md) | Run 1 findings: menu navigation, frame pacing; tests |
| [plan-gpu-threading.md](plan-gpu-threading.md) | Port Bloodborne 0.3+ performance work (threading, waits, memory), ranked |
| [plan-ue4-motion-vectors.md](plan-ue4-motion-vectors.md) | Real UE4 motion vectors, depth, jitter, camera |
| [plan-fsr-frame-generation.md](plan-fsr-frame-generation.md) | FSR 3 frame generation |
| [plan-dlss.md](plan-dlss.md) | DLSS SR inputs, DLSS FG via NGX Vulkan |
| [gpu-threading-reference.md](gpu-threading-reference.md) | Invariants, symptoms, things not to retry: scheduler/recording |
| [upscaler-reference.md](upscaler-reference.md) | Invariants, symptoms, things not to retry: MV / FSR / DLSS / FG |
| [building-and-tools.md](building-and-tools.md) | Toolchain, build, run, profiling, paths |
| [repo-boundaries.md](repo-boundaries.md) | Remotes, branches (`main` / `lab` / `feat/*`), what may be pushed or copied |
| [pr-checklist.md](pr-checklist.md) | The bar for anything proposed upstream (scope, code, proof, commits) |
| [pr-template.md](pr-template.md) | Copy to `pr-<topic>.md` per PR; becomes the PR description |
| [handoff-2026-10-18.md](handoff-2026-10-18.md) | Latest handoff |
| `data\` | Measured logs and captures, linked from the docs |

## Elsewhere

- The port's own technical docs (inherited, mostly Bloodborne/GoW3): `docs/upscaler.md`, `docs/motion_vectors.md`,
  `docs/parallel_gpu.md`, `docs/DEPTH_ADAPTIVE_TAA.md`, `docs/MODS.md`; Windows notes `packaging/windows/README.md`.
- Bloodborne reference: `C:\Users\Ty\MediEvil\bloodborne_pc` (`docs/CHANGES_*.md`, `docs/ROADMAP.md`).
- MGS2 reference (read only): `C:\Users\Ty\MGS2-Lab\MGSHDFix\research\docs\` (`fsr-reference.md`, `hud-dlssg.md`,
  `handoff-2026-10-01-dlss.md`, `plan-framegen-backends.md`).
