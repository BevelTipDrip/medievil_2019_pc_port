---
id: task-queue-fences
title: "Task queue reads without extra fences: UE4's lock-free FIFO Pop / Push keep one mfence per pass (experimental)"
area: ue4
status: experimental
platforms: all
default_windows: off
default_deck: off
origin: ours
class: M
commits:
files: patches/MediEvil.xml, launcher/medievil_launcher.py, tests/test_medievil_patches.py
symbols: BUILTIN_PATCH_ROWS
switches:
tests:
docs: study-sv-hotspots.md, plan-work-queue.md (P18)
depends_on:
related: nav-purge-nofence, rhi-pending-deletes-nofence
---
## What it does

UE 4.21's `TLockFreePointerListFIFOBase` (the task graph's queues) is a Michael-Scott queue. Pop reads Head, Tail,
the head's next link and Head again; Push reads Tail, its next link and Tail again; both publish only with
`lock cmpxchg`. The PS4 build's AtomicRead wraps every read in `mfence; load; mfence`: 8 fences a Pop pass, 6 a
Push pass, on every thread that uses a task queue. The built-in eboot patch "Task queue reads without extra fences
(CPU, experimental)" keeps the first fence of each pass and turns the others into `nop dword [rax]` (0f1f00): 7
per Pop copy, 5 per Push copy, 38 per version. The loops' exact bytes occur 4 times (Pop) and twice (Push) per
version (other instantiations of the template): all patched. Loop heads (image offsets) 01.02: Pop 0x6e7c22,
0x6f09e2, 0xa48ac2, 0xa48c52, Push 0x6ea2c2, 0x6efd82; 01.00: Pop 0x6e5062, 0x6ede22, 0xa45e42, 0xa45fd2, Push
0x6e7702, 0x6ed1c2. Off by default; launcher: Performance, after the RHI patch; restart-only.

## Invariants

- x86 (TSO): a load is never reordered with older loads, and the queue's own writes are locked instructions (full
  barriers), so the fences between the loop's reads order nothing. The kept first fence still orders whatever the
  caller stored before the queue operation (a Dekker-style flag-then-check in a caller keeps its StoreLoad barrier).
- Only these loops (found by their exact bytes, checked against both real eboots: no mismatch). The image has
  ~3,300 fence groups: do not generalise.
- Experimental: turn on by default only after runs with no hang, crash or missing work (tasks lost would show as
  stalls or objects not updating).

## Evidence

- study-sv-hotspots.md: the game thread's biggest rise in the Sleeping Village fights (task-graph queues 0.65 ->
  1.1-1.3 ms a frame; the FIFO fence groups 0.5-0.8 ms of the game thread, ~1.1 ms across threads).
- Built 2026-11-25; not run.

## History and pitfalls

- Planned as P18 (3) after the navigation purge and RHI pending-delete patches; the user asked for it built.
