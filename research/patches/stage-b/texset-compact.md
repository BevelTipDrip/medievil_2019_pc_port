---
id: texset-compact
title: Texture-set memo in a compact layout (64-byte index per slot, images apart)
area: stage-b
status: active
platforms: all
default_windows: on
default_deck: on
origin: ours
class: G
commits: 1e4ca3d, 366d634
files: gpu/shim/gow3_texset_table.h, gpu/shim/gow3_texset_table.cpp, gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp, gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.h, gpu/shim/gow3_toggles.h, tests/test_texset_table.cpp
symbols: TextureSetHead, TextureSetHotRow, TextureSetColdRow, CompactTextureSets, LegacyTextureSets, BindTexturesFromSetIn, KeepTextureSet, TextureSetStamp, Choose, WaysOf, TableMemory, HugePageMode, TextureSetLegacy
switches: toggle2:TextureSetLegacy
tests:
docs: impl-texset-index.md#2-the-compact-texture-set-memo, plan-linux-deck.md#spin-compact-texture-memo-and-the-task-split-2026-11-04, study-deck-patch-audit.md#2-scores
depends_on: lineage-texture-set-memo, texset-region-stamps
related: T2
checked: 2026-11-07
---
## What it does

The memo's 32,768 slots no longer sit in one `std::array<TextureSet>` of ~1.55 KiB a slot (52.2 MB). There are three
arrays: a 64-byte `TextureSetHead` index (key, last use, generation, stamp, scene generation, stage, count), a
`TextureSetHotRow` of 16 x 48-byte entries a hit reads, and a `TextureSetColdRow` of guest ranges read only for
region stamps and the fill. In all, 44.0 MB in one 2 MiB-aligned `TableMemory`, written at construction. Linux
asks for transparent huge pages (`MADV_HUGEPAGE`, then `MADV_COLLAPSE`). One template, `BindTexturesFromSetIn<Table>`,
serves both layouts through small adapters (`CompactTextureSets`, `LegacyTextureSets`). Second toggle word 2048
switches live to the old table, which is made on first use. deck-lab only.

## Invariants

- Both layouts make the same decisions: the slot choice is `Gow3TexSet::Choose` + `WaysOf` for both; the fill
  stores the same values; the stamp is the same sum (`TextureSetStamp(u32)` over the cold row). `texset-table-test`
  checks the slot choice against the old code over 2,000,000 lookups. A change to one layout's lookup or fill that
  is not made to the other breaks the A/B and the "same hit rate" check.
- Empty index entries keep the old defaults (`generation = ~0`, key 0 = empty). Key 0 marks a dropped set (`key |= 1`
  at the lookup).
- Each layout keeps its own sets while the other is in use. Coming back finds older sets, which the generation /
  stamp / image checks validate; those checks must stay in the shared template.
- The legacy table is made on stage B on its first use (52.2 MB, a one-off stall estimated at ~20-80 ms on the Deck
  [inferred]). Do not use bit 2048 during a measurement window.

## Interactions

- texset-region-stamps: the cold row exists only for its stamps. Without stamps it is still filled.
- T2's wide sets (17-32 images) keep the old layout in their own table (`WideTextureSets`).
- The memo itself is lineage-texture-set-memo; first word 2^45 still switches it off entirely.

## Evidence

- Deck (`deck-texset-1.log`, live A/B `0 0` vs `0 2048`, 30 s x 5): 76.6 vs 76.5 FPS, texset 1.051 vs 1.046 ms, hit
  rates identical (98.25 / 98.26%), 44 MB in 21 huge pages. Neutral: the cost of texture binding is not
  memory-bound. Kept for 8 MB less. Not verified in game beyond that A/B (no visual change expected: same decisions).
- The patch audit had suspected the 52 MB layout, with an estimated 0.15-0.5 ms of stage B a heavy Deck frame
  [inferred]; the A/B did not show it.

## History and pitfalls

- A fingerprint index in front of the table was rejected on analysis: the slot is computed from the key, so an
  index adds a dependent read and saves none (impl-texset-index.md).
- `366d634`: a braced list in a range-for (`HugePageMode`) did not build on Linux; it is now a static array.
- On the PC (96 MB L3) no change was expected or measured.
