// SPDX-License-Identifier: GPL-3.0-or-later
#include "option_keys.h"
#include "option_rows.h"
#include "engine_state.h"
#include "../input/bindings.h"
#include <atomic>
#include <chrono>
#include <cstring>
#include <string>

extern bool debug_menu_active();
extern std::uint32_t menu_confirm_button();
#include "../input/input.h"

namespace BbEngine::OptionKeys {
namespace {
using Clock = std::chrono::steady_clock;

constexpr std::uint32_t kPageRow = 119000, kFirstActionRow = 119001;
constexpr std::uint32_t kActionText = 119100, kPageText = 119200;
constexpr std::uint32_t kResetArmed = 119210, kResetDone = 119211, kResetIdle = 119212;
constexpr std::uint32_t kAwaitKey = 119213, kUnbound = 119214, kHowTo = 119215, kResetCaption = 119216;
static_assert(kBindCount == 33 && kBindPages == 5 && kBindPerPage == 7,
              "pc_option_messages.tsv has a caption for each action and page");
static_assert(kBindDebugMenu == kBindCount - 1);
constexpr const char* kPageNames[kBindPages] = {"Movement", "Combat", "Items and Gestures", "Camera and Menus", "Other"};
constexpr int kRows = 1 + kBindPerPage;
constexpr std::uint32_t kPadRight = 0x20, kPadLeft = 0x80;
constexpr auto kResetWindow = std::chrono::seconds(4);

struct Line {
    char16_t label[48];
    char16_t help[160];
    char16_t value[64];
    std::uint8_t choice;
};
Line g_lines[kRows];

struct Screen {
    std::uint8_t* dialog = nullptr;
    std::uint8_t* rows = nullptr;
    std::uint8_t* widgets[kRows] = {};
    std::uint32_t buttons = 0;
};
Screen g_screen;
int g_page = 0;
enum class Reset { Idle, Armed, Done };
Reset g_reset = Reset::Idle;
Clock::time_point g_reset_at;

template <std::size_t N> void Copy(char16_t (&out)[N], const char16_t* text) {
    std::size_t i = 0;
    for (; text && text[i] && i + 1 < N; ++i) out[i] = text[i];
    out[i] = 0;
}
template <std::size_t N> void Copy(char16_t (&out)[N], const char* ascii) {
    std::size_t i = 0;
    for (; ascii && ascii[i] && i + 1 < N; ++i) out[i] = char16_t(static_cast<unsigned char>(ascii[i]));
    out[i] = 0;
}
template <std::size_t N>
void Say(char16_t (&out)[N], std::uint32_t id, const char* english, std::uint32_t group = OptionRows::kMenuText) {
    if (const char16_t* text = OptionRows::Message(id, group)) Copy(out, text);
    else Copy(out, english);
}

int Listed() { return debug_menu_active() ? kBindCount : kBindDebugMenu; }
int ActionOf(int row) { return g_page * kBindPerPage + row - 1; }
bool IsAction(int row) { return row > 0 && row < kRows && ActionOf(row) < Listed(); }
bool IsReset(int row) { return row > 0 && row < kRows && ActionOf(row) == Listed(); }

void FillReset(int row) {
    switch (g_reset) {
    case Reset::Armed: Say(g_lines[row].value, kResetArmed, "Press again"); break;
    case Reset::Done: Say(g_lines[row].value, kResetDone, "Restored"); break;
    case Reset::Idle: Say(g_lines[row].value, kResetIdle, "All actions"); break;
    }
}

void FillValue(int row) {
    if (IsReset(row)) return FillReset(row);
    if (!IsAction(row)) {
        g_lines[row].value[0] = 0;
        return;
    }
    char key[64];
    host_binding_describe(ActionOf(row), key, sizeof key);
    if (std::strcmp(key, "Unbound") == 0) Say(g_lines[row].value, kUnbound, key);
    else Copy(g_lines[row].value, key);
}

void Fill(int page) {
    g_page = (page % kBindPages + kBindPages) % kBindPages;
    Say(g_lines[0].label, kPageRow, "Page");
    Say(g_lines[0].help, kPageRow, "Left and right change the group of actions shown.", OptionRows::kLineHelp);
    Say(g_lines[0].value, kPageText + g_page, kPageNames[g_page]);
    for (int r = 1; r < kRows; ++r) {
        Line& line = g_lines[r];
        if (IsReset(r)) {
            Say(line.label, kResetCaption, "Restore Defaults");
            Say(line.help, kResetCaption,
                "Every action back to its default key and mouse button. Enter or click, then again to confirm.",
                OptionRows::kLineHelp);
        } else if (IsAction(r)) {
            const BindingInfo& b = host_binding_info(ActionOf(r));
            Say(line.label, kActionText + ActionOf(r), b.label);
            char16_t help[160], how[96];
            Say(help, kActionText + ActionOf(r), b.help, OptionRows::kLineHelp);
            Say(how, kHowTo, " Enter or click to rebind, Delete to clear.");
            Copy(line.help, (std::u16string(help) + how).c_str());
        } else {
            line.label[0] = line.help[0] = 0;
        }
        FillValue(r);
    }
}

bool Alive() {
    if (!g_screen.dialog || !OptionRows::IsList(g_screen.rows)) return false;
    for (int r = 0; r < kRows; ++r)
        if (!OptionRows::WritesTo(g_screen.widgets[r], &g_lines[r].choice)) return false;
    return true;
}

void RedrawValues() {
    for (int r = 0; r < kRows; ++r) OptionRows::Redraw(OptionRows::ValueList(g_screen.widgets[r]));
}

void Step(int row, bool focused) {
    bool page_turned = false, values = false;
    if (g_reset != Reset::Idle &&
        ((focused && row >= 0 && !IsReset(row)) || Clock::now() - g_reset_at > kResetWindow)) {
        g_reset = Reset::Idle;
        for (int r = 1; r < kRows; ++r) {
            if (!IsReset(r)) continue;
            FillReset(r);
            OptionRows::Redraw(OptionRows::ValueList(g_screen.widgets[r]));
        }
    }
    if (host_bind_capture_take_done() >= 0) {
        for (int r = 1; r < kRows; ++r) FillValue(r);
        values = true;
    }
    if (focused && row >= 0 && host_bind_capturing() < 0) {
        const std::uint32_t buttons = BbInput::LastPad().buttons;
        const std::uint32_t pressed = buttons & ~g_screen.buttons;
        g_screen.buttons = buttons;
        const std::uint32_t confirm = menu_confirm_button();
        if (row == 0 && (pressed & (confirm | kPadRight | kPadLeft))) {
            Fill(g_page + ((pressed & kPadLeft) ? -1 : 1));
            page_turned = values = true;
        } else if (IsAction(row) && host_bind_take_clear_key()) {
            host_binding_clear(ActionOf(row));
            FillValue(row);
            OptionRows::Redraw(OptionRows::ValueList(g_screen.widgets[row]));
        } else if (IsAction(row) && (pressed & confirm)) {
            Say(g_lines[row].value, kAwaitKey, "Press a key...");
            OptionRows::Redraw(OptionRows::ValueList(g_screen.widgets[row]));
            host_bind_capture_begin(ActionOf(row));
        } else if (IsReset(row) && (pressed & confirm)) {
            if (g_reset == Reset::Armed) {
                host_bindings_load_defaults();
                g_reset = Reset::Done;
                for (int r = 1; r < kRows; ++r) FillValue(r);
                host_log("pc-options: key bindings restored to their defaults");
            } else {
                g_reset = Reset::Armed;
            }
            g_reset_at = Clock::now();
            FillReset(row);
            values = true;
        }
    }
    if (page_turned) OptionRows::Redraw(g_screen.rows);
    if (values) RedrawValues();
}
}

void Build(void* dialog, void*) {
    g_screen = Screen{};
    g_screen.buttons = BbInput::LastPad().buttons;
    Fill(g_page);
    for (int r = 0; r < kRows; ++r)
        OptionRows::AddText(dialog, &g_lines[r].choice, r ? kFirstActionRow + r - 1 : kPageRow, g_lines[r].label,
                            g_lines[r].help, g_lines[r].value);
    auto* d = static_cast<std::uint8_t*>(dialog);
    const int took = OptionRows::RowCount(d);
    if (took < kRows) {
        host_log("pc-options: PCKeys: %d of %d rows taken; the bindings show but cannot change here", took, kRows);
        return;
    }
    g_screen.rows = OptionRows::RowsList(d);
    for (int r = 0; r < kRows; ++r) g_screen.widgets[r] = OptionRows::RowWidget(d, r);
    g_screen.dialog = d;
    if (!Alive()) {
        host_log("pc-options: PCKeys: the dialog is not laid out as 1.09's; the bindings show but cannot change here");
        g_screen = Screen{};
    }
}

void Update(std::uint8_t* list, bool focused) {
    if (!g_screen.dialog) return;
    int row = -2;
    if (list == g_screen.rows) row = -1;
    for (int r = 0; r < kRows && row == -2; ++r)
        if (list == OptionRows::ValueList(g_screen.widgets[r])) row = r;
    if (row == -2) return;
    if (!Alive()) {
        g_screen = Screen{};
        return;
    }
    Step(row, focused);
}

bool ClickDecides(const std::uint8_t* list, int row) {
    return g_screen.dialog && list == g_screen.rows && (IsAction(row) || IsReset(row)) && Alive();
}
}
