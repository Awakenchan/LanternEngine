// SPDX-License-Identifier: GPL-2.0-or-later
#include "bbport_overlay.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <map>
#include <cstring>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <cctype>
#include <cfloat>
#include <string>
#include <vector>

#include <SDL3/SDL.h>
#include "../bbnet/room/room_service.h"
#include "bbport_menu_i18n.h"
#include "bbport_settings.h"
#include "bbport_usage.h"
#include "bbport_looks.h"
#include "engine/cheats.h"
#include "engine/graphics.h"
#include "engine/online_fixes.h"
#include "input/input.h"
#include "voice.h"
#include "hud_gauge.h"
#include "face_shot.h"
#include "imgui_internal.h"
#include "imgui.h"
#include "imgui_impl_vulkan.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_optiscaler.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

#ifdef _WIN32
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
alignas(16) static const unsigned char bb_font_ttf[] = {
#embed BB_FONT_PATH
};
#pragma GCC diagnostic pop
static const unsigned char* const bb_font_ttf_end = bb_font_ttf + sizeof(bb_font_ttf);
#else
asm(".section .rodata\n"
    ".balign 16\n"
    ".hidden bb_font_ttf\n"
    ".global bb_font_ttf\n"
    "bb_font_ttf:\n"
    ".incbin \"" BB_FONT_PATH "\"\n"
    ".hidden bb_font_ttf_end\n"
    ".global bb_font_ttf_end\n"
    "bb_font_ttf_end:\n"
    ".previous\n");
extern "C" const unsigned char bb_font_ttf[];
extern "C" const unsigned char bb_font_ttf_end[];
#endif

#ifdef _WIN32
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
alignas(16) static const unsigned char bb_font_cjk[] = {
#embed BB_CJK_FONT_PATH
};
#pragma GCC diagnostic pop
static const unsigned char* const bb_font_cjk_end = bb_font_cjk + sizeof(bb_font_cjk);
#else
asm(".section .rodata\n"
    ".balign 16\n"
    ".hidden bb_font_cjk\n"
    ".global bb_font_cjk\n"
    "bb_font_cjk:\n"
    ".incbin \"" BB_CJK_FONT_PATH "\"\n"
    ".hidden bb_font_cjk_end\n"
    ".global bb_font_cjk_end\n"
    "bb_font_cjk_end:\n"
    ".previous\n");
extern "C" const unsigned char bb_font_cjk[];
extern "C" const unsigned char bb_font_cjk_end[];
#endif

#ifdef _WIN32
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
alignas(16) static const unsigned char bb_font_name[] = {
#embed BB_NAME_FONT_PATH
};
#pragma GCC diagnostic pop
static const unsigned char* const bb_font_name_end = bb_font_name + sizeof(bb_font_name);
#else
asm(".section .rodata\n"
    ".balign 16\n"
    ".hidden bb_font_name\n"
    ".global bb_font_name\n"
    "bb_font_name:\n"
    ".incbin \"" BB_NAME_FONT_PATH "\"\n"
    ".hidden bb_font_name_end\n"
    ".global bb_font_name_end\n"
    "bb_font_name_end:\n"
    ".previous\n");
extern "C" const unsigned char bb_font_name[];
extern "C" const unsigned char bb_font_name_end[];
#endif

extern "C" void runtime_restart(void);

namespace Vulkan {
extern std::atomic<u64> g_bb_compile_ns_total;
extern std::atomic<u32> g_bb_compiles_total;
}

namespace BbOverlay {

namespace {

std::mutex imgui_mutex;
bool initialized = false;
std::atomic<bool> menu_open{false};
bool l3_down = false, r3_down = false;
std::atomic<bool> coop_open{false};
std::atomic<bool> binding_talk_key{false};
int coop_row = 0, coop_column = 0;
std::string coop_armed;
std::chrono::steady_clock::time_point coop_armed_at{};
bool dirty = false;
float base_scale = 1.0f;
ImFont* name_font = nullptr;

std::mutex prompt_mutex;
std::atomic<bool> prompt_active{false};
std::string prompt_title, prompt_text;

std::chrono::steady_clock::time_point last_present{};
float frame_ms_avg = 0.0f;

float PixelDensity(SDL_WindowID id);

void SetOpen(bool value) {
    if (menu_open.exchange(value) == value) {
        return;
    }
    if (value) {
        if (SDL_Window* window = SDL_GetMouseFocus()) {
            float x = 0.0f, y = 0.0f;
            SDL_GetMouseState(&x, &y);
            const float density = PixelDensity(SDL_GetWindowID(window));
            ImGui::GetIO().AddMousePosEvent(x * density, y * density);
        }
    }
    if (!value && dirty) {
        dirty = false;
        BbSettings::Save();
    }
}

ImGuiKey KeyFromSdl(SDL_Keycode key) {
    if (key >= SDLK_A && key <= SDLK_Z) {
        return ImGuiKey(ImGuiKey_A + int(key - SDLK_A));
    }
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
    case SDLK_LCTRL: return ImGuiKey_LeftCtrl;
    case SDLK_RCTRL: return ImGuiKey_RightCtrl;
    case SDLK_LSHIFT: return ImGuiKey_LeftShift;
    case SDLK_RSHIFT: return ImGuiKey_RightShift;
    case SDLK_LALT: return ImGuiKey_LeftAlt;
    case SDLK_RALT: return ImGuiKey_RightAlt;
    default: return ImGuiKey_None;
    }
}

ImGuiKey KeyFromGamepad(u8 button) {
    switch (button) {
    case SDL_GAMEPAD_BUTTON_SOUTH: return ImGuiKey_GamepadFaceDown;
    case SDL_GAMEPAD_BUTTON_EAST: return ImGuiKey_GamepadFaceRight;
    case SDL_GAMEPAD_BUTTON_WEST: return ImGuiKey_GamepadFaceLeft;
    case SDL_GAMEPAD_BUTTON_NORTH: return ImGuiKey_GamepadFaceUp;
    case SDL_GAMEPAD_BUTTON_DPAD_UP: return ImGuiKey_GamepadDpadUp;
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return ImGuiKey_GamepadDpadDown;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return ImGuiKey_GamepadDpadLeft;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return ImGuiKey_GamepadDpadRight;
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return ImGuiKey_GamepadL1;
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return ImGuiKey_GamepadR1;
    case SDL_GAMEPAD_BUTTON_START: return ImGuiKey_GamepadStart;
    case SDL_GAMEPAD_BUTTON_BACK: return ImGuiKey_GamepadBack;
    default: return ImGuiKey_None;
    }
}

float PixelDensity(SDL_WindowID id) {
    SDL_Window* window = SDL_GetWindowFromID(id);
    const float density = window ? SDL_GetWindowPixelDensity(window) : 1.0f;
    return density > 0.0f ? density : 1.0f;
}

template <typename T>
void Store(std::atomic<T>& target, T value, bool changed) {
    if (changed) {
        target = value;
        dirty = true;
    }
}

void Checkbox(const char* label, std::atomic<bool>& value) {
    bool v = value;
    const bool changed = ImGui::Checkbox(label, &v);
    Store(value, v, changed);
}

bool Slider(const char* label, std::atomic<float>& value, float lo, float hi,
            const char* format = "%.2f", ImGuiSliderFlags flags = ImGuiSliderFlags_None) {
    float v = value;
    const bool changed = ImGui::SliderFloat(label, &v, lo, hi, format, flags);
    Store(value, v, changed);
    return changed;
}

int custom_vials = 20, custom_bullets = 20;

void Cheat(const char* label, std::atomic<int>& value, BbEngine::Cheats::Which which, int& custom) {
    ImGui::PushID(label);
    const char* modes[] = {BbSettings::MenuText("Off", "Выкл."),
                           BbSettings::MenuText("Infinite", "Бесконечно"),
                           BbSettings::MenuText("Custom", "Своё количество")};
    const int current = value;
    int mode = current == BbSettings::CheatOff ? 0 : current == BbSettings::CheatInfinite ? 1 : 2;
    if (current > 0) {
        custom = current;
    }
    if (ImGui::Combo(label, &mode, modes, 3)) {
        Store(value, mode == 0 ? BbSettings::CheatOff : mode == 1 ? BbSettings::CheatInfinite : custom, true);
    }
    if (mode == 2) {
        int amount = std::clamp(custom, 1, BbSettings::CheatMaxAmount);
        const bool changed = ImGui::SliderInt(BbSettings::MenuText("Amount", "Количество"), &amount, 1,
                                              BbSettings::CheatMaxAmount, "%d", ImGuiSliderFlags_AlwaysClamp);
        if (changed) {
            custom = std::clamp(amount, 1, BbSettings::CheatMaxAmount);
            Store(value, custom, true);
        }
    }
    if (const char* why = BbEngine::Cheats::Unavailable(which)) {
        ImGui::TextDisabled("%s: %s", BbSettings::MenuText("Unavailable", "Недоступно"), BbSettings::MenuText(why));
    }
    ImGui::PopID();
}

ImVec2 last_hint{-1.0f, -1.0f};

void Hint(const char* text) {
    const ImVec2 setting_at = ImGui::GetItemRectMin();
    const bool setting = (setting_at.x != last_hint.x || setting_at.y != last_hint.y) &&
                         ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip);
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    last_hint = ImGui::GetItemRectMin();
    if (ImGui::BeginItemTooltip() || (setting && ImGui::BeginTooltip())) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0f);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

void CheatUnavailable(BbEngine::Cheats::Which which) {
    if (const char* why = BbEngine::Cheats::Unavailable(which)) {
        ImGui::TextDisabled("%s: %s", BbSettings::MenuText("Unavailable", "Недоступно"), BbSettings::MenuText(why));
    }
}

void CheatToggle(const char* label, std::atomic<bool>& value, BbEngine::Cheats::Which which, const char* hint) {
    ImGui::PushID(label);
    Checkbox(label, value);
    Hint(hint);
    CheatUnavailable(which);
    ImGui::PopID();
}

template <std::size_t N>
void CheatLevels(const char* label, std::atomic<int>& value, const int (&levels)[N], const char* const (&names)[N],
                 BbEngine::Cheats::Which which, const char* hint) {
    ImGui::PushID(label);
    const int current = value;
    int index = 0;
    for (std::size_t i = 0; i < N; ++i) {
        if (levels[i] == current) {
            index = int(i);
        }
    }
    if (ImGui::Combo(label, &index, names, int(N))) {
        Store(value, levels[index], true);
    }
    Hint(hint);
    CheatUnavailable(which);
    ImGui::PopID();
}

void CheatStats() {
    const bool off = BbEngine::Cheats::Unavailable(BbEngine::Cheats::Echoes) != nullptr;
    ImGui::BeginDisabled(off);
    ImGui::TextUnformatted(BbSettings::MenuText("Blood Echoes", "Отголоски крови"));
    for (int amount : {1000, 10000, 100000}) {
        char label[32];
        std::snprintf(label, sizeof(label), "+%d,%03d", amount / 1000, 0);
        ImGui::SameLine();
        if (ImGui::Button(label)) {
            BbEngine::Cheats::AddEchoes(amount);
        }
    }
    ImGui::SameLine();
    if (ImGui::Button(BbSettings::MenuText("+1 Insight", "+1 озарение"))) {
        BbEngine::Cheats::AddInsight(1);
    }
    ImGui::EndDisabled();
    Hint(BbSettings::MenuText(
        "Added to the character in the world, up to the game's limits (999,999,999 Blood Echoes, 99 Insight). "
        "They are saved with your character: turning cheats off does not take them back.",
        "Добавляются персонажу в игре, до пределов игры (999 999 999 отголосков, 99 озарения). Они "
        "сохраняются вместе с персонажем: выключение читов их не отнимает."));
    CheatUnavailable(BbEngine::Cheats::Echoes);
}

char search_text[96] = "";
std::string search_query;
const char* tab_title = "";
const char* section_title = nullptr;
bool section_matches = false;
int shown_items = 0;

void AppendFolded(std::string& out, char32_t c) {
    if ((c >= 'A' && c <= 'Z') || (c >= 0xC0 && c <= 0xDE && c != 0xD7) ||
        (c >= 0x391 && c <= 0x3AB && c != 0x3A2) || (c >= 0x410 && c <= 0x42F)) {
        c += 32;
    } else if (c >= 0x400 && c <= 0x40F) {
        c += 80;
    } else if (c == 0x130 || c == 0x131) {
        c = 'i';
    } else if ((c >= 0x100 && c <= 0x137) || (c >= 0x14A && c <= 0x177)) {
        c |= 1;
    } else if ((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17E)) {
        c += c & 1;
    } else if (c == 0x178) {
        c = 0xFF;
    }
    if (c < 0x80) {
        out += char(c);
    } else if (c < 0x800) {
        out += char(0xC0 | (c >> 6));
        out += char(0x80 | (c & 0x3F));
    } else if (c < 0x10000) {
        out += char(0xE0 | (c >> 12));
        out += char(0x80 | ((c >> 6) & 0x3F));
        out += char(0x80 | (c & 0x3F));
    } else {
        out += char(0xF0 | (c >> 18));
        out += char(0x80 | ((c >> 12) & 0x3F));
        out += char(0x80 | ((c >> 6) & 0x3F));
        out += char(0x80 | (c & 0x3F));
    }
}

std::string Fold(const char* text) {
    std::string out;
    const auto* p = reinterpret_cast<const unsigned char*>(text);
    while (*p && !(p[0] == '#' && p[1] == '#')) {
        char32_t c = *p++;
        int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
        c &= extra == 3 ? 0x07 : extra == 2 ? 0x0F : extra == 1 ? 0x1F : 0x7F;
        for (; extra > 0 && (*p & 0xC0) == 0x80; --extra) {
            c = (c << 6) | (*p++ & 0x3F);
        }
        AppendFolded(out, c);
    }
    const auto first = out.find_first_not_of(' ');
    const auto last = out.find_last_not_of(' ');
    return first == std::string::npos ? std::string{} : out.substr(first, last - first + 1);
}

bool Holds(const char* text) {
    return text && Fold(text).find(search_query) != std::string::npos;
}

bool Matches(const char* english, const char* russian) {
    if (Holds(english) || (russian && Holds(russian))) {
        return true;
    }
    for (int l = 0; l < BbSettings::MenuLanguageCount; ++l) {
        const char* text = BbSettings::MenuTranslation(BbSettings::MenuLanguage(l), english);
        if (text != english && Holds(text)) {
            return true;
        }
    }
    return false;
}

bool Searching() {
    return !search_query.empty();
}

void Section(const char* english, const char* russian = nullptr) {
    section_title = russian ? BbSettings::MenuText(english, russian) : BbSettings::MenuText(english);
    section_matches = Searching() && Matches(english, russian);
}

bool WholeSection() {
    return !Searching() || section_matches;
}

bool Shown(const char* english, const char* russian = nullptr) {
    if (!WholeSection() && !Matches(english, russian)) {
        return false;
    }
    if (section_title) {
        if (Searching()) {
            const std::string title = std::string(tab_title) + "  >  " + section_title;
            ImGui::SeparatorText(title.c_str());
        } else {
            ImGui::SeparatorText(section_title);
        }
        section_title = nullptr;
    }
    ++shown_items;
    return true;
}

const char* Item(const char* english, const char* russian = nullptr) {
    if (!Shown(english, russian)) {
        return nullptr;
    }
    return russian ? BbSettings::MenuText(english, russian) : BbSettings::MenuText(english);
}

void HdrSection() {
    auto& s = BbSettings::Get();
    Section("HDR output", "Вывод HDR");
    if (const char* label = Item("HDR output (restart required)", "Вывод HDR (после перезапуска)")) {
        Checkbox(label, s.hdr);
        Hint(BbSettings::MenuText(
            "Shows the game's SDR picture on an HDR display without washed-out colors, with optional "
            "highlight expansion. Windows HDR must be on (Settings > Display > Use HDR). Turning it "
            "on or off applies after restarting the game.",
            "Показывает SDR-картинку игры на HDR-дисплее без блёклых цветов, с необязательным "
            "расширением светлых участков. В Windows должен быть включён HDR (Параметры > Дисплей > "
            "Использовать HDR). Включение и выключение применяются после перезапуска игры."));
    }
    const int output = s.hdr_output.load();
    if (output == BbSettings::HdrOutputOff) {
        if (s.startup_hdr && WholeSection() && Shown("HDR output", "Вывод HDR")) {
            ImGui::PushTextWrapPos();
            ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.3f, 1.0f), "%s",
                               BbSettings::MenuText(
                                   "Not active: the display offers no HDR format (is Windows HDR on?)",
                                   "Не активен: дисплей не предлагает HDR-формат (включён ли HDR "
                                   "в Windows?)"));
            ImGui::PopTextWrapPos();
        }
        return;
    }
    if (WholeSection() && Shown("HDR output", "Вывод HDR")) {
        ImGui::TextDisabled("%s", output == BbSettings::HdrOutputScRgb
                                      ? BbSettings::MenuText("Active: scRGB (FP16)", "Активен: scRGB (FP16)")
                                      : BbSettings::MenuText("Active: HDR10 (PQ, BT.2020)",
                                                             "Активен: HDR10 (PQ, BT.2020)"));
    }
    int mode = std::clamp(s.hdr_mode.load(), 0, BbSettings::HdrModeCount - 1);
    if (const char* label = Item("HDR mode", "Режим HDR")) {
        const char* modes[BbSettings::HdrModeCount] = {
            BbSettings::MenuText("Native (from the game's HDR scene)", "Нативный (из HDR-сцены игры)"),
            BbSettings::MenuText("Expand (from the SDR picture)", "Расширение (из SDR-картинки)"),
        };
        const bool mode_changed = ImGui::Combo(label, &mode, modes, BbSettings::HdrModeCount);
        Store(s.hdr_mode, mode, mode_changed);
        Hint(BbSettings::MenuText(
            "Native: the brightness of lamps, fire, the moon and other highlights comes from the "
            "game's own HDR scene, before its tonemap clipped it to SDR white. Midtones, colours and "
            "the UI stay as in the SDR picture. Menus, loading screens and movies (no scene) use "
            "Expand. Expand: the SDR picture's brightest parts are stretched towards the peak.",
            "Нативный: яркость ламп, огня, луны и других светлых мест берётся из собственной "
            "HDR-сцены игры, до того как тонмаппинг обрезал её до белого SDR. Средние тона, цвета и "
            "интерфейс остаются как в SDR-картинке. В меню, на загрузках и в видеороликах (без сцены) "
            "работает «Расширение». Расширение: самые яркие места SDR-картинки растягиваются к пику."));
    }
    if (const char* label = Item("Paper white (nits)", "Белый SDR (нит)")) {
        Slider(label, s.hdr_paper_white, BbSettings::HdrPaperWhiteMin, BbSettings::HdrPaperWhiteMax, "%.0f",
               ImGuiSliderFlags_AlwaysClamp);
    }
    const float display_peak = s.hdr_display_peak.load();
    const bool from_display = s.hdr_peak_auto.load() && display_peak >= BbSettings::HdrPeakMin;
    if (const char* label = Item("Peak brightness from the display", "Пиковая яркость от дисплея")) {
        Checkbox(label, s.hdr_peak_auto);
        if (from_display) {
            char reported[96];
            std::snprintf(reported, sizeof(reported), "%s: %.0f",
                          BbSettings::MenuText("The display reports (nits)", "Дисплей сообщает (нит)"),
                          display_peak);
            ImGui::TextDisabled("%s", reported);
        } else if (s.hdr_peak_auto.load()) {
            ImGui::TextDisabled("%s", BbSettings::MenuText("The display reports no peak: the value below is used",
                                                           "Дисплей не сообщает пик: используется значение ниже"));
        }
    }
    if (const char* label = Item("Peak brightness (nits)", "Пиковая яркость (нит)")) {
        ImGui::BeginDisabled(from_display);
        Slider(label, s.hdr_peak, BbSettings::HdrPeakMin, BbSettings::HdrPeakMax, "%.0f",
               ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_Logarithmic);
        ImGui::EndDisabled();
    }
    if (const char* label = Item("Highlight boost", "Усиление светлых участков")) {
        Slider(label, s.hdr_boost, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
        Hint(BbSettings::MenuText(
            "Paper white: how bright the game's white (and this menu) is; about 200 suits a dim room. "
            "Peak: your display's peak brightness. From the display: the value Windows reports for it "
            "(the Windows HDR Calibration app's once it has run); turn it off to set it yourself. "
            "Highlight boost: 0 shows the SDR picture as it is; higher values let the brightest parts "
            "(lamps, fire, the moon) rise towards the peak. In Native mode it is how much of the "
            "scene's own highlight range is shown: 0.25 about half, 1 all of it (up to the peak). "
            "Changes apply live. Ctrl+click a slider to type an exact value.",
            "Белый SDR: яркость белого в игре (и этого меню); около 200 подходит для неяркой комнаты. "
            "Пиковая яркость: максимум вашего дисплея. «От дисплея» — значение, которое сообщает Windows "
            "(после приложения «Калибровка HDR» — его значение); выключите, чтобы задать вручную. "
            "Усиление светлых участков: 0 — SDR-картинка как есть; больше — "
            "самые яркие места (лампы, огонь, луна) поднимаются к пику. В нативном режиме — какая "
            "доля собственного диапазона светлых мест сцены показывается: 0.25 — около половины, "
            "1 — весь (до пика). Меняются сразу. Ctrl+клик по ползунку — ввести точное значение."));
    }
    if (mode == BbSettings::HdrModeNative) {
        if (const char* label = Item("UI brightness", "Яркость интерфейса")) {
            Slider(label, s.hdr_ui_brightness, BbSettings::HdrUiBrightnessMin, BbSettings::HdrUiBrightnessMax,
                   "%.2f", ImGuiSliderFlags_AlwaysClamp);
        }
        if (const char* label = Item("UI colour", "Цвета интерфейса")) {
            Slider(label, s.hdr_ui_color, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
            Hint(BbSettings::MenuText(
                "Native mode only: the game's HUD, menus, subtitles and loading screens. UI brightness: their "
                "white, times paper white. UI colour: 0 shows their exact sRGB colours, 1 the wider colours a "
                "wide-gamut monitor shows SDR with (the usual look; exact sRGB can look washed out next to "
                "it). Live.",
                "Только в нативном режиме: интерфейс игры, меню, субтитры и экраны загрузки. Яркость "
                "интерфейса: его белый, умноженный на белый SDR. Цвета интерфейса: 0 — точные цвета sRGB, "
                "1 — более насыщенные, как на мониторе с широким охватом в SDR (привычный вид; точный sRGB "
                "рядом с ним может казаться блёклым). Меняются сразу."));
        }
    }
    if (const char* label = Item("Compare: SDR | HDR", "Сравнение: SDR | HDR")) {
        Checkbox(label, s.hdr_compare);
        Hint(BbSettings::MenuText(
            "The left half of the screen as an SDR display shows the game (at paper white), the right "
            "half in HDR, to judge the settings above. Not saved.",
            "Левая половина экрана — как игру показывает SDR-дисплей (при белом SDR), правая — в HDR, "
            "чтобы оценить настройки выше. Не сохраняется."));
    }
}

void AoQualityItem() {
    auto& s = BbSettings::Get();
    const char* label = Item("Ambient occlusion quality", "Качество затенения (AO)");
    if (!label) {
        return;
    }
    const char* levels[] = {
        BbSettings::MenuText("Regular GTAO", "Обычный GTAO"),
        BbSettings::MenuText("High (full-res bitmask GTAO, denoised, experimental)",
                             "Высокое (битмаск GTAO в полном разрешении, с шумоподавлением, "
                             "экспериментальное)")};
    int quality = std::clamp(s.graphics_ao_quality.load(), 0, 1);
    if (ImGui::Combo(label, &quality, levels, 2)) {
        Store(s.graphics_ao_quality, quality, true);
    }
    Hint(BbSettings::MenuText(
        "Regular GTAO is unchanged and selected by default. High traces three visibility-bitmask "
        "GTAO directions at the full internal render resolution (it follows DLSS/FSR scaling) and "
        "applies a depth/normal-aware denoiser for sharper, more stable contact shadows. Higher GPU "
        "cost; GTAO must also be on. Both modes are computed fresh each frame from a fixed sampling "
        "pattern, so they stay stable in motion with no ghosting or trailing. Applied when the game "
        "restarts.",
        "Обычный GTAO не меняется и выбран по умолчанию. Высокое трассирует три направления "
        "битмаск GTAO в полном внутреннем разрешении рендера (следует масштабированию DLSS/FSR) "
        "и применяет фильтр шумоподавления с учётом глубины и нормалей для более чётких и "
        "стабильных контактных теней. Повышает нагрузку на GPU; GTAO должен быть включён. "
        "Оба режима рассчитываются заново каждый кадр по фиксированному шаблону выборки, "
        "поэтому остаются стабильными в движении без шлейфов и ореолов. "
        "Применяется после перезапуска игры."));
    if (s.graphics_ao_quality != s.startup_ao_quality) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", BbSettings::MenuText("Applied when the game restarts.",
                                                       "Применяется после перезапуска игры."));
    }
}

void PictureSection() {
    auto& s = BbSettings::Get();
    Section("Picture", "Изображение");
    const bool live = BbEngine::Graphics::Enabled();
    if (!live && WholeSection() && Shown("Picture", "Изображение")) {
        ImGui::TextDisabled("%s", BbSettings::MenuText("Unavailable: the game's code differs from version 1.09.",
                                                       "Недоступно: код игры отличается от версии 1.09."));
    }
    ImGui::BeginDisabled(!live);
    if (const char* label = Item("Picture tuning", "Настройка изображения")) {
        Checkbox(label, s.graphics_controls);
        Hint(BbSettings::MenuText(
            "Changes apply live, Picture tuning too, to the game's own colour, fog and lighting of each area "
            "(the HUD and menus keep theirs). Moving one turns Picture tuning on; off: the game's own picture. 1.00 and Hue 0 are the game's. Fog 0 clears the haze over distant streets. The "
            "same settings are in the game's System menu (PC Graphics, PC Effects, PC Colour) and the "
            "launcher. Ctrl+click a slider to type any value, beyond its ends too (brightness, saturation and fog "
            "up to 5, contrast 0.5-1.5, hue -180-180, bloom up to 10, shadow distance 0.25-8).",
            "Меняются сразу, как и сама настройка: собственные цвет, туман и освещение каждой области "
            "(интерфейс и меню не меняются). Выкл. — картинка игры. 1.00 и оттенок 0 — как в игре. Туман 0 убирает дымку над дальними улицами. Те же "
            "настройки есть в системном меню игры (PC Graphics, PC Effects, PC Colour) и в лаунчере. "
            "Ctrl+клик по ползунку — ввести любое значение, в том числе за его краями (яркость, "
            "насыщенность и туман до 5, контраст 0.5-1.5, оттенок -180-180, bloom до 10, дальность теней "
            "0.25-8)."));
    }
    const bool vignette = s.graphics_vignette;
    bool moved = false;
    const auto slider = [&](const char* english, const char* russian, std::atomic<float>& value, float lo,
                            float hi, const char* format = "%.2f") {
        if (const char* label = Item(english, russian)) {
            moved |= Slider(label, value, lo, hi, format);
        }
    };
    slider("Brightness", "Яркость", s.graphics_brightness, 0.0f, 3.0f);
    slider("Contrast", "Контраст", s.graphics_contrast, 0.8f, 1.2f, "%.3f");
    slider("Saturation", "Насыщенность", s.graphics_saturation, 0.0f, 3.0f);
    slider("Hue", "Оттенок", s.graphics_hue, -180.0f, 180.0f, "%.0f");
    slider("Fog", "Туман", s.graphics_fog, 0.0f, 3.0f);
    slider("Bloom", nullptr, s.graphics_bloom, 0.0f, 3.0f);
    if (const char* label = Item("Vignette", "Виньетка")) {
        Checkbox(label, s.graphics_vignette);
        moved |= vignette != s.graphics_vignette;
    }
    slider("Ambient occlusion strength", "Сила затенения (AO)", s.graphics_ao_strength, 0.0f, 3.0f);
    slider("Shadow distance", "Дальность теней", s.graphics_shadow_scale, 0.5f, 4.0f);
    slider("Indirect lighting strength", "Сила непрямого освещения (GI)", s.graphics_gi_strength, 0.0f, 12.0f);
    slider("Reflection strength", "Сила отражений", s.graphics_ssr_intensity, 0.0f, 3.0f);
    slider("Volumetric sunlight strength", "Сила объёмного света", s.graphics_volumetric_intensity, 0.0f, 3.0f);
    slider("Lamp fog strength", "Сила тумана ламп", s.graphics_point_fog_intensity, 0.0f, 3.0f);
    for (int e = BbSettings::EffectFirstLighting; e < BbSettings::EffectCount; ++e) {
        const auto& effect = BbSettings::Effects[e];
        if (const char* label = Item(effect.label, effect.label_ru)) {
            Checkbox(label, s.effects[e]);
            if (!BbSettings::EffectIsLive(e)) {
                Hint(BbSettings::MenuText("Applied when the game restarts.", "Применяется после перезапуска игры."));
                if (s.effects[e] != s.startup_effects[e]) {
                    ImGui::SameLine();
                    ImGui::TextDisabled("%s", BbSettings::MenuText("Applied when the game restarts.",
                                                                   "Применяется после перезапуска игры."));
                }
            }
            if (e == BbSettings::EffectModernSsr && s.effects[e] &&
                !s.effects[BbSettings::EffectSsr]) {
                ImGui::TextDisabled("%s", BbSettings::MenuText("Needs Screen-space reflections on",
                                                               "Нужны включённые экранные отражения"));
            }
        }
        if (e == BbSettings::EffectGtao) {
            AoQualityItem();
        }
    }
    if (const char* heading = Item("Looks", "Образы")) {
        ImGui::TextUnformatted(heading);
        for (int look = 0; look < BbLooks::PresetCount; ++look) {
            if (look != 0) {
                ImGui::SameLine();
            }
            if (ImGui::Button(BbSettings::MenuText(BbLooks::PresetName(look), BbLooks::PresetNameRu(look)))) {
                BbLooks::Apply(look);
            }
        }
        if (BbLooks::HasPrevious()) {
            if (ImGui::Button(BbSettings::MenuText("Back to my settings", "Вернуть мои настройки"))) {
                BbLooks::GoBack();
            }
        }
        if (BbLooks::NeedsRestart()) {
            ImGui::TextDisabled("%s", BbSettings::MenuText("Part of this look applies when the game restarts.",
                                                           "Часть образа применится после перезапуска игры."));
        }
    }
    if (moved && !s.graphics_controls) {
        Store(s.graphics_controls, true, true);
    }
    if (const char* label = Item("Game's own picture", "Как в игре")) {
        if (ImGui::Button(label)) {
            Store(s.graphics_brightness, 1.0f, true);
            Store(s.graphics_contrast, 1.0f, true);
            Store(s.graphics_saturation, 1.0f, true);
            Store(s.graphics_hue, 0.0f, true);
            Store(s.graphics_fog, 1.0f, true);
            Store(s.graphics_bloom, 1.0f, true);
            Store(s.graphics_vignette, true, true);
            Store(s.graphics_ao_strength, 1.0f, true);
            Store(s.graphics_shadow_scale, 1.0f, true);
            Store(s.graphics_gi_strength, 6.0f, true);
            Store(s.graphics_ssr_intensity, 1.0f, true);
            Store(s.graphics_volumetric_intensity, 1.0f, true);
            Store(s.graphics_point_fog_intensity, 1.0f, true);
        }
    }
    ImGui::EndDisabled();
}

void EffectsSection() {
    auto& s = BbSettings::Get();
    Section("Game effects (restart required)", "Эффекты игры (после перезапуска)");
    if (const char* label = Item("Model detail", "Детализация моделей")) {
        const char* lods[] = {BbSettings::MenuText("Highest (-2)", "Максимальная (-2)"),
                              BbSettings::MenuText("Game default", "Как в игре"),
                              BbSettings::MenuText("Lower (1)", "Ниже (1)"),
                              BbSettings::MenuText("Lowest (2)", "Минимальная (2)")};
        static constexpr int lod_values[] = {-2, 0, 1, 2};
        int lod_index = 1;
        for (int i = 0; i < 4; ++i) {
            if (lod_values[i] == s.model_lod) lod_index = i;
        }
        if (ImGui::BeginCombo(label, lods[lod_index])) {
            for (int i = 0; i < 4; ++i) {
                if (ImGui::Selectable(lods[i], i == lod_index)) {
                    Store(s.model_lod, lod_values[i], true);
                }
            }
            ImGui::EndCombo();
        }
    }
    for (int e = 0; e < BbSettings::EffectFirstLighting; ++e) {
        const auto& effect = BbSettings::Effects[e];
        const char* label = Item(effect.label, effect.label_ru);
        if (!label) {
            continue;
        }
        Checkbox(label, s.effects[e]);
        if (std::strcmp(effect.key, "effect_dynamic_shadows") == 0) {
            Hint(BbSettings::MenuText(
                "Effects are enabled and disabled by game patches at startup (patches/Bloodborne.xml). "
                "Motion blur and shadows from dynamic lights place a significant load on the GPU.",
                "Эффекты включаются и выключаются патчами игры при запуске (patches/Bloodborne.xml). "
                "Размытие в движении и тени от динамических источников заметно нагружают GPU."));
        } else if (std::strcmp(effect.key, "debug_menu") == 0) {
            Hint(BbSettings::MenuText(
                "Free camera: hold Cross and press L3 (keyboard: Space + Z). "
                "Debug menu: left touchpad / Tab. Requires DbgFont14h.ccm and DbgFont14h.tpf "
                "in dvdroot_ps4/font from Nexus mod #253. Right touchpad: Backspace.",
                "Свободная камера: удерживайте Cross и нажимайте L3 (клавиатура: Space + Z). "
                "Debug menu: левый touchpad / Tab. Нужны DbgFont14h.ccm и DbgFont14h.tpf "
                "в dvdroot_ps4/font из мода Nexus #253. Правый touchpad: Backspace."));
        }
    }
}

void UpscalerSection() {
    auto& s = BbSettings::Get();
    Section("Temporal upscaler", "Временной апскейлер");
    if (const char* label = Item("Upscaler", "Апскейлер")) {
        const char* upscalers[] = {
            BbSettings::MenuText("Off", "Выкл"), "FSR 3.1", "FSR 4 (INT8)", "FSR 4.1.1 (INT8)",
            BbSettings::MenuText("TAA (native anti-aliasing)", "TAA (нативное сглаживание)"), "DLSS (NVIDIA)",
            "FSR (AMD FSR SDK)", "XeSS (Intel)"};
        static_assert(sizeof(upscalers) / sizeof(upscalers[0]) == BbSettings::UpscalerCount);
        const int upscaler = s.upscaler;
        if (ImGui::BeginCombo(label, upscalers[upscaler])) {
            for (int i = 0; i < BbSettings::UpscalerCount; ++i) {
                const bool supported = i == BbSettings::UpscalerFsr4     ? s.fsr4_supported.load()
                                       : i == BbSettings::UpscalerFsr411 ? s.fsr411_supported.load()
                                       : i == BbSettings::UpscalerDlss   ? s.dlss_supported.load()
                                       : i == BbSettings::UpscalerFsr    ? s.fsr_sdk_supported.load()
                                       : i == BbSettings::UpscalerXess   ? s.xess_supported.load()
                                                                         : true;
                ImGui::BeginDisabled(!supported);
                if (ImGui::Selectable(upscalers[i], i == upscaler)) {
                    Store(s.upscaler, i, true);
                }
                ImGui::EndDisabled();
                if (!supported) {
                    ImGui::SameLine();
                    const bool use_sdk = i == BbSettings::UpscalerFsr4 && s.fsr_sdk_supported.load();
                    ImGui::TextDisabled("%s", use_sdk ? BbSettings::MenuText("— too slow on this GPU: use FSR (AMD FSR SDK)",
                                                                             "— слишком медленно на этой видеокарте: выберите FSR (AMD FSR SDK)")
                                                      : BbSettings::MenuText("— not supported by this GPU",
                                                                             "— не поддерживается видеокартой"));
                }
            }
            ImGui::EndCombo();
        }
        if (s.upscaler == BbSettings::UpscalerXess) {
            Hint(BbSettings::MenuText(
                "Intel XeSS Super Resolution (XeSS SDK 3.0.2): the XMX model on Intel Arc, the DP4a "
                "model on other GPUs. The preset picks the render size; XeSS takes the quality mode "
                "nearest to it. Windows only; needs libxess.dll in engine/xess.",
                "Intel XeSS Super Resolution (XeSS SDK 3.0.2): модель XMX на Intel Arc, модель DP4a "
                "на остальных видеокартах. Пресет задаёт разрешение рендера; XeSS берёт ближайший к "
                "нему режим качества. Только Windows; нужен libxess.dll в engine/xess."));
        } else if (s.upscaler == BbSettings::UpscalerFsr) {
            Hint(BbSettings::MenuText(
                "AMD's own FSR from the FSR SDK 2.4: FSR 4.1.2 on Radeon RX 6000 and newer and on "
                "Ryzen APUs that AMD supports (FP8 on RX 9000, INT8 on RX 6000/7000, a lighter INT8 "
                "model on some APUs), FSR 3.1.6 on other GPUs. The SDK picks the model by itself.",
                "Собственный FSR от AMD из FSR SDK 2.4: FSR 4.1.2 на Radeon RX 6000 и новее и на "
                "поддерживаемых AMD процессорах Ryzen (FP8 на RX 9000, INT8 на RX 6000/7000, облегчённая "
                "модель INT8 на некоторых APU), FSR 3.1.6 на остальных видеокартах. Модель SDK выбирает сам."));
        } else if (s.upscaler == BbSettings::UpscalerFsr411) {
            Hint(BbSettings::MenuText(
                "FSR 4.1.1 in INT8 mode: the model from AMD's 4.1.1 DLL, reproduced in Vulkan "
                "(output matches the DLL). One model for Native through Performance and another "
                "for Ultra Performance. Assets: tools/fsr4cap/build_assets.sh (requires the DLL "
                "and Proton).",
                "FSR 4.1.1 в режиме INT8: модель из DLL AMD 4.1.1, воспроизведённая в Vulkan "
                "(результат совпадает с DLL). Одна модель для Native..Performance и отдельная "
                "для Ultra Performance. Ассеты: tools/fsr4cap/build_assets.sh (нужны DLL и "
                "Proton)."));
        } else if (s.upscaler == BbSettings::UpscalerFsr4) {
            Hint(BbSettings::MenuText(
                "FSR 4 in INT8 mode (v07 model from AMD FidelityFX SDK sources). Higher quality "
                "than FSR 3.1, but the pass is more demanding. Changing the preset rebuilds the "
                "model (a brief pause). Assets: tools/fetch_fsr4_assets.sh.",
                "FSR 4 в режиме INT8 (модель v07 из исходников AMD FidelityFX SDK). Качество выше, "
                "чем у FSR 3.1, но проход тяжелее. Смена пресета пересобирает модель (короткая "
                "пауза). Ассеты: tools/fetch_fsr4_assets.sh."));
        }
        if (const char* problem = s.fsr4_problem.load()) {
            ImGui::PushTextWrapPos();
            ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.3f, 1.0f), BbSettings::MenuText("Upscaler unavailable: %s"),
                               BbSettings::MenuText(problem));
            if (!BbSettings::IsFsr4(s.upscaler))
                ImGui::TextUnformatted(
                    BbSettings::MenuText("The mode selected above is active. You can select it again to retry."));
            ImGui::PopTextWrapPos();
        }
    }
    if (BbSettings::IsFsr4(s.upscaler)) {
        if (const char* label = Item("FSR 4: auto exposure", "FSR 4: авто-экспозиция")) {
            Checkbox(label, s.fsr4_auto_exposure);
        }
        if (const char* label = Item("FSR 4: invert jitter sign", "FSR 4: обратный знак jitter")) {
            Checkbox(label, s.fsr4_invert_jitter);
            Hint(BbSettings::MenuText(
                "For diagnosing ghosting: the FSR 4 network normalizes color by exposure and uses "
                "it to decide when to discard previous frames. Changes apply immediately, without a "
                "restart.",
                "Проверка при гостинге: сеть FSR 4 нормирует цвет по экспозиции и по ней решает, "
                "когда отбросить прошлые кадры. Меняются сразу, без перезапуска."));
        }
    }
    const char* frame_gens[] = {BbSettings::MenuText("Off", "Выкл"), "FSR 3.1", "DLSS (NVIDIA)"};
    static_assert(std::size(frame_gens) == BbSettings::FrameGenCount);
    const int frame_gen = std::clamp(s.frame_gen.load(), 0, BbSettings::FrameGenCount - 1);
    const bool dlss_loaded = s.streamline_loaded.load();
    const bool dlss_fg = s.dlss_fg_supported.load();
    if (const char* label = Item("Frame generation", "Генерация кадров")) {
        if (ImGui::BeginCombo(label, frame_gens[frame_gen])) {
            for (int i = 0; i < BbSettings::FrameGenCount; ++i) {
                const bool unavailable = i == BbSettings::FrameGenDlss && dlss_loaded && !dlss_fg;
                ImGui::BeginDisabled(unavailable);
                if (ImGui::Selectable(frame_gens[i], i == frame_gen)) {
                    Store(s.frame_gen, i, true);
                }
                ImGui::EndDisabled();
                if (unavailable) {
                    ImGui::SameLine();
                    ImGui::TextDisabled("%s", BbSettings::MenuText("— not supported by this GPU",
                                                                   "— не поддерживается видеокартой"));
                }
            }
            ImGui::EndCombo();
        }
        if (frame_gen == BbSettings::FrameGenDlss) {
            Hint(BbSettings::MenuText(
                "NVIDIA DLSS Frame Generation: generated frames from the game's depth, motion vectors "
                "and the frame without its UI, paced by the driver, with NVIDIA Reflex. RTX 40 (one "
                "frame) or RTX 50 (up to five); Hardware-accelerated GPU scheduling on. Loads when the "
                "game starts with it selected. Off in menus, movies and with HDR output.",
                "NVIDIA DLSS Frame Generation: кадры из глубины, векторов движения и кадра без "
                "интерфейса игры, подачу ведёт драйвер, с NVIDIA Reflex. RTX 40 (один кадр) или RTX 50 "
                "(до пяти); должно быть включено аппаратное планирование GPU. Загружается, если игра "
                "запущена с этим выбором. Не работает в меню, роликах и с HDR."));
        } else {
            Hint(BbSettings::MenuText(
                "Shows an extra frame between the game's own (2x smoother motion). Adds a little input "
                "lag. Needs an upscaler; off in menus, movies and with HDR output.",
                "Показывает дополнительный кадр между кадрами игры (движение в 2 раза плавнее). Немного "
                "увеличивает задержку ввода. Нужен апскейлер; не работает в меню, роликах и с HDR."));
        }
        if (frame_gen == BbSettings::FrameGenDlss) {
            ImGui::PushTextWrapPos();
            if (!dlss_loaded) {
                ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "%s",
                                   BbSettings::MenuText("Restart the game to load DLSS Frame Generation.",
                                                        "Перезапустите игру, чтобы загрузить DLSS Frame Generation."));
            }
            if (const char* problem = s.dlss_fg_problem.load()) {
                ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.3f, 1.0f),
                                   BbSettings::MenuText("DLSS Frame Generation unavailable: %s",
                                                        "DLSS Frame Generation недоступна: %s"),
                                   BbSettings::MenuText(problem));
            }
            ImGui::PopTextWrapPos();
        }
    }
    if (frame_gen == BbSettings::FrameGenDlss || Searching()) {
        if (const char* label = Item("DLSS: generated frames", "DLSS: сгенерированных кадров")) {
            const int max_frames = dlss_fg ? std::max(1, s.dlss_fg_max_frames.load()) : BbSettings::FrameGenFramesMax;
            int frames = std::clamp(s.frame_gen_frames.load(), 1, max_frames);
            char shown[16];
            std::snprintf(shown, sizeof(shown), "%d (%dx)", frames, frames + 1);
            if (ImGui::SliderInt(label, &frames, 1, max_frames, shown, ImGuiSliderFlags_AlwaysClamp)) {
                Store(s.frame_gen_frames, frames, true);
            }
            Hint(BbSettings::MenuText(
                "Generated frames per frame the game renders: 1 doubles the frames shown, 3 is 4x. More "
                "than one needs an RTX 50 GPU (DLSS multi frame generation). Live.",
                "Сгенерированных кадров на каждый кадр игры: 1 удваивает число кадров, 3 — это 4x. "
                "Больше одного — только RTX 50 (DLSS Multi Frame Generation). Меняется сразу."));
        }
        const bool dynamic_ok = s.dlss_fg_dynamic_supported.load();
        if (const char* label = Item("DLSS: dynamic multi frame generation",
                                     "DLSS: динамическая генерация кадров")) {
            ImGui::BeginDisabled(!dynamic_ok && !Searching());
            Checkbox(label, s.frame_gen_dynamic);
            ImGui::EndDisabled();
            Hint(dynamic_ok
                     ? BbSettings::MenuText(
                           "The driver picks 2x, 3x or 4x moment by moment to reach the target (up to "
                           "the generated frames above): fewer generated frames when the game is fast, "
                           "less lag and fewer artifacts. Live.",
                           "Драйвер сам выбирает 2x, 3x или 4x, чтобы достичь цели (не больше числа "
                           "кадров выше): меньше сгенерированных кадров, когда игра быстрая, меньше "
                           "задержка и артефактов. Меняется сразу.")
                     : BbSettings::MenuText("Needs an RTX 50 GPU, a recent driver and DLSS Frame Generation loaded.",
                                            "Нужна RTX 50, свежий драйвер и загруженная DLSS Frame Generation."));
        }
        if (const char* label = Item("DLSS: dynamic target FPS", "DLSS: целевой FPS")) {
            ImGui::BeginDisabled(!s.frame_gen_dynamic.load());
            int target = s.frame_gen_target_fps.load();
            char shown[32];
            if (target == 0) {
                std::snprintf(shown, sizeof(shown), "%s", BbSettings::MenuText("Refresh rate", "Частота экрана"));
            } else {
                std::snprintf(shown, sizeof(shown), "%d FPS", target);
            }
            if (ImGui::SliderInt(label, &target, 0, 360, shown, ImGuiSliderFlags_AlwaysClamp)) {
                Store(s.frame_gen_target_fps, target, true);
            }
            ImGui::EndDisabled();
        }
    }
    if (const char* label = Item("Frame generation: game at a share of the refresh rate",
                                 "Генерация кадров: игра на доле частоты экрана")) {
        ImGui::BeginDisabled(frame_gen == BbSettings::FrameGenOff);
        Checkbox(label, s.frame_gen_cap);
        ImGui::EndDisabled();
        Hint(BbSettings::MenuText(
            "Every generated frame reaches the screen evenly: the game at the refresh rate divided by "
            "the frames shown per game frame (60 + 60 at 120 Hz with one generated frame). Off: the "
            "game runs faster, but the screen skips some generated frames.",
            "Каждый сгенерированный кадр равномерно попадает на экран: игра на частоте экрана, "
            "делённой на число показанных кадров (60 + 60 при 120 Гц с одним сгенерированным). Выкл: "
            "игра быстрее, но часть сгенерированных кадров экран пропускает."));
    }
    if (dlss_loaded || Searching()) {
        if (const char* label = Item("NVIDIA Reflex")) {
            const char* modes[] = {BbSettings::MenuText("Off", "Выкл"), BbSettings::MenuText("On", "Вкл"),
                                   BbSettings::MenuText("On + Boost", "Вкл + Boost")};
            static_assert(std::size(modes) == BbSettings::ReflexCount);
            int mode = std::clamp(s.reflex.load(), 0, BbSettings::ReflexCount - 1);
            if (ImGui::Combo(label, &mode, modes, BbSettings::ReflexCount)) {
                Store(s.reflex, mode, true);
            }
            Hint(BbSettings::MenuText(
                "Lower input lag: the GPU's queue is kept short. On (at least) whenever DLSS Frame "
                "Generation runs; Boost also keeps the GPU clocks up. Only with DLSS Frame Generation "
                "loaded.",
                "Меньше задержка ввода: очередь GPU остаётся короткой. Включается всегда, когда "
                "работает DLSS Frame Generation; Boost ещё держит частоты GPU. Только когда DLSS Frame "
                "Generation загружена."));
        }
    }
    if (s.anti_lag_supported || Searching()) {
        if (const char* label = Item("AMD Anti-Lag 2")) {
            bool anti_lag = s.anti_lag.load();
            if (ImGui::Checkbox(label, &anti_lag)) {
                Store(s.anti_lag, anti_lag, true);
            }
            Hint(BbSettings::MenuText(
                "Lower input lag: the Radeon driver starts each frame as late as it can. Only on "
                "AMD drivers that offer it. It turns itself off until the next start if the driver "
                "holds the game too long.",
                "Меньше задержка ввода: драйвер Radeon начинает каждый кадр как можно позже. Только "
                "с драйверами AMD, которые это поддерживают. Выключается до следующего запуска, если "
                "драйвер слишком долго держит игру."));
        }
    }
    const bool upscaler_on = s.upscaler != BbSettings::UpscalerOff;
    const bool taa = s.upscaler == BbSettings::UpscalerTaa;
    ImGui::BeginDisabled(!upscaler_on);
    if (const char* label = Item("Preset", "Пресет")) {
        ImGui::BeginDisabled(taa);
        const int preset = taa ? BbSettings::NativeAA : s.preset.load();
        char preset_label[64];
        std::snprintf(preset_label, sizeof(preset_label), "%s (x%.1f)",
                      BbSettings::MenuText(BbSettings::PresetName(preset)), BbSettings::PresetScale(preset));
        if (ImGui::BeginCombo(label, preset_label)) {
            for (int i = 0; i < BbSettings::PresetCount; ++i) {
                char choice[64];
                const float scale = BbSettings::PresetScale(i);
                const int output = s.output_res;
                std::snprintf(
                    choice, sizeof(choice),
                    BbSettings::MenuText("%s (x%.1f, render %dx%d)", "%s (x%.1f, рендер %dx%d)"),
                    BbSettings::MenuText(BbSettings::PresetName(i)), scale,
                    int(std::lround(BbSettings::OutputWidth(output) / scale / 4) * 4),
                    int(std::lround(BbSettings::OutputHeight(output) / scale / 4) * 4));
                if (ImGui::Selectable(choice, i == preset)) {
                    Store(s.preset, i, true);
                }
            }
            ImGui::EndCombo();
        }
        ImGui::EndDisabled();
        if (taa) {
            ImGui::TextWrapped("%s", BbSettings::MenuText(
                                         "TAA anti-aliases the scene at the output resolution, without an FSR "
                                         "model or upscaling. The saved preset returns when an upscaler is selected."));
        }
        if (!taa && s.preset == BbSettings::PresetCustom) {
            if (const char* scale_label = Item("Render scale (Custom)", "Масштаб рендера (Custom)")) {
                Slider(scale_label, s.render_scale, BbSettings::RenderScaleMin, BbSettings::RenderScaleMax, "x%.2f");
                Hint(BbSettings::MenuText(
                    "Output / render size for the Custom preset: 1.00 renders at the output (DLAA, Native "
                    "AA), 1.30 is between Native and Quality, 3.00 is Ultra Performance. FSR 4 and DLSS run "
                    "the model of the nearest preset. Ctrl+click to type a value.",
                    "Отношение вывода к рендеру для пресета Custom: 1.00 — рендер в разрешении вывода "
                    "(DLAA, Native AA), 1.30 — между Native и Quality, 3.00 — Ultra Performance. FSR 4 и "
                    "DLSS берут модель ближайшего пресета. Ctrl+клик — ввести значение."));
            }
        }
        ImGui::Text(BbSettings::MenuText("Active scene render: %d x %d"), s.active_render_width.load(),
                    s.active_render_height.load());
        if (BbSettings::FixedRenderSession()) {
            ImGui::Text(BbSettings::MenuText("Preset at startup: %s"),
                        BbSettings::MenuText(BbSettings::PresetName(s.startup_preset)));
            if (const char* automatic = std::getenv("BB_AUTO_RENDER_RES");
                automatic && automatic[0] == '1') {
                Hint(BbSettings::MenuText(
                    "With an output other than 1080p the whole game renders at the preset resolution "
                    "(patched at startup): fastest on the Steam Deck and weaker GPUs. Preset or output "
                    "changes apply after a restart. \"Live resolution changes\" below allows changing "
                    "without a restart (post-processing then stays at 1080p, slower)."));
            } else {
                Hint(BbSettings::MenuText(
                    "BB_RENDER_RES fixes the scene size at startup. Remove this explicit variable "
                    "to change resolution and presets without restarting the game."));
            }
        } else {
            Hint(BbSettings::MenuText(
                "Native AA: the upscaler works as anti-aliasing. The other presets lower the scene "
                "render resolution relative to the output. The UI is drawn at the output resolution. "
                "The preset applies from the next frame without restarting the game."));
        }
    }
    if (const char* label = Item("Sharpening (RCAS)")) {
        Checkbox(label, s.sharpen);
    }
    if (const char* label = Item("Sharpness")) {
        ImGui::BeginDisabled(!s.sharpen);
        Slider(label, s.sharpness, 0.0f, 2.0f);
        ImGui::EndDisabled();
        Hint(BbSettings::MenuText(
            "Up to 1: the upscaler's own sharpening (RCAS). Above 1 another RCAS pass is added. "
            "DLSS has no sharpening of its own: an RCAS pass does all of it. "
            "Ctrl+click the slider to type an exact value."));
    }
    if (const char* label = Item("Texture mip bias", "Mip bias текстур")) {
        Slider(label, s.mip_bias, BbSettings::MipBiasMin, BbSettings::MipBiasMax, "%+.2f");
        Hint(BbSettings::MenuText(
            "Added to the texture detail the upscaler already asks for (the render-to-output ratio, "
            "and DLSS's extra level): below 0 sharper textures (more shimmer), above 0 softer. 0: "
            "the upscaler's own. Live.",
            "Добавляется к детализации текстур, которую уже задаёт апскейлер (соотношение рендера и "
            "вывода и лишний уровень DLSS): меньше 0 — чётче (больше мерцания), больше 0 — мягче. 0 — "
            "как у апскейлера. Меняется сразу."));
    }
    if (const char* label = Item("Sub-pixel jitter")) {
        Checkbox(label, s.jitter);
        Hint(BbSettings::MenuText(
            "Each frame the scene shifts by a fraction of a pixel, and the upscaler gathers more "
            "detail from several frames. Without it you only get history-based anti-aliasing."));
    }
    ImGui::EndDisabled();
}

void DlssSection() {
    auto& s = BbSettings::Get();
    if (s.upscaler != BbSettings::UpscalerDlss && !Searching()) {
        return;
    }
    Section("DLSS");
    if (const char* label = Item("DLSS model", "Модель DLSS")) {
        static constexpr int presets[] = {BbSettings::DlssPresetAuto, 11, 10, 13, 12, 5, 6,
                                          BbSettings::DlssPresetDefault};
        const char* names[] = {
            BbSettings::MenuText("Auto: K, M for Performance, L for Ultra Performance",
                                 "Авто: K, M для Performance, L для Ultra Performance"),
            BbSettings::MenuText("K: transformer (default)", "K: трансформер (по умолчанию)"),
            BbSettings::MenuText("J: transformer", "J: трансформер"),
            BbSettings::MenuText("M: transformer, 2nd generation", "M: трансформер 2-го поколения"),
            BbSettings::MenuText("L: transformer, 2nd generation (Ultra Performance)",
                                 "L: трансформер 2-го поколения (Ultra Performance)"),
            BbSettings::MenuText("E: CNN", "E: CNN"),
            BbSettings::MenuText("F: CNN (Ultra Performance, DLAA)", "F: CNN (Ultra Performance, DLAA)"),
            BbSettings::MenuText("The DLL's default", "По умолчанию DLL"),
        };
        static_assert(std::size(presets) == std::size(names));
        const int current = s.dlss_preset;
        int index = -1;
        for (int i = 0; i < int(std::size(presets)); ++i) {
            if (presets[i] == current) index = i;
        }
        char other[48];
        std::snprintf(other, sizeof(other), "%s %c", BbSettings::MenuText("Preset", "Пресет"),
                      char('A' + std::clamp(current, 1, BbSettings::DlssPresetMax) - 1));
        if (ImGui::BeginCombo(label, index >= 0 ? names[index] : other)) {
            for (int i = 0; i < int(std::size(presets)); ++i) {
                if (ImGui::Selectable(names[i], i == index)) {
                    Store(s.dlss_preset, presets[i], true);
                }
            }
            ImGui::EndCombo();
        }
        Hint(BbSettings::MenuText(
            "Which NVIDIA model DLSS runs; it changes at once (a brief pause). K keeps thin and moving "
            "objects steadier; E is a little faster on RTX 20 and 30 cards but softer in motion. M and L "
            "need a DLSS 4.5 nvngx_dlss.dll (310.5 or newer). A preset the DLL refuses runs its default. "
            "The launcher's Graphics page has the same setting.",
            "Какую модель NVIDIA запускает DLSS; меняется сразу (короткая пауза). K устойчивее на тонких "
            "и движущихся объектах; E чуть быстрее на RTX 20 и 30, но мягче в движении. Для M и L нужен "
            "nvngx_dlss.dll из DLSS 4.5 (310.5 или новее). Пресет, который DLL не принимает, заменяется "
            "её пресетом по умолчанию. Та же настройка есть на странице «Графика» лаунчера."));
        const int applied = s.dlss_applied_preset;
        if (applied > 0) {
            ImGui::TextDisabled(BbSettings::MenuText("Running: preset %c", "Работает: пресет %c"),
                                char('A' + applied - 1));
        } else if (applied == 0) {
            ImGui::TextDisabled("%s", BbSettings::MenuText("Running: the DLL's default preset",
                                                           "Работает: пресет DLL по умолчанию"));
        }
        if (applied == 0 && current != BbSettings::DlssPresetDefault) {
            ImGui::PushTextWrapPos();
            ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.3f, 1.0f),
                               BbSettings::MenuText("This DLSS version refuses preset %c: its default runs",
                                                    "Эта версия DLSS не принимает пресет %c: работает её пресет"),
                               char('A' + std::clamp(current, 1, BbSettings::DlssPresetMax) - 1));
            ImGui::PopTextWrapPos();
        }
    }
    if (const char* label = Item("DLSS: auto exposure", "DLSS: авто-экспозиция")) {
        Checkbox(label, s.dlss_auto_exposure);
        Hint(BbSettings::MenuText(
            "DLSS works out the frame's exposure itself, for the game's tonemapped frame too (the HDR "
            "scene at 1080p always has it). Can steady dark scenes that ghost. Live (a brief pause).",
            "DLSS сам определяет экспозицию кадра, и для готового кадра игры (HDR-сцена при 1080p — "
            "всегда). Может успокоить тёмные сцены с гостингом. Меняется сразу (короткая пауза)."));
    }
    if (const char* label = Item("Upscale before post-processing (experimental)", "Апскейл до постобработки (эксперимент)")) {
        Checkbox(label, s.upscale_before_post);
        Hint(BbSettings::MenuText(
            "Outputs other than 1080p: the upscaler takes the HDR scene before the game's post-processing, as NVIDIA's "
            "DLSS guide asks, and the post runs at the output size. Uses more GPU time and memory. Applies after a restart.",
            "Вывод не 1080p: апскейлер берёт HDR-сцену до постобработки игры, как просит руководство NVIDIA по DLSS, а "
            "постобработка идёт в разрешении вывода. Больше нагрузка на GPU и память. Применяется после перезапуска."));
    }
    if (const char* label = Item("DLSS: less wobble while turning (experimental)", "DLSS: меньше дрожания при поворотах (эксперимент)")) {
        Checkbox(label, s.dlss_jitter_fade);
        Hint(BbSettings::MenuText(
            "Fades the sub-pixel jitter out while the camera moves. The image holds steadier, but NVIDIA's "
            "DLSS guide warns against jitter only on a still camera. Off: as the guide asks. Live.",
            "Убирает субпиксельный jitter, пока камера движется. Картинка стабильнее, но руководство NVIDIA "
            "по DLSS предостерегает от jitter только при неподвижной камере. Выкл.: как в руководстве. Меняется сразу."));
    }
    if (const char* label = Item("DLSS: sharper textures (mip bias -1)", "DLSS: чётче текстуры (mip bias -1)")) {
        Checkbox(label, s.dlss_mip_bias);
        Hint(BbSettings::MenuText(
            "Textures one mip level sharper than the render resolution alone gives, as NVIDIA's DLSS "
            "guide asks. Off: only the render-to-output ratio. Live.",
            "Текстуры на один mip-уровень чётче, чем даёт само разрешение рендера, как советует "
            "руководство NVIDIA по DLSS. Выкл.: только соотношение рендера и вывода. Меняется сразу."));
    }
}

void Fsr3Section() {
    auto& s = BbSettings::Get();
    if (s.upscaler != BbSettings::UpscalerFsr3 && !Searching()) {
        return;
    }
    Section("FSR 3.1 tuning", "Настройка FSR 3.1");
    struct Tuning {
        const char* en;
        const char* ru;
        float velocity, reactiveness, shading, accumulation, disocclusion;
    };
    static constexpr Tuning presets[] = {
        {"Default (AMD's)", "По умолчанию (AMD)", 1.0f, 1.0f, 1.0f, 0.333f, -0.333f},
        {"Steady bright pixels", "Стабильные яркие пиксели", 0.5f, 1.0f, 1.0f, 0.333f, -0.333f},
        {"Less ghosting", "Меньше шлейфов", 1.0f, 1.5f, 1.5f, 0.25f, -0.333f},
        {"Less flicker", "Меньше мерцания", 1.0f, 1.0f, 1.0f, 0.4f, -0.1f},
    };
    if (const char* label = Item("Preset", "Пресет")) {
        int index = -1;
        for (int i = 0; i < int(std::size(presets)); ++i) {
            const auto& p = presets[i];
            const auto near = [](float a, float b) { return std::abs(a - b) < 0.0005f; };
            if (near(s.fsr3_velocity_factor, p.velocity) && near(s.fsr3_reactiveness_scale, p.reactiveness) &&
                near(s.fsr3_shading_change_scale, p.shading) && near(s.fsr3_accumulation, p.accumulation) &&
                near(s.fsr3_min_disocclusion, p.disocclusion)) {
                index = i;
            }
        }
        if (ImGui::BeginCombo(label, index >= 0 ? BbSettings::MenuText(presets[index].en, presets[index].ru)
                                                : BbSettings::MenuText("Custom", "Своё"))) {
            for (int i = 0; i < int(std::size(presets)); ++i) {
                if (ImGui::Selectable(BbSettings::MenuText(presets[i].en, presets[i].ru), i == index)) {
                    Store(s.fsr3_velocity_factor, presets[i].velocity, true);
                    Store(s.fsr3_reactiveness_scale, presets[i].reactiveness, true);
                    Store(s.fsr3_shading_change_scale, presets[i].shading, true);
                    Store(s.fsr3_accumulation, presets[i].accumulation, true);
                    Store(s.fsr3_min_disocclusion, presets[i].disocclusion, true);
                }
            }
            ImGui::EndCombo();
        }
        Hint(BbSettings::MenuText(
            "FSR 3.1's own tuning, as OptiScaler sets it. Steady bright pixels: lamps and sparks shimmer "
            "less in motion. Less ghosting: fewer trails behind moving things, thin details flicker a "
            "little more. Less flicker: thin swaying things (grass, hair) flicker less, a little more "
            "ghosting.",
            "Собственная настройка FSR 3.1, как в OptiScaler. Стабильные яркие пиксели: фонари и искры "
            "меньше мерцают в движении. Меньше шлейфов: меньше следов за движущимся, тонкие детали "
            "мерцают чуть больше. Меньше мерцания: тонкое (трава, волосы) мерцает меньше, шлейфов "
            "чуть больше."));
    }
    if (const char* label = Item("Velocity factor", "Учёт скорости")) {
        Slider(label, s.fsr3_velocity_factor, 0.0f, 1.0f, "%.2f");
    }
    if (const char* label = Item("Reactiveness", "Реактивность")) {
        Slider(label, s.fsr3_reactiveness_scale, 0.0f, 4.0f, "%.2f");
    }
    if (const char* label = Item("Shading change", "Изменение освещения")) {
        Slider(label, s.fsr3_shading_change_scale, 0.0f, 4.0f, "%.2f");
    }
    if (const char* label = Item("Accumulation per frame", "Накопление за кадр")) {
        Slider(label, s.fsr3_accumulation, 0.0f, 1.0f, "%.3f");
    }
    if (const char* label = Item("Disocclusion accumulation", "Накопление при раскрытии")) {
        Slider(label, s.fsr3_min_disocclusion, -1.0f, 1.0f, "%.3f");
    }
}

void MaskSection() {
    auto& s = BbSettings::Get();
    const bool upscaler_on = s.upscaler != BbSettings::UpscalerOff;
    const bool taa = s.upscaler == BbSettings::UpscalerTaa;
    ImGui::BeginDisabled(!upscaler_on);
    Section("Reactive mask");
    ImGui::BeginDisabled(taa);
    if (const char* label = Item("Enable mask")) {
        Checkbox(label, s.reactive);
        Hint(BbSettings::MenuText(
            "Marks transparent effects (particles, haze) so the upscaler relies less on past "
            "frames. Fewer trails behind effects, but shimmer returns underneath them."));
        const char* use = s.upscaler == BbSettings::UpscalerFsr3
                              ? BbSettings::MenuText("FSR 3.1 uses it as its reactive mask.",
                                                     "FSR 3.1 берёт её как свою реактивную маску.")
                          : s.upscaler == BbSettings::UpscalerXess
                              ? BbSettings::MenuText("XeSS uses it as its responsive pixel mask.",
                                                     "XeSS берёт её как маску отзывчивых пикселей.")
                          : s.upscaler == BbSettings::UpscalerDlss
                              ? BbSettings::MenuText("Current DLSS models do not use it, only the deprecated preset F.",
                                                     "Текущие модели DLSS её не используют, только устаревший пресет F.")
                          : BbSettings::IsFsr4(s.upscaler)
                              ? BbSettings::MenuText("FSR 4 has no mask input: the marked pixels are blended with "
                                                     "this frame after it.",
                                                     "У FSR 4 нет входа для маски: отмеченные пиксели смешиваются "
                                                     "с текущим кадром после него.")
                              : BbSettings::MenuText("TAA does not use it.", "TAA её не использует.");
        ImGui::PushTextWrapPos();
        ImGui::TextDisabled("%s", use);
        ImGui::PopTextWrapPos();
    }
    ImGui::BeginDisabled(!s.reactive);
    if (const char* label = Item("Scale")) {
        Slider(label, s.reactive_scale, 0.0f, 4.0f);
    }
    if (const char* label = Item("Threshold")) {
        Slider(label, s.reactive_threshold, 0.0f, 1.0f);
    }
    if (const char* label = Item("Maximum")) {
        Slider(label, s.reactive_max, 0.0f, 1.0f);
    }
    if (const char* label = Item("Show mask (debug)")) {
        bool show_mask = s.debug_view == BbSettings::DebugReactive;
        if (ImGui::Checkbox(label, &show_mask)) {
            s.debug_view = show_mask ? BbSettings::DebugReactive : BbSettings::DebugNone;
        }
        Hint(BbSettings::MenuText(
            "Red: how far each pixel stops relying on past frames (Maximum = full red); the rest of "
            "the picture is darkened. Scale makes changes count more, Threshold drops faint ones "
            "(thin mist). Not saved.",
            "Красный: насколько пиксель перестаёт полагаться на прошлые кадры (Maximum — полностью "
            "красный); остальная картинка затемнена. Scale усиливает изменения, Threshold отбрасывает "
            "слабые (лёгкий туман). Не сохраняется."));
    }
    ImGui::EndDisabled();
    ImGui::EndDisabled();

    Section("Motion vectors", "Векторы движения");
    if (const char* label = Item("Character motion vectors", "Векторы движения персонажей")) {
        Checkbox(label, s.object_motion);
        Hint(BbSettings::MenuText(
            "Accurate vectors for animated objects: clothing and weapons break up less "
            "during movement. The static scene does not get an additional pass. "
            "Changes apply after restarting the game.",
            "Точные векторы для анимированных объектов: одежда и оружие меньше рассыпаются "
            "при движении. Статичная сцена не получает дополнительный проход. "
            "Изменение применяется после перезапуска игры."));
    }
    if (const char* label = Item("Show motion vectors (debug)", "Показать векторы движения (отладка)")) {
        bool show_motion = s.debug_view == BbSettings::DebugMotion;
        if (ImGui::Checkbox(label, &show_motion)) {
            s.debug_view = show_motion ? BbSettings::DebugMotion : BbSettings::DebugNone;
        }
        Hint(BbSettings::MenuText(
            "Red/green: horizontal/vertical motion (8 pixels = full brightness). "
            "Blue: the pixel received an accurate object vector, not just camera motion. "
            "The upscaler treats a moving object with no blue or red/green as "
            "stationary, causing trails.",
            "Красный/зелёный: движение по горизонтали/вертикали (8 пикселей = полная яркость). "
            "Синий: пиксель получил точный вектор объекта, а не только движение камеры. "
            "Движущийся предмет без синего и без красного/зелёного апскейлер считает "
            "неподвижным, отсюда шлейф."));
    }
    ImGui::EndDisabled();
}

void OptiScalerSection() {
    auto& s = BbSettings::Get();
    const auto& opti = Vulkan::OptiScaler::Detect();
    Section("OptiScaler");
    if (WholeSection() && Shown("OptiScaler")) {
        ImGui::PushTextWrapPos();
        if (!opti.loaded) {
            ImGui::TextDisabled("%s", BbSettings::MenuText(
                                          "Not loaded. OptiScaler is a DLL put next to the engine (often as winmm.dll "
                                          "or dxgi.dll) with its OptiScaler.ini; it takes over DLSS. The switches below "
                                          "apply when it is there.",
                                          "Не загружен. OptiScaler — это DLL рядом с движком (обычно winmm.dll или "
                                          "dxgi.dll) с файлом OptiScaler.ini; он перехватывает DLSS. Переключатели ниже "
                                          "действуют, когда он есть."));
        } else {
            ImGui::Text(BbSettings::MenuText("Loaded: %s as %s", "Загружен: %s как %s"),
                        opti.version.empty() ? "OptiScaler" : ("v" + opti.version).c_str(), opti.module.c_str());
            ImGui::Text("%s",
                        Vulkan::OptiScaler::Compat()
                            ? BbSettings::MenuText("Compatibility mode in this run: on", "Режим совместимости сейчас: вкл.")
                            : BbSettings::MenuText("Compatibility mode in this run: off", "Режим совместимости сейчас: выкл."));
            if (!opti.frame_generation.empty()) {
                ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f),
                                   BbSettings::MenuText(
                                       "Its own frame generation is on (%s). If the GPU is lost, set Frame "
                                       "Generation to none in OptiScaler's menu and use Frame generation above.",
                                       "Включена его собственная генерация кадров (%s). Если GPU теряется, "
                                       "выберите none для Frame Generation в меню OptiScaler и используйте "
                                       "генерацию кадров выше."),
                                   opti.frame_generation.c_str());
            }
        }
        ImGui::PopTextWrapPos();
    }
    if (const char* label = Item("Compatibility mode (restart required)", "Режим совместимости (после перезапуска)")) {
        Checkbox(label, s.optiscaler_compat);
        Hint(BbSettings::MenuText(
            "Keeps the engine on the paths OptiScaler's hooks handle: DLSS calls wait for the GPU queue, "
            "the Vulkan device is created where OptiScaler can prepare it, and one thread sends the GPU "
            "work while its frame generation is on (a little FPS). Off: the engine runs as without "
            "OptiScaler. OptiScaler's own settings (its upscaler, its frame generation) are in its own "
            "overlay and OptiScaler.ini. Applies after restarting the game.",
            "Держит движок на путях, которые выдерживают хуки OptiScaler: вызовы DLSS ждут очередь GPU, "
            "устройство Vulkan создаётся там, где OptiScaler может его подготовить, а работу GPU "
            "отправляет один поток, пока включена его генерация кадров (немного FPS). Выкл.: движок "
            "работает как без OptiScaler. Собственные настройки OptiScaler (его апскейлер, его генерация "
            "кадров) — в его оверлее и OptiScaler.ini. Применяется после перезапуска игры."));
        if (const char* env = std::getenv("BB_OPTISCALER_COMPAT"); env && env[0] == '0') {
            ImGui::TextDisabled("%s", BbSettings::MenuText(
                                          "Off for this run: the launcher's Engine > OptiScaler compatibility is off.",
                                          "Выключен в этом запуске: в лаунчере выключено Engine > OptiScaler "
                                          "compatibility."));
        }
    }
    ImGui::BeginDisabled(!s.optiscaler_compat);
    ImGui::Indent();
    if (const char* label = Item("DLSS calls wait for the GPU queue", "Вызовы DLSS ждут очередь GPU")) {
        Checkbox(label, s.optiscaler_ngx_lock);
    }
    if (const char* label = Item("Vulkan device on a thread of its own", "Устройство Vulkan в отдельном потоке")) {
        Checkbox(label, s.optiscaler_clean_thread);
    }
    if (const char* label = Item("One GPU submit thread", "Один поток отправки GPU")) {
        const char* modes[] = {BbSettings::MenuText("With its frame generation only", "Только с его генерацией кадров"),
                               BbSettings::MenuText("Always", "Всегда"), BbSettings::MenuText("Off", "Выкл.")};
        const int current = s.optiscaler_sync_submit;
        int index = current == BbSettings::OptiSubmitOn ? 1 : current == BbSettings::OptiSubmitOff ? 2 : 0;
        if (ImGui::Combo(label, &index, modes, 3)) {
            Store(s.optiscaler_sync_submit,
                  index == 1 ? int(BbSettings::OptiSubmitOn)
                             : index == 2 ? int(BbSettings::OptiSubmitOff) : int(BbSettings::OptiSubmitAuto),
                  true);
        }
        Hint(BbSettings::MenuText(
            "The parts of the compatibility mode, each applied after a restart. Turn one off only to "
            "find which one a crash with OptiScaler comes from, and post your log either way.",
            "Части режима совместимости, каждая применяется после перезапуска. Выключайте их по одной, "
            "только чтобы найти, из-за какой падает игра с OptiScaler, и в любом случае пришлите лог."));
    }
    ImGui::Unindent();
    ImGui::EndDisabled();
}

void OutputSection() {
    auto& s = BbSettings::Get();
    Section("Output resolution", "Разрешение вывода");
    if (const char* label = Item("Output resolution", "Разрешение вывода")) {
        const char* outputs[] = {"1280 x 720", "1600 x 900", "1920 x 1080", "2560 x 1440", "3840 x 2160",
            "2560 x 1080", "3440 x 1440", "3840 x 1600", "3840 x 1080", "5120 x 1440", "5120 x 2160",
            "7680 x 2160", BbSettings::MenuText("Display (auto)"), "Custom"};
        static_assert(std::size(outputs) == BbSettings::OutputCount);
        static char custom[48];
        std::snprintf(custom, sizeof custom, "Custom (%d x %d)", s.custom_output_w, s.custom_output_h);
        outputs[BbSettings::OutputCustom] = custom;
        const int output = s.output_res;
        if (ImGui::BeginCombo(label, outputs[output])) {
            for (int i = 0; i < BbSettings::OutputCount; ++i) {
                if (ImGui::Selectable(outputs[i], i == output)) {
                    Store(s.output_res, i, true);
                }
            }
            ImGui::EndCombo();
        }
        if (BbSettings::FixedRenderSession()) {
            Hint(BbSettings::MenuText(
                "Size of the final frame and UI. The preset sets the scene size relative to "
                "the output: 4K Performance = 1920x1080. Applies after restarting the game.",
                "Размер готового кадра и интерфейса. Пресет задаёт размер сцены относительно "
                "вывода: 4K Performance = 1920x1080. Применяется после перезапуска игры."));
        } else {
            Hint(BbSettings::MenuText(
                "The final frame and UI size changes at the next frame boundary. "
                "The preset sets the scene size relative to the output: 4K Performance = 1920x1080. "
                "Changing the size resets FSR history and may cause a brief pause.",
                "Размер готового кадра и интерфейса меняется на границе следующего кадра. "
                "Пресет задаёт размер сцены относительно вывода: 4K Performance = 1920x1080. "
                "Смена размера сбрасывает историю FSR и может вызвать короткую паузу."));
        }
    }
    if (const char* label = Item("Live resolution changes", "Смена разрешения на лету")) {
        const char* live_modes[] = {BbSettings::MenuText("Auto (based on GPU)", "Авто (по видеокарте)"),
                                    BbSettings::MenuText("Off (faster)", "Выключена (быстрее)"),
                                    BbSettings::MenuText("On", "Включена")};
        const int live = s.live_resolution + 1;
        if (ImGui::BeginCombo(label, live_modes[live])) {
            for (int i = 0; i < 3; ++i) {
                if (ImGui::Selectable(live_modes[i], i == live)) {
                    Store(s.live_resolution, i - 1, true);
                }
            }
            ImGui::EndCombo();
        }
        Hint(BbSettings::MenuText(
            "On: output resolution and preset change without restarting, but game post-processing "
            "stays at 1080p, which is noticeably slower on Steam Deck and older GPUs. "
            "Off: everything renders at the preset resolution, and changes require a restart. Auto "
            "enables this on powerful discrete GPUs. Applies after restarting the game.",
            "Включена: разрешение вывода и пресет меняются без перезапуска, но постобработка игры "
            "остаётся в 1080p — на Steam Deck и старых видеокартах это заметно медленнее. "
            "Выключена: всё рисуется в разрешении пресета, смена — через перезапуск. Авто включает "
            "её на мощных дискретных видеокартах. Применяется после перезапуска игры."));
    }
}

void CameraSection() {
    auto& s = BbSettings::Get();
    Section("PC Camera");
    if (WholeSection() && Shown("PC Camera")) {
        ImGui::TextDisabled("%s", BbSettings::MenuText("Enable PC Camera in the launcher before starting; changes here are live."));
    }
    if (const char* label = Item("Camera FOV scale")) {
        Slider(label, s.camera_fov_scale, 1.0f, 1.5f);
    }
    if (const char* label = Item("Camera distance scale")) {
        Slider(label, s.camera_distance_scale, 0.5f, 1.5f);
    }
    if (const char* label = Item("Camera height scale")) {
        Slider(label, s.camera_height_scale, 0.5f, 1.5f);
    }
}

void CheatsSection() {
    auto& s = BbSettings::Get();
    Section("Cheats", "Читы");
    if (const char* label = Item("Blood Vials", "Флаконы крови")) {
        Cheat(label, s.cheat_vials, BbEngine::Cheats::Vials, custom_vials);
    }
    if (const char* label = Item("Quicksilver Bullets", "Ртутные пули")) {
        Cheat(label, s.cheat_bullets, BbEngine::Cheats::Bullets, custom_bullets);
        Hint(BbSettings::MenuText(
            "Changes apply live. Infinite: kept full at the normal carry limit, even from none. Custom: the count is "
            "refilled to the amount and never lowered. Off restores the game's item data and keeps the "
            "counts you have. While a cheat is on, the counts it gives are saved with your character. "
            "Do not play online with cheats on.",
            "Применяются сразу. Бесконечно: запас всегда полный, даже если предметов не было. Своё "
            "количество: запас пополняется до него и никогда не уменьшается. Выкл. возвращает данные "
            "предметов игры, а имеющееся количество остаётся. Пока чит включён, полученные предметы "
            "сохраняются вместе с персонажем. Не играйте по сети с включёнными читами."));
    }
    if (const char* label = Item("Infinite stamina", "Бесконечная выносливость")) {
        CheatToggle(label, s.cheat_stamina, BbEngine::Cheats::Stamina,
                    BbSettings::MenuText("Your stamina stays full (enemies are not affected). Off: nothing to restore.",
                                         "Ваша выносливость всегда полная (на врагов не действует). Выкл.: "
                                         "восстанавливать нечего."));
    }
    if (const char* label = Item("Stealth", "Скрытность")) {
        CheatToggle(label, s.cheat_stealth, BbEngine::Cheats::Stealth,
                    BbSettings::MenuText("Enemies neither see nor hear you: the game's debug flags Player Hide and "
                                         "Player Silence. Off puts the flags back as they were.",
                                         "Враги вас не видят и не слышат: отладочные флаги игры Player Hide и Player "
                                         "Silence. Выкл. возвращает флаги как были."));
    }
    if (const char* label = Item("Firearm range", "Дальность огнестрела")) {
        static const char* const ranges[] = {"Off", "150%", "200%", "300%"};
        const char* const names[] = {BbSettings::MenuText(ranges[0], "Выкл."), ranges[1], ranges[2], ranges[3]};
        CheatLevels(label, s.cheat_gun_range, BbSettings::CheatGunRanges, names, BbEngine::Cheats::GunRange,
                    BbSettings::MenuText(
                        "Bullets of the pistols, rifles, blunderbusses, cannons, Gatling Gun and the trick "
                        "weapons' guns fly farther. Not Rosmarinus, the Flamesprayer, arcane or hunter tools. "
                        "Off restores the game's bullet data.",
                        "Пули пистолетов, ружей, мушкетонов, пушек, пулемёта и огнестрела трансформируемого "
                        "оружия летят дальше. Не Розмарин, огнемёт, тайные и охотничьи инструменты. Выкл. "
                        "возвращает данные пуль игры."));
    }
    if (const char* label = Item("Keep Blood Echoes on death", "Не терять отголоски при смерти")) {
        CheatToggle(label, s.cheat_keep_echoes, BbEngine::Cheats::KeepEchoes,
                    BbSettings::MenuText("Dying keeps your Blood Echoes; the bloodstain holds none. Off restores the "
                                         "game's code. Echoes you kept stay in your save.",
                                         "При смерти отголоски остаются у вас, в пятне крови их нет. Выкл. "
                                         "возвращает код игры. Сохранённые отголоски остаются в сохранении."));
    }
    if (const char* label = Item("Blood Echo multiplier", "Множитель отголосков")) {
        static const char* const multipliers[] = {"x1", "x2", "x5", "x10"};
        const char* const names[] = {BbSettings::MenuText("x1 (off)", "x1 (выкл.)"), multipliers[1], multipliers[2],
                                     multipliers[3]};
        CheatLevels(label, s.cheat_echo_multiplier, BbSettings::CheatEchoMultipliers, names,
                    BbEngine::Cheats::EchoMultiplier,
                    BbSettings::MenuText(
                        "Blood Echoes from enemies and bosses times this. An enemy already loaded may give the "
                        "normal amount until the area loads again. x1 restores the game's data; echoes gained "
                        "stay in your save.",
                        "Отголоски за врагов и боссов умножаются. Уже загруженный враг может дать обычное "
                        "количество, пока область не загрузится заново. x1 возвращает данные игры; полученные "
                        "отголоски остаются в сохранении."));
    }
    if (const char* label = Item("No weapon wear", "Оружие не изнашивается")) {
        CheatToggle(label, s.cheat_no_durability, BbEngine::Cheats::NoDurability,
                    BbSettings::MenuText("Equipped weapons are kept at full durability. Off stops it; durability is "
                                         "saved as it is.",
                                         "Экипированное оружие всегда с полной прочностью. Выкл. прекращает это; "
                                         "прочность сохраняется как есть."));
    }
    if (const char* label = Item("Infinite consumables", "Бесконечные расходники")) {
        CheatToggle(label, s.cheat_consumables, BbEngine::Cheats::Consumables,
                    BbSettings::MenuText(
                        "Molotovs, knives, pebbles, oil urns, antidotes, sedatives, elixirs, papers, Numbing Mist, "
                        "Shining Coins and similar tools are not used up. Not Blood Vials or Quicksilver Bullets "
                        "(above), key items, materials, or Insight and Blood Echo items. Off restores the item data.",
                        "Коктейли Молотова, ножи, камешки, масло, антидоты, успокоительное, эликсиры, бумага, "
                        "онемляющий туман, блестящие монеты и подобные инструменты не расходуются. Не флаконы крови и "
                        "ртутные пули (выше), не ключевые предметы, материалы, предметы озарения и отголосков. Выкл. "
                        "возвращает данные предметов."));
    }
    if (Shown("Blood Echoes", "Отголоски крови")) {
        CheatStats();
    }
}

void MouseSection();

void FpsSection() {
    auto& s = BbSettings::Get();
    Section("FPS counter", "Счётчик FPS");
    if (const char* label = Item("FPS counter in corner", "Счётчик FPS в углу")) {
        Checkbox(label, s.show_fps);
    }
    if (const char* label = Item("Note while shaders build", "Надпись при сборке шейдеров")) {
        Checkbox(label, s.show_compile_note);
        Hint(BbSettings::MenuText(
            "Shows \"Building shaders\" in a corner while the game builds shaders for a place it "
            "shows for the first time. A stutter with this note is shader building and is gone on "
            "the next visit; one without it is loading.",
            "Показывает «Сборка шейдеров» в углу, пока игра собирает шейдеры для места, которое "
            "показывает впервые. Рывок с этой надписью — сборка шейдеров, при следующем посещении "
            "его не будет; без неё — загрузка."));
    }
    if (!s.show_fps && !Searching()) {
        MouseSection();
        return;
    }
    ImGui::Indent();
    ImGui::PushID("fps_counter");
    if (const char* label = Item("Frame rate", "Частота кадров")) {
        Checkbox(label, s.show_fps_rate);
    }
    if (const char* label = Item("Frame time", "Время кадра")) {
        Checkbox(label, s.show_fps_frame_time);
    }
    if (const char* label = Item("Upscaler", "Апскейлер")) {
        Checkbox(label, s.show_fps_upscaler);
    }
    if (const char* label = Item("CPU usage", "Загрузка ЦП")) {
        Checkbox(label, s.show_fps_cpu);
    }
    if (const char* label = Item("GPU usage", "Загрузка ГП")) {
        Checkbox(label, s.show_fps_gpu);
    }
    if (const char* label = Item("Position", "Положение")) {
        const char* positions[] = {BbSettings::MenuText("Top right", "Сверху справа"),
                                   BbSettings::MenuText("Top left", "Сверху слева"),
                                   BbSettings::MenuText("Bottom right", "Снизу справа"),
                                   BbSettings::MenuText("Bottom left", "Снизу слева"),
                                   BbSettings::MenuText("Top centre", "Сверху по центру")};
        static_assert(std::size(positions) == BbSettings::FpsPositionCount);
        int position = std::clamp(s.fps_position.load(), 0, BbSettings::FpsPositionCount - 1);
        if (ImGui::Combo(label, &position, positions, BbSettings::FpsPositionCount)) {
            Store(s.fps_position, position, true);
        }
    }
    if (const char* label = Item("Size", "Размер")) {
        float scale = s.fps_scale;
        if (ImGui::SliderFloat(label, &scale, 0.75f, 2.0f, "%.2fx")) {
            Store(s.fps_scale, scale, true);
        }
    }
    const auto edit_colour = [](const char* label, std::atomic<unsigned>& rgb) {
        const unsigned v = rgb;
        float c[3] = {float((v >> 16) & 0xFF) / 255.0f, float((v >> 8) & 0xFF) / 255.0f,
                      float(v & 0xFF) / 255.0f};
        if (ImGui::ColorEdit3(label, c, ImGuiColorEditFlags_NoInputs)) {
            const auto byte = [](float f) { return unsigned(std::lround(std::clamp(f, 0.0f, 1.0f) * 255.0f)); };
            Store(rgb, (byte(c[0]) << 16) | (byte(c[1]) << 8) | byte(c[2]), true);
        }
    };
    if (const char* label = Item("Text colour", "Цвет текста")) {
        edit_colour(label, s.fps_text_color);
    }
    if (const char* label = Item("Text opacity", "Непрозрачность текста")) {
        float text_opacity = s.fps_text_opacity;
        if (ImGui::SliderFloat(label, &text_opacity, 0.1f, 1.0f, "%.2f")) {
            Store(s.fps_text_opacity, text_opacity, true);
        }
    }
    if (const char* label = Item("Background colour", "Цвет фона")) {
        edit_colour(label, s.fps_bg_color);
    }
    if (const char* label = Item("Background opacity", "Непрозрачность фона")) {
        float opacity = s.fps_bg_opacity;
        if (ImGui::SliderFloat(label, &opacity, 0.0f, 1.0f, "%.2f")) {
            Store(s.fps_bg_opacity, opacity, true);
        }
    }
    if (const char* label = Item("Border", "Рамка")) {
        Checkbox(label, s.fps_border);
    }
    ImGui::PopID();
    ImGui::Unindent();
    MouseSection();
}

void MouseSection() {
    auto& s = BbSettings::Get();
    Section("Mouse", "Мышь");
    if (const char* label = Item("Hide mouse cursor", "Скрывать курсор мыши")) {
        Checkbox(label, s.hide_cursor);
        Hint(BbSettings::MenuText("Hidden 2 seconds after the mouse stops moving; moving it brings it back.",
                                  "Скрывается через 2 секунды после остановки мыши; движение возвращает его."));
    }
}

void VoiceSection() {
    auto& s = BbSettings::Get();
    Section("Voice chat", "Голосовой чат");
    if (const char* label = Item("Voice chat", "Голосовой чат")) {
        Checkbox(label, s.voice_enabled);
        Hint(BbSettings::MenuText("Talk with the hunters in your seamless room. It goes through the room's encryption; nobody sees your address.",
                                  "Разговор с охотниками в вашей комнате. Идёт через шифрование комнаты; ваш адрес никто не видит."));
    }
    if (!s.voice_enabled) return;
    if (const char* label = Item("Mute my microphone", "Выключить мой микрофон")) Checkbox(label, s.voice_mic_muted);
    if (const char* label = Item("Push to talk", "Говорить по кнопке")) {
        Checkbox(label, s.voice_push_to_talk);
        Hint(BbSettings::MenuText("Off: your microphone sends whenever you speak.", "Выкл.: микрофон передаёт, когда вы говорите."));
    }
    if (const char* label = Item("Talk key", "Кнопка разговора")) {
        const std::string current = binding_talk_key ? std::string(BbSettings::MenuText("Press a key or mouse button...", "Нажмите кнопку..."))
                                                     : BbVoice::KeyName(s.voice_talk_key);
        if (ImGui::Button((current + "###talk_key").c_str())) binding_talk_key = true;
        ImGui::SameLine();
        ImGui::TextUnformatted(label);
        Hint(BbSettings::MenuText("Press it, then the key or mouse button you want to hold while talking. Esc keeps the old one.",
                                  "Нажмите, затем кнопку, которую будете держать во время разговора. Esc оставляет прежнюю."));
    }
    if (const char* label = Item("Voice volume", "Громкость голоса")) {
        float percent = s.voice_volume * 100.0f;
        if (ImGui::SliderFloat(label, &percent, 0.0f, 500.0f, "%.0f%%")) {
            s.voice_volume = percent / 100.0f;
            dirty = true;
        }
        Hint(BbSettings::MenuText("How loud the other hunters are, up to 500%.", "Громкость других охотников, до 500%."));
    }
    for (const auto& t : BbVoice::Talkers()) {
        ImGui::PushID(t.slot + 1000);
        bool muted = t.muted;
        const std::string label = std::string(BbSettings::MenuText("Mute ", "Заглушить ")) + t.name + (t.talking ? "  (speaking)" : "");
        if (ImGui::Checkbox(label.c_str(), &muted)) BbVoice::SetMuted(t.name, muted);
        ImGui::PopID();
    }
}

std::string ConnectionText(const BbRoom::PeerStatus& peer) {
    char text[64];
    switch (peer.state) {
    case BbRoom::LinkState::Good:
        std::snprintf(text, sizeof text, "%d %s, %s", peer.ping_ms, BbSettings::MenuText("ms", "мс"), BbSettings::MenuText("good", "хорошо"));
        return text;
    case BbRoom::LinkState::Slow:
        std::snprintf(text, sizeof text, "%d %s, %s", peer.ping_ms, BbSettings::MenuText("ms", "мс"), BbSettings::MenuText("slow", "медленно"));
        return text;
    case BbRoom::LinkState::Reconnecting:
        return BbSettings::MenuText("reconnecting", "переподключается");
    case BbRoom::LinkState::Unknown:
        break;
    }
    return BbSettings::MenuText("no ping", "нет пинга");
}

ImU32 ConnectionColor(BbRoom::LinkState state) {
    switch (state) {
    case BbRoom::LinkState::Good: return IM_COL32(150, 196, 130, 255);
    case BbRoom::LinkState::Slow: return IM_COL32(226, 186, 96, 255);
    case BbRoom::LinkState::Reconnecting: return IM_COL32(220, 120, 100, 255);
    case BbRoom::LinkState::Unknown: break;
    }
    return IM_COL32(150, 142, 126, 230);
}

const BbRoom::PeerStatus* FindPeer(const std::vector<BbRoom::PeerStatus>& peers, int slot) {
    for (const BbRoom::PeerStatus& peer : peers)
        if (peer.slot == slot) return &peer;
    return nullptr;
}

std::string VisitorName(int slot, const std::string& name) {
    return BbRoom::IsVisitorSlot(slot) ? name + BbSettings::MenuText(" [Visitor]", " [Посетитель]") : name;
}

void ConnectionList(const std::vector<std::pair<int, std::string>>& listed) {
    if (!BbSettings::Get().coop_status) return;
    for (const BbRoom::PeerStatus& peer : BbRoom::Connections()) {
        if (std::any_of(listed.begin(), listed.end(), [&](const auto& m) { return m.first == peer.slot; })) continue;
        if (BbRoom::IsHost() && BbRoom::IsVisitorSlot(peer.slot)) continue;
        ImGui::TextUnformatted(VisitorName(peer.slot, peer.name).c_str());
        ImGui::SameLine();
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(ConnectionColor(peer.state)), "%s", ConnectionText(peer).c_str());
    }
}

void CoopOptionsSection() {
    auto& s = BbSettings::Get();
    Section("Co-op notices", "Уведомления совместной игры");
    if (const char* label = Item("Connection status", "Состояние соединения")) {
        Checkbox(label, s.coop_status);
        Hint(BbSettings::MenuText("Shows each hunter's ping in the party lists, and a notice while someone is reconnecting.",
                                  "Показывает пинг охотников в списках отряда и уведомление, пока кто-то переподключается."));
    }
    if (const char* label = Item("Ping on the party HUD", "Пинг на панели отряда")) {
        Checkbox(label, s.coop_hud_ping);
        Hint(BbSettings::MenuText("Shows each hunter's ping beside the distance on their party HUD row.",
                                  "Показывает пинг охотника рядом с расстоянием в его строке на панели отряда."));
    }
    if (const char* label = Item("Join and leave notices", "Уведомления о входе и выходе")) {
        Checkbox(label, s.coop_notices);
        Hint(BbSettings::MenuText("A short line when a hunter joins, leaves, is kicked, reconnects or dies.",
                                  "Короткая строка, когда охотник входит, выходит, выгнан, переподключился или погиб."));
    }
    if (const char* label = Item("Requests to come to the host", "Просьбы перенести к хозяину")) {
        Checkbox(label, s.coop_warp_requests);
        BbRoom::SetWarpRequests(s.coop_warp_requests);
        Hint(BbSettings::MenuText("A guest asks from the F2 list and the host accepts with Bring. Off: guests cannot ask you, and you cannot ask.",
                                  "Гость просит в списке F2, хозяин принимает кнопкой «Призвать». Выкл.: вас не смогут попросить, и вы не сможете."));
    }
    if (BbRoom::IsHost() && BbRoom::OpenRoomAllowed()) {
        if (const char* label = Item("Open my room to visitors", "Открыть мою комнату для посетителей")) {
            Checkbox(label, s.coop_open_room);
            BbRoom::SetOpenRoom(s.coop_open_room);
            Hint(BbSettings::MenuText("While on, your game gives your room's invite code to Lantern's co-op server, and the Lantern event team can see it in Discord and join for surprise events, so your room is not private from them. Everyone in the room sees a line when a visitor joins, and you can Kick them. Nobody is moved: guests keep control of their own summons. Off or leaving takes the room off the list at once; New room in the launcher retires a code they already saw.",
                                      "Пока включено, ваша игра передаёт код приглашения вашей комнаты серверу Lantern, а команда мероприятий Lantern видит его в Discord и может зайти на неожиданное событие, поэтому от них комната не закрыта. Все в комнате видят строку, когда входит посетитель, и вы можете его выгнать. Никого не переносит: гости сами управляют своими призывами. Выключение или выход сразу убирает комнату из списка; «Новая комната» в лаунчере отменяет уже увиденный код."));
            if (s.coop_open_room) {
                ImGui::SameLine();
                ImGui::TextDisabled("%s", BbRoom::OpenRoomListed() ? BbSettings::MenuText("listed", "в списке")
                                                                   : BbSettings::MenuText("not listed yet", "пока не в списке"));
            }
        }
    }
}

void CoopSection() {
    namespace Online = BbEngine::OnlineFixes;
    Section("Seamless co-op", "Бесшовная совместная игра");
    if (!Item("Seamless co-op", "Бесшовная совместная игра")) return;
    if (!BbRoom::Active()) {
        ImGui::TextWrapped("%s", BbSettings::MenuText("Seamless co-op is off. Turn on Play online and Seamless co-op on the launcher's Online page.",
                                                      "Бесшовная совместная игра выключена. Включите её на странице «Онлайн» в лаунчере."));
        return;
    }
    ImGui::TextWrapped("%s", BbSettings::MenuText("In the game: F2, or hold L3 and press D-pad Up, opens the co-op menu.",
                                                  "В игре: F2 или L3 + крестовина вверх открывают меню совместной игры."));
    const bool host = BbRoom::IsHost();
    const int role = Online::PartyRole();
    const bool summoning = Online::Summoning();
    if (host) {
        if (ImGui::Button(summoning ? BbSettings::MenuText("Stop summoning", "Остановить призыв")
                                    : BbSettings::MenuText("Start summon", "Начать призыв"))) {
            if (summoning) Online::StopSummon();
            else Online::StartSummon();
        }
        Hint(BbSettings::MenuText("Summons your friends' signs until the party is full. No bell needed; friends press Send sign.",
                                  "Призывает знаки друзей, пока отряд не заполнится. Колокол не нужен; друзья нажимают «Отправить знак»."));
        int lamp_slot = 0;
        std::string lamp_who;
        std::uint32_t lamp_id = 0;
        if (BbRoom::PendingLampAsk(lamp_slot, lamp_who, lamp_id)) {
            ImGui::TextWrapped("%s%s (%s)", lamp_who.c_str(), BbSettings::MenuText(" asks the party to travel", " просит отряд отправиться"), Online::LampPlace(lamp_id).c_str());
            if (ImGui::Button(BbSettings::MenuText("Accept: the whole party travels", "Принять: весь отряд отправится"))) Online::AnswerLampAsk(true);
            ImGui::SameLine();
            if (ImGui::Button(BbSettings::MenuText("Stay here", "Остаться здесь"))) Online::AnswerLampAsk(false);
        }
        const auto members = BbRoom::PartyMembers();
        const auto peers = BbRoom::Connections();
        if (members.empty()) {
            ImGui::TextDisabled("%s", BbSettings::MenuText("Nobody in your world yet.", "В вашем мире пока никого."));
        } else {
            for (const auto& [slot, name] : members) {
                ImGui::PushID(slot);
                ImGui::TextUnformatted(VisitorName(slot, name).c_str());
                const BbRoom::PeerStatus* peer = FindPeer(peers, slot);
                if (peer && BbSettings::Get().coop_status) {
                    ImGui::SameLine();
                    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(ConnectionColor(peer->state)), "%s", ConnectionText(*peer).c_str());
                }
                if (peer && peer->asked) {
                    ImGui::SameLine();
                    ImGui::TextUnformatted(BbSettings::MenuText("asks to come", "просит перенести"));
                }
                ImGui::SameLine();
                if (ImGui::SmallButton(BbSettings::MenuText("Bring", "Призвать к себе"))) BbRoom::SendBring(slot);
                ImGui::SameLine();
                if (ImGui::SmallButton(BbSettings::MenuText("Kick", "Выгнать"))) BbRoom::Kick(slot);
                ImGui::PopID();
            }
            if (ImGui::Button(BbSettings::MenuText("Bring all players", "Призвать всех к себе"))) BbRoom::SendBring(0);
            Hint(BbSettings::MenuText("Moves hunters in your area to your side. Someone in another area comes with your lamp instead.",
                                      "Переносит охотников из вашей области к вам. Из другой области — через ваш фонарь."));
            if (ImGui::Button(BbSettings::MenuText("End session", "Завершить сеанс"))) {
                Online::StopSummon();
                for (const auto& [slot, name] : members) BbRoom::Kick(slot);
            }
            Hint(BbSettings::MenuText("Sends everyone back to their own world. Your room stays open.",
                                      "Отправляет всех в их миры. Комната остаётся открытой."));
        }
        for (const BbRoom::PeerStatus& peer : peers) {
            if (!BbRoom::IsVisitorSlot(peer.slot) || std::any_of(members.begin(), members.end(), [&](const auto& m) { return m.first == peer.slot; })) continue;
            ImGui::PushID(peer.slot);
            ImGui::TextUnformatted(VisitorName(peer.slot, peer.name).c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton(BbSettings::MenuText("Kick", "Выгнать"))) BbRoom::Kick(peer.slot);
            ImGui::PopID();
        }
        ConnectionList(members);
        return;
    }
    ConnectionList({});
    if (role == 6) {
        ImGui::TextUnformatted(BbSettings::MenuText("You are in the host's world.", "Вы в мире хозяина."));
        if (BbSettings::Get().coop_warp_requests) {
            const bool waiting = BbRoom::WarpWaiting();
            if (ImGui::Button(waiting ? BbSettings::MenuText("Waiting for the host...", "Ждём хозяина...")
                                      : BbSettings::MenuText("Ask the host to bring you", "Попросить хозяина перенести вас")) &&
                !waiting)
                BbRoom::SendWarpAsk();
            ImGui::SameLine();
        }
        if (ImGui::Button(BbSettings::MenuText("Return to your own world", "Вернуться в свой мир"))) Online::LeaveHost();
        return;
    }
    if (ImGui::Button(summoning ? BbSettings::MenuText("Take back the sign", "Убрать знак")
                                : BbSettings::MenuText("Send sign", "Отправить знак"))) {
        if (summoning) Online::StopSummon();
        else Online::StartSummon();
    }
    Hint(BbSettings::MenuText("Puts your sign in the host's world until the host summons you. No bell needed.",
                              "Ставит ваш знак в мире хозяина, пока он вас не призовёт. Колокол не нужен."));
}

enum Tab : int { TabUpscaling, TabDisplay, TabPicture, TabGameplay, TabInterface, TabCoop, TabCount };

const char* TabTitle(int tab) {
    switch (tab) {
    case TabUpscaling: return BbSettings::MenuText("Upscaling", "Апскейлинг");
    case TabDisplay: return BbSettings::MenuText("Display", "Экран");
    case TabPicture: return BbSettings::MenuText("Picture", "Изображение");
    case TabGameplay: return BbSettings::MenuText("Gameplay", "Игра");
    case TabCoop: return BbSettings::MenuText("Co-op", "Совместная игра");
    default: return BbSettings::MenuText("Interface", "Интерфейс");
    }
}

void DrawTab(int tab) {
    tab_title = TabTitle(tab);
    ImGui::PushID(tab);
    switch (tab) {
    case TabUpscaling:
        UpscalerSection();
        DlssSection();
        Fsr3Section();
        MaskSection();
        OptiScalerSection();
        break;
    case TabDisplay:
        OutputSection();
        HdrSection();
        break;
    case TabPicture:
        PictureSection();
        EffectsSection();
        break;
    case TabGameplay:
        CameraSection();
        CheatsSection();
        break;
    case TabCoop:
        CoopSection();
        CoopOptionsSection();
        VoiceSection();
        break;
    default:
        FpsSection();
        break;
    }
    section_title = nullptr;
    ImGui::PopID();
}

int current_tab = TabUpscaling;
int pending_tab = -1;

void Menu() {
    auto& s = BbSettings::Get();
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImVec2 pos(viewport->WorkPos.x + 40.0f * base_scale, viewport->WorkPos.y + 40.0f * base_scale);
    if (s.menu_x >= 0.0f && s.menu_y >= 0.0f) {
        const float margin = 80.0f * base_scale;
        pos.x = viewport->WorkPos.x +
                std::clamp(s.menu_x * viewport->WorkSize.x, 0.0f, std::max(viewport->WorkSize.x - margin, 0.0f));
        pos.y = viewport->WorkPos.y +
                std::clamp(s.menu_y * viewport->WorkSize.y, 0.0f, std::max(viewport->WorkSize.y - margin, 0.0f));
    }
    static int last_frame = -2;
    static float window_top = 0.0f;
    const float min_height = 240.0f * base_scale;
    const float bottom = viewport->WorkPos.y + viewport->WorkSize.y - 8.0f * base_scale;
    pos.y = std::min(pos.y, std::max(viewport->WorkPos.y, bottom - min_height));
    const bool appearing = ImGui::GetFrameCount() != last_frame + 1;
    const float top = appearing ? pos.y : window_top;
    const float max_height = std::max(min_height, bottom - top);
    ImGui::SetNextWindowSizeConstraints(ImVec2(360.0f * base_scale, min_height), ImVec2(FLT_MAX, max_height));
    ImGui::SetNextWindowPos(pos, ImGuiCond_Appearing);
    ImGui::SetNextWindowSize(ImVec2(640.0f * base_scale, std::min(max_height, 820.0f * base_scale)),
                             ImGuiCond_Appearing);
    bool keep_open = true;
    if (!ImGui::Begin(
            BbSettings::MenuText("Bloodborne - Graphics  (Insert / L3+R3)###bbport_settings",
                                 "Bloodborne — настройки  (Insert / L3+R3)###bbport_settings"),
            &keep_open, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
        ImGui::End();
        return;
    }
    last_frame = ImGui::GetFrameCount();
    window_top = ImGui::GetWindowPos().y;
    if (!ImGui::IsWindowAppearing() && viewport->WorkSize.x > 0.0f && viewport->WorkSize.y > 0.0f) {
        const ImVec2 at = ImGui::GetWindowPos();
        const float fx = (at.x - viewport->WorkPos.x) / viewport->WorkSize.x;
        const float fy = (at.y - viewport->WorkPos.y) / viewport->WorkSize.y;
        if (std::abs(at.x - pos.x) >= 1.0f || std::abs(at.y - pos.y) >= 1.0f) {
            s.menu_x = fx;
            s.menu_y = fy;
            dirty = true;
        }
    }

    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_F, ImGuiInputFlags_RouteGlobal)) {
        ImGui::SetKeyboardFocusHere();
    }
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##search", BbSettings::MenuText("Search settings (Ctrl+F)", "Поиск настроек (Ctrl+F)"),
                             search_text, sizeof(search_text), ImGuiInputTextFlags_EscapeClearsAll);
    search_query = Fold(search_text);

    static const char* languages[] = {"English",  "Русский",  "Español", "Português (Brasil)",
                                      "Français", "Deutsch",  "Italiano", "Polski", "Türkçe", "简体中文"};
    static_assert(std::size(languages) == BbSettings::MenuLanguageCount);
    int language = int(s.menu_language.load());
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 11.0f);
    if (ImGui::Combo(BbSettings::MenuText("Language"), &language, languages, BbSettings::MenuLanguageCount)) {
        s.menu_language =
            BbSettings::MenuLanguage(std::clamp(language, 0, BbSettings::MenuLanguageCount - 1));
        BbSettings::Save();
    }
    ImGui::SameLine();
    ImGui::Text(BbSettings::MenuText("%.0f FPS  (%.1f ms)", "%.0f FPS  (%.1f мс)"),
                frame_ms_avg > 0.0f ? 1000.0f / frame_ms_avg : 0.0f, frame_ms_avg);

    if (!Searching()) {
        const bool previous = ImGui::IsKeyPressed(ImGuiKey_GamepadL2, false) ||
                              ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_PageUp, ImGuiInputFlags_RouteGlobal);
        const bool next = ImGui::IsKeyPressed(ImGuiKey_GamepadR2, false) ||
                          ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_PageDown, ImGuiInputFlags_RouteGlobal);
        if (previous != next) {
            pending_tab = (current_tab + (next ? 1 : TabCount - 1)) % TabCount;
        }
    }
    static float footer_height = 0.0f;
    const ImVec2 page(0.0f, -std::max(footer_height, ImGui::GetFrameHeightWithSpacing() * 2.0f));
    shown_items = 0;
    if (!Searching()) {
        if (ImGui::BeginTabBar("##tabs")) {
            for (int tab = 0; tab < TabCount; ++tab) {
                const std::string label = std::string(TabTitle(tab)) + "###tab" + std::to_string(tab);
                const ImGuiTabItemFlags flags = tab == pending_tab ? ImGuiTabItemFlags_SetSelected : 0;
                if (ImGui::BeginTabItem(label.c_str(), nullptr, flags)) {
                    current_tab = tab;
                    if (ImGui::BeginChild("##page", page, ImGuiChildFlags_NavFlattened)) {
                        DrawTab(tab);
                    }
                    ImGui::EndChild();
                    ImGui::EndTabItem();
                }
            }
            ImGui::EndTabBar();
        }
        pending_tab = -1;
    } else {
        if (ImGui::BeginChild("##results", page, ImGuiChildFlags_NavFlattened)) {
            for (int tab = 0; tab < TabCount; ++tab) {
                DrawTab(tab);
            }
            if (shown_items == 0) {
                ImGui::TextDisabled(BbSettings::MenuText("Nothing matches \"%s\"", "Ничего не найдено по «%s»"),
                                    search_text);
            }
        }
        ImGui::EndChild();
    }

    const float footer_top = ImGui::GetCursorPosY();
    bool restart =
        s.object_motion != s.startup_object_motion || s.model_lod != s.startup_model_lod ||
        s.live_resolution != s.startup_live_resolution || s.hdr != s.startup_hdr ||
        s.graphics_motion_blur_quality != s.startup_motion_blur_quality ||
        s.graphics_ao_quality != s.startup_ao_quality ||
        s.graphics_shadow_resolution != s.startup_shadow_resolution ||
        s.graphics_local_shadow_resolution != s.startup_local_shadow_resolution ||
        BbSettings::ResolutionNeedsRestart();
    for (int e = 0; e < BbSettings::EffectCount; ++e) {
        restart |= !BbSettings::EffectIsLive(e) && s.effects[e] != s.startup_effects[e];
    }
    restart |= Vulkan::OptiScaler::Detect().loaded &&
               (s.optiscaler_compat != s.startup_optiscaler.compat ||
                s.optiscaler_ngx_lock != s.startup_optiscaler.ngx_lock ||
                s.optiscaler_clean_thread != s.startup_optiscaler.clean_thread ||
                s.optiscaler_sync_submit != s.startup_optiscaler.sync_submit);
    if (restart) {
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "%s",
                           BbSettings::MenuText("Changes apply after restarting the game",
                                                "Изменения применятся после перезапуска игры"));
        ImGui::SameLine();
        if (ImGui::Button(
                BbSettings::MenuText("Apply and restart game", "Применить и перезапустить игру"))) {
            BbSettings::Save();
            runtime_restart();
        }
    }
    if (ImGui::Button(BbSettings::MenuText("Close", "Закрыть"))) {
        keep_open = false;
    }
    static bool quit_armed = false;
    ImGui::SameLine();
    const std::string quit_label =
        std::string(quit_armed ? BbSettings::MenuText("Press again to quit", "Нажмите ещё раз")
                               : BbSettings::MenuText("Quit game", "Выйти из игры")) + "###quit";
    if (ImGui::Button(quit_label.c_str())) {
        if (quit_armed) {
            SDL_Event quit{};
            quit.type = SDL_EVENT_QUIT;
            SDL_PushEvent(&quit);
        }
        quit_armed = !quit_armed;
    }
    if (quit_armed && !ImGui::IsItemHovered() && !ImGui::IsItemFocused()) {
        quit_armed = false;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%s", BbSettings::MenuText("Settings are saved to lantern.ini",
                                             "Настройки сохраняются в lantern.ini"));
    ImGui::PushTextWrapPos();
    ImGui::TextDisabled("%s", BbSettings::MenuText(
                                  "Tabs: L2 / R2 or Ctrl+PgUp / Ctrl+PgDn. Search: Ctrl+F, Esc clears it. "
                                  "A setting's (?) also shows while it is selected.",
                                  "Вкладки: L2 / R2 или Ctrl+PgUp / Ctrl+PgDn. Поиск: Ctrl+F, Esc очищает. "
                                  "Подсказка (?) видна и когда настройка выбрана."));
    ImGui::PopTextWrapPos();
    footer_height = ImGui::GetCursorPosY() - footer_top + ImGui::GetStyle().ItemSpacing.y;
    ImGui::End();
    if (!keep_open) {
        SetOpen(false);
    }
}

ImVec2 fps_box_min{}, fps_box_max{};

std::mutex compile_note_mutex;
bool compile_note_primed = false;
u64 compile_note_seen_ns = 0, compile_note_burst_ns = 0;
u32 compile_note_seen = 0, compile_note_burst = 0;
std::chrono::steady_clock::time_point compile_note_last{};

bool CompileNoteActive() {
    if (!BbSettings::Get().show_compile_note) {
        return false;
    }
    const u64 ns = Vulkan::g_bb_compile_ns_total.load(std::memory_order_relaxed);
    const u32 count = Vulkan::g_bb_compiles_total.load(std::memory_order_relaxed);
    const auto now = std::chrono::steady_clock::now();
    std::scoped_lock lock{compile_note_mutex};
    if (!compile_note_primed) {
        compile_note_primed = true;
        compile_note_seen_ns = ns;
        compile_note_seen = count;
        return false;
    }
    if (ns != compile_note_seen_ns) {
        if (now - compile_note_last > std::chrono::seconds(2)) {
            compile_note_burst_ns = 0;
            compile_note_burst = 0;
        }
        compile_note_burst_ns += ns - compile_note_seen_ns;
        compile_note_burst += count - compile_note_seen;
        compile_note_seen_ns = ns;
        compile_note_seen = count;
        compile_note_last = now;
    }
    return compile_note_burst_ns >= 30'000'000ull &&
           now - compile_note_last < std::chrono::milliseconds(1500);
}

void CompileNote() {
    u32 burst;
    {
        std::scoped_lock lock{compile_note_mutex};
        burst = compile_note_burst;
    }
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float pad = 12.0f * base_scale;
    const bool beside_counter = fps_box_max.x > fps_box_min.x;
    const bool bottom = beside_counter && fps_box_min.y > viewport->WorkPos.y + viewport->WorkSize.y * 0.5f;
    const float x = beside_counter ? fps_box_max.x : viewport->WorkPos.x + viewport->WorkSize.x - pad;
    const float y = !beside_counter ? viewport->WorkPos.y + pad
                    : bottom        ? fps_box_min.y - pad * 0.5f
                                    : fps_box_max.y + pad * 0.5f;
    ImGui::SetNextWindowPos(ImVec2(x, y), ImGuiCond_Always, ImVec2(1.0f, bottom ? 1.0f : 0.0f));
    ImGui::SetNextWindowBgAlpha(0.55f);
    ImGui::Begin("##compile_note", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::Text("%s (%u)", BbSettings::MenuText("Building shaders", "Сборка шейдеров"), burst);
    ImGui::End();
}

bool RelayNoteActive() {
    const std::int64_t ms = BbRoom::ConnectedForMs();
    return ms >= 0 && ms < 8000 && BbRoom::SponsoredRelay();
}

void RelayNote() {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float pad = 24.0f * base_scale;
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f, viewport->WorkPos.y + pad),
                            ImGuiCond_Always, ImVec2(0.5f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.55f);
    ImGui::Begin("##relay_note", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::TextUnformatted(BbSettings::MenuText("Connected to the Lantern co-op relay (Frankfurt), hosted by 1337 Hosting",
                                                "Подключено к серверу совместной игры Lantern (Франкфурт), хостинг 1337 Hosting"));
    ImGui::End();
}

bool SummonNoteActive() {
    std::string text;
    return BbRoom::CurrentNotice(text);
}

void SummonNote() {
    std::string text;
    if (!BbRoom::CurrentNotice(text)) return;
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float k = base_scale;
    ImDrawList* draw = ImGui::GetForegroundDrawList();
    ImFont* font = name_font ? name_font : ImGui::GetFont();
    const float px = ImGui::GetFontSize() * 1.35f;
    const ImVec2 size = font->CalcTextSizeA(px, FLT_MAX, 0.0f, text.c_str());
    const float cx = viewport->WorkPos.x + viewport->WorkSize.x * 0.5f;
    const float y = viewport->WorkPos.y + viewport->WorkSize.y * 0.72f;
    const float half = std::max(size.x * 0.5f + 80.0f * k, 360.0f * k);
    const ImVec2 a(cx - half, y), b(cx + half, y + size.y + 24.0f * k);
    draw->AddRectFilledMultiColor(a, ImVec2(cx, b.y), IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 190), IM_COL32(0, 0, 0, 190),
                                  IM_COL32(0, 0, 0, 0));
    draw->AddRectFilledMultiColor(ImVec2(cx, a.y), b, IM_COL32(0, 0, 0, 190), IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 0),
                                  IM_COL32(0, 0, 0, 190));
    draw->AddLine(a, ImVec2(b.x, a.y), IM_COL32(200, 190, 170, 140), 1.0f * k);
    draw->AddLine(ImVec2(a.x, b.y), b, IM_COL32(200, 190, 170, 140), 1.0f * k);
    draw->AddText(font, px, ImVec2(cx - size.x * 0.5f, y + 12.0f * k), IM_COL32(226, 216, 192, 255), text.c_str());
}

std::vector<BbRoom::PartyLine> ShownPartyLines() {
    const auto& s = BbSettings::Get();
    std::vector<BbRoom::PartyLine> shown;
    for (BbRoom::PartyLine& line : BbRoom::PartyLines()) {
        const bool warp = line.kind == BbRoom::PartyLineKind::WarpAsked || line.kind == BbRoom::PartyLineKind::WarpSeen ||
                          line.kind == BbRoom::PartyLineKind::WarpOff || line.kind == BbRoom::PartyLineKind::WarpNoAnswer ||
                          line.kind == BbRoom::PartyLineKind::BringFar;
        if (line.kind == BbRoom::PartyLineKind::Visitor || (warp ? s.coop_warp_requests.load() : s.coop_notices.load())) shown.push_back(std::move(line));
    }
    return shown;
}

std::string PartyLineText(const BbRoom::PartyLine& line) {
    const std::string who = line.name.empty() ? std::string(BbSettings::MenuText("A hunter", "Охотник")) : line.name;
    switch (line.kind) {
    case BbRoom::PartyLineKind::EnteredRoom: return who + BbSettings::MenuText(" entered the room", " вошёл в комнату");
    case BbRoom::PartyLineKind::JoinedWorld: return who + BbSettings::MenuText(" joined the world", " присоединился к миру");
    case BbRoom::PartyLineKind::Left: return who + BbSettings::MenuText(" left", " вышел");
    case BbRoom::PartyLineKind::Kicked: return who + BbSettings::MenuText(" was kicked", " выгнан");
    case BbRoom::PartyLineKind::Reconnected: return who + BbSettings::MenuText(" reconnected", " переподключился");
    case BbRoom::PartyLineKind::Died: return who + BbSettings::MenuText(" died", " погиб");
    case BbRoom::PartyLineKind::BackOnline: return BbSettings::MenuText("Connection is back", "Соединение восстановлено");
    case BbRoom::PartyLineKind::WarpAsked: return who + BbSettings::MenuText(" asks to come to you, press F2 then Bring", " просит перенести к вам, нажмите F2, затем Призвать");
    case BbRoom::PartyLineKind::WarpSeen: return BbSettings::MenuText("The host has your request", "Хозяин получил вашу просьбу");
    case BbRoom::PartyLineKind::WarpOff: return BbSettings::MenuText("The host is not taking requests", "Хозяин не принимает просьбы");
    case BbRoom::PartyLineKind::WarpNoAnswer: return BbSettings::MenuText("The host did not answer, maybe an older version", "Хозяин не ответил, возможно старая версия");
    case BbRoom::PartyLineKind::LampAsked: return who + BbSettings::MenuText(" asks the party to travel, press F2 to answer", " просит отряд отправиться, нажмите F2, чтобы ответить");
    case BbRoom::PartyLineKind::LampSeen: return BbSettings::MenuText("The host has your request to travel", "Хозяин получил вашу просьбу отправиться");
    case BbRoom::PartyLineKind::LampAgreed: return BbSettings::MenuText("The host accepted: the party travels", "Хозяин согласился: отряд отправляется");
    case BbRoom::PartyLineKind::LampRefused: return BbSettings::MenuText("The host stays here, you stay with the party", "Хозяин остаётся здесь, вы остаётесь с отрядом");
    case BbRoom::PartyLineKind::LampNoAnswer: return BbSettings::MenuText("The host did not answer, you stay with the party", "Хозяин не ответил, вы остаётесь с отрядом");
    case BbRoom::PartyLineKind::HostTravels: return BbSettings::MenuText("The host is travelling, you follow", "Хозяин отправляется в путь, вы следуете за ним");
    case BbRoom::PartyLineKind::TravelLost: return BbSettings::MenuText("The party split on the way, the bells bring it back together", "Группа разделилась в пути, колокола соберут её снова");
    case BbRoom::PartyLineKind::StayedWithParty: return BbSettings::MenuText("You were taken, but the party holds on to you: you are back with the host", "Вас схватили, но отряд вас не отпустил: вы снова рядом с хозяином");
    case BbRoom::PartyLineKind::BringFar: return BbSettings::MenuText("The host is in another area, use the host's lamp", "Хозяин в другой области, идите через его фонарь");
    case BbRoom::PartyLineKind::Visitor: return BbSettings::MenuText("A visitor from the event team entered the room", "В комнату вошёл посетитель из команды мероприятий");
    }
    return {};
}

void PartyLines() {
    const auto lines = ShownPartyLines();
    if (lines.empty()) return;
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float k = base_scale;
    ImDrawList* draw = ImGui::GetForegroundDrawList();
    ImFont* font = name_font ? name_font : ImGui::GetFont();
    const float px = ImGui::GetFontSize() * 1.15f;
    const float right = viewport->WorkPos.x + viewport->WorkSize.x - 40.0f * k;
    float y = viewport->WorkPos.y + 240.0f * k;
    for (const BbRoom::PartyLine& line : lines) {
        const std::string text = PartyLineText(line);
        const ImVec2 size = font->CalcTextSizeA(px, FLT_MAX, 0.0f, text.c_str());
        const int shade = int(170.0f * line.alpha);
        const ImVec2 a(right - size.x - 60.0f * k, y), b(right + 12.0f * k, y + size.y + 10.0f * k);
        draw->AddRectFilledMultiColor(a, b, IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, shade), IM_COL32(0, 0, 0, shade), IM_COL32(0, 0, 0, 0));
        draw->AddText(font, px, ImVec2(right - size.x, y + 5.0f * k), IM_COL32(226, 216, 192, int(255.0f * line.alpha)), text.c_str());
        y += size.y + 16.0f * k;
    }
}

bool ReconnectNoteActive() {
    BbRoom::ReconnectNotice notice;
    return BbSettings::Get().coop_status && BbRoom::CurrentReconnect(notice);
}

void ReconnectNote() {
    BbRoom::ReconnectNotice notice;
    if (!BbSettings::Get().coop_status || !BbRoom::CurrentReconnect(notice)) return;
    std::string text;
    if (notice.gave_up) {
        text = BbSettings::MenuText("Could not reach the room, seamless co-op is off until the next start",
                                    "Комната недоступна, совместная игра выключена до следующего запуска");
    } else if (notice.own) {
        text = std::string(BbSettings::MenuText("Connection lost, reconnecting... ", "Соединение потеряно, переподключение... ")) + std::to_string(notice.seconds) +
               BbSettings::MenuText(" s", " с");
    } else {
        for (const std::string& name : notice.names) text += (text.empty() ? "" : ", ") + name;
        text += BbSettings::MenuText(" reconnecting...", " переподключается...");
    }
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float k = base_scale;
    ImDrawList* draw = ImGui::GetForegroundDrawList();
    ImFont* font = name_font ? name_font : ImGui::GetFont();
    const float px = ImGui::GetFontSize() * 1.1f;
    const ImVec2 size = font->CalcTextSizeA(px, FLT_MAX, 0.0f, text.c_str());
    const float cx = viewport->WorkPos.x + viewport->WorkSize.x * 0.5f;
    const float y = viewport->WorkPos.y + 64.0f * k;
    const float half = size.x * 0.5f + 60.0f * k;
    draw->AddRectFilledMultiColor(ImVec2(cx - half, y), ImVec2(cx, y + size.y + 12.0f * k), IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 190), IM_COL32(0, 0, 0, 190), IM_COL32(0, 0, 0, 0));
    draw->AddRectFilledMultiColor(ImVec2(cx, y), ImVec2(cx + half, y + size.y + 12.0f * k), IM_COL32(0, 0, 0, 190), IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 190));
    draw->AddText(font, px, ImVec2(cx - size.x * 0.5f, y + 6.0f * k), IM_COL32(226, 186, 96, 255), text.c_str());
}

bool AnnouncementActive() {
    std::string text;
    float alpha = 0.0f;
    return BbRoom::CurrentAnnouncement(text, alpha);
}

void Announcement() {
    std::string text;
    float alpha = 0.0f;
    if (!BbRoom::CurrentAnnouncement(text, alpha)) return;
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float k = base_scale;
    ImDrawList* draw = ImGui::GetForegroundDrawList();
    ImFont* font = name_font ? name_font : ImGui::GetFont();
    const char* title = BbSettings::MenuText("Server announcement", "Объявление сервера");
    const float px = ImGui::GetFontSize() * 1.35f;
    const float title_px = ImGui::GetFontSize() * 0.95f;
    const float wrap = std::min(viewport->WorkSize.x * 0.7f, 900.0f * k);
    const ImVec2 size = font->CalcTextSizeA(px, FLT_MAX, wrap, text.c_str());
    const ImVec2 title_size = font->CalcTextSizeA(title_px, FLT_MAX, 0.0f, title);
    const float cx = viewport->WorkPos.x + viewport->WorkSize.x * 0.5f;
    const float y = viewport->WorkPos.y + viewport->WorkSize.y * 0.12f;
    const float half = std::max(std::max(size.x, title_size.x) * 0.5f + 80.0f * k, 360.0f * k);
    const float height = title_size.y + size.y + 34.0f * k;
    const ImVec2 a(cx - half, y), b(cx + half, y + height);
    const auto shade = [&](int r, int g, int b_, int a_) { return IM_COL32(r, g, b_, int(float(a_) * alpha)); };
    draw->AddRectFilledMultiColor(a, ImVec2(cx, b.y), shade(0, 0, 0, 0), shade(0, 0, 0, 200), shade(0, 0, 0, 200), shade(0, 0, 0, 0));
    draw->AddRectFilledMultiColor(ImVec2(cx, a.y), b, shade(0, 0, 0, 200), shade(0, 0, 0, 0), shade(0, 0, 0, 0), shade(0, 0, 0, 200));
    draw->AddLine(a, ImVec2(b.x, a.y), shade(200, 190, 170, 140), 1.0f * k);
    draw->AddLine(ImVec2(a.x, b.y), b, shade(200, 190, 170, 140), 1.0f * k);
    draw->AddText(font, title_px, ImVec2(cx - title_size.x * 0.5f, y + 10.0f * k), shade(170, 160, 138, 255), title);
    draw->AddText(font, px, ImVec2(cx - size.x * 0.5f, y + title_size.y + 22.0f * k), shade(226, 216, 192, 255), text.c_str(), nullptr, wrap);
}

void FpsCounter() {
    const auto& s = BbSettings::Get();
    char text[128];
    int n = 0;
    if (s.show_fps_rate) {
        n += std::snprintf(text + n, sizeof(text) - n, "%.0f FPS",
                           frame_ms_avg > 0.0f ? 1000.0f / frame_ms_avg : 0.0f);
        if (s.frame_gen_active) {
            n += std::snprintf(text + n, sizeof(text) - n, " (game %.0f, frame gen 2x)",
                               s.frame_gen_game_fps.load());
        }
    }
    if (s.show_fps_frame_time) {
        n += std::snprintf(text + n, sizeof(text) - n, "%s%.1f ms", n ? "  " : "", frame_ms_avg);
    }
    if (s.show_fps_upscaler) {
        n += std::snprintf(text + n, sizeof(text) - n, "%s%s", n ? "  " : "",
                           BbSettings::UpscalerName(s.upscaler));
    }
    if (s.show_fps_cpu || s.show_fps_gpu) {
        const BbUsage::Sample use = BbUsage::Get(s.show_fps_cpu, s.show_fps_gpu);
        if (s.show_fps_cpu && use.cpu >= 0.0f) {
            n += std::snprintf(text + n, sizeof(text) - n, "%sCPU %.0f%%", n ? "  " : "", use.cpu);
        }
        if (s.show_fps_gpu && use.gpu >= 0.0f) {
            n += std::snprintf(text + n, sizeof(text) - n, "%sGPU %.0f%%", n ? "  " : "", use.gpu);
        }
    }
    if (n == 0) {
        return;
    }
    static char shown[sizeof(text)] = "";
    static auto shown_at = std::chrono::steady_clock::time_point{};
    const auto now = std::chrono::steady_clock::now();
    if (shown[0] == '\0' || now - shown_at >= std::chrono::milliseconds(500)) {
        std::memcpy(shown, text, sizeof(text));
        shown_at = now;
    }
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float pad = 12.0f * base_scale;
    const int position = std::clamp(s.fps_position.load(), 0, BbSettings::FpsPositionCount - 1);
    const bool right = position == BbSettings::FpsTopRight || position == BbSettings::FpsBottomRight;
    const bool bottom = position == BbSettings::FpsBottomRight || position == BbSettings::FpsBottomLeft;
    const bool centre = position == BbSettings::FpsTopCentre;
    const float x = centre  ? viewport->WorkPos.x + viewport->WorkSize.x * 0.5f
                    : right ? viewport->WorkPos.x + viewport->WorkSize.x - pad
                            : viewport->WorkPos.x + pad;
    const float y = bottom ? viewport->WorkPos.y + viewport->WorkSize.y - pad : viewport->WorkPos.y + pad;
    ImGui::SetNextWindowPos(ImVec2(x, y), ImGuiCond_Always,
                            ImVec2(centre ? 0.5f : right ? 1.0f : 0.0f, bottom ? 1.0f : 0.0f));
    static float min_width = 0.0f;
    static size_t min_width_key = 0;
    const size_t width_key = size_t(s.show_fps_rate) | size_t(s.show_fps_frame_time) << 1 |
                             size_t(s.show_fps_upscaler) << 2 | size_t(s.show_fps_cpu) << 3 |
                             size_t(s.show_fps_gpu) << 4 | size_t(s.frame_gen_active) << 5 |
                             size_t(s.upscaler) << 6 | size_t(s.fps_scale * 100.0f) << 12;
    if (width_key != min_width_key) {
        min_width = 0.0f;
        min_width_key = width_key;
    }
    ImGui::SetNextWindowSizeConstraints(ImVec2(min_width, 0.0f), ImVec2(FLT_MAX, FLT_MAX));
    const auto colour = [](unsigned rgb, float alpha) {
        return ImVec4(float((rgb >> 16) & 0xFF) / 255.0f, float((rgb >> 8) & 0xFF) / 255.0f,
                      float(rgb & 0xFF) / 255.0f, alpha);
    };
    ImGui::PushStyleColor(ImGuiCol_WindowBg, colour(s.fps_bg_color, s.fps_bg_opacity));
    ImGui::PushStyleColor(ImGuiCol_Text, colour(s.fps_text_color, std::clamp(s.fps_text_opacity.load(), 0.1f, 1.0f)));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, s.fps_border ? 1.0f : 0.0f);
    ImGui::Begin("##fps", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::SetWindowFontScale(std::clamp(s.fps_scale.load(), 0.75f, 2.0f));
    ImGui::TextUnformatted(shown);
    min_width = std::max(min_width, ImGui::GetWindowWidth());
    fps_box_min = ImGui::GetWindowPos();
    fps_box_max = ImVec2(fps_box_min.x + ImGui::GetWindowWidth(), fps_box_min.y + ImGui::GetWindowHeight());
    ImGui::End();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(2);
}

void GameBar(ImDrawList* draw, ImVec2 at, ImVec2 size, float fill, ImU32 light, ImU32 dark, float dim, float k) {
    const auto faded = [dim](ImU32 c) {
        const ImU32 a = ImU32(float((c >> IM_COL32_A_SHIFT) & 0xFF) * dim);
        return (c & ~IM_COL32_A_MASK) | (a << IM_COL32_A_SHIFT);
    };
    const ImVec2 end(at.x + size.x, at.y + size.y);
    draw->AddRectFilled(ImVec2(at.x - k, at.y - k), ImVec2(end.x + k, end.y + k), IM_COL32(0, 0, 0, 170));
    draw->AddRectFilled(at, end, IM_COL32(24, 14, 12, 210));
    if (fill > 0.0f) {
        const ImVec2 fill_end(at.x + size.x * fill, end.y);
        draw->AddRectFilledMultiColor(at, fill_end, faded(light), faded(light), faded(dark), faded(dark));
        draw->AddLine(ImVec2(at.x, at.y + 0.5f * k), ImVec2(fill_end.x, at.y + 0.5f * k), faded(IM_COL32(255, 220, 200, 70)),
                      k);
    }
    draw->AddRect(ImVec2(at.x - k, at.y - k), ImVec2(end.x + k, end.y + k), IM_COL32(168, 156, 136, int(200 * dim)), 0.0f,
                  0, k);
    draw->AddRectFilled(ImVec2(end.x, at.y - 2.0f * k), ImVec2(end.x + 2.0f * k, end.y + 2.0f * k),
                        IM_COL32(200, 190, 170, int(220 * dim)));
}

constexpr float kOrnamentArm = 64.0f / 153.0f, kOrnamentScroll = 43.0f / 153.0f;

ImTextureData* GaugeTexture() {
    static ImTextureData* texture = nullptr;
    if (texture) return texture;
    BbHudGauge::Start(base_scale >= 1.4f);
    const BbHudGauge::Art* art = BbHudGauge::Ready();
    if (!art) return nullptr;
    texture = IM_NEW(ImTextureData)();
    texture->Create(ImTextureFormat_RGBA32, art->width, art->height);
    std::memcpy(texture->GetPixels(), art->pixels.data(), art->pixels.size() * 4);
    ImGui::RegisterUserTexture(texture);
    return texture;
}

void GaugePiece(ImDrawList* draw, ImTextureData* texture, const BbHudGauge::Piece& piece, ImVec2 a, ImVec2 b, float fill,
                ImU32 tint) {
    const float w = float(texture->Width), h = float(texture->Height);
    const ImVec2 uv0(float(piece.x) / w, float(piece.y) / h);
    const ImVec2 uv1(float(piece.x + piece.w * fill) / w, float(piece.y + piece.h) / h);
    draw->AddImage(texture->GetTexRef(), a, ImVec2(a.x + (b.x - a.x) * fill, b.y), uv0, uv1, tint);
}

void TexturedBar(ImDrawList* draw, ImTextureData* texture, const BbHudGauge::Piece& fill_piece, ImVec2 at, ImVec2 size,
                 float fill, float dim, float k) {
    const BbHudGauge::Art* art = BbHudGauge::Ready();
    const ImU32 tint = IM_COL32(255, 255, 255, int(255 * dim));
    const float inset_x = size.y * 0.35f, inset_y = size.y * 0.25f;
    GaugePiece(draw, texture, art->frame, ImVec2(at.x - inset_x, at.y - inset_y), ImVec2(at.x + size.x + inset_x, at.y + size.y + inset_y),
               1.0f, tint);
    if (fill > 0.0f) {
        GaugePiece(draw, texture, fill_piece, at, ImVec2(at.x + size.x, at.y + size.y), fill, tint);
        const float edge = at.x + size.x * fill;
        draw->AddRectFilled(ImVec2(edge - k, at.y), ImVec2(edge + k, at.y + size.y), IM_COL32(230, 226, 214, int(230 * dim)));
    }
}

void Ornament(ImDrawList* draw, ImTextureData* texture, const BbHudGauge::Piece& piece, ImVec2 a, ImVec2 b, bool upright,
              ImU32 tint) {
    const float w = float(texture->Width), h = float(texture->Height);
    const float arm = float(piece.w) * kOrnamentArm, scroll = float(piece.w) * kOrnamentScroll;
    const float total = arm * 2.0f + scroll;
    const float length = upright ? b.y - a.y : b.x - a.x;
    const float v0 = float(piece.y) / h, v1 = float(piece.y + piece.h) / h;
    struct Part {
        float from, size, u0, u1;
    };
    const float u = float(piece.x);
    const Part parts[] = {{0.0f, arm, u, u + arm}, {arm, scroll, u + arm, u + arm + scroll}, {arm + scroll, arm, u + arm, u}};
    for (const Part& part : parts) {
        const float s0 = part.from / total * length, s1 = (part.from + part.size) / total * length;
        const ImVec2 uv0(part.u0 / w, v0), uv1(part.u1 / w, v1);
        if (!upright) {
            draw->AddImage(texture->GetTexRef(), ImVec2(a.x + s0, a.y), ImVec2(a.x + s1, b.y), uv0, uv1, tint);
        } else {
            draw->AddImageQuad(texture->GetTexRef(), ImVec2(a.x, a.y + s0), ImVec2(a.x, a.y + s1), ImVec2(b.x, a.y + s1),
                               ImVec2(b.x, a.y + s0), ImVec2(uv0.x, uv0.y), ImVec2(uv1.x, uv0.y), ImVec2(uv1.x, uv1.y),
                               ImVec2(uv0.x, uv1.y), tint);
        }
    }
}

void Flourish(ImDrawList* draw, ImTextureData* texture, const BbHudGauge::Piece& piece, ImVec2 a, ImVec2 b, ImU32 tint,
              bool flip = false) {
    const float w = float(texture->Width), h = float(texture->Height);
    const float arm = float(piece.w) * kOrnamentArm, scroll = float(piece.w) * kOrnamentScroll;
    float v0 = float(piece.y) / h, v1 = float(piece.y + piece.h) / h;
    if (flip) std::swap(v0, v1);
    const float u = float(piece.x);
    const float height = b.y - a.y;
    const float curl = std::min(height * scroll / float(piece.h), (b.x - a.x) * 0.45f);
    const float line0 = u + arm * 0.45f, line1 = u + arm * 0.95f;
    draw->AddImage(texture->GetTexRef(), a, ImVec2(a.x + curl, b.y), ImVec2((u + arm) / w, v0), ImVec2((u + arm + scroll) / w, v1), tint);
    draw->AddImage(texture->GetTexRef(), ImVec2(a.x + curl, a.y), ImVec2(b.x - curl, b.y), ImVec2(line0 / w, v0), ImVec2(line1 / w, v1), tint);
    draw->AddImage(texture->GetTexRef(), ImVec2(b.x - curl, a.y), b, ImVec2((u + arm + scroll) / w, v0), ImVec2((u + arm) / w, v1), tint);
}

ImTextureData* PortraitTexture(const std::string& key, const std::vector<std::uint32_t>& rgba, std::uint32_t version) {
    struct Entry {
        ImTextureData* texture = nullptr;
        std::uint32_t version = 0;
    };
    static std::map<std::string, Entry> cache;
    Entry& e = cache[key];
    if (rgba.size() != std::size_t(BbFaceShot::kSize) * BbFaceShot::kSize) return e.texture;
    if (!e.texture) {
        e.texture = IM_NEW(ImTextureData)();
        e.texture->Create(ImTextureFormat_RGBA32, BbFaceShot::kSize, BbFaceShot::kSize);
        std::memcpy(e.texture->GetPixels(), rgba.data(), rgba.size() * 4);
        ImGui::RegisterUserTexture(e.texture);
        e.version = version;
    } else if (e.version != version && e.texture->Status == ImTextureStatus_OK) {
        std::memcpy(e.texture->GetPixels(), rgba.data(), rgba.size() * 4);
        const ImTextureRect all{0, 0, std::uint16_t(BbFaceShot::kSize), std::uint16_t(BbFaceShot::kSize)};
        e.texture->Updates.push_back(all);
        e.texture->UpdateRect = all;
        e.texture->SetStatus(ImTextureStatus_WantUpdates);
        e.version = version;
    }
    return e.texture;
}

ImTextureData* PortraitOf(const std::string& name) {
    static std::map<std::string, std::pair<std::vector<std::uint32_t>, std::uint32_t>> pixels;
    auto& [rgba, version] = pixels[name];
    if (!BbFaceShot::Get(name, rgba, version)) return nullptr;
    return PortraitTexture("party:" + name, rgba, version);
}

void PortraitToast() {
    static std::vector<std::uint32_t> rgba;
    static std::uint32_t version = 0;
    if (!BbFaceShot::JustTaken() || !BbFaceShot::OwnRgba(rgba, version)) return;
    ImTextureData* texture = PortraitTexture("own", rgba, version);
    if (!texture) return;
    const float k = base_scale;
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImDrawList* draw = ImGui::GetForegroundDrawList();
    const float size = 96.0f * k;
    const ImVec2 a(viewport->WorkPos.x + viewport->WorkSize.x - size - 40.0f * k, viewport->WorkPos.y + 120.0f * k);
    draw->AddRectFilled(ImVec2(a.x - 4.0f * k, a.y - 4.0f * k), ImVec2(a.x + size + 4.0f * k, a.y + size + 4.0f * k),
                        IM_COL32(0, 0, 0, 200));
    draw->AddImage(texture->GetTexRef(), a, ImVec2(a.x + size, a.y + size));
    ImFont* font = name_font ? name_font : ImGui::GetFont();
    const char* text = BbSettings::MenuText("Portrait saved", "Портрет сохранён");
    const float px = ImGui::GetFontSize() * 1.1f;
    const ImVec2 t = font->CalcTextSizeA(px, FLT_MAX, 0.0f, text);
    draw->AddText(font, px, ImVec2(a.x + (size - t.x) * 0.5f, a.y + size + 8.0f * k), IM_COL32(226, 216, 192, 255), text);
}

void SpectatorBanner() {
    std::string name;
    int choices = 0;
    if (!BbEngine::OnlineFixes::SpectatedName(name, choices)) return;
    const float k = base_scale;
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImDrawList* draw = ImGui::GetBackgroundDrawList();
    ImFont* font = name_font ? name_font : ImGui::GetFont();
    const ImU32 parchment = IM_COL32(226, 216, 192, 255), shadow = IM_COL32(0, 0, 0, 210);
    const float center = viewport->WorkPos.x + viewport->WorkSize.x * 0.5f;
    const float top = viewport->WorkPos.y + viewport->WorkSize.y - 190.0f * k;
    const float title_size = ImGui::GetFontSize() * 1.0f, name_size = ImGui::GetFontSize() * 1.6f, hint_size = ImGui::GetFontSize() * 0.9f;
    const float half = 300.0f * k;
    draw->AddRectFilledMultiColor(ImVec2(center - half, top - 8.0f * k), ImVec2(center, top + 86.0f * k), IM_COL32(4, 4, 6, 0),
                                  IM_COL32(4, 4, 6, 170), IM_COL32(4, 4, 6, 170), IM_COL32(4, 4, 6, 0));
    draw->AddRectFilledMultiColor(ImVec2(center, top - 8.0f * k), ImVec2(center + half, top + 86.0f * k), IM_COL32(4, 4, 6, 170),
                                  IM_COL32(4, 4, 6, 0), IM_COL32(4, 4, 6, 0), IM_COL32(4, 4, 6, 170));
    const auto line = [&](const char* text, float px, float y, ImU32 colour) {
        const ImVec2 size = font->CalcTextSizeA(px, FLT_MAX, 0.0f, text);
        const ImVec2 at(center - size.x * 0.5f, y);
        draw->AddText(font, px, ImVec2(at.x + k, at.y + k), shadow, text);
        draw->AddText(font, px, at, colour, text);
    };
    line(BbSettings::MenuText("Spectating", "Наблюдение"), title_size, top, IM_COL32(196, 160, 92, 255));
    line(name.c_str(), name_size, top + 22.0f * k, parchment);
    if (ImTextureData* gauge = GaugeTexture()) {
        if (const BbHudGauge::Art* art = BbHudGauge::Ready()) {
            const float w = 200.0f * k, h = w * float(art->header.h) / (float(art->header.w) * (2.0f * kOrnamentArm + kOrnamentScroll));
            Ornament(draw, gauge, art->header, ImVec2(center - w * 0.5f, top - h - 2.0f * k), ImVec2(center + w * 0.5f, top - 2.0f * k), false,
                     IM_COL32(255, 255, 255, 220));
        }
    }
    if (choices > 1) {
        const bool mouse = host_input_device() == InputDevice::KeyboardMouse;
        line(mouse ? BbSettings::MenuText("Left mouse: next hunter", "ЛКМ: следующий охотник")
                   : BbSettings::MenuText("R1 / RB: next hunter", "R1 / RB: следующий охотник"),
             hint_size, top + 58.0f * k, IM_COL32(226, 216, 192, 200));
    }
}

void VoiceBadge(ImDrawList* draw, ImVec2 corner, bool muted, float k) {
    const float radius = 10.0f * k;
    const ImVec2 c(corner.x - radius * 0.35f, corner.y - radius * 0.35f);
    draw->AddCircleFilled(c, radius, IM_COL32(8, 7, 8, 235), 24);
    draw->AddCircle(c, radius, IM_COL32(196, 176, 130, 230), 24, 1.0f * k);
    if (muted) {
        const ImU32 red = IM_COL32(222, 74, 58, 255);
        const float mic = 2.4f * k;
        draw->AddRectFilled(ImVec2(c.x - mic, c.y - 6.0f * k), ImVec2(c.x + mic, c.y + 1.5f * k), red, mic);
        draw->PathArcTo(ImVec2(c.x, c.y), 4.4f * k, 0.0f, 3.14159f, 12);
        draw->PathStroke(red, 0, 1.2f * k);
        draw->AddLine(ImVec2(c.x, c.y + 4.4f * k), ImVec2(c.x, c.y + 6.6f * k), red, 1.2f * k);
        draw->AddLine(ImVec2(c.x - 6.0f * k, c.y - 6.0f * k), ImVec2(c.x + 6.0f * k, c.y + 6.0f * k), red, 1.6f * k);
        return;
    }
    const float now = float(ImGui::GetTime());
    static constexpr float kSpeed[4] = {7.1f, 9.3f, 6.2f, 8.4f}, kPhase[4] = {0.0f, 1.4f, 2.6f, 0.8f};
    const float wide = 2.0f * k, gap = 1.2f * k;
    const float start = c.x - (wide * 4.0f + gap * 3.0f) * 0.5f;
    for (int i = 0; i < 4; ++i) {
        const float high = (2.5f + 4.0f * (0.5f + 0.5f * std::sin(now * kSpeed[i] + kPhase[i]))) * k;
        const float x = start + float(i) * (wide + gap);
        draw->AddRectFilled(ImVec2(x, c.y - high), ImVec2(x + wide, c.y + high), IM_COL32(226, 186, 96, 255));
    }
}

void PartyHud(const std::vector<BbEngine::OnlineFixes::PartyRow>& rows) {
    ImTextureData* gauge = GaugeTexture();
    const BbHudGauge::Art* art = gauge ? BbHudGauge::Ready() : nullptr;
    static const ImU32 accents[] = {IM_COL32(196, 160, 92, 255), IM_COL32(92, 160, 170, 255),
                                    IM_COL32(176, 78, 64, 255), IM_COL32(140, 110, 180, 255)};
    const float k = base_scale;
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImDrawList* draw = ImGui::GetBackgroundDrawList();
    const float left = viewport->WorkPos.x + 40.0f * k;
    const float frame_h = 50.0f * k;
    const float frame_w = art ? frame_h * float(art->portrait.w) / float(art->portrait.h) : frame_h;
    const float card_w = 320.0f * k, card_h = frame_h + 10.0f * k, gap = 6.0f * k;
    const ImU32 fallen = IM_COL32(176, 52, 44, 255);
    float top = viewport->WorkPos.y + 240.0f * k;
    ImFont* font = name_font ? name_font : ImGui::GetFont();
    const float name_size = ImGui::GetFontSize() * 1.3f;
    const float where_size = ImGui::GetFontSize() * 1.0f;
    const ImU32 parchment = IM_COL32(226, 216, 192, 255);
    const float ornament_ratio = art ? float(art->header.h) / (float(art->header.w) * (2.0f * kOrnamentArm + kOrnamentScroll)) : 0.0f;
    if (art && !rows.empty()) {
        const float header_w = 200.0f * k, header_h = header_w * ornament_ratio;
        Ornament(draw, gauge, art->header, ImVec2(left, top - header_h - 4.0f * k), ImVec2(left + header_w, top - 4.0f * k),
                 false, IM_COL32(255, 255, 255, 220));
    }
    const auto talkers = BbVoice::Talkers();
    const auto peers = BbRoom::Connections();
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const auto& row = rows[i];
        const ImU32 accent = row.down ? fallen : accents[i % std::size(accents)];
        const float dim = row.down ? 0.45f : 1.0f;
        const ImVec2 a(left, top), b(left + card_w, top + card_h);
        draw->AddRectFilledMultiColor(a, b, IM_COL32(4, 4, 6, 170), IM_COL32(4, 4, 6, 0), IM_COL32(4, 4, 6, 0),
                                      IM_COL32(4, 4, 6, 170));
        if (art) {
            const float thick = 8.0f * k;
            const ImU32 bright = IM_COL32(((accent >> IM_COL32_R_SHIFT) & 0xFF) / 2 + 128, ((accent >> IM_COL32_G_SHIFT) & 0xFF) / 2 + 128,
                                          ((accent >> IM_COL32_B_SHIFT) & 0xFF) / 2 + 128, 255);
            Ornament(draw, gauge, art->header, ImVec2(left - thick - 3.0f * k, top), ImVec2(left - 3.0f * k, top + card_h), true,
                     bright);
        } else {
            draw->AddRectFilled(a, ImVec2(left + 1.5f * k, top + card_h), accent);
        }
        const ImVec2 box(left + 8.0f * k, top + 5.0f * k);
        const ImVec2 box_end(box.x + frame_w, box.y + frame_h);
        char initial[8] = {row.name.empty() ? '?' : char(std::toupper(static_cast<unsigned char>(row.name[0]))), 0};
        const float initial_px = name_size * 1.5f;
        const ImVec2 initial_size = font->CalcTextSizeA(initial_px, FLT_MAX, 0.0f, initial);
        const ImVec2 initial_at(box.x + (frame_w - initial_size.x) * 0.5f, box.y + (frame_h - initial_size.y) * 0.5f);
        const float inset = 4.0f * k;
        const ImVec2 inner(box.x + inset, box.y + inset), inner_end(box_end.x - inset, box_end.y - inset);
        ImTextureData* face = PortraitOf(row.name);
        if (art) {
            GaugePiece(draw, gauge, art->portrait, box, box_end, 1.0f, IM_COL32(255, 255, 255, row.down ? 150 : 255));
            if (!face)
                GaugePiece(draw, gauge, i == 0 ? art->rust : art->teal, inner, inner_end, 1.0f,
                           row.down ? IM_COL32(150, 60, 50, 255) : IM_COL32(255, 255, 255, 230));
        } else {
            draw->AddRectFilled(box, box_end, IM_COL32(18, 20, 26, 230));
        }
        if (face) {
            const float crop = 0.5f * (1.0f - (inner_end.x - inner.x) / (inner_end.y - inner.y));
            draw->AddImage(face->GetTexRef(), inner, inner_end, ImVec2(crop, 0.0f), ImVec2(1.0f - crop, 1.0f),
                           row.down ? IM_COL32(170, 70, 60, 200) : IM_COL32(255, 255, 255, 255));
        } else {
            draw->AddText(font, initial_px, ImVec2(initial_at.x + k, initial_at.y + k), IM_COL32(0, 0, 0, 200), initial);
            draw->AddText(font, initial_px, initial_at,
                          row.down ? IM_COL32(170, 70, 60, 255)
                                   : (parchment & ~IM_COL32_A_MASK) | (ImU32(255 * dim) << IM_COL32_A_SHIFT),
                          initial);
        }
        if (art) {
            const float fil_h = frame_w * float(art->filigree.h) / float(art->filigree.w);
            GaugePiece(draw, gauge, art->filigree, ImVec2(box.x, box.y - fil_h * 0.6f), ImVec2(box_end.x, box.y + fil_h * 0.4f),
                       1.0f, IM_COL32(255, 255, 255, row.down ? 150 : 255));
        } else {
            draw->AddRect(box, box_end, accent, 0.0f, 0, 1.5f * k);
        }
        const float text_x = box_end.x + 12.0f * k;
        const float bar_w = card_w - (text_x - left) - 24.0f * k;
        const std::string shown_name = row.dev ? row.name + " [Dev]" : row.name;
        draw->AddText(font, name_size, ImVec2(text_x + k, top + 7.0f * k), IM_COL32(0, 0, 0, int(200 * dim)), shown_name.c_str());
        draw->AddText(font, name_size, ImVec2(text_x, top + 6.0f * k),
                      (parchment & ~IM_COL32_A_MASK) | (ImU32(255 * dim) << IM_COL32_A_SHIFT), shown_name.c_str());
        for (const BbVoice::Talker& talker : talkers) {
            if (talker.name != row.name || (!talker.muted && !talker.talking)) continue;
            VoiceBadge(draw, box_end, talker.muted, k);
            break;
        }
        char where[32] = "";
        if (row.down) {
            std::snprintf(where, sizeof where, "%s", BbSettings::MenuText("dead", "мёртв"));
        } else if (row.distance >= 0.0f) {
            std::snprintf(where, sizeof where, "%.0f m", row.distance);
        }
        const float arrow_room = row.distance >= 0.0f && !row.down ? 16.0f * k : 0.0f;
        const float where_w = where[0] ? font->CalcTextSizeA(where_size, FLT_MAX, 0.0f, where).x : 0.0f;
        if (BbSettings::Get().coop_hud_ping) {
            for (const BbRoom::PeerStatus& peer : peers) {
                if (peer.name != row.name || peer.state == BbRoom::LinkState::Unknown) continue;
                char ping[32] = "...";
                if (peer.state != BbRoom::LinkState::Reconnecting) std::snprintf(ping, sizeof ping, "%d %s", peer.ping_ms, BbSettings::MenuText("ms", "мс"));
                const float ping_px = where_size * 0.8f;
                const ImVec2 size = font->CalcTextSizeA(ping_px, FLT_MAX, 0.0f, ping);
                const float name_end = text_x + font->CalcTextSizeA(name_size, FLT_MAX, 0.0f, shown_name.c_str()).x + 6.0f * k;
                const ImVec2 at(text_x + bar_w - arrow_room - where_w - (where[0] ? 10.0f * k : 0.0f) - size.x, top + 7.0f * k + (where_size - ping_px));
                if (at.x < name_end) break;
                draw->AddText(font, ping_px, ImVec2(at.x + k, at.y + k), IM_COL32(0, 0, 0, 200), ping);
                draw->AddText(font, ping_px, at, ConnectionColor(peer.state), ping);
                break;
            }
        }
        if (where[0]) {
            const ImVec2 size = font->CalcTextSizeA(where_size, FLT_MAX, 0.0f, where);
            const ImVec2 at(text_x + bar_w - arrow_room - size.x, top + 7.0f * k);
            draw->AddText(font, where_size, ImVec2(at.x + k, at.y + k), IM_COL32(0, 0, 0, 200), where);
            draw->AddText(font, where_size, at, row.down ? IM_COL32(200, 90, 70, 255) : IM_COL32(214, 206, 186, 235), where);
        }
        if (arrow_room > 0.0f) {
            const ImVec2 c(text_x + bar_w - 6.0f * k, top + 7.0f * k + where_size * 0.55f);
            const ImVec2 d(std::sin(row.bearing), -std::cos(row.bearing));
            const ImVec2 p(-d.y, d.x);
            const float tip = 6.0f * k, back = 4.0f * k, wide = 4.0f * k;
            draw->AddTriangleFilled(ImVec2(c.x + d.x * tip, c.y + d.y * tip),
                                    ImVec2(c.x - d.x * back + p.x * wide, c.y - d.y * back + p.y * wide),
                                    ImVec2(c.x - d.x * back - p.x * wide, c.y - d.y * back - p.y * wide),
                                    IM_COL32(220, 214, 200, 230));
        }
        const float hp_y = top + 31.0f * k, hp_h = 7.0f * k;
        if (art) TexturedBar(draw, gauge, art->health, ImVec2(text_x, hp_y), ImVec2(bar_w, hp_h), row.health, dim, k);
        else GameBar(draw, ImVec2(text_x, hp_y), ImVec2(bar_w, hp_h), row.health, IM_COL32(196, 46, 40, 255),
                     IM_COL32(112, 16, 16, 255), dim, k);
        const float sp_y = hp_y + hp_h + 4.0f * k, sp_h = 5.0f * k;
        if (art) TexturedBar(draw, gauge, art->stamina, ImVec2(text_x, sp_y), ImVec2(bar_w * 0.8f, sp_h), row.stamina, dim, k);
        else GameBar(draw, ImVec2(text_x, sp_y), ImVec2(bar_w * 0.8f, sp_h), row.stamina, IM_COL32(112, 172, 88, 255),
                     IM_COL32(46, 98, 42, 255), dim, k);
        top += card_h + gap;
    }
}

void TextPrompt() {
    std::string title, text;
    {
        std::scoped_lock lock{prompt_mutex};
        title = prompt_title;
        text = prompt_text;
    }
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
                                   viewport->WorkPos.y + viewport->WorkSize.y * 0.5f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowBgAlpha(0.9f);
    ImGui::Begin("##textprompt", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::TextUnformatted(title.c_str());
    ImGui::Separator();
    ImGui::Text("%s_", text.c_str());
    ImGui::Separator();
    ImGui::TextUnformatted(BbSettings::MenuText("Keyboard: type, Backspace = delete, Enter = OK, Esc = cancel"));
    ImGui::End();
}

}

void SetTextPrompt(bool active, const std::string& prompt, const std::string& text) {
    {
        std::scoped_lock lock{prompt_mutex};
        prompt_title = prompt;
        prompt_text = text;
    }
    prompt_active = active;
}

enum class OverlayKeys { Both, Insert, Off };
static OverlayKeys MenuKeys() {
    static const OverlayKeys keys = [] {
        const char* env = std::getenv("BB_OVERLAY_KEYS");
        const std::string value = env ? env : "";
        return value == "off" ? OverlayKeys::Off : value == "insert" ? OverlayKeys::Insert : OverlayKeys::Both;
    }();
    return keys;
}

void Init(const Vulkan::Instance& instance, vk::Format format, u32 image_count) {
    BbRoom::SetWarpRequests(BbSettings::Get().coop_warp_requests);
    BbRoom::SetOpenRoom(BbSettings::Get().coop_open_room);
    std::scoped_lock lock{imgui_mutex};
    if (initialized) {
        return;
    }
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigDebugHighlightIdConflicts = false;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    io.BackendPlatformName = "lantern";

    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 6.0f;
    style.FrameRounding = 4.0f;
    style.GrabRounding = 4.0f;
    style.Colors[ImGuiCol_WindowBg].w = 0.92f;

    ImFontConfig font_config;
    font_config.FontDataOwnedByAtlas = false;
    io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(bb_font_ttf),
                                   int(bb_font_ttf_end - bb_font_ttf), 18.0f, &font_config);
    ImFontConfig cjk_config;
    cjk_config.FontDataOwnedByAtlas = false;
    cjk_config.MergeMode = true;
    io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(bb_font_cjk),
                                   int(bb_font_cjk_end - bb_font_cjk), 18.0f, &cjk_config);
    ImFontConfig name_config;
    name_config.FontDataOwnedByAtlas = false;
    name_font = io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(bb_font_name),
                                               int(bb_font_name_end - bb_font_name), 18.0f, &name_config);
    ImFontConfig name_fallback = font_config;
    name_fallback.MergeMode = true;
    io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(bb_font_ttf), int(bb_font_ttf_end - bb_font_ttf), 18.0f,
                                   &name_fallback);
    io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(bb_font_cjk), int(bb_font_cjk_end - bb_font_cjk), 18.0f,
                                   &cjk_config);

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
    info.DescriptorPoolSize = 16;
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
    initialized = true;
    std::printf("Overlay: menu ready (%s)\n", MenuKeys() == OverlayKeys::Both     ? "Insert or L3+R3"
                                             : MenuKeys() == OverlayKeys::Insert ? "Insert; L3+R3 off"
                                                                                 : "shortcuts off");
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

void CoopInput(int move, int side, bool select, bool back);

bool HandleEvent(const SDL_Event& event) {
    std::scoped_lock lock{imgui_mutex};
    if (!initialized) {
        return false;
    }
    ImGuiIO& io = ImGui::GetIO();
    const bool is_open = menu_open;
    switch (event.type) {
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP: {
        const bool down = event.type == SDL_EVENT_KEY_DOWN;
        if (binding_talk_key && down && !event.key.repeat) {
            if (event.key.key != SDLK_ESCAPE) BbSettings::Get().voice_talk_key = int(event.key.scancode);
            binding_talk_key = false;
            BbSettings::Save();
            return true;
        }
        if (!is_open && down && !event.key.repeat && event.key.key == SDLK_F2) {
            coop_open = !coop_open;
            coop_armed.clear();
            return true;
        }
        if (!is_open && coop_open) {
            if (down) {
                const SDL_Keycode key = event.key.key;
                const int move = key == SDLK_UP || key == SDLK_W ? -1 : key == SDLK_DOWN || key == SDLK_S ? 1 : 0;
                const int side = key == SDLK_LEFT || key == SDLK_A ? -1 : key == SDLK_RIGHT || key == SDLK_D ? 1 : 0;
                const bool select = !event.key.repeat && (key == SDLK_RETURN || key == SDLK_SPACE || key == SDLK_E || key == SDLK_KP_ENTER);
                const bool back = !event.key.repeat && (key == SDLK_ESCAPE || key == SDLK_BACKSPACE || key == SDLK_Q);
                CoopInput(move, side, select, back);
            }
            return true;
        }
        if (down && !event.key.repeat &&
            ((event.key.key == SDLK_INSERT && (is_open || MenuKeys() != OverlayKeys::Off)) ||
             (is_open && event.key.key == SDLK_ESCAPE && !io.WantTextInput))) {
            SetOpen(event.key.key == SDLK_INSERT ? !is_open : false);
            return true;
        }
        if (!is_open) {
            return false;
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
        if (button == SDL_GAMEPAD_BUTTON_LEFT_STICK) {
            l3_down = down;
        } else if (button == SDL_GAMEPAD_BUTTON_RIGHT_STICK) {
            r3_down = down;
        }
        if (down && l3_down && r3_down && (is_open || MenuKeys() == OverlayKeys::Both)) {
            coop_open = false;
            SetOpen(!is_open);
            return true;
        }
        if (!is_open && down && l3_down && button == SDL_GAMEPAD_BUTTON_DPAD_UP) {
            coop_open = !coop_open;
            coop_armed.clear();
            return true;
        }
        if (!is_open && coop_open) {
            if (down) {
                const int move = button == SDL_GAMEPAD_BUTTON_DPAD_UP ? -1 : button == SDL_GAMEPAD_BUTTON_DPAD_DOWN ? 1 : 0;
                const int side = button == SDL_GAMEPAD_BUTTON_DPAD_LEFT ? -1 : button == SDL_GAMEPAD_BUTTON_DPAD_RIGHT ? 1 : 0;
                CoopInput(move, side, button == SDL_GAMEPAD_BUTTON_SOUTH, button == SDL_GAMEPAD_BUTTON_EAST);
            }
            return true;
        }
        if (!is_open) {
            return false;
        }
        if (const ImGuiKey key = KeyFromGamepad(button); key != ImGuiKey_None) {
            io.AddKeyEvent(key, down);
        }
        return true;
    }
    case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
        if (!is_open) {
            return false;
        }
        const u8 axis = event.gaxis.axis;
        if (axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER || axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) {
            const float value = std::clamp(float(event.gaxis.value) / 32767.0f, 0.0f, 1.0f);
            io.AddKeyAnalogEvent(axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER ? ImGuiKey_GamepadL2 : ImGuiKey_GamepadR2,
                                 value > 0.5f, value);
        }
        return true;
    }
    case SDL_EVENT_TEXT_INPUT: {
        if (!is_open) {
            return false;
        }
        io.AddInputCharactersUTF8(event.text.text);
        return true;
    }
    case SDL_EVENT_MOUSE_MOTION: {
        if (!is_open && !coop_open) {
            return false;
        }
        const float density = PixelDensity(event.motion.windowID);
        io.AddMousePosEvent(event.motion.x * density, event.motion.y * density);
        return true;
    }
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP: {
        if (binding_talk_key && event.type == SDL_EVENT_MOUSE_BUTTON_DOWN && event.button.button != SDL_BUTTON_LEFT) {
            BbSettings::Get().voice_talk_key = -int(event.button.button);
            binding_talk_key = false;
            BbSettings::Save();
            return true;
        }
        if (!is_open && !coop_open) {
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

bool Visible() {
    return initialized && (menu_open || prompt_active || BbSettings::Get().show_fps || CompileNoteActive() || RelayNoteActive() || SummonNoteActive() || AnnouncementActive() || ReconnectNoteActive() || !ShownPartyLines().empty() || coop_open ||
                           !BbEngine::OnlineFixes::PartyRows().empty() || BbEngine::OnlineFixes::Spectating() || BbFaceShot::JustTaken());
}

bool MenuOpen() {
    return menu_open;
}

bool CapturesInput() {
    return menu_open || prompt_active || coop_open;
}

enum class CoopKind { LampYes, LampNo, Summon, Member, Player, Visitor, BringAll, End, Join, GoTo, Leave, Voice, Mic, Speak, TalkKey, Volume, Listen, Close };

struct CoopItem {
    CoopKind kind;
    int slot = 0;
    std::string name;
};

std::vector<CoopItem> CoopItems() {
    namespace Online = BbEngine::OnlineFixes;
    std::vector<CoopItem> items;
    if (BbRoom::Active()) {
        if (BbRoom::IsHost()) {
            int asker = 0;
            std::string who;
            std::uint32_t lamp = 0;
            if (BbRoom::PendingLampAsk(asker, who, lamp)) {
                items.push_back({CoopKind::LampYes, asker, who});
                items.push_back({CoopKind::LampNo, asker, who});
            }
            items.push_back({CoopKind::Summon});
            const auto members = BbRoom::PartyMembers();
            for (const auto& [slot, name] : members) items.push_back({CoopKind::Member, slot, VisitorName(slot, name)});
            for (const BbRoom::PeerStatus& peer : BbRoom::Connections()) {
                const bool member = std::any_of(members.begin(), members.end(), [&](const auto& m) { return m.first == peer.slot; });
                if (member) continue;
                if (BbRoom::IsVisitorSlot(peer.slot)) items.push_back({CoopKind::Visitor, peer.slot, VisitorName(peer.slot, peer.name)});
                else if (BbSettings::Get().coop_status) items.push_back({CoopKind::Player, peer.slot, peer.name});
            }
            if (!members.empty()) {
                items.push_back({CoopKind::BringAll});
                items.push_back({CoopKind::End});
            }
        } else {
            if (BbSettings::Get().coop_status) {
                for (const BbRoom::PeerStatus& peer : BbRoom::Connections()) items.push_back({CoopKind::Player, peer.slot, VisitorName(peer.slot, peer.name)});
            }
            if (Online::PartyRole() == 6) {
                if (BbSettings::Get().coop_warp_requests) items.push_back({CoopKind::GoTo});
                items.push_back({CoopKind::Leave});
            } else {
                items.push_back({CoopKind::Join});
            }
        }
    }
    if (BbRoom::Active()) {
        items.push_back({CoopKind::Voice});
        if (BbSettings::Get().voice_enabled) {
            items.push_back({CoopKind::Mic});
            items.push_back({CoopKind::Speak});
            if (BbSettings::Get().voice_push_to_talk) items.push_back({CoopKind::TalkKey});
            items.push_back({CoopKind::Volume});
            for (const auto& t : BbVoice::Talkers()) items.push_back({CoopKind::Listen, t.slot, t.name});
        }
    }
    items.push_back({CoopKind::Close});
    return items;
}

bool CoopArmed(const std::string& what) {
    const auto now = std::chrono::steady_clock::now();
    if (coop_armed == what && now - coop_armed_at < std::chrono::seconds(4)) {
        coop_armed.clear();
        return true;
    }
    coop_armed = what;
    coop_armed_at = now;
    return false;
}

void CoopActivate(const CoopItem& item) {
    namespace Online = BbEngine::OnlineFixes;
    switch (item.kind) {
    case CoopKind::LampYes:
        if (CoopArmed("lamp")) Online::AnswerLampAsk(true);
        break;
    case CoopKind::LampNo:
        Online::AnswerLampAsk(false);
        break;
    case CoopKind::Summon:
    case CoopKind::Join:
        if (Online::Summoning()) Online::StopSummon();
        else Online::StartSummon();
        break;
    case CoopKind::Member:
        if (coop_column == 0) BbRoom::SendBring(item.slot);
        else if (CoopArmed("kick:" + std::to_string(item.slot))) BbRoom::Kick(item.slot);
        break;
    case CoopKind::Visitor:
        if (CoopArmed("kick:" + std::to_string(item.slot))) BbRoom::Kick(item.slot);
        break;
    case CoopKind::BringAll:
        BbRoom::SendBring(0);
        break;
    case CoopKind::End:
        if (CoopArmed("end")) {
            Online::StopSummon();
            for (const auto& [slot, name] : BbRoom::PartyMembers()) BbRoom::Kick(slot);
        }
        break;
    case CoopKind::GoTo:
        BbRoom::SendWarpAsk();
        break;
    case CoopKind::Player:
        break;
    case CoopKind::Leave:
        if (CoopArmed("leave")) Online::LeaveHost();
        break;
    case CoopKind::Voice:
        BbSettings::Get().voice_enabled = !BbSettings::Get().voice_enabled;
        BbSettings::Save();
        break;
    case CoopKind::Mic:
        BbSettings::Get().voice_mic_muted = !BbSettings::Get().voice_mic_muted;
        BbSettings::Save();
        break;
    case CoopKind::Speak:
        BbSettings::Get().voice_push_to_talk = !BbSettings::Get().voice_push_to_talk;
        BbSettings::Save();
        break;
    case CoopKind::TalkKey:
        binding_talk_key = true;
        break;
    case CoopKind::Volume:
        break;
    case CoopKind::Listen: {
        bool muted = false;
        for (const auto& t : BbVoice::Talkers())
            if (t.name == item.name) muted = t.muted;
        BbVoice::SetMuted(item.name, !muted);
        break;
    }
    case CoopKind::Close:
        coop_open = false;
        break;
    }
}

void CoopInput(int move, int side, bool select, bool back) {
    const auto items = CoopItems();
    const int count = int(items.size());
    if (back) {
        coop_open = false;
        return;
    }
    if (move) {
        coop_row = (coop_row + move + count) % count;
        while (items[coop_row].kind == CoopKind::Player) coop_row = (coop_row + move + count) % count;
        coop_armed.clear();
    }
    coop_row = std::clamp(coop_row, 0, count - 1);
    if (side && items[coop_row].kind == CoopKind::Volume) {
        auto& volume = BbSettings::Get().voice_volume;
        volume = std::clamp(volume + (side < 0 ? -0.25f : 0.25f), 0.0f, 5.0f);
        BbSettings::Save();
    }
    if (side && items[coop_row].kind == CoopKind::Member) {
        coop_column = side < 0 ? 0 : 1;
        coop_armed.clear();
    }
    if (select) CoopActivate(items[coop_row]);
}

bool CoopArmedNow(const std::string& what) {
    return coop_armed == what && std::chrono::steady_clock::now() - coop_armed_at < std::chrono::seconds(4);
}

std::string LampAskPlace() {
    int slot = 0;
    std::string who;
    std::uint32_t lamp = 0;
    if (!BbRoom::PendingLampAsk(slot, who, lamp)) return {};
    return " (" + BbEngine::OnlineFixes::LampPlace(lamp) + ")";
}

std::string CoopLabel(const CoopItem& item, bool chosen) {
    namespace Online = BbEngine::OnlineFixes;
    switch (item.kind) {
    case CoopKind::LampYes:
        return chosen && CoopArmedNow("lamp") ? std::string(BbSettings::MenuText("Press again: the whole party travels", "Нажмите ещё раз: весь отряд отправится"))
                                              : item.name + BbSettings::MenuText(" asks to travel: accept", " просит отправиться: принять") + LampAskPlace();
    case CoopKind::LampNo:
        return BbSettings::MenuText("Stay here", "Остаться здесь");
    case CoopKind::Summon:
        return Online::Summoning() ? BbSettings::MenuText("Stop summoning", "Остановить призыв")
                                   : BbSettings::MenuText("Start summon", "Начать призыв");
    case CoopKind::Join:
        return Online::Summoning() ? BbSettings::MenuText("Take back the sign", "Убрать знак")
                                   : BbSettings::MenuText("Send sign", "Отправить знак");
    case CoopKind::GoTo:
        return BbRoom::WarpWaiting() ? BbSettings::MenuText("Waiting for the host...", "Ждём хозяина...")
                                     : BbSettings::MenuText("Ask the host to bring you", "Попросить хозяина перенести вас");
    case CoopKind::Player:
        return item.name;
    case CoopKind::Visitor:
        return std::string(chosen && CoopArmedNow("kick:" + std::to_string(item.slot)) ? BbSettings::MenuText("Press again to kick ", "Нажмите ещё раз, чтобы выгнать ")
                                                                                       : BbSettings::MenuText("Kick ", "Выгнать ")) + item.name;
    case CoopKind::Member:
        if (chosen && CoopArmedNow("kick:" + std::to_string(item.slot)))
            return std::string(BbSettings::MenuText("Press again to send home ", "Нажмите ещё раз, чтобы выгнать ")) + item.name;
        return item.name;
    case CoopKind::BringAll:
        return BbSettings::MenuText("Bring all hunters", "Призвать всех к себе");
    case CoopKind::End:
        return chosen && CoopArmedNow("end") ? BbSettings::MenuText("Press again to end the session", "Нажмите ещё раз, чтобы завершить")
                                             : BbSettings::MenuText("End session", "Завершить сеанс");
    case CoopKind::Leave:
        return chosen && CoopArmedNow("leave") ? BbSettings::MenuText("Press again to go back", "Нажмите ещё раз, чтобы вернуться")
                                               : BbSettings::MenuText("Return to your own world", "Вернуться в свой мир");
    case CoopKind::Voice:
        return BbSettings::Get().voice_enabled ? BbSettings::MenuText("Voice chat: on", "Голосовой чат: вкл.")
                                               : BbSettings::MenuText("Voice chat: off", "Голосовой чат: выкл.");
    case CoopKind::Mic:
        if (!BbSettings::Get().voice_mic_muted && BbVoice::MicSilent())
            return BbSettings::MenuText("Microphone: no sound, check Windows", "Микрофон: нет звука, проверьте Windows");
        return BbSettings::Get().voice_mic_muted ? BbSettings::MenuText("Microphone: muted", "Микрофон: выкл.")
                                                 : BbSettings::MenuText("Microphone: on", "Микрофон: вкл.");
    case CoopKind::Speak:
        return BbSettings::Get().voice_push_to_talk ? BbSettings::MenuText("Speak: hold the talk key", "Говорить: по кнопке")
                                                    : BbSettings::MenuText("Speak: when you talk", "Говорить: голосом");
    case CoopKind::TalkKey:
        return binding_talk_key ? std::string(BbSettings::MenuText("Press a key or mouse button...", "Нажмите кнопку..."))
                                : std::string(BbSettings::MenuText("Talk key: ", "Кнопка разговора: ")) + BbVoice::KeyName(BbSettings::Get().voice_talk_key);
    case CoopKind::Volume: {
        char text[64];
        std::snprintf(text, sizeof text, "%s %.0f%%   < >", BbSettings::MenuText("Voice volume:", "Громкость голоса:"),
                      BbSettings::Get().voice_volume * 100.0f);
        return text;
    }
    case CoopKind::Listen: {
        bool muted = false, talking = false;
        for (const auto& t : BbVoice::Talkers())
            if (t.slot == item.slot) muted = t.muted, talking = t.talking;
        return item.name + (muted ? BbSettings::MenuText(": muted", ": заглушён") : talking ? BbSettings::MenuText(": speaking", ": говорит")
                                                                                           : BbSettings::MenuText(": heard", ": слышно"));
    }
    case CoopKind::Close:
        return BbSettings::MenuText("Close", "Закрыть");
    }
    return {};
}

void CoopMenu() {
    ImTextureData* gauge = GaugeTexture();
    const BbHudGauge::Art* art = gauge ? BbHudGauge::Ready() : nullptr;
    const auto items = CoopItems();
    coop_row = std::clamp(coop_row, 0, int(items.size()) - 1);
    while (items[coop_row].kind == CoopKind::Player) coop_row = (coop_row + 1) % int(items.size());
    const auto peers = BbRoom::Connections();
    const float k = base_scale;
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImDrawList* draw = ImGui::GetForegroundDrawList();
    ImFont* font = name_font ? name_font : ImGui::GetFont();
    const float title_px = ImGui::GetFontSize() * 1.9f, item_px = ImGui::GetFontSize() * 1.45f, hint_px = ImGui::GetFontSize() * 1.05f;
    const float row_h = item_px + 18.0f * k;
    const float width = 760.0f * k;
    const float height = title_px + 70.0f * k + row_h * float(items.size()) + hint_px + 40.0f * k;
    const float cx = viewport->WorkPos.x + viewport->WorkSize.x * 0.5f;
    const float top = viewport->WorkPos.y + (viewport->WorkSize.y - height) * 0.5f;
    const ImU32 parchment = IM_COL32(226, 216, 192, 255), faded = IM_COL32(150, 142, 126, 230), red = IM_COL32(220, 120, 100, 255);
    draw->AddRectFilled(viewport->WorkPos, ImVec2(viewport->WorkPos.x + viewport->WorkSize.x, viewport->WorkPos.y + viewport->WorkSize.y),
                        IM_COL32(0, 0, 0, 110));
    const ImVec2 a(cx - width * 0.5f, top), b(cx + width * 0.5f, top + height);
    draw->AddRectFilledMultiColor(a, ImVec2(cx, b.y), IM_COL32(0, 0, 0, 40), IM_COL32(4, 4, 6, 225), IM_COL32(4, 4, 6, 225),
                                  IM_COL32(0, 0, 0, 40));
    draw->AddRectFilledMultiColor(ImVec2(cx, a.y), b, IM_COL32(4, 4, 6, 225), IM_COL32(0, 0, 0, 40), IM_COL32(0, 0, 0, 40),
                                  IM_COL32(4, 4, 6, 225));
    const auto line = [&](float y, float half, bool flip) {
        if (art) {
            const float h = 18.0f * k;
            Flourish(draw, gauge, art->header, ImVec2(cx - half, y - h * 0.5f), ImVec2(cx + half, y + h * 0.5f), IM_COL32(255, 255, 255, 235),
                     flip);
        } else {
            draw->AddLine(ImVec2(cx - half, y), ImVec2(cx + half, y), IM_COL32(200, 190, 170, 160), k);
        }
    };
    const auto text = [&](float px, ImVec2 at, ImU32 color, const char* str) {
        draw->AddText(font, px, ImVec2(at.x + k, at.y + k), IM_COL32(0, 0, 0, 210), str);
        draw->AddText(font, px, at, color, str);
    };
    const char* title = BbSettings::MenuText("Seamless Co-op", "Бесшовная совместная игра");
    const ImVec2 title_size = font->CalcTextSizeA(title_px, FLT_MAX, 0.0f, title);
    text(title_px, ImVec2(cx - title_size.x * 0.5f, top + 18.0f * k), parchment, title);
    float y = top + title_px + 34.0f * k;
    line(y, 85.0f * k, false);
    y += 22.0f * k;
    const ImGuiIO& io = ImGui::GetIO();
    const ImVec2 mouse = io.MousePos;
    const bool moved = io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f;
    for (int i = 0; i < int(items.size()); ++i) {
        const CoopItem& item = items[i];
        const ImVec2 ra(a.x + 40.0f * k, y), rb(b.x - 40.0f * k, y + row_h);
        const bool over = mouse.x >= ra.x && mouse.x < rb.x && mouse.y >= ra.y && mouse.y < rb.y;
        if (over && moved && item.kind != CoopKind::Player) coop_row = i;
        const bool chosen = i == coop_row;
        if (chosen) {
            const float mid = (ra.x + rb.x) * 0.5f;
            draw->AddRectFilledMultiColor(ra, ImVec2(mid, rb.y), IM_COL32(120, 100, 60, 0), IM_COL32(120, 100, 60, 70),
                                          IM_COL32(120, 100, 60, 70), IM_COL32(120, 100, 60, 0));
            draw->AddRectFilledMultiColor(ImVec2(mid, ra.y), rb, IM_COL32(120, 100, 60, 70), IM_COL32(120, 100, 60, 0),
                                          IM_COL32(120, 100, 60, 0), IM_COL32(120, 100, 60, 70));
        }
        const std::string label = CoopLabel(item, chosen);
        const float ty = y + (row_h - item_px) * 0.5f;
        if (item.kind == CoopKind::Member) {
            text(item_px, ImVec2(ra.x + 20.0f * k, ty), chosen ? parchment : faded, label.c_str());
            const float label_end = ra.x + 20.0f * k + font->CalcTextSizeA(item_px, FLT_MAX, 0.0f, label.c_str()).x + 16.0f * k;
            const BbRoom::PeerStatus* peer = FindPeer(peers, item.slot);
            const char* bring = BbSettings::MenuText("Bring", "Призвать");
            const char* kick = BbSettings::MenuText("Kick", "Выгнать");
            const float kick_w = font->CalcTextSizeA(item_px, FLT_MAX, 0.0f, kick).x;
            const float bring_w = font->CalcTextSizeA(item_px, FLT_MAX, 0.0f, bring).x;
            const float kick_x = rb.x - 20.0f * k - kick_w, bring_x = kick_x - 40.0f * k - bring_w;
            if (peer && (peer->asked || BbSettings::Get().coop_status)) {
                std::string status = BbSettings::Get().coop_status ? ConnectionText(*peer) : std::string();
                if (peer->asked) status = BbSettings::MenuText("asks to come", "просит перенести");
                const ImU32 tone = peer->asked ? parchment : ConnectionColor(peer->state);
                if (label_end + font->CalcTextSizeA(hint_px, FLT_MAX, 0.0f, status.c_str()).x < bring_x)
                    text(hint_px, ImVec2(label_end, ty + (item_px - hint_px) * 0.5f), tone, status.c_str());
            }
            const bool on_bring = chosen && coop_column == 0, on_kick = chosen && coop_column == 1;
            text(item_px, ImVec2(bring_x, ty), on_bring ? parchment : faded, bring);
            text(item_px, ImVec2(kick_x, ty), on_kick ? red : faded, kick);
            const float under = ty + item_px + 2.0f * k;
            if (on_bring) draw->AddLine(ImVec2(bring_x, under), ImVec2(bring_x + bring_w, under), parchment, k);
            if (on_kick) draw->AddLine(ImVec2(kick_x, under), ImVec2(kick_x + kick_w, under), red, k);
            if (over && ImGui::IsMouseClicked(0)) {
                coop_row = i;
                if (mouse.x >= bring_x && mouse.x < bring_x + bring_w) {
                    coop_column = 0;
                    CoopActivate(item);
                } else if (mouse.x >= kick_x && mouse.x < kick_x + kick_w) {
                    coop_column = 1;
                    CoopActivate(item);
                }
            }
        } else if (item.kind == CoopKind::Player) {
            text(item_px, ImVec2(ra.x + 20.0f * k, ty), faded, label.c_str());
            if (const BbRoom::PeerStatus* peer = FindPeer(peers, item.slot)) {
                const std::string status = ConnectionText(*peer);
                const float status_w = font->CalcTextSizeA(hint_px, FLT_MAX, 0.0f, status.c_str()).x;
                text(hint_px, ImVec2(rb.x - 20.0f * k - status_w, ty + (item_px - hint_px) * 0.5f), ConnectionColor(peer->state), status.c_str());
            }
        } else {
            const ImVec2 size = font->CalcTextSizeA(item_px, FLT_MAX, 0.0f, label.c_str());
            text(item_px, ImVec2(cx - size.x * 0.5f, ty), chosen ? parchment : faded, label.c_str());
            if (over && ImGui::IsMouseClicked(0)) {
                coop_row = i;
                CoopActivate(item);
            }
        }
        y += row_h;
    }
    y += 10.0f * k;
    const char* hint = BbSettings::MenuText("Enter / Cross: select      Arrows / D-pad: move      Esc / Circle: back",
                                            "Enter / Крест: выбрать      Стрелки: двигаться      Esc / Круг: назад");
    const ImVec2 hint_size = font->CalcTextSizeA(hint_px, FLT_MAX, 0.0f, hint);
    text(hint_px, ImVec2(cx - hint_size.x * 0.5f, y + 6.0f * k), faded, hint);
    line(y + hint_px + 22.0f * k, width * 0.5f - 30.0f * k, true);
}

void Render(vk::CommandBuffer cmdbuf, vk::ImageView view, vk::Extent2D extent) {
    const auto now = std::chrono::steady_clock::now();
    const float ms = std::chrono::duration<float, std::milli>(now - last_present).count();
    last_present = now;
    if (ms > 0.0f && ms < 1000.0f) {
        frame_ms_avg = frame_ms_avg == 0.0f ? ms : frame_ms_avg * 0.95f + ms * 0.05f;
    }
    if (!Visible()) {
        return;
    }
    std::scoped_lock lock{imgui_mutex};
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(float(extent.width), float(extent.height));
    io.DeltaTime = ms > 0.0f && ms < 1000.0f ? ms / 1000.0f : 1.0f / 60.0f;
    const float scale = std::max(float(extent.height) / 1080.0f, 0.75f);
    if (std::abs(scale - base_scale) > 0.01f) {
        ImGuiStyle& style = ImGui::GetStyle();
        style.ScaleAllSizes(scale / base_scale);
        style.FontScaleMain = scale;
        base_scale = scale;
    }

    ImGui_ImplVulkan_NewFrame();
    ImGui::NewFrame();
    if (menu_open) {
        Menu();
    }
    fps_box_min = fps_box_max = ImVec2{};
    if (BbSettings::Get().show_fps && !menu_open) {
        FpsCounter();
    }
    if (!menu_open && CompileNoteActive()) {
        CompileNote();
    }
    if (!menu_open && RelayNoteActive()) {
        RelayNote();
    }
    if (!menu_open) {
        SummonNote();
        Announcement();
        ReconnectNote();
        PartyLines();
    }
    if (prompt_active && !menu_open) {
        TextPrompt();
    }
    if (!menu_open) {
        const auto party = BbEngine::OnlineFixes::PartyRows();
        if (!party.empty()) PartyHud(party);
        SpectatorBanner();
        PortraitToast();
    }
    if (coop_open && !menu_open) {
        CoopMenu();
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
        std::scoped_lock submit_lock{Vulkan::Scheduler::submit_mutex};
        ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmdbuf);
    }
    cmdbuf.endRendering();
}

}
