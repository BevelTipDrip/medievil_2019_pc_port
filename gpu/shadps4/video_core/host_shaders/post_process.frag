// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#version 450

layout (location = 0) in vec2 uv;
layout (location = 0) out vec4 color;

layout (binding = 0) uniform sampler2D texSampler;

layout (push_constant) uniform settings {
    float gamma;
    bool hdr;
    bool srgb_input;
    // gow3 HDR near-black correction (pp_pass.h Settings): PQ codes below black_limit remapped through black_lut.
    uint black_count;
    float black_limit;
    float pad0, pad1, pad2;
    float black_lut[24];
} pp;

const float cutoff = 0.0031308, a = 1.055, b = 0.055, d = 12.92;
vec3 gamma(vec3 rgb) {
    return mix(
        a * pow(rgb, vec3(1.0 / (2.4 + 1.0 - pp.gamma))) - b,
        d * rgb / pp.gamma,
        lessThan(rgb, vec3(cutoff))
    );
}

// Exact inverse of gamma() at unit gamma, for buffers that are sRGB encoded but must be sampled
// through a UNORM view because Vulkan has no sRGB variant of the 10-bit format.
vec3 degamma(vec3 rgb) {
    return mix(
        pow(max(rgb + b, 0.0) / a, vec3(2.4)),
        rgb / d,
        lessThan(rgb, vec3(d * cutoff))
    );
}

// The inverse of the display's measured near-black response, on PQ codes (each channel; identity at the limit).
float BlackCorrect(float code) {
    if (code >= pp.black_limit || code <= 0.0) {
        return code;
    }
    const float at = code / pp.black_limit * float(pp.black_count - 1u);
    const uint i = min(uint(at), pp.black_count - 2u);
    return mix(pp.black_lut[i], pp.black_lut[i + 1u], at - float(i));
}

void main() {
    vec4 color_linear = texture(texSampler, uv);
    if (pp.hdr) {
        color = color_linear;
        if (pp.black_count > 1u) {
            color.rgb = vec3(BlackCorrect(color.r), BlackCorrect(color.g), BlackCorrect(color.b));
        }
    } else {
        if (pp.srgb_input) color_linear.rgb = degamma(color_linear.rgb);
        color = vec4(gamma(color_linear.rgb), color_linear.a);
    }
}
