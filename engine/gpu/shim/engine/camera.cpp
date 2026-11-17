// SPDX-License-Identifier: GPL-3.0-or-later
#include "camera.h"
#include "widescreen.h"
#include "engine_hooks.h"
#include "bbport_platform.h"
#include "bbport_settings.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <span>
#include <vector>

namespace BbEngine::Camera {
namespace {
constexpr std::size_t kRepositoryLoad = 0x143acf2;
constexpr std::uint8_t kLeaR14[] = {0x4c, 0x8d, 0x35};
constexpr std::size_t kSlotCount = 0x143ad35;
constexpr std::uint8_t kSlotCountCode[] = {0x83, 0xb8, 0xb8, 0x09, 0x00, 0x00, 0x00};
constexpr std::size_t kSlotFirst = 0x143ad42;
constexpr std::uint8_t kSlotFirstCode[] = {0x48, 0x8b, 0x80, 0xc0, 0x09, 0x00, 0x00};
constexpr std::size_t kCountAt = 0x9b8, kFirstAt = 0x9c0, kHolder = 0x70, kFile = 0x70;

constexpr std::size_t kFovCap = 0x143af4e;
constexpr std::uint8_t kCapped[] = {0x48, 0x0f, 0x46, 0xc6};
constexpr std::uint8_t kUncapped[] = {0x48, 0x89, 0xf0, 0x90};

constexpr char kType[] = "LOCK_CAM_PARAM_ST";
constexpr std::size_t kFields[3] = {0x0, 0xc, 0x14};

std::uint64_t g_repository = 0;
bool g_active = false;
bool g_controls = false;

struct Row {
    std::uint64_t at;
    std::array<float, 3> source, written;
};
std::uint64_t g_file = 0;
std::vector<Row> g_rows;
std::array<float, 4> g_said{1, 1, 1, 1};
unsigned g_ticks = 0;

template <typename T> bool Peek(std::uint64_t at, T* out) {
    return at && BbPlatform::ReadProcessMemory(reinterpret_cast<const void*>(at), out, sizeof(T));
}
std::uint64_t Follow(std::uint64_t at) {
    std::uint64_t v = 0;
    return Peek(at, &v) ? v : 0;
}
bool IsLockCam(std::uint64_t file) {
    char type[sizeof kType]{};
    return file && BbPlatform::ReadProcessMemory(reinterpret_cast<const void*>(file + 0xc), type, sizeof type) &&
           std::memcmp(type, kType, sizeof kType) == 0;
}

std::uint64_t FindFile() {
    const std::uint64_t repository = Follow(g_repository);
    std::int32_t count = 0;
    if (!repository || !Peek(repository + kCountAt, &count) || count <= 0) return 0;
    const std::uint64_t file = Follow(Follow(Follow(repository + kFirstAt) + kHolder) + kFile);
    return IsLockCam(file) ? file : 0;
}

bool ReadRows(std::uint64_t file) {
    std::uint16_t count = 0;
    std::uint8_t format = 0, flags = 0;
    if (!Peek(file + 0xa, &count) || !count || count > 4096 || !Peek(file + 0x2d, &format) || !Peek(file + 0x2e, &flags))
        return false;
    const bool wide = format > 3 && (flags & 2);
    const std::size_t stride = wide ? 0x18 : 12;
    std::vector<Row> rows;
    rows.reserve(count);
    for (unsigned i = 0; i < count; ++i) {
        std::uint64_t offset = 0;
        if (wide) {
            if (!Peek(file + 0x40 + stride * i + 8, &offset)) return false;
        } else {
            std::uint32_t o = 0;
            if (!Peek(file + 0x40 + stride * i + 4, &o)) return false;
            offset = o;
        }
        if (offset < 0x40 + stride * count || offset > 32u << 20) return false;
        Row row{file + offset, {}, {}};
        for (unsigned f = 0; f < 3; ++f)
            if (!Peek(row.at + kFields[f], &row.source[f]) || !std::isfinite(row.source[f])) return false;
        if (!(row.source[2] > 1 && row.source[2] < 179)) return false;
        row.written = row.source;
        rows.push_back(row);
    }
    g_rows = std::move(rows);
    g_file = file;
    std::printf("Engine: camera: LockCamParam, %u rows\n", unsigned(count));
    return true;
}

float Value(float source, unsigned field, const std::array<float, 4>& scale) {
    float v = source * scale[field];
    if (field == 2 && scale[3] > 1.0f) {
        constexpr float kRad = 3.14159265f / 180.0f;
        v = std::min(170.0f, 2.0f * std::atan(std::tan(v * kRad * 0.5f) * scale[3]) / kRad);
    }
    return v;
}
}

bool Install(std::uint8_t* image, std::size_t size) {
    const auto& s = BbSettings::Get();
    const int w = BbSettings::OutputWidth(s.startup_output_res), h = BbSettings::OutputHeight(s.startup_output_res);
    const bool narrow = s.widescreen && w > 0 && h > 0 && std::int64_t(w) * 9 < std::int64_t(h) * 16;
    g_controls = s.camera_controls;
    if (!g_controls && !narrow) return false;
    auto matches = [&](std::size_t at, const std::uint8_t* b, std::size_t n) {
        return at <= size && n <= size - at && std::memcmp(image + at, b, n) == 0;
    };
    if (!matches(kRepositoryLoad, kLeaR14, sizeof kLeaR14) || !matches(kSlotCount, kSlotCountCode, sizeof kSlotCountCode) ||
        !matches(kSlotFirst, kSlotFirstCode, sizeof kSlotFirstCode) ||
        (!matches(kFovCap, kCapped, sizeof kCapped) && !matches(kFovCap, kUncapped, sizeof kUncapped))) {
        std::printf("Engine: camera off: the follow camera's code is not 1.09's\n");
        return false;
    }
    std::int32_t d = 0;
    std::memcpy(&d, image + kRepositoryLoad + sizeof kLeaR14, 4);
    const std::size_t repository = std::size_t(std::int64_t(kRepositoryLoad + sizeof kLeaR14 + 4) + d);
    if (repository > size - 8) return false;
    const bool capped = matches(kFovCap, kCapped, sizeof kCapped);
    if (const WriteResult result = WriteCode({image, kFovCap, capped ? std::span(kCapped) : std::span(kUncapped), kUncapped});
        result != WriteResult::Applied) {
        std::printf("Engine: camera off: the follow camera's FOV cap %s\n", Describe(result));
        return false;
    }
    g_repository = reinterpret_cast<std::uint64_t>(image + repository);
    g_active = true;
    std::printf(g_controls ? "Engine: camera adjustments enabled (distance, height and FOV)\n"
                           : "Engine: camera FOV for a screen narrower than 16:9 (Vert+)\n");
    return true;
}

bool Active() { return g_active; }

void Tick() {
    if (!g_active) return;
    const auto& s = BbSettings::Get();
    const std::array<float, 4> scale = {g_controls ? s.camera_distance_scale.load() : 1.0f,
                                        g_controls ? s.camera_height_scale.load() : 1.0f,
                                        g_controls ? s.camera_fov_scale.load() : 1.0f, Widescreen::NarrowFovFactor()};
    for (const float f : scale) if (!std::isfinite(f)) return;
    const bool neutral = scale == std::array<float, 4>{1, 1, 1, 1};
    if (neutral && g_said == scale) return;
    if (!g_file || g_ticks++ % 150 == 0) {
        const std::uint64_t file = FindFile();
        if (!file) return;
        if (file != g_file && !ReadRows(file)) return;
    }
    if (!IsLockCam(g_file)) {
        g_file = 0;
        g_rows.clear();
        return;
    }
    for (Row& row : g_rows) {
        for (unsigned f = 0; f < 3; ++f) {
            float now = 0;
            if (!Peek(row.at + kFields[f], &now) || !std::isfinite(now)) {
                g_file = 0;
                g_rows.clear();
                return;
            }
            if (now != row.written[f]) row.source[f] = now;
            const float want = Value(row.source[f], f, scale);
            if (now != want) std::memcpy(reinterpret_cast<void*>(row.at + kFields[f]), &want, sizeof want);
            row.written[f] = want;
        }
    }
    if (scale != g_said) {
        std::printf("Engine: camera distance x%.2f, height x%.2f, FOV x%.2f, Vert+ x%.4f\n", scale[0], scale[1],
                    scale[2], scale[3]);
        g_said = scale;
    }
}
}
