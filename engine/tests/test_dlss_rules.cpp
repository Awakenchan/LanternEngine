// SPDX-License-Identifier: GPL-2.0-or-later
#include <cstdio>

#include "gpu/shim/dlss_rules.h"

namespace {

namespace D = BbDlss;

int g_failed = 0;

void Check(bool ok, [[maybe_unused]] const char* what) {
    if (!ok) {
        ++g_failed;
#ifndef LANTERN_DLSS_RULES_MUTANTS
        std::printf("dlss rules: FAILED %s\n", what);
#endif
    }
}

constexpr D::Quality kModes[] = {D::Dlaa, D::MaxQuality, D::Balanced, D::MaxPerf, D::UltraPerformance};

void TestSavedPresets() {
    Check(D::FromSaved(D::kPresetE) == D::kPresetAuto, "a saved E runs auto");
    Check(D::FromSaved(D::kPresetF) == D::kPresetAuto, "a saved F runs auto");
    for (const int kept : {D::kPresetAuto, D::kPresetDefault, D::kPresetJ, D::kPresetK, D::kPresetL, D::kPresetM}) {
        Check(D::FromSaved(kept) == kept, "a saved J, K, L, M, auto or default stays");
    }
    Check(D::Deprecated(D::kPresetE) && D::Deprecated(D::kPresetF), "E and F are deprecated");
    Check(!D::Deprecated(D::kPresetK) && !D::Deprecated(D::kPresetAuto), "K and auto are not");
    for (const int offered : {D::kPresetAuto, D::kPresetDefault, D::kPresetJ, D::kPresetK, D::kPresetL, D::kPresetM}) {
        Check(D::Offered(offered), "auto, default, J, K, L and M are offered");
    }
    for (const int gone : {1, 4, D::kPresetE, D::kPresetF, 7, 9, 14, 15, 26}) {
        Check(!D::Offered(gone), "A-D, E, F, G-I and N onwards are not offered");
    }
}

void TestGeneration() {
    Check(D::TuringOrAmpere(D::kNvidia, 0x1e04), "RTX 2080 Ti is Turing");
    Check(D::TuringOrAmpere(D::kNvidia, 0x1f08), "RTX 2060 is Turing");
    Check(D::TuringOrAmpere(D::kNvidia, 0x2204), "RTX 3090 is Ampere");
    Check(D::TuringOrAmpere(D::kNvidia, 0x2504), "RTX 3060 is Ampere");
    Check(D::TuringOrAmpere(D::kNvidia, 0x25a0), "RTX 3050 laptop is Ampere");
    Check(!D::TuringOrAmpere(D::kNvidia, 0x2600), "0x2600 starts Ada");
    Check(!D::TuringOrAmpere(D::kNvidia, 0x2684), "RTX 4090 is Ada");
    Check(!D::TuringOrAmpere(D::kNvidia, 0x2882), "RTX 4060 is Ada");
    Check(!D::TuringOrAmpere(D::kNvidia, 0x2d04), "RTX 5060 Ti is Blackwell");
    Check(!D::TuringOrAmpere(D::kNvidia, 0x2b85), "RTX 5090 is Blackwell");
    Check(!D::TuringOrAmpere(0x1002, 0x1e04), "an AMD device id is never Turing");
    Check(!D::TuringOrAmpere(0x8086, 0x2204), "an Intel device id is never Ampere");
}

void TestAutoPerMode() {
    Check(D::AutoPreset(D::Dlaa, false) == D::kPresetK, "auto on RTX 40/50: K for DLAA");
    Check(D::AutoPreset(D::MaxQuality, false) == D::kPresetK, "auto on RTX 40/50: K for Quality");
    Check(D::AutoPreset(D::Balanced, false) == D::kPresetK, "auto on RTX 40/50: K for Balanced");
    Check(D::AutoPreset(D::MaxPerf, false) == D::kPresetM, "auto on RTX 40/50: M for Performance");
    Check(D::AutoPreset(D::UltraPerformance, false) == D::kPresetL, "auto on RTX 40/50: L for Ultra Performance");
    for (const auto mode : kModes) {
        Check(D::AutoPreset(mode, true) == D::kPresetK, "auto on RTX 20/30: K for every mode");
    }
}

void TestApplied() {
    for (const bool older : {false, true}) {
        for (const auto mode : kModes) {
            const int wanted = D::AutoPreset(mode, older);
            Check(D::Applied(D::kPresetAuto, mode, older) == wanted, "auto applies the mode's preset");
            Check(D::Applied(D::kPresetE, mode, older) == wanted, "a live E applies auto");
            Check(D::Applied(D::kPresetF, mode, older) == wanted, "a live F applies auto");
            Check(D::Applied(D::kPresetK, mode, older) == D::kPresetK, "K stays K");
            Check(D::Applied(D::kPresetM, mode, older) == D::kPresetM, "M stays M, also on RTX 20/30");
            Check(D::Applied(D::kPresetJ, mode, older) == D::kPresetJ, "J stays J");
            Check(D::Applied(D::kPresetDefault, mode, older) == D::kPresetDefault, "default stays default");
            Check(D::Applied(40, mode, older) == D::kPresetLast, "out of range clamps to Z");
        }
    }
    Check(D::Applied(D::kPresetAuto, D::MaxPerf, false) == D::kPresetM, "auto Performance on RTX 40 is M");
    Check(D::Applied(D::kPresetAuto, D::MaxPerf, true) == D::kPresetK, "auto Performance on RTX 30 is K");
}

void TestResets() {
    Check(!D::CameraCut(0.0f, 1280), "a still camera is no cut");
    Check(!D::CameraCut(640.0f, 1280), "half the render width is no cut");
    Check(D::CameraCut(641.0f, 1280), "more than half the render width is a cut");
    Check(D::CameraCut(1500.0f, 1920), "a lamp warp's jump is a cut");
    Check(!D::CameraCut(900.0f, 1920), "a fast turn is no cut");

    Check(D::LoadArrived(true, false), "the load request clearing is an arrival");
    Check(!D::LoadArrived(false, true), "a load starting is no arrival");
    Check(!D::LoadArrived(true, true), "a load in progress is no arrival");
    Check(!D::LoadArrived(false, false), "play is no arrival");

    Check(!D::ResetHistory(false, true, 3, 3), "a steady frame keeps history");
    Check(D::ResetHistory(true, true, 3, 3), "a setting change resets");
    Check(D::ResetHistory(false, false, 3, 3), "a frame without the upscaler before resets");
    Check(D::ResetHistory(false, true, 3, 4), "a respawn or lamp arrival resets");
    Check(D::ResetHistory(false, true, 0xffffffffu, 0), "the cut counter wrapping still resets");

    Check(D::ColorNeedsCleaning(true, true), "a float colour with storage is checked");
    Check(!D::ColorNeedsCleaning(true, false), "no storage, no check in place");
    Check(!D::ColorNeedsCleaning(false, true), "a UNORM colour cannot hold NaN");
}

int RunSuite() {
    g_failed = 0;
    TestSavedPresets();
    TestGeneration();
    TestAutoPerMode();
    TestApplied();
    TestResets();
    return g_failed;
}

}

int main() {
#ifdef LANTERN_DLSS_RULES_MUTANTS
    int survivors = 0;
    for (int m = 0; m <= D::kMutants; ++m) {
        D::g_mutant = m;
        const int found = RunSuite();
        if (m == 0) {
            if (found) {
                std::printf("dlss rules mutants: the unmutated run failed %d checks\n", found);
                return 1;
            }
        } else if (!found) {
            std::printf("dlss rules mutants: mutant %d survived\n", m);
            ++survivors;
        } else {
            std::printf("dlss rules mutants: mutant %d caught by %d checks\n", m, found);
        }
    }
    if (survivors) return 1;
    std::printf("dlss rules mutants: every mutant caught\n");
    return 0;
#else
    const int failed = RunSuite();
    if (failed) {
        std::printf("dlss rules: %d checks failed\n", failed);
        return 1;
    }
    std::printf("dlss rules: all checks passed\n");
    return 0;
#endif
}
