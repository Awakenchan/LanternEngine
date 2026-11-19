// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstdio>
#include <cstdlib>
#include <vector>

#include "gpu/shadps4/video_core/renderer_vulkan/vk_fsr_framegen_rules.h"

using namespace Vulkan::FsrFgRules;

namespace {

int failures = 0;

void Check(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        ++failures;
    }
}

constexpr std::uint32_t G = QueueGraphics, C = QueueCompute, T = QueueTransfer;

std::vector<Family> Nvidia() {
    return {{G | C | T, 16, true}, {T, 2, false}, {C | T, 8, true}};
}

std::vector<Family> Radeon() {
    return {{G | C | T, 1, true}, {C | T, 4, true}, {T, 2, false}};
}

std::vector<Family> RadeonTwoCompute() {
    return {{G | C | T, 1, true}, {C | T, 2, true}, {T, 1, false}};
}

std::vector<Family> SingleQueue() {
    return {{G | C | T, 1, true}};
}

std::uint32_t Total(const QueuePlan& plan) {
    std::uint32_t total = 0;
    for (const auto count : plan.counts) {
        total += count;
    }
    return total;
}

void NoExtraQueuesUnlessWanted() {
    for (const auto& families : {Nvidia(), Radeon(), RadeonTwoCompute(), SingleQueue()}) {
        const std::uint32_t graphics = 0;
        const std::uint32_t readback = families.size() > 1 ? 1u : NoFamily;
        for (const std::uint32_t rb : {readback, NoFamily}) {
            const QueuePlan plan = PlanQueues(families, graphics, rb, false);
            Check(!plan.Extra() && !plan.acquire.Valid() && !plan.async.Valid(), "not wanted: no AMD queues");
            Check(plan.counts.size() == families.size(), "not wanted: a count per family");
            Check(plan.counts[graphics] == 1, "not wanted: one game queue");
            Check(Total(plan) == (rb == NoFamily ? 1u : 2u), "not wanted: only the game and readback queues");
            if (rb != NoFamily) {
                Check(plan.counts[rb] == 1, "not wanted: one readback queue");
            }
        }
    }
}

void NvidiaLayout() {
    const QueuePlan plan = PlanQueues(Nvidia(), 0, 2, true);
    Check(plan.Extra() && !plan.problem, "NVIDIA: planned");
    Check(plan.present.family == 0 && plan.present.index == 1, "NVIDIA: present from the graphics family, queue 1");
    Check(plan.acquire.family == 1 && plan.acquire.index == 0, "NVIDIA: acquire from the transfer family");
    Check(plan.async.family == 2 && plan.async.index == 1, "NVIDIA: async compute after the readback queue");
    Check(plan.counts[0] == 2 && plan.counts[1] == 1 && plan.counts[2] == 2, "NVIDIA: counts");
}

void RadeonLayout() {
    const QueuePlan plan = PlanQueues(Radeon(), 0, 1, true);
    Check(plan.Extra() && !plan.problem, "Radeon: planned");
    Check(plan.present.family == 1 && plan.present.index == 1, "Radeon: present from the compute family");
    Check(plan.acquire.family == 2 && plan.acquire.index == 0, "Radeon: acquire from the transfer family");
    Check(plan.async.family == 1 && plan.async.index == 2, "Radeon: async compute from the compute family");
    Check(plan.counts[0] == 1, "Radeon: the game queue alone in the graphics family");
}

void TightLayouts() {
    const QueuePlan two = PlanQueues(RadeonTwoCompute(), 0, 1, true);
    Check(two.Extra() && two.present.family == 1 && two.present.index == 1, "two compute queues: present");
    Check(two.acquire.family == 2, "two compute queues: acquire from transfer");
    Check(!two.async.Valid(), "two compute queues: no async queue, which is optional");
    const QueuePlan single = PlanQueues(SingleQueue(), 0, NoFamily, true);
    Check(!single.Extra() && single.problem, "one queue: no plan, with a reason");
    Check(Total(single) == 1, "one queue: the device stays as it was");
    std::vector<Family> no_present = Radeon();
    no_present[0].present = false;
    no_present[1].present = false;
    const QueuePlan none = PlanQueues(no_present, 0, 1, true);
    Check(!none.Extra() && none.problem && Total(none) == 2, "no present-capable spare queue: the device stays as it was");
}

void TransferImplied() {
    const std::vector<Family> families{{G | C, 1, true}, {C, 4, true}, {0, 2, false}, {T, 1, false}};
    const QueuePlan plan = PlanQueues(families, 0, NoFamily, true);
    Check(plan.present.family == 1, "a compute family without the transfer bit presents");
    Check(plan.acquire.family == 3, "image acquire on the transfer family, never on a family without copies");
    const std::vector<Family> no_copy{{G | C, 1, true}, {C, 1, true}, {0, 4, false}};
    const QueuePlan tight = PlanQueues(no_copy, 0, NoFamily, true);
    Check(!tight.Extra() || tight.acquire.family != 2, "a family that cannot copy is never AMD's acquire queue");
}

void FrameIdsGoUpByOne() {
    FrameState state;
    std::uint64_t last = 0;
    for (int i = 0; i < 200; ++i) {
        const bool ready = (i / 37) % 2 == 0;
        const FrameDecision decision = NextFrame(state, ready);
        Check(decision.frame_id == last + 1, "frame ids go up by exactly one, ready or not");
        last = decision.frame_id;
    }
    ContextRemade(state);
    Check(NextFrame(state, true).frame_id == last + 1, "frame ids keep going after the context is made again");
}

void DebounceAfterOffFrames() {
    FrameState state;
    int first_on = -1;
    for (int i = 0; i < 40; ++i) {
        const FrameDecision decision = NextFrame(state, true);
        if (decision.enabled && first_on < 0) {
            first_on = i;
            Check(decision.reset && decision.switched, "the first frame on resets and switches");
        } else if (decision.enabled) {
            Check(!decision.reset && !decision.switched, "frames after the first on neither reset nor switch");
        }
    }
    Check(first_on == DefaultReadyFrames - 1, "on after 30 ready frames in a row");
    const FrameDecision off = NextFrame(state, false);
    Check(!off.enabled && off.switched && !off.reset, "one off frame (a menu) switches off at once");
    for (int i = 0; i < DefaultReadyFrames - 1; ++i) {
        Check(!NextFrame(state, true).enabled, "still off before 30 ready frames again");
    }
    const FrameDecision back = NextFrame(state, true);
    Check(back.enabled && back.reset && back.switched, "on again at the 30th ready frame, with a reset");
    for (int i = 0; i < 10; ++i) {
        NextFrame(state, i % 3 != 0);
    }
    Check(!state.on || state.ready_in_a_row >= DefaultReadyFrames, "an interrupted run starts the count over");
}

void NoDebounce() {
    FrameState state;
    const FrameDecision first = NextFrame(state, true, 0);
    Check(first.enabled && first.reset, "ready frames needed 0: on at the first ready frame");
    Check(!NextFrame(state, false, 0).enabled, "ready frames needed 0: off when not ready");
    for (int i = 0; i < 1000; ++i) {
        NextFrame(state, true);
    }
    Check(state.ready_in_a_row <= DefaultReadyFrames, "the count is held, no overflow");
}

void ContextRemadeSwitchesOn() {
    FrameState state;
    for (int i = 0; i < DefaultReadyFrames; ++i) {
        NextFrame(state, true);
    }
    Check(state.on, "on after the debounce");
    ContextRemade(state);
    Check(!NextFrame(state, true).enabled, "a new context waits for the debounce again");
}

void OffReasons() {
    Check(WhyOff(true, true, false, false, true) == Off::None, "a game frame with inputs: on");
    Check(WhyOff(false, true, false, false, true) == Off::NotGameFrame, "a frame shown again: off");
    Check(WhyOff(true, false, false, false, true) == Off::NoInputs, "menus, movies, loading: off");
    Check(WhyOff(true, true, true, false, true) == Off::Menu, "the Insert menu: off");
    Check(WhyOff(true, true, false, true, true) == Off::Hdr, "HDR output: off");
    Check(WhyOff(true, false, false, true, true) == Off::Hdr, "HDR output is named before missing inputs");
    Check(WhyOff(true, true, false, false, false) == Off::NoContext, "no context: off");
    Check(OffText(Off::None) == nullptr && OffText(Off::Menu) != nullptr, "texts");
}

}

int main() {
    NoExtraQueuesUnlessWanted();
    NvidiaLayout();
    RadeonLayout();
    TightLayouts();
    TransferImplied();
    FrameIdsGoUpByOne();
    DebounceAfterOffFrames();
    NoDebounce();
    ContextRemadeSwitchesOn();
    OffReasons();
    if (failures) {
        std::printf("FAIL: %d fsr frame generation rule checks\n", failures);
        return 1;
    }
    std::puts("PASS: fsr frame generation queue plan, frame ids, debounce and off frames");
    return 0;
}
