// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <cstdint>

#ifdef LANTERN_DLSS_RULES_MUTANTS
namespace BbDlss {
inline int g_mutant = 0;
}
#define LANTERN_DLSS_RULES_PICK(n, wrong, right) (::BbDlss::g_mutant == (n) ? (wrong) : (right))
#define LANTERN_DLSS_RULES_FN inline
#else
#define LANTERN_DLSS_RULES_PICK(n, wrong, right) (right)
#define LANTERN_DLSS_RULES_FN constexpr
#endif

namespace BbDlss {

inline constexpr int kMutants = 14;

inline constexpr int kPresetAuto = -1, kPresetDefault = 0, kPresetE = 5, kPresetF = 6, kPresetJ = 10,
                     kPresetK = 11, kPresetL = 12, kPresetM = 13, kPresetLast = 26;

enum Quality : int { MaxPerf = 0, Balanced = 1, MaxQuality = 2, UltraPerformance = 3, Dlaa = 5 };

inline constexpr std::uint32_t kNvidia = 0x10de, kFirstAdaDevice = 0x2600;

LANTERN_DLSS_RULES_FN bool Deprecated(int preset) {
    return LANTERN_DLSS_RULES_PICK(1, preset == kPresetE, preset == kPresetE || preset == kPresetF);
}

LANTERN_DLSS_RULES_FN bool Offered(int preset) {
    return preset == kPresetDefault || preset == kPresetAuto ||
           (preset >= kPresetJ && preset <= LANTERN_DLSS_RULES_PICK(2, kPresetL, kPresetM));
}

LANTERN_DLSS_RULES_FN int FromSaved(int preset) {
    return Deprecated(preset) ? LANTERN_DLSS_RULES_PICK(3, kPresetK, kPresetAuto) : preset;
}

LANTERN_DLSS_RULES_FN bool TuringOrAmpere(std::uint32_t vendor, std::uint32_t device) {
    return LANTERN_DLSS_RULES_PICK(4, true, vendor == kNvidia) &&
           LANTERN_DLSS_RULES_PICK(5, device <= kFirstAdaDevice, device < kFirstAdaDevice);
}

LANTERN_DLSS_RULES_FN int AutoPreset(int quality, bool turing_or_ampere) {
    if (LANTERN_DLSS_RULES_PICK(6, false, turing_or_ampere)) {
        return kPresetK;
    }
    if (quality == LANTERN_DLSS_RULES_PICK(7, Balanced, MaxPerf)) {
        return LANTERN_DLSS_RULES_PICK(8, kPresetL, kPresetM);
    }
    if (quality == UltraPerformance) {
        return LANTERN_DLSS_RULES_PICK(9, kPresetM, kPresetL);
    }
    return kPresetK;
}

LANTERN_DLSS_RULES_FN int Applied(int setting, int quality, bool turing_or_ampere) {
    if (setting == kPresetAuto || LANTERN_DLSS_RULES_PICK(10, false, Deprecated(setting))) {
        return AutoPreset(quality, turing_or_ampere);
    }
    return std::clamp(setting, kPresetDefault, kPresetLast);
}

LANTERN_DLSS_RULES_FN bool CameraCut(float moved_pixels, std::uint32_t render_width) {
    return moved_pixels > float(render_width) * LANTERN_DLSS_RULES_PICK(11, 2.0f, 0.5f);
}

LANTERN_DLSS_RULES_FN bool LoadArrived(bool asked_before, bool asked_now) {
    return asked_before && LANTERN_DLSS_RULES_PICK(12, asked_now, !asked_now);
}

LANTERN_DLSS_RULES_FN bool ResetHistory(bool settings_changed, bool dispatched_last_frame, std::uint32_t cuts_seen,
                                        std::uint32_t cuts_now) {
    return settings_changed || !dispatched_last_frame ||
           LANTERN_DLSS_RULES_PICK(13, false, cuts_seen != cuts_now);
}

LANTERN_DLSS_RULES_FN bool ColorNeedsCleaning(bool float_color, bool storage) {
    return LANTERN_DLSS_RULES_PICK(14, storage, float_color && storage);
}

}
