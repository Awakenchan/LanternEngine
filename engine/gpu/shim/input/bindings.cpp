// SPDX-License-Identifier: GPL-3.0-or-later
#include "bindings.h"
#include "input.h"
#include "menu_pointer.h"
#include "pad.h"
#include <SDL3/SDL.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace {
using namespace PadBits;

constexpr BindingInfo kActions[kBindCount] = {
    {"move_forward", "Move Forward", "Left stick up.", 0, "W", 0},
    {"move_back", "Move Back", "Left stick down.", 0, "S", 0},
    {"move_left", "Move Left", "Left stick left.", 0, "A", 0},
    {"move_right", "Move Right", "Left stick right.", 0, "D", 0},
    {"walk", "Walk", "Hold with the movement keys to walk.", 0, "Left Alt", 0},
    {"roll", "Dodge / Sprint", "Circle. Tap to dodge, hold to sprint.", kCircle, "Space", 0},
    {"lock_on", "Lock On", "R3. Lock on to a target, or reset the camera.", kR3, "Q", 0},

    {"attack", "Attack", "R1. With the strong attack modifier held, R2.", kR1, nullptr, 1},
    {"strong_attack", "Strong Attack", "R2. Hold to charge.", kR2, "V", 0},
    {"strong_modifier", "Strong Attack Modifier", "Held with Attack, attacks strong (R2).", 0, "Left Shift", 0},
    {"transform", "Transform", "L1. Transform the trick weapon.", kL1, "C", 2},
    {"firearm", "Firearm / Parry", "L2. Shoot the left-hand weapon.", kL2, "X", 3},
    {"blood_vial", "Blood Vial", "Triangle. Restore HP with a Blood Vial.", kTriangle, "R", 0},
    {"use_item", "Use Item", "Square. Use the selected quick item.", kSquare, "F", 0},

    {"interact", "Interact", "Cross. Talk, examine, pick up, open.", kCross, "E", 0},
    {"switch_item", "Switch Item", "D-pad down. Cycle the quick items.", kDown, "Down", 0},
    {"switch_right", "Switch Right Weapon", "D-pad right.", kRight, "Right", 0},
    {"switch_left", "Switch Left Weapon", "D-pad left.", kLeft, "Left", 0},
    {"dpad_up", "D-pad Up", "D-pad up.", kUp, "Up", 0},
    {"gestures", "Gestures", "The touchpad's left side.", kTouchPad, "G", 0},
    {"personal_effects", "Personal Effects", "The touchpad's right side.", kTouchPad, "T", 0},

    {"look_up", "Look Up", "Right stick up, for the camera without the mouse.", 0, "I", 0},
    {"look_down", "Look Down", "Right stick down.", 0, "K", 0},
    {"look_left", "Look Left", "Right stick left.", 0, "J", 0},
    {"look_right", "Look Right", "Right stick right.", 0, "L", 0},
    {"menu", "Menu", "Options. Open the menu.", kOptions, "Escape", 0},
    {"confirm", "Menu Confirm", "OK, in menus.", 0, "Return", 0},
    {"back", "Menu Back", "Return, in menus.", 0, "Backspace", 0},

    {"l3", "Jump / Left Stick Press", "L3. Jumps when Jump on its own key is on.", kL3, "Left Ctrl", 0},
    {"dodge", "Dodge", "A tap of Circle, also while the sprint key is held.", 0, nullptr, 0},
    {"backstep", "Backstep", "Steps back at once, also while moving or sprinting.", 0, nullptr, 0},
    {"sprint", "Sprint", "Sprints at once while held, and never dodges.", 0, nullptr, 0},
    {"debug_menu", "Debug Menu", "Opens and closes the developers' debug menu (the Debug Menu plugin).", 0, "`", 0},
};

struct OldName {
    const char* name;
    int action;
};
constexpr OldName kOldNames[] = {
    {"dpad_down", kBindSwitchItem}, {"dpad_left", kBindSwitchLeft}, {"dpad_right", kBindSwitchRight}};

enum Modifier : std::uint8_t { kCtrl = 1, kShift = 2, kAlt = 4 };

constexpr int kMouseInputs = 7;

struct Input {
    std::uint16_t code = 0;
    bool mouse = false;
    std::uint8_t modifiers = 0;
    bool None() const { return code == 0; }
    bool operator==(const Input&) const = default;
};

constexpr int kKeySlot = 0, kMouseSlot = 1, kFirstExtra = 2, kSlots = 5;
struct Binding {
    Input slot[kSlots];
};

std::mutex g_lock;
Binding g_bindings[kBindCount];
std::atomic<std::uint64_t> g_serial{1};
std::atomic<bool> g_dodge_down{false}, g_backstep_down{false}, g_sprint_down{false};

void Changed() { g_serial.fetch_add(1, std::memory_order_relaxed); }

const char* KeyName(int scancode) {
    const char* name = SDL_GetScancodeName(static_cast<SDL_Scancode>(scancode));
    return name && *name ? name : nullptr;
}

std::string ModifierText(std::uint8_t modifiers) {
    std::string text;
    if (modifiers & kCtrl) text += "Ctrl+";
    if (modifiers & kShift) text += "Shift+";
    if (modifiers & kAlt) text += "Alt+";
    return text;
}

std::string IniText(const Input& in) {
    if (in.mouse) {
        const std::string name = in.code == 6 ? "WheelUp" : in.code == 7 ? "WheelDown" : "Mouse" + std::to_string(in.code);
        return ModifierText(in.modifiers) + name;
    }
    const char* name = KeyName(in.code);
    return name ? ModifierText(in.modifiers) + name : std::string();
}

std::string ScreenText(const Input& in) {
    static constexpr const char* kMouseNames[kMouseInputs + 1] = {
        nullptr, "Left Button", "Middle Button", "Right Button", "Button 4", "Button 5", "Wheel Up", "Wheel Down"};
    const char* name = nullptr;
    if (in.mouse) {
        name = in.code <= kMouseInputs ? kMouseNames[in.code] : nullptr;
    } else {
        name = KeyName(in.code);
        if (name && std::strcmp(name, "`") == 0) name = "` (backtick)";
    }
    return name ? ModifierText(in.modifiers) + name : std::string();
}

std::string Trimmed(const std::string& text) {
    const auto keep = [](char c) { return c != ' ' && c != '\t' && c != '"'; };
    std::size_t first = 0, last = text.size();
    while (first < last && !keep(text[first])) ++first;
    while (last > first && !keep(text[last - 1])) --last;
    return text.substr(first, last - first);
}

Input ParseInput(std::string text) {
    Input in;
    static constexpr struct {
        const char* prefix;
        std::uint8_t modifier;
    } kPrefixes[] = {{"Ctrl+", kCtrl}, {"Control+", kCtrl}, {"Shift+", kShift}, {"Alt+", kAlt}};
    for (bool found = true; found;) {
        found = false;
        for (const auto& p : kPrefixes) {
            const std::size_t n = std::strlen(p.prefix);
            if (text.size() > n && SDL_strncasecmp(text.c_str(), p.prefix, n) == 0) {
                in.modifiers |= p.modifier;
                text = Trimmed(text.substr(n));
                found = true;
            }
        }
    }
    if (text.size() == 6 && text.compare(0, 5, "Mouse") == 0 && text[5] >= '1' && text[5] <= '5') {
        in.mouse = true;
        in.code = std::uint16_t(text[5] - '0');
    } else if (text == "WheelUp" || text == "WheelDown") {
        in.mouse = true;
        in.code = text == "WheelUp" ? 6 : 7;
    } else {
        const SDL_Scancode sc = SDL_GetScancodeFromName(text.c_str());
        in.code = sc > SDL_SCANCODE_UNKNOWN ? std::uint16_t(sc) : 0;
    }
    return in;
}

int ActionNamed(const char* name) {
    for (int a = 0; a < kBindCount; ++a) {
        if (std::strcmp(name, kActions[a].key) == 0) return a;
    }
    for (const OldName& old : kOldNames) {
        if (std::strcmp(name, old.name) == 0) return old.action;
    }
    return -1;
}

bool Valid(int action) { return action >= 0 && action < kBindCount; }

int SlotFor(const Binding& b, const Input& in) {
    if (!in.mouse && b.slot[kKeySlot].None()) return kKeySlot;
    if (in.mouse && !in.modifiers && b.slot[kMouseSlot].None()) return kMouseSlot;
    for (int s = kFirstExtra; s < kSlots; ++s) {
        if (b.slot[s].None()) return s;
    }
    return -1;
}

std::atomic<std::uint8_t> g_keyboard_modifiers{0};
std::atomic<bool> g_walk_on_mouse{false};

constexpr float kWalkPush = 0.6f;

constexpr int kOpposites[][2] = {
    {kBindMoveF, kBindMoveB}, {kBindMoveL, kBindMoveR}, {kBindLookU, kBindLookD}, {kBindLookL, kBindLookR}};

template <typename IsDown>
void HeldOn(bool mouse, IsDown down, std::uint8_t modifiers, bool held[kBindCount]) {
    struct Candidate {
        int action;
        Input input;
    };
    Candidate found[kBindCount * kSlots];
    int count = 0;
    {
        std::scoped_lock lock(g_lock);
        for (int a = 0; a < kBindCount; ++a) {
            for (const Input& in : g_bindings[a].slot) {
                if (!in.None() && in.mouse == mouse && down(in.code) && (in.modifiers & ~modifiers) == 0)
                    found[count++] = {a, in};
            }
        }
    }
    for (int a = 0; a < kBindCount; ++a) held[a] = false;
    for (int i = 0; i < count; ++i) {

        bool outranked = false;
        for (int j = 0; j < count && !outranked; ++j) {
            const std::uint8_t mine = found[i].input.modifiers, theirs = found[j].input.modifiers;
            outranked = found[j].input.code == found[i].input.code && theirs != mine && (theirs & mine) == mine;
        }
        if (!outranked) held[found[i].action] = true;
    }
}

std::uint8_t ModifiersDown(const bool* keys, int count) {
    const auto down = [&](SDL_Scancode sc) { return sc < count && keys[sc]; };
    std::uint8_t m = 0;
    if (down(SDL_SCANCODE_LCTRL) || down(SDL_SCANCODE_RCTRL)) m |= kCtrl;
    if (down(SDL_SCANCODE_LSHIFT) || down(SDL_SCANCODE_RSHIFT)) m |= kShift;
    if (down(SDL_SCANCODE_LALT) || down(SDL_SCANCODE_RALT)) m |= kAlt;
    return m;
}

std::uint8_t ModifiersOf(SDL_Keymod mods) {
    std::uint8_t m = 0;
    if (mods & SDL_KMOD_CTRL) m |= kCtrl;
    if (mods & SDL_KMOD_SHIFT) m |= kShift;
    if (mods & SDL_KMOD_ALT) m |= kAlt;
    return m;
}

bool IsModifierKey(int sc) {
    return sc == SDL_SCANCODE_LCTRL || sc == SDL_SCANCODE_RCTRL || sc == SDL_SCANCODE_LSHIFT ||
           sc == SDL_SCANCODE_RSHIFT || sc == SDL_SCANCODE_LALT || sc == SDL_SCANCODE_RALT;
}

using Clock = std::chrono::steady_clock;
std::int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
}

std::atomic<int> g_capturing{-1};
std::atomic<std::int64_t> g_capture_began{0};
std::atomic<int> g_capture_modifier{0};
std::atomic<int> g_capture_done{-1};

std::atomic<bool> g_after_capture{false};
std::atomic<std::int64_t> g_after_capture_since{0};
constexpr std::int64_t kAfterCaptureMaxMs = 1500;

constexpr std::int64_t kCaptureClickMs = 300;
std::atomic<std::int64_t> g_clear_pressed{0};
constexpr std::int64_t kClearKeyMs = 250;

void TakeFromOthers(int action, const Input& in) {
    for (int b = 0; b < kBindCount; ++b) {
        if (b == action) continue;
        for (Input& slot : g_bindings[b].slot) {
            if (slot == in) {
                slot = {};
                host_log("keys: %s gives up %s", kActions[b].key, IniText(in).c_str());
            }
        }
    }
}

void EndCapture(int action, const char* outcome) {
    Changed();
    g_capture_modifier.store(0, std::memory_order_relaxed);
    g_capturing.store(-1, std::memory_order_relaxed);
    g_after_capture_since.store(NowMs(), std::memory_order_relaxed);
    g_after_capture.store(true, std::memory_order_relaxed);
    g_capture_done.store(action, std::memory_order_relaxed);
    char now[160];
    host_binding_describe(action, now, sizeof now);
    host_log("keys: %s %s, now %s", kActions[action].key, outcome, now);
}

void BindKey(int action, int scancode, std::uint8_t modifiers) {
    const Input in{std::uint16_t(scancode), false, modifiers};
    {
        std::scoped_lock lock(g_lock);
        TakeFromOthers(action, in);
        g_bindings[action].slot[kKeySlot] = in;
    }
    EndCapture(action, "rebound");
}
}

const BindingInfo& host_binding_info(int action) { return kActions[Valid(action) ? action : 0]; }

std::uint64_t host_bindings_serial() { return g_serial.load(std::memory_order_relaxed); }

void host_bindings_load_defaults() {
    {
        std::scoped_lock lock(g_lock);
        for (int a = 0; a < kBindCount; ++a) {
            Binding& b = g_bindings[a];
            b = {};
            if (const char* key = kActions[a].default_key)
                b.slot[kKeySlot].code = std::uint16_t(SDL_GetScancodeFromName(key));
            if (const int mouse = kActions[a].default_mouse) b.slot[kMouseSlot] = {std::uint16_t(mouse), true, 0};
        }
    }
    Changed();
}

bool host_bindings_parse(const char* name, const char* value) {
    const int action = ActionNamed(name);
    if (action < 0) return false;
    const std::string text = value;
    Binding b;
    for (std::size_t at = 0; at <= text.size();) {
        std::size_t comma = text.find(',', at);
        if (comma == std::string::npos) comma = text.size();
        const std::string token = Trimmed(text.substr(at, comma - at));
        at = comma + 1;
        if (token.empty() || token == "none") continue;
        const Input in = ParseInput(token);
        if (in.None()) {
            host_log("keys: %s=%s: \"%s\" is not a key or mouse input", kActions[action].key, value, token.c_str());
            continue;
        }
        const int slot = SlotFor(b, in);
        if (slot < 0) {
            host_log("keys: %s=%s: more than %d inputs, \"%s\" left out", kActions[action].key, value, kSlots,
                         token.c_str());
            continue;
        }
        b.slot[slot] = in;
    }
    {
        std::scoped_lock lock(g_lock);
        g_bindings[action] = b;
    }
    Changed();
    return true;
}

std::string host_binding_ini_value(int action) {
    if (!Valid(action)) return "none";
    Binding b;
    {
        std::scoped_lock lock(g_lock);
        b = g_bindings[action];
    }
    std::string value;
    for (const Input& in : b.slot) {
        const std::string text = in.None() ? std::string() : IniText(in);
        if (text.empty()) continue;
        if (!value.empty()) value += ", ";
        value += text;
    }
    return value.empty() ? "none" : value;
}

int host_binding_key(int action) {
    if (!Valid(action)) return 0;
    std::scoped_lock lock(g_lock);
    return g_bindings[action].slot[kKeySlot].code;
}

int host_binding_mouse(int action) {
    if (!Valid(action)) return 0;
    std::scoped_lock lock(g_lock);
    return g_bindings[action].slot[kMouseSlot].code;
}

void host_binding_describe(int action, char* out, std::size_t size) {
    if (!size) return;
    std::string text;
    if (Valid(action)) {
        Binding b;
        {
            std::scoped_lock lock(g_lock);
            b = g_bindings[action];
        }
        for (const Input& in : b.slot) {
            const std::string one = in.None() ? std::string() : ScreenText(in);
            if (one.empty()) continue;
            if (!text.empty()) text += "  /  ";
            text += one;
        }
    }
    std::snprintf(out, size, "%s", text.empty() ? "Unbound" : text.c_str());
}

void host_binding_clear(int action) {
    if (!Valid(action)) return;
    {
        std::scoped_lock lock(g_lock);
        g_bindings[action] = {};
    }
    Changed();
    host_log("keys: %s cleared", kActions[action].key);
}

namespace {
bool g_toggles[kBindCount] = {};
std::atomic<bool> g_toggled[kBindCount] = {};

bool LoadToggles() {
    const char* env = std::getenv("BB_TOGGLE_KEYS");
    if (!env) return false;
    bool any = false;
    std::string name;
    for (const char* p = env;; ++p) {
        if (*p && *p != ',') {
            if (*p != ' ') name += *p;
            continue;
        }
        const int action = ActionNamed(name.c_str());
        if (action >= 0 && action != kBindMenu && action != kBindConfirm && action != kBindBack &&
            action != kBindDebugMenu) {
            g_toggles[action] = any = true;
        }
        name.clear();
        if (!*p) break;
    }
    return any;
}

void ApplyToggles(bool was[kBindCount], bool held[kBindCount]) {
    static const bool any = LoadToggles();
    if (!any) return;
    const bool menu = menu_pointer_in_menu();
    for (int a = 0; a < kBindCount; ++a) {
        if (!g_toggles[a]) continue;
        if (held[a] && !was[a] && !menu) g_toggled[a].store(!g_toggled[a].load(std::memory_order_relaxed), std::memory_order_relaxed);
        was[a] = held[a];
        held[a] = !menu && g_toggled[a].load(std::memory_order_relaxed);
    }
}
}

void host_bindings_keys_held(const bool* keys, int count, bool held[kBindCount]) {
    const std::uint8_t modifiers = ModifiersDown(keys, count);
    g_keyboard_modifiers.store(modifiers, std::memory_order_relaxed);
    HeldOn(false, [&](int sc) { return sc < count && keys[sc]; }, modifiers, held);
    static bool was[kBindCount] = {};
    ApplyToggles(was, held);

    for (const auto& pair : kOpposites) {
        if (held[pair[0]] && held[pair[1]]) held[pair[0]] = held[pair[1]] = false;
    }
    held[kBindWalk] = held[kBindWalk] || g_walk_on_mouse.load(std::memory_order_relaxed);
}

void host_bindings_mouse_held(std::uint32_t inputs, bool held[kBindCount]) {
    HeldOn(true, [&](int m) { return m >= 1 && m <= kMouseInputs && (inputs >> (m - 1) & 1); },
           g_keyboard_modifiers.load(std::memory_order_relaxed), held);
    static bool was[kBindCount] = {};
    ApplyToggles(was, held);
    g_walk_on_mouse.store(held[kBindWalk], std::memory_order_relaxed);
}

bool host_bindings_dodge(std::uint64_t now_ms, bool menu, PadState& pad) {
    constexpr std::uint64_t kShortestStepMs = 20, kLongestStepMs = 67, kSprintTailMs = 150;
    constexpr int kGaps = 8;
    static bool was_dodge = false, was_backstep = false, from_sprint = false, still = false;
    static std::uint64_t began = 0, step_ms = kLongestStepMs, last_read = 0, last_sprint = 0;
    static std::uint64_t gaps[kGaps] = {};
    static int gap_at = 0;
    if (last_read && now_ms > last_read) {
        gaps[gap_at] = now_ms - last_read;
        gap_at = (gap_at + 1) % kGaps;
    }
    last_read = now_ms;
    const bool dodge = g_dodge_down.exchange(false, std::memory_order_relaxed);
    const bool backstep = g_backstep_down.exchange(false, std::memory_order_relaxed);
    const bool sprint = g_sprint_down.exchange(false, std::memory_order_relaxed);
    const bool dodge_pressed = dodge && !was_dodge, backstep_pressed = backstep && !was_backstep;
    was_dodge = dodge;
    was_backstep = backstep;
    if (menu) {
        began = 0;
        last_sprint = 0;
        return false;
    }
    if (sprint) {
        last_sprint = std::max<std::uint64_t>(now_ms, 1);
        pad.buttons |= kCircle;
    }
    const bool at_once = last_sprint && now_ms - last_sprint < kSprintTailMs;
    if ((dodge_pressed || backstep_pressed) && !began) {
        began = std::max<std::uint64_t>(now_ms, 1);
        from_sprint = (pad.buttons & kCircle) != 0;
        still = backstep_pressed;
        const std::uint64_t frame = *std::max_element(gaps, gaps + kGaps);
        step_ms = std::clamp<std::uint64_t>(frame + frame / 4, kShortestStepMs, kLongestStepMs);
    }
    if (!began) return at_once;
    const std::uint64_t step = (now_ms - began) / step_ms + (from_sprint ? 0 : 1);
    if (step > 2) {
        began = 0;
        return at_once;
    }
    if (step == 1) pad.buttons |= kCircle;
    else pad.buttons &= ~std::uint32_t(kCircle);
    if (still) pad.lx = pad.ly = kStickRest;
    return at_once && step == 0;
}

void host_bindings_apply(const bool held[kBindCount], bool strong, PadState& pad) {
    const bool menu = menu_pointer_in_menu();
    if (held[kBindDodge]) g_dodge_down.store(true, std::memory_order_relaxed);
    if (held[kBindBackstep]) g_backstep_down.store(true, std::memory_order_relaxed);
    if (held[kBindSprint]) g_sprint_down.store(true, std::memory_order_relaxed);
    for (int a = 0; a < kBindCount; ++a) {
        if (!held[a]) continue;
        if ((a == kBindConfirm || a == kBindBack) && !menu) continue;
        std::uint32_t bit = kActions[a].button;
        if (a == kBindAttack && strong) bit = kR2;
        if (a == kBindConfirm) bit = menu_confirm_button();
        if (a == kBindBack) bit = menu_back_button();
        pad.buttons |= bit;
        if (bit == kL2) pad.l2 = 255;
        if (bit == kR2) pad.r2 = 255;
        switch (a) {
        case kBindLookU: pad.ry = kStickLow; break;
        case kBindLookD: pad.ry = kStickHigh; break;
        case kBindLookL: pad.rx = kStickLow; break;
        case kBindLookR: pad.rx = kStickHigh; break;
        case kBindGestures:
        case kBindEffects:

            if (pad.touch_count < 2) {
                PadState::Finger& f = pad.touch[pad.touch_count++];
                f.x = std::uint16_t(a == kBindGestures ? kTouchWidth / 4 : kTouchWidth * 3 / 4);
                f.y = std::uint16_t(kTouchHeight / 2);
                f.id = 200;
            }
            break;
        default: break;
        }
    }

    const int x = int(held[kBindMoveR]) - int(held[kBindMoveL]);
    const int y = int(held[kBindMoveB]) - int(held[kBindMoveF]);
    if (x || y) {
        int reach = kStickHigh - kStickRest;
        if (held[kBindWalk]) reach = int(std::lround(reach * kWalkPush / (x && y ? std::sqrt(2.0f) : 1.0f)));
        if (x) pad.lx = std::uint8_t(kStickRest + x * reach);
        if (y) pad.ly = std::uint8_t(kStickRest + y * reach);
    }
}

void host_bind_capture_begin(int action) {
    if (!Valid(action)) return;
    g_capture_modifier.store(0, std::memory_order_relaxed);
    g_capture_began.store(NowMs(), std::memory_order_relaxed);
    g_capturing.store(action, std::memory_order_relaxed);
    host_log("keys: rebinding %s: a key, a mouse button or the wheel (Escape keeps it, Delete unbinds it)",
                 kActions[action].key);
}

int host_bind_capturing() { return g_capturing.load(std::memory_order_relaxed); }

bool host_bind_capture_key(int scancode, unsigned sdl_mods) {
    const int action = g_capturing.load(std::memory_order_relaxed);
    if (action < 0) return false;
    if (scancode == SDL_SCANCODE_ESCAPE) {
        EndCapture(action, "unchanged");
    } else if (scancode == SDL_SCANCODE_DELETE) {
        {
            std::scoped_lock lock(g_lock);
            g_bindings[action] = {};
        }
        EndCapture(action, "unbound");
    } else if (IsModifierKey(scancode)) {
        g_capture_modifier.store(scancode, std::memory_order_relaxed);
    } else {
        BindKey(action, scancode, ModifiersOf(SDL_Keymod(sdl_mods)));
    }
    return true;
}

bool host_bind_capture_key_up(int scancode) {
    const int action = g_capturing.load(std::memory_order_relaxed);
    if (action < 0 || !scancode || g_capture_modifier.load(std::memory_order_relaxed) != scancode) return false;
    BindKey(action, scancode, 0);
    return true;
}

bool host_bind_capture_mouse(int input) {
    const int action = g_capturing.load(std::memory_order_relaxed);
    if (action < 0 || input < 1 || input > kMouseInputs) return false;
    const std::int64_t since = NowMs() - g_capture_began.load(std::memory_order_relaxed);
    if (since < kCaptureClickMs) {
        host_log("keys: a mouse press %lld ms into the capture is part of the click that began it",
                     static_cast<long long>(since));
        return true;
    }
    const Input in{std::uint16_t(input), true, ModifiersOf(SDL_GetModState())};
    {
        std::scoped_lock lock(g_lock);
        TakeFromOthers(action, in);
        Binding& b = g_bindings[action];
        if (!in.modifiers) {
            b.slot[kMouseSlot] = in;
        } else {

            int slot = -1;
            for (int s = kFirstExtra; s < kSlots && slot < 0; ++s) {
                if (b.slot[s] == in) slot = s;
            }
            for (int s = kFirstExtra; s < kSlots && slot < 0; ++s) {
                if (b.slot[s].None()) slot = s;
            }
            b.slot[slot < 0 ? kSlots - 1 : slot] = in;
        }
    }
    EndCapture(action, "rebound");
    return true;
}

int host_bind_capture_take_done() { return g_capture_done.exchange(-1); }

bool host_bind_capture_blocking() {
    return g_capturing.load(std::memory_order_relaxed) >= 0 || g_after_capture.load(std::memory_order_relaxed);
}

void host_bind_capture_note_held(bool any_down) {
    if (g_after_capture.load(std::memory_order_relaxed) &&
        (!any_down || NowMs() - g_after_capture_since.load(std::memory_order_relaxed) > kAfterCaptureMaxMs))
        g_after_capture.store(false, std::memory_order_relaxed);
}

void host_bind_note_clear_key() { g_clear_pressed.store(NowMs(), std::memory_order_relaxed); }

bool host_bind_take_clear_key() {
    const std::int64_t at = g_clear_pressed.exchange(0);
    return at && NowMs() - at < kClearKeyMs;
}
