// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

namespace BbEngine::GxSealed {
using u32 = std::uint32_t;
using u64 = std::uint64_t;

struct Hit {
    u32 id = 0;
    u32 generation = 0;
    u64 base = 0, size = 0;
};

enum Retired : std::uint8_t { ByRelease, ByUpdate, ByCpuWrite, ByGpuWrite, kRetiredKinds };

class Registry {
public:
    explicit Registry(u32 capacity = 1u << 18) : capacity_(capacity), entries_(new Entry[capacity]) {}

    u32 Seal(u64 base, u64 size) {
        if (!size) return 0;
        std::scoped_lock lock(mutex_);
        RetireOverlapsLocked(base, size, ByUpdate);
        u32 id;
        if (!free_.empty()) {
            id = free_.back();
            free_.pop_back();
        } else {
            if (next_ >= capacity_) return 0;
            id = next_++;
        }
        Entry& e = entries_[id];
        e.base = base;
        e.size = size;
        e.generation.fetch_add(1, std::memory_order_release);
        e.live.store(true, std::memory_order_release);
        by_base_[base] = id;
        ++sealed_;
        return id;
    }

    void RetireRange(u64 address, u64 size, Retired why) {
        if (!size) return;
        std::scoped_lock lock(mutex_);
        RetireOverlapsLocked(address, size, why);
    }

    bool Find(u64 address, u64 size, Hit* out) const {
        if (!size) return false;
        std::scoped_lock lock(mutex_);
        auto at = by_base_.upper_bound(address);
        if (at == by_base_.begin()) return false;
        --at;
        const Entry& e = entries_[at->second];
        if (address < e.base || address - e.base >= e.size) return false;
        *out = {at->second, e.generation.load(std::memory_order_acquire), e.base, e.size};
        return true;
    }

    bool Valid(const Hit& hit) const {
        if (!hit.id || hit.id >= capacity_) return false;
        const Entry& e = entries_[hit.id];
        return e.live.load(std::memory_order_acquire) && e.generation.load(std::memory_order_acquire) == hit.generation;
    }

    u64 Sealed() const { return sealed_; }
    u64 RetiredCount(Retired why) const { return retired_[why]; }
    u32 Live() const {
        std::scoped_lock lock(mutex_);
        return static_cast<u32>(by_base_.size());
    }

private:
    struct Entry {
        u64 base = 0, size = 0;
        std::atomic<u32> generation{0};
        std::atomic<bool> live{false};
    };

    void RetireOverlapsLocked(u64 address, u64 size, Retired why) {
        auto at = by_base_.upper_bound(address);
        if (at != by_base_.begin()) --at;
        while (at != by_base_.end() && at->first < address + size) {
            Entry& e = entries_[at->second];
            if (e.base + e.size > address) {
                e.live.store(false, std::memory_order_release);
                e.generation.fetch_add(1, std::memory_order_release);
                free_.push_back(at->second);
                ++retired_[why];
                at = by_base_.erase(at);
            } else {
                ++at;
            }
        }
    }

    u32 capacity_;
    std::unique_ptr<Entry[]> entries_;
    mutable std::mutex mutex_;
    std::map<u64, u32> by_base_;
    std::vector<u32> free_;
    u32 next_ = 1;
    u64 sealed_ = 0;
    u64 retired_[kRetiredKinds]{};
};
}
