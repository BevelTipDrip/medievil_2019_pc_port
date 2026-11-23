// SPDX-License-Identifier: GPL-2.0-or-later
#include "gow3_levels.h"
#include "gow3_settings.h"

#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" const char* runtime_saveinfo_level_name(int index); // src/runtime_saveinfo.c (the launcher's cache)

namespace {

/// The address of the game's TNameEntryArray pointer (src/level_hook.h), 0 when the build has no known name table.
std::atomic<uintptr_t> names_global{0};
std::atomic<bool> suspend_movies{true};

// The text of an FName passed by value (low 32 bits ComparisonIndex, high 32 bits Number): ASCII (other characters
// become '?'), "_<Number - 1>" appended when Number != 0. False when the table is not there or the index is out of
// range. No lock: the game publishes chunks once and never moves or frees them.
bool FNameText(uint64_t fname, char* out, size_t size) {
    const uintptr_t global = names_global.load(std::memory_order_acquire);
    const int32_t index = int32_t(uint32_t(fname)), number = int32_t(fname >> 32);
    if (!global || index < 0 || size < 2) {
        return false;
    }
    const auto* names = *reinterpret_cast<const unsigned char* const*>(global);
    if (!names) {
        return false;
    }
    int32_t count;
    std::memcpy(&count, names + 0x400, sizeof(count));
    if (index >= count) {
        return false;
    }
    const auto* chunk = reinterpret_cast<const unsigned char* const* const*>(names)[index >> 14];
    const unsigned char* entry = chunk ? chunk[index & 0x3fff] : nullptr;
    if (!entry) {
        return false;
    }
    size_t n = 0;
    if (entry[0] & 1) {
        const auto* wide = reinterpret_cast<const uint16_t*>(entry + 0x10);
        for (; wide[n] && n + 1 < size && n < 1023; ++n) {
            out[n] = wide[n] < 0x80 ? char(wide[n]) : '?';
        }
    } else {
        const auto* ansi = reinterpret_cast<const char*>(entry + 0x10);
        for (; ansi[n] && n + 1 < size && n < 1023; ++n) {
            out[n] = static_cast<unsigned char>(ansi[n]) < 0x80 ? ansi[n] : '?';
        }
    }
    out[n] = '\0';
    if (number != 0) {
        std::snprintf(out + n, size - n, "_%d", number - 1);
    }
    return n > 0;
}

bool StartsWithNoCase(const std::string& text, const char* prefix) {
    const size_t n = std::strlen(prefix);
    if (text.size() < n) {
        return false;
    }
    for (size_t i = 0; i < n; ++i) {
        if (std::tolower(static_cast<unsigned char>(text[i])) != std::tolower(static_cast<unsigned char>(prefix[i]))) {
            return false;
        }
    }
    return true;
}

} // namespace

namespace Gow3Levels {

bool Available() {
    return names_global.load(std::memory_order_relaxed) != 0;
}

std::string Title(const std::string& level) {
    if (StartsWithNoCase(level, "MapScreen")) {
        return "Map";
    }
    if (StartsWithNoCase(level, "Level_IN_MM")) {
        return "Main menu";
    }
    if (StartsWithNoCase(level, "Level_AV_GameIntro")) {
        return "Intro";
    }
    if (StartsWithNoCase(level, "Level_AV_")) {
        return "Cutscene " + level.substr(9);
    }
    // Level_NN_CODE: the game's title of level NN (the launcher's cache of the game's level list).
    if (StartsWithNoCase(level, "Level_") && level.size() > 9 && std::isdigit(static_cast<unsigned char>(level[6])) &&
        std::isdigit(static_cast<unsigned char>(level[7])) && level[8] == '_') {
        if (const char* title = runtime_saveinfo_level_name(std::atoi(level.c_str() + 6)); title && title[0]) {
            return title;
        }
    }
    return level;
}

} // namespace Gow3Levels

// src/probe.c install_level_hook: whether the movies are suspended at a level change, and the name table (0: none).
// The boot map is loaded without OpenLevel (DefaultEngine.ini GameDefaultMap): it is the running level until the
// first change.
extern "C" void medievil_level_setup(int suspend, uintptr_t names) {
    suspend_movies.store(suspend != 0, std::memory_order_relaxed);
    names_global.store(names, std::memory_order_release);
    if (names) {
        Gow3Settings::LevelChanged("Level_AV_GameIntro");
    }
}

extern "C" int gow3_level_movie_suspend(void) {
    return suspend_movies.load(std::memory_order_relaxed) ? 1 : 0;
}

// The game opens a level (the game thread, before OpenLevel runs): the per-level settings follow it.
extern "C" void gow3_level_opened(uint64_t fname) {
    char text[256];
    if (!FNameText(fname, text, sizeof(text))) {
        return;
    }
    Gow3Settings::LevelChanged(text);
    if constexpr (GOW3_DEV_TOOLS) {
        std::printf("Level: %s (%s)\n", text, Gow3Levels::Title(text).c_str()); // dev build: an event, not periodic
    }
}
