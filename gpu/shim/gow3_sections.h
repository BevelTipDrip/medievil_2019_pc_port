// SPDX-License-Identifier: GPL-2.0-or-later
// gow3 GOW3_SECTIONS=1 (diagnostics): the time the draw recording thread (stage B) spends per
// part of a packet, inclusive (nested parts count in their parent too), printed per packet with
// the draw pipe statistics. Ported from the Bloodborne port's bbport_sections.h (BB_SECTIONS).
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <x86intrin.h>

namespace Gow3Sections {
enum Id : std::uint32_t {
    Packet,          ///< a whole packet of the draw pipe
    Task,            ///< ordered tasks (fences, end of pipe, signals, DMA, flips)
    EndOfPipe,       ///< TextureCache::ProcessDownloadImages (within tasks)
    DrawRecord,
    BeforeRecord,    ///< D2's CPU-write queue, O3's submission
    PrepareRenderState,
    BindResources,
    BindBuffers,
    BindTextures,
    ObtainBuffer,
    BeginRendering,  ///< Rasterizer::BeginRendering (render state, targets)
    ResolveVertexBuffers,
    ResolveIndexBuffer,
    EmitVertexBuffers,
    FlushBarriers,
    PipelineBind,    ///< Pipeline::BindResources (push descriptors, push constants)
    UpdateDynamicState,
    Kick,            ///< ResetBindings and the hand-over to the Vulkan recorder
    DispatchRecord,
    Count,
};
inline constexpr const char* Names[Count] = {
    "packet",   "tasks",  "end of pipe", "draw",         "before record", "render state",
    "bind",     "buffers", "textures",   "ObtainBuffer", "begin render",  "vertex",
    "index",    "emit vertex", "barriers", "descriptors", "dynamic",      "kick",
    "dispatch",
};
inline std::array<std::atomic<std::uint64_t>, Count> cycles{};

inline bool Enabled() {
    static const bool on = [] {
        const char* env = std::getenv("GOW3_SECTIONS");
        return env && env[0] == '1';
    }();
    return on;
}
/// Set on the draw recording thread (DrawPipe::Run): only its time is counted.
inline thread_local bool recording_thread = false;

struct Scope {
    explicit Scope(Id id_) : id{id_}, start{Enabled() && recording_thread ? __rdtsc() : 0} {}
    ~Scope() {
        if (start) {
            cycles[id].fetch_add(__rdtsc() - start, std::memory_order_relaxed);
        }
    }
    Id id;
    std::uint64_t start;
};
} // namespace Gow3Sections

#define GOW3_SECTION_JOIN2(a, b) a##b
#define GOW3_SECTION_JOIN(a, b) GOW3_SECTION_JOIN2(a, b)
#define GOW3_SECTION(id) Gow3Sections::Scope GOW3_SECTION_JOIN(gow3_section_scope_, __LINE__){Gow3Sections::id}
