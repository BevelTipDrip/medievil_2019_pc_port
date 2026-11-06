// SPDX-License-Identifier: GPL-2.0-or-later
// gow3: stage A (the GPU command thread, shadPS4:GpuComm) per-packet cuts and their counters
// (research/docs/impl-stage-a-decode.md).
#pragma once

#include <array>
#include <atomic>
#include <cstdint>

namespace Gow3StageA {

/// A2: a filter in front of Rasterizer::PendingWriteOverlaps' scan. The 64 KiB pages of the
/// guest address space that any pending write covers, hashed into 4096 bits. Every byte of every
/// pending write range is on a set bit (Mark on each new or grown range, a rebuild when the list
/// is pruned), so a range whose pages are all clear overlaps no pending write. A range too wide to
/// mark sets `all` (the filter then answers nothing until the next rebuild).
struct PendingPages {
    static constexpr unsigned PageShift = 16;
    static constexpr std::uint64_t MaxMarkPages = 256;  ///< 16 MiB; wider: `all`
    static constexpr std::uint64_t MaxQueryPages = 16;  ///< wider queries are not filtered
    std::array<std::uint64_t, 64> bits{};
    bool all = false;

    static unsigned Slot(std::uint64_t page) noexcept {
        return unsigned((page * 0x9E3779B97F4A7C15ull) >> 52); // 12 bits
    }
    void Clear() noexcept {
        bits.fill(0);
        all = false;
    }
    /// [begin, end): a pending write range (or the part a merge added).
    void Mark(std::uint64_t begin, std::uint64_t end) noexcept {
        if (end <= begin || all) {
            return;
        }
        const std::uint64_t first = begin >> PageShift, last = (end - 1) >> PageShift;
        if (last - first >= MaxMarkPages) {
            all = true;
            return;
        }
        for (std::uint64_t page = first; page <= last; ++page) {
            const unsigned slot = Slot(page);
            bits[slot / 64] |= std::uint64_t{1} << (slot % 64);
        }
    }
    /// True only when no page of [begin, end) is marked (so no marked range overlaps it).
    [[nodiscard]] bool Untouched(std::uint64_t begin, std::uint64_t end) const noexcept {
        if (all || end <= begin) {
            return false;
        }
        const std::uint64_t first = begin >> PageShift, last = (end - 1) >> PageShift;
        if (last - first >= MaxQueryPages) {
            return false;
        }
        for (std::uint64_t page = first; page <= last; ++page) {
            const unsigned slot = Slot(page);
            if ((bits[slot / 64] >> (slot % 64)) & 1) {
                return false;
            }
        }
        return true;
    }
};

/// GOW3_FRAME_STATS: "Stage A cuts per frame" line (driver.cpp). Written by stage A only (a
/// relaxed load and store, no locked add), read and cleared by the report.
enum Counter : unsigned {
    ring_bindings,    ///< buffers CollectRingBindings considered (not special, not empty)
    ring_reused,      ///< bound from an earlier copy (ring reuse)
    ring_copied,      ///< copied into the constant ring
    pending_checks,   ///< PendingWriteOverlaps calls
    pending_bounds,   ///< answered by the bounds (or no entry)
    pending_filtered, ///< answered by the A2 filter
    pending_scanned,  ///< entries scanned
    NumCounters
};
inline std::array<std::atomic<std::uint64_t>, NumCounters> counters{};
inline void Add(Counter counter, std::uint64_t n = 1) noexcept {
    auto& c = counters[counter];
    c.store(c.load(std::memory_order_relaxed) + n, std::memory_order_relaxed);
}

} // namespace Gow3StageA
