// SPDX-FileCopyrightText: Copyright 2026 MediEvil PC port
// SPDX-License-Identifier: GPL-2.0-or-later

// gow3: MediEvil's scene grading LUT in HDR (vk_ue4_hdr.cpp; research/docs/plan-hdr-final.md). Replaces UE 4.21's
// combine LUT builder (fs a4152b3be93c832a) while the game outputs HDR10 and the HDR look is ours: a 32^3 LUT indexed
// as the game's HDR tonemapper indexes it (PQ of the scene colour x 1e-4; the map screen's tonemapper), or ours at
// x 0.01 (flag bit 2; ue4_tonemap.frag), from the game's own parameter
// block: the game's graded colour through our tone curve (ue4_hdr_map.glsl HdrPiecewiseNits), in BT.2020 PQ, / 1.05
// as the tonemapper multiplies its sample by 1.05. The curve's values are the area's (ue4_hdr_area.frag, drawn just
// before from the same block: with and without the level LUT, blended by Level LUT tone) with the user's adjustments.
#version 450
#extension GL_GOOGLE_include_directive : require

layout(location = 0) flat in int slice;
layout(location = 0) out vec4 color;

layout(std430, set = 0, binding = 0) readonly buffer Params {
    uint data[];
} params;

layout(push_constant) uniform Push {
    float lut_size;  ///< 32
    uint flags;      ///< bit 0: film black clip forced to 0; bit 2: our LUT (indexed at PQ(scene x 0.01))
    float game_nits; ///< SDR white in nits (mid grey at 18% of it, x the area's grey level)
    float peak_nits;
    vec4 adj0;       ///< mid contrast x, toe length x, toe strength +, toe clamp +
    vec4 adj1;       ///< black floor x, Level LUT tone (0 without the LUT's tone, 1 the game's), highlight saturation
    vec4 shoulder;   ///< start (share of the peak), length (stops), angle (0-1)
} pc;

/// The area's values (ue4_hdr_area.frag): texels 0-1 with the level LUT, 2-3 without.
layout(set = 0, binding = 2) uniform sampler2D area;

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

/// The curve's constants (ue4_hdr_map.glsl) from the area's values and the user's adjustments.
void AreaCurve(out vec4 pw0, out vec4 pw1, out vec4 pw2, out vec4 pw3) {
    const vec4 a0 = texelFetch(area, ivec2(0, 0), 0), a1 = texelFetch(area, ivec2(1, 0), 0);
    const vec4 n0 = texelFetch(area, ivec2(2, 0), 0), n1 = texelFetch(area, ivec2(3, 0), 0);
    const float tone = clamp(pc.adj1.y, 0.0, 1.0);
    const vec4 v0 = mix(n0, a0, tone); // floor, grey, gamma, toe length
    const vec4 v1 = mix(n1, a1, tone); // toe strength, clamp
    const float gamma = max(v0.z * pc.adj0.x, 0.3);
    const float len = clamp(v0.w * pc.adj0.y, 0.005, 0.17);
    const float clamp_ = clamp(v1.y + pc.adj0.w, 0.0, 0.02);
    const float strength = clamp(v1.x + pc.adj0.z, 0.0, 0.9);
    const float b = min(strength * len, 0.17 - clamp_); // the pin (0.18 - clamp - b) stays above 0.01
    const float grey_nits = 0.18 * pc.game_nits * v0.y;
    pw0 = vec4(clamp_, len, b, gamma);
    pw1 = vec4(grey_nits, gamma * len / max(len - b, 1e-6),
               grey_nits * pow(max(len - b, 1e-6) / (0.18 - clamp_ - b), gamma), pc.adj1.z);
    pw2 = vec4(pc.shoulder.x * pc.peak_nits, pc.peak_nits * exp2(pc.shoulder.y), pc.peak_nits, pc.shoulder.z);
    pw3 = vec4(max(v0.x * pc.adj1.x, 0.0) * pc.game_nits, 0.0, 0.0, 0.0);
}

void main() {
    // The game's decode of a texel: R and G from the texel centre ((i + 0.5) / size), B from the slice, each to
    // i / (size - 1), then inverse PQ x 10000 (bit-exact with the game's shader: LutDecodeHdrTexel).
    vec3 scene = LutDecodeHdrTexel(gl_FragCoord.xy / pc.lut_size, uint(slice));
    if ((pc.flags & 4u) != 0u) {
        scene *= 0.01; // our LUT, read by our tonemapper (ue4_tonemap.frag) at PQ(scene x 0.01): 100x finer near black
    }
    vec4 pw0, pw1, pw2, pw3;
    AreaCurve(pw0, pw1, pw2, pw3);
    const vec3 nits = HdrPiecewiseNits(scene, (pc.flags & 1u) != 0u, pw0, pw1, pw2, pw3);
    color = vec4(HdrNitsToPq(nits) / 1.05, 0.0);
}
