// SPDX-License-Identifier: GPL-3.0-or-later
#include "../gpu/shim/input/bindings.h"
#include "../gpu/shim/input/mouse_turn.h"
#include "../gpu/shim/input/pad.h"
#include <SDL3/SDL.h>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

void host_log(const char*, ...) {}
std::uint32_t menu_confirm_button() { return PadBits::kCross; }
std::uint32_t menu_back_button() { return PadBits::kCircle; }
bool g_in_menu = false;
bool menu_pointer_in_menu() { return g_in_menu; }

namespace {
struct Keyboard {
    bool keys[SDL_SCANCODE_COUNT] = {};
    bool held[kBindCount] = {};
    Keyboard& Down(SDL_Scancode sc, bool down = true) {
        keys[sc] = down;
        host_bindings_keys_held(keys, SDL_SCANCODE_COUNT, held);
        return *this;
    }
};

PadState Pad(std::initializer_list<int> actions, bool strong = false) {
    bool held[kBindCount] = {};
    for (int a : actions) held[a] = true;
    PadState pad;
    host_bindings_apply(held, strong, pad);
    return pad;
}

std::string Described(int action) {
    char text[160];
    host_binding_describe(action, text, sizeof text);
    return text;
}

void Defaults() {
    host_bindings_load_defaults();
    assert(host_binding_key(kBindMoveF) == SDL_SCANCODE_W && host_binding_key(kBindWalk) == SDL_SCANCODE_LALT);
    assert(host_binding_key(kBindAttack) == 0 && host_binding_mouse(kBindAttack) == 1);
    assert(host_binding_ini_value(kBindAttack) == "Mouse1");
    assert(host_binding_ini_value(kBindTransform) == "C, Mouse2");
    assert(Described(kBindTransform) == "C  /  Middle Button");
    assert(Described(kBindDebugMenu) == "` (backtick)");
    assert(std::string(host_binding_info(kBindRoll).key) == "roll" && host_binding_info(kBindRoll).button == PadBits::kCircle);
}

void Movement() {
    host_bindings_load_defaults();
    PadState p = Pad({kBindMoveF});
    assert(p.lx == kStickRest && p.ly == kStickLow);
    p = Pad({kBindMoveF, kBindMoveR});
    assert(p.lx == kStickHigh && p.ly == kStickLow);

    const PadState straight = Pad({kBindMoveF, kBindWalk}), diagonal = Pad({kBindMoveF, kBindMoveR, kBindWalk});
    const float reach = float(kStickRest - straight.ly);
    assert(reach > 60 && reach < 90);
    assert(std::abs(std::hypot(float(diagonal.lx) - kStickRest, float(diagonal.ly) - kStickRest) - reach) <= 1.5f);

    Keyboard k;
    k.Down(SDL_SCANCODE_W).Down(SDL_SCANCODE_S);
    assert(!k.held[kBindMoveF] && !k.held[kBindMoveB]);
    k.Down(SDL_SCANCODE_S, false);
    assert(k.held[kBindMoveF]);
}

void Buttons() {
    host_bindings_load_defaults();
    PadState p = Pad({kBindAttack}, true);
    assert(p.buttons == PadBits::kR2 && p.r2 == 255);
    p = Pad({kBindFirearm});
    assert(p.buttons == PadBits::kL2 && p.l2 == 255);
    p = Pad({kBindConfirm, kBindBack});
    assert(p.buttons == 0);
    g_in_menu = true;
    p = Pad({kBindConfirm, kBindBack});
    g_in_menu = false;
    assert(p.buttons == (PadBits::kCross | PadBits::kCircle));
    p = Pad({kBindLookL, kBindLookD});
    assert(p.rx == kStickLow && p.ry == kStickHigh);

    p = Pad({kBindGestures, kBindEffects});
    assert(p.buttons == PadBits::kTouchPad && p.touch_count == 2);
    assert(p.touch[0].x < kTouchWidth / 2 && p.touch[1].x > kTouchWidth / 2 && p.touch[0].id == p.touch[1].id);
    p = Pad({kBindDebugMenu});
    assert(p.buttons == 0);
}

void IniValues() {
    host_bindings_load_defaults();
    assert(host_bindings_parse("attack", "E, Mouse2"));
    assert(host_binding_key(kBindAttack) == SDL_SCANCODE_E && host_binding_mouse(kBindAttack) == 2);
    assert(host_binding_ini_value(kBindAttack) == "E, Mouse2");
    assert(host_bindings_parse("use_item", "F, Shift+E, Ctrl+Mouse4"));
    assert(host_binding_ini_value(kBindUseItem) == "F, Shift+E, Ctrl+Mouse4");
    assert(Described(kBindUseItem) == "F  /  Shift+E  /  Ctrl+Button 4");
    assert(host_bindings_parse("lock_on", " \"control+alt+Q\" , WheelDown "));
    assert(host_binding_ini_value(kBindLockOn) == "Ctrl+Alt+Q, WheelDown");

    assert(host_bindings_parse("interact", "E, NoSuchKey, Mouse5"));
    assert(host_binding_ini_value(kBindInteract) == "E, Mouse5");
    assert(host_bindings_parse("blood_vial", "R, 1, 2, 3, 4, 5"));
    assert(host_binding_ini_value(kBindBloodVial) == "R, 1, 2, 3");
    assert(host_bindings_parse("gestures", "none") && host_binding_ini_value(kBindGestures) == "none");
    assert(Described(kBindGestures) == "Unbound");

    assert(host_bindings_parse("dpad_down", "Z") && host_binding_key(kBindSwitchItem) == SDL_SCANCODE_Z);
    assert(!host_bindings_parse("jump", "Space"));
}

void Modifiers() {
    host_bindings_load_defaults();
    assert(host_bindings_parse("use_item", "F, Shift+E, Ctrl+Mouse4"));
    assert(host_bindings_parse("interact", "E, Mouse5"));
    Keyboard k;
    k.Down(SDL_SCANCODE_E);
    assert(k.held[kBindInteract] && !k.held[kBindUseItem]);
    k.Down(SDL_SCANCODE_LSHIFT);
    assert(!k.held[kBindInteract] && k.held[kBindUseItem] && k.held[kBindStrongMod]);
    k.Down(SDL_SCANCODE_E, false).Down(SDL_SCANCODE_LSHIFT, false).Down(SDL_SCANCODE_RCTRL);
    bool mouse[kBindCount];
    host_bindings_mouse_held(1u << 3, mouse);
    assert(mouse[kBindUseItem]);
    k.Down(SDL_SCANCODE_RCTRL, false);
    host_bindings_mouse_held(1u << 3, mouse);
    assert(!mouse[kBindUseItem]);

    assert(host_bindings_parse("walk", "Mouse5"));
    host_bindings_mouse_held(1u << 4, mouse);
    k.Down(SDL_SCANCODE_W);
    assert(k.held[kBindWalk]);
    host_bindings_mouse_held(0, mouse);
    k.Down(SDL_SCANCODE_W);
    assert(!k.held[kBindWalk]);
}

void Rebinding() {
    host_bindings_load_defaults();
    const std::uint64_t serial = host_bindings_serial();

    host_bind_capture_begin(kBindAttack);
    assert(host_bind_capturing() == kBindAttack && host_bind_capture_blocking());
    assert(host_bind_capture_key(SDL_SCANCODE_F));
    assert(host_bind_capturing() < 0 && host_bind_capture_take_done() == kBindAttack && host_bind_capture_take_done() < 0);
    assert(host_binding_key(kBindAttack) == SDL_SCANCODE_F && host_binding_key(kBindUseItem) == 0);
    assert(host_binding_mouse(kBindAttack) == 1 && host_bindings_serial() != serial);

    assert(host_bind_capture_blocking());
    host_bind_capture_note_held(true);
    assert(host_bind_capture_blocking());
    host_bind_capture_note_held(false);
    assert(!host_bind_capture_blocking());

    host_bind_capture_begin(kBindAttack);
    host_bind_capture_key(SDL_SCANCODE_ESCAPE);
    assert(host_binding_ini_value(kBindAttack) == "F, Mouse1");
    host_bind_capture_note_held(false);
    host_bind_capture_begin(kBindAttack);
    host_bind_capture_key(SDL_SCANCODE_DELETE);
    assert(host_binding_ini_value(kBindAttack) == "none");
    host_bind_capture_note_held(false);
    assert(host_bindings_parse("attack", "F, Mouse1"));
    host_binding_clear(kBindAttack);
    assert(host_binding_ini_value(kBindAttack) == "none");

    host_bind_capture_begin(kBindLockOn);
    assert(host_bind_capture_key(SDL_SCANCODE_LSHIFT, SDL_KMOD_LSHIFT) && host_bind_capturing() == kBindLockOn);
    assert(!host_bind_capture_key_up(SDL_SCANCODE_Q));
    assert(host_bind_capture_key(SDL_SCANCODE_Q, SDL_KMOD_LSHIFT) && host_bind_capturing() < 0);
    assert(host_binding_ini_value(kBindLockOn) == "Shift+Q");
    host_bind_capture_note_held(false);
    host_bind_capture_begin(kBindLockOn);
    host_bind_capture_key(SDL_SCANCODE_RSHIFT, SDL_KMOD_RSHIFT);
    assert(host_bind_capture_key_up(SDL_SCANCODE_RSHIFT) && host_binding_ini_value(kBindLockOn) == "Right Shift");
    host_bind_capture_note_held(false);

    assert(!host_bind_capture_key(SDL_SCANCODE_K) && !host_bind_capture_mouse(1));

    host_bind_capture_begin(kBindInteract);
    assert(host_bind_capture_mouse(2) && host_bind_capturing() == kBindInteract);
    std::this_thread::sleep_for(std::chrono::milliseconds(320));
    assert(host_bind_capture_mouse(2) && host_bind_capturing() < 0);
    assert(host_binding_ini_value(kBindInteract) == "E, Mouse2" && host_binding_ini_value(kBindTransform) == "C");
    host_bind_capture_note_held(false);

    host_bind_note_clear_key();
    assert(host_bind_take_clear_key() && !host_bind_take_clear_key());
    host_bindings_load_defaults();
}

std::uint64_t g_now = 1000;
int g_failed = 0;

void Check(bool ok, int line) {
    if (ok) return;
    std::fprintf(stderr, "pad actions: check at line %d failed\n", line);
    ++g_failed;
}
#define CHECK(condition) Check(condition, __LINE__)

struct Step {
    PadState pad;
    bool at_once;
};

Step Frame(std::uint8_t actions, bool menu = false, std::uint64_t ms = 20) {
    g_now += ms;
    Step s{};
    s.pad.lx = kStickHigh;
    s.pad.ly = kStickLow;
    host_bindings_pad_actions(actions);
    s.at_once = host_bindings_dodge(g_now, menu, s.pad);
    return s;
}

bool Circle(const Step& s) { return (s.pad.buttons & PadBits::kCircle) != 0; }

void Settle() {
    for (int i = 0; i < 12; ++i) Frame(0, i < 2);
}

int PadActions() {
    g_failed = 0;
    host_bindings_load_defaults();
    Settle();

    Step s = Frame(4);
    CHECK(Circle(s) && s.at_once);
    s = Frame(0, false, 100);
    CHECK(!Circle(s) && s.at_once);
    s = Frame(0, false, 100);
    CHECK(!Circle(s) && !s.at_once);
    Settle();

    s = Frame(1);
    CHECK(Circle(s) && !s.at_once && s.pad.lx == kStickHigh && s.pad.ly == kStickLow);
    s = Frame(1);
    CHECK(Circle(s));
    s = Frame(1);
    CHECK(!Circle(s));
    s = Frame(1);
    CHECK(!Circle(s));
    s = Frame(0);
    s = Frame(1);
    CHECK(Circle(s));
    Settle();

    s = Frame(2);
    CHECK(Circle(s) && s.pad.lx == kStickRest && s.pad.ly == kStickRest);
    s = Frame(0);
    CHECK(Circle(s) && s.pad.lx == kStickRest);
    s = Frame(0);
    CHECK(!Circle(s) && s.pad.lx == kStickRest);
    s = Frame(0);
    s = Frame(0);
    CHECK(!Circle(s) && s.pad.lx == kStickHigh);
    Settle();

    s = Frame(4);
    CHECK(Circle(s));
    s = Frame(4 | 1);
    CHECK(!Circle(s) && s.at_once);
    s = Frame(4 | 1);
    CHECK(!Circle(s));
    s = Frame(4 | 1);
    CHECK(Circle(s));
    s = Frame(4 | 1);
    CHECK(!Circle(s));
    s = Frame(4 | 1);
    CHECK(Circle(s) && s.at_once);
    Settle();

    for (std::uint8_t a : {1, 2, 4}) {
        s = Frame(a, true);
        CHECK(!Circle(s) && !s.at_once && s.pad.lx == kStickHigh);
        s = Frame(a, true);
        CHECK(!Circle(s) && !s.at_once);
        s = Frame(0, true);
    }
    s = Frame(0);
    CHECK(!Circle(s) && !s.at_once);
    Settle();

    CHECK(host_bindings_parse("sprint", "B"));
    CHECK(host_bindings_parse("dodge", "N"));
    Keyboard k;
    k.Down(SDL_SCANCODE_B);
    s = Step{};
    host_bindings_apply(k.held, false, s.pad);
    g_now += 20;
    s.at_once = host_bindings_dodge(g_now, false, s.pad);
    CHECK(Circle(s) && s.at_once);
    k.Down(SDL_SCANCODE_B, false).Down(SDL_SCANCODE_N);
    s = Step{};
    host_bindings_apply(k.held, false, s.pad);
    g_now += 20;
    host_bindings_dodge(g_now, false, s.pad);
    CHECK(Circle(s));
    k.Down(SDL_SCANCODE_N, false);
    host_bindings_load_defaults();
    Settle();
    return g_failed;
}

void MouseCameraTurn() {
    using MouseTurn::DegreesPerCount;
    assert(std::abs(DegreesPerCount(5) - 0.25f) < 1e-6f && std::abs(DegreesPerCount(0) - 0.1f) < 1e-6f);
    assert(std::abs(DegreesPerCount(-1) - 0.08f) < 1e-6f && std::abs(DegreesPerCount(20) - 0.7f) < 1e-6f);
    assert(DegreesPerCount(99) == DegreesPerCount(20) && DegreesPerCount(-99) == DegreesPerCount(-10));
    assert(DegreesPerCount(-10) > 0 && DegreesPerCount(-10) < DegreesPerCount(-9));

    const MouseTurn::Settings s{DegreesPerCount(5), false, false};
    const auto turned = [&](int updates) {
        float pitch = 0, yaw = 0;
        for (int i = 0; i < updates; ++i) MouseTurn::Turn(120.0f / updates, 60.0f / updates, s, pitch, yaw, -1, 1);
        return std::pair{pitch, yaw};
    };
    const auto a = turned(30), b = turned(60), c = turned(90);
    assert(std::abs(a.first - b.first) < 1e-5f && std::abs(a.second - c.second) < 1e-5f);
    assert(std::abs(a.second - 120 * 0.25f * 0.0174532924f) < 1e-5f && a.first > 0);
    float pitch = 0, yaw = 0;
    assert(!MouseTurn::Turn(0, 0, s, pitch, yaw, -1, 1) && pitch == 0 && yaw == 0);
    const MouseTurn::Settings inverted{DegreesPerCount(5), true, true};
    assert(MouseTurn::Turn(10, 10, inverted, pitch, yaw, -1, 1) && pitch < 0 && yaw < 0);

    MouseTurn::Turn(10000, 10000, s, pitch, yaw, -0.5f, 0.5f);
    assert(pitch == 0.5f && yaw >= -3.1416f && yaw <= 3.1416f);
    MouseTurn::Turn(0, -10000, s, pitch, yaw, -0.5f, 0.5f);
    assert(pitch == -0.5f);
}
}

int main() {
#ifdef LANTERN_PAD_ACTIONS_MUTANTS
    int survivors = 0;
    for (int m = 0; m <= kPadActionsMutants; ++m) {
        g_pad_actions_mutant = m;
        const int found = PadActions();
        if (m == 0 && found) {
            std::printf("pad actions mutants: the unmutated run failed %d checks\n", found);
            return 1;
        }
        if (m > 0 && !found) {
            std::printf("pad actions mutants: mutant %d survived\n", m);
            ++survivors;
        }
        if (m > 0 && found) std::printf("pad actions mutants: mutant %d caught by %d checks\n", m, found);
    }
    if (survivors) return 1;
    std::puts("pad actions mutants: every mutant caught");
    return 0;
#endif
    Defaults();
    Movement();
    Buttons();
    IniValues();
    Modifiers();
    Rebinding();
    MouseCameraTurn();
    assert(PadActions() == 0);
    std::puts("PC input: PASS (defaults, movement and walking, buttons and touchpad, lantern.ini values, modifiers, "
              "rebinding, the mouse camera's turn, controller dodge/backstep/sprint)");
}
