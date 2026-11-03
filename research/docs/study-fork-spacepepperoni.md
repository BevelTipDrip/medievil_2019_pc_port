# Study: spacepepperoni's Linux / Steam Deck fork, and what `lab` should take from it

Status: study, 2026-11-03. Read only: nothing was built, launched, committed or pushed in either repo. Parent:
[START-HERE.md](START-HERE.md). Related: [plan-linux-deck.md](plan-linux-deck.md) (our Deck plan, Proton run 1),
[fix-inventory.md](fix-inventory.md), [study-glyphs.md](study-glyphs.md), `data/deck-proton-1.log`.

Repos read:
- Theirs: `C:\Users\Ty\MediEvil\forks\medievil_ps4_Linux`, `origin/main` = `4ae7198` (2026-11-03), 120 commits past
  upstream 1.3.8 (`424f3ea8`). The whole diff is 53 files, +3,797 / -424. The author's commit messages say "Linux fork",
  and many changes come "from the Uncharted Linux fork" (hashes such as `824a5c0` and `3f7be65` are that fork's).
- Ours: `lab` = `5d82812`.

Both repos are GPL-2.0-or-later, so code can be ported. The commit message must name their commit hash, and the
Uncharted hash where they give one.

Marks:
- **[code]**: read in the code (theirs at `4ae7198`, ours at `5d82812`).
- **[msg]**: from their commit message. Those are their measurements, on their Deck or on "pyro", which their
  messages call "the Deck's APU" (`d0884ba`, `433cd1a`).
- **[inferred]**: reasoning, not checked.

Nothing here is verified in game in the project's sense.

## Summary

- **Most of their Deck speed comes from settings and from running natively, not from one big GPU fix.** Their Deck
  mode does these things:
  - 30 FPS cap;
  - 720p scene inside a 1080p output;
  - Low preset with `r.TiledDeferredShading=0` (Hilltop Mausoleum 21-23 to 30 FPS [msg]);
  - preshadows off;
  - native AppImage instead of Proton.

  On the GPU thread they removed waits:
  - compute-queue drains and compute fences;
  - fence-time host-copy waits;
  - end-of-shader fences;
  - constant-ring polling;
  - a yield-spinning drain.

  In the runtime they shortened the region-table lock work on Linux. `lab` already has about half of the GPU-thread
  items in its own form (E1, W2, V3, R3, lock-free region reads, lean barriers (a), D3 + twins). The rest are small
  and portable. Section 2 lists both.
- **General fixes we lack, worth porting:**
  - stencil clears that write only the clearing draw's pixels (issue #40 flicker, Pumpkin Gorge shadows);
  - compute-queue drain and fence pipelining;
  - `DrawPipe::Drain` sleeping instead of yield-spinning;
  - display-buffer label waits that are already met no longer drain;
  - constant ring `Retire` reusing the known tick;
  - copy threads reading guest memory through the backing view (crash as a cinematic ended);
  - AvPlayer: one join at a time, wake before join, never 0 channels;
  - audio NaN guard;
  - skin cache and morph targets off (stretched shield);
  - the firefly jitter byte edit.
- **One warning that touches our defaults.** They tried keeping sampled depth in
  `DepthReadOnlyStencilAttachmentOptimal`, which is our lean-barriers part (b), on by default. It "hung the render
  thread" on their Deck-APU machine, and they reverted it the same morning (`64454ab` then `ec81d34`). Our
  `GOW3_DEPTH_SAMPLE_IN_PLACE=0` is the switch to test first if the Deck hangs or loses the device.
- **Their fork changes nothing in presentation** (swapchain, window, vblank thread). It runs natively, not under
  Proton, so it does not explain our Proton black screen. Its one relevant hint: its Deck mode avoids 1280x720 output
  on purpose (`2e0c6c2`). Our Proton run used 1280x720 output with our "Boot display size 1280x720" eboot patch, and
  that patch was never run at 720p on the PC (plan-stage-b-split.md:1009, "Not run yet: 720p"). Section 3 ranks the
  suspects.
- **Their button prompts are repainted at start from the player's own textures.** They erase the PS symbols and draw
  letters with a system font. No outside art, nothing shipped. The detile / retile code matches our study-glyphs.md
  rule exactly, and its cooking pipeline is reusable. Its coverage is smaller than our plan's (section 4).
- **Surprise: their AppImage needs glibc 2.41 (SteamOS 3.7+).** The user's Deck is on SteamOS 3.6.20 (glibc 2.39), so
  their release would not start there until the update.

## 1. Every commit, with a verdict

Final state on `origin/main` is what counts. Reverts are grouped with what they revert. Verdict keys:
- **Adopt (Linux/Deck)**
- **Port (general)**
- **Have** (with our equivalent)
- **Not wanted**
- **Unclear** (with the test that decides)

### 1.1 Packaging, CI, launcher

| Commits | What it does | Category | Verdict |
|---|---|---|---|
| `cc2e434` (CI part), `9d7fe24`, `1803166`, `2eddec6` | GitHub Actions build of the AppImage in a Debian 13 container (glibc 2.41), published as `v<ver>-linux.<run>`. Vulkan headers 1.4.344 from source (Debian has 1.4.309, no `eMesaKosmickrisp`). `g++` for `appimage.sh`, `zip` for the Tk 9 build | packaging | **Adopt** the recipe as the reference for our WSL build. Two notes: (1) glibc 2.41 does not run on the user's SteamOS 3.6.20 (glibc 2.39, plan-linux-deck.md "The Deck as found"), so our base must be older (Debian 12 / Ubuntu 22.04) or wait for the SteamOS update; (2) 1.4.344 was enough for them, while our plan says 1.4.363+ (PR #70). Recheck on our tree [inferred: `lab` may use newer enums]. Publishing a release still needs the user's go |
| `cc2e434` (`sync-upstream.yml`) | Merges each upstream release through an `upstream-merge` branch; conflicts open a PR | CI | **Not wanted**: we merge upstream by hand with a test (plan-upstream-merge.md), and `origin` is public |
| `cc2e434` (`install.sh`), `9acd9b8`, `2953974`, `4afb32c` | `curl \| sh` installer into `~/Applications` with a menu entry; README rewrite | packaging, docs | **Unclear**: only if we publish Linux releases (the user's call) |
| `22d6f92` | AppImage uses the host's `libwayland-*` and `libgbm`. The bundled Debian 13 `libwayland-client`, loaded first by SDL3, lacked a symbol SteamOS's Mesa needs, so "every Vulkan driver failed to load" [msg] | packaging (Deck) | **Adopt**: this is our plan's S4 item (#13 follow-up 2) with a concrete symptom. One line in `appimage.sh` `host_lib()` |
| `cc2e434` (`launcher/medievil_linux.py`, 911 lines), `bbport_steam.py` | Self-update from the fork's releases; **Add to Steam** (writes Steam's binary `shortcuts.vdf`, from Supermedo's bloodborne_pc; not in our `bloodborne_pc` checkout); **Steam Deck mode** (on by default on SteamOS, locks its settings and restores the player's own when turned off) | launcher (Deck) | **Adopt** Add to Steam and the Deck-mode mechanism (our plan's "Handheld preset"). **Not wanted**: the updater as written (points at their repo; our release channel is undecided) |
| `57fec3d` | Add to Steam also sets library art: cover and wide grid downloaded from SteamGridDB (game 5249914) when added, hero cut from the player's `sce_sys/pic0.png`. Nothing shipped | launcher (Deck) | **Adopt** with Add to Steam. SteamGridDB art is community-uploaded: fetching it on the player's machine ships nothing, which is the safe way |
| `37aec40` | Gaming Mode: a full-screen Update / Cancel prompt (controller through SDL3, touch, mouse) before the game when the fork has a newer release | launcher (Deck) | **Unclear**: only with our own releases |
| `55086ba`, `335ffcb` | Launcher: Sir Daniel icon; GPU-data switches, HDR, the Patches page and diagnostics hidden on Linux and reset to defaults before every start; GitHub and update corner | launcher | **Not wanted**: resetting switches before every start defeats our A/B workflow (fix-inventory "Switches"). The icon's art source is not stated |
| `08ee0b7`, `2d2a365` | `extra-env.txt` beside the launcher settings: `KEY=VALUE` lines added to the game's environment. `GOW3_UE4_CVARS` applied last | launcher, diagnostics | **Have** `GOW3_UE4_CVARS`, applied last (`scripts/ue4_override.py:511-518`). **Adopt** `extra-env.txt`: on the Deck it sets variables without editing Steam launch options, which suits one-change-per-launch tests |
| `489efb2` | The GPU wait report also sets `GOW3_IMAGE_STATS` | diagnostics | **Have**: `GOW3_TEXCACHE_STATS` |

### 1.2 Steam Deck mode values and the Low preset (UE4 config, class M)

| Commits | What it does | Verdict |
|---|---|---|
| `c6cc0b0` | Deck mode 30 FPS by default (60 selectable) | **Adopt** as the Deck preset's default. Our target is 60 on the OLED (plan-linux-deck.md "direction"), so offer both |
| `2e0c6c2` | Deck mode: **1080p output with a 720p scene** instead of 1280x720 output. At 720p output, upstream's live `r.SetRes` showed the 1080p loading screens and movies cropped and zoomed in [msg] | **Adopt** as the Deck preset (ours: `output_resolution=1920x1080`, `render_scale=67`), and see section 3: it is also the first Proton test |
| `cfc9b39` | Deck mode runs UE4 on a fixed clock (`bUseFixedFrameRate`, `GOW3_FIXED_FRAME_RATE` = the Deck rate): slow frames slow the game down instead of jumping | **Unclear**: changes game speed under load; against our pacing work (tick = cap). Offer it as an option and A/B the title pan on the Deck |
| `4519594`, `50ee1fa`, `428c051` | 60 Hz physics substeps under the fixed clock; now opt-in (`GOW3_PHYSICS_SUBSTEPS=1`) | **Not wanted** (did not fix the wisp; first suspect for the hologram dragons wandering [msg]) |
| `4519594`, `56496d5` | Low preset local-light shadow threshold 0.15 to 0.1, then back: at 0.1 the Hilltop Mausoleum turned bright pink because "those lights' shadows come out inverted" [msg] | **Unclear, worth a look**: inverted local-light shadows sound like a port bug (stencil or depth), not a setting |
| `f6a90fe` | Low: volumetric fog on with a coarser grid (16 px, 64 slices) instead of `r.VolumetricFog=0`: off, the height fog drew full inscattering, giving pink-glowing floors and a washed-out Mausoleum [msg] | **Adopt** in our `low` (ours has `r.VolumetricFog=0`, `scripts/ue4_override.py:221`) |
| `433cd1a` (config part) | Low: `r.TiledDeferredShading=0` (Mausoleum on the Deck APU: 21-23 to 30 FPS [msg]) | **Adopt** in the Deck preset; check on the PC (a UE4 setting, may help any AMD iGPU) |
| `b136400` | Low: sun shadow 2048 px, `r.ShadowQuality=3` (filtered): edges stop jumping, same FPS on the Deck [msg] | **Adopt** in the Deck preset |
| `fbef777` | Low: `r.ViewDistanceScale=1` (0.7 hid walls and barrels under the top-down camera in the Sleeping Village) | **Adopt** in our `low` (ours 0.7, `ue4_override.py:224`) |
| `05bda5c` (part) | `r.Shadow.Preshadows=0` in every preset: a dark square around Dan in Pumpkin Gorge on the Deck [msg] | **Unclear for the PC** (we have the live switch, default on). **Adopt** off in the Deck preset |

### 1.3 GPU thread, synchronisation, memory (shadPS4 video core)

| Commits | What it does | Verdict (our side checked in code) |
|---|---|---|
| `6ef7566` (part 1), `d0884ba` (ring) | Constant ring 32 to 64 to 256 MiB. `Retire()` first uses the GPU tick already read, and calls `semaphore->Refresh()` (an ioctl) only when that frees nothing. Pumpkin Gorge (~6,000 draws a frame) filled 64 MiB; the GPU thread then spent ~72% of its time in that ioctl [msg] | **Port** the `RetireKnown` part: ours refreshes on every `Retire()` (`vk_constant_ring.h:146-161`), although only when the ring is full. **Unclear** for the size: ours is 96 MiB with `GOW3_CONSTANT_RING_MB`, and ring reuse (N-32) and the HZB remap cut both bytes and draws. Test: the "ring full" pressure report (`ReportPressure`) in a Deck run of Pumpkin Gorge |
| `d0884ba` (stream) | Stream buffer 128 to 384 MiB: the draw recording thread waited in `StreamBuffer::Reserve` ~36% of its time [msg] | **Unclear**: ours 128 MiB (`buffer_cache.cpp:337`), but S2 binds clean small buffers from the arena. Add a Reserve-wait counter before resizing. On a 1 GB UMA carve-out, 256 + 384 MiB of device-local host-visible memory is a large share (our U1) |
| `6ef7566` (part 2) | `DrawPipe::Drain` sleeps on `consumed` (atomic wait) after 4,096 pauses + 32 yields, and stage B notifies only when a drainer waits (seq_cst flag). Upstream #30 profiled 15% in `sched_yield` | **Port**: ours yields forever after 4,096 pauses (`vk_draw_pipe.h:117-123`). Plan L3 |
| `6ef7566` (part 3) | A display-buffer label `WAIT_REG_MEM` that is already met does not drain the draw pipe (`GOW3_VO_LABEL_DRAIN=1`: always) | **Port**: ours always drains there (`liverpool.cpp:1361-1367`). Keep our `CloseRingReuse()` before it |
| `d82d86c` | Compute queues (from Uncharted `3f7be65`, `824a5c0`): the pipe is drained only before packets that need it (not before Nop, SetShReg, SetQueueReg, AcquireMem, dispatches, DmaData, IndirectBuffer). A compute `ReleaseMem` memory fence goes to the draw recording thread in order (`RunInOrder`) and is written after the host copies, instead of a drain and an inline wait. The Deck was blocked on host copies 22-24% with this and `a08d0b0` [msg] | **Port**: ours drains before every compute packet (`liverpool.cpp:1500`) and waits inline at `ReleaseMem` (`:1732-1749`). Check before porting: compute dispatches go through `PostDraw(..., &CsRegs())` (`vk_rasterizer.cpp:1442-1444`). Without a drain before each `SetShReg`, stage B must snapshot the CS registers when the dispatch is posted. Confirm it copies them |
| `a08d0b0`, `094b727` | `ProcessDownloadImages` skips the host-copy wait when there is nothing to download, and when no readback can retire yet (~70 waits a frame on the Deck [msg]) | **Have** an equivalent: W2 (`GOW3_EOP_TARGETED`, on) waits only for copies that read the queued readbacks' and downloads' ranges (`texture_cache.cpp:338-357`), so an idle fence waits for nothing. Our P2 (`GOW3_IDLE_EOP_SKIP`, opt-in) is their first half. It was dropped as a default because the copies then landed at once (T-P2: worst frames 29-42 ms) |
| `81442ce` | End-of-shader memory fences go to the recording thread (`GOW3_EOS_DEFER`) | **Have**: E1 (`cb1da6c`, `GOW3_EOS_DEFERRED`, `liverpool.cpp:436-462`), the same design and the same GDS exception |
| `c7ebc03`, `7401069`, `2488a68`, `81c7289`, `f9a74de`, `05682ea`, `2ff4265`, `433cd1a` (data-path part) | **Mapped arenas + deferred merges**, final state **on** (`GOW3_ARENA_MAPPED`, `GOW3_DEFER_DOWNLOADS`). On a UMA GPU the arena blocks come from a device-local host-visible type and are mapped. Each 64 KiB granule keeps the tick of its last GPU write. A CPU access to GPU-written data reads the mapping once that submission is done. If it is not done and the access is a write, a snapshot is kept and the GPU bytes are merged later, keeping bytes the CPU changed. Submissions end with a host-read barrier. Checks of the GPU are limited to one every 250 µs | **Have a different equivalent for the waits**: D3 (wait for the newest write's submission, then copy on the readback queue), N-13 (page-only downloads), GPU write twins (the same "keep the CPU's bytes" merge), D2 (precise writes). On the PC: downloads ~0/s (fix-inventory section 3). The mapped read itself is our U2 / U3. **Unclear** for the Deck: measure downloads/s and wait ms there first. Their history warns: the default flipped four times in one day; "stale GPU data: white, pink and washed-out lighting"; a hang (nested fault spinning on `pending_lock`); an illegal shader instruction on a level load from a merge landing in reused memory |
| `97f0ac9` | Copy threads read guest memory through the backing view (`runtime_memory_read_backing`): a copy thread reading the guest mapping crashed (SIGSEGV in `CopySparseMemory`) as a cinematic ended | **Port** (small): ours still `memcpy`s from the guest address after a region lookup (`gpu/shim/gow3gpu.cpp:116-128`), racing an unmap or a protection change. We already have `runtime_memory_read_backing` (`runtime_memory.c:1253`). Cost: it takes the read lock per chunk; measure upload time with `GOW3_SECTIONS` |
| `717338d` | Volumes drawn as 2D arrays served by the 3D image when the driver has `image2DViewOf3D` without `sampler2DViewOf3D` (SteamOS RADV). Without it the Deck rebuilt and copied the translucency volumes and the colour-grading LUT ~13 times a frame (~37 MB of copies) [msg] | **Have** for render targets: V3's view rule needs no extension (`IsVolumeSliceTarget`, `texture_cache.cpp:114-123`; `image_view.cpp:99-102`). Their storage-binding case is not covered by ours. **Unclear**: count volume expands with `GOW3_TEXCACHE_STATS` on the Deck. Also our log line "2D views of 3D on" reports the extension only (`vk_instance.cpp:764-766`), not the two features: add them |
| `2cfb243`, `59387dc` | **Stencil clears write exactly the clearing draw's pixels.** A GNM stencil clear is a draw with `STENCIL_CLEAR_ENABLE`. It was a render-pass load clear of the whole buffer, which also wiped bits other lights still read: shadows cut along straight edges, flickering black shapes (Pumpkin Gorge, upstream issue #40). The draw now runs with stencil always / replace with the clear value, mask 0xff, and the depth-stencil buffer attached even when the game's stencil test is off (`vk_depth_stencil_state.h`). `GOW3_STENCIL_CLEAR=rect` / `load` keep the older modes | **Port (general)**: ours is upstream's whole-buffer load clear (`vk_rasterizer.cpp:3228`, `:3332-3334`). Interacts with our render-state memo (`:3012-3013`), lean barriers and V4 stencil-plane copies: port with its switch and A/B it |
| `e8fa1ad`, `05bda5c` (part) | Depth clears drawn the same way (viewport depth range = clear value, depth always). Made **opt-in** before release (`GOW3_DEPTH_CLEAR=draw`): the square around Dan turned out to be the preshadows | **Unclear**: port only as an opt-in, after the stencil fix |
| `64454ab` (a) | No barrier for a read after a read in the same read-only layout (~36,000 of ~47,000 pass breaks per 5 s in Pumpkin Gorge [msg]) | **Have**: lean barriers (a) (`4b5d65c` and others, `GOW3_SKIP_READ_BARRIERS`, `image.cpp:547-556`) |
| `64454ab` (b), **reverted** `ec81d34` | Sampled depth keeps `DepthReadOnlyStencilAttachmentOptimal` instead of switching. Reverted 18 minutes later: "hung the render thread on pyro" (the Deck APU) | **Conflict / warning**: this is our lean barriers (b), on by default (`GOW3_DEPTH_SAMPLE_IN_PLACE`, `vk_rasterizer.cpp:3299-3304`, `vk_runtime.cpp:222`). Fine on NVIDIA (T-F6/T-F7). On RADV it is the first suspect for a hang or device loss. Test on the Deck: `GOW3_DEPTH_SAMPLE_IN_PLACE=0` |
| `8d6bf40`, **reverted** `ba0d35f` | Depth target stays in the writable layout unless the draw samples it (~240 pass breaks a frame) | **Not wanted** (reverted 47 minutes later, no reason given) |
| `aea2165`, `e87679c`, `4c63bf4`, `05bda5c` (part) | Frame-wait serving: guest write faults served while the GPU thread waits for a frame (`WaitServing`, a separate waiting queue). Opt-in (`GOW3_FRAME_WAIT_SERVE=1`): it gave the Mausoleum random light colours. `4c63bf4` puts waiting commands back in order when it is off (issue #7, Deck and ROG Ally) | **Not wanted**: reordering guest-fault work ahead of queued commands breaks data order. Ours never reordered |
| `6dcb901`, `b182492` | `r.SkinCache.Mode=0`, `r.MorphTarget.Mode=0` in every preset: the GPU skin cache's compute-written vertices came out stretched (a long white spike on Dan's silver shield) | **Port (general, no code)**: we set neither (`scripts/ue4_override.py`). Test on the PC first with `GOW3_UE4_CVARS=r.SkinCache.Mode=0;r.MorphTarget.Mode=0`. If the spike shows on NVIDIA too, the root cause is a port bug in compute vertex output, worth a later look |
| `5e7b0e5` | Firefly jitter: `BP_Firefly.OrbitTarget` turns by `Lerp(turn, Speed / Radius, 0.4)` without frame-time scaling. A 4-byte edit in the override pak sets alpha 0 | **Port** (M). Not scaled by the frame time, so likely worse at our 120+ FPS [inferred]. Test: the firefly beside Dan at 120 FPS, before and after |
| `c758207`, **reverted** `7f69812` | Two UE4 particle curve-injection pipelines compiled synchronously: with async compiles their draws were skipped, so torch flames had zero size | **Not wanted as is** (reverted, no reason given). The symptom is a warning for our async compiles: torches without flames after a cold pipeline cache |
| `5892ff9`, **reverted** `cc9e5b4` | File reads invalidate their range afterwards (stale level data, "random colours per run") | **Not wanted** (reverted the next day, no reason). The symptom matches the Mausoleum colour reports |
| `1f190b5`, `0f2b29c`, `6b97a8b`, `faebb8e`, `c130aad`, `7a392d6`, `d26b09b` | Diagnostics: `GOW3_PASS_BREAKS` (pass ends and in-pass barrier flushes by caller, layout pairs), host-copy waits by site, buffer downloads per frame, "Game pacing" (spread of the guest's flips) | **Have**: `GOW3_PERF_DIAG` (pass ends, barriers by site, host-copy waits by caller, write-fault downloads), the flip trace (`GOW3_FLIP_TRACE`, guest submit times) |
| `f24f457`, `05731bc` | Game-side pacing in `sceGnmSubmitAndFlip...`, now off: two clocks gave 15-57 ms frames [msg] | **Not wanted**: ours paces by tick = cap |

### 1.4 Runtime (loader, guest memory, threads)

| Commits | What it does | Verdict |
|---|---|---|
| `029ee2a`, reverted `0f8d9b0`, reapplied `7800f32` | `-ftls-model=initial-exec` for `libgow3gpu.so` (`__tls_get_addr` was ~3% of the draw recording thread, ~+1.5 FPS [msg]) | **Adopt (Linux)**: Windows links the library statically, no gain there |
| `954df48`, reverted `81de946`, reapplied `1232650` | `scePthreadGetthreadid` caches `gettid` per thread (150,000 syscalls in 4 s on the render thread) | **Adopt (Linux)**: ours calls `syscall(SYS_gettid)` every time (`runtime_extra.c:181`); Windows reads the TEB |
| `def86f9`, reverted `7f1e6d2`, reapplied `088973f` | Pool free-page search from a per-chunk first-free index with `memchr` (1440p graveyard: light 77 to 100, heavy 58 to 70 FPS [msg]) | **Have**: R3 (`2097554`, `free_hint`, `runtime_memory.c:922-1010`) |
| `ad7123c`, reverted `e3ae92b`, reapplied `033cb1f` | Region cache indexed by address (256 slots, 1 MiB each) instead of 8 round-robin entries (48% misses [msg]) | **Have an equivalent**: lock-free seqlock search on a miss (`clamp_optimistic`, `GOW3_OPTIMISTIC_CLAMP`), plus L2 bucket stamps (`runtime_memory.c:1269-1340`), so a miss costs a binary search, not the read lock. **Unclear**: worth it only if misses show up in a Deck profile |
| `bb9841f`, reverted `e308448`, reapplied `a3f4c1d` | Linux: pool commit / decommit make their `mmap` calls outside the region-table lock; decommit replaces the range with a reservation in one `mmap` (heavy 68.5 to 75.8 FPS [msg]). Windows still maps under the lock | **Adopt (Linux)**, or our L2 (keep views across a decommit on Linux, plan-linux-deck.md 5.4). Choose by a Deck profile. Both target the same `pool_decommit_impl` cost |
| `45390a5`, reverted `0c1f156`, reapplied `95e295e` | The table rwlock spins (1,000 pauses) before blocking; the hook queue and the pool page maps get their own spinlocks; `direct_unmap` suppresses its duplicate hooks with a flag (heavy ~77 to ~87 FPS [msg]) | **Adopt (Linux)**, partly: our readers rarely take the lock (seqlock), so the writer side and the separate pool and hook locks are the useful part |
| The six reverts above (2026-10-28) and reapplies (2026-10-30) | Reverted together and reapplied together, with no reason in either message | Probably taken out while another bug was isolated [inferred]. Not a warning about these changes |
| `0ddcabc` | `Kernel::Thread::Join`: one caller at a time (mutex). A thread stopping itself only `try_lock`s and detaches. The demuxer joined its decoders while `sceAvPlayerStop` joined them too: the second join waited on a gone thread, and **the game hung on the world map after the first level** [msg] | **Port**: ours has no join lock (`gpu/shim/core/libraries/kernel/threads.h:31-48`). On Windows the failed join is caught, waited and detached: the "HLE thread: join failed" line in every PC run and in `deck-proton-1.log`. On native Linux it is the hang. Compare with upstream PR #68 (dvilelaf) before porting; they address the same race |
| `0deb475` | `AvPlayerSource::Stop`: request stop on all three threads, notify every queue they sleep on, then join | **Port** (with `0ddcabc`; ours, `avplayer_source.cpp:304`, notifies only inside the threads) |
| `e88bf70` | AvPlayer never reports 0 audio channels (MediEvil divides by channels x 2: SIGFPE after a level on the Deck) | **Port** (`avplayer_source.cpp:166`, `:815`) |
| `6503421`, `9ec7ee4`, reverted `6fcceba`, reapplied `ba89450`, `58a7e18` (part) | Under a frame limit below 60, the limit steps to 60 while a movie plays and the frames are "just the movie" (<60 draws to start, back above 120), so 30 FPS movies are not beaten against a 30 cap. The first version also unlocked the in-engine title cinematic | **Adopt** for caps below 60 (the Deck; also any PC user at 30). Keep the draw-count gate: that was the reason for the revert |
| `934abc4`, `a7be15f`, `3f9c739` | AvPlayer: a video frame is due up to 16 ms early (`GOW3_MOVIE_EARLY_MS`); catch up when more than 67 ms behind; sound handed out at most 150 ms ahead (`GOW3_MOVIE_AUDIO_LEAD_MS`), with the wall clock as the movie clock (sound was ~1 s behind the picture [msg]) | **Port**, after one PC check: lip sync in a cutscene with talking (the intro). Each has its switch |
| `371f153` | Audio: NaN / infinite float samples go out as silence and are logged (setting an enemy on fire popped, then silence until the level was restarted [msg]) | **Port** (`runtime_audio.c`, no `isfinite` today) |
| `58a7e18`, `03209a3`, `401762b` | `sceAudioOutGetPortState` reports the port's 8 channels (it said 2, so Wwise did its own matrix downmix: a phase-inverted copy ~11 ms later in the other ear, "doubled, hollow" effects on headphones). The port folds the surrounds into stereo at 0.35 (`GOW3_AUDIO_SURROUND`; the Linux launcher defaults to 0, fronts and centre only) | **Unclear**: ours reports 2 (`runtime_audio.c:300`) and folds at 0.71 (`:181`). Test: a PC listening A/B on headphones at a zombie (`GOW3_AUDIO_PORT_CHANNELS=2` vs 8). With a real 7.1 device nothing should change |
| `0db13ee` | `GOW3_AUDIO_TRACE`: logs `sceNgs2PanInit` / `PanGetVolumeMatrix` arguments (both no-ops) | **Optional** diagnostic |

### 1.5 Input and test tools

| Commits | What it does | Verdict |
|---|---|---|
| `989ee5d`, `1ecdd89` | Pad recording without `GOW3_PAD_RECORD`: F9 or both back grips. Recordings stamped with (segment, frame) instead of milliseconds (a new segment after any frame over 700 ms). Replays frame for frame on the fixed 30 FPS clock | **Adopt (Deck test bench)**: repeatable Deck runs for the S-steps. We have the millisecond recorder (`runtime_pad.c:349`, from upstream), which drifts after a load [msg] |
| `42da5cd` | R4 or F10 writes `MARK n` with the frame position into the log, plus a gamescope screenshot in `<user>/marks/`. Under Steam the grips never reach SDL, so R4 is read from the controller's hidraw device (28de:1205) | **Adopt (Deck dev loop)**: the user can flag a glitch while Claude reads the log over SSH |
| `c409fcc`, `3ad9f59`, both **reverted** (`7215678`, `46a26fc`) | D-pad debounce in menus: the merchant menu moved two entries per press on the Deck; the Deck's D-pad "chatters 30-70 ms mid-press" [msg] | **Not wanted** (both reverted, no reason). Warning: expect the same report on our Deck builds |
| `caa9708` | Xbox and Steam Deck button pictures | Section 4 |
| Merges `31609da`, `b663fe8` (tag v1.3.8), `69790e0`, `8d649be`, `164e080` (revert of PR #3 until a release was approved), `26b31f2`, `e2142d5`, `b773c68`, `810aa82`, `802f8ed`, `4ae7198` | No content of their own | — |

## 2. Steam Deck performance

Upstream reports: 7-13 FPS under Proton and 13-18 FPS with the native AppImage in the graveyard (plan-linux-deck.md
1.5). The user saw a video of this fork near a stable 30 FPS. Their messages give these causes, in order of likely
size [msg, with my ranking inferred]:

| # | Change | Their numbers | `lab` |
|---|---|---|---|
| 1 | **Settings**: 30 FPS cap, 720p scene in 1080p output, Low preset with `r.TiledDeferredShading=0`, preshadows off, view distance 1 | Mausoleum 21-23 to 30 with tiled deferred off; Dan's Crypt held 30 at the 720p scene | Not there yet. Our presets are upstream's plus our PC cvars. Our console preset adds `r.DBuffer=0` + `r.EarlyZPass=1` (no forced prepass) and HZB occlusion through the remap, which cut ~600 and ~300-650 draws a frame on the PC. They have neither, so `lab` should need fewer draws than their build at the same settings [inferred] |
| 2 | **Native, not Proton** | upstream: 13-18 vs 7-13 | Our Deck run 1 was Proton. Native `lab` is untested (plan S3-S5) |
| 3 | **Ring and stream sizes, ring `Retire`** | Pumpkin Gorge: GPU thread ~72% in the semaphore ioctl, draw recording ~36% in `Reserve`, GPU idle ~40% | Ring 96 MiB (configurable) with ring reuse (BAR writes -81% on the PC), stream 128 MiB with S2. Probably fills less often [inferred]. Test with the pressure line before resizing |
| 4 | **Host-copy waits at fences** (`a08d0b0`, `094b727`, `81442ce`, `d82d86c`) | GPU thread blocked 17-24% to 1.8% (Uncharted on the Deck) | E1 and W2 cover the graphics-queue fences. **Compute queues still drain and wait inline**: port `d82d86c` |
| 5 | **Buffer download waits** (mapped arenas, deferred merges) | One small CPU-written block every third frame cost a whole GPU frame (~35 ms, 18-19 FPS) | D3 waits only for the write's own submission and copies on the readback queue (a compute-only family, present on RADV); twins avoid the wait for small pages; D2 removes most downloads. Measure downloads/s on the Deck before taking theirs |
| 6 | **Volume views with `image2DViewOf3D` alone** | ~37 MB of copies a frame in the Mausoleum | V3 serves render targets without the extension. Storage use not covered; check expands on the Deck |
| 7 | **Region-table lock and pool mapping on Linux** (`bb9841f`, `45390a5`, `ad7123c`, `def86f9`, `954df48`, `029ee2a`) | graveyard heavy 58 to ~87 FPS in steps, at 1440p ultra (likely their desktop, cherry-picked) | R3 and lock-free reads cover the reader side. **Linux-only parts missing**: mmap outside the lock (or our L2 keep-views), thread-id cache, TLS model |
| 8 | **`DrawPipe::Drain` sleeping**, VO label wait without drain | upstream #30: 15% in `sched_yield` | Missing; port |
| 9 | Lean barriers (a) | ~36,000 of ~47,000 pass breaks per 5 s | **Have**. Their (b) was reverted for a hang; **ours keeps (b) on**: test it first on the Deck |

What `lab` does that they do not, and that should matter on a 4-core Deck [inferred]:
- the HZB occlusion remap (fewer draws);
- no forced depth prepass;
- the stream-class table;
- texture-set memo by region stamps;
- bulk descriptor copy and sampler memo (stage B per draw);
- parked render targets;
- new images undefined (fewer uploads);
- M1 keep-views. Under Proton this Windows code path is active; natively it is not.

And one thing that costs us CPU there: the precise vblank tick spins up to 700 µs of each period (plan L1).
`deck-proton-1.log` shows a 120 Hz tick with spin 700 µs, about 8% of one core. At the 960 Hz default it would be
about two thirds of one core.

## 3. Presentation under gamescope / Proton

### 3.1 What the fork tells us

- **Their fork changes nothing in presentation** [code]. `vk_presenter.cpp` only gets the opt-in frame-wait serving,
  and `driver.cpp` only the movie limit step and statistics. `vk_swapchain.cpp` and `gpu/shim/window.cpp` are
  unchanged. They run natively through SDL3 on gamescope, never under Proton. So it holds no Proton fix.
- **Two hints, though:**
  - Their Deck mode deliberately avoids 1280x720 output (`2e0c6c2`). It uses the 1080p output with a 720p scene and
    lets gamescope scale.
  - Their AppImage needed the host's `libwayland` before any Vulkan driver loaded (`22d6f92`). That one is native
    only.

### 3.2 What run 1 shows (`data/deck-proton-1.log`, plan-linux-deck.md "Proton run 1")

- Black in **fullscreen and in borderless** (the user tried both), so the display mode is out.
- Audio plays, the two movies decode (149 / 159 frames), and the guest flips at a steady 60 FPS ("Perf" lines). Those
  lines count guest flips, not successful `vkQueuePresentKHR` calls.
- The run ends with "The game exited (code 1)" right after `Slow image create: 31.4 ms, 4096x4096` once the main menu
  loads.
- There is no `Fault:` line. Our Windows fault handler prints one and exits with 3 (`src/probe.c:192-252`). A guest
  `_exit` prints "guest requested _exit" (`runtime_kernel.c:179-183`), and a closed window exits with 0
  (`gpu/shim/gow3gpu.cpp:246-251`). **Code 1 with no message came from outside the runtime's own paths**: Wine, the
  Unix side of the driver, or an `exit(1)` in a library [inferred].
- **Since upstream 1.3.8, these presentation defaults changed (all on in run 1)**, from fix-inventory section 6 and
  `scripts/graphics_options.py:267-282`:
  - precise vblank tick (`GOW3_PRECISE_TICK`; spin via `CreateWaitableTimerExW(HIGH_RESOLUTION)`, which Wine may not
    honour, leaving a pure spin [inferred]);
  - presents on the vblank thread (`present_inline=1`, i.e. `GOW3_PRESENT_THREAD=0`);
  - precise limiter with hold;
  - time-critical + MMCSS priority (`GOW3_PRESENT_PRIORITY`).
- The Reflex path, present ids and present wait are inert on RADV without their variables (`vk_instance.cpp`
  CreateDevice: `GOW3_DISPLAY_TRACE`, `GOW3_REFLEX`).
- `window.cpp` gained only `FitWindowedSize` and the cursor hide.
- **The output size and its eboot patch are also new**: the run applied "Boot display size 1280x720" (log line 12),
  and our I1 boot-size patch was never run at 720p (plan-stage-b-split.md:1009).

### 3.3 Suspects for the black screen, ranked

1. **The 1280x720 output with our boot-size eboot patch.** The movies are black too, so whatever is wrong is wrong
   before any 3D scene. That points at the display path or the UI canvas size, not at scene features. Our 720p boot
   rect is the one untested part of that path. Upstream at 720p took a different route (live `r.SetRes`) and showed
   pictures, cropped. [inferred]
2. **Presentation threading and timing new since 1.3.8**: presenting from the vblank thread at time-critical
   priority, with the precise tick and limiter. Under winevulkan a present could fail every time (for example
   `OUT_OF_DATE` with no recreate if the window reports 0x0) while guest flips go on.
   `Presenter::Present` only recreates when the window size is non-zero (`vk_presenter.cpp`, the `swapchain.Present()`
   branch), and present results are not logged. [inferred]
3. **A `lab` GPU default that misbehaves on RADV under Wine**: the readback queue (D3, a second, compute-only queue
   family), new images undefined (N-11), park on unmap (N-24). Less likely: none of them is on the movie path's final
   copy to the swapchain [inferred].

### 3.4 Suspects for the exit after the menu loads, ranked

1. **GPU hang or device loss from lean barriers (b)**, sampled depth in `DepthReadOnlyStencilAttachmentOptimal`. The
   main menu is the first scene with shadow projections, and the fork's identical change "hung the render thread" on
   the Deck APU (`ec81d34`).
2. **Memory on the 1 GB UMA carve-out**: a 4096x4096 image was created just before. Our fixed buffers take
   device-local host-visible memory: ring 96 MiB and stream 128 MiB, plus residency chunks and the spare image block.
3. **The movie suspend hook at `OpenLevel`**, which fired at 16.4 s, right before the menu load (`GOW3_MOVIE_SUSPEND`,
   `src/probe.c:596-598`).

### 3.5 Tests: one change per launch, settings and variables first

Steam launch options take Linux environment variables in front of `%command%`, and Proton passes them to the game.
Before any launch:
- Read `journalctl -k -b` on the Deck over SSH for `amdgpu` ring timeouts or GPU resets from run 1 (if it has not
  rebooted since).
- Check `medievil-windows/user/crashdumps`.

| Launch | Where | Change (everything else as run 1) | Decides |
|---|---|---|---|
| P0 | **PC** | `output_resolution=1280x720` in the launcher, nothing else. No Deck needed | Black on the PC too means suspect 1 is confirmed, and the bug is ours at 720p, not Proton's |
| D1 | Deck | `PROTON_LOG=1 %command%` (already planned), run 1 settings | Wine's reason for exit code 1 (`steam-<appid>.log`: unhandled exception, Vulkan errors, RADV messages) |
| D2 | Deck | output **1920x1080**, `render_scale=67` (the fork's Deck mode) | Picture shown: suspect 1 (with P0) |
| D3 | Deck | if still black: `present_inline=0`, `present_precise_tick=0`, `present_limiter=standard` (upstream-like presentation, one preset change) | Picture shown: suspect 2; then bisect the three keys, one per launch |
| D4 | Deck | `GOW3_DEPTH_SAMPLE_IN_PLACE=0 %command%` | No exit at the menu: exit suspect 1 |
| D5 | Deck | `GOW3_MOVIE_SUSPEND=0 %command%` | Exit suspect 3 |
| D6 | Deck | `GOW3_CONSTANT_RING_MB=32 GOW3_STAGING_KEEP_MB=128 %command%` (one memory preset) | Exit suspect 2 |
| Code 1 | — | Log every `vkQueuePresentKHR` result that is not success, with the window and swapchain size (throttled), and print once whether the high-resolution waitable timer was created | Turns "black with flips" into a reason. Small, diagnostic only |
| Code 2 | — | Print `sampler2DViewOf3D` / `image2DViewOf3D` and the memory heaps at start (U1) | Facts for V3 and U1 on this driver |

D4 and D5 can be tried before D2 and D3 if the user wants the exit understood first. They do not depend on the
picture.

## 4. Button prompts (`caa9708`)

### 4.1 How theirs works [code: `scripts/button_prompts.py`, `scripts/ue4_override.py` `apply`]

- At every start (cached per style and per pak size and mtime in `out/`), `prompt_files(game, style)` reads textures
  from the player's own paks:
  - the tutorial bone prompts `UI/Textures/ButtonPrompts/PS4_{Cross,Circle,Square,Triangle}` (DXT5),
    `PS4_Options`, `PS4_Touch_Pad`, `PS4_L1/R1/L2/R2` (Xbox only);
  - `HUD_PS4Button_Triangle`;
  - the top 80 rows of `UI/Sprites/HUD/InventoryButtonAtlas` (BGRA).
- Each picture is redrawn in code:
  - **Face buttons**: the coloured PS symbol (high saturation) is masked, filled in from its surroundings (repeated
    blur) and replaced by a letter: Xbox A / B / X / Y in their colours with a dark outline, or the Deck's white
    letters, on a dark disc inside the parchment rim for the bone prompts.
  - **Options and touchpad**: Options becomes ☰ with "MENU", the touchpad becomes ⧉ (View).
  - **Shoulders** (Xbox only): L1 / R1 / L2 / R2 become LB / RB / LT / RT.
- The font is DejaVu Sans Bold from the system (or Pillow's default font).
- Tiling is the same rule as study-glyphs.md 2.1: 8x8-element tiles in rows, Morton order inside, padded to 8. Their
  `detile` / `retile` match our `tile_1d`.
- DXT5 is written with Pillow's DDS encoder (`pixel_format='DXT5'`, Pillow 11.2+ [inferred: our 10.4 cannot encode
  BC3]).
- The repainted `.uasset` / `.uexp` pairs go into the override pak beside the other edits.
- Choice: the launcher's "Button pictures" (PlayStation / Xbox / Steam Deck) sets `GOW3_BUTTON_PROMPTS`. Deck mode
  switches to the Deck style unless the player chose Xbox.

### 4.2 Compared with our plan (study-glyphs.md)

| | Theirs | Our plan |
|---|---|---|
| Art | Repainted from the game's own PS art at run time: the steel and parchment rims stay, letters are font-drawn. Nothing shipped, nothing from Microsoft or Valve | CC0 pack (juliocacko, "Default" for Xbox, retro for keyboard), scaled into the cells |
| Coverage | 4 face cells, Options, touchpad and (Xbox) 4 shoulder cells of the atlas, plus 4 face bone prompts, Options and touchpad bone prompts, 4 shoulder bone prompts, `HUD_PS4Button_Triangle` | All 16 atlas cells (d-pad, sticks too), `L1_Active` / `R1_Active` / `Options_Active` (bestiary, inventory and options pages), the 3 bone prompts the game uses, optionally the controller picture and the "L3" label |
| Gaps in theirs | `L1_Active`, `R1_Active`, `Options_Active` untouched: the inventory and bestiary tabs keep PS art. Of the bone textures they edit, only `PS4_Cross` and `PS4_Triangle` are referenced (study-glyphs.md 1.3: `PS4_R1` plus those two; Circle, Square, Options, Touch_Pad, L1/L2/R2 and `HUD_PS4Button_Triangle` are named by no asset header), so most of those edits are inert. The Modern layout's `PS4_R1` prompt keeps "R1" in Xbox style | — |
| Keyboard | None | Keyboard key caps (G4) |
| Remaps | Ignored | Remap-aware cells (a+) |
| Format handling | Finds `PF_` and reads size and offset relative to it; only the first mip | Full `.uexp` layout (study-glyphs.md 2.1), same restriction (same size and format) |

**What we can reuse:**
- the cooking pipeline:
  - `parse()` (needs a stricter check on our side);
  - `detile` / `retile`;
  - `load_texture` / `store_texture` with Pillow's DXT5 writer, which removes our "needs a BC3 encoder" step if we
    raise Pillow to 11.2+;
  - `prompt_files`' cache keyed by pak size and mtime;
- the `read_entry` refactor of `ue4_override.read_file`;
- the integration point in `apply`;
- their repaint method as a **zero-art fallback** for any cell the pack lacks.

Licensing: their code is GPL-2.0-or-later (credit `caa9708`). The images are generated on the player's machine from
the player's game. DejaVu fonts are under the Bitstream Vera licence (permissive) and come from the system, not
shipped. No third-party art was taken. Our plan's CC0 pack stays the art source.

## 5. Recommended port order (smallest risk first)

Each item is one commit on `lab`, with their hash (and the Uncharted hash where given) in the message, and one
launch with the user's go to test it.

| # | Port | Risk | Test |
|---|---|---|---|
| 0 | Proton tests P0, D1-D6 (no code) | none | Section 3.5 |
| 1 | `r.SkinCache.Mode=0`, `r.MorphTarget.Mode=0` (`6dcb901`): first as `GOW3_UE4_CVARS`, no build | L | PC: Dan's silver shield (white spike?) and the Enchanted Earth opening; FPS unchanged in a fight |
| 2 | Audio NaN guard (`371f153`) | L | PC: set enemies on fire for a minute; no "NaN" line on the PC means nothing changed (expected); sound keeps playing |
| 3 | AvPlayer: join lock (`0ddcabc`), wake before join (`0deb475`), 0-channel guard (`e88bf70`); compare with PR #68 first | L | PC: logo, intro, finish a level (exit movie), world map. The "HLE thread: join failed" line should be gone. Deck native later: five level ends without a hang |
| 4 | Firefly byte edit (`5e7b0e5`) | L | PC at 120 FPS: the firefly beside Dan, standing and after a jump; before and after |
| 5 | Copy threads through the backing view (`97f0ac9`, `CopySparseSerial` part only) | L-M | PC: `GOW3_SECTIONS` upload time per frame before and after, the intro cinematic's end, a level load |
| 6 | `ConstantRing` `RetireKnown` (`6ef7566` part 1); a Reserve-wait counter for the stream buffer | L | PC: N-32's lows unchanged; ring pressure lines. Deck: Pumpkin Gorge pressure line, for the size decision |
| 7 | VO label wait already met: no drain (`6ef7566` part 3), behind an env switch | L-M | PC: `GOW3_PERF_DIAG` drains per second by reason; flip trace unchanged; no tearing or half frames |
| 8 | `DrawPipe::Drain` sleep (`6ef7566` part 2), behind an env switch | M (missed wake-ups would stall) | PC: lows A/B in the heavy area (two launches); stage A drain cycles. Deck: CPU use |
| 9 | Compute queues (`d82d86c`): drains only where needed, compute `ReleaseMem` fences pipelined; first confirm CS-register snapshotting in `PostDraw` | M | PC: host-copy waits by site in `GOW3_PERF_DIAG`; visual check of particles, torches, lighting in two levels; `GOW3_ASC_DRAIN=all` / `GOW3_ASC_FENCE_PIPE=0` as the A/B |
| 10 | Stencil clears exact (`2cfb243`, `59387dc`) with `GOW3_STENCIL_CLEAR=load` as the old path | M | PC: Pumpkin Gorge, the Pumpkin King arena: shadow edges and the backdrop, both modes. Then torches and box shadows (V4). Deck: issue #40 flicker |
| 11 | Movie step to 60 under caps below 60 (`ba89450` final state), AvPlayer early, catch-up and audio lead (`934abc4`, `a7be15f`, `3f9c739`) | M | PC at a 30 cap: intro movie smooth, title cinematic still capped; lip sync in the intro at 120 FPS |
| 12 | Linux-only runtime: TLS model (`7800f32`), thread-id cache (`1232650`), pool mapping outside the lock + lock spin (`a3f4c1d`, `95e295e`), or our L2 | M | Deck native: `perf` share of `__tls_get_addr`, `gettid`, rwlock waits; graveyard FPS |
| 13 | Deck preset values (section 1.2), Deck mode, Add to Steam + art, `extra-env.txt`, AppImage host `libwayland` / `libgbm`, R4 / F10 marks, frame-stamped recording | L (launcher, scripts) | Deck: Game Mode start from the shortcut, settings locked and restored, marks in the log |
| 14 | Mapped arenas and deferred merges, or our U2 / U3 | H | Only if Deck runs show download waits; A/B with toggles in one launch |

Not to take: frame-wait serving, the waiting-queue reorder, game-side pacing, physics substeps, the depth layout
"stays writable" change, the D-pad debounce, `sync-upstream.yml`, the launcher's hidden and reset switches.

## Open questions

- Did the user's upstream 1.3.8 Proton test use 1080p output? If so, suspect 1 for the black screen gains weight.
- Which SteamOS and Mesa did the fork's author use? `717338d` says SteamOS RADV lacks `sampler2DViewOf3D`. Our run 1
  log does not show the features (Code 2 fixes that).
- Is "pyro" a Steam Deck or another Van Gogh machine? The messages say "the Deck's APU".
- The fork's issue #7 (Mausoleum lights pink or washed out at random, Deck and ROG Ally) is open on their side. Check
  ours there on the Deck before porting any of their data-path changes.
