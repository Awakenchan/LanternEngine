// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <vector>

namespace BbEngine::WorldCopy {

constexpr std::size_t kGroupBytes = 125;
constexpr std::size_t kEntryBytes = 4 + kGroupBytes;
constexpr std::size_t kMostPerFrame = 64;
constexpr std::uint8_t kFirstPart = 1, kLastPart = 2;
using Group = std::array<std::uint8_t, kGroupBytes>;
using Image = std::map<std::uint32_t, Group>;

constexpr std::uint32_t kDreamArea = 21, kDungeonArea = 29;

constexpr bool ShareGroup(std::uint32_t group) {
    const std::uint32_t type = group / 10000, area = group / 100 % 100, block = group / 10 % 10, zone = group % 10;
    if (type == 0) return area == 0 && block == 0 && (zone <= 2 || zone == 9);
    if (type != 1 && type != 5 && type != 6 && type != 7) return false;
    if (group == 60009) return false;
    if (area == 0) return block == 0;
    if (area == kDreamArea || area == kDungeonArea) return false;
    return !(type == 1 && zone >= 4 && zone <= 6);
}

constexpr bool ShareFlag(std::uint32_t id) {
    if (!ShareGroup(id / 1000)) return false;
    if (id >= 20 && id <= 29) return false;
    if (id >= 2000 && id <= 2099) return false;
    if (id >= 9020 && id <= 9026) return false;
    if (id == 9180) return false;
    if (id >= 9440 && id <= 9451) return false;
    if (id >= 9500 && id <= 9599) return false;
    if (id >= 9900 && id <= 9919) return false;
    if (id >= 70009000 && id <= 70009499) return false;
    return true;
}

inline Group MaskOf(std::uint32_t group) {
    Group mask{};
    for (std::uint32_t bit = 0; bit < kGroupBytes * 8; ++bit)
        if (ShareFlag(group * 1000 + bit)) mask[bit >> 3] |= std::uint8_t(0x80u >> (bit & 7));
    return mask;
}

inline Group Masked(const std::uint8_t* bytes, const Group& mask) {
    Group out{};
    for (std::size_t i = 0; i < kGroupBytes; ++i) out[i] = bytes[i] & mask[i];
    return out;
}

inline int WriteShared(std::uint8_t* to, const Group& value, const Group& mask) {
    int changed = 0;
    for (std::size_t i = 0; i < kGroupBytes; ++i) {
        const std::uint8_t next = std::uint8_t((to[i] & ~mask[i]) | (value[i] & mask[i]));
        if (next != to[i]) ++changed;
        to[i] = next;
    }
    return changed;
}

inline int WriteChanged(std::uint8_t* to, const Group& was, const Group& now, const Group& mask) {
    int changed = 0;
    for (std::size_t i = 0; i < kGroupBytes; ++i) {
        const std::uint8_t differ = std::uint8_t((was[i] ^ now[i]) & mask[i]);
        const std::uint8_t next = std::uint8_t((to[i] & ~differ) | (now[i] & differ));
        if (next != to[i]) ++changed;
        to[i] = next;
    }
    return changed;
}

inline bool IsZero(const Group& group) {
    for (const std::uint8_t b : group)
        if (b) return false;
    return true;
}

inline std::vector<std::vector<std::uint8_t>> Encode(const Image& groups, bool whole) {
    std::vector<std::vector<std::uint8_t>> frames;
    std::vector<std::uint8_t> frame;
    std::size_t count = 0;
    const auto flush = [&] {
        if (frame.empty()) frame.push_back(0);
        frames.push_back(std::move(frame));
        frame.clear();
        count = 0;
    };
    for (const auto& [group, bytes] : groups) {
        if (frame.empty()) frame.push_back(0);
        const std::size_t at = frame.size();
        frame.resize(at + kEntryBytes);
        std::memcpy(frame.data() + at, &group, 4);
        std::memcpy(frame.data() + at + 4, bytes.data(), kGroupBytes);
        if (++count == kMostPerFrame) flush();
    }
    if (!frame.empty() || frames.empty()) flush();
    if (whole) {
        frames.front()[0] |= kFirstPart;
        frames.back()[0] |= kLastPart;
    }
    return frames;
}

constexpr bool IsPickupGroup(std::uint32_t group) {
    return group / 10000 == 5;
}

constexpr bool IsNpcGroup(std::uint32_t group) {
    return group == 1 || group / 10000 == 7;
}

constexpr std::uint32_t kGiftFirst = 9040, kGiftLast = 9079;

constexpr bool IsGiftFlag(std::uint32_t id) {
    return id >= kGiftFirst && id <= kGiftLast;
}

inline int CountBits(const Group& bits) {
    int n = 0;
    for (std::uint8_t b : bits)
        for (; b; b &= std::uint8_t(b - 1)) ++n;
    return n;
}

inline Group TurnedOn(const Group& was, const Group& now, const Group& mask) {
    Group out{};
    for (std::size_t i = 0; i < kGroupBytes; ++i) out[i] = std::uint8_t(now[i] & ~was[i] & mask[i]);
    return out;
}

inline int SetBits(std::uint8_t* to, const Group& bits) {
    int changed = 0;
    for (std::size_t i = 0; i < kGroupBytes; ++i) {
        const std::uint8_t next = std::uint8_t(to[i] | bits[i]);
        if (next != to[i]) ++changed;
        to[i] = next;
    }
    return changed;
}

class Witness {
public:
    static constexpr int kMostAtOnce = 24;

    void Forget() {
        seen_.clear();
        ready_ = false;
    }
    bool Ready() const { return ready_; }
    void Start(const Image& now) {
        seen_ = now;
        ready_ = true;
    }
    Image Step(const Image& now, const std::map<std::uint32_t, Group>& masks, bool& rebuilt) {
        Image fresh;
        int bits = 0;
        for (const auto& [group, value] : now) {
            const auto mask = masks.find(group);
            if (mask == masks.end()) continue;
            const auto was = seen_.find(group);
            const Group on = TurnedOn(was == seen_.end() ? Group{} : was->second, value, mask->second);
            if (IsZero(on)) continue;
            bits += CountBits(on);
            fresh[group] = on;
        }
        seen_ = now;
        rebuilt = bits > kMostAtOnce;
        if (rebuilt) fresh.clear();
        return fresh;
    }

private:
    Image seen_;
    bool ready_ = false;
};

inline Group RangeBits(std::uint32_t group, std::uint32_t first, std::uint32_t last) {
    Group bits{};
    const std::uint32_t from = group * 1000, to = from + kGroupBytes * 8 - 1;
    for (std::uint32_t id = first < from ? from : first; id <= last && id <= to; ++id)
        bits[(id - from) >> 3] |= std::uint8_t(0x80u >> ((id - from) & 7));
    return bits;
}

inline void Remove(Group& from, const Group& bits) {
    for (std::size_t i = 0; i < kGroupBytes; ++i) from[i] &= std::uint8_t(~bits[i]);
}

inline void Add(Group& to, const Group& bits) {
    for (std::size_t i = 0; i < kGroupBytes; ++i) to[i] |= bits[i];
}

inline int CopyMasked(std::uint8_t* to, const std::uint8_t* from, const Group& mask) {
    return WriteShared(to, Masked(from, mask), mask);
}

class Receiver {
public:
    bool Take(const std::uint8_t* data, std::size_t size, Image& changed) {
        if (size < 1 || (size - 1) % kEntryBytes) return false;
        const std::uint8_t parts = data[0];
        if (parts & kFirstPart) {
            building_.clear();
            whole_ = true;
        }
        const bool in_whole = whole_;
        for (std::size_t at = 1; at < size; at += kEntryBytes) {
            std::uint32_t group = 0;
            std::memcpy(&group, data + at, 4);
            if (!ShareGroup(group)) continue;
            Group value;
            std::memcpy(value.data(), data + at + 4, kGroupBytes);
            if (in_whole) building_[group] = value;
            else Set(group, value, changed);
        }
        if (in_whole && (parts & kLastPart)) {
            whole_ = false;
            for (const auto& [group, was] : image_)
                if (!building_.count(group)) changed[group] = Group{};
            for (const auto& [group, value] : building_) Set(group, value, changed);
            for (auto it = image_.begin(); it != image_.end();)
                it = building_.count(it->first) ? std::next(it) : image_.erase(it);
            building_.clear();
            have_ = true;
        }
        return true;
    }
    bool Have() const { return have_; }
    const Image& Current() const { return image_; }
    Group Value(std::uint32_t group) const {
        const auto it = image_.find(group);
        return it == image_.end() ? Group{} : it->second;
    }

private:
    void Set(std::uint32_t group, const Group& value, Image& changed) {
        const auto it = image_.find(group);
        if (it != image_.end() && it->second == value) return;
        image_[group] = value;
        changed[group] = value;
    }
    Image image_, building_;
    bool whole_ = false, have_ = false;
};

}
