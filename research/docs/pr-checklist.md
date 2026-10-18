# PR checklist: the bar for anything proposed upstream

Parent: [START-HERE.md](START-HERE.md). Branch and remote rules: [repo-boundaries.md](repo-boundaries.md).
Every PR has a record, `pr-<topic>.md` (copy [pr-template.md](pr-template.md)), which becomes the PR description.
Nothing is pushed or opened without the user's go.

## Scope

- [ ] One change per PR. Unrelated cleanups go in their own PR.
- [ ] Cut from an up-to-date `main` (`git fetch upstream`, fast-forward `main`, then `git switch -c feat/<topic>`).
- [ ] Only code and the port's own docs. No `research/`, no lab-only diagnostics, no game files, no NVIDIA/FSR 4
      binaries, no `out/`, no submodule changes (FSR-Vulkan changes go in `gpu/patches/fsr-vulkan/`).

## Code

- [ ] Reads like the surrounding code: naming (`gow3_` / `GOW3_` / `Gow3`), comment density and style (short,
      factual, says why; addresses and offsets named), C11/C++ idiom of the file, existing helpers reused.
- [ ] Windows and Linux both still build (the port builds on both; `#ifdef _WIN32` where needed).
- [ ] Behaviour changes sit behind the port's existing setting style (env var / `medievil.ini` / launcher) with a
      safe default; a version- or build-specific path fails safe and logs why.
- [ ] No warnings added. No dead code, no debug prints left in.
- [ ] Code from the Bloodborne port: commit message names the Bloodborne commit and files.

## Proof (in the PR record, with links to the evidence)

- [ ] **What**: the problem, shown in the code (`file:line`) and/or a log.
- [ ] **How**: the mechanism, in enough detail to review without running it.
- [ ] **How it fits**: every file touched and why; which existing mechanisms it uses or changes; what callers see;
      what happens on unsupported versions or hardware.
- [ ] Build log clean (`research/tools/build-clang64.sh`, `out/build.log` ends `build.sh exit: 0`).
- [ ] `bash build.sh --test` passes, plus new tests where the change is testable in isolation.
- [ ] In-game results, run by the user: what was checked, on which game build (eboot sha256), settings, and the
      user's own words. Before/after numbers for performance changes (same route, `GOW3_FRAME_STATS`), with logs in
      `research/docs/data/` (summarised in the PR, not attached raw unless asked).
- [ ] Risks and limits stated honestly, including what was **not** tested.

## Commits

- [ ] English, imperative subject, body explains why. Upstream style for reference: `MediEvil 1.3.2: <summary>`
      for releases; ours are topic commits, e.g. `Native output: find r.SetRes by pattern instead of 01.00 address`.
- [ ] Each commit builds. History squashed to what a reviewer needs.
- [ ] Ends with the attribution lines the session requires.

## Before opening

- [ ] The user has read the PR record and the diff, and said go.
- [ ] `git diff upstream/main...feat/<topic> --stat` matches the record's file list exactly.
- [ ] Record status set to `proposed`, then `PR #n` with the URL once opened; roadmap Progress table updated.
