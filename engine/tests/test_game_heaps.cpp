// SPDX-License-Identifier: GPL-3.0-or-later
#include <cstdio>

#include "start/game_heaps.h"

namespace {
using namespace start::game_heaps;

constexpr std::uint64_t kGxDefault = 128 * kMiB;
constexpr std::uint64_t kGxMax = 256 * kMiB;

int Failures() {
    int failures = 0;
    const auto expect = [&](bool ok, const char* what) {
        if (!ok) {
            std::printf("FAIL: %s\n", what);
            ++failures;
        }
    };
    expect(SunShadowQuality(0) == 0 && SunShadowQuality(1) == 1 && SunShadowQuality(2) == 2, "qualities 0 to 2 kept");
    expect(SunShadowQuality(9) == 2, "a quality above 2 is 2, as the engine clamps it");
    expect(SunShadowQuality(-3) == 0, "a negative quality is the game's own");
    expect(SunAtlasSide(0) == 2048 && SunAtlasSide(1) == 4096 && SunAtlasSide(2) == 8192,
           "atlas sides 2048, 4096 and 8192 as the engine writes them");
    expect(RoundUpToMiB(0) == 0 && RoundUpToMiB(kMiB) == kMiB, "whole MiB stay");
    expect(RoundUpToMiB(1) == kMiB && RoundUpToMiB(kMiB + 1) == 2 * kMiB, "a partial MiB rounds up");
    expect(SunShadowExtraBytes(0) == 0, "the game's own atlas needs no room");
    expect(SunShadowExtraBytes(-1) == 0, "a negative quality needs no room");
    expect(SunShadowExtraBytes(1) == 60 * kMiB, "4096 atlas: (4096^2 - 1024^2) x 4 B = 60 MiB");
    expect(SunShadowExtraBytes(2) == 252 * kMiB, "8192 atlas: (8192^2 - 1024^2) x 4 B = 252 MiB");
    expect(Plan(Base::Ps4, 0, 0) == kPs4, "nothing chosen keeps the PS4 heaps");
    expect(Plan(Base::Enlarged, 0, 0) == kEnlarged, "enlarged heaps stay 3952 and 2560 MiB");
    expect(Plan(Base::Ps4, kGxDefault, 0) == Sizes{kGraphicsPrivatePs4 + kGxDefault, kGraphicsPrivateBPs4},
           "the GX range adds to the first heap only");
    expect(Plan(Base::Ps4, kGxDefault, 252 * kMiB) == Sizes{kGraphicsPrivatePs4 + 380 * kMiB, kGraphicsPrivateBPs4},
           "PS4 heaps grow by the GX range and the sun atlas");
    expect(Plan(Base::Enlarged, kGxDefault, 60 * kMiB) ==
               Sizes{kGraphicsPrivateEnlarged + 60 * kMiB, kGraphicsPrivateBEnlarged},
           "enlarged heaps grow by the sun atlas and already hold the GX range");

    expect(DirectMemoryNeed(kPs4) == 4594 * kMiB, "PS4 heaps: 1068 + 2287 + 886 + 353 = 4594 MiB");
    expect(DirectMemoryNeed(Plan(Base::Ps4, kGxDefault, 252 * kMiB)) == 4974 * kMiB,
           "default GX range and 8192 atlas: 4974 MiB");
    expect(Fits(Plan(Base::Ps4, kGxDefault, 252 * kMiB), kPoolPs4Mib), "4974 MiB fits the 5056 MiB pool");
    expect(!Fits(Plan(Base::Ps4, kGxMax, 252 * kMiB), kPoolPs4Mib), "5102 MiB does not fit the 5056 MiB pool");
    expect(Fits(Sizes{kGraphicsPrivatePs4 + 462 * kMiB, kGraphicsPrivateBPs4}, kPoolPs4Mib),
           "exactly 5056 MiB fits");
    expect(PoolMib(false, 0, Base::Ps4) == 5056 && PoolMib(false, 0, Base::Enlarged) == 9152,
           "the start's pools when BB_DMEM_MB is unset");
    expect(PoolMib(true, 6000, Base::Ps4) == 6000 && PoolMib(true, 100, Base::Enlarged) == 5056 &&
               PoolMib(true, 20000, Base::Ps4) == 5056,
           "BB_DMEM_MB is taken as the runtime takes it");

    Decision d = Decide({});
    expect(d.outcome == Outcome::Sized && d.sizes == kPs4 && !d.write_private && !d.write_private_b && !d.heap_patch,
           "nothing chosen writes nothing");
    d = Decide({.gx_region_extra = kGxDefault});
    expect(d.write_private && !d.write_private_b && !d.heap_patch, "the GX range writes the first heap only");
    d = Decide({.enlarge_wanted = true});
    expect(d.base == Base::Enlarged && d.sizes == kEnlarged && d.write_private && d.write_private_b && d.heap_patch,
           "enlarged heaps write both words and ask for the large pool");
    d = Decide({.gx_region_extra = kGxDefault, .sun_quality = 2});
    expect(d.base == Base::Ps4 && d.sun_quality == 2 && !d.sun_room_reduced && d.fits && !d.heap_patch,
           "8192 atlas on the default GX range stays in the PS4 pool");
    d = Decide({.enlarge_allowed = true, .gx_region_extra = kGxMax, .sun_quality = 2});
    expect(d.base == Base::Enlarged && d.enlarged_for_room && d.sun_quality == 2 && d.fits && d.heap_patch,
           "8192 atlas on the largest GX range enlarges the heaps when the large pool is allowed");
    d = Decide({.gx_region_extra = kGxMax, .sun_quality = 2});
    expect(d.base == Base::Ps4 && !d.enlarged_for_room && d.sun_room_reduced && d.sun_quality == 1 && d.fits &&
               d.sizes == Plan(Base::Ps4, kGxMax, 60 * kMiB),
           "without the large pool the heaps hold the largest atlas that fits");
    d = Decide({.table_written_by_patch = true, .sun_quality = 2});
    expect(d.outcome == Outcome::PatchOwnsTable && !d.write_private && !d.write_private_b && d.heap_patch,
           "a patch that writes the heap table owns it");
    d = Decide({.table_is_ps4 = false, .enlarge_wanted = true, .sun_quality = 2});
    expect(d.outcome == Outcome::TableDiffers && !d.write_private && !d.heap_patch,
           "an executable with another table is left alone");
    return failures;
}
}

int main() {
#ifdef GAME_HEAPS_TRIALS
    int survived = 0;
    for (int trial = 1; trial <= kTrials; ++trial) {
        game_heaps_trial = trial;
        const int failures = Failures();
        std::printf("game heaps trial %d: %s (%d checks failed)\n", trial, failures ? "caught" : "SURVIVED", failures);
        if (!failures) ++survived;
    }
    game_heaps_trial = 0;
    const int shipped = Failures();
    std::printf("game heaps trials: %d of %d caught, shipped lines %s\n", kTrials - survived, kTrials,
                shipped ? "FAIL" : "pass");
    return survived || shipped ? 1 : 0;
#else
    const int failures = Failures();
    std::printf("game heaps: %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
#endif
}
