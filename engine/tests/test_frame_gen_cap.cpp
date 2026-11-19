// SPDX-License-Identifier: GPL-3.0-or-later
#include <cstdio>

#include "gpu/shim/lantern_frame_gen_cap.h"

namespace {
int failures = 0;

void Expect(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        ++failures;
    }
}

std::uint32_t Old(std::uint32_t base, bool active, int asked, std::uint32_t refresh, bool cap) {
    if (!active) {
        return base;
    }
    const std::uint32_t shown = std::uint32_t(std::clamp(asked, 2, 6));
    std::uint32_t limit = base / shown;
    const std::uint32_t refresh_share = refresh / shown;
    if (refresh_share >= 20 && cap) {
        limit = limit == 0 ? refresh_share : std::min(limit, refresh_share);
    }
    return limit == 0 ? base : limit;
}
}

int main() {
    using namespace LanternFrameGenCap;

    for (int asked = 2; asked <= 4; ++asked) {
        for (int delivered = 1; delivered <= asked; ++delivered) {
            const float game = 60.0f, shown = game * float(delivered);
            const int got = DeliveredMultiplier(asked, false, game, shown, 0);
            char what[96];
            std::snprintf(what, sizeof(what), "asked %dx, delivered %dx", asked, delivered);
            Expect(got == delivered, what);
            const std::uint32_t want = delivered == 1 ? 240u : 240u / std::uint32_t(delivered);
            std::snprintf(what, sizeof(what), "limit for asked %dx, delivered %dx", asked, delivered);
            Expect(Limit(240, true, got, 144, false) == want, what);
        }
    }

    Expect(DeliveredMultiplier(3, false, 60.0f, 240.0f, 0) == 3, "more than asked is clamped to asked");
    Expect(DeliveredMultiplier(4, false, 60.0f, 240.0f, 0) == 4, "4x delivered");
    Expect(DeliveredMultiplier(3, false, 0.0f, 0.0f, 0) == 3, "no measurement yet uses asked");
    Expect(DeliveredMultiplier(3, true, 60.0f, 120.0f, 0) == 3, "dynamic keeps asked");
    Expect(DeliveredMultiplier(3, false, 60.0f, 60.0f * 2.4f, 3) == 3, "hysteresis holds 3x at 2.4");
    Expect(DeliveredMultiplier(3, false, 60.0f, 60.0f * 2.2f, 3) == 2, "drops to 2x below 2.25");
    Expect(DeliveredMultiplier(3, false, 60.0f, 60.0f * 2.6f, 2) == 2, "hysteresis holds 2x at 2.6");
    Expect(DeliveredMultiplier(3, false, 60.0f, 60.0f * 2.8f, 2) == 3, "rises to 3x above 2.75");

    Expect(Limit(240, true, 2, 144, true) == 72, "2x capped to the refresh share");
    Expect(Limit(240, true, 3, 144, true) == 48, "3x capped to the refresh share");
    Expect(Limit(0, true, 2, 144, true) == 72, "no base limit takes the refresh share");
    Expect(Limit(0, true, 2, 144, false) == 0, "no limit stays none");

    for (std::uint32_t base : {0u, 30u, 60u, 120u, 144u, 240u}) {
        for (std::uint32_t refresh : {0u, 60u, 144u, 240u}) {
            for (bool cap : {false, true}) {
                Expect(Limit(base, false, 3, refresh, cap) == base, "frame generation off leaves the limit");
                Expect(Limit(base, false, 0, refresh, cap) == Old(base, false, 2, refresh, cap), "off matches the old limit");
                for (int asked = 2; asked <= 6; ++asked) {
                    Expect(Limit(base, true, asked, refresh, cap) == Old(base, true, asked, refresh, cap),
                           "full delivery matches the old limit");
                }
            }
        }
    }

    Expect(FixedRateLimit(90, true, 2, 90) == 45, "fixed 90 on 90 Hz with 2x: the game at 45, 90 shown");
    Expect(FixedRateLimit(120, true, 2, 144) == 72, "fixed 120 on 144 Hz with 2x: the game at 72");
    Expect(FixedRateLimit(60, true, 2, 144) == 60, "a fixed rate under the refresh share stays");
    Expect(FixedRateLimit(120, true, 3, 240) == 80, "3x on 240 Hz: 80");
    Expect(FixedRateLimit(120, true, 4, 60) == 120, "a share under 20 FPS is not used");
    Expect(FixedRateLimit(90, true, 1, 90) == 90, "nothing generated: the fixed rate");
    Expect(FixedRateLimit(90, false, 2, 90) == 90, "frame generation off: the fixed rate");
    Expect(FixedRateLimit(90, true, 2, 0) == 90, "unknown refresh: the fixed rate");
    for (std::uint32_t base : {30u, 45u, 60u, 90u, 120u, 144u}) {
        for (std::uint32_t refresh : {60u, 75u, 90u, 120u, 144u, 165u, 240u}) {
            for (int delivered = 2; delivered <= 4; ++delivered) {
                const std::uint32_t game = FixedRateLimit(base, true, delivered, refresh);
                const bool share_used = refresh / std::uint32_t(delivered) >= 20;
                Expect(game <= base, "never faster than the chosen rate");
                Expect(!share_used || game * std::uint32_t(delivered) <= refresh,
                       "shown frames never outrun the display");
            }
        }
    }

    if (failures) {
        std::printf("frame-gen-cap-test: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("frame-gen-cap-test: ok\n");
    return 0;
}
