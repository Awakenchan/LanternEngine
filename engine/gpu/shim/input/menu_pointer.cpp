// SPDX-License-Identifier: GPL-3.0-or-later
#include "menu_pointer.h"
#include "../engine/hook_entries.h"
#include "input.h"
#include "pad.h"
#include "../bbport_settings.h"
#include "../engine/engine_hooks.h"
#include "../engine/engine_state.h"
#include "../engine/option_menu.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iterator>

namespace MenuPointer {
namespace {
using BbEngine::Call;

constexpr std::size_t kListUpdate = 0x1acff30;

constexpr const auto& kListUpdateEntry = BbEngine::HookEntries::kMenuPointerListUpdateEntry;
constexpr std::size_t kSetCursor = 0x1acf6b0, kScrollBy = 0x1ad0dd0, kHideCursors = 0x1acf0e0;
constexpr std::size_t kChildClip = 0x1adb220, kCursorClipName = 0x4934044;
constexpr std::size_t kStageMatrix = 0x2040;
constexpr std::size_t kCodeEnd = 0x50d96dc;

constexpr std::size_t kListener = 0x30, kItems = 0x70, kCursor = 0x74, kCursorOverride = 0x78;
constexpr std::size_t kCells = 0x7c, kLines = 0x80, kAlong = 0x84, kScrollLines = 0x24c;
constexpr std::size_t kRootType = 0xc8, kRootData = 0xd0, kProvider = 0x258, kWraps = 0x2b0;

constexpr std::size_t kValue = 0x30, kValueInterface = 0x40, kValueType = 0x48, kValueData = 0x50;
constexpr std::size_t kDisplayObject = 0x88, kParent = 0x38, kFlagsHigh = 0x6b, kBoundsSlot = 0x1f0;
constexpr std::uint8_t kVisible = 0x40;

constexpr std::size_t kMenuFlow = 0x5580e38, kFlowBlocked = 0xa78, kFlowMenu = 0xa88, kFlowStarted = 0xb11;
constexpr std::size_t kInputBlocker = 0x5562878, kBlockerBusy = 0x2e4;

constexpr std::size_t kRegion = 0x5128948;
constexpr int kRegionCircleDecides = 5;

constexpr std::size_t kMenuSound = 0x557fc60, kMenuSoundLabel = 0x557fc68, kCursorMoveLabel = 0x49344e4;

std::uint8_t* g_image = nullptr;
bool g_log = false;

std::atomic<std::int64_t> g_focus_ms{-100000};
std::atomic<std::int64_t> g_choice_ms{-100000};
std::atomic<bool> g_holding_row{false};
std::atomic<bool> g_click{false};
std::atomic<std::int64_t> g_click_ms{0};
std::atomic<std::uint32_t> g_press{0};
constexpr std::int64_t kClickMs = 250, kInMenuMs = 400, kNavigatingMs = 70;

std::int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

std::uint64_t Image(std::size_t offset) { return reinterpret_cast<std::uint64_t>(g_image) + offset; }

template <typename T> bool Read(std::uint64_t at, T& out) {
    return at >= 0x10000 && BbEngine::host_read_safe(reinterpret_cast<const void*>(at), &out, sizeof out);
}
template <typename T> T Field(const std::uint8_t* object, std::size_t offset) {
    T v;
    std::memcpy(&v, object + offset, sizeof v);
    return v;
}

bool GameFunction(std::uint64_t at) { return g_image && at >= Image(0) && at < Image(kCodeEnd); }
std::uint64_t VirtualFunction(std::uint64_t object, std::size_t slot) {
    std::uint64_t vtable = 0, fn = 0;
    return object && Read(object, vtable) && Read(vtable + slot, fn) && GameFunction(fn) ? fn : 0;
}

class Wrapped {
public:
    Wrapped() = default;
    Wrapped(const Wrapped&) = delete;
    Wrapped& operator=(const Wrapped&) = delete;
    ~Wrapped() {
        if (!(Field<std::uint32_t>(bytes_, kValueType) & 0x40)) return;
        const auto interface = Field<std::uint64_t>(bytes_, kValueInterface);
        if (const std::uint64_t release = VirtualFunction(interface, 0x18))
            Call(release, interface, bytes_ + kValue, Field<std::uint64_t>(bytes_, kValueData));
    }
    std::uint8_t* Bytes() { return bytes_; }

    std::uint64_t Object() const {
        std::uint64_t object = 0;
        const auto data = Field<std::uint64_t>(bytes_, kValueData);
        return (Field<std::uint32_t>(bytes_, kValueType) & 0x8f) == 10 && Read(data + kDisplayObject, object) ? object : 0;
    }

private:
    alignas(16) std::uint8_t bytes_[0x80]{};
};

bool ItemAt(std::uint8_t* list, std::uint64_t cell, Wrapped& out) {
    const auto provider = Field<std::uint64_t>(list, kProvider);
    const std::uint64_t make = VirtualFunction(provider, 0x18);
    if (!make) return false;
    Call(make, out.Bytes(), provider, &cell);
    return out.Object() != 0;
}
void Child(Wrapped& parent, const char* path, Wrapped& out) { Call(Image(kChildClip), out.Bytes(), parent.Bytes(), path); }
void CursorClip(Wrapped& item, Wrapped& out) {
    Call(Image(kChildClip), out.Bytes(), item.Bytes(), Image(kCursorClipName));
}

bool Shown(std::uint64_t object) {
    for (int depth = 0; object && depth < 32; ++depth) {
        std::uint8_t flags = 0;
        if (!Read(object + kFlagsHigh, flags) || !(flags & kVisible) || !Read(object + kParent, object)) return false;
    }
    return true;
}

struct Rect {
    float left = 0, top = 0, right = 0, bottom = 0;
    float CentreX() const { return (left + right) / 2; }
    float CentreY() const { return (top + bottom) / 2; }
};

bool Contains(std::uint64_t object, float x, float y, float grow, Rect* on_stage) {
    const std::uint64_t bounds_fn = VirtualFunction(object, kBoundsSlot);
    if (!bounds_fn) return false;
    alignas(16) float identity[8] = {1, 0, 0, 0, 0, 1, 0, 0};
    alignas(16) float b[4] = {};
    Call(bounds_fn, b, object, identity);
    if (!(b[2] > b[0] && b[3] > b[1])) return false;
    alignas(16) float m[8] = {1, 0, 0, 0, 0, 1, 0, 0};
    Call(Image(kStageMatrix), object, m);
    if (on_stage) {

        Rect r{1e9f, 1e9f, -1e9f, -1e9f};
        for (const float bx : {b[0], b[2]}) {
            for (const float by : {b[1], b[3]}) {
                const float sx = (m[0] * bx + m[1] * by + m[3]) / 20, sy = (m[4] * bx + m[5] * by + m[7]) / 20;
                r = {std::min(r.left, sx), std::min(r.top, sy), std::max(r.right, sx), std::max(r.bottom, sy)};
            }
        }
        *on_stage = r;
    }
    const float det = m[0] * m[5] - m[1] * m[4];
    if (std::fabs(det) < 1e-12f) return false;

    const float px = x * 20 - m[3], py = y * 20 - m[7];
    const float lx = (m[5] * px - m[1] * py) / det, ly = (m[0] * py - m[4] * px) / det;
    const float g = grow * 20 / std::sqrt(std::fabs(det));
    return lx >= b[0] - g && lx <= b[2] + g && ly >= b[1] - g && ly <= b[3] + g;
}

int CursorOf(const std::uint8_t* list) {
    const auto over = Field<std::int32_t>(list, kCursorOverride);
    return over >= 0 ? over : Field<std::int32_t>(list, kCursor);
}

struct Pointed {
    int index = -1;
    std::uint64_t cell = 0;
};

Pointed ItemUnder(std::uint8_t* list, float x, float y, bool log) {
    const int items = Field<std::int32_t>(list, kItems), cells = Field<std::int32_t>(list, kCells);
    const int lines = Field<std::int32_t>(list, kLines);
    if (items <= 0 || cells <= 0 || lines <= 0 || cells > 64 || lines > 64) return {};
    const int along = Field<std::int32_t>(list, kAlong), scrolled = Field<std::int32_t>(list, kScrollLines);
    const bool wraps = Field<std::uint8_t>(list, kWraps) != 0;

    const int line_length = lines == 1 ? std::max({cells, items, 1}) : cells;
    Pointed best;
    float best_distance = 0;
    for (int line = 0; line < lines; ++line) {
        for (int cell = 0; cell < cells; ++cell) {
            int index = (scrolled + line) * line_length + along + cell;
            if (wraps) index %= items;
            if (index < 0 || index >= items) continue;
            const std::uint64_t packed = std::uint32_t(cell) | std::uint64_t(std::uint32_t(line)) << 32;
            Wrapped item;
            if (!ItemAt(list, packed, item) || !Shown(item.Object())) continue;
            Wrapped highlight;
            CursorClip(item, highlight);
            const std::uint64_t target = highlight.Object() ? highlight.Object() : item.Object();
            Rect where;
            bool on = Contains(target, x, y, 0, &where);
            for (const char* widget : {"Widgets/ComboBox", "Widgets/Slider", "Slider"}) {
                if (on) break;
                Wrapped part;
                Child(item, widget, part);
                Rect widget_where;
                if (part.Object() && Shown(part.Object()) && Contains(part.Object(), x, y, 0, &widget_where)) {
                    on = true;
                    where = widget_where;
                }
            }
            if (log)
                host_log("pointer:   item %d (cell %d, line %d) %s at %.0f,%.0f-%.0f,%.0f%s", index, cell, line,
                         target == item.Object() ? "itself" : "its Cursor", where.left, where.top, where.right,
                         where.bottom, on ? "  <- pointer" : "");
            if (!on) continue;
            const float dx = where.CentreX() - x, dy = where.CentreY() - y, distance = dx * dx + dy * dy;
            if (best.index < 0 || distance < best_distance) {
                best = {index, packed};
                best_distance = distance;
            }
        }
    }
    return best;
}

std::uint32_t ClickMeaning(std::uint8_t* list, const Pointed& p, float x, float y) {
    const std::uint32_t decide = menu_confirm_button();
    if (BbEngine::Options::ClickDecides(list, p.index)) return decide;
    Wrapped item;
    if (!ItemAt(list, p.cell, item)) return decide;
    const auto on = [&](const char* path, float grow, Rect* where) {
        Wrapped part;
        Child(item, path, part);
        const bool shown = part.Object() && Shown(part.Object());
        const bool hit = shown && Contains(part.Object(), x, y, grow, where);
        if (g_log && part.Object())
            host_log("pointer:   %s %s at %.0f,%.0f-%.0f,%.0f%s", path, shown ? "shown" : "hidden", where->left,
                     where->top, where->right, where->bottom, hit ? "  <- click" : "");
        return hit;
    };
    Rect where;
    if (on("Widgets/Slider/LeftArrow", 14, &where)) return PadBits::kLeft;
    if (on("Widgets/Slider/RightArrow", 14, &where)) return PadBits::kRight;
    if (on("Widgets/ComboBox", 0, &where)) return x < where.CentreX() ? PadBits::kLeft : PadBits::kRight;
    if (on("Slider", 0, &where)) return x < where.CentreX() ? PadBits::kLeft : PadBits::kRight;
    return decide;
}

void ShowCursorOf(std::uint8_t* list, std::uint64_t cell) {
    Wrapped item;
    if (!ItemAt(list, cell, item)) return;
    Wrapped highlight;
    CursorClip(item, highlight);
    if (!highlight.Object()) return;
    if (const std::uint64_t set_visible = VirtualFunction(reinterpret_cast<std::uint64_t>(highlight.Bytes()), 8))
        Call(set_visible, highlight.Bytes(), 1);
}

void CursorMoved(std::uint8_t* list, int index) {
    const auto listener = Field<std::uint64_t>(list, kListener);
    if (const std::uint64_t told = VirtualFunction(listener, 0x10)) {
        std::int32_t at = index;
        Call(told, listener, &at);
    }
    auto* sound = reinterpret_cast<std::int32_t*>(Image(kMenuSound));
    if (sound[1] <= 0) {
        sound[0] = 0;
        sound[1] = 0;
        const std::uint64_t label = Image(kCursorMoveLabel);
        std::memcpy(reinterpret_cast<void*>(Image(kMenuSoundLabel)), &label, sizeof label);
    }
}

std::uint64_t Flow() {
    std::uint64_t flow = 0;
    return g_image && Read(Image(kMenuFlow), flow) ? flow : 0;
}

bool MenuOpen() {
    std::uint64_t menu = 0;
    const std::uint64_t flow = Flow();
    return flow && Read(flow + kFlowMenu, menu) && menu;
}

bool InputBlocked() {
    std::uint64_t blocked = 0, blocker = 0;
    std::uint32_t busy = 0;
    const std::uint64_t flow = Flow();
    if (flow && Read(flow + kFlowBlocked, blocked) && blocked) return true;
    return g_image && Read(Image(kInputBlocker), blocker) && blocker && Read(blocker + kBlockerBusy, busy) && busy;
}

bool RegionDecidesWithCircle() {
    std::int32_t region = -1;
    return g_image && Read(Image(kRegion), region) && region == kRegionCircleDecides;
}

bool InMenu() {
    return FrontEnd() || MenuOpen() || NowMs() - g_focus_ms.load(std::memory_order_relaxed) < kInMenuMs;
}

bool DirectionHeld() {
    const PadState pad = BbInput::LastPad();
    if (pad.buttons & PadBits::kDpad) return true;
    if (BbSettings::Get().menu_move && !FrontEnd()) return false;
    const auto pushed = [](std::uint8_t v) { return v < 64 || v > 192; };
    return pushed(pad.lx) || pushed(pad.ly);
}

struct Seen {
    const std::uint8_t* list = nullptr;
    float x = 0, y = 0;
    std::int64_t ms = 0;
    bool described = false;
};
Seen g_seen[32];
unsigned g_seen_next = 0;
constexpr std::int64_t kSeenMs = 250;

Seen& SeenBy(const std::uint8_t* list, bool& fresh) {
    const std::int64_t now = NowMs();
    for (Seen& s : g_seen) {
        if (s.list != list) continue;
        fresh = now - s.ms > kSeenMs;
        if (fresh) s.described = false;
        s.ms = now;
        return s;
    }
    Seen& s = g_seen[g_seen_next++ % std::size(g_seen)];
    s = {list, 0, 0, now, false};
    fresh = true;
    return s;
}

bool ClickWaiting() {
    if (!g_click.load(std::memory_order_relaxed)) return false;
    if (NowMs() - g_click_ms.load(std::memory_order_relaxed) <= kClickMs) return true;
    g_click.store(false, std::memory_order_relaxed);
    return false;
}

void SpinnerClick(std::uint8_t* list, float x, float y) {
    std::uint64_t root = 0;
    const auto data = Field<std::uint64_t>(list, kRootData);
    if ((Field<std::uint32_t>(list, kRootType) & 0x8f) != 10 || !Read(data + kDisplayObject, root) || !root) return;
    Rect where;
    const bool on = Shown(root) && Contains(root, x, y, 0, &where);
    if (g_log)
        host_log("pointer: spinner %p at %.0f,%.0f-%.0f,%.0f%s", static_cast<void*>(list), where.left, where.top,
                 where.right, where.bottom, on ? "  <- click" : "");
    if (on && g_click.exchange(false))
        g_press.store(x < where.CentreX() ? PadBits::kLeft : PadBits::kRight, std::memory_order_relaxed);
}

bool OnScreen(const std::uint8_t* list) {
    std::uint64_t root = 0;
    const auto data = Field<std::uint64_t>(list, kRootData);
    if ((Field<std::uint32_t>(list, kRootType) & 0x8f) != 10 || !Read(data + kDisplayObject, root) || !root) return true;
    return Shown(root);
}

std::atomic<const std::uint8_t*> g_choice_list{nullptr};

void PointerStep(std::uint8_t* list, const std::uint8_t* focus) {
    if (!focus || !*focus || InputBlocked()) return;
    const std::int64_t focused_at = NowMs();
    g_focus_ms.store(focused_at, std::memory_order_relaxed);
    if (Field<std::int32_t>(list, kItems) > 1 && OnScreen(list)) {
        g_choice_ms.store(focused_at, std::memory_order_relaxed);
        g_choice_list.store(list, std::memory_order_relaxed);
    }
    if (g_holding_row.load(std::memory_order_relaxed)) return;
    float x, y;
    if (!BbInput::MouseIsMenuPointer() || !BbInput::PointerOnStage(x, y)) return;
    bool fresh;
    Seen& seen = SeenBy(list, fresh);
    const bool moved = !fresh && (x != seen.x || y != seen.y);
    seen.x = x;
    seen.y = y;

    if (BbInput::TextEntryOpen()) return;
    if (g_log && !seen.described) {
        seen.described = true;
        host_log("pointer: list %p: %d items, %d cells x %d lines, cursor %d, scrolled %d/%d", static_cast<void*>(list),
                 Field<std::int32_t>(list, kItems), Field<std::int32_t>(list, kCells), Field<std::int32_t>(list, kLines),
                 CursorOf(list), Field<std::int32_t>(list, kScrollLines), Field<std::int32_t>(list, kAlong));
        ItemUnder(list, x, y, true);
    }
    const bool click = ClickWaiting();
    if (Field<std::int32_t>(list, kLines) == 1 && Field<std::int32_t>(list, kCells) == 1 &&
        Field<std::int32_t>(list, kItems) > 1) {
        if (click) SpinnerClick(list, x, y);
        return;
    }
    if (!moved && !click) return;
    if (DirectionHeld()) {
        static int told = 0;
        if (g_log && told++ < 60) host_log("pointer: list %p: not hovered, a direction is held", static_cast<void*>(list));
        return;
    }
    const Pointed p = ItemUnder(list, x, y, false);
    if (p.index < 0) return;
    if (click) g_click.store(false, std::memory_order_relaxed);
    const int was = CursorOf(list);
    int now = was;
    if (p.index != was) {

        const int along = Field<std::int32_t>(list, kAlong), lines = Field<std::int32_t>(list, kScrollLines);
        const auto answer = std::int32_t(Call(Image(kSetCursor), list, p.index));
        const int back_along = along - Field<std::int32_t>(list, kAlong);
        const int back_lines = lines - Field<std::int32_t>(list, kScrollLines);
        if (answer == 1 && (back_along || back_lines)) {
            Call(Image(kScrollBy), list, back_along, back_lines);

            Call(Image(kHideCursors), list);
            ShowCursorOf(list, p.cell);
        }
        now = CursorOf(list);
        if (g_log) host_log("pointer: list %p hover %d -> %d (SetCursor %d)", static_cast<void*>(list), was, now, answer);
    }

    if (click && now == p.index) {
        const std::uint32_t press = ClickMeaning(list, p, x, y);
        g_press.store(press, std::memory_order_relaxed);
        if (g_log) host_log("pointer: list %p click on item %d -> pad %04x", static_cast<void*>(list), p.index, press);
    }
    if (now != was) CursorMoved(list, now);
}

std::int64_t __attribute__((sysv_abi)) OnListUpdate(std::uint64_t, std::uint64_t* saved) noexcept {
    auto* list = reinterpret_cast<std::uint8_t*>(saved[5]);
    const auto* focus = reinterpret_cast<const std::uint8_t*>(saved[4]);
    PointerStep(list, focus);
    BbEngine::Options::ListUpdate(list, focus && *focus);
    return 0;
}
}

bool Install(std::uint8_t* image, std::size_t size) {
    g_image = image;
    const char* log = std::getenv("BB_POINTER_LOG");
    g_log = log && log[0] == '1';
    BbEngine::Hook hook;
    if (!BbEngine::Prepare(hook, image, size, kListUpdate, BbEngine::SiteKind::Entry, kListUpdateEntry, OnListUpdate)) {
        host_log("pointer: off, the menus' list update is not 1.09's");
        return false;
    }
    if (const BbEngine::WriteResult result = BbEngine::Commit(std::span(&hook, 1)); result != BbEngine::WriteResult::Applied) {
        host_log("pointer: off, the menus' list update could not be written (%s)", BbEngine::Describe(result));
        return false;
    }
    host_log("pointer: the menus' lists follow the mouse");
    return true;
}

bool FrontEnd() {
    std::uint8_t started = 1;
    const std::uint64_t flow = Flow();
    return flow && Read(flow + kFlowStarted, started) && !started;
}

bool Navigating() {
    const bool front = FrontEnd(), open = !front && MenuOpen();
    const bool choice = !front && !open && NowMs() - g_choice_ms.load(std::memory_order_relaxed) < kNavigatingMs;
    const int why = front ? 1 : open ? 2 : choice ? 3 : 0;
    static int last = -1, told = 0;
    if (why != last && told < 40) {
        ++told;
        static const char* const kWhy[] = {"off", "the front end", "a menu the game opened", "a list with a choice"};
        if (why == 3) host_log("menu keys: %s (%p)", kWhy[why], static_cast<const void*>(g_choice_list.load(std::memory_order_relaxed)));
        else host_log("menu keys: %s", kWhy[why]);
    }
    last = why;
    return why != 0;
}

void HoldRow(bool hold) { g_holding_row.store(hold, std::memory_order_relaxed); }

void Click() {
    g_click_ms.store(NowMs(), std::memory_order_relaxed);
    g_click.store(true, std::memory_order_relaxed);
}

std::uint32_t TakePress() { return g_press.exchange(0); }
}

std::uint32_t menu_confirm_button() {
    return MenuPointer::RegionDecidesWithCircle() ? PadBits::kCircle : PadBits::kCross;
}

std::uint32_t menu_back_button() { return menu_confirm_button() == PadBits::kCircle ? PadBits::kCross : PadBits::kCircle; }

bool menu_pointer_in_menu() { return MenuPointer::InMenu(); }
