---
id: cacheline-groups
title: Deck #6: cache-line groups for draw pipe / scheduler fields written and read by different threads
area: stage-b
status: active
platforms: all
default_windows: on
default_deck: on
origin: ours
class: G
commits: 8dd5881e74
files: gpu/shim/gow3_cacheline.h, gpu/shadps4/video_core/renderer_vulkan/vk_scheduler.h, gpu/shadps4/video_core/renderer_vulkan/vk_draw_pipe.h, gpu/CMakeLists.txt, build.sh
symbols: GOW3_CACHE_LINE, GOW3_CACHELINE_PADDING, pending_polls, priority_pending_ops, record_chunk, wake_deadline_ns, recorder_mutex, direct_mode, host_copies_done, host_copy_sources_mutex, busy_cycles, handler, head
switches:
tests:
docs: impl-stage-b-bundle-2.md#6-cache-line-groups, impl-stage-b-bundle-2.md#build-and-test, plan-linux-deck.md#built-and-researched-while-the-deck-was-away-2026-11-04-nothing-run-yet
depends_on: lineage-draw-pipe
conflicts_with:
supersedes:
related: recorder-lean, stage-b-spin, stage-b-core
checked: 2026-11-07
---
## What it does

`GOW3_CACHE_LINE` (`alignas(64)`, gow3_cacheline.h) starts each group of fields that one thread writes often and
another reads often, so a write no longer takes the line from the reader's L1. No field moved. In `Scheduler`:
`pending_polls` / `num_pending_ops` (read every draw) apart from the pending-ops mutexes and `priority_pending_ops`;
the recording thread's `record_chunk` / `chunk_lock` / `recording_owner`; `wake_deadline_ns` (with `full_chunks`); the recorder's
`recorder_mutex` group (queue, `recorder_busy`, `queued_chunks`, `recorder_running`, `recorder_sleeping`);
`direct_mode` / `host_copies_issued`; `host_copies_done`; `host_copy_sources_mutex`. In `DrawPipe`: `busy_cycles`
(stage B per packet) apart from `packets` (stage A per Commit); `handler` (with `idle`, `context`, `ring`) and
`head` each start a line (`published` was already `alignas(64)`).
Build option, not a runtime switch: `GOW3_CACHELINE_PADDING=OFF` (CMake / build.sh) gives the old layout exactly.

## Invariants

- A new field that one thread writes on a hot path must go into the group of the thread that writes it, or start a
  group of its own; placing it in another thread's group brings the false sharing back (no compile error shows it).
- The marked fields only change alignment; no code may depend on these classes' size or offsets (no memcpy of a
  `Scheduler` / `DrawPipe`, no static_assert on their size; none found). The alignment propagates to the owners
  (`Presenter` holds `draw_scheduler` by value, `DrawPipe` is a `make_unique`): heap allocation relies on C++17
  aligned `new`.
- `GOW3_CACHELINE_PADDING` must be the same for every translation unit (it is a compile definition set in
  gpu/CMakeLists.txt, only when OFF; the header defaults to padded); a mix would give two layouts of one class (ODR
  violation). Every target including these headers is in gpu/CMakeLists.txt today (the tests too); a target added
  elsewhere would silently be padded.

## Interactions

- recorder-lean added `wake_deadline_ns`, stage-b-spin added `recorder_running`; both are placed by this layout.
- Bigger in effect where cores sit far apart or the fabric clock is low (Zen 2 on the Deck); stage-b-core changes
  which cores share lines, not which fields do.

## Evidence

- Expected (impl doc): 0.05-0.2 us a draw. Built Windows (LTO) and Linux; a `-nopad` AppImage was built for the
  A/B (`MediEvil-x86_64-nopad.AppImage`, from `0c57f23`, the docs commit right after `8dd5881`); the Deck test order
  later asks to rebuild it from the current commit first, since the default AppImage moved on.
- No result recorded: the padding vs none launch stayed on the to-do list ("Test every built-but-untested item ...
  padding"). Not measured, not verified in game; kept because it is layout only.

## History and pitfalls

- Default ON in both builds since `8dd5881`. The WSL `out/` tree was once configured with padding OFF for the
  `-nopad` image; `build.sh` passes the option every time, so a default build reconfigures it ON.
- To measure: one launch each with the default and the `-nopad` AppImage built from the same current commit, same
  spot and toggles; compare stage B us per draw. One launch each is within noise for a change this size.
