// SPDX-License-Identifier: GPL-2.0-or-later
#include "gow3_overlay.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdarg>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <SDL3/SDL.h>
#include "gow3_settings.h"
#include "gow3_toggles.h"
#include "gow3_orbs.h"
#include "gow3_actors.h"
#include "gow3_graphics.h"
#include "gow3_perf_stats.h"
#include "../../src/actor_hook.h"

extern Gow3ActorSlot gow3_actor_slots[2];
namespace Gow3Overlay {
extern std::atomic<bool> screenshot_requested;
}
extern "C" void runtime_restart(void); // src/probe.c: starts run.py again, ends this process
extern "C" void runtime_ue4_request_cvar(const char* name, const char* value); // live console variables
extern "C" void runtime_ue4_particle_single_pass(int on); // src/runtime_extra.c: r.GPUParticle.FixDeltaSeconds
u32 Gow3DisplayRefreshHz(); // gow3gpu.cpp: the display refresh rate
extern "C" void gow3gpu_choice_style(const char* dir); // gow3gpu.cpp: the game UI cache's look, loaded once
extern "C" const char* runtime_file_user_dir(void); // src/runtime_file.c: the saves folder
#include "common/elf_info.h"
#include "imgui.h"
#include "imgui_internal.h" // RegisterUserTexture (save pictures)
#include "imgui_impl_vulkan.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_frame_capture.h"
#include "video_core/renderer_vulkan/vk_ue4_motion.h"
#include "video_core/renderer_vulkan/vk_ue4_upscaler.h"

// DejaVu Sans (Cyrillic), embedded (third_party/fonts, Bitstream Vera license).
#ifdef _WIN32
asm(".section .rdata,\"dr\"\n"
    ".balign 16\n"
    ".global gow3_font_ttf\n"
    "gow3_font_ttf:\n"
    ".incbin \"" GOW3_FONT_PATH "\"\n"
    ".global gow3_font_ttf_end\n"
    "gow3_font_ttf_end:\n"
    ".text\n");
#else
asm(".section .rodata\n"
    ".balign 16\n"
    ".hidden gow3_font_ttf\n"
    ".global gow3_font_ttf\n"
    "gow3_font_ttf:\n"
    ".incbin \"" GOW3_FONT_PATH "\"\n"
    ".hidden gow3_font_ttf_end\n"
    ".global gow3_font_ttf_end\n"
    "gow3_font_ttf_end:\n"
    ".previous\n");
#endif
extern "C" const unsigned char gow3_font_ttf[];
extern "C" const unsigned char gow3_font_ttf_end[];

namespace Gow3Overlay {

namespace {

std::mutex imgui_mutex; // the ImGui context: window thread (input) and present thread
bool initialized = false;
std::atomic<bool> menu_open{false};
// The runtime asked for the menu (RequestOpen: the pause menu's PC Settings row, game thread); taken by Render.
std::atomic<bool> open_requested{false};
std::atomic<bool> loading{false};
std::atomic<u32> loading_done{0}, loading_total{0};
std::atomic<u32> background_done{0}, background_total{0};
bool dirty = false; // settings changed while open: saved on close
int requested_tab = -1; // switch on the next frame (L1/R1, Q/E, menu opened)
// The game's text dialog (SetTextEntry), guarded by imgui_mutex.
bool text_entry_active = false;
std::string text_entry_prompt, text_entry_text;
// The game's save list (sceSaveDataDialog): a choice made with the arrows/D-pad.
std::atomic<bool> choice_active{false};
std::string choice_title;
std::vector<std::string> choice_items;
std::vector<std::string> choice_details, choice_more; // per item; empty: the plain list
bool choice_expanded = false;                           // Square: `more` in place of `details`
// The game's menu look for the list with details and the settings menu (SetChoiceStyle): images
// become textures on the present thread (UpdateChoiceTextures); both fonts and all textures, or the
// plain look.
enum StyleImage { StyleFrame, StyleCross, StyleCircle, StyleSquare, StyleL1, StyleR1, StyleTouchpad, StyleImageCount };
struct ChoiceStyle {
    Gow3Png::Image pending[StyleImageCount];
    ImTextureData* textures[StyleImageCount] = {};
    ImFont* title = nullptr;
    ImFont* text = nullptr;
    bool Ready() const {
        return title && text && std::all_of(std::begin(textures), std::end(textures), [](auto* t) { return t; });
    }
} choice_style;
// MediEvil's Options menu colours (the Load list and the settings menu in its look).
constexpr ImU32 Cream = IM_COL32(240, 234, 220, 255), Gold = IM_COL32(245, 196, 30, 255),
                Green = IM_COL32(72, 226, 42, 255), Dim = IM_COL32(176, 166, 150, 255),
                White = IM_COL32(250, 248, 242, 255), FocusFill = IM_COL32(255, 214, 120, 26),
                PanelFill = IM_COL32(28, 19, 13, 255);
int choice_index = 0;
std::atomic<int> choice_result{-1}; // -1 open, -2 cancelled, else the chosen index
// Save pictures: decoded by BeginChoice, made textures on the present thread (null = none).
// Closed lists' textures wait in choice_retired until the backend has destroyed them.
std::vector<Gow3Png::Image> choice_pictures;
std::vector<ImTextureData*> choice_textures, choice_retired;
std::atomic<bool> choice_retiring{false};
float base_scale = 1.0f;

// Present rate for the FPS counter.
std::chrono::steady_clock::time_point last_present{};
float frame_ms_avg = 0.0f;

// MediEvil render scale (Display > Scaling and sharpening): each change reallocates every scene target
// in the game, so a new value is applied once the slider is let go and no step came for 0.3 s
// (or when the menu closes), not at every step of a drag.
bool render_scale_pending = false;
std::chrono::steady_clock::time_point render_scale_edit{};

void ApplyRenderScale(bool now) {
    if (!render_scale_pending ||
        (!now && std::chrono::steady_clock::now() - render_scale_edit < std::chrono::milliseconds(300))) {
        return;
    }
    render_scale_pending = false;
    Gow3Settings::RequestRenderScale(Gow3Settings::Get().render_scale);
}

bool menu_buttons_fresh = false; // the menu just opened: buttons held now wait for release (MenuButtons)

void SetOpen(bool value) {
    if (menu_open.exchange(value) == value) {
        return;
    }
    if (value) {
        requested_tab = Gow3Settings::Get().menu_tab;
        menu_buttons_fresh = true;
    }
    if (!value) {
        ApplyRenderScale(true);
    }
    if (!value && dirty) {
        dirty = false;
        Gow3Settings::Save();
    }
}

// The keyboard key that opens the menu: GOW3_MENU_KEY (an SDL key name, the launcher's Controls page),
// Insert without it or for a name SDL does not know.
SDL_Keycode MenuKey() {
    static const SDL_Keycode key = [] {
        const char* name = std::getenv("GOW3_MENU_KEY");
        const SDL_Keycode chosen = name && name[0] ? SDL_GetKeyFromName(name) : SDLK_UNKNOWN;
        return chosen != SDLK_UNKNOWN ? chosen : SDLK_INSERT;
    }();
    return key;
}

// The gamepad buttons that open the menu together (raw SDL buttons, not the game's binds): GOW3_MENU_PAD,
// SDL gamepad names joined by '+' ("back+start", "paddle1", "lefttrigger+rightstick"), all held. R3+L2
// without it or for a name SDL does not know (L3+R3 is a game action in God of War III).
static_assert(SDL_GAMEPAD_BUTTON_COUNT <= 32);
struct MenuPadCombo {
    uint32_t buttons = 0;  // bits by SDL_GamepadButton
    unsigned triggers = 0; // 1 left, 2 right
    std::string name;      // for the title and the log
    bool Held(uint32_t held, unsigned triggers_held) const {
        return (held & buttons) == buttons && (triggers_held & triggers) == triggers;
    }
};

const MenuPadCombo& MenuPad() {
    static const MenuPadCombo combo = [] {
        const MenuPadCombo r3_l2{1u << SDL_GAMEPAD_BUTTON_RIGHT_STICK, 1u, "R3+L2"};
        const char* env = std::getenv("GOW3_MENU_PAD");
        if (!env || !env[0]) {
            return r3_l2;
        }
        MenuPadCombo combo{0, 0, env};
        const std::string_view spec(env);
        for (size_t begin = 0; begin <= spec.size();) {
            const size_t end = std::min(spec.find('+', begin), spec.size());
            std::string name(spec.substr(begin, end - begin));
            name.erase(0, name.find_first_not_of(' '));
            name.erase(name.find_last_not_of(' ') + 1);
            const SDL_GamepadAxis axis = SDL_GetGamepadAxisFromString(name.c_str());
            const SDL_GamepadButton button = SDL_GetGamepadButtonFromString(name.c_str());
            if (axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER || axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) {
                combo.triggers |= axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER ? 1u : 2u;
            } else if (button != SDL_GAMEPAD_BUTTON_INVALID) {
                combo.buttons |= 1u << button;
            } else {
                std::printf("Overlay: GOW3_MENU_PAD=%s: '%s' is no gamepad button; R3+L2 opens the menu\n", env,
                            name.c_str());
                return r3_l2;
            }
            begin = end + 1;
        }
        return combo;
    }();
    return combo;
}

ImGuiKey KeyFromSdl(SDL_Keycode key) {
    switch (key) {
    case SDLK_TAB: return ImGuiKey_Tab;
    case SDLK_LEFT: return ImGuiKey_LeftArrow;
    case SDLK_RIGHT: return ImGuiKey_RightArrow;
    case SDLK_UP: return ImGuiKey_UpArrow;
    case SDLK_DOWN: return ImGuiKey_DownArrow;
    case SDLK_PAGEUP: return ImGuiKey_PageUp;
    case SDLK_PAGEDOWN: return ImGuiKey_PageDown;
    case SDLK_HOME: return ImGuiKey_Home;
    case SDLK_END: return ImGuiKey_End;
    case SDLK_DELETE: return ImGuiKey_Delete;
    case SDLK_BACKSPACE: return ImGuiKey_Backspace;
    case SDLK_SPACE: return ImGuiKey_Space;
    case SDLK_RETURN: return ImGuiKey_Enter;
    case SDLK_KP_ENTER: return ImGuiKey_KeypadEnter;
    case SDLK_ESCAPE: return ImGuiKey_Escape;
    case SDLK_Q: return ImGuiKey_Q;
    case SDLK_E: return ImGuiKey_E;
    case SDLK_LCTRL: return ImGuiKey_LeftCtrl;
    case SDLK_RCTRL: return ImGuiKey_RightCtrl;
    case SDLK_LSHIFT: return ImGuiKey_LeftShift;
    case SDLK_RSHIFT: return ImGuiKey_RightShift;
    case SDLK_LALT: return ImGuiKey_LeftAlt;
    case SDLK_RALT: return ImGuiKey_RightAlt;
    default: return ImGuiKey_None;
    }
}

float PixelDensity(SDL_WindowID id) {
    SDL_Window* window = SDL_GetWindowFromID(id);
    const float density = window ? SDL_GetWindowPixelDensity(window) : 1.0f;
    return density > 0.0f ? density : 1.0f;
}

// Marks the settings dirty when a widget changed them.
template <typename T>
void Store(std::atomic<T>& target, T value, bool changed) {
    if (changed) {
        target = value;
        dirty = true;
    }
}

// The help of the focused row (pad, keys or the mouse over it), shown in the footer.
const char* footer_help = nullptr;

constexpr int TabCount = 5;

// The PS4 buttons the menu reads (the runtime's bits, runtime_pad.c menu_buttons).
namespace Ps4 {
constexpr uint32_t Up = 0x10, Right = 0x20, Down = 0x40, Left = 0x80, L1 = 0x400, R1 = 0x800, Circle = 0x2000,
                   Cross = 0x4000, Square = 0x8000, Touchpad = 0x100000, Directions = Up | Right | Down | Left;
/// APPLY & RESTART: held together for RestartHold. The touchpad is the PS4 button the launcher calls Touchpad (its
/// controller button: Share / Create, SDL_GAMEPAD_BUTTON_BACK, and the DualSense touchpad); Tab by default on keys.
constexpr uint32_t RestartCombo = Square | Touchpad;
}

// The menu in the game's look (choice_style ready; StyledMenu) for this frame. menu_u is the frame's
// scale (drawn height / 870, as `u` in StyledChoiceBox); the sizes below are in the frame's pixels.
bool styled = false;
float menu_u = 1.0f;

// The styled body's items this frame (screen top and bottom of each row, heading, note, info row): the list
// shows whole items only, cut at the last one that fits (StyledMenu). The visible band of the last frame:
// rows outside it take no mouse input. The scroll bar's mouse area is hovered (rows do not take the hover).
std::vector<std::pair<float, float>> body_items;
float body_visible_top = -FLT_MAX, body_visible_bottom = FLT_MAX;
bool scrollbar_hovered = false;
bool in_styled_body = false;

void RecordItem(float top, float bottom) {
    if (in_styled_body) {
        body_items.emplace_back(top, bottom);
    }
}

// Whether a row lies wholly in the band the list showed last frame (always outside the styled body).
bool RowVisible(ImVec2 min, ImVec2 max) {
    return !in_styled_body || (min.y >= body_visible_top - 0.5f && max.y <= body_visible_bottom + 0.5f);
}

// The next row needs a new start to apply: a small "RESTART" tag left of its value (styled), or the word
// after its name (plain). Set by Restart(), taken by the next row.
bool row_restart = false;
void Restart(bool on = true) {
    row_restart = on;
}

// APPLY & RESTART (research/docs/plan-settings-menu.md section 10): Square and Touchpad (Share / Create) held
// together for 5 s, or a mouse button held on its prompt, while restart-only changes wait; letting go earlier
// cancels. It ends the game without saving its progress, so a single press never does it.
constexpr std::chrono::milliseconds RestartHold{5000};
std::chrono::steady_clock::time_point restart_hold_since{};
bool restart_holding = false;    // the combination (or the prompt) is held now
bool restart_hold_blocked = false; // held when the menu opened: wait for release first
bool restart_prompt_held = false;  // a mouse button holds the prompt (StyledMenu / PlainMenu, last frame)

bool RestartAvailable() {
    const char* command = std::getenv("GOW3_RESTART_COMMAND");
    return command && command[0];
}

void RestartNow() {
    restart_holding = false;
    // The new launch confirms the options once the game shows; run.py restores the
    // previous ones if it never does (restart_unconfirmed).
    Gow3Settings::Get().restart_unconfirmed = true;
    Gow3Settings::Save();
    runtime_restart();
}

// How far the hold has come, 0 to 1 (the prompt's fill).
float RestartProgress() {
    if (!restart_holding) {
        return 0.0f;
    }
    const auto held = std::chrono::steady_clock::now() - restart_hold_since;
    return std::clamp(std::chrono::duration<float>(held).count() /
                          std::chrono::duration<float>(RestartHold).count(), 0.0f, 1.0f);
}

// Each presented frame (MenuButtons): the combination held (`buttons`) or the prompt held starts and keeps the
// hold; at 5 s the game restarts.
void RestartHoldStep(uint32_t buttons) {
    const bool combo = (buttons & Ps4::RestartCombo) == Ps4::RestartCombo;
    if (!combo) {
        restart_hold_blocked = false;
    }
    const bool held = ((combo && !restart_hold_blocked) || restart_prompt_held) && RestartAvailable() &&
                      Gow3Settings::RestartChanges() > 0;
    if (!held) {
        restart_holding = false;
        return;
    }
    if (!restart_holding) {
        restart_holding = true;
        restart_hold_since = std::chrono::steady_clock::now();
    }
    if (RestartProgress() >= 1.0f) {
        RestartNow();
    }
}

// Pad and keyboard navigation of the menu (research/docs/plan-settings-menu.md): the rows a tab draws are
// numbered in order; the focused one is highlighted, shows its help, takes left / right (changes its
// value) and Cross (changes it, or presses it) and is scrolled into view when the pad or keys moved the
// focus. The mouse focuses the row it moves over. ImGui's own navigation is off (Init).
struct MenuNav {
    int focus[TabCount] = {}; // the focused row of each tab, kept while the menu is closed
    int tab = 0;              // the tab being drawn
    int rows = 0;             // rows drawn so far this frame
    int last_rows = 0;        // rows drawn in the last frame
    int body_rows = 0;        // of those, the tab's own (scrolled)
    int move = 0, step = 0;   // up / down and left / right since the last frame
    bool press = false;       // Cross since the last frame
    bool reveal = false;      // scroll the focused row into view
    bool in_body = false;     // drawing the scrolled body
} nav;

int& Focus() {
    return nav.focus[nav.tab];
}

// Whether the next row drawn has the focus.
bool NextRowFocused() {
    return nav.rows == Focus();
}

// What a press of the menu's buttons does (MenuButtons; SDL events when the runtime gave no input).
void MenuPress(uint32_t pressed) {
    if (!pressed) {
        return;
    }
    nav.move += (pressed & Ps4::Up) ? -1 : (pressed & Ps4::Down) ? 1 : 0;
    nav.step += (pressed & Ps4::Left) ? -1 : (pressed & Ps4::Right) ? 1 : 0;
    nav.press |= (pressed & Ps4::Cross) != 0;
    if (pressed & (Ps4::L1 | Ps4::R1)) {
        const int from = requested_tab >= 0 ? requested_tab : Gow3Settings::Get().menu_tab.load();
        requested_tab = (from + ((pressed & Ps4::R1) ? 1 : TabCount - 1)) % TabCount;
    }
    if (pressed & Ps4::Circle) {
        SetOpen(false);
    }
}

// The runtime's buttons for the menu (runtime_pad.c menu_buttons: pad, keys and mouse binds as mapped,
// the arrows / Enter / Esc fallbacks, the left stick as a D-pad), read every presented frame while it is open.
std::atomic<uint32_t (*)()> menu_input{nullptr};

// imgui_mutex held, before NewFrame. Presses act once; a held direction repeats after 400 ms every
// 120 ms, left / right every 40 ms once held 1.2 s (long sliders). Buttons held when the menu opened
// (the combo that opened it) wait for release. Nothing while a number box is typed in.
void MenuButtons(uint32_t buttons) {
    using Clock = std::chrono::steady_clock;
    static uint32_t previous = 0, repeating = 0;
    static Clock::time_point held_since{}, repeat_at{};
    if (!menu_open) {
        return;
    }
    if (menu_buttons_fresh) {
        menu_buttons_fresh = false;
        restart_holding = restart_prompt_held = false;
        restart_hold_blocked = (buttons & Ps4::RestartCombo) == Ps4::RestartCombo;
        previous = buttons;
        repeating = 0;
        nav.move = nav.step = 0;
        nav.press = false;
        return;
    }
    const uint32_t pressed = buttons & ~previous;
    previous = buttons;
    if (ImGui::GetIO().WantTextInput) {
        repeating = 0;
        restart_holding = false;
        return;
    }
    RestartHoldStep(buttons);
    const auto now = Clock::now();
    uint32_t fire = 0;
    if (const uint32_t directions = pressed & Ps4::Directions) {
        repeating = directions & (~directions + 1); // one direction: the lowest bit
        fire = repeating;
        held_since = now;
        repeat_at = now + std::chrono::milliseconds(400);
    } else if (repeating && (buttons & repeating) && now >= repeat_at) {
        fire = repeating;
        const bool fast = (repeating & (Ps4::Left | Ps4::Right)) && now - held_since > std::chrono::milliseconds(1200);
        repeat_at = now + std::chrono::milliseconds(fast ? 40 : 120);
    } else if (!(buttons & repeating)) {
        repeating = 0;
    }
    MenuPress(fire | (pressed & ~Ps4::Directions));
}

// Before a tab's rows: counts restart and a pending up / down moves the focus (wrapping).
void NavStartTab(int tab) {
    nav.tab = tab;
    nav.rows = 0;
    if (nav.move && nav.last_rows > 0) {
        Focus() = ((Focus() + nav.move) % nav.last_rows + nav.last_rows) % nav.last_rows;
        nav.reveal = true;
    }
    nav.move = 0;
}

// After the menu: the focus stays on a row that exists; what no row took is dropped.
void NavEndFrame() {
    nav.last_rows = nav.rows;
    Focus() = nav.rows > 0 ? std::clamp(Focus(), 0, nav.rows - 1) : 0;
    nav.move = nav.step = 0;
    nav.press = nav.reveal = false;
}

// What the pad and keys did to a row.
struct RowNav {
    int step = 0;       // left / right
    bool press = false; // Cross
};

bool LastItemEnabled() {
    return !(ImGui::GetItemFlags() & ImGuiItemFlags_Disabled);
}

// Ends a focusable row whose rectangle is min..max: the mouse moving over it focuses it; the focused
// row shows its help, is scrolled into view after a pad or key move (the body's first row to the top,
// its last to the bottom, so notes under it show) and gets the pad's change if it is enabled.
RowNav NavRow(ImVec2 min, ImVec2 max, const char* help, bool enabled) {
    const int index = nav.rows++;
    RecordItem(min.y, max.y);
    const ImGuiIO& io = ImGui::GetIO();
    if ((io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f) && ImGui::IsMouseHoveringRect(min, max, false) &&
        ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) && RowVisible(min, max) &&
        !scrollbar_hovered) {
        Focus() = index;
    }
    RowNav result;
    if (index != Focus()) {
        return result;
    }
    footer_help = help;
    if (nav.reveal && nav.in_body) {
        nav.reveal = false;
        if (index == 0) {
            ImGui::SetScrollY(0.0f);
        } else if (index == nav.body_rows - 1) {
            ImGui::SetScrollY(ImGui::GetScrollMaxY());
        } else {
            const float margin = ImGui::GetFrameHeight();
            ImGui::ScrollToRect(ImGui::GetCurrentWindow(), ImRect(min.x, min.y - margin, max.x, max.y + margin),
                                ImGuiScrollFlags_KeepVisibleEdgeY);
        }
    }
    if (enabled) {
        result.step = std::exchange(nav.step, 0);
        result.press = std::exchange(nav.press, false);
    }
    return result;
}

// Row sizes in frame pixels (2026-11-13: smaller, more rows in view; research/docs/plan-settings-menu.md 9).
constexpr float RowHeight = 38.0f, InfoHeight = 30.0f, RowText = 26.0f, InfoText = 23.0f, NoteText = 22.0f,
                SectionText = 26.0f, ValueColumn = 0.55f, RightMargin = 18.0f;

float Px(float frame_px) {
    return frame_px * menu_u;
}

// The game's text font size for `frame_px`, in whole pixels: ImGui 1.92 rasterises each size once.
float TextPx(float frame_px) {
    return std::max(std::round(Px(frame_px)), 8.0f);
}

// A colour with the style's alpha (dimmed inside BeginDisabled).
ImU32 Col(ImU32 color) {
    return ImGui::GetColorU32(color);
}

// Two-column rows: the name on the left, the control on the right (the game's look draws its own).
bool BeginRows(const char* id) {
    if (styled) {
        return true;
    }
    if (!ImGui::BeginTable(id, 2, ImGuiTableFlags_SizingStretchProp)) {
        return false;
    }
    ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthStretch, 0.52f);
    ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch, 0.48f);
    return true;
}

void EndRows() {
    if (!styled) {
        ImGui::EndTable();
    }
}

// A section heading; in the game's look "-NAME-" in green, centred (as the Load list's "-LEVELS-").
void Section(const char* name) {
    if (!styled) {
        ImGui::SeparatorText(name);
        return;
    }
    std::string heading = std::string("-") + name + "-";
    std::transform(heading.begin(), heading.end(), heading.begin(),
                   [](unsigned char c) { return char(std::toupper(c)); });
    const float size = TextPx(SectionText), width = ImGui::GetContentRegionAvail().x;
    const float top = ImGui::GetCursorScreenPos().y;
    ImGui::Dummy(ImVec2(0.0f, Px(8.0f)));
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const float w = choice_style.text->CalcTextSizeA(size, FLT_MAX, 0.0f, heading.c_str()).x;
    ImGui::GetWindowDrawList()->AddText(choice_style.text, size, ImVec2(at.x + (width - w) * 0.5f, at.y), Col(Green),
                                        heading.c_str());
    ImGui::Dummy(ImVec2(width, size * 1.2f));
    RecordItem(top, ImGui::GetItemRectMax().y);
}

// A wrapped note between rows (Dim), e.g. why a setting waits for the next start.
void Note(const char* format, ...) IM_FMTARGS(1);
void Note(const char* format, ...) {
    char text[512];
    va_list args;
    va_start(args, format);
    std::vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("%s", text);
    ImGui::PopTextWrapPos();
    RecordItem(ImGui::GetItemRectMin().y, ImGui::GetItemRectMax().y);
}

// The RESTART tag of a styled row: small, dim, letter-spaced, in a rounded outline whose right edge is just
// left of the value column. Returns its left edge (the name is clipped there).
float RestartTag(float value_x, float mid) {
    ImDrawList* draw = ImGui::GetWindowDrawList();
    constexpr const char* Text = "RESTART";
    const float size = TextPx(15.0f), spacing = Px(1.5f), pad = Px(7.0f), h = Px(22.0f);
    float text_w = spacing * float(std::strlen(Text) - 1);
    for (const char* c = Text; *c; ++c) {
        text_w += choice_style.text->CalcTextSizeA(size, FLT_MAX, 0.0f, c, c + 1).x;
    }
    const float x1 = value_x - Px(14.0f), x0 = x1 - text_w - 2.0f * pad;
    draw->AddRect(ImVec2(x0, mid - h * 0.5f), ImVec2(x1, mid + h * 0.5f), Col(IM_COL32(176, 166, 150, 150)), h * 0.5f,
                  0, std::max(Px(1.2f), 1.0f));
    float x = x0 + pad;
    for (const char* c = Text; *c; ++c) {
        draw->AddText(choice_style.text, size, ImVec2(x, mid - size * 0.55f), Col(Dim), c, c + 1);
        x += choice_style.text->CalcTextSizeA(size, FLT_MAX, 0.0f, c, c + 1).x + spacing;
    }
    return x0;
}

// A row in the game's look: the focus fill and the name (Gold when focused) are drawn first; controls
// with their own mouse handling (a slider's track, a number box) come next and take the mouse first;
// StyledRowHit then adds the row's hit box. The value goes from value_x to right.
struct RowArea {
    ImVec2 min, max;
    float value_x = 0.0f, right = 0.0f, mid = 0.0f;
    bool focused = false;
    bool clicked = false; // a click on the row outside its controls
};

RowArea BeginStyledRow(const char* label, float width = 0.0f) {
    RowArea r;
    r.focused = NextRowFocused();
    width = width > 0.0f ? width : ImGui::GetContentRegionAvail().x;
    r.min = ImGui::GetCursorScreenPos();
    r.max = ImVec2(r.min.x + width, r.min.y + Px(RowHeight));
    r.value_x = r.min.x + width * ValueColumn;
    r.right = r.max.x - Px(RightMargin);
    r.mid = (r.min.y + r.max.y) * 0.5f;
    ImGui::PushID(label);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    if (r.focused) {
        draw->AddRectFilled(ImVec2(r.min.x, r.min.y + Px(1.0f)), ImVec2(r.max.x - Px(6.0f), r.max.y - Px(1.0f)),
                            Col(FocusFill), Px(10.0f));
    }
    const float size = TextPx(RowText);
    const float name_end = std::exchange(row_restart, false) ? RestartTag(r.value_x, r.mid) - Px(6.0f)
                                                              : r.value_x - Px(10.0f);
    const ImVec4 clip(r.min.x, r.min.y, name_end, r.max.y);
    draw->AddText(choice_style.text, size, ImVec2(r.min.x + Px(12.0f), r.mid - size * 0.55f),
                  Col(r.focused ? Gold : Cream), label, nullptr, 0.0f, &clip);
    return r;
}

void StyledRowHit(RowArea& r) {
    ImGui::SetCursorScreenPos(r.min);
    const ImVec2 size(r.max.x - r.min.x, r.max.y - r.min.y);
    if (RowVisible(r.min, r.max)) {
        r.clicked = ImGui::InvisibleButton("##row", size);
    } else { // cut off at the list's edge: no clicks on the hidden part
        ImGui::Dummy(size);
        r.clicked = false;
    }
}

// The direction a click on a choice row means: the left arrow's quarter of the value column goes back.
int ClickStep(const RowArea& r) {
    return ImGui::GetIO().MousePos.x < r.value_x + (r.right - r.value_x) * 0.25f ? -1 : 1;
}

// A value between arrows ("<  value  >"); a value too wide is drawn smaller (to 60%), then clipped.
void DrawChoice(const RowArea& r, const char* text, bool arrows = true) {
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const float arrow = Px(9.0f), x0 = r.value_x, x1 = r.right, mid = r.mid;
    const float room = x1 - x0 - 5.0f * arrow;
    float size = TextPx(RowText);
    const float wide = choice_style.text->CalcTextSizeA(size, FLT_MAX, 0.0f, text).x;
    if (wide > room) {
        size = std::max(std::floor(size * room / wide), std::round(size * 0.6f));
    }
    const float w = choice_style.text->CalcTextSizeA(size, FLT_MAX, 0.0f, text).x;
    const ImVec4 clip(x0 + 2.0f * arrow, r.min.y, x1 - 2.0f * arrow, r.max.y);
    draw->AddText(choice_style.text, size, ImVec2(std::max((x0 + x1 - w) * 0.5f, clip.x), mid - size * 0.55f),
                  Col(r.focused ? Gold : Cream), text, nullptr, 0.0f, &clip);
    if (arrows) {
        const ImU32 color = Col(r.focused ? Gold : Dim);
        draw->AddTriangleFilled(ImVec2(x0, mid), ImVec2(x0 + arrow, mid - arrow), ImVec2(x0 + arrow, mid + arrow), color);
        draw->AddTriangleFilled(ImVec2(x1, mid), ImVec2(x1 - arrow, mid + arrow), ImVec2(x1 - arrow, mid - arrow), color);
    }
}

// A slider in the value column: a track filled to `fraction`, a knob, `text` after it; `room` frame pixels
// stay free on the right (text, number box). The track's hit box is submitted here (before
// StyledRowHit): returns the fraction the mouse sets while it holds the track, else -1.
float StyledSlider(const RowArea& r, float fraction, const char* text, bool* dragging, float room = 130.0f) {
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const float x0 = r.value_x + Px(12.0f), x1 = r.right - Px(room), mid = r.mid, thick = Px(6.0f);
    const float knob = x0 + (x1 - x0) * std::clamp(fraction, 0.0f, 1.0f);
    const ImU32 color = Col(r.focused ? Gold : Cream);
    draw->AddRectFilled(ImVec2(x0, mid - thick * 0.5f), ImVec2(x1, mid + thick * 0.5f), Col(IM_COL32(86, 70, 54, 255)),
                        thick * 0.5f);
    draw->AddRectFilled(ImVec2(x0, mid - thick * 0.5f), ImVec2(knob, mid + thick * 0.5f), color, thick * 0.5f);
    draw->AddCircleFilled(ImVec2(knob, mid), Px(10.0f), color);
    if (text) {
        const float size = TextPx(RowText);
        draw->AddText(choice_style.text, size, ImVec2(x1 + Px(18.0f), mid - size * 0.55f), color, text);
    }
    ImGui::SetCursorScreenPos(ImVec2(x0 - Px(12.0f), r.min.y));
    const ImVec2 track(x1 - x0 + Px(24.0f), r.max.y - r.min.y);
    bool held = false;
    if (RowVisible(r.min, r.max)) {
        ImGui::InvisibleButton("##track", track);
        held = ImGui::IsItemActive();
    } else {
        ImGui::Dummy(track);
    }
    if (dragging) {
        *dragging = held;
    }
    return held ? std::clamp((ImGui::GetIO().MousePos.x - x0) / (x1 - x0), 0.0f, 1.0f) : -1.0f;
}

float row_top = 0.0f; // the plain look's row being drawn: its top (RowEnd)

// A row of the plain look: the name, then the control (PushID; PopID after it). A focusable row with the
// focus gets a gold background.
void Row(const char* label, bool focusable = true) {
    ImGui::TableNextRow();
    if (focusable && NextRowFocused()) {
        ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, IM_COL32(255, 214, 120, 48));
    }
    ImGui::TableNextColumn();
    row_top = ImGui::GetCursorScreenPos().y;
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    if (std::exchange(row_restart, false)) {
        ImGui::SameLine();
        ImGui::TextDisabled("RESTART");
    }
    ImGui::TableNextColumn();
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::PushID(label);
}

// Ends a row's control (the last item) as a focusable row across the window; enabled unless the last
// item is disabled (or as `enabled` says).
RowNav RowEnd(const char* help, int enabled = -1) {
    const float left = ImGui::GetWindowPos().x;
    return NavRow(ImVec2(left, row_top), ImVec2(left + ImGui::GetWindowWidth(), ImGui::GetItemRectMax().y), help,
                  enabled < 0 ? LastItemEnabled() : enabled != 0);
}

// The same for the game's look.
RowNav StyledRowEnd(const RowArea& r, const char* help, int enabled = -1) {
    return NavRow(r.min, r.max, help, enabled < 0 ? LastItemEnabled() : enabled != 0);
}

// On / off: a checkbox, or "<  On  >". Pad: left off, right on, Cross flips; a click on the row flips.
// Returns whether `on` changed.
bool ToggleRow(const char* label, bool& on, const char* help) {
    bool changed = false;
    RowNav in;
    if (styled) {
        RowArea r = BeginStyledRow(label);
        StyledRowHit(r);
        if (r.clicked) {
            on = !on;
            changed = true;
        }
        DrawChoice(r, on ? "On" : "Off");
        in = StyledRowEnd(r, help);
    } else {
        Row(label);
        changed = ImGui::Checkbox("##value", &on);
        in = RowEnd(help);
    }
    if (in.press || in.step) {
        const bool next = in.press ? !on : in.step > 0;
        changed |= next != on;
        on = next;
    }
    ImGui::PopID();
    return changed;
}

void CheckRow(const char* label, std::atomic<bool>& value, const char* help = nullptr) {
    bool v = value;
    const bool changed = ToggleRow(label, v, help);
    Store(value, v, changed);
}

// A choice among `count` labels: `current` -1 (a value the menu does not list) shows `unlisted` until
// changed. Returns the chosen index, -1 when unchanged. Pad: left / right step (stopping at the ends),
// Cross the next one (wrapping). The game's look: a click on the left arrow goes back, elsewhere on.
int ChoiceIndex(const char* label, int current, const char* const* labels, int count, const char* help,
                const char* unlisted = "(custom)") {
    int chosen = current;
    const char* shown = current >= 0 && current < count ? labels[current] : unlisted;
    RowNav in;
    if (styled) {
        RowArea r = BeginStyledRow(label);
        StyledRowHit(r);
        if (r.clicked && count > 0) {
            chosen = current < 0 ? 0 : (current + ClickStep(r) + count) % count;
        }
        DrawChoice(r, shown);
        in = StyledRowEnd(r, help);
    } else {
        Row(label);
        if (ImGui::BeginCombo("##value", shown)) {
            for (int i = 0; i < count; ++i) {
                if (ImGui::Selectable(labels[i], i == current)) {
                    chosen = i;
                }
            }
            ImGui::EndCombo();
        }
        in = RowEnd(help);
    }
    if (in.step && count > 0) {
        chosen = current < 0 ? (in.step > 0 ? 0 : count - 1) : std::clamp(current + in.step, 0, count - 1);
    }
    if (in.press && count > 0) {
        chosen = current < 0 ? 0 : (current + 1) % count;
    }
    ImGui::PopID();
    return chosen != current ? chosen : -1;
}

// A choice storing an index, or a value from `values`.
void ChoiceRow(const char* label, std::atomic<int>& target, const char* const* labels, const int* values,
               int count, const char* help = nullptr) {
    int current = -1;
    for (int i = 0; i < count; ++i) {
        if ((values ? values[i] : i) == target.load()) current = i;
    }
    if (const int chosen = ChoiceIndex(label, current, labels, count, help); chosen >= 0) {
        Store(target, values ? values[chosen] : chosen, true);
    }
}

// Choice over a plain int index (-1: a value the menu does not list, shown until changed).
void IndexChoiceRow(const char* label, int& value, const char* const* labels, int count, const char* help,
                    const char* unlisted = "(custom)") {
    if (const int chosen = ChoiceIndex(label, value, labels, count, help, unlisted); chosen >= 0) {
        value = chosen;
        dirty = true;
    }
}

// An int slider; `plus_minus` adds - / + buttons in the plain look (render scale). Returns the new
// value. Pad: left / right by `step`. `dragging`: the mouse holds the slider.
int IntSliderRow(const char* label, int value, int lo, int hi, int step, const char* format, const char* help,
                 bool* dragging = nullptr, bool plus_minus = false) {
    int v = value;
    RowNav in;
    if (styled) {
        RowArea r = BeginStyledRow(label);
        char text[32];
        std::snprintf(text, sizeof(text), format, value);
        const float set = StyledSlider(r, hi > lo ? float(value - lo) / float(hi - lo) : 0.0f, text, dragging);
        if (set >= 0.0f) {
            v = lo + int(std::round(set * float(hi - lo)));
        }
        StyledRowHit(r);
        in = StyledRowEnd(r, help);
    } else {
        Row(label);
        const ImGuiStyle& style = ImGui::GetStyle();
        const float button = ImGui::GetFrameHeight();
        if (plus_minus) {
            ImGui::SetNextItemWidth(-(2.0f * (button + style.ItemSpacing.x)));
        }
        ImGui::SliderInt("##value", &v, lo, hi, format, ImGuiSliderFlags_AlwaysClamp);
        if (dragging) {
            *dragging = ImGui::IsItemActive();
        }
        if (plus_minus) {
            ImGui::SameLine();
            if (ImGui::Button("-", ImVec2(button, 0.0f))) {
                v = value - 1;
            }
            ImGui::SameLine();
            if (ImGui::Button("+", ImVec2(button, 0.0f))) {
                v = value + 1;
            }
        }
        in = RowEnd(help);
    }
    v += in.step * step;
    ImGui::PopID();
    return std::clamp(v, lo, hi);
}

void SliderRow(const char* label, std::atomic<int>& value, int lo, int hi, const char* format, const char* help,
               int step = 1) {
    const int v = value;
    const int next = IntSliderRow(label, v, lo, hi, step, format, help);
    Store(value, next, next != v);
}

// A 0.1x to 100x multiplier with a 1x button (the game's look: "<  1.00x  >", a click steps by 1.25).
// Pad: left / right divide / multiply by 1.1, Cross 1x.
void MultiplierRow(const char* label, std::atomic<float>& value, const char* help) {
    bool reset = false;
    RowNav in;
    if (styled) {
        RowArea r = BeginStyledRow(label);
        StyledRowHit(r);
        if (r.clicked) {
            value = Gow3Orbs::ClampMultiplier(value * (ClickStep(r) > 0 ? 1.25f : 0.8f));
            dirty = true;
        }
        char text[32];
        std::snprintf(text, sizeof(text), "%.2fx", value.load());
        DrawChoice(r, text);
        in = StyledRowEnd(r, help);
    } else {
        Row(label);
        const ImGuiStyle& style = ImGui::GetStyle();
        ImGui::SetNextItemWidth(-(ImGui::CalcTextSize("1x").x + style.FramePadding.x * 2 + style.ItemSpacing.x));
        float v = value;
        if (ImGui::SliderFloat("##value", &v, 0.1f, 100.0f, "%.2fx",
                               ImGuiSliderFlags_Logarithmic | ImGuiSliderFlags_AlwaysClamp)) {
            value = Gow3Orbs::ClampMultiplier(v);
            dirty = true;
        }
        ImGui::SameLine();
        reset = ImGui::SmallButton("1x");
        in = RowEnd(help);
    }
    if (in.step) {
        value = Gow3Orbs::ClampMultiplier(std::round(value * (in.step > 0 ? 1.1f : 1.0f / 1.1f) * 100.0f) / 100.0f);
        dirty = true;
    }
    if (reset || in.press) {
        value = 1.0f;
        dirty = true;
    }
    ImGui::PopID();
}

// A float slider with a number box beside it for typing an exact value (Ctrl+click types into the slider
// too, plain look). `clamp` keeps the stored value in range and on its steps. Pad: left / right by `step`.
void NumberRow(const char* label, std::atomic<float>& value, float lo, float hi, float step, const char* format,
               float (*clamp)(float), const char* help) {
    RowNav in;
    if (styled) {
        RowArea r = BeginStyledRow(label);
        constexpr float Box = 110.0f; // frame pixels
        const float set = StyledSlider(r, (value - lo) / (hi - lo), nullptr, nullptr, Box + 24.0f);
        if (set >= 0.0f) {
            value = clamp(lo + set * (hi - lo));
            dirty = true;
        }
        ImGui::SetCursorScreenPos(ImVec2(r.right - Px(Box), r.mid - ImGui::GetFrameHeight() * 0.5f));
        ImGui::SetNextItemWidth(Px(Box));
        float v = value;
        if (ImGui::InputFloat("##number", &v, 0.0f, 0.0f, format)) {
            value = clamp(std::round(v / step) * step);
            dirty = true;
        }
        StyledRowHit(r);
        in = StyledRowEnd(r, help);
    } else {
        Row(label);
        const ImGuiStyle& style = ImGui::GetStyle();
        const float box = ImGui::CalcTextSize("0.0000").x + style.FramePadding.x * 2;
        ImGui::SetNextItemWidth(-(box + style.ItemSpacing.x));
        float v = value;
        if (ImGui::SliderFloat("##slider", &v, lo, hi, format, ImGuiSliderFlags_AlwaysClamp)) {
            value = clamp(v);
            dirty = true;
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(box);
        v = value;
        if (ImGui::InputFloat("##number", &v, 0.0f, 0.0f, format)) {
            value = clamp(std::round(v / step) * step);
            dirty = true;
        }
        in = RowEnd(help);
    }
    if (in.step) {
        value = clamp(std::round((value + in.step * step) / step) * step);
        dirty = true;
    }
    ImGui::PopID();
}

// A row of buttons (the game resolutions): returns the one pressed, or -1. `enabled`: per button. Pad:
// left / right pick one (outlined, or Gold and underlined in the game's look), Cross presses it.
int ButtonsRow(const char* label, const char* const* labels, int count, const bool* enabled, const char* help) {
    ImGuiStorage* storage = ImGui::GetStateStorage();
    const ImGuiID pick_id = ImGui::GetID(label);
    int pick = std::clamp(storage->GetInt(pick_id, 0), 0, count - 1);
    int pressed = -1;
    RowNav in;
    if (styled) {
        RowArea r = BeginStyledRow(label);
        StyledRowHit(r);
        const float cell = (r.right - r.value_x) / float(count), size = TextPx(RowText);
        if (r.clicked) {
            const int i = int((ImGui::GetIO().MousePos.x - r.value_x) / cell);
            if (i >= 0 && i < count && enabled[i]) {
                pressed = pick = i;
            }
        }
        ImDrawList* draw = ImGui::GetWindowDrawList();
        for (int i = 0; i < count; ++i) {
            const float w = choice_style.text->CalcTextSizeA(size, FLT_MAX, 0.0f, labels[i]).x;
            const ImVec2 at(r.value_x + cell * (float(i) + 0.5f) - w * 0.5f, r.mid - size * 0.55f);
            const bool picked = r.focused && i == pick;
            const ImU32 color = !enabled[i] ? Col(IM_COL32(176, 166, 150, 110)) : Col(picked ? Gold : Cream);
            draw->AddText(choice_style.text, size, at, color, labels[i]);
            if (picked) {
                draw->AddLine(ImVec2(at.x, at.y + size * 1.05f), ImVec2(at.x + w, at.y + size * 1.05f), color, Px(2.0f));
            }
        }
        in = StyledRowEnd(r, help, 1);
    } else {
        const bool focused = NextRowFocused();
        Row(label);
        for (int i = 0; i < count; ++i) {
            if (i) {
                ImGui::SameLine();
            }
            ImGui::BeginDisabled(!enabled[i]);
            if (ImGui::Button(labels[i])) {
                pressed = i;
            }
            ImGui::EndDisabled();
            if (focused && i == pick) {
                ImGui::GetWindowDrawList()->AddRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(),
                                                    IM_COL32(245, 196, 30, 255), ImGui::GetStyle().FrameRounding, 0,
                                                    2.0f * base_scale);
            }
        }
        in = RowEnd(help, 1);
    }
    pick = std::clamp(pick + in.step, 0, count - 1);
    storage->SetInt(pick_id, pick);
    if (in.press && enabled[pick]) {
        pressed = pick;
    }
    ImGui::PopID();
    return pressed;
}

// A row to read, not change (not focusable): the game's look as the Load list's details, the name in
// Cream and the value in Gold.
void InfoRow(const char* label, const char* text) {
    if (styled) {
        const float width = ImGui::GetContentRegionAvail().x, size = TextPx(InfoText);
        const ImVec2 at = ImGui::GetCursorScreenPos();
        const float mid = at.y + Px(InfoHeight) * 0.5f, value_x = at.x + width * ValueColumn;
        ImDrawList* draw = ImGui::GetWindowDrawList();
        const ImVec4 clip(at.x, at.y, value_x - Px(10.0f), at.y + Px(InfoHeight));
        draw->AddText(choice_style.text, size, ImVec2(at.x + Px(12.0f), mid - size * 0.55f), Col(Cream), label,
                      nullptr, 0.0f, &clip);
        const float w = choice_style.text->CalcTextSizeA(size, FLT_MAX, 0.0f, text).x;
        const float right = at.x + width - Px(RightMargin);
        draw->AddText(choice_style.text, size, ImVec2(std::max((value_x + right - w) * 0.5f, value_x), mid - size * 0.55f),
                      Col(Gold), text);
        ImGui::Dummy(ImVec2(width, Px(InfoHeight)));
        RecordItem(at.y, at.y + Px(InfoHeight));
        return;
    }
    Row(label, false);
    ImGui::TextDisabled("%s", text);
    ImGui::PopID();
}

const char* UpscalerLabel(int upscaler) {
    switch (upscaler) {
    case Gow3Settings::UpscalerFsr3: return "FSR 3.1";
    case Gow3Settings::UpscalerFsr4: return "FSR 4";
    case Gow3Settings::UpscalerFsr411: return "FSR 4.1.1";
    case Gow3Settings::UpscalerTaa: return "TAA";
    case Gow3Settings::UpscalerDlss: return "DLSS";
    default: return "";
    }
}

const Vulkan::Instance* overlay_instance = nullptr; // for the overlay's measurements

// The performance overlay's text with the current settings (also the menu's preview).
std::string OverlayText() {
    namespace P = Gow3PerfStats;
    const auto& s = Gow3Settings::Get();
    const unsigned items = s.overlay_items;
    if ((items & (P::Gpu | P::Cpu | P::Ram | P::Vram)) && overlay_instance) {
        P::Start(*overlay_instance);
    }
    P::Sample sample = P::Latest();
    sample.fps = frame_ms_avg > 0.0f ? 1000.0f / frame_ms_avg : 0.0f;
    sample.frame_ms = frame_ms_avg;
    sample.upscaler = UpscalerLabel(s.upscaler);
    return P::Format(sample, items, s.overlay_layout == 0);
}

constexpr const char* Unsupported = "Unavailable: game version or code signature mismatch.";

// The cheats, multipliers and game patches hook God of War III's code (CUSA01623 only).
bool IsGow3() {
    return Common::ElfInfo::Instance().GameSerial().starts_with("CUSA01623");
}

[[maybe_unused]] void GameTab() { // no tab for now (2026-11-13): God of War III only
    auto& s = Gow3Settings::Get();
    if (!IsGow3()) {
        Section("Game");
        const std::string_view serial = Common::ElfInfo::Instance().GameSerial();
        if (serial.starts_with("CUSA11227") || serial.starts_with("CUSA12982")) {
            // Issue #37: players read the generic note as "MediEvil has no cheats".
            ImGui::TextWrapped("MediEvil's cheats are in the launcher: Game page, Cheats. The developer "
                               "cheat menu (level select, weapons, gold, life bottles and more) opens "
                               "with R3 during play; Unlimited ammo keeps every weapon full. Both apply "
                               "when the game starts from the launcher.");
            return;
        }
        ImGui::TextWrapped("%s runs without code patches. The cheats and multipliers of this menu are "
                           "written for God of War III and stay hidden here; image, performance and "
                           "overlay options work in every game.",
                           std::string(Common::ElfInfo::Instance().Title()).c_str());
        return;
    }
    Section("Cheats");
    if (BeginRows("##cheats")) {
        static const char* const names[] = {"Infinite health", "Infinite magic", "Infinite item meter",
                                            "Infinite Rage of Sparta", "Max red orbs",
                                            "Unlock Very Hard", "Infinite Double Jumps"};
        static const char* const helps[] = {
            "Health stays full. Only Kratos is protected. Turning it off does not undo past refills.",
            "Magic stays full.", "The item meter (bow, Helios' head, boots) stays full.",
            "Rage of Sparta stays full.", "Red orbs stay at the maximum.",
            "Enable before opening New Game. Does not change an existing save difficulty.",
            "Repeat air jumps without landing. Turn off for normal movement."};
        for (unsigned cheat = 0; cheat < GOW3_CHEAT_COUNT; ++cheat) {
            const bool available = (s.cheats_supported.load() & (1u << cheat)) != 0;
            ImGui::BeginDisabled(!available);
            CheckRow(names[cheat], s.cheats[cheat], available ? helps[cheat] : Unsupported);
            ImGui::EndDisabled();
        }
        EndRows();
    }

    Section("Multipliers");
    if (BeginRows("##multipliers")) {
        static const char* const reset[] = {"Reset all to 1x"};
        static const bool reset_enabled[] = {true};
        if (ButtonsRow("All multipliers", reset, 1, reset_enabled,
                       "Every multiplier back to 1x (the game's normal values).") == 0) {
            for (auto* m : {&s.red_orb_multiplier, &s.green_orb_multiplier, &s.blue_orb_multiplier,
                            &s.gold_orb_multiplier, &s.damage_dealt, &s.damage_taken}) {
                *m = 1.0f;
            }
            dirty = true;
        }
        const bool max_orbs = (s.cheats_supported.load() & (1u << 4)) && s.cheats[4].load();
        ImGui::BeginDisabled(!s.red_orbs_supported.load() || max_orbs);
        MultiplierRow("Red orbs", s.red_orb_multiplier,
                      !s.red_orbs_supported.load() ? Unsupported
                      : max_orbs ? "Turn off Max red orbs to use the multiplier."
                                 : "Red orbs gained. Orbs you have and prices stay the same.");
        ImGui::EndDisabled();
        ImGui::BeginDisabled(!s.orb_pickup_supported.load());
        const char* pickup = s.orb_pickup_supported.load() ? nullptr : Unsupported;
        MultiplierRow("Green orbs (health)", s.green_orb_multiplier,
                      pickup ? pickup : "Health from each green orb. The bar stays within its maximum.");
        MultiplierRow("Blue orbs (magic)", s.blue_orb_multiplier,
                      pickup ? pickup : "Magic from each blue orb. The bar stays within its maximum.");
        MultiplierRow("Gold orbs (Rage of Sparta)", s.gold_orb_multiplier,
                      pickup ? pickup : "Rage of Sparta from each gold orb. The bar stays within its maximum.");
        ImGui::EndDisabled();
        ImGui::BeginDisabled(!s.damage_supported.load());
        const char* damage = s.damage_supported.load() ? nullptr : Unsupported;
        MultiplierRow("Damage dealt", s.damage_dealt, damage ? damage : "Damage Kratos does to enemies.");
        MultiplierRow("Damage taken", s.damage_taken, damage ? damage : "Damage Kratos takes.");
        ImGui::EndDisabled();
        EndRows();
    }

    Section("HUD");
    if (BeginRows("##hud")) {
        ImGui::BeginDisabled(!s.actor_watch_supported.load());
        CheckRow("Enemy health bar", s.enemy_health_bar,
                 s.actor_watch_supported.load() ? "A thin bar at the top for the last enemy hit." : Unsupported);
        ImGui::EndDisabled();
        EndRows();
    }
}

// Controls: the free camera's automatic moves (src/camera_hook.h), each live. Ticked = the game's camera.
void ControlsTab() {
    auto& s = Gow3Settings::Get();
    Section("Mouse");
    static const bool mouse_on = [] {
        const char* env = std::getenv("GOW3_MOUSE");
        return env && std::strcmp(env, "1") == 0;
    }();
    if (!mouse_on) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("The mouse is off: launcher > Controls > Mouse look and buttons (applied at the next start). "
                            "These values are kept for then.");
        ImGui::PopTextWrapPos();
    }
    if (BeginRows("##mouse")) {
        NumberRow("Sensitivity", s.mouse_sensitivity, Gow3Settings::MouseSensitivityMin, 0.1f, 0.001f, "%.3f",
                  Gow3Settings::ClampMouseSensitivity,
                  "Degrees the camera turns per mouse count, horizontally (0.022 is the usual PC default; at 1600 "
                  "DPI about 35 degrees per inch). Type a value up to 0.2 in the box.");
        NumberRow("Vertical / horizontal", s.mouse_vertical_ratio, Gow3Settings::MouseRatioMin,
                  Gow3Settings::MouseRatioMax, 0.01f, "%.2f", Gow3Settings::ClampMouseRatio,
                  "Vertical sensitivity as a share of the horizontal one: below 1, up and down turns less.");
        CheckRow("Invert vertical", s.mouse_invert_y, "Pushing the mouse forward looks down instead of up.");
        EndRows();
    }
    Section("Advanced camera");
    const uint32_t found = s.camera_found.load();
    ImGui::PushTextWrapPos(0.0f);
    if (!s.camera_hook.load()) {
        ImGui::TextDisabled("Not available for this game build (the camera hook knows MediEvil 01.00 and 01.02).");
    } else {
        ImGui::TextDisabled("Untick a move to turn it off; changes apply at once. Only the free camera changes: fixed, "
                            "rail and boss cameras keep the game's moves.");
        if (!found) {
            ImGui::TextDisabled("Not seen yet: the moves are found once Dan is in a free camera area.");
        }
    }
    ImGui::PopTextWrapPos();
    static const char* const titles[Gow3Settings::CameraMoveCount] = {
        "Camera follows Dan", "Sideways swing", "Suggested directions",
        "Turn around posts", "Run zoom and tilt", "Vertical look re-centres"};
    static const char* const hints[Gow3Settings::CameraMoveCount] = {
        "While Dan moves, the camera turns toward where he is going. Off: only you turn it (mouse or right "
        "stick), at the same speed standing or moving.",
        "Running sideways swings the camera up to about 20 degrees.",
        "Entering some areas turns the camera toward the way ahead for up to a second (13 areas).",
        "Near a few posts the camera turns so the post does not block the view (2 areas).",
        "The camera pulls in or out and tilts when Dan starts running.",
        "While Dan moves or attacks, the up / down angle eases back to the middle."};
    if (BeginRows("##camera")) {
        for (int move = 0; move < Gow3Settings::CameraMoveCount; ++move) {
            const bool seen = (found >> move) & 1;
            CheckRow(titles[move], s.camera_moves[move],
                     seen || !s.camera_hook.load() ? hints[move]
                                                   : "Not found yet: applies once its camera area has been used.");
        }
        EndRows();
    }
}

// MediEvil's frame buffer sizes (medievil.ini output_resolution, Gow3Settings::UE4OutputKeys' order).
constexpr const char* OutputLabels[] = {"Match display", "4K", "1800p", "1440p", "1080p (original)", "720p"};

// MediEvil: the 3D scene's size per side compared with the frame buffer, in 1% steps, switched live
// (Gow3Settings::RequestRenderScale, applied by ApplyRenderScale) and kept in medievil.ini.
void RenderScaleRow() {
    namespace S = Gow3Settings;
    auto& s = S::Get();
    const int lo = S::LiveRenderScaleMin(), live_max = S::LiveRenderScaleMax();
    static char help[256];
    std::snprintf(help, sizeof(help),
                  "The 3D scene's size compared with the frame buffer. Below 100%%: faster, rebuilt by the upscaler. "
                  "Above: sharper, slower. Live from %d%% to %d%%, higher on the next start.",
                  lo, live_max);
    Restart(S::RenderScaleNeedsRestart());
    bool active = false;
    const int v = IntSliderRow("Render scale", s.render_scale, lo, S::RenderScaleMax, 1, "%d%%", help, &active, true);
    if (v != s.render_scale) {
        s.render_scale = v;
        dirty = true;
        render_scale_pending = true;
        render_scale_edit = std::chrono::steady_clock::now();
    }
    if (active) {
        render_scale_edit = std::chrono::steady_clock::now(); // applied once let go
    }
}

// The render scale's notes, shown only when the chosen scale waits for the next start.
void RenderScaleNotes() {
    namespace S = Gow3Settings;
    const auto& s = S::Get();
    if (s.render_scale > S::LiveRenderScaleMax()) {
        Note("%d%% applies at the next start (APPLY & RESTART); until then the scene stays at %d%%, the most the "
             "game's memory was set up for when it started.",
             s.render_scale, s.live_render_scale);
    } else if (S::RenderScaleNeedsRestart()) {
        Note("Anti-aliasing is not temporal (launcher > Performance): the scale applies now, the anti-aliasing "
             "stays as the game started until the next start (below 100%% a start always uses temporal AA).");
    }
}

// Display: the frame buffer and the window, the frame cap, VSync, scaling and sharpening, what runs now.
void DisplayTab() {
    namespace G = Gow3Graphics;
    namespace S = Gow3Settings;
    auto& s = S::Get();
    Section("Resolution");
    if (BeginRows("##resolution")) {
        if (IsGow3()) {
            // God of War III: a resolution patch.
            static const char* const resolution_labels[] = {
                "1080p (original)", "480p", "720p", "1440p", "1800p", "4K", "720p", "1440p", "1800p", "4K"};
            Restart();
            ChoiceRow("Render resolution", s.render_resolution, resolution_labels, nullptr,
                      int(G::Resolutions.size()), "The resolution the game draws at. Higher is sharper and slower.");
        } else {
            // MediEvil: the game's output (output_resolution), saved for the next start: the game boots at it
            // (run.py, the boot display size patch). Restart-only (the user, 2026-11-13; plan section 10).
            Restart();
            IndexChoiceRow("Frame buffer size", s.output_resolution, OutputLabels, int(std::size(OutputLabels)),
                           "The size the game draws its picture and HUD at (the PS4's output); the window scales it "
                           "to fit. Match display for a sharp 1:1 image. Applies at the next start.",
                           "Custom");
        }
        ChoiceRow("Display mode", s.display_mode, G::DisplayModeNames.data(), nullptr, G::DisplayModeCount,
                  "Windowed, borderless window or exclusive fullscreen.");
        EndRows();
    }

    Section("Frame cap");
    if (BeginRows("##frame_cap")) {
        // The highest frame rate the game runs at (Unreal Engine 4: run.py's override pak).
        static const char* const engine_labels[] = {"60 (original)", "120 (experimental)", "240 (experimental)"};
        Restart();
        ChoiceRow("Engine frame rate", s.engine_fps, engine_labels, G::EngineFps.data(), int(G::EngineFps.size()),
                  "The highest frame rate the game itself runs at.");
        static const char* const fps_labels[] = {"Auto: refresh (VRR: below it, Reflex rule)",
                                                 "Auto: half refresh (VRR: one below)", "30", "60", "119 (VRR, 240 Hz)",
                                                 "120", "141 (VRR, 144 Hz)", "237 (VRR, 240 Hz)", "240", "Unlimited",
                                                 "Half display refresh (fixed refresh)"};
        ChoiceRow("Frame rate limit", s.fps_limit, fps_labels, G::FpsLimits.data(), int(G::FpsLimits.size()),
                  "Caps the frame rate. The engine frame rate (above) is the highest it can go.");
        CheckRow("G-Sync / FreeSync display", s.display_vrr,
                 "On: the automatic limits stay just below the refresh (VRR). Off (fixed refresh): exactly the "
                 "refresh or half of it, with VSync on.");
        const int limit = G::ResolveFpsLimit(s.fps_limit, Gow3DisplayRefreshHz(), s.display_vrr);
        const int effective = G::EffectiveFps(limit, s.startup_engine_fps);
        if (s.fps_limit < 0) {
            char text[64];
            std::snprintf(text, sizeof(text), "%d FPS (%u Hz display)", effective, Gow3DisplayRefreshHz());
            InfoRow("Effective limit", text);
        } else if (limit == 0 || effective < limit) {
            char text[48];
            std::snprintf(text, sizeof(text), "%d FPS (engine frame rate)", effective);
            InfoRow("Effective limit", text);
        }
        EndRows();
    }

    Section("VSync");
    if (BeginRows("##vsync")) {
        // The launcher's "Off, allow tearing" / "Adaptive" fix the present mode (GOW3_PRESENT_MODE), which wins
        // over this switch (vsync-control): shown, but greyed out, as the launcher's choice is what runs.
        static const bool fixed_mode = [] {
            const char* mode = std::getenv("GOW3_PRESENT_MODE");
            return mode && mode[0];
        }();
        ImGui::BeginDisabled(fixed_mode);
        CheckRow("VSync", s.vsync,
                 fixed_mode ? "No effect: the launcher fixes the present mode (VSync: Off, allow tearing or Adaptive). "
                              "Choose On or Off there to switch it here."
                            : "Waits for the monitor: no tearing, a little more input delay.");
        ImGui::EndDisabled();
        static const char* const queued_labels[] = {"1 (lowest latency)", "2", "Unlimited"};
        ChoiceRow("Frames queued", s.frames_queued, queued_labels, G::FramesQueued.data(),
                  int(G::FramesQueued.size()), "More frames queued: steadier FPS, slower response to input.");
        EndRows();
    }

    const int iw = s.image_width, ih = s.image_height, ww = s.window_width, wh = s.window_height;
    const bool upscaling = iw < ww && ih < wh;
    const bool downscaling = iw > ww || ih > wh;
    // While AMD FSR runs, RCAS is off and FSR sharpens by its own strength (fsr_sharpness).
    const bool fsr_running = Vulkan::Ue4Upscaler::SharpensItself();
    const bool rcas_blocked = s.rcas && !s.rcas_applied && !fsr_running;
    Section("Scaling and sharpening");
    if (BeginRows("##scaling")) {
        CheckRow("Upscale with FSR 1", s.fsr1,
                 IsGow3() ? "Enlarges a smaller game image to the window. For more FPS: Render resolution 720p + FSR 1."
                          : "Enlarges a frame buffer smaller than the window with AMD FSR 1. For more FPS: a smaller "
                            "Frame buffer size with FSR 1.");
        {
            // The game's TAA, and the upscalers this system can run (Vulkan::Ue4Upscaler).
            static constexpr const char* names[] = {"Game's TAA", "NVIDIA DLSS", "AMD FSR"};
            const char* labels[S::Ue4UpscalerCount];
            int values[S::Ue4UpscalerCount];
            int count = 0;
            for (int kind = 0; kind < S::Ue4UpscalerCount; ++kind) {
                if (kind == 0 || (kind == 1 && s.ue4_dlss_available) || (kind == 2 && s.ue4_fsr_available)) {
                    labels[count] = names[kind];
                    values[count++] = kind;
                }
            }
            ChoiceRow("Anti-aliasing / upscaler", s.ue4_upscaler, labels, values, count,
                      "Replaces the game's anti-aliasing: native anti-aliasing at Render scale 100, upscaling "
                      "below it. AMD FSR sharpens by its own strength, DLSS and the game's TAA with Sharpening "
                      "(RCAS). F9 switches between them in the game. DLSS needs an NVIDIA RTX GPU and "
                      "nvngx_dlss.dll in the bin folder (see README-Windows.txt).");
        }
        if (!IsGow3()) {
            RenderScaleRow();
            EndRows();
            RenderScaleNotes();
            if (!BeginRows("##scaling_after")) return;
        }
        if (s.ue4_fsr_available) {
            SliderRow("AMD FSR sharpening", s.fsr_sharpness, 0, 100, "%d%%",
                      "FSR's own sharpening while AMD FSR runs, apart from the RCAS strength. 0 turns it off.", 5);
        }
        ImGui::BeginDisabled(rcas_blocked);
        CheckRow("Sharpening (RCAS)", s.rcas,
                 rcas_blocked && downscaling ? "No effect: the frame buffer is larger than the window."
                 : rcas_blocked              ? "Needs FSR 1: the frame buffer is smaller than the window."
                 : fsr_running ? "Off while AMD FSR runs: FSR sharpens by its own strength (AMD FSR sharpening)."
                               : "Sharpens the final image (the game's TAA, NVIDIA DLSS).");
        ImGui::EndDisabled();
        ImGui::BeginDisabled(!s.rcas || rcas_blocked);
        SliderRow("Sharpening strength", s.rcas_strength, 0, 100, "%d%%", "How strong the sharpening is.", 5);
        ImGui::EndDisabled();
        EndRows();
    }

    Section("Information");
    if (BeginRows("##information")) {
        char text[96];
        std::snprintf(text, sizeof(text), "%d x %d", iw, ih);
        InfoRow("Frame buffer", text);
        if (!IsGow3()) {
            if (iw > 0 && ih > 0) {
                std::snprintf(text, sizeof(text), "%d x %d (%d%%)", S::ScaledSize(iw, s.live_render_scale),
                              S::ScaledSize(ih, s.live_render_scale), s.live_render_scale);
            } else {
                std::snprintf(text, sizeof(text), "%d%%", s.live_render_scale);
            }
            InfoRow("Render resolution", text);
        }
        std::snprintf(text, sizeof(text), "%d x %d", ww, wh);
        InfoRow("Window size", text);
        InfoRow("FSR 1", !s.fsr1 ? "off" : upscaling ? "active" : "no effect (frame buffer not smaller)");
        if (fsr_running && s.fsr_sharpness > 0) {
            std::snprintf(text, sizeof(text), "AMD FSR, %d%%", s.fsr_sharpness.load());
        }
        InfoRow("Sharpening", fsr_running ? (s.fsr_sharpness > 0 ? text : "AMD FSR, off")
                              : s.rcas_applied ? "applied"
                                               : "not applied");
        if (Vulkan::Ue4Upscaler::Running()) {
            std::snprintf(text, sizeof(text), "%s running", Vulkan::Ue4Upscaler::RunningName());
        }
        InfoRow("Upscaler", Vulkan::Ue4Upscaler::Running() ? text
                            : s.ue4_upscaler != 0          ? "chosen (waiting for the game's TAA)"
                                                           : "the game's TAA");
        EndRows();
    }
}

// Graphics: Graphics quality, then every graphics setting (research/docs/plan-graphics-settings.md): the live
// ones switch through UE4 console variables at once, the others (RESTART) at the next start.
void GraphicsTab() {
    namespace S = Gow3Settings;
    auto& s = S::Get();
    Section("Graphics quality");
    if (IsGow3()) {
        Note("God of War III has no graphics settings here: its render resolution is on the Display tab.");
        return;
    }
    if (!BeginRows("##graphics")) {
        return;
    }
    {
        // Graphics quality sets every graphics setting; it shows the level they all match, else Custom.
        static const char* const quality_labels[] = {"Ultra", "High", "Console (PS4)", "Medium",
                                                     "Low (integrated graphics)", "Deck Optimized"};
        static_assert(std::size(quality_labels) == S::UE4DeckLevel + 1);
        const int matched = S::UE4MatchedLevel(s, s.ue4_quality);
        const int level = ChoiceIndex(
            "Graphics quality", matched, quality_labels, int(std::size(quality_labels)),
            "Sets every graphics setting: Console is exactly the PS4, High and Ultra go beyond it, Medium and Low "
            "are for integrated graphics, Deck Optimized is the Steam Deck's. Custom: some settings changed by hand.",
            "Custom");
        if (level >= 0) {
            S::UE4ApplyLevel(level);
            dirty = true;
        }
    }
    const bool preshadows = s.preshadows;
    CheckRow("Preshadows", s.preshadows,
             "Shadows that Dan and enemies cast from fixed lights such as torches. Off: faster in busy views with "
             "many lights (the Graveyard horizon held the frame cap); some of those shadows go.");
    if (s.preshadows != preshadows) {
        runtime_ue4_request_cvar("r.Shadow.Preshadows", s.preshadows ? "1" : "0");
    }
    static const char* const effects_labels[] = {"100%", "75%", "50%"};
    const int effects = s.effects_resolution;
    IndexChoiceRow("Effects resolution", s.effects_resolution, effects_labels, 3,
                   "Smoke, fire and enemy death effects drawn at a lower resolution and blended in. At 4K, 50% is "
                   "the PS4's 1080p effect detail; several deaths at once cost 2-3 ms a frame at 100%.");
    if (s.effects_resolution != effects && s.effects_resolution >= 0) {
        runtime_ue4_request_cvar("r.SeparateTranslucencyScreenPercentage", S::UE4EffectsKeys[s.effects_resolution]);
    }
    // The live settings in the table's order, then the ones a start applies (RESTART).
    for (const bool live : {true, false}) {
        for (int o = 0; o < S::UE4LiveCount; ++o) {
            const auto& option = S::UE4LiveOptions[o];
            if (option.live != live) continue;
            const int before = s.ue4_live[o];
            Restart(!live);
            IndexChoiceRow(option.title, s.ue4_live[o], option.labels, option.count, option.hint);
            if (s.ue4_live[o] != before) {
                S::RequestUE4Live(o, s.ue4_live[o]);
            }
        }
    }
    EndRows();
}

extern "C" void runtime_memory_set_perf(int optimistic, int keep_access);

// Performance: the patches (live ones first, then the shader compile switch, then the ones a start applies),
// the GPU readback modes, and the Steam Deck switches (medievil.ini deck_*, research/patches/deck).
void PerformanceTab() {
    namespace S = Gow3Settings;
    auto& s = S::Get();
    Section("Performance patches");
    if (IsGow3()) {
        if (BeginRows("##perf_gow3")) {
            CheckRow("Compile new shaders in the background", s.async_shaders,
                     "Fewer stutters in new areas; an object or effect may appear a moment later.");
            Restart();
            CheckRow("Game reads GPU data without waiting", s.stale_readback,
                     "Big FPS gain. Switch it off if lighting or objects look wrong.");
            Restart();
            CheckRow("Deferred GPU readbacks", s.deferred_readback,
                     "Fewer waits for the GPU. Switch it off if textures or shadows look wrong.");
            EndRows();
        }
        return;
    }
    static const char* const titles[S::PerfCount] = {
        "HZB occlusion culling", "Skip the decal depth prepass", "Single GPU particle pass",
        "Small buffers from VRAM", "Texture set memo", "Lock-free memory map lookups", "Cheap memory frees",
        "GPU data without waits", "4K memory sizes", "Fewer render pass breaks", "Cheaper draw binding",
        "Constant ring reuse", "Particle flip guard"};
    static const char* const hints[S::PerfCount] = {
        "Skips objects hidden behind others, without waiting for the GPU. Heavy areas ~130 -> ~165 FPS.",
        "No second depth-only drawing of the scene for nearly invisible decals: ~600 fewer draws, ~10%.",
        "One particle simulation pass a frame instead of an extra empty one: up to ~190 fewer draws. Needs "
        "Particle flip guard (below): without it some lone torches freeze and an ember stream shows in Dan's crypt.",
        "Unchanged small buffers are read from video memory instead of copied every draw: ~5%.",
        "Remembered texture bindings survive other textures streaming in: binding ~7% cheaper.",
        "The GPU threads read the memory map without waiting for the game: fewer panning spikes.",
        "Freed memory keeps its page access: frees ~7x cheaper, smoother 0.1% lows.",
        "Readback queue, GPU write twins and deferred fences: removed the constant micro-stutter.",
        "More graphics memory and texture pool at 4K native: no reloads on small camera moves.",
        "No depth barriers between effect draws: fights' 0.1% lows 100 -> 117 at a 120 cap.",
        "Descriptors copied in one piece, samplers reused: ~0.3 ms less draw-thread work in heavy frames.",
        "Constants bound again in a command buffer reuse their first copy: heavy-area 0.1% lows ~145 -> ~185.",
        "One state flip per particle simulation: the single particle pass then has no frozen torches or crypt "
        "floor embers. No cost measured."};
    // The workbook's order (research/docs/data/settings-menu-items.xlsx): the live patches, the particle flip
    // guard under the single pass, then the ones a start applies.
    static constexpr S::PerfPatch live_order[] = {S::PerfParticleSinglePass, S::PerfParticleFlipGuard,
                                                  S::PerfSmallBuffers,       S::PerfLockfreeMemory,
                                                  S::PerfCheapFrees,         S::PerfLeanBinding,
                                                  S::PerfRingReuse};
    static constexpr S::PerfPatch restart_order[] = {S::PerfHzbOcclusion, S::PerfNoDecalPrepass, S::PerfTextureMemo,
                                                     S::PerfGpuDataNoWaits, S::Perf4kMemory, S::PerfLeanBarriers};
    if (BeginRows("##perf_live")) {
        for (const S::PerfPatch p : live_order) {
            Restart(!S::PerfLive[p]);
            CheckRow(titles[p], s.perf[p], hints[p]);
        }
        EndRows();
    }
    // The live ones in the memory runtime (src/runtime_memory.c); the buffer cache reads its own.
    runtime_memory_set_perf(s.perf[S::PerfLockfreeMemory] ? 1 : 0, s.perf[S::PerfCheapFrees] ? 1 : 0);
    // The particle pass through its console variable at the next game-thread poll (once per change).
    runtime_ue4_particle_single_pass(s.perf[S::PerfParticleSinglePass] ? 1 : 0);
    if (Gow3Toggle2::Disabled(Gow3Toggle2::ParticleFixedStep)) {
        Note("Single GPU particle pass: held off by the toggle file (second word 8192); this choice applies when "
             "that bit clears.");
    }
    if (s.perf[S::PerfParticleSinglePass] && !s.startup_perf[S::PerfParticleFlipGuard]) {
        Note("Particle flip guard is not running in this start: with the single pass some torches freeze.");
    }
    if (BeginRows("##perf_restart")) {
        CheckRow("Compile new shaders in the background", s.async_shaders,
                 "Fewer stutters in new areas; an object or effect may appear a moment later.");
        for (const S::PerfPatch p : restart_order) {
            Restart(!S::PerfLive[p]);
            CheckRow(titles[p], s.perf[p], hints[p]);
        }
        Restart();
        CheckRow("Game reads GPU data without waiting", s.stale_readback,
                 "Big FPS gain. Switch it off if lighting or objects look wrong.");
        Restart();
        CheckRow("Deferred GPU readbacks", s.deferred_readback,
                 "Fewer waits for the GPU. Switch it off if textures or shadows look wrong.");
        EndRows();
    }
    // The Steam Deck's measured wins (launcher: Steam Deck performance switches; Deck Optimized turns them on).
    // Windows hides the Linux-only ones (run.py never sets them there).
    Section("Deck tuned");
    if (BeginRows("##deck")) {
        static const auto helps = [] {
            std::array<std::string, S::DeckSwitchCount> out;
            for (int d = 0; d < S::DeckSwitchCount; ++d) {
#ifdef _WIN32
                out[d] = std::string(S::DeckSwitches[d].hint) + " Measured on the Steam Deck; untested on Windows.";
#else
                out[d] = std::string(S::DeckSwitches[d].hint) + " Measured on the Steam Deck.";
#endif
            }
            return out;
        }();
        for (int d = 0; d < S::DeckSwitchCount; ++d) {
#ifdef _WIN32
            if (S::DeckSwitches[d].linux_only) continue;
#endif
            Restart();
            CheckRow(S::DeckSwitches[d].title, s.deck[d], helps[d].c_str());
        }
        EndRows();
    }
}

// The performance overlay as it will look, at its own font size (Font size: overlay_scale).
void OverlayPreview() {
    const auto& s = Gow3Settings::Get();
    ImGui::TextDisabled("Preview");
    const float top = ImGui::GetCursorScreenPos().y;
    if (styled) { // the overlay's own font at its own size, not the menu's
        ImFont* font = ImGui::GetIO().Fonts->Fonts[0];
        ImGui::PushFont(font, font->LegacySize);
    }
    ImGui::BeginChild("##preview", ImVec2(0, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY);
    ImGui::SetWindowFontScale(s.overlay_scale / 100.0f);
    const std::string text = OverlayText();
    ImGui::TextUnformatted(text.empty() ? "(nothing selected)" : text.c_str());
    ImGui::SetWindowFontScale(1.0f);
    ImGui::EndChild();
    RecordItem(top, ImGui::GetItemRectMax().y);
    if (styled) {
        ImGui::PopFont();
    }
}

void OverlayTab() {
    namespace P = Gow3PerfStats;
    auto& s = Gow3Settings::Get();
    if (BeginRows("##overlay_on")) {
        CheckRow("Show overlay", s.show_fps, "The performance overlay in a corner of the screen.");
        EndRows();
    }
    ImGui::BeginDisabled(!s.show_fps);
    Section("What to show");
    if (BeginRows("##overlay_items")) {
        static const char* const names[] = {"FPS", "Frametime", "Upscaler", "GPU (name and use)", "CPU (game)",
                                            "RAM (game / total)", "VRAM (used / available)"};
        for (unsigned item = 0; item < 7; ++item) {
            const unsigned bit = 1u << item;
            bool on = (s.overlay_items & bit) != 0;
            if (ToggleRow(names[item], on, "Shown in the performance overlay.")) {
                s.overlay_items = on ? (s.overlay_items | bit) : (s.overlay_items & ~bit);
                dirty = true;
            }
        }
        EndRows();
    }
    OverlayPreview();
    Section("Appearance");
    if (BeginRows("##appearance")) {
        SliderRow("Font size", s.overlay_scale, 50, 300, "%d%%", "Size of the overlay text.", 10);
        static const char* const corners[] = {"Top left", "Top right", "Bottom left", "Bottom right"};
        ChoiceRow("Corner", s.overlay_corner, corners, nullptr, 4, "Where the overlay sits.");
        SliderRow("Background opacity", s.overlay_opacity, 0, 100, "%d%%", "0% is no background.", 5);
        static const char* const layouts[] = {"One line", "One item per line"};
        ChoiceRow("Layout", s.overlay_layout, layouts, nullptr, 2, "Items side by side or stacked.");
        EndRows();
    }
    ImGui::EndDisabled();
    Section("About");
    if (BeginRows("##about")) {
        const auto& elf = Common::ElfInfo::Instance();
        char text[160];
        std::snprintf(text, sizeof(text), "%.*s v%.*s", int(elf.GameSerial().size()), elf.GameSerial().data(),
                      int(elf.AppVer().size()), elf.AppVer().data());
        InfoRow("Game", text);
        const u32 total = background_total;
        if (total) {
            std::snprintf(text, sizeof(text), "loading %u%%", u32(u64(background_done) * 100 / total));
        } else {
            std::snprintf(text, sizeof(text), "ready");
        }
        InfoRow("Shader cache", text);
        if (!IsGow3()) { // the hooks below are God of War III's (cheats, orbs, damage, enemy health)
            EndRows();
            return;
        }
        std::string hooks;
        const auto add = [&](bool on, const char* name) {
            if (on) {
                hooks += hooks.empty() ? name : std::string(", ") + name;
            }
        };
        add(s.cheats_supported.load() != 0, "cheats");
        add(s.red_orbs_supported.load(), "red orbs");
        add(s.orb_pickup_supported.load(), "orb pickups");
        add(s.damage_supported.load(), "damage");
        add(s.actor_watch_supported.load(), "enemy health");
        InfoRow("Game hooks", hooks.empty() ? "none" : hooks.c_str());
        EndRows();
    }
}

int RestartChanges() {
    return Gow3Settings::RestartChanges();
}

// The tab's content: CONTROLS, DISPLAY, GRAPHICS, PERFORMANCE, OVERLAY (2026-11-13). GameTab (God of War
// III's cheats and multipliers) is kept but has no tab for now.
void TabBody(int tab) {
    nav.in_body = true;
    switch (tab) {
    case 0: ControlsTab(); break;
    case 1: DisplayTab(); break;
    case 2: GraphicsTab(); break;
    case 3: PerformanceTab(); break;
    default: OverlayTab(); break;
    }
    nav.in_body = false;
    nav.body_rows = nav.rows;
}

// The menu key and the pad combination, for the title and the default help.
const std::string& OpenKeys() {
    static const std::string keys = std::string(SDL_GetKeyName(MenuKey())) + " / " + MenuPad().name;
    return keys;
}

// The plain look (ImGui's own), when the game's menu look is not there.
void PlainMenu() {
    auto& s = Gow3Settings::Get();
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
                                   viewport->WorkPos.y + viewport->WorkSize.y * 0.5f),
                            ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(std::min(760.0f * base_scale, viewport->WorkSize.x * 0.95f),
                                    std::min(680.0f * base_scale, viewport->WorkSize.y * 0.9f)),
                             ImGuiCond_Appearing);
    bool keep_open = true;
    // gow3: the game's own title (param.sfo), not a fixed one.
    static const std::string heading = [] {
        const std::string_view title = Common::ElfInfo::Instance().Title();
        return std::string(title.empty() ? "MediEvil" : title) + "  (" + OpenKeys() + ")";
    }();
    if (!ImGui::Begin(heading.c_str(), &keep_open, ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }
    const float footer = ImGui::GetFrameHeightWithSpacing() * 2.0f + ImGui::GetTextLineHeightWithSpacing() * 2.0f;
    if (ImGui::BeginTabBar("##tabs")) {
        static const char* const tabs[TabCount] = {"Controls", "Display", "Graphics", "Performance", "Overlay"};
        for (int tab = 0; tab < TabCount; ++tab) {
            const ImGuiTabItemFlags flags = requested_tab == tab ? ImGuiTabItemFlags_SetSelected : 0;
            if (!ImGui::BeginTabItem(tabs[tab], nullptr, flags)) {
                continue;
            }
            if (s.menu_tab != tab && requested_tab < 0) {
                s.menu_tab = tab;
                dirty = true;
            }
            NavStartTab(tab);
            if (ImGui::BeginChild("##body", ImVec2(0, -footer))) {
                TabBody(tab);
            }
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        if (requested_tab >= 0) {
            s.menu_tab = requested_tab;
            dirty = true;
            requested_tab = -1;
        }
        ImGui::EndTabBar();
    }

    ImGui::Separator();
    const int changes = RestartChanges();
    const float progress = RestartProgress();
    restart_prompt_held = false;
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("%s", progress > 0.0f ? "Keep holding: the game starts again with the new settings in 5 seconds."
                              : footer_help   ? footer_help
                                              : "D-pad (or arrows): choose and change. Cross (or Enter): change. L1 / "
                                                "R1: switch tabs. Circle (or Esc): close. The buttons as bound in the "
                                                "launcher. Settings are saved in medievil.ini.");
    ImGui::PopTextWrapPos();
    if (changes) {
        ImGui::TextColored(ImVec4(0.85f, 0.72f, 0.45f, 1.0f), "%d change%s need%s a restart (unsaved progress is lost).",
                           changes, changes == 1 ? "" : "s", changes == 1 ? "s" : "");
        ImGui::SameLine();
        ImGui::BeginDisabled(!RestartAvailable());
        char label[96];
        std::snprintf(label, sizeof(label), "Hold Square + Touchpad 5 s (or hold here): apply and restart%s",
                      progress > 0.0f ? "..." : "");
        ImGui::Button(label);
        restart_prompt_held = ImGui::IsItemActive();
        if (progress > 0.0f) {
            const ImVec2 min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
            ImGui::GetWindowDrawList()->AddRectFilled(min, ImVec2(min.x + (max.x - min.x) * progress, max.y),
                                                      IM_COL32(245, 196, 30, 90), ImGui::GetStyle().FrameRounding);
        }
        ImGui::EndDisabled();
    } else {
        ImGui::NewLine();
    }
    const float close_width = ImGui::CalcTextSize("Close").x + ImGui::GetStyle().FramePadding.x * 2;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - close_width);
    if (ImGui::Button("Close")) {
        keep_open = false;
    }
    ImGui::End();
    if (!keep_open) {
        SetOpen(false);
    }
}

// The game's font and colours for ImGui's own items in the styled menu (notes, the number box, the
// preview's frame).
constexpr std::pair<ImGuiCol, ImU32> MenuColors[] = {
    {ImGuiCol_Text, Cream},
    {ImGuiCol_TextDisabled, Dim},
    {ImGuiCol_FrameBg, IM_COL32(52, 38, 28, 255)},
    {ImGuiCol_FrameBgHovered, IM_COL32(72, 54, 38, 255)},
    {ImGuiCol_FrameBgActive, IM_COL32(92, 68, 46, 255)},
    {ImGuiCol_ChildBg, IM_COL32(0, 0, 0, 110)},
    {ImGuiCol_Border, IM_COL32(176, 166, 150, 90)},
    {ImGuiCol_TextSelectedBg, IM_COL32(245, 196, 30, 90)},
};

void PushMenuStyle() {
    ImGui::PushFont(choice_style.text, TextPx(NoteText) / ImGui::GetStyle().FontScaleMain);
    for (const auto& [color, value] : MenuColors) {
        ImGui::PushStyleColor(color, value);
    }
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(Px(8.0f), Px(2.0f)));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(Px(8.0f), Px(3.0f)));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, Px(6.0f));
}

void PopMenuStyle() {
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(int(std::size(MenuColors)));
    ImGui::PopFont();
}

// The menu in MediEvil's Options menu look (research/docs/plan-settings-menu.md): the Load list's frame,
// fonts, colours and prompts. The title, the tabs between the L1 / R1 prompts, the tab's rows in a scrolled
// list (whole rows only), the focused row's help under it, the prompts inside the frame's bottom (clickable;
// APPLY & RESTART while changes wait for a start). Drawn in the frame's own pixels (1181x870, `menu_u`): the
// frame keeps its width (86% of the height, as before) and its straight middle (rows 300-600) is stretched so
// the frame fills 92% of the height; everything under the middle moves down by `stretch` (section 9).
constexpr float FrameW = 1181.0f, FrameH = 870.0f;
constexpr float TitlePx = 50.0f, TitleY = 116.0f, TabPx = 26.0f, TabY = 176.0f, TabIcon = 38.0f, TabRule = 216.0f;
constexpr float BodyX = 64.0f, BodyW = 1053.0f, BodyTop = 224.0f;
constexpr float PromptRule = 714.0f, PromptTop = 722.0f, PromptIcon = 34.0f, PromptLabel = 26.0f;
constexpr float HelpText = 22.0f, HelpLines = 3.0f, HelpTop = PromptRule - 8.0f - HelpLines * HelpText;
constexpr float BodyBottom = HelpTop - 4.0f;

void StyledMenu() {
    auto& s = Gow3Settings::Get();
    if (requested_tab >= 0) {
        s.menu_tab = requested_tab;
        dirty = true;
        requested_tab = -1;
    }
    const int tab = std::clamp(s.menu_tab.load(), 0, TabCount - 1);
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float height = std::min(viewport->WorkSize.y * 0.86f, viewport->WorkSize.x * 0.96f * FrameH / FrameW);
    const float width = height * FrameW / FrameH;
    menu_u = height / FrameH;
    const float stretch = std::max(0.0f, (viewport->WorkSize.y * 0.92f - height) / menu_u);
    const ImVec2 at(std::floor(viewport->WorkPos.x + (viewport->WorkSize.x - width) * 0.5f),
                    std::floor(viewport->WorkPos.y + viewport->WorkSize.y * 0.03f));
    const auto P = [&](float x, float y) { return ImVec2(at.x + x * menu_u, at.y + y * menu_u); };
    const auto B = [&](float x, float y) { return P(x, y + stretch); }; // under the stretched middle
    const auto text_width = [](ImFont* face, float size, const char* text) {
        return face->CalcTextSizeA(size, FLT_MAX, 0.0f, text).x;
    };
    // The game (its pause menu too) dimmed 80% behind the frame.
    ImGui::GetBackgroundDrawList()->AddRectFilled(viewport->Pos, ImVec2(viewport->Pos.x + viewport->Size.x,
                                                                        viewport->Pos.y + viewport->Size.y),
                                                  IM_COL32(0, 0, 0, 204));
    ImGui::SetNextWindowPos(at, ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(width, (FrameH + stretch) * menu_u), ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    const bool shown = ImGui::Begin("##settings", nullptr,
                                    ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground |
                                        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                        ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar(2);
    if (!shown) {
        ImGui::End();
        return;
    }
    ImDrawList* draw = ImGui::GetWindowDrawList();
    // The frame's middle filled opaque as in the Options menu, the frame over it in three slices (its straight
    // middle stretched), the title.
    draw->AddRectFilled(P(20, 106), B(1161, 768), PanelFill, Px(34.0f));
    {
        const ImTextureRef frame = choice_style.textures[StyleFrame]->GetTexRef();
        const float top = 300.0f / FrameH, bottom = 600.0f / FrameH;
        draw->AddImage(frame, P(0, 0), P(FrameW, 300), ImVec2(0.0f, 0.0f), ImVec2(1.0f, top));
        draw->AddImage(frame, P(0, 300), B(FrameW, 600), ImVec2(0.0f, top), ImVec2(1.0f, bottom));
        draw->AddImage(frame, B(0, 600), B(FrameW, FrameH), ImVec2(0.0f, bottom), ImVec2(1.0f, 1.0f));
    }
    {
        const char* title = "-SETTINGS-";
        const float size = TextPx(TitlePx);
        draw->AddText(choice_style.title, size,
                      ImVec2(at.x + (width - text_width(choice_style.title, size, title)) * 0.5f, P(0, TitleY).y),
                      White, title);
    }

    // The tabs between the L1 and R1 prompts (each clickable); the current one Gold and underlined.
    {
        static const char* const tabs[TabCount] = {"CONTROLS", "DISPLAY", "GRAPHICS", "PERFORMANCE", "OVERLAY"};
        float size = TextPx(TabPx), icon = Px(TabIcon), gap = Px(22.0f);
        float total = 2.0f * (icon + gap) - gap;
        for (const char* name : tabs) {
            total += text_width(choice_style.text, size, name) + gap;
        }
        if (total > Px(1040.0f)) { // another font: keep the strip inside the frame
            const float fit = Px(1040.0f) / total;
            size = std::floor(size * fit);
            icon *= fit;
            gap *= fit;
            total = Px(1040.0f);
        }
        float x = at.x + (width - total) * 0.5f;
        const float y = P(0, TabY).y, line = y + size * 0.5f;
        const auto icon_button = [&](const char* id, StyleImage image) {
            const ImVec2 min(x, line - icon * 0.5f), max(x + icon, line + icon * 0.5f);
            draw->AddImage(choice_style.textures[image]->GetTexRef(), min, max);
            ImGui::SetCursorScreenPos(min);
            const bool pressed = ImGui::InvisibleButton(id, ImVec2(icon, icon));
            x += icon + gap;
            return pressed;
        };
        if (icon_button("##l1", StyleL1)) {
            MenuPress(Ps4::L1);
        }
        for (int i = 0; i < TabCount; ++i) {
            const float w = text_width(choice_style.text, size, tabs[i]);
            ImGui::SetCursorScreenPos(ImVec2(x, y));
            ImGui::PushID(i);
            if (ImGui::InvisibleButton("##tab", ImVec2(w, size * 1.2f))) {
                requested_tab = i;
            }
            const bool hovered = ImGui::IsItemHovered();
            ImGui::PopID();
            draw->AddText(choice_style.text, size, ImVec2(x, y), i == tab ? Gold : hovered ? White : Dim, tabs[i]);
            if (i == tab) {
                draw->AddLine(ImVec2(x, y + size * 1.12f), ImVec2(x + w, y + size * 1.12f), Gold, Px(2.0f));
            }
            x += w + gap;
        }
        if (icon_button("##r1", StyleR1)) {
            MenuPress(Ps4::R1);
        }
        draw->AddLine(P(64, TabRule), P(1117, TabRule), IM_COL32(245, 196, 30, 70), Px(2.0f));
    }

    // The game's button prompts inside the frame's bottom (clear of the title screen's copyright line); drawn
    // before the rows so a click on SELECT reaches the focused row in this frame. While restart-only changes wait,
    // "[Square]+[Touchpad] HOLD: APPLY & RESTART", filled as the hold goes on (a mouse button held on it counts too).
    const float progress = RestartProgress();
    restart_prompt_held = false;
    {
        struct Prompt {
            StyleImage icons[2];
            int count;
            const char* joiner; // between the two icons ("+": held together)
            const char* label;
            uint32_t press;     // a click's button; 0: the hold prompt
            bool enabled;
        };
        Prompt prompts[4];
        int count = 0;
        prompts[count++] = {{StyleCross, StyleCross}, 1, nullptr, "SELECT", Ps4::Cross, true};
        prompts[count++] = {{StyleCircle, StyleCircle}, 1, nullptr, "GO BACK", Ps4::Circle, true};
        prompts[count++] = {{StyleL1, StyleR1}, 2, nullptr, "CHANGE TAB", Ps4::R1, true};
        if (RestartChanges() > 0) {
            prompts[count++] = {{StyleSquare, StyleTouchpad}, 2, "+", "HOLD: APPLY & RESTART", 0, RestartAvailable()};
        }
        constexpr float Pair = 6.0f, Gap = 10.0f, Fit = 1040.0f;
        const float space = count > 3 ? 32.0f : 56.0f;
        float label_size = TextPx(PromptLabel);
        const auto measure = [&](float size, float* labels) {
            float fixed = 0.0f;
            *labels = 0.0f;
            for (int i = 0; i < count; ++i) {
                fixed += (PromptIcon + Pair) * float(prompts[i].count) - Pair + Gap + space;
                if (prompts[i].joiner) fixed += text_width(choice_style.text, size, prompts[i].joiner) / menu_u + Pair;
                *labels += text_width(choice_style.text, size, prompts[i].label) / menu_u;
            }
            return fixed - space;
        };
        float labels = 0.0f;
        float fixed = measure(label_size, &labels);
        if (fixed + labels > Fit && labels > 0.0f) { // four prompts in a narrow font: smaller labels
            label_size = std::max(std::floor(label_size * (Fit - fixed) / labels), 8.0f);
            fixed = measure(label_size, &labels);
        }
        float x = (FrameW - (fixed + labels)) * 0.5f;
        draw->AddLine(B(64, PromptRule), B(1117, PromptRule), IM_COL32(245, 196, 30, 70), Px(2.0f));
        for (int i = 0; i < count; ++i) {
            const Prompt& prompt = prompts[i];
            const ImU32 tint = prompt.enabled ? IM_COL32_WHITE : IM_COL32(255, 255, 255, 110);
            const float start = x;
            float end = x + (PromptIcon + Pair) * float(prompt.count) - Pair + Gap +
                        text_width(choice_style.text, label_size, prompt.label) / menu_u;
            if (prompt.joiner) end += text_width(choice_style.text, label_size, prompt.joiner) / menu_u + Pair;
            if (!prompt.press && progress > 0.0f) { // the hold's fill, under the prompt
                draw->AddRectFilled(B(start - 6.0f, PromptTop - 3.0f), B(start - 6.0f + (end - start + 12.0f) * progress,
                                                                         PromptTop + PromptIcon + 3.0f),
                                    IM_COL32(245, 196, 30, 90), Px(8.0f));
                draw->AddRect(B(start - 6.0f, PromptTop - 3.0f), B(end + 6.0f, PromptTop + PromptIcon + 3.0f),
                              IM_COL32(245, 196, 30, 160), Px(8.0f), 0, Px(1.5f));
            }
            for (int k = 0; k < prompt.count; ++k) {
                if (k > 0 && prompt.joiner) {
                    draw->AddText(choice_style.text, label_size,
                                  B(x, PromptTop + (PromptIcon - PromptLabel) * 0.5f - 1.0f), prompt.enabled ? White : Dim,
                                  prompt.joiner);
                    x += text_width(choice_style.text, label_size, prompt.joiner) / menu_u + Pair;
                }
                draw->AddImage(choice_style.textures[prompt.icons[k]]->GetTexRef(), B(x, PromptTop),
                               B(x + PromptIcon, PromptTop + PromptIcon), ImVec2(0, 0), ImVec2(1, 1), tint);
                x += PromptIcon + Pair;
            }
            x += Gap - Pair;
            draw->AddText(choice_style.text, label_size, B(x, PromptTop + (PromptIcon - PromptLabel) * 0.5f - 1.0f),
                          prompt.enabled ? White : Dim, prompt.label);
            x = end;
            ImGui::SetCursorScreenPos(B(start, PromptTop));
            const bool clicked = ImGui::InvisibleButton(prompt.label, ImVec2(Px(end - start), Px(PromptIcon)));
            if (prompt.press && clicked && prompt.enabled) {
                MenuPress(prompt.press);
            }
            if (!prompt.press && ImGui::IsItemActive()) {
                restart_prompt_held = true; // RestartHoldStep, next frame
            }
            x += space;
        }
    }

    // The tab's rows, scrolled, whole rows only, with a thin scroll bar (wheel, click or drag) when they do not fit.
    ImGui::SetCursorScreenPos(P(BodyX, BodyTop));
    PushMenuStyle();
    ImVec2 body_pos{}, body_size{};
    float scroll = 0.0f, scroll_max = 0.0f;
    if (ImGui::BeginChild("##body", ImVec2(Px(BodyW), Px(BodyBottom + stretch - BodyTop)), ImGuiChildFlags_None,
                          ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar)) {
        body_pos = ImGui::GetWindowPos();
        body_size = ImGui::GetWindowSize();
        // The scroll bar's mouse area first (ImGui gives the hover to the first item), from the last frame's
        // scroll range: a click on the track jumps there, the thumb drags.
        scrollbar_hovered = false;
        if (const float range = ImGui::GetScrollMaxY(); range > 0.0f) {
            const ImVec2 start = ImGui::GetCursorScreenPos();
            const float view = body_size.y, bar = std::max(view * view / (view + range), Px(30.0f));
            const float thumb = body_pos.y + (view - bar) * ImGui::GetScrollY() / range;
            ImGui::SetCursorScreenPos(ImVec2(body_pos.x + body_size.x - Px(16.0f), body_pos.y));
            ImGui::InvisibleButton("##scrollbar", ImVec2(Px(16.0f), view));
            static float grab = 0.0f; // where the thumb was taken, from its top
            const float mouse = ImGui::GetIO().MousePos.y;
            if (ImGui::IsItemActivated()) {
                grab = mouse >= thumb && mouse <= thumb + bar ? mouse - thumb : bar * 0.5f;
            }
            if (ImGui::IsItemActive() && view > bar) {
                ImGui::SetScrollY(std::clamp((mouse - grab - body_pos.y) / (view - bar), 0.0f, 1.0f) * range);
            }
            scrollbar_hovered = ImGui::IsItemHovered() || ImGui::IsItemActive();
            ImGui::SetCursorScreenPos(start);
        }
        body_items.clear();
        in_styled_body = true;
        NavStartTab(tab);
        TabBody(tab);
        in_styled_body = false;
        scroll = ImGui::GetScrollY();
        scroll_max = ImGui::GetScrollMaxY();
        // Whole items only: the list's drawing is cut at the bottom of the last item that fits and at the top of
        // the first one wholly in view (the last row was cut mid-text, T-SM1).
        float cut_top = body_pos.y, cut_bottom = body_pos.y + body_size.y;
        if (scroll_max > 0.0f && !body_items.empty()) {
            float top = cut_bottom, bottom = cut_top;
            for (const auto& [item_top, item_bottom] : body_items) {
                if (item_top >= cut_top - 0.5f) top = std::min(top, item_top);
                if (item_bottom <= cut_bottom + 0.5f) bottom = std::max(bottom, item_bottom);
            }
            if (bottom > top) {
                cut_top = top;
                cut_bottom = bottom;
                for (ImDrawCmd& cmd : ImGui::GetWindowDrawList()->CmdBuffer) {
                    cmd.ClipRect.y = std::max(cmd.ClipRect.y, cut_top);
                    cmd.ClipRect.w = std::min(cmd.ClipRect.w, cut_bottom);
                }
            }
        }
        body_visible_top = cut_top;
        body_visible_bottom = cut_bottom;
    }
    ImGui::EndChild();
    PopMenuStyle();
    if (scroll_max > 0.0f) { // the scroll bar (the frame's draw list: the list's own is cut to whole rows)
        const float view = body_size.y, bar = std::max(view * view / (view + scroll_max), Px(30.0f));
        const float top = body_pos.y + (view - bar) * scroll / scroll_max;
        const float x = body_pos.x + body_size.x - Px(5.0f);
        if (scrollbar_hovered) {
            draw->AddRectFilled(ImVec2(x, body_pos.y), ImVec2(x + Px(5.0f), body_pos.y + view),
                                IM_COL32(200, 196, 190, 40), Px(3.0f));
        }
        draw->AddRectFilled(ImVec2(x, top), ImVec2(x + Px(5.0f), top + bar),
                            IM_COL32(200, 196, 190, scrollbar_hovered ? 200 : 120), Px(3.0f));
    }

    // Under the list: the focused row's help (up to three lines), else how to open and close the menu.
    {
        static const std::string idle = OpenKeys() + ": open and close this menu. Settings are saved in medievil.ini.";
        const bool holding = progress > 0.0f;
        const char* help = holding ? "Keep holding: the game starts again with the new settings in 5 seconds; progress "
                                     "since the last save is lost. Let go to cancel (the changes stay saved)."
                           : footer_help ? footer_help
                                         : idle.c_str();
        const float size = TextPx(HelpText);
        const ImVec2 from = B(72, HelpTop), to = B(1110, HelpTop + HelpLines * HelpText + 2.0f);
        const ImVec4 clip(from.x, from.y, to.x, to.y);
        draw->AddText(choice_style.text, size, from, footer_help || holding ? Cream : Dim, help, nullptr, to.x - from.x,
                      &clip);
    }
    ImGui::End();
}

void Menu() {
    ApplyRenderScale(false);
    footer_help = nullptr;
    // The game's look needs its files, and room (a tiny window keeps the plain look).
    styled = choice_style.Ready() && ImGui::GetMainViewport()->WorkSize.y >= 360.0f;
    if (styled) {
        StyledMenu();
    } else {
        PlainMenu();
    }
    styled = false;
    NavEndFrame();
}

// Enemy health bar from the damage hook's records (gow3_actors.cpp).
// Only the copied values are read, never the game's memory: an actor may be gone by now.
void HealthDisplay() {
    struct Seen {
        uint32_t seq = 0;
        std::chrono::steady_clock::time_point at{};
    };
    static Seen seen[2];
    const auto& s = Gow3Settings::Get();
    if (!s.actor_watch_supported.load()) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    Gow3ActorSlot slot[2];
    for (int i = 0; i < 2; ++i) {
        std::memcpy(&slot[i], const_cast<const Gow3ActorSlot*>(&gow3_actor_slots[i]), sizeof(slot[i]));
        if (slot[i].seq != seen[i].seq) {
            seen[i] = {slot[i].seq, now};
        }
    }
    const auto recent = [&](int i, int seconds) {
        return seen[i].seq != 0 && now - seen[i].at < std::chrono::seconds(seconds);
    };
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImDrawList* draw = ImGui::GetForegroundDrawList();
    const float scale = base_scale;
    if (s.enemy_health_bar && recent(1, 4) && Gow3Actors::Valid(slot[1].health, slot[1].max_health)) {
        // Minimal: a thin green bar centered on the screen's top edge; it shrinks toward the center
        // so it stays symmetric, with the health in numbers below.
        const ImVec2 display = ImGui::GetIO().DisplaySize;
        const float width = 260.0f * scale, height = 5.0f * scale, center = display.x * 0.5f;
        const float top = 36.0f * scale;
        const float fraction = Gow3Actors::Fraction(slot[1].health, slot[1].max_health);
        draw->AddRectFilled(ImVec2(center - width * 0.5f - scale, top - scale),
                            ImVec2(center + width * 0.5f + scale, top + height + scale),
                            IM_COL32(0, 0, 0, 140), 2 * scale);
        const float half = width * 0.5f * fraction;
        draw->AddRectFilled(ImVec2(center - half, top), ImVec2(center + half, top + height),
                            IM_COL32(70, 190, 80, 220), 2 * scale);
        char text[48];
        std::snprintf(text, sizeof(text), "%.0f / %.0f", std::ceil(slot[1].health), slot[1].max_health);
        const float font = ImGui::GetFontSize() * 0.9f;
        const ImVec2 size = ImGui::GetFont()->CalcTextSizeA(font, FLT_MAX, 0.0f, text);
        const ImVec2 at{center - size.x * 0.5f, top + height + 4.0f * scale};
        draw->AddText(ImGui::GetFont(), font, ImVec2(at.x + scale, at.y + scale), IM_COL32(0, 0, 0, 160), text);
        draw->AddText(ImGui::GetFont(), font, at, IM_COL32(225, 235, 220, 220), text);
    }
}

// The game's text dialog: what is typed, and how to finish (keyboard or controller).
void TextEntryBox() {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
                                   viewport->WorkPos.y + viewport->WorkSize.y * 0.42f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(viewport->WorkSize.x * 0.32f, 0.0f),
                                        ImVec2(viewport->WorkSize.x * 0.8f, FLT_MAX));
    ImGui::SetNextWindowBgAlpha(0.92f);
    ImGui::Begin("##text_entry", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::TextColored(ImVec4(0.85f, 0.72f, 0.45f, 1.0f), "%s", text_entry_prompt.c_str());
    ImGui::Separator();
    ImGui::SetWindowFontScale(1.4f);
    ImGui::Text("%s_", text_entry_text.c_str());
    ImGui::SetWindowFontScale(1.0f);
    ImGui::Separator();
    ImGui::TextDisabled("Keyboard: type, Backspace to erase, Enter = OK, Esc = cancel");
    ImGui::TextDisabled("Controller: Cross (A) = OK, Circle (B) = cancel");
    ImGui::End();
}

std::vector<std::string_view> SplitLines(const std::string& text) {
    std::vector<std::string_view> lines;
    const std::string_view all(text);
    for (size_t begin = 0;;) {
        const size_t end = all.find('\n', begin);
        lines.push_back(all.substr(begin, end == std::string_view::npos ? std::string_view::npos : end - begin));
        if (end == std::string_view::npos) {
            return lines;
        }
        begin = end + 1;
    }
}

// The save list in MediEvil's Options menu look (frame, fonts and button prompts from the game,
// scripts/game_ui_assets.py): the saves on the left, the chosen one's level picture and details
// on the right, the prompts under the frame. Drawn directly, in the frame's own pixels (1181x870)
// so it keeps its proportions. The texts come from runtime_saveinfo.c: an entry's first line is
// its name ("Continue" or "Slot N"); details are "Label: value" lines; the level list is
// "Name: state[, chalice]" lines under a heading.
void StyledChoiceBox() {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImDrawList* draw = ImGui::GetForegroundDrawList();
    constexpr float FrameW = 1181.0f, FrameH = 870.0f;
    // 76% of the height from 6% down: the prompts under the frame end at about 88%, above the title
    // screen's copyright line (about 89%).
    float width = viewport->WorkSize.y * 0.76f * FrameW / FrameH;
    width = std::min(width, viewport->WorkSize.x * 0.94f);
    const float height = width * FrameH / FrameW, u = height / FrameH;
    const ImVec2 at(viewport->WorkPos.x + (viewport->WorkSize.x - width) * 0.5f,
                    viewport->WorkPos.y + viewport->WorkSize.y * 0.06f);
    const auto P = [&](float x, float y) { return ImVec2(at.x + x * u, at.y + y * u); };
    ImFont* const title_font = choice_style.title;
    ImFont* const font = choice_style.text;
    const auto text = [&](ImFont* face, float px, ImVec2 pos, ImU32 color, std::string_view s) {
        draw->AddText(face, px * u, pos, color, s.data(), s.data() + s.size());
    };
    const auto text_width = [&](ImFont* face, float px, std::string_view s) {
        return face->CalcTextSizeA(px * u, FLT_MAX, 0.0f, s.data(), s.data() + s.size()).x;
    };

    // The frame's transparent middle is filled as in the Options menu (opaque: the title screen's
    // logo showed through), then the frame drawn over it.
    draw->AddRectFilled(P(20, 106), P(1161, 768), PanelFill, 34.0f * u);
    draw->AddImage(choice_style.textures[0]->GetTexRef(), at, ImVec2(at.x + width, at.y + height));
    std::string heading = "-" + choice_title + "-";
    std::transform(heading.begin(), heading.end(), heading.begin(),
                   [](unsigned char c) { return char(std::toupper(c)); });
    text(title_font, 66.0f, ImVec2(at.x + (width - text_width(title_font, 66.0f, heading)) * 0.5f, P(0, 112).y),
         White, heading);

    // Left: the saves, scrolled to keep the chosen one in view.
    constexpr float ListX = 70.0f, ListTop = 205.0f, ListBottom = 742.0f, ListW = 450.0f;
    constexpr float Big = 38.0f, Small = 25.0f, Line = 1.18f, Gap = 20.0f;
    std::vector<float> tops, heights;
    float total = 0.0f;
    for (const std::string& item : choice_items) {
        const float h = Big * Line + Small * Line * float(SplitLines(item).size() - 1);
        tops.push_back(total);
        heights.push_back(h);
        total += h + Gap;
    }
    static float scroll = 0.0f;
    const float view = ListBottom - ListTop;
    if (!tops.empty()) {
        const int i = std::clamp(choice_index, 0, int(tops.size()) - 1);
        scroll = std::min(scroll, tops[i]);
        scroll = std::max(scroll, tops[i] + heights[i] - view);
        scroll = std::clamp(scroll, 0.0f, std::max(total - Gap - view, 0.0f));
    }
    draw->PushClipRect(P(ListX - 16, ListTop - 4), P(ListX + ListW, ListBottom), true);
    for (int i = 0; i < int(choice_items.size()); ++i) {
        const bool chosen = i == choice_index;
        float y = ListTop + tops[i] - scroll;
        if (y + heights[i] < ListTop - 4 || y > ListBottom) {
            continue;
        }
        if (chosen) {
            draw->AddRectFilled(P(ListX - 14, y - 4), P(ListX + ListW - 6, y + heights[i] + 4), FocusFill,
                                10.0f * u);
        }
        const auto lines = SplitLines(choice_items[i]);
        for (size_t l = 0; l < lines.size(); ++l) {
            const bool first = l == 0;
            const ImU32 color = chosen && l < 2 ? Gold
                                : first     ? (lines[0] == "Continue" ? Green : Cream)
                                : l == 1    ? Cream
                                            : Dim;
            text(font, first ? Big : Small, P(ListX, y), color, lines[l]);
            y += (first ? Big : Small) * Line;
        }
    }
    draw->PopClipRect();
    if (total - Gap > view) { // more saves than fit: a thin scroll bar
        const float bar = view * view / (total - Gap), top = ListTop + (view - bar) * scroll / (total - Gap - view);
        draw->AddRectFilled(P(ListX + ListW + 2, top), P(ListX + ListW + 7, top + bar), IM_COL32(200, 196, 190, 120),
                            3.0f * u);
    }

    // Right: the chosen save's level picture and details, or (Square) every level in it.
    constexpr float RightX = 548.0f, RightW = 552.0f;
    const int i = choice_index;
    float y = ListTop;
    if (choice_expanded && !choice_more[i].empty()) {
        constexpr float Size = 24.0f;
        const auto lines = SplitLines(choice_more[i]);
        const std::string_view header = "-LEVELS-";
        text(font, 32.0f, P(RightX + (RightW - text_width(font, 32.0f, header) / u) * 0.5f, y), Green, header);
        y += 32.0f * Line + 6.0f;
        for (size_t l = 1; l < lines.size(); ++l) {
            const std::string_view line = lines[l];
            if (line.empty()) {
                continue;
            }
            const size_t colon = line.find(": ");
            const std::string_view name = line.substr(0, colon), state = colon == std::string_view::npos ? "" : line.substr(colon + 2);
            text(font, Size, P(RightX, y), Cream, name);
            const bool done = state.starts_with("completed");
            text(font, Size, P(RightX + RightW * 0.64f, y), done ? White : Dim, done ? "Completed" : "Open");
            if (state.find("chalice") != std::string_view::npos) {
                text(font, Size, P(RightX + RightW * 0.84f, y), Gold, "Chalice");
            }
            y += Size * Line;
        }
    } else {
        // The details sit under the picture: labels from its left edge (a small margin in), values
        // from its middle (the user, 2026-11-10).
        constexpr float PictureW = 448.0f, PictureH = PictureW * 9.0f / 16.0f;
        const float picture_x = RightX + (RightW - PictureW) * 0.5f;
        const float label_x = picture_x + 8.0f, value_x = picture_x + PictureW * 0.48f;
        if (ImTextureData* tex = i < int(choice_textures.size()) ? choice_textures[i] : nullptr) {
            draw->AddImage(tex->GetTexRef(), P(picture_x, y), P(picture_x + PictureW, y + PictureH));
            draw->AddRect(P(picture_x - 2, y - 2), P(picture_x + PictureW + 2, y + PictureH + 2),
                          IM_COL32(186, 186, 192, 255), 4.0f * u, 0, 2.5f * u);
            y += PictureH + 18.0f;
        }
        constexpr float Size = 25.0f;
        const std::string& details = choice_details[i];
        for (const std::string_view line : SplitLines(details.empty() ? std::string("No details for this entry.") : details)) {
            if (line.empty()) {
                y += 10.0f;
                continue;
            }
            const size_t colon = line.find(": ");
            if (colon == std::string_view::npos) {
                text(font, Size, P(label_x, y), Cream, line);
            } else {
                text(font, Size, P(label_x, y), Cream, line.substr(0, colon));
                text(font, Size, P(value_x, y), Gold, line.substr(colon + 2));
            }
            y += Size * Line;
        }
    }

    // The game's button prompts under the frame, as in its Options menu.
    struct Prompt {
        ImTextureData* icon;
        const char* label;
    };
    const Prompt prompts[] = {{choice_style.textures[1], "SELECT"},
                              {choice_style.textures[2], "GO BACK"},
                              {choice_style.textures[3], choice_expanded ? "SUMMARY" : "DETAILS"}};
    constexpr float Icon = 50.0f, Label = 36.0f, Space = 70.0f;
    float row = 0.0f;
    for (const Prompt& prompt : prompts) {
        row += Icon + 12.0f + text_width(font, Label, prompt.label) / u + Space;
    }
    float x = (FrameW - (row - Space)) * 0.5f;
    const float top = FrameH + 8.0f;
    for (const Prompt& prompt : prompts) {
        draw->AddImage(prompt.icon->GetTexRef(), P(x, top), P(x + Icon, top + Icon));
        x += Icon + 12.0f;
        text(font, Label, P(x, top + (Icon - Label) * 0.5f - 2.0f), White, prompt.label);
        x += text_width(font, Label, prompt.label) / u + Space;
    }
}

// The save list with details: the saves on the left (scrolling with the selection), the chosen
// one's level picture and what it holds on the right; Square / Tab shows every level in it.
void DetailedChoiceBox() {
    if (choice_style.Ready()) {
        StyledChoiceBox();
        return;
    }
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
                                   viewport->WorkPos.y + viewport->WorkSize.y * 0.5f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(viewport->WorkSize.x * 0.62f, viewport->WorkSize.y * 0.72f), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.94f);
    constexpr ImGuiWindowFlags passive =
        ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing;
    ImGui::Begin("##choice", nullptr, ImGuiWindowFlags_NoDecoration | passive);
    ImGui::TextColored(ImVec4(0.85f, 0.72f, 0.45f, 1.0f), "%s", choice_title.c_str());
    ImGui::Separator();
    const float footer = ImGui::GetTextLineHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y * 2.0f;
    const float body = std::max(ImGui::GetContentRegionAvail().y - footer, 1.0f);
    ImGui::BeginChild("##choice_list", ImVec2(ImGui::GetContentRegionAvail().x * 0.42f, body), ImGuiChildFlags_None,
                      passive);
    for (int i = 0; i < int(choice_items.size()); ++i) {
        if (i == choice_index) {
            ImGui::TextColored(ImVec4(1.0f, 0.9f, 0.55f, 1.0f), "> %s", choice_items[i].c_str());
            ImGui::SetScrollHereY(0.5f);
        } else {
            ImGui::Text("  %s", choice_items[i].c_str());
        }
        ImGui::Spacing();
    }
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##choice_details", ImVec2(0.0f, body), ImGuiChildFlags_None, passive);
    const int i = choice_index;
    const std::string& more = choice_more[i];
    if (choice_expanded && !more.empty()) {
        ImGui::TextWrapped("%s", more.c_str());
    } else {
        if (ImTextureData* tex = i < int(choice_textures.size()) ? choice_textures[i] : nullptr) {
            const float width = ImGui::GetContentRegionAvail().x;
            const float height = std::min(width * 9.0f / 16.0f, body * 0.55f);
            ImGui::Image(tex->GetTexRef(), ImVec2(height * 16.0f / 9.0f, height));
            ImGui::Spacing();
        }
        const std::string& details = choice_details[i];
        ImGui::TextWrapped("%s", details.empty() ? "No details for this entry." : details.c_str());
    }
    ImGui::EndChild();
    ImGui::Separator();
    ImGui::TextDisabled("D-pad / arrows: choose   Cross / Enter: load   Circle / Esc: cancel   Square: %s "
                        "(the buttons as bound in the launcher)",
                        choice_expanded ? "summary" : "all levels");
    ImGui::End();
}

void ChoiceBox() {
    if (!choice_details.empty()) {
        DetailedChoiceBox();
        return;
    }
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
                                   viewport->WorkPos.y + viewport->WorkSize.y * 0.5f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(viewport->WorkSize.x * 0.36f, 0.0f),
                                        ImVec2(viewport->WorkSize.x * 0.8f, viewport->WorkSize.y * 0.8f));
    ImGui::SetNextWindowBgAlpha(0.94f);
    ImGui::Begin("##choice", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::TextColored(ImVec4(0.85f, 0.72f, 0.45f, 1.0f), "%s", choice_title.c_str());
    ImGui::Separator();
    // Save pictures are 228x128; rows without one keep the text aligned when others have one.
    const bool pictures = std::any_of(choice_textures.begin(), choice_textures.end(),
                                      [](ImTextureData* tex) { return tex != nullptr; });
    const ImVec2 picture_size(228.0f * base_scale, 128.0f * base_scale);
    for (int i = 0; i < int(choice_items.size()); ++i) {
        const bool selected = i == choice_index;
        ImGui::BeginGroup();
        ImTextureData* tex = i < int(choice_textures.size()) ? choice_textures[i] : nullptr;
        if (tex) {
            ImGui::Image(tex->GetTexRef(), picture_size);
        } else if (pictures) {
            ImGui::Dummy(picture_size);
        }
        if (pictures) {
            ImGui::SameLine();
        }
        if (selected) {
            ImGui::TextColored(ImVec4(1.0f, 0.9f, 0.55f, 1.0f), "> %s", choice_items[i].c_str());
        } else {
            ImGui::Text("  %s", choice_items[i].c_str());
        }
        ImGui::EndGroup();
        if (selected) {
            if (pictures) {
                const ImVec2 min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
                const float pad = 3.0f * base_scale;
                ImGui::GetWindowDrawList()->AddRect(ImVec2(min.x - pad, min.y - pad),
                                                    ImVec2(max.x + pad, max.y + pad),
                                                    IM_COL32(255, 230, 140, 255), 0.0f, 0,
                                                    2.0f * base_scale);
            }
            ImGui::SetScrollHereY();
        }
    }
    ImGui::Separator();
    ImGui::TextDisabled("D-pad / arrows: choose   Cross / Enter: OK   Circle / Esc: cancel (as bound in the launcher)");
    ImGui::End();
}

void LoadingScreen() {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
                                   viewport->WorkPos.y + viewport->WorkSize.y * 0.5f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(viewport->WorkSize.x * 0.4f, 0.0f), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.0f);
    ImGui::Begin("##loading", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                     ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing);
    const u32 done = loading_done, total = loading_total;
    ImGui::SetWindowFontScale(1.6f);
    ImGui::TextUnformatted(total ? "Loading shader cache..." : "Preparing graphics...");
    ImGui::SetWindowFontScale(1.0f);
    if (total) {
        char count[64];
        std::snprintf(count, sizeof(count), "%u / %u (%u%%)", done, total,
                      u32(u64(done) * 100 / total));
        ImGui::ProgressBar(float(done) / float(total), ImVec2(-1.0f, 0.0f), count);
    }
    ImGui::TextDisabled("The first start after an update takes longer.");
    ImGui::End();
}

// Diagnostics A/B tests (GOW3_TOGGLE_FILE): a large label names the active set of switched-off
// optimizations, so a player can say which one stopped a glitch.
const char* TestLabel() {
    switch (runtime_disabled_optimizations) {
    case 0: return nullptr;
    case 0xFFFFFFFFFFFFFFFFull: return "TEST A";
    case (32u | 128u | 1024u | (1u << 30)): return "TEST B";
    case 0xFFFFFFFF00000000ull | 4096u | 8192u: return "TEST C";
    case (64u | 256u | 16384u | 524288u): return "TEST D";
    default: return "TEST";
    }
}

void FpsCounter() {
    const auto& s = Gow3Settings::Get();
    std::string text = s.show_fps ? OverlayText() : std::string{};
    if (const char* label = TestLabel()) {
        text = std::string(label) + (text.empty() ? "" : "\n") + text;
    }
    if (text.empty()) {
        return;
    }
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float pad = 12.0f * base_scale;
    const int corner = s.overlay_corner;
    const bool right = corner == 1 || corner == 3, bottom = corner >= 2;
    ImGui::SetNextWindowPos(ImVec2(right ? viewport->WorkPos.x + viewport->WorkSize.x - pad : viewport->WorkPos.x + pad,
                                   bottom ? viewport->WorkPos.y + viewport->WorkSize.y - pad : viewport->WorkPos.y + pad),
                            ImGuiCond_Always, ImVec2(right ? 1.0f : 0.0f, bottom ? 1.0f : 0.0f));
    ImGui::SetNextWindowBgAlpha(s.overlay_opacity / 100.0f);
    ImGui::Begin("##fps", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::SetWindowFontScale((TestLabel() ? 250.0f : float(s.overlay_scale)) / 100.0f);
    ImGui::TextUnformatted(text.c_str());
    ImGui::End();
}

// F8 / F9 (UE4 DLSS): what the key switched to, large, top centre, for 3 s.
void DlssNotice(const char* text) {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
                                   viewport->WorkPos.y + 40.0f * base_scale),
                            ImGuiCond_Always, ImVec2(0.5f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.6f);
    ImGui::Begin("##dlss_notice", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoFocusOnAppearing |
                     ImGuiWindowFlags_NoNav);
    ImGui::SetWindowFontScale(2.0f);
    ImGui::TextUnformatted(text);
    ImGui::End();
}

void BackgroundLoading() {
    const u32 done = background_done, total = std::max(background_total.load(), 1u);
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float pad = 12.0f * base_scale;
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + pad,
                                   viewport->WorkPos.y + viewport->WorkSize.y - pad),
                            ImGuiCond_Always, ImVec2(0.0f, 1.0f));
    ImGui::SetNextWindowBgAlpha(0.35f);
    ImGui::Begin("##background_loading", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::TextDisabled("Loading shader cache %u%%", u32(u64(done) * 100 / total));
    ImGui::End();
}

} // namespace

void Init(const Vulkan::Instance& instance, vk::Format format, u32 image_count) {
    std::scoped_lock lock{imgui_mutex};
    if (initialized) {
        return;
    }
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr; // window positions are not kept
    // No ImGui navigation: the menu has its own (MenuNav), whose Cross / Enter would act twice with it.
    io.BackendPlatformName = "gow3";

    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 6.0f;
    style.FrameRounding = 4.0f;
    style.GrabRounding = 4.0f;
    style.Colors[ImGuiCol_WindowBg].w = 0.92f;

    ImFontConfig font_config;
    font_config.FontDataOwnedByAtlas = false;
    io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(gow3_font_ttf),
                                   int(gow3_font_ttf_end - gow3_font_ttf), 18.0f, &font_config);

    const vk::Instance vk_instance = instance.GetInstance();
    ImGui_ImplVulkan_LoadFunctions(
        instance.ApiVersion(),
        [](const char* name, void* user) {
            return VULKAN_HPP_DEFAULT_DISPATCHER.vkGetInstanceProcAddr(
                *static_cast<const vk::Instance*>(user), name);
        },
        const_cast<vk::Instance*>(&vk_instance));

    const VkFormat color_format = static_cast<VkFormat>(format);
    ImGui_ImplVulkan_InitInfo info{};
    info.ApiVersion = instance.ApiVersion();
    info.Instance = vk_instance;
    info.PhysicalDevice = instance.GetPhysicalDevice();
    info.Device = instance.GetDevice();
    info.QueueFamily = instance.GetGraphicsQueueFamilyIndex();
    info.Queue = instance.GetGraphicsQueue();
    // Font atlas plus the save list pictures (up to 33, and a closed list's until freed).
    info.DescriptorPoolSize = 128;
    info.MinImageCount = std::max(image_count, 2u);
    info.ImageCount = std::max(image_count, 2u);
    info.UseDynamicRendering = true;
    info.PipelineInfoMain.PipelineRenderingCreateInfo = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR,
        .colorAttachmentCount = 1,
        .pColorAttachmentFormats = &color_format,
    };
    if (!ImGui_ImplVulkan_Init(&info)) {
        std::printf("Overlay: ImGui Vulkan backend init failed\n");
        ImGui::DestroyContext();
        return;
    }
    overlay_instance = &instance;
    initialized = true;
    std::printf("Overlay: menu ready (%s or %s)\n", SDL_GetKeyName(MenuKey()), MenuPad().name.c_str());
    // The game's menu look for the settings menu (and the save list) from the game UI cache, the folder
    // the save list names (runtime_services.c), read off this thread at start so the menu has it from
    // its first opening; SetChoiceStyle waits for imgui_mutex until Init returns.
    std::thread([] {
        gow3gpu_choice_style((std::string(runtime_file_user_dir()) + "/cache/game_ui").c_str());
    }).detach();
}

void UpdateTextInput(SDL_Window* window) {
    bool want = false;
    {
        std::scoped_lock lock{imgui_mutex};
        want = initialized && menu_open && ImGui::GetIO().WantTextInput;
    }
    if (want != SDL_TextInputActive(window)) {
        if (want) {
            SDL_StartTextInput(window);
        } else {
            SDL_StopTextInput(window);
        }
    }
}

// imgui_mutex held. The backend frees a texture some frames after its last use.
void RetireChoiceTextures() {
    for (ImTextureData* tex : choice_textures) {
        if (tex) {
            tex->WantDestroyNextFrame = true;
            choice_retired.push_back(tex);
        }
    }
    choice_textures.clear();
    choice_pictures.clear();
    choice_retiring = !choice_retired.empty();
}

// imgui_mutex held, before NewFrame: new pictures become textures, destroyed ones are freed.
void UpdateChoiceTextures() {
    for (int i = 0; i < StyleImageCount; ++i) {
        Gow3Png::Image& image = choice_style.pending[i];
        if (image.rgba.empty() || choice_style.textures[i]) {
            continue;
        }
        ImTextureData* tex = IM_NEW(ImTextureData)();
        tex->Create(ImTextureFormat_RGBA32, image.width, image.height);
        std::memcpy(tex->GetPixels(), image.rgba.data(), image.rgba.size());
        ImGui::RegisterUserTexture(tex);
        choice_style.textures[i] = tex;
        image = {};
    }
    if (!choice_pictures.empty()) {
        choice_textures.assign(choice_pictures.size(), nullptr);
        for (size_t i = 0; i < choice_pictures.size(); ++i) {
            const Gow3Png::Image& picture = choice_pictures[i];
            if (picture.rgba.empty()) {
                continue;
            }
            ImTextureData* tex = IM_NEW(ImTextureData)();
            tex->Create(ImTextureFormat_RGBA32, picture.width, picture.height);
            std::memcpy(tex->GetPixels(), picture.rgba.data(), picture.rgba.size());
            ImGui::RegisterUserTexture(tex);
            choice_textures[i] = tex;
        }
        choice_pictures.clear();
    }
    std::erase_if(choice_retired, [](ImTextureData* tex) {
        if (tex->Status != ImTextureStatus_Destroyed) {
            return false;
        }
        ImGui::UnregisterUserTexture(tex);
        IM_DELETE(tex);
        return true;
    });
    choice_retiring = !choice_retired.empty();
}

// The save list takes the keys and buttons that move and confirm; the rest stays with the game
// (held neutral by CapturesInput).
// Moves or closes the list (lock held). finish: 1 accept, 2 cancel.
void ChoiceStep(int move, int finish, bool toggle = false) {
    if (!choice_active) {
        return;
    }
    if (toggle) {
        choice_expanded = !choice_expanded;
    }
    const int count = int(choice_items.size());
    if (move && count) {
        choice_index = (choice_index + move + count) % count;
    }
    if (finish) {
        choice_result = finish == 1 && count ? choice_index : -2;
        choice_active = false;
        RetireChoiceTextures();
    }
}

// The runtime's buttons for the list (gamepad and keyboard as mapped, scripted input, the left
// stick as up/down), polled every frame: the game may stop reading the pad during the dialog and
// raw window events depend on focus. Holding up or down repeats.
uint32_t (*choice_input)() = nullptr;

void PollChoiceInput() {
    static std::mutex poll_mutex; // the present thread and the game thread both poll
    std::scoped_lock poll_lock{poll_mutex};
    static uint32_t previous = 0;
    static bool open = false;
    static std::chrono::steady_clock::time_point repeat_at{};
    if (!choice_input) {
        return;
    }
    if (!choice_active) {
        open = false;
        return;
    }
    const uint32_t buttons = choice_input();
    const auto now = std::chrono::steady_clock::now();
    if (!open) { // buttons held when the list opened (the press that opened it) wait for release
        open = true;
        previous = buttons;
        return;
    }
    constexpr uint32_t Up = 0x10, Down = 0x40, Circle = 0x2000, Cross = 0x4000, Square = 0x8000;
    const uint32_t pressed = buttons & ~previous;
    previous = buttons;
    int move = (pressed & Up) ? -1 : (pressed & Down) ? 1 : 0;
    if (move) {
        repeat_at = now + std::chrono::milliseconds(400);
    } else if ((buttons & (Up | Down)) && now >= repeat_at) {
        move = (buttons & Up) ? -1 : 1;
        repeat_at = now + std::chrono::milliseconds(120);
    }
    const int finish = (pressed & Cross) ? 1 : (pressed & Circle) ? 2 : 0;
    const bool toggle = (pressed & Square) != 0;
    if (move || finish || toggle) {
        std::scoped_lock lock{imgui_mutex};
        ChoiceStep(move, finish, toggle);
    }
}

// The list's input is PollChoiceInput's (runtime_pad.c list_buttons: the launcher's binds, and the arrows, Enter
// and Esc as fallbacks where no bind takes them, as in the settings menu; plan-settings-menu.md section 10). These
// events act only without that poll: the fallback keys and the pad's default buttons.
bool HandleChoiceEvent(const SDL_Event& event) {
    int move = 0, finish = 0; // finish: 1 accept, 2 cancel
    bool toggle = false;      // Square: the details' other page
    if (event.type == SDL_EVENT_KEY_DOWN) {
        if (choice_input) {
            return true; // PollChoiceInput reads the keys as bound
        }
        const SDL_Keycode key = event.key.key;
        move = key == SDLK_UP ? -1 : key == SDLK_DOWN ? 1 : 0;
        finish = key == SDLK_RETURN || key == SDLK_KP_ENTER ? 1 : key == SDLK_ESCAPE ? 2 : 0;
    } else if (event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
        if (choice_input) {
            return true; // PollChoiceInput reads the gamepad
        }
        const u8 button = event.gbutton.button;
        move = button == SDL_GAMEPAD_BUTTON_DPAD_UP ? -1 : button == SDL_GAMEPAD_BUTTON_DPAD_DOWN ? 1 : 0;
        finish = button == SDL_GAMEPAD_BUTTON_SOUTH ? 1 : button == SDL_GAMEPAD_BUTTON_EAST ? 2 : 0;
        toggle = button == SDL_GAMEPAD_BUTTON_WEST;
    } else {
        return event.type == SDL_EVENT_KEY_UP || event.type == SDL_EVENT_GAMEPAD_BUTTON_UP;
    }
    ChoiceStep(move, finish, toggle);
    return true;
}

void SetChoiceInput(uint32_t (*buttons)()) {
    choice_input = buttons;
}

void SetMenuInput(uint32_t (*buttons)()) {
    menu_input = buttons;
}

// The gamepad buttons and triggers held (window thread, imgui_mutex held): the menu's combination.
uint32_t pad_held = 0;
unsigned triggers_held = 0;

bool HandleEvent(const SDL_Event& event) {
    std::scoped_lock lock{imgui_mutex};
    if (!initialized) {
        return false;
    }
    if (choice_active && !menu_open) {
        return HandleChoiceEvent(event);
    }
    ImGuiIO& io = ImGui::GetIO();
    const bool is_open = menu_open;
    switch (event.type) {
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP: {
        const bool down = event.type == SDL_EVENT_KEY_DOWN;
        static const bool capture_key = [] {
            const char* env = std::getenv("GOW3_FRAME_CAPTURE_KEY");
            return env && *env == '1';
        }();
        if (down && !event.key.repeat && capture_key && event.key.key == SDLK_F11) {
            Vulkan::FrameCapture::Request();
            return true;
        }
        // F9: the next upscaler (UE4's TAA, DLSS, FSR: those this system runs). Not with Ctrl or Alt: NVIDIA's development
        // DLSS DLL uses Ctrl+Alt+F9 / F10 / F12 for its own debug hotkeys.
        const bool plain_key = !(event.key.mod & (SDL_KMOD_CTRL | SDL_KMOD_ALT));
        // F8: the next jitter sign handed to the upscaler, shown on screen.
        if (down && !event.key.repeat && plain_key && event.key.key == SDLK_F8 &&
            Vulkan::Ue4Upscaler::Enabled()) {
            Vulkan::Ue4Upscaler::CycleJitterSign();
            return true;
        }
        if (down && !event.key.repeat && plain_key && event.key.key == SDLK_F9 &&
            Vulkan::Ue4Upscaler::Enabled()) {
            Vulkan::Ue4Upscaler::Toggle();
            Gow3Settings::Save();
            return true;
        }
        // F10: the next UE4 motion vector view (GOW3_UE4_MOTION_VIEW set only).
        if (down && !event.key.repeat && plain_key && event.key.key == SDLK_F10 &&
            Vulkan::Ue4Motion::Enabled()) {
            Vulkan::Ue4Motion::CycleViewMode();
            return true;
        }
        // F12: a PNG screenshot of the game's picture. Shift+F12: raw frames for bug reports
        // (GOW3_FRAME_DUMP_TRIGGER, set by the launcher).
        if (down && !event.key.repeat && plain_key && event.key.key == SDLK_F12 &&
            !(event.key.mod & SDL_KMOD_SHIFT)) {
            screenshot_requested.store(true, std::memory_order_release);
            return true;
        }
        if (down && !event.key.repeat && plain_key && event.key.key == SDLK_F12) {
            if (const char* trigger = std::getenv("GOW3_FRAME_DUMP_TRIGGER")) {
                if (std::FILE* f = std::fopen(trigger, "wb")) {
                    std::fclose(f);
                    std::printf("Overlay: F12 frame dump requested\n");
                    std::fflush(stdout);
                }
            }
            return true;
        }
        if (down && !event.key.repeat && event.key.key == MenuKey()) {
            SetOpen(!is_open);
            return true;
        }
        if (!is_open) {
            return false;
        }
        // The menu's keys come from the runtime's poll (MenuButtons): the keys as bound to PS buttons, and the
        // arrows, Enter and Esc as fallbacks where no bind takes them. Without that poll, only those fallbacks;
        // Esc while a number box is typed in is ImGui's (cancels the edit).
        if (down && !menu_input && !io.WantTextInput) {
            switch (event.key.key) {
            case SDLK_UP: MenuPress(Ps4::Up); break;
            case SDLK_DOWN: MenuPress(Ps4::Down); break;
            case SDLK_LEFT: MenuPress(Ps4::Left); break;
            case SDLK_RIGHT: MenuPress(Ps4::Right); break;
            case SDLK_RETURN:
            case SDLK_KP_ENTER: MenuPress(Ps4::Cross); break;
            case SDLK_ESCAPE: MenuPress(Ps4::Circle); break;
            default: break;
            }
        }
        io.AddKeyEvent(ImGuiMod_Ctrl, (event.key.mod & SDL_KMOD_CTRL) != 0);
        io.AddKeyEvent(ImGuiMod_Shift, (event.key.mod & SDL_KMOD_SHIFT) != 0);
        io.AddKeyEvent(ImGuiMod_Alt, (event.key.mod & SDL_KMOD_ALT) != 0);
        if (const ImGuiKey key = KeyFromSdl(event.key.key); key != ImGuiKey_None) {
            io.AddKeyEvent(key, down);
        }
        return true;
    }
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP: {
        const bool down = event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN;
        const u8 button = event.gbutton.button;
        // The combination that opens the menu (MenuPad, R3+L2 by default), when its last button goes down.
        if (button < SDL_GAMEPAD_BUTTON_COUNT) {
            const MenuPadCombo& combo = MenuPad();
            const bool was = combo.Held(pad_held, triggers_held);
            pad_held = down ? pad_held | (1u << button) : pad_held & ~(1u << button);
            if (down && !was && combo.Held(pad_held, triggers_held) && ((combo.buttons >> button) & 1)) {
                SetOpen(!is_open);
                return true;
            }
        }
        if (!is_open) {
            return false;
        }
        // The open menu polls the pad (MenuButtons); without the runtime's input, these events drive it.
        if (down && !menu_input) {
            switch (button) {
            case SDL_GAMEPAD_BUTTON_DPAD_UP: MenuPress(Ps4::Up); break;
            case SDL_GAMEPAD_BUTTON_DPAD_DOWN: MenuPress(Ps4::Down); break;
            case SDL_GAMEPAD_BUTTON_DPAD_LEFT: MenuPress(Ps4::Left); break;
            case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: MenuPress(Ps4::Right); break;
            case SDL_GAMEPAD_BUTTON_SOUTH: MenuPress(Ps4::Cross); break;
            case SDL_GAMEPAD_BUTTON_EAST: MenuPress(Ps4::Circle); break;
            case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: MenuPress(Ps4::L1); break;
            case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: MenuPress(Ps4::R1); break;
            default: break;
            }
        }
        return true;
    }
    case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
        const SDL_GamepadAxis axis = SDL_GamepadAxis(event.gaxis.axis);
        if (axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER || axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) {
            const unsigned bit = axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER ? 1u : 2u;
            const bool down = event.gaxis.value > 16000;
            const MenuPadCombo& combo = MenuPad();
            const bool was = combo.Held(pad_held, triggers_held);
            triggers_held = down ? triggers_held | bit : triggers_held & ~bit;
            if (down && !was && combo.Held(pad_held, triggers_held) && (combo.triggers & bit)) {
                SetOpen(!is_open);
                return true;
            }
        }
        return false;
    }
    case SDL_EVENT_TEXT_INPUT: {
        // Typed characters (Ctrl+click on a slider, a text field): key events alone erase but
        // do not type. SDL sends them while text input is on (UpdateTextInput).
        if (!is_open) {
            return false;
        }
        io.AddInputCharactersUTF8(event.text.text);
        return true;
    }
    case SDL_EVENT_MOUSE_MOTION: {
        if (!is_open) {
            return false;
        }
        const float density = PixelDensity(event.motion.windowID);
        io.AddMousePosEvent(event.motion.x * density, event.motion.y * density);
        return true;
    }
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP: {
        if (!is_open) {
            return false;
        }
        const int button = event.button.button == SDL_BUTTON_LEFT    ? 0
                           : event.button.button == SDL_BUTTON_RIGHT  ? 1
                           : event.button.button == SDL_BUTTON_MIDDLE ? 2
                                                                      : -1;
        if (button >= 0) {
            io.AddMouseButtonEvent(button, event.type == SDL_EVENT_MOUSE_BUTTON_DOWN);
        }
        return true;
    }
    case SDL_EVENT_MOUSE_WHEEL:
        if (!is_open) {
            return false;
        }
        io.AddMouseWheelEvent(event.wheel.x, event.wheel.y);
        return true;
    default:
        return false;
    }
}

// After "Apply and restart", the new options are kept once the game has shown about ten
// seconds of frames after loading (run.py restores the previous ones otherwise).
void ConfirmRestart() {
    static u32 frames = 0;
    auto& s = Gow3Settings::Get();
    if (!s.restart_unconfirmed || loading || ++frames < 600) {
        return;
    }
    s.restart_unconfirmed = false;
    Gow3Settings::Save();
    std::puts("Settings: graphics options confirmed after restart");
}

void SetBackgroundLoading(u32 done, u32 total) {
    background_done = done;
    background_total = total;
}

void SetLoading(bool active, u32 done, u32 total) {
    loading_done = done;
    loading_total = total;
    loading = active;
}

std::atomic<bool> screenshot_requested{false};

bool TakeScreenshotRequest() {
    return screenshot_requested.exchange(false, std::memory_order_acq_rel);
}

bool Visible() {
    const auto& s = Gow3Settings::Get();
    return initialized && (loading || background_total || menu_open || open_requested || text_entry_active ||
                           choice_active ||
                           runtime_disabled_optimizations != 0 ||
                           choice_retiring || s.show_fps || Vulkan::Ue4Upscaler::Notice() ||
                           (s.actor_watch_supported && s.enemy_health_bar));
}

bool CapturesInput() {
    return menu_open || text_entry_active || choice_active;
}

void RequestOpen() {
    open_requested.store(true);
}

void BeginChoice(const std::string& title, const std::vector<std::string>& items,
                 std::vector<Gow3Png::Image> pictures, std::vector<std::string> details,
                 std::vector<std::string> more, int focus) {
    std::scoped_lock lock{imgui_mutex};
    RetireChoiceTextures();
    choice_pictures = std::move(pictures);
    choice_pictures.resize(items.size());
    choice_title = title;
    choice_items = items;
    choice_details = std::move(details);
    choice_more = std::move(more);
    if (!choice_details.empty()) {
        choice_details.resize(items.size());
        choice_more.resize(items.size());
    }
    choice_expanded = false;
    choice_index = focus >= 0 && focus < int(items.size()) ? focus : 0;
    choice_result = -1;
    choice_active = true;
}

int PollChoice() {
    return choice_result;
}

void SetChoiceStyle(ChoiceStyleFiles files) {
    std::scoped_lock lock{imgui_mutex};
    if (!initialized || choice_style.title) {
        return;
    }
    ImFont* fonts[2] = {};
    for (int i = 0; i < 2; ++i) {
        // The atlas owns (and frees) a copy made with its allocator.
        void* data = IM_ALLOC(files.fonts[i].size());
        std::memcpy(data, files.fonts[i].data(), files.fonts[i].size());
        ImFontConfig config;
        config.FontDataOwnedByAtlas = true;
        fonts[i] = ImGui::GetIO().Fonts->AddFontFromMemoryTTF(data, int(files.fonts[i].size()), 32.0f, &config);
    }
    if (!fonts[0] || !fonts[1]) {
        return;
    }
    choice_style.title = fonts[0];
    choice_style.text = fonts[1];
    std::printf("Overlay: the game's menu look loaded (settings menu, save list)\n");
    static_assert(sizeof(files.images) / sizeof(files.images[0]) == StyleImageCount);
    for (int i = 0; i < StyleImageCount; ++i) {
        choice_style.pending[i] = std::move(files.images[i]);
    }
}

void SetTextEntry(bool active, const std::string& prompt, const std::string& text) {
    std::scoped_lock lock{imgui_mutex};
    text_entry_active = active;
    text_entry_prompt = prompt;
    text_entry_text = text;
}

// GOW3_FPS_LOG=1: presented frames per second and the worst frame, every 5 s (benchmarks). The launcher
// sets 1; 0 (extra-env.txt) turns it off for a run without periodic log lines.
void LogPresent() {
    static const bool fps_log = [] {
        const char* value = std::getenv("GOW3_FPS_LOG");
        return value != nullptr && std::strcmp(value, "0") != 0;
    }();
    if (!fps_log) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    static auto window_start = now, previous = now;
    static unsigned frames = 0;
    static float worst = 0.0f;
    ++frames;
    worst = std::max(worst, std::chrono::duration<float, std::milli>(now - previous).count());
    previous = now;
    const float elapsed = std::chrono::duration<float>(now - window_start).count();
    if (elapsed >= 5.0f) {
        std::printf("Perf: %.1f FPS, worst frame %.1f ms\n", frames / elapsed, worst);
        std::fflush(stdout);
        window_start = now;
        frames = 0;
        worst = 0.0f;
    }
}

void Render(vk::CommandBuffer cmdbuf, vk::ImageView view, vk::Extent2D extent) {
    // Present interval for the FPS readout (measured also while nothing is drawn).
    const auto now = std::chrono::steady_clock::now();
    const float ms = std::chrono::duration<float, std::milli>(now - last_present).count();
    last_present = now;
    if (ms > 0.0f && ms < 1000.0f) {
        frame_ms_avg = frame_ms_avg == 0.0f ? ms : frame_ms_avg * 0.95f + ms * 0.05f;
    }
    ConfirmRestart();
    PollChoiceInput();
    if (open_requested.exchange(false)) {
        SetOpen(true);
    }
    if (!Visible()) {
        return;
    }
    // The menu's buttons, read before the lock as the save list's (the runtime takes its pad lock).
    uint32_t (*const menu_buttons)() = menu_input;
    const uint32_t menu_held = menu_open && menu_buttons ? menu_buttons() : 0;
    std::scoped_lock lock{imgui_mutex};
    MenuButtons(menu_held);
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(float(extent.width), float(extent.height));
    io.DeltaTime = ms > 0.0f && ms < 1000.0f ? ms / 1000.0f : 1.0f / 60.0f;
    // UI scale follows the display height (1080p = 1).
    const float scale = std::max(float(extent.height) / 1080.0f, 0.75f);
    if (std::abs(scale - base_scale) > 0.01f) {
        ImGuiStyle& style = ImGui::GetStyle();
        style.ScaleAllSizes(scale / base_scale);
        style.FontScaleMain = scale;
        base_scale = scale;
    }

    UpdateChoiceTextures();
    ImGui_ImplVulkan_NewFrame();
    ImGui::NewFrame();
    if (loading) {
        LoadingScreen();
    }
    if (menu_open) {
        Menu();
    }
    if ((Gow3Settings::Get().show_fps || TestLabel()) && !menu_open) {
        FpsCounter();
    }
    if (!loading && !menu_open) {
        HealthDisplay();
    }
    if (background_total && !loading && !menu_open) {
        BackgroundLoading();
    }
    if (const char* notice = Vulkan::Ue4Upscaler::Notice(); notice && !menu_open) {
        DlssNotice(notice);
    }
    if (text_entry_active) {
        TextEntryBox();
    }
    if (choice_active && !menu_open) {
        ChoiceBox();
    }
    ImGui::Render();

    const vk::RenderingAttachmentInfo attachment{
        .imageView = view,
        .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .loadOp = vk::AttachmentLoadOp::eLoad,
        .storeOp = vk::AttachmentStoreOp::eStore,
    };
    cmdbuf.beginRendering(vk::RenderingInfo{
        .renderArea = {{0, 0}, extent},
        .layerCount = 1,
        .colorAttachmentCount = 1,
        .pColorAttachments = &attachment,
    });
    {
        // Font atlas uploads submit to the graphics queue themselves.
        std::scoped_lock submit_lock{Vulkan::Scheduler::submit_mutex};
        ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmdbuf);
    }
    cmdbuf.endRendering();
}

} // namespace Gow3Overlay
