// SPDX-FileCopyrightText: Copyright 2026 MediEvil PC port
// SPDX-License-Identifier: GPL-2.0-or-later

#include "video_core/renderer_vulkan/vk_ue4_hdr.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include "gow3_settings.h"
#include "video_core/buffer_cache/buffer_cache.h"
#include "video_core/host_shaders/ue4_hdr_lut_frag.h"
#include "video_core/host_shaders/ue4_hdr_lut_vert.h"
#include "video_core/host_shaders/ue4_pattern_frag.h"
#include "video_core/host_shaders/ue4_ui_composite_frag.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"

namespace Vulkan {

namespace {
/// ue4_hdr_lut.frag's push constants.
struct LutPush {
    float game_nits;
    float gamma;
    float lut_size;
    u32 flags;
    float peak_nits;
    float highlights;
};
/// ue4_pattern.frag's push constants (the layout's range is this size; LutPush fills its start).
struct PatchPush {
    float game_nits;
    float gamma;
    u32 flags;
    float peak_nits;
    float highlights;
    float pad[3];
    float scene[4]; ///< offset 32 (a vec4 in the shader's block)
};
static_assert(sizeof(PatchPush) == 48 && sizeof(LutPush) <= sizeof(PatchPush));
/// ue4_ui_composite.frag's push constants.
struct CompositePush {
    float target_size[2];
    float ui_nits;
    float gamma;
    u32 flags;
};
static_assert(sizeof(CompositePush) == 20);
constexpr u32 OutputDeviceDword = 138; ///< study-hdr.md "LUT parameters"
constexpr u32 InverseGammaDword = 5;   ///< the game's Brightness (2.2 / UE display gamma)
} // namespace

Ue4HdrGrading::Ue4HdrGrading(const Instance& instance_, Scheduler& scheduler_,
                             VideoCore::TextureCache& texture_cache_, VideoCore::BufferCache& buffer_cache_,
                             Runtime& runtime_)
    : instance{instance_}, scheduler{scheduler_}, texture_cache{texture_cache_}, buffer_cache{buffer_cache_},
      runtime{runtime_} {
    const auto device = instance.GetDevice();
    vert = vk::UniqueShaderModule(CompileSPV(UE4_HDR_LUT_VERT, device), device);
    frag = vk::UniqueShaderModule(CompileSPV(UE4_HDR_LUT_FRAG, device), device);
    patch_frag = vk::UniqueShaderModule(CompileSPV(UE4_PATTERN_FRAG, device), device);
    composite_frag = vk::UniqueShaderModule(CompileSPV(UE4_UI_COMPOSITE_FRAG, device), device);
    const vk::DescriptorSetLayoutBinding binding{0, vk::DescriptorType::eStorageBuffer, 1,
                                                 vk::ShaderStageFlagBits::eFragment};
    set_layout = Check(device.createDescriptorSetLayoutUnique({
        .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
        .bindingCount = 1,
        .pBindings = &binding,
    }));
    const auto set = *set_layout;
    const vk::PushConstantRange push{vk::ShaderStageFlagBits::eFragment, 0, sizeof(PatchPush)};
    layout = Check(device.createPipelineLayoutUnique({
        .setLayoutCount = 1,
        .pSetLayouts = &set,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &push,
    }));
    // The UI composite: two sampled images (scene, UI; texelFetch, no sampler).
    const std::array composite_bindings{
        vk::DescriptorSetLayoutBinding{0, vk::DescriptorType::eSampledImage, 1, vk::ShaderStageFlagBits::eFragment},
        vk::DescriptorSetLayoutBinding{1, vk::DescriptorType::eSampledImage, 1, vk::ShaderStageFlagBits::eFragment},
    };
    composite_set_layout = Check(device.createDescriptorSetLayoutUnique({
        .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
        .bindingCount = u32(composite_bindings.size()),
        .pBindings = composite_bindings.data(),
    }));
    const auto composite_set = *composite_set_layout;
    const vk::PushConstantRange composite_push{vk::ShaderStageFlagBits::eFragment, 0, sizeof(CompositePush)};
    composite_layout = Check(device.createPipelineLayoutUnique({
        .setLayoutCount = 1,
        .pSetLayouts = &composite_set,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &composite_push,
    }));
}

Ue4HdrGrading::~Ue4HdrGrading() = default;

bool Ue4HdrGrading::Wanted(const std::array<u32, ParamDwords>& block) {
    if (Gow3Settings::Get().hdr_look.load(std::memory_order_relaxed) != Gow3Settings::HdrLookSdr) {
        return false;
    }
    // An integer, as the game's shader tests it (`data[138] >= 3u`; run 24 read it as a float and never matched).
    const u32 device = block[OutputDeviceDword];
    return device >= 3 && device <= 6; // the ST-2084 / scRGB devices: the LUT indexed by PQ
}

vk::Pipeline Ue4HdrGrading::Pipeline(vk::Format format, Kind kind) {
    for (const auto& [key, pipeline] : pipelines) {
        if (key.first == format && key.second == kind) {
            return *pipeline;
        }
    }
    const vk::PipelineInputAssemblyStateCreateInfo input_assembly{.topology = vk::PrimitiveTopology::eTriangleList};
    const vk::PipelineMultisampleStateCreateInfo multisampling{.rasterizationSamples = vk::SampleCountFlagBits::e1};
    const std::array dynamic_states{vk::DynamicState::eViewportWithCount, vk::DynamicState::eScissorWithCount};
    const vk::PipelineDynamicStateCreateInfo dynamic_info{.dynamicStateCount = u32(dynamic_states.size()),
                                                          .pDynamicStates = dynamic_states.data()};
    const std::array stages{
        vk::PipelineShaderStageCreateInfo{.stage = vk::ShaderStageFlagBits::eVertex, .module = *vert, .pName = "main"},
        vk::PipelineShaderStageCreateInfo{.stage = vk::ShaderStageFlagBits::eFragment,
                                          .module = kind == Kind::Patch       ? *patch_frag
                                                    : kind == Kind::Composite ? *composite_frag
                                                                              : *frag,
                                          .pName = "main"},
    };
    const vk::PipelineRenderingCreateInfo rendering{.colorAttachmentCount = 1, .pColorAttachmentFormats = &format};
    const vk::PipelineColorBlendAttachmentState blend{
        .colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                          vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA};
    const vk::PipelineColorBlendStateCreateInfo color_blending{.attachmentCount = 1, .pAttachments = &blend};
    const vk::PipelineViewportStateCreateInfo viewport_info{};
    const vk::PipelineVertexInputStateCreateInfo vertex_input{};
    const vk::PipelineRasterizationStateCreateInfo raster{.cullMode = vk::CullModeFlagBits::eNone, .lineWidth = 1.f};
    const vk::PipelineDepthStencilStateCreateInfo depth_state{};
    auto pipeline = instance.GetDevice().createGraphicsPipelineUnique(
        VK_NULL_HANDLE, {
                            .pNext = &rendering,
                            .stageCount = u32(stages.size()),
                            .pStages = stages.data(),
                            .pVertexInputState = &vertex_input,
                            .pInputAssemblyState = &input_assembly,
                            .pViewportState = &viewport_info,
                            .pRasterizationState = &raster,
                            .pMultisampleState = &multisampling,
                            .pDepthStencilState = &depth_state,
                            .pColorBlendState = &color_blending,
                            .pDynamicState = &dynamic_info,
                            .layout = kind == Kind::Composite ? *composite_layout : *layout,
                        });
    if (pipeline.result != vk::Result::eSuccess) {
        std::printf("UE4 HDR grading: pipeline for format %s failed; the game's grading LUT is kept\n",
                    vk::to_string(format).c_str());
        return {};
    }
    const vk::Pipeline result = *pipeline.value;
    pipelines.emplace_back(std::pair{format, kind}, std::move(pipeline.value));
    std::printf("UE4 HDR grading: pipeline ready (%s)\n", vk::to_string(format).c_str());
    return result;
}

bool Ue4HdrGrading::Run(VideoCore::ImageId target, const VideoCore::TextureCache::ImageDesc& desc,
                        const std::array<u32, ParamDwords>& block) {
    if (failed || !target) {
        return false;
    }
    const auto& range = desc.view_info.range;
    const u32 layers = range.extent.layers;
    const u32 width = std::max(desc.info.size.width >> range.base.level, 1u);
    const u32 height = std::max(desc.info.size.height >> range.base.level, 1u);
    if (layers < 2 || width != height || width != layers) {
        // Not the 32^3 LUT this pass knows (another LUT size or a target without its layers).
        std::printf("UE4 HDR grading: unexpected LUT target %ux%u, %u layers; the game's grading LUT is kept\n",
                    width, height, layers);
        failed = true;
        return false;
    }
    const vk::Pipeline pipeline = Pipeline(desc.view_info.format);
    if (!pipeline) {
        failed = true;
        return false;
    }
    scheduler.EndRendering();
    // As the skipped draw's colour target: GPU-modified, its aliases told, the view the game's draw would render to.
    const vk::ImageView view = *texture_cache.FindRenderTarget(target, desc).image_view;
    runtime.Transit(&texture_cache.GetImage(target), vk::ImageLayout::eColorAttachmentOptimal,
                    vk::PipelineStageFlagBits2::eColorAttachmentOutput, vk::AccessFlagBits2::eColorAttachmentWrite,
                    range);
    runtime.FlushBarriers();

    // The game's Brightness setting (UE's display gamma) is InverseGamma.y = 2.2 / display gamma (dword 5), a power on
    // the graded colour made for SDR screens. The game's ACES HDR path never applied it; ours would (run 26: 1.354, a
    // display gamma of 1.625, crushed the shadows). In HDR it is neutral: Game brightness and Gamma take its place.
    std::array<u32, ParamDwords> params_block = block;
    if (const u32 game_power = block[InverseGammaDword]; game_power != 0x3f800000u && !brightness_noted) {
        brightness_noted = true;
        float power;
        std::memcpy(&power, &game_power, sizeof(power));
        std::printf("UE4 HDR grading: the game's Brightness (display gamma %.3f, power %.3f) is neutral in HDR\n",
                    power > 0.0f ? 2.2 / power : 0.0, power);
    }
    params_block[InverseGammaDword] = 0x3f800000u; // 1.0f
    auto& stream = buffer_cache.GetStreamBuffer();
    const u64 offset = stream.Copy(params_block.data(), sizeof(params_block), instance.StorageMinAlignment());
    const vk::Buffer params = stream.Handle();
    const auto& settings = Gow3Settings::Get();
    const LutPush push{
        .game_nits = float(settings.hdr_game_nits.load(std::memory_order_relaxed)),
        .gamma = float(settings.hdr_gamma.load(std::memory_order_relaxed)) / 10.0f,
        .lut_size = float(width),
        .flags = 0u,
        .peak_nits = float(settings.hdr_peak_nits.load(std::memory_order_relaxed)),
        .highlights = float(settings.hdr_highlights.load(std::memory_order_relaxed)) / 100.0f,
    };
    const vk::PipelineLayout pipeline_layout = *layout;
    scheduler.Record([=](vk::CommandBuffer cmdbuf) {
        const vk::RenderingAttachmentInfo attachment{
            .imageView = view,
            .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
            .loadOp = vk::AttachmentLoadOp::eDontCare,
            .storeOp = vk::AttachmentStoreOp::eStore,
        };
        cmdbuf.beginRendering({
            .renderArea = {{0, 0}, {width, height}},
            .layerCount = layers,
            .colorAttachmentCount = 1,
            .pColorAttachments = &attachment,
        });
        const vk::DescriptorBufferInfo info{.buffer = params, .offset = offset, .range = sizeof(block)};
        const vk::WriteDescriptorSet write{.dstBinding = 0,
                                           .descriptorCount = 1,
                                           .descriptorType = vk::DescriptorType::eStorageBuffer,
                                           .pBufferInfo = &info};
        cmdbuf.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline);
        cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eGraphics, pipeline_layout, 0, write);
        cmdbuf.pushConstants(pipeline_layout, vk::ShaderStageFlagBits::eFragment, 0, sizeof(push), &push);
        cmdbuf.setViewportWithCount(vk::Viewport{0.f, 0.f, float(width), float(height), 0.f, 1.f});
        cmdbuf.setScissorWithCount(vk::Rect2D{{0, 0}, {width, height}});
        cmdbuf.draw(3, layers, 0, 0);
        cmdbuf.endRendering();
    });
    // The guest pipelines' dynamic state must be emitted again after this pipeline.
    scheduler.GetDynamicState().Invalidate();
    if (++runs == 1) {
        std::printf("UE4 HDR grading: the game's SDR grading in HDR (our LUT, %ux%ux%u, %s; game brightness %.0f "
                    "nits, gamma %s)\n",
                    width, height, layers, vk::to_string(desc.view_info.format).c_str(), push.game_nits,
                    push.gamma > 0.0f ? std::to_string(push.gamma).substr(0, 3).c_str() : "off (sRGB)");
    }
    return true;
}


void Ue4HdrGrading::RunPatch(VideoCore::ImageId target, const VideoCore::TextureCache::ImageDesc& desc,
                             const std::array<u32, ParamDwords>& block, const float scene[3], float area,
                             bool hdr_output, bool neutral_brightness) {
    if (!target) {
        return;
    }
    const vk::Pipeline pipeline = Pipeline(desc.view_info.format, Kind::Patch);
    if (!pipeline) {
        return;
    }
    const auto& range = desc.view_info.range;
    const u32 width = std::max(desc.info.size.width >> range.base.level, 1u);
    const u32 height = std::max(desc.info.size.height >> range.base.level, 1u);
    const float side = std::sqrt(std::clamp(area, 0.001f, 1.0f));
    const u32 patch_w = std::clamp(u32(std::lround(side * float(width))), 1u, width);
    const u32 patch_h = std::clamp(u32(std::lround(side * float(height))), 1u, height);
    const vk::Rect2D scissor{{s32((width - patch_w) / 2), s32((height - patch_h) / 2)}, {patch_w, patch_h}};
    scheduler.EndRendering();
    const vk::ImageView view = *texture_cache.FindRenderTarget(target, desc).image_view;
    runtime.Transit(&texture_cache.GetImage(target), vk::ImageLayout::eColorAttachmentOptimal,
                    vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                    vk::AccessFlagBits2::eColorAttachmentWrite | vk::AccessFlagBits2::eColorAttachmentRead, range);
    runtime.FlushBarriers();
    std::array<u32, ParamDwords> params_block = block;
    if (hdr_output || neutral_brightness) {
        params_block[InverseGammaDword] = 0x3f800000u; // neutral: HDR as Run does; SDR when asked ("neutral")
    }
    auto& stream = buffer_cache.GetStreamBuffer();
    const u64 offset = stream.Copy(params_block.data(), sizeof(params_block), instance.StorageMinAlignment());
    const vk::Buffer params = stream.Handle();
    const auto& settings = Gow3Settings::Get();
    const PatchPush push{
        .game_nits = float(settings.hdr_game_nits.load(std::memory_order_relaxed)),
        .gamma = float(settings.hdr_gamma.load(std::memory_order_relaxed)) / 10.0f,
        .flags = hdr_output ? 1u : 0u,
        .peak_nits = float(settings.hdr_peak_nits.load(std::memory_order_relaxed)),
        .highlights = float(settings.hdr_highlights.load(std::memory_order_relaxed)) / 100.0f,
        .pad = {},
        .scene = {scene[0], scene[1], scene[2], 0.0f},
    };
    const vk::PipelineLayout pipeline_layout = *layout;
    scheduler.Record([=](vk::CommandBuffer cmdbuf) {
        const vk::RenderingAttachmentInfo attachment{
            .imageView = view,
            .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
            .loadOp = vk::AttachmentLoadOp::eLoad,
            .storeOp = vk::AttachmentStoreOp::eStore,
        };
        cmdbuf.beginRendering({
            .renderArea = {{0, 0}, {width, height}},
            .layerCount = 1,
            .colorAttachmentCount = 1,
            .pColorAttachments = &attachment,
        });
        const vk::DescriptorBufferInfo info{.buffer = params, .offset = offset, .range = sizeof(params_block)};
        const vk::WriteDescriptorSet write{.dstBinding = 0,
                                           .descriptorCount = 1,
                                           .descriptorType = vk::DescriptorType::eStorageBuffer,
                                           .pBufferInfo = &info};
        cmdbuf.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline);
        cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eGraphics, pipeline_layout, 0, write);
        cmdbuf.pushConstants(pipeline_layout, vk::ShaderStageFlagBits::eFragment, 0, sizeof(push), &push);
        cmdbuf.setViewportWithCount(vk::Viewport{0.f, 0.f, float(width), float(height), 0.f, 1.f});
        cmdbuf.setScissorWithCount(scissor);
        cmdbuf.draw(3, 1, 0, 0);
        cmdbuf.endRendering();
    });
    scheduler.GetDynamicState().Invalidate();
    if (++patch_runs == 1) {
        std::printf("Patterns: game patch over the tonemapper's output (%ux%u %s, %s)\n", width, height,
                    vk::to_string(desc.view_info.format).c_str(), hdr_output ? "HDR" : "SDR");
    }
}

bool Ue4HdrGrading::RunComposite(VideoCore::ImageId target, const VideoCore::TextureCache::ImageDesc& desc,
                                 const CompositeInput& scene, const CompositeInput& ui, bool ui_srgb) {
    if (composite_failed || !target || !scene.view || !ui.view) {
        return false;
    }
    const vk::Pipeline pipeline = Pipeline(desc.view_info.format, Kind::Composite);
    if (!pipeline) {
        composite_failed = true;
        return false;
    }
    const auto& range = desc.view_info.range;
    const u32 width = std::max(desc.info.size.width >> range.base.level, 1u);
    const u32 height = std::max(desc.info.size.height >> range.base.level, 1u);
    scheduler.EndRendering();
    const vk::ImageView view = *texture_cache.FindRenderTarget(target, desc).image_view;
    runtime.Transit(&texture_cache.GetImage(target), vk::ImageLayout::eColorAttachmentOptimal,
                    vk::PipelineStageFlagBits2::eColorAttachmentOutput, vk::AccessFlagBits2::eColorAttachmentWrite,
                    range);
    runtime.FlushBarriers();
    const auto& settings = Gow3Settings::Get();
    const CompositePush push{
        .target_size = {float(width), float(height)},
        .ui_nits = float(settings.hdr_ui_nits.load(std::memory_order_relaxed)),
        .gamma = float(settings.hdr_gamma.load(std::memory_order_relaxed)) / 10.0f,
        .flags = ui_srgb ? 1u : 0u,
    };
    const vk::PipelineLayout pipeline_layout = *composite_layout;
    scheduler.Record([=](vk::CommandBuffer cmdbuf) {
        const vk::RenderingAttachmentInfo attachment{
            .imageView = view,
            .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
            .loadOp = vk::AttachmentLoadOp::eDontCare,
            .storeOp = vk::AttachmentStoreOp::eStore,
        };
        cmdbuf.beginRendering({
            .renderArea = {{0, 0}, {width, height}},
            .layerCount = 1,
            .colorAttachmentCount = 1,
            .pColorAttachments = &attachment,
        });
        const std::array infos{
            vk::DescriptorImageInfo{VK_NULL_HANDLE, scene.view, scene.layout},
            vk::DescriptorImageInfo{VK_NULL_HANDLE, ui.view, ui.layout},
        };
        const std::array writes{
            vk::WriteDescriptorSet{.dstBinding = 0,
                                   .descriptorCount = 1,
                                   .descriptorType = vk::DescriptorType::eSampledImage,
                                   .pImageInfo = &infos[0]},
            vk::WriteDescriptorSet{.dstBinding = 1,
                                   .descriptorCount = 1,
                                   .descriptorType = vk::DescriptorType::eSampledImage,
                                   .pImageInfo = &infos[1]},
        };
        cmdbuf.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline);
        cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eGraphics, pipeline_layout, 0, writes);
        cmdbuf.pushConstants(pipeline_layout, vk::ShaderStageFlagBits::eFragment, 0, sizeof(push), &push);
        cmdbuf.setViewportWithCount(vk::Viewport{0.f, 0.f, float(width), float(height), 0.f, 1.f});
        cmdbuf.setScissorWithCount(vk::Rect2D{{0, 0}, {width, height}});
        cmdbuf.draw(3, 1, 0, 0);
        cmdbuf.endRendering();
    });
    scheduler.GetDynamicState().Invalidate();
    if (++composite_runs == 1) {
        std::printf("UE4 HDR UI composite: ours (%ux%u %s; UI %.0f nits, gamma %s%s)\n", width, height,
                    vk::to_string(desc.view_info.format).c_str(), push.ui_nits,
                    push.gamma > 0.0f ? std::to_string(push.gamma).substr(0, 3).c_str() : "off (sRGB)",
                    ui_srgb ? ", UI view sRGB" : "");
    }
    return true;
}

} // namespace Vulkan
