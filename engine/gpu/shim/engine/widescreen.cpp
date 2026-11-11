// SPDX-License-Identifier: GPL-3.0-or-later
#include "widescreen.h"
#include "camera.h"
#include "engine_hooks.h"
#include "bbport_platform.h"
#include "bbport_settings.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace BbEngine::Widescreen {
namespace {
constexpr float kGameAspect = 16.0f / 9.0f;
constexpr std::uint32_t kStageWidth = 1920, kStageHeight = 1080;

constexpr std::size_t kStageWidthAt = 0x51289f8, kStageHeightAt = 0x51289fc;
enum class Edge : std::uint8_t { None, Right, Bottom };
struct StageRead {
    std::size_t at;
    bool height;
    bool ecx;
    Edge edge;
};
constexpr StageRead kStageReads[] = {
    {0x15e83af, false, false, Edge::None},   {0x1bfd491, false, false, Edge::Right},
    {0x1bfd4d1, true, false, Edge::Bottom},  {0x1644357, false, false, Edge::None},
    {0x1644365, true, false, Edge::None},    {0x1644c55, false, false, Edge::None},
    {0x1644c63, true, false, Edge::None},    {0x16452c7, false, false, Edge::None},
    {0x16452d5, true, false, Edge::None},    {0x1654df6, false, false, Edge::Right},
    {0x1654e1c, true, false, Edge::Bottom},  {0x1655611, true, false, Edge::None},
    {0x1655628, false, false, Edge::None},   {0x16b3372, false, true, Edge::Right},
    {0x16b3398, true, false, Edge::Bottom},
};

struct Clip {
    std::size_t at;
    std::array<std::uint8_t, 3> game;
    std::size_t n;
};
constexpr Clip kLeftClips[] = {{0x1bfd473, {0x77, 0x30}, 2}, {0x1654dd9, {0x77, 0x2f}, 2},
                               {0x16b3370, {0x77, 0x5a}, 2}, {0x1642520, {0x0f, 0x96, 0xc1}, 3}};
constexpr Clip kTopClips[] = {{0x1bfd4cf, {0x77, 0x14}, 2}, {0x1654e1a, {0x77, 0x14}, 2},
                              {0x16b3396, {0x77, 0x34}, 2}, {0x164253c, {0x77, 0x20}, 2}};
constexpr std::size_t kBounds = 0x4926ea4;
constexpr std::size_t kPlates[] = {0x48f9a00, 0x48f9a60, 0x48f9ad0};

constexpr std::size_t kCameraBlend = 0x14368b0;
constexpr std::uint8_t kCameraBlendEntry[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41,
                                              0x55, 0x41, 0x54, 0x53, 0x48, 0x83, 0xec, 0x48};
constexpr std::size_t kCameraA = 0x60, kCameraB = 0x68, kAspect = 0x54;

std::uint8_t* g_image = nullptr;
std::size_t g_size = 0;
bool g_installed = false;
std::atomic<float> g_aspect{kGameAspect}, g_replaced{kGameAspect};
float g_applied = 0;

bool Matches(std::size_t at, const void* bytes, std::size_t n) {
    return at <= g_size && n <= g_size - at && std::memcmp(g_image + at, bytes, n) == 0;
}

std::array<std::uint8_t, 9> Original(const StageRead& r) {
    std::array<std::uint8_t, 9> b = {0x48, 0x8d, std::uint8_t(r.ecx ? 0x0d : 0x05), 0, 0, 0, 0, 0x8b,
                                     std::uint8_t(r.ecx ? 0x09 : 0x00)};
    const auto d = std::int32_t(std::int64_t(r.height ? kStageHeightAt : kStageWidthAt) - std::int64_t(r.at + 7));
    std::memcpy(b.data() + 3, &d, 4);
    return b;
}
std::array<std::uint8_t, 9> Pinned(const StageRead& r, std::uint32_t value) {
    std::array<std::uint8_t, 9> b = {std::uint8_t(r.ecx ? 0xb9 : 0xb8), 0, 0, 0, 0, 0x0f, 0x1f, 0x40, 0x00};
    std::memcpy(b.data() + 1, &value, 4);
    return b;
}
std::uint32_t Native(const StageRead& r) { return r.height ? kStageHeight : kStageWidth; }

void SetClip(const Clip& c, bool open) {
    static constexpr std::uint8_t kNop2[] = {0x66, 0x90}, kAlwaysCl[] = {0xb1, 0x01, 0x90};
    const std::uint8_t* bytes = !open ? c.game.data() : c.n == 3 ? kAlwaysCl : kNop2;
    WriteCode(g_image + c.at, bytes, c.n);
}

void FixCamera(std::uint64_t camera) {
    float now = 0;
    if (!camera || !BbPlatform::ReadProcessMemory(reinterpret_cast<const void*>(camera + kAspect), &now, sizeof now))
        return;
    const float want = g_aspect.load(std::memory_order_relaxed);
    if (now != want && (std::fabs(now - kGameAspect) < 1e-4f || now == g_replaced.load(std::memory_order_relaxed)))
        std::memcpy(reinterpret_cast<void*>(camera + kAspect), &want, sizeof want);
}

std::int64_t __attribute__((sysv_abi)) BeforeCameraBlend(std::uint64_t, const std::uint64_t* saved) {
    const std::uint64_t manager = saved[5];
    if (!manager) return 0;
    for (const std::size_t at : {kCameraA, kCameraB}) {
        std::uint64_t camera = 0;
        if (BbPlatform::ReadProcessMemory(reinterpret_cast<const void*>(manager + at), &camera, sizeof camera))
            FixCamera(camera);
    }
    FixCamera(manager);
    return 0;
}

void ApplyStage(float aspect) {
    const float rx = std::max(1.0f, aspect / kGameAspect), ry = std::max(1.0f, kGameAspect / aspect);
    const auto right = std::uint32_t(kStageWidth + std::ceil(kStageWidth / 2 * (rx - 1)));
    const auto bottom = std::uint32_t(kStageHeight + std::ceil(kStageHeight / 2 * (ry - 1)));
    const float scales[] = {0.5f * rx, -0.5f * ry}, bounds[] = {float(right), float(bottom)};
    for (const std::size_t at : kPlates) WriteCode(g_image + at, scales, sizeof scales);
    WriteCode(g_image + kBounds, bounds, sizeof bounds);
    for (const auto& r : kStageReads) {
        if (r.edge == Edge::None) continue;
        const std::uint32_t edge = r.edge == Edge::Right ? right : bottom;
        WriteCode(g_image + r.at + 1, &edge, sizeof edge);
    }
    for (const auto& c : kLeftClips) SetClip(c, rx > 1.0005f);
    for (const auto& c : kTopClips) SetClip(c, ry > 1.0005f);
    std::printf("Engine: camera aspect %.4f, world plates x%u / y%u, %s\n", aspect, right, bottom,
                ry <= 1.0005f ? "vertical FOV preserved"
                : Camera::Active() ? "horizontal FOV preserved (Vert+)"
                                   : "vertical FOV preserved (narrower view: camera hook off)");
}
}

bool Install(std::uint8_t* image, std::size_t size) {
    g_image = image;
    g_size = size;
    if (!BbSettings::Get().widescreen) return false;
    for (const auto& r : kStageReads) {
        if (!Matches(r.at, Original(r).data(), 9) && !Matches(r.at, Pinned(r, Native(r)).data(), 9)) {
            std::printf("Engine: widescreen off: the UI's stage code at %zx is not 1.09's\n", r.at);
            return false;
        }
    }
    const float scales[] = {0.5f, -0.5f}, bounds[] = {float(kStageWidth), float(kStageHeight)};
    const auto off = [](const char* what, std::size_t at) {
        std::printf("Engine: widescreen off: %s at %zx is not 1.09's\n", what, at);
        return false;
    };
    for (const std::size_t at : kPlates)
        if (!Matches(at, scales, sizeof scales)) return off("a UI plate scale", at);
    if (!Matches(kBounds, bounds, sizeof bounds)) return off("the UI stage bounds", kBounds);
    for (const auto& c : kLeftClips) if (!Matches(c.at, c.game.data(), c.n)) return off("a left clip", c.at);
    for (const auto& c : kTopClips) if (!Matches(c.at, c.game.data(), c.n)) return off("a top clip", c.at);
    Hook hook;
    if (!Prepare(hook, image, size, kCameraBlend, kCameraBlendEntry, BeforeCameraBlend))
        return off("the camera blend entry", kCameraBlend);
    for (const auto& r : kStageReads) std::memcpy(image + r.at, Pinned(r, Native(r)).data(), 9);
    Commit(std::span(&hook, 1));
    g_installed = true;
    Tick();
    std::printf("Engine: widescreen enabled: native camera aspect and %zu Scaleform stage pins\n",
                std::size(kStageReads));
    return true;
}

void Tick() {
    if (!g_installed) return;
    const auto& s = BbSettings::Get();
    const int index = std::clamp(BbSettings::ResolutionNeedsRestart() ? s.startup_output_res : s.output_res.load(), 0,
                                 BbSettings::OutputCount - 1);
    const float want = s.widescreen ? float(BbSettings::OutputWidth(index)) / float(BbSettings::OutputHeight(index))
                                    : kGameAspect;
    if (!std::isfinite(want) || want <= 0 || want == g_applied) return;
    g_replaced = g_aspect.load();
    g_aspect = want;
    ApplyStage(want);
    g_applied = want;
}

float NarrowFovFactor() {
    if (!g_installed) return 1.0f;
    const float a = g_aspect.load();
    return a > 0.1f && a < kGameAspect ? kGameAspect / a : 1.0f;
}
}
