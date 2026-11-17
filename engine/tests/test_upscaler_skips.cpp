// SPDX-License-Identifier: GPL-2.0-or-later
#include "gpu/shadps4/video_core/renderer_vulkan/upscaler_skips.h"
#include <cmath>
#include <cstdio>

namespace S = Vulkan::UpscalerSkips;

namespace {
int g_failed = 0;

void Check(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAILED: %s\n", what);
        ++g_failed;
    }
}

bool Near(float a, float b) {
    return std::abs(a - b) < 0.01f;
}

constexpr float kRender = 1376.0f, kOutput = 3440.0f;

float DlssBias(std::uint32_t unresolved) {
    return S::SceneBias(true, false, unresolved, kRender, kOutput, true, 0.0f);
}

void TestStreak() {
    std::uint32_t frames = 0;
    for (std::uint32_t i = 0; i < S::kUnresolvedFramesBeforeNative; ++i) {
        Check(S::TemporalInputs(frames), "jitter and bias stay on before the threshold");
        frames = S::NextUnresolved(frames, false);
    }
    Check(frames == S::kUnresolvedFramesBeforeNative, "every skipped frame counts");
    Check(!S::TemporalInputs(frames), "the threshold's frame turns jitter and bias off");
    Check(!S::TemporalInputs(S::kUnresolvedFramesBeforeNative - 0), "exactly the threshold is off");
    Check(S::TemporalInputs(S::kUnresolvedFramesBeforeNative - 1), "one below the threshold is on");
    for (int i = 0; i < 1000; ++i) frames = S::NextUnresolved(frames, false);
    Check(frames == S::kUnresolvedFramesBeforeNative, "the count stops at the threshold");
    frames = S::NextUnresolved(frames, true);
    Check(frames == 0 && S::TemporalInputs(frames), "one resolved frame turns them back on");
}

void TestBias() {
    Check(Near(DlssBias(0), std::log2(kRender / kOutput) - 1.0f), "DLSS: log2(render/output) - 1");
    Check(Near(DlssBias(0), -2.32f), "1376 of 3440 with DLSS is -2.32");
    Check(Near(S::SceneBias(true, false, 0, kRender, kOutput, false, 0.0f), -1.32f), "without the DLSS extra: the ratio");
    Check(Near(S::SceneBias(true, false, 0, kRender, kOutput, true, 0.5f), -1.82f), "the player's offset adds");
    Check(DlssBias(S::kUnresolvedFramesBeforeNative) == 0.0f, "unresolved frames: bias 0");
    Check(S::SceneBias(true, false, S::kUnresolvedFramesBeforeNative, kRender, kOutput, true, 0.5f) == 0.0f,
          "unresolved frames: no offset either");
    Check(Near(DlssBias(S::NextUnresolved(S::kUnresolvedFramesBeforeNative, true)), -2.32f),
          "the guide's value is back after one resolved frame");
    Check(S::SceneBias(false, false, 0, kRender, kOutput, true, 0.0f) == 0.0f, "no upscaler: 0");
    Check(S::SceneBias(true, true, 0, kRender, kOutput, false, 0.0f) == 0.0f, "TAA: 0");
    Check(Near(S::SceneBias(true, false, 0, 1920.0f, 1920.0f, true, 0.0f), -1.0f), "DLAA: the extra mip only");
}

void TestReasons() {
    Check(S::CopyReason(false, true, true, true) == S::Reason::Off, "off first");
    Check(S::CopyReason(true, false, false, true) == S::Reason::NoFinishedFrame, "no finished frame");
    Check(S::CopyReason(true, true, false, true) == S::Reason::OtherTarget, "UI in another target");
    Check(S::CopyReason(true, true, true, false) == S::Reason::CameraNotReady, "camera not ready");
    Check(S::CopyReason(true, true, true, true) == S::Reason::NotDispatched, "everything ready");
}

void TestAccept() {
    Check(S::AcceptFrame(true, false, false), "the finished frame itself");
    Check(S::AcceptFrame(false, true, true), "the UI draw's own RGBA8 target of the scene's size");
    Check(!S::AcceptFrame(false, true, false), "another size is not the frame");
    Check(!S::AcceptFrame(false, false, true), "another format is not the frame");
}

void TestDepthPick() {
    const S::PassDepth passes[] = {{1376, 576, true}, {512, 512, true}, {1376, 576, true}, {1376, 576, false}};
    Check(S::PickDepth(passes, 4, 1376, 576) == 2, "the latest matching pass with a camera");
    Check(S::PickDepth(passes, 2, 1376, 576) == 0, "an earlier pass when only it matches");
    Check(S::PickDepth(passes, 4, 1920, 1080) == -1, "no pass of the frame's size");
    const S::PassDepth no_camera[] = {{1376, 576, false}};
    Check(S::PickDepth(no_camera, 1, 1376, 576) == -1, "a pass without a camera is not picked");
}

void TestWindow() {
    S::Window window;
    for (int i = 0; i < 5; ++i) window.Add(S::Path::Upscaled, S::Reason::Upscaled);
    for (int i = 0; i < 3; ++i) window.Add(S::Path::Copy, S::Reason::CameraNotReady);
    window.Add(S::Path::Copy, S::Reason::OtherTarget);
    window.Add(S::Path::None, S::Reason::NoUiDraw);
    Check(window.Frames() == 10, "every frame counts once");
    Check(window.Skipped() == 5, "copy and none frames are skips");
    Check(S::Describe("dlss", window) ==
              "dlss 5, copy 4, none 1 of 10 frames (no UI draw 1, UI drawn into another target 1, camera not ready 3)",
          "the frame-stats line");
    S::Window clean;
    clean.Add(S::Path::Upscaled, S::Reason::Upscaled);
    Check(S::Describe("dlss", clean) == "dlss 1, copy 0, none 0 of 1 frames", "no reasons without skips");
}

void TestAfterLoad() {
    S::AfterLoad after;
    for (int i = 0; i < 100; ++i) Check(!after.Frame(false, false), "no line before any load");
    for (int i = 0; i < 50; ++i) Check(!after.Frame(true, false), "no line during the load");
    bool printed = false;
    std::uint32_t frames = 0;
    while (!printed && frames < 100) {
        printed = after.Frame(false, false);
        ++frames;
    }
    Check(printed && frames == S::kUnresolvedFramesBeforeNative, "one line once the streak reaches the threshold");
    for (int i = 0; i < 100; ++i) Check(!after.Frame(false, false), "only one line a load");
    after.Frame(true, false);
    for (std::uint32_t i = 0; i + 1 < S::kUnresolvedFramesBeforeNative; ++i) after.Frame(false, false);
    Check(!after.Frame(false, true), "a resolved frame ends the streak");
    printed = false;
    for (std::uint32_t i = 0; i < S::kUnresolvedFramesBeforeNative; ++i) printed = after.Frame(false, false);
    Check(printed, "a streak that starts later after the load prints");
}

int RunSuite() {
    g_failed = 0;
    TestStreak();
    TestBias();
    TestReasons();
    TestAccept();
    TestDepthPick();
    TestWindow();
    TestAfterLoad();
    return g_failed;
}
}

int main() {
#ifdef LANTERN_UPSCALER_SKIPS_MUTANTS
    int survivors = 0;
    for (int m = 0; m <= S::kMutants; ++m) {
        S::g_mutant = m;
        const int found = RunSuite();
        if (m == 0) {
            if (found) {
                std::printf("upscaler skips mutants: the unmutated run failed %d checks\n", found);
                return 1;
            }
        } else if (!found) {
            std::printf("upscaler skips mutants: mutant %d survived\n", m);
            ++survivors;
        } else {
            std::printf("upscaler skips mutants: mutant %d caught by %d checks\n", m, found);
        }
    }
    if (survivors) return 1;
    std::printf("upscaler skips mutants: every mutant caught\n");
    return 0;
#else
    if (RunSuite()) {
        std::printf("upscaler skips: %d failures\n", g_failed);
        return 1;
    }
    std::printf("upscaler skips: all tests passed\n");
    return 0;
#endif
}
