// SPDX-FileCopyrightText: Copyright 2026 MediEvil PC port
// SPDX-License-Identifier: GPL-2.0-or-later

// gow3: ImGui's fragment shader for an HDR10 swapchain (A2B10G10R10, ST 2084, BT.2020): the overlay's
// display-encoded colours (made for an SDR monitor, gamma 2.2) become BT.2020 PQ with white at UI_NITS.
// Blending stays the main pipeline's (in PQ code values). Same interface as imgui_impl_vulkan.cpp's
// built-in glsl_shader.frag.
#version 450 core
layout(location = 0) out vec4 fColor;
layout(set = 0, binding = 0) uniform texture2D _Texture;
layout(set = 1, binding = 0) uniform sampler _Sampler;
layout(location = 0) in struct { vec4 Color; vec2 UV; } In;

// White in nits: the menu's UI brightness (Gow3Settings hdr_ui_nits). The only specialization constant:
// gow3_overlay.cpp writes the value into its OpSpecConstant (the backend's pipeline takes no
// specialization info), one pipeline per value.
layout(constant_id = 0) const float UI_NITS = 100.0;

vec3 Pq(vec3 nits) {
    const vec3 y = pow(clamp(nits / 10000.0, 0.0, 1.0), vec3(0.1593017578125));
    return pow((0.8359375 + 18.8515625 * y) / (1.0 + 18.6875 * y), vec3(78.84375));
}

void main() {
    const vec4 c = In.Color * texture(sampler2D(_Texture, _Sampler), In.UV.st);
    const vec3 linear = pow(max(c.rgb, vec3(0.0)), vec3(2.2));
    // BT.709 -> BT.2020 primaries (columns).
    const mat3 to_2020 = mat3(0.6274040, 0.0690970, 0.0163916,
                              0.3292820, 0.9195400, 0.0880132,
                              0.0433136, 0.0113612, 0.8955950);
    fColor = vec4(Pq(to_2020 * linear * UI_NITS), c.a);
}
