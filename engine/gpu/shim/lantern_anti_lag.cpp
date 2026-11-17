// SPDX-License-Identifier: GPL-3.0-or-later
#include "lantern_anti_lag.h"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <shared_mutex>
#include "bbport_settings.h"
#include "lantern_anti_lag_pacer.h"

namespace LanternAntiLag {
namespace {

std::shared_mutex lifetime;
std::unique_ptr<Pacer> pacer;
std::atomic<bool> running{false};

Limits ConfiguredLimits() {
    Limits limits;
    if (const char* env = std::getenv("BB_ANTI_LAG_WAIT_MS"); env && env[0]) {
        limits.wait = std::chrono::milliseconds(std::clamp(std::atoi(env), 5, 500));
    }
    return limits;
}

} // namespace

bool Allowed() {
    const char* env = std::getenv("BB_ANTI_LAG");
    return !(env && env[0] == '0');
}

void Start(VkDevice device, PFN_vkGetDeviceProcAddr resolve) {
    const auto update =
        reinterpret_cast<PFN_vkAntiLagUpdateAMD>(resolve(device, "vkAntiLagUpdateAMD"));
    if (!update) {
        std::printf("AMD Anti-Lag 2: the driver lists it without its function; off\n");
        return;
    }
    const Limits limits = ConfiguredLimits();
    std::unique_lock lock{lifetime};
    pacer = std::make_unique<Pacer>(
        [device, update](Stage stage, std::uint64_t frame) {
            const VkAntiLagPresentationInfoAMD presentation{
                .sType = VK_STRUCTURE_TYPE_ANTI_LAG_PRESENTATION_INFO_AMD,
                .pNext = nullptr,
                .stage = stage == Stage::Present ? VK_ANTI_LAG_STAGE_PRESENT_AMD
                                                 : VK_ANTI_LAG_STAGE_INPUT_AMD,
                .frameIndex = frame,
            };
            const VkAntiLagDataAMD data{
                .sType = VK_STRUCTURE_TYPE_ANTI_LAG_DATA_AMD,
                .pNext = nullptr,
                .mode = stage == Stage::Off ? VK_ANTI_LAG_MODE_OFF_AMD : VK_ANTI_LAG_MODE_ON_AMD,
                .maxFPS = 0,
                .pPresentationInfo = stage == Stage::Off ? nullptr : &presentation,
            };
            update(device, &data);
        },
        limits);
    running.store(true, std::memory_order_release);
    BbSettings::Get().anti_lag_supported = true;
    std::printf("AMD Anti-Lag 2: offered by the driver, setting %s, driver waits cut at %lld ms "
                "(BB_ANTI_LAG=0: never on; BB_ANTI_LAG_WAIT_MS)\n",
                BbSettings::Get().anti_lag.load() ? "on" : "off",
                static_cast<long long>(limits.wait.count()));
}

void BeforePresent() {
    if (!running.load(std::memory_order_acquire)) {
        return;
    }
    std::shared_lock lock{lifetime};
    if (pacer) {
        pacer->BeforePresent();
    }
}

void Shutdown() {
    running.store(false, std::memory_order_release);
    std::unique_lock lock{lifetime};
    pacer.reset();
    BbSettings::Get().anti_lag_supported = false;
}

} // namespace LanternAntiLag

extern "C" void bbgpu_anti_lag_before_input(void) {
    using namespace LanternAntiLag;
    if (!running.load(std::memory_order_acquire)) {
        return;
    }
    std::shared_lock lock{lifetime};
    if (pacer) {
        const auto& s = BbSettings::Get();
        pacer->BeforeInput(s.anti_lag.load(std::memory_order_relaxed) &&
                           !s.fsr_fg_running.load(std::memory_order_relaxed));
    }
}
