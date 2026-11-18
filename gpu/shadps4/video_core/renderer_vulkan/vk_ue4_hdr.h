// SPDX-FileCopyrightText: Copyright 2026 MediEvil PC port
// SPDX-License-Identifier: GPL-2.0-or-later

// gow3: HDR that keeps the game's SDR look (research/docs/plan-hdr-sdr-match.md). While the game outputs HDR10, its
// scene grading LUT draw (UE 4.21's combine LUT builder, fs a4152b3be93c832a) is replaced by our own pass
// (ue4_hdr_lut.vert / .frag): the same 32-slice LUT target, written from the game's parameter block (with the Color
// tab's changes), the SDR-graded colour at the game brightness in BT.2020 PQ. Menu: Display > HDR > HDR look.

#pragma once

#include <array>
#include <utility>
#include <vector>
#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/renderer_vulkan/vk_scheduler.h" // texture_cache.h's inline members use the Scheduler
#include "video_core/texture_cache/texture_cache.h"

namespace VideoCore {
class BufferCache;
} // namespace VideoCore

namespace Vulkan {

class Instance;
class Scheduler;
class Runtime;

class Ue4HdrGrading {
public:
    /// UE 4.21's combine LUT builder (MediEvil 01.00 / 01.02) and its parameter block size.
    static constexpr u64 LutBuilderPs = 0xa4152b3be93c832aull;
    static constexpr u32 ParamDwords = 144;

    Ue4HdrGrading(const Instance& instance, Scheduler& scheduler, VideoCore::TextureCache& texture_cache,
                  VideoCore::BufferCache& buffer_cache, Runtime& runtime);
    ~Ue4HdrGrading();

    /// The settings want our LUT for a builder draw whose block selects an HDR output device (3-6).
    static bool Wanted(const std::array<u32, ParamDwords>& block);

    /// Records our pass into the builder draw's colour target (`target`, `desc`: the draw's colour buffer 0) from
    /// `block` (the game's parameters, grading applied). True: written, the game's draw is to be skipped.
    bool Run(VideoCore::ImageId target, const VideoCore::TextureCache::ImageDesc& desc, const std::array<u32, ParamDwords>& block);

    /// Test patterns (gow3_patterns.h "game" mode): a centred patch over the tonemapper's output (`target`, `desc`),
    /// scene-linear `scene` graded from `block` (the game's, this frame) as the tonemapper would write it; HDR in our
    /// SDR-look encoding (the game's Brightness neutral), SDR the game's device 0 (its Brightness kept).
    void RunPatch(VideoCore::ImageId target, const VideoCore::TextureCache::ImageDesc& desc,
                  const std::array<u32, ParamDwords>& block, const float scene[3], float area, bool hdr_output,
                  bool neutral_brightness = false);

private:
    vk::Pipeline Pipeline(vk::Format format, bool patch = false);

    const Instance& instance;
    Scheduler& scheduler;
    VideoCore::TextureCache& texture_cache;
    VideoCore::BufferCache& buffer_cache;
    Runtime& runtime;
    vk::UniqueShaderModule vert, frag, patch_frag;
    vk::UniqueDescriptorSetLayout set_layout;
    vk::UniquePipelineLayout layout;
    std::vector<std::pair<std::pair<vk::Format, bool>, vk::UniquePipeline>> pipelines;
    u64 runs = 0;
    bool failed = false;
    bool brightness_noted = false; ///< the game's Brightness logged once
    u64 patch_runs = 0;
};

} // namespace Vulkan
