---
id: rhi-pending-deletes-nofence
title: "RHI pending-delete check without the two mfences per NumRefs read (FRHIResource::FlushPendingDeletes)"
area: ue4
status: active
platforms: all
default_windows: on
default_deck: on
origin: ours
class: S
commits:
files: patches/MediEvil.xml, launcher/medievil_launcher.py, tests/test_medievil_patches.py, tests/test_patches.py, tests/test_launcher_controls.py
symbols: BUILTIN_PATCH_ROWS
switches:
tests:
docs: study-sv-hotspots.md, plan-work-queue.md (P18)
depends_on:
related: nav-purge-nofence
---
## What it does

UE 4.21's `FRHIResource::FlushPendingDeletes` (render thread) reads each pending resource's `NumRefs` through the PS4
build's AtomicRead, `mfence; mov eax, [rdi+8]; mfence`. The built-in eboot patch "RHI pending-delete check without
fences (CPU)" keeps the load and turns the fences into one 6-byte nop at three sites per version (01.02 image
offsets 0xf7c1d8 / 0xf7c348 / 0xf7c498, 01.00 0xf79558 / 0xf796c8 / 0xf79818; XML addresses +0x400000). On by
default since v2.5.0 (launcher: Performance, after the navigation purge); restart-only.

## Invariants

- The same safety argument as nav-purge-nofence: on x86 a load is not reordered with later loads or stores (the
  second fence orders nothing); the first keeps only this thread's earlier stores ahead of the load (the
  CurrentlyDeleting pointer, a debug aid no shipping code reads to decide about NumRefs). The resurrect branch keeps
  its full barrier (`lock or [rsp], 0`). The decision (delete or keep) is unchanged.
- Only these sites: each matched with the following `mov dword [rdi+0xc], 0; lock or` (MarkedForDelete); the same
  9 bytes occur elsewhere in other code.
- `Original` bytes checked per line (both real eboots: no mismatch, 2026-11-25).

## Evidence

- study-sv-hotspots.md: 0.03 ms a frame calm, 0.17-0.21 ms in the Sleeping Village fights (render thread, run 4).
- Built 2026-11-25; not run.

## History and pitfalls

- Found by the hot-spot study of P18 as the closest match to the navigation purge's shape.
