// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Vulkan::Motion {

#ifdef LANTERN_MOTION_HISTORY_MUTANTS
enum class Mutant : int {
    None = 0,
    NoIncumbency,
    NoCharacterPriority,
    PerDraw,
    StillKeepsReservation,
    RevalidateBurst,
    kCount
};
inline int g_mutant = 0;
inline bool Is(Mutant m) {
    return g_mutant == int(m);
}
#define LANTERN_MH_MUTANT(name) (::Vulkan::Motion::Is(::Vulkan::Motion::Mutant::name))
#else
#define LANTERN_MH_MUTANT(name) false
#endif

struct VertexRange {
    uint32_t first{}, count{};
    bool operator==(const VertexRange&) const = default;
};

// Reserve only the vertices referenced by this draw, not the whole shared vertex buffer.
// Primitive restart markers are not vertex shader invocations. Reject signed overflow.
template <class Index, size_t Extent>
VertexRange IndexedRange(std::span<const Index, Extent> indices, int32_t base_vertex, bool restart) {
    uint32_t lo = UINT32_MAX, hi = 0;
    for (const auto index : indices) {
        if (restart && index == std::numeric_limits<Index>::max()) {
            continue;
        }
        lo = std::min(lo, uint32_t(index));
        hi = std::max(hi, uint32_t(index));
    }
    if (lo > hi || int64_t(lo) + base_vertex < 0 ||
        int64_t(hi) + base_vertex > INT32_MAX) {
        return {};
    }
    return {uint32_t(int64_t(lo) + base_vertex), hi - lo + 1};
}

// Vertex shader constant buffers (stride 16) of Bloodborne's G-buffer shaders: the 864-byte
// scene constants (camera), 416-byte model constants and palettes of 3x4 bone matrices
// (48 bytes per bone). Characters have 656+ bytes; weapons and props 2-8 bones (96-384).
enum class BufferRole { Other, Skeleton, SmallSkeleton };
inline BufferRole ClassifyBuffer(uint32_t size) {
    if (size == 864 || size > 16384) {
        return BufferRole::Other;
    }
    if (size >= 640) {
        return BufferRole::Skeleton;
    }
    return size >= 96 && size % 48 == 0 ? BufferRole::SmallSkeleton : BufferRole::Other;
}

// Scanning and hashing the index list of every stored draw each frame took ~10% of the GPU
// thread. Static meshes keep their index buffers, so a result is reused and fully verified
// again every `Revalidate` frames. A stale result only costs camera-quality vectors: the
// shader bounds every history access by the stored range.
class IndexRangeCache {
public:
    static constexpr uint64_t Revalidate = 32, Unused = 600;
    struct Key {
        uint64_t address{};
        uint32_t count{}, index_size{};
        int32_t base_vertex{};
        bool restart{};
        bool operator==(const Key&) const = default;
    };
    struct Result {
        VertexRange range;
        uint64_t topology{};
    };
    struct Stats {
        uint64_t hits{}, scans{}, stale{};
    } stats;

    static uint64_t Phase(const Key& key) {
        if (LANTERN_MH_MUTANT(RevalidateBurst)) {
            return 0;
        }
        return (uint64_t(KeyHash{}(key)) >> 32) % Revalidate;
    }
    template <class Scan>
    Result Get(const Key& key, uint64_t frame, Scan&& scan) {
        auto& entry = entries[key];
        entry.last_use = frame;
        if (entry.valid && frame - entry.verified < Revalidate) {
            ++stats.hits;
            return entry.result;
        }
        ++stats.scans;
        const Result fresh = scan();
        if (entry.valid && (fresh.topology != entry.result.topology ||
                            !(fresh.range == entry.result.range))) {
            ++stats.stale; // the cached result was wrong for up to Revalidate frames
        }
        entry.result = fresh;
        entry.verified = entry.valid ? frame : frame - Phase(key);
        entry.valid = true;
        return entry.result;
    }
    void Trim(uint64_t frame) {
        std::erase_if(entries, [frame](const auto& e) { return frame - e.second.last_use > Unused; });
    }
    size_t Size() const { return entries.size(); }

private:
    struct Entry {
        Result result;
        uint64_t verified{}, last_use{};
        bool valid{};
    };
    struct KeyHash {
        size_t operator()(const Key& k) const {
            uint64_t h = k.address * 0x9e3779b97f4a7c15ULL;
            h ^= (uint64_t(k.count) << 32 | uint32_t(k.base_vertex)) + (h << 6) + (h >> 2);
            h ^= uint64_t(k.index_size << 1 | uint32_t(k.restart)) + (h << 6) + (h >> 2);
            return size_t(h);
        }
    };
    std::unordered_map<Key, Entry, KeyHash> entries;
};

// Open-addressing map for the per-frame tables of History: entries are keyed per draw and
// the tables are cleared every frame. std::unordered_map allocated a node per insert (four
// per draw) and freed them all at the frame end; here clear() only starts a new generation.
template <class Key, class Value, class Hash>
class FlatMap {
public:
    FlatMap() : slots(64) {}

    Value& operator[](const Key& key) {
        return Insert(key).first->value;
    }
    /// Inserts `value` when `key` is absent (like std::unordered_map::emplace).
    void emplace(const Key& key, const Value& value) {
        const auto [slot, inserted] = Insert(key);
        if (inserted) {
            slot->value = value;
        }
    }
    const Value* find(const Key& key) const {
        const size_t mask = slots.size() - 1;
        for (size_t i = Start(key) & mask;; i = (i + 1) & mask) {
            const Slot& slot = slots[i];
            if (slot.generation != generation) {
                return nullptr;
            }
            if (slot.key == key) {
                return &slot.value;
            }
        }
    }
    void clear() {
        size = 0;
        if (++generation == 0) { // wrapped: stale slots could look current
            for (auto& slot : slots) {
                slot.generation = 0;
            }
            generation = 1;
        }
    }
    void swap(FlatMap& other) noexcept {
        slots.swap(other.slots);
        std::swap(size, other.size);
        std::swap(generation, other.generation);
    }

private:
    struct Slot {
        Key key{};
        Value value{};
        uint32_t generation = 0;
    };
    static size_t Start(const Key& key) {
        const uint64_t h = uint64_t(Hash{}(key)) * 0x9e3779b97f4a7c15ULL;
        return size_t(h ^ (h >> 29));
    }
    std::pair<Slot*, bool> Insert(const Key& key) {
        if ((size + 1) * 2 > slots.size()) {
            Grow();
        }
        const size_t mask = slots.size() - 1;
        for (size_t i = Start(key) & mask;; i = (i + 1) & mask) {
            Slot& slot = slots[i];
            if (slot.generation != generation) {
                slot = {key, Value{}, generation};
                ++size;
                return {&slot, true};
            }
            if (slot.key == key) {
                return {&slot, false};
            }
        }
    }
    void Grow() {
        std::vector<Slot> old(slots.size() * 2);
        old.swap(slots);
        const uint32_t live = generation;
        generation = 1;
        size = 0;
        for (const auto& slot : old) {
            if (slot.generation == live) {
                *Insert(slot.key).first = {slot.key, slot.value, generation};
            }
        }
    }

    std::vector<Slot> slots;
    size_t size = 0;
    uint32_t generation = 1;
};

inline uint32_t JitterPhases(uint32_t render_width, uint32_t output_width) {
    if (!render_width || !output_width) {
        return 8;
    }
    const double ratio = double(output_width) / render_width;
    return uint32_t(std::clamp(std::ceil(8.0 * ratio * ratio), 8.0, 256.0));
}

struct Draw {
    uint64_t shader{}, geometry{}, indices{}, topology{};
    uint32_t index_count{}, instances{}, first_instance{};
    VertexRange vertices;
    bool operator==(const Draw& rhs) const = default;
};

// CPU bookkeeping shared with the regression test. Draw identity includes topology and
// offsets: a reused index buffer or another submesh must never read uninitialised history.
class History {
public:
    explicit History(uint32_t capacity) : capacity{capacity} {}

    struct Allocation {
        uint32_t store{}, load{}, vertices{}, instances{}, first_vertex{}, first_instance{};
    };
    struct Stats {
        uint64_t draws{}, stored{}, loaded{}, unmatched{}, exhausted{}, invalid{}, still{},
            held{}, exhausted_characters{};
    } stats;
    struct Totals {
        uint64_t draws{}, exhausted{}, exhausted_characters{}, exhausted_vertices{};
    } totals;

    // A small skeleton (weapon, prop; most are static world pieces) only needs history
    // while its bone palette changes: with last frame's bones its motion is the camera's.
    // Checked before the index scan of Prepare, with a cheap identity. The first frame
    // of a change stores positions (nothing to load yet), the next one loads them.
    //
    // bbport: a draw whose palette changed keeps its history for `hold` more frames
    // (SetHold, BB_OBJECT_MOTION_HOLD in vk_object_motion.cpp). Without it, bones that move
    // on some frames only never got a vector: a changed frame that follows an unchanged one
    // has nothing to load (that frame stored nothing; it counts as unmatched), and the frame
    // after it is "still" again. The palettes hold world-space bones (the G-buffer vertex
    // shaders multiply by the palette, then by the scene camera; the model constants only
    // give the first bone index), so an attached weapon changes its palette only on the
    // frames the game updates its attachment, which at 120 FPS need not be every rendered
    // frame. Ludwig's Holy Blade on the back showed it: orange (camera motion) in the debug
    // view while the body, a large skeleton stored every frame, was blue, and DLSS dropped
    // the blade's history while the player ran. A held frame stores and loads like a moving
    // one; for an object that did not move its vector equals the camera's, so holding costs
    // only the vertex stores of objects that moved within the last `hold` frames.
    struct GateKey {
        uint64_t shader{}, geometry{}, indices{};
        uint32_t index_count{}, instances{}, first_instance{}, occurrence{};
        bool operator==(const GateKey&) const = default;
    };
    void SetHold(uint32_t frames) { hold = frames; }
    bool Moving(GateKey key, uint64_t palette) {
        key.occurrence = 0;
        key.occurrence = gate_occurrences[key]++;
        const auto* prev = gate_previous.find(key);
        GateState state{palette, 0};
        bool moving = false;
        if (prev && prev->palette != palette) {
            moving = true;
            state.hold = hold;
        } else if (prev && prev->hold) {
            moving = true; // unchanged, but changed within the last `hold` frames
            state.hold = prev->hold - 1;
            ++stats.held;
        }
        if (!moving && prev && !LANTERN_MH_MUTANT(StillKeepsReservation)) {
            Release(prev->object);
        }
        GateState& slot = gate_current[key];
        slot = state;
        gate_slot = moving ? &slot : nullptr;
        stats.still += moving ? 0 : 1;
        return moving;
    }

    void NextFrame() {
        previous.swap(current);
        current.clear();
        occurrences.clear();
        gate_previous.swap(gate_current);
        gate_current.clear();
        gate_occurrences.clear();
        gate_slot = nullptr;
        prev_objects.swap(objects);
        objects.clear();
        object_ids.clear();
        pending = {};
        for (auto& object : prev_objects) {
            object.tier = Tier(object.character, object.admitted);
            object.claimed = false;
            pending[object.tier] += object.demand;
        }
        promised = 0;
        ++frame;
        used = 0;
    }

    Allocation Prepare(const Draw& draw, uint64_t object = 0, bool character = false) {
        GateState* const gate = std::exchange(gate_slot, nullptr);
        ++stats.draws;
        ++totals.draws;
        Key key{draw, 0};
        key.occurrence = occurrences[key]++;
        const uint64_t count = uint64_t(draw.vertices.count) * draw.instances;
        if (!count || uint64_t(draw.vertices.first) + draw.vertices.count - 1 > INT32_MAX ||
            uint64_t(draw.first_instance) + draw.instances > UINT32_MAX) {
            ++stats.invalid;
            return {};
        }
        const Slot* prev = previous.find(key);
        Allocation result{0, 0, draw.vertices.count, draw.instances,
                          draw.vertices.first, draw.first_instance};
        if (prev && prev->store) {
            result.load = prev->store;
            ++stats.loaded;
        } else {
            ++stats.unmatched;
        }
        if (!object || LANTERN_MH_MUTANT(PerDraw)) {
            object = uint64_t(Hash{}(key)) | (uint64_t(1) << 63);
        }
        const ObjectState* last = prev && prev->object < prev_objects.size()
                                      ? &prev_objects[prev->object]
                                      : nullptr;
        if (last) {
            Release(prev->object);
        }
        uint32_t index;
        if (const uint32_t* found = object_ids.find(object)) {
            index = *found;
        } else {
            index = uint32_t(objects.size());
            object_ids.emplace(object, index);
            ObjectState& fresh = objects.emplace_back();
            fresh.character = character && !LANTERN_MH_MUTANT(NoCharacterPriority);
            const bool incumbent = last && last->admitted && !LANTERN_MH_MUTANT(NoIncumbency);
            const uint64_t expected = last ? std::max(last->demand, count) : count;
            const uint32_t tier = Tier(fresh.character, incumbent);
            uint64_t ahead = 0;
            for (uint32_t t = 0; t < tier; ++t) {
                ahead += pending[t];
            }
            fresh.admitted = uint64_t(used) + promised + expected + ahead <= capacity;
            if (fresh.admitted) {
                fresh.remaining = expected;
                promised += expected;
            }
        }
        ObjectState& owner = objects[index];
        owner.demand += count;
        if (owner.admitted) {
            const uint64_t charge = std::min(count, owner.remaining);
            owner.remaining -= charge;
            promised -= charge;
        }
        if (owner.admitted && count <= capacity - used) {
            result.store = 1 + uint32_t(frame & 1) * capacity + used;
            used += uint32_t(count);
            ++stats.stored;
        } else {
            ++stats.exhausted;
            ++totals.exhausted;
            totals.exhausted_vertices += count;
            if (character) {
                ++stats.exhausted_characters;
                ++totals.exhausted_characters;
            }
        }
        current.emplace(key, Slot{result.store, index});
        if (gate) {
            gate->object = index;
        }
        return result;
    }

    uint32_t Used() const { return used; }
    uint32_t Capacity() const { return capacity; }

private:
    static constexpr uint32_t NoObject = UINT32_MAX;
    static uint32_t Tier(bool character, bool incumbent) {
        return (character ? 0u : 2u) + (incumbent ? 0u : 1u);
    }
    void Release(uint32_t index) {
        if (index >= prev_objects.size() || prev_objects[index].claimed) {
            return;
        }
        ObjectState& object = prev_objects[index];
        object.claimed = true;
        pending[object.tier] -= object.demand;
    }
    struct Key {
        Draw draw;
        uint32_t occurrence;
        bool operator==(const Key&) const = default;
    };
    struct Slot {
        uint32_t store{}, object{NoObject};
    };
    struct ObjectState {
        uint64_t demand{}, remaining{};
        uint32_t tier{};
        bool character{}, admitted{}, claimed{};
    };
    struct IdHash {
        size_t operator()(uint64_t id) const { return size_t(id ^ (id >> 32)); }
    };
    struct Hash {
        size_t operator()(const Key& key) const {
            uint64_t h = 0;
            const auto mix = [&](uint64_t v) { h ^= v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2); };
            const auto& d = key.draw;
            mix(d.shader); mix(d.geometry); mix(d.indices); mix(d.topology);
            mix(d.index_count); mix(d.instances); mix(d.first_instance);
            mix(d.vertices.first); mix(d.vertices.count); mix(key.occurrence);
            return size_t(h);
        }
    };
    uint32_t capacity{}, used{};
    uint32_t hold{}; ///< frames a small skeleton keeps its history after a change (SetHold)
    uint64_t frame{};
    FlatMap<Key, Slot, Hash> previous, current;
    FlatMap<Key, uint32_t, Hash> occurrences;
    std::vector<ObjectState> objects, prev_objects;
    FlatMap<uint64_t, uint32_t, IdHash> object_ids;
    std::array<uint64_t, 4> pending{};
    uint64_t promised{};
    struct GateHash {
        size_t operator()(const GateKey& key) const {
            uint64_t h = 0;
            const auto mix = [&](uint64_t v) { h ^= v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2); };
            mix(key.shader); mix(key.geometry); mix(key.indices); mix(key.index_count);
            mix(key.instances); mix(key.first_instance); mix(key.occurrence);
            return size_t(h);
        }
    };
    struct GateState {
        uint64_t palette{};
        uint32_t hold{}; ///< further frames this draw keeps its history (Moving)
        uint32_t object{NoObject};
    };
    FlatMap<GateKey, GateState, GateHash> gate_previous, gate_current;
    FlatMap<GateKey, uint32_t, GateHash> gate_occurrences;
    GateState* gate_slot{};
};

} // namespace Vulkan::Motion
