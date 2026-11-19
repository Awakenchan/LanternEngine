// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

#ifdef LANTERN_SEALED_MUTANTS
namespace BbEngine::GxSealed {
inline int g_mutant = 0;
}
#define LANTERN_SEALED_PICK(n, wrong, right) (::BbEngine::GxSealed::g_mutant == (n) ? (wrong) : (right))
#else
#define LANTERN_SEALED_PICK(n, wrong, right) (right)
#endif

namespace BbEngine::GxSealed {
using u32 = std::uint32_t;
using u64 = std::uint64_t;

inline constexpr int kMutants = 3;

inline bool Holds(u64 base, u64 bytes, u64 address, u64 size) {
    return address >= base && LANTERN_SEALED_PICK(3, address - base <= bytes, address - base < bytes) &&
           LANTERN_SEALED_PICK(1, true, LANTERN_SEALED_PICK(2, size < bytes - (address - base), size <= bytes - (address - base)));
}

struct Hit {
    u32 id = 0;
    u32 generation = 0;
    u64 base = 0, size = 0;
};

enum Retired : std::uint8_t {
    ByRelease,
    ByOverlap,
    ByCpuWrite,
    ByGpuWrite,
    ByFault,
    ByAsset,
    ByImageStore,
    ByEviction,
    ByDemotion,
    ByCopyBack,
    ByPromotion,
    kRetiredKinds
};
inline constexpr const char* kRetiredNames[kRetiredKinds] = {"release",   "overlapping seal",   "CPU write", "GPU write",
                                                             "write fault", "fresh asset", "image store", "eviction",
                                                             "demotion",  "copy-back", "promotion"};
inline constexpr bool LayoutEvent(Retired why) {
    return why == ByEviction || why == ByDemotion || why == ByCopyBack || why == ByPromotion;
}
inline constexpr bool OwnSite(Retired why) {
    return why == ByFault || why == ByAsset || why == ByImageStore || LayoutEvent(why);
}

inline constexpr std::size_t kBufferObjectBytes = 0x58;
inline constexpr std::size_t kBufferBaseOffset = 0x20;
inline constexpr std::size_t kBufferIdOffset = 0x28;
inline constexpr std::size_t kBufferSizeOffset = 0x2c;
inline constexpr std::size_t kBufferAlignedSizeOffset = 0x30;
inline constexpr std::size_t kBufferKindOffset = 0x38;
inline constexpr std::size_t kBufferUsageOffset = 0x39;
inline constexpr std::size_t kBufferCpuAccessOffset = 0x3a;
inline constexpr std::size_t kBufferBindOffset = 0x3b;
inline constexpr std::size_t kBufferDynamicOffset = 0x40;

inline constexpr std::uint8_t kKindBuffer = 1;
enum Usage : std::uint8_t { UsageDefault, UsageImmutable, UsageDynamic, UsageStaging, kUsages };
inline constexpr const char* kUsageNames[kUsages] = {"default", "immutable", "dynamic", "staging"};
inline constexpr std::uint8_t kBindVertex = 0x01;
inline constexpr std::uint8_t kBindIndex = 0x02;
inline constexpr std::uint8_t kBindConstant = 0x04;
inline constexpr std::uint8_t kBindShaderResource = 0x08;
inline constexpr std::uint8_t kBindStreamOutput = 0x10;
inline constexpr std::uint8_t kBindUnorderedAccess = 0x80;
inline constexpr unsigned kBindFlags = 8;
inline constexpr const char* kBindNames[kBindFlags] = {"VB", "IB", "CB", "SRV", "SO", "0x20", "0x40", "UAV"};

struct BufferTraits {
    std::uint8_t kind = 0, usage = 0, cpu_access = 0, bind = 0;
    bool dynamic = false, has_data = false;
};

enum Refusal : std::uint8_t {
    Sealable,
    RefusedKind,
    RefusedNotGeometry,
    RefusedUnorderedAccess,
    RefusedStreamOutput,
    RefusedDynamic,
    RefusedUsage,
    RefusedNoData,
    kRefusals
};
inline constexpr const char* kRefusalNames[kRefusals] = {"sealable", "not a buffer", "neither VB nor IB", "UAV",
                                                         "stream output", "dynamic", "dynamic or staging usage",
                                                         "default without data"};

inline constexpr Refusal SealRule(const BufferTraits& t) {
    if (t.kind != kKindBuffer) return RefusedKind;
    if (!(t.bind & (kBindVertex | kBindIndex))) return RefusedNotGeometry;
    if (t.bind & kBindUnorderedAccess) return RefusedUnorderedAccess;
    if (t.bind & kBindStreamOutput) return RefusedStreamOutput;
    if (t.dynamic) return RefusedDynamic;
    if (t.usage != UsageImmutable && t.usage != UsageDefault) return RefusedUsage;
    if (t.usage == UsageDefault && !t.has_data) return RefusedNoData;
    return Sealable;
}

enum Holder : std::uint8_t { HolderImmutable, HolderDefaultData, HolderDefaultEmpty, HolderDynamic, HolderOtherGx, kHolders };

inline constexpr Holder HolderOf(const BufferTraits& t) {
    if (t.dynamic || t.usage == UsageDynamic) return HolderDynamic;
    if (t.usage == UsageImmutable) return HolderImmutable;
    if (t.usage == UsageDefault) return t.has_data ? HolderDefaultData : HolderDefaultEmpty;
    return HolderOtherGx;
}

struct SealTally {
    std::uint64_t sealed[kUsages][1u << kBindFlags]{};
    std::uint64_t refused[kRefusals]{};
    std::uint64_t faults_in_sealed = 0, faults_left_sealed = 0;
};

inline int FormatSeals(char* out, std::size_t room, const char* when, const SealTally& t) {
    std::size_t used = 0;
    const auto put = [&](const char* format, auto... args) {
        if (used >= room) return;
        const int n = std::snprintf(out + used, room - used, format, args...);
        if (n > 0) used += static_cast<std::size_t>(n);
    };
    std::uint64_t sealed = 0, refused = 0;
    for (const auto& by_bind : t.sealed)
        for (const std::uint64_t n : by_bind) sealed += n;
    for (unsigned r = RefusedKind; r < kRefusals; ++r) refused += t.refused[r];
    put("GX seals (%s): %llu sealed, %llu refused; sealed by usage and bind:", when, static_cast<unsigned long long>(sealed),
        static_cast<unsigned long long>(refused));
    for (unsigned usage = 0; usage < kUsages; ++usage) {
        for (unsigned bind = 0; bind < (1u << kBindFlags); ++bind) {
            if (!t.sealed[usage][bind]) continue;
            put(" %s", kUsageNames[usage]);
            const char* sep = " ";
            for (unsigned flag = 0; flag < kBindFlags; ++flag) {
                if (!((bind >> flag) & 1)) continue;
                put("%s%s", sep, kBindNames[flag]);
                sep = "|";
            }
            put(" %llu;", static_cast<unsigned long long>(t.sealed[usage][bind]));
        }
    }
    put("%s", " refused:");
    for (unsigned r = RefusedKind; r < kRefusals; ++r) {
        if (t.refused[r]) put(" %s %llu;", kRefusalNames[r], static_cast<unsigned long long>(t.refused[r]));
    }
    put(" guest write faults in a sealed range %llu, still sealed after the fault %llu",
        static_cast<unsigned long long>(t.faults_in_sealed), static_cast<unsigned long long>(t.faults_left_sealed));
    return static_cast<int>(used < room ? used : room - 1);
}

class Registry {
public:
    explicit Registry(u32 capacity = 1u << 18) : capacity_(capacity), entries_(new Entry[capacity]) {}

    u32 Seal(u64 base, u64 size) {
        if (!size) return 0;
        std::scoped_lock lock(mutex_);
        RetireOverlapsLocked(base, size, ByOverlap);
        if (track_) retired_at_.erase(base);
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
        sealed_.fetch_add(1, std::memory_order_release);
        return id;
    }

    void RetireRange(u64 address, u64 size, Retired why) {
        if (!size) return;
        std::scoped_lock lock(mutex_);
        ++notes_;
        RetireOverlapsLocked(address, size, why);
        if (track_ && why == ByRelease) retired_at_.erase(address);
    }

    void TrackRetired() {
        std::scoped_lock lock(mutex_);
        track_ = true;
    }

    bool RetiredAt(u64 address, Retired* why) const {
        std::scoped_lock lock(mutex_);
        auto at = retired_at_.upper_bound(address);
        if (at == retired_at_.begin()) return false;
        --at;
        if (address - at->first >= at->second.size) return false;
        *why = at->second.why;
        return true;
    }

    bool Find(u64 address, u64 size, Hit* out) const {
        if (!size) return false;
        std::scoped_lock lock(mutex_);
        return FindLocked(address, out);
    }

    void FindAll(const u64* addresses, const u64* sizes, u32 count, Hit* out, bool* found) const {
        std::scoped_lock lock(mutex_);
        for (u32 i = 0; i < count; ++i) found[i] = sizes[i] != 0 && FindLocked(addresses[i], &out[i]);
    }

    bool Valid(const Hit& hit) const {
        if (!hit.id || hit.id >= capacity_) return false;
        const Entry& e = entries_[hit.id];
        return e.live.load(std::memory_order_acquire) && e.generation.load(std::memory_order_acquire) == hit.generation;
    }

    u64 Sealed() const { return sealed_.load(std::memory_order_acquire); }
    u64 RetiredCount(Retired why) const { return retired_[why]; }
    u32 Live() const {
        std::scoped_lock lock(mutex_);
        return static_cast<u32>(by_base_.size());
    }
    u64 Notes() const {
        std::scoped_lock lock(mutex_);
        return notes_;
    }

private:
    struct Entry {
        u64 base = 0, size = 0;
        std::atomic<u32> generation{0};
        std::atomic<bool> live{false};
    };

    bool FindLocked(u64 address, Hit* out) const {
        auto at = by_base_.upper_bound(address);
        if (at == by_base_.begin()) return false;
        --at;
        const Entry& e = entries_[at->second];
        if (address < e.base || address - e.base >= e.size) return false;
        *out = {at->second, e.generation.load(std::memory_order_acquire), e.base, e.size};
        return true;
    }

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
                if (track_) retired_at_[e.base] = {e.size, why};
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
    std::atomic<u64> sealed_{0};
    u64 retired_[kRetiredKinds]{};
    u64 notes_ = 0;
    struct RetiredRange {
        u64 size;
        Retired why;
    };
    std::map<u64, RetiredRange> retired_at_;
    bool track_ = false;
};
}
