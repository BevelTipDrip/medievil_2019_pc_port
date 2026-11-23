// SPDX-License-Identifier: GPL-2.0-or-later
// gow3: the level the game runs (per-level settings, research/docs/plan-hdr-final.md): its name from the OpenLevel
// hook (src/probe.c install_level_hook, src/level_hook.h) into Gow3Settings::LevelChanged, and the game's title of a
// level name for the menu.

#pragma once

#include <string>

namespace Gow3Levels {

/// The hook found the game's name table: level names are known.
bool Available();
/// The game's title of a level name (Level_01_GY1 -> the launcher's cache of the game's level titles: "The
/// Graveyard"); "Main menu", "Map", "Intro", "Cutscene ..." for the others; the name itself otherwise.
std::string Title(const std::string& level);

} // namespace Gow3Levels
