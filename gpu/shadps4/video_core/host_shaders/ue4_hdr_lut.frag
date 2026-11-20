// SPDX-FileCopyrightText: Copyright 2026 MediEvil PC port
// SPDX-License-Identifier: GPL-2.0-or-later

// gow3: MediEvil's scene grading LUT in HDR, kept to the game's SDR look (vk_ue4_hdr.cpp;
// research/docs/plan-hdr-sdr-match.md). Replaces UE 4.21's combine LUT builder (fs a4152b3be93c832a) while the game
// outputs HDR10: the same 32^3 LUT, indexed as the game's HDR tonemapper indexes it (PQ of the scene colour x 1e-4),
// from the game's own parameter block. The game's chain (ue4_grade.glsl, ported from the translated shader) gives the
// SDR-graded colour G, shown as an SDR screen would at the game brightness, with highlights above the knee and a
// roll-off to the display's peak (ue4_hdr_map.glsl), in BT.2020 PQ, / 1.05 as the tonemapper multiplies its sample by
// 1.05.
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
    float peak_nits; ///< the display's peak (roll-off)
    float highlights;///< 0..1: highlights above the knee (0 = the SDR look only)
} pc;

float GradeParam(uint i) {
    return uintBitsToFloat(params.data[i]);
}

#include "ue4_grade.glsl"
#include "ue4_hdr_map.glsl"

void main() {
    // The game's decode of a texel: R and G from the texel centre ((i + 0.5) / size), B from the slice, each to
    // i / (size - 1), then inverse PQ x 10000 (bit-exact with the game's shader: LutDecodeHdrTexel).
    const vec3 scene = LutDecodeHdrTexel(gl_FragCoord.xy / pc.lut_size, uint(slice));
    const vec3 nits = HdrSceneToNits(scene, (pc.flags & 1u) != 0u, pc.game_nits, pc.gamma, pc.peak_nits,
                                     pc.highlights);
    color = vec4(HdrNitsToPq(nits) / 1.05, 0.0);
}
