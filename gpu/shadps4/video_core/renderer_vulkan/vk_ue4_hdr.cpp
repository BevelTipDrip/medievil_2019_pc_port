// SPDX-FileCopyrightText: Copyright 2026 MediEvil PC port
// SPDX-License-Identifier: GPL-2.0-or-later

#include "video_core/renderer_vulkan/vk_ue4_hdr.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "gow3_settings.h"
#include "video_core/buffer_cache/buffer_cache.h"
#include "video_core/host_shaders/ue4_hdr_lut_blend_frag.h"
#include "video_core/host_shaders/ue4_hdr_lut_frag.h"
#include "video_core/host_shaders/ue4_hdr_lut_vert.h"
#include "video_core/host_shaders/ue4_tonemap_frag.h"
#include "video_core/host_shaders/ue4_ui_composite_frag.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"

namespace Vulkan {

namespace {
/// ue4_hdr_lut.frag's push constants.
struct LutPush {
    float lut_size;
    u32 flags;            ///< bit 0: film black clip forced to 0
    float pad[2];
    float pw[3][4];       ///< the curve (PiecewiseFromSettings), offset 16
};
/// The piecewise tone curve (study-hdr.md H40; ue4_hdr_map.glsl HdrPiecewiseNits) from the settings, as the shader's
/// constants: pw0 = (toe clamp, toe length, b, gamma), pw1 = (grey nits, toe power, toe nits, highlight saturation),
/// pw2 = (shoulder start, shoulder end, peak, shoulder angle).
struct Piecewise {
    float pw[3][4];
};
Piecewise PiecewiseFromSettings() {
    const auto& s = Gow3Settings::Get();
    const auto get = [](const std::atomic<int>& v) { return float(v.load(std::memory_order_relaxed)); };
    Piecewise p{};
    const float clamp_ = get(s.hdr_pw_toe_clamp) / 10000.0f;
    const float length = get(s.hdr_pw_toe_length) / 1000.0f;
    const float strength = get(s.hdr_pw_toe_strength) / 100.0f;
    const float gamma = get(s.hdr_pw_gamma) / 100.0f;
    const float b = strength * length; // < length (strength <= 0.9) and < 0.18 (length <= 0.15)
    const float grey = 0.18f * get(s.hdr_game_nits);
    const float peak = get(s.hdr_peak_nits);
    p.pw[0][0] = clamp_;
    p.pw[0][1] = length;
    p.pw[0][2] = b;
    p.pw[0][3] = gamma;
    p.pw[1][0] = grey;
    p.pw[1][1] = gamma * length / (length - b);
    p.pw[1][2] = grey * std::pow((length - b) / (0.18f - b), gamma);
    p.pw[1][3] = get(s.hdr_pw_saturation) / 100.0f;
    p.pw[2][0] = get(s.hdr_pw_shoulder_start) / 100.0f * peak;
    p.pw[2][1] = peak * std::exp2(get(s.hdr_pw_shoulder_length) / 10.0f);
    p.pw[2][2] = peak;
    p.pw[2][3] = get(s.hdr_pw_shoulder_angle) / 100.0f;
    return p;
}
static_assert(sizeof(LutPush) == 64);
/// ue4_ui_composite.frag's push constants.
struct CompositePush {
    float target_size[2];
    float ui_nits;
    float gamma;
    u32 flags;
};
static_assert(sizeof(CompositePush) == 20);
/// The UI as an SDR PC monitor shows it (gamma 2.2; the gamma rows were dropped, plan-hdr-final.md).
constexpr float UiGamma = 2.2f;
constexpr u32 OutputDeviceDword = 138; ///< study-hdr.md "LUT parameters"
constexpr u32 InverseGammaDword = 5;   ///< the game's Brightness (2.2 / UE display gamma)
constexpr vk::Format OwnLutFormat = vk::Format::eR16G16B16A16Sfloat;
} // namespace

Ue4HdrGrading::Ue4HdrGrading(const Instance& instance_, Scheduler& scheduler_,
                             VideoCore::TextureCache& texture_cache_, VideoCore::BufferCache& buffer_cache_,
                             Runtime& runtime_)
    : instance{instance_}, scheduler{scheduler_}, texture_cache{texture_cache_}, buffer_cache{buffer_cache_},
      runtime{runtime_} {
    const auto device = instance.GetDevice();
    vert = vk::UniqueShaderModule(CompileSPV(UE4_HDR_LUT_VERT, device), device);
    frag = vk::UniqueShaderModule(CompileSPV(UE4_HDR_LUT_FRAG, device), device);
    composite_frag = vk::UniqueShaderModule(CompileSPV(UE4_UI_COMPOSITE_FRAG, device), device);
    blend_frag = vk::UniqueShaderModule(CompileSPV(UE4_HDR_LUT_BLEND_FRAG, device), device);
    const vk::DescriptorSetLayoutBinding binding{0, vk::DescriptorType::eStorageBuffer, 1,
                                                 vk::ShaderStageFlagBits::eFragment};
    set_layout = Check(device.createDescriptorSetLayoutUnique({
        .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
        .bindingCount = 1,
        .pBindings = &binding,
    }));
    const auto set = *set_layout;
    const vk::PushConstantRange push{vk::ShaderStageFlagBits::eFragment, 0, sizeof(LutPush)};
    layout = Check(device.createPipelineLayoutUnique({
        .setLayoutCount = 1,
        .pSetLayouts = &set,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &push,
    }));
    // The blend builder: the parameter block and the level LUT (combined image sampler, the game's filtering).
    const std::array blend_bindings{
        vk::DescriptorSetLayoutBinding{0, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eFragment},
        vk::DescriptorSetLayoutBinding{1, vk::DescriptorType::eCombinedImageSampler, 1,
                                       vk::ShaderStageFlagBits::eFragment},
    };
    blend_set_layout = Check(device.createDescriptorSetLayoutUnique({
        .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
        .bindingCount = u32(blend_bindings.size()),
        .pBindings = blend_bindings.data(),
    }));
    const auto blend_set = *blend_set_layout;
    blend_layout = Check(device.createPipelineLayoutUnique({
        .setLayoutCount = 1,
        .pSetLayouts = &blend_set,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &push,
    }));
    lut_sampler = Check(device.createSamplerUnique({
        .magFilter = vk::Filter::eLinear,
        .minFilter = vk::Filter::eLinear,
        .mipmapMode = vk::SamplerMipmapMode::eNearest,
        .addressModeU = vk::SamplerAddressMode::eClampToEdge,
        .addressModeV = vk::SamplerAddressMode::eClampToEdge,
        .addressModeW = vk::SamplerAddressMode::eClampToEdge,
        .maxLod = 0.0f,
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
    // Our tonemapper: the game's constants (storage buffer), the dirt mask, scene, bloom, eye adaptation (combined
    // image samplers: linear, clamp to edge) and our LUT (3D).
    tonemap_frag = vk::UniqueShaderModule(CompileSPV(UE4_TONEMAP_FRAG, device), device);
    std::array<vk::DescriptorSetLayoutBinding, 6> tonemap_bindings{};
    for (u32 i = 0; i < tonemap_bindings.size(); ++i) {
        tonemap_bindings[i] = {i, i == 0 ? vk::DescriptorType::eStorageBuffer : vk::DescriptorType::eCombinedImageSampler,
                               1, vk::ShaderStageFlagBits::eFragment};
    }
    tonemap_set_layout = Check(device.createDescriptorSetLayoutUnique({
        .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
        .bindingCount = u32(tonemap_bindings.size()),
        .pBindings = tonemap_bindings.data(),
    }));
    const auto tonemap_set = *tonemap_set_layout;
    tonemap_layout = Check(device.createPipelineLayoutUnique({
        .setLayoutCount = 1,
        .pSetLayouts = &tonemap_set,
    }));
}

Ue4HdrGrading::~Ue4HdrGrading() = default;

void Ue4HdrGrading::RemapBlendBlock(const u32* src, std::array<u32, ParamDwords>& dst) {
    for (u32 i = 0; i <= 44; ++i) {
        dst[i] = src[i];
    }
    for (u32 i = 45; i <= 141; ++i) {
        dst[i] = src[i + 20];
    }
    dst[142] = src[48]; // LUTWeights[0]: the encoded colour
    dst[143] = src[52]; // LUTWeights[1]: the level LUT
}

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
                                          .module = kind == Kind::Composite ? *composite_frag
                                                    : kind == Kind::LutBlend  ? *blend_frag
                                                    : kind == Kind::Tonemap   ? *tonemap_frag
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
                            .layout = kind == Kind::Composite ? *composite_layout
                                      : kind == Kind::LutBlend  ? *blend_layout
                                      : kind == Kind::Tonemap   ? *tonemap_layout
                                                                : *layout,
                        });
    if (pipeline.result != vk::Result::eSuccess) {
        std::printf("UE4 HDR grading: pipeline for format %s failed; the game's grading LUT is kept\n",
                    vk::to_string(format).c_str());
        return {};
    }
    const vk::Pipeline result = *pipeline.value;
    pipelines.emplace_back(std::pair{format, kind}, std::move(pipeline.value));
    if constexpr (GOW3_DEV_TOOLS) {
        std::printf("UE4 HDR grading: pipeline ready (%s)\n", vk::to_string(format).c_str());
    }
    return result;
}

bool Ue4HdrGrading::EnsureOwnLut(u32 size) {
    if (own_lut_failed) {
        return false;
    }
    if (own_lut_layers && own_lut_size == size) {
        return true;
    }
    const auto device = instance.GetDevice();
    own_lut = VideoCore::UniqueImage(device, instance.GetAllocator());
    own_lut.Create({
        .flags = vk::ImageCreateFlagBits::e2DArrayCompatible,
        .imageType = vk::ImageType::e3D,
        .format = OwnLutFormat,
        .extent = {size, size, size},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1,
        .tiling = vk::ImageTiling::eOptimal,
        .usage = vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eSampled,
    });
    const vk::Image image = vk::Image(own_lut);
    if (!image) {
        std::printf("UE4 HDR grading: our LUT image could not be made; the game's tonemapper is kept\n");
        own_lut_failed = true;
        return false;
    }
    own_lut_layers = Check(device.createImageViewUnique({
        .image = image,
        .viewType = vk::ImageViewType::e2DArray,
        .format = OwnLutFormat,
        .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, size},
    }));
    own_lut_volume = Check(device.createImageViewUnique({
        .image = image,
        .viewType = vk::ImageViewType::e3D,
        .format = OwnLutFormat,
        .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
    }));
    own_lut_size = size;
    own_lut_built = false;
    return true;
}

bool Ue4HdrGrading::Run(VideoCore::ImageId target, const VideoCore::TextureCache::ImageDesc& desc,
                        const std::array<u32, ParamDwords>& block, const CompositeInput* level_lut) {
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
    const Kind kind = level_lut ? Kind::LutBlend : Kind::Lut;
    const vk::Pipeline pipeline = Pipeline(desc.view_info.format, kind);
    if (!pipeline) {
        failed = true;
        return false;
    }
    if (GOW3_DEV_TOOLS && level_lut && !blend_noted) {
        blend_noted = true;
        float w0, w1;
        std::memcpy(&w0, &block[142], sizeof(w0));
        std::memcpy(&w1, &block[143], sizeof(w1));
        std::printf("UE4 HDR grading: the builder with a level grading LUT (fs 9f6bac6cba984956), weights %.3f / %.3f\n",
                    w0, w1);
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
    if (const u32 game_power = block[InverseGammaDword];
        GOW3_DEV_TOOLS && game_power != 0x3f800000u && !brightness_noted) {
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
    const Piecewise piecewise = PiecewiseFromSettings();
    LutPush push{
        .lut_size = float(width),
        .flags = 0u,
    };
    std::memcpy(push.pw, piecewise.pw, sizeof(push.pw));
    const vk::PipelineLayout pipeline_layout = level_lut ? *blend_layout : *layout;
    const CompositeInput lut = level_lut ? *level_lut : CompositeInput{};
    const vk::Sampler sampler = *lut_sampler;
    // Our LUT (our tonemapper's, PQ(scene x 0.01)), in the same recording. Without it the game's tonemapper stays.
    const bool own = EnsureOwnLut(width);
    const vk::Pipeline own_pipeline = own ? Pipeline(OwnLutFormat, kind) : vk::Pipeline{};
    const vk::ImageView own_view = own_pipeline ? *own_lut_layers : vk::ImageView{};
    const vk::Image own_image = own_pipeline ? vk::Image(own_lut) : vk::Image{};
    LutPush own_push = push;
    own_push.flags |= 4u;
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
        const vk::DescriptorImageInfo lut_info{sampler, lut.view, lut.layout};
        const std::array writes{
            vk::WriteDescriptorSet{.dstBinding = 0,
                                   .descriptorCount = 1,
                                   .descriptorType = vk::DescriptorType::eStorageBuffer,
                                   .pBufferInfo = &info},
            vk::WriteDescriptorSet{.dstBinding = 1,
                                   .descriptorCount = 1,
                                   .descriptorType = vk::DescriptorType::eCombinedImageSampler,
                                   .pImageInfo = &lut_info},
        };
        cmdbuf.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline);
        cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eGraphics, pipeline_layout, 0,
                                    vk::ArrayProxy<const vk::WriteDescriptorSet>(lut.view ? 2u : 1u, writes.data()));
        cmdbuf.pushConstants(pipeline_layout, vk::ShaderStageFlagBits::eFragment, 0, sizeof(push), &push);
        cmdbuf.setViewportWithCount(vk::Viewport{0.f, 0.f, float(width), float(height), 0.f, 1.f});
        cmdbuf.setScissorWithCount(vk::Rect2D{{0, 0}, {width, height}});
        cmdbuf.draw(3, layers, 0, 0);
        cmdbuf.endRendering();
        if (!own_view) {
            return;
        }
        const vk::ImageSubresourceRange whole{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
        const vk::ImageMemoryBarrier2 to_write{
            .srcStageMask = vk::PipelineStageFlagBits2::eFragmentShader,
            .srcAccessMask = vk::AccessFlagBits2::eShaderRead,
            .dstStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
            .dstAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
            .oldLayout = vk::ImageLayout::eUndefined, // every texel rewritten
            .newLayout = vk::ImageLayout::eColorAttachmentOptimal,
            .image = own_image,
            .subresourceRange = whole,
        };
        cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &to_write});
        const vk::RenderingAttachmentInfo own_attachment{
            .imageView = own_view,
            .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
            .loadOp = vk::AttachmentLoadOp::eDontCare,
            .storeOp = vk::AttachmentStoreOp::eStore,
        };
        cmdbuf.beginRendering({
            .renderArea = {{0, 0}, {width, height}},
            .layerCount = layers,
            .colorAttachmentCount = 1,
            .pColorAttachments = &own_attachment,
        });
        cmdbuf.bindPipeline(vk::PipelineBindPoint::eGraphics, own_pipeline);
        cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eGraphics, pipeline_layout, 0,
                                    vk::ArrayProxy<const vk::WriteDescriptorSet>(lut.view ? 2u : 1u, writes.data()));
        cmdbuf.pushConstants(pipeline_layout, vk::ShaderStageFlagBits::eFragment, 0, sizeof(own_push), &own_push);
        cmdbuf.setViewportWithCount(vk::Viewport{0.f, 0.f, float(width), float(height), 0.f, 1.f});
        cmdbuf.setScissorWithCount(vk::Rect2D{{0, 0}, {width, height}});
        cmdbuf.draw(3, layers, 0, 0);
        cmdbuf.endRendering();
        const vk::ImageMemoryBarrier2 to_read{
            .srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
            .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eFragmentShader,
            .dstAccessMask = vk::AccessFlagBits2::eShaderRead,
            .oldLayout = vk::ImageLayout::eColorAttachmentOptimal,
            .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
            .image = own_image,
            .subresourceRange = whole,
        };
        cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &to_read});
    });
    own_lut_built = own_view != vk::ImageView{};
    // The guest pipelines' dynamic state must be emitted again after this pipeline.
    scheduler.GetDynamicState().Invalidate();
    if (GOW3_DEV_TOOLS && ++runs == 1) {
        std::printf("UE4 HDR grading: our LUT (%ux%ux%u, %s)\n", width, height, layers,
                    vk::to_string(desc.view_info.format).c_str());
    }
    return true;
}


bool Ue4HdrGrading::RunTonemap(VideoCore::ImageId target, const VideoCore::TextureCache::ImageDesc& desc,
                               const TonemapInputs& in, const std::array<u32, TonemapDwords>& constants,
                               const vk::Rect2D& rect, const vk::ClearColorValue* clear) {
    if (tonemap_failed || !target || !own_lut_built || !in.scene.view || !in.bloom.view || !in.dirt_mask.view ||
        !in.eye.view) {
        return false;
    }
    const vk::Pipeline pipeline = Pipeline(desc.view_info.format, Kind::Tonemap);
    if (!pipeline) {
        tonemap_failed = true;
        return false;
    }
    const auto& range = desc.view_info.range;
    const u32 width = std::max(desc.info.size.width >> range.base.level, 1u);
    const u32 height = std::max(desc.info.size.height >> range.base.level, 1u);
    scheduler.EndRendering();
    // As the skipped draw's colour target (the inputs were made shader-readable by the caller).
    const vk::ImageView view = *texture_cache.FindRenderTarget(target, desc).image_view;
    runtime.Transit(&texture_cache.GetImage(target), vk::ImageLayout::eColorAttachmentOptimal,
                    vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                    vk::AccessFlagBits2::eColorAttachmentWrite | vk::AccessFlagBits2::eColorAttachmentRead, range);
    runtime.FlushBarriers();
    auto& stream = buffer_cache.GetStreamBuffer();
    const u64 offset = stream.Copy(constants.data(), sizeof(constants), instance.StorageMinAlignment());
    const vk::Buffer buffer = stream.Handle();
    const vk::Sampler sampler = *lut_sampler;
    const vk::ImageView lut_view = *own_lut_volume;
    const vk::PipelineLayout pipeline_layout = *tonemap_layout;
    const vk::Rect2D scissor = rect;
    const bool clear_first = clear != nullptr;
    const vk::ClearColorValue clear_value = clear ? *clear : vk::ClearColorValue{};
    scheduler.Record([=](vk::CommandBuffer cmdbuf) {
        const vk::RenderingAttachmentInfo attachment{
            .imageView = view,
            .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
            .loadOp = clear_first ? vk::AttachmentLoadOp::eClear : vk::AttachmentLoadOp::eLoad,
            .storeOp = vk::AttachmentStoreOp::eStore,
            .clearValue = vk::ClearValue{.color = clear_value},
        };
        cmdbuf.beginRendering({
            .renderArea = {{0, 0}, {width, height}},
            .layerCount = 1,
            .colorAttachmentCount = 1,
            .pColorAttachments = &attachment,
        });
        const vk::DescriptorBufferInfo info{.buffer = buffer, .offset = offset, .range = sizeof(constants)};
        const std::array images{
            vk::DescriptorImageInfo{sampler, in.dirt_mask.view, in.dirt_mask.layout},
            vk::DescriptorImageInfo{sampler, in.scene.view, in.scene.layout},
            vk::DescriptorImageInfo{sampler, in.bloom.view, in.bloom.layout},
            vk::DescriptorImageInfo{sampler, in.eye.view, in.eye.layout},
            vk::DescriptorImageInfo{sampler, lut_view, vk::ImageLayout::eShaderReadOnlyOptimal},
        };
        std::array<vk::WriteDescriptorSet, 6> writes{};
        writes[0] = {.dstBinding = 0,
                     .descriptorCount = 1,
                     .descriptorType = vk::DescriptorType::eStorageBuffer,
                     .pBufferInfo = &info};
        for (u32 i = 0; i < images.size(); ++i) {
            writes[i + 1] = {.dstBinding = i + 1,
                             .descriptorCount = 1,
                             .descriptorType = vk::DescriptorType::eCombinedImageSampler,
                             .pImageInfo = &images[i]};
        }
        cmdbuf.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline);
        cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eGraphics, pipeline_layout, 0, writes);
        cmdbuf.setViewportWithCount(vk::Viewport{0.f, 0.f, float(width), float(height), 0.f, 1.f});
        cmdbuf.setScissorWithCount(scissor);
        cmdbuf.draw(3, 1, 0, 0);
        cmdbuf.endRendering();
    });
    scheduler.GetDynamicState().Invalidate();
    if (GOW3_DEV_TOOLS && ++tonemap_runs == 1) {
        std::printf("UE4 HDR tonemapper: ours (%ux%u %s, rect %d,%d %ux%u)\n", width, height,
                    vk::to_string(desc.view_info.format).c_str(), rect.offset.x, rect.offset.y, rect.extent.width,
                    rect.extent.height);
    }
    return true;
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
        .gamma = UiGamma,
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
    if (GOW3_DEV_TOOLS && ++composite_runs == 1) {
        std::printf("UE4 HDR UI composite: ours (%ux%u %s; UI %.0f nits, gamma %s%s)\n", width, height,
                    vk::to_string(desc.view_info.format).c_str(), push.ui_nits,
                    push.gamma > 0.0f ? std::to_string(push.gamma).substr(0, 3).c_str() : "off (sRGB)",
                    ui_srgb ? ", UI view sRGB" : "");
    }
    return true;
}

vk::ImageView Ue4HdrGrading::RunBackdrop(const CompositeInput& scene, const CompositeInput& ui, u32 width,
                                         u32 height) {
    constexpr vk::Format format = vk::Format::eR8G8B8A8Unorm;
    if (composite_failed || !scene.view || !ui.view || !width || !height) {
        return {};
    }
    const vk::Pipeline pipeline = Pipeline(format, Kind::Composite);
    if (!pipeline) {
        return {};
    }
    const auto device = instance.GetDevice();
    if (!backdrop_view || backdrop_width != width || backdrop_height != height) {
        if (backdrop_view) {
            struct Old {
                VideoCore::UniqueImage image;
                vk::UniqueImageView view;
            };
            auto old = std::make_shared<Old>(Old{std::move(backdrop), std::move(backdrop_view)});
            scheduler.DeferOperation([old] {});
        }
        backdrop = VideoCore::UniqueImage(device, instance.GetAllocator());
        backdrop.Create({
            .imageType = vk::ImageType::e2D,
            .format = format,
            .extent = {width, height, 1},
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = vk::SampleCountFlagBits::e1,
            .tiling = vk::ImageTiling::eOptimal,
            .usage = vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eSampled,
        });
        backdrop_view = Check(device.createImageViewUnique({
            .image = vk::Image(backdrop),
            .viewType = vk::ImageViewType::e2D,
            .format = format,
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        }));
        backdrop_width = width;
        backdrop_height = height;
    }
    scheduler.EndRendering();
    runtime.FlushBarriers(); // the inputs' transitions (PrepareHdrBlur)
    const auto& settings = Gow3Settings::Get();
    const CompositePush push{
        .target_size = {float(width), float(height)},
        .ui_nits = float(settings.hdr_ui_nits.load(std::memory_order_relaxed)),
        .gamma = UiGamma,
        .flags = 2u,
    };
    const vk::PipelineLayout pipeline_layout = *composite_layout;
    const vk::Image image = vk::Image(backdrop);
    const vk::ImageView view = *backdrop_view;
    scheduler.Record([=](vk::CommandBuffer cmdbuf) {
        const vk::ImageSubresourceRange range{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
        const vk::ImageMemoryBarrier2 to_write{
            .srcStageMask = vk::PipelineStageFlagBits2::eFragmentShader,
            .srcAccessMask = vk::AccessFlagBits2::eShaderRead,
            .dstStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
            .dstAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
            .oldLayout = vk::ImageLayout::eUndefined, // fully rewritten
            .newLayout = vk::ImageLayout::eColorAttachmentOptimal,
            .image = image,
            .subresourceRange = range,
        };
        cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &to_write});
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
        const vk::ImageMemoryBarrier2 to_read{
            .srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
            .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eFragmentShader,
            .dstAccessMask = vk::AccessFlagBits2::eShaderRead,
            .oldLayout = vk::ImageLayout::eColorAttachmentOptimal,
            .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
            .image = image,
            .subresourceRange = range,
        };
        cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &to_read});
    });
    scheduler.GetDynamicState().Invalidate();
    if (GOW3_DEV_TOOLS && ++backdrop_runs == 1) {
        std::printf("UE4 HDR UI composite: background blur reads our backdrop (%ux%u, UI %.0f nits)\n", width,
                    height, push.ui_nits);
    }
    return view;
}

} // namespace Vulkan
