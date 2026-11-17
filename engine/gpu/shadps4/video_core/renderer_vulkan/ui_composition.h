// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace Vulkan::UiComposition {

enum class Background { None, Copy, Temporal };

// UI composition must not require scene depth/camera or an active FSR context.
constexpr Background Choose(bool scaled, bool ui_draw, bool scene_ready, bool fsr_active) {
    if (!scaled || !ui_draw) {
        return Background::None;
    }
    return scene_ready && fsr_active ? Background::Temporal : Background::Copy;
}

template <typename Format>
constexpr bool FrameFormat(Format format) {
    return format == Format::eR8G8B8A8Unorm || format == Format::eR8G8B8A8Srgb;
}

// The game's UI movie viewport: 1920x1080, or with Widescreen on a screen of another shape that
// shape around it (2520x1080 at 21:9, 1920x1200 at 16:10; scripts/patches.py ui_size, passed as
// BB_UI_SIZE; with Sharp UI the output's own size). Scaleform then shows the 1920x1080 stage whole
// and centred (scaled to fit a larger viewport), and what the game places past the stage's edges - the enemy gauges, co-op names and the
// lock-on marker at the screen's sides (engine/widescreen.cpp) - stays inside the viewport
// instead of being clipped at the 16:9 box.
inline std::array<float, 2> ParseUiSize(const char* value) {
    unsigned w = 0, h = 0;
    if (value && std::sscanf(value, "%ux%u", &w, &h) == 2 && w >= 1920 && h >= 1080 &&
        w <= 8192 && h <= 8192) {
        return {float(w), float(h)};
    }
    return {1920.0f, 1080.0f};
}
inline std::array<float, 2> UiSize() {
    static const auto size = ParseUiSize(std::getenv("BB_UI_SIZE"));
    return size;
}

inline bool ViewportIs(float width, float height, std::array<float, 2> size) {
    return std::abs(std::abs(width) - size[0]) < 0.5f && std::abs(std::abs(height) - size[1]) < 0.5f;
}
// A UI movie's viewport: the stage's 1920x1080, or the widened one.
inline bool NativeViewport(float width, float height, std::array<float, 2> ui = UiSize()) {
    return ViewportIs(width, height, {1920.0f, 1080.0f}) || ViewportIs(width, height, ui);
}
// The viewport over the whole guest target a UI pass draws into: its render-size rectangle (the
// game aligns the target a few pixels larger). Its vertices are in that target's pixels.
inline bool TargetViewport(float width, float height, uint32_t target_width,
                           uint32_t target_height) {
    return std::abs(std::abs(width) - float(target_width)) < 8.0f &&
           std::abs(std::abs(height) - float(target_height)) < 8.0f;
}
// A viewport of its own inside a UI pass, smaller than the UI viewport and neither a movie's nor
// the target's (an item icon's rectangle): placed in the UI viewport's pixels, as the movie that
// holds it is.
inline bool InnerViewport(float width, float height, uint32_t target_width,
                          uint32_t target_height, std::array<float, 2> ui = UiSize()) {
    const float w = std::abs(width), h = std::abs(height);
    return w >= 1.0f && h >= 1.0f && w <= ui[0] + 0.5f && h <= ui[1] + 0.5f &&
           !NativeViewport(width, height, ui) &&
           !TargetViewport(width, height, target_width, target_height);
}

// A scene rendered at a UI viewport's size (Sharp UI with Native AA: the scene, the UI viewport
// and the output all 2560x1440; or a scaled scene of exactly 1920x1080, as Performance at 4K):
// the game's full-screen post passes then have a UI movie's viewport too, so a native viewport
// no longer tells a UI draw apart. (The UI composition started on such a pass, into a post-process
// target before the game's frame: the menus' icon sheet showed over the screen.) The game's
// targets are aligned a few pixels larger than the scene, as TargetViewport.
inline bool SceneHasUiViewportSize(uint32_t scene_width, uint32_t scene_height,
                                   std::array<float, 2> ui = UiSize()) {
    const auto near = [&](std::array<float, 2> size) {
        return std::abs(float(scene_width) - size[0]) < 8.0f &&
               std::abs(float(scene_height) - size[1]) < 8.0f;
    };
    return near({1920.0f, 1080.0f}) || near(ui);
}

// A UI viewport widened past the 1920x1080 stage (Widescreen, or Sharp UI at the output's size).
inline bool WidenedUiViewport(float width, float height, std::array<float, 2> ui = UiSize()) {
    return !ViewportIs(ui[0], ui[1], {1920.0f, 1080.0f}) && ViewportIs(width, height, ui);
}

// The scene at the widened UI viewport's size (Sharp UI with Native AA): its post passes then
// have that viewport too.
inline bool SceneNearUiSize(uint32_t scene_width, uint32_t scene_height,
                            std::array<float, 2> ui = UiSize()) {
    return std::abs(float(scene_width) - ui[0]) < 8.0f && std::abs(float(scene_height) - ui[1]) < 8.0f;
}

// Scaleform draws, including the first stencil/movie pass. A native-size viewport
// alone also matches every fullscreen post pass when guest targets stay at 1080p.
constexpr bool MovieShader(uint64_t hash) {
    return hash == 0x34e8a281 || hash == 0x81d336ce || hash == 0x09957251 ||
           hash == 0x24042a9b || hash == 0xa400228b;
}

// Scaleform clip masks write stencil with no colour exports. Observe them before
// the masked image so a late UI-depth preparation cannot erase the clip mask.
constexpr bool ObserveDraw(uint32_t mrt_mask, uint64_t vs_hash, bool has_stencil) {
    return std::popcount(mrt_mask) == 1 ||
           (mrt_mask == 0 && has_stencil && MovieShader(vs_hash));
}

struct FitTransform {std::array<float,2> scale;std::array<float,2> offset;};
// A UI viewport of `ui` units shown whole, centred and unstretched in a w x h image.
inline FitTransform Fit(uint32_t w,uint32_t h,std::array<float,2> ui={1920.0f,1080.0f}) {
    const float s=std::min(float(w)/ui[0],float(h)/ui[1]);
    return {{s,s},{(float(w)-ui[0]*s)/2,(float(h)-ui[1]*s)/2}};
}
inline std::array<float, 2> Scale(uint32_t guest_width, uint32_t guest_height,
                                uint32_t output_width, uint32_t output_height,
                                bool native_coordinates,
                                std::array<float, 2> ui = {1920.0f, 1080.0f}) {
    return {float(output_width) / (native_coordinates ? ui[0] : float(guest_width)),
            float(output_height) / (native_coordinates ? ui[1] : float(guest_height))};
}
// The 1920x1080 stage inside a UI viewport of `ui` units: shown whole and centred, scaled up
// when the viewport is larger than it both ways (Sharp UI).
inline float StageScale(std::array<float, 2> ui) {
    return std::max(1.0f, std::min(ui[0] / 1920.0f, ui[1] / 1080.0f));
}

} // namespace Vulkan::UiComposition
