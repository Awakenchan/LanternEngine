// SPDX-License-Identifier: GPL-3.0-or-later
#include <array>
#include <cstdio>
#include <cstring>

#include "gpu/shim/engine/reg_window.h"

namespace {
int failures = 0;

void Expect(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        ++failures;
    }
}

constexpr std::uint32_t kComputeWindowDwords = 0x50;
constexpr std::uint32_t kComputeUserData15 = 0x4f;
}

int main() {
    using BbEngine::RegWindow::Clamp;
    static_assert(Clamp(kComputeWindowDwords, 0, 4).dwords == 4 && !Clamp(kComputeWindowDwords, 0, 4).clamped);

    const auto user_data_15 = Clamp(kComputeWindowDwords, kComputeUserData15, 4);
    Expect(user_data_15.dwords == 1 && user_data_15.clamped, "4 dwords at USER_DATA_15 keep the one that fits");
    const auto last_fit = Clamp(kComputeWindowDwords, kComputeUserData15 - 3, 4);
    Expect(last_fit.dwords == 4 && !last_fit.clamped, "4 dwords ending at the window's last dword fit");
    const auto at_end = Clamp(kComputeWindowDwords, kComputeWindowDwords, 2);
    Expect(at_end.dwords == 0 && at_end.clamped, "a write starting one past the window copies nothing");
    const auto empty = Clamp(kComputeWindowDwords, kComputeWindowDwords, 0);
    Expect(empty.dwords == 0 && !empty.clamped, "an empty write past the window is not a clamp");
    const auto whole = Clamp(kComputeWindowDwords, 0, kComputeWindowDwords);
    Expect(whole.dwords == kComputeWindowDwords && !whole.clamped, "the whole window");
    const auto over = Clamp(kComputeWindowDwords, 0, 0xffffffffu);
    Expect(over.dwords == kComputeWindowDwords && over.clamped, "a huge count stays inside");

    struct Guarded {
        std::array<std::uint32_t, kComputeWindowDwords> window{};
        std::array<std::uint32_t, 4> after{};
    } g;
    const std::uint32_t values[4] = {1, 2, 3, 4};
    const auto copy = Clamp(kComputeWindowDwords, kComputeUserData15, 4);
    std::memcpy(g.window.data() + kComputeUserData15, values, copy.dwords * sizeof(std::uint32_t));
    Expect(g.window[kComputeUserData15] == 1 && g.after == std::array<std::uint32_t, 4>{}, "nothing written past the window");

    if (failures) {
        std::printf("reg window: %d failures\n", failures);
        return 1;
    }
    std::printf("reg window: all tests passed\n");
    return 0;
}
