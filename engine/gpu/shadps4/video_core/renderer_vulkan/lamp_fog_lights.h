// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The lights lamp fog draws in a frame. The game repeats a light in every screen tile it touches,
// so each is kept once. When there are more than the pass takes, the ones that show most stay: the
// order the tiles arrive in moves with the camera, and fog that went by it popped on and off.

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <unordered_set>
#include <utility>
#include <vector>

namespace LampFog {

using Light = std::array<uint32_t, 16>;

// The fields point_volumetric.comp reads: position in view space, inverse radius squared, colour.
inline bool Readable(const Light& light) {
    const auto word = [&](size_t i) { return std::bit_cast<float>(light[i]); };
    for (const size_t i : {0u, 1u, 2u, 3u, 4u, 5u, 6u}) {
        if (!std::isfinite(word(i))) {
            return false;
        }
    }
    return word(3) > 0.0f;
}

// How much of the frame's fog the light is. 0 for a light the shader skips or that is behind the
// camera.
inline float Importance(const Light& light) {
    if (!Readable(light)) {
        return 0.0f;
    }
    const auto word = [&](size_t i) { return std::bit_cast<float>(light[i]); };
    const float x = word(0), y = word(1), z = word(2), inverse_radius_squared = word(3);
    float r = word(4), g = word(5), b = word(6);
    r = std::max(r, 0.0f);
    g = std::max(g, 0.0f);
    b = std::max(b, 0.0f);
    const float over = std::max(1.0f, std::max({r, g, b}) / 4.0f);
    const float luminance = (0.2126f * r + 0.7152f * g + 0.0722f * b) / over;
    const float radius = 1.0f / std::sqrt(inverse_radius_squared);
    if (!(luminance > 0.0f) || z + radius <= 0.0f) {
        return 0.0f;
    }
    // A ray through the light crosses fog in proportion to the radius and the shader fades it with
    // distance; the light covers a share of the screen that goes with (radius / distance)^2.
    const float distance_squared = x * x + y * y + z * z;
    const float radius_squared = radius * radius;
    const float coverage =
        distance_squared > radius_squared ? radius_squared / distance_squared : 1.0f;
    return luminance * radius * std::exp(-0.025f * std::sqrt(distance_squared)) * coverage;
}

struct Selection {
    std::vector<Light> lights;
    size_t visible = 0;
};

// The same lights give the same result in any order, whatever the camera did to the tiles.
inline Selection Select(std::span<const Light> lights, size_t limit) {
    std::vector<std::pair<float, size_t>> ranked;
    ranked.reserve(lights.size());
    for (size_t i = 0; i < lights.size(); ++i) {
        if (const float importance = Importance(lights[i]); importance > 0.0f) {
            ranked.emplace_back(importance, i);
        }
    }
    Selection selection;
    selection.visible = ranked.size();
    if (ranked.size() > limit) {
        std::nth_element(ranked.begin(), ranked.begin() + limit, ranked.end(),
                         [&](const auto& a, const auto& b) {
                             return a.first != b.first ? a.first > b.first
                                                       : lights[a.second] < lights[b.second];
                         });
        ranked.resize(limit);
    }
    selection.lights.reserve(ranked.size());
    for (const auto& [importance, index] : ranked) {
        selection.lights.push_back(lights[index]);
    }
    return selection;
}

class FrameLights {
public:
    // Far more than a frame has: only a bound on what bad guest data can make this hold.
    static constexpr size_t MaxGathered = 4096;

    void Clear() {
        lights.clear();
        seen.clear();
    }

    bool Empty() const {
        return lights.empty();
    }

    void Add(const uint32_t* records, uint32_t count) {
        for (uint32_t i = 0; i < count && lights.size() < MaxGathered; ++i) {
            Light light;
            std::copy_n(records + size_t(i) * light.size(), light.size(), light.begin());
            if (seen.insert({light[0], light[1], light[2], light[3]}).second) {
                lights.push_back(light);
            }
        }
    }

    const std::vector<Light>& Lights() const {
        return lights;
    }

private:
    // Position and radius: what stays the same for one light across the tiles of a frame.
    using Key = std::array<uint32_t, 4>;
    struct KeyHash {
        size_t operator()(const Key& key) const {
            uint64_t hash = 14695981039346656037ull;
            for (const uint32_t word : key) {
                hash = (hash ^ word) * 1099511628211ull;
            }
            return size_t(hash ^ (hash >> 32));
        }
    };

    std::vector<Light> lights;
    std::unordered_set<Key, KeyHash> seen;
};

// The fog with steady fog off: no Memory, no Select, only the first lights to arrive, in the order
// the tiles brought them.
inline constexpr size_t AsListedLimit = 64;

inline std::span<const Light> AsListed(std::span<const Light> seen) {
    return seen.first(std::min(seen.size(), AsListedLimit));
}

// 3x4 rows, as the game's scene constants hold them.
struct Camera {
    std::array<float, 12> world_to_view{}, view_to_world{};
    bool valid = false;
};

// The game lists a light only while it lights a surface in view, so turning the camera can drop a
// light the camera stands inside, and all its fog with it. The fog keeps each light where it was
// last seen for a while after that, and fades lights in and out instead of popping them.
class Memory {
public:
    // The game lists lamps in groups while the player walks: a slow fade keeps a group from looking
    // like a switch being thrown.
    static constexpr float FadeIn = 1.5f, FadeOut = 1.0f;
    // A light that moves (a torch carried by an enemy) is not left glowing where it was.
    static constexpr float FadeInMoving = 0.2f, FadeOutMoving = 0.3f;
    // A standing lamp stays while the camera is this close to its fog, whether the game lists it or
    // not: the game can leave out a lamp the camera stands beside for as long as the view stays put.
    static constexpr float KeepNear = 40.0f;
    // Seconds kept after the game last listed a light, once it is further away than that: long
    // enough that walking back through a place finds its lamps still lit.
    static constexpr float HoldStill = 30.0f, HoldMoving = 0.25f;
    static constexpr float MovedRecently = 2.0f;
    // How far a listed light may be from where it was and still be the same light standing still.
    // Lamps come out of the view-space records wobbling while the camera moves, by more the further
    // off they are, so a frame-to-frame test counted whole lists of lamps as moving.
    static constexpr float SameLight = 0.5f, SameLightPerMetre = 0.05f;
    // A light that has strayed this many times that distance from where it first appeared is carried
    // by something (an enemy's torch), and stays so: it can stop for longer than MovedRecently, and
    // when the enemy dies the game drops it for good, so it is not held like a lamp the game left out.
    static constexpr float CarriedFactor = 4.0f;
    // A flash: a thrown fire bottle makes lights that are listed for a second or less, bright
    // (colour 1.5 and up), and are then gone for good, a few metres from the camera. A bright light
    // the game spawns that it stops listing after so short a time, this close, is not held like a
    // lamp the game left out. A dim one is: the game lists lamp props (colour about 1.2) late and
    // leaves them out for a second or two while the camera is near them, and then lists them again.
    // The map's own lamps are not included either way.
    static constexpr float ShortLived = 2.0f, ShortLivedNear = 15.0f, FlashBrightness = 1.4f;
    // The light is judged in the moments after the game drops it, not later when the camera comes by.
    static constexpr float FlashWindow = 0.1f;
    // Word 7 of a record is 20 or more (20, 50, 1000) on the map's own lights and 1 or less on the
    // lights the game spawns.
    static constexpr float MapLightWord = 5.0f;
    // And how far its reach may differ. Lamps can stand closer than that distance (a blue river
    // light under a warm bridge lamp): taken for one, the game listing either alone switched one
    // fog between the two lights' colours and sizes.
    static constexpr float SameRadius = 0.1f;
    // Over the limit, a light already drawn keeps its place until another counts for this much
    // more: lights close in importance do not trade places back and forth as the camera moves.
    static constexpr float KeepChosen = 1.25f;
    // A warp or a menu's preview camera: what was remembered is not around this camera.
    static constexpr float CameraJump = 20.0f;
    // Far more than a place has: only a bound on what bad guest data can make this hold.
    static constexpr size_t MaxRemembered = 1024;

    void Clear() {
        remembered.clear();
        has_position = false;
    }

    // The lights to draw this frame, in view space, from the lights the game listed, at most
    // `limit` of them. Without a camera there is no telling where anything is: the game's list as
    // it is.
    std::vector<Light> Update(std::span<const Light> seen, const Camera& camera, float seconds,
                              size_t limit = MaxRemembered) {
        if (!camera.valid) {
            Clear();
            return {seen.begin(), seen.end()};
        }
        const std::array<float, 3> position{camera.view_to_world[3], camera.view_to_world[7],
                                            camera.view_to_world[11]};
        if (has_position && DistanceSquared(position, last_position) > CameraJump * CameraJump) {
            remembered.clear();
        }
        last_position = position;
        has_position = true;

        for (auto& known : remembered) {
            known.since_seen += seconds;
        }
        std::vector<bool> matched(remembered.size());
        for (const auto& light : seen) {
            if (!Readable(light)) {
                continue;
            }
            const auto view = Position(light);
            const auto world = Transform(camera.view_to_world, view);
            const float same =
                SameLight + SameLightPerMetre * std::sqrt(DistanceSquared(view, {0, 0, 0}));
            const float radius = Radius(light);
            size_t nearest = remembered.size();
            float nearest_distance = same * same;
            for (size_t i = 0; i < matched.size(); ++i) {
                const float distance = DistanceSquared(world, remembered[i].world);
                const float known_radius = Radius(remembered[i].record);
                if (!matched[i] && distance <= nearest_distance &&
                    std::abs(radius - known_radius) <=
                        SameRadius * std::max(radius, known_radius)) {
                    nearest = i;
                    nearest_distance = distance;
                }
            }
            if (nearest != remembered.size()) {
                matched[nearest] = true;
                auto& known = remembered[nearest];
                // Only while listed: a moving light the game dropped has not come to rest.
                if (DistanceSquared(world, known.settled) > same * same) {
                    known.settled = world;
                    known.since_moved = 0.0f;
                } else {
                    known.since_moved += seconds;
                }
                known.world = world;
                known.record = light;
                known.since_seen = 0.0f;
                known.listed_for += seconds;
                known.flash = false;
                // The estimate of where a light is moves by up to `same`, more the further off it
                // was seen: strayed further than that from where it began, with room to spare.
                known.slack = std::max(known.slack, same);
                if (DistanceSquared(world, known.origin) >
                    CarriedFactor * CarriedFactor * known.slack * known.slack) {
                    known.carried = true;
                }
            } else if (remembered.size() < MaxRemembered) {
                // Long past the hold it would take to count as moving.
                remembered.push_back({.record = light,
                                      .world = world,
                                      .settled = world,
                                      .origin = world,
                                      .slack = same,
                                      .colour = Limited(light),
                                      .since_moved = MovedRecently});
            }
        }

        std::erase_if(remembered, [&](Known& known) {
            if (known.since_seen > 0.0f && known.since_seen <= FlashWindow &&
                known.listed_for < ShortLived &&
                !(std::bit_cast<float>(known.record[7]) >= MapLightWord) &&
                std::max({std::bit_cast<float>(known.record[4]), std::bit_cast<float>(known.record[5]),
                          std::bit_cast<float>(known.record[6])}) >= FlashBrightness &&
                DistanceSquared(known.world, position) <= ShortLivedNear * ShortLivedNear) {
                known.flash = true;
            }
            const bool moving = Moving(known);
            const bool kept = moving ? known.since_seen <= HoldMoving
                                     : known.since_seen <= HoldStill || Near(known, position);
            if (kept) {
                known.weight =
                    std::min(1.0f, known.weight + seconds / (moving ? FadeInMoving : FadeIn));
            } else {
                known.weight -= seconds / (moving ? FadeOutMoving : FadeOut);
            }
            if (known.weight <= 0.0f) {
                return true;
            }
            // The game can switch a light's colour in one frame: on at full after fading it out,
            // or to another colour. The fog goes there over the fade times; per channel, about
            // 95% of the way in the fade's time.
            const auto target = Limited(known.record);
            for (size_t i = 0; i < target.size(); ++i) {
                const float fade = target[i] > known.colour[i]
                                       ? (moving ? FadeInMoving : FadeIn)
                                       : (moving ? FadeOutMoving : FadeOut);
                known.colour[i] += (target[i] - known.colour[i]) *
                                   (1.0f - std::exp(-3.0f * seconds / fade));
                if (std::abs(target[i] - known.colour[i]) <=
                    1e-4f * std::max(1.0f, target[i])) {
                    known.colour[i] = target[i];
                }
            }
            return false;
        });
        return Choose(camera, seconds, limit);
    }

private:
    struct Known {
        Light record{};
        std::array<float, 3> world{};
        // Where it last came to rest: a light moves when it strays from here.
        std::array<float, 3> settled{};
        // Where it first appeared, and how far off the estimate of a position has been.
        std::array<float, 3> origin{};
        float slack = 0.0f;
        bool carried = false;
        // Seconds the game has listed it, and whether it was a flash when the game dropped it: it
        // goes as a moving light does, until it is listed again.
        float listed_for = 0.0f;
        bool flash = false;
        // The colour drawn, limited as the shader limits it: it follows the game's.
        std::array<float, 3> colour{};
        float weight = 0.0f, since_seen = 0.0f, since_moved = 0.0f;
        // How much of it the light limit lets through, and whether it is among the lights taken.
        float chosen = 1.0f;
        bool wanted = true, shown = false;
    };


    static bool Moving(const Known& known) {
        return known.carried || known.flash || known.since_moved < MovedRecently;
    }

    // The lights taken when there are more than `limit`: the ones that count most, in the frame's
    // view. A light that drops out fades out, and keeps its place until it has, so no more than
    // `limit` are ever drawn; the one taken instead fades in once there is room.
    std::vector<Light> Choose(const Camera& camera, float seconds, size_t limit) {
        std::vector<std::pair<float, size_t>> ranked;
        ranked.reserve(remembered.size());
        for (size_t i = 0; i < remembered.size(); ++i) {
            const auto& known = remembered[i];
            if (float importance = Importance(Draw(known, camera, 1.0f)); importance > 0.0f) {
                if (known.wanted && known.chosen > 0.0f) {
                    importance *= KeepChosen;
                }
                ranked.emplace_back(importance, i);
            }
        }
        std::ranges::sort(ranked, [&](const auto& a, const auto& b) {
            return a.first != b.first ? a.first > b.first
                                      : remembered[a.second].record < remembered[b.second].record;
        });
        const size_t taken = std::min(limit, ranked.size());
        // Places go first to the lights drawn last frame, which were no more than `limit`: a
        // light that comes into view cannot push out one already on screen.
        size_t drawn = 0;
        for (size_t r = taken; r < ranked.size(); ++r) {
            auto& known = remembered[ranked[r].second];
            known.wanted = false;
            known.chosen = known.shown ? std::max(0.0f, known.chosen -
                                                            seconds / (Moving(known) ? FadeOutMoving
                                                                                     : FadeOut))
                                       : 0.0f;
            drawn += known.chosen > 0.0f;
        }
        for (const bool was_shown : {true, false}) {
            for (size_t r = 0; r < taken; ++r) {
                auto& known = remembered[ranked[r].second];
                if (known.shown != was_shown) {
                    continue;
                }
                known.wanted = true;
                if (drawn < limit) {
                    known.chosen = std::min(
                        1.0f, known.chosen + seconds / (Moving(known) ? FadeInMoving : FadeIn));
                    ++drawn;
                } else {
                    // No room until a light leaving has gone: it comes in from nothing then.
                    known.chosen = 0.0f;
                }
            }
        }
        for (auto& known : remembered) {
            known.shown = false;
        }
        std::vector<Light> lights;
        lights.reserve(remembered.size());
        for (const auto& [importance, index] : ranked) {
            auto& known = remembered[index];
            if (known.chosen > 0.0f) {
                known.shown = true;
                lights.push_back(Draw(known, camera, known.weight * known.chosen));
            }
        }
        // Lights out of the frame's fog take no place and keep what the limit let through; they
        // are passed on all the same, and Select leaves them out.
        for (auto& known : remembered) {
            if (!known.shown && known.chosen > 0.0f &&
                !(Importance(Draw(known, camera, 1.0f)) > 0.0f)) {
                lights.push_back(Draw(known, camera, known.weight * known.chosen));
            }
        }
        return lights;
    }

    static float Radius(const Light& light) {
        return 1.0f / std::sqrt(std::bit_cast<float>(light[3]));
    }

    // The colour as the shader limits it: past 4 only the hue counts.
    static std::array<float, 3> Limited(const Light& light) {
        std::array<float, 3> rgb;
        for (size_t i = 0; i < rgb.size(); ++i) {
            rgb[i] = std::max(std::bit_cast<float>(light[4 + i]), 0.0f);
        }
        const float over = std::max(1.0f, std::max({rgb[0], rgb[1], rgb[2]}) / 4.0f);
        for (auto& channel : rgb) {
            channel /= over;
        }
        return rgb;
    }

    static bool Near(const Known& known, const std::array<float, 3>& position) {
        const float radius = 1.0f / std::sqrt(std::bit_cast<float>(known.record[3]));
        return std::sqrt(DistanceSquared(known.world, position)) - radius <= KeepNear;
    }

    static std::array<float, 3> Position(const Light& light) {
        return {std::bit_cast<float>(light[0]), std::bit_cast<float>(light[1]),
                std::bit_cast<float>(light[2])};
    }

    static std::array<float, 3> Transform(const std::array<float, 12>& m,
                                          const std::array<float, 3>& p) {
        return {m[0] * p[0] + m[1] * p[1] + m[2] * p[2] + m[3],
                m[4] * p[0] + m[5] * p[1] + m[6] * p[2] + m[7],
                m[8] * p[0] + m[9] * p[1] + m[10] * p[2] + m[11]};
    }

    static float DistanceSquared(const std::array<float, 3>& a, const std::array<float, 3>& b) {
        const float x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];
        return x * x + y * y + z * z;
    }

    // The colour is limited as the shader limits it before it is faded: faded first, the shader's
    // limit would undo the fade of a bright light.
    static Light Draw(const Known& known, const Camera& camera, float strength) {
        Light light = known.record;
        const auto view = Transform(camera.world_to_view, known.world);
        for (size_t i = 0; i < view.size(); ++i) {
            light[i] = std::bit_cast<uint32_t>(view[i]);
        }
        for (size_t i = 0; i < known.colour.size(); ++i) {
            light[4 + i] = std::bit_cast<uint32_t>(known.colour[i] * strength);
        }
        return light;
    }

    std::vector<Known> remembered;
    std::array<float, 3> last_position{};
    bool has_position = false;
};

}
