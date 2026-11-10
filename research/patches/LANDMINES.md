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
   **Confirmed in code and logs 2026-11-07; fix built `adfeb67` (A3 restart-only; live A1 + A2 = 3145728); run on the Deck: the cuts redo had draw preparation in all 287 windows (plan-linux-deck.md "Cuts redo").** (code + logs; the uncapped table had no draw preparation in any state). Caveats added to
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
   on Deck plan L1 / audit item 1. **Confirmed in code 2026-11-07:** with a cap or Reflex each pass ends waiting for
   `tick_deadline` (now + one tick, set after the precise wait), so the next `PreciseWait` never sleeps or spins; the
   end wait is a plain `sleep_until` (Reflex, standard) or a 100 us spin (precise limiter). The ~0.65-core cost was
   computed, not measured. Harmless: Windows defaults (Reflex) do not pay it, the Deck ini has it off.
6. **The Deck "write faults" are mostly unmaps, not fault windows**: `ExtendWriteFault` never counts; whole
   decommitted ranges marked CPU-modified do (plan-linux-deck.md native run 7's correction is wrong by the code).
   **Checked 2026-11-07: half right.** `ExtendWriteFault` does not count (doc clause corrected). The counter is every
   protected page marked CPU-modified by any cause: real faults (~1 a frame by the fault map), guest unmaps via
   `UnmapMemory` -> `InvalidateMemory` (not the "unmap discard" path, 0/s on the Deck), file reads and other
   invalidations. Which dominates is unmeasured; a per-cause counter would tell, if the number matters again.
7. **E1's evidence is W2's**: "T-E1 ordered tasks 0.52 -> 0.23" in fix-inventory.md is E1+W2 against T-S3; T-E1
   toggled only W2. No E1-only number exists ([E1](stage-b/e1.md)). **Checked 2026-11-07:** right, but E1 is what
   lets W2 pay off (without E1, W2 moved the wait into the end-of-shader fence, T-W2); the gain belongs to the pair.
   fix-inventory.md's row reworded; keeping both stands.
8. **The rescore's 63% recorder busy on the Deck** predates recorder lean; the A2 / C13 estimates built on it are
   out of date. **Checked 2026-11-07:** the recorder is at 41% of a core uncapped (cuts redo), 28% at a 60 cap. Stale,
   but it strengthens both conclusions (C13 parallel recording and A2 small copies off the recorder gain even less);
   note added to study-deck-rescore-rejected.md.

## Latent bugs (code reading)

9. **Windows half of the pool leak** ([pool-kept-pages](memory/pool-kept-pages.md), [M1](memory/m1.md)):
   `pool_take_locked`'s second loop and `pool_take_at_locked` on a stale kept view hand kept pages to another address
   un-zeroed. With decommit-keep-access the old view also stays writable onto pages someone else now owns. Fix
   (zero there) belongs on `lab` with its regression pass.
   **Fix built `8d4607e` on deck-lab (2026-11-07), behind `GOW3_POOL_ZERO_FOREIGN=1`, off by default (`49f8fd7`), not
   run.** Not certain Windows needs it: confirmed in code only; T-M1b's 52 kept-view unmaps in ~10 min suggest free
   pages ran out then (inferred). Next: a normal PC session with frame stats reads "handed over with their bytes";
   if it is above 0, an A/B with the switch on, judged on lows and hitches.
   **Found on Windows 2026-11-09** (the user, Start-menu build `7c4a5cf`, 01.02): purple reflections in the Hall of
   Heroes and the Hilltop Mausoleum's wrong lighting, both on the PC; no setting changed it (DLSS, Reflections,
   quality level). **Fix built: on by default since then** (`GOW3_POOL_ZERO_FOREIGN=0` turns it off).
   Runs `pool-zero-0102` (~16 level loads, Mausoleum correct) and `hall-heroes-0102` (new game to the Hall of Heroes,
   no glitch): **0 pages zeroed or handed over in both**, so the leak path did not run there; neither is evidence for
   or against the fix. Both were `run-game.ps1` runs **without the launcher's 8 Deck switches**, which every
   Start-menu session on the PC runs ("Windows: untested": early_frame_eop, decode_pacing, drain_hybrid, texset_wide,
   region_index, pending_filter, reg_hash_paired, particle_flip_guard); the glitch was seen only in such a session,
   after many hours on earlier builds without it. Non-deterministic. The user (2026-11-09): keep the launcher as it
   was (switches on) with the zeroing fix on, play on and report a recurrence.
   **Recurred 2026-11-09 ~22:10** (the user, launcher run with the 8 Deck switches on, zeroing fix on, 01.02; also
   seen on 01.00 the same evening, walls fully white there): Hall of Heroes anteroom lit by a teal light through its
   walls (round falloff, pillar shadows), on some loads only. Reloading via the debug menu's level select brings it
   and clears it within a few tries. Screenshots `research-data/hall-heroes-glitch-20261109/` (untracked). The
   zeroing counters were not printed (no `GOW3_FRAME_STATS`), so the run says nothing about the leak path.
   Run `hall-glitch-framestats-0102` (broken load seen): **0 pages zeroed or handed over in all 50 windows**, so
   not the pool leak. RenderDoc captures (frame 13615 broken, 30045 clean; `research-data/hall-heroes-glitch-20261109/`):
   the teal is added by the reflection environment pass (`fs_0xd3caef1f45107cdd`, eid 4897); the capture
   cubemap array (70 slots) and the capture data (positions, radii, slots) are identical in both. The
   difference: the reflection constants' float at +16, read as `SkyLightCubemapBrightness` [inferred from
   UE4 4.21's layout], is **0.0 broken and 65535.0 clean**. UE4 divides reflections by an average brightness
   blended from it (up to a clamp), so 0 floods the walls (teal here, white on 01.00). That value comes from a
   one-time GPU readback at level load; in stale-readback mode a first copy's read sees bytes no copy wrote
   (lineage-deferred-readbacks). Suspect: readback-first-wait / tiny-target-readback territory, on Windows.
   **Fix built, verified by the user (2026-11-09):** the readback log showed the 1x1 sky brightness read as a
   first copy at a new address every load; `GOW3_READBACK_FIRST_WAIT` (readback-first-wait) on by default.
10. **Linux crash window in `CopySparseSerial`** ([copy-thread-backing-view](deck/copy-thread-backing-view.md)): if
    `runtime_memory_read_backing` returns 0 after a lock-free lookup said "mapped", the code memcpys from a PROT_NONE
    guest address: the native run 7 SIGSEGV. The Linux branch should zero-fill or retry.
    **Confirmed in logs, fix built `a4fc572` (2026-11-07), not run:** all three Deck crash logs (native runs 5 and 7, nav-1) died there, and so did the cuts redo (2026-11-07 21:14,
    `data/deck-cutsredo-crash.log`: `CopySparseSerial` from `BufferCache::UploadCopies` on a copy-pool thread, resolved
    with nm against the AppImage that ran).
11. **Texture-set memo ordering** ([texset-region-stamps](stage-b/texset-region-stamps.md)): `RegisterImage` /
    `UnregisterImage` raise `registry_generation` before `BumpRegions`, and the lookup reads the generation first. A
    registration beside a lookup can memo a set with stale stamps. Swapping the two lines removes it.
    **Fix built `c44a640` (2026-11-07), not run:** stamps bumped first.
12. **"Apply and restart" keeps the old perf switches** ([perf-patches-ini](launcher/perf-patches-ini.md)): the
    restart inherits the environment and pops only `GOW3_DERIVED`. A restart-only patch turned back on in the menu
    stays off; the live particle pass reverts to off; likely also the old VideoOut size after an output change.
    **Fix built `a39d538` (2026-11-07), not run:** every variable run.py adds is derived again.
13. **`GOW3_ASYNC_FENCES` is unsafe** ([async-fences](stage-b/async-fences.md)): an async EOP label can reach the
    guest before the recorder's small copies and deferred E1 / W3 labels.
14. **Deck Optimized is not platform-gated** ([deck-preset](deck/deck-preset.md)): chosen on Windows it turns on
    R1 and the other Deck switches there, untested. Changing quality in the game's menu clears the preset but leaves
    `deck_switches` on.
    **Fix built `af284f9` (2026-11-07), not run:** the switches one by one, Linux-only ones greyed out on Windows,
    the applied set in the log (the user's design).
15. **Two defaults for `stale_readback`** ([lineage-deferred-readbacks](caches/lineage-deferred-readbacks.md)): run.py
    and the Deck preset use 1, the launcher's `INI_DEFAULTS` 0. A fresh launcher install runs precise readbacks, a
    mode the user's ini never tests.
    **Fix built `8600b28` (2026-11-07), not run:** the launcher default is now 1, as run.py and the Deck preset (the
    user's call: upstream's reason for 0 is likely patched since; not checked). Inis with the key are unchanged.
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
