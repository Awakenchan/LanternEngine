// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/key_prompts.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <initializer_list>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <SDL3/SDL.h>
#include <xxhash.h>

#include "bbport_settings.h"
#include "hud_gauge.h"
#include "engine/change_appearance.h"
#include "engine/keycap_art.h"
#include "input/bindings.h"
#include "input/menu_pointer.h"
#include "input/input.h"

namespace KeyPrompts {
namespace {

static_assert(kIconBytes == Keycap::kBytes);

enum Icon : int {
    kCross, kL1, kL2, kL3, kLeftStick, kDpadDown, kDpadLeftRight, kDpadUp, kDpadUpDown, kDpad,
    kCircle, kR1, kR2, kR3, kRightStick, kSquare, kTriangle, kOptions, kTouchLeft, kTouchRight,
    kIconCount,
};
constexpr std::uint64_t kHashes[kIconCount] = {
    0x2310f7c71b939e0bull,
    0xdf0f0104031e5f67ull,
    0xfadfdba7de6c76b0ull,
    0xd210779fe3f51673ull,
    0xcc75c8c3bfbf5b35ull,
    0xce0de7ced6567e92ull,
    0x294394b7ad99f129ull,
    0xaa338448add2c9f0ull,
    0xad221c010b1f4101ull,
    0x57d112cbe3da49b1ull,
    0xed95e3b0221d51d1ull,
    0x665da1b194608b8aull,
    0x73f2b5572f49f886ull,
    0xe0848ecdcdd5582dull,
    0x03b41e07b779abf7ull,
    0x51f55dd737e72648ull,
    0xeb8fef4557aea451ull,
    0xadd716f21d77cfdfull,
    0xce3869fe20faa939ull,
    0x9376eea6c514d1fbull,
};

constexpr std::uint32_t kPadCross = 0x4000, kPadCircle = 0x2000;

std::uint64_t Fnv(const void* data, std::size_t size) {
    std::uint64_t h = 0xcbf29ce484222325ull;
    for (std::size_t i = 0; i < size; ++i) {
        h = (h ^ static_cast<const std::uint8_t*>(data)[i]) * 0x100000001b3ull;
    }
    return h;
}

struct State {
    bool keys = false;
    bool menu = false;
    bool preview = false;
    std::uint32_t confirm = 0;
    bool mouse_camera = false;
    std::uint64_t serial = 0;
    bool xbox = false;
    bool operator==(const State&) const = default;
};

bool XboxIcons() {
    switch (BbSettings::Get().pad_icons.load(std::memory_order_relaxed)) {
    case BbSettings::PadIconsXbox: return true;
    case BbSettings::PadIconsPlayStation: return false;
    default: {
        const int type = sys_pad_type();
        return type != SDL_GAMEPAD_TYPE_UNKNOWN && type != SDL_GAMEPAD_TYPE_PS3 &&
               type != SDL_GAMEPAD_TYPE_PS4 && type != SDL_GAMEPAD_TYPE_PS5;
    }
    }
}

State Current() {
    State s;
    const auto& settings = BbSettings::Get();
    const int mode = settings.button_prompts.load(std::memory_order_relaxed);
    s.keys = settings.pc_controls && (mode == BbSettings::PromptsKeyboard ||
                                      (mode == BbSettings::PromptsAuto &&
                                       host_input_device() == InputDevice::KeyboardMouse));
    if (s.keys) {
        s.menu = menu_pointer_in_menu();
        s.preview = s.menu && BbEngine::ChangeAppearance::PreviewShown();
        s.confirm = menu_confirm_button();
        s.mouse_camera = settings.mouse_camera;
        s.serial = host_bindings_serial();
    } else {
        s.xbox = XboxIcons();
    }
    return s;
}

std::string KeyLabel(int scancode) {
    const char* name = SDL_GetScancodeName(static_cast<SDL_Scancode>(scancode));
    if (!name || !*name) {
        return "?";
    }
    static constexpr std::pair<std::string_view, const char*> kShort[] = {
        {"Left Shift", "Shift"}, {"Right Shift", "Shift"}, {"Left Ctrl", "Ctrl"}, {"Right Ctrl", "Ctrl"},
        {"Left Alt", "Alt"},     {"Right Alt", "Alt"},     {"Left GUI", "Win"},   {"Right GUI", "Win"},
        {"Return", "Enter"},     {"Keypad Enter", "Enter"}, {"Escape", "Esc"},    {"Backspace", "Bksp"},
        {"CapsLock", "Caps"},    {"Delete", "Del"},        {"Insert", "Ins"},     {"PageUp", "PgUp"},
        {"PageDown", "PgDn"},    {"PrintScreen", "PrtSc"}, {"ScrollLock", "ScrLk"}, {"Numlock", "NumLk"},
        {"`", "~"},
    };
    for (const auto& [from, to] : kShort) {
        if (from == name) {
            return to;
        }
    }
    std::string label = name;
    if (label.rfind("Keypad ", 0) == 0) {
        label = "Num" + label.substr(7);
    }
    return label.size() > 5 ? label.substr(0, 5) : label;
}

std::string ActionLabel(int action) {
    if (const int key = host_binding_key(action)) {
        return KeyLabel(key);
    }
    static constexpr const char* kMouse[] = {"-", "LMB", "RMB", "MMB", "M4", "M5", "WhUp", "WhDn"};
    const int m = host_binding_mouse(action);
    return kMouse[m >= 0 && m <= 7 ? m : 0];
}

std::optional<Keycap::Spec> ActionSpec(int action) {
    using Keycap::Spec;
    if (const int key = host_binding_key(action)) {
        return Spec{Spec::Key, {KeyLabel(key)}};
    }
    const int m = host_binding_mouse(action);
    if (m == 4 || m == 5) {
        return Spec{Spec::Key, {m == 4 ? "M4" : "M5"}};
    }
    if (m >= 1 && m <= 7) {
        return Spec{Spec::Mouse, {}, m};
    }
    return std::nullopt;
}

Keycap::Spec Caps(std::initializer_list<int> actions) {
    Keycap::Spec spec{Keycap::Spec::Keys, {}};
    for (const int action : actions) {
        spec.labels.push_back(ActionLabel(action));
    }
    return spec;
}

std::optional<Keycap::Spec> PadSpec(int icon) {
    using namespace Keycap;
    static constexpr struct {
        Icon icon;
        std::uint32_t bit;
    } kButtons[] = {
        {kCross, kPadCross}, {kCircle, kPadCircle}, {kSquare, 0x8000}, {kTriangle, 0x1000},
        {kL1, 0x400},        {kR1, 0x800},          {kL2, 0x100},      {kR2, 0x200},
        {kL3, 0x2},          {kR3, 0x4},            {kOptions, 0x8},   {kTouchLeft, 0x100000},
        {kTouchRight, 0x100000},
    };
    const auto it = std::find_if(std::begin(kButtons), std::end(kButtons), [&](const auto& b) { return b.icon == icon; });
    if (it == std::end(kButtons)) {
        return std::nullopt;
    }
    int button;
    switch (sys_pad_button(it->bit, icon == kTouchRight)) {
    case SDL_GAMEPAD_BUTTON_SOUTH: button = PadA; break;
    case SDL_GAMEPAD_BUTTON_EAST: button = PadB; break;
    case SDL_GAMEPAD_BUTTON_WEST: button = PadX; break;
    case SDL_GAMEPAD_BUTTON_NORTH: button = PadY; break;
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: button = PadLB; break;
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: button = PadRB; break;
    case SDL_GAMEPAD_BUTTON_COUNT: button = PadLT; break;
    case SDL_GAMEPAD_BUTTON_COUNT + 1: button = PadRT; break;
    case SDL_GAMEPAD_BUTTON_LEFT_STICK: button = PadLS; break;
    case SDL_GAMEPAD_BUTTON_RIGHT_STICK: button = PadRS; break;
    case SDL_GAMEPAD_BUTTON_BACK: button = PadView; break;
    case SDL_GAMEPAD_BUTTON_START: button = PadMenu; break;
    case SDL_GAMEPAD_BUTTON_MISC1: button = PadShare; break;
    case SDL_GAMEPAD_BUTTON_DPAD_UP: button = PadUp; break;
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: button = PadDown; break;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: button = PadLeft; break;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: button = PadRight; break;
    case SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1: button = PadP1; break;
    case SDL_GAMEPAD_BUTTON_RIGHT_PADDLE2: button = PadP2; break;
    case SDL_GAMEPAD_BUTTON_LEFT_PADDLE1: button = PadP3; break;
    case SDL_GAMEPAD_BUTTON_LEFT_PADDLE2: button = PadP4; break;
    case SDL_GAMEPAD_BUTTON_TOUCHPAD: button = PadTouchpad; break;
    case SDL_GAMEPAD_BUTTON_GUIDE: button = PadGuide; break;
    default: return std::nullopt;
    }
    return Spec{Spec::Pad, {}, button};
}

std::optional<Keycap::Spec> IconSpec(int icon, const State& s) {
    using Keycap::Spec;
    if (!s.keys) {
        return s.xbox ? PadSpec(icon) : std::nullopt;
    }
    if (s.preview && (icon == kL1 || icon == kR1)) {
        return Spec{Spec::Mouse, {}, icon == kR1 ? 6 : 7};
    }
    if (s.menu) {
        switch (icon) {
        case kCross: return ActionSpec(s.confirm == kPadCross ? kBindConfirm : kBindBack);
        case kCircle: return ActionSpec(s.confirm == kPadCircle ? kBindConfirm : kBindBack);
        case kDpadUp: return Spec{Spec::Key, {"Up"}};
        case kDpadDown: return Spec{Spec::Key, {"Down"}};
        case kDpadLeftRight: return Spec{Spec::Keys, {"Left", "Right"}};
        case kDpadUpDown: return Spec{Spec::Keys, {"Up", "Down"}};
        case kDpad: return Spec{Spec::Keys, {"Up", "Left", "Down", "Right"}};
        default: break;
        }
    }
    switch (icon) {
    case kCross: return ActionSpec(kBindInteract);
    case kCircle: return ActionSpec(kBindRoll);
    case kTriangle: return ActionSpec(kBindBloodVial);
    case kSquare: return ActionSpec(kBindUseItem);
    case kR1: return ActionSpec(kBindAttack);
    case kR2: return ActionSpec(kBindStrong);
    case kL1: return ActionSpec(kBindTransform);
    case kL2: return ActionSpec(kBindFirearm);
    case kR3: return ActionSpec(kBindLockOn);
    case kL3: return ActionSpec(kBindL3);
    case kOptions: return ActionSpec(kBindMenu);
    case kTouchLeft: return ActionSpec(kBindGestures);
    case kTouchRight: return ActionSpec(kBindEffects);
    case kDpadUp: return ActionSpec(kBindDpadUp);
    case kDpadDown: return ActionSpec(kBindSwitchItem);
    case kDpadLeftRight: return Caps({kBindSwitchLeft, kBindSwitchRight});
    case kDpadUpDown: return Caps({kBindDpadUp, kBindSwitchItem});
    case kDpad: return Caps({kBindDpadUp, kBindSwitchLeft, kBindSwitchItem, kBindSwitchRight});
    case kLeftStick: return Caps({kBindMoveF, kBindMoveL, kBindMoveB, kBindMoveR});
    case kRightStick:
        if (s.mouse_camera) {
            return Spec{Spec::Mouse, {}, 0};
        }
        return Caps({kBindLookU, kBindLookL, kBindLookD, kBindLookR});
    default: return std::nullopt;
    }
}

std::mutex g_mutex;
State g_state;
bool g_have_state = false;
std::atomic<bool> g_logged{false};

void SetState(const State& now) {
    static int shown = -1;
    if (!now.keys && shown != int(now.xbox)) {
        shown = int(now.xbox);
        host_log("button prompts: %s controller icons", now.xbox ? "Xbox" : "the game's (PlayStation)");
    }
    g_state = now;
    g_have_state = true;
}
auto& g_drawn = *new std::unordered_map<std::string, std::vector<std::uint8_t>>;

constexpr int kHud = kIconCount;
constexpr std::uint64_t kHudHash = 0x08d886c6323fa447ull;
constexpr int kHudBlockY = 30, kHudBlocksX = 7, kHudBlocksY = 8;
constexpr struct {
    Icon icon;
    int block_x;
    int left;
} kHudButtons[] = {{kCircle, 165, 1}, {kCross, 173, 2}, {kL3, 181, 2}};
constexpr int kHudTop = 3;

std::size_t HudBlock(int bx, int by) {
    const int tile = (by >> 3) * int(kHudWidth / 32) + (bx >> 3);
    int morton = 0;
    for (int k = 0; k < 3; ++k) {
        morton |= ((bx >> k) & 1) << (2 * k) | ((by >> k) & 1) << (2 * k + 1);
    }
    return std::size_t(tile * 64 + morton) * 16;
}

std::uint64_t HudHash(const std::uint8_t* data) {
    std::uint64_t h = 0xcbf29ce484222325ull;
    for (const auto& button : kHudButtons) {
        for (int by = kHudBlockY; by < kHudBlockY + kHudBlocksY; ++by) {
            for (int bx = button.block_x; bx < button.block_x + kHudBlocksX; ++bx) {
                const std::uint8_t* b = data + HudBlock(bx, by);
                for (int i = 0; i < 16; ++i) {
                    h = (h ^ b[i]) * 0x100000001b3ull;
                }
            }
        }
    }
    return h;
}

const std::vector<std::uint8_t>& Drawn(const Keycap::Spec& spec, int hud_left) {
    const std::string id = (hud_left < 0 ? std::string("kg|") : "hud" + std::to_string(hud_left) + '|') + spec.Id();
    auto it = g_drawn.find(id);
    if (it != g_drawn.end()) {
        return it->second;
    }
    std::vector<std::uint8_t> out;
    if (hud_left < 0) {
        std::uint8_t rgba[Keycap::kSize * Keycap::kSize * 4];
        Keycap::Draw(spec, rgba);
        out.resize(kIconBytes);
        Keycap::Encode(rgba, out.data());
    } else {
        constexpr int size = 26, w = kHudBlocksX * 4, h = kHudBlocksY * 4;
        std::uint8_t cap[size * size * 4];
        Keycap::Draw(spec, cap, size);
        std::vector<std::uint8_t> region(std::size_t(w) * h * 4, 0);
        for (int y = 0; y < size; ++y) {
            std::memcpy(&region[(std::size_t(y + kHudTop) * w + hud_left) * 4], &cap[y * size * 4], size * 4);
        }
        out.resize(std::size_t(kHudBlocksX) * kHudBlocksY * 16);
        Keycap::EncodeBlocks(region.data(), w, h, out.data());
    }
    return g_drawn.emplace(id, std::move(out)).first->second;
}

void LogFound() {
    if (!g_logged.exchange(true)) {
        const auto& settings = BbSettings::Get();
        host_log("button prompts: the game's button icons found (button_prompts = %s, pad_icons = %s)",
                 BbSettings::ButtonPromptsNames[std::clamp(settings.button_prompts.load(), 0, BbSettings::PromptsCount - 1)],
                 BbSettings::PadIconsNames[std::clamp(settings.pad_icons.load(), 0, BbSettings::PadIconsCount - 1)]);
    }
}

}

int Identify(const void* data, std::size_t size, std::uint32_t width, std::uint32_t height) {
    if (!data) {
        return -1;
    }
    if (size == kIconBytes && width == 32 && height == 32) {
        const std::uint64_t hash = Fnv(data, size);
        for (int icon = 0; icon < kIconCount; ++icon) {
            if (kHashes[icon] == hash) {
                LogFound();
                return icon;
            }
        }
    } else if (size == kHudBytes && width == kHudWidth && height == kHudHeight &&
               HudHash(static_cast<const std::uint8_t*>(data)) == kHudHash) {
        LogFound();
        return kHud;
    }
    return -1;
}

bool Draw(int texture, std::uint8_t* data, std::size_t size) {
    if (texture < 0 || texture > kHud || size != (texture == kHud ? kHudBytes : kIconBytes)) {
        return false;
    }
    std::scoped_lock lock{g_mutex};
    if (!g_have_state) {
        SetState(Current());
    }
    if (!g_state.keys && !g_state.xbox) {
        return false;
    }
    if (texture != kHud) {
        const auto spec = IconSpec(texture, g_state);
        if (!spec) {
            return false;
        }
        std::memcpy(data, Drawn(*spec, -1).data(), kIconBytes);
        return true;
    }
    bool drawn = false;
    for (const auto& button : kHudButtons) {
        const auto spec = IconSpec(button.icon, g_state);
        if (!spec) {
            continue;
        }
        const std::uint8_t* blocks = Drawn(*spec, button.left).data();
        for (int by = 0; by < kHudBlocksY; ++by) {
            for (int bx = 0; bx < kHudBlocksX; ++bx) {
                std::memcpy(data + HudBlock(button.block_x + bx, kHudBlockY + by), blocks, 16);
                blocks += 16;
            }
        }
        drawn = true;
    }
    return drawn;
}

void DecodeHud(const std::uint8_t* data, std::uint8_t* rgba) {
    std::uint32_t texels[16];
    for (int by = 0; by < int(kHudHeight / 4); ++by) {
        for (int bx = 0; bx < int(kHudWidth / 4); ++bx) {
            BbHudGauge::DecodeBc7Block(data + HudBlock(bx, by), texels);
            for (int y = 0; y < 4; ++y) {
                std::memcpy(rgba + (std::size_t(by * 4 + y) * kHudWidth + bx * 4) * 4, &texels[y * 4], 16);
            }
        }
    }
}

std::array<HudRect, 3> HudPrompts() {
    static_assert(std::size(kHudButtons) == 3);
    std::array<HudRect, 3> rects{};
    for (std::size_t i = 0; i < rects.size(); ++i) {
        rects[i] = {kHudButtons[i].block_x * 4, kHudBlockY * 4, kHudBlocksX * 4, kHudBlocksY * 4};
    }
    return rects;
}

std::uint64_t HudHashes(const std::uint8_t* data, std::array<std::uint64_t, 3>& prompts) {
    // The prompts' blocks, in the sheet's order, and each prompt's own hash.
    std::vector<std::size_t> masked;
    for (std::size_t i = 0; i < prompts.size(); ++i) {
        XXH3_state_t* state = XXH3_createState();
        XXH3_64bits_reset(state);
        for (int by = kHudBlockY; by < kHudBlockY + kHudBlocksY; ++by) {
            for (int bx = kHudButtons[i].block_x; bx < kHudButtons[i].block_x + kHudBlocksX; ++bx) {
                const std::size_t at = HudBlock(bx, by);
                XXH3_64bits_update(state, data + at, 16);
                masked.push_back(at);
            }
        }
        prompts[i] = XXH3_64bits_digest(state);
        XXH3_freeState(state);
    }
    // The sheet with those blocks zeroed, streamed.
    std::sort(masked.begin(), masked.end());
    static constexpr std::uint8_t kZero[16] = {};
    XXH3_state_t* state = XXH3_createState();
    XXH3_64bits_reset(state);
    std::size_t from = 0;
    for (const std::size_t at : masked) {
        XXH3_64bits_update(state, data + from, at - from);
        XXH3_64bits_update(state, kZero, 16);
        from = at + 16;
    }
    XXH3_64bits_update(state, data + from, kHudBytes - from);
    const std::uint64_t hash = XXH3_64bits_digest(state);
    XXH3_freeState(state);
    return hash;
}

bool Changed() {
    const State now = Current();
    std::scoped_lock lock{g_mutex};
    if (!g_have_state) {
        SetState(now);
        return false;
    }
    if (now == g_state) {
        return false;
    }
    SetState(now);
    return true;
}

}
