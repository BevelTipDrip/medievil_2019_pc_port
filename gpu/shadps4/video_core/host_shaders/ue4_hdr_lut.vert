// SPDX-FileCopyrightText: Copyright 2026 MediEvil PC port
// SPDX-License-Identifier: GPL-2.0-or-later

// gow3: the HDR grading LUT pass (vk_ue4_hdr.cpp; research/docs/plan-hdr-sdr-match.md): one full-screen triangle per
// LUT slice, the instance as the layer (shaderOutputLayer), the slice passed on to the pixel shader.
#version 450
#extension GL_ARB_shader_viewport_layer_array : require

layout(location = 0) flat out int slice;

void main() {
    const vec2 pos = vec2(float((gl_VertexIndex & 1) << 2), float((gl_VertexIndex & 2) << 1));
    gl_Position = vec4(pos - vec2(1.0), 0.0, 1.0);
    gl_Layer = gl_InstanceIndex;
    slice = gl_InstanceIndex;
}
