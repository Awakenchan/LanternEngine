// SPDX-License-Identifier: GPL-3.0-or-later
#include "input.h"
#include "bindings.h"
#include "ini_bindings.h"
#include "menu_pointer.h"
#include "mouse_camera.h"
#include "../bbport_overlay.h"
#include "../bbport_settings.h"
#include "../engine/change_appearance.h"
#include "../engine/online_fixes.h"
#include <SDL3/SDL.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace {
using namespace PadBits;

std::atomic<bool> g_active{false};
std::atomic<InputDevice> g_device{InputDevice::KeyboardMouse};
std::atomic<bool> g_focused{true}, g_in_window{true}, g_text_entry{false};

std::atomic<bool> g_relative{false}, g_wants_relative{false};

struct Mouse {
    float x = 0, y = 0;
    int window_w = 1920, window_h = 1080;
    std::uint32_t held = 0;
    std::uint32_t pressed = 0;
    float wheel = 0;
    float drag_x = 0, drag_y = 0;
};
std::mutex g_mouse_lock;
Mouse g_mouse;

std::mutex g_pad_lock;
PadState g_last_pad;

std::uint64_t g_saved_serial = 0;

constexpr float kPreviewLeft = 1028, kPreviewRight = 1810, kPreviewTop = 117, kPreviewBottom = 895;

constexpr std::uint32_t kLeftButton = SDL_BUTTON_LMASK, kRightButton = SDL_BUTTON_RMASK;

constexpr std::uint32_t kWheelUpInput = 1u << 5, kWheelDownInput = 1u << 6;

const char* ConfigPath() {
    const char* path = std::getenv("BB_CONFIG");
    return path && *path ? path : "lantern.ini";
}

void LoadBindings() {
    host_bindings_load_defaults();
    FILE* file = std::fopen(ConfigPath(), "r");
    if (!file) return;
    char line[512];
    while (std::fgets(line, sizeof line, file)) {
        char* value = std::strchr(line, '=');
        if (!value || std::strncmp(line, "bind.", 5) != 0) continue;
        *value++ = 0;
        for (char* end = value - 1; end > line && (end[-1] == ' ' || end[-1] == '\t');) *--end = 0;
        value[std::strcspn(value, "\r\n")] = 0;
        host_bindings_parse(line + 5, value);
    }
    std::fclose(file);
}

void NoteDevice(InputDevice d) { g_device.store(d, std::memory_order_relaxed); }

std::uint8_t StickFromUnit(float v) { return std::uint8_t(std::clamp(int(kStickRest) + int(127 * v), 0, int(kStickHigh))); }

constexpr std::uint64_t kSettleMs = 250;
std::uint64_t g_settle_until = 0;
std::uint32_t g_dpad_blocked = 0;
std::uint32_t g_last_buttons = 0;
bool g_turning_hunter = false;

constexpr float kDragStepPixels = 8, kDragStartPixels = 6;
bool g_dragging_value = false, g_drag_moved = false;
constexpr std::size_t kSprintHoldAt = 0x5127b48;
constexpr float kSprintHold = 0.4f;
std::atomic<float*> g_sprint_hold{nullptr};
float g_drag_anchor = 0;
int g_drag_steps = 0;
bool g_drag_released = true;

std::uint32_t DpadFromKeys(const bool* keys, const bool held[kBindCount], bool keys_move_hunter) {
    if (!keys) return 0;
    const bool movement = !keys_move_hunter;
    std::uint32_t dpad = 0;
    if (keys[SDL_SCANCODE_UP] || (movement && held[kBindMoveF])) dpad |= kUp;
    if (keys[SDL_SCANCODE_DOWN] || (movement && held[kBindMoveB])) dpad |= kDown;
    if (keys[SDL_SCANCODE_LEFT] || (movement && held[kBindMoveL])) dpad |= kLeft;
    if (keys[SDL_SCANCODE_RIGHT] || (movement && held[kBindMoveR])) dpad |= kRight;
    return dpad;
}

void WatchSprint(const bool held[kBindCount]) {
    static bool was_sprinting = false;
    static int reports = 0;
    const bool sprinting = held[kBindRoll];
    const int moving = held[kBindMoveF] + held[kBindMoveB] + held[kBindMoveL] + held[kBindMoveR];
    if (was_sprinting && !sprinting && moving >= 2 && reports < 20) {
        ++reports;
        host_log("Dodge / Sprint key read as let go while moving %s%s%s%s (if it was still held, the keyboard drops "
                 "that combination of keys)",
                 held[kBindMoveF] ? "F" : "", held[kBindMoveB] ? "B" : "", held[kBindMoveL] ? "L" : "",
                 held[kBindMoveR] ? "R" : "");
    }
    was_sprinting = sprinting;
}

void ApplyMouseBindings(std::uint32_t inputs, bool keyboard_strong, PadState& pad) {
    bool held[kBindCount];
    host_bindings_mouse_held(inputs, held);
    host_bindings_apply(held, keyboard_strong || held[kBindStrongMod], pad);
}

struct MouseRead {
    std::uint32_t held = 0, pressed = 0;
    float wheel = 0, drag_x = 0, drag_y = 0;
};

MouseRead TakeMouse() {
    std::scoped_lock lock(g_mouse_lock);
    MouseRead r{g_mouse.held, g_mouse.pressed, g_mouse.wheel, g_mouse.drag_x, g_mouse.drag_y};
    g_mouse.pressed = 0;
    g_mouse.wheel = g_mouse.drag_x = g_mouse.drag_y = 0;
    return r;
}

std::uint32_t DragValue(const MouseRead& mouse, bool preview) {
    float x = 0, y = 0;
    const bool on_stage = BbInput::PointerOnStage(x, y);
    if (preview && (mouse.pressed & kLeftButton) && !g_turning_hunter && on_stage) {
        g_dragging_value = true;
        g_drag_moved = false;
        g_drag_anchor = x;
        g_drag_steps = 0;
    }
    if (!preview || !(mouse.held & kLeftButton) || g_turning_hunter) {
        if (g_dragging_value) MenuPointer::HoldRow(false);
        g_dragging_value = false;
    }
    if (g_dragging_value && on_stage) {
        if (!g_drag_moved && std::fabs(x - g_drag_anchor) >= kDragStartPixels) {
            g_drag_moved = true;
            MenuPointer::HoldRow(true);
        }
        if (g_drag_moved) {
            const int steps = int((x - g_drag_anchor) / kDragStepPixels);
            g_drag_steps += steps;
            g_drag_anchor += float(steps) * kDragStepPixels;
        }
    }
    if (!g_drag_released || g_drag_steps == 0) {
        g_drag_released = true;
        return 0;
    }
    g_drag_released = false;
    const bool right = g_drag_steps > 0;
    g_drag_steps += right ? -1 : 1;
    return right ? kRight : kLeft;
}

void AddKeyboardAndMouse(PadState& pad, const bool* keys, int nkeys, const MouseRead& mouse, bool menu,
                         bool keys_move_hunter, bool camera) {
    const auto& s = BbSettings::Get();
    bool held[kBindCount] = {};
    if (keys) host_bindings_keys_held(keys, nkeys, held);
    if (g_device.load(std::memory_order_relaxed) == InputDevice::KeyboardMouse) {

        for (std::uint8_t* stick : {&pad.lx, &pad.ly, &pad.rx, &pad.ry}) {
            if (*stick == 128) *stick = kStickRest;
        }
    }
    const std::uint64_t now = SDL_GetTicks();
    const bool navigating = menu && !keys_move_hunter && MenuPointer::Navigating() && now >= g_settle_until;
    const std::uint32_t dpad = DpadFromKeys(keys, held, keys_move_hunter);
    g_dpad_blocked &= dpad;
    if (keys && !menu) WatchSprint(held);
    if ((navigating || keys_move_hunter) && keys) {
        pad.buttons |= dpad & ~g_dpad_blocked;
        if (keys[SDL_SCANCODE_RETURN]) pad.buttons |= menu_confirm_button();
        if (keys[SDL_SCANCODE_BACKSPACE]) pad.buttons |= menu_back_button();
        if (!keys_move_hunter) {
            for (int a = kBindMoveF; a <= kBindMoveR; ++a) held[a] = false;
            for (int a = kBindLookU; a <= kBindLookR; ++a) held[a] = false;
        }
    }
    host_bindings_apply(held, held[kBindStrongMod], pad);

    const bool preview = menu && BbEngine::ChangeAppearance::PreviewShown();
    if (!preview || !(mouse.held & kLeftButton)) g_turning_hunter = false;
    if (preview && (mouse.pressed & kLeftButton)) {
        float x, y;
        g_turning_hunter = BbInput::PointerOnStage(x, y) && x >= kPreviewLeft && x <= kPreviewRight &&
                           y >= kPreviewTop && y <= kPreviewBottom;
    }

    if (menu) {
        if (s.mouse_menu) {
            if ((mouse.pressed & kLeftButton) && !g_turning_hunter) MenuPointer::Click();
            if (preview) ApplyMouseBindings((mouse.held | mouse.pressed) & ~kLeftButton, held[kBindStrongMod], pad);
            else if (mouse.pressed & kRightButton) pad.buttons |= menu_back_button();
            if (preview && mouse.wheel != 0) {
                pad.buttons |= mouse.wheel > 0 ? kR1 : kL1;
            } else if (navigating || keys_move_hunter) {
                if (mouse.wheel > 0) pad.buttons |= kUp;
                if (mouse.wheel < 0) pad.buttons |= kDown;
            }
            pad.buttons |= MenuPointer::TakePress();
            pad.buttons |= DragValue(mouse, preview);
        }

        const std::uint32_t closing = menu_confirm_button() | menu_back_button() | kOptions;
        if (pad.buttons & ~g_last_buttons & closing) {
            g_dpad_blocked |= dpad;
            g_settle_until = now + kSettleMs;
        }
    } else {
        const std::uint32_t wheel = mouse.wheel > 0 ? kWheelUpInput : mouse.wheel < 0 ? kWheelDownInput : 0;
        ApplyMouseBindings(mouse.held | mouse.pressed | wheel, held[kBindStrongMod], pad);
    }

    if (g_turning_hunter) {
        const float gain = 2.5f * float(std::max(1, int(s.mouse_sensitivity)));
        pad.rx = std::uint8_t(std::clamp(int(kStickRest) + int(mouse.drag_x * gain), 0, int(kStickHigh)));
        pad.ry = std::uint8_t(std::clamp(int(kStickRest) + int(mouse.drag_y * gain), 0, int(kStickHigh)));
    }
    float fx, fy;
    if (camera && MouseCamera::Flick(fx, fy)) {
        pad.rx = StickFromUnit(fx);
        pad.ry = StickFromUnit(fy);
    }
    const bool sprint_at_once = host_bindings_dodge(now, menu, pad);
    if (float* hold = g_sprint_hold.load(std::memory_order_relaxed)) *hold = sprint_at_once ? 0.0f : kSprintHold;
    g_last_buttons = pad.buttons;
}
}

void host_log(const char* format, ...) {
    std::fputs("Input: ", stdout);
    va_list args;
    va_start(args, format);
    std::vprintf(format, args);
    va_end(args);
    std::putchar('\n');
}

InputDevice host_input_device() { return g_device.load(std::memory_order_relaxed); }

bool debug_menu_active() { return false; }

namespace BbInput {
bool Enabled() { return g_active.load(std::memory_order_relaxed) && BbSettings::Get().pc_controls; }

bool Install(std::uint8_t* image, std::size_t size) {
    if (!BbSettings::Get().pc_controls) return false;
    LoadBindings();
    BbInputConfig::append_bindings = [](BbInputConfig::Items& lines) {
        for (int a = 0; a < kBindDebugMenu; ++a)
            lines.emplace_back(std::string("bind.") + host_binding_info(a).key, host_binding_ini_value(a));
    };
    g_saved_serial = host_bindings_serial();
    const bool camera = MouseCamera::Install(image, size);
    if (size >= kSprintHoldAt + sizeof(float)) {
        float hold = 0;
        std::memcpy(&hold, image + kSprintHoldAt, sizeof hold);
        if (hold == kSprintHold) g_sprint_hold.store(reinterpret_cast<float*>(image + kSprintHoldAt), std::memory_order_relaxed);
        else host_log("the Sprint key sprints after the game's own wait (not the 1.09 game code)");
    }
    MenuPointer::Install(image, size);
    g_active.store(true, std::memory_order_relaxed);
    host_log("keyboard and mouse play on, the mouse camera %s", camera ? "ready" : "unavailable");
    return true;
}

void Tick() {
    if (!g_active.load(std::memory_order_relaxed)) return;
    const std::uint64_t serial = host_bindings_serial();
    if (serial != g_saved_serial) {
        BbSettings::Save();
        g_saved_serial = serial;
    }
}

void Event(const SDL_Event& e, bool blocked) {
    if (!g_active.load(std::memory_order_relaxed)) return;
    switch (e.type) {
    case SDL_EVENT_WINDOW_FOCUS_LOST: {
        g_focused = false;
        g_wants_relative = false;
        MouseCamera::Reset();
        std::scoped_lock lock(g_mouse_lock);
        g_mouse.held = g_mouse.pressed = 0;
        g_mouse.wheel = 0;
        return;
    }
    case SDL_EVENT_WINDOW_FOCUS_GAINED: g_focused = true; break;
    case SDL_EVENT_WINDOW_MOUSE_LEAVE: g_in_window = false; break;
    case SDL_EVENT_WINDOW_MOUSE_ENTER: g_in_window = true; break;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN: NoteDevice(InputDevice::Pad); break;
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
        if (std::abs(e.gaxis.value) > 8000) NoteDevice(InputDevice::Pad);
        break;
    default: break;
    }
    if (blocked) return;
    switch (e.type) {
    case SDL_EVENT_KEY_DOWN:
        NoteDevice(InputDevice::KeyboardMouse);
        host_bind_capture_key(e.key.scancode, e.key.mod);
        if (e.key.scancode == SDL_SCANCODE_DELETE) host_bind_note_clear_key();
        break;
    case SDL_EVENT_KEY_UP: host_bind_capture_key_up(e.key.scancode); break;
    case SDL_EVENT_MOUSE_MOTION: {
        NoteDevice(InputDevice::KeyboardMouse);
        std::scoped_lock lock(g_mouse_lock);
        g_mouse.x = e.motion.x;
        g_mouse.y = e.motion.y;
        if (g_relative && g_focused) {
            MouseCamera::AddCounts(e.motion.xrel, e.motion.yrel);
        } else if (g_mouse.held & kLeftButton) {
            g_mouse.drag_x += e.motion.xrel;
            g_mouse.drag_y += e.motion.yrel;
        }
        break;
    }
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP: {
        NoteDevice(InputDevice::KeyboardMouse);
        const int button = e.button.button;
        const std::uint32_t bit = button >= 1 && button <= 5 ? 1u << (button - 1) : 0;
        const bool down = e.type == SDL_EVENT_MOUSE_BUTTON_DOWN;
        const bool rebound = down && host_bind_capture_mouse(button);
        std::scoped_lock lock(g_mouse_lock);
        g_mouse.x = e.button.x;
        g_mouse.y = e.button.y;
        if (down) {
            g_mouse.held |= bit;
            if (!rebound) g_mouse.pressed |= bit;
        } else {
            g_mouse.held &= ~bit;
        }
        break;
    }
    case SDL_EVENT_MOUSE_WHEEL:
        NoteDevice(InputDevice::KeyboardMouse);
        if (e.wheel.y != 0 && !host_bind_capture_mouse(e.wheel.y > 0 ? 6 : 7)) {
            std::scoped_lock lock(g_mouse_lock);
            g_mouse.wheel += e.wheel.y;
        }
        break;
    default: break;
    }
}

void Pump(SDL_Window* window, bool text_active) {
    g_text_entry = text_active;
    if (!g_active.load(std::memory_order_relaxed)) return;

    if (!g_focused && (SDL_GetWindowFlags(window) & SDL_WINDOW_INPUT_FOCUS)) g_focused = true;
    int w = 0, h = 0;
    SDL_GetWindowSize(window, &w, &h);
    {
        std::scoped_lock lock(g_mouse_lock);
        g_mouse.window_w = w;
        g_mouse.window_h = h;
    }
    const bool want = g_wants_relative && BbSettings::Get().pc_controls && !text_active && !BbOverlay::CapturesInput() &&
                      g_focused;
    if (want != g_relative && SDL_SetWindowRelativeMouseMode(window, want)) {
        g_relative = want;
        MouseCamera::Reset();
        if (want) SDL_HideCursor();
        else SDL_ShowCursor();
    }
}

bool MouseIsMenuPointer() {
    return BbSettings::Get().mouse_menu && !BbOverlay::MenuOpen() && !g_relative &&
           g_device.load(std::memory_order_relaxed) == InputDevice::KeyboardMouse;
}

bool PointerOnStage(float& x, float& y) {
    std::scoped_lock lock(g_mouse_lock);
    if (!g_in_window || !g_focused || g_relative || g_mouse.window_w <= 0 || g_mouse.window_h <= 0) return false;

    const auto& s = BbSettings::Get();
    const int output = std::clamp(BbSettings::ResolutionNeedsRestart() ? s.startup_output_res : s.output_res.load(), 0,
                                  BbSettings::OutputCount - 1);
    const float ow = float(BbSettings::OutputWidth(output)), oh = float(BbSettings::OutputHeight(output));
    const float ww = float(g_mouse.window_w), wh = float(g_mouse.window_h);
    const float fit = std::min(ww / ow, wh / oh);
    const float output_x = (ww - ow * fit) / 2, output_y = (wh - oh * fit) / 2;
    if (!s.widescreen) {
        x = (g_mouse.x - output_x) / fit * 1920 / ow;
        y = (g_mouse.y - output_y) / fit * 1080 / oh;
        return true;
    }
    const float stage = std::min(ow / 1920, oh / 1080);
    const float stage_x = output_x + (ow - 1920 * stage) * fit / 2, stage_y = output_y + (oh - 1080 * stage) * fit / 2;
    x = (g_mouse.x - stage_x) / (fit * stage);
    y = (g_mouse.y - stage_y) / (fit * stage);
    return true;
}

bool TextEntryOpen() { return g_text_entry.load(std::memory_order_relaxed); }

PadState LastPad() {
    std::scoped_lock lock(g_pad_lock);
    return g_last_pad;
}
}

extern "C" int bbgpu_menu_buttons(std::uint32_t* confirm, std::uint32_t* back) {
    if (!g_active.load(std::memory_order_relaxed)) return 0;
    *confirm = menu_confirm_button();
    *back = menu_back_button();
    return MenuPointer::Navigating() ? 1 : 0;
}

static void SpectateButton(std::uint32_t& buttons) {
    static bool was = false;
    const bool down = (buttons & PadBits::kR1) != 0;
    if (BbEngine::OnlineFixes::Spectating() && !menu_pointer_in_menu()) {
        if (down && !was) BbEngine::OnlineFixes::SpectateNext();
        buttons &= ~PadBits::kR1;
    }
    was = down;
}

extern "C" int bbgpu_pc_input(BbPcPad* d, const bool* keys, int nkeys, int gamepad_present) {
    const auto& s = BbSettings::Get();
    if (!g_active.load(std::memory_order_relaxed) || !s.pc_controls) {
        g_wants_relative = false;
        SpectateButton(d->buttons);
        return 0;
    }
    PadState pad;
    pad.buttons = d->buttons;
    pad.l2 = d->l2;
    pad.r2 = d->r2;
    if (gamepad_present) {
        pad.lx = d->lx;
        pad.ly = d->ly;
        pad.rx = d->rx;
        pad.ry = d->ry;
    }
    pad.touch_count = std::min<std::uint8_t>(d->touch_count, 2);
    for (int i = 0; i < pad.touch_count; ++i) pad.touch[i] = {d->touch[i].x, d->touch[i].y, d->touch[i].id};

    const bool keyboard_mouse = g_device.load(std::memory_order_relaxed) == InputDevice::KeyboardMouse;
    const bool menu = menu_pointer_in_menu();
    const bool keys_move_hunter = menu && s.menu_move && !MenuPointer::FrontEnd();
    const bool camera = s.mouse_camera && MouseCamera::Installed() && (!menu || (keys_move_hunter && !s.mouse_menu)) &&
                        !g_text_entry && g_focused;
    g_wants_relative = camera && keyboard_mouse;
    MouseCamera::Publish(s.mouse_sensitivity, s.mouse_invert_x, s.mouse_invert_y,
                         camera && keyboard_mouse && !s.mouse_auto_rotation);

    MouseRead mouse = TakeMouse();
    if (BbEngine::OnlineFixes::Spectating() && !menu) {
        if (mouse.pressed & kLeftButton) BbEngine::OnlineFixes::SpectateNext();
        mouse.held &= ~kLeftButton;
        mouse.pressed &= ~kLeftButton;
    }
    bool any_down = mouse.held != 0;
    for (int i = 0; keys && i < nkeys && !any_down; ++i) any_down = keys[i];
    host_bind_capture_note_held(any_down);
    if (!g_focused || g_text_entry || host_bind_capture_blocking()) {

        pad = PadState{};
        g_wants_relative = false;
    } else {
        AddKeyboardAndMouse(pad, keys, nkeys, mouse, menu, keys_move_hunter, camera);
    }
    SpectateButton(pad.buttons);
    {
        std::scoped_lock lock(g_pad_lock);
        g_last_pad = pad;
    }
    d->buttons = pad.buttons;
    d->lx = pad.lx;
    d->ly = pad.ly;
    d->rx = pad.rx;
    d->ry = pad.ry;
    d->l2 = pad.l2;
    d->r2 = pad.r2;
    d->touch_count = pad.touch_count;
    for (int i = 0; i < pad.touch_count; ++i) {
        d->touch[i].x = pad.touch[i].x;
        d->touch[i].y = pad.touch[i].y;
        d->touch[i].id = pad.touch[i].id;
    }
    return 1;
}
