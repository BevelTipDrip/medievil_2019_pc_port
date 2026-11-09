// SPDX-License-Identifier: GPL-2.0-or-later
// gow3 U1: UE4 frame map, log only (vk_ue4_frame_map.h).

#include "video_core/renderer_vulkan/vk_ue4_frame_map.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace Vulkan {

namespace {

// View uniform buffer offsets in bytes (UE 4.21, checked in two captures: study-taau-frame-map.md
// section 3).
constexpr u32 OffsetViewToClipJitter = 384 + 2 * 16; // ViewToClip row 2, x and y
constexpr u32 OffsetClipToPrevClip = 1824;
constexpr u32 OffsetJitter = 1888;
constexpr u32 OffsetViewSize = 1952;
constexpr u32 OffsetBufferSize = 1968;
constexpr u32 OffsetPreExposure = 2004;
constexpr u32 OffsetNearPlane = 2112;
constexpr u32 OffsetMipBias = 2128;
constexpr u32 OffsetFrameNumber = 2140;
constexpr u32 OffsetFrameIndex = 2144;
constexpr u32 OffsetCameraCut = 2148;

template <typename T>
T Load(const u8* data, u32 offset) {
    T value;
    std::memcpy(&value, data + offset, sizeof(T));
    return value;
}

template <typename... Args>
std::string Format(const char* format, Args... args) {
    char text[512];
    std::snprintf(text, sizeof(text), format, args...);
    return text;
}

template <typename T, size_t N>
bool AddDistinct(std::array<T, N>& list, u32& count, T value) {
    if (std::find(list.begin(), list.begin() + count, value) != list.begin() + count) {
        return false;
    }
    if (count < N) {
        list[count++] = value;
    }
    return true;
}

std::string Shader(const char* stage, u64 hash) {
    return Format("%s %016llx", stage, static_cast<unsigned long long>(hash));
}

} // namespace

bool Ue4FrameMap::Enabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("GOW3_UE4_FRAME_MAP");
        return value && value[0] == '1';
    }();
    return enabled;
}

Ue4FrameMap::Ue4FrameMap(Sink sink_) : sink{std::move(sink_)} {
    Print("UE4 frame map: on (log only)");
}

void Ue4FrameMap::Print(const std::string& line) const {
    if (sink) {
        sink(line);
    } else {
        std::printf("%s\n", line.c_str());
    }
}

Ue4FrameMap::View Ue4FrameMap::ReadView(const u8* data) {
    View view;
    view.valid = true;
    for (u32 i = 0; i < 4; ++i) {
        view.jitter[i] = Load<float>(data, OffsetJitter + i * 4);
    }
    view.jitter_in_projection = Load<float>(data, OffsetViewToClipJitter) == view.jitter[0] &&
                                Load<float>(data, OffsetViewToClipJitter + 4) == view.jitter[1];
    view.view_size = {Load<float>(data, OffsetViewSize), Load<float>(data, OffsetViewSize + 4)};
    view.buffer_size = {Load<float>(data, OffsetBufferSize),
                        Load<float>(data, OffsetBufferSize + 4)};
    view.pre_exposure = Load<float>(data, OffsetPreExposure);
    view.near_plane = Load<float>(data, OffsetNearPlane);
    view.mip_bias = Load<float>(data, OffsetMipBias);
    view.frame_number = Load<u32>(data, OffsetFrameNumber);
    view.index = Load<u32>(data, OffsetFrameIndex);
    view.camera_cut = Load<u32>(data, OffsetCameraCut); // nonzero (1 or 1.0f) is a cut
    for (u32 i = 0; i < 16; ++i) {
        const float identity = i % 5 == 0 ? 1.0f : 0.0f;
        const float element = Load<float>(data, OffsetClipToPrevClip + i * 4);
        view.camera_delta = std::max(view.camera_delta, std::fabs(element - identity));
    }
    return view;
}

void Ue4FrameMap::OnPass(const Pass& pass) {
    // A new frame: a draw into a display buffer other than this frame's, or a second tonemap.
    if (!pass.compute && pass.display &&
        ((frame.display && pass.target.address != frame.display) ||
         (pass.hash == Tonemap && frame.tonemap_draws))) {
        Close();
    }
    ++frame.passes;
    const VAddr velocity = frame.velocity.address ? frame.velocity.address : last_velocity;
    const VAddr depth = frame.velocity_depth ? frame.velocity_depth : last_depth;
    bool reads_velocity = false;
    bool reads_depth = false;
    for (u32 i = 0; i < pass.num_reads; ++i) {
        reads_velocity |= velocity && pass.reads[i] == velocity;
        reads_depth |= depth && pass.reads[i] == depth;
    }

    // Velocity: one R16G16_UNORM colour target drawn with a depth buffer (its shaders are per
    // material, so it is found by its target).
    if (!pass.compute && pass.target_rg16 && pass.targets == 1 && pass.depth) {
        if (!frame.velocity.address) {
            frame.velocity = pass.target;
            frame.velocity_depth = pass.depth;
        }
        ++(pass.target.address == frame.velocity.address ? frame.velocity_draws
                                                         : frame.other_rg16_draws);
        return;
    }

    if (reads_velocity && frame.readers.size() < 16) {
        const std::pair<const char*, u64> reader{pass.stage, pass.hash};
        if (std::ranges::find(frame.readers, reader) == frame.readers.end()) {
            frame.readers.push_back(reader);
        }
    }

    const bool taa_draw = !pass.compute && (pass.hash == TaaNormal || pass.hash == TaaResponsive);
    const bool taa_dispatch = pass.compute && pass.hash == TaaCompute;
    if (taa_draw || taa_dispatch) {
        ++(taa_draw ? frame.taa_draws : frame.taa_dispatches);
        u32 count = frame.taa_hashes[1] ? 2 : frame.taa_hashes[0] ? 1 : 0;
        AddDistinct(frame.taa_hashes, count, pass.hash);
        frame.taa_stage = pass.stage;
        if (taa_dispatch) {
            frame.taa_groups = pass.groups;
            frame.taa_output = pass.num_writes ? pass.writes[0] : Image{};
        } else {
            frame.taa_output = pass.target;
        }
        frame.taa_reads_velocity |= reads_velocity;
        frame.taa_reads_depth |= reads_depth;
        if (!frame.view.valid) {
            if (pass.view) {
                frame.view = ReadView(pass.view);
                frame.taa_without_view = false;
            } else {
                frame.taa_without_view = true;
                frame.taa_buffer_sizes = pass.buffer_sizes;
                frame.taa_num_buffers = pass.num_buffers;
            }
        }
    }
    if (pass.compute && pass.hash == TaauOther) {
        ++frame.taau_other_dispatches;
        frame.taau_other_groups = pass.groups;
    }

    if (!pass.compute && pass.display) {
        if (!frame.display) {
            frame.display = pass.target.address;
            frame.display_target = pass.target;
        }
        ++frame.display_draws;
        AddDistinct(frame.display_shaders, frame.num_display_shaders, pass.hash);
        if (pass.hash == Tonemap) {
            ++frame.tonemap_draws;
        } else if (pass.hash == FirstUi) {
            ++frame.ui_draws;
        } else if (frame.tonemap_draws && !frame.ui_draws) {
            ++frame.before_ui_other;
        }
    }
}

std::string Ue4FrameMap::Describe(const Frame& f) const {
    std::string text;
    if (f.velocity_draws) {
        text += Format("velocity %ux%u (%u draws", f.velocity.width, f.velocity.height,
                       f.velocity_draws);
        text += f.other_rg16_draws ? Format(", %u into other RG16 targets)", f.other_rg16_draws)
                                   : std::string(")");
    } else {
        text += "no velocity pass";
    }
    if (f.taa_draws || f.taa_dispatches) {
        text += f.taa_dispatches ? Format("; TAA compute x%u", f.taa_dispatches)
                                 : Format("; TAA draws x%u", f.taa_draws);
        for (const u64 hash : f.taa_hashes) {
            if (hash) {
                text += " " + Shader(f.taa_stage, hash);
            }
        }
        if (f.taa_dispatches) {
            text += Format(" (%ux%ux%u groups)", f.taa_groups[0], f.taa_groups[1],
                           f.taa_groups[2]);
        }
        text += Format(" -> %ux%u, reads velocity %s depth %s", f.taa_output.width,
                       f.taa_output.height, f.taa_reads_velocity ? "yes" : "no",
                       f.taa_reads_depth ? "yes" : "no");
    } else {
        text += "; no TAA";
    }
    if (f.view.valid) {
        const auto& v = f.view;
        text += Format("; View %gx%g in %gx%g, jitter %.3f %.3f px (index %u%s), frame %u, "
                       "mip bias %.2f, pre-exposure %.2f, near %g, camera delta %.4f%s",
                       v.view_size[0], v.view_size[1], v.buffer_size[0], v.buffer_size[1],
                       v.jitter[0] * v.view_size[0] / 2, -v.jitter[1] * v.view_size[1] / 2,
                       v.index, v.jitter_in_projection ? "" : ", not in ViewToClip",
                       v.frame_number, v.mip_bias, v.pre_exposure, v.near_plane, v.camera_delta,
                       v.camera_cut ? ", camera cut" : "");
    } else if (f.taa_without_view) {
        text += "; TAA without a View buffer, buffer sizes";
        for (u32 i = 0; i < f.taa_num_buffers; ++i) {
            text += Format(" %u", f.taa_buffer_sizes[i]);
        }
    }
    if (f.taau_other_dispatches) {
        text += Format("; %s x%u (%ux%ux%u groups)", Shader("cs", TaauOther).c_str(),
                       f.taau_other_dispatches, f.taau_other_groups[0], f.taau_other_groups[1],
                       f.taau_other_groups[2]);
    }
    if (f.display_draws) {
        text += Format("; display %ux%u, %u draws:", f.display_target.width,
                       f.display_target.height, f.display_draws);
        for (u32 i = 0; i < f.num_display_shaders; ++i) {
            const u64 hash = f.display_shaders[i];
            text += " " + Shader("fs", hash);
            text += hash == Tonemap ? Format(" (tonemap x%u)", f.tonemap_draws)
                    : hash == FirstUi ? Format(" (UI x%u)", f.ui_draws)
                                      : std::string();
        }
        if (f.before_ui_other) {
            text += Format(", %u other draws between tonemap and UI", f.before_ui_other);
        }
    } else {
        text += "; no display draw";
    }
    if (!f.readers.empty()) {
        text += "; velocity read by";
        for (const auto& [stage, hash] : f.readers) {
            text += " " + Shader(stage, hash);
        }
    }
    return text;
}

std::string Ue4FrameMap::Signature(const Frame& f) const {
    std::string text = Format("v%d %ux%u t%d%d", f.velocity_draws != 0, f.velocity.width,
                              f.velocity.height, f.taa_draws != 0, f.taa_dispatches != 0);
    for (const u64 hash : f.taa_hashes) {
        text += Format(" %llx", static_cast<unsigned long long>(hash));
    }
    text += Format(" %ux%u %d%d%d", f.taa_output.width, f.taa_output.height,
                   f.taa_reads_velocity, f.taa_reads_depth, f.view.valid);
    if (f.view.valid) {
        text += Format(" %gx%g %gx%g", f.view.view_size[0], f.view.view_size[1],
                       f.view.buffer_size[0], f.view.buffer_size[1]);
    }
    text += Format(" o%d d%ux%u", f.taau_other_dispatches != 0, f.display_target.width,
                   f.display_target.height);
    for (u32 i = 0; i < std::min(f.num_display_shaders, 2u); ++i) {
        text += Format(" %llx", static_cast<unsigned long long>(f.display_shaders[i]));
    }
    auto readers = f.readers;
    std::ranges::sort(readers, [](const auto& a, const auto& b) { return a.second < b.second; });
    for (const auto& [stage, hash] : readers) {
        text += Format(" r%llx", static_cast<unsigned long long>(hash));
    }
    return text;
}

void Ue4FrameMap::Close() {
    if (!frame.passes) {
        return;
    }
    ++frames_total;
    ++counts.frames;
    counts.velocity += frame.velocity_draws != 0;
    counts.taa += frame.taa_draws || frame.taa_dispatches;
    counts.taa_draws += frame.taa_draws != 0;
    counts.taa_compute += frame.taa_dispatches != 0;
    counts.view += frame.view.valid;
    counts.tonemap += frame.tonemap_draws != 0;
    counts.ui += frame.ui_draws != 0;
    if (frame.velocity.address) {
        last_velocity = frame.velocity.address;
        last_depth = frame.velocity_depth;
    }
    const auto limited = [&](const std::string& line) {
        if (counts.changes < ChangeLines) {
            ++counts.changes;
            Print(line);
        } else {
            ++counts.unshown;
        }
    };
    if (frame.view.valid) {
        const auto& v = frame.view;
        if (have_frame_number && v.frame_number == last_frame_number) {
            ++counts.repeats;
        } else if (have_frame_number && v.frame_number != last_frame_number + 1) {
            ++counts.gaps;
        }
        have_frame_number = true;
        last_frame_number = v.frame_number;
        if (v.camera_cut) {
            ++counts.cuts;
            limited(Format("UE4 frame map: camera cut at View frame %u", v.frame_number));
        }
        const std::pair<float, float> jitter{v.jitter[0], v.jitter[1]};
        if (jitters.size() < 64 && std::ranges::find(jitters, jitter) == jitters.end()) {
            jitters.push_back(jitter);
        }
    }
    for (const auto& reader : frame.readers) {
        if (seen_readers.size() < 64 &&
            std::ranges::find(seen_readers, reader) == seen_readers.end()) {
            seen_readers.push_back(reader);
            Print("UE4 frame map: velocity read by " + Shader(reader.first, reader.second) +
                  Format(" (first at frame %llu)", static_cast<unsigned long long>(frames_total)));
        }
    }
    std::string signature = Signature(frame);
    if (signature != last_signature) {
        limited(Format("UE4 frame map: frame %llu: ", static_cast<unsigned long long>(frames_total)) +
                Describe(frame));
        last_signature = std::move(signature);
    }
    if (counts.frames >= SummaryFrames) {
        Print(Format("UE4 frame map: %u frames: velocity %u, TAA %u (draws %u, compute %u), "
                     "View %u, tonemap %u, UI %u; camera cuts %u, View frame gaps %u, "
                     "repeats %u, jitter values %zu; %u lines not shown",
                     counts.frames, counts.velocity, counts.taa, counts.taa_draws,
                     counts.taa_compute, counts.view, counts.tonemap, counts.ui, counts.cuts,
                     counts.gaps, counts.repeats, jitters.size(), counts.unshown));
        Print("UE4 frame map: last frame: " + Describe(frame));
        counts = {};
        jitters.clear();
    }
    frame = {};
}

} // namespace Vulkan
