# Repo boundaries

Parent: [START-HERE.md](START-HERE.md). How changes reach upstream: [pr-checklist.md](pr-checklist.md).

## Remotes (set 2026-10-18)

| Remote | URL | Rule |
|---|---|---|
| `upstream` | github.com/bc160201983/medievil_2019_pc_port | **Fetch only.** Push URL is set to `DISABLED-never-push-to-upstream` on purpose. Changes reach it only as pull requests from the fork. |
| `origin` | github.com/BevelTipDrip/medievil_2019_pc_port (the user's fork, **public**) | Only `feat/*` PR branches (and `main` kept equal to upstream) are pushed here, and **only when the user says so.** Never `research/`. |

## Branches

| Branch | Contains | Pushed? |
|---|---|---|
| `main` | Exactly `upstream/main` (tracks it). No commits of ours. Update with `git fetch upstream` + fast-forward. | to `origin` only to keep the fork current, with the user's go |
| `lab` | Our working branch: `research/`, experiments, diagnostics, unfinished work. | **No** (local only; the fork is public). Private remote: **open** (the user to decide). |
| `feat/<topic>` | One change for one PR, cut from up-to-date `main`. Clean commits, only code and the port's own docs (`README.md`, `docs/`, `packaging/`). **No `research/`, no lab-only switches.** | to `origin`, with the user's go, then PR to `upstream/main` |

A finished change is rebuilt cleanly on a `feat/` branch from `main` (cherry-pick or rewrite), never by pushing `lab`.

## Other repos

| Repo | Path | Rule |
|---|---|---|
| Submodules | `gpu/third_party/*`, `third_party/LibAtrac9` | Do not commit inside them. FSR-Vulkan changes go in `gpu/patches/fsr-vulkan/*.patch`. `build.sh` applies those patches, so `git status` shows `m gpu/third_party/fsr-vulkan`: expected, never staged. |
| Bloodborne port | `C:\Users\Ty\MediEvil\bloodborne_pc` | Reference, no edits. GPL-2.0: code may be ported; cite the Bloodborne commit in the commit message and PR record. |
| MGS2 Lab | `C:\Users\Ty\MGS2-Lab` | **Read only.** No edits, builds or state-changing git. Ideas and math may be reused; it is D3D11/D3D12. Its `research\tools\` are models for ours. |
| MGS tools data | `C:\mgspf_tools\` | Other projects' data and Ghidra project: **never write there.** Only the Ghidra install is used. |
| Our research data | `C:\Users\Ty\MediEvil\research-data\` | Outside git: Ghidra project, captures, dumps, CSVs. |

Licences: this port GPL-2.0-or-later; Bloodborne GPL-2.0. NVIDIA DLSS DLLs, FSR 4 assets and game files stay out of git.
