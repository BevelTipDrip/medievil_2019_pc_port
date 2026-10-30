// SPDX-License-Identifier: GPL-2.0-or-later
#include "gow3_settings.h"
#include "gow3_orbs.h"
#include "gow3_graphics.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdarg>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace Gow3Settings {

namespace {

const char* Path() {
    const char* env = std::getenv("GOW3_CONFIG");
    return env && env[0] ? env : "medievil.ini";
}

float Clamp(float v, float lo, float hi) {
    return std::clamp(v, lo, hi);
}

void Set(Values& v, const std::string& key, const std::string& value) {
    const float f = float(std::atof(value.c_str()));
    const int i = std::atoi(value.c_str());
    for (int p = 0; p < PerfCount; ++p) {
        if (key == PerfKeys[p]) {
            v.perf[p] = value != "0";
            return;
        }
    }
    const auto index_of = [&](const auto& keys) {
        for (int k = 0; k < int(std::size(keys)); ++k) {
            if (value == keys[k]) return k;
        }
        return -1;
    };
    if (key == "ue4_quality") {
        v.ue4_quality = index_of(UE4QualityKeys);
        v.ue4_quality_raw = value;
        return;
    }
    if (key == "output_resolution") {
        v.output_resolution = index_of(UE4OutputKeys);
        v.output_resolution_raw = value;
        return;
    }
    if (key == "effects_resolution") {
        v.effects_resolution = index_of(UE4EffectsKeys);
        v.effects_resolution_raw = value;
        return;
    }
    if (key == "render_scale") {
        v.render_scale = index_of(UE4ScaleKeys);
        v.render_scale_raw = value;
        return;
    }
    static const char* const overlay_keys[] = {"overlay_fps", "overlay_frametime", "overlay_upscaler",
                                               "overlay_gpu", "overlay_cpu", "overlay_ram", "overlay_vram"};
    for (unsigned item = 0; item < 7; ++item) {
        if (key == overlay_keys[item]) {
            const unsigned bit = 1u << item;
            v.overlay_items = i ? (v.overlay_items | bit) : (v.overlay_items & ~bit);
            return;
        }
    }
    if (key == "overlay_scale") {
        v.overlay_scale = std::clamp(i, 50, 300);
        return;
    }
    if (key == "overlay_corner") {
        v.overlay_corner = i >= 0 && i <= 3 ? i : 1;
        return;
    }
    if (key == "overlay_opacity") {
        v.overlay_opacity = std::clamp(i, 0, 100);
        return;
    }
    if (key == "overlay_layout") {
        v.overlay_layout = i == 1 ? 1 : 0;
        return;
    }
    if (key == "menu_tab") {
        v.menu_tab = i >= 0 && i <= 3 ? i : 0;
        return;
    }
    const char* cheat_keys[] = {"cheat_health", "cheat_magic", "cheat_item", "cheat_rage", "cheat_orbs",
                                "cheat_very_hard", "cheat_double_jumps"};
    for (unsigned cheat = 0; cheat < GOW3_CHEAT_COUNT; ++cheat) {
        if (key == cheat_keys[cheat]) {
            v.cheats[cheat] = i != 0;
            return;
        }
    }
    if (key == "upscaler") {
        for (int u = 0; u < UpscalerCount; ++u) {
            if (value == UpscalerName(u)) {
                v.upscaler = u;
            }
        }
    } else if (key == "preset") {
        v.preset = std::clamp(i, 0, PresetCount - 1);
    } else if (key == "sharpen") {
        v.sharpen = i != 0;
    } else if (key == "sharpness") {
        v.sharpness = Clamp(f, 0.0f, 2.0f);
    } else if (key == "jitter") {
        v.jitter = i != 0;
    } else if (key == "reactive") {
        v.reactive = i != 0;
    } else if (key == "object_motion") {
        v.object_motion = i != 0;
    } else if (key == "reactive_scale") {
        v.reactive_scale = Clamp(f, 0.0f, 16.0f);
    } else if (key == "reactive_threshold") {
        v.reactive_threshold = Clamp(f, 0.0f, 1.0f);
    } else if (key == "reactive_max") {
        v.reactive_max = Clamp(f, 0.0f, 1.0f);
    } else if (key == "debug_view") {
        v.debug_view = std::clamp(i, 0, DebugViewCount - 1);
    } else if (key == "show_fps") {
        v.show_fps = i != 0;
    } else if (key == "display_mode") {
        v.display_mode = Gow3Graphics::ParseDisplayMode(value);
    } else if (key == "vsync") {
        v.vsync = i != 0;
    } else if (key == "fps_limit") {
        v.fps_limit = Gow3Graphics::ParseFpsLimit(i);
    } else if (key == "display_vrr") {
        v.display_vrr = i != 0;
    } else if (key == "async_shaders") {
        v.async_shaders = i != 0;
    } else if (key == "fsr1") {
        v.fsr1 = i != 0;
    } else if (key == "rcas") {
        v.rcas = i != 0;
    } else if (key == "rcas_strength") {
        v.rcas_strength = Gow3Graphics::ParseRcasStrength(value.empty() ? -1 : i);
    } else if (key == "frames_queued") {
        v.frames_queued = Gow3Graphics::ParseFramesQueued(value.empty() ? -1 : i);
    } else if (key == "render_resolution") {
        v.render_resolution = Gow3Graphics::ParseResolution(value);
    } else if (key == "engine_fps") {
        v.engine_fps = Gow3Graphics::ParseEngineFps(i);
    } else if (key == "deferred_readback") {
        v.deferred_readback = i != 0;
    } else if (key == "stale_readback") {
        v.stale_readback = i != 0;
    } else if (key == "restart_unconfirmed") {
        v.restart_unconfirmed = i != 0;
    } else if (key == "enemy_health_bar") {
        v.enemy_health_bar = i != 0;
    } else if (key == "red_orb_multiplier") {
        v.red_orb_multiplier = Gow3Orbs::ClampMultiplier(f);
    } else if (key == "damage_dealt") {
        v.damage_dealt = Gow3Orbs::ClampMultiplier(f);
    } else if (key == "damage_taken") {
        v.damage_taken = Gow3Orbs::ClampMultiplier(f);
    } else if (key == "green_orb_multiplier") {
        v.green_orb_multiplier = Gow3Orbs::ClampMultiplier(f);
    } else if (key == "blue_orb_multiplier") {
        v.blue_orb_multiplier = Gow3Orbs::ClampMultiplier(f);
    } else if (key == "gold_orb_multiplier") {
        v.gold_orb_multiplier = Gow3Orbs::ClampMultiplier(f);
    } else if (key == "fsr4_auto_exposure") {
        v.fsr4_auto_exposure = i != 0;
    } else if (key == "fsr4_invert_jitter") {
        v.fsr4_invert_jitter = i != 0;
    } else if (key == "live_resolution") {
        v.live_resolution = value == "auto" ? -1 : std::clamp(i, 0, 1);
    } else if (key == "output_res") {
        for (int r = 0; r < OutputCount; ++r) {
            if (value == std::to_string(OutputWidths[r]) + "x" + std::to_string(OutputHeights[r])) {
                v.output_res = r;
            }
        }
    }
}

} // namespace

Values& Get() {
    static Values values;
    return values;
}

void Load() {
    auto& v = Get();
    if (FILE* file = std::fopen(Path(), "r")) {
        char line[256];
        while (std::fgets(line, sizeof(line), file)) {
            std::string text{line};
            text.erase(text.find_last_not_of(" \t\r\n") + 1);
            const auto eq = text.find('=');
            if (text.empty() || text[0] == '#' || eq == std::string::npos) {
                continue;
            }
            Set(v, text.substr(0, eq), text.substr(eq + 1));
        }
        std::fclose(file);
        std::printf("Settings: %s\n", Path());
    }
    // Environment overrides (scripts, A/B tests).
    if (const char* env = std::getenv("GOW3_UPSCALER")) {
        v.upscaler = UpscalerOff;
        for (int u = 0; u < UpscalerCount; ++u) {
            if (std::strcmp(env, UpscalerName(u)) == 0) v.upscaler = u;
        }
    }
    const std::pair<const char*, const char*> env_keys[] = {
        {"GOW3_FSR_SHARPNESS", "sharpness"},        {"GOW3_JITTER", "jitter"},
        {"GOW3_REACTIVE", "reactive"},              {"GOW3_REACTIVE_SCALE", "reactive_scale"},
        {"GOW3_REACTIVE_THRESHOLD", "reactive_threshold"}, {"GOW3_REACTIVE_MAX", "reactive_max"},
        {"GOW3_UPSCALE_PRESET", "preset"},            {"GOW3_OBJECT_MOTION", "object_motion"},
        {"GOW3_ASYNC_SHADERS", "async_shaders"},      {"GOW3_DEFERRED_READBACK", "deferred_readback"},
        {"GOW3_STALE_READBACK", "stale_readback"},     {"GOW3_FSR1", "fsr1"},
        {"GOW3_RCAS", "rcas"},                        {"GOW3_FRAMES_AHEAD", "frames_queued"},
    };
    for (const auto& [env, key] : env_keys) {
        if (const char* value = std::getenv(env); value && value[0]) {
            Set(v, key, value);
        }
    }
    v.startup_preset = v.preset;
    v.startup_upscaler = v.upscaler;
    v.startup_object_motion = v.object_motion;
    v.startup_output_res = v.output_res;
    v.startup_live_resolution = v.live_resolution;
    v.startup_render_resolution = v.render_resolution;
    v.startup_engine_fps = v.engine_fps;
    v.startup_deferred_readback = v.deferred_readback;
    v.startup_stale_readback = v.stale_readback;
    // A launch's own switches (run.py from medievil.ini, or a test) show as the patches' state.
    static constexpr std::pair<const char*, const char*> perf_off[PerfCount] = {
        {"GOW3_HZB_REMAP", "0"},     {"GOW3_DECAL_PREPASS", "1"},       {"GOW3_PARTICLE_FIXED_STEP", "1"},
        {"GOW3_STREAM_CLEAN", "0"},  {"GOW3_TEXSET_REGIONS", "0"},      {"GOW3_OPTIMISTIC_CLAMP", "0"},
        {"GOW3_DECOMMIT_KEEP_ACCESS", "0"}, {"GOW3_READBACK_QUEUE", "0"}, {"GOW3_PS4_MEMORY", "1"},
        {"GOW3_SKIP_READ_BARRIERS", "0"}};
    for (int p = 0; p < PerfCount; ++p) {
        if (const char* env = std::getenv(perf_off[p].first); env && env[0]) {
            v.perf[p] = std::strcmp(env, perf_off[p].second) != 0;
        }
        v.startup_perf[p] = v.perf[p];
    }
    // Small buffers from VRAM was tested in MediEvil only: on where run.py turns it on.
    {
        const char* env = std::getenv("GOW3_STREAM_CLEAN");
        v.perf[PerfSmallBuffers] = env && env[0] == '1';
        v.startup_perf[PerfSmallBuffers] = v.perf[PerfSmallBuffers];
    }
    v.startup_ue4_quality = v.ue4_quality;
    v.startup_output_resolution = v.output_resolution;
    v.startup_render_scale = v.render_scale;
    v.startup_effects_resolution = v.effects_resolution;
    TakeBaseline();
}

int RestartChanges() {
    const auto& v = Get();
    int changes = (v.render_resolution != v.startup_render_resolution) + (v.engine_fps != v.startup_engine_fps) +
                  (v.deferred_readback != v.startup_deferred_readback) +
                  (v.stale_readback != v.startup_stale_readback) + (v.ue4_quality != v.startup_ue4_quality) +
                  (v.output_resolution != v.startup_output_resolution) +
                  (v.render_scale != v.startup_render_scale) +
                  (v.effects_resolution != v.startup_effects_resolution);
    for (int p = 0; p < PerfCount; ++p) {
        changes += !PerfLive[p] && v.perf[p] != v.startup_perf[p];
    }
    return changes;
}

bool GraphicsNeedRestart() {
    return RestartChanges() != 0;
}

void ConfigureUpscalerSupport(bool fsr4, bool fsr411) {
    auto& v = Get();
    v.fsr4_supported = fsr4;
    v.fsr411_supported = fsr4 && fsr411;
    const int requested = v.upscaler;
    if ((requested == UpscalerFsr4 && !v.fsr4_supported) ||
        (requested == UpscalerFsr411 && !v.fsr411_supported)) {
        v.fsr4_problem = "GPU does not support the selected FSR 4 shaders; using FSR 3.1";
        std::printf("Upscaler: %s unsupported on this GPU; falling back to FSR 3.1 before the first frame\n",
                    UpscalerName(requested));
        v.upscaler = UpscalerFsr3;
    }
}

void ConfigureDlssSupport(bool available, const char* problem) {
    auto& v = Get();
    v.dlss_supported = available;
    static std::string kept;
    kept = problem ? problem : "";
    v.dlss_problem = available || kept.empty() ? nullptr : kept.c_str();
    if (v.upscaler == UpscalerDlss && !available) {
        std::printf("Upscaler: DLSS unavailable (%s); falling back to FSR 3.1\n",
                    kept.empty() ? "gow3_dlss.dll or nvngx_dlss.dll missing" : kept.c_str());
        v.upscaler = UpscalerFsr3;
    }
}

bool FixedRenderSession() {
    const char* size = std::getenv("GOW3_RENDER_RES");
    return size && size[0];
}

int RenderPreset() {
    const auto& v = Get();
    return FixedRenderSession() ? v.startup_preset :
        v.upscaler == UpscalerTaa ? NativeAA : v.preset.load();
}

bool ResolutionNeedsRestart() {
    const auto& v = Get();
    // TAA needs the live path (native guest targets): run.sh selects it on restart.
    return FixedRenderSession() &&
        (v.preset != v.startup_preset || v.output_res != v.startup_output_res ||
         (v.upscaler == UpscalerOff) != (v.startup_upscaler == UpscalerOff) ||
         (v.upscaler == UpscalerTaa) != (v.startup_upscaler == UpscalerTaa));
}

namespace {
// "key" of a "key=value" line, empty for comments and other lines.
std::string LineKey(const std::string& line) {
    const size_t eq = line.find('=');
    if (eq == std::string::npos || line.empty() || line[0] == '#') {
        return {};
    }
    size_t end = eq;
    while (end > 0 && (line[end - 1] == ' ' || line[end - 1] == '\t')) {
        --end;
    }
    return line.substr(0, end);
}

std::vector<std::string> ReadLines(const char* path) {
    std::vector<std::string> lines;
    if (FILE* file = std::fopen(path, "r")) {
        char buffer[1024];
        while (std::fgets(buffer, sizeof(buffer), file)) {
            std::string line = buffer;
            while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) {
                line.pop_back();
            }
            lines.push_back(std::move(line));
        }
        std::fclose(file);
    }
    return lines;
}
} // namespace

namespace {
void Append(std::string& out, const char* format, ...) {
    char buffer[2048];
    va_list args;
    va_start(args, format);
    const int n = std::vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    if (n > 0) out.append(buffer, std::min<std::size_t>(std::size_t(n), sizeof(buffer) - 1));
}

// Every setting this menu owns as "key" -> "value" (the text Save writes).
std::map<std::string, std::string> Serialize(const Values& v) {
    std::string out;
    Append(out,
           "upscaler=%s\npreset=%d\nsharpen=%d\nsharpness=%.2f\njitter=%d\n"
           "reactive=%d\nobject_motion=%d\nreactive_scale=%.2f\nreactive_threshold=%.2f\nreactive_max=%.2f\n"
           "debug_view=%d\nshow_fps=%d\nfsr4_auto_exposure=%d\nfsr4_invert_jitter=%d\n",
           UpscalerName(v.upscaler), v.preset.load(), int(v.sharpen.load()),
           v.sharpness.load(), int(v.jitter.load()), int(v.reactive.load()),
           int(v.object_motion.load()),
           v.reactive_scale.load(), v.reactive_threshold.load(), v.reactive_max.load(),
           v.debug_view.load(), int(v.show_fps.load()),
           int(v.fsr4_auto_exposure.load()), int(v.fsr4_invert_jitter.load()));
    Append(out, "output_res=%dx%d\n", OutputWidths[v.output_res], OutputHeights[v.output_res]);
    Append(out, "red_orb_multiplier=%.3f\n", v.red_orb_multiplier.load());
    Append(out, "damage_dealt=%.3f\n", v.damage_dealt.load());
    Append(out, "damage_taken=%.3f\n", v.damage_taken.load());
    Append(out, "green_orb_multiplier=%.3f\n", v.green_orb_multiplier.load());
    Append(out, "blue_orb_multiplier=%.3f\n", v.blue_orb_multiplier.load());
    Append(out, "gold_orb_multiplier=%.3f\n", v.gold_orb_multiplier.load());
    Append(out, "cheat_health=%d\ncheat_magic=%d\ncheat_item=%d\ncheat_rage=%d\ncheat_orbs=%d\n"
                "cheat_very_hard=%d\ncheat_double_jumps=%d\n",
           int(v.cheats[0].load()), int(v.cheats[1].load()), int(v.cheats[2].load()),
           int(v.cheats[3].load()), int(v.cheats[4].load()),
           int(v.cheats[5].load()), int(v.cheats[6].load()));
    Append(out, "enemy_health_bar=%d\n", int(v.enemy_health_bar.load()));
    Append(out,
           "overlay_fps=%d\noverlay_frametime=%d\noverlay_upscaler=%d\noverlay_gpu=%d\n"
           "overlay_cpu=%d\noverlay_ram=%d\noverlay_vram=%d\noverlay_scale=%d\noverlay_corner=%d\n"
           "overlay_opacity=%d\noverlay_layout=%d\nmenu_tab=%d\n",
           int(v.overlay_items >> 0 & 1), int(v.overlay_items >> 1 & 1), int(v.overlay_items >> 2 & 1),
           int(v.overlay_items >> 3 & 1), int(v.overlay_items >> 4 & 1), int(v.overlay_items >> 5 & 1),
           int(v.overlay_items >> 6 & 1), v.overlay_scale.load(), v.overlay_corner.load(),
           v.overlay_opacity.load(), v.overlay_layout.load(), v.menu_tab.load());
    Append(out,
           "display_mode=%s\nvsync=%d\nfps_limit=%d\ndisplay_vrr=%d\nasync_shaders=%d\n"
           "fsr1=%d\nrcas=%d\nrcas_strength=%d\nframes_queued=%d\n"
           "render_resolution=%s\nengine_fps=%d\ndeferred_readback=%d\nstale_readback=%d\n"
           "restart_unconfirmed=%d\n",
           Gow3Graphics::DisplayModeKeys[std::clamp(v.display_mode.load(), 0, 2)].data(),
           int(v.vsync.load()), v.fps_limit.load(), int(v.display_vrr.load()), int(v.async_shaders.load()),
           int(v.fsr1.load()), int(v.rcas.load()), v.rcas_strength.load(), v.frames_queued.load(),
           Gow3Graphics::Resolutions[std::clamp(v.render_resolution.load(), 0, int(Gow3Graphics::Resolutions.size()) - 1)].data(),
           v.engine_fps.load(), int(v.deferred_readback.load()), int(v.stale_readback.load()),
           int(v.restart_unconfirmed.load()));
    // Read by run.sh at start.
    Append(out, "live_resolution=%s\n", v.live_resolution < 0 ? "auto" : v.live_resolution ? "1" : "0");
    for (int p = 0; p < PerfCount; ++p) {
        Append(out, "%s=%d\n", PerfKeys[p], int(v.perf[p].load()));
    }
    Append(out, "ue4_quality=%s\n", v.ue4_quality >= 0 ? UE4QualityKeys[v.ue4_quality] : v.ue4_quality_raw.c_str());
    Append(out, "output_resolution=%s\n",
           v.output_resolution >= 0 ? UE4OutputKeys[v.output_resolution] : v.output_resolution_raw.c_str());
    Append(out, "render_scale=%s\n", v.render_scale >= 0 ? UE4ScaleKeys[v.render_scale] : v.render_scale_raw.c_str());
    Append(out, "effects_resolution=%s\n", v.effects_resolution >= 0 ? UE4EffectsKeys[v.effects_resolution]
                                                                    : v.effects_resolution_raw.c_str());
    std::map<std::string, std::string> values;
    std::size_t at = 0;
    while (at < out.size()) {
        const std::size_t nl = out.find('\n', at);
        const std::string line = out.substr(at, nl == std::string::npos ? std::string::npos : nl - at);
        at = nl == std::string::npos ? out.size() : nl + 1;
        if (const auto eq = line.find('='); eq != std::string::npos) {
            values[line.substr(0, eq)] = line.substr(eq + 1);
        }
    }
    return values;
}

// The values as loaded (environment overrides included) or last saved: Save writes only the
// settings changed in the menu since then.
std::map<std::string, std::string>& Baseline() {
    static std::map<std::string, std::string> baseline;
    return baseline;
}
} // namespace

void TakeBaseline() {
    Baseline() = Serialize(Get());
}

void Save() {
    // gow3: only the settings changed in the menu are written, in place; every other line of
    // medievil.ini (the launcher's keys, values this menu did not touch) stays as it was.
    // Rewriting every key saved environment overrides of test launches (frames queued, readback
    // modes) and this menu's own defaults over the launcher's settings.
    const auto current = Serialize(Get());
    std::map<std::string, std::string> changes;
    for (const auto& [key, value] : current) {
        const auto it = Baseline().find(key);
        if (it == Baseline().end() || it->second != value) {
            changes[key] = value;
        }
    }
    if (changes.empty()) {
        return;
    }
    std::vector<std::string> lines = ReadLines(Path());
    if (lines.empty()) {
        lines.push_back("# MediEvil settings (in-game menu: Insert / R3+L2)");
    }
    std::set<std::string> done;
    for (std::string& line : lines) {
        const std::string key = LineKey(line);
        if (const auto it = changes.find(key); it != changes.end() && !key.empty()) {
            line = key + "=" + it->second;
            done.insert(key);
        }
    }
    for (const auto& [key, value] : changes) {
        if (!done.contains(key)) {
            lines.push_back(key + "=" + value);
        }
    }
    FILE* file = std::fopen(Path(), "w");
    if (!file) {
        std::printf("Settings: cannot write %s\n", Path());
        return;
    }
    for (const std::string& line : lines) {
        std::fprintf(file, "%s\n", line.c_str());
    }
    std::fclose(file);
    for (const auto& [key, value] : changes) {
        Baseline()[key] = value;
    }
}

float PresetScale(int preset) {
    static constexpr float scales[PresetCount] = {1.0f, 1.5f, 1.7f, 2.0f, 3.0f};
    return scales[std::clamp(preset, 0, PresetCount - 1)];
}

const char* PresetName(int preset) {
    static constexpr const char* names[PresetCount] = {"Native AA", "Quality", "Balanced",
                                                       "Performance", "Ultra Performance"};
    return names[std::clamp(preset, 0, PresetCount - 1)];
}

const char* UpscalerName(int upscaler) {
    static constexpr const char* names[UpscalerCount] = {"off", "fsr3", "fsr4", "fsr411", "taa", "dlss"};
    return names[std::clamp(upscaler, 0, UpscalerCount - 1)];
}

} // namespace Gow3Settings
