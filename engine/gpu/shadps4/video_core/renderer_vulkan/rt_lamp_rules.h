// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

#ifdef LANTERN_RT_LAMPS_MUTANTS
namespace Vulkan::RtLamps {
inline int g_mutant = 0;
}
#define LANTERN_RT_LAMPS_PICK(n, wrong, right) (::Vulkan::RtLamps::g_mutant == (n) ? (wrong) : (right))
#else
#define LANTERN_RT_LAMPS_PICK(n, wrong, right) (right)
#endif

namespace Vulkan::RtLamps {

inline constexpr std::uint32_t RecordWords = 16;
using Record = std::array<std::uint32_t, RecordWords>;

struct Family {
    std::uint32_t count = 0;
    std::uint32_t stride = RecordWords;
    bool radius_in_w = false;
    bool unknown = false;
    [[nodiscard]] bool Lamp() const noexcept {
        return count != 0 || unknown;
    }
};

inline Family TileFamily(std::uint64_t hash, bool extended) {
    switch (hash) {
    case 0x370174f7:
    case 0x7cc2c044:
        return {1};
    case 0x186d69d5:
    case 0x600f86b0:
        return {2};
    case 0xd22cbfd1:
        return {3};
    case 0x3105494c:
        return {4};
    case 0xd75fe2ff:
        return {5};
    case 0x07d07cf3:
        return {6};
    case 0xf2531727:
        return {7};
    case 0xc6edb19a:
        return {8};
    case 0xb59b4c4e:
        return {9};
    case 0x505ebdbe:
        return {10};
    case 0x22b357f3:
        return {11};
    case 0x9f9ef267:
        return {12};
    default:
        break;
    }
    if (!LANTERN_RT_LAMPS_PICK(1, true, extended)) {
        return {};
    }
    switch (hash) {
    case 0x89f9ee3e:
        return {3};
    case 0x799bfa32:
        return {LANTERN_RT_LAMPS_PICK(2, 10u, 12u)};
    case 0xbae5454e:
        return {1, LANTERN_RT_LAMPS_PICK(3, RecordWords, 32u), LANTERN_RT_LAMPS_PICK(4, false, true)};
    default:
        return {};
    }
}

inline constexpr std::uint32_t ConeMark = 0x53504f54u;
inline constexpr std::uint32_t ConeAt = 14, ConeFlag = 15;
using Cone = std::array<std::uint32_t, 16>;

struct ConeRecord {
    std::size_t record = 0;
    Cone matrix{};
};

inline void Normalize(const std::uint32_t* in, const Family& family, std::uint32_t* out,
                      std::vector<ConeRecord>* cones = nullptr, std::size_t out_index = 0) {
    for (std::uint32_t i = 0; i < family.count; ++i) {
        const std::uint32_t* src = in + std::size_t(i) * LANTERN_RT_LAMPS_PICK(5, RecordWords, family.stride);
        std::uint32_t* dst = out + std::size_t(i) * RecordWords;
        std::copy_n(src, RecordWords, dst);
        if (family.radius_in_w) {
            const float r = std::bit_cast<float>(src[3]);
            const float inv = std::isfinite(r) && r > 0.0f ? LANTERN_RT_LAMPS_PICK(6, r, 1.0f / (r * r)) : 0.0f;
            dst[3] = std::bit_cast<std::uint32_t>(inv);
            dst[ConeAt] = 0;
            dst[ConeFlag] = 0;
            if (cones && family.stride >= 2 * RecordWords && LANTERN_RT_LAMPS_PICK(24, false, true)) {
                dst[ConeFlag] = ConeMark;
                ConeRecord cone{out_index + std::size_t(i) * RecordWords, {}};
                std::copy_n(src + RecordWords, RecordWords, cone.matrix.begin());
                cones->push_back(cone);
            }
        }
    }
}

inline std::array<float, 4> ConeQ(const Cone& matrix, const std::array<float, 3>& p);

inline bool InCone(const Cone& matrix, const std::array<float, 3>& p) {
    const std::array<float, 4> q = ConeQ(matrix, p);
    if (!(q[2] > 0.0f) && LANTERN_RT_LAMPS_PICK(25, false, true)) {
        return false;
    }
    if (!(q[3] > 0.0f)) {
        return false;
    }
    return q[0] * q[0] + q[1] * q[1] <= LANTERN_RT_LAMPS_PICK(26, 4.0f, 1.0f) * q[3] * q[3];
}

struct WorldLight {
    std::array<float, 3> position{};
    float radius = 0;
    std::array<float, 3> colour{};
    float intensity = 0;
};
static_assert(sizeof(WorldLight) == 32);

inline constexpr std::uint32_t KindPoint = 1, KindSpot = 3;
inline constexpr float MaxRadius = 1000.0f;

inline bool Usable(std::uint32_t kind, std::uint8_t enabled, const std::array<float, 4>& position,
                   const std::array<float, 4>& diffuse, WorldLight& out) {
    if (!enabled || (kind != KindPoint && kind != LANTERN_RT_LAMPS_PICK(22, 2u, KindSpot))) {
        return false;
    }
    for (const float v : position) {
        if (!std::isfinite(v)) {
            return false;
        }
    }
    for (const float v : diffuse) {
        if (!std::isfinite(v)) {
            return false;
        }
    }
    if (!(position[3] > 0.0f) || position[3] > MaxRadius) {
        return false;
    }
    out = {{position[0], position[1], position[2]}, position[3], {diffuse[0], diffuse[1], diffuse[2]}, diffuse[3]};
    return true;
}

inline float Word(const Record& r, std::size_t i) {
    return std::bit_cast<float>(r[i]);
}

inline bool ToView(const std::array<float, 12>& world_to_view, const WorldLight& light, Record& out) {
    if (!std::isfinite(light.radius) || !(light.radius > 0.0f)) {
        return false;
    }
    std::array<float, 3> v{};
    for (int r = 0; r < 3; ++r) {
        float sum = LANTERN_RT_LAMPS_PICK(7, 0.0f, world_to_view[r * 4 + 3]);
        for (int k = 0; k < 3; ++k) {
            sum += world_to_view[LANTERN_RT_LAMPS_PICK(8, k * 4 + r, r * 4 + k)] * light.position[k];
        }
        v[r] = sum;
    }
    const float scale = std::max(light.intensity, 0.0f);
    std::array<float, 3> colour{};
    for (int i = 0; i < 3; ++i) {
        const float c = std::max(light.colour[i], 0.0f);
        colour[i] = c * c * scale;
    }
    const float inv = LANTERN_RT_LAMPS_PICK(9, light.radius, 1.0f / (light.radius * light.radius));
    const std::array<float, 7> values{v[0], v[1], v[2], inv, colour[0], colour[1], colour[2]};
    for (const float value : values) {
        if (!std::isfinite(value)) {
            return false;
        }
    }
    out = {};
    for (std::size_t i = 0; i < values.size(); ++i) {
        out[i] = std::bit_cast<std::uint32_t>(values[i]);
    }
    return true;
}

struct Rect {
    std::int32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    [[nodiscard]] bool Empty() const noexcept {
        return x0 >= x1 || y0 >= y1;
    }
    [[nodiscard]] Rect Clip(const Rect& other) const noexcept {
        return {std::max(x0, other.x0), std::max(y0, other.y0), std::min(x1, other.x1), std::min(y1, other.y1)};
    }
    bool operator==(const Rect&) const = default;
};

inline Rect ScreenRect(const std::array<float, 16>& proj, const Record& light, std::uint32_t width,
                       std::uint32_t height) {
    const Rect full{0, 0, std::int32_t(width), std::int32_t(height)};
    const float inv = Word(light, 3);
    if (!(inv > 0.0f)) {
        return {};
    }
    const float r = 1.0f / std::sqrt(inv);
    float lo_x = std::numeric_limits<float>::max(), lo_y = lo_x, hi_x = -lo_x, hi_y = -lo_x;
    std::uint32_t front = 0;
    for (std::uint32_t c = 0; c < 8; ++c) {
        const std::array<float, 4> p{Word(light, 0) + ((c & 1) ? r : -r), Word(light, 1) + ((c & 2) ? r : -r),
                                     Word(light, 2) + ((c & 4) ? r : -r), 1.0f};
        std::array<float, 4> clip{};
        for (int row = 0; row < 4; ++row) {
            for (int k = 0; k < 4; ++k) {
                clip[row] += proj[row * 4 + k] * p[k];
            }
        }
        if (!(clip[3] > 1e-4f)) {
            continue;
        }
        ++front;
        const float px = (clip[0] / clip[3] * 0.5f + 0.5f) * float(width);
        const float py = (0.5f - clip[1] / clip[3] * 0.5f) * float(height);
        lo_x = std::min(lo_x, px);
        hi_x = std::max(hi_x, px);
        lo_y = std::min(lo_y, py);
        hi_y = std::max(hi_y, py);
    }
    if (front == 0) {
        return {};
    }
    if (front != LANTERN_RT_LAMPS_PICK(10, front, 8u)) {
        return full;
    }
    const auto bound = [](float v, std::int32_t most) {
        return std::int32_t(std::clamp(v, -1.0f, float(most) + 1.0f));
    };
    const Rect rect{bound(std::floor(lo_x), full.x1), bound(std::floor(lo_y), full.y1),
                    bound(std::ceil(hi_x) + 1.0f, full.x1), bound(std::ceil(hi_y) + 1.0f, full.y1)};
    return rect.Clip(full);
}

inline constexpr float SameLightMetres = 0.3f, SameLightPerMetre = 0.02f;

inline float SameLightTolerance(const Record& a) {
    const float far = std::sqrt(Word(a, 0) * Word(a, 0) + Word(a, 1) * Word(a, 1) + Word(a, 2) * Word(a, 2));
    const float tolerance = LANTERN_RT_LAMPS_PICK(12, 0.0f, SameLightMetres + SameLightPerMetre * far);
    return tolerance * tolerance;
}

inline bool SameLight(const Record& a, const Record& b, float tolerance_squared) {
    const float dx = Word(a, 0) - Word(b, 0), dy = Word(a, 1) - Word(b, 1),
                dz = LANTERN_RT_LAMPS_PICK(11, 0.0f, Word(a, 2) - Word(b, 2));
    return dx * dx + dy * dy + dz * dz <= tolerance_squared;
}

inline bool SameLight(const Record& a, const Record& b) {
    return SameLight(a, b, SameLightTolerance(a));
}

inline WorldLight ViewToWorld(const std::array<float, 12>& view_to_world, const Record& light) {
    WorldLight out{};
    for (int r = 0; r < 3; ++r) {
        float sum = LANTERN_RT_LAMPS_PICK(27, 0.0f, view_to_world[r * 4 + 3]);
        for (int k = 0; k < 3; ++k) {
            sum += view_to_world[r * 4 + k] * Word(light, std::size_t(k));
        }
        out.position[r] = sum;
    }
    const float inv = Word(light, 3);
    out.radius = inv > 0.0f ? 1.0f / std::sqrt(inv) : 0.0f;
    out.colour = {1.0f, 1.0f, 1.0f};
    out.intensity = 1.0f;
    return out;
}

inline float Importance(const Record& light) {
    const float inv = Word(light, 3);
    if (!(inv > 0.0f)) {
        return 0.0f;
    }
    const float luminance = 0.299f * std::max(Word(light, 4), 0.0f) + 0.587f * std::max(Word(light, 5), 0.0f) +
                            0.114f * std::max(Word(light, 6), 0.0f);
    const float d2 = Word(light, 0) * Word(light, 0) + Word(light, 1) * Word(light, 1) + Word(light, 2) * Word(light, 2);
    const float r2 = 1.0f / inv;
    const float coverage = d2 > r2 ? r2 / d2 : 1.0f;
    const float value = luminance * coverage;
    return std::isfinite(value) ? value : 0.0f;
}

struct Placed {
    Record record{};
    Rect rect{};
    float tolerance = 0.0f;
    std::array<float, 2> centre{};
    bool centred = false;
    std::array<float, 3> world{};
    bool placed_in_world = false;
};

inline bool ScreenPoint(const std::array<float, 16>& proj, const Record& light, std::uint32_t width,
                        std::uint32_t height, std::array<float, 2>& out) {
    const std::array<float, 4> p{Word(light, 0), Word(light, 1), Word(light, 2), 1.0f};
    std::array<float, 4> clip{};
    for (int row = 0; row < 4; ++row) {
        for (int k = 0; k < 4; ++k) {
            clip[row] += proj[row * 4 + k] * p[k];
        }
    }
    if (!(clip[3] > 1e-4f)) {
        return false;
    }
    out = {(clip[0] / clip[3] * 0.5f + 0.5f) * float(width), (0.5f - clip[1] / clip[3] * 0.5f) * float(height)};
    return std::isfinite(out[0]) && std::isfinite(out[1]);
}

inline std::vector<Placed> FrameLights(std::span<const WorldLight> world, const std::array<float, 12>& world_to_view,
                                       const std::array<float, 16>& proj, std::uint32_t width, std::uint32_t height) {
    std::vector<Placed> out;
    out.reserve(world.size());
    for (const WorldLight& light : world) {
        Placed placed;
        if (!ToView(world_to_view, light, placed.record)) {
            continue;
        }
        placed.rect = ScreenRect(proj, placed.record, width, height);
        placed.tolerance = SameLightTolerance(placed.record);
        placed.centred = ScreenPoint(proj, placed.record, width, height, placed.centre);
        placed.world = light.position;
        placed.placed_in_world = true;
        if (!placed.rect.Empty()) {
            out.push_back(placed);
        }
    }
    return out;
}

inline constexpr std::uint32_t ConeBit = 0x80000000u;

struct BatchCount {
    std::uint32_t outside = 0, matched = 0, elsewhere = 0, added = 0, dropped = 0, cells = 0, cells_cut = 0;
};

struct Drawn {
    Record record{};
    Rect rect{};
};

class DrawnLights {
public:
    static constexpr std::size_t Most = 4096;
    static constexpr float Cell = 2.0f;

    void Add(const WorldLight& light, const Rect& rect) {
        if (now.entries.size() >= Most) {
            ++truncated;
            return;
        }
        now.Add(light.position, rect);
    }

    void Roll() {
        last = LANTERN_RT_LAMPS_PICK(29, List{}, std::move(now));
        now = {};
        truncated = 0;
    }

    [[nodiscard]] std::uint32_t Truncated() const noexcept {
        return truncated;
    }

    [[nodiscard]] std::uint64_t LinearScans() const noexcept {
        return linear_scans;
    }

    void Near(const std::array<float, 3>& position, float tolerance_squared, std::vector<Rect>& rects) const {
        for (const List* list : {&now, &last}) {
            Gather(*list, position, tolerance_squared, rects);
        }
    }

private:
    struct Entry {
        std::array<float, 3> position;
        Rect rect;
    };
    struct List {
        std::vector<Entry> entries;
        std::unordered_map<std::uint64_t, std::vector<std::uint32_t>> buckets;
        void Add(const std::array<float, 3>& position, const Rect& rect) {
            buckets[Key(Cells(position))].push_back(std::uint32_t(entries.size()));
            entries.push_back({position, rect});
        }
    };

    static std::array<int, 3> Cells(const std::array<float, 3>& p) {
        std::array<int, 3> out{};
        for (std::size_t i = 0; i < 3; ++i) {
            const float v = std::clamp(p[i] / Cell, -1.0e6f, 1.0e6f);
            out[i] = std::isfinite(v) ? int(std::floor(v)) : 0;
        }
        return out;
    }
    static std::uint64_t Key(const std::array<int, 3>& c) {
        const auto part = [](int v) { return std::uint64_t(std::uint32_t(v + (1 << 20)) & 0x1fffffu); };
        return (part(c[0]) << 42) | (part(c[1]) << 21) | part(c[2]);
    }

    void Gather(const List& list, const std::array<float, 3>& position, float tolerance_squared,
                std::vector<Rect>& rects) const {
        if (list.entries.empty()) {
            return;
        }
        const auto close = [&](const Entry& e) {
            const float dx = e.position[0] - position[0], dy = e.position[1] - position[1],
                        dz = e.position[2] - position[2];
            return dx * dx + dy * dy + dz * dz <= tolerance_squared;
        };
        const int reach = LANTERN_RT_LAMPS_PICK(32, 0, int(std::ceil(std::sqrt(std::max(tolerance_squared, 0.0f)) / Cell)));
        const std::size_t span = std::size_t(2 * reach + 1);
        if (LANTERN_RT_LAMPS_PICK(37, false, span * span * span > list.buckets.size())) {
            ++linear_scans;
            for (const Entry& e : list.entries) {
                if (close(e)) {
                    rects.push_back(e.rect);
                }
            }
            return;
        }
        const auto at = Cells(position);
        for (int dx = -reach; dx <= reach; ++dx) {
            for (int dy = -reach; dy <= reach; ++dy) {
                for (int dz = -reach; dz <= reach; ++dz) {
                    const auto it = list.buckets.find(Key({at[0] + dx, at[1] + dy, at[2] + dz}));
                    if (it == list.buckets.end()) {
                        continue;
                    }
                    for (const std::uint32_t i : it->second) {
                        if (close(list.entries[i])) {
                            rects.push_back(list.entries[i].rect);
                        }
                    }
                }
            }
        }
    }

    List now, last;
    std::uint32_t truncated = 0;
    mutable std::uint64_t linear_scans = 0;
};

inline constexpr std::size_t MaxCellsPerLight = 16;

struct Lamp {
    Record record{};
    std::vector<Rect> cells;
};

inline bool Overlapped(std::span<const Rect> rects, const Rect& where) {
    return std::ranges::any_of(rects, [&](const Rect& r) { return LANTERN_RT_LAMPS_PICK(30, true, !r.Clip(where).Empty()); });
}

inline std::vector<Lamp> ForBatch(std::span<const Placed> frame, std::span<const Drawn> tile_lights,
                                  const DrawnLights& elsewhere, const std::array<float, 12>& view_to_world,
                                  std::span<const Rect> unknown_cells, std::size_t limit, BatchCount& count) {
    std::vector<Rect> unknown(unknown_cells.begin(), unknown_cells.end());
    if (LANTERN_RT_LAMPS_PICK(35, false, true)) {
        std::ranges::sort(unknown, [](const Rect& a, const Rect& b) {
            return std::tie(a.x0, a.y0, a.x1, a.y1) < std::tie(b.x0, b.y0, b.x1, b.y1);
        });
        unknown.erase(std::unique(unknown.begin(), unknown.end()), unknown.end());
    }
    struct Candidate {
        float importance;
        std::size_t index;
    };
    std::vector<Candidate> ranked;
    std::vector<Lamp> lamps;
    std::vector<Rect> here_rects, other_rects;
    for (const Placed& light : frame) {
        if (!std::ranges::any_of(unknown, [&](const Rect& where) { return !light.rect.Clip(where).Empty(); })) {
            ++count.outside;
            continue;
        }
        const float tolerance = light.tolerance > 0.0f ? light.tolerance : SameLightTolerance(light.record);
        here_rects.clear();
        other_rects.clear();
        for (const Drawn& d : tile_lights) {
            if (SameLight(light.record, d.record, tolerance)) {
                here_rects.push_back(d.rect);
            }
        }
        if (LANTERN_RT_LAMPS_PICK(28, false, true)) {
            const bool stored = LANTERN_RT_LAMPS_PICK(38, false, light.placed_in_world);
            elsewhere.Near(stored ? light.world : ViewToWorld(view_to_world, light.record).position, tolerance,
                           other_rects);
        }
        Lamp lamp{light.record, {}};
        bool here = false, other = false;
        for (const Rect& where : unknown) {
            const Rect clip = light.rect.Clip(where);
            if (clip.Empty()) {
                continue;
            }
            if (Overlapped(here_rects, where)) {
                here = true;
                continue;
            }
            if (Overlapped(other_rects, where)) {
                other = true;
                continue;
            }
            lamp.cells.push_back(clip);
        }
        if (lamp.cells.empty()) {
            ++(here ? count.matched : other ? count.elsewhere : count.outside);
            continue;
        }
        const float importance = Importance(light.record);
        if (!(importance > 0.0f)) {
            ++count.outside;
            continue;
        }
        if (lamp.cells.size() > LANTERN_RT_LAMPS_PICK(34, std::size_t(1) << 30, MaxCellsPerLight)) {
            const bool centred = LANTERN_RT_LAMPS_PICK(36, false, light.centred);
            const auto inside = [&](float v, std::int32_t lo, std::int32_t hi) {
                return LANTERN_RT_LAMPS_PICK(39, v, std::clamp(v, float(lo), float(hi)));
            };
            const float cx = centred ? inside(light.centre[0], light.rect.x0, light.rect.x1)
                                     : 0.5f * float(light.rect.x0 + light.rect.x1);
            const float cy = centred ? inside(light.centre[1], light.rect.y0, light.rect.y1)
                                     : 0.5f * float(light.rect.y0 + light.rect.y1);
            const auto distance = [&](const Rect& r) {
                const float dx = 0.5f * float(r.x0 + r.x1) - cx, dy = 0.5f * float(r.y0 + r.y1) - cy;
                return dx * dx + dy * dy;
            };
            std::ranges::stable_sort(lamp.cells, [&](const Rect& a, const Rect& b) {
                return LANTERN_RT_LAMPS_PICK(33, distance(a) > distance(b), distance(a) < distance(b));
            });
            count.cells_cut += std::uint32_t(lamp.cells.size() - MaxCellsPerLight);
            lamp.cells.resize(MaxCellsPerLight);
        }
        ranked.push_back({importance, lamps.size()});
        lamps.push_back(std::move(lamp));
    }
    std::ranges::sort(ranked, [&](const Candidate& a, const Candidate& b) {
        return a.importance != b.importance
                   ? LANTERN_RT_LAMPS_PICK(13, a.importance < b.importance, a.importance > b.importance)
                   : a.index < b.index;
    });
    const std::size_t take = std::min(ranked.size(), LANTERN_RT_LAMPS_PICK(14, ranked.size(), limit));
    count.dropped += std::uint32_t(ranked.size() - take);
    count.added += std::uint32_t(take);
    std::vector<Lamp> out;
    out.reserve(take);
    for (std::size_t i = 0; i < take; ++i) {
        count.cells += std::uint32_t(lamps[ranked[i].index].cells.size());
        out.push_back(std::move(lamps[ranked[i].index]));
    }
    return out;
}

struct ConeProbe {
    std::array<float, 4> apex{}, ahead{};
    std::array<float, 3> axis{};
    bool valid = false;
};

inline std::array<float, 4> ConeQ(const Cone& matrix, const std::array<float, 3>& p) {
    std::array<float, 4> q{};
    for (int row = 0; row < 4; ++row) {
        q[row] = std::bit_cast<float>(matrix[row * 4 + 3]);
        for (int k = 0; k < 3; ++k) {
            q[row] += std::bit_cast<float>(matrix[row * 4 + k]) * p[k];
        }
    }
    return q;
}

inline ConeProbe ProbeCone(const Cone& matrix, const std::array<float, 3>& position) {
    const auto m = [&](int r, int c) { return std::bit_cast<float>(matrix[r * 4 + c]); };
    std::array<float, 3> axis{m(0, 1) * m(1, 2) - m(0, 2) * m(1, 1), m(0, 2) * m(1, 0) - m(0, 0) * m(1, 2),
                              m(0, 0) * m(1, 1) - m(0, 1) * m(1, 0)};
    const float length = std::sqrt(axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2]);
    ConeProbe probe;
    if (!(length > 0.0f) || !std::isfinite(length)) {
        return probe;
    }
    for (float& v : axis) {
        v /= length;
    }
    const float forward = m(2, 0) * axis[0] + m(2, 1) * axis[1] + m(2, 2) * axis[2];
    if (LANTERN_RT_LAMPS_PICK(31, false, forward < 0.0f)) {
        for (float& v : axis) {
            v = -v;
        }
    }
    probe.axis = axis;
    probe.apex = ConeQ(matrix, position);
    probe.ahead = ConeQ(matrix, {position[0] + axis[0], position[1] + axis[1], position[2] + axis[2]});
    probe.valid = true;
    return probe;
}

inline constexpr std::uint32_t MaxKeep = 3;
inline constexpr float MinWeight = 1e-5f;

struct Pick {
    std::array<float, MaxKeep + 1> top_w{};
    std::array<std::uint32_t, MaxKeep + 1> top{};
    float rest_w = 0.0f;
    std::uint32_t rest = 0;
    float wsum = 0.0f;
    [[nodiscard]] std::uint32_t Rays() const noexcept {
        std::uint32_t rays = rest_w > 0.0f ? 1u : 0u;
        for (const float w : top_w) {
            rays += w > 0.0f ? 1u : 0u;
        }
        return rays;
    }
};

inline float NextUniform(std::uint32_t& seed) {
    seed = seed * 747796405u + 2891336453u;
    const std::uint32_t mixed = ((seed >> ((seed >> 28u) + 4u)) ^ seed) * 277803737u;
    return float((mixed >> 22u) ^ mixed) * (1.0f / 4294967296.0f);
}

inline Pick PickLights(std::span<const float> weights, std::uint32_t keep, std::uint32_t seed) {
    keep = std::min(keep, LANTERN_RT_LAMPS_PICK(15, MaxKeep + 1, MaxKeep));
    Pick pick;
    for (std::uint32_t i = 0; i < weights.size(); ++i) {
        const float w = weights[i];
        if (!(w > MinWeight)) {
            continue;
        }
        pick.wsum += w;
        float cand_w = w;
        std::uint32_t cand = i;
        for (std::uint32_t j = 0; j < keep; ++j) {
            if (LANTERN_RT_LAMPS_PICK(16, cand_w < pick.top_w[j] || pick.top_w[j] == 0.0f, cand_w > pick.top_w[j])) {
                std::swap(cand_w, pick.top_w[j]);
                std::swap(cand, pick.top[j]);
            }
        }
        if (cand_w > 0.0f) {
            pick.rest_w += LANTERN_RT_LAMPS_PICK(17, w, cand_w);
            const float u = NextUniform(seed);
            if (LANTERN_RT_LAMPS_PICK(18, true, u * pick.rest_w < cand_w)) {
                pick.rest = cand;
            }
        }
    }
    return pick;
}

inline float Estimate(const Pick& pick, std::span<const float> visibility) {
    if (!(pick.wsum > 0.0f)) {
        return 1.0f;
    }
    float sum = 0.0f;
    for (std::uint32_t j = 0; j < pick.top_w.size(); ++j) {
        if (pick.top_w[j] > 0.0f) {
            sum += pick.top_w[j] * visibility[pick.top[j]];
        }
    }
    if (pick.rest_w > 0.0f) {
        sum += pick.rest_w * visibility[pick.rest];
    }
    return std::clamp(sum / pick.wsum, 0.0f, 1.0f);
}

inline constexpr std::uint32_t FeedCapacity = 1024;
inline constexpr std::uint64_t FreshMs = 250;

inline bool Fresh(std::uint64_t stamp_ms, std::uint64_t now_ms) {
    return stamp_ms != 0 && now_ms >= stamp_ms &&
           now_ms - stamp_ms <= LANTERN_RT_LAMPS_PICK(19, std::numeric_limits<std::uint64_t>::max(), FreshMs);
}

struct Nothing {
    void operator()() const noexcept {}
};

class Feed {
public:
    template <class Mid = Nothing>
    void Publish(std::span<const WorldLight> lights, std::uint64_t stamp_ms, Mid&& mid = {}) {
        const std::uint64_t s = sequence.load(std::memory_order_relaxed);
        sequence.store(s + 1, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        const auto n = std::uint32_t(
            std::min<std::size_t>(lights.size(), LANTERN_RT_LAMPS_PICK(20, FeedCapacity / 2, FeedCapacity)));
        count.store(n, std::memory_order_relaxed);
        stamp.store(stamp_ms, std::memory_order_relaxed);
        mid();
        for (std::uint32_t i = 0; i < n; ++i) {
            const auto values = std::bit_cast<std::array<std::uint32_t, 8>>(lights[i]);
            for (std::uint32_t k = 0; k < 8; ++k) {
                words[i * 8 + k].store(values[k], std::memory_order_relaxed);
            }
        }
        sequence.store(s + 2, std::memory_order_release);
    }

    template <class Mid = Nothing>
    bool Read(std::vector<WorldLight>& out, std::uint64_t& stamp_ms, Mid&& mid = {}) const {
        const std::uint64_t s = sequence.load(std::memory_order_acquire);
        if (LANTERN_RT_LAMPS_PICK(21, false, (s & 1u) != 0)) {
            return false;
        }
        const std::uint32_t n = std::min(count.load(std::memory_order_relaxed), FeedCapacity);
        mid();
        std::vector<WorldLight> got(n);
        for (std::uint32_t i = 0; i < n; ++i) {
            std::array<std::uint32_t, 8> values{};
            for (std::uint32_t k = 0; k < 8; ++k) {
                values[k] = words[i * 8 + k].load(std::memory_order_relaxed);
            }
            got[i] = std::bit_cast<WorldLight>(values);
        }
        const std::uint64_t when = stamp.load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        if (LANTERN_RT_LAMPS_PICK(23, false, sequence.load(std::memory_order_relaxed) != s)) {
            return false;
        }
        out = std::move(got);
        stamp_ms = when;
        return true;
    }

private:
    std::atomic<std::uint64_t> sequence{0};
    std::atomic<std::uint32_t> count{0};
    std::atomic<std::uint64_t> stamp{0};
    std::array<std::atomic<std::uint32_t>, FeedCapacity * 8> words{};
};

inline Feed& SharedFeed() {
    static Feed feed;
    return feed;
}

inline bool ProxyTargets(bool reduced_depth, bool eligible0, bool eligible1) {
    return reduced_depth && LANTERN_RT_LAMPS_PICK(40, eligible0 || eligible1, eligible0 && eligible1);
}

inline bool LightTargetUsable(bool format_ok, bool proxied, bool storage, bool transfer_src) {
    return format_ok && (LANTERN_RT_LAMPS_PICK(41, true, proxied) || (storage && LANTERN_RT_LAMPS_PICK(42, true, transfer_src)));
}

inline constexpr std::uint64_t PointVtable = 0x53b7670, SpotVtable = 0x53b76d0, LineVtable = 0x53b7610;

inline std::uint32_t KindOfVtable(std::uint64_t offset) {
    return offset == PointVtable                                          ? 1u
           : offset == LANTERN_RT_LAMPS_PICK(43, PointVtable, SpotVtable) ? 2u
           : offset == LineVtable                                         ? 3u
                                                                          : 0u;
}

inline bool GameLightEntry(std::uint64_t vtable_offset, std::uint32_t kind) {
    const std::uint32_t by_vtable = KindOfVtable(vtable_offset);
    return LANTERN_RT_LAMPS_PICK(44, true, by_vtable != 0) && LANTERN_RT_LAMPS_PICK(45, true, by_vtable == kind);
}

inline bool GameLightFeeds(std::uint64_t vtable_offset, std::uint32_t kind) {
    return GameLightEntry(vtable_offset, kind) && LANTERN_RT_LAMPS_PICK(46, kind != 0, kind == 1u);
}

#ifdef LANTERN_RT_LAMPS_MUTANTS
inline constexpr int kMutants = 46;
#endif

}
