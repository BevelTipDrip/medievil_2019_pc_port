// SPDX-License-Identifier: GPL-2.0-or-later
// gow3 GOW3_REFLEX_MARKERS (Reflex R1, research/patches/present/reflex-markers-r1.md): NVIDIA Reflex
// (VK_NV_low_latency2) is told where each game frame's work is. A frame gets an ID when stage A starts
// decoding it (its first top-level submission after the previous flip). The ID travels with the frame
// (stage B's in-order task, the flip packet, the VideoOut flip request) and becomes its present ID.
// Every queue submission carries the ID of the frame stage B records (VkLatencySubmissionPresentIdNV).
// RENDERSUBMIT_START is set at stage A's frame start, RENDERSUBMIT_END when the frame is queued for
// present, PRESENT_START / END at the present.
// 1: SIMULATION_START / END stay where they were (after the Reflex sleep, at the present), with the
//    frame's ID. 2: both at stage A's frame start, right before RENDERSUBMIT_START.
// Off (default): On() is false after one static load, nothing here runs, present IDs count up at the
// present as before.
#pragma once

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <mutex>

namespace Gow3Reflex {

/// GOW3_REFLEX_MARKERS at start: 0 off (default), 1 on, 2 on with the simulation markers at stage A.
inline int Mode() {
    static const int mode = [] {
        const char* env = std::getenv("GOW3_REFLEX_MARKERS");
        return env && (env[0] == '1' || env[0] == '2') && env[1] == '\0' ? env[0] - '0' : 0;
    }();
    return mode;
}

namespace Detail {
inline std::atomic<bool> active{false};
inline std::atomic<std::uint64_t> submit_frame{0};
inline std::atomic<std::uint64_t> flip_frame{0};
} // namespace Detail

/// Vulkan::Instance, once: VK_NV_low_latency2 is enabled (GOW3_REFLEX=1 on a driver that has it).
inline void SetActive(bool low_latency2) {
    Detail::active.store(Mode() != 0 && low_latency2, std::memory_order_relaxed);
}

/// The switch is on and Reflex is active. Off: the switch is tested first, no atomic is read.
inline bool On() {
    return Mode() != 0 && Detail::active.load(std::memory_order_relaxed);
}

/// GOW3_REFLEX_MARKERS=2: SIMULATION_START / END at stage A's frame start.
inline bool SimulationAtStageA() {
    return Mode() == 2;
}

/// VkLatencyMarkerNV values (vk_swapchain.cpp checks them against the header).
enum Marker : int {
    SimulationStart = 0,
    SimulationEnd = 1,
    RendersubmitStart = 2,
    RendersubmitEnd = 3,
    PresentStart = 4,
    PresentEnd = 5,
};

/// Frame IDs, which are also the present IDs (VK_KHR_present_id) while On(). Present IDs must rise
/// strictly per swapchain, also across presents that are no game frame (menu redraws, loading and
/// blank frames) and around frames that are never shown (dropped). Frames are presented in the
/// order stage A reserved them (one request queue).
/// - Reserve (stage A): the next ID, never 0.
/// - Present (presents are serialised: Scheduler::submit_mutex): the frame's own ID when it is above
///   the last present ID. Else (a present that is no game frame: frame 0; or a frame such a present
///   overtook) the last + 1, and later reservations jump to above last + 1 + Window, so the frames
///   still in flight (fewer than Window) take last + 2, + 3 ... below every later frame's ID.
/// Each result is above the previous one in both branches, whatever was reserved: strictly
/// increasing by construction. A frame keeps its ID unless a non-game present came between its
/// reservation and its present.
class FrameIds {
public:
    static constexpr std::uint64_t Window = 64;

    std::uint64_t Reserve() noexcept {
        return next.fetch_add(1, std::memory_order_relaxed) + 1;
    }
    /// The ID Present(frame) would give now (the presenting thread).
    std::uint64_t Peek(std::uint64_t frame) const noexcept {
        const std::uint64_t prev = last.load(std::memory_order_relaxed);
        return frame > prev ? frame : prev + 1;
    }
    std::uint64_t Present(std::uint64_t frame) noexcept {
        const std::uint64_t id = Peek(frame);
        last.store(id, std::memory_order_relaxed);
        if (id != frame) {
            const std::uint64_t floor = id + Window;
            std::uint64_t cur = next.load(std::memory_order_relaxed);
            while (cur < floor &&
                   !next.compare_exchange_weak(cur, floor, std::memory_order_relaxed)) {
            }
        }
        return id;
    }
    std::uint64_t Last() const noexcept {
        return last.load(std::memory_order_relaxed);
    }

private:
    std::atomic<std::uint64_t> next{0}; ///< the last ID reserved (or the floor a present raised)
    std::atomic<std::uint64_t> last{0}; ///< the last present ID
};

inline FrameIds& Ids() {
    static FrameIds ids;
    return ids;
}

/// Stage B in stream order (stage A without the draw pipeline): the frame recorded from here on.
inline void SetSubmitFrame(std::uint64_t id) {
    Detail::submit_frame.store(id, std::memory_order_relaxed);
}
/// Any submitting thread: the presentID its submission carries (0: no frame yet, none).
inline std::uint64_t SubmitFrame() {
    return Detail::submit_frame.load(std::memory_order_relaxed);
}

/// SignalFlip hands the frame to the guest's flip handler, which runs VideoOutDriver::SubmitFlipInternal
/// in the same call (later on the recording thread with GOW3_FLIP_DEFERRED).
inline void SetFlipFrame(std::uint64_t id) {
    Detail::flip_frame.store(id, std::memory_order_relaxed);
}
inline std::uint64_t TakeFlipFrame() {
    return Detail::flip_frame.exchange(0, std::memory_order_relaxed);
}

/// Markers come from stage A, stage B and the presenting thread; all go through one sink (the
/// swapchain) under one lock, which Swapchain::Create / Destroy also take around the handle.
using MarkerSink = void (*)(void* context, int marker, std::uint64_t id);
struct Sink {
    std::mutex mutex;
    MarkerSink fn{};
    void* context{};
};
inline Sink& GetSink() {
    static Sink sink;
    return sink;
}
inline std::mutex& SinkMutex() {
    return GetSink().mutex;
}
inline void SetSink(MarkerSink fn, void* context) {
    auto& sink = GetSink();
    std::scoped_lock lock{sink.mutex};
    sink.fn = fn;
    sink.context = context;
}
inline void SetMarker(int marker, std::uint64_t id) {
    if (id == 0) {
        return;
    }
    auto& sink = GetSink();
    std::scoped_lock lock{sink.mutex};
    if (sink.fn) {
        sink.fn(sink.context, marker, id);
    }
}

} // namespace Gow3Reflex
