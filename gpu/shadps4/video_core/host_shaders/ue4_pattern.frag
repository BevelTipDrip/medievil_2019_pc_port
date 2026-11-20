// SPDX-FileCopyrightText: Copyright 2026 MediEvil PC port
// SPDX-License-Identifier: GPL-2.0-or-later

// gow3: a test patch through the game's grading (gow3_patterns.h "game" mode; research/docs/plan-meter-measurement.md):
// a scene-linear value graded with the game's chain from this frame's parameter block (ue4_grade.glsl), written over the
// tonemapper's output as the tonemapper writes it (its LUT sample x 1.05): SDR the sRGB curve of the graded colour (the
// game's device 0, its Brightness included), HDR our encoding (ue4_hdr_map.glsl, as ue4_hdr_lut.frag). The
// tonemapper's target holds blue in its first channel (its shader writes the LUT's z there), so the output is swizzled
// the same way. Drawn with a scissor: the patch only; the rest of the frame stays the game's.
#version 450
#extension GL_GOOGLE_include_directive : require

layout(location = 0) flat in int slice;
layout(location = 0) out vec4 color;

layout(std430, set = 0, binding = 0) readonly buffer Params {
    uint data[];
} params;

layout(push_constant) uniform Push {
    float game_nits; ///< HDR: SDR white in nits
    float gamma;     ///< HDR: gamma emulation (0 = none)
    uint flags;      ///< bit 0: HDR output
    float peak_nits; ///< HDR: the display's peak (roll-off)
    float highlights;///< HDR: 0..1 highlights above the knee
    float pad0;
    float pad1;
    float pad2;
    vec4 scene;      ///< the patch's scene-linear value (rgb)
} pc;

float GradeParam(uint i) {
    return uintBitsToFloat(params.data[i]);
}

#include "ue4_grade.glsl"
#include "ue4_hdr_map.glsl"

void main() {
    vec3 value;
    if ((pc.flags & 1u) != 0u) {
        value = HdrNitsToPq(HdrSceneToNits(pc.scene.rgb, false, pc.game_nits, pc.gamma, pc.peak_nits,
                                           pc.highlights));
    } else {
        vec3 untonemapped, graded;
        GradeScene(pc.scene.rgb, false, untonemapped, graded);
        value = SrgbEncode(graded);
    }
    color = vec4(value.zyx, 1.0);
}
