---
id: lineage-toggle-file
title: Live toggle file: a decimal mask re-read every 250 ms; a set bit switches a fix off (second and third words)
area: tooling
status: active
platforms: all
default_windows: on
default_deck: on
origin: upstream (inherited lineage); second word ours (e0c7566), third word ours (5ead842)
class: G
commits: e0c7566, 5ead84222d
files: src/runtime_memory.c, gpu/shim/gow3_toggles.h, gpu/shim/gow3_overlay.cpp, launcher/medievil_launcher.py, research/tools/toggle-ab.ps1, research/tools/deck.sh, research/tools/deck_summary.py
symbols: toggle_watcher, runtime_disabled_optimizations, runtime_disabled_optimizations2, runtime_toggle_frame_cap, runtime_memory_set_gpu_hooks, POOL_KEEP_VIEWS, Gow3Toggle, Gow3Toggle2, Disabled, TestLabel
switches: GOW3_TOGGLE_FILE, toggle:NarrowImageStages
tests: N-30, N-31, N-32, Launch 14
docs: fix-inventory.md#for-other-ports-how-to-read-this, study-live-switches.md#how-live-works-today-three-mechanisms
depends_on:
related: deck-test-tooling, M1, narrow-image-stages, bulk-descriptor-copy, sampler-memo, ring-reuse, perf-patches-ini
---
## What it does

When `GOW3_TOGGLE_FILE` is set, `runtime_memory_set_gpu_hooks` starts `toggle_watcher`, which every 250 ms reads
up to three decimal numbers (`fscanf "%llu %llu %llu"`): `runtime_disabled_optimizations` (`Gow3Toggle`, bits
0-63), `runtime_disabled_optimizations2` (`Gow3Toggle2`, from `e0c7566`) and `runtime_toggle_frame_cap` (a frame
cap in FPS, `5ead842`). A set bit switches that optimisation **off**; `Disabled(bit)` is a relaxed load per use.
Each change is logged ("Runtime: disabled optimizations mask=...", "second word=...", "toggle file frame cap=...").
The launcher always sets the variable (`<user>/frame-dumps/toggles.txt`); `toggle-ab.ps1` and `deck.sh ab` /
`steps` alternate states in one launch, and the logs are split by state afterwards.

## Invariants

- A bit can only switch a path off: a path whose start-time switch was off (or whose state is read once into a
  `static const`) cannot be switched on, and some bits only act together with an opt-in variable (e.g. 262144
  with `GOW3_ASYNC_FENCES=1`). A new toggle must be read at use, not cached at start.
- Missing words read as 0; a missing or unreadable file reads as all 0 (every optimisation on, no cap). Every writer
  must write all the words it wants kept: `toggle-ab.ps1` writes "N 0" / "0 N", dropping the other word and the cap.
- Bit 31 of the first word has two meanings: `Gow3Toggle::NarrowImageStages` and Windows' `POOL_KEEP_VIEWS`
  (M1). Writing 2147483648 switches both off. Bits 20-29 are raw motion / upscaler debug toggles. A new bit must
  be checked against both headers (`gow3_toggles.h`, the `#define`s in `src/*.c`).
- The overlay's `TestLabel` reads the first word only: any non-zero first word shows "TEST" (A-D for four fixed
  sets) on screen; second-word A/Bs show no label.

## Interactions

- deck-test-tooling: added the third word and `deck.sh steps`; deck_summary.py labels windows by the log lines.
- M1, narrow-image-stages: share bit 31 (above).
- bulk-descriptor-copy, sampler-memo, ring-reuse: the same-launch A/Bs N-30 to N-32 through the second word.
- perf-patches-ini: the user-facing switches; some match bits (perf_texture_memo: bit 45, perf_gpu_data_no_waits:
  bits 60 and 41), where the bit is the stronger "off" (study-live-switches.md).

## Evidence

- N-30 (bulk copy, second word bit 1): descriptors 0.217 vs 0.337 us per draw median. N-31 (sampler memo, bit 2):
  0.041 vs 0.126 us. N-32 (ring reuse, bit 4): 1% low ~188 on vs ~179 off. Launch 14: the cap sweep through the third
  word. The mechanism is live-verified [V] in study-live-switches.md.

## History and pitfalls

- All 64 bits of the first word are taken; new switches go in the second word (value 64 there is reserved for the
  particle-sim skip, impl-draw-reduction.md).
- Frames within ~1 s of a switch mix two states: the split tools skip them (`toggle_ab_sections.py`,
  `flip_toggle_ab.py`, deck_summary's "mixed" windows).
- On the Deck `deck.sh` writes `~/.local/share/medievil-pc/toggles.txt`: `GOW3_TOGGLE_FILE` must name that file
  there, or the A/B writes go nowhere and every window shows one state.
