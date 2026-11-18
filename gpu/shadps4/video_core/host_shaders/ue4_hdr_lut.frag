// SPDX-FileCopyrightText: Copyright 2026 MediEvil PC port
// SPDX-License-Identifier: GPL-2.0-or-later

// gow3: MediEvil's scene grading LUT in HDR, kept to the game's SDR look (vk_ue4_hdr.cpp;
// research/docs/plan-hdr-sdr-match.md). Replaces UE 4.21's combine LUT builder (fs a4152b3be93c832a) while the game
// outputs HDR10: the same 32^3 LUT, indexed as the game's HDR tonemapper indexes it (PQ of the scene colour x 1e-4),
// from the game's own parameter block. The game's chain (ue4_grade.glsl, ported from the translated shader) gives the
// SDR-graded colour G; step 1 shows G as an SDR screen would (optionally through gamma emulation) at the game
// brightness, in BT.2020 PQ, / 1.05 as the tonemapper multiplies its sample by 1.05.
#version 450
#extension GL_GOOGLE_include_directive : require

layout(location = 0) flat in int slice;
layout(location = 0) out vec4 color;

layout(std430, set = 0, binding = 0) readonly buffer Params {
    uint data[];
} params;

layout(push_constant) uniform Push {
    float game_nits; ///< SDR white (1.0 of G) in nits
    float gamma;     ///< gamma emulation: the SDR display's gamma (2.2), 0 = none (the sRGB curve as is)
    float lut_size;  ///< 32
    uint flags;      ///< bit 0: film black clip forced to 0 (RenoDX)
} pc;

float GradeParam(uint i) {
    return uintBitsToFloat(params.data[i]);
}

#include "ue4_grade.glsl"

// The sRGB curve, continued as a power law above 1 (G may exceed 1: no saturate in UE 4.21's chain).
vec3 SrgbEncode(vec3 c) {
    c = max(c, vec3(0.0));
    return mix(1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, 12.92 * c, lessThan(c, vec3(0.0031308)));
}

vec3 PqEncode(vec3 nits) {
    const vec3 y = pow(clamp(nits / 10000.0, 0.0, 1.0), vec3(0.1593017578125));
    return pow((0.8359375 + 18.8515625 * y) / (1.0 + 18.6875 * y), vec3(78.84375));
}

void main() {
    // The game's decode of a texel: R and G from the texel centre ((i + 0.5) / size), B from the slice, each to
    // i / (size - 1), then inverse PQ x 10000 (bit-exact with the game's shader: LutDecodeHdrTexel).
    const vec3 scene = LutDecodeHdrTexel(gl_FragCoord.xy / pc.lut_size, uint(slice));
    vec3 untonemapped, graded;
    GradeScene(scene, (pc.flags & 1u) != 0u, untonemapped, graded);
    // As the SDR screen showed it: G encoded with the sRGB curve (the game's device 0), decoded by a `gamma` display.
    vec3 linear = max(graded, vec3(0.0));
    if (pc.gamma > 0.0) {
        linear = pow(SrgbEncode(graded), vec3(pc.gamma));
    }
    // BT.709 -> BT.2020 (columns), nits, PQ.
    const mat3 to_2020 = mat3(0.6274040, 0.0690970, 0.0163916,
                              0.3292820, 0.9195400, 0.0880132,
                              0.0433136, 0.0113612, 0.8955950);
    color = vec4(PqEncode(to_2020 * linear * pc.game_nits) / 1.05, 0.0);
}
