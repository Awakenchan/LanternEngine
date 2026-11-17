// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cmath>
#include <cstddef>
#include <cstdint>

#ifdef LANTERN_FPS_BOX_MUTANTS
namespace BbEngine::FpsBox {
inline int g_mutant = 0;
}
#define LANTERN_FPS_BOX_PICK(n, wrong, right) (::BbEngine::FpsBox::g_mutant == (n) ? (wrong) : (right))
#else
#define LANTERN_FPS_BOX_PICK(n, wrong, right) (right)
#endif

namespace BbEngine::FpsBox {
inline constexpr int kMutants = 6;
inline constexpr std::int64_t kShrinkMs = 2000;

struct Fit {
    std::size_t key = 0;
    std::size_t length = 0;
    float width = 0.0f;
    bool shrinking = false;
    std::int64_t shorter_since = 0;
};

inline float Width(Fit& fit, std::size_t key, std::size_t length, float text_width, float padding, std::int64_t now_ms) {
    const float wanted = std::ceil(text_width) + LANTERN_FPS_BOX_PICK(4, 0.0f, 2.0f * padding);
    const bool refit = fit.width <= 0.0f || LANTERN_FPS_BOX_PICK(3, false, key != fit.key) ||
                       wanted > fit.width || LANTERN_FPS_BOX_PICK(6, length == fit.length && wanted != fit.width, false);
    if (!refit && LANTERN_FPS_BOX_PICK(1, false, length < fit.length)) {
        if (!fit.shrinking) {
            fit.shrinking = true;
            fit.shorter_since = now_ms;
        }
        if (now_ms - fit.shorter_since < LANTERN_FPS_BOX_PICK(2, std::int64_t(0), kShrinkMs)) return fit.width;
    } else if (!refit) {
        if (length > fit.length) fit.length = length;
        fit.shrinking = false;
        return fit.width;
    }
    fit.key = key;
    fit.length = length;
    fit.width = wanted;
    fit.shrinking = false;
    return fit.width;
}

inline float Left(float anchor, float width, float pivot) { return anchor - width * LANTERN_FPS_BOX_PICK(5, 0.0f, pivot); }
}
