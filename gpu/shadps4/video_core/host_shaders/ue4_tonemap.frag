// SPDX-FileCopyrightText: Copyright 2026 MediEvil PC port
// SPDX-License-Identifier: GPL-2.0-or-later

// gow3: our HDR tonemapper (research/docs/plan-hdr-final.md step 2; vk_ue4_hdr.cpp RunTonemap). Replaces UE 4.21's HDR
// tonemapper draw (fs 12178a9314cb761e with vs e76703c6ddfc5f06) while the HDR look is ours: the same maths, in the
// same order and from the same constants (its IR: user/shader/dumps/fs_0x12178a9314cb761e.irprogram.txt; its vertex
// shader: study-hdr.md H44), but our LUT (32^3 RGBA16F, indexed by PQ of the scene colour x 0.01: fine near black)
// and no grain (the game adds +-1/512 per channel against 8-bit banding; in 10-bit PQ only noise).
//   colour = exposure * (bloom * (bloom scale + dirt tint * dirt mask) + scene * scene scale) * vignette
//   output = LUT(PQ(colour x 0.01)) x 1.05, written blue first (the target's view stores BGRA), alpha the LUT
//   sample's luma (BT.601 x 1.05) as the game writes it.
// TONEMAP_MAP: the map screen's tonemapper instead (fs 52770387e0d6363a, same vertex shader; study-hdr.md H47): the
// same fields 4 dwords later, plus UE4's sharpen (4 neighbour taps), colour fringe, grain jitter and film grain (kept:
// the map's look); only the 8-bit dither is left out. Its pixel shader buffer 0 has 44 dwords: 0-23 where the levels'
// are, 24-43 after the viewport ([64, 84)).
#version 450

layout(location = 0) out vec4 color;

/// The game's constants as its draw binds them: [0, 24) the pixel shader's buffer 0, [24, 32) its buffer 1, [32, 44)
/// the vertex shader's buffer 0, [44, 60) its buffer 1 (dwords); [60, 64) the draw's viewport (x scale, x offset,
/// y scale, y offset: pixel = offset + NDC x scale).
layout(std430, set = 0, binding = 0) readonly buffer Constants {
    uint data[];
} k;
layout(set = 0, binding = 1) uniform sampler2D dirt_mask; ///< image 0
layout(set = 0, binding = 2) uniform sampler2D scene;     ///< image 1
layout(set = 0, binding = 3) uniform sampler2D bloom;     ///< image 2
layout(set = 0, binding = 4) uniform sampler2D eye;       ///< the vertex shader's image 0 (eye adaptation, 1x1)
layout(set = 0, binding = 5) uniform sampler3D lut;       ///< ours (Ue4HdrGrading::Run)

float P(uint i) { return uintBitsToFloat(k.data[i]); }       // pixel shader buffer 0
float D(uint i) { return uintBitsToFloat(k.data[24u + i]); } // pixel shader buffer 1 (dirt-mask tint)
float V0(uint i) { return uintBitsToFloat(k.data[32u + i]); } // vertex shader buffer 0
float V1(uint i) { return uintBitsToFloat(k.data[44u + i]); } // vertex shader buffer 1
float VP(uint i) { return uintBitsToFloat(k.data[60u + i]); } // viewport

vec3 PqIndex(vec3 c) {
    // As the game's (abs, the ST.2084 constants, x 0.96875 + 0.015625), at x 0.01 instead of x 0.0001.
    const vec3 p = exp2(0.15930176 * log2(abs(0.01 * c)));
    const vec3 v = exp2(78.84375 * log2((fma(vec3(18.851562), p, vec3(0.8359375))) / (18.6875 * p + 1.0)));
    return fma(vec3(0.96875), v, vec3(0.015625));
}

#ifdef TONEMAP_MAP
// The map's pixel shader buffer 0: dwords 24-43 sit at [64, 84).
float M(uint i) { return i < 24u ? P(i) : uintBitsToFloat(k.data[40u + i]); }

float Luma(vec3 c) { return 0.3 * c.x + 0.59 * c.y + 0.11 * c.z; }

void main() {
    const vec2 frag = gl_FragCoord.xy;
    // The vertex shader's outputs at this pixel (as in the levels' pass; its extra ones below).
    const float ndc_x = (frag.x - VP(1u)) / VP(0u);
    const float ndc_y = -((frag.y - VP(3u)) / VP(2u));
    const vec2 p = (vec2(ndc_x, ndc_y) + 1.0) / (2.0 * vec2(V0(8u), V0(9u)));
    const vec2 u = (p - vec2(V0(2u), V0(3u))) / vec2(V0(0u), V0(1u));
    const vec2 uv = vec2(V0(10u), V0(11u)) * fma(vec2(V0(4u), V0(5u)), u, vec2(V0(6u), V0(7u)));
    const float aspect = V1(5u) * V1(6u);
    const float scale = 1.4142 * inversesqrt(aspect * aspect + 1.0);
    const float vig_x = ndc_x * scale;
    const float vig_y = -ndc_y * aspect * scale;
    const float exposure = texelFetch(eye, ivec2(0), 0).x;
    // Its attribute 2: the grain jitter's target (half a texel, -x / +y) and the grain coordinates (UV + V1(8..9));
    // attribute 3: the colour fringe's coordinates (UV back to centred pixels / half size).
    const vec2 jitter_to = uv + vec2(-0.5 * V1(2u), 0.5 * V1(3u));
    const vec2 grain_uv = uv + vec2(V1(8u), V1(9u));
    const vec2 fringe = (uv * vec2(V1(0u), V1(1u)) - vec2(V1(14u), V1(15u))) / vec2(V1(12u), V1(13u));

    // The pixel shader. Grain: UE4's GrainFromUV.
    const float grain = fract(493013.0 * sin(6.2831855 * fract(0.15915494 * fma(grain_uv.y, 543.31, grain_uv.x))));
    const float jitter = M(26u) - M(26u) * grain * grain;
    const vec2 scene_uv = fma(vec2(jitter), jitter_to - uv, uv);
    const vec2 jitter_offset = scene_uv - uv;
    // Colour fringe: red and green sampled outwards by M(40) / M(41) beyond M(42) of the centre (0 on the map today).
    const vec2 off = -sign(fringe) * clamp(abs(fringe) - M(42u), 0.0, 1.0);
    const vec2 red_px = fma(vec2(M(40u)) * off + fringe, vec2(M(36u), M(37u)), vec2(M(38u), M(39u)));
    const vec2 green_px = fma(vec2(M(41u)) * off + fringe, vec2(M(36u), M(37u)), vec2(M(38u), M(39u)));
    const vec2 lo = vec2(M(28u), M(29u)), hi = vec2(M(30u), M(31u));
    const vec2 red_uv = max(lo, min(hi, fma(vec2(P(2u), P(3u)), red_px, jitter_offset)));
    const vec2 green_uv = max(lo, min(hi, fma(vec2(P(2u), P(3u)), green_px, jitter_offset)));
    vec3 s = vec3(texture(scene, red_uv).x, texture(scene, green_uv).y,
                  texture(scene, max(lo, min(hi, scene_uv))).z);
    // Sharpen (M(21); 0 on the map today, then skipped): the four taps as the port runs them.
    if (M(21u) != 0.0) {
        const vec3 t1 = textureLodOffset(scene, scene_uv, 0.0, ivec2(1, 0)).xyz;
        const vec3 t2 = textureLodOffset(scene, scene_uv, 0.0, ivec2(-1, 0)).xyz;
        const vec3 t3 = textureLodOffset(scene, scene_uv, 0.0, ivec2(-1, -1)).xyz;
        const vec3 t4 = textureLodOffset(scene, scene_uv, 0.0, ivec2(-1, 1)).xyz;
        const float a0 = Luma(s);
        const float edge = exposure * max(abs(a0 - Luma(t4)),
                                          max(max(abs(a0 - Luma(t2)), abs(a0 - Luma(t1))), abs(a0 - Luma(t3))));
        const float amount = M(21u) * -clamp(1.0 - edge, 0.0, 1.0);
        s = fma(((t1 + t2) + t3 + t4) - 4.0 * s, vec3(amount), s);
    }
    const vec3 scene_rgb = vec3(M(12u), M(13u), M(14u)) * s;
    const vec2 dirt_uv = vec2(P(6u) * (frag.x - float(k.data[8])), P(7u) * (frag.y - float(k.data[9])));
    const vec3 dirt = texture(dirt_mask, dirt_uv).xyz;
    const vec2 bloom_uv = max(vec2(M(32u), M(33u)), min(vec2(M(34u), M(35u)), uv));
    const vec3 bloom_rgb = texture(bloom, bloom_uv).xyz;
    const float vx = M(20u) * vig_x;
    const float vy = M(20u) * vig_y;
    const float v = 1.0 / (1.0 + (vy * vy + vx * vx));
    const vec3 bloom_scale = vec3(D(0u), D(1u), D(2u)) * dirt + vec3(M(16u), M(17u), M(18u));
    const float film_grain = fma(M(24u), grain, M(25u));
    const vec3 c = exposure * (bloom_scale * bloom_rgb + scene_rgb) * (v * v) * film_grain;
    const vec3 graded = texture(lut, PqIndex(c)).xyz;
    const float alpha = clamp(0.11969999 * graded.z + (graded.y * 0.61635 + 0.31394997 * graded.x), 0.0, 1.0);
    color = vec4((graded * 1.05).zyx, alpha);
}
#else
void main() {
    const vec2 frag = gl_FragCoord.xy;
    // The viewport back to the vertex shader's NDC (its position y is the negated ndc_y below).
    const float ndc_x = (frag.x - VP(1u)) / VP(0u);
    const float ndc_y = -((frag.y - VP(3u)) / VP(2u));
    // The vertex shader: NDC = 2 * V0(8..9) * p - 1 for its pixel p = V0(0..1) * u + V0(2..3), u in [0, 1] the unit
    // rectangle; its UV is V0(10..11) * (V0(4..5) * u + V0(6..7)). At this pixel:
    const vec2 p = (vec2(ndc_x, ndc_y) + 1.0) / (2.0 * vec2(V0(8u), V0(9u)));
    const vec2 u = (p - vec2(V0(2u), V0(3u))) / vec2(V0(0u), V0(1u));
    const vec2 uv = vec2(V0(10u), V0(11u)) * fma(vec2(V0(4u), V0(5u)), u, vec2(V0(6u), V0(7u)));
    // Its vignette coordinates: NDC x and the flipped NDC y scaled by the aspect (V1(5) * V1(6)), x sqrt(2) /
    // sqrt(1 + aspect^2).
    const float aspect = V1(5u) * V1(6u);
    const float scale = 1.4142 * inversesqrt(aspect * aspect + 1.0);
    const float vig_x = ndc_x * scale;
    const float vig_y = -ndc_y * aspect * scale;
    // Exposure: the eye-adaptation texel.
    const float exposure = texelFetch(eye, ivec2(0), 0).x;

    // The pixel shader.
    const vec2 dirt_uv = vec2(P(2u) * (frag.x - float(k.data[4])), P(3u) * (frag.y - float(k.data[5])));
    const vec3 dirt = texture(dirt_mask, dirt_uv).xyz;
    const vec3 scene_rgb = texture(scene, uv).xyz;
    const vec2 bloom_uv = max(vec2(P(20u), P(21u)), min(vec2(P(22u), P(23u)), uv));
    const vec3 bloom_rgb = texture(bloom, bloom_uv).xyz;
    const float vx = P(16u) * vig_x;
    const float vy = P(16u) * vig_y;
    const float v = 1.0 / (1.0 + (vy * vy + vx * vx));
    const vec3 bloom_scale = vec3(D(0u), D(1u), D(2u)) * dirt + vec3(P(12u), P(13u), P(14u));
    const vec3 c = exposure * (bloom_scale * bloom_rgb + vec3(P(8u), P(9u), P(10u)) * scene_rgb) * (v * v);
    const vec3 graded = texture(lut, PqIndex(c)).xyz;
    const float alpha = clamp(0.11969999 * graded.z + (graded.y * 0.61635 + 0.31394997 * graded.x), 0.0, 1.0);
    const vec3 out_rgb = graded * 1.05;
    color = vec4(out_rgb.zyx, alpha);
}
#endif
