// SPDX-FileCopyrightText: Copyright 2026 MediEvil PC port
// SPDX-License-Identifier: GPL-2.0-or-later

// gow3: MediEvil's scene grading LUT in HDR (vk_ue4_hdr.cpp; research/docs/plan-hdr-final.md). Replaces UE 4.21's
// combine LUT builder (fs a4152b3be93c832a) while the game outputs HDR10 and the HDR look is ours: the same 32^3 LUT,
// indexed as the game's HDR tonemapper indexes it (PQ of the scene colour x 1e-4), from the game's own parameter
// block: the game's graded colour through our tone curve (ue4_hdr_map.glsl HdrPiecewiseNits), in BT.2020 PQ, / 1.05
// as the tonemapper multiplies its sample by 1.05.
#version 450
#extension GL_GOOGLE_include_directive : require

layout(location = 0) flat in int slice;
layout(location = 0) out vec4 color;

layout(std430, set = 0, binding = 0) readonly buffer Params {
    uint data[];
} params;

layout(push_constant) uniform Push {
    float lut_size;  ///< 32
    uint flags;      ///< bit 0: film black clip forced to 0
    float pad[2];
    vec4 pw0;        ///< the curve's constants (vk_ue4_hdr.cpp PiecewiseFromSettings)
    vec4 pw1;
    vec4 pw2;
} pc;

float GradeParam(uint i) {
    return uintBitsToFloat(params.data[i]);
}

#ifdef GRADE_LUT_BLEND
// Built a second time with GRADE_LUT_BLEND=1 (ue4_hdr_lut_blend_frag) for the builder with a level grading LUT
// (fs 9f6bac6cba984956): the game's own view of its 256x16 LUT texture (bgra swizzle: true RGB here), sampled
// linear / clamp to edge as the game's sampler; the weights are in params 142 / 143 (vk_rasterizer.cpp remaps the block).
layout(set = 0, binding = 1) uniform sampler2D grade_lut;
vec3 GradeLutSample(vec2 uv) {
    return texture(grade_lut, uv).rgb;
}
#endif

#include "ue4_grade.glsl"
#include "ue4_hdr_map.glsl"

void main() {
    // The game's decode of a texel: R and G from the texel centre ((i + 0.5) / size), B from the slice, each to
    // i / (size - 1), then inverse PQ x 10000 (bit-exact with the game's shader: LutDecodeHdrTexel).
    const vec3 scene = LutDecodeHdrTexel(gl_FragCoord.xy / pc.lut_size, uint(slice));
    const vec3 nits = HdrPiecewiseNits(scene, (pc.flags & 1u) != 0u, pc.pw0, pc.pw1, pc.pw2);
    color = vec4(HdrNitsToPq(nits) / 1.05, 0.0);
}
