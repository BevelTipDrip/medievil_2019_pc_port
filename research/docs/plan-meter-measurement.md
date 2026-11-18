# Measuring the game's output with the user's colorimeter (plan, 2026-11-17, not built)

Parent: [plan-hdr-sdr-match.md](plan-hdr-sdr-match.md) (steps 3-4: test chart, calibration) and [study-hdr.md](study-hdr.md)
H28 (the SDR image and its presentation verified in a capture; a washed-out default still unexplained). The user has an
X-Rite ColorMunki Display (i1Display Pro family) and HCFR. Research agent, 2026-11-17, read the HCFR source
(github.com/arthur-liberman/hcfr-code) and ArgyllCMS docs; nothing that talks to the meter was run. [V] source / docs,
[I] inferred.

## What is on the machine

- HCFR (`C:\Program Files (x86)\HCFR Calibration`) links Argyll's instrument library itself (`ArgyllMeterWrapper.cpp`):
  emissive spot mode, i1d3 integration 0.4 s (refresh mode: whole refresh periods), optional low-light averaging
  ("Adapt": 5 samples < 1 cd/m2, 4 < 2, 3 < 5, 2 < 10). It runs Argyll's `dispwin` only to load a linear video LUT. [V]
- Argyll binaries: `HCFR\Tools\spotread.exe` 1.8.3, `dispwin.exe` 1.7.0; DisplayCAL's Argyll 2.1.2
  (`%APPDATA%\DisplayCAL\dl\Argyll_V2.1.2\bin`). Current: 3.5.0 (Feb 2026); 3.0.0 fixed low-level red readings of the i1d3
  on OLED ("0x83" error). Use 3.5.0 for scripted readings. [V]
- A display correction for this screen: `HCFR\Etalon_Argyll\i1 DisplayPro, ColorMunki Display & Dell, AW3225QF (i1 Pro
  2).ccmx` (fit dE2000 0.026). spotread takes it with `-X`; Argyll lists installed ones from `C:\ProgramData\ArgyllCMS`.
  [V]

## Readings from a script

`spotread -e -x -O -X "<ccmx>"` (`-e` emissive cd/m2, `-O` one reading and exit, for scripting; `-Y n` non-refresh;
leave adaptive integration on for near-black). Parse `Result is XYZ: X Y Z, Yxy: ...`. Each `-O` reopens the meter
(~1-2 s) [I]; faster: one spotread kept open in a pseudo-console (pywinpty / wexpect, as DisplayCAL does), a key per
reading. Averaging: repeat readings per HCFR's Adapt rule. [V spotread doc]

## HCFR's conventions (to match its numbers)

- 10% window (side sqrt(0.1) x screen), black background (or an APL-keeping background), 8-bit codes, video range
  16-235 by default (`RGB_16_235=1`; full range `floor(p*255+0.5)`). [V]
- Timing: 80 ms after the pattern, then 300 ms + 100 ms, then one reading (~480 ms). Optional bright-frame insert
  against ABL: 50% for 500 ms every 10 patterns. [V]
- Greyscale 0-100% in 10% steps (full range 0, 26, 51, 77, 102, 128, 153, 178, 204, 229, 255); near-black 0-4% (SDR),
  0-8% in 2% (ST2084); saturations 0/25/50/75/100% for R G B C M Y. HDR: the same signal percentages as PQ codes
  (10% steps = 0, 0.33, 2.4, 10.2, 32.5, 94, 244, 610, 1555, 3835, 10000 cd/m2), targets with BT.2390 roll-off. [V / I]
- PGenerator protocol (HCFR's network pattern generator, `Generator.cpp`, `RB8PGenerator.dll`): UDP "Who is a
  PGenerator" to port 1977, then TCP 85; messages end `\x02\x0D`, each answered `OK\x02\x0D`; `CMD:GET_RESOLUTION`,
  `RGB=RECTANGLE;w,h;100;r,g,b;bgR,bgG,bgB;-1,-1,x,y` per patch, `QUIT`. 8-bit only. Loopback discovery untested. [V/I]

## Plan

1. **The game as a pattern generator** (a local socket; the PGenerator command subset plus our own): a patch window on
   black at a requested value, answered only once a frame showing it was presented. Two injection points:
   - **raw**: written into the final frame before presentation: SDR code values (8 / 10-bit), HDR PQ codes or nits.
     Measures the port's output, Windows, the GPU and the display.
   - **pipeline**: a scene-linear value through the game's own grading LUT (looked up as its tonemapper does: log index
     in SDR, PQ index x 1.05 in HDR), then the UI composite and presentation. Measures the game's chain at neutral
     settings against its maths (the offline harness gives every patch's target).
   The game keeps rendering while unfocused (the script or HCFR is the foreground window).
2. **A Python script**: drives the socket and spotread 3.5.0 with the ccmx; HCFR's timing and levels; writes CSV and a
   report (luminance against target, EOTF / gamma, dE2000).
3. **References**: SDR: HCFR's own GDI generator full screen on the same display (or the script's raw patches, which go
   through the same swapchain). HDR: not the Windows HDR desktop (it maps SDR); the raw PQ path, cross-checked once
   with madTPG HDR or HCFR's NVIDIA HDR10 passthrough.
4. Optional: HCFR itself as the client (PGenerator), for its reports (8-bit only).

User setup: a fixed AW3225QF mode (HDR 400 True Black or Peak 1000); Windows HDR on for HDR runs, off for SDR; Auto HDR,
Night light and automatic colour management off; dark room, 15 min warm-up, meter centred on the window.
