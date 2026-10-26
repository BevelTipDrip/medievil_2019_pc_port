# Study: why the intro movie is never stopped

Status: study, 2026-10-26. Read-only reverse engineering. Nothing was built, run or changed in the port. Parent:
[START-HERE.md](START-HERE.md).
Evidence: capstone over the 01.00 image (`research-data\ghidra\stageb\py\eimg.py` and `relocs.py` loaders), Ghidra
12.1.4 headless **read-only** on `research-data\ghidra\proj`, the game's own paks (read with `scripts/ue4_override.py`'s
`read_file`), the two mp4 headers, and `user/last_run.log`. Scripts and raw outputs are in
`research-data\ghidra\intro\`: `run.sh <out> <Script.java> args`, `scripts\AvRefs.java`, `scripts\DecompSite.java`,
`decA.txt` / `decA2.txt` / `decA3.txt` (PS4Media player), `decB.txt` (PS4MoviePlayer), `vt.py <slot> <n>` (relocated
vtable slots), `qw.py`, `fs.py <string>`, `paks.py <regex>` / `extract.py <pak path>`, and the extracted assets in
`assets\`. Callers come from `research-data\ghidra\bootres\callers.py`, rip/branch references from `riprefs.exe`,
disassembly from `edis.py`.
**Addresses are eboot image offsets** (Ghidra names them image + 0x1000000, `patches/MediEvil.xml` uses image + 0x400000).
**[V]** means verified statically (binary, assets or log). **[I]** means inferred (UE 4.21 knowledge or reasoning). None
of this was tested in game.

## Summary

- **The game never asks to stop the intro, and nothing in the port is missing.** Both movies play through the
  **PS4Media plugin** player (UE4 Media Framework, `IMediaPlayer` "PS4Media"/"PS4 AvPlayer") on **one persistent
  UMediaPlayer asset, `/Game/Movies/VideoPlayer`**. The intro level script `Level_AV_GameIntro` calls only `OpenSource`
  and `Play` on it, and handles `OnMediaOpened`. The widget `UI_VideoPlayer` that shows the movie ends it with its own
  timer (`MovieLength`, 180.0 s for the intro row of `MovieDataTable`) or with the skip button (`Skipped`, `EndMovie`,
  `SetMovieAndLoadLevel`), then loads the next level. No asset in that chain names `Close`, `Pause`, `Seek`,
  `SetRate` or `Rewind`, so no Blueprint there can call them. **[V]** for the name maps, **[I]** for the flow between the
  nodes.
- **sceAvPlayerStop / sceAvPlayerClose come from one place: the destructor of the player's AvPlayer object**
  (0x45a540: `sceAvPlayerStop(h); sceAvPlayerClose(h);`, return values ignored). It runs only when the last
  `TSharedPtr` to that object drops, which `IMediaPlayer::Close` (0x45dfb0) does. **[V]** The logo's Stop and Close are
  a side effect of `OpenSource(VIDEO_GameIntro)` on the same UMediaPlayer: the facade closes the current player (the
  logo), then opens the intro on a `TAsync` thread (0x46b0d0 → 0x45a710 → 0x45a820: Init, AddSource). The log shows it:
  Stop and Close at 7.243 s, `TAsync 0` created, intro Init at 7.246 s. **[V]** log and code, **[I]** the facade step.
  The logo is closed because the intro reuses its player, not because the logo ended (the logo is 5.06 s but was
  closed at 7.24 s, the length of the `OOELogo_Sequence` level sequence). **[I]**
- **Nothing reopens or closes `VideoPlayer` after the intro**, so its player keeps ticking. `TickFetch` (0x45eae0 →
  0x45b4c0) calls `sceAvPlayerGetVideoDataEx` and `TickAudio` (0x45ea70 → 0x45b170) calls `sceAvPlayerGetAudioData`
  every frame. The intro is decoded until EOF (197.2 s) even after the widget is gone. This is the game's own
  behaviour: the asset outlives the level change (no Close in the log, even at exit). On PS4 the hidden movie would
  play out the same way on the hardware decoder, where it costs almost nothing. **[V]** no stop call exists, **[I]**
  PS4 parity.
- **What the port does differently (secondary, not the cause):**
  1. At EOF the port sends `SCE_AVPLAYER_STATE_STOP` (`UpdateEndOfFileState`). The game maps STATE_STOP to
     `PlaybackSuspended` (0x45c5c0) and sets its state to Paused with rate 0 (`TickInput` 0x45f1d0). The game finds the
     end of a clip with its **own clock** (time += rate × dt ≥ duration → `PlaybackEndReached`, then
     `IsActive ? sceAvPlayerStop : state = Stopped`). If the port's STATE_STOP arrives before that clock reaches the
     duration, `OnEndReached` never fires. Nothing in the intro listens to it, so this does not matter here. **[V]**
     code, **[I]** the race.
  2. `sceAvPlayerStop` on a finished source returns `OPERATION_FAILED` ("already stopped"). The game ignores the
     value. **[V]**
  3. The suspected "IsActive stays true while audio is queued" is not the cause. The game only calls `IsActive` in
     its end-of-clip, seek, SetRate and SetLooping paths, never to decide whether to close. **[V]**
- **Recommended fix (port-side, no game files changed):** when the game changes level, quietly pause every AvPlayer
  that is still playing. Hook `UGameplayStatics::execOpenLevel` (**0x290ed50**, the native thunk for the Blueprint
  `OpenLevel` that `OverbiteGameInstance` uses; it calls `UGameplayStatics::OpenLevel` 0x209f690) with the port's
  existing guest-hook mechanism (`src/*_hook.h`). The hook calls a new port function that sets each playing
  `AvPlayerSource`'s pause flag (like `Pause()`: `m_is_paused = true`, `m_pause_time = now`). It sends no event,
  changes no state, and frees no buffer. `GetVideoData` / `GetAudioData` then return false, so the decoder threads fill
  the free buffers and block (the port decodes only when the game hands buffers back), and the demuxer idles. The game
  sees nothing: its clock still reaches 197 s, it finds `IsActive` true and calls `sceAvPlayerStop` itself, and a later
  `OpenSource` on `VideoPlayer` still closes the player normally. **[I]** (one launch to confirm, see the end of this
  page).
  - The alternative, a game data patch: edit `UI_VideoPlayer`'s `EndMovie` (or the level script) in the override pak
    to call `VideoPlayer.Close()`. This needs a new import and inserted Kismet bytecode, so it cannot be a same-size
    edit like `INFINITE_AMMO_EDITS`. It is higher effort and riskier. **[I]**

## 1. The two AvPlayer wrappers in the eboot

All AvPlayer imports and their call sites (`callers.py AvPlayer`) **[V]**:

| Import | PS4Media plugin (cluster A) | PS4MoviePlayer plugin (cluster B) |
|---|---|---|
| sceAvPlayerInit | 0x45a909 in 0x45a820 | 0x517e21 in 0x517b00 |
| sceAvPlayerPostInit | 0x45a9e8 in 0x45a820 | 0x517e91 in 0x517b00 |
| sceAvPlayerSetAvSyncMode | 0x45aa14 in 0x45a820 | - |
| sceAvPlayerAddSource / Ex | 0x45ae2a / 0x45b09b in 0x45a820 | 0x519a80 in 0x519500 |
| sceAvPlayerStreamCount, GetStreamInfo, Enable/DisableStream | 0x45f800 (TracksChanged) | 0x518150 (event callback) |
| sceAvPlayerStart | 0x45f314 (TickInput), 0x45f66b (SetRate), 0x45f7a1, 0x46230a (Seek) | 0x51820c (callback, on READY) |
| sceAvPlayerPause / Resume / SetTrickSpeed | 0x45f5d0 (SetRate) etc. | - |
| sceAvPlayerJumpToTime | 0x45f34c, 0x45f6a3/0x45f6fc, 0x45f7d8, 0x462339 | - |
| sceAvPlayerSetLooping | 0x4623bc in 0x462360, 0x46242c in 0x4623d0 | - |
| sceAvPlayerIsActive | 0x45f308, **0x45f541**, 0x45f795, 0x4622fe, 0x4623ac, 0x46241c | 0x5184d4, 0x519c10 |
| sceAvPlayerGetVideoDataEx | 0x45b681 in 0x45b4c0 | 0x51884d in 0x5184a0 |
| sceAvPlayerGetAudioData | 0x45b291 in 0x45b170 | 0x519f2a in 0x519e90 (audio thread) |
| sceAvPlayerCurrentTime | 0x45f3c7 / 0x45f404 in 0x45f1d0 | - |
| **sceAvPlayerStop** | **0x45a556 in 0x45a540**, **0x45f54d in 0x45f1d0** | 0x519437 in 0x5193b0 |
| **sceAvPlayerClose** | **0x45a55f in 0x45a540** | 0x517a0c in 0x517980 |

- **Cluster B is the PS4MoviePlayer startup-movie streamer** (the FAvPlayerMovieStreamer-like one). It builds
  `Movies/%s.%s` + `mp4` (0x3c43f78), opens its own audio port (`sceAudioOutOpen(0xff, …, 0x400, 48000, 1)` at 0x5182ab),
  starts a thread named `FAVPlayer_audio` (0x3c44075, next to `"PS4MoviePlayer"` at 0x3c44066), and calls
  `sceAvPlayerStart` from its event callback on STATE_READY (0x518150, `if (event == 2)`). **It is not used in these
  runs**: no `FAVPlayer_audio` thread in the log, and it never calls `SetLooping`, which both logged movies call. **[V]**
- **Cluster A is the PS4Media plugin `IMediaPlayer`.** Its player name is the FName `"PS4Media"` (0x45e2e0), its display
  name `"PS4 AvPlayer"` (0x3c36d06), and it checks for a `file://` prefix (0x3c35ed0). **It plays both the logo and the
  intro**: SetLooping is only here, and the intro's Init runs on the `TAsync 0` thread through 0x46b0d0 → 0x45a710 →
  0x45a820. **[V]**

### Cluster A layout (relocated vtable at 0x4e89638.., `vt.py 4e89640 80`) **[V]** slots, **[I]** names

| Slot target | Role | Notes |
|---|---|---|
| 0x45dfb0 | `IMediaPlayer::Close` | releases the AvPlayer object (+0x30/+0x38), resets state, sends TracksChanged(9) and MediaClosed(1) to the sink |
| 0x45dd90 (+ thunks 0x45ded0…0x45df80) | destructor | calls 0x45dfb0 |
| 0x45ea70 | `TickAudio` | → 0x45b170 → `sceAvPlayerGetAudioData` (only while rate == 1.0) |
| 0x45eae0 | `TickFetch` | → 0x45b4c0 → `sceAvPlayerGetVideoDataEx` |
| 0x45f1d0 | `TickInput` | drains the event queue, runs the clock, finds the end of a clip |
| 0x4622a0 / 0x462360 | `Seek` / `SetLooping` (IMediaControls) | |
| 0x45f5d0 (called from TickInput) | `SetRate` | Start / Resume / Pause / SetTrickSpeed |

The AvPlayer object (held by the player at +0x30) has the handle at +0x70. Its destructor 0x45a540 is reached only
through the shared-reference controller's DestroyObject at 0x46b0a0 (`add rdi,0x10; jmp 0x45a540`). There are no
direct callers. **[V]**

## 2. When the game calls Stop / Close

### 2a. The destructor (the only Close)

```c
// 0x45a540  (decA.txt)
if (*(long *)(this + 0x70) != 0) {        // AvPlayer handle
    sceAvPlayerStop(handle);               // 0x45a556, result ignored
    sceAvPlayerClose(handle);              // 0x45a55f
}
... release samples / pools ...
```

It runs when `IMediaPlayer::Close` (0x45dfb0) drops the last reference. The facade calls Close for
`UMediaPlayer::Close`, for a new `OpenSource`/`OpenUrl` on the same UMediaPlayer, and when the UMediaPlayer is
destroyed. **[V]** 0x45dfb0 / 0x46b0a0, **[I]** the facade side (UE 4.21 `FMediaPlayerFacade`).

### 2b. End of clip in TickInput (the second Stop)

The game finds the end with its own clock, not with an AvPlayer event (`edis.py 45f4f0 45f590`, decompile in decA.txt):

```c
// 0x45f1d0 TickInput, while state (+0x64) == 3 (Playing)
time(+0x68) += rate(+0x60) * DeltaTime;
if (time < 0 || time >= duration(+0x40)) {
    if (!looping(+0xa0)) {
        sink->ReceiveMediaEvent(5 /*PlaybackEndReached*/);   // 0x45f53c
        if (sceAvPlayerIsActive(h))                          // 0x45f541
            sceAvPlayerStop(h);                              // 0x45f54d
        else { rate = 0; state = 5 /*Stopped*/; time = 0; }  // 0x45f554
        sink->ReceiveMediaEvent(7 /*PlaybackSuspended*/);
    } else time %= duration;
}
```

For the logo, the log has exactly one `Stop` line followed by `Close`, the destructor's pair. So at 0x45f541 the port
answered `IsActive = false` (the source had reached EOF and drained), or the clock never got there. **[V]** log.

### 2c. The event callback (eventReplacement, registered in 0x45a820)

Init data built at 0x45a820 **[V]**: memory replacement 0x18e1e20 / 0x18e20a0 / 0x18e1f60 / 0x18e2280, file replacement
0x45c4d0 / 0x45c4f0 / 0x45c510 / 0x45c590 (object = player), **event callback 0x45c5c0** (object = player),
debug level 0, priority 700, `num_output_video_framebuffers` = argument, `auto_start` = 0.

The callback 0x45c5c0 only queues Media Framework events (EMediaEvent). TickInput applies them on the game thread.
**[V]** mapping, **[I]** the EMediaEvent names (UE 4.21 order):

| AvPlayer event | Queued | TickInput effect |
|---|---|---|
| 1 STATE_STOP | 7 PlaybackSuspended | rate 0, state Paused (2) |
| 2 STATE_READY | 9 TracksChanged, 3 MediaOpened | streams and duration (0x45f800), then state Stopped (5) |
| 3 STATE_PLAY | 6 PlaybackResumed | state Playing (3), time = max(time, CurrentTime) |
| 4 STATE_PAUSE | 7 PlaybackSuspended | Paused |
| 5 STATE_BUFFERING | 0 MediaBuffering | buffering flag |
| 0x20 WARNING 0x806a00a1 (LOOPING_BACK) | 5 PlaybackEndReached | Stopped unless looping |
| 0x20 WARNING 0x806a00a3 (JUMP_COMPLETE) | 8 SeekCompleted | time = CurrentTime |
| 0x20 WARNING 0x806a0004 / 0x40 DRM | 4 MediaOpenFailed | Error |

**No event leads to Stop or Close.** STATE_STOP is treated as a pause. **[V]**

## 3. What the intro and the skip do (the game's Blueprints)

Assets read from `pakchunk0-ps4.pak` (`paks.py`, `extract.py`, then the name maps). **[V]** names, **[I]** graph:

- `/Game/Movies/VideoPlayer`: a `MediaPlayer` asset (PlayOnOpen set, a PlayerGuid). `VIDEO_OOE_Logo` and
  `VIDEO_GameIntro` are `FileMediaSource`s. `T_VideoPlayer` (MediaTexture) and `M_VideoPlayer` show it.
- `Maps/Level_AV_GameIntro` (level script): imports `VideoPlayer`, `VIDEO_OOE_Logo`, `VIDEO_GameIntro`,
  `OOELogo_Sequence`, `Level_AV_GameIntro_Sequence`, `UI_VideoPlayer`, `UI_SIEPresentsLogo`. The only MediaPlayer
  functions it names are **`OpenSource`, `Play`, `OnMediaOpened`** (events `OnOOEMovieOpened`, `OpenedUrl`,
  `PlayGameIntro`, `EndMovie`, `Skipped`, `CinSkipped`, `LoadLevelFromCinematic`).
- `UI/Movies/UI_VideoPlayer` (widget): `Tick`, `MovieLength`, `IsInputKeyDown(Gamepad_FaceButton_Right)`, `Skipped`,
  `EndMovie`, `SetMovieAndLoadLevel`, `LoadOnFinish`, Wwise `CIN_SE_AV_GameIntro_Skip` / `_Complete`. **No MediaPlayer
  function at all** (not even OpenSource).
- `Movies/VideoPlayerManager/MovieDataTable`: the `VIDEO_GameIntro` row has `Length` = 0x43340000 = **180.0 s** (the file
  is 197.2 s).
- `OverbiteGameInstance` uses `OpenLevel`. `OverbiteGameInstanceFunctions` has `LoadLevelFromCinematic` (its `Pause` /
  `IsPlaying` belong to sequences and AI, since it imports nothing from `/Script/MediaAssets`).
- Searched in all of these: `Close`, `Pause` (as a media call), `Seek`, `SetRate`, `Rewind`, `OpenUrl`, `OnEndReached`.
  **None of them appears.**

So the skip **does nothing to the player**. It ends the widget (Wwise skip events, `EndMovie`) and loads the next
level. The intro's player keeps running hidden until EOF, or until something else is opened on `VideoPlayer`. **[V]**
no call exists, **[I]** the order of the nodes.

Media files (mp4 `mdhd`) **[V]**: `gameintro.mp4` has video 30000/5916000 = 197.2 s and AAC 48 kHz, 197.248 s.
`ooi_logo_plain_black.mp4` has 5.005 s of video and 5.056 s of audio.

## 4. The port compared

| Game expects (PS4Media) | Port does (`avplayer_state.cpp`, `avplayer_source.cpp`) | Effect |
|---|---|---|
| READY after AddSource → TracksChanged, MediaOpened | controller thread: `FindStreams` → `SetState(Ready)` → StateReady | matches the log (AddSource 7.331, SetLooping 7.352, Start 7.353) **[V]** |
| Start → STATE_PLAY → PlaybackResumed | `Start()` emits StatePlay synchronously | OK |
| End: game clock ≥ duration, then `IsActive ? Stop : Stopped` | at demux EOF `OnEOF` → EndOfFile. When drained (`IsActive` false), `UpdateEndOfFileState` emits **StateStop** | game queues PlaybackSuspended → Paused. If that comes before its clock reaches the duration, PlaybackEndReached is lost. Harmless here (nobody listens) **[I]** |
| Stop on a finished player | `AvPlayerSource::Stop` returns false → `OPERATION_FAILED` + "already stopped" | ignored by 0x45a540 **[V]** |
| `IsActive` | `state != Stop/Error && (!eof or any queue non-empty)` | used only in end-of-clip, seek, SetRate, SetLooping. Not related to closing **[V]** |
| Hidden playback until EOF | decodes as long as the game hands back buffers (`GetVideoData` pops a frame, frees its buffer) | full 1080p software decode and upload for ~197 s. PS4 does the same in hardware **[I]** |

Note on the log: this `last_run.log` run lasted about 170 s after the intro's Start (34 five-second `Perf:` lines after
line 330), so it does not reach the intro's natural end. The "no Stop at the end" claim needs a run of more than 205 s.
**[V]**

## 5. Recommended fix

**Port-side "pause stale movies on level change"** (one change, one launch):

1. In the port's AvPlayer: keep a registry of live `AvPlayerState`s (add in the constructor, remove in the
   destructor, under a mutex). Add `AvPlayerSuspendAllForLevelChange()`. For every player in state `Play` (or
   `Buffering`) it calls a new `AvPlayerSource::Suspend()` that sets `m_pause_time = now; m_is_paused = true` (what
   `Pause()` does) and nothing else. No `SetState`, no event, no buffer released. A buffer the game's render thread
   may still be converting therefore stays valid. **[I]**
2. Hook the guest `UGameplayStatics::execOpenLevel` at **0x290ed50** (or `UGameplayStatics::OpenLevel` at 0x209f690)
   with the existing hook mechanism (`src/cheat_hook.h` style: signature check, rel32 to a stub). The stub calls the
   function above, then runs the original code. 0x290ed50 is the function paired with the name `"OpenLevel"` in the
   UGameplayStatics native table at 0x519f930 (names in alphabetical order: `MakeHitResult`, `OpenLevel`,
   `ParseOption`, …). Its body parses the parameters and calls 0x209f690. **[V]** table and calls, **[I]** that every
   level change of this game goes through it.
3. Add one trace line per suspend (`AvPlayer … suspended at OpenLevel (N frames delivered)`) so the first launch shows
   that it fired on the skip.

Why this is safe **[I]**: while paused, `GetVideoData` / `GetAudioData` return false (`!IsActive() || m_is_paused`), so at
most `num_output_video_framebuffers` more frames get decoded before the decoder threads block on `m_video_buffers` (and
the audio thread on its buffers). The demuxer sleeps once 30 video and 8 audio packets are queued. The game keeps its
own clock: at 197 s it sees `IsActive` true and calls `sceAvPlayerStop` itself (logged), and the MediaTexture keeps its
last frame. A later `OpenSource` on `VideoPlayer` closes the player through 0x45a540 as usual. No movie in this game
needs to keep playing across `OpenLevel`: every cinematic goes through `UI_VideoPlayer`, which ends the movie before it
loads the level.

Not recommended:
- Making `IsActive` false or emitting StateStop on the skip: the port has no AvPlayer-level signal for the skip.
- Freeing the source on the hook (`Stop()`): it releases the current frame buffer under the render thread.
- The pak Blueprint edit (`EndMovie` → `VideoPlayer.Close`): it works on PS4 terms, but it needs Kismet bytecode
  insertion and a new import, and it changes a game asset.

Optional, separate: send STATE_STOP at EOF only after the game's clock could have reached the duration (or not at
all, since the game ignores STOP for the end), so that `OnEndReached` fires reliably. That matters for any Blueprint
that listens to it. MediEvil's intro does not.

## 6. Open questions

- Does the skip go through `OpenLevel` (`SetMovieAndLoadLevel` → `LoadLevelFromCinematic` → game instance
  `OpenLevel`)? The trace line in step 3 answers it on the first launch. If it does not, hook the level-change function
  it does use (`UEngine::Browse` / `LoadMap`; the shipping build has no `LoadMap: %s` log string). **[I]**
- Why the UMediaPlayer asset is never garbage-collected after the level change (no Close even at exit): probably a
  class reference that stays loaded (`UI_VideoPlayer` → `M_VideoPlayer` → `T_VideoPlayer` → `VideoPlayer`). Not
  checked. **[I]**
- Whether real PS4 libSceAvPlayer sends STATE_STOP at EOF. If it does, the PS4 has the same race between STATE_STOP
  and the game's own clock. **[I]**
- A run of more than 205 s after the intro starts, to confirm the end-of-clip path (a `Stop` from 0x45f54d or none).
