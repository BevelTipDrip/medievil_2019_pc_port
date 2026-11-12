// SPDX-License-Identifier: GPL-2.0-or-later
#include "gow3_overlay.h"

#include <algorithm>
#include <atomic>
#include <cctype>
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
extern "C" void runtime_ue4_request_setres(const char* text); // src/runtime_extra.c: live r.SetRes
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
enum StyleImage { StyleFrame, StyleCross, StyleCircle, StyleSquare, StyleL1, StyleR1, StyleImageCount };
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

// MediEvil render scale (Performance > Applied now): each change reallocates every scene target
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
                   Cross = 0x4000, Directions = Up | Right | Down | Left;
}

// Pad and keyboard navigation of the menu (research/docs/plan-settings-menu.md): the rows a tab draws are
// numbered in order; the focused one is highlighted, shows its help, takes left / right (changes its
// value) and Cross (changes it, or presses it) and is scrolled into view when the pad or keys moved the
// focus. The mouse focuses the row it moves over. ImGui's own navigation is off (Init).
struct MenuNav {
    int focus[TabCount] = {}; // the focused row of each tab, kept while the menu is closed
    int tab = 0;              // the tab being drawn
    int rows = 0;             // rows drawn so far this frame
    int last_rows = 0;        // rows drawn in the last frame (the pinned restart row included)
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

// The runtime's buttons for the menu (runtime_pad.c menu_buttons: pad, keys and mouse binds as
// mapped, fixed menu keys, the left stick as a D-pad), read every presented frame while it is open.
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
        return;
    }
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
    const ImGuiIO& io = ImGui::GetIO();
    if ((io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f) && ImGui::IsMouseHoveringRect(min, max, false) &&
        ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem)) {
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

// The menu in the game's look (choice_style ready; StyledMenu) for this frame. menu_u is the frame's
// scale (drawn height / 870, as `u` in StyledChoiceBox); the sizes below are in the frame's pixels.
bool styled = false;
float menu_u = 1.0f;
constexpr float RowHeight = 42.0f, InfoHeight = 34.0f, RowText = 28.0f, InfoText = 25.0f, NoteText = 23.0f,
                SectionText = 30.0f, ValueColumn = 0.55f, RightMargin = 18.0f;

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
    ImGui::Dummy(ImVec2(0.0f, Px(8.0f)));
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const float w = choice_style.text->CalcTextSizeA(size, FLT_MAX, 0.0f, heading.c_str()).x;
    ImGui::GetWindowDrawList()->AddText(choice_style.text, size, ImVec2(at.x + (width - w) * 0.5f, at.y), Col(Green),
                                        heading.c_str());
    ImGui::Dummy(ImVec2(width, size * 1.2f));
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
    const ImVec4 clip(r.min.x, r.min.y, r.value_x - Px(10.0f), r.max.y);
    draw->AddText(choice_style.text, size, ImVec2(r.min.x + Px(12.0f), r.mid - size * 0.55f),
                  Col(r.focused ? Gold : Cream), label, nullptr, 0.0f, &clip);
    return r;
}

void StyledRowHit(RowArea& r) {
    ImGui::SetCursorScreenPos(r.min);
    r.clicked = ImGui::InvisibleButton("##row", ImVec2(r.max.x - r.min.x, r.max.y - r.min.y));
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
    ImGui::InvisibleButton("##track", ImVec2(x1 - x0 + Px(24.0f), r.max.y - r.min.y));
    const bool held = ImGui::IsItemActive();
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
void IndexChoiceRow(const char* label, int& value, const char* const* labels, int count, const char* help) {
    if (const int chosen = ChoiceIndex(label, value, labels, count, help); chosen >= 0) {
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

void GameTab() {
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

void ImageTab() {
    namespace G = Gow3Graphics;
    auto& s = Gow3Settings::Get();
    Section("Display");
    if (BeginRows("##display")) {
        ChoiceRow("Display mode", s.display_mode, G::DisplayModeNames.data(), nullptr, G::DisplayModeCount,
                  "Windowed, borderless window or exclusive fullscreen.");
        CheckRow("VSync", s.vsync, "Waits for the monitor: no tearing, a little more input delay.");
        EndRows();
    }
    const int iw = s.image_width, ih = s.image_height, ww = s.window_width, wh = s.window_height;
    const bool upscaling = iw < ww && ih < wh;
    const bool downscaling = iw > ww || ih > wh;
    const bool rcas_blocked = s.rcas && !s.rcas_applied;
    Section("Scaling and sharpening");
    if (BeginRows("##scaling")) {
        CheckRow("Upscale with FSR 1", s.fsr1,
                 "Enlarges a smaller game image to the window. For more FPS: Render resolution 720p + FSR 1.");
        ImGui::BeginDisabled(rcas_blocked);
        CheckRow("Sharpening (RCAS)", s.rcas,
                 rcas_blocked && downscaling ? "No effect: the game's image is larger than the window."
                 : rcas_blocked              ? "Needs FSR 1: the game's image is smaller than the window."
                                             : "Sharpens the image: with AMD FSR it is FSR's own sharpening, "
                                               "otherwise (the game's TAA, NVIDIA DLSS) the final image.");
        ImGui::EndDisabled();
        ImGui::BeginDisabled(!s.rcas || rcas_blocked);
        SliderRow("Sharpening strength", s.rcas_strength, 0, 100, "%d%%", "How strong the sharpening is.", 5);
        ImGui::EndDisabled();
        {
            // The game's TAA, and the upscalers this system can run (Vulkan::Ue4Upscaler).
            static constexpr const char* names[] = {"Game's TAA", "NVIDIA DLSS", "AMD FSR"};
            const char* labels[Gow3Settings::Ue4UpscalerCount];
            int values[Gow3Settings::Ue4UpscalerCount];
            int count = 0;
            for (int kind = 0; kind < Gow3Settings::Ue4UpscalerCount; ++kind) {
                if (kind == 0 || (kind == 1 && s.ue4_dlss_available) || (kind == 2 && s.ue4_fsr_available)) {
                    labels[count] = names[kind];
                    values[count++] = kind;
                }
            }
            ChoiceRow("Anti-aliasing / upscaler", s.ue4_upscaler, labels, values, count,
                      "Replaces the game's anti-aliasing: native anti-aliasing at Render scale 100, upscaling "
                      "below it. Sharpening is off while it runs. F9 switches between them in the game. DLSS "
                      "needs an NVIDIA RTX GPU and nvngx_dlss.dll in the bin folder (see README-Windows.txt).");
        }
        EndRows();
    }
    Section("Information");
    if (BeginRows("##image_info")) {
        char text[96];
        std::snprintf(text, sizeof(text), "%d x %d", iw, ih);
        InfoRow("Game image", text);
        std::snprintf(text, sizeof(text), "%d x %d", ww, wh);
        InfoRow("Window", text);
        InfoRow("FSR 1", !s.fsr1 ? "off" : upscaling ? "active" : "no effect (image not smaller than window)");
        InfoRow("Sharpening", s.rcas_applied ? "applied" : "not applied");
        if (Vulkan::Ue4Upscaler::Running()) {
            std::snprintf(text, sizeof(text), "%s running", Vulkan::Ue4Upscaler::RunningName());
        }
        InfoRow("Upscaler", Vulkan::Ue4Upscaler::Running() ? text
                            : s.ue4_upscaler != 0          ? "chosen (waiting for the game's TAA)"
                                                           : "the game's TAA");
        EndRows();
    }
}

// MediEvil: the 3D scene's size per side compared with the output, in 1% steps, switched live
// (Gow3Settings::RequestRenderScale, applied by ApplyRenderScale) and kept in medievil.ini.
void RenderScaleSection() {
    namespace S = Gow3Settings;
    auto& s = S::Get();
    const int lo = S::LiveRenderScaleMin(), live_max = S::LiveRenderScaleMax();
    static char help[512];
    std::snprintf(help, sizeof(help),
                  "The 3D scene's size per side compared with the output, in 1%% steps. Below 100%%: drawn "
                  "smaller and rebuilt at full size by the game's temporal upscaler (faster). Above: drawn "
                  "larger and scaled down (sharper, slower). The HUD stays at the output size. Switched now "
                  "(each change re-makes the scene's buffers: one short hitch) from %d%% to %d%%; larger "
                  "values on the next start, as the game's memory is set up for the scene it starts with.",
                  lo, live_max);
    if (!BeginRows("##render_scale")) {
        return;
    }
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
    const int iw = s.image_width, ih = s.image_height;
    char text[96];
    if (iw > 0 && ih > 0) {
        std::snprintf(text, sizeof(text), "%d x %d at %d%% of %d x %d", S::ScaledSize(iw, s.live_render_scale),
                      S::ScaledSize(ih, s.live_render_scale), s.live_render_scale, iw, ih);
    } else {
        std::snprintf(text, sizeof(text), "%d%%", s.live_render_scale);
    }
    InfoRow("Scene now", text);
    EndRows();
    if (s.render_scale > live_max) {
        ImGui::TextWrapped("%d%% applies at the next start (Apply and restart). Until then the scene stays at "
                           "%d%%: live changes go up to %d%%, the scene size the game's memory was set up for "
                           "when it started.",
                           s.render_scale, s.live_render_scale, live_max);
    } else if (S::RenderScaleNeedsRestart()) {
        ImGui::TextWrapped("Anti-aliasing is not temporal (launcher > Performance): applied now, but the "
                           "anti-aliasing stays as the game started until the next start (below 100%% a start "
                           "always uses temporal AA for its upsampling).");
    }
}

extern "C" void runtime_memory_set_perf(int optimistic, int keep_access);

// MediEvil: the launcher's performance patches. Live ones apply at once.
void PerfPatchesSection() {
    namespace S = Gow3Settings;
    auto& s = S::Get();
    static const char* const titles[S::PerfCount] = {
        "HZB occlusion culling", "Skip the decal depth prepass", "Single GPU particle pass",
        "Small buffers from VRAM", "Texture set memo", "Lock-free memory map lookups", "Cheap memory frees",
        "GPU data without waits", "4K memory sizes", "Fewer render pass breaks", "Cheaper draw binding",
        "Constant ring reuse"};
    static const char* const hints[S::PerfCount] = {
        "Skips objects hidden behind others, without waiting for the GPU. Heavy areas ~130 -> ~165 FPS.",
        "No second depth-only drawing of the scene for nearly invisible decals: ~600 fewer draws, ~10%.",
        "One particle simulation pass a frame instead of an extra empty one: up to ~190 fewer draws. Off by "
        "default: some lone torches burn very slowly and an ember stream can show in Dan's crypt with it.",
        "Unchanged small buffers are read from video memory instead of copied every draw: ~5%.",
        "Remembered texture bindings survive other textures streaming in: binding ~7% cheaper.",
        "The GPU threads read the memory map without waiting for the game: fewer panning spikes.",
        "Freed memory keeps its page access: frees ~7x cheaper, smoother 0.1% lows.",
        "Readback queue, GPU write twins and deferred fences: removed the constant micro-stutter.",
        "More graphics memory and texture pool at 4K native: no reloads on small camera moves.",
        "No depth barriers between effect draws: fights' 0.1% lows 100 -> 117 at a 120 cap.",
        "Descriptors copied in one piece, samplers reused: ~0.3 ms less draw-thread work in heavy frames.",
        "Constants bound again in a command buffer reuse their first copy: heavy-area 0.1% lows ~145 -> ~185."};
    Section("Performance patches");
    if (BeginRows("##perf_live")) {
        for (int p = 0; p < S::PerfCount; ++p) {
            if (S::PerfLive[p]) CheckRow(titles[p], s.perf[p], hints[p]);
        }
        EndRows();
    }
    // The live ones in the memory runtime (src/runtime_memory.c); the buffer cache reads its own.
    runtime_memory_set_perf(s.perf[S::PerfLockfreeMemory] ? 1 : 0, s.perf[S::PerfCheapFrees] ? 1 : 0);
    // The particle pass through its console variable at the next game-thread poll (once per change).
    runtime_ue4_particle_single_pass(s.perf[S::PerfParticleSinglePass] ? 1 : 0);
    if (Gow3Toggle2::Disabled(Gow3Toggle2::ParticleFixedStep)) {
        ImGui::TextDisabled("Single GPU particle pass: held off by the toggle file (second word 8192); this "
                            "choice applies when that bit clears.");
    }
    ImGui::TextDisabled("Applied on restart:");
    if (BeginRows("##perf_restart")) {
        for (int p = 0; p < S::PerfCount; ++p) {
            if (!S::PerfLive[p]) CheckRow(titles[p], s.perf[p], hints[p]);
        }
        EndRows();
    }
}

void PerformanceTab() {
    namespace G = Gow3Graphics;
    auto& s = Gow3Settings::Get();
    Section("Live");
    if (BeginRows("##live")) {
        static const char* const fps_labels[] = {"Auto: refresh (VRR: below it, Reflex rule)",
                                                 "Auto: half refresh (VRR: one below)", "30", "60", "119 (VRR, 240 Hz)",
                                                 "120", "141 (VRR, 144 Hz)", "237 (VRR, 240 Hz)", "240", "Unlimited",
                                                 "Half display refresh (fixed refresh)"};
        ChoiceRow("Frame rate limit", s.fps_limit, fps_labels, G::FpsLimits.data(), int(G::FpsLimits.size()),
                  "Caps the frame rate. The engine frame rate (below) is the highest it can go.");
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
        static const char* const queued_labels[] = {"1 (lowest latency)", "2", "Unlimited"};
        ChoiceRow("Frames queued", s.frames_queued, queued_labels, G::FramesQueued.data(),
                  int(G::FramesQueued.size()), "More frames queued: steadier FPS, slower response to input.");
        CheckRow("Compile new shaders in the background", s.async_shaders,
                 "Fewer stutters in new areas; an object or effect may appear a moment later.");
        EndRows();
    }
    if (!IsGow3()) {
        // MediEvil: the game's own resolution, switched live (as its debug menu does). The debug
        // menu's gamepad navigation needs 1080p: switch there for cheats, then back.
        Section("Game resolution now");
        static constexpr const char* labels[] = {"1080p", "1440p", "1800p", "4K"};
        static constexpr const char* values[] = {"1920x1080", "2560x1440", "3200x1800", "3840x2160"};
        // Render scales above 100% supersample (r.ScreenPercentage up to 200): a larger output on
        // top of that ran out of the PS4 memory budget (1800p at 200% needs 6400x3600 targets:
        // crash). Those keep 1080p here.
        // A live render scale stays at or below the start's when that is above 100% (RequestRenderScale).
        const bool supersampling = s.startup_render_scale > 100;
        if (BeginRows("##game_resolution")) {
            const bool enabled[] = {true, !supersampling, !supersampling, !supersampling};
            if (const int pressed = ButtonsRow("Switch the output to", labels, 4, enabled,
                                               "The game's output size now, until the next start.");
                pressed >= 0) {
                runtime_ue4_request_setres(values[pressed]);
            }
            EndRows();
        }
        ImGui::TextWrapped("%s", supersampling
                                     ? "Unavailable when the game started with a render scale above 100% (the "
                                       "scene is already drawn larger; more would run out of game memory). Start "
                                       "with Render scale 100% or lower to switch the output size here."
                                     : "Switches the game's output now, until the next start (Output resolution "
                                       "below is the size it starts at). Use 1080p for the debug menu (R3), then "
                                       "switch back.");
        // UE4 console variables switched live (src/runtime_extra.c), kept in medievil.ini.
        Section("Applied now");
        RenderScaleSection();
        if (BeginRows("##live")) {
            const bool preshadows = s.preshadows;
            CheckRow("Preshadows", s.preshadows,
                     "Shadows that Dan and enemies cast from fixed lights such as torches. Off: "
                     "faster in busy views with many lights (the Graveyard horizon held the frame "
                     "cap); some of those shadows go.");
            if (s.preshadows != preshadows) {
                runtime_ue4_request_cvar("r.Shadow.Preshadows", s.preshadows ? "1" : "0");
            }
            static const char* const effects_labels[] = {"100%", "75%", "50%"};
            const int effects = s.effects_resolution;
            IndexChoiceRow("Effects resolution", s.effects_resolution, effects_labels, 3,
                           "Smoke, fire and enemy death effects drawn at a lower resolution and "
                           "blended in. At 4K, 50% is the PS4's 1080p effect detail; several deaths "
                           "at once cost 2-3 ms a frame at 100%. The cost grows with the render resolution.");
            if (s.effects_resolution != effects && s.effects_resolution >= 0) {
                runtime_ue4_request_cvar("r.SeparateTranslucencyScreenPercentage",
                                         Gow3Settings::UE4EffectsKeys[s.effects_resolution]);
            }
            // Graphics quality sets every graphics setting (the live ones now, the others below at the next
            // start); it shows the level they all match, else Custom (research/docs/plan-graphics-settings.md).
            {
                namespace S = Gow3Settings;
                static const char* const quality_labels[] = {"Ultra", "High", "Console (PS4)", "Medium",
                                                             "Low (integrated graphics)", "Deck Optimized"};
                const int matched = S::UE4MatchedLevel(s, s.ue4_quality);
                const int level = ChoiceIndex(
                    "Graphics quality", matched, quality_labels, int(std::size(S::UE4QualityKeys)),
                    "Sets every graphics setting: Console is exactly the PS4, High and Ultra go beyond it, "
                    "Medium and Low are for integrated graphics. Custom: some settings changed by hand. "
                    "The settings under \"Applied on restart\" take effect at the next start.",
                    "Custom");
                if (level >= 0) {
                    S::UE4ApplyLevel(level);
                    dirty = true;
                }
            }
            for (int o = 0; o < Gow3Settings::UE4LiveCount; ++o) {
                const auto& option = Gow3Settings::UE4LiveOptions[o];
                if (!option.live) continue; // under "Applied on restart"
                const int before = s.ue4_live[o];
                IndexChoiceRow(option.title, s.ue4_live[o], option.labels, option.count, option.hint);
                if (s.ue4_live[o] != before) {
                    Gow3Settings::RequestUE4Live(o, s.ue4_live[o]);
                }
            }
            EndRows();
        }
    }
    Section("Applied on restart");
    if (BeginRows("##restart")) {
        if (IsGow3()) {
            // God of War III: a resolution patch.
            static const char* const resolution_labels[] = {
                "1080p (original)", "480p", "720p", "1440p", "1800p", "4K", "720p", "1440p", "1800p", "4K"};
            ChoiceRow("Render resolution", s.render_resolution, resolution_labels, nullptr,
                      int(G::Resolutions.size()), "The resolution the game draws at. Higher is sharper and slower.");
        } else {
            // MediEvil: the size the game starts at (launcher > Performance; run.py: the boot display
            // size patch). Its render scale is live, above (RenderScaleSection).
            static const char* const output_labels[] = {"Match display", "4K (3840x2160)", "1800p (3200x1800)",
                                                        "1440p (2560x1440)", "1080p (original)", "720p (1280x720)"};
            IndexChoiceRow("Output resolution", s.output_resolution, output_labels, 6,
                           "The size of the game's picture; it starts at this size. Match the display for a "
                           "sharp 1:1 image. Custom sizes: output_resolution=WxH in medievil.ini.");
        }
        // The highest frame rate the game runs at (Unreal Engine 4: run.py's override pak).
        static const char* const engine_labels[] = {"60 (original)", "120 (experimental)", "240 (experimental)"};
        ChoiceRow("Engine frame rate", s.engine_fps, engine_labels, G::EngineFps.data(),
                  int(G::EngineFps.size()), "The highest frame rate the game itself runs at.");
        CheckRow("Game reads GPU data without waiting", s.stale_readback,
                 "Big FPS gain. Switch it off if lighting or objects look wrong.");
        CheckRow("Deferred GPU readbacks", s.deferred_readback,
                 "Fewer waits for the GPU. Switch it off if textures or shadows look wrong.");
        if (!IsGow3()) {
            // MediEvil: the graphics settings that are not live (Graphics quality, above, sets them too).
            for (int o = 0; o < Gow3Settings::UE4LiveCount; ++o) {
                const auto& option = Gow3Settings::UE4LiveOptions[o];
                if (!option.live) {
                    IndexChoiceRow(option.title, s.ue4_live[o], option.labels, option.count, option.hint);
                }
            }
        }
        EndRows();
    }
    if (!IsGow3()) {
        PerfPatchesSection();
    }
}

// The performance overlay as it will look, at its own font size (Font size: overlay_scale).
void OverlayPreview() {
    const auto& s = Gow3Settings::Get();
    ImGui::TextDisabled("Preview");
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

// The tab's content.
void TabBody(int tab) {
    nav.in_body = true;
    switch (tab) {
    case 0: GameTab(); break;
    case 1: ControlsTab(); break;
    case 2: ImageTab(); break;
    case 3: PerformanceTab(); break;
    default: OverlayTab(); break;
    }
    nav.in_body = false;
    nav.body_rows = nav.rows;
}

// "Apply and restart" asks for a second press within 3 s: it ends the game without saving.
std::chrono::steady_clock::time_point restart_armed{};

bool RestartArmed() {
    return std::chrono::steady_clock::now() < restart_armed;
}

void RestartPressed() {
    if (!RestartArmed()) {
        restart_armed = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        return;
    }
    // The new launch confirms the options once the game shows; run.py restores the
    // previous ones if it never does (restart_unconfirmed).
    Gow3Settings::Get().restart_unconfirmed = true;
    Gow3Settings::Save();
    runtime_restart();
}

constexpr const char* RestartHelp = "Starts the game again with the new settings. Progress since the last save is lost.";

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
        static const char* const tabs[TabCount] = {"Game", "Controls", "Image", "Performance", "Overlay"};
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
    if (changes && NextRowFocused()) {
        footer_help = RestartHelp; // the restart row below has the focus
    }
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("%s", footer_help ? footer_help
                                          : "D-pad or arrows: choose and change. Cross (A) / Enter: change. L1 / R1 "
                                            "or Q / E: switch tabs. Circle (B) / Esc: close. Settings are saved in "
                                            "medievil.ini.");
    ImGui::PopTextWrapPos();
    if (changes) {
        ImGui::TextColored(ImVec4(0.85f, 0.72f, 0.45f, 1.0f), "%d change%s need%s a restart (unsaved progress is lost).",
                           changes, changes == 1 ? "" : "s", changes == 1 ? "s" : "");
        ImGui::SameLine();
        const bool focused = NextRowFocused();
        ImGui::BeginDisabled(!std::getenv("GOW3_RESTART_COMMAND"));
        bool pressed = ImGui::Button(RestartArmed() ? "Press again to restart" : "Apply and restart");
        if (focused) {
            ImGui::GetWindowDrawList()->AddRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(),
                                                IM_COL32(245, 196, 30, 255), ImGui::GetStyle().FrameRounding, 0,
                                                2.0f * base_scale);
        }
        pressed |= NavRow(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), RestartHelp, LastItemEnabled()).press;
        ImGui::EndDisabled();
        if (pressed) {
            RestartPressed();
        }
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
// fonts, colours and prompts. The title, the tabs between the L1 / R1 prompts, the tab's rows in a
// scrolled body, the restart row while changes wait for a start, the focused row's help, the prompts
// under the frame (clickable). Drawn in the frame's own pixels (1181x870), so it keeps its proportions.
void StyledMenu() {
    auto& s = Gow3Settings::Get();
    if (requested_tab >= 0) {
        s.menu_tab = requested_tab;
        dirty = true;
        requested_tab = -1;
    }
    const int tab = std::clamp(s.menu_tab.load(), 0, TabCount - 1);
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    constexpr float FrameW = 1181.0f, FrameH = 870.0f, PromptsH = 72.0f;
    // 86% of the height from 3% down (the Load list: 76% from 6%), the prompts under the frame.
    const float width = std::min(viewport->WorkSize.y * 0.86f * FrameW / FrameH, viewport->WorkSize.x * 0.96f);
    const float height = width * FrameH / FrameW;
    menu_u = height / FrameH;
    const ImVec2 at(std::floor(viewport->WorkPos.x + (viewport->WorkSize.x - width) * 0.5f),
                    std::floor(viewport->WorkPos.y + viewport->WorkSize.y * 0.03f));
    const auto P = [&](float x, float y) { return ImVec2(at.x + x * menu_u, at.y + y * menu_u); };
    const auto text_width = [](ImFont* face, float size, const char* text) {
        return face->CalcTextSizeA(size, FLT_MAX, 0.0f, text).x;
    };
    ImGui::SetNextWindowPos(at, ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(width, height + Px(PromptsH)), ImGuiCond_Always);
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
    // The frame's middle filled opaque as in the Options menu, the frame over it, the title.
    draw->AddRectFilled(P(20, 106), P(1161, 768), PanelFill, Px(34.0f));
    draw->AddImage(choice_style.textures[StyleFrame]->GetTexRef(), at, ImVec2(at.x + width, at.y + height));
    {
        const char* title = "-SETTINGS-";
        const float size = TextPx(66.0f);
        draw->AddText(choice_style.title, size,
                      ImVec2(at.x + (width - text_width(choice_style.title, size, title)) * 0.5f, P(0, 112).y), White,
                      title);
    }

    // The tabs between the L1 and R1 prompts (each clickable); the current one Gold and underlined.
    {
        static const char* const tabs[TabCount] = {"GAME", "CONTROLS", "IMAGE", "PERFORMANCE", "OVERLAY"};
        float size = TextPx(28.0f), icon = Px(44.0f), gap = Px(22.0f);
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
        const float y = P(0, 198).y, line = y + size * 0.5f;
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
        draw->AddLine(P(64, 248), P(1117, 248), IM_COL32(245, 196, 30, 70), Px(2.0f));
    }

    // The game's button prompts under the frame, as the Load list's; drawn before the rows so a click on
    // SELECT reaches the focused row in this frame.
    {
        struct Prompt {
            StyleImage icons[2];
            int count;
            const char* label;
            uint32_t press;
        };
        const Prompt prompts[] = {{{StyleCross, StyleCross}, 1, "SELECT", Ps4::Cross},
                                  {{StyleCircle, StyleCircle}, 1, "GO BACK", Ps4::Circle},
                                  {{StyleL1, StyleR1}, 2, "CHANGE TAB", Ps4::R1}};
        constexpr float Icon = 50.0f, Label = 36.0f, Space = 70.0f, Pair = 6.0f;
        const float label_size = TextPx(Label);
        float row = 0.0f;
        for (const Prompt& prompt : prompts) {
            row += (Icon + Pair) * float(prompt.count) - Pair + 12.0f +
                   text_width(choice_style.text, label_size, prompt.label) / menu_u + Space;
        }
        float x = (FrameW - (row - Space)) * 0.5f;
        const float top = FrameH + 8.0f;
        for (const Prompt& prompt : prompts) {
            const float start = x;
            for (int i = 0; i < prompt.count; ++i) {
                draw->AddImage(choice_style.textures[prompt.icons[i]]->GetTexRef(), P(x, top), P(x + Icon, top + Icon));
                x += Icon + Pair;
            }
            x += 12.0f - Pair;
            draw->AddText(choice_style.text, label_size, P(x, top + (Icon - Label) * 0.5f - 2.0f), White, prompt.label);
            x += text_width(choice_style.text, label_size, prompt.label) / menu_u;
            ImGui::SetCursorScreenPos(P(start, top));
            if (ImGui::InvisibleButton(prompt.label, ImVec2(Px(x - start), Px(Icon)))) {
                MenuPress(prompt.press);
            }
            x += Space;
        }
    }

    // The tab's rows, scrolled, with a thin scroll bar when they do not fit (as the Load list's).
    const int changes = RestartChanges();
    constexpr float BodyX = 64.0f, BodyW = 1053.0f, BodyTop = 258.0f;
    const float body_bottom = changes ? 598.0f : 640.0f;
    ImGui::SetCursorScreenPos(P(BodyX, BodyTop));
    PushMenuStyle();
    if (ImGui::BeginChild("##body", ImVec2(Px(BodyW), Px(body_bottom - BodyTop)), ImGuiChildFlags_None,
                          ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar)) {
        NavStartTab(tab);
        TabBody(tab);
        if (const float max_scroll = ImGui::GetScrollMaxY(); max_scroll > 0.0f) {
            const ImVec2 body = ImGui::GetWindowPos();
            const float view = ImGui::GetWindowHeight(), bar_w = Px(5.0f);
            const float bar = std::max(view * view / (view + max_scroll), Px(30.0f));
            const float top = body.y + (view - bar) * ImGui::GetScrollY() / max_scroll;
            const float x = body.x + ImGui::GetWindowWidth() - bar_w;
            ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(x, top), ImVec2(x + bar_w, top + bar),
                                                      IM_COL32(200, 196, 190, 120), Px(3.0f));
        }
    }
    ImGui::EndChild();

    // The restart row (focusable, after the tab's rows) while changes wait for a start.
    if (changes) {
        char label[64];
        std::snprintf(label, sizeof(label), "%d change%s need%s a restart", changes, changes == 1 ? "" : "s",
                      changes == 1 ? "s" : "");
        ImGui::SetCursorScreenPos(P(BodyX, 600));
        ImGui::BeginDisabled(!std::getenv("GOW3_RESTART_COMMAND"));
        RowArea r = BeginStyledRow(label, Px(BodyW));
        StyledRowHit(r);
        DrawChoice(r, RestartArmed() ? "PRESS AGAIN TO RESTART" : "APPLY AND RESTART", false);
        const RowNav in = StyledRowEnd(r, RestartHelp);
        ImGui::PopID();
        ImGui::EndDisabled();
        if (r.clicked || in.press) {
            RestartPressed();
        }
    }
    PopMenuStyle();

    // The focused row's help (the rows set it), else how to open and close the menu.
    {
        static const std::string idle = OpenKeys() + ": open and close this menu. Settings are saved in medievil.ini.";
        const char* help = footer_help ? footer_help : idle.c_str();
        const float size = TextPx(24.0f);
        const ImVec2 from = P(72, 650), to = P(1110, 764);
        const ImVec4 clip(from.x, from.y, to.x, to.y);
        draw->AddText(choice_style.text, size, from, footer_help ? Cream : Dim, help, nullptr, to.x - from.x, &clip);
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
    ImGui::TextDisabled("Up/Down: choose   Cross (A) / Enter: load   Circle (B) / Esc: cancel   Square (X) / Tab: %s",
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
    ImGui::TextDisabled("Up/Down or D-pad: choose   Enter or Cross (A): OK   Esc or Circle (B): cancel");
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

bool HandleChoiceEvent(const SDL_Event& event) {
    int move = 0, finish = 0; // finish: 1 accept, 2 cancel
    bool toggle = false;      // Square / Tab: the details' other page
    if (event.type == SDL_EVENT_KEY_DOWN) {
        const SDL_Keycode key = event.key.key;
        toggle = key == SDLK_TAB;
        move = key == SDLK_UP || key == SDLK_W ? -1 : key == SDLK_DOWN || key == SDLK_S ? 1 : 0;
        finish = key == SDLK_RETURN || key == SDLK_KP_ENTER || key == SDLK_SPACE ? 1
                 : key == SDLK_ESCAPE || key == SDLK_BACKSPACE                   ? 2
                                                                                 : 0;
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
        // The menu's keys come from the runtime's poll (MenuButtons: Esc closes there). Without it,
        // from these events; Esc while a number box is typed in is ImGui's (cancels the edit).
        if (down && !menu_input && !io.WantTextInput) {
            switch (event.key.key) {
            case SDLK_UP: MenuPress(Ps4::Up); break;
            case SDLK_DOWN: MenuPress(Ps4::Down); break;
            case SDLK_LEFT: MenuPress(Ps4::Left); break;
            case SDLK_RIGHT: MenuPress(Ps4::Right); break;
            case SDLK_RETURN:
            case SDLK_KP_ENTER: MenuPress(Ps4::Cross); break;
            case SDLK_ESCAPE: MenuPress(Ps4::Circle); break;
            case SDLK_Q: MenuPress(Ps4::L1); break;
            case SDLK_E: MenuPress(Ps4::R1); break;
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
    return initialized && (loading || background_total || menu_open || text_entry_active || choice_active ||
                           runtime_disabled_optimizations != 0 ||
                           choice_retiring || s.show_fps || Vulkan::Ue4Upscaler::Notice() ||
                           (s.actor_watch_supported && s.enemy_health_bar));
}

bool CapturesInput() {
    return menu_open || text_entry_active || choice_active;
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
