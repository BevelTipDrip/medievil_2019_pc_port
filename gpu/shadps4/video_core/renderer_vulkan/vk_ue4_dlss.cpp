// SPDX-License-Identifier: GPL-2.0-or-later
// gow3 U3: DLAA in place of UE4's TAA (vk_ue4_dlss.h).

#include "video_core/renderer_vulkan/vk_ue4_dlss.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

#include "video_core/renderer_vulkan/vk_dlss.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_ue4_motion.h"
#include "video_core/texture_cache/texture_cache.h"

namespace Vulkan {

namespace {

// View uniform buffer offsets (study-taau-frame-map.md section 3).
constexpr u32 OffsetClipToPrevClip = 1824;
constexpr u32 OffsetJitter = 1888;
constexpr u32 OffsetViewSize = 1952;
constexpr u32 OffsetNearPlane = 2112;
constexpr u32 OffsetFrameNumber = 2140;

template <typename T>
T Load(const u8* data, u32 offset) {
    T value;
    std::memcpy(&value, data + offset, sizeof(T));
    return value;
}

/// The largest one-frame move, in pixels, of five screen points (the centre and four near the
/// corners) at infinity and at 10 m, through View.ClipToPrevClip (row vector times matrix).
/// Logged with each reset only: no reset rule uses it.
float CameraJumpPixels(const u8* view, std::array<float, 2> size) {
    std::array<std::array<float, 4>, 4> m;
    std::memcpy(m.data(), view + OffsetClipToPrevClip, sizeof(m));
    // Reversed Z, infinite far plane: device Z = near / view distance (UE units, cm).
    const float near_plane = Load<float>(view, OffsetNearPlane);
    const std::array<float, 2> depths{0.0f, near_plane > 0.0f ? near_plane / 1000.0f : 0.0f};
    static constexpr std::array<std::array<float, 2>, 5> points{
        {{0.0f, 0.0f}, {-0.8f, -0.8f}, {0.8f, -0.8f}, {-0.8f, 0.8f}, {0.8f, 0.8f}}};
    float worst = 0.0f;
    for (const float z : depths) {
        for (const auto& p : points) {
            std::array<float, 4> prev;
            for (u32 c = 0; c < 4; ++c) {
                prev[c] = p[0] * m[0][c] + p[1] * m[1][c] + z * m[2][c] + m[3][c];
            }
            if (!(std::fabs(prev[3]) > 1e-6f)) {
                return 1e30f;
            }
            const float dx = (p[0] - prev[0] / prev[3]) * size[0] * 0.5f;
            const float dy = (p[1] - prev[1] / prev[3]) * size[1] * 0.5f;
            worst = std::max(worst, std::sqrt(dx * dx + dy * dy));
        }
    }
    return std::isfinite(worst) ? worst : 1e30f;
}

} // namespace

std::atomic<bool> Ue4Dlss::on{true};
std::atomic<s64> Ue4Dlss::last_run_ns{0};

bool Ue4Dlss::Running() {
    const s64 now = std::chrono::steady_clock::now().time_since_epoch().count();
    const s64 last = last_run_ns.load(std::memory_order_relaxed);
    return last != 0 && now - last < 100'000'000;
}

bool Ue4Dlss::Enabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("GOW3_UE4_DLSS");
        return value && value[0] == '1';
    }();
    return enabled;
}

void Ue4Dlss::Toggle() {
    if (!Enabled()) {
        return;
    }
    const bool now = !on.load(std::memory_order_relaxed);
    on.store(now, std::memory_order_relaxed);
    std::printf("UE4 DLSS: %s\n", now ? "DLAA" : "UE4 TAA");
    std::fflush(stdout);
}

Ue4Dlss::Ue4Dlss(const Instance& instance_, Scheduler& scheduler_,
                 VideoCore::TextureCache& texture_cache_, Runtime& runtime_, Ue4Motion& motion_)
    : instance{instance_}, scheduler{scheduler_}, texture_cache{texture_cache_}, runtime{runtime_},
      motion{motion_} {
    const Dlss* dlss = Dlss::Get();
    std::printf("UE4 DLSS: on (F9 switches DLAA / UE4 TAA)%s\n",
                dlss ? "" : "; DLSS DLLs not found: UE4 TAA stays");
}

Ue4Dlss::~Ue4Dlss() = default;

bool Ue4Dlss::Fail(const char* why) {
    static const char* last = nullptr;
    if (why != last) {
        last = why;
        std::printf("UE4 DLSS: UE4 TAA this frame (%s)\n", why);
    }
    was_on = false;
    return false;
}

void Ue4Dlss::OnColorTarget(VideoCore::ImageId id, VAddr address) {
    if (id != color.id || address != color.address) {
        auto* image = texture_cache.TryGetImage(id, 0);
        color = image ? Target{id, image->image_uid, address} : Target{};
    }
}

bool Ue4Dlss::OnTaaDraw(const u8* view, std::span<const VAddr> reads, VideoCore::ImageId output_id,
                        VAddr output_address) {
    if (!view) {
        return Fail("no View buffer");
    }
    const u32 frame_number = Load<u32>(view, OffsetFrameNumber);
    if (!on.load(std::memory_order_relaxed)) {
        was_on = false;
        return false;
    }
    if (was_on && have_frame && frame_number == last_frame) {
        return true; // the second TAA draw: DLSS already wrote the output
    }
    Dlss* dlss = Dlss::Get();
    if (!dlss || !dlss->Available()) {
        return Fail(dlss ? "DLSS not available" : "DLSS DLLs not found");
    }
    Ue4Motion::LastPass pass;
    if (!motion.GetPass(frame_number, pass)) {
        return Fail("no motion vectors");
    }
    auto* color_image = color.id ? texture_cache.TryGetImage(color.id, color.uid) : nullptr;
    if (!color_image || color.address == output_address ||
        std::ranges::find(reads, color.address) == reads.end() ||
        color_image->info.pixel_format != vk::Format::eR16G16B16A16Sfloat) {
        return Fail("TAA's colour input not found");
    }
    auto* output_image = texture_cache.TryGetImage(output_id, 0);
    if (!output_image || output_image->info.pixel_format != vk::Format::eR16G16B16A16Sfloat) {
        return Fail("TAA's output not RGBA16F");
    }
    auto* depth_image = pass.depth;
    // The rendered rectangle, from (0, 0); every input holds it.
    const std::array<float, 2> view_size{Load<float>(view, OffsetViewSize),
                                         Load<float>(view, OffsetViewSize + 4)};
    const u32 width = std::min({u32(view_size[0]), color_image->info.size.width,
                                depth_image->info.size.width, pass.width,
                                output_image->info.size.width});
    const u32 height = std::min({u32(view_size[1]), color_image->info.size.height,
                                 depth_image->info.size.height, pass.height,
                                 output_image->info.size.height});
    if (width < 64 || height < 64) {
        return Fail("view too small");
    }

    // Reset: the first frame after UE4's TAA ran, or a View frame number that is not the next
    // one. The game's camera cuts, skips and level loads all move it by more than one; pans, however
    // fast, never do (run cut-evidence). View.CameraCut stays 0 in this game; camera-motion rules
    // misfired (u3-dlaa: a ClipToPrevClip element above 1.0 reset ~6% of moving frames).
    const float camera_jump = CameraJumpPixels(view, view_size);
    const char* reset_reason = !was_on                          ? "first frame"
                               : frame_number != last_frame + 1 ? "frame number"
                                                                : nullptr;

    const auto device = instance.GetDevice();
    if (!output.image || output_width != width || output_height != height) {
        if (output.image) {
            struct Old {
                VideoCore::UniqueImage image;
                vk::UniqueImageView view;
            };
            auto old = std::make_shared<Old>(Old{std::move(output), std::move(output_view)});
            scheduler.DeferOperation([old] {});
        }
        output = VideoCore::UniqueImage(device, instance.GetAllocator());
        output.Create({
            .imageType = vk::ImageType::e2D,
            .format = vk::Format::eR16G16B16A16Sfloat,
            .extent = {width, height, 1},
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = vk::SampleCountFlagBits::e1,
            .tiling = vk::ImageTiling::eOptimal,
            .usage = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled |
                     vk::ImageUsageFlagBits::eTransferSrc,
        });
        output_view = Check(device.createImageViewUnique({
            .image = vk::Image(output),
            .viewType = vk::ImageViewType::e2D,
            .format = vk::Format::eR16G16B16A16Sfloat,
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        }));
        output_width = width;
        output_height = height;
    }

    scheduler.EndRendering();
    runtime.Transit(color_image, vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits2::eAllCommands,
                    vk::AccessFlagBits2::eShaderRead);
    runtime.Transit(depth_image, vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits2::eAllCommands,
                    vk::AccessFlagBits2::eShaderRead);
    runtime.Transit(output_image, vk::ImageLayout::eTransferDstOptimal,
                    vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferWrite);
    runtime.FlushBarriers();
    const vk::ImageView color_view = Check(device.createImageView({
        .image = color_image->GetImage(),
        .viewType = vk::ImageViewType::e2D,
        .format = vk::Format::eR16G16B16A16Sfloat,
        .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
    }));
    const vk::ImageView depth_view = Check(device.createImageView({
        .image = depth_image->GetImage(),
        .viewType = vk::ImageViewType::e2D,
        .format = depth_image->info.pixel_format,
        .subresourceRange = {vk::ImageAspectFlagBits::eDepth, 0, 1, 0, 1},
    }));
    scheduler.DeferOperation([device, color_view, depth_view] {
        device.destroyImageView(color_view);
        device.destroyImageView(depth_view);
    });

    // NGX records into a command buffer of its own choosing: recorded directly, in order.
    const auto cmdbuf = scheduler.CommandBuffer();
    const vk::Image output_handle = vk::Image(output);
    const std::array<vk::ImageMemoryBarrier2, 2> before{{
        {
            // U2's motion pass wrote the vectors.
            .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .srcAccessMask = vk::AccessFlagBits2::eShaderWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .dstAccessMask = vk::AccessFlagBits2::eShaderRead,
            .oldLayout = vk::ImageLayout::eGeneral,
            .newLayout = vk::ImageLayout::eGeneral,
            .image = pass.image,
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        },
        {
            // Last frame's copy read the output; its contents are not needed.
            .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .srcAccessMask = vk::AccessFlagBits2::eTransferRead,
            .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .dstAccessMask = vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite,
            .oldLayout = vk::ImageLayout::eUndefined,
            .newLayout = vk::ImageLayout::eGeneral,
            .image = output_handle,
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        },
    }};
    cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = u32(before.size()),
                             .pImageMemoryBarriers = before.data()});

    const Dlss::FeatureDesc desc{width, height, width, height, 0 /* DLAA */, true, true};
    bool ok = true;
    if (!dlss->HasFeature(desc)) {
        // The previous feature may still be in use by submitted work; `cmdbuf` stays open.
        scheduler.WaitSubmitted();
        dlss->ReleaseFeature();
        ok = dlss->CreateFeature(cmdbuf, desc);
        reset_reason = "feature created";
        std::printf("UE4 DLSS: DLAA feature %ux%u%s\n", width, height, ok ? "" : " failed");
    }
    const auto now = std::chrono::steady_clock::now();
    float frame_ms = std::chrono::duration<float, std::milli>(now - last_time).count();
    if (frame_ms <= 0.0f || frame_ms > 200.0f) {
        frame_ms = 16.6f;
    }
    last_time = now;
    // UE4's TemporalAAJitter.xy is the projection's NDC offset: pixels = x * W / 2, -y * H / 2
    // (y down). The sign NGX wants is checked with the development DLL (Ctrl+Alt+F9).
    const float jitter_x = Load<float>(view, OffsetJitter) * view_size[0] * 0.5f;
    const float jitter_y = -Load<float>(view, OffsetJitter + 4) * view_size[1] * 0.5f;
    if (ok) {
        ok = dlss->Evaluate(cmdbuf, {
            .color = {color_image->GetImage(), color_view, vk::Format::eR16G16B16A16Sfloat,
                      vk::ImageAspectFlagBits::eColor, width, height},
            .depth = {depth_image->GetImage(), depth_view, depth_image->info.pixel_format,
                      vk::ImageAspectFlagBits::eDepth, width, height},
            .motion = {pass.image, pass.view, vk::Format::eR16G16B16A16Sfloat,
                       vk::ImageAspectFlagBits::eColor, width, height},
            .output = {output_handle, *output_view, vk::Format::eR16G16B16A16Sfloat,
                       vk::ImageAspectFlagBits::eColor, width, height},
            .jitter_x = jitter_x,
            .jitter_y = jitter_y,
            .reset = reset_reason != nullptr,
            .frame_ms = frame_ms,
            .sharpness = 0.0f,
        });
    }
    if (!ok) {
        std::printf("UE4 DLSS: evaluation failed; UE4 TAA from now on\n");
        on.store(false, std::memory_order_relaxed);
        return Fail("DLSS failed");
    }
    // Into TAA's colour target: bloom, the tonemap, the SSR-like pass and (with UE4's TAA back)
    // the next TAA read it there.
    const vk::ImageMemoryBarrier2 to_copy{
        .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .srcAccessMask = vk::AccessFlagBits2::eShaderWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .dstAccessMask = vk::AccessFlagBits2::eTransferRead,
        .oldLayout = vk::ImageLayout::eGeneral,
        .newLayout = vk::ImageLayout::eTransferSrcOptimal,
        .image = output_handle,
        .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
    };
    cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &to_copy});
    const vk::ImageCopy region{
        .srcSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
        .dstSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
        .extent = {width, height, 1},
    };
    cmdbuf.copyImage(output_handle, vk::ImageLayout::eTransferSrcOptimal,
                     output_image->GetImage(), vk::ImageLayout::eTransferDstOptimal, region);

    if (reset_reason) {
        ++resets;
        if (resets <= 50) {
            std::printf("UE4 DLSS: reset (%s, View frame %u, camera moved %.0f px)\n",
                        reset_reason, frame_number, camera_jump);
        }
    }
    if (++evaluations == 1 || evaluations % 3000 == 0) {
        std::printf("UE4 DLSS: %llu DLAA frames, %llu resets, %ux%u, jitter %.3f %.3f px\n",
                    static_cast<unsigned long long>(evaluations),
                    static_cast<unsigned long long>(resets), width, height, jitter_x, jitter_y);
    }
    have_frame = true;
    last_frame = frame_number;
    was_on = true;
    last_run_ns.store(std::chrono::steady_clock::now().time_since_epoch().count(),
                      std::memory_order_relaxed);
    return true;
}

} // namespace Vulkan
