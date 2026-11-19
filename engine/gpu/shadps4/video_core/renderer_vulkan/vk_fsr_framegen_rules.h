// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

namespace Vulkan::FsrFgRules {

inline constexpr std::uint32_t NoFamily = ~0u;
inline constexpr std::uint32_t QueueGraphics = 1u, QueueCompute = 2u, QueueTransfer = 4u;

inline std::uint32_t QueueAbilities(std::uint32_t flags) {
    return flags & (QueueGraphics | QueueCompute) ? flags | QueueTransfer : flags;
}

struct Family {
    std::uint32_t flags = 0;
    std::uint32_t count = 0;
    bool present = false;
};

struct QueueRef {
    std::uint32_t family = NoFamily;
    std::uint32_t index = 0;
    [[nodiscard]] bool Valid() const {
        return family != NoFamily;
    }
};

struct QueuePlan {
    std::vector<std::uint32_t> counts;
    QueueRef present, acquire, async;
    const char* problem = nullptr;
    [[nodiscard]] bool Extra() const {
        return present.Valid();
    }
};

inline QueuePlan PlanQueues(const std::vector<Family>& families, std::uint32_t graphics,
                            std::uint32_t readback, bool wanted) {
    QueuePlan plan;
    plan.counts.assign(families.size(), 0);
    if (graphics < families.size()) {
        plan.counts[graphics] = 1;
    }
    if (readback < families.size() && readback != graphics) {
        plan.counts[readback] = 1;
    }
    if (!wanted || graphics >= families.size()) {
        return plan;
    }
    std::vector<std::uint32_t> counts = plan.counts;
    const auto spare = [&](std::uint32_t f) { return counts[f] < families[f].count; };
    const auto take = [&](std::uint32_t f) { return QueueRef{f, counts[f]++}; };
    const auto pick = [&](auto&& accept) {
        for (std::uint32_t f = 0; f < families.size(); ++f) {
            if (spare(f) && accept(families[f], f)) {
                return take(f);
            }
        }
        return QueueRef{};
    };
    const auto has = [](const Family& family, std::uint32_t bits) {
        return (QueueAbilities(family.flags) & bits) == bits;
    };
    QueueRef present;
    if (families[graphics].present && spare(graphics)) {
        present = take(graphics);
    } else {
        present = pick([&](const Family& family, std::uint32_t) {
            return family.present && has(family, QueueCompute | QueueTransfer);
        });
    }
    if (!present.Valid()) {
        plan.problem = "the GPU has no spare queue to present from";
        return plan;
    }
    QueueRef acquire = pick([&](const Family& family, std::uint32_t) {
        return has(family, QueueTransfer) && !(family.flags & (QueueGraphics | QueueCompute));
    });
    if (!acquire.Valid()) {
        acquire = pick([&](const Family& family, std::uint32_t) {
            return has(family, QueueTransfer) && !(family.flags & QueueGraphics);
        });
    }
    if (!acquire.Valid()) {
        acquire = pick([&](const Family& family, std::uint32_t) { return has(family, QueueTransfer); });
    }
    if (!acquire.Valid()) {
        plan.problem = "the GPU has no spare queue for AMD's image acquire";
        return plan;
    }
    QueueRef async = pick([&](const Family& family, std::uint32_t) {
        return has(family, QueueCompute) && !(family.flags & QueueGraphics);
    });
    if (!async.Valid()) {
        async = pick([&](const Family& family, std::uint32_t) { return has(family, QueueCompute); });
    }
    plan.counts = counts;
    plan.present = present;
    plan.acquire = acquire;
    plan.async = async;
    return plan;
}

inline constexpr int DefaultReadyFrames = 30;

enum class Off { None, NotGameFrame, NoInputs, Menu, Hdr, NoContext };

inline Off WhyOff(bool game_frame, bool inputs, bool menu_open, bool hdr, bool context) {
    if (!game_frame) {
        return Off::NotGameFrame;
    }
    if (hdr) {
        return Off::Hdr;
    }
    if (!inputs) {
        return Off::NoInputs;
    }
    if (menu_open) {
        return Off::Menu;
    }
    if (!context) {
        return Off::NoContext;
    }
    return Off::None;
}

inline const char* OffText(Off off) {
    switch (off) {
    case Off::None:
        return nullptr;
    case Off::NotGameFrame:
        return "a frame shown again";
    case Off::NoInputs:
        return "no upscaled game frame (menus, movies, loading screens)";
    case Off::Menu:
        return "the Insert menu is open";
    case Off::Hdr:
        return "HDR output is on";
    case Off::NoContext:
        return "no frame generation context";
    }
    return nullptr;
}

struct FrameState {
    std::uint64_t frame_id = 0;
    int ready_in_a_row = 0;
    bool on = false;
};

struct FrameDecision {
    std::uint64_t frame_id = 0;
    bool enabled = false;
    bool reset = false;
    bool switched = false;
};

inline FrameDecision NextFrame(FrameState& state, bool ready, int ready_frames = DefaultReadyFrames) {
    const int need = std::max(ready_frames, 0);
    FrameDecision decision;
    decision.frame_id = ++state.frame_id;
    state.ready_in_a_row = ready ? std::min(state.ready_in_a_row + 1, std::max(need, 1)) : 0;
    decision.enabled = ready && (state.on || state.ready_in_a_row >= need);
    decision.reset = decision.enabled && !state.on;
    decision.switched = decision.enabled != state.on;
    state.on = decision.enabled;
    return decision;
}

inline void ContextRemade(FrameState& state) {
    state.ready_in_a_row = 0;
    state.on = false;
}

}
