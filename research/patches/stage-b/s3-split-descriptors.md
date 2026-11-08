---
id: S3-split-descriptors
title: S3 split descriptor sets: images + samplers in a cached set per memo entry, only buffers pushed (not built)
area: stage-b
status: rejected
platforms: all
default_windows: n/a
default_deck: n/a
origin: ours
class: G
commits:
files: gpu/shadps4/video_core/renderer_vulkan/vk_pipeline_common.cpp, gpu/shadps4/video_core/renderer_vulkan/vk_graphics_pipeline.cpp, gpu/shadps4/shader_recompiler/resource.h
symbols: BindResources, pushDescriptorSetKHR, uses_push_descriptors, buf_offsets
switches:
tests:
docs: study-stage-b-native.md#inside-stage-b-no-game-hooks, study-stage-b-native.md#4-ranking, study-stage-b-per-draw.md#4-ranked-cuts, study-deck-rescore-rejected.md#c-analysed-gated-or-ranked-never-built
depends_on: lineage-texture-set-memo
related: bulk-descriptor-copy, S1-delta-draw, sampler-memo, texset-compact
checked: 2026-11-07
---
## What it does

Not built. Today `Pipeline::BindResources` pushes every descriptor of a draw (buffers, images, samplers) with one
`pushDescriptorSetKHR` in set 0 when the layout fits the push limit (`uses_push_descriptors`), or writes a set
otherwise. S3: images and samplers go into a regular descriptor set cached per texture-set memo entry and bound with
one `bindDescriptorSets`; only buffers are pushed. Needs a two-set pipeline layout in the shader recompiler.

## Invariants

- The cached set must be valid exactly as long as the memo entry is: any registry change, rebind or layout change that
  invalidates the entry must retire its set, and a set may not be freed while a command buffer still uses it.
- Every recompiled pipeline layout (graphics and compute) would need the same two-set split, or a draw binds a set
  against an incompatible layout (validation error, wrong descriptors). Host passes keep their own layouts.
- Ring offsets cannot move into push constants instead: `PushData::buf_offsets` is `u8` (`resource.h`).
- Alternative named in the study (incremental pushes of only the changed bindings) needs the push semantics checked
  against the Vulkan spec and on RADV and NVIDIA first (per-draw study C8).

## Interactions

- lineage-texture-set-memo, texset-compact, sampler-memo: S3 keys its sets on the memo entry; the sampler memo
  already took part of the sampler cost.
- bulk-descriptor-copy (shipped): cut the per-write `RecordData` cost S3 was partly aimed at; the rescore scales
  what is left (0.86 us/draw descriptors on the Deck, 17.7 writes a push).
- S1-delta-draw: also reuses descriptors for repeated draws; overlaps (rescore: S3 ~ per-draw study C8).

## Evidence

- Native study (PC, 2026-10-20): descriptors 0.22 -> ~0.08 us; 0.1-0.14 us per packet, +4-6% heavy view; ranked 4,
  effort medium-high, risk medium ("layout change in the recompiler").
- Rescore C3 (Deck): **-0.4 to -0.7 ms** a frame (descriptors x3.7). Hindsight: open. Nothing built or measured;
  not verified in game.

## History and pitfalls

- Never started: the effort is in the recompiler's binding scheme, and the bulk copy took the cheap part first.
- To retry: build behind a toggle (fall back to the single pushed set), measure the "descriptors" section with
  `GOW3_SECTIONS=1` on the Deck, where stage B is the wall.
