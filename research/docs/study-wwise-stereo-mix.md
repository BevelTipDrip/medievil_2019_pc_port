# Study: can Wwise mix in stereo on the Steam Deck to cut `AK::EventManager`'s CPU?

Status: study, 2026-11-05, on `deck-lab` (`4c56d21`). Read only: nothing built, launched or committed. Parent:
[study-deck-other-threads.md](study-deck-other-threads.md) §1 (Wwise at ~28-31% of a CPU in gameplay on the Deck).

Inputs:
- the port's audio HLE (`src/runtime_audio.c`);
- both eboots, disassembled with capstone 5.0.7 under `py -3.14 -I`: 01.00 (`eboot-0100.elf`, sha256 `a87ae3b3…3b94`)
  and 01.02 (`eboot-0102.elf` = `out/eboot.elf`, sha256 `31074869…a69b`);
- the game's pak data (`Wwise.uplugin`, `WwiseAudio/PS4/Init.bnk`, `Init.txt` / `.xml`, `PluginInfo.xml`,
  `Content/Localization/Game/en/Game.locres`, the config inis);
- spacepepperoni's Linux fork (`forks/medievil_ps4_Linux`, commits `58a7e18`, `03209a3`, `401762b`).

Scripts are in the session scratchpad, `wwise-stereo\`: `ld.py`, `wdis.py`, `callers.py`, `xr.py`, `map0100.py`,
`grepstr.py`, `paks.py`.

Marks: **[code]** read in this tree or in the eboot disassembly. **[log]** a line from a log in `data/`.
**[msg]** a commit message or measurement by another fork. **[inferred]** reasoning, not checked. Nothing here is
"verified" in the project's sense.

## Answer

1. **Wwise already mixes in stereo, on the Deck and on every PC.** The 28-31% is already the cost of a stereo mix.
   No stereo saving is left to take.
   - The question's premise, and §1.5 of the parent study ("Wwise mixes 7.1 because the guest opens an 8-channel
     port"), are wrong.
   - Wwise's PS4 "System" sink always opens an **8-channel float port** (format 7, `FLOAT_8CH_STD`). It then calls
     `sceAudioOutGetPortState` once and takes **`state.channel`** as the main mix layout ([code], §2).
   - The port reports `channel = 2` for every port with more than 2 channels (`src/runtime_audio.c:300`, unchanged
     since the first commit). So Wwise picks `0x3102` (stereo, FL FR).
   - The whole bus tree runs at stereo: the busses, the reverb aux busses, and the master limiter. Init.bnk has no
     bus with its own channel layout ([inferred], §3).
   - Wwise writes each frame as `L R 0 0 0 0 0 0` ([code], §2.3). It also sets the port volume of channels 2-7 to 0
     ([code], §2.2).
   - So the port's 7.1-to-stereo downmix only folds zeros.
2. **What decides the layout:**
   - The game leaves `AkInitSettings.settingsMainOutput.channelConfig` at 0 ([code], §2.4). The menu has only FX,
     Music and Speech volume ([code], §4).
   - So the only input is the `channel` byte from `sceAudioOutGetPortState` on the main port, read once at sink
     init:

     | `channel` reported | Wwise does | Effect |
     |---|---|---|
     | 2 | stereo | today |
     | 6 | 5.1 | |
     | 8 | 7.1 | |
     | 1 | keeps 7.1 | |
     | 3, 4, 5, 7 | sink init fails | the "No Output" sink: silence |

   - `sceAudioOutGetSystemState` and the other speaker queries are not imported.
3. **Recommended change for the Deck's CPU: none on the output layout.** Two guard rails instead:
   - **Do not port the fork's `58a7e18`** (report 8 channels) to `deck-lab`. It makes Wwise mix 7.1: per-voice pans
     to 8 outputs, and reverbs and limiter on 8 channels. The guess is +25-60% on `AK::EventManager`, about +8-18% of
     a CPU on the Deck ([inferred], §5).
   - **Never report 1, 3, 4, 5 or 7.**
   - Smaller items:
     - The port spends ~0.5-1 ms/s per port volume-scaling and downmixing channels that are zero. It could pass L/R
       straight through when channels 2-7 are muted, saving <0.2% of a CPU ([inferred]). This is not worth a launch
       on its own.
     - The real Wwise levers are elsewhere: the software ATRAC9 decode and the voice count (parent §1.3).
4. **PCs with a 5.1/7.1 device.** Today they also get the stereo mix, on the front pair only; the other six
   channels are silent ([code] + [inferred]). Real surround would mean reporting 6 or 8, gated on the SDL device's
   channel count. That is a fidelity feature for upstream with its own A/B. It costs CPU, not saves it. Defaults on
   Windows must not change without one.
5. **Confidence: high** that the current port gets a stereo mix:
   - the disassembly of both builds is unambiguous;
   - the fork heard the change when it reported 8 instead of 2 [msg].

   Unverified:
   - no log line yet shows the volume array Wwise sets (one cheap log line would prove it, §6);
   - the 7.1 cost estimate is not measured.

## 1. Every audio-related import and how the port implements it [code]

Both builds import the same set. 01.02 import stubs are shown (01.00 in brackets where they matter).

| Import(s) | Who calls (01.02) | Port implementation |
|---|---|---|
| `sceAudioOutInit` | Wwise sink 0x351845f; UE4 0x518075, 0x6bb5e0, 0x6c53e0 | `audio_init`, `runtime_audio.c:91` |
| `sceAudioOutOpen` | Wwise 0x35184ce [0x351570e]: type 0, 512 frames, format 7 (FLOAT_8CH_STD). UE4 NGS2 audio device 0x6bab1a [0x6b81ba]: type 0, 256 frames, 8ch float = log "port 0". UE4 0x6c64d5 [0x6c3b75]: format 5, the AudioMixerAudioOut module, off (`EnableAudioMixer=false`) [inferred]. UE4 0x5180bb / 0x519fc5 [0x5182ab / 0x51a1b5]: format 1, S16 stereo, 1024 frames (media audio [inferred]) | `audio_open`, `:98-148`. Format to channels `{1,2,8,1,2,8,8,8}`. An 8ch port on a stereo/mono device goes to SDL as 2ch (`:127-130`) |
| `sceAudioOutGetPortState` | Wwise only: init 0x35184f2 [0x3515732]. Per-buffer polls 0x3518cc7, 0x3518d64, 0x3518e2f and query 0x3518f3e [0x3515f07, 0x3515fa4, 0x351606f, 0x351617e], **only for the BGM/DVR-bypass sink** (port type 1) | `audio_state`, `:289-303`. Main/BGM: `output=0x01`, `channel=min(channels,2)`. Voice/personal: 0x40/1. Pad: 0x04/1 |
| `sceAudioOutSetVolume` | Wwise 0x3518594 [0x35157d4], once at init; UE4 0x6bab84, 0x6c64fc | `audio_volume`, `:274-288` (per-channel gain applied in `output_port`, `:207`) |
| `sceAudioOutOutputs` | Wwise thread `AK::LibAudioOut` 0x35187fe [0x3515a3e] | `audio_outputs`, `:265-273` |
| `sceAudioOutOutput` / `sceAudioOutClose` | UE4 (0x51xxxx, 0x6bxxxx, 0x6c6xxx); Wwise closes at 0x351884b, 0x3518ac6, 0x3518c20 | `:263`, `:149` |
| `sceAudioOutGetLastOutputTime` | not imported | exported anyway (`:304`) |
| `sceAudioOutGetSystemState`, `sceAudioOutSetMainOutput`, HDMI/speaker info calls | **not imported** | none |
| `sceAjm*` (13) | Wwise's codec path | `src/runtime_ajm.c` (LibAtrac9, FFmpeg MP3, synchronous on the caller) |
| `sceNgs2*` (14) | UE4's NGS2 audio device | `src/runtime_extra.c:566+`, a silent renderer (port 0 gets zeros) |
| `sceAvPlayerGetAudioData` | movies | `gpu/shadps4/core/libraries/avplayer/avplayer.cpp` |
| `sceVoiceQoS*` (12) | voice chat | `src/runtime_extra.c:511+`, no-ops |
| `sceSystemService*` (ParamGetInt, GetStatus, ...) | system queries; none audio-related | `src/runtime_services.c:562` |

The fork's study [study-fork-spacepepperoni.md](study-fork-spacepepperoni.md) row `58a7e18` already flagged this
call as decisive. This study supplies the code behind it.

## 2. Wwise's PS4 sink: how the layout is chosen [code]

Wwise version: **2018.1.5 build 6835, UE4 integration for 4.21** (`Engine/Plugins/Runtime/Wwise/Wwise.uplugin`:
`"VersionName" : "2018.1.5.6835.1218"`). Banks are `SoundbankVersion="132"` (Init.xml; `BKHD` version 0x84).
The project is `MediEvil_Wwise_Project` (PluginInfo.xml). Its plug-ins are Silence, McDSP ML1, Parametric EQ,
Delay, RoomVerb, Convolution Reverb, Meter, Time Stretch, Pitch Shifter, Harmonizer, Reflect, plus the sinks "System"
(0xAE0007) and "No Output" (0xB50007).

Wwise code sits **0x2dc0 lower in 01.00** than in 01.02 (every site below matched by a masked byte window,
`map0100.py`). Addresses are 01.02 [01.00].

### 2.1 Registration and creation

- Sink plug-ins registered at 0x34da250 [0x34d7490]:
  - IDs 0xAE System, 0xAF DVR bypass, 0xB0 communication, 0xB1 personal, 0xB3, 0xB4 aux all use the factory
    0x35181e0 [0x3515420];
  - 0xB5 "No Output" has its own factory.
- The core's device creation, 0x34a8900:
  - takes shareset 0 as the System sink (`mov [dev+0x18], 0xae0007`);
  - builds `AkAudioFormat` at `rbp-0x40`: `uSampleRate = 48000`, **`channelConfig = [dev+0x64]`** (the device's
    `AkOutputSettings.channelConfig`) (0x34a8a22-0x34a8a4a [0x34a5c62]);
  - calls the sink's `Init(alloc, ctx, params, io_rFormat)` through vtable +0x38 at 0x34a8ac7.

### 2.2 `CAkPS4Sink::Init`, 0x35182e0 [0x3515520]

```
3518347  switch on (sink plugin id >> 16) - 0xAE
           0 System  : port type 0 (MAIN), open config ebx = 0x63f108 (7.1)
           1 DVR     : type 1 (BGM), 7.1, attr bit 16
           2,3,6     : types 2, 3, 127, stereo 0x3102     4: type 4 (PADSPK), mono 0x4101
3518480  open format from (ebx & 0xf): 1 -> 3 (FLOAT_MONO), 2 -> 4 (FLOAT_STEREO), 8 -> 7 (FLOAT_8CH_STD)
35184ce  sceAudioOutOpen(user, type, 0, frames, 48000, format | attr)          -> port 1 in the log [log]
35184ee  [this+0x44] = ebx (7.1)
35184f2  sceAudioOutGetPortState(port, &st)                                     ; st.output u16, st.channel u8
35184fc  if (st.output & 0x80) && st.channel == 0: st.channel = 8 (the open config's count)
3518514  if st.output == 0 && type == BGM: notify (0x3483e20)
3518525  n = io_rFormat.channelConfig.uNumChannels ([r8+4], byte); if n == 0: n = st.channel
3518530  switch n (table 0x3f7821c [0x3f7531c]):
           2    -> [this+0x44] = 0x3102 (FL FR), port volumes ch2..7 = 0           (0x351854f [0x351578f])
           6    -> [this+0x44] = 0x60f106 (5.1 with sides), port volumes ch6,7 = 0 (0x351856e [0x35157ae])
           1, 8 -> keep [this+0x44] = 7.1                                          (0x351857f [0x35157bf])
           3,4,5,7 -> return AK_Fail                                               (0x35185a7 [0x35157e7])
358d     io_rFormat.channelConfig = [this+0x44]                                     ; the main mix layout
3594     sceAudioOutSetVolume(port, 0xff, vols)        ; vols start at 0x8000 (0 dB) x 8, rodata 0x3f781e0
```

- **The main port's state is read only here, once.** The per-buffer `GetPortState` calls (0x3518c90 / 0x3518d40 /
  0x3518dc0: PassData / PassSilence paths, and 0x3518ee0) are behind `cmp [this+0x1c], 1`: the BGM port only.
  They watch `output == 0` (DVR bypass disconnected). Nothing polls the main port for device changes.
- A failed Init (AK_Fail) makes the core fall back to the dummy sink (0x34a8b08-0x34a8b35 sets 0xB50007; dummy
  Init 0x34da120 [0x34d7360] defaults to stereo). The result is silence from the game.

### 2.3 What reaches the port

- The sink's write buffer is declared with the **open** layout: `[this+0x50] = ebx = 0x63f108`, 8ch × 512 frames
  (`NextWriteBuffer` 0x3518fb0 [0x35161f0]).
- The base `Consume` 0x34da030 [0x34d7270] calls the mix-to-output dispatcher 0x3504820 [0x3501a60]. The
  dispatcher keys on the input mask (main mix) and the output mask (port).
- **Stereo in, 7.1 out** is 0x3504fa0 [0x35021e0]. Per frame it stores `L R 0 0` and `0 0 0 0` (`vinsertps …, 0x9c`
  zeroes lanes 2-3; `xmm2 = 0` for the upper four).
- The `AK::LibAudioOut` thread (0x3518730 [0x3515970]) hands the ring slot to `sceAudioOutOutputs`.
- The port then:
  - multiplies channels 2-7 by volume 0;
  - runs `downmix_stereo` (`:173-189`), whose centre and surround terms are zero, so it outputs
    `soft_limit(L)`, `soft_limit(R)`.

### 2.4 The game's init settings

UE4's `FAkAudioDevice` init, 0x521789-0x52185f [0x521979-0x521a4f]:
- zeroes `initSettings+0x30..0x3f` (`vmovups [rbp-0xe0], xmm0`; `initSettings` at `rbp-0x110`);
- calls `AK::SoundEngine::GetDefaultInitSettings` 0x3445760 [0x34429a0]. That clears `settingsMainOutput` (+0x30:
  shareset, idDevice, ePanningRule, channelConfig) through 0x348fb90 (`vmovups [rdi], 0`);
- writes `initSettings+0x60` (1), `+0x68` (a callback), `+0x70`, and one platform field (`0x8000000`);
- calls `SoundEngine::Init` at 0x52185f.

Nothing writes `settingsMainOutput`. So `io_rFormat.channelConfig` is 0 and **the port state decides**. Panning
rule 0 is "speakers".

## 3. The bus tree follows the main mix [inferred]

- `Init.bnk` (37,338 bytes): `HIRC` with 91 busses, 72 aux busses, 2 audio devices ("System", "No Output"), 64
  custom effects and 11 effect sharesets.
- It contains no AkChannelConfig word for stereo (0x3102), 5.1 (0x3F106 / 0x60F106), 7.1 (0x63F108), 5.0, 7.0,
  4.0 or mono. So no bus has a fixed layout: every bus is "defined by parent" and runs at the main mix layout,
  stereo today. This is a byte search, not a full HIRC parse.
- The effects that scale with the channel count (from `Init.txt`):
  - RoomVerb on most `Verb_*` aux busses (~50);
  - Convolution Reverb on 12 (AudioEase IRs: LongSixWideEchoes, DistantEcho, ForestAuterlitz, LongAndComplex);
  - Delay;
  - Reflect (aux bus `Reflect`);
  - Meter (the ducker side-chains);
  - McDSP ML1 (limiter).

## 4. No game or project setting overrides it [code]

- Game options: `Game.locres` (en) has `PMENU_AUDIO` with `FX Volume`, `Music Volume` and `Speech Volume` only.
  There is no speaker, output or dynamic-range option.
- Config: `OVERBITE/Config/DefaultEngine.ini` `[Audio] MaxChannels=224` (UE4 sources, not Wwise). Neither
  `PS4Engine.ini` nor `DefaultGame.ini` has an `AkSettings` section. `Engine/Config/PS4/PS4Engine.ini`:
  `AudioDeviceModuleName=Ngs2` (UE4's own silent device, port 0).
- The Wwise 2018.1 integration has no PS4 initialization-settings object; that came in 2019.1 [inferred].
  `SetPanningRule` and `SetSpeakerAngles` exist only as Blueprint functions (strings at 0x3c4daac, 0x3c4dabb). They
  do not change the layout.

## 5. CPU: what scales with the output channel count [inferred]

Inside Wwise's audio frame (512 samples, 93.75/s):

| Work | Scales with | 2ch to 8ch |
|---|---|---|
| ATRAC9 decode (port, software, on the calling thread) | voices × source channels | 1x |
| Resampling / pitch, voice LPF/HPF, voice-level effects | voices × source channels | 1x |
| 3D panning and mixing of each voice into its bus | voices × source channels × **output channels** | ~4x |
| Bus mixing, volume ramps, ducking meters | busses × **bus channels** | ~4x |
| RoomVerb (FDN shared, per-output decorrelation and tone filters), Convolution (one FFT convolution per output channel), Delay, Reflect, ML1 | **bus channels** | 2.5-4x |
| Interleave to the port | **output channels** | 4x (but `L R 0…` is already 8-wide) |

A typical game frame splits about half and half between the per-voice part and the per-channel part. On that
guess, 7.1 would cost +25-60% on `AK::EventManager` over today's stereo. With today's ~28-31% of a CPU (parent §1.2),
7.1 would land at ~35-50% of a Deck CPU. **That is the cost the current `channel = 2` already avoids.** The
reverse, below stereo, is not reachable: `channel = 1` gives 7.1 (§2.2).

How to measure, if ever wanted:
- `research/tools/deck.sh threads 10` in the same gameplay spot. It gives utime+stime per thread for
  `AK::EventManage` (comm is cut at 15 characters) and `AK::LibAudioOut`.
- Use `cut -d' ' -f14,15` separately to split user and system time.
- Compare with the parent's 28-31% (wait-site upper bound).

## 6. Candidate changes

| Change | What Wwise does | Risk | Verdict |
|---|---|---|---|
| A. Keep `channel = 2` (today) | stereo mix, `L R 0…`, ch2-7 muted | none | **Keep.** Already the cheapest layout |
| B. Report 8 (fork `58a7e18`) | 7.1 mix; surrounds carry content (delayed copies, [msg] `03209a3`) | more CPU; the port must fold surrounds (the fork lowered them to 0.35, then to "off" by default, `401762b`) | **Not on the Deck.** On a PC only behind a gate (C) |
| C. Report the SDL device's count: 2 if ≤2, 6 if 6, 8 if 8, else 2 | 5.1/7.1 mix on real surround devices only | PC surround becomes real (wanted, but must be heard: centre dialogue, LFE, rear reverb); the 5.1 PS4 layout is FL FR FC LFE SL SR, so with std format 7 the port must place 6ch content in its 8ch stream correctly | **Separate PC feature**, opt-in first. Not a Deck CPU item |
| D. Report 1, 3, 4, 5, 7 | 1 gives 7.1; the others fail Init, then silence | high | **Never** |
| E. Change the format of the 8ch open | the sink fails or mis-sizes its ring (the open config also sizes the buffer, 0x3518dc0) | high | **No** |
| F. Patch guest code (`[this+0x44]`, the jump table) | nothing to gain over A | — | No |
| G. Port side: when the port's channels 2-7 are at volume 0, skip the 8ch loop and the downmix, copy L/R | same output (bit-exact except the soft limiter, which stays) | low | Optional, <0.2% of a CPU; bundle it with other audio work |

A restart-only switch for a Deck A/B exists in the fork's form (`GOW3_AUDIO_PORT_CHANNELS=8` reverts to 7.1). It
would be for measuring only; the state is read once at sink init.

## 7. Recommended Deck test (one launch, optional)

The answer does not need a launch. If the user wants the claim confirmed, **one launch** with one diagnostic line
does it. No behaviour change.

1. Change, not made here: `audio_volume` prints the port id, `flags` and `volume[0..7]` the first time each port
   is set. Expected for port 1: `flags 0xff, 32768 32768 0 0 0 0 0 0` (stereo mix). `32768 ×8` would mean 7.1, and
   `×6, 0, 0` 5.1.
2. Launch on the Deck as usual, through the graveyard. Listen for nothing new: sound must be unchanged.
3. Measure: `deck.sh threads 10` at the parent's graveyard spot, for a baseline of `AK::EventManage` /
   `AK::LibAudioOut` utime+stime at today's (stereo) layout.
4. Optional second launch, only if the 7.1 cost is wanted as a number: `GOW3_AUDIO_PORT_CHANNELS=8` (needs the
   fork's 4-line switch). Expect `AK::EventManage` higher and a "doubled" sound on the Deck speakers [msg]. Then go
   back.

## 8. Corrections to earlier docs

- [study-deck-other-threads.md](study-deck-other-threads.md):
  - §1.3, "7.1 to stereo downmix": the downmix runs, but over zeros. The input is Wwise's stereo mix in an 8ch
    frame.
  - §1.5: the cause is wrong. Wwise mixes stereo because the port reports `channel = 2`, not 7.1 because of the
    8ch open.
- `runtime_audio.c:120-124` (upstream 1.3.4, issue #14): "its surround reverb mixed oddly". On this port the
  surrounds carry no signal (§2.3). The fix's effect was the level: SDL's 7.1-to-2 downmix attenuated the fronts.
