---
id: memory-chore
title: Memory area: tests and test build fixes with no behaviour of their own
area: memory
status: chore
platforms: linux
default_windows: n/a
default_deck: n/a
origin: ours
class: G
commits: bfbd4957e2, c7a6a405e8
files: tests/test_runtime.c, tests/test_sema.c
symbols: pool_pages, runtime_image_base, runtime_image_size
switches:
tests:
docs:
depends_on:
conflicts_with:
supersedes:
related: pool-kept-pages, game-update-0102, M1
verified: 2026-11-07
---
## What it does

- `bfbd495`: `runtime-test` and `sema-test` link again. Since the 01.02 hooks (`1e8150f`, game-update-0102) the
  runtime (`src/runtime_extra.c`, `src/runtime_waitsites.c`) calls `runtime_image_base` / `runtime_image_size`, which
  live in `src/probe.c`; the tests do not link probe.c, so `build.sh --test` failed at the link on Linux. Both tests
  now define them as "no image" (NULL, 0), as they already define `runtime_restart`.
- `c7a6a40`: `runtime-test --pool-pages` (`pool_pages`), pool-kept-pages' unit test. It expands a 64 KiB pool (4 pool
  pages: the whole pool), then checks that a range decommitted and committed at its address gets its own bytes back,
  and that another range taking those pages on a full pool reads zeros, both ways. With `GOW3_POOL_KEEP_PAGES=0` it
  only prints what the old behaviour does; with the default it asserts. With Linux's default unlocked pool maps it
  runs `pool_commit_unlocked` / `pool_decommit_unlocked`; the locked path only with toggle2:PoolMapsLocked (256).

## Invariants

- The stubs must return "no image": the UE4 hooks look for none and so stay inert in tests. If probe.c's image
  functions gain callers elsewhere in the runtime, the tests need those stubs too, or `build.sh --test` breaks again.
- `--pool-pages` assumes it owns the whole pool (one expand, 4 pages): case 2 only tests anything if no free page
  is left, so `pool_take_locked`'s second loop has to hand it the first range's kept pages. With another pool chunk
  holding free pages it passes without testing. Run it as its own process (its own flag), not inside the full run.
- Case 3 relies on `pool_take_kept_locked` rejecting the first range's stale `kept_table` entries: their pages now
  belong to the second range (`pool_owner` check), so the commit falls through to `pool_zero_foreign_locked`.

## History and pitfalls

- The tests build only on Linux (`build.sh --test`: CMake `gow3_runtime_executable` in the `elseif (GOW3_STATIC_GPU)`
  branch of `gpu/CMakeLists.txt`, or `$CC` in build.sh with static off; Windows CMake has no runtime-test). Built on
  Windows, case 2 would fail (read from the code, not run): M1's kept views mark decommitted pages `POOL_KEPT`
  (`pool_give_locked`), `pool_take_locked`'s second loop hands them to another address, and
  `pool_zero_foreign_locked` is Linux-only, so the second range would read 0xA5.
- The full `runtime-test` run stops earlier (posix_mutexes' destroy-while-locked check, comment in `main`), so
  `--pool-pages` and `--backing-copies` (A1) are only reached through their own flags.
