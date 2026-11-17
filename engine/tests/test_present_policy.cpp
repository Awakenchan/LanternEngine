// SPDX-License-Identifier: GPL-2.0-or-later
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <iterator>
#include <optional>
#include <vector>
#include "gpu/shadps4/video_core/renderer_vulkan/vk_present_policy.h"

using namespace Vulkan::PresentPolicy;

namespace {

struct Tally {
    int failures = 0;
    bool verbose = true;
};

Tally& Current() {
    static Tally tally;
    return tally;
}

void Check(bool ok, const char* what) {
    if (!ok) {
        if (Current().verbose) {
            std::printf("FAIL: %s\n", what);
        }
        ++Current().failures;
    }
}

class FakeSwapchain final : public SwapchainProbe {
public:
    std::deque<SurfaceResult> acquire_results;
    std::optional<SurfaceState> surface;
    std::vector<std::uint64_t> acquire_timeouts;
    mutable std::uint32_t surface_queries = 0;
    std::uint32_t next_index = 0;

    SurfaceResult AcquireImage(std::uint64_t timeout_ns, std::uint32_t& image_index) override {
        acquire_timeouts.push_back(timeout_ns);
        if (acquire_results.empty()) {
            return SurfaceResult::Failed;
        }
        const SurfaceResult result = acquire_results.front();
        acquire_results.pop_front();
        if (result == SurfaceResult::Success || result == SurfaceResult::Suboptimal) {
            image_index = next_index++;
        }
        return result;
    }

    std::optional<SurfaceState> CurrentSurface() const override {
        ++surface_queries;
        return surface;
    }
};

constexpr SurfaceState kBuilt{{1920, 1080}, false};
constexpr SurfaceState kLarger{{2560, 1440}, false};
constexpr SurfaceState kHdrOn{{1920, 1080}, true};
constexpr std::uint32_t kUntouchedIndex = 99;
constexpr int kStickyReports = 5;
constexpr int kErrorResults = 3;
constexpr std::uint64_t kStart = 1'000'000'000;
constexpr std::uint64_t kTick = 16'000'000;

void ModeSwitch() {
    Check(PolicyModeFromSwitch(nullptr) == PolicyMode::Old, "switch unset: old path");
    Check(PolicyModeFromSwitch("0") == PolicyMode::Old, "switch 0: old path");
    Check(PolicyModeFromSwitch("") == PolicyMode::Old, "switch empty: old path");
    Check(PolicyModeFromSwitch("1") == PolicyMode::New, "switch 1: new policy");
}

void Mappings() {
    Check(ToSurfaceResult(VK_SUCCESS) == SurfaceResult::Success, "success maps");
    Check(ToSurfaceResult(VK_SUBOPTIMAL_KHR) == SurfaceResult::Suboptimal, "suboptimal maps");
    Check(ToSurfaceResult(VK_TIMEOUT) == SurfaceResult::Timeout, "timeout maps");
    Check(ToSurfaceResult(VK_NOT_READY) == SurfaceResult::NotReady, "not ready maps");
    Check(ToSurfaceResult(VK_ERROR_OUT_OF_DATE_KHR) == SurfaceResult::OutOfDate, "out of date maps");
    Check(ToSurfaceResult(VK_ERROR_SURFACE_LOST_KHR) == SurfaceResult::SurfaceLost,
          "surface lost maps");
    Check(ToSurfaceResult(VK_ERROR_UNKNOWN) == SurfaceResult::Failed, "unknown error maps to failed");
    Check(ToSurfaceResult(VK_ERROR_DEVICE_LOST) == SurfaceResult::Failed, "device lost maps to failed");

    const VkPresentModeKHR vk_modes[] = {VK_PRESENT_MODE_IMMEDIATE_KHR, VK_PRESENT_MODE_MAILBOX_KHR,
                                         VK_PRESENT_MODE_FIFO_KHR, VK_PRESENT_MODE_FIFO_RELAXED_KHR};
    const PresentMode modes[] = {PresentMode::Immediate, PresentMode::Mailbox, PresentMode::Fifo,
                                 PresentMode::FifoRelaxed};
    for (std::size_t i = 0; i < std::size(modes); ++i) {
        Check(ToPolicyMode(vk_modes[i]) == modes[i], "Vulkan present mode maps to the policy mode");
        Check(ToVkMode(modes[i]) == vk_modes[i], "policy mode maps back to the Vulkan mode");
    }
    Check(ToPolicyMode(VK_PRESENT_MODE_SHARED_DEMAND_REFRESH_KHR) == PresentMode::Unknown,
          "other Vulkan modes are unknown");
    Check(ToVkMode(PresentMode::Unknown) == VK_PRESENT_MODE_FIFO_KHR, "unknown asks for FIFO");

    Check(PresentModeFromName("Immediate") == PresentMode::Immediate, "Immediate by name");
    Check(PresentModeFromName("Mailbox") == PresentMode::Mailbox, "Mailbox by name");
    Check(PresentModeFromName("Fifo") == PresentMode::Fifo, "Fifo by name");
    Check(PresentModeFromName("FifoRelaxed") == PresentMode::FifoRelaxed, "FifoRelaxed by name");
    Check(PresentModeFromName("immediate") == PresentMode::Unknown, "names are exact");
}

void OldPathDecisions() {
    State state{PolicyMode::Old};
    FakeSwapchain fake;
    fake.surface = kBuilt;
    fake.acquire_results = {SurfaceResult::Suboptimal, SurfaceResult::Timeout,
                            SurfaceResult::NotReady, SurfaceResult::OutOfDate,
                            SurfaceResult::Success};
    std::uint32_t index = kUntouchedIndex;

    const AcquireOutcome suboptimal = AcquireImage(fake, kBuilt, state, kStart, index);
    Check(suboptimal.step == AcquireStep::Acquired, "mode 0: a suboptimal image is presented");
    Check(suboptimal.rebuild_after_present, "mode 0: a suboptimal acquire rebuilds after it");
    Check(AcquireImage(fake, kBuilt, state, kStart, index).step == AcquireStep::Rebuild,
          "mode 0: a timeout rebuilds as before");
    Check(AcquireImage(fake, kBuilt, state, kStart, index).step == AcquireStep::Rebuild,
          "mode 0: not ready rebuilds as before");
    Check(AcquireImage(fake, kBuilt, state, kStart, index).step == AcquireStep::Rebuild,
          "mode 0: out of date rebuilds");
    Check(AcquireImage(fake, kBuilt, state, kStart, index).step == AcquireStep::Acquired,
          "mode 0: success acquires");
    for (const std::uint64_t timeout : fake.acquire_timeouts) {
        Check(timeout == kInfiniteWaitNs, "mode 0: acquire waits without a timeout");
    }
    Check(state.health.acquire_timeouts.load() == 0, "mode 0: nothing counted as a skip");

    OnSwapchainCreated(state);
    for (int i = 0; i < kStickyReports; ++i) {
        Check(OnSuboptimal(fake, kBuilt, state, kStart + i * kTick),
              "mode 0: every suboptimal present rebuilds");
    }
    Check(fake.surface_queries == 0, "mode 0: the surface is never queried mid-frame");

    const std::vector<PresentMode> no_immediate{PresentMode::Mailbox, PresentMode::Fifo};
    const std::vector<PresentMode> fifo_only{PresentMode::Fifo};
    Check(ChoosePresentMode(PolicyMode::Old, PresentMode::Immediate, no_immediate) ==
              PresentMode::Fifo,
          "mode 0: immediate unsupported falls back to FIFO");
    Check(ChoosePresentMode(PolicyMode::Old, PresentMode::Mailbox, no_immediate) ==
              PresentMode::Mailbox,
          "mode 0: a supported request is kept");
    Check(ChoosePresentMode(PolicyMode::Old, PresentMode::FifoRelaxed, fifo_only) ==
              PresentMode::Fifo,
          "mode 0: FIFO relaxed unsupported falls back to FIFO");

    Check(PacingPresentMode(PolicyMode::Old, PresentMode::Immediate, PresentMode::Fifo) ==
              PresentMode::Immediate,
          "mode 0: pacing follows the requested mode");
    Check(!RedrawGivesUp(PolicyMode::Old), "mode 0: the redraw waits for its frame");
    Check(RedrawWaitNs(PolicyMode::Old) == kInfiniteWaitNs, "mode 0: the redraw waits forever");
    Check(AcquireTimeoutNs(PolicyMode::Old) == kInfiniteWaitNs, "mode 0: acquire waits forever");
    Check(RenderFrameWaitNs(PolicyMode::Old) == kInfiniteWaitNs, "mode 0: the render frame waits forever");
}

void AcquireTimeoutSkipsAndRecovers() {
    FakeSwapchain fake;
    fake.acquire_results = {SurfaceResult::Timeout, SurfaceResult::Timeout, SurfaceResult::Success};
    State state{PolicyMode::New};
    std::uint32_t index = kUntouchedIndex;

    const AcquireOutcome first = AcquireImage(fake, kBuilt, state, kStart, index);
    Check(first.step == AcquireStep::Skip, "timeout skips the frame");
    Check(first.first_timeout, "first timeout is the one that logs");
    Check(!first.rebuild_after_present, "timeout does not rebuild");
    Check(index == kUntouchedIndex, "timeout leaves the image index alone");

    const AcquireOutcome second = AcquireImage(fake, kBuilt, state, kStart, index);
    Check(second.step == AcquireStep::Skip, "second timeout skips too");
    Check(!second.first_timeout, "second timeout does not log");

    const AcquireOutcome third = AcquireImage(fake, kBuilt, state, kStart, index);
    Check(third.step == AcquireStep::Acquired, "next frame acquires");
    Check(index == 0, "acquired image index is returned");
    Check(state.health.acquire_timeouts.load() == 2, "two timeouts counted");

    Check(fake.acquire_timeouts.size() == 3, "three acquires issued");
    for (const std::uint64_t timeout : fake.acquire_timeouts) {
        Check(timeout == kPresentWaitTimeoutNs, "acquire uses the finite timeout");
    }
    Check(RedrawGivesUp(PolicyMode::New), "mode 1: the redraw gives up");
    Check(RedrawWaitNs(PolicyMode::New) == kPresentWaitTimeoutNs, "mode 1: the redraw waits 500 ms");
    Check(RenderFrameWaitNs(PolicyMode::New) == kPresentWaitTimeoutNs, "mode 1: the render frame waits in 500 ms slices");
}

void NotReadyIsSkippedLikeTimeout() {
    FakeSwapchain fake;
    fake.acquire_results = {SurfaceResult::NotReady};
    State state{PolicyMode::New};
    std::uint32_t index = 0;
    const AcquireOutcome outcome = AcquireImage(fake, kBuilt, state, kStart, index);
    Check(outcome.step == AcquireStep::Skip, "not ready skips the frame");
    Check(state.health.acquire_timeouts.load() == 1, "not ready counted");
}

void ErrorsRebuild() {
    FakeSwapchain fake;
    fake.acquire_results = {SurfaceResult::OutOfDate, SurfaceResult::SurfaceLost,
                            SurfaceResult::Failed};
    State state{PolicyMode::New};
    std::uint32_t index = 0;
    for (int i = 0; i < kErrorResults; ++i) {
        Check(AcquireImage(fake, kBuilt, state, kStart, index).step == AcquireStep::Rebuild,
              "out of date, surface lost and other errors rebuild");
    }
    Check(state.health.acquire_timeouts.load() == 0, "errors are not timeouts");
    Check(fake.surface_queries == 0, "errors need no surface query");
}

void SuboptimalSameSurfaceRebuildsOncePerEpisode() {
    FakeSwapchain fake;
    fake.surface = kBuilt;
    State state{PolicyMode::New};

    Check(OnSuboptimal(fake, kBuilt, state, kStart), "first suboptimal of an episode rebuilds once");
    Check(OnSuboptimal(fake, kBuilt, state, kStart + kTick),
          "a second report before the new swapchain keeps the request");
    Check(fake.surface_queries == 1, "pending request needs no second query");
    Check(state.health.suboptimal_latch_rebuilds.load() == 1, "one rebuild counted for the episode");

    OnSwapchainCreated(state);
    for (int i = 0; i < kStickyReports; ++i) {
        Check(!OnSuboptimal(fake, kBuilt, state, kStart + (2 + i) * kTick),
              "sticky suboptimal after the rebuild: no further rebuild");
    }
    Check(fake.surface_queries == 1, "sticky reports within a second do no surface query");
    Check(state.health.suboptimal_reports.load() == 2 + kStickyReports, "every report counted");
    Check(state.health.suboptimal_latch_rebuilds.load() == 1, "still one rebuild");
    Check(state.health.suboptimal_changed_rebuilds.load() == 0, "no change rebuilds");

    Check(!OnSuboptimal(fake, kBuilt, state, kStart + kStickyRecheckIntervalNs),
          "a recheck that finds the same surface does not rebuild");
    Check(fake.surface_queries == 2, "one recheck after a second");
    Check(state.health.suboptimal_rechecks.load() == 1, "recheck counted");
}

void StickyEpisodeThenSurfaceChanges(const SurfaceState& changed, const char* what) {
    FakeSwapchain fake;
    fake.surface = kBuilt;
    State state{PolicyMode::New};
    Check(OnSuboptimal(fake, kBuilt, state, kStart), "episode opens with a rebuild");
    OnSwapchainCreated(state);
    Check(!OnSuboptimal(fake, kBuilt, state, kStart + kTick), "sticky episode latched");

    fake.surface = changed;
    Check(!OnSuboptimal(fake, kBuilt, state, kStart + 2 * kTick),
          "a change inside the recheck interval waits for it");
    Check(OnSuboptimal(fake, kBuilt, state, kStart + kStickyRecheckIntervalNs), what);
    Check(state.health.suboptimal_changed_rebuilds.load() == 1, "the change rebuild is counted");
    Check(OnSuboptimal(fake, kBuilt, state, kStart + kStickyRecheckIntervalNs + kTick),
          "the request stays until the new swapchain");
    OnSwapchainCreated(state);
    Check(!OnSuboptimal(fake, changed, state, kStart + kStickyRecheckIntervalNs + 2 * kTick),
          "the rebuilt swapchain is latched again");
}

void StickyRecheckIsBounded() {
    FakeSwapchain fake;
    fake.surface = kBuilt;
    State state{PolicyMode::New};
    Check(OnSuboptimal(fake, kBuilt, state, kStart), "episode opens");
    OnSwapchainCreated(state);
    constexpr int kFrames = 130;
    for (int i = 1; i <= kFrames; ++i) {
        Check(!OnSuboptimal(fake, kBuilt, state, kStart + i * kTick), "same surface never rebuilds");
    }
    Check(fake.surface_queries == 3, "about two seconds of sticky reports recheck twice");
    Check(state.health.suboptimal_rechecks.load() == 2, "two rechecks counted");

    fake.surface = std::nullopt;
    Check(!OnSuboptimal(fake, kBuilt, state, kStart + 4 * kStickyRecheckIntervalNs),
          "a failed recheck does not rebuild");
    Check(state.health.suboptimal_recheck_failures.load() == 1, "failed recheck counted apart");
    Check(state.health.suboptimal_unknown_rebuilds.load() == 0, "and not as an unknown rebuild");
}

void EpisodeEndsOnCleanPresent() {
    FakeSwapchain fake;
    fake.surface = kBuilt;
    State state{PolicyMode::New};
    Check(OnSuboptimal(fake, kBuilt, state, kStart), "first episode rebuilds");
    OnSwapchainCreated(state);
    Check(!OnSuboptimal(fake, kBuilt, state, kStart + kTick), "same episode does not");
    OnPresentSucceeded(state);
    Check(OnSuboptimal(fake, kBuilt, state, kStart + 2 * kTick),
          "a new episode after a clean present rebuilds once again");
}

void CreateOutsideEpisodeDoesNotLatch() {
    FakeSwapchain fake;
    fake.surface = kBuilt;
    State state{PolicyMode::New};
    OnSwapchainCreated(state);
    Check(OnSuboptimal(fake, kBuilt, state, kStart),
          "a swapchain made before any suboptimal report does not use up the episode");
}

void SuboptimalAcquireNewSizeRebuilds() {
    FakeSwapchain fake;
    fake.acquire_results = {SurfaceResult::Suboptimal};
    fake.surface = kLarger;
    State state{PolicyMode::New};
    std::uint32_t index = kUntouchedIndex;
    const AcquireOutcome outcome = AcquireImage(fake, kBuilt, state, kStart, index);
    Check(outcome.step == AcquireStep::Acquired, "suboptimal image is still presented");
    Check(index == 0, "suboptimal image index is returned");
    Check(outcome.rebuild_after_present, "size change: rebuild after the present");
    Check(state.health.suboptimal_changed_rebuilds.load() == 1, "change rebuild counted");
    Check(state.health.suboptimal_latch_rebuilds.load() == 0, "not counted as a same-surface rebuild");

    State unknown{PolicyMode::New};
    fake.surface = std::nullopt;
    Check(OnSuboptimal(fake, kBuilt, unknown, kStart), "unknown surface rebuilds");
    Check(unknown.health.suboptimal_unknown_rebuilds.load() == 1, "counted as an unknown rebuild");
    Check(unknown.health.suboptimal_changed_rebuilds.load() == 0, "not as a change rebuild");
}

void FirstTimeoutLatchIsPerCounter() {
    PresentHealth health;
    Check(CountFirst(health.render_frame_wait_timeouts), "render frame wait logs first");
    Check(CountFirst(health.redraw_wait_timeouts), "redraw wait has its own first");
    Check(!CountFirst(health.render_frame_wait_timeouts), "render frame wait logs once");
    Check(!CountFirst(health.redraw_wait_timeouts), "redraw wait logs once");
}

void PresentModeFallbackChain() {
    using M = PresentMode;
    constexpr PolicyMode kNew = PolicyMode::New;
    const std::vector<M> all{M::Immediate, M::Mailbox, M::Fifo, M::FifoRelaxed};
    const std::vector<M> fifo_only{M::Fifo};
    const std::vector<M> no_mailbox{M::Immediate, M::Fifo, M::FifoRelaxed};
    const std::vector<M> no_immediate{M::Mailbox, M::Fifo};
    const std::vector<M> nothing{};

    for (const PolicyMode mode : {PolicyMode::Old, PolicyMode::New}) {
        for (const M requested : {M::Immediate, M::Mailbox, M::Fifo, M::FifoRelaxed}) {
            Check(ChoosePresentMode(mode, requested, all) == requested, "supported request is kept");
            Check(ChoosePresentMode(mode, requested, fifo_only) == M::Fifo, "only FIFO: FIFO");
            Check(ChoosePresentMode(mode, requested, nothing) == M::Fifo, "empty list: FIFO");
            Check(PresentModeChain(mode, requested).back() == M::Fifo, "every chain ends in FIFO");
        }
    }
    Check(ChoosePresentMode(kNew, M::Immediate, no_immediate) == M::Mailbox,
          "immediate falls back to mailbox");
    Check(ChoosePresentMode(kNew, M::Mailbox, no_mailbox) == M::Fifo,
          "mailbox falls back to FIFO, never to a tearing mode");
    Check(ChoosePresentMode(kNew, M::FifoRelaxed, no_immediate) == M::Fifo,
          "FIFO relaxed falls back to FIFO");
    Check(ChoosePresentMode(kNew, M::Unknown, nothing) == M::Fifo,
          "a mode outside the table falls back to FIFO");

    Check(PacingPresentMode(kNew, M::Immediate, M::Mailbox) == M::Mailbox,
          "mode 1: pacing follows the mode actually chosen");
    Check(PacingPresentMode(kNew, M::Immediate, M::Immediate) == M::Immediate,
          "mode 1: a kept request paces as itself");
}

int RunSuite() {
    Current().failures = 0;
    ModeSwitch();
    Mappings();
    OldPathDecisions();
    AcquireTimeoutSkipsAndRecovers();
    NotReadyIsSkippedLikeTimeout();
    ErrorsRebuild();
    SuboptimalSameSurfaceRebuildsOncePerEpisode();
    StickyEpisodeThenSurfaceChanges(kLarger, "sticky episode, then the size changes: rebuild");
    StickyEpisodeThenSurfaceChanges(kHdrOn, "sticky episode, then Windows HDR turns on: rebuild");
    StickyRecheckIsBounded();
    EpisodeEndsOnCleanPresent();
    CreateOutsideEpisodeDoesNotLatch();
    SuboptimalAcquireNewSizeRebuilds();
    FirstTimeoutLatchIsPerCounter();
    PresentModeFallbackChain();
    return Current().failures;
}

}

int main() {
    const int baseline = RunSuite();
    if (baseline != 0) {
        std::printf("present-policy-test: %d failures\n", baseline);
        return EXIT_FAILURE;
    }
#if defined(PRESENT_POLICY_FAULTS)
    Current().verbose = false;
    int survivors = 0;
    for (int fault = 1; fault <= kPresentPolicyFaultCount; ++fault) {
        PresentPolicyFaultSelector() = fault;
        if (RunSuite() == 0) {
            std::printf("FAIL: fault %d was not caught\n", fault);
            ++survivors;
        }
    }
    PresentPolicyFaultSelector() = 0;
    if (survivors != 0) {
        std::printf("present-policy-test: %d faults not caught\n", survivors);
        return EXIT_FAILURE;
    }
    std::printf("present-policy-test: ok, %d faults caught\n", kPresentPolicyFaultCount);
#else
    std::printf("present-policy-test: ok\n");
#endif
    return EXIT_SUCCESS;
}
