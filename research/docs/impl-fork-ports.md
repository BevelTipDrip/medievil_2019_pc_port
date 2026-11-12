# Fork ports: three upstream PRs on branch `fork-ports`

Status (2026-11-12): re-applied unchanged on branch `upstream-ports` from `deck-lab` `e2594e3` (`babe332`,
`dd14bd3`, `75f2b89`) for 2.3.0, after upstream merged all three (1.4.x: `2099f27`, `e688328`, `ebdfa96` with fix-ups
`d3c91aa` / `907d8e6`; identical files). Earlier status: implemented 2026-11-05, **built on Windows, not run in game** (nothing here is verified in the project's
sense). Branch `fork-ports`, from `lab` `63b0c76`, local only, not pushed. It exists for full regression testing on
the PC (RTX 4090) and the Steam Deck before anything is merged into `lab` or `deck-lab`. Parent:
[START-HERE.md](START-HERE.md). Source of the shortlist: `study-forks-survey.md` section 9, rows 1, 2 and 5 (that
study is on `deck-lab` and in the main checkout, not on this branch).

| # | Commit | Author (kept) | From | Fixes | Switch |
|---|---|---|---|---|---|
| 1 | `b182a97` | dvilela | upstream PR #68, `42b2afb` | Hang after the first movie / at a level end (AvPlayer thread joins); the "HLE thread: join failed" line on Windows | none (see 1.5) |
| 2 | `f130b8d` | Porticus | upstream PR #52, `40d105e` | Torch flames (GPU particles) missing on a cold pipeline cache | `GOW3_CURVE_SYNC=0` |
| 3 | `42d25a1` | Porticus | upstream PR #64, `ffa6a72` (hotplug part only) | Pads plugged in after start not seen | `GOW3_PAD_HOTPLUG=0` |

All three files are identical on `lab` and `deck-lab` (`git diff lab deck-lab` on `threads.h`,
`vk_pipeline_cache.cpp`, `window.cpp`, `runtime_pad.c` and `avplayer/` is empty), so the three commits cherry-pick
onto `deck-lab` as they are for the Deck build.

Licence: upstream and both forks are GPL-2.0. Each commit keeps the original author (`git commit --author`), names
the PR and hash, and ends with our attribution line.

## 1. AvPlayer thread joins (dvilelaf, PR #68)

### 1.1 What it fixes

The game stops each movie with `sceAvPlayerStop`, which joins AvPlayer's demuxer and decoder threads
(`AvPlayerSource::Stop`, `gpu/shadps4/core/libraries/avplayer/avplayer_source.cpp:304`). The threads also joined
themselves when they finished (`:602-607`, `:763`, `:882`), and a self-join **detached**. A decoder detaching itself
while the demuxer or the game thread was joining it left that join on a released thread:
- native Linux (the Deck's AppImage): `pthread_join` on a detached thread, a hang. After the logo the screen stays
  black, sound goes on (dvilelaf); on the world map after a level (spacepepperoni `0ddcabc`). Fits upstream issues
  #50 (map softlock, Deck), #60 (freeze on "restart level" / "quit to map"), #59 (black at the second start)
  [inferred].
- Windows (libc++): the join throws, the port catches it, prints `HLE thread: join failed (thread::join failed: No
  such device or address); waiting for it to finish` and waits up to 5 s for the thread's function to end. It is in
  every PC log at the logo's end (the user's `user\last_run.log` of 2026-11-05: one, between the logo's `Start`
  and `Stop`).

### 1.2 The change

`gpu/shim/core/libraries/kernel/threads.h` (`Kernel::Thread`):
- a thread joining itself returns at once; its owner joins it later; nothing detaches while another thread may join;
- one joiner at a time (`join_mutex`); the self check is made on an atomic id before the lock (the owner may hold the
  lock while joining this very thread);
- `Stop()` requests the stop through a saved `stop_source`, without waiting for another joiner (the demuxer may be
  joining a decoder that only ends once it is asked to stop);
- `Joinable()` = started and its function has not returned: a finished thread still counts as stopped, as before,
  so `AvPlayerSource::Stop`'s "Could not stop playback: already stopped" path is unchanged;
- `Run()` stops and joins a previous thread first; the destructor detaches only when a thread destroys its own object.

### 1.3 How ours differs, adaptation

- Our `threads.h` was **byte-identical** to the PR's base (upstream 1.3.8 `424f3ea`; last touched by `a7cf186`), so
  the header is taken unchanged. No adaptation in code.
- Our AvPlayer has two additions upstream lacks, checked against the new join rules:
  - `6fd8e15` call trace (`AvPlayer <t> s: <handle> Stop -> ...` lines): no thread logic.
  - `f0ced8c` level-change suspend (`AvPlayerState::Suspend`, `avplayer_state.cpp:216`): pauses the source only. Its
    decoders then wait in `m_video_buffers_cv.Wait(stop, ...)` / `m_audio_buffers_cv.Wait(stop, ...)`
    (`avplayer_source.cpp:725`, `:847`), which wake on the stop request, so `Stop()` still ends them. The demuxer's
    full-queue wait is a 5 ms sleep loop on `stop_requested()` (`:546-550`).
- The players' destruction path (`sceAvPlayerClose` -> `~AvPlayerState` -> `~AvPlayerSource` -> `Stop()` and the
  members' `~Thread`) runs on the game thread, never on a player thread. A thread that finished and was never joined
  (all three after a movie's natural end: `Stop` returns "already stopped") is now joined by `~Thread` at `Close`,
  instead of having detached itself. The intro's player is never closed by the game (study-intro-movie.md), so its
  finished threads stay un-joined until exit, as harmless as the detached ones were.
- Not taken: spacepepperoni's `0ddcabc` (same race; keeps the self-detach behind `try_lock`, reads `joinable()` /
  `get_id()` outside the lock: a data race). Separate, not in this branch: their `0deb475` (wake movie threads
  before the join) and `e88bf70` (never 0 audio channels).

### 1.4 Offline check (no game)

A standalone program (scratchpad `threadtest\thread_test.cpp`, not committed) runs the AvPlayer pattern 400 times
with MSYS2 CLANG64 / libc++, the port's toolchain: demuxer + two decoders that self-join, the demuxer joining them,
the "game" stopping all three at varied moments, then a restart on the same objects, a natural end, and a thread
deleting its own object.
- With `lab`'s `threads.h`: **905 `HLE thread: join failed` lines** (894 "No such device or address", the PC log's
  message, and 11 "Invalid argument").
- With this branch's: none, every Stop leaves nothing running, `OK: 400 stop/restart cycles, self delete fine`.

This shows the race and its fix in the header alone, on Windows. It does not show the game's own timings.

### 1.5 Risk and why no switch

Risk: low to medium (thread lifetime: a mistake hangs a movie's end). The new waits are a second joiner waiting for
the first (previously both joined at once, undefined behaviour) and `Run()` / `~Thread` joining a finished thread
(immediate). No switch: the old path *is* the race, so a switch back would only bring the hang back; the A/B is a
build without `b182a97`.

### 1.6 Test

PC (the movie race is timing-dependent; on Windows the visible symptom is the log line, not a hang):
1. Start, let the Other Ocean logo (`ooi_logo_plain_black.mp4`) end, then the intro (`gameintro.mp4`): watch some,
   skip it. Into the crypt, and play on past ~200 s after the intro started: the level change suspended the intro
   (`... suspended at a level change`), and when the game's own clock reaches its end it should stop that player
   itself, joining its paused threads (study-intro-movie.md, [inferred] there). If an `AvPlayer ... Stop` line for
   `gameintro.mp4` shows: no hitch or hang at that moment.
2. Log (`user\last_run.log`): **no `HLE thread: join failed` line** (every `lab` log has one at the logo's end).
   `AvPlayer ... Stop` / `Close` lines for the logo as before. The two `Could not stop playback: already stopped`
   warnings at the logo's close are expected (unchanged).
3. Finish a level (its exit movie), the world map, "restart level", "quit to map" (#60), quit to the title and start
   again (#59). No hang, no black screen with sound.
4. Second start of the game in a row (#59).

Deck (native AppImage, where the race is a hang): five starts past the logo into the crypt, and five level ends to
the world map, no hang. On Linux the old code mostly hung instead of printing; with the fix no `HLE thread: join
failed` line should appear there either.

## 2. Particle curve injection compiled synchronously (Porticus, PR #52)

### 2.1 What it fixes

UE4 fills its particle curve texture (`FParticleCurveTexture`, 512x512) once at level load with a burst of
injection draws (vs `0x113f0c20873a7659`, ps `0x1b1e7c76d6a281d3`); nothing redraws it. With async shader
compilation (`async_shaders=1`, the default), a draw whose program or pipeline is still compiling is skipped
(`GetGraphicsPipeline` returns null). On a cold cache Porticus counted 80 of 84 injection draws lost: the texture
kept its red clear, and GPU sprites sampling it got a zero size: no torch flame, only the distortion sprite as a dark
smudge (PR screenshots: the torch on the right wall at the start of the crypt). Tested by them on Linux RADV with the
pipeline cache off, never on Windows.

### 2.2 The change

`gpu/shadps4/video_core/renderer_vulkan/vk_pipeline_cache.cpp`: `MustCompileSync(hash)` is true for the two
programs. `GetProgram` compiles them on the spot (not queued), and `GetGraphicsPipeline` builds a pipeline that uses
either of them synchronously. Everything else stays async.

### 2.3 How ours differs, adaptation

- Our `vk_pipeline_cache.cpp` was **identical** to the PR's base (upstream 1.3.7; unchanged in 1.3.8), so the PR
  applies unchanged.
- Our draw pipe's prepared draws (`TryPreparedPipeline`, from `vk_draw_prep.cpp`) only hand out pipelines that are
  already built; a missing curve pipeline falls through to the regular path, where the exception sits. So the
  exception covers both paths.
- The synchronous path is the one `async_shaders=0` uses (launcher / overlay "Compile new shaders in the
  background" off): already exercised. It waits for a program still pending, and the background warm-up keeps the
  first pipeline published for a key (`PublishFirst`), so a pipeline built here wins over a warm-up copy.
- **The hashes are ours too**: `user\cache\CUSA11227\0x113f0c20873a7659_0.spv` and `0x1b1e7c76d6a281d3_0.spv` are in
  the user's PC cache (game CUSA11227; Porticus used CUSA12982). Program hashes come from the shader code in the paks.
- Added by us: `GOW3_CURVE_SYNC=0` (back to async for these two, for an A/B in one build), and one stdout line per
  run and program when the exception compiles a program not in the cache yet:
  `gow3: curve injection program 0x113f0c20873a7659 ready synchronously (N ms)`. No line with a warm cache (the
  warm-up already has the program).

### 2.4 Why spacepepperoni reverted it

They took it as `c758207` (committed 2026-10-28 20:26) and reverted it as `7f69812` (23:07 the same evening) with
the bare "Revert ..." message. [inferred, from their history] That evening was a revert hunt for stale GPU data:
the six Porticus PR #57 runtime commits reverted at 20:43, "CPU-mapped buffer arenas off by default (stale GPU data:
white, pink and washed-out lighting)" at 21:01, "Back to the Mausoleum-correct GPU data path" at 22:49, then this
revert. The cause turned out to be their buffer path; the six were reapplied on 10-30 after the 1.3.8 merge, this
one never was. Nothing in their history or PR texts says it caused a problem.

### 2.5 Risk

Low. Per level load on a cold cache: two shaders and their pipeline(s) compiled on the GPU thread, a hitch of a few
to tens of ms (the log line gives the shader part). With a warm cache: no change (the program is cached; the extra
cost is a hash compare per draw on the regular path).

### 2.6 Test: cold shader cache

The cache: `<port>\user\cache\CUSA11227\` on the PC (`C:\Users\Ty\MediEvil\medievil_2019_pc_port\user\cache\` for
the main checkout); on the Deck `~/.local/share/medievil-pc/user/cache/` (AppImage) or
`~/medievil-dev/data/user/cache/` (dev runs). **Only the `cache` folder: never the rest of `user\` (saves in
`user\savedata`).** The launcher's "Clear shader cache" deletes exactly `user\cache`. Better for a test: rename
`user\cache` to `user\cache.warm` and rename it back afterwards (the rebuild otherwise stutters for minutes). The
NVIDIA / Mesa driver caches need not be cleared: the skip happens in the port before the driver; a warm driver cache
only makes compiles faster, which may hide the bug.

PC, one launch each, both with the cache renamed away:
1. **A (bug present?)**: `run-game.ps1 -Env @{GOW3_CURVE_SYNC='0'} -Tag curve-off`. Load the crypt (new game or the
   save's start). Expected if the bug shows on Windows: the torch on the right wall at the crypt's start has no
   flame, only a dark smudge. Unknown whether a 4090's compile speed hides it (survey "Unverified").
2. **B (fix)**: default (no env), cache renamed away again. Same torch: flame present. Log: two
   `gow3: curve injection program ... ready synchronously` lines, with their ms (the level-load hitch).
3. Warm cache (cache folder back): flame present, no such line; no level-load hitch compared with `lab`.

Deck: the same A/B; the Deck's slower compiles make the cold case likelier to show. Also other GPU-particle effects
at level start (fire, wisps).

## 3. Pads plugged in after start (Porticus, PR #64, hotplug part)

### 3.1 What it fixes

A pad connected while the game ran was never seen (Porticus, Bazzite + distrobox, 8BitDo Ultimate; three reconnect
cycles fixed). Two causes:
- Windows: SDL hears of new devices through a message window on the thread that started the gamepad subsystem. In
  our port that is the window thread (`gpu/shim/window.cpp:21`) or, if the game polls pads first, the game thread
  (`src/runtime_pad.c` `current_gamepads`), which never pumps messages. `SDL_HINT_JOYSTICK_THREAD=1` gives SDL its own
  thread for device notifications and raw input (a Windows-only hint).
- In a container (`/run/.containerenv` or `/.dockerenv`: distrobox, toolbox) udev's hotplug events do not arrive:
  `SDL_JOYSTICK_DISABLE_UDEV=1` and `SDL_HIDAPI_UDEV=0` make SDL watch `/dev/input`.

### 3.2 How ours differs, adaptation

- Taken: the three hints, in `window.cpp` and in `runtime_pad.c`, whichever starts the gamepad subsystem first (SDL
  reads them once, at joystick init), as in the PR.
- Not taken (we have it): the PR's pointer hiding on pad input. We have Bloodborne's rule (`WindowSDL::UpdateCursor`,
  `window.cpp`, Bloodborne `4d4d208`: hidden in fullscreen, and in a window after 3 s without mouse motion); the PR's
  `UpdateCursor(event)` would conflict with it.
- Not taken: the PR's `Window: SDL gamepad added (id N)` line. `runtime_pad.c` already logs `Runtime: gamepad
  connected: <name> (USB vid:pid, n open)` when it opens a new pad (it looks once a second) and `Runtime: gamepad
  disconnected: <name>`.
- Added: `GOW3_PAD_HOTPLUG=0` (no hints: SDL's defaults, as before), and in a container the line `Window: container:
  SDL watches /dev/input for pads (no udev)`.

### 3.3 Risk

Low, with a switch because it touches all pad input on Windows: device notifications and raw input move to an SDL
thread. What to re-check is pad input as a whole, including input while another window has the focus (issue #25,
`SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS`). Native Linux outside a container (the Deck's AppImage, [inferred])
gets only the joystick-thread hint, which SDL ignores off Windows: no change.

### 3.4 Test

PC:
1. Start with no pad connected. At the title, plug the pad in (USB; then Bluetooth on another launch if used). Log:
   `Runtime: gamepad connected: ...` within about a second; the pad drives the menu.
2. Unplug: `Runtime: gamepad disconnected: ...`; the keyboard still works. Replug: connected again, the pad works.
3. Pad already connected at start: works as on `lab`; with the launcher window focused the pad still drives the game
   (issue #25).
4. If anything regresses: `-Env @{GOW3_PAD_HOTPLUG='0'}` for the A/B.

Deck: the built-in controller works as before (Steam Input). Plug an external pad after start (USB-C or Bluetooth):
connected line, and it plays. The log shows whether the container hints applied (the `Window: container` line: only
for a binary run inside distrobox).

## 4. Regression checklist for `fork-ports` before any merge

Run with the user's go, one launch per item group, logs tagged (`run-game.ps1 -Tag fp-<n>`). PC first, then the
Deck build (the three commits cherry-picked onto `deck-lab`; they apply as they are).

| # | Where | Check | Pass |
|---|---|---|---|
| R1 | PC | Cold boot: logo, intro (skip), crypt | No `HLE thread: join failed`; no hang; the movie trace lines as on `lab` |
| R2 | PC | Cold shader cache (rename `user\cache`), crypt start, `GOW3_CURVE_SYNC=0` then default | Torch flame missing (A) / present (B); B logs two `curve injection program` lines with their ms |
| R3 | PC | Warm cache, normal play 10-15 min (graveyard, a fight) | FPS, 1% / 0.1% lows and frame-time graph as on `lab` (same settings, same spot); no new hitches at level loads |
| R4 | PC | Finish a level, world map, restart level, quit to map, quit to title and start again | No hang, no black screen (#59, #60) |
| R5 | PC | Second game start in a row | Boots, logo and intro play |
| R6 | PC | Pad: unplug / replug in game; start without a pad, plug it in; launcher focused | Connected / disconnected lines; pad works each time; background input still works |
| R7 | PC | Keyboard and mouse only, no pad | Unchanged (cursor hides as before) |
| R8 | Deck | Five starts past the logo; five level ends to the world map | No hang |
| R9 | Deck | Cold cache, crypt start (A/B as R2) | Flame present with the fix; hitch size from the log line |
| R10 | Deck | 30 min normal play at the Deck preset | FPS and pacing as `deck-lab`; no new hitches |
| R11 | Deck | External pad plugged after start; built-in controls | Pad seen; built-in unchanged |

Before merging: the logs of R1-R11 in `research/docs/data/`, the user's confirmation per item ("verified" = the
user saw it in game), and the regression numbers (R3, R10) compared with a `lab` / `deck-lab` run on the same day.

## 5. Build and tests (2026-11-05, this branch at `42d25a1`)

- Windows (MSYS2 CLANG64, `research/tools/build-clang64.sh` in the worktree): `build.sh exit: 0`; the 5 warnings in
  `out/gpu-build.log` are all in files these commits do not touch (fsr-vulkan C23 initializer, `vk_instance.cpp`,
  `gow3_sampler.cpp` x2, `gow3_overlay.cpp`). `vk_pipeline_cache.cpp`, `avplayer_source.cpp` (includes
  `threads.h`), `window.cpp` and `runtime_pad.c` were rebuilt.
- Python (`py -3.14 -m unittest discover -s tests`, Git Bash): 124 run, the 8 known failures (2 failures, 6 errors),
  nothing new. From PowerShell `test_mods ... cleans_view` also fails (`bash` resolves to WSL's there); no Python
  changed.
- The `threads.h` check of 1.4.
- Not done: a Linux compile (the PRs' authors built all three on Linux; our additions are `getenv`, `printf` and
  `access`, already used in these files or their platform headers). Not run in game anywhere.
