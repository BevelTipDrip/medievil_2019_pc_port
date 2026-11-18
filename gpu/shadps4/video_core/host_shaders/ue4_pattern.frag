// SPDX-FileCopyrightText: Copyright 2026 MediEvil PC port
// SPDX-License-Identifier: GPL-2.0-or-later

// gow3: a test patch through the game's grading (gow3_patterns.h "game" mode; research/docs/plan-meter-measurement.md):
// a scene-linear value graded with the game's chain from this frame's parameter block (ue4_grade.glsl), written over the
// tonemapper's output as the tonemapper writes it (its LUT sample x 1.05): SDR the sRGB curve of the graded colour (the
// game's device 0, its Brightness included), HDR our SDR-look encoding (ue4_hdr_lut.frag). The tonemapper's target holds
// blue in its first channel (its shader writes the LUT's z there), so the output is swizzled the same way. Drawn with a
// scissor: the patch only; the rest of the frame stays the game's.
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
    float pad;
    vec4 scene;      ///< the patch's scene-linear value (rgb)
} pc;

float GradeParam(uint i) {
    return uintBitsToFloat(params.data[i]);
}

#include "ue4_grade.glsl"

vec3 SrgbEncode(vec3 c) {
    c = max(c, vec3(0.0));
    return mix(1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, 12.92 * c, lessThan(c, vec3(0.0031308)));
}

vec3 PqEncode(vec3 nits) {
    const vec3 y = pow(clamp(nits / 10000.0, 0.0, 1.0), vec3(0.1593017578125));
    return pow((0.8359375 + 18.8515625 * y) / (1.0 + 18.6875 * y), vec3(78.84375));
}

void main() {
    vec3 untonemapped, graded;
    GradeScene(pc.scene.rgb, false, untonemapped, graded);
    vec3 value;
    if ((pc.flags & 1u) != 0u) {
        vec3 linear = max(graded, vec3(0.0));
        if (pc.gamma > 0.0) {
            linear = pow(SrgbEncode(graded), vec3(pc.gamma));
        }
        const mat3 to_2020 = mat3(0.6274040, 0.0690970, 0.0163916,
                                  0.3292820, 0.9195400, 0.0880132,
                                  0.0433136, 0.0113612, 0.8955950);
        value = PqEncode(to_2020 * linear * pc.game_nits);
    } else {
        value = SrgbEncode(graded);
    }
    color = vec4(value.zyx, 1.0);
}
