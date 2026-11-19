// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

#ifdef LANTERN_UPSCALER_SKIPS_MUTANTS
namespace Vulkan::UpscalerSkips {
inline int g_mutant = 0;
}
#define LANTERN_UPSCALER_SKIPS_PICK(n, wrong, right) (::Vulkan::UpscalerSkips::g_mutant == (n) ? (wrong) : (right))
#define LANTERN_UPSCALER_SKIPS_CONSTEXPR inline
#else
#define LANTERN_UPSCALER_SKIPS_CONSTEXPR constexpr
#define LANTERN_UPSCALER_SKIPS_PICK(n, wrong, right) (right)
#endif

namespace Vulkan::UpscalerSkips {

inline constexpr std::uint32_t kUnresolvedFramesBeforeNative = 8;
inline constexpr int kMutants = 12;

enum class Path : std::uint8_t { Upscaled, Copy, None, Count };

enum class Reason : std::uint8_t {
    Upscaled,
    NoUiDraw,
    Off,
    NoFinishedFrame,
    OtherTarget,
    CameraNotReady,
    SizeMismatch,
    NotDispatched,
    Count
};

inline constexpr std::size_t kReasons = std::size_t(Reason::Count);

constexpr const char* ReasonName(Reason reason) {
    switch (reason) {
    case Reason::Upscaled: return "upscaled";
    case Reason::NoUiDraw: return "no UI draw";
    case Reason::Off: return "upscaler off or failed";
    case Reason::NoFinishedFrame: return "no finished frame";
    case Reason::OtherTarget: return "UI drawn into another target";
    case Reason::CameraNotReady: return "camera not ready";
    case Reason::SizeMismatch: return "depth and colour sizes differ";
    case Reason::NotDispatched: return "upscale not dispatched";
    case Reason::Count: break;
    }
    return "unknown";
}

LANTERN_UPSCALER_SKIPS_CONSTEXPR Reason CopyReason(bool active, bool has_finished_frame, bool finished_is_target, bool camera_ready) {
    if (!active) return Reason::Off;
    if (!has_finished_frame) return Reason::NoFinishedFrame;
    if (LANTERN_UPSCALER_SKIPS_PICK(1, false, !finished_is_target)) return Reason::OtherTarget;
    if (!camera_ready) return Reason::CameraNotReady;
    return Reason::NotDispatched;
}

LANTERN_UPSCALER_SKIPS_CONSTEXPR bool AcceptFrame(bool is_finished_frame, bool frame_format, bool scene_size) {
    return is_finished_frame || (frame_format && LANTERN_UPSCALER_SKIPS_PICK(10, true, scene_size));
}

struct PassDepth {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    bool has_camera = false;
};

LANTERN_UPSCALER_SKIPS_CONSTEXPR int PickDepth(const PassDepth* passes, std::size_t count, std::uint32_t width, std::uint32_t height) {
    int picked = -1;
    for (std::size_t i = 0; i < count; ++i) {
        const auto& pass = passes[i];
        if (pass.width != width || pass.height != height) continue;
        if (!LANTERN_UPSCALER_SKIPS_PICK(12, true, pass.has_camera)) continue;
        if (LANTERN_UPSCALER_SKIPS_PICK(11, picked < 0, true)) picked = int(i);
    }
    return picked;
}

LANTERN_UPSCALER_SKIPS_CONSTEXPR std::uint32_t NextUnresolved(std::uint32_t frames, bool resolved) {
    if (LANTERN_UPSCALER_SKIPS_PICK(2, false, resolved)) return 0;
    return frames < kUnresolvedFramesBeforeNative ? frames + 1 : frames;
}

LANTERN_UPSCALER_SKIPS_CONSTEXPR bool TemporalInputs(std::uint32_t unresolved) {
    return LANTERN_UPSCALER_SKIPS_PICK(3, unresolved <= kUnresolvedFramesBeforeNative,
                                       unresolved < kUnresolvedFramesBeforeNative);
}

inline float SceneBias(bool active, bool taa, std::uint32_t unresolved, float render, float output,
                       bool dlss_extra, float offset) {
    if (!active || taa) return 0.0f;
    if (LANTERN_UPSCALER_SKIPS_PICK(4, false, !TemporalInputs(unresolved))) return 0.0f;
    const float scale_bias = render > 0.0f && render < output ? std::log2(render / output) : 0.0f;
    return scale_bias - (LANTERN_UPSCALER_SKIPS_PICK(5, false, dlss_extra) ? 1.0f : 0.0f) + offset;
}

struct Window {
    std::array<std::uint32_t, std::size_t(Path::Count)> paths{};
    std::array<std::uint32_t, kReasons> reasons{};

    void Add(Path path, Reason reason) {
        ++paths[std::size_t(path)];
        if (path != Path::Upscaled) ++reasons[std::size_t(reason)];
    }
    std::uint32_t Frames() const { return paths[0] + paths[1] + paths[2]; }
    std::uint32_t Skipped() const { return LANTERN_UPSCALER_SKIPS_PICK(6, paths[1], paths[1] + paths[2]); }
};

inline std::string Describe(const char* upscaler, const Window& window) {
    char line[256];
    std::snprintf(line, sizeof(line), "%s %u, copy %u, none %u of %u frames", upscaler,
                  window.paths[0], window.paths[1], window.paths[2], window.Frames());
    std::string out = line;
    if (window.Skipped()) {
        out += " (";
        bool first = true;
        for (std::size_t i = 1; i < kReasons; ++i) {
            if (!window.reasons[i]) continue;
            std::snprintf(line, sizeof(line), "%s%s %u", first ? "" : ", ", ReasonName(Reason(i)),
                          window.reasons[i]);
            out += line;
            first = false;
        }
        out += ")";
    }
    return out;
}

struct AfterLoad {
    bool loading = false;
    bool armed = false;
    std::uint32_t unresolved = 0;

    bool Frame(bool loading_now, bool resolved) {
        if (loading_now) {
            loading = true;
            return false;
        }
        if (loading) {
            loading = false;
            armed = LANTERN_UPSCALER_SKIPS_PICK(8, false, true);
            unresolved = 0;
        }
        if (!armed) return false;
        unresolved = LANTERN_UPSCALER_SKIPS_PICK(7, false, resolved) ? 0 : unresolved + 1;
        if (unresolved < LANTERN_UPSCALER_SKIPS_PICK(9, 1u, kUnresolvedFramesBeforeNative)) return false;
        armed = false;
        return true;
    }
};

} // namespace Vulkan::UpscalerSkips
