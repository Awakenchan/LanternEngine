// SPDX-License-Identifier: GPL-2.0-or-later
#include <cassert>
#include <cstdio>
#include "gpu/shadps4/video_core/renderer_vulkan/ui_composition.h"

namespace {
enum class Format { eUndefined, eR8G8B8A8Unorm, eR8G8B8A8Srgb, eB8G8R8A8Unorm, eB8G8R8A8Srgb, eR16G16B16A16Sfloat, eA2B10G10R10UnormPack32 };
}

int main() {
    using namespace Vulkan::UiComposition;
    static_assert(FrameFormat(Format::eR8G8B8A8Unorm) && FrameFormat(Format::eR8G8B8A8Srgb));
    static_assert(!FrameFormat(Format::eUndefined) && !FrameFormat(Format::eB8G8R8A8Unorm) &&
                  !FrameFormat(Format::eB8G8R8A8Srgb) && !FrameFormat(Format::eR16G16B16A16Sfloat) &&
                  !FrameFormat(Format::eA2B10G10R10UnormPack32));
    for (const auto format : {Format::eUndefined, Format::eR8G8B8A8Unorm, Format::eR8G8B8A8Srgb, Format::eB8G8R8A8Unorm,
                              Format::eB8G8R8A8Srgb, Format::eR16G16B16A16Sfloat, Format::eA2B10G10R10UnormPack32}) {
        const bool finished_frame = FrameFormat(format);
        const bool ui_draw = format == Format::eR8G8B8A8Unorm || format == Format::eR8G8B8A8Srgb;
        assert(finished_frame == ui_draw);
    }
    assert(Choose(true, true, false, true) == Background::Copy);
    assert(Choose(true, true, false, false) == Background::Copy);
    assert(Choose(true, true, true, true) == Background::Temporal);
    assert(Choose(true, true, true, false) == Background::Copy);
    assert(Choose(true, false, true, true) == Background::None);
    assert(Choose(false, true, true, true) == Background::None);
    assert(NativeViewport(1920, -1080));
    assert(!NativeViewport(960, -540));
    constexpr std::array<float, 2> ui4k{3840.0f, 2160.0f};
    assert(WidenedUiViewport(3840, -2160, ui4k));
    assert(!WidenedUiViewport(1920, -1080, ui4k));
    assert(!WidenedUiViewport(1920, -1080));
    assert(SceneHasUiViewportSize(1916, 1076, ui4k) && !SceneNearUiSize(1916, 1076, ui4k));
    assert(SceneNearUiSize(3840, 2160, ui4k));
    assert(MovieShader(0x34e8a281) && MovieShader(0x24042a9b));
    assert(!MovieShader(0x0b0acf50));
    assert(!MovieShader(0x6c62a79f));
    assert(ObserveDraw(0, 0xa400228b, true));
    assert(ObserveDraw(0, 0x34e8a281, true));
    assert(ObserveDraw(1, 0x44072322, true));
    assert(!ObserveDraw(0, 0x6c62a79f, true));
    assert(!ObserveDraw(0, 0xa400228b, false));
    assert(!ObserveDraw(3, 0xa400228b, true));
    const auto display = Scale(960, 540, 1920, 1080, false);
    assert(display[0] == 2 && display[1] == 2);
    const auto ui = Scale(960, 540, 1920, 1080, true);
    assert(ui[0] == 1 && ui[1] == 1);
    const auto larger = Scale(960, 540, 3840, 2160, true);
    assert(larger[0] == 2 && larger[1] == 2);
    for(auto size: {std::array<uint32_t,2>{1920,1080},{2560,1440},{3840,2160},{3440,1440},{5120,1440}}) {
        auto f=Fit(size[0],size[1]);
        assert(f.scale[0]==f.scale[1]);
        assert(f.offset[0]>=0 && f.offset[1]>=0);
        assert(std::abs(1920*f.scale[0]+2*f.offset[0]-size[0])<.01f);
        assert(std::abs(1080*f.scale[1]+2*f.offset[1]-size[1])<.01f);
    }
    assert(Fit(1920,1080).offset==(std::array<float,2>{0,0}));
    assert(std::abs(Fit(3440,1440).offset[0]-440)<.01f);
    assert(std::abs(Fit(5120,1440).offset[0]-1280)<.01f);
    const auto wide = ParseUiSize("2520x1080");
    assert(wide[0] == 2520 && wide[1] == 1080);
    assert(ParseUiSize(nullptr)[0] == 1920 && ParseUiSize("800x600")[0] == 1920 && ParseUiSize("junk")[1] == 1080);
    assert(NativeViewport(2520, -1080, wide) && NativeViewport(1920, 1080, wide) && !NativeViewport(2560, 1080, wide));
    const auto w = Fit(3360, 1440, wide);
    assert(w.scale[0] == w.scale[1] && std::abs(w.offset[0]) < .01f && std::abs(w.offset[1]) < .01f);
    assert(std::abs((300 * w.scale[0] + w.offset[0]) - Fit(3360, 1440).offset[0]) < .01f);
    const auto t = Fit(1920, 1200, ParseUiSize("1920x1200"));
    assert(t.scale[0] == 1 && t.offset[1] == 0);
    const auto sharp = ParseUiSize("3840x2160");
    assert(TargetViewport(1916, -1076, 1920, 1080) && TargetViewport(1708, 960, 1712, 960));
    assert(!TargetViewport(128, 128, 1920, 1080));
    assert(InnerViewport(128, -128, 1920, 1080, sharp) && InnerViewport(2560, 1440, 1712, 960, sharp));
    assert(!InnerViewport(3840, 2160, 1920, 1080, sharp) && !InnerViewport(1920, 1080, 1920, 1080, sharp));
    assert(!InnerViewport(1916, 1076, 1920, 1080, sharp) && !InnerViewport(4096, 2160, 1920, 1080, sharp));
    assert(!InnerViewport(0, 0, 1920, 1080, sharp));
    assert(SceneHasUiViewportSize(2560, 1440, ParseUiSize("2560x1440")));
    assert(SceneHasUiViewportSize(1920, 1080, sharp) && SceneHasUiViewportSize(1924, 1084, sharp));
    assert(!SceneHasUiViewportSize(1708, 960, ParseUiSize("2560x1440")));
    assert(!SceneHasUiViewportSize(2560, 1440, ParseUiSize(nullptr)));
    assert(!SceneHasUiViewportSize(2296, 960, ParseUiSize("3360x1440")));
    assert(!SceneHasUiViewportSize(3440, 1440, ParseUiSize("3360x1440")));
    std::puts("UI composition: PASS (menu without camera, HUD, FSR off, viewport scaling, frame formats)");
}
