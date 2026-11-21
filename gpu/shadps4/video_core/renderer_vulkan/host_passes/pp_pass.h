//  SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
//  SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan {
class Frame;
}

namespace Vulkan::HostPasses {

class PostProcessingPass {
public:
    /// gow3: the frame image's format while the swapchain is HDR10 (Presenter::RecreateFrame).
    static constexpr vk::Format HDR_FRAME_FORMAT = vk::Format::eA2B10G10R10UnormPack32;

    /// gow3: points of the HDR near-black correction (Settings::black_lut).
    static constexpr u32 BLACK_LUT_SIZE = 24;

    struct Settings {
        float gamma = 1.0f;
        u32 hdr = 0;
        u32 srgb_input = 0;
        /// gow3 HDR near-black correction (research/docs/plan-meter-measurement.md): `black_count` > 1 remaps PQ codes
        /// below `black_limit` through `black_lut` (output codes at evenly spaced input codes 0..black_limit), the
        /// inverse of the display's measured near-black response. 0: off. 128 bytes in all (push constants).
        u32 black_count = 0;
        float black_limit = 0.0f;
        float pad[3] = {};
        float black_lut[BLACK_LUT_SIZE] = {};
    };
    static_assert(sizeof(Settings) == 128, "pp push constants: 128 bytes at most on every device");

    void Create(vk::Device device, vk::Format surface_format);

    void Render(vk::CommandBuffer cmdbuf, vk::ImageView input, vk::Extent2D input_size,
                Frame& output, Settings settings);

private:
    vk::UniquePipeline pipeline{};
    vk::UniquePipeline pipeline_hdr{};
    vk::UniquePipelineLayout pipeline_layout{};
    vk::UniquePipelineLayout pipeline_layout_hdr{}; ///< gow3: Settings in full (the SDR layout: its first 12 bytes)
    vk::UniqueDescriptorSetLayout desc_set_layout{};
    vk::UniqueSampler sampler{};
};

} // namespace Vulkan::HostPasses
