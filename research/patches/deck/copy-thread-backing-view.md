---
id: copy-thread-backing-view
title: Linux: copy threads read guest memory through the backing view (crash in CopySparseMemory)
area: deck
status: active
platforms: linux
default_windows: n/a
default_deck: on
origin: spacepepperoni 97f0ac9
class: G
commits: 8e7bb25, a4fc572
files: gpu/shim/gow3gpu.cpp, src/runtime_memory.c
symbols: CopySparseSerial, CopySparseMemory, runtime_memory_read_backing, runtime_memory_region, backing_base
switches:
tests: Native run 7, Native run 8
docs: plan-linux-deck.md#native-run-7-gate-2-stats-2026-11-03-appimage-3efee7a-gow3_frame_stats1-by-file-log-datadeck-native-7-crashlog, plan-linux-deck.md#native-run-8-gate-2-in-the-graveyard-2026-11-03-appimage-aec1592-67-log-datadeck-native-8log
depends_on:
related: A1, pool-kept-pages, D4, lockfree-region-reads, linux-build-lab
verified: 2026-11-07
---
## What it does

`CopySparseSerial` (behind `MemoryManager::CopySparseMemory`: constant ring copies, stream copies and uploads, split
across the copy threads for large sizes) walks the range by region. On Linux a mapped region is now read with
`runtime_memory_read_backing` (the memfd's second view `backing_base`, under the region table's read lock) instead
of `memcpy` from the guest address. Linux unmaps a pool page on every decommit (Windows keeps the view), and UE4
recommits pool pages several times a frame, so a copy thread could read an unmapped page.

## Invariants

- On Linux no GPU-side copy may read a mapped guest range through its guest address without holding the table lock
  (or A1's generation check): a pool decommit can replace its pages at any time (since D4 one `mmap` of a
  `PROT_NONE` reservation outside the lock, `pool_decommit_unlocked`; `munmap` + `mmap` before) and the read is a
  SIGSEGV in libc `memcpy`. New copy paths must use `runtime_memory_read_backing` / `runtime_memory_copy_backing` too.
- D4's order makes the locked read safe: a decommit enters the range as `KIND_RESERVED` under the write lock before
  it changes the host mapping, and a commit maps first and enters the table after, so "mapped" seen under the read
  lock means the pages are there. A change to that order breaks this card.
- `runtime_memory_read_backing` copies only from non-reserved table entries (`vmas[i].kind != KIND_RESERVED`) under
  `read_lock()`, and returns 0 if any part is reserved or absent; a decommitted pool range is `KIND_RESERVED`.
- `backing_base` must stay mapped for the process's life over the whole memfd.
- Unmapped ranges read as zero (`memset`), as before.

## Interactions

- `A1` (`GOW3_BACKING_COPY=1`, Deck preset) puts a lock-free path in front: `runtime_memory_copy_backing` copies
  without the lock and checks `table_generation`; on 0 it falls through to this locked loop. So on the Deck this
  card is A1's fallback; on other Linux setups it is the only path.
- `pool-kept-pages`: what the backing view holds for a recommitted page (its own bytes, or zeroed foreign pages)
  is decided there; this card only makes the read safe.
- `D4` / `lockfree-region-reads`: the region lookup before the read (`runtime_memory_region`: the per-thread cache,
  then the optimistic search) usually takes no lock, which is what leaves the window below.

## Evidence

- Native run 7 (AppImage `3efee7a`): "Host fault (signal 11) in libc memcpy, address 0x2005500000" from
  `Core::MemoryManager::CopySparseMemory` on a copy thread, after the main menu (`data/deck-native-7-crash.log`).
- Native run 8 (AppImage `aec1592`, with `8e7bb25`): no crash; graveyard 64-67 FPS. No crash of this kind is
  recorded since. Not a visual fix; nothing to verify in game beyond "no crash".

## History and pitfalls

- **Fixed `a4fc572` (2026-11-07, LANDMINES item 10):** the lock-free `runtime_memory_region` could say "mapped" and a
  decommit land before the locked `runtime_memory_read_backing`; the fall-through `memcpy` from the guest address
  (PROT_NONE) was the signal 11 of native runs 5 and 7 and nav-1 (libc memcpy in `CopySparseMemory` at a 1 MiB
  boundary). Linux now never reads through the guest mapping: one more lookup, then zeros.
- Same crash and fix as spacepepperoni/medievil_ps4_Linux `97f0ac9` (GPL-2.0).
- Residual window (confirmed in the code 2026-11-07, not seen in a run): `runtime_memory_region` may say "mapped"
  without the lock; if the range is decommitted (or split) before `runtime_memory_read_backing` takes the lock, that
  returns 0 and `CopySparseSerial` falls through to `std::memcpy` from the guest address, now `PROT_NONE`: the
  original crash. On Linux the `else` after a failed backing read should zero-fill (or retry the lookup), not
  `memcpy`. With A1 on, the window is reached only when A1 also returned 0.
- The same commit corrected the frame stats: "write faults/s" counts pages marked CPU-written, not signals (~1 real
  fault a frame on the Deck).
