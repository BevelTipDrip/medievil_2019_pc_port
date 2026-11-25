# Review: performance items from 2026-11-14 to 11-18 that were never acted on (2026-11-24, static)

Parent: [plan-work-queue.md](plan-work-queue.md). Read-only review: no build, no launch, no commit. Sources: the live
session transcript (records 2026-11-14T00:00 to 11-18T12:00 UTC, user text and assistant text only, extract
`research-data/session-review-20261124/transcript-1114-1118.txt`, scripts beside it), the work queue (all rows),
[study-compiler-optimizations.md](study-compiler-optimizations.md), [study-tooling-off-cost.md](study-tooling-off-cost.md),
[study-perf-deepdive-20261123.md](study-perf-deepdive-20261123.md), the patch cards (`patchmap.py find`), and
`git log --all` for the window and after. Transcript times are UTC; commit times are local (UTC-5).

Status words: **done** (built and run or verified, or closed by a decision recorded with it), **run** (built and run,
not verified by the user), **rejected** (measured and turned down, or turned down by the user), **never started**
(proposed or queued, nothing built or measured; includes parked items), **half done** (started, part left, or fix
built and not run).

## Items

### Compiler (study-compiler-optimizations.md, queued by the user 2026-11-16 22:31 UTC "Que them up along with PGO")

| # | Item | Proposed / agreed | Status now | Evidence |
|---|---|---|---|---|
| C1 | Clang PGO: fix `GOW3_PGO`, train, A/B | study rank 1; the user 11-17 00:54 "Let's start working on Clang PGO" | **rejected** (run: average same, spikes and lows worse; off by default, `GOW3_PGO=use` opt-in) | `ef5d9d2`, `d6bb436`, `7158c95`; card tooling/clang-pgo |
| C2 | Refined PGO: gameplay-only training (reset counters after the level load), keep rare code from being cold | the user 11-17 02:29 "Having a way to 'trim' the profile would be great"; 03:09 "a more refined PGO build. We can revisit it later" | **never started** (parked) | card tooling/clang-pgo "Retry ideas" |
| C3 | x86-64-v3 / v4 release exes, A/B on the 7800X3D | the user 11-16 18:35 "Using AVX2 or maybe AVX512 ... seems worthwhile" | **rejected** (run: more 8-12 ms gameplay spikes; the user: "the baseline is the winner") | `db8b7d1`, `a0f1ba0`; card tooling/cpu-level-windows |
| C4 | run.py picks the highest exe the CPU supports, launcher override, stripped variants | proposal 11-16 18:36, only if C3 showed in the lows | **rejected** (conditional on C3) | queue row "Compiler optimizations" |
| C5 | Re-test the Deck's v3 build (it lost before the CPU-side fixes) | the user 11-17 02:36 "it's worth revisiting soon" | **never started** | card deck/s1-v3-build: "built, opt-in, unmeasured" |
| C6 | ThinLTO for atrac9 | study rank 3; assistant 11-17 03:09 "two untested smaller items" | **never started** | `gpu/CMakeLists.txt:309` still `INTERPROCEDURAL_OPTIMIZATION OFF` |
| C7 | `-Wl,--gc-sections,--icf=safe` | study rank 6 (same message) | **never started** | no `icf` in the CMake files or build.sh |
| C8 | BOLT after PGO, lld as the Deck default | study ranks 4-5 | **never started** (BOLT depends on C2) | - |
| C9 | `-O3` under PGO; XXH3 dispatch; `-march=znver4` personal build | study ranks 7-9 | **never started** (all depend on C2 or a profile) | - |
| C10 | OpenTuner | the user 11-16 18:06 asked | **rejected** (no fast low-noise benchmark) | study "Findings" 5 |

### Diagnostics that cost when off (study-tooling-off-cost.md; the user 11-15 14:53 "add it to our to do list")

| # | Item | Proposed / agreed | Status now | Evidence |
|---|---|---|---|---|
| O1 | Step 1: unconditional counters behind the stats switch | the user 11-16 13:06 "Yes, start on step 1" | **done** (run; the user: "cleaned up most of the remaining stutters") | `fa136ee`, `f5b0bc7`; card tooling/stats-counters-gated |
| O2 | Step 2: `GOW3_DEV_TOOLS` release / dev builds | the user 11-16 18:06 (decisions 1-3) | **half done**: A/B verified (the user 11-17 00:54 "I'd call this verified"), but `LOG_DEBUG` was not compiled out (gate in the wrong log.h, 881 sites) and runtime-switchable dev diagnostics stayed (perf deep dive 11-23). Finished in P5 on `perf-hygiene`: fix built, not run | `21886b6`; `a1bb455`..`aa3d304` |
| O3 | Step 3: launcher greys dev diagnostics; Linux release build and AppImage | the user 11-17 04:51 "Let's work on 1, the deck needs some love" | **done** (Deck played the release AppImage: no regression seen) | `3b05a90`, `bda5f7f`, `ed952f0` |
| O4 | Small fix: semaphore `getenv` inside the global semaphore lock | inventory 11-16; assistant lists 11-17 00:49, 06:06 | **half done** (fix built in P5, `GOW3_DEV_TOOLS &&` before the getenv; not run) | `00358ca`; `src/runtime_sema.c:75` |
| O5 | Small fix: debug-marker labels formatted before their gate | inventory 11-16 | **half done** (fix built in P5, not run) | `perf-hygiene`, `vk_rasterizer.cpp:431-435, 4194` |
| O6 | Small fix: `GOW3_FRAME_STATS=0` half-enables stats | inventory 11-16 | **done** | `fa136ee` |
| O7 | Always-on log lines (unbuffered stdout; semaphore-create printf under the global lock, missing-file lines) | inventory 11-16: "rest later" | **half done**: P6 makes each line one write (`6352211`, fix built, not run); the semaphore-create `printf` still runs inside the global lock (`runtime_sema.c:59`) | queue P6 |
| O8 | Optional reverse-order confirmation pair for step 1 | assistant 11-16 13:39 | **never started** (optional; step 2's A/B superseded it) | - |
| O9 | HDR tooling compiled out, HDR pipeline built only with HDR | the user 11-17 20:06 "make sure all of the tooling added for HDR can be compiled out" | **done**; the proposed grading "on" flag and per-shader list memo were dropped after code reading (nanoseconds) | `b00e39f`; transcript 11-17 20:27 |

### Guest locks and waits (the static-view spikes, 2026-11-15/16)

| # | Item | Proposed / agreed | Status now | Evidence |
|---|---|---|---|---|
| S1 | Guest rwlocks without the global registry lock and calloc | the user 11-15 20:23 "Let's look at a the expensive lock path" | **run** (game thread rwlock time ~0; not verified as a felt change) | `4632e2e`; card runtime/rwlock-fast-path |
| S2 | Change 2a: timed condition waits end at their deadline | the user 11-15 23:18 "Let's do change 2 next" | **done** (the user 11-16 01:36 "B is legitimately better"; 1% / 0.1% low 220.4 / 193.7 -> 233.2 / 227.7) | `e12ffaa`, `57d3450` |
| S3 | 2a spin option `GOW3_COND_SPIN_US` | the user 11-15 23:31 "put the spin option in at the start" | **done** (built, opt-in; the user 11-16 00:21 chose B, no spin) | card runtime/cond-precise-timeout |
| S4 | Change 2b: guest mutexes on SRWLOCK | the user 11-16 12:28 "Let's do 2b" | **never started** as code: measured on the PC (no gain worth building); the user 11-17 03:28 "keep it on the list ... On the deck, basically every thread is being used". Deck re-measure (a futex prototype) not started | `031f54a`, `3507bdf`; queue row "change 2b" |
| S5 | Exit log line counting guest lock calls a second | assistant 11-16 12:29 | **rejected** (superseded by the wait-sites data; led to the dev/release build question) | - |
| S6 | RHIThread's 2-3 ms wait: name the holder | the user 11-17 03:28 "Yes, we can work on the RHIThread waits" | **done** (found: paced by presentation, Reflex sleep before the present) | `0a74757`..`178814e` |
| S7 | Reflex vs the port's own limiter in heavy combat | assistant 11-17 04:32 option 1 | **rejected** (the user 04:43: Reflex's stability is "leagues ahead") | queue row "RHIThread waits" |
| S8 | `GOW3_GPU_PROFILE` for in-frame GPU idle | assistant 11-17 04:32 option 3 | **rejected** (the user: "Only something like Nvidias Nsight can do that"); no Nsight run was queued in its place | same row |
| S9 | Draw-prep worker priority A/B (IDLE / below / normal) under a `cpu_hog.py` burst load | assistant 11-15 23:13; the user 11-17 04:23 "We might want to try the thread priority change on the helper threads next", 04:43 "We can try this at a later time" | **never started** (no `cpu_hog.py`; `GOW3_PREP_PRIORITY=below` still never A/B'd) | queue row "Draw-prep workers starved"; card stage-b/prep-priority |
| S10 | Make stage A's decode faster on the PC (its work time is on the path to the GPU) | the user 11-17 04:43 pushback: "If stage B only spent 10% of its time waiting for packets from stage A, stage B could finish sooner" | **never started**; recorded only as a remark in the RHIThread row, no item (the Deck's A1-A3 are older) | queue row "RHIThread waits" |

### Spikes, stalls and tracing

| # | Item | Proposed / agreed | Status now | Evidence |
|---|---|---|---|---|
| P1 | Flip trace rows in memory, written at exit | the user 11-16 01:46 "let's do #1" | **done** (run) | `78b8e17`; LANDMINES 8b |
| P2 | `gpu_ready` rows opt-in | the user 11-16 11:31 "Let's do a run with it off" | **done** (run; the user: fewer, smaller spikes) | `aa4c74b` |
| P3 | The 1.36 s periodic stall (a Vulkan call blocking ~5 ms): one long undisturbed JitterGpu recording | the user 11-15 22:04 "Let's start with A" | **never started** after the first try (the profile was fixed; the recording was never made; parked) | queue row "Small frame-time spikes" (b) |
| P4 | The same stall by toggles (DLSS vs TAA, pageable video memory) | assistant 11-15 22:00 option B; the user "then B if it doesn't pan out" | **never started** (fallback to P3) | - |
| P5 | GPU-side clusters under VRR (present back-pressure) | assistant 11-15 23:00 | **rejected** (the user 23:08: the present settings are deliberate) | queue row "GPU-side frame clusters" |
| P6 | Trace the first-use spikes (save menu, Graveyard's first view, Tome pickup) | assistant 11-17 00:49 and 06:06 item 1 | **rejected** (the user 11-17 06:33: "#1 is somewhat inevitable ... asset streaming always has a chance of causing a frametime spike"); not recorded in any doc | transcript only |
| P7 | Stage A copy faults that stall 1-3 ms: name the handler, count at 100% on PC and Deck | the user 11-15 15:59 "Yes, let's add it to the queue, that might be more impactful on lower spec hardware" | **never started** | queue row "Stage A copy faults" |
| P8 | 200% deadlock: deferred copies read the backing view | the user 11-15 14:00 "Let's actually go for C" | **run** (three 200% runs without a hang; not verified; the 75% A/B was inconclusive) | `3a84125`; card stage-b/deferred-copy-backing |
| P9 | Stage A's deferral check also looks at image protection | assistant 11-15 15:20 option 2 | **rejected** (the user: "I am fairly certain option 1 is going to be heavy"; backing-view read chosen) | - |
| P10 | 1 GB framebuffer pool (tester's 200% OOM) | assistant 11-15 13:56 option B | **never started** (parked; the user: 8K internal is pointless) | queue row "Render scale above 100%" (1) |
| P11 | Cap render scale at 4K output (e.g. 150%) | assistant 11-15 13:56 option A (recommended) | **never started** (the user picked C; 200% at 4K still ends in the pool OOM for players) | - |
| P12 | Confirm the mouse-log waves with a `GOW3_MOUSE_LOG=1` run | queue row 11-15 ("would confirm") | **never started** (left partly open by the user). Note: mouse read stats are now compiled out of release (P5), so this needs the dev build | `95a5487`, `d1662e7` |
| P13 | A repeatable stress test for A/Bs | the user 11-15 16:46 "we really need some kind of easilly repeatable stress test" | **half done**: static study of the debug menu and engine facilities today (`52d33b3`, study-debug-menu-automation.md); nothing built. P5 compiled pad record / replay out of release (`00358ca`): a replay route now needs the dev build or a release switch | queue row "A repeatable stress test" |
| P14 | WPR: a note per capture before re-arm; hotkey numbering continues after the highest file | the user 11-15 17:53 "add a note to the trace before re-arming is allowed, but we can do that later" | **never started** | `research/tools/wpr-hotkey.ps1:59` still `$n = 0` |
| P15 | Write faults on the game threads (~760 a frame) | carried; assistant 11-17 06:06 item 3 | **never started** (the deep dive's P8, guest faults draining the pipe, is the related open item) | queue rows "Write faults", P8 |
| P16 | Raise the wait trace's threshold before using it for process-wide stalls | assistant 11-15 22:00 | **done** (`GOW3_WAIT_TRACE_MIN_US`, default 500 us) | card tooling/wait-trace |

### Steam Deck and FSR

| # | Item | Proposed / agreed | Status now | Evidence |
|---|---|---|---|---|
| D1 | Deck frame pacing: traces at PC depth; revisit early frame EOP / decode pacing, drain hybrid, thread priorities; maybe a Deck-only build | the user 11-17 06:02 "we need to revisit some switches ... Let's table it for now" | **never started** (tabled) | queue row "Deck frame pacing"; `ed952f0` |
| D2 | A Linux sampler (signal-based or `perf` with frame pointers) for that tracing | assistant 11-17 06:02 | **never started** | same row |
| D3 | Deck release-vs-dev A/B; pull the release run's log (`deck.sh pull`) before it is overwritten | 11-17 (the Deck had to charge) | **never started** | queue row "Logging and trace code" (end) |
| D4 | Linux `build.sh --test` does not link (test_pad stubs) | found 11-17 | **never started** | queue row |
| D5 | FSR 2.x for the Deck (cheaper than 3.1) | the user 11-14 05:01 "We might want to offer FSR 2.1 ... We'll address that later" | **never started** | plan-fsr.md l.73 |
| F1 | FSR reactive mask: slider at 0 and hidden | the user 11-14 08:47 "Put the slider at 0 and hide it" | **done** | `2553714` |
| F2 | The motion-pass barrier step 4 widened to all commands (a GPU wait on every DLSS / FSR frame) | found 11-14 08:55; the user 09:02 "What's this change that introduced another GPU wait?" | **done** (back to compute-only) | `2553714` |
| F3 | Revert the bit-3 stencil trace `5a796af` | assistant 11-14 08:43 and 08:55 asked | **never started** (never answered; still on copy-fault-trace / deck-lab / hdr, runs only with `GOW3_UE4_MOTION_VIEW`) | `5a796af` |
| F4 | A cheaper upscaler stats switch | FSR side item 11-14 04:11 | **done** in effect (compiled out of release in step 2) | `21886b6` |

### Shipping

| # | Item | Proposed / agreed | Status now | Evidence |
|---|---|---|---|---|
| R1 | Ship the verified gains (precise condition waits, rwlocks, gated counters, release build, flip trace fixes) | the user 11-17 03:09 "I think we know exactly what we should ship" | **half done**: all of it is on `copy-fault-trace` / `hdr` / `hdr-final` / `perf-hygiene`, pushed to the fork, but not in any release (v2.4.0 was tagged 11-15 04:30 local, before it) and not on `deck-lab`, the default branch. Planned to ship with v2.4.1 (plan-hdr-final.md) | `git tag --contains e12ffaa`: none |

### Non-performance request found on the way

| # | Item | Proposed / agreed | Status now | Evidence |
|---|---|---|---|---|
| N1 | Delete save slots from the Load list (details page shown, hold the PC Settings restart combo, a hint on the details page) | the user 11-15 13:21 "I do want to add another feature: deleting slots from the menu" | **never started**; no trace in any doc, card or commit | transcript only |

**Counts:** 56 items. Done 13, run 2, rejected 10, never started 25, half done 6.

## Asked for by the user with no trace in the docs

- **N1, save slot deletion** (11-15 13:21): in no doc, card, queue row or commit.
- **P6, first-use spikes:** the user's decision (11-17 06:33, "somewhat inevitable") is recorded nowhere; the queue has
  no row for them.
- **S10, a faster stage A on the PC** (11-17 04:43): only a remark inside the RHIThread row, no item.
- **S8, Nsight for in-frame GPU idle** (11-17 04:43): only a remark; no Nsight run queued.
- **The general instruction of 11-17 03:28** ("we have written off things before that we thought would be too small
  to notice ... our work in the past day has encompassed fixes we previously determined to be not worth doing"):
  applied to 2b only; no sweep of earlier "too small" rejections was queued (the 11-04 Deck re-score predates it).

## Worth doing now (ranked)

1. **Run the `perf-hygiene` bundle (P3-P6, one launch against the saved exe).** It finishes O2, O4, O5 and O7 from this
   window (LOG_DEBUG really out of release, the semaphore getenv, marker labels, one write per log line). Built, only
   the run is missing.
2. **Ship R1 with v2.4.1.** The verified gains of 11-15/16 (0.1% low 193.7 -> 227.7 at the cap; heavy-stretch spikes
   about 5x fewer) have been on side branches for 9 days; players and the Deck default branch still run without them.
3. **The repeatable stress test (P13).** Every remaining compiler and scheduling A/B (C2, C5, C6/C7, S9) is below
   run-to-run noise without it. Decide first how a release build replays a route, since P5 made pad replay dev-only.
4. **Draw-prep priority under load (S9).** The switch exists; it needs `cpu_hog.py` and one A/B. The user asked for it
   twice; it matters for players with background load and for the Deck.
5. **Stage A copy-fault stalls (P7).** A dev-build count at 100% on the PC and the Deck with the existing copy-fault
   trace; cheap, and the user tied it to lower-spec hardware.
6. **A Deck session (D3, C5, D1, S4's Deck re-measure):** pull the log, release vs dev, the v3 build, then the pacing
   traces. "The deck needs some love" (11-17).
7. **atrac9 ThinLTO + `--gc-sections --icf=safe` (C6, C7):** build-only changes, bundle into one A/B after 3.
8. **Refined PGO (C2):** only once 3 exists (gameplay-only training, rare code not cold, reverse-order pairs).

Not now: P3 / P4 (needs a long undisturbed recording), P10 / P11 (8K internal not wanted; P11 is a small safety cap
if players keep hitting the 200% OOM), D5 (FSR 2.x), F3, P14 (small tooling, any time).
