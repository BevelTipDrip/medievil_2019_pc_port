# Landmines found while building the patch map (2026-11-07)

The card checkers found these by reading the code against the cards and the docs. **None of it has been run or
measured yet:** treat each item as a lead to confirm before acting on it. The cards named have the detail. Ordered by
what they could cost: wrong past conclusions first, then latent bugs, then traps for future changes.

## Past measurements that may be wrong

1. **A3 flipped live disables draw preparation for the rest of the launch** ([A3](stage-a/a3.md)). Stage A's and
   the prep workers' register checksums never match again after a live flip of second-word bit 4194304. The workers
   are reseeded only at the first submission or a rebase. In `data/deck-stagea-1.log` "prepared pipelines used" drops
   from ~90% to 0 at line 2646 and stays there. `deck-launch15.log` does the same from lines 9198 and 20691. So round
   1 launch 7, the 1,200 MHz A1-A3 pair and Launch 15's later live A/Bs (drain hybrid, R1/R2/T1/T2, signal sleep, the
   uncapped table) ran mostly without draw preparation in both states. The comment above `PairedRegisterHash` and
   impl-stage-a-decode.md:197 claim it recovers; the code says it does not. Fix: force a rebase when the fold
   changes, or make A3 restart-only.
   **Confirmed 2026-11-07; fixed in `adfeb67`; cut table redone with draw preparation alive (plan-linux-deck.md "Cuts redo") (A3 restart-only; live A1 + A2 = 3145728).** (code + logs; the uncapped table had no draw preparation in any state). Caveats added to
   plan-linux-deck.md (Round 1, Launch 15) and impl-stage-a-decode.md.
2. **The PC toggle scripts and a launcher-started game watch different files** ([lineage-toggle-file](tooling/lineage-toggle-file.md)).
   `toggle-ab.ps1` / `toggle-groups.ps1` only reach games started by `run-game.ps1`. In a launcher-started game a
   "no difference" A/B can mean nothing changed. **Checked 2026-11-07: no past result affected** (T-0102-2 logged
   every mask it got; N-30 to N-32 show per-state differences). A trap for future runs only.
3. **Toggle bit 31 is two switches on Windows** ([narrow-image-stages](stage-b/narrow-image-stages.md), [M1](memory/m1.md)):
   narrow image stages and M1's kept pool views. Every Windows bit-31 A/B (N-5 included) also flipped M1. On Linux
   it is only narrow image stages. **Confirmed in code** (`POOL_KEEP_VIEWS` = `0x80000000`). Effect on past results
   small: N-4 / N-5 judged GPU time (and N-16 already corrected it to -0.8%); narrow stages stays on either way.
4. **A two-word toggle write drops the live frame cap** ([lineage-toggle-file](tooling/lineage-toggle-file.md)):
   any two-word writer (`toggle-ab.ps1`, a two-word `deck.sh ab` / `steps` state) sends the limit back to the menu's.
   A cap at or above the vblank rate is ignored, so the cap word works only with the uncapped (480 Hz tick) presets.
   **By design** (the third word overrides only while set); Launch 15 used three words in every capped state. Trap only.
5. **The precise tick likely does nothing under the Windows defaults** ([precise-tick](present/precise-tick.md)):
   with a cap or Reflex the tick is timed by the condition-variable timeout and `PreciseWait` always finds its slot
   passed. Its spin (the patch audit's ~0.65 of a core) is live only uncapped without Reflex. Measure before acting
   on Deck plan L1 / audit item 1.
6. **The Deck "write faults" are mostly unmaps, not fault windows**: `ExtendWriteFault` never counts; whole
   decommitted ranges marked CPU-modified do (plan-linux-deck.md native run 7's correction is wrong by the code).
7. **E1's evidence is W2's**: "T-E1 ordered tasks 0.52 -> 0.23" in fix-inventory.md is E1+W2 against T-S3; T-E1
   toggled only W2. No E1-only number exists ([E1](stage-b/e1.md)).
8. **The rescore's 63% recorder busy on the Deck** predates recorder lean; the A2 / C13 estimates built on it are
   out of date.

## Latent bugs (code reading)

9. **Windows half of the pool leak** ([pool-kept-pages](memory/pool-kept-pages.md), [M1](memory/m1.md)):
   `pool_take_locked`'s second loop and `pool_take_at_locked` on a stale kept view hand kept pages to another address
   un-zeroed. With decommit-keep-access the old view also stays writable onto pages someone else now owns. Fix
   (zero there) belongs on `lab` with its regression pass.
10. **Linux crash window in `CopySparseSerial`** ([copy-thread-backing-view](deck/copy-thread-backing-view.md)): if
    `runtime_memory_read_backing` returns 0 after a lock-free lookup said "mapped", the code memcpys from a PROT_NONE
    guest address: the native run 7 SIGSEGV. The Linux branch should zero-fill or retry.
11. **Texture-set memo ordering** ([texset-region-stamps](stage-b/texset-region-stamps.md)): `RegisterImage` /
    `UnregisterImage` raise `registry_generation` before `BumpRegions`, and the lookup reads the generation first. A
    registration beside a lookup can memo a set with stale stamps. Swapping the two lines removes it.
12. **"Apply and restart" keeps the old perf switches** ([perf-patches-ini](launcher/perf-patches-ini.md)): the
    restart inherits the environment and pops only `GOW3_DERIVED`. A restart-only patch turned back on in the menu
    stays off; the live particle pass reverts to off; likely also the old VideoOut size after an output change.
13. **`GOW3_ASYNC_FENCES` is unsafe** ([async-fences](stage-b/async-fences.md)): an async EOP label can reach the
    guest before the recorder's small copies and deferred E1 / W3 labels.
14. **Deck Optimized is not platform-gated** ([deck-preset](deck/deck-preset.md)): chosen on Windows it turns on
    R1 and the other Deck switches there, untested. Changing quality in the game's menu clears the preset but leaves
    `deck_switches` on.
15. **Two defaults for `stale_readback`** ([lineage-deferred-readbacks](caches/lineage-deferred-readbacks.md)): run.py
    and the Deck preset use 1, the launcher's `INI_DEFAULTS` 0. A fresh launcher install runs precise readbacks, a
    mode the user's ini never tests.
16. **Newest-frame + the default inline present** can run `Presenter::Present` on two threads at once;
    one-clock + `GOW3_NEWEST_FRAME` gives two clocks again; one-clock + `GOW3_VBLANK_SYNC` never flips
    ([newest-frame](present/newest-frame.md), [one-clock](present/one-clock.md)). Never run.
17. **Thread nice**: if the live bit is already set when stage B starts, stage B keeps nice -10; live off never
    restores the vblank thread's timer slack ([thread-nice](deck/thread-nice.md)).

## Hidden dependencies (traps for the next change)

18. `perf_lockfree_memory=0` also turns off A1 and R1-region-index (they need the optimistic clamp and the region
    buckets) ([lockfree-region-reads](memory/lockfree-region-reads.md)).
19. Fault-page downloads without a wait need D3's readback queue; `perf_gpu_data_no_waits=0` turns D3 off and leaves
    them on, so every finished-page download takes a full GPU wait ([fault-page-download](caches/fault-page-download.md)).
20. HZB remap needs `GOW3_DEFERRED_READBACK=1`, otherwise it falls back to the game's own results every frame
    ([hzb-remap](ue4/hzb-remap.md)).
21. The single particle pass is off only because run.py exports `GOW3_PARTICLE_FIXED_STEP=1`; any launch that skips
    run.py gets it on ([particle-single-fixed](ue4/particle-single-fixed.md)).
22. Finer occlusion culling's auto rule (scene up to 1280x800) adds the patch even when unticked and with
    `perf_hzb_occlusion=0`; only `GOW3_HZB_FULL_SIZE=0` keeps it off ([finer-occlusion-culling](ue4/finer-occlusion-culling.md)).
23. U2's cost stays with the live bit off; only `GOW3_ARENA_HOST_VISIBLE=0` at start removes it ([U2](caches/u2.md)).
24. Windows' unlocked GPU copies rely on decommit-keep-access, not M1: with `perf_cheap_frees=0` a racing copy faults
    as before M1 ([decommit-keep-access](memory/decommit-keep-access.md)).
25. Linux `host_map` never bumps the L2 buckets; any new Linux mmap-over-a-mapping path must call `region_changed`
    first, or L2, R1 and A1 break ([L2](memory/l2.md)).
26. `GOW3_FRAME_STATS` is parsed three ways: `=0` turns on the stage A/B printf lines, `=y` opens the driver block
    with counters reading 0.

## Test IDs that mean more than one run

T-M1 (three runs), T-L1 / T-L2, T-P1 to T-P5, T-H1, T-L5, T-W3; card IDs R1 (three patches) and R2 (two). Check
the date in [TESTS.md](TESTS.md) before trusting a lookup.
