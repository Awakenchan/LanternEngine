// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vulkan/vulkan_core.h>

namespace Vulkan::PresentPolicy {

#if defined(PRESENT_POLICY_FAULTS)
inline int& PresentPolicyFaultSelector() {
    static int selected = 0;
    return selected;
}
#define PRESENT_POLICY_FAULT(n) (::Vulkan::PresentPolicy::PresentPolicyFaultSelector() == (n))
#else
#define PRESENT_POLICY_FAULT(n) false
#endif

inline constexpr const char* kPolicySwitch = "LANTERN_PRESENT_POLICY";

inline constexpr std::uint64_t kPresentWaitTimeoutMs = 500;
inline constexpr std::uint64_t kPresentWaitTimeoutNs =
    std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::milliseconds{kPresentWaitTimeoutMs})
        .count();
inline constexpr std::uint64_t kStickyRecheckIntervalNs =
    std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::seconds{1}).count();
inline constexpr std::uint64_t kInfiniteWaitNs = ~std::uint64_t{0};

enum class PolicyMode : std::uint8_t { Old, New };

[[nodiscard]] inline PolicyMode PolicyModeFromSwitch(const char* value) {
    const bool on = value != nullptr && value[0] == '1';
    return on != PRESENT_POLICY_FAULT(1) ? PolicyMode::New : PolicyMode::Old;
}

enum class SurfaceResult : std::uint8_t {
    Success,
    Suboptimal,
    Timeout,
    NotReady,
    OutOfDate,
    SurfaceLost,
    Failed,
};

enum class PresentMode : std::uint8_t {
    Immediate,
    Mailbox,
    Fifo,
    FifoRelaxed,
    Unknown,
};

[[nodiscard]] inline SurfaceResult ToSurfaceResult(VkResult result) {
    switch (result) {
    case VK_SUCCESS:
        return SurfaceResult::Success;
    case VK_SUBOPTIMAL_KHR:
        return PRESENT_POLICY_FAULT(2) ? SurfaceResult::Success : SurfaceResult::Suboptimal;
    case VK_TIMEOUT:
        return SurfaceResult::Timeout;
    case VK_NOT_READY:
        return SurfaceResult::NotReady;
    case VK_ERROR_OUT_OF_DATE_KHR:
        return SurfaceResult::OutOfDate;
    case VK_ERROR_SURFACE_LOST_KHR:
        return SurfaceResult::SurfaceLost;
    default:
        return SurfaceResult::Failed;
    }
}

[[nodiscard]] inline PresentMode ToPolicyMode(VkPresentModeKHR mode) {
    switch (mode) {
    case VK_PRESENT_MODE_IMMEDIATE_KHR:
        return PresentMode::Immediate;
    case VK_PRESENT_MODE_MAILBOX_KHR:
        return PRESENT_POLICY_FAULT(3) ? PresentMode::Unknown : PresentMode::Mailbox;
    case VK_PRESENT_MODE_FIFO_KHR:
        return PresentMode::Fifo;
    case VK_PRESENT_MODE_FIFO_RELAXED_KHR:
        return PresentMode::FifoRelaxed;
    default:
        return PresentMode::Unknown;
    }
}

[[nodiscard]] inline VkPresentModeKHR ToVkMode(PresentMode mode) {
    switch (mode) {
    case PresentMode::Immediate:
        return VK_PRESENT_MODE_IMMEDIATE_KHR;
    case PresentMode::Mailbox:
        return VK_PRESENT_MODE_MAILBOX_KHR;
    case PresentMode::FifoRelaxed:
        return PRESENT_POLICY_FAULT(4) ? VK_PRESENT_MODE_FIFO_KHR
                                       : VK_PRESENT_MODE_FIFO_RELAXED_KHR;
    case PresentMode::Fifo:
    case PresentMode::Unknown:
        break;
    }
    return VK_PRESENT_MODE_FIFO_KHR;
}

[[nodiscard]] inline PresentMode PresentModeFromName(std::string_view name) {
    if (name == "Immediate") {
        return PresentMode::Immediate;
    }
    if (name == "Mailbox") {
        return PresentMode::Mailbox;
    }
    if (name == "Fifo") {
        return PresentMode::Fifo;
    }
    if (name == "FifoRelaxed") {
        return PresentMode::FifoRelaxed;
    }
    return PresentMode::Unknown;
}

struct SurfaceExtent {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    friend bool operator==(const SurfaceExtent&, const SurfaceExtent&) = default;
};

struct SurfaceState {
    SurfaceExtent extent;
    bool hdr_offered = false;
};

[[nodiscard]] inline bool SameSurface(const SurfaceState& a, const SurfaceState& b) {
    return a.extent == b.extent && (a.hdr_offered == b.hdr_offered || PRESENT_POLICY_FAULT(5));
}

struct PresentHealth {
    std::atomic<std::uint64_t> acquire_timeouts{0};
    std::atomic<std::uint64_t> suboptimal_reports{0};
    std::atomic<std::uint64_t> suboptimal_changed_rebuilds{0};
    std::atomic<std::uint64_t> suboptimal_unknown_rebuilds{0};
    std::atomic<std::uint64_t> suboptimal_latch_rebuilds{0};
    std::atomic<std::uint64_t> suboptimal_rechecks{0};
    std::atomic<std::uint64_t> suboptimal_recheck_failures{0};
    std::atomic<std::uint64_t> render_frame_wait_timeouts{0};
    std::atomic<std::uint64_t> redraw_wait_timeouts{0};
};

struct SuboptimalEpisode {
    bool open = false;
    bool rebuild_requested = false;
    bool rebuilt = false;
    std::uint64_t last_check_ns = 0;
};

struct State {
    State() = default;
    explicit State(PolicyMode policy_mode) : mode(policy_mode) {}
    PolicyMode mode = PolicyMode::Old;
    SuboptimalEpisode episode;
    PresentHealth health;
};

class SwapchainProbe {
public:
    SwapchainProbe() = default;
    SwapchainProbe(const SwapchainProbe&) = delete;
    SwapchainProbe& operator=(const SwapchainProbe&) = delete;
    virtual ~SwapchainProbe() = default;
    [[nodiscard]] virtual SurfaceResult AcquireImage(std::uint64_t timeout_ns,
                                                     std::uint32_t& image_index) = 0;
    [[nodiscard]] virtual std::optional<SurfaceState> CurrentSurface() const = 0;
};

enum class AcquireStep : std::uint8_t { Acquired, Skip, Rebuild };

struct AcquireOutcome {
    AcquireStep step = AcquireStep::Rebuild;
    SurfaceResult result = SurfaceResult::Failed;
    bool rebuild_after_present = false;
    bool first_timeout = false;
};

[[nodiscard]] inline bool CountFirst(std::atomic<std::uint64_t>& counter) {
    const bool first = counter.fetch_add(1, std::memory_order_relaxed) == 0;
    return first || PRESENT_POLICY_FAULT(6);
}

[[nodiscard]] inline std::uint64_t AcquireTimeoutNs(PolicyMode mode) {
    const bool finite = mode == PolicyMode::New && !PRESENT_POLICY_FAULT(7);
    return finite || PRESENT_POLICY_FAULT(8) ? kPresentWaitTimeoutNs : kInfiniteWaitNs;
}

[[nodiscard]] inline bool RedrawGivesUp(PolicyMode mode) {
    return (mode == PolicyMode::New) != PRESENT_POLICY_FAULT(9);
}

[[nodiscard]] inline std::uint64_t RedrawWaitNs(PolicyMode mode) {
    return RedrawGivesUp(mode) ? kPresentWaitTimeoutNs : kInfiniteWaitNs;
}

[[nodiscard]] inline std::uint64_t RenderFrameWaitNs(PolicyMode mode) {
    return (mode == PolicyMode::New) != PRESENT_POLICY_FAULT(25) ? kPresentWaitTimeoutNs : kInfiniteWaitNs;
}

[[nodiscard]] inline bool RecheckSticky(SwapchainProbe& probe, const SurfaceState& built,
                                        State& state, std::uint64_t now_ns) {
    SuboptimalEpisode& episode = state.episode;
    if (PRESENT_POLICY_FAULT(10)) {
        return false;
    }
    const bool due = now_ns >= episode.last_check_ns + kStickyRecheckIntervalNs;
    if (!due && !PRESENT_POLICY_FAULT(11)) {
        return false;
    }
    episode.last_check_ns = now_ns;
    state.health.suboptimal_rechecks.fetch_add(1, std::memory_order_relaxed);
    const std::optional<SurfaceState> current = probe.CurrentSurface();
    if (!current) {
        state.health.suboptimal_recheck_failures.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    if (SameSurface(*current, built)) {
        return false;
    }
    episode.rebuild_requested = true;
    state.health.suboptimal_changed_rebuilds.fetch_add(1, std::memory_order_relaxed);
    return true;
}

[[nodiscard]] inline bool OnSuboptimal(SwapchainProbe& probe, const SurfaceState& built,
                                       State& state, std::uint64_t now_ns) {
    state.health.suboptimal_reports.fetch_add(1, std::memory_order_relaxed);
    if (state.mode == PolicyMode::Old) {
        return !PRESENT_POLICY_FAULT(12);
    }
    SuboptimalEpisode& episode = state.episode;
    if (PRESENT_POLICY_FAULT(13)) {
        return false;
    }
    if (episode.rebuild_requested) {
        return true;
    }
    if (episode.open && episode.rebuilt && !PRESENT_POLICY_FAULT(14)) {
        return RecheckSticky(probe, built, state, now_ns);
    }
    episode.open = true;
    episode.rebuild_requested = true;
    episode.last_check_ns = now_ns;
    const std::optional<SurfaceState> current = probe.CurrentSurface();
    if (!current) {
        state.health.suboptimal_unknown_rebuilds.fetch_add(1, std::memory_order_relaxed);
    } else if (SameSurface(*current, built)) {
        state.health.suboptimal_latch_rebuilds.fetch_add(1, std::memory_order_relaxed);
    } else {
        state.health.suboptimal_changed_rebuilds.fetch_add(1, std::memory_order_relaxed);
    }
    return true;
}

inline void OnSwapchainCreated(State& state) {
    if (PRESENT_POLICY_FAULT(15)) {
        return;
    }
    state.episode.rebuilt = state.episode.open;
    state.episode.rebuild_requested = false;
}

inline void OnPresentSucceeded(State& state) {
    if (PRESENT_POLICY_FAULT(16)) {
        return;
    }
    state.episode = {};
}

[[nodiscard]] inline AcquireOutcome AcquireImage(SwapchainProbe& probe, const SurfaceState& built,
                                                 State& state, std::uint64_t now_ns,
                                                 std::uint32_t& image_index) {
    const SurfaceResult result = probe.AcquireImage(AcquireTimeoutNs(state.mode), image_index);
    switch (result) {
    case SurfaceResult::Success:
        return {AcquireStep::Acquired, result, false, false};
    case SurfaceResult::Suboptimal:
        return {AcquireStep::Acquired, result, OnSuboptimal(probe, built, state, now_ns), false};
    case SurfaceResult::Timeout:
    case SurfaceResult::NotReady:
        if (state.mode == PolicyMode::Old && !PRESENT_POLICY_FAULT(17)) {
            break;
        }
        if (PRESENT_POLICY_FAULT(18)) {
            return {AcquireStep::Rebuild, result, false, false};
        }
        return {AcquireStep::Skip, result, false, CountFirst(state.health.acquire_timeouts)};
    case SurfaceResult::OutOfDate:
    case SurfaceResult::SurfaceLost:
    case SurfaceResult::Failed:
        break;
    }
    if (PRESENT_POLICY_FAULT(19)) {
        return {AcquireStep::Skip, result, false, false};
    }
    return {AcquireStep::Rebuild, result, false, false};
}

struct PresentModeRow {
    PresentMode requested;
    PresentMode second;
};

inline constexpr std::array kPresentModeRows{
    PresentModeRow{PresentMode::Immediate, PresentMode::Mailbox},
    PresentModeRow{PresentMode::Mailbox, PresentMode::Fifo},
    PresentModeRow{PresentMode::FifoRelaxed, PresentMode::Fifo},
    PresentModeRow{PresentMode::Fifo, PresentMode::Fifo},
};

inline constexpr std::size_t kPresentModeChainLength = 3;

[[nodiscard]] inline std::array<PresentMode, kPresentModeChainLength> PresentModeChain(
    PolicyMode mode, PresentMode requested) {
    if (mode == PolicyMode::Old && !PRESENT_POLICY_FAULT(20)) {
        return {requested, PresentMode::Fifo, PresentMode::Fifo};
    }
    for (const PresentModeRow& row : kPresentModeRows) {
        if (row.requested == requested) {
            const PresentMode second =
                PRESENT_POLICY_FAULT(21) && requested == PresentMode::Mailbox ? PresentMode::Immediate
                                                                              : row.second;
            return {requested, second, PresentMode::Fifo};
        }
    }
    return {requested, PresentMode::Fifo, PresentMode::Fifo};
}

[[nodiscard]] inline PresentMode ChoosePresentMode(PolicyMode mode, PresentMode requested,
                                                   std::span<const PresentMode> supported) {
    for (const PresentMode candidate : PresentModeChain(mode, requested)) {
        for (const PresentMode offered : supported) {
            if (offered == candidate) {
                return candidate;
            }
        }
    }
    return PRESENT_POLICY_FAULT(22) ? requested : PresentMode::Fifo;
}

[[nodiscard]] inline PresentMode PacingPresentMode(PolicyMode mode, PresentMode requested,
                                                   PresentMode chosen) {
    const bool chosen_counts = mode == PolicyMode::New && !PRESENT_POLICY_FAULT(23);
    return chosen_counts || PRESENT_POLICY_FAULT(24) ? chosen : requested;
}

inline constexpr int kPresentPolicyFaultCount = 25;

}
