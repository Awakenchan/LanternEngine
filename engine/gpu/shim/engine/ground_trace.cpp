// SPDX-License-Identifier: GPL-3.0-or-later
#include "ground_trace.h"
#include "engine_hooks.h"
#include "engine_state.h"
#include "bbport_platform.h"
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <span>

namespace BbEngine::GroundTrace {
namespace {
constexpr std::size_t kGroundProbe = 0x1a4a730;
const std::uint8_t kGroundProbePrologue[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55,
                                             0x41, 0x54, 0x53, 0x48, 0x81, 0xec, 0xe8, 0x09, 0x00, 0x00};
constexpr std::size_t kWorldChrMan = 0x553e878;
constexpr std::uint32_t kMaxLines = 40000;

std::uint64_t g_image = 0;
std::atomic<std::uint32_t> g_lines{0};
const auto g_start = std::chrono::steady_clock::now();
float g_last_y = 0.0f;
std::uint64_t g_last_proxy = 0;

template <typename T> T Read(std::uint64_t at, T fallback = T{}) {
    T value{};
    return at && BbPlatform::ReadProcessMemory(reinterpret_cast<const void*>(at), &value, sizeof(T)) ? value
                                                                                                     : fallback;
}

std::int64_t __attribute__((sysv_abi)) OnGroundProbe(std::uint64_t, const std::uint64_t* saved) {
    const std::uint64_t proxy = saved[5];
    const std::uint64_t step = saved[4];
    const std::uint64_t local = Read<std::uint64_t>(Read<std::uint64_t>(g_image + kWorldChrMan) + 0x60);
    if (!proxy || !local || Read<std::uint64_t>(proxy + 8) != local) return 0;
    const float x = Read<float>(proxy + 0x1e0), y = Read<float>(proxy + 0x1e4), z = Read<float>(proxy + 0x1e8);
    if (proxy == g_last_proxy && std::fabs(y - g_last_y) < 1e-5f && std::fabs(Read<float>(proxy + 0x1a4)) < 1e-5f) {
        const float vx = Read<float>(proxy + 0x250), vz = Read<float>(proxy + 0x258);
        if (vx * vx + vz * vz < 1e-6f) return 0;
    }
    g_last_proxy = proxy;
    g_last_y = y;
    const std::uint32_t n = g_lines.fetch_add(1, std::memory_order_relaxed);
    if (n >= kMaxLines) {
        if (n == kMaxLines) host_log("Ground trace: %u lines written; the rest of the session is not traced", kMaxLines);
        return 0;
    }
    const std::uint64_t display = Read<std::uint64_t>(Read<std::uint64_t>(proxy + 0x218) + 0x38);
    const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_start).count();
    host_log("Ground: t %.4f dt %.5f pos %.4f %.4f %.4f snap %.4f move %.4f shown %.4f vel %.3f %.3f %.3f "
             "slide %.3f %.3f %.3f slope %.1f flag %u grounded %u touched %u riding %u radius %.3f reach %.3f %.3f "
             "w %.3f",
             t, double(Read<float>(step + 8)), double(x), double(y), double(z), double(Read<float>(proxy + 0x274)),
             double(Read<float>(proxy + 0x2e4)), display ? double(Read<float>(display + 0x74)) : -9999.0,
             double(Read<float>(proxy + 0x250)), double(Read<float>(proxy + 0x254)),
             double(Read<float>(proxy + 0x258)), double(Read<float>(proxy + 0x1a0)),
             double(Read<float>(proxy + 0x1a4)), double(Read<float>(proxy + 0x1a8)),
             double(Read<float>(proxy + 0x1bc)), unsigned(Read<std::uint8_t>(proxy + 0x1b8)),
             unsigned(Read<std::uint8_t>(proxy + 0x212)), unsigned(Read<std::uint8_t>(proxy + 0x321)),
             unsigned(Read<std::uint8_t>(proxy + 0x328)), double(Read<float>(proxy + 0x358)),
             double(Read<float>(proxy + 0x294)), double(Read<float>(proxy + 0x298)),
             double(Read<float>(proxy + 0x31c)));
    return 0;
}
}

bool Install(std::uint8_t* image, std::size_t size) {
    const char* on = std::getenv("BB_GROUND_TRACE");
    if (!on || on[0] != '1') return false;
    Hook hook;
    if (!Prepare(hook, image, size, kGroundProbe, kGroundProbePrologue, OnGroundProbe)) {
        host_log("Ground trace off: the ground probe differs from 1.09");
        return false;
    }
    g_image = reinterpret_cast<std::uint64_t>(image);
    Commit(std::span{&hook, 1});
    host_log("Ground trace: on (BB_GROUND_TRACE), a line for each frame the hunter moves, at most %u", kMaxLines);
    return true;
}
}
