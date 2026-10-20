// SPDX-License-Identifier: GPL-2.0-or-later
// gow3: MediEvil HZB occlusion remap (research/docs/study-hzb-double-buffer.md, src/hzb_hook.h).
//
// UE4 renders the bounds of every primitive it tests into a 256x256 results texture and reads
// it on the CPU the next frame, by the index each primitive got when it was added (AddBounds).
// The port's GPU runs about a frame behind the game, so the copy the game maps is usually not
// done: stale readbacks then hand it an older frame's results under this frame's indices (wrong
// culling, T-HZ1) and precise ones wait for the GPU (T-HZ3). After the game's MapResults this
// serves the newest finished copy instead, remapped by primitive key (PrimitiveId, CustomIndex)
// from the indices of the frame that produced it to the indices the game reads now. Primitives
// the older frame did not test read as visible, the game's own rule for missing results.
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <utility>
#include <vector>
#include "core/memory.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"

namespace {
using Key = std::uint64_t;
using Entry = std::pair<Key, std::uint32_t>; // key, results index

constexpr std::uint64_t ResultsBytes = 256 * 256 * 4;
constexpr std::uint32_t MaxIndex = 256 * 256;

// FHZBOcclusionTester (at FSceneViewState + 0x170).
constexpr std::size_t TesterInViewState = 0x170, ResultsBuffer = 0x40, ValidFrameNumber = 0x48;
// FSceneViewState::PrimitiveOcclusionHistorySet (TSet of 0x58-byte FPrimitiveOcclusionHistory).
constexpr std::size_t HistoryElements = 0x1360, HistoryNum = 0x1368, HashInline = 0x1398,
                      HashPointer = 0x13a0, HashSize = 0x13a8;
constexpr std::size_t HistoryStride = 0x58, PrimitiveId = 0x00, LastTestFrame = 0x2c,
                      TestIndex = 0x34, CustomIndex = 0x44, HashNext = 0x50;

template <typename T>
T Load(const std::uint8_t* p) {
    T v;
    std::memcpy(&v, p, sizeof(v));
    return v;
}

/// Byte offset of a result in the texture (IsVisible, 0x1534260): 8x8 blocks, 32 blocks a row.
std::uint32_t Texel(std::uint32_t i) {
    const std::uint32_t x = (i & 7) | ((i >> 3) & 0xf8);
    const std::uint32_t y = ((i >> 3) & 7) | ((i >> 8) & 0x3ffff8);
    return 4 * (x + 256 * y);
}
/// Bytes of the texture that hold results 0..max_index (a block row is 8 texel rows).
std::uint64_t BytesFor(std::uint32_t max_index) {
    return std::min<std::uint64_t>(ResultsBytes, (std::uint64_t{max_index >> 8} + 1) * 8 * 256 * 4);
}

struct Frame {
    std::uint64_t seq = 0; ///< readback copy number of the results these indices address
    std::uint32_t max_index = 0;
    std::vector<Entry> entries; ///< sorted by key
};

struct Tester {
    std::uintptr_t tester = 0;
    std::uintptr_t address = 0; ///< guest address of the results texture
    std::array<Frame, 4> ring;
    std::vector<std::uint8_t> source, served;
};

struct Stats {
    std::uint64_t calls = 0, invalid = 0, remapped = 0, no_new_copy = 0, unavailable = 0;
    std::array<std::uint64_t, 6> age{}; // 0, 1, 2, 3, >3 frames, no source
    std::uint64_t keys = 0, unmatched = 0;
    double us_total = 0, us_max = 0;
};

std::mutex mutex;
std::array<Tester, 8> testers;
std::vector<Entry> scratch;
Stats stats;
auto report_time = std::chrono::steady_clock::now();

/// The histories the game looks up this frame: tested in the frame whose results it maps.
bool CollectKeys(const std::uint8_t* view_state, std::uint32_t frame, std::vector<Entry>& out,
                 std::uint32_t& max_index) {
    out.clear();
    max_index = 0;
    const auto* elements = Load<const std::uint8_t*>(view_state + HistoryElements);
    const auto num = Load<std::int32_t>(view_state + HistoryNum);
    auto* hash = Load<const std::int32_t*>(view_state + HashPointer);
    if (!hash) hash = reinterpret_cast<const std::int32_t*>(view_state + HashInline);
    const auto hash_size = Load<std::int32_t>(view_state + HashSize);
    if (num < 0 || hash_size <= 0 || hash_size > (1 << 22) || (hash_size & (hash_size - 1))) return false;
    if (num && !elements) return false;
    std::uint64_t steps = 0;
    for (std::int32_t bucket = 0; bucket < hash_size; ++bucket) {
        for (std::int32_t id = hash[bucket]; id != -1;) {
            if (id < 0 || id >= num || ++steps > (1u << 22)) return false;
            const std::uint8_t* h = elements + std::size_t(id) * HistoryStride;
            if ((Load<std::uint32_t>(h + LastTestFrame) & 0x7fffffff) == frame) {
                const auto index = Load<std::uint32_t>(h + TestIndex);
                if (index >= MaxIndex) return false;
                const Key key = Load<std::uint32_t>(h + PrimitiveId) |
                                Key{Load<std::uint32_t>(h + CustomIndex)} << 32;
                out.emplace_back(key, index);
                max_index = std::max(max_index, index);
            }
            id = Load<std::int32_t>(h + HashNext);
        }
    }
    std::ranges::sort(out, {}, &Entry::first);
    return true;
}

void Report(std::chrono::steady_clock::time_point now) {
    if (now - report_time < std::chrono::seconds(5)) return;
    const auto& s = stats;
    std::printf("HZB remap (5 s): %llu maps (%llu invalid, %llu remapped, %llu without a new copy, "
                "%llu unavailable); age 0/1/2/3/>3/none %llu/%llu/%llu/%llu/%llu/%llu; keys %llu "
                "(%llu read visible); %.1f us avg, %.1f us max\n",
                (unsigned long long)s.calls, (unsigned long long)s.invalid,
                (unsigned long long)s.remapped, (unsigned long long)s.no_new_copy,
                (unsigned long long)s.unavailable, (unsigned long long)s.age[0],
                (unsigned long long)s.age[1], (unsigned long long)s.age[2],
                (unsigned long long)s.age[3], (unsigned long long)s.age[4],
                (unsigned long long)s.age[5], (unsigned long long)s.keys,
                (unsigned long long)s.unmatched, s.remapped ? s.us_total / s.remapped : 0.0, s.us_max);
    stats = {};
    report_time = now;
}

void Remap(std::uint8_t* tester) {
    const auto valid = Load<std::uint32_t>(tester + ValidFrameNumber);
    if (valid == 0xffffffffu) {
        ++stats.invalid;
        return;
    }
    const auto address = Load<std::uintptr_t>(tester + ResultsBuffer);
    auto* rasterizer = Core::Memory::Instance()->GetRasterizer();
    if (!address || !rasterizer) {
        ++stats.unavailable;
        return;
    }
    auto state = std::ranges::find(testers, reinterpret_cast<std::uintptr_t>(tester), &Tester::tester);
    if (state == testers.end()) {
        state = std::ranges::find(testers, std::uintptr_t{0}, &Tester::tester);
        if (state == testers.end()) {
            ++stats.unavailable;
            return;
        }
        state->tester = reinterpret_cast<std::uintptr_t>(tester);
        state->source.resize(ResultsBytes);
        state->served.resize(ResultsBytes);
    }
    if (state->address != address) { // a new results texture (RHI re-init): older indices are void
        state->address = address;
        for (auto& f : state->ring) f.seq = 0;
    }
    std::uint32_t max_index = 0;
    if (!CollectKeys(tester - TesterInViewState, valid, scratch, max_index)) {
        ++stats.unavailable;
        return;
    }
    if (scratch.empty()) return; // nothing is read this frame
    std::uint32_t copy_max = max_index;
    for (const auto& f : state->ring) {
        if (f.seq) copy_max = std::max(copy_max, f.max_index);
    }
    const auto snap = rasterizer->GetTextureCache().SnapshotReadback(
        address, ResultsBytes, state->source.data(), BytesFor(copy_max));
    if (!snap.ok) {
        ++stats.unavailable;
        return;
    }
    std::uint8_t* served = state->served.data();
    const auto fill_visible = [&] {
        for (const auto& [key, index] : scratch) std::memset(served + Texel(index), 0xff, 4);
        std::memcpy(tester + ResultsBuffer, &served, sizeof(served));
    };
    // The game's map returns after the copy of the frame it reads was queued (its label is
    // written after the readback is), so every frame with results has a new copy number. No
    // new number: the indices cannot be tied to a copy; everything reads visible.
    auto same = snap.pushed ? std::ranges::find(state->ring, snap.pushed, &Frame::seq) : state->ring.end();
    if (!snap.pushed || same != state->ring.end()) {
        if (same != state->ring.end()) same->seq = 0;
        ++stats.no_new_copy;
        fill_visible();
        return;
    }
    auto slot = std::ranges::min_element(state->ring, {}, &Frame::seq);
    slot->seq = snap.pushed;
    slot->max_index = max_index;
    slot->entries.swap(scratch);
    const Frame& now = *slot;
    const auto age = snap.pushed - snap.published;
    const Frame* source = nullptr;
    if (snap.published) {
        const auto found = std::ranges::find(state->ring, snap.published, &Frame::seq);
        if (found != state->ring.end()) source = &*found;
    }
    ++stats.age[!source ? 5 : std::min<std::uint64_t>(age, 4)];
    stats.keys += now.entries.size();
    if (!source) {
        stats.unmatched += now.entries.size();
        for (const auto& [key, index] : now.entries) std::memset(served + Texel(index), 0xff, 4);
    } else {
        // Both lists are sorted by key.
        auto it = source->entries.begin();
        const auto end = source->entries.end();
        const std::uint8_t* bytes = state->source.data();
        for (const auto& [key, index] : now.entries) {
            while (it != end && it->first < key) ++it;
            if (it != end && it->first == key) {
                std::memcpy(served + Texel(index), bytes + Texel(it->second), 4);
            } else {
                std::memset(served + Texel(index), 0xff, 4);
                ++stats.unmatched;
            }
        }
    }
    std::memcpy(tester + ResultsBuffer, &served, sizeof(served));
    ++stats.remapped;
}
} // namespace

extern "C" void __attribute__((sysv_abi)) medievil_hzb_after_map(void* tester) {
    const auto start = std::chrono::steady_clock::now();
    std::scoped_lock lock{mutex};
    ++stats.calls;
    const auto remapped = stats.remapped;
    Remap(static_cast<std::uint8_t*>(tester));
    const auto now = std::chrono::steady_clock::now();
    if (stats.remapped != remapped) {
        const double us = std::chrono::duration<double, std::micro>(now - start).count();
        stats.us_total += us;
        stats.us_max = std::max(stats.us_max, us);
    }
    Report(now);
}
