// SPDX-License-Identifier: GPL-2.0-or-later
// gow3: in-game settings menu (Dear ImGui), drawn by the presenter into the swapchain image
// after the game frame, at display resolution. Insert (keyboard, GOW3_MENU_KEY) or R3+L2 (gamepad,
// GOW3_MENU_PAD) opens it; while it is open the game gets no pad/keyboard input. Settings live in
// gow3_settings.h; research/docs/plan-settings-menu.md has its look and controls.

#pragma once

#include <string>
#include <vector>
#include "common/types.h"
#include "gow3_png.h"
#include "video_core/renderer_vulkan/vk_common.h"

union SDL_Event;
struct SDL_Window;

namespace Vulkan {
class Instance;
}

namespace Gow3Overlay {

/// Present thread, once: the ImGui context and its Vulkan backend.
void Init(const Vulkan::Instance& instance, vk::Format format, u32 image_count);

/// Window thread, for every SDL event: true when the menu consumed it.
bool HandleEvent(const SDL_Event& event);
/// Turns SDL text input on while the menu edits a value (window thread, once per poll).
void UpdateTextInput(SDL_Window* window);

/// Start-up loading screen (pipeline cache warm-up): `total` 0 shows no count.
void SetLoading(bool active, u32 done, u32 total);
/// Background cache warm-up: a small corner note while the game runs; `total` 0 hides it.
void SetBackgroundLoading(u32 done, u32 total);

/// Whether anything is drawn this frame (menu open or FPS counter on).
bool Visible();

/// F12: true once after a screenshot was asked for (the presenter saves the next frame).
bool TakeScreenshotRequest();

/// Counts a presented frame for GOW3_FPS_LOG.
void LogPresent();

/// Present thread: draws into `view` (layout ColorAttachmentOptimal).
void Render(vk::CommandBuffer cmdbuf, vk::ImageView view, vk::Extent2D extent);

/// The menu is open or a text entry is shown: the game's input is held neutral.
bool CapturesInput();

/// Any thread: open the menu on the next presented frame (the pause menu's PC Settings row,
/// src/pc_settings_hook.h). Buttons held then wait for release, as when the menu key opens it.
void RequestOpen();

/// The runtime's buttons (PS4 bits) that move and confirm the save list, polled every frame.
void SetChoiceInput(uint32_t (*buttons)());
/// Reads the save list's buttons now (also done every presented frame).
void PollChoiceInput();
/// The runtime's buttons (PS4 bits) that drive the settings menu, polled every presented frame
/// while it is open: the D-pad moves and changes, Cross changes, Circle closes, L1 / R1 switch tabs.
void SetMenuInput(uint32_t (*buttons)());

/// Window thread: the game's text dialog (sceImeDialog). Shown as a box on screen until
/// `active` is false; typing goes to the window (sdl_window), the box only displays it.
void SetTextEntry(bool active, const std::string& prompt, const std::string& text);

/// The game's save list: shown until the player chooses (arrows/D-pad, Enter/Cross) or
/// cancels (Esc/Circle). `pictures`: one per item (empty = none). With `details` (per item, may
/// be empty), the list is drawn on the left and the chosen item's picture and details on the
/// right; Square / Tab switches the right side to its `more` text. Without, each picture is drawn
/// beside its item. PollChoice: -1 while open, -2 cancelled, else the chosen index.
void BeginChoice(const std::string& title, const std::vector<std::string>& items,
                 std::vector<Gow3Png::Image> pictures, std::vector<std::string> details,
                 std::vector<std::string> more, int focus);
int PollChoice();

/// The game's menu look for the save list with details and the settings menu
/// (scripts/game_ui_assets.py): the Options menu frame, the Cross / Circle / Square / L1 / R1
/// prompts, the title and text fonts.
struct ChoiceStyleFiles {
    Gow3Png::Image images[7]; ///< frame, cross, circle, square, l1, r1, touchpad (APPLY & RESTART, with square)
    std::vector<unsigned char> fonts[2]; ///< title, text (TrueType / OpenType)
};
void SetChoiceStyle(ChoiceStyleFiles files);

} // namespace Gow3Overlay
