// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <type_traits>
#include <unordered_map>

namespace BbEngine::PipelineMemo {

enum Prepared : std::uint8_t {
    Used,
    Pending,
    Unavailable,
    Checksum,
    Program,
    Flat,
    NoPipeline,
    Records,
    Rejected,
    kPrepared
};

inline const char* PreparedName(Prepared reason) {
    static constexpr const char* names[kPrepared] = {
        "used", "pending", "unavailable", "checksum", "program", "flat data", "no pipeline yet",
        "GX record", "draw dropped"};
    return reason < kPrepared ? names[reason] : "?";
}

struct PreparedTally {
    std::array<std::uint64_t, kPrepared> counts{};

    void Note(Prepared reason) {
        ++counts[reason < kPrepared ? reason : Rejected];
    }
    std::uint64_t Total() const {
        std::uint64_t total = 0;
        for (const auto count : counts) total += count;
        return total;
    }
    std::uint64_t Missed() const {
        return Total() - counts[Used];
    }
    void Clear() {
        counts = {};
    }
    int Format(char* out, std::size_t size) const {
        int at = std::snprintf(out, size, "%llu used, %llu not",
                               static_cast<unsigned long long>(counts[Used]),
                               static_cast<unsigned long long>(Missed()));
        bool first = true;
        for (std::uint32_t r = Used + 1; r < kPrepared; ++r) {
            if (!counts[r] || at < 0 || std::size_t(at) >= size) continue;
            at += std::snprintf(out + at, size - at, "%s%s %llu", first ? " (" : ", ",
                                PreparedName(Prepared(r)), static_cast<unsigned long long>(counts[r]));
            first = false;
        }
        if (!first && at >= 0 && std::size_t(at) < size) at += std::snprintf(out + at, size - at, ")");
        return at;
    }
};

class KeyBuilder {
public:
    static constexpr std::uint32_t kWords = 256;

    void Clear() {
        count = 0;
        overflow = false;
    }
    template <typename T>
    void Add(const T& value) {
        static_assert(std::is_trivially_copyable_v<T>);
        constexpr std::uint32_t need = (sizeof(T) + 7) / 8;
        if (count + need > kWords) {
            overflow = true;
            return;
        }
        std::uint64_t tmp[need] = {};
        std::memcpy(tmp, &value, sizeof(T));
        for (std::uint32_t i = 0; i < need; ++i) words[count++] = tmp[i];
    }
    bool Overflow() const {
        return overflow;
    }
    std::uint32_t Count() const {
        return count;
    }
    std::uint64_t Hash() const {
        std::uint64_t h = 0x27D4EB2F165667C5ull ^ (std::uint64_t(count) * 0x9E3779B97F4A7C15ull);
        for (std::uint32_t i = 0; i < count; ++i) {
            std::uint64_t k = words[i] * 0xC2B2AE3D27D4EB4Full;
            k = (k << 31) | (k >> 33);
            h ^= k * 0x9E3779B97F4A7C15ull;
            h = ((h << 27) | (h >> 37)) * 0x9E3779B97F4A7C15ull + 0x85EBCA77C2B2AE63ull;
        }
        h ^= h >> 33;
        h *= 0xFF51AFD7ED558CCDull;
        h ^= h >> 33;
        h *= 0xC4CEB9FE1A85EC53ull;
        h ^= h >> 33;
        return h;
    }

private:
    std::array<std::uint64_t, kWords> words{};
    std::uint32_t count = 0;
    bool overflow = false;
};

enum Miss : std::uint8_t { Unseen, Unkeyable, kMisses };

struct Stats {
    std::uint64_t probes = 0, hits = 0, differ = 0, compared = 0;
    std::array<std::uint64_t, kMisses> misses{};
    std::uint64_t key_ns = 0, select_ns = 0, timed = 0;

    double HitShare() const {
        return probes ? double(hits) / double(probes) : 0.0;
    }
    bool PassesGate() const {
        return probes && compared && HitShare() >= 0.8 && differ == 0;
    }
};

class Memo {
public:
    static constexpr std::size_t kMaxEntries = std::size_t(1) << 18;

    const void* Probe(bool keyable, std::uint64_t key) {
        ++stats.probes;
        if (!keyable) {
            ++stats.misses[Unkeyable];
            return nullptr;
        }
        const auto it = entries.find(key);
        if (it == entries.end()) {
            ++stats.misses[Unseen];
            return nullptr;
        }
        ++stats.hits;
        return it->second;
    }
    void Remember(std::uint64_t key, const void* pipeline) {
        if (!pipeline) return;
        if (entries.size() >= kMaxEntries) entries.clear();
        entries[key] = pipeline;
    }
    bool Compare(const void* memo, const void* actual) {
        ++stats.compared;
        const bool same = memo == actual;
        stats.differ += !same;
        return same;
    }
    void Time(std::uint64_t key_ns, std::uint64_t select_ns) {
        stats.key_ns += key_ns;
        stats.select_ns += select_ns;
        ++stats.timed;
    }
    std::size_t Size() const {
        return entries.size();
    }
    Stats TakeStats() {
        Stats out = stats;
        stats = {};
        return out;
    }
    const Stats& Current() const {
        return stats;
    }

private:
    std::unordered_map<std::uint64_t, const void*> entries;
    Stats stats;
};

inline int FormatStats(const Stats& s, bool compare, std::size_t entries, char* out, std::size_t size) {
    const double key_us = s.timed ? double(s.key_ns) / double(s.timed) / 1000.0 : 0.0;
    const double select_us = s.timed ? double(s.select_ns) / double(s.timed) / 1000.0 : 0.0;
    return std::snprintf(out, size,
                         "Pipeline memo (LANTERN_PIPELINE_MEMO=%c): %llu draws, %llu hits (%.1f%%), "
                         "misses: %llu unseen, %llu unkeyable; %s%llu differ of %llu compared; "
                         "%zu remembered; key %.3f us a draw, selection %.3f us a draw (%llu timed)",
                         compare ? '2' : '1', static_cast<unsigned long long>(s.probes),
                         static_cast<unsigned long long>(s.hits), s.HitShare() * 100.0,
                         static_cast<unsigned long long>(s.misses[Unseen]),
                         static_cast<unsigned long long>(s.misses[Unkeyable]),
                         compare ? "" : "not compared, ", static_cast<unsigned long long>(s.differ),
                         static_cast<unsigned long long>(s.compared), entries, key_us, select_us,
                         static_cast<unsigned long long>(s.timed));
}

class FreshFlats {
public:
    static constexpr std::uint32_t kEntries = 8;

    void Reset() {
        count = 0;
    }
    void Note(const void* info, std::uint64_t base, const void* user_data, std::size_t size) {
        for (std::uint32_t i = 0; i < count; ++i) {
            if (entries[i].info == info) {
                entries[i] = {info, base, user_data, size};
                return;
            }
        }
        if (count < kEntries) entries[count++] = {info, base, user_data, size};
    }
    void Forget(const void* info) {
        for (std::uint32_t i = 0; i < count; ++i) {
            if (entries[i].info == info) {
                entries[i] = entries[--count];
                return;
            }
        }
    }
    bool Fresh(const void* info, std::uint64_t base, const void* user_data, std::size_t size) const {
        for (std::uint32_t i = 0; i < count; ++i) {
            const Entry& e = entries[i];
            if (e.info == info) {
                return e.base == base && e.user_data == user_data && e.size == size;
            }
        }
        return false;
    }
    std::uint32_t Count() const {
        return count;
    }

private:
    struct Entry {
        const void* info;
        std::uint64_t base;
        const void* user_data;
        std::size_t size;
    };
    std::array<Entry, kEntries> entries{};
    std::uint32_t count = 0;
};

inline std::uint32_t StagesToRefresh(std::uint32_t unfilled, std::uint32_t refreshed) {
    return unfilled & ~refreshed;
}

}
