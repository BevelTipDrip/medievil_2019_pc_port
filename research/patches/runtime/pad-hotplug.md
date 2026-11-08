---
id: pad-hotplug
title: Pads plugged in after start: SDL hotplug hints (fork port, #64)
area: runtime
status: active
platforms: all
default_windows: on
default_deck: on
origin: upstream PR #64 (Porticus ffa6a72, hotplug part only)
class: G
commits: 42d25a1dcd
files: gpu/shim/window.cpp, src/runtime_pad.c
symbols: WindowSDL, current_gamepads, sdl_ready
switches:
tests:
docs: study-forks-survey.md#9-worth-porting-shortlist, study-forks-survey.md#22-other-porticus-branches
depends_on:
related: upstream-audio-input, cursor-hide
checked: 2026-11-07
---
## What it does

Only on branch `fork-ports`: not in `lab` or `deck-lab` yet. The write-up is `research/docs/impl-fork-ports.md` §3
on that branch. Its switch `GOW3_PAD_HOTPLUG` exists only there, so the facts do not list it. Before SDL starts the
gamepad subsystem, the port sets `SDL_HINT_JOYSTICK_THREAD=1`. On Windows SDL then hears of new devices on its own
thread instead of through a message window on the thread that started the subsystem, which can be the game thread
(`current_gamepads`) that never pumps messages. In a container (`/run/.containerenv` or `/.dockerenv`: distrobox,
toolbox) it also sets `SDL_JOYSTICK_DISABLE_UDEV=1` and `SDL_HIDAPI_UDEV=0`, so SDL watches `/dev/input`.
`GOW3_PAD_HOTPLUG=0` leaves SDL's defaults. Defaults below: on where the code exists (`fork-ports`).

## Invariants

- The hints are set before the first `SDL_InitSubSystem(SDL_INIT_GAMEPAD)` on both paths: the `WindowSDL`
  constructor and `current_gamepads` while `sdl_ready` is 0. SDL reads them once, at joystick init. A hint set
  after the first init does nothing, and a third place that starts the subsystem must set them too.
- The two paths must set the same hints and test the same switch, otherwise the result depends on which thread
  starts the subsystem first. Only the window path prints the container line. When the window started SDL first,
  `current_gamepads` sets the hints once more before `SDL_WasInit`: harmless, they are read only at init.
- New pads are still found by `current_gamepads`' scan once a second (`SDL_GetGamepads`). The hints only make SDL's
  device list current; they do not open pads.

## Interactions

- upstream-audio-input: in the same two files. The constructor sets `SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS`
  (issue #25) just before the new hints; `current_gamepads` is upstream's every-controller scan, and the new block
  runs at its top. On Windows device notifications and raw input move to an SDL
  thread, so input while another window has the focus is the thing to re-check.
- cursor-hide: the PR's pointer hiding on pad input was not taken. It would conflict with Bloodborne's rule in
  `WindowSDL::UpdateCursor`. The PR's "SDL gamepad added" log line was not taken either: `runtime_pad.c` already logs
  `Runtime: gamepad connected`.
- Deck: native AppImage outside a container gets only the joystick-thread hint, which SDL ignores off Windows: no
  change there [inferred in impl-fork-ports.md]. The built-in controller goes through Steam Input either way.

## Evidence

- Porticus (PR #64): Bazzite + distrobox, 8BitDo Ultimate, three reconnect cycles fixed [msg].
- Ours: built on Windows (2026-11-05). **Not run in game.** Not verified in game. Test plan: impl-fork-ports.md §3.4
  and regression item R6 (start without a pad and plug it in; unplug and replug; pad input with the launcher focused).

## History and pitfalls

- Shortlist row 5 of study-forks-survey.md §9, taken as the hotplug part only.
- Risk: low, but it touches all pad input on Windows. If input regresses, A/B with `GOW3_PAD_HOTPLUG=0` before
  anything else.
- If a Deck launch ever runs inside a container that has `/run/.containerenv` or `/.dockerenv` (check before
  assuming the Steam runtime has neither), SDL loses udev there too and watches `/dev/input` [inferred].
- A Linux compile of the branch was not done; the additions are `getenv`, `printf` and `access` (`unistd.h`).
