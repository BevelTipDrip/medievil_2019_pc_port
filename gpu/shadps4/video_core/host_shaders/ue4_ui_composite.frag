// SPDX-FileCopyrightText: Copyright 2026 MediEvil PC port
// SPDX-License-Identifier: GPL-2.0-or-later

// gow3: the HDR UI composite (research/docs/plan-hdr-sdr-match.md step 3), replacing UE 4.21's (fs c401ca35f9bceb8e)
// while the HDR look is "SDR grading kept". The game's reads the UI image's gamma-encoded values as linear and sends
// them through ACES (x1.5, RRT + ODT 1000: washed out, hues shifted) and squashes the scene under translucent UI. Here:
// the UI as an SDR screen shows it (decoded at `gamma`, 0 = the sRGB curve) at the UI brightness, blended over the scene
// as SDR blends (in the encoded signal, white = the UI brightness), so a 50% black panel dims as in SDR. Where the UI is
// empty the scene's PQ value passes through untouched. Channels: the scene, UI and target store blue first; the game's
// views (`bgra`) give its shader true RGB and it writes out.x from in.z. So: inputs through the game's views, the colour
// maths in true RGB, the output written `.bgr`. (The first build read through identity views: the pass-through was
// right, but the BT.709 -> 2020 matrix ran on swapped channels and turned greens mint; the user's run 2026-11-19.)
#version 450
#extension GL_EXT_samplerless_texture_functions : require

layout(set = 0, binding = 0) uniform texture2D scene_tex; ///< the tonemapper's output: BT.2020 PQ
layout(set = 0, binding = 1) uniform texture2D ui_tex;    ///< Slate's UI: premultiplied, gamma-encoded BT.709

layout(push_constant) uniform Push {
    vec2 target_size; ///< the composite's target in pixels
    float ui_nits;    ///< UI white in nits (UI brightness)
    float gamma;      ///< SDR display decode (0 = the sRGB curve)
    uint flags;       ///< bit 0: the UI view is sRGB (the sample is already linear)
} pc;

layout(location = 0) out vec4 color;

vec3 SrgbEncode(vec3 c) {
    c = max(c, vec3(0.0));
    return mix(1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, 12.92 * c, lessThan(c, vec3(0.0031308)));
}

vec3 SrgbDecode(vec3 c) {
    c = max(c, vec3(0.0));
    return mix(pow((c + 0.055) / 1.055, vec3(2.4)), c / 12.92, lessThan(c, vec3(0.04045)));
}

// The SDR signal <-> linear (1.0 = UI white) of the emulated SDR screen.
vec3 Decode(vec3 signal) {
    return pc.gamma > 0.0 ? pow(max(signal, vec3(0.0)), vec3(pc.gamma)) : SrgbDecode(signal);
}

vec3 Encode(vec3 linear) {
    return pc.gamma > 0.0 ? pow(max(linear, vec3(0.0)), vec3(1.0 / pc.gamma)) : SrgbEncode(linear);
}

vec3 PqDecode(vec3 pq) {
    const vec3 p = pow(clamp(pq, 0.0, 1.0), vec3(1.0 / 78.84375));
    return 10000.0 * pow(max(p - 0.8359375, 0.0) / (18.8515625 - 18.6875 * p), vec3(1.0 / 0.1593017578125));
}

vec3 PqEncode(vec3 nits) {
    const vec3 y = pow(clamp(nits / 10000.0, 0.0, 1.0), vec3(0.1593017578125));
    return pow((0.8359375 + 18.8515625 * y) / (1.0 + 18.6875 * y), vec3(78.84375));
}

void main() {
    const vec2 uv = gl_FragCoord.xy / pc.target_size;
    const ivec2 scene_size = textureSize(scene_tex, 0);
    const ivec2 ui_size = textureSize(ui_tex, 0);
    const vec3 scene_pq = texelFetch(scene_tex, min(ivec2(uv * vec2(scene_size)), scene_size - 1), 0).rgb;
    const vec4 ui = texelFetch(ui_tex, min(ivec2(uv * vec2(ui_size)), ui_size - 1), 0);
    if (ui.a <= 0.0 && all(lessThanEqual(ui.rgb, vec3(0.0)))) {
        color = vec4(scene_pq.bgr, 1.0);
        return;
    }
    // BT.709 -> BT.2020 (columns), applied to the UI's linear light.
    const mat3 to_2020 = mat3(0.6274040, 0.0690970, 0.0163916,
                              0.3292820, 0.9195400, 0.0880132,
                              0.0433136, 0.0113612, 0.8955950);
    const vec3 ui_signal = (pc.flags & 1u) != 0u ? SrgbEncode(ui.rgb) : ui.rgb;
    const vec3 ui_2020 = Encode(to_2020 * Decode(ui_signal));
    const vec3 scene_signal = Encode(PqDecode(scene_pq) / pc.ui_nits);
    const vec3 blended = ui_2020 + scene_signal * (1.0 - clamp(ui.a, 0.0, 1.0));
    color = vec4(PqEncode(Decode(blended) * pc.ui_nits).bgr, 1.0);
}
