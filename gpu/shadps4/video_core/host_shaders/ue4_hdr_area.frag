// SPDX-FileCopyrightText: Copyright 2026 MediEvil PC port
// SPDX-License-Identifier: GPL-2.0-or-later

// gow3: the area's tone curve values (research/docs/plan-hdr-final.md step 3; study-hdr.md H42, H45). Drawn into a
// 4x1 RGBA32F image before our LUT pass, from the game's parameter block (and its level LUT, GRADE_LUT_BLEND): the
// game's SDR look on a grey ramp (G clipped at 1, sRGB-encoded, gamma 2.2, relative to SDR white) as a function of
// Y(U), then fixed rules: black floor = the curve at black; grey = the curve at Y(U) 0.18 / 0.18; gamma = log-log
// slope of (curve - floor) from 0.18 to 0.36; toe length, strength and clamp = our curve through the game's at Y(U)
// 0.07, 0.02, 0.006 (damped Newton on log residuals). Texels 0-1: with the level LUT, 2-3: without it (its weights
// 1 / 0); x: (floor, grey, gamma, toe length), y: (toe strength, clamp, solved: 1 / fallback: 0, 0).
#version 450
#extension GL_GOOGLE_include_directive : require

layout(location = 0) flat in int slice;
layout(location = 0) out vec4 color;

layout(std430, set = 0, binding = 0) readonly buffer Params {
    uint data[];
} params;

bool g_without_lut = false; // GradeParam reports the LUT weights 1 / 0

float GradeParam(uint i) {
    if (g_without_lut && i == 142u) {
        return 1.0;
    }
    if (g_without_lut && i == 143u) {
        return 0.0;
    }
    return uintBitsToFloat(params.data[i]);
}

#ifdef GRADE_LUT_BLEND
layout(set = 0, binding = 1) uniform sampler2D grade_lut;
vec3 GradeLutSample(vec2 uv) {
    return texture(grade_lut, uv).rgb;
}
#endif

#include "ue4_grade.glsl"

const vec3 Y709 = vec3(0.2126, 0.7152, 0.0722);
const int RampN = 40;            ///< scene 0.0005 to 20, log-spaced
const float RampLo = -3.30103;   ///< log10(0.0005)
const float RampHi = 1.30103;    ///< log10(20)
float g_ly[RampN];               ///< log Y(U)
float g_ln[RampN];               ///< log of the SDR look (relative to SDR white)
float g_floor;

float SrgbEnc(float c) {
    c = max(c, 0.0);
    return c < 0.0031308 ? 12.92 * c : 1.055 * pow(c, 1.0 / 2.4) - 0.055;
}

void BuildRamp() {
    for (int i = 0; i < RampN; ++i) {
        const float s = pow(10.0, mix(RampLo, RampHi, float(i) / float(RampN - 1)));
        vec3 u, g;
        GradeScene(vec3(s), false, u, g);
        const vec3 gc = clamp(g, 0.0, 1.0);
        const float look = dot(Y709, vec3(pow(SrgbEnc(gc.x), 2.2), pow(SrgbEnc(gc.y), 2.2), pow(SrgbEnc(gc.z), 2.2)));
        g_ly[i] = log(max(dot(Y709, u), 1e-9));
        g_ln[i] = log(max(look, 1e-9));
        if (i == 0) {
            g_floor = look;
        }
    }
}

/// The game's curve at Y(U) y (log-log interpolation; clamped at the ramp's ends).
float At(float y) {
    const float ly = log(y);
    if (ly <= g_ly[0]) {
        return exp(g_ln[0]);
    }
    for (int i = 1; i < RampN; ++i) {
        if (ly <= g_ly[i]) {
            const float t = (ly - g_ly[i - 1]) / max(g_ly[i] - g_ly[i - 1], 1e-9);
            return exp(mix(g_ln[i - 1], g_ln[i], t));
        }
    }
    return exp(g_ln[RampN - 1]);
}

/// Our curve below grey (relative to SDR white), as ue4_hdr_map.glsl HdrPiecewiseUnrolled + the floor.
float Ours(float y, float floor_, float grey, float gamma, float len, float strength, float clamp_) {
    const float s = max(y - clamp_, 0.0);
    const float b = strength * len;
    const float pin = 0.18 - clamp_ - b;
    float n;
    if (s >= len) {
        n = 0.18 * grey * pow(max(s - b, 1e-12) / pin, gamma);
    } else {
        const float n_len = 0.18 * grey * pow(max(len - b, 1e-12) / pin, gamma);
        n = s > 0.0 ? n_len * pow(s / len, gamma * len / (len - b)) : 0.0;
    }
    const float f = max(1.0 - y / 0.18, 0.0);
    return n + floor_ * f * f;
}

const vec3 Anchors = vec3(0.07, 0.02, 0.006);

vec3 Residual(vec3 x, float floor_, float grey, float gamma, vec3 targets) {
    vec3 r;
    for (int k = 0; k < 3; ++k) {
        r[k] = log(max(Ours(Anchors[k], floor_, grey, gamma, exp(x.x), x.y, x.z), 1e-9) / targets[k]);
    }
    return r;
}

/// Solves (toe length, strength, clamp); returns them and the largest anchor residual in w.
vec4 SolveToe(float floor_, float grey, float gamma) {
    const vec3 targets = vec3(At(Anchors.x), At(Anchors.y), At(Anchors.z));
    const vec3 lo = vec3(log(0.02), 0.0, 0.0), hi = vec3(log(0.16), 0.9, 0.006);
    vec3 x = vec3(log(0.08), 0.35, 0.002);
    const vec3 h = vec3(1e-3, 1e-3, 2e-5);
    for (int it = 0; it < 12; ++it) {
        const vec3 r = Residual(x, floor_, grey, gamma, targets);
        mat3 j; // columns: d r / d x_k
        for (int k = 0; k < 3; ++k) {
            vec3 dx = vec3(0.0);
            dx[k] = h[k];
            j[k] = (Residual(x + dx, floor_, grey, gamma, targets) - r) / h[k];
        }
        if (abs(determinant(j)) < 1e-12) {
            break;
        }
        const vec3 step = inverse(j) * -r;
        float lambda = 1.0;
        const float r2 = dot(r, r);
        for (int b = 0; b < 6; ++b) {
            const vec3 xn = clamp(x + lambda * step, lo, hi);
            const vec3 rn = Residual(xn, floor_, grey, gamma, targets);
            if (dot(rn, rn) < r2) {
                x = xn;
                break;
            }
            lambda *= 0.5;
        }
    }
    const vec3 r = Residual(x, floor_, grey, gamma, targets);
    return vec4(exp(x.x), x.y, x.z, max(max(abs(r.x), abs(r.y)), abs(r.z)));
}

void main() {
    const int texel = int(gl_FragCoord.x);
    g_without_lut = texel >= 2;
    BuildRamp();
    const float floor_ = g_floor;
    const float a18 = At(0.18);
    const float grey = a18 / 0.18;
    const float gamma = clamp(log(max(At(0.36) - floor_, 1e-9) / max(a18 - floor_, 1e-9)) / log(2.0), 0.5, 3.0);
    const vec4 toe = SolveToe(floor_, grey, gamma);
    // Fallback (a block this cannot follow): the engine defaults' values (H42 / H45).
    const bool ok = toe.w < 0.05 && grey > 0.0;
    const vec4 a = ok ? vec4(floor_, grey, gamma, toe.x) : vec4(0.0, 1.01, 1.17, 0.06);
    const vec4 b = ok ? vec4(toe.y, toe.z, 1.0, 0.0) : vec4(0.40, 0.0042, 0.0, 0.0);
    color = (texel & 1) == 0 ? a : b;
}
