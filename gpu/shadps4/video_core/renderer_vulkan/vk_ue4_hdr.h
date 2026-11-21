// SPDX-FileCopyrightText: Copyright 2026 MediEvil PC port
// SPDX-License-Identifier: GPL-2.0-or-later

// gow3: HDR that keeps the game's SDR look (research/docs/plan-hdr-sdr-match.md). While the game outputs HDR10, its
// scene grading LUT draw (UE 4.21's combine LUT builder, fs a4152b3be93c832a) is replaced by our own pass
// (ue4_hdr_lut.vert / .frag): the same 32-slice LUT target, written from the game's parameter block (with the Color
// tab's changes), the SDR-graded colour at the game brightness in BT.2020 PQ. Its UI composite (fs c401ca35f9bceb8e)
// is replaced by ue4_ui_composite.frag: the UI as an SDR screen shows it at the UI brightness, blended as SDR blends.
// Menu: Display > HDR > HDR look.

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
    /// The builder with a level grading LUT texture (BLENDCOUNT=2: Hilltop Mausoleum; study-hdr.md H36): our block
    /// with 20 dwords (LUTWeights) inserted at 45, 164 in all; LUTWeights[0] / [1] at 48 / 52, the LUT a 256x16
    /// texture (image 0). Its grading is ours plus that blend (ue4_grade.glsl GRADE_LUT_BLEND, checked offline).
    static constexpr u64 LutBuilderBlendPs = 0x9f6bac6cba984956ull;
    static constexpr u32 BlendParamDwords = 164;
    static constexpr bool IsLutBuilder(u64 hash) {
        return hash == LutBuilderPs || hash == LutBuilderBlendPs;
    }
    static constexpr u32 LutBlockDwords(u64 hash) {
        return hash == LutBuilderBlendPs ? BlendParamDwords : ParamDwords;
    }
    /// A dword of our layout (GradeAt, GOW3_UE4_GRADE) in the blend builder's block.
    static constexpr u32 BlendDword(u32 dword) {
        return dword >= 45 ? dword + 20 : dword;
    }
    /// The blend builder's block in our layout: 0-44 as they are, 65-161 to 45-141, the weights 48 / 52 to 142 / 143.
    static void RemapBlendBlock(const u32* src, std::array<u32, ParamDwords>& dst);
    /// UE 4.21's HDR UI composite (scene PQ + Slate's UI image -> the display buffer).
    static constexpr u64 UiCompositePs = 0xc401ca35f9bceb8eull;
    /// UE's background blur (Slate): its first pass samples what is drawn so far, in HDR the empty UI image. Two
    /// first passes seen (study-hdr.md H32 / H33): the full-size blur (weapons tab, pause menu) and the half-size
    /// downsample (the map screen's level-select and quit popups, followed by the blur at half size).
    static constexpr u64 UiBlurPs = 0x8c6fbe78cd6698bbull;
    static constexpr u64 UiBlurDownsamplePs = 0x8c6fbe7824203398ull;
    static constexpr bool IsUiBlurFirstPass(u64 hash) {
        return hash == UiBlurPs || hash == UiBlurDownsamplePs;
    }

    /// A sampled input of the UI composite: its view (identity mapping) and layout.
    struct CompositeInput {
        vk::ImageView view;
        vk::ImageLayout layout;
    };

    Ue4HdrGrading(const Instance& instance, Scheduler& scheduler, VideoCore::TextureCache& texture_cache,
                  VideoCore::BufferCache& buffer_cache, Runtime& runtime);
    ~Ue4HdrGrading();

    /// The settings want our LUT for a builder draw whose block selects an HDR output device (3-6).
    static bool Wanted(const std::array<u32, ParamDwords>& block);
    /// Our LUT is built for the HDR tonemapper indexing it at PQ(scene x 0.01) (its HdrLutScalePass variant)
    /// instead of the game's x 0.0001: 100x finer near black, no purple shift in dark tones (study-hdr.md H38).
    /// GOW3_HDR_LUT_X100=0: the game's index (A/B). Read once.
    static bool ScaledIndex();

    /// Records our pass into the builder draw's colour target (`target`, `desc`: the draw's colour buffer 0) from
    /// `block` (the game's parameters in our layout, grading applied) and, for the blend builder, its level LUT
    /// (`level_lut`: the game's view). True: written, the game's draw is to be skipped.
    bool Run(VideoCore::ImageId target, const VideoCore::TextureCache::ImageDesc& desc,
             const std::array<u32, ParamDwords>& block, const CompositeInput* level_lut = nullptr);

    /// Test patterns (gow3_patterns.h "game" mode): a centred patch over the tonemapper's output (`target`, `desc`),
    /// scene-linear `scene` graded from `block` (the game's, this frame) as the tonemapper would write it; HDR in our
    /// SDR-look encoding (the game's Brightness neutral), SDR the game's device 0 (its Brightness kept).
    void RunPatch(VideoCore::ImageId target, const VideoCore::TextureCache::ImageDesc& desc,
                  const std::array<u32, ParamDwords>& block, const float scene[3], float area, bool hdr_output,
                  bool neutral_brightness = false);

    /// Records our UI composite into the game's composite target (`target`, `desc`) from the scene (the tonemapper's
    /// PQ output) and Slate's UI image (`ui_srgb`: its view decodes sRGB). True: written, the game's draw is skipped.
    bool RunComposite(VideoCore::ImageId target, const VideoCore::TextureCache::ImageDesc& desc,
                      const CompositeInput& scene, const CompositeInput& ui, bool ui_srgb);

    /// The backdrop the blur's first pass reads instead of the HDR UI image (`width` x `height`, RGBA8, true RGB):
    /// the UI so far over the scene in the SDR signal. Null when it could not be made.
    vk::ImageView RunBackdrop(const CompositeInput& scene, const CompositeInput& ui, u32 width, u32 height);

private:
    enum class Kind : u8 { Lut, Patch, Composite, LutBlend };
    vk::Pipeline Pipeline(vk::Format format, Kind kind = Kind::Lut);

    const Instance& instance;
    Scheduler& scheduler;
    VideoCore::TextureCache& texture_cache;
    VideoCore::BufferCache& buffer_cache;
    Runtime& runtime;
    vk::UniqueShaderModule vert, frag, patch_frag, composite_frag, blend_frag;
    vk::UniqueDescriptorSetLayout set_layout, composite_set_layout, blend_set_layout;
    vk::UniquePipelineLayout layout, composite_layout, blend_layout;
    vk::UniqueSampler lut_sampler; ///< the blend builder's sampler: linear, clamp to edge (as the game's)
    bool blend_noted = false;
    std::vector<std::pair<std::pair<vk::Format, Kind>, vk::UniquePipeline>> pipelines;
    u64 runs = 0;
    bool failed = false;
    bool brightness_noted = false; ///< the game's Brightness logged once
    u64 patch_runs = 0;
    u64 composite_runs = 0;
    bool composite_failed = false;
    VideoCore::UniqueImage backdrop;
    vk::UniqueImageView backdrop_view;
    u32 backdrop_width = 0, backdrop_height = 0;
    u64 backdrop_runs = 0;
};

} // namespace Vulkan
