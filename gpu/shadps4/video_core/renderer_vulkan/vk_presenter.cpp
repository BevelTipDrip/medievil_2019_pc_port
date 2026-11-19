// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#ifdef _WIN32
#include <windows.h>
#endif
#include <cstdlib>
#include <immintrin.h>
#include "common/debug.h"
#include "common/thread.h"
#include "gow3_flip_trace.h"
#include "gow3_stage_b_wait.h"
#include "common/elf_info.h"
#include "common/io_file.h"
#include "common/path_util.h"
#include "common/singleton.h"
#include "core/debug_state.h"
#include "core/emulator_settings.h"
#include "sdl_window.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/renderdoc.h"
#include "video_core/renderer_vulkan/vk_platform.h"
#include "gow3_overlay.h"
#include "gow3_patterns.h"
#include "gow3_settings.h"
#include "gow3_graphics.h"
#include "video_core/renderer_vulkan/vk_temporal_upscaler.h"
#include "video_core/renderer_vulkan/vk_presenter.h"
#include "video_core/renderer_vulkan/vk_warmup_inbox.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/texture_cache/image.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <csetjmp>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <memory>
#include <span>
#include <sstream>
#include <system_error>
#include <vector>
#include <thread>
#include <vk_mem_alloc.h>
#include <miniz.h>
#ifdef MemoryBarrier
#undef MemoryBarrier // gow3: winnt.h macro (through fmt), clashes with vk::MemoryBarrier
#endif

namespace Vulkan {

namespace {
// gow3 HDR near-black correction (pp_pass.h Settings::black_lut): hdr-black-correction.txt beside medievil.ini holds
// "requested measured" nit pairs from a meter run (measure_patches.py --write-correction); the table maps each input PQ
// code below the limit to the code whose measured output is the input's luminance. Re-read when the file changes
// (checked at most once a second).
struct BlackCorrection {
    std::filesystem::file_time_type stamp{};
    std::chrono::steady_clock::time_point checked{};
    bool valid = false;
    float limit = 0.0f;
    std::array<float, HostPasses::PostProcessingPass::BLACK_LUT_SIZE> lut{};
} black_correction;

float PqFromNits(double nits) {
    const double y = std::pow(std::clamp(nits / 10000.0, 0.0, 1.0), 0.1593017578125);
    return float(std::pow((0.8359375 + 18.8515625 * y) / (1.0 + 18.6875 * y), 78.84375));
}
double NitsFromPq(double code) {
    const double p = std::pow(std::clamp(code, 0.0, 1.0), 1.0 / 78.84375);
    return 10000.0 * std::pow(std::max(p - 0.8359375, 0.0) / (18.8515625 - 18.6875 * p), 1.0 / 0.1593017578125);
}

void RefreshBlackCorrection() {
    auto& c = black_correction;
    const auto now = std::chrono::steady_clock::now();
    if (now - c.checked < std::chrono::seconds(1)) {
        return;
    }
    c.checked = now;
    const char* config = std::getenv("GOW3_CONFIG");
    const auto path = std::filesystem::path(config && config[0] ? config : "medievil.ini").parent_path() /
                      "hdr-black-correction.txt";
    std::error_code error;
    const auto stamp = std::filesystem::last_write_time(path, error);
    if (error) {
        c.valid = false;
        return;
    }
    if (c.valid && stamp == c.stamp) {
        return;
    }
    c.stamp = stamp;
    c.valid = false;
    std::vector<std::pair<double, double>> points{{0.0, 0.0}}; // requested, measured
    std::ifstream file(path);
    for (std::string line; std::getline(file, line);) {
        if (line.empty() || line[0] == '#') {
            continue;
        }
        std::istringstream in(line);
        double requested = 0, measured = 0;
        if (in >> requested >> measured && requested > 0 && measured > 0) {
            points.emplace_back(requested, measured);
        }
    }
    std::sort(points.begin(), points.end());
    for (size_t i = 1; i < points.size(); ++i) { // measured output never falls as the request rises
        points[i].second = std::max(points[i].second, points[i - 1].second);
    }
    if (points.size() < 4) {
        std::printf("HDR near-black correction: %s has too few points; off\n", path.string().c_str());
        return;
    }
    // The limit: the highest request; the table maps to identity there.
    const double limit_nits = points.back().first;
    c.limit = PqFromNits(limit_nits);
    const size_t n = c.lut.size();
    for (size_t k = 0; k < n; ++k) {
        const double target = NitsFromPq(double(c.limit) * double(k) / double(n - 1));
        // The request whose measured output is `target` (interpolated between the measured points).
        double request = target;
        for (size_t i = 1; i < points.size(); ++i) {
            if (target <= points[i].second) {
                const auto& [r0, m0] = points[i - 1];
                const auto& [r1, m1] = points[i];
                request = m1 > m0 ? r0 + (r1 - r0) * (target - m0) / (m1 - m0) : r1;
                break;
            }
        }
        // Blend to identity over the last 15% of the table, so it joins the uncorrected range without a step.
        const double w = std::clamp((double(k) / double(n - 1) - 0.85) / 0.15, 0.0, 1.0);
        request = request * (1.0 - w) + target * w;
        c.lut[k] = PqFromNits(request);
    }
    c.lut[0] = 0.0f;
    c.valid = true;
    std::printf("HDR near-black correction: %zu points from %s, up to %.1f nits (1 nit -> sent as %.3f)\n",
                points.size() - 1, path.string().c_str(), limit_nits,
                NitsFromPq(c.lut[std::min<size_t>(n - 1, size_t(std::lround(PqFromNits(1.0) / c.limit * (n - 1))))]));
    std::fflush(stdout);
}
} // namespace

namespace {
// gow3 test patterns (gow3_patterns.h, research/docs/plan-meter-measurement.md): a raw patch written into the swapchain
// image by a buffer-to-image copy (exact bits, any format), the rest of the frame optionally cleared. Three slots in one
// host buffer, one per frame in flight, refilled when the patch or the format changes.
struct RawPatchBuffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation allocation = nullptr;
    u8* mapped = nullptr;
    static constexpr u32 SlotPixels = 2048u * 2048u;
    struct Slot {
        u32 pixel = 0, width = 0, height = 0;
        bool filled = false;
    } slots[3];
    u32 next = 0;
} raw_patch;

/// The pixel of normalized codes `v` in `format`, or false for a format this does not know.
bool PackPixel(vk::Format format, const float v[3], u32& pixel) {
    const auto c8 = [&](int i) { return u32(std::lround(std::clamp(v[i], 0.0f, 1.0f) * 255.0f)); };
    const auto c10 = [&](int i) { return u32(std::lround(std::clamp(v[i], 0.0f, 1.0f) * 1023.0f)); };
    switch (format) {
    case vk::Format::eB8G8R8A8Unorm:
    case vk::Format::eB8G8R8A8Srgb:
        pixel = c8(2) | c8(1) << 8 | c8(0) << 16 | 255u << 24;
        return true;
    case vk::Format::eR8G8B8A8Unorm:
    case vk::Format::eR8G8B8A8Srgb:
        pixel = c8(0) | c8(1) << 8 | c8(2) << 16 | 255u << 24;
        return true;
    case vk::Format::eA2B10G10R10UnormPack32:
        pixel = c10(0) | c10(1) << 10 | c10(2) << 20 | 3u << 30;
        return true;
    case vk::Format::eA2R10G10B10UnormPack32:
        pixel = c10(2) | c10(1) << 10 | c10(0) << 20 | 3u << 30;
        return true;
    default:
        return false;
    }
}

bool IsSrgbFormat(vk::Format format) {
    return format == vk::Format::eB8G8R8A8Srgb || format == vk::Format::eR8G8B8A8Srgb;
}

/// Records the patch into `image` (TransferDstOptimal, after the frame's blit). The image stays TransferDstOptimal.
void RecordRawPatch(const Instance& instance, vk::CommandBuffer cmdbuf, vk::Image image, vk::Extent2D extent,
                    vk::Format format, const vk::ImageSubresourceRange& range) {
    const Gow3Patterns::Patch patch = Gow3Patterns::Current();
    if (patch.mode != Gow3Patterns::Raw) {
        return;
    }
    u32 pixel = 0;
    if (!PackPixel(format, patch.value, pixel)) {
        static bool warned = false;
        if (!std::exchange(warned, true)) {
            std::printf("Patterns: swapchain format %s not supported for raw patches\n",
                        vk::to_string(format).c_str());
        }
        return;
    }
    if (!raw_patch.buffer) {
        const VkBufferCreateInfo ci{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                    .size = VkDeviceSize(RawPatchBuffer::SlotPixels) * 4 * 3,
                                    .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT};
        const VmaAllocationCreateInfo alloc{.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT |
                                                     VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT,
                                            .usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST};
        VmaAllocationInfo info{};
        if (vmaCreateBuffer(instance.GetAllocator(), &ci, &alloc, &raw_patch.buffer, &raw_patch.allocation,
                            &info) != VK_SUCCESS) {
            raw_patch.buffer = VK_NULL_HANDLE;
            std::printf("Patterns: no host buffer for raw patches\n");
            return;
        }
        raw_patch.mapped = static_cast<u8*>(info.pMappedData);
    }
    const float side = std::sqrt(std::clamp(patch.area, 0.001f, 1.0f));
    const u32 width = std::clamp(u32(std::lround(side * float(extent.width))), 1u, std::min(extent.width, 2048u));
    const u32 height = std::clamp(u32(std::lround(side * float(extent.height))), 1u, std::min(extent.height, 2048u));
    const u32 slot_index = raw_patch.next++ % 3;
    auto& slot = raw_patch.slots[slot_index];
    u32* const pixels = reinterpret_cast<u32*>(raw_patch.mapped) + size_t(slot_index) * RawPatchBuffer::SlotPixels;
    if (!slot.filled || slot.pixel != pixel || slot.width != width || slot.height != height) {
        std::fill(pixels, pixels + size_t(width) * height, pixel);
        vmaFlushAllocation(instance.GetAllocator(), raw_patch.allocation,
                           VkDeviceSize(slot_index) * RawPatchBuffer::SlotPixels * 4, VkDeviceSize(width) * height * 4);
        slot = {pixel, width, height, true};
    }
    const vk::MemoryBarrier after_write{.srcAccessMask = vk::AccessFlagBits::eTransferWrite,
                                        .dstAccessMask = vk::AccessFlagBits::eTransferWrite};
    cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eTransfer, {},
                           after_write, {}, {});
    if (patch.background) {
        // A clear colour is linear for an sRGB format (encoded on write) and the code itself for UNORM.
        std::array<float, 4> bg{patch.bg[0], patch.bg[1], patch.bg[2], 1.0f};
        if (IsSrgbFormat(format)) {
            for (int c = 0; c < 3; ++c) {
                const float v = bg[c];
                bg[c] = v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f);
            }
        }
        cmdbuf.clearColorImage(image, vk::ImageLayout::eTransferDstOptimal, vk::ClearColorValue{bg}, range);
        cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eTransfer, {},
                               after_write, {}, {});
    }
    const vk::BufferImageCopy region{
        .bufferOffset = VkDeviceSize(slot_index) * RawPatchBuffer::SlotPixels * 4,
        .bufferRowLength = width,
        .bufferImageHeight = height,
        .imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
        .imageOffset = {s32((extent.width - width) / 2), s32((extent.height - height) / 2), 0},
        .imageExtent = {width, height, 1},
    };
    cmdbuf.copyBufferToImage(vk::Buffer(raw_patch.buffer), image, vk::ImageLayout::eTransferDstOptimal, region);
}
} // namespace

bool CanBlitToSwapchain(const vk::PhysicalDevice physical_device, vk::Format format) {
    const vk::FormatProperties props{physical_device.getFormatProperties(format)};
    return static_cast<bool>(props.optimalTilingFeatures & vk::FormatFeatureFlagBits::eBlitDst);
}

[[nodiscard]] vk::ImageSubresourceLayers MakeImageSubresourceLayers() {
    return vk::ImageSubresourceLayers{
        .aspectMask = vk::ImageAspectFlagBits::eColor,
        .mipLevel = 0,
        .baseArrayLayer = 0,
        .layerCount = 1,
    };
}

[[nodiscard]] vk::ImageBlit MakeImageBlit(s32 frame_width, s32 frame_height, s32 dst_width,
                                          s32 dst_height, s32 offset_x, s32 offset_y) {
    return vk::ImageBlit{
        .srcSubresource = MakeImageSubresourceLayers(),
        .srcOffsets =
            std::array{
                vk::Offset3D{
                    .x = 0,
                    .y = 0,
                    .z = 0,
                },
                vk::Offset3D{
                    .x = frame_width,
                    .y = frame_height,
                    .z = 1,
                },
            },
        .dstSubresource = MakeImageSubresourceLayers(),
        .dstOffsets =
            std::array{
                vk::Offset3D{
                    .x = offset_x,
                    .y = offset_y,
                    .z = 0,
                },
                vk::Offset3D{
                    .x = offset_x + dst_width,
                    .y = offset_y + dst_height,
                    .z = 1,
                },
            },
    };
}

[[nodiscard]] vk::ImageBlit MakeImageBlitStretch(s32 frame_width, s32 frame_height,
                                                 s32 swapchain_width, s32 swapchain_height) {
    return MakeImageBlit(frame_width, frame_height, swapchain_width, swapchain_height, 0, 0);
}

static vk::Rect2D FitImage(s32 frame_width, s32 frame_height, s32 swapchain_width,
                           s32 swapchain_height) {
    float frame_aspect = static_cast<float>(frame_width) / frame_height;
    float swapchain_aspect = static_cast<float>(swapchain_width) / swapchain_height;

    u32 dst_width = swapchain_width;
    u32 dst_height = swapchain_height;

    if (frame_aspect > swapchain_aspect) {
        dst_height = static_cast<s32>(swapchain_width / frame_aspect);
    } else {
        dst_width = static_cast<s32>(swapchain_height * frame_aspect);
    }

    const s32 offset_x = (swapchain_width - dst_width) / 2;
    const s32 offset_y = (swapchain_height - dst_height) / 2;

    return vk::Rect2D{{offset_x, offset_y}, {dst_width, dst_height}};
}

[[nodiscard]] vk::ImageBlit MakeImageBlitFit(s32 frame_width, s32 frame_height, s32 swapchain_width,
                                             s32 swapchain_height) {
    const auto& dst_rect = FitImage(frame_width, frame_height, swapchain_width, swapchain_height);

    return MakeImageBlit(frame_width, frame_height, dst_rect.extent.width, dst_rect.extent.height,
                         dst_rect.offset.x, dst_rect.offset.y);
}

// gow3: screenshot capture and ImGui overlays removed; the port presents directly.

Presenter::Presenter(Frontend::WindowSDL& window_, AmdGpu::Liverpool* liverpool_)
    : window{window_}, liverpool{liverpool_},
      instance{window, EmulatorSettings.GetGpuId(), EmulatorSettings.IsVkValidationEnabled(),
               EmulatorSettings.IsVkCrashDiagnosticEnabled()},
      draw_scheduler{instance, true}, present_scheduler{instance}, flip_scheduler{instance},
      swapchain{instance, window}, runtime{instance, draw_scheduler},
      rasterizer{std::make_unique<Rasterizer>(instance, draw_scheduler, runtime, liverpool)},
      texture_cache{rasterizer->GetTextureCache()} {
    const u32 num_images = swapchain.GetImageCount();
    const vk::Device device = instance.GetDevice();

    // Create presentation frames.
    present_frames.resize(num_images);
    for (u32 i = 0; i < num_images; i++) {
        Frame& frame = present_frames[i];
        frame.id = i;
        auto fence = Check<"create present done fence">(
            device.createFence({.flags = vk::FenceCreateFlagBits::eSignaled}));
        frame.present_done = fence;
        free_queue.push(&frame);
    }

    fsr_pass.Create(device, instance.GetAllocator(), num_images);
    pp_pass.Create(device, swapchain.GetSurfaceFormat().format);
    Gow3Overlay::Init(instance, swapchain.GetSurfaceFormat().format, num_images);

    const bool binary_cache = EmulatorSettingsImpl::Flag("GOW3_PIPELINE_BINARIES", true) &&
                              EmulatorSettings.IsPipelineCacheEnabled();
    const bool capture_only = EmulatorSettingsImpl::Flag("GOW3_PIPELINE_BINARY_ISOLATED", false) &&
                              EmulatorSettingsImpl::Flag("GOW3_PIPELINE_CAPTURE_ONLY", false);
    // MediEvil keeps the on-demand start: the background warm-up of the Uncharted port (pipelines
    // built from the cache during the title screen) drew levels black with red outlines here
    // (the Graveyard, 29 Oct 2026; validation: descriptor layouts missing bindings).
    if (UseOnDemandPipelines(binary_cache, instance.GetPipelineBinaryGlobalKey().keySize != 0,
                            capture_only)) {
        rasterizer->GetPipelineCache().StartOnDemand();
        return;
    }

    // gow3: by default the cached pipelines load while the game runs (a corner note shows the
    // progress). Otherwise rebuilding them takes up to minutes before the first game frame:
    // show what is happening instead of a black window.
    const char* background_warmup = std::getenv("GOW3_BACKGROUND_WARMUP");
    if ((!background_warmup || *background_warmup != '0') &&
        rasterizer->GetPipelineCache().StartBackgroundWarmUp()) {
        return;
    }
    Gow3Overlay::SetLoading(true, 0, 0);
    PresentLoadingFrame();
    auto last = std::chrono::steady_clock::now();
    rasterizer->GetPipelineCache().WarmUp([&](u32 done, u32 total) {
        const auto now = std::chrono::steady_clock::now();
        if (now - last < std::chrono::milliseconds(250)) {
            return;
        }
        last = now;
        Gow3Overlay::SetLoading(true, done, total);
        PresentLoadingFrame();
    });
    Gow3Overlay::SetLoading(false, 0, 0);

}

Presenter::~Presenter() {

    draw_scheduler.Finish();
    present_scheduler.Finish();
    flip_scheduler.Finish();
    Check(draw_scheduler.CommandBuffer().reset());
    Check(present_scheduler.CommandBuffer().reset());
    Check(flip_scheduler.CommandBuffer().reset());

    const vk::Device device = instance.GetDevice();
    for (auto& frame : present_frames) {
        vmaDestroyImage(instance.GetAllocator(), frame.image, frame.allocation);
        device.destroyImageView(frame.image_view);
        device.destroyFence(frame.present_done);
    }
}

bool Presenter::IsVideoOutSurface(const AmdGpu::ColorBuffer& color_buffer) const {
    return std::ranges::find(vo_buffers_addr, color_buffer.Address()) != vo_buffers_addr.cend();
}

void Presenter::RecreateFrame(Frame* frame, u32 width, u32 height) {
    const vk::Device device = instance.GetDevice();
    if (frame->image_view) {
        device.destroyImageView(frame->image_view);
    }
    if (frame->image) {
        vmaDestroyImage(instance.GetAllocator(), frame->image, frame->allocation);
    }

    // gow3: 10-bit while the swapchain is HDR10, so the game's PQ frame is not cut to 8 bits before
    // the blit (pp_pass draws it with its HDR pipeline; study-hdr.md, step 3).
    const vk::Format format = swapchain.GetHDR() ? HostPasses::PostProcessingPass::HDR_FRAME_FORMAT
                                                 : swapchain.GetSurfaceFormat().format;
    const vk::ImageCreateInfo image_info = {
        .flags = vk::ImageCreateFlagBits::eMutableFormat,
        .imageType = vk::ImageType::e2D,
        .format = format,
        .extent = {width, height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1,
        .usage = vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eTransferDst |
                 vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eSampled,
    };

    const VmaAllocationCreateInfo alloc_info = {
        .flags = VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT,
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
        .requiredFlags = 0,
        .preferredFlags = 0,
        .pool = VK_NULL_HANDLE,
        .pUserData = nullptr,
    };

    VkImage unsafe_image{};
    VkImageCreateInfo unsafe_image_info = static_cast<VkImageCreateInfo>(image_info);

    VkResult result = vmaCreateImage(instance.GetAllocator(), &unsafe_image_info, &alloc_info,
                                     &unsafe_image, &frame->allocation, nullptr);
    if (result != VK_SUCCESS) [[unlikely]] {
        LOG_CRITICAL(Render_Vulkan, "Failed allocating texture with error {}",
                     vk::to_string(vk::Result{result}));
        UNREACHABLE();
    }
    frame->image = vk::Image{unsafe_image};
    SetObjectName(device, frame->image, "Frame image #{}", frame->id);

    const vk::ImageViewCreateInfo view_info = {
        .image = frame->image,
        .viewType = vk::ImageViewType::e2D,
        .format = format,
        .subresourceRange{
            .aspectMask = vk::ImageAspectFlagBits::eColor,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
    };
    auto view = Check<"create frame image view">(device.createImageView(view_info));
    frame->image_view = view;
    frame->width = width;
    frame->height = height;

    frame->is_hdr = swapchain.GetHDR();
}

Frame* Presenter::PrepareLastFrame() {
    if (last_submit_frame == nullptr) {
        return nullptr;
    }

    Frame* frame = last_submit_frame;

    while (true) {
        vk::Result result = instance.GetDevice().waitForFences(frame->present_done, false,
                                                               std::numeric_limits<u64>::max());
        if (result == vk::Result::eSuccess) {
            break;
        }
        if (result == vk::Result::eTimeout) {
            continue;
        }
        ASSERT_MSG(result != vk::Result::eErrorDeviceLost,
                   "Device lost during waiting for a frame");
    }

    auto& scheduler = flip_scheduler;
    scheduler.EndRendering();
    const auto cmdbuf = scheduler.CommandBuffer();

    const auto frame_subresources = vk::ImageSubresourceRange{
        .aspectMask = vk::ImageAspectFlagBits::eColor,
        .baseMipLevel = 0,
        .levelCount = 1,
        .baseArrayLayer = 0,
        .layerCount = VK_REMAINING_ARRAY_LAYERS,
    };

    const auto pre_barrier =
        vk::ImageMemoryBarrier2{.srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                                .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentRead,
                                .dstStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                                .dstAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
                                .oldLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
                                .newLayout = vk::ImageLayout::eGeneral,
                                .image = frame->image,
                                .subresourceRange{frame_subresources}};

    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &pre_barrier,
    });

    // Flush frame creation commands.
    frame->ready_semaphore = scheduler.GetWorkSemaphore()->Handle();
    frame->ready_tick = scheduler.CurrentTick();
    SubmitInfo info{};
    scheduler.Flush(info);
    return frame;
}

static vk::Format GetFrameViewFormat(const Libraries::VideoOut::PixelFormat format) {
    switch (format) {
    case Libraries::VideoOut::PixelFormat::A8B8G8R8Srgb:
        return vk::Format::eR8G8B8A8Srgb;
    case Libraries::VideoOut::PixelFormat::A8R8G8B8Srgb:
        return vk::Format::eB8G8R8A8Srgb;
    case Libraries::VideoOut::PixelFormat::A2R10G10B10:
    case Libraries::VideoOut::PixelFormat::A2R10G10B10Srgb:
    case Libraries::VideoOut::PixelFormat::A2R10G10B10Bt2020Pq:
        return vk::Format::eA2R10G10B10UnormPack32;
    default:
        break;
    }
    UNREACHABLE_MSG("Unknown format={}", static_cast<u32>(format));
    return {};
}

// MediEvil: F12 screenshot of the game's picture as a PNG (GOW3_SCREENSHOT_DIR), encoded on a
// thread of its own once the GPU is done (a 4K PNG takes a moment).
static void SaveScreenshot(const Instance& instance, Scheduler& scheduler, vk::CommandBuffer cmdbuf,
                           vk::Image image, vk::ImageLayout layout, u32 w, u32 h, vk::Format format) {
    const bool bgra = format == vk::Format::eB8G8R8A8Unorm || format == vk::Format::eB8G8R8A8Srgb;
    const bool rgba = format == vk::Format::eR8G8B8A8Unorm || format == vk::Format::eR8G8B8A8Srgb;
    const bool a2r10 = format == vk::Format::eA2R10G10B10UnormPack32;
    const bool a2b10 = format == vk::Format::eA2B10G10R10UnormPack32;
    if (!bgra && !rgba && !a2r10 && !a2b10) {
        std::printf("Screenshot: picture format %s is not supported\n", vk::to_string(format).c_str());
        return;
    }
    const VkBufferCreateInfo buffer_ci{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                       .size = VkDeviceSize(w) * h * 4,
                                       .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    const VmaAllocationCreateInfo alloc_ci{
        .flags = VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT,
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST};
    VkBuffer buffer{};
    VmaAllocation allocation{};
    VmaAllocationInfo info{};
    if (vmaCreateBuffer(instance.GetAllocator(), &buffer_ci, &alloc_ci, &buffer, &allocation,
                        &info) != VK_SUCCESS) {
        return;
    }
    cmdbuf.copyImageToBuffer(image, layout, buffer,
                             vk::BufferImageCopy{.imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
                                                 .imageExtent = {w, h, 1}});
    const char* env = std::getenv("GOW3_SCREENSHOT_DIR");
    const std::filesystem::path dir = env && *env ? env : "screenshots";
    const std::time_t now = std::time(nullptr);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    char stamp[64];
    std::strftime(stamp, sizeof(stamp), "MediEvil_%Y%m%d_%H%M%S", &local);
    scheduler.DeferPriorityOperation([allocator = instance.GetAllocator(), buffer, allocation, info, w, h,
                                      bgra, a2r10, a2b10, dir, name = std::string{stamp}] {
        vmaInvalidateAllocation(allocator, allocation, 0, VK_WHOLE_SIZE);
        std::vector<u8> rgb(size_t(w) * h * 3);
        const auto* src = static_cast<const u32*>(info.pMappedData);
        for (size_t i = 0, n = size_t(w) * h; i < n; ++i) {
            const u32 p = src[i];
            u8 r, g, b;
            if (a2r10 || a2b10) {
                const u8 hi = u8(((p >> 20) & 0x3ff) >> 2), mid = u8(((p >> 10) & 0x3ff) >> 2),
                         lo = u8((p & 0x3ff) >> 2);
                r = a2r10 ? hi : lo;
                g = mid;
                b = a2r10 ? lo : hi;
            } else {
                const u8 c0 = u8(p), c1 = u8(p >> 8), c2 = u8(p >> 16);
                r = bgra ? c2 : c0;
                g = c1;
                b = bgra ? c0 : c2;
            }
            rgb[i * 3] = r;
            rgb[i * 3 + 1] = g;
            rgb[i * 3 + 2] = b;
        }
        vmaDestroyBuffer(allocator, buffer, allocation);
        std::thread([rgb = std::move(rgb), w, h, dir, name] {
            size_t size = 0;
            void* png = tdefl_write_image_to_png_file_in_memory_ex(rgb.data(), int(w), int(h), 3, &size,
                                                                   MZ_DEFAULT_LEVEL, MZ_FALSE);
            if (!png) {
                return;
            }
            std::error_code ec;
            std::filesystem::create_directories(dir, ec);
            std::filesystem::path path = dir / (name + ".png");
            for (int n = 2; std::filesystem::exists(path); ++n) {
                path = dir / (name + "_" + std::to_string(n) + ".png");
            }
#ifdef _WIN32
            std::FILE* f = _wfopen(path.wstring().c_str(), L"wb");
#else
            std::FILE* f = std::fopen(path.c_str(), "wb");
#endif
            if (f) {
                std::fwrite(png, 1, size, f);
                std::fclose(f);
                std::printf("Screenshot: %s (%ux%u)\n", path.string().c_str(), w, h);
                std::fflush(stdout);
            }
            mz_free(png);
        }).detach();
    });
}

// gow3: raw copy of an image for GOW3_FRAME_DUMP_TRIGGER, written once the GPU is done.
static void DumpRaw(const Instance& instance, Scheduler& scheduler, vk::CommandBuffer cmdbuf,
                    vk::Image image, vk::ImageLayout layout, u32 w, u32 h, const char* name,
                    int index) {
    const VkBufferCreateInfo buffer_ci{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                       .size = VkDeviceSize(w) * h * 4,
                                       .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    const VmaAllocationCreateInfo alloc_ci{
        .flags = VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT,
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST};
    VkBuffer buffer{};
    VmaAllocation allocation{};
    VmaAllocationInfo info{};
    if (vmaCreateBuffer(instance.GetAllocator(), &buffer_ci, &alloc_ci, &buffer, &allocation,
                        &info) != VK_SUCCESS) {
        return;
    }
    cmdbuf.copyImageToBuffer(image, layout, buffer,
                             vk::BufferImageCopy{.imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
                                                 .imageExtent = {w, h, 1}});
    const char* dir = std::getenv("GOW3_DUMP_DIR");
    char path[512];
    std::snprintf(path, sizeof(path), "%s/%s_%03d_%ux%u.raw", dir && *dir ? dir : "out/dump", name,
                  index, w, h);
    scheduler.DeferPriorityOperation([allocator = instance.GetAllocator(), buffer, allocation, info,
                                      size = buffer_ci.size, file = std::string{path}] {
        vmaInvalidateAllocation(allocator, allocation, 0, VK_WHOLE_SIZE);
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path{file}.parent_path(), ec);
        if (FILE* f = std::fopen(file.c_str(), "wb")) {
            std::fwrite(info.pMappedData, 1, size, f);
            std::fclose(f);
        }
        vmaDestroyBuffer(allocator, buffer, allocation);
    });
}

Frame* Presenter::PrepareFrame(const Libraries::VideoOut::BufferAttributeGroup& attribute,
                               VAddr cpu_address) {
    // gow3: GOW3_FRAME_DUMP_TRIGGER=<file>: when the file exists it is removed and this frame's
    // guest display buffer and presented image are written to GOW3_DUMP_DIR (default out/dump)
    // as raw 32-bit pixels (menus and movies included; the upscaler dumps scene frames only).
    static const char* frame_dump_trigger = std::getenv("GOW3_FRAME_DUMP_TRIGGER");
    static int frame_dump_index = 0;
    // gow3: GOW3_FRAME_DUMP_COUNT=N dumps N consecutive frames per trigger (flicker reports).
    static const int frame_dump_count = [] {
        const char* env = std::getenv("GOW3_FRAME_DUMP_COUNT");
        return env ? std::clamp(std::atoi(env), 1, 64) : 1;
    }();
    static int frame_dump_remaining = 0;
    // The trigger file is looked for 4 times a second (a remove() per frame cost 1% of the
    // draw recording thread).
    // Shift+F12 (the overlay's key handler) asks through an atomic; the file is for outside tools,
    // dev build only (release: no file poll on this thread).
    if (frame_dump_remaining == 0 && Gow3Overlay::TakeFrameDumpRequest()) {
        frame_dump_remaining = frame_dump_count;
    }
    static auto next_trigger_check = std::chrono::steady_clock::now();
    const bool check_trigger = GOW3_DEV_TOOLS && frame_dump_trigger && frame_dump_remaining == 0 &&
                               std::chrono::steady_clock::now() >= next_trigger_check;
    if (check_trigger) {
        next_trigger_check = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
    }
    if (check_trigger && std::remove(frame_dump_trigger) == 0) {
        frame_dump_remaining = frame_dump_count;
    }
    const bool frame_dump = frame_dump_remaining > 0;
    if (frame_dump) {
        --frame_dump_remaining;
    }
    // gow3: scaled upscaler presets: the output-size display buffer drawn by the port.
    TemporalUpscaler::Display display{};
    const bool upscaled = rasterizer->GetUpscaler().DisplayOverride(cpu_address, display);
    VideoCore::ImageId image_id{};
    if (!upscaled) {
        auto desc = VideoCore::TextureCache::ImageDesc{attribute, cpu_address};
        image_id = texture_cache.FindImage(desc);
        texture_cache.UpdateImage(image_id);
    }

    Frame* frame = GetRenderFrame();

    const auto frame_subresources = vk::ImageSubresourceRange{
        .aspectMask = vk::ImageAspectFlagBits::eColor,
        .baseMipLevel = 0,
        .levelCount = 1,
        .baseArrayLayer = 0,
        .layerCount = VK_REMAINING_ARRAY_LAYERS,
    };

    const auto pre_barrier = vk::ImageMemoryBarrier2{
        .srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentRead,
        .dstStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .dstAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
        .oldLayout = vk::ImageLayout::eUndefined,
        .newLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .image = frame->image,
        .subresourceRange{frame_subresources},
    };

    draw_scheduler.EndRendering();
    const auto cmdbuf = draw_scheduler.CommandBuffer();
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &pre_barrier,
    });

    VideoCore::ImageViewInfo view_info{};
    view_info.format = GetFrameViewFormat(attribute.attrib.pixel_format);
    // Exclude alpha from output frame to avoid blending with UI.
    view_info.mapping.a = vk::ComponentSwizzle::eOne;

    vk::ImageView image_view{};
    vk::Extent2D image_size{};
    if (upscaled) {
        const auto device = instance.GetDevice();
        image_view = Check(device.createImageView({
            .image = display.image,
            .viewType = vk::ImageViewType::e2D,
            .format = view_info.format,
            .components = {vk::ComponentSwizzle::eIdentity, vk::ComponentSwizzle::eIdentity,
                           vk::ComponentSwizzle::eIdentity, vk::ComponentSwizzle::eOne},
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        }));
        draw_scheduler.DeferOperation([device, image_view] { device.destroyImageView(image_view); });
        image_size = vk::Extent2D{display.width, display.height};
        const vk::ImageMemoryBarrier2 to_read{
            .srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
            .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .dstAccessMask = vk::AccessFlagBits2::eShaderRead,
            .oldLayout = vk::ImageLayout::eGeneral,
            .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
            .image = display.image,
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        };
        cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &to_read});
        if (Gow3Overlay::TakeScreenshotRequest()) {
            auto to_copy = to_read;
            to_copy.srcStageMask = vk::PipelineStageFlagBits2::eAllCommands;
            to_copy.srcAccessMask = vk::AccessFlagBits2::eShaderRead;
            to_copy.dstStageMask = vk::PipelineStageFlagBits2::eTransfer;
            to_copy.dstAccessMask = vk::AccessFlagBits2::eTransferRead;
            to_copy.oldLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
            to_copy.newLayout = vk::ImageLayout::eTransferSrcOptimal;
            cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &to_copy});
            SaveScreenshot(instance, draw_scheduler, cmdbuf, display.image,
                           vk::ImageLayout::eTransferSrcOptimal, display.width, display.height,
                           display.format);
            auto back = to_copy;
            back.srcStageMask = vk::PipelineStageFlagBits2::eTransfer;
            back.srcAccessMask = vk::AccessFlagBits2::eTransferRead;
            back.dstStageMask = vk::PipelineStageFlagBits2::eAllCommands;
            back.dstAccessMask = vk::AccessFlagBits2::eShaderRead;
            back.oldLayout = vk::ImageLayout::eTransferSrcOptimal;
            back.newLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
            cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &back});
        }
    } else {
        auto& image = texture_cache.GetImage(image_id);
        if (Gow3Overlay::TakeScreenshotRequest()) {
            runtime.Transit(&image, vk::ImageLayout::eTransferSrcOptimal,
                            vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferRead);
            runtime.FlushBarriers();
            SaveScreenshot(instance, draw_scheduler, cmdbuf, image.GetImage(),
                           vk::ImageLayout::eTransferSrcOptimal, image.info.size.width,
                           image.info.size.height, image.info.pixel_format);
        }
        if (frame_dump) {
            runtime.Transit(&image, vk::ImageLayout::eTransferSrcOptimal,
                            vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferRead);
            runtime.FlushBarriers();
            DumpRaw(instance, draw_scheduler, cmdbuf, image.GetImage(),
                    vk::ImageLayout::eTransferSrcOptimal, image.info.size.width,
                    image.info.size.height, "display", frame_dump_index);
        }
        // gow3 U2: the UE4 motion vector debug view (GOW3_UE4_MOTION_VIEW) reads the display
        // buffer in a compute pass and is presented in its place.
        vk::ImageView motion_view{};
        if (auto* motion = rasterizer->GetUe4Motion()) {
            runtime.Transit(&image, vk::ImageLayout::eShaderReadOnlyOptimal,
                            vk::PipelineStageFlagBits2::eFragmentShader |
                                vk::PipelineStageFlagBits2::eComputeShader,
                            vk::AccessFlagBits2::eShaderRead);
            runtime.FlushBarriers();
            motion_view = motion->Compose(cmdbuf, image, view_info.format);
        }
        image_view = motion_view ? motion_view : *image.FindView(view_info).image_view;
        image_size = vk::Extent2D{image.info.size.width, image.info.size.height};
        runtime.Transit(&image, vk::ImageLayout::eShaderReadOnlyOptimal,
                        vk::PipelineStageFlagBits2::eFragmentShader,
                        vk::AccessFlagBits2::eShaderRead);
        runtime.FlushBarriers();
    }
    expected_ratio = static_cast<float>(image_size.width) / static_cast<float>(image_size.height);

    // gow3: FSR 1, RCAS and its strength come from the in-game menu, every frame.
    const auto& graphics = Gow3Settings::Get();
    fsr_settings.enable = graphics.fsr1;
    // gow3 F2: no RCAS while FSR runs (FSR's own sharpening, fsr_sharpness); over DLSS and UE4's TAA
    // this RCAS (the user, 2026-11-12: the slider sharpens DLSS too; it was off over DLAA before).
    fsr_settings.use_rcas = graphics.rcas && !Ue4Upscaler::SharpensItself();
    fsr_settings.rcas_attenuation = Gow3Graphics::RcasAttenuation(graphics.rcas_strength);
    Gow3Settings::Get().image_width = int(image_size.width);
    Gow3Settings::Get().image_height = int(image_size.height);
    Gow3Settings::Get().window_width = int(frame->width);
    Gow3Settings::Get().window_height = int(frame->height);
    Gow3Settings::Get().rcas_applied =
        fsr_settings.use_rcas &&
        Gow3Graphics::FsrPassRuns(fsr_settings.enable, true, int(image_size.width), int(image_size.height),
                                  int(frame->width), int(frame->height));
    image_view = fsr_pass.Render(cmdbuf, image_view, image_size, {frame->width, frame->height},
                                 fsr_settings, frame->is_hdr);

    // Vulkan has no sRGB variant of the 10-bit format, so an A2R10G10B10Srgb buffer reaches
    // the post process pass still sRGB encoded and has to be decoded there instead.
    pp_settings.srgb_input =
        attribute.attrib.pixel_format == Libraries::VideoOut::PixelFormat::A2R10G10B10Srgb;
    // gow3 HDR near-black correction (opt-in, a meter measurement of this display).
    pp_settings.black_count = 0;
    if (frame->is_hdr && Gow3Settings::Get().hdr_black_correction.load(std::memory_order_relaxed)) [[unlikely]] {
        RefreshBlackCorrection();
        if (black_correction.valid) {
            pp_settings.black_count = u32(black_correction.lut.size());
            pp_settings.black_limit = black_correction.limit;
            std::copy(black_correction.lut.begin(), black_correction.lut.end(), pp_settings.black_lut);
        }
    }
    pp_pass.Render(cmdbuf, image_view, image_size, *frame, pp_settings);
    if (frame_dump) {
        // The presented image as the swapchain blit reads it (General after the pass).
        const vk::MemoryBarrier2 done{.srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                                      .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
                                      .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
                                      .dstAccessMask = vk::AccessFlagBits2::eTransferRead};
        cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &done});
        DumpRaw(instance, draw_scheduler, cmdbuf, frame->image, vk::ImageLayout::eGeneral,
                frame->width, frame->height, "output", frame_dump_index);
        ++frame_dump_index;
    }

    // Flush frame creation commands.
    frame->ready_semaphore = draw_scheduler.GetWorkSemaphore()->Handle();
    frame->ready_tick = draw_scheduler.CurrentTick();
    SubmitInfo info{};
    draw_scheduler.Flush(info);

    // gow3: the GPU command thread runs at most GOW3_FRAMES_AHEAD (default 1) guest frames
    // ahead of the GPU: it waits here for the frame that many flips back. When the GPU is the
    // bottleneck it finishes frames at an even rate; without this bound the command thread ran
    // ahead and then blocked wherever a resource ran out, so flips (and the guest's frame
    // timing) came in bursts: 12.5/25 ms alternation at 80 FPS. 0 turns it off.
    // The in-game menu changes it live (Gow3Settings::frames_queued; GOW3_FRAMES_AHEAD at start).
    const u32 frames_ahead = u32(std::max(0, Gow3Settings::Get().frames_queued.load()));
    if (!frames_ahead) {
        recent_frame_ticks.clear();
    } else {
        recent_frame_ticks.push_back(frame->ready_tick);
        while (recent_frame_ticks.size() > frames_ahead) {
            const u64 tick = recent_frame_ticks.front();
            recent_frame_ticks.pop_front();
            if (recent_frame_ticks.size() == frames_ahead) {
                draw_scheduler.Wait(tick);
            }
        }
    }
    return frame;
}

Frame* Presenter::PrepareBlankFrame(bool present_thread) {
    // Request a free presentation frame.
    Frame* frame = GetRenderFrame();

    auto& scheduler = present_thread ? present_scheduler : draw_scheduler;
    scheduler.EndRendering();

    const auto cmdbuf = scheduler.CommandBuffer();

    constexpr vk::ImageSubresourceRange simple_subresource = {
        .aspectMask = vk::ImageAspectFlagBits::eColor,
        .levelCount = 1,
        .layerCount = 1,
    };
    const auto pre_barrier = vk::ImageMemoryBarrier2{
        .srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentRead,
        .dstStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .dstAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
        .oldLayout = vk::ImageLayout::eUndefined,
        .newLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .image = frame->image,
        .subresourceRange = simple_subresource,
    };

    const auto post_barrier = vk::ImageMemoryBarrier2{
        .srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eFragmentShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderRead,
        .oldLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .newLayout = vk::ImageLayout::eGeneral,
        .image = frame->image,
        .subresourceRange = simple_subresource,
    };

    const vk::RenderingAttachmentInfo attachment = {
        .imageView = frame->image_view,
        .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .loadOp = vk::AttachmentLoadOp::eClear,
        .storeOp = vk::AttachmentStoreOp::eStore,
    };
    const vk::RenderingInfo rendering_info = {
        .renderArea =
            {
                .extent = {frame->width, frame->height},
            },
        .layerCount = 1,
        .colorAttachmentCount = 1u,
        .pColorAttachments = &attachment,
    };

    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &pre_barrier,
    });

    cmdbuf.beginRendering(rendering_info);
    cmdbuf.endRendering();

    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &post_barrier,
    });

    // Flush frame creation commands.
    frame->ready_semaphore = scheduler.GetWorkSemaphore()->Handle();
    frame->ready_tick = scheduler.CurrentTick();
    SubmitInfo info{};
    scheduler.Flush(info);
    return frame;
}

void Presenter::PresentLoadingFrame() {
    if (window.GetWidth() == 0 || window.GetHeight() == 0) {
        return;
    }
    swapchain.UpdateVsync();
    if (window.GetWidth() != swapchain.GetWidth() || window.GetHeight() != swapchain.GetHeight()) {
        swapchain.Recreate(window.GetWidth(), window.GetHeight());
    }
    if (!swapchain.AcquireNextImage()) {
        return;
    }
    const vk::Image image = swapchain.Image();
    const vk::Extent2D extent = swapchain.GetExtent();
    const auto cmdbuf = present_scheduler.CommandBuffer();
    const vk::ImageSubresourceRange range{
        .aspectMask = vk::ImageAspectFlagBits::eColor,
        .levelCount = 1,
        .layerCount = VK_REMAINING_ARRAY_LAYERS,
    };
    const auto barrier = [&](vk::ImageLayout from, vk::ImageLayout to, vk::AccessFlags src,
                             vk::AccessFlags dst) {
        cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
                               vk::PipelineStageFlagBits::eAllCommands, {}, {}, {},
                               vk::ImageMemoryBarrier{
                                   .srcAccessMask = src,
                                   .dstAccessMask = dst,
                                   .oldLayout = from,
                                   .newLayout = to,
                                   .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                   .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                   .image = image,
                                   .subresourceRange = range,
                               });
    };
    barrier(vk::ImageLayout::eUndefined, vk::ImageLayout::eTransferDstOptimal, {},
            vk::AccessFlagBits::eTransferWrite);
    cmdbuf.clearColorImage(image, vk::ImageLayout::eTransferDstOptimal,
                           vk::ClearColorValue{std::array<float, 4>{0.0f, 0.0f, 0.0f, 1.0f}},
                           range);
    barrier(vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::eColorAttachmentOptimal,
            vk::AccessFlagBits::eTransferWrite, vk::AccessFlagBits::eColorAttachmentWrite);
    Gow3Overlay::Render(cmdbuf, swapchain.ImageView(), extent, swapchain.GetHDR());
    barrier(vk::ImageLayout::eColorAttachmentOptimal, vk::ImageLayout::ePresentSrcKHR,
            vk::AccessFlagBits::eColorAttachmentWrite, {});
    SubmitInfo info{};
    info.AddWait(swapchain.GetImageAcquiredSemaphore());
    info.AddSignal(swapchain.GetPresentReadySemaphore());
    present_scheduler.Flush(info);
    {
        std::scoped_lock submit_lock{Scheduler::submit_mutex};
        swapchain.Present();
    }
    // A few frames a second during start-up: waiting keeps semaphores and the swapchain simple.
    present_scheduler.Finish();
}

void Presenter::Present(Frame* frame, bool is_reusing_frame, bool is_game_frame) {
    // Free the frame for reuse
    const auto free_frame = [&] {
        if (!is_reusing_frame) {
            last_submit_frame = frame;
            std::scoped_lock fl{free_mutex};
            free_queue.push(frame);
            free_cv.notify_one();
        }
    };

    // gow3: a minimized window has no pixels on Windows (0x0); keep the swapchain and skip
    // presenting until the window is restored.
    if (window.GetWidth() == 0 || window.GetHeight() == 0) {
        free_frame();
        return;
    }

    // Recreate the swapchain if the window was resized.
    swapchain.UpdateVsync();
    if (window.GetWidth() != swapchain.GetWidth() || window.GetHeight() != swapchain.GetHeight()) {
        swapchain.Recreate(window.GetWidth(), window.GetHeight());
    }

    if (!swapchain.AcquireNextImage()) {
        swapchain.Recreate(window.GetWidth(), window.GetHeight());
        if (!swapchain.AcquireNextImage()) {
            // User resizes the window too fast and GPU can't keep up. Skip this frame.
            LOG_WARNING(Render_Vulkan, "Skipping frame!");
            free_frame();
            return;
        }
    }

    // Reset fence for queue submission. Do it here instead of GetRenderFrame() because we may
    // skip frame because of slow swapchain recreation. If a frame skip occurs, we skip signal
    // the frame's present fence and future GetRenderFrame() call will hang waiting for this frame.
    const auto reset_result = instance.GetDevice().resetFences(frame->present_done);
    ASSERT_MSG(reset_result == vk::Result::eSuccess,
               "Unexpected error resetting present done fence: {}", vk::to_string(reset_result));

    // gow3: the game frame is blitted (letterboxed) straight into the swapchain image.
    const vk::Image swapchain_image = swapchain.Image();
    auto& scheduler = present_scheduler;
    const auto cmdbuf = scheduler.CommandBuffer();

    if (EmulatorSettings.IsVkHostMarkersEnabled()) {
        cmdbuf.beginDebugUtilsLabelEXT(vk::DebugUtilsLabelEXT{
            .pLabelName = "Present",
        });
    }

    {
        const vk::Extent2D extent = swapchain.GetExtent();
        SetExpectedGameSize(s32(extent.width), s32(extent.height));
        const vk::ImageSubresourceRange color_range{
            .aspectMask = vk::ImageAspectFlagBits::eColor,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = VK_REMAINING_ARRAY_LAYERS,
        };
        const std::array pre_barriers{
            vk::ImageMemoryBarrier{
                .srcAccessMask = vk::AccessFlagBits::eNone,
                .dstAccessMask = vk::AccessFlagBits::eTransferWrite,
                .oldLayout = vk::ImageLayout::eUndefined,
                .newLayout = vk::ImageLayout::eTransferDstOptimal,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = swapchain_image,
                .subresourceRange = color_range,
            },
            vk::ImageMemoryBarrier{
                .srcAccessMask = vk::AccessFlagBits::eColorAttachmentWrite,
                .dstAccessMask = vk::AccessFlagBits::eTransferRead,
                .oldLayout = vk::ImageLayout::eGeneral,
                .newLayout = vk::ImageLayout::eTransferSrcOptimal,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = frame->image,
                .subresourceRange = color_range,
            },
        };
        cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
                               vk::PipelineStageFlagBits::eTransfer, vk::DependencyFlagBits::eByRegion,
                               {}, {}, pre_barriers);
        const vk::ClearColorValue black{std::array<float, 4>{0.0f, 0.0f, 0.0f, 1.0f}};
        cmdbuf.clearColorImage(swapchain_image, vk::ImageLayout::eTransferDstOptimal, black, color_range);
        const vk::MemoryBarrier clear_done{
            .srcAccessMask = vk::AccessFlagBits::eTransferWrite,
            .dstAccessMask = vk::AccessFlagBits::eTransferWrite,
        };
        cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eTransfer,
                               vk::DependencyFlagBits::eByRegion, clear_done, {}, {});
        vk::ImageBlit blit = MakeImageBlitFit(frame->width, frame->height, extent.width, extent.height);
        if (Gow3Patterns::Active()) [[unlikely]] {
            // gow3 test patterns: a game patch on black: only the patch's part of the frame (its centre, the patch's
            // share of the screen) reaches the cleared swapchain image, so nothing around it lights the screen.
            if (const auto patch = Gow3Patterns::Current(); patch.mode == Gow3Patterns::Game && patch.background) {
                const float side = std::sqrt(std::clamp(patch.area, 0.001f, 1.0f));
                const auto centre = [side](std::array<vk::Offset3D, 2>& o) {
                    for (const int axis : {0, 1}) {
                        auto get = [&](vk::Offset3D& v) -> s32& { return axis == 0 ? v.x : v.y; };
                        const s32 a = get(o[0]), b = get(o[1]);
                        const float mid = 0.5f * float(a + b), half = 0.5f * side * float(b - a);
                        get(o[0]) = s32(std::lround(mid - half));
                        get(o[1]) = s32(std::lround(mid + half));
                    }
                };
                centre(blit.srcOffsets);
                centre(blit.dstOffsets);
            }
        }
        cmdbuf.blitImage(frame->image, vk::ImageLayout::eTransferSrcOptimal, swapchain_image,
                         vk::ImageLayout::eTransferDstOptimal, blit, vk::Filter::eLinear);
        // gow3 test patterns: a raw patch over the frame (the image is still a copy destination after the blit).
        if (Gow3Patterns::Active()) [[unlikely]] {
            // The image's format now: GetSurfaceFormat is the SDR format even while the swapchain is HDR10 (the first
            // HDR meter run packed 8-bit BGRA into the 10-bit image: white / magenta patches).
            const vk::Format format = swapchain.GetHDR() ? HostPasses::PostProcessingPass::HDR_FRAME_FORMAT
                                                         : swapchain.GetSurfaceFormat().format;
            RecordRawPatch(instance, cmdbuf, swapchain_image, extent, format, color_range);
        }
        // gow3: the settings menu / FPS counter over the frame, at display resolution.
        // gow3 HDR: every present, overlay or not (the grading LUT pass and the menu's HDR rows read it).
        Gow3Settings::Get().hdr_active.store(swapchain.GetHDR(), std::memory_order_relaxed);
        const bool overlay = Gow3Overlay::Visible();
        const std::array post_barriers{
            vk::ImageMemoryBarrier{
                .srcAccessMask = vk::AccessFlagBits::eTransferWrite,
                .dstAccessMask = overlay ? vk::AccessFlagBits::eColorAttachmentRead |
                                               vk::AccessFlagBits::eColorAttachmentWrite
                                         : vk::AccessFlagBits::eNone,
                .oldLayout = vk::ImageLayout::eTransferDstOptimal,
                .newLayout = overlay ? vk::ImageLayout::eColorAttachmentOptimal
                                     : vk::ImageLayout::ePresentSrcKHR,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = swapchain_image,
                .subresourceRange = color_range,
            },
            vk::ImageMemoryBarrier{
                .srcAccessMask = vk::AccessFlagBits::eTransferRead,
                .dstAccessMask = vk::AccessFlagBits::eColorAttachmentWrite,
                .oldLayout = vk::ImageLayout::eTransferSrcOptimal,
                .newLayout = vk::ImageLayout::eGeneral,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = frame->image,
                .subresourceRange = color_range,
            },
        };
        cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
                               vk::PipelineStageFlagBits::eAllCommands,
                               vk::DependencyFlagBits::eByRegion, {}, {}, post_barriers);
        Gow3Overlay::LogPresent();
        Gow3Patterns::NotePresented();
        if (overlay) {
            Gow3Overlay::Render(cmdbuf, swapchain.ImageView(), extent, swapchain.GetHDR());
            const vk::ImageMemoryBarrier to_present{
                .srcAccessMask = vk::AccessFlagBits::eColorAttachmentWrite,
                .dstAccessMask = vk::AccessFlagBits::eNone,
                .oldLayout = vk::ImageLayout::eColorAttachmentOptimal,
                .newLayout = vk::ImageLayout::ePresentSrcKHR,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = swapchain_image,
                .subresourceRange = color_range,
            };
            cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eColorAttachmentOutput,
                                   vk::PipelineStageFlagBits::eBottomOfPipe,
                                   vk::DependencyFlagBits::eByRegion, {}, {}, to_present);
        }
    }
    if (EmulatorSettings.IsVkHostMarkersEnabled()) {
        cmdbuf.endDebugUtilsLabelEXT();
    }

    // Flush vulkan commands.

    SubmitInfo info{};
    info.AddWait(swapchain.GetImageAcquiredSemaphore());
    info.AddWait(frame->ready_semaphore, frame->ready_tick);
    info.AddSignal(swapchain.GetPresentReadySemaphore());
    info.AddSignal(frame->present_done);
    scheduler.Flush(info);

    // gow3 GOW3_LIMIT_IN_PRESENT: the frame limit's slot, waited for here, after the variable
    // work above (T-F9: released exactly on time, frames still reached the present 0.35 +- 0.25 ms
    // later; the RTSS limiter waits at this point and held 8.33 ms). The last 0.5 ms are spun
    // holding submit_mutex: taken after the slot, waiting for a submission on another thread put
    // +-0.2 ms back on the frame times (T-F10); the RTSS wait sits inside it.
    const auto deadline = present_deadline;
    present_deadline = {};
    // gow3 GOW3_LIMIT_AFTER_PRESENT=1: present at once, then hold the thread until the slot (the
    // RTSS limiter's order, back edge: T-F11 showed its present *returns* 8.33 ms apart while ours,
    // waiting before the call, returned 8.14-8.53 ms apart: the driver's 0.2-0.5 ms inside it).
    static const bool after_present = [] {
        const char* env = std::getenv("GOW3_LIMIT_AFTER_PRESENT");
        return env && env[0] == '1';
    }();
    const auto wait_until = [](std::chrono::steady_clock::time_point until) {
#ifdef _WIN32
        thread_local HANDLE hires = CreateWaitableTimerExW(nullptr, nullptr, 0x00000002, TIMER_ALL_ACCESS);
        static const auto spin = std::chrono::microseconds([] {
            const char* env = std::getenv("GOW3_PACER_SPIN_US");
            return env ? std::atol(env) : 700L;
        }());
        if (const auto now = std::chrono::steady_clock::now(); hires && now < until - spin) {
            LARGE_INTEGER due;
            due.QuadPart = -std::max<LONGLONG>(
                1, std::chrono::duration_cast<std::chrono::nanoseconds>(until - spin - now).count() / 100);
            if (SetWaitableTimerEx(hires, &due, 0, nullptr, nullptr, nullptr, 0)) {
                WaitForSingleObject(hires, INFINITE);
            }
        }
#endif
        while (std::chrono::steady_clock::now() < until) {
            _mm_pause();
        }
    };
    const bool limited = deadline != std::chrono::steady_clock::time_point{} && !after_present;
    if (limited) {
        const auto lock_at = deadline - std::chrono::microseconds(500);
        wait_until(lock_at);
    }
    // gow3 Reflex sleep before the present (T-F23: sleeping after it made the frame starts exact
    // but left the presents +-0.4 ms off, with flicker; RTSS sleeps in its present hook).
    if (is_game_frame && ReflexSleepBeforePresent()) {
        swapchain.LatencySleep();
    }
    // Present to swapchain.
    {
        std::scoped_lock submit_lock{Scheduler::submit_mutex};
        while (limited && std::chrono::steady_clock::now() < deadline) {
            _mm_pause();
        }
        if (swapchain.PresentTag() >= 0) {
            Gow3FlipTrace::Note("present_start", swapchain.PresentTag()); // flip trace only
        }
        if (!swapchain.Present() && window.GetWidth() != 0 && window.GetHeight() != 0) {
            swapchain.Recreate(window.GetWidth(), window.GetHeight());
        }
    }
    if (after_present && deadline != std::chrono::steady_clock::time_point{}) {
        wait_until(deadline);
    }

    free_frame();
    if (!is_reusing_frame && is_game_frame) {
        DebugState.IncFlipFrameNum();
    }
}

bool Presenter::WaitFrameReady(Frame* frame, std::chrono::steady_clock::time_point until) {
    if (frame == nullptr) {
        return true;
    }
    return WaitReady(frame->ready_semaphore, frame->ready_tick, until);
}

void Presenter::TraceGpuReady(const Frame* frame, s64 flip_arg) {
    if (frame == nullptr || !Gow3FlipTrace::GpuReadyEnabled()) {
        return;
    }
    {
        std::scoped_lock lock{gpu_ready_mutex};
        if (!gpu_ready_thread.joinable()) {
            gpu_ready_thread = std::jthread([this](std::stop_token token) { GpuReadyThread(token); });
        }
        gpu_ready_queue.push_back({frame->ready_semaphore, frame->ready_tick, flip_arg});
    }
    gpu_ready_cv.notify_one();
}

void Presenter::GpuReadyThread(std::stop_token token) {
    Common::SetCurrentThreadName("gow3:GpuReady");
    while (!token.stop_requested()) {
        GpuReadyEntry next{};
        {
            std::unique_lock lock{gpu_ready_mutex};
            if (!gpu_ready_cv.wait(lock, token, [&] { return !gpu_ready_queue.empty(); })) {
                return;
            }
            next = gpu_ready_queue.front();
            gpu_ready_queue.pop_front();
        }
        // Frames finish in queue order; a frame not finished in a second is left out.
        const auto give_up = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        while (!token.stop_requested() && std::chrono::steady_clock::now() < give_up) {
            if (WaitReady(next.semaphore, next.tick,
                          std::chrono::steady_clock::now() + std::chrono::milliseconds(20))) {
                Gow3FlipTrace::Note("gpu_ready", next.flip_arg);
                break;
            }
        }
    }
}

bool Presenter::WaitReady(vk::Semaphore handle_in, u64 tick,
                          std::chrono::steady_clock::time_point until) {
    Semaphore* semaphore = nullptr;
    for (Scheduler* scheduler : {&draw_scheduler, &flip_scheduler, &present_scheduler}) {
        if (scheduler->GetWorkSemaphore()->Handle() == handle_in) {
            semaphore = scheduler->GetWorkSemaphore();
        }
    }
    if (semaphore == nullptr || semaphore->IsFree(tick)) {
        return true;
    }
    semaphore->Refresh();
    if (semaphore->IsFree(tick)) {
        return true;
    }
    const auto now = std::chrono::steady_clock::now();
    if (until <= now) {
        return false;
    }
    const vk::Semaphore handle = semaphore->Handle();
    const u64 value = tick;
    const vk::SemaphoreWaitInfo wait_info = {
        .semaphoreCount = 1,
        .pSemaphores = &handle,
        .pValues = &value,
    };
    const u64 timeout = std::chrono::duration_cast<std::chrono::nanoseconds>(until - now).count();
    if (instance.GetDevice().waitSemaphores(&wait_info, timeout) != vk::Result::eSuccess) {
        return false;
    }
    semaphore->Refresh();
    return true;
}

void Presenter::DiscardFrame(Frame* frame) {
    // Its present_done fence is still signaled (only Present resets it) and the next use of the
    // frame is ordered after this one's copy on the same queue.
    std::scoped_lock fl{free_mutex};
    free_queue.push(frame);
    free_cv.notify_one();
}

Frame* Presenter::GetRenderFrame() {
    // gow3 #4 (gow3_stage_b_wait.h): on stage B with GOW3_STAGE_B_SPIN=1 both waits below spin
    // first (the fence is asked every 20 us: an ioctl on RADV); their sleeps are timed.
    Gow3StageBWait::Spin([this] {
        std::scoped_lock lock{free_mutex};
        return !free_queue.empty();
    }, 2'000);
    // Wait for free presentation frames
    Frame* frame;
    {
        std::unique_lock lock{free_mutex};
        if (free_queue.empty()) {
            Gow3StageBWait::SleepTimer sleep{Gow3StageBWait::Present};
            free_cv.wait(lock, [this] { return !free_queue.empty(); });
        }
        LOG_DEBUG(Render_Vulkan, "Got render frame, remaining {}", free_queue.size() - 1);

        // Take the frame from the queue
        frame = free_queue.front();
        free_queue.pop();
    }

    const vk::Device device = instance.GetDevice();
    vk::Result result{};

    const auto wait = [&]() {
        result = device.waitForFences(frame->present_done, false, std::numeric_limits<u64>::max());
        return result;
    };

    Gow3StageBWait::Spin(
        [&] { return device.getFenceStatus(frame->present_done) == vk::Result::eSuccess; }, 20'000);
    // Wait for the presentation to be finished so all frame resources are free
    {
        Gow3StageBWait::SleepTimer sleep{Gow3StageBWait::Present};
        while (wait() != vk::Result::eSuccess) {
            ASSERT_MSG(result != vk::Result::eErrorDeviceLost,
                       "Device lost during waiting for a frame");
            // Retry if the waiting times out
            if (result == vk::Result::eTimeout) {
                continue;
            }
        }
    }

    if (frame->width != expected_frame_width || frame->height != expected_frame_height ||
        frame->is_hdr != swapchain.GetHDR()) {
        RecreateFrame(frame, expected_frame_width, expected_frame_height);
    }

    return frame;
}

void Presenter::SetExpectedGameSize(s32 width, s32 height) {
    if (width <= 0 || height <= 0) {
        return; // no surface (minimized window): keep the last frame size
    }
    const float ratio = (float)width / (float)height;

    expected_frame_height = height;
    expected_frame_width = width;
    if (ratio > expected_ratio) {
        expected_frame_width = static_cast<s32>(height * expected_ratio);
    } else {
        expected_frame_height = static_cast<s32>(width / expected_ratio);
    }
}

} // namespace Vulkan
