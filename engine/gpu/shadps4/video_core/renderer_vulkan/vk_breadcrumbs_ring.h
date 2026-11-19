// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <type_traits>
#include <unordered_map>

namespace Vulkan::Breadcrumbs {

struct DrawResource {
    uint64_t handle = 0;
    uint64_t guest = 0;
    uint64_t guest_size = 0;
    uint64_t gpu_base = 0;
    uint64_t gpu_size = 0;
    uint64_t offset = 0;
    uint64_t range = 0;
    uint32_t format = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint16_t depth = 0;
    uint8_t layers = 0;
    bool image = false;
};

constexpr uint32_t MaxDrawResources = 20;

struct DrawResources {
    uint64_t id = 0;
    uint32_t stream = 0;
    uint32_t count = 0;
    uint32_t dropped = 0;
    uint32_t image_descriptors = 0;
    uint32_t buffer_descriptors = 0;
    std::array<DrawResource, MaxDrawResources> items{};

    DrawResource* Add() {
        if (count < MaxDrawResources) {
            return &items[count++];
        }
        ++dropped;
        return nullptr;
    }
};

static_assert(std::is_trivially_copyable_v<DrawResources>);
static_assert(std::is_standard_layout_v<DrawResources>);
static_assert(sizeof(DrawResources) % sizeof(uint64_t) == 0);
static_assert(sizeof(DrawResource) % sizeof(uint64_t) == 0);
static_assert(std::atomic_ref<uint64_t>::is_always_lock_free);

struct Nearest {
    int index = -1;
    bool inside = false;
    bool past_end = false;
    uint64_t distance = ~uint64_t(0);
};

inline Nearest Locate(const DrawResources& draw, uint64_t address) {
    Nearest best;
    for (uint32_t i = 0; i < draw.count; ++i) {
        const DrawResource& r = draw.items[i];
        if (!r.gpu_size) {
            continue;
        }
        const uint64_t lo = r.gpu_base;
        const uint64_t hi = r.gpu_base + r.gpu_size;
        if (address >= lo && address < hi) {
            if (!best.inside || r.gpu_size < draw.items[best.index].gpu_size) {
                best = {int(i), true, false, 0};
            }
            continue;
        }
        if (best.inside) {
            continue;
        }
        const bool past = address >= hi;
        const uint64_t distance = past ? address - hi : lo - address;
        if (distance < best.distance) {
            best = {int(i), false, past, distance};
        }
    }
    return best;
}

class DrawSlot {
public:
    static constexpr size_t Words = sizeof(DrawResources) / sizeof(uint64_t);

    static constexpr size_t UsedWords(uint32_t count) {
        const size_t bytes = offsetof(DrawResources, items) +
                             std::min(count, MaxDrawResources) * sizeof(DrawResource);
        return (bytes + sizeof(uint64_t) - 1) / sizeof(uint64_t);
    }

    bool TryWrite(const DrawResources& draw) {
        uint64_t seq = sequence.load(std::memory_order_relaxed);
        if ((seq & 1) || !sequence.compare_exchange_strong(seq, seq + 1, std::memory_order_acquire,
                                                                     std::memory_order_relaxed)) {
            return false;
        }
        std::atomic_thread_fence(std::memory_order_release);
        const auto* source = reinterpret_cast<const std::byte*>(&draw);
        const size_t used = UsedWords(draw.count);
        for (size_t i = 0; i < used; ++i) {
            uint64_t word;
            std::memcpy(&word, source + i * sizeof(uint64_t), sizeof(word));
            std::atomic_ref<uint64_t>{words[i]}.store(word, std::memory_order_relaxed);
        }
        sequence.store(seq + 2, std::memory_order_release);
        return true;
    }

    std::optional<DrawResources> Read() const {
        const uint64_t before = sequence.load(std::memory_order_acquire);
        if (before == 0 || (before & 1)) {
            return std::nullopt;
        }
        std::array<uint64_t, Words> copy{};
        for (size_t i = 0; i < Words; ++i) {
            copy[i] = std::atomic_ref<uint64_t>{words[i]}.load(std::memory_order_relaxed);
        }
        std::atomic_thread_fence(std::memory_order_acquire);
        if (sequence.load(std::memory_order_relaxed) != before) {
            return std::nullopt;
        }
        DrawResources draw;
        std::memcpy(&draw, copy.data(), sizeof(draw));
        draw.count = std::min(draw.count, MaxDrawResources);
        return draw;
    }

private:
    alignas(64) std::atomic<uint64_t> sequence{0};
    alignas(std::atomic_ref<uint64_t>::required_alignment) mutable std::array<uint64_t, Words> words{};
};

template <uint32_t Size>
class DrawRing {
public:
    bool Publish(const DrawResources& draw) {
        for (uint32_t attempt = 0; attempt < Size; ++attempt) {
            if (slots[next.fetch_add(1, std::memory_order_relaxed) % Size].TryWrite(draw)) {
                return true;
            }
        }
        return false;
    }

    std::optional<DrawResources> Read(uint32_t stream, uint64_t id) const {
        for (const DrawSlot& slot : slots) {
            if (auto draw = slot.Read(); draw && draw->stream == stream && draw->id == id) {
                return draw;
            }
        }
        return std::nullopt;
    }

    uint64_t Recorded() const {
        return next.load(std::memory_order_relaxed);
    }

private:
    std::array<DrawSlot, Size> slots{};
    std::atomic<uint64_t> next{0};
};

template <typename Map>
typename Map::iterator FindUnbound(Map& map, uint64_t base, uint64_t handle) {
    auto [lo, hi] = map.equal_range(base);
    for (auto it = lo; it != hi; ++it) {
        if (it->second.handle == handle) {
            return it;
        }
    }
    return handle == 0 && lo != hi ? lo : map.end();
}

class HandleRanges {
public:
    struct Range {
        uint64_t base = 0;
        uint64_t size = 0;
    };

    void Bind(uint64_t handle, uint64_t base, uint64_t size) {
        std::scoped_lock lock{mutex};
        auto [it, inserted] = ranges.try_emplace(handle, Range{base, size});
        if (!inserted) {
            it->second = Merge(it->second, {base, size});
        }
    }

    static Range Merge(Range stored, Range added) {
        const uint64_t stored_end = stored.base + stored.size;
        const uint64_t added_end = added.base + added.size;
        if (added.base > stored_end || stored.base > added_end) {
            return added;
        }
        const uint64_t lo = std::min(stored.base, added.base);
        return {lo, std::max(stored_end, added_end) - lo};
    }

    void Unbind(uint64_t handle, uint64_t base) {
        std::scoped_lock lock{mutex};
        if (auto it = ranges.find(handle); it != ranges.end() && it->second.base == base) {
            ranges.erase(it);
        }
    }

    void Resolve(std::span<DrawResource> items) const {
        std::scoped_lock lock{mutex};
        for (DrawResource& item : items) {
            const auto it = ranges.find(item.handle);
            const Range range = it == ranges.end() ? Range{} : it->second;
            item.gpu_base = range.base;
            item.gpu_size = range.size;
        }
    }

    Range Find(uint64_t handle) const {
        std::scoped_lock lock{mutex};
        const auto it = ranges.find(handle);
        return it == ranges.end() ? Range{} : it->second;
    }

    size_t Size() const {
        std::scoped_lock lock{mutex};
        return ranges.size();
    }

private:
    mutable std::mutex mutex;
    std::unordered_map<uint64_t, Range> ranges;
};

} // namespace Vulkan::Breadcrumbs
