// SPDX-License-Identifier: GPL-2.0-or-later
// gow3: test patterns for measuring the port's output with a colorimeter (research/docs/plan-meter-measurement.md).
// A local socket (GOW3_PATTERN_SERVER) asks for a patch window; the presenter and the renderer draw it:
//   raw:  known code values written into the final frame just before presentation (a buffer-to-image copy: exact
//         bits whatever the swapchain format; SDR sRGB codes, HDR10 PQ codes). The port's output, Windows, the GPU
//         and the display.
//   game: a scene-linear value through the game's grading (its parameter block this frame, ue4_grade.glsl), written
//         over the tonemapper's output as the tonemapper would write it; then the UI composite and presentation.
// Messages end in "\x02\r" (HCFR's PGenerator framing) or "\n"; each patch is answered "OK" once a frame showing it
// has been presented. Commands:
//   GOW3:RAW;r,g,b[;area[;black]]   normalized code values (0-1) of the output encoding; area of the screen (0.1);
//                                   "black": the rest of the frame black
//   GOW3:GAME;x[,y,z][;area[;black][;neutral]] scene-linear value(s) through the game's grading; "black": only the
//                                   patch's part of the frame is shown (the presenter blits it onto black); "neutral":
//                                   the game's Brightness at 1 (power 1, the HDR pass's), not the slider's
//   GOW3:OFF, GOW3:INFO             patch off; "OK:hdr=0|1;w=W;h=H"
//   PGenerator (HCFR): CMD:GET_RESOLUTION, CMD:GET_GPU_MEMORY, RGB=TEXT;..., SETCONF:..., RGB=RECTANGLE;w,h;100;r,g,b;
//   bgR,bgG,bgB;-1,-1,x,y, TESTTEMPLATERAMDISK:HCFR:r,g,b;bgR,bgG,bgB, TESTTEMPLATE:..., QUIT (8-bit codes).
// GOW3_PATTERN_SERVER=1: TCP 127.0.0.1:GOW3_PATTERN_PORT (21977); =pgen: also TCP 85 on all interfaces and the UDP
// 1977 discovery answer, for HCFR. Unset: no thread, no socket; the draw paths test one relaxed load.

#pragma once

#include <atomic>
#include <cstdint>

namespace Gow3Patterns {

enum Mode : int { Off = 0, Raw = 1, Game = 2 };

struct Patch {
    Mode mode = Off;
    float value[3] = {};  ///< raw: normalized output codes; game: scene-linear
    float area = 0.1f;    ///< share of the screen (centred, the screen's aspect)
    bool background = false; ///< raw: the rest of the frame set to `bg`
    bool neutral_brightness = false; ///< game: the game's Brightness at its neutral value (as the HDR pass uses)
    float bg[3] = {};
};

/// Reads GOW3_PATTERN_SERVER once and starts the server thread when it is set.
void Start();
/// A patch is shown (any mode); the game mode.
bool Active();
bool GameActive();
/// The current patch (a copy).
Patch Current();
/// The presenter, after each present: answers patches that are now on screen.
void NotePresented();

} // namespace Gow3Patterns
