// SPDX-FileCopyrightText: Copyright 2026 MediEvil PC port
// SPDX-License-Identifier: GPL-2.0-or-later

// gow3: our HDR look (research/docs/plan-hdr-final.md). While the game outputs HDR10, its scene grading LUT draw
// (UE 4.21's combine LUT builder, fs a4152b3be93c832a / 9f6bac6cba984956) is replaced by our own pass
// (ue4_hdr_lut.vert / .frag): the same 32-slice LUT target, written from the game's parameter block (with the Color
// tab's changes) through our tone curve (ue4_hdr_map.glsl) in BT.2020 PQ, and our own LUT (32^3 RGBA16F, indexed at
// PQ(scene x 0.01)) that our tonemapper pass (ue4_tonemap.frag) reads in place of the game's HDR tonemapper draw
// (fs 12178a9314cb761e; the map screen's variant keeps the game's draw and LUT). Its UI composite (fs c401ca35f9bceb8e)
// is replaced by ue4_ui_composite.frag: the UI as an SDR screen shows it at the UI brightness, blended as SDR blends.
// Menu: Color > HDR > HDR look.

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
    /// UE 4.21's HDR tonemapper (the levels'; study-hdr.md H38, H44): replaced by RunTonemap. The map screen's
    /// (52770387e0d6363a, with sharpening and grain) keeps the game's draw.
    static constexpr u64 TonemapPs = 0x12178a9314cb761eull;
    /// The constants RunTonemap takes, in dwords: the pixel shader's buffers 0 / 1, the vertex shader's 0 / 1, then the
    /// draw's viewport (x scale, x offset, y scale, y offset: pixel = offset + NDC x scale, as the port maps it).
    static constexpr u32 TonemapPs0 = 24, TonemapPs1 = 8, TonemapVs0 = 12, TonemapVs1 = 16, TonemapViewport = 4;
    static constexpr u32 TonemapDwords = TonemapPs0 + TonemapPs1 + TonemapVs0 + TonemapVs1 + TonemapViewport;
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

    /// Records our pass into the builder draw's colour target (`target`, `desc`: the draw's colour buffer 0) from
    /// `block` (the game's parameters in our layout, grading applied) and, for the blend builder, its level LUT
    /// (`level_lut`: the game's view). True: written, the game's draw is to be skipped.
    bool Run(VideoCore::ImageId target, const VideoCore::TextureCache::ImageDesc& desc,
             const std::array<u32, ParamDwords>& block, const CompositeInput* level_lut = nullptr);

    /// Our LUT has been built (this session): RunTonemap can replace the game's tonemapper.
    bool OwnLutReady() const {
        return own_lut_built;
    }
    /// The tonemapper's sampled inputs (the game's views; the eye-adaptation texel is the vertex shader's image).
    struct TonemapInputs {
        CompositeInput dirt_mask, scene, bloom, eye;
    };
    /// Records our tonemapper into the game's tonemapper target (`target`, `desc`) from its inputs and constants (the
    /// game's buffers as its draw binds them, TonemapDwords), within `rect` (the rectangle its draw covers). `clear`:
    /// the target's pending fast clear (its render pass starts with it, as the game's would), or null. True: written,
    /// the game's draw is skipped.
    bool RunTonemap(VideoCore::ImageId target, const VideoCore::TextureCache::ImageDesc& desc, const TonemapInputs& in,
                    const std::array<u32, TonemapDwords>& constants, const vk::Rect2D& rect,
                    const vk::ClearColorValue* clear);

    /// Records our UI composite into the game's composite target (`target`, `desc`) from the scene (the tonemapper's
    /// PQ output) and Slate's UI image (`ui_srgb`: its view decodes sRGB). True: written, the game's draw is skipped.
    bool RunComposite(VideoCore::ImageId target, const VideoCore::TextureCache::ImageDesc& desc,
                      const CompositeInput& scene, const CompositeInput& ui, bool ui_srgb);

    /// The backdrop the blur's first pass reads instead of the HDR UI image (`width` x `height`, RGBA8, true RGB):
    /// the UI so far over the scene in the SDR signal. Null when it could not be made.
    vk::ImageView RunBackdrop(const CompositeInput& scene, const CompositeInput& ui, u32 width, u32 height);

private:
    enum class Kind : u8 { Lut, Composite, LutBlend, Tonemap };
    vk::Pipeline Pipeline(vk::Format format, Kind kind = Kind::Lut);
    /// Our LUT image (32^3 RGBA16F, 3D, its slices renderable as a 2D array), made at the first build.
    bool EnsureOwnLut(u32 size);

    const Instance& instance;
    Scheduler& scheduler;
    VideoCore::TextureCache& texture_cache;
    VideoCore::BufferCache& buffer_cache;
    Runtime& runtime;
    vk::UniqueShaderModule vert, frag, composite_frag, blend_frag, tonemap_frag;
    vk::UniqueDescriptorSetLayout set_layout, composite_set_layout, blend_set_layout, tonemap_set_layout;
    vk::UniquePipelineLayout layout, composite_layout, blend_layout, tonemap_layout;
    vk::UniqueSampler lut_sampler; ///< the blend builder's sampler: linear, clamp to edge (as the game's)
    bool blend_noted = false;
    std::vector<std::pair<std::pair<vk::Format, Kind>, vk::UniquePipeline>> pipelines;
    u64 runs = 0;
    bool failed = false;
    bool brightness_noted = false; ///< the game's Brightness logged once (dev build)
    u64 composite_runs = 0;
    bool composite_failed = false;
    VideoCore::UniqueImage backdrop;
    vk::UniqueImageView backdrop_view;
    u32 backdrop_width = 0, backdrop_height = 0;
    u64 backdrop_runs = 0;
    VideoCore::UniqueImage own_lut;
    vk::UniqueImageView own_lut_layers; ///< 2D array view (rendering)
    vk::UniqueImageView own_lut_volume; ///< 3D view (sampling)
    u32 own_lut_size = 0;
    bool own_lut_built = false;
    bool own_lut_failed = false;
    bool tonemap_failed = false;
    u64 tonemap_runs = 0;
};

} // namespace Vulkan
