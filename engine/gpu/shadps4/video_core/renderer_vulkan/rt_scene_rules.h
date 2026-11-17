// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#ifdef LANTERN_RT_RULES_MUTANTS
namespace Vulkan::RtRules {
inline int g_mutant = 0;
}
#define LANTERN_RT_RULES_PICK(n, wrong, right) (::Vulkan::RtRules::g_mutant == (n) ? (wrong) : (right))
#else
#define LANTERN_RT_RULES_PICK(n, wrong, right) (right)
#endif

namespace Vulkan::RtRules {

class CameraSlots {
public:
    std::uint32_t Index(const void* camera) {
        const auto [it, added] = slots.try_emplace(camera, std::uint32_t(slots.size()));
        return LANTERN_RT_RULES_PICK(1, std::uint32_t(slots.size() - 1), it->second);
    }
    void Clear() noexcept {
        slots.clear();
    }
    [[nodiscard]] std::size_t Size() const noexcept {
        return slots.size();
    }

private:
    std::unordered_map<const void*, std::uint32_t> slots;
};

inline std::uint32_t LinearCameraIndex(std::vector<const void*>& cameras, const void* camera) {
    std::uint32_t index = 0;
    for (; index < cameras.size(); ++index) {
        if (cameras[index] == camera) {
            break;
        }
    }
    if (index == cameras.size()) {
        cameras.push_back(camera);
    }
    return index;
}

enum class Tier : std::uint8_t { Character, Near, Far };

inline constexpr float NearMeters = 25.0f;

inline Tier PickTier(bool character, float distance) {
    if (LANTERN_RT_RULES_PICK(21, false, character)) {
        return Tier::Character;
    }
    return distance >= 0.0f && LANTERN_RT_RULES_PICK(22, distance > NearMeters, distance < NearMeters)
               ? Tier::Near
               : Tier::Far;
}

inline float Distance(const float* eye, const float* at) {
    const float dx = at[0] - eye[0], dy = at[1] - eye[1], dz = at[2] - eye[2];
    const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
    return std::isfinite(d) ? d : -1.0f;
}

struct NewEntryBudget {
    std::uint32_t max_entries = 0;
    std::uint64_t max_ns = 0;
    std::uint32_t made = 0;
    std::uint64_t spent_ns = 0;

    [[nodiscard]] bool On() const noexcept {
        return max_entries != 0 || max_ns != 0;
    }
    [[nodiscard]] bool CountLeft() const noexcept {
        return max_entries == 0 || LANTERN_RT_RULES_PICK(2, made > max_entries, made < max_entries);
    }
    [[nodiscard]] bool TimeLeft() const noexcept {
        return max_ns == 0 || LANTERN_RT_RULES_PICK(3, true, spent_ns < max_ns);
    }
    [[nodiscard]] bool Allow() const noexcept {
        return CountLeft() && TimeLeft();
    }
    [[nodiscard]] bool HalfLeft() const noexcept {
        return (max_entries == 0 || made * 2 < max_entries) &&
               (max_ns == 0 || LANTERN_RT_RULES_PICK(23, spent_ns < max_ns, spent_ns * 2 < max_ns));
    }
    [[nodiscard]] bool AllowTier(Tier tier) const noexcept {
        switch (tier) {
        case Tier::Character:
            return true;
        case Tier::Near:
            return Allow();
        case Tier::Far:
            break;
        }
        return LANTERN_RT_RULES_PICK(24, Allow(), HalfLeft());
    }
    void Spend(std::uint64_t ns) noexcept {
        ++made;
        spent_ns += LANTERN_RT_RULES_PICK(4, 0, ns);
    }
    void Reset() noexcept {
        made = 0;
        spent_ns = LANTERN_RT_RULES_PICK(5, spent_ns, 0);
    }
};

inline std::uint64_t TriangleBound(std::uint32_t index_count, std::uint32_t instances, bool strip) {
    const std::uint64_t per =
        strip ? (index_count >= 3 ? index_count - 2u : 0u) : LANTERN_RT_RULES_PICK(6, index_count / 4u, index_count / 3u);
    return per * instances;
}

inline bool FitsScene(std::uint64_t scene_bytes, std::uint64_t limit, std::uint64_t bytes) {
    return LANTERN_RT_RULES_PICK(7, scene_bytes < limit, scene_bytes + bytes <= limit);
}

class RetryGate {
public:
    [[nodiscard]] bool Blocked(std::uint64_t key, std::uint64_t frame) const {
        const auto it = until.find(key);
        return it != until.end() && LANTERN_RT_RULES_PICK(8, frame <= it->second + 1, frame < it->second);
    }
    void Fail(std::uint64_t key, std::uint64_t frame, std::uint64_t wait) {
        until[key] = frame + wait;
    }
    void Prune(std::uint64_t frame) {
        std::erase_if(until, [frame](const auto& item) { return item.second <= frame; });
    }
    [[nodiscard]] std::size_t Size() const noexcept {
        return until.size();
    }

private:
    std::unordered_map<std::uint64_t, std::uint64_t> until;
};

struct EvictLimits {
    std::uint64_t unused_frames = 240;
    std::uint64_t kept_frames = 2;
    std::uint64_t hold_frames = 90;
    bool hold = false;
};

struct EvictResult {
    std::uint64_t keep_from = 0;
    std::uint64_t spared = 0;
    std::uint64_t pressure_from = 0;
};

inline EvictResult EvictBefore(std::vector<std::pair<std::uint64_t, std::uint64_t>> aged,
                               std::uint64_t bytes, std::uint64_t limit, std::uint64_t frame,
                               const EvictLimits& limits) {
    EvictResult result{frame > limits.unused_frames ? frame - limits.unused_frames : 0, 0};
    if (bytes <= limit - limit / 8) {
        return result;
    }
    const std::uint64_t target =
        limits.hold ? LANTERN_RT_RULES_PICK(9, limit - limit / 4, limit - limit * 3 / 8) : limit - limit / 4;
    const std::uint64_t protect =
        limits.hold && LANTERN_RT_RULES_PICK(10, true, bytes <= limit) ? limits.hold_frames : limits.kept_frames;
    std::ranges::sort(aged);
    std::size_t at = 0;
    for (; at < aged.size() && bytes > target && aged[at].first + protect < frame; ++at) {
        bytes -= std::min(bytes, aged[at].second);
        result.keep_from = std::max(result.keep_from, aged[at].first + 1);
        result.pressure_from = LANTERN_RT_RULES_PICK(61, 0, std::max(result.pressure_from, aged[at].first + 1));
    }
    if (limits.hold) {
        for (std::size_t k = at; k < aged.size() && bytes > target; ++k) {
            if (aged[k].first + protect >= frame && aged[k].first + limits.kept_frames < frame) {
                ++result.spared;
            }
        }
    }
    return result;
}

inline constexpr std::uint64_t KeepFrames = 4;

inline bool KeepInScene(bool keep_on, bool built, std::uint64_t age) {
    return keep_on && LANTERN_RT_RULES_PICK(11, true, built) &&
           LANTERN_RT_RULES_PICK(19, true, age <= KeepFrames);
}

inline bool RefitInstead(bool keep_on, bool built, bool over_cap) {
    return keep_on && over_cap && LANTERN_RT_RULES_PICK(20, true, built);
}

inline bool BuiltAfterUnrecorded(bool built, bool update) {
    return built && LANTERN_RT_RULES_PICK(12, true, update);
}

enum class Rigid : std::uint8_t { Unknown, Still, Moved, Written };

inline constexpr std::uint32_t PaletteHashLimit = 16384;

inline bool PaletteBuffer(std::uint32_t size, bool only_const_reads) {
    return LANTERN_RT_RULES_PICK(13, true, !only_const_reads) && size >= 48 &&
           LANTERN_RT_RULES_PICK(14, true, size != 864);
}

inline bool PaletteHashed(std::uint32_t size) {
    return LANTERN_RT_RULES_PICK(25, true, size <= PaletteHashLimit);
}

struct Bones {
    std::uint64_t hash = 0;
    bool present = false;
    bool written = false;
    bool pooled = false;
    std::uint32_t bytes = 0;
};

inline Rigid CheckBones(std::uint64_t built, const Bones& now) {
    if (!LANTERN_RT_RULES_PICK(16, true, now.present)) {
        return Rigid::Unknown;
    }
    if (LANTERN_RT_RULES_PICK(15, false, now.written)) {
        return Rigid::Written;
    }
    if (LANTERN_RT_RULES_PICK(26, false, now.pooled)) {
        return Rigid::Unknown;
    }
    return LANTERN_RT_RULES_PICK(17, built == now.hash, built != now.hash) ? Rigid::Moved : Rigid::Still;
}

inline std::uint64_t BonesAfterCapture([[maybe_unused]] std::uint64_t built, const Bones& now) {
    return LANTERN_RT_RULES_PICK(18, built, now.hash);
}

struct RigidBones {
    std::uint64_t hash[2] = {0, 0};

    [[nodiscard]] Rigid Check(bool capturing, bool camera_pass, const Bones& now) const {
        if (!LANTERN_RT_RULES_PICK(27, true, capturing)) {
            return Rigid::Unknown;
        }
        return CheckBones(hash[camera_pass], now);
    }
    void Captured(bool camera_pass, const Bones& now) {
        if (LANTERN_RT_RULES_PICK(29, true, now.present && !now.written && !now.pooled)) {
            const bool slot = LANTERN_RT_RULES_PICK(28, !camera_pass, camera_pass);
            hash[slot] = BonesAfterCapture(hash[slot], now);
            hash[!slot] = LANTERN_RT_RULES_PICK(30, hash[!slot], 0);
        }
    }
};

struct ReadyMesh {
    std::uint64_t key = 0;
    float distance = -1.0f;
    bool unbounded = false;
    std::uint32_t triangles = 0;
};

inline bool Nearer(const ReadyMesh& a, const ReadyMesh& b) {
    if (a.unbounded != b.unbounded) {
        return a.unbounded;
    }
    const bool a_known = a.distance >= 0.0f, b_known = b.distance >= 0.0f;
    if (a_known != b_known) {
        return LANTERN_RT_RULES_PICK(32, b_known, a_known);
    }
    if (!a_known) {
        return false;
    }
    return LANTERN_RT_RULES_PICK(31, a.distance > b.distance, a.distance < b.distance);
}

struct HandOffLimits {
    std::uint32_t entries = 64;
    std::uint64_t triangles = 200000;
};

template <class T>
class HandOff {
public:
    HandOff() = default;
    HandOff(const HandOff&) = delete;
    HandOff& operator=(const HandOff&) = delete;
    void Add(const ReadyMesh& mesh, T value) {
        items.push_back({mesh, std::move(value)});
    }
    template <class Use>
    std::uint32_t Take(const HandOffLimits& limits, Use&& use) {
        std::stable_sort(items.begin(), items.end(),
                         [](const Item& a, const Item& b) { return Nearer(a.mesh, b.mesh); });
        std::uint32_t taken = 0;
        std::uint64_t triangles = 0;
        std::vector<Item> left;
        for (Item& item : items) {
            const bool room =
                LANTERN_RT_RULES_PICK(33, taken <= limits.entries, taken < limits.entries) &&
                (taken == 0 ||
                 LANTERN_RT_RULES_PICK(34, true, triangles + item.mesh.triangles <= limits.triangles));
            const bool exempt = LANTERN_RT_RULES_PICK(35, false, item.mesh.unbounded);
            if (exempt || room) {
                if (!exempt) {
                    ++taken;
                    triangles += item.mesh.triangles;
                }
                use(item.mesh, std::move(item.value));
            } else if (LANTERN_RT_RULES_PICK(36, false, true)) {
                left.push_back(std::move(item));
            }
        }
        items = std::move(left);
        return taken;
    }
    template <class Retire>
    void Drain(Retire&& retire) {
        for (std::size_t i = 0; i < LANTERN_RT_RULES_PICK(45, items.size() / 2, items.size()); ++i) {
            retire(items[i].mesh, std::move(items[i].value));
        }
        items.clear();
    }
    [[nodiscard]] std::size_t Size() const noexcept {
        return items.size();
    }

private:
    struct Item {
        ReadyMesh mesh;
        T value;
    };
    std::vector<Item> items;
};

class SetupKeys {
public:
    [[nodiscard]] bool Pending(std::uint64_t key) const {
        return LANTERN_RT_RULES_PICK(37, false, keys.contains(key));
    }
    void Submitted(std::uint64_t key, std::uint64_t bytes) {
        if (keys.try_emplace(key, bytes).second) {
            total += bytes;
        }
    }
    void Landed(std::uint64_t key) {
        const auto it = keys.find(key);
        if (it == keys.end() || !LANTERN_RT_RULES_PICK(38, false, true)) {
            return;
        }
        total -= LANTERN_RT_RULES_PICK(47, 0, std::min(total, it->second));
        keys.erase(it);
    }
    [[nodiscard]] std::size_t Size() const noexcept {
        return keys.size();
    }
    [[nodiscard]] std::uint64_t Bytes() const noexcept {
        return total;
    }

private:
    std::unordered_map<std::uint64_t, std::uint64_t> keys;
    std::uint64_t total = 0;
};

inline bool FitsWithPending(std::uint64_t scene_bytes, std::uint64_t pending_bytes, std::uint64_t limit,
                            std::uint64_t bytes) {
    return FitsScene(scene_bytes + LANTERN_RT_RULES_PICK(48, 0, pending_bytes), limit, bytes);
}

struct CopyBudget {
    std::uint64_t max_bytes = 4ull << 20;
    std::size_t max_in_flight = 1024;
    std::uint64_t used = 0;

    [[nodiscard]] bool Allow(Tier tier, std::uint64_t bytes, std::size_t in_flight) const noexcept {
        if (LANTERN_RT_RULES_PICK(46, false, tier == Tier::Character)) {
            return true;
        }
        if (LANTERN_RT_RULES_PICK(40, false, in_flight >= max_in_flight)) {
            return false;
        }
        if (LANTERN_RT_RULES_PICK(41, false, used == 0)) {
            return true;
        }
        if (tier == Tier::Near) {
            return used + bytes <= max_bytes;
        }
        return used + bytes <= LANTERN_RT_RULES_PICK(39, max_bytes, max_bytes / 2);
    }
    void Spend(std::uint64_t bytes) noexcept {
        used += bytes;
    }
    void Reset() noexcept {
        used = 0;
    }
};

enum class Landing : std::uint8_t { Insert, Retire };

inline Landing Land(bool device_lost, bool key_taken, bool fits) {
    return LANTERN_RT_RULES_PICK(44, false, device_lost) || LANTERN_RT_RULES_PICK(42, false, key_taken) ||
                   !LANTERN_RT_RULES_PICK(43, true, fits)
               ? Landing::Retire
               : Landing::Insert;
}


inline std::uint32_t OrderedBits(float value) {
    const auto bits = std::bit_cast<std::uint32_t>(value);
    return (bits & 0x80000000u) ? ~bits : LANTERN_RT_RULES_PICK(49, bits, bits | 0x80000000u);
}

inline float FromOrderedBits(std::uint32_t bits) {
    return std::bit_cast<float>((bits & 0x80000000u) ? LANTERN_RT_RULES_PICK(50, bits, bits & 0x7fffffffu)
                                                      : ~bits);
}

inline constexpr std::uint32_t EmptyLow = 0xffffffffu, EmptyHigh = 0;

struct Box {
    std::array<float, 3> lo{}, hi{};
};

inline bool BoxFromBits(const std::uint32_t* bits, Box& out) {
    for (int i = 0; i < 3; ++i) {
        out.lo[i] = FromOrderedBits(bits[i]);
        out.hi[i] = FromOrderedBits(bits[3 + i]);
        if (LANTERN_RT_RULES_PICK(51, false, !std::isfinite(out.lo[i]) || !std::isfinite(out.hi[i]) ||
                                                 out.lo[i] > out.hi[i])) {
            return false;
        }
    }
    return true;
}

inline bool OutsideFrustum(const Box& box, const std::array<float, 12>& view,
                           const std::array<float, 4>& proj) {
    const float sx = std::abs(proj[0]), sy = std::abs(proj[1]);
    std::array<int, 5> out{};
    for (int corner = 0; corner < 8; ++corner) {
        const float p[3] = {corner & 1 ? box.hi[0] : box.lo[0], corner & 2 ? box.hi[1] : box.lo[1],
                            corner & 4 ? box.hi[2] : box.lo[2]};
        float v[3];
        for (int r = 0; r < 3; ++r) {
            v[r] = view[r * 4] * p[0] + view[r * 4 + 1] * p[1] + view[r * 4 + 2] * p[2] + view[r * 4 + 3];
        }
        out[0] += LANTERN_RT_RULES_PICK(54, false, v[2] <= 0.0f) ? 1 : 0;
        out[1] += sx * v[0] < -v[2] ? 1 : 0;
        out[2] += LANTERN_RT_RULES_PICK(52, sx * v[0] < v[2], sx * v[0] > v[2]) ? 1 : 0;
        out[3] += sy * v[1] < -v[2] ? 1 : 0;
        out[4] += sy * v[1] > v[2] ? 1 : 0;
    }
    return std::ranges::any_of(out, [](int n) { return LANTERN_RT_RULES_PICK(53, n > 0, n == 8); });
}

inline float BoxDistance(const Box& box, const float* at) {
    float sum = 0.0f;
    for (int i = 0; i < 3; ++i) {
        const float d = std::max({box.lo[i] - at[i], 0.0f, at[i] - box.hi[i]});
        sum += d * d;
    }
    return std::sqrt(sum);
}

inline constexpr float ShadowReachMeters = 40.0f;

inline bool StaticCaster(bool built, bool dynamic, bool rebuilt) {
    return built && !dynamic && !rebuilt;
}

inline bool Hidden(std::uint64_t seen_frame, std::uint64_t gone_frame) {
    return LANTERN_RT_RULES_PICK(59, seen_frame <= gone_frame, seen_frame < gone_frame);
}

enum class Undrawn : std::uint8_t { Add, Gone, Far };

inline Undrawn ClassifyUndrawn(bool outside, float distance, float reach) {
    if (!(distance >= 0.0f) || LANTERN_RT_RULES_PICK(56, distance >= reach * 2.0f, distance > reach)) {
        return Undrawn::Far;
    }
    return LANTERN_RT_RULES_PICK(55, true, outside) ? Undrawn::Add : Undrawn::Gone;
}

struct Caster {
    std::uint32_t id = 0;
    float distance = 0.0f;
};

inline std::size_t KeepNearest(std::vector<Caster>& casters, std::size_t room) {
    std::ranges::sort(casters, [](const Caster& a, const Caster& b) {
        return a.distance != b.distance ? LANTERN_RT_RULES_PICK(58, a.distance > b.distance, a.distance < b.distance)
                                        : a.id < b.id;
    });
    const std::size_t dropped = casters.size() > room ? casters.size() - room : 0;
    casters.resize(std::min(casters.size(), LANTERN_RT_RULES_PICK(57, room + 1, room)));
    return dropped;
}

inline constexpr std::uint64_t StaticFrames = 2700, DarkFrames = 30, RecentFrames = 600;

inline std::uint64_t StaticKeepFrom(const EvictResult& plan, std::uint64_t frame) {
    return std::max(plan.pressure_from,
                    LANTERN_RT_RULES_PICK(63, std::uint64_t(0), frame > StaticFrames ? frame - StaticFrames : 0));
}

inline bool KeepStatics(bool persistent, bool dark) {
    return persistent && LANTERN_RT_RULES_PICK(64, true, !dark);
}

inline bool Evicted(std::uint64_t seen_frame, const EvictResult& plan, bool keep_statics, bool is_static,
                    std::uint64_t frame) {
    return seen_frame < (LANTERN_RT_RULES_PICK(60, false, keep_statics && is_static) ? StaticKeepFrom(plan, frame)
                                                                                     : plan.keep_from);
}

inline bool StaleReturn(std::uint64_t frame, std::uint64_t seen_frame, std::uint64_t unused_frames,
                        std::uint64_t stored_hash, std::uint64_t hash) {
    return LANTERN_RT_RULES_PICK(65, true, frame - seen_frame > unused_frames) &&
           LANTERN_RT_RULES_PICK(66, false, stored_hash != hash);
}

inline bool RecentCaster(std::uint64_t frame, std::uint64_t seen_frame) {
    return LANTERN_RT_RULES_PICK(67, true, frame - seen_frame <= RecentFrames);
}

inline bool SceneUnchanged(std::uint32_t count, std::uint32_t built_count, std::uint32_t drawn,
                           std::uint32_t built_drawn) {
    return count == built_count && LANTERN_RT_RULES_PICK(62, true, drawn == built_drawn);
}

template <class T>
class TickQueue {
public:
    void Add(std::uint64_t tick, T value) {
        items.push_back({tick, std::move(value)});
    }
    [[nodiscard]] std::vector<T> TakeDone(std::uint64_t gpu_tick) {
        std::size_t count = 0;
        while (count < items.size() &&
               LANTERN_RT_RULES_PICK(68, items[count].tick < gpu_tick, items[count].tick <= gpu_tick)) {
            ++count;
        }
        if (LANTERN_RT_RULES_PICK(69, count < items.size(), false)) {
            ++count;
        }
        std::vector<T> done;
        done.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            done.push_back(std::move(items[LANTERN_RT_RULES_PICK(70, count - 1 - i, i)].value));
        }
        items.erase(items.begin(), items.begin() + std::ptrdiff_t(LANTERN_RT_RULES_PICK(71, 0, count)));
        return done;
    }
    [[nodiscard]] std::uint64_t FirstTick() const noexcept {
        return items.empty() ? 0 : items.front().tick;
    }
    [[nodiscard]] bool Empty() const noexcept {
        return items.empty();
    }
    [[nodiscard]] std::size_t Size() const noexcept {
        return items.size();
    }

private:
    struct Item {
        std::uint64_t tick;
        T value;
    };
    std::deque<Item> items;
};

struct Pose {
    std::uint64_t hash = 0;
    bool known = false;
};

struct CapturedPose {
    std::uint64_t hash[2] = {0, 0};
    bool known[2] = {false, false};

    [[nodiscard]] bool Changed(bool camera_pass, const Pose& now) const {
        if (LANTERN_RT_RULES_PICK(72, false, !now.known || !known[camera_pass])) {
            return true;
        }
        return LANTERN_RT_RULES_PICK(73, hash[camera_pass] == now.hash, hash[camera_pass] != now.hash);
    }
    void Captured(bool camera_pass, const Pose& now) {
        const bool slot = LANTERN_RT_RULES_PICK(74, !camera_pass, camera_pass);
        hash[slot] = now.hash;
        known[slot] = LANTERN_RT_RULES_PICK(75, true, now.known);
        known[!slot] = LANTERN_RT_RULES_PICK(81, known[!slot], false);
    }
};

inline constexpr std::uint64_t CutRefitEvery = 4;

inline bool CutOnly(std::uint8_t mask, std::uint8_t cut_bits) {
    return mask != 0 && LANTERN_RT_RULES_PICK(76, (mask & cut_bits) != 0, (mask & ~cut_bits) == 0);
}

enum class Refit : std::uint8_t { Now, Unchanged, Wait };

inline Refit DynamicRefit(bool on, bool pose_changed, bool cut_only, std::uint64_t frame, std::uint64_t key) {
    if (!LANTERN_RT_RULES_PICK(77, true, on)) {
        return Refit::Now;
    }
    if (!LANTERN_RT_RULES_PICK(78, true, pose_changed)) {
        return Refit::Unchanged;
    }
    if (LANTERN_RT_RULES_PICK(79, false, cut_only) &&
        LANTERN_RT_RULES_PICK(80, (frame + key) % CutRefitEvery == 0, (frame + key) % CutRefitEvery != 0)) {
        return Refit::Wait;
    }
    return Refit::Now;
}

#ifdef LANTERN_RT_RULES_MUTANTS
inline constexpr int kMutants = 81;
#endif

}
