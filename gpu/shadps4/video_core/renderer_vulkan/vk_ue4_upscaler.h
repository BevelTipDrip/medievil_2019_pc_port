// SPDX-License-Identifier: GPL-2.0-or-later
// gow3 U3 / U4 (plan-ue4-motion-vectors.md, "U3 design", "U4 design") and F1 (plan-fsr.md): an
// upscaler in place of UE4's TAA. At render scale 100 (U3), at the first of the two native TAA draws,
// native anti-aliasing (DLAA); below it (U4) at the TAAU compute dispatch, upscaling (DLSS's quality
// from the output / render ratio). Linear HDR with auto exposure, reversed depth, on TAA's colour
// input, the scene depth and U2's motion vectors with the game's jitter; the result goes into TAA's
// output and UE4's TAA is skipped. Which upscaler: the ue4_upscaler setting (taa, dlss, fsr; menu,
// F9, ini). The front end here finds the inputs and owns the output; a backend (NVIDIA DLSS through
// gow3_dlss.dll, Dlss::Get) only creates its context and records the evaluation.

#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
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

/// The ue4_upscaler setting (Gow3Settings; its ini names: Gow3Settings::Ue4UpscalerKeys).
enum Ue4UpscalerKind : int { Ue4Taa = 0, Ue4DlssKind = 1, Ue4FsrKind = 2, Ue4UpscalerKinds = 3 };

/// One image handed to a backend: the rectangle from (0, 0) it reads or writes.
struct Ue4UpscaleImage {
    vk::Image image;
    vk::ImageView view;
    vk::Format format;
    vk::ImageAspectFlags aspect;
    u32 width, height;
    vk::ImageUsageFlags usage; ///< the image's creation usage (FSR describes it)
};

/// A backend's context: input and output rectangles (equal at render scale 100).
struct Ue4UpscaleDesc {
    u32 input_width, input_height, output_width, output_height;
    bool operator==(const Ue4UpscaleDesc&) const = default;
};

/// One frame: RGBA16F colour, D32 reversed infinite depth, RGBA16F motion (render pixels, previous
/// minus current), an RGBA16F storage output; jitter in render pixels (the sign F8 chose).
struct Ue4UpscaleFrame {
    Ue4UpscaleImage color, depth, motion, output;
    float jitter_x, jitter_y;
    bool reset;
    float frame_ms;
    float near_plane; ///< UE units (cm); the far plane is infinite
    float fov_y;      ///< vertical field of view, radians
};

class Ue4UpscalerBackend;
/// AMD FSR 3.1 (vk_ue4_fsr.cpp), or null when this GPU lacks what it needs.
std::unique_ptr<Ue4UpscalerBackend> MakeFsrBackend(const Instance& instance);

class Ue4UpscalerBackend {
public:
    virtual ~Ue4UpscalerBackend() = default;
    /// For the log and the menu ("DLSS", "FSR").
    [[nodiscard]] virtual const char* Name() const = 0;
    /// This system can run it (DLLs, GPU features).
    [[nodiscard]] virtual bool Available() const = 0;
    /// The context exists for `desc`.
    [[nodiscard]] virtual bool Has(const Ue4UpscaleDesc& desc) const = 0;
    /// Replaces the context; the GPU is done with the old one. Records into `cmdbuf` if it must.
    virtual bool Create(vk::CommandBuffer cmdbuf, const Ue4UpscaleDesc& desc) = 0;
    /// On the recording thread, in order with the frame: the output's GENERAL layout is set.
    virtual bool Evaluate(vk::CommandBuffer cmdbuf, const Ue4UpscaleFrame& frame) = 0;
};

class Ue4Upscaler {
public:
    /// The hooks exist (GOW3_UE4_HOOKS=0 removes them): whether an upscaler runs is the
    /// ue4_upscaler setting and its backend's availability.
    static bool Enabled();
    /// F9: the next available upscaler after the current one (TAA, DLSS, FSR, TAA, ...). The
    /// caller saves the settings.
    static void Toggle();
    /// F8: the next jitter sign handed to the upscaler (+x +y, -x +y, +x -y, -x -y), shown on
    /// screen and logged; the history restarts. NVIDIA's own Ctrl+Alt+F9 setting cannot be read back.
    static void CycleJitterSign();
    /// The on-screen notice of the last F8 / F9 press while it should show (3 s), else null.
    static const char* Notice();
    /// An upscaler replaced TAA within the last 100 ms (the presenter keeps RCAS off then).
    static bool Running();
    /// The upscaler that ran last (while Running()), for the menu: "DLSS", "FSR".
    static const char* RunningName();
    /// Enabled and an upscaler other than TAA chosen: worth looking up the TAAU output image.
    static bool On();

    /// GOW3_UPSCALER_STATS=1 (plan-fsr.md, the lows): the CPU time the upscaler path costs per frame,
    /// logged every 5 s by Run: the rasterizer's hook (every draw while On), its motion pass setup,
    /// the front end and the backend's recording.
    static bool StatsOn();
    /// For the timers: a steady clock in ns, 0 when the stats are off.
    static s64 StatsClock();
    /// Stage B: one call of the rasterizer's hook (ObserveFrameMap) took `ns` (since StatsClock).
    static void NoteHook(s64 start_ns);
    /// Stage B: the motion pass setup (Ue4Motion::OnTaa) inside a hook call.
    static void NoteMotion(s64 start_ns);

    Ue4Upscaler(const Instance& instance, Scheduler& scheduler, VideoCore::TextureCache& texture_cache,
                Runtime& runtime, Ue4Motion& motion);
    ~Ue4Upscaler();

    /// A draw into one RGBA16F colour target (not TAA): the last one before TAA is TAA's colour
    /// input (the separate translucency composite).
    void OnColorTarget(VideoCore::ImageId id, VAddr address);
    /// A native TAA draw with the View buffer bound, sampling `reads`, into `output`. True: the
    /// upscaler covers it (recorded at the first draw of the frame), so the draw must be skipped.
    /// `mark_written` gives `output` a render target's texture cache bookkeeping (the skipped
    /// draw would have): called before the upscaler writes it.
    bool OnTaaDraw(const u8* view, std::span<const VAddr> reads, VideoCore::ImageId output,
                   VAddr output_address, const std::function<void()>& mark_written);
    /// The TAAU compute dispatch (U4), writing `output` (`output_width` x `output_height` from its
    /// T#). True: the upscaler wrote it, so the dispatch must be skipped. `mark_written` gives it
    /// a storage write's texture cache bookkeeping.
    bool OnTaauDispatch(const u8* view, std::span<const VAddr> reads, VideoCore::ImageId output,
                        VAddr output_address, u32 output_width, u32 output_height,
                        const std::function<void()>& mark_written);

private:
    struct Target {
        VideoCore::ImageId id{};
        u64 uid = 0;
        VAddr address = 0;
    };
    bool Fail(const char* why);
    /// Both hooks: `output_width` 0 = native (into the draws' colour target).
    bool Run(const u8* view, std::span<const VAddr> reads, VideoCore::ImageId output_id,
             VAddr output_address, u32 output_width, u32 output_height,
             const std::function<void()>& mark_written);

    const Instance& instance;
    Scheduler& scheduler;
    VideoCore::TextureCache& texture_cache;
    Runtime& runtime;
    Ue4Motion& motion;
    /// By Ue4UpscalerKind (TAA: none); null when not built in.
    std::array<std::unique_ptr<Ue4UpscalerBackend>, Ue4UpscalerKinds> backends;

    Target color;
    bool have_frame = false;
    u32 last_frame = 0;  ///< View frame number of the last frame an upscaler ran
    int last_kind = Ue4Taa; ///< the upscaler of that frame: another one resets
    bool was_on = false; ///< an upscaler ran in the last frame with TAA draws: else the next one resets
    VideoCore::UniqueImage output;
    vk::UniqueImageView output_view;
    u32 own_width = 0, own_height = 0;
    std::chrono::steady_clock::time_point last_time{};
    u64 evaluations = 0, resets = 0;
    static std::atomic<int> jitter_sign;      ///< 0-3, see CycleJitterSign
    static std::atomic<bool> sign_changed;    ///< the next frame resets
    static std::atomic<s64> notice_until_ns;  ///< steady clock
    static std::atomic<const char*> notice;
    static std::atomic<bool> evaluate_failed; ///< set on the recording thread
    static std::atomic<s64> last_run_ns; ///< steady clock
    static std::atomic<const char*> running_name;
};

} // namespace Vulkan
