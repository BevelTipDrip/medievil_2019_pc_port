---
id: S1-streaming
title: r.Streaming.AmortizeCPUToGPUCopy to split the panning bursts (streaming test S1)
area: ue4
status: rejected
platforms: all
default_windows: off
default_deck: off
origin: ours
class: M
commits:
files:
symbols:
switches:
tests:
docs: plan-frame-pacing.md#panning-stall-anatomy-2026-10-19-pan-1etl-no-launch, fix-inventory.md#10-tried-and-dropped
depends_on:
related: 4k-memory, ue4-cvars-tool, I1
---
## What it does

A streaming test, not a patch. On 2026-10-19 one launch set `r.Streaming.AmortizeCPUToGPUCopy=1;
r.Streaming.MaxNumTexturesToStreamPerFrame=4` through `GOW3_UE4_CVARS` to spread texture streaming over frames and
split the 4K panning stalls. ID note: this is "S1" / "T-S1" of plan-frame-pacing.md on 2026-10-19. It is not the
stage B T-S1 of plan-stage-b-split.md (`GOW3_SECTIONS`), and not the Deck's S1-v3-build.

## Invariants

- Nothing in the build. Do not add the two cvars back to `ue4_override` as a stall fix: the bursts are whole asset
  groups loaded by visibility / region streaming, not one texture pool's per-frame upload.
- Some PS4 device profiles in pakchunk0 already set `r.Streaming.AmortizeCPUToGPUCopy=1` /
  `MaxNumTexturesToStreamPerFrame=1`, so a pak value competes with the profile's (SetBy priority, see 4k-memory).

## Interactions

- 4k-memory (S2, the same night): the larger pool fixed small pans, not large ones. That run also showed the same
  asset groups every time.
- ue4-cvars-tool: the test path.

## Evidence

- S1 (2026-10-19 01:27-01:31, launched by Claude with the user's go, `data/t-s1-amortize.log`). The user: "somewhat
  improved, but I can pretty consistently create a stutter in the same spot". Stall frames unchanged in kind:
  panning bursts of 661-1,009 ms with 197-282 new images and 318-395 MB of uploads in one frame.
- Later summary (plan-frame-pacing): "S1 (`AmortizeCPUToGPUCopy`, 4 textures a frame) little effect".

## History and pitfalls

- Tried: amortized CPU-to-GPU streaming copies, 4 textures a frame. Measured: the bursts did not split. Dropped
  because they are groups of textures evicted when panning away (T-C2: up to 362 images / 709 MiB freed by unmaps per
  5 s) and reloaded at new addresses. UE4 throttles none of that.
- The 3,283 ms "kind B" frame in that log is the Load Game dialog, not a gameplay hitch.
- Worth retrying only if the burst turns out to be texture-pool uploads rather than level / region streaming. The
  later work on the panning stall went to the port side (wait chains, image creation, uploads).
