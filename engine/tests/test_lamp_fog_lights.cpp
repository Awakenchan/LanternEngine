// SPDX-License-Identifier: GPL-3.0-or-later
#include <algorithm>
#include <array>
#include <bit>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <limits>
#include <vector>
#include "gpu/shadps4/video_core/renderer_vulkan/lamp_fog_lights.h"

using namespace LampFog;

namespace {
Light Make(float x, float y, float z, float radius, float r = 1, float g = 1, float b = 1) {
    Light light{};
    const std::array<float, 7> values{x, y, z, 1 / (radius * radius), r, g, b};
    std::ranges::transform(values, light.begin(), [](float v) { return std::bit_cast<uint32_t>(v); });
    return light;
}

std::vector<uint32_t> Records(const std::vector<Light>& lights) {
    std::vector<uint32_t> words;
    for (const auto& light : lights) {
        words.insert(words.end(), light.begin(), light.end());
    }
    return words;
}

float Z(const Light& light) {
    return std::bit_cast<float>(light[2]);
}

float Word(const Light& light, size_t i) {
    return std::bit_cast<float>(light[i]);
}

// A camera at x along the world's x axis, looking down z.
Camera At(float x) {
    return {.world_to_view = {1, 0, 0, -x, 0, 1, 0, 0, 0, 0, 1, 0},
            .view_to_world = {1, 0, 0, x, 0, 1, 0, 0, 0, 0, 1, 0},
            .valid = true};
}

// The same place, turned around the world's y axis.
Camera Turned(float angle) {
    const float c = std::cos(angle), s = std::sin(angle);
    return {.world_to_view = {c, 0, -s, 0, 0, 1, 0, 0, s, 0, c, 0},
            .view_to_world = {c, 0, s, 0, 0, 1, 0, 0, -s, 0, c, 0},
            .valid = true};
}

constexpr float Frame = 1.0f / 60.0f;
// Frames a new standing lamp takes to reach full strength.
constexpr int FadedIn = int(Memory::FadeIn / Frame) + 2;
}

int main() {
    // Repeated in many tiles: one light, however the colour words differ.
    {
        FrameLights frame;
        const auto light = Make(1, 2, 10, 5);
        auto recoloured = light;
        recoloured[4] = std::bit_cast<uint32_t>(0.5f);
        const auto words = Records({light, recoloured, light, Make(1, 2, 11, 5)});
        frame.Add(words.data(), 4);
        frame.Add(words.data(), 4);
        const auto selection = Select(frame.Lights(), 64);
        assert(selection.lights.size() == 2 && selection.visible == 2);
        frame.Clear();
        assert(frame.Empty() && Select(frame.Lights(), 64).lights.empty());
    }
    // Under the limit: everything the shader can draw is kept.
    {
        std::vector<Light> lights;
        for (int i = 0; i < 64; ++i) {
            lights.push_back(Make(float(i), 0, 20 + float(i), 8));
        }
        FrameLights frame;
        const auto words = Records(lights);
        frame.Add(words.data(), uint32_t(lights.size()));
        assert(Select(frame.Lights(), 256).lights.size() == 64);
    }
    // Over it: the nearest stay and the same set comes out whatever order the tiles arrive in.
    {
        std::vector<Light> lights;
        for (int i = 0; i < 300; ++i) {
            lights.push_back(Make(0, 0, 5 + float(i), 6));
        }
        std::vector<std::vector<Light>> selected;
        for (int order = 0; order < 3; ++order) {
            auto arrival = lights;
            if (order == 1) std::ranges::reverse(arrival);
            if (order == 2) std::ranges::rotate(arrival, arrival.begin() + 137);
            FrameLights frame;
            const auto words = Records(arrival);
            frame.Add(words.data(), uint32_t(arrival.size()));
            auto selection = Select(frame.Lights(), 256);
            assert(selection.lights.size() == 256 && selection.visible == 300);
            std::ranges::sort(selection.lights);
            selected.push_back(selection.lights);
            for (const auto& light : selection.lights) {
                assert(Z(light) < 5 + 256);
            }
        }
        assert(selected[0] == selected[1] && selected[0] == selected[2]);
    }
    // Equal lights at the limit: broken by the record, so still independent of arrival order.
    {
        std::vector<Light> lights;
        for (int i = 0; i < 8; ++i) {
            const float angle = float(i) * 0.7853981f;
            lights.push_back(Make(10 * std::cos(angle), 10 * std::sin(angle), 0, 4));
        }
        FrameLights forward, backward;
        const auto words = Records(lights);
        forward.Add(words.data(), 8);
        auto reversed = lights;
        std::ranges::reverse(reversed);
        const auto reversed_words = Records(reversed);
        backward.Add(reversed_words.data(), 8);
        auto a = Select(forward.Lights(), 3).lights, b = Select(backward.Lights(), 3).lights;
        std::ranges::sort(a);
        std::ranges::sort(b);
        assert(a.size() == 3 && a == b);
    }
    // Brighter, wider and nearer each count for more; a light around the camera still counts.
    {
        const float base = Importance(Make(0, 0, 30, 5));
        assert(Importance(Make(0, 0, 30, 5, 2, 2, 2)) > base);
        assert(Importance(Make(0, 0, 30, 10)) > base);
        assert(Importance(Make(0, 0, 60, 5)) < base);
        assert(Importance(Make(0, 0, 1, 5)) > 0);
        assert(Importance(Make(0, 0, 0, 5)) > 0);
    }
    // The shader's own limit on colour: past 4 only the hue matters.
    {
        const float at_limit = Importance(Make(0, 0, 30, 5, 4, 4, 4));
        assert(std::abs(Importance(Make(0, 0, 30, 5, 400, 400, 400)) - at_limit) < at_limit * 1e-4f);
    }
    // Nothing the shader would skip takes a place.
    {
        const float nan = std::numeric_limits<float>::quiet_NaN();
        const float inf = std::numeric_limits<float>::infinity();
        auto zero_radius = Make(0, 0, 10, 5);
        zero_radius[3] = 0;
        auto negative_radius = Make(0, 0, 11, 5);
        negative_radius[3] = std::bit_cast<uint32_t>(-1.0f);
        const auto words = Records({Make(nan, 0, 10, 5), Make(0, inf, 12, 5), zero_radius,
                                    negative_radius, Make(0, 0, 13, 5, 0, 0, 0),
                                    Make(0, 0, 14, 5, -1, -1, -1), Make(0, 0, 15, 5, nan, 1, 1),
                                    Make(0, 0, -50, 5), Make(0, 0, 16, 5)});
        FrameLights frame;
        frame.Add(words.data(), 9);
        const auto selection = Select(frame.Lights(), 256);
        assert(selection.visible == 1 && selection.lights.size() == 1);
        assert(Z(selection.lights[0]) == 16);
    }
    // A light behind the camera whose sphere still reaches past it is in the frame's fog.
    assert(Importance(Make(0, 0, -2, 5)) > 0);
    // Bad guest data cannot grow this without bound.
    {
        FrameLights frame;
        std::vector<Light> lights;
        for (size_t i = 0; i < FrameLights::MaxGathered + 50; ++i) {
            lights.push_back(Make(float(i) * 0.01f, 0, 20, 5));
        }
        const auto words = Records(lights);
        frame.Add(words.data(), uint32_t(lights.size()));
        assert(Select(frame.Lights(), lights.size()).visible == FrameLights::MaxGathered);
    }
    // A lamp near the camera stays for as long as the camera does, listed or not, moved with the
    // camera.
    {
        Memory memory;
        const std::vector<Light> lamp{Make(0, 0, 20, 7.5f, 2, 1, 1)};
        std::vector<Light> drawn;
        for (int i = 0; i < FadedIn; ++i) drawn = memory.Update(lamp, At(0), Frame);
        assert(drawn.size() == 1 && Word(drawn[0], 4) == 2.0f);
        for (int i = 0; i < int(2 * Memory::HoldStill / Frame); ++i) {
            drawn = memory.Update({}, At(5), Frame);
            assert(drawn.size() == 1 && Word(drawn[0], 4) == 2.0f);
            assert(Word(drawn[0], 0) == -5.0f && Z(drawn[0]) == 20.0f);
            assert(drawn[0][3] == lamp[0][3]);
        }
    }
    // How near counts from the edge of the lamp's fog: a wide lamp stays from further off.
    {
        const float x = Memory::KeepNear + 5;
        for (const float radius : {2.0f, 30.0f}) {
            Memory memory;
            const std::vector<Light> lamp{Make(0, 0, 0, radius)};
            memory.Update(lamp, At(0), Frame);
            std::vector<Light> drawn;
            for (float walked = 0; walked < x; walked += 10) {
                drawn = memory.Update({}, At(std::min(walked + 10, x)), Frame);
            }
            for (int i = 0; i < int((Memory::HoldStill + Memory::FadeOut + 1) / Frame); ++i) {
                drawn = memory.Update({}, At(x), Frame);
            }
            assert(drawn.empty() == (radius == 2.0f));
        }
    }
    // Once the camera has gone from it, a lamp the game drops stays where it was for the hold. Then
    // it fades, and it is gone.
    {
        Memory memory;
        const std::vector<Light> lamp{Make(0, 0, 20, 7.5f, 2, 1, 1)};
        std::vector<Light> drawn;
        for (int i = 0; i < FadedIn; ++i) drawn = memory.Update(lamp, At(0), Frame);
        const float far = Memory::KeepNear + 40;
        float held = 0;
        for (float x = 0; x < far; x += 15) {
            memory.Update({}, At(x), Frame);
            held += Frame;
        }
        while (held < Memory::HoldStill - 0.1f) {
            drawn = memory.Update({}, At(far), Frame);
            held += Frame;
            assert(drawn.size() == 1 && Word(drawn[0], 4) == 2.0f);
            assert(Word(drawn[0], 0) == -far && Z(drawn[0]) == 20.0f);
        }
        float fading = Word(drawn[0], 4);
        int frames = 0;
        while (!(drawn = memory.Update({}, At(far), Frame)).empty()) {
            const float now = Word(drawn[0], 4);
            assert(now <= fading);
            fading = now;
            ++frames;
            assert(float(frames) * Frame < 0.1f + Memory::FadeOut + 0.1f);
        }
        // From the last frame at strength to none, in about the fade's time past the hold.
        assert(float(frames) * Frame > Memory::FadeOut * 0.8f);
    }
    // Back before the hold is over: the same lamp, once, with no dip and no second copy.
    {
        Memory memory;
        const std::vector<Light> lamp{Make(0, 0, 20, 7.5f)};
        for (int i = 0; i < FadedIn; ++i) memory.Update(lamp, At(0), Frame);
        for (int i = 0; i < int(20.0f / Frame); ++i) memory.Update({}, At(0), Frame);
        const auto drawn = memory.Update(lamp, At(0), Frame);
        assert(drawn.size() == 1 && Word(drawn[0], 4) == 1.0f);
    }
    // The camera moving does not make a standing lamp look like a new one.
    {
        Memory memory;
        std::vector<Light> drawn;
        float x = 0;
        for (int i = 0; i < FadedIn; ++i) {
            x = float(i) * 0.2f;
            drawn = memory.Update(std::vector<Light>{Make(10 - x, 0, 20, 5)}, At(x), Frame);
            assert(drawn.size() == 1);
        }
        assert(Word(drawn[0], 4) == 1.0f);
        // And it still counts as standing: the long hold.
        for (int i = 0; i < 60; ++i) drawn = memory.Update({}, At(x), Frame);
        assert(drawn.size() == 1 && Word(drawn[0], 4) == 1.0f);
    }
    // A new light fades in, at the same pace at any frame rate.
    {
        for (const float step : {1.0f / 30.0f, 1.0f / 60.0f, 1.0f / 144.0f}) {
            Memory memory;
            const std::vector<Light> lamp{Make(0, 0, 20, 5)};
            std::vector<Light> drawn;
            float time = 0;
            while (time < Memory::FadeIn * 0.5f) {
                drawn = memory.Update(lamp, At(0), step);
                time += step;
            }
            assert(std::abs(Word(drawn[0], 4) - time / Memory::FadeIn) < 0.01f);
            for (int i = 0; i < int(Memory::FadeIn / step) + 2; ++i) {
                drawn = memory.Update(lamp, At(0), step);
            }
            assert(Word(drawn[0], 4) == 1.0f);
        }
    }
    // A turning camera does not make a standing lamp count as moving, though its position comes
    // out off by what the camera turned: it stays when the game drops it.
    {
        Memory memory;
        float angle = 0, before = 0;
        std::vector<Light> drawn;
        for (int i = 0; i < FadedIn; ++i) {
            const float x = -std::sin(before) * 30, z = std::cos(before) * 30;
            before = angle;
            angle += i % 2 ? 0.04f : 0.01f;
            drawn = memory.Update(std::vector<Light>{Make(x, 0, z, 5)}, Turned(angle), Frame);
            assert(drawn.size() == 1);
        }
        assert(Word(drawn[0], 4) == 1.0f);
        for (int i = 0; i < 60; ++i) drawn = memory.Update({}, Turned(angle), Frame);
        assert(drawn.size() == 1 && Word(drawn[0], 4) == 1.0f);
    }
    // A light that moves comes up quickly and is held only briefly: a carried torch does not stay
    // behind for seconds, whether it walks in front of a still camera or beside a moving one.
    for (const bool camera_follows : {false, true}) {
        Memory memory;
        std::vector<Light> drawn;
        float x = 0;
        for (int i = 0; i < 60; ++i) {
            x = float(i) * 0.1f;
            drawn = memory.Update(std::vector<Light>{camera_follows ? Make(0, 0, 10, 5)
                                                                    : Make(x, 0, 10, 5)},
                                  At(camera_follows ? x : 0), Frame);
        }
        assert(Word(drawn[0], 4) == 1.0f);
        const auto camera = At(camera_follows ? x : 0);
        float time = 0;
        while (!memory.Update({}, camera, Frame).empty()) {
            time += Frame;
            assert(time < Memory::HoldMoving + Memory::FadeOutMoving + 0.1f);
        }
        assert(time >= Memory::HoldMoving);
    }
    // Two lamps side by side stay two, and each keeps its own colour.
    {
        Memory memory;
        const std::vector<Light> lamps{Make(0, 0, 20, 5, 1, 0, 0), Make(0.3f, 0, 20, 5, 0, 1, 0)};
        std::vector<Light> drawn;
        for (int i = 0; i < FadedIn; ++i) drawn = memory.Update(lamps, At(0), Frame);
        assert(drawn.size() == 2);
        if (Word(drawn[0], 0) != 0.0f) std::swap(drawn[0], drawn[1]);
        assert(Word(drawn[0], 4) == 1.0f && Word(drawn[0], 5) == 0.0f);
        assert(Word(drawn[1], 4) == 0.0f && Word(drawn[1], 5) == 1.0f);
    }
    // Two lamps closer than the distance that counts as one, with different reaches (a blue river
    // light under a warm bridge lamp), listed one at a time: still two, each with its own colour.
    {
        Memory memory;
        const auto warm = Make(0, 0, 20, 2, 1, 0.5f, 0.25f);
        const auto blue = Make(0.4f, 0, 20, 0.6f, 0, 0.5f, 1);
        std::vector<Light> drawn;
        for (int i = 0; i < 4 * FadedIn; ++i) {
            drawn = memory.Update(std::vector<Light>{i % 3 ? warm : blue}, At(0), Frame);
            for (const auto& light : drawn) {
                const auto& own = light[3] == warm[3] ? warm : blue;
                assert(light[3] == warm[3] || light[3] == blue[3]);
                // Fading in, but never the other lamp's colour.
                assert(std::abs(Word(light, 4) * Word(own, 6) - Word(light, 6) * Word(own, 4)) <
                       1e-6f);
            }
        }
        assert(drawn.size() == 2);
        for (const auto& light : drawn) {
            const auto& own = light[3] == warm[3] ? warm : blue;
            assert(Word(light, 4) == Word(own, 4) && Word(light, 5) == Word(own, 5) &&
                   Word(light, 6) == Word(own, 6));
        }
    }
    // A lamp whose reach comes out a little different from frame to frame is still one lamp.
    {
        Memory memory;
        std::vector<Light> drawn;
        for (int i = 0; i < FadedIn; ++i) {
            drawn = memory.Update(std::vector<Light>{Make(0, 0, 20, i % 2 ? 5.0f : 5.25f)}, At(0),
                                  Frame);
        }
        assert(drawn.size() == 1 && Word(drawn[0], 4) == 1.0f);
    }
    // The game turning a lamp's colour off and back on at full in one frame: the fog follows over
    // the fade times, at the same pace at any frame rate, and ends on the game's colour.
    {
        std::vector<float> halfway;
        for (const float step : {1.0f / 30.0f, 1.0f / 60.0f, 1.0f / 144.0f}) {
            Memory memory;
            const std::vector<Light> lit{Make(0, 0, 20, 5, 2, 2, 2)};
            const std::vector<Light> dark{Make(0, 0, 20, 5, 0, 0, 0)};
            std::vector<Light> drawn;
            for (int i = 0; i < int(Memory::FadeIn / step) + 2; ++i) {
                drawn = memory.Update(lit, At(0), step);
            }
            assert(Word(drawn[0], 4) == 2.0f);
            float before = 2.0f;
            for (float time = 0; time < Memory::FadeOut; time += step) {
                drawn = memory.Update(dark, At(0), step);
                const float now = drawn.empty() ? 0.0f : Word(drawn[0], 4);
                assert(now <= before && before - now < 2.0f * 4.0f * step / Memory::FadeOut);
                before = now;
            }
            assert(before < 2.0f * 0.1f);
            for (int i = 0; i < int(2 * Memory::FadeOut / step); ++i) {
                memory.Update(dark, At(0), step);
            }
            drawn = memory.Update(lit, At(0), step);
            assert(!drawn.empty() && Word(drawn[0], 4) < 2.0f * 4.0f * step / Memory::FadeIn);
            float time = step;
            while (time < Memory::FadeIn * 0.5f) {
                drawn = memory.Update(lit, At(0), step);
                time += step;
            }
            halfway.push_back(Word(drawn[0], 4));
            for (int i = 0; i < int(4 * Memory::FadeIn / step); ++i) {
                drawn = memory.Update(lit, At(0), step);
            }
            assert(Word(drawn[0], 4) == 2.0f);
        }
        for (const float value : halfway) {
            assert(std::abs(value - halfway[0]) < 0.05f && value > 0.5f && value < 2.0f * 0.95f);
        }
    }
    // Over the limit: never more drawn than it takes. A lamp that comes in waits for the one it
    // pushes out to fade, then fades in; nothing switches off or on in a frame.
    {
        Memory memory;
        constexpr size_t Limit = 4;
        std::vector<Light> lamps;
        for (int i = 0; i < 6; ++i) {
            lamps.push_back(Make(float(i) * 3, 0, 20 + float(i), 5));
        }
        std::vector<Light> drawn;
        for (int i = 0; i < FadedIn; ++i) drawn = memory.Update(lamps, At(0), Frame, Limit);
        assert(drawn.size() == Limit);
        for (const auto& light : drawn) {
            assert(Z(light) < 24 && Word(light, 4) == 1.0f);
        }
        lamps.push_back(Make(0, 0, 6, 5));
        const auto strength = [&](float z) {
            for (const auto& light : drawn) {
                if (Z(light) == z) return Word(light, 4);
            }
            return 0.0f;
        };
        float leaving = strength(23), coming = 0;
        int frames = 0;
        while (coming < 1.0f) {
            drawn = memory.Update(lamps, At(0), Frame, Limit);
            assert(drawn.size() <= Limit);
            const float now_leaving = strength(23), now_coming = strength(6);
            assert(now_leaving <= leaving && leaving - now_leaving <= Frame / Memory::FadeOut + 1e-4f);
            // Its own fade in and the limit's can overlap.
            assert(now_coming >= coming &&
                   now_coming - coming <= 2 * Frame / Memory::FadeIn + 1e-4f);
            leaving = now_leaving;
            coming = now_coming;
            if (coming > 0) assert(leaving == 0);
            ++frames;
            assert(float(frames) * Frame < Memory::FadeOut + 2 * Memory::FadeIn + 0.5f);
        }
        assert(float(frames) * Frame > Memory::FadeOut);
        for (const float z : {20.0f, 21.0f, 22.0f}) assert(strength(z) == 1.0f);
    }
    // Lights close in importance do not trade places as one gets a little ahead; one well ahead
    // does take the place.
    {
        Memory memory;
        const auto a = Make(0, 0, 20, 5), b = Make(5, 0, 20, 5);
        std::vector<Light> drawn;
        for (int i = 0; i < FadedIn; ++i) {
            drawn = memory.Update(std::vector<Light>{a, b}, At(0), Frame, 1);
        }
        assert(drawn.size() == 1 && Word(drawn[0], 0) == 0.0f);
        const float ahead = 1.1f * Importance(a) / Importance(b);
        for (int i = 0; i < 4 * FadedIn; ++i) {
            drawn = memory.Update(std::vector<Light>{a, Make(5, 0, 20, 5, ahead, ahead, ahead)},
                                  At(0), Frame, 1);
            assert(drawn.size() == 1 && Word(drawn[0], 0) == 0.0f);
        }
        const float well_ahead = 2.0f * Importance(a) / Importance(b);
        for (int i = 0; i < 4 * FadedIn; ++i) {
            drawn = memory.Update(
                std::vector<Light>{a, Make(5, 0, 20, 5, well_ahead, well_ahead, well_ahead)},
                At(0), Frame, 1);
            assert(drawn.size() <= 1);
        }
        assert(drawn.size() == 1 && Word(drawn[0], 0) == 5.0f);
    }
    // A fade dims a bright light from the start: the shader's limit on colour does not undo it.
    {
        Memory memory;
        const std::vector<Light> lamp{Make(0, 0, 20, 5, 400, 200, 0)};
        auto drawn = memory.Update(lamp, At(0), Memory::FadeIn * 0.5f);
        assert(std::abs(Word(drawn[0], 4) - 2.0f) < 0.001f);
        assert(std::abs(Word(drawn[0], 5) - 1.0f) < 0.001f);
        for (int i = 0; i < FadedIn; ++i) drawn = memory.Update(lamp, At(0), Frame);
        assert(Word(drawn[0], 4) == 4.0f && Word(drawn[0], 5) == 2.0f);
    }
    // A warp forgets what was around the old place.
    {
        Memory memory;
        const std::vector<Light> lamp{Make(0, 0, 20, 5)};
        for (int i = 0; i < 30; ++i) memory.Update(lamp, At(0), Frame);
        assert(memory.Update({}, At(Memory::CameraJump + 1), Frame).empty());
    }
    // Without a camera: the game's list as it is, and nothing kept from it.
    {
        Memory memory;
        const std::vector<Light> lamp{Make(0, 0, 20, 5)};
        const auto drawn = memory.Update(lamp, Camera{}, Frame);
        assert(drawn == lamp);
        assert(memory.Update({}, At(0), Frame).empty());
    }
    // Records the shader would skip are not remembered.
    {
        Memory memory;
        const float nan = std::numeric_limits<float>::quiet_NaN();
        auto zero_radius = Make(0, 0, 10, 5);
        zero_radius[3] = 0;
        const std::vector<Light> bad{Make(nan, 0, 10, 5), zero_radius, Make(0, 0, 12, 5, nan, 1, 1)};
        assert(memory.Update(bad, At(0), Frame).empty());
    }
    // Bad guest data cannot grow the memory without bound.
    {
        Memory memory;
        std::vector<Light> lights;
        for (size_t i = 0; i < Memory::MaxRemembered + 50; ++i) {
            lights.push_back(Make(float(i) * 2, 0, 20, 5));
        }
        assert(memory.Update(lights, At(0), Frame).size() == Memory::MaxRemembered);
    }
    std::puts("Lamp fog lights: PASS (each light once, kept by importance in any arrival order, "
              "lights the shader skips left out, bounded; remembered through the hold, moved with "
              "the camera, faded in and out at any frame rate, forgotten on a warp; neighbours with "
              "other reaches kept apart, the game's colour followed over the fades, faded at the "
              "light limit)");
}
