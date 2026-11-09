// SPDX-License-Identifier: GPL-2.0-or-later
// gow3 U3 (plan-ue4-motion-vectors.md, "U3 design"): DLAA in place of UE4's TAA at render scale
// 100. At the first of the two native TAA draws, DLSS (DLAA quality, linear HDR with auto
// exposure, reversed depth) runs on TAA's colour input, the scene depth and U2's motion vectors
// with the game's jitter; its result is copied into TAA's colour target and both TAA draws are
// skipped. Under TAAU (the compute TAA) nothing changes. GOW3_UE4_DLSS=1, off by default; F9
// switches between DLAA and UE4's TAA live. Needs gow3_dlss.dll and nvngx_dlss.dll (Dlss::Get).

#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <span>

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/texture_cache/image.h"

namespace VideoCore {
class TextureCache;
}

namespace Vulkan {

class Instance;
class Scheduler;
class Runtime;
class Ue4Motion;

class Ue4Dlss {
public:
    /// GOW3_UE4_DLSS=1 (read once).
    static bool Enabled();
    /// F9: DLAA on / off (UE4's TAA).
    static void Toggle();
    /// DLAA replaced TAA within the last 100 ms (the presenter keeps RCAS off then).
    static bool Running();

    Ue4Dlss(const Instance& instance, Scheduler& scheduler, VideoCore::TextureCache& texture_cache,
            Runtime& runtime, Ue4Motion& motion);
    ~Ue4Dlss();

    /// A draw into one RGBA16F colour target (not TAA): the last one before TAA is TAA's colour
    /// input (the separate translucency composite).
    void OnColorTarget(VideoCore::ImageId id, VAddr address);
    /// A native TAA draw with the View buffer bound, sampling `reads`, into `output`. True: DLSS
    /// covers it (recorded at the first draw of the frame), so the draw must be skipped.
    bool OnTaaDraw(const u8* view, std::span<const VAddr> reads, VideoCore::ImageId output,
                   VAddr output_address);

private:
    struct Target {
        VideoCore::ImageId id{};
        u64 uid = 0;
        VAddr address = 0;
    };
    bool Fail(const char* why);

    const Instance& instance;
    Scheduler& scheduler;
    VideoCore::TextureCache& texture_cache;
    Runtime& runtime;
    Ue4Motion& motion;

    Target color;
    bool have_frame = false;
    u32 last_frame = 0;  ///< View frame number of the last frame DLSS ran
    bool was_on = false; ///< DLSS ran in the last frame with TAA draws: else the next one resets
    VideoCore::UniqueImage output;
    vk::UniqueImageView output_view;
    u32 output_width = 0, output_height = 0;
    std::chrono::steady_clock::time_point last_time{};
    u64 evaluations = 0, resets = 0;
    static std::atomic<bool> on;
    static std::atomic<s64> last_run_ns; ///< steady clock
};

} // namespace Vulkan
