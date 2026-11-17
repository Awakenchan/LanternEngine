// SPDX-License-Identifier: GPL-3.0-or-later
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <thread>
#include <vector>

#include "gpu/shadps4/video_core/renderer_vulkan/rt_lamp_rules.h"

namespace {
namespace L = Vulkan::RtLamps;
int g_failed = 0;

void Expect(bool ok, const char* what) {
    if (!ok) {
        ++g_failed;
#ifndef LANTERN_RT_LAMPS_MUTANTS
        std::printf("FAILED: %s\n", what);
#else
        (void)what;
#endif
    }
}

const L::DrawnLights kNone;
const std::array<float, 12> kIdentity{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};

bool Near(float a, float b, float eps = 1e-4f) {
    return std::abs(a - b) <= eps * std::max(1.0f, std::abs(b));
}

L::Record Light(float x, float y, float z, float radius, float r = 1, float g = 1, float b = 1) {
    L::Record out{};
    const std::array<float, 7> v{x, y, z, 1.0f / (radius * radius), r, g, b};
    for (std::size_t i = 0; i < v.size(); ++i) {
        out[i] = std::bit_cast<std::uint32_t>(v[i]);
    }
    return out;
}

std::array<float, 16> Projection(float width, float height) {
    const float f = 1.0f / std::tan(0.5f), aspect = width / height, n = 0.1f, far = 500.0f;
    return {f / aspect, 0, 0, 0, 0, f, 0, 0, 0, 0, far / (far - n), -far * n / (far - n), 0, 0, 1, 0};
}

void TestFamilies() {
    const std::array<std::pair<std::uint64_t, std::uint32_t>, 14> known{{{0x370174f7, 1}, {0x7cc2c044, 1},
        {0x186d69d5, 2}, {0x600f86b0, 2}, {0xd22cbfd1, 3}, {0x3105494c, 4}, {0xd75fe2ff, 5}, {0x07d07cf3, 6},
        {0xf2531727, 7}, {0xc6edb19a, 8}, {0xb59b4c4e, 9}, {0x505ebdbe, 10}, {0x22b357f3, 11}, {0x9f9ef267, 12}}};
    for (const auto& [hash, count] : known) {
        for (const bool extended : {false, true}) {
            const auto f = L::TileFamily(hash, extended);
            Expect(f.count == count && f.stride == 16 && !f.radius_in_w && !f.unknown, "a known tile shader keeps its count");
        }
    }
    for (const std::uint64_t hash : {0xbae5454eull, 0x89f9ee3eull, 0x799bfa32ull}) {
        Expect(!L::TileFamily(hash, false).Lamp(), "the three new shaders stay unknown with the switch off");
    }
    const auto spot = L::TileFamily(0xbae5454e, true);
    Expect(spot.count == 1 && spot.stride == 32 && spot.radius_in_w, "bae5454e: one light, 32 words, radius in word 3");
    const auto f3 = L::TileFamily(0x89f9ee3e, true);
    Expect(f3.count == 3 && f3.stride == 16 && !f3.radius_in_w, "89f9ee3e: three lights of 16 words");
    const auto f12 = L::TileFamily(0x799bfa32, true);
    Expect(f12.count == 12 && f12.stride == 16 && !f12.radius_in_w, "799bfa32: twelve lights of 16 words");
    Expect(!L::TileFamily(0x12345678, true).Lamp(), "an unknown hash is not a lamp family");
    L::Family grid;
    grid.unknown = true;
    Expect(grid.Lamp() && grid.count == 0, "a grid draw of an unknown shader is a lamp draw without records");
}

void TestNormalize() {
    std::vector<std::uint32_t> in(64, 0);
    const std::array<float, 7> spot{1.5f, -2.0f, 7.0f, 4.0f, 0.5f, 0.25f, 0.125f};
    for (std::size_t i = 0; i < spot.size(); ++i) {
        in[i] = std::bit_cast<std::uint32_t>(spot[i]);
    }
    for (std::size_t i = 16; i < 32; ++i) {
        in[i] = std::bit_cast<std::uint32_t>(99.0f);
    }
    std::vector<std::uint32_t> out(16, 0);
    L::Normalize(in.data(), L::TileFamily(0xbae5454e, true), out.data());
    Expect(std::bit_cast<float>(out[0]) == 1.5f && std::bit_cast<float>(out[2]) == 7.0f, "the 32-word record keeps its position");
    Expect(Near(std::bit_cast<float>(out[3]), 1.0f / 16.0f), "the 32-word record's radius becomes 1/r^2");
    Expect(std::bit_cast<float>(out[4]) == 0.5f && std::bit_cast<float>(out[6]) == 0.125f, "the 32-word record keeps its colour");
    std::vector<std::uint32_t> twelve(12 * 16 + 16, 0), flat(12 * 16, 0);
    for (std::uint32_t i = 0; i < 12; ++i) {
        twelve[i * 16] = std::bit_cast<std::uint32_t>(float(i) + 0.5f);
        twelve[i * 16 + 3] = std::bit_cast<std::uint32_t>(0.25f);
    }
    L::Normalize(twelve.data(), L::TileFamily(0x799bfa32, true), flat.data());
    bool same = true;
    for (std::uint32_t i = 0; i < 12; ++i) {
        same = same && std::bit_cast<float>(flat[i * 16]) == float(i) + 0.5f &&
               std::bit_cast<float>(flat[i * 16 + 3]) == 0.25f;
    }
    Expect(same, "799bfa32's twelve lights come out at 16 words each, 1/r^2 untouched");
    std::vector<std::uint32_t> two_spots(64, 0), two_out(32, 0);
    for (std::uint32_t i = 0; i < 2; ++i) {
        two_spots[i * 32] = std::bit_cast<std::uint32_t>(10.0f + float(i));
        two_spots[i * 32 + 3] = std::bit_cast<std::uint32_t>(2.0f);
    }
    L::Family two = L::TileFamily(0xbae5454e, true);
    two.count = 2;
    L::Normalize(two_spots.data(), two, two_out.data());
    Expect(std::bit_cast<float>(two_out[16]) == 11.0f, "the second 32-word record is read 32 words on");
}

void TestUsable() {
    L::WorldLight out;
    Expect(L::Usable(1, 1, {1, 2, 3, 5}, {1, 1, 1, 2}, out) && out.radius == 5 && out.position[2] == 3 &&
               out.intensity == 2,
           "an enabled point light is used");
    Expect(L::Usable(3, 1, {1, 2, 3, 5}, {1, 1, 1, 2}, out), "an enabled spot light is used");
    Expect(!L::Usable(2, 1, {1, 2, 3, 5}, {1, 1, 1, 2}, out), "a directional light is not");
    Expect(!L::Usable(1, 0, {1, 2, 3, 5}, {1, 1, 1, 2}, out), "a disabled light is not");
    Expect(!L::Usable(1, 1, {1, 2, 3, 0}, {1, 1, 1, 2}, out), "a light without a radius is not");
    Expect(!L::Usable(1, 1, {NAN, 2, 3, 5}, {1, 1, 1, 2}, out), "a torn position is not");
    Expect(!L::Usable(1, 1, {1, 2, 3, 5000}, {1, 1, 1, 2}, out), "an absurd radius is not");
}

void TestToView() {
    const std::array<float, 12> at5{1, 0, 0, -5, 0, 1, 0, 0, 0, 0, 1, 0};
    L::Record r;
    Expect(L::ToView(at5, {{5, 0, 10}, 4, {0.5f, 1, 0}, 2}, r), "a light converts");
    Expect(Near(L::Word(r, 0), 0) && Near(L::Word(r, 1), 0) && Near(L::Word(r, 2), 10), "the camera's position is taken off");
    Expect(Near(L::Word(r, 3), 1.0f / 16.0f), "word 3 is 1/r^2");
    Expect(Near(L::Word(r, 4), 0.5f) && Near(L::Word(r, 5), 2.0f) && L::Word(r, 6) == 0.0f,
           "colour is squared to linear and scaled by intensity");
    const float a = 0.7f, c = std::cos(a), s = std::sin(a);
    const std::array<float, 12> turned{c, 0, -s, 1, 0, 1, 0, -2, s, 0, c, 3};
    const std::array<float, 3> p{2, 5, -7};
    Expect(L::ToView(turned, {{p[0], p[1], p[2]}, 1, {1, 1, 1}, 1}, r), "a turned camera converts");
    Expect(Near(L::Word(r, 0), c * p[0] - s * p[2] + 1) && Near(L::Word(r, 1), p[1] - 2) &&
               Near(L::Word(r, 2), s * p[0] + c * p[2] + 3),
           "the rows of the world-to-view matrix apply as rows");
    Expect(!L::ToView(at5, {{5, 0, 10}, 0, {1, 1, 1}, 1}, r), "no radius, no light");
    Expect(!L::ToView({NAN, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0}, {{5, 0, 10}, 1, {1, 1, 1}, 1}, r),
           "a torn camera gives no light");
}

void TestScreenRect() {
    const auto proj = Projection(1920, 1080);
    const auto centre = L::ScreenRect(proj, Light(0, 0, 20, 1), 1920, 1080);
    Expect(!centre.Empty() && centre.x0 > 800 && centre.x1 < 1120 && centre.y0 > 400 && centre.y1 < 680 &&
               centre.x0 < 960 && centre.x1 > 960,
           "a small light ahead covers a small rect around the centre");
    Expect(L::ScreenRect(proj, Light(0, 0, -20, 1), 1920, 1080).Empty(), "a light behind the camera covers nothing");
    Expect(L::ScreenRect(proj, Light(0, 0, 1, 3), 1920, 1080) == L::Rect{0, 0, 1920, 1080},
           "a light around the camera covers the screen");
    Expect(L::ScreenRect(proj, Light(400, 0, 20, 1), 1920, 1080).Empty(), "a light far to the side covers nothing");
    const auto edge = L::ScreenRect(proj, Light(18, 0, 20, 2), 1920, 1080);
    Expect(!edge.Empty() && edge.x1 <= 1920 && edge.x0 > 960, "a light at the edge is clipped to the screen");
}

void TestSameLight() {
    Expect(L::SameLight(Light(1, 2, 10, 3), Light(1.1f, 2.05f, 10.1f, 3)), "a wobble of a few cm is the same light");
    Expect(L::SameLight(Light(0, 0, 40, 3), Light(0.4f, 0.4f, 40.3f, 9)), "far off, the tolerance grows; the radius does not matter");
    Expect(!L::SameLight(Light(1, 2, 10, 3), Light(2.2f, 2, 10, 3)), "a metre apart is two lights");
    Expect(!L::SameLight(Light(1, 2, 10, 3), Light(1, 2, 11.2f, 3)), "a metre apart in depth is two lights");
}

void TestForBatch() {
    const auto proj = Projection(1920, 1080);
    const std::array<float, 12> identity{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    std::vector<L::WorldLight> world;
    world.push_back({{0, 0, 10}, 4, {1, 1, 1}, 1});
    world.push_back({{-60, 0, 10}, 1, {1, 1, 1}, 1});
    world.push_back({{0, 0, -30}, 2, {1, 1, 1}, 1});
    for (int i = 0; i < 40; ++i) {
        world.push_back({{float(i % 8) - 4.0f, float(i / 8) - 2.0f, 12.0f + float(i)}, 3, {1, 1, 1}, 1.0f + float(i % 5)});
    }
    const auto frame = L::FrameLights(world, identity, proj, 1920, 1080);
    Expect(frame.size() == world.size() - 2, "lights behind or off the screen are left out of the frame");
    const std::vector<L::Drawn> tiles{{Light(0.05f, 0.02f, 10.05f, 4), {0, 0, 1920, 1080}}};
    L::BatchCount count;
    const L::Rect covered{0, 0, 1920, 1080};
    const std::vector<L::Rect> whole{covered};
    const auto added = L::ForBatch(frame, tiles, kNone, kIdentity, whole, 16, count);
    Expect(count.matched == 1, "the light the tile already has is not added again");
    Expect(added.size() == 16 && count.added == 16 && count.dropped == world.size() - 3 - 16,
           "a batch takes at most the limit");
    bool sorted = true;
    for (std::size_t i = 1; i < added.size(); ++i) {
        sorted = sorted && L::Importance(added[i - 1].record) >= L::Importance(added[i].record);
    }
    Expect(sorted, "the strongest lights are kept first");
    float best_left_out = 0;
    for (const auto& light : frame) {
        const bool in = std::ranges::any_of(added, [&](const L::Lamp& a) { return a.record == light.record; });
        if (!in && !L::SameLight(light.record, tiles[0].record)) {
            best_left_out = std::max(best_left_out, L::Importance(light.record));
        }
    }
    Expect(!added.empty() && best_left_out <= L::Importance(added.back().record), "no light left out outshines one kept");
    L::BatchCount small;
    const std::vector<L::Rect> corner_rect{L::Rect{0, 0, 40, 40}};
    const auto corner = L::ForBatch(frame, {}, kNone, kIdentity, corner_rect, 16, small);
    Expect(corner.empty() && small.outside == frame.size(), "a batch in a corner with no lamp there takes nothing");
    for (const auto& a : added) {
        Expect(a.cells.size() == 1 && !a.cells[0].Empty() && a.cells[0].x0 >= covered.x0 && a.cells[0].x1 <= covered.x1,
               "an added light stays in the batch");
    }
    L::BatchCount none;
    Expect(L::ForBatch(frame, {}, kNone, kIdentity, {}, 16, none).empty(),
           "a batch without unrecognised lamp draws takes no game lights");
}

void TestCrossBatch() {
    const auto proj = Projection(1920, 1080);
    const float a = 0.4f, c = std::cos(a), s = std::sin(a);
    const std::array<float, 12> to_view{c, 0, -s, 2, 0, 1, 0, -1, s, 0, c, 4};
    const std::array<float, 12> to_world{c, 0, s, -(c * 2 + s * 4), 0, 1, 0, 1, -s, 0, c, -(-s * 2 + c * 4)};
    const L::Record seen = Light(1.0f, 0.5f, 12.0f, 3);
    const L::WorldLight back = L::ViewToWorld(to_world, seen);
    L::Record again;
    Expect(L::ToView(to_view, back, again) && Near(L::Word(again, 0), 1.0f, 1e-3f) &&
               Near(L::Word(again, 2), 12.0f, 1e-3f) && Near(L::Word(again, 3), 1.0f / 9.0f, 1e-3f),
           "a tile light goes to the world and back to the same view position");
    std::vector<L::WorldLight> world{back, {{40, 0, 40}, 3, {1, 1, 1}, 1}};
    for (auto& w : world) {
        w.colour = {1, 1, 1};
    }
    world[1] = L::ViewToWorld(to_world, Light(-2.0f, 0.0f, 15.0f, 3));
    const auto frame = L::FrameLights(world, to_view, proj, 1920, 1080);
    Expect(frame.size() == 2, "both game lights are in view");
    L::DrawnLights drawn;
    drawn.Add(back, {0, 0, 1920, 1080});
    const std::vector<L::Rect> whole{L::Rect{0, 0, 1920, 1080}};
    L::BatchCount count;
    auto added = L::ForBatch(frame, {}, drawn, to_world, whole, 16, count);
    Expect(added.size() == 1 && count.elsewhere == 1, "a light another batch drew this frame is not added here");
    drawn.Roll();
    L::BatchCount next;
    added = L::ForBatch(frame, {}, drawn, to_world, whole, 16, next);
    Expect(added.size() == 1 && next.elsewhere == 1, "a light a tile drew last frame is still left out");
    drawn.Roll();
    L::BatchCount later;
    added = L::ForBatch(frame, {}, drawn, to_world, whole, 16, later);
    Expect(added.size() == 2 && later.elsewhere == 0, "two frames later it can be added again");
    L::DrawnLights again_drawn;
    again_drawn.Add(back, {0, 0, 1920, 1080});
    L::BatchCount other_camera;
    added = L::ForBatch(frame, {}, again_drawn, kIdentity, whole, 16, other_camera);
    Expect(added.size() == 1 && other_camera.elsewhere == 1,
           "the drawn list is searched at the game light's own world position, whatever camera the batch has");
}

void TestCone() {
    const float t = std::tan(0.5236f);
    std::vector<std::uint32_t> in(32, 0), out(16, 0);
    const std::array<float, 16> m{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, t, 0};
    for (std::size_t i = 0; i < 16; ++i) {
        in[16 + i] = std::bit_cast<std::uint32_t>(m[i]);
    }
    in[2] = std::bit_cast<std::uint32_t>(5.0f);
    in[3] = std::bit_cast<std::uint32_t>(10.0f);
    in[15] = std::bit_cast<std::uint32_t>(1.0f);
    std::vector<L::ConeRecord> cones;
    L::Normalize(in.data(), L::TileFamily(0xbae5454e, true), out.data(), &cones, 48);
    Expect(cones.size() == 1 && cones[0].record == 48 && out[L::ConeFlag] == L::ConeMark && out[L::ConeAt] == 0,
           "a spot record is marked and its matrix kept");
    Expect(cones.size() == 1 && std::bit_cast<float>(cones[0].matrix[14]) == t, "the kept matrix is words 16-31");
    std::vector<L::ConeRecord> none;
    std::vector<std::uint32_t> plain(16, 0), plain_out(16, 0);
    L::Normalize(plain.data(), L::TileFamily(0x370174f7, false), plain_out.data(), &none, 0);
    Expect(none.empty() && plain_out[L::ConeFlag] == 0, "a point record has no cone");
    L::Cone cone{};
    for (std::size_t i = 0; i < 16; ++i) {
        cone[i] = std::bit_cast<std::uint32_t>(m[i]);
    }
    Expect(L::InCone(cone, {0, 0, 5}) && L::InCone(cone, {2, 0, 5}), "a point inside the cone is lit");
    Expect(!L::InCone(cone, {4, 0, 5}) && !L::InCone(cone, {0, 3.5f, 5}), "a point outside the cone is not");
    const std::array<float, 16> tube{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    L::Cone behind{};
    for (std::size_t i = 0; i < 16; ++i) {
        behind[i] = std::bit_cast<std::uint32_t>(tube[i]);
    }
    Expect(L::InCone(behind, {0.5f, 0, 3}) && !L::InCone(behind, {0.5f, 0, -3}), "behind the spot is not lit");
}

void TestTorchCells() {
    const auto proj = Projection(1920, 1080);
    const std::array<float, 12> identity{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    const std::vector<L::WorldLight> torch{{{0, 0, 6}, 8, {1, 0.7f, 0.4f}, 2}};
    const auto frame = L::FrameLights(torch, identity, proj, 1920, 1080);
    Expect(frame.size() == 1 && frame[0].rect == L::Rect{0, 0, 1920, 1080}, "a big near light covers the screen");
    L::Record seen{};
    L::ToView(identity, torch[0], seen);
    const std::vector<L::Drawn> left{{seen, {0, 0, 640, 1080}}};
    const std::vector<L::Rect> right{L::Rect{1280, 0, 1920, 1080}};
    L::BatchCount count;
    auto added = L::ForBatch(frame, left, kNone, kIdentity, right, 16, count);
    Expect(added.size() == 1 && added[0].cells.size() == 1 && added[0].cells[0] == right[0] && count.matched == 0,
           "a light a known tile drew in another cell still shadows the unrecognised cell");
    L::BatchCount other;
    L::DrawnLights left_drawn;
    left_drawn.Add(torch[0], left[0].rect);
    added = L::ForBatch(frame, {}, left_drawn, kIdentity, right, 16, other);
    Expect(added.size() == 1 && other.elsewhere == 0, "the same holds for a cell drawn in another batch");
    const std::vector<L::Drawn> over{{seen, {1200, 0, 1400, 1080}}};
    L::BatchCount same;
    added = L::ForBatch(frame, over, kNone, kIdentity, right, 16, same);
    Expect(added.empty() && same.matched == 1, "a light a known tile drew over the same cell is not added");
    const std::vector<L::Rect> two{L::Rect{0, 0, 300, 300}, L::Rect{1400, 0, 1920, 300}};
    L::BatchCount split;
    added = L::ForBatch(frame, over, kNone, kIdentity, two, 16, split);
    Expect(added.size() == 1 && added[0].cells.size() == 2 && split.added == 1 && split.cells == 2,
           "one light can cover several unrecognised cells, one record for all of them");
}

void TestCells() {
    const auto proj = Projection(1920, 1080);
    const std::array<float, 12> identity{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    const std::vector<L::WorldLight> torch{{{0, 0, 6}, 8, {1, 0.7f, 0.4f}, 2}};
    const auto frame = L::FrameLights(torch, identity, proj, 1920, 1080);
    std::vector<L::Rect> grid;
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 5; ++x) {
            grid.push_back({x * 384, y * 270, x * 384 + 384, y * 270 + 270});
        }
    }
    L::BatchCount count;
    const auto added = L::ForBatch(frame, {}, kNone, kIdentity, grid, 16, count);
    Expect(added.size() == 1 && added[0].cells.size() == L::MaxCellsPerLight && count.cells_cut == 4 &&
               count.cells == L::MaxCellsPerLight,
           "a light keeps at most 16 cells and the cut ones are counted");
    const auto has = [&](const L::Rect& r) { return std::ranges::find(added[0].cells, r) != added[0].cells.end(); };
    Expect(!added.empty() && has(L::Rect{768, 270, 1152, 540}) && has(L::Rect{768, 540, 1152, 810}),
           "the cells nearest the light's centre are kept");
    Expect(!added.empty() && !has(L::Rect{0, 0, 384, 270}) && !has(L::Rect{1536, 810, 1920, 1080}),
           "the corner cells are the ones cut");
    const std::vector<L::Rect> twice{L::Rect{100, 100, 400, 400}, L::Rect{100, 100, 400, 400}};
    L::BatchCount once;
    const auto single = L::ForBatch(frame, {}, kNone, kIdentity, twice, 16, once);
    Expect(single.size() == 1 && single[0].cells.size() == 1, "two unrecognised draws over one cell give the light once");
    L::DrawnLights across;
    across.Add({{1.95f, 0, 6}, 8, {1, 1, 1}, 1}, {0, 0, 640, 1080});
    for (int i = 0; i < 40; ++i) {
        across.Add({{100.0f + 4.0f * float(i), 0, 6}, 2, {1, 1, 1}, 1}, {0, 0, 10, 10});
    }
    L::Record right{};
    L::ToView(identity, {{2.05f, 0, 6}, 8, {1, 1, 1}, 1}, right);
    std::vector<L::Rect> rects;
    across.Near({2.05f, 0, 6}, L::SameLightTolerance(right), rects);
    Expect(rects.size() == 1 && rects[0] == L::Rect{0, 0, 640, 1080} && across.LinearScans() == 0,
           "the buckets find a light across a bucket boundary");
    rects.clear();
    across.Near({2.05f, 0, 6}, 400.0f, rects);
    Expect(rects.size() == 1 && across.LinearScans() == 1, "a wide search scans the list instead of the buckets");
    std::vector<L::WorldLight> offset{{{3, -2, 3}, 10, {1, 0.7f, 0.4f}, 2}};
    const auto low = L::FrameLights(offset, identity, proj, 1920, 1080);
    Expect(low.size() == 1 && low[0].rect == L::Rect{0, 0, 1920, 1080} && low[0].centred && low[0].centre[0] > 1536 &&
               low[0].centre[1] > 810,
           "a near light low and right covers the screen but projects to the lower right");
    L::BatchCount corner;
    const auto kept = L::ForBatch(low, {}, kNone, kIdentity, grid, 16, corner);
    const auto keeps = [&](const L::Rect& r) {
        return !kept.empty() && std::ranges::find(kept[0].cells, r) != kept[0].cells.end();
    };
    Expect(keeps(L::Rect{1536, 810, 1920, 1080}) && !keeps(L::Rect{0, 0, 384, 270}),
           "the cells kept are the ones around where the light is, not the screen centre");
    std::vector<L::WorldLight> far_off{{{4.09f, -4.51f, 1.0f}, 10, {1, 0.7f, 0.4f}, 2}};
    const auto off = L::FrameLights(far_off, identity, proj, 1920, 1080);
    Expect(off.size() == 1 && off[0].centred && off[0].centre[0] > 4000 && off[0].centre[1] > 4000,
           "a near light can project far off the screen");
    L::BatchCount clamped;
    const auto ranked = L::ForBatch(off, {}, kNone, kIdentity, grid, 16, clamped);
    const auto holds = [&](const L::Rect& r) {
        return !ranked.empty() && std::ranges::find(ranked[0].cells, r) != ranked[0].cells.end();
    };
    Expect(holds(L::Rect{384, 0, 768, 270}) && !holds(L::Rect{0, 810, 384, 1080}),
           "a point off the screen is brought into the light's rect before the cells are ranked");
}

void TestPlainRecords() {
    std::vector<std::uint32_t> in(16, 0), out(16, 0);
    in[0] = std::bit_cast<std::uint32_t>(1.0f);
    in[3] = std::bit_cast<std::uint32_t>(0.5f);
    in[L::ConeAt] = 99;
    in[L::ConeFlag] = L::ConeMark;
    std::vector<L::ConeRecord> cones;
    L::Normalize(in.data(), L::TileFamily(0x370174f7, true), out.data(), &cones, 0);
    Expect(out == in && cones.empty(), "a plain record keeps words 14 and 15 and gets no cone");
    std::vector<std::uint32_t> twelve(12 * 16, 7), flat(12 * 16, 0);
    L::Normalize(twelve.data(), L::TileFamily(0x799bfa32, true), flat.data(), &cones, 0);
    Expect(flat == twelve && cones.empty(), "799bfa32 records are copied word for word");
}

void TestConeProbe() {
    const float t = std::tan(0.5236f);
    const float a = 0.6f, c = std::cos(a), s = std::sin(a);
    const std::array<float, 3> at{2, 1, 8};
    const std::array<float, 3> dir{s, 0, c};
    const std::array<float, 3> side{c, 0, -s};
    const std::array<float, 16> m{-side[0], -side[1], -side[2], side[0] * at[0] + side[2] * at[2],
                                  0, 1, 0, -at[1],
                                  dir[0], dir[1], dir[2], -(dir[0] * at[0] + dir[2] * at[2]),
                                  t * dir[0], t * dir[1], t * dir[2], -t * (dir[0] * at[0] + dir[2] * at[2])};
    L::Cone cone{};
    for (std::size_t i = 0; i < 16; ++i) {
        cone[i] = std::bit_cast<std::uint32_t>(m[i]);
    }
    const auto probe = L::ProbeCone(cone, at);
    Expect(probe.valid && std::abs(probe.apex[3]) < 1e-4f, "q.w is about 0 at the spot's own position");
    Expect(Near(probe.axis[0], dir[0], 1e-3f) && Near(probe.axis[2], dir[2], 1e-3f), "the axis points where the spot shines");
    Expect(probe.ahead[2] > 0.0f && probe.ahead[3] > 0.0f &&
               probe.ahead[0] * probe.ahead[0] + probe.ahead[1] * probe.ahead[1] <= probe.ahead[3] * probe.ahead[3],
           "a metre along the axis is inside the cone");
    Expect(L::InCone(cone, {at[0] + dir[0] * 3, at[1], at[2] + dir[2] * 3}) &&
               !L::InCone(cone, {at[0] - dir[0] * 3, at[1], at[2] - dir[2] * 3}),
           "ahead of the spot is lit, behind it is not");
}

void TestPickBound() {
    std::mt19937 rng(11);
    std::uniform_real_distribution<float> weight(0.0f, 1.0f);
    for (const std::uint32_t n : {10u, 100u, 300u}) {
        for (int round = 0; round < 40; ++round) {
            std::vector<float> w(n);
            for (float& v : w) {
                v = weight(rng) < 0.2f ? 0.0f : weight(rng);
            }
            for (const std::uint32_t keep : {1u, 2u, 3u, 9u}) {
                const auto pick = L::PickLights(w, keep, std::uint32_t(round * 977 + n));
                const std::uint32_t bound = std::min(keep, L::MaxKeep) + 1;
                Expect(pick.Rays() <= bound, "a pixel traces at most the kept lights plus one");
                std::vector<float> sorted = w;
                std::ranges::sort(sorted, std::greater<>());
                bool top = true;
                for (std::uint32_t j = 0; j < std::min(keep, L::MaxKeep); ++j) {
                    top = top && pick.top_w[j] == sorted[j] && w[pick.top[j]] == sorted[j];
                }
                Expect(top, "the kept lights are the strongest, strongest first");
                float total = pick.rest_w;
                for (const float t : pick.top_w) {
                    total += t;
                }
                Expect(Near(total, pick.wsum, 1e-3f), "kept and sampled weights add up to the whole");
            }
        }
    }
    const std::vector<float> few{8.0f, 1.0f, 2.0f, 3.0f};
    std::array<int, 4> chosen{};
    const int draws = 30000;
    for (int seed = 0; seed < draws; ++seed) {
        ++chosen[L::PickLights(few, 1, std::uint32_t(seed) * 2654435761u + 1u).rest];
    }
    Expect(chosen[0] == 0, "the strongest light is traced, never sampled");
    for (int i = 1; i < 4; ++i) {
        Expect(std::abs(double(chosen[i]) / draws - few[i] / 6.0) < 0.02, "the rest is sampled by weight");
    }
    const std::vector<float> vis{1.0f, 0.0f, 1.0f, 0.0f};
    const std::vector<float> rising{1.0f, 2.0f, 3.0f, 8.0f};
    const std::vector<float> rising_vis{1.0f, 0.0f, 1.0f, 0.0f};
    double mean = 0;
    for (int seed = 0; seed < draws; ++seed) {
        mean += L::Estimate(L::PickLights(rising, 1, std::uint32_t(seed) * 2654435761u + 1u), rising_vis);
    }
    Expect(std::abs(mean / draws - 4.0 / 14.0) < 0.01, "the estimate is right on average");
    Expect(L::Estimate(L::PickLights(few, 3, 5), vis) == L::Estimate(L::PickLights(few, 3, 9), vis),
           "with room for every light but one the estimate does not depend on the noise");
    Expect(L::PickLights(std::vector<float>{0.0f, 1e-6f}, 2, 1).Rays() == 0, "no light, no ray");
}

void TestFeed() {
    L::Feed feed;
    std::vector<L::WorldLight> out;
    std::uint64_t stamp = 0;
    Expect(feed.Read(out, stamp) && out.empty() && stamp == 0, "an empty feed reads as no lights");
    const std::vector<L::WorldLight> three{{{1, 2, 3}, 4, {1, 0, 0}, 1}, {{5, 6, 7}, 8, {0, 1, 0}, 1}, {{9, 9, 9}, 1, {0, 0, 1}, 1}};
    feed.Publish(three, 1000);
    Expect(feed.Read(out, stamp) && out.size() == 3 && out[1].position[2] == 7 && out[2].radius == 1 && stamp == 1000,
           "a published list reads back");
    std::vector<L::WorldLight> many(2000, three[0]);
    feed.Publish(many, 2000);
    Expect(feed.Read(out, stamp) && out.size() == L::FeedCapacity, "a long list is cut to the feed's room");
    feed.Publish(three, 3000);
    std::vector<L::WorldLight> kept = three;
    std::uint64_t kept_stamp = 7;
    const bool torn = feed.Read(kept, kept_stamp, [&] { feed.Publish(many, 4000); });
    Expect(!torn && kept.size() == 3 && kept_stamp == 7, "a size change during a read is refused and the old list stays");
    bool during = true;
    feed.Publish(three, 5000, [&] {
        std::vector<L::WorldLight> partial;
        std::uint64_t when = 0;
        during = feed.Read(partial, when);
    });
    Expect(!during, "a read while a list is being written is refused");
    Expect(feed.Read(out, stamp) && out.size() == 3 && stamp == 5000, "the next read after a write gets the new list");
    Expect(L::Fresh(1000, 1200) && !L::Fresh(1000, 1300) && !L::Fresh(0, 10) && !L::Fresh(2000, 1000),
           "a list older than a quarter second is stale");
    L::Feed shared;
    std::atomic<bool> done{false};
    std::atomic<int> bad{0}, good{0};
    std::thread writer([&] {
        std::vector<L::WorldLight> list;
        for (int k = 1; k <= 4000; ++k) {
            list.assign(std::size_t(k % 50) + 1, L::WorldLight{{float(k), float(k), float(k)}, float(k), {}, 0});
            shared.Publish(list, std::uint64_t(k));
        }
        done = true;
    });
    std::vector<L::WorldLight> got;
    while (!done) {
        std::uint64_t when = 0;
        if (!shared.Read(got, when) || when == 0) {
            continue;
        }
        ++good;
        const bool whole = got.size() == std::size_t(when % 50) + 1 &&
                           std::ranges::all_of(got, [&](const L::WorldLight& l) {
                               return l.position[0] == float(when) && l.radius == float(when);
                           });
        bad += whole ? 0 : 1;
    }
    writer.join();
    Expect(bad == 0, "every list read while another thread writes is whole");
}

void TestGameLightEntries() {
    Expect(L::GameLightEntry(0x53b7670, 1) && L::GameLightFeeds(0x53b7670, 1), "a point light with the point vtable feeds the lamps");
    Expect(L::GameLightEntry(0x53b76d0, 2) && !L::GameLightFeeds(0x53b76d0, 2), "a spot is a valid entry but does not feed yet");
    Expect(L::GameLightEntry(0x53b7610, 3) && !L::GameLightFeeds(0x53b7610, 3), "a line light is a valid entry but does not feed");
    Expect(!L::GameLightEntry(0x53b7670, 2), "a vtable that disagrees with the kind is rejected");
    Expect(!L::GameLightEntry(0x1234, 1) && !L::GameLightFeeds(0x1234, 1), "garbage that is no light vtable is rejected");
    Expect(!L::GameLightEntry(0, 0), "an empty entry is rejected");
}

void TestLightTargets() {
    Expect(L::ProxyTargets(true, true, true), "both light targets with proxies over the reduced depth use the proxies");
    Expect(!L::ProxyTargets(true, true, false), "light targets use proxies only when both have one");
    Expect(!L::ProxyTargets(false, true, true), "without a reduced depth the light targets are drawn at full size");
    Expect(L::LightTargetUsable(true, false, true, true),
           "a full-size light target over the reduced depth is drawn into, not skipped");
    Expect(!L::LightTargetUsable(true, false, false, true), "a full-size target needs storage");
    Expect(!L::LightTargetUsable(true, false, true, false), "a full-size target needs to be copyable");
    Expect(L::LightTargetUsable(true, true, false, false), "a proxied target does not need the full-size usage");
    Expect(!L::LightTargetUsable(false, true, true, true), "a target of another format is skipped");
}

int RunSuite() {
    g_failed = 0;
    TestFamilies();
    TestLightTargets();
    TestGameLightEntries();
    TestNormalize();
    TestUsable();
    TestToView();
    TestScreenRect();
    TestSameLight();
    TestForBatch();
    TestTorchCells();
    TestCells();
    TestPlainRecords();
    TestConeProbe();
    TestCrossBatch();
    TestCone();
    TestPickBound();
    TestFeed();
    return g_failed;
}
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
#ifdef LANTERN_RT_LAMPS_MUTANTS
    int survivors = 0;
    for (int m = 0; m <= L::kMutants; ++m) {
        L::g_mutant = m;
        const int found = RunSuite();
        if (m == 0 && found) {
            std::printf("rt lamps mutants: the unmutated run failed %d checks\n", found);
            return 1;
        }
        if (m && !found) {
            std::printf("rt lamps mutants: mutant %d survived\n", m);
            ++survivors;
        }
    }
    if (survivors) {
        return 1;
    }
    std::printf("rt lamps mutants: every mutant caught\n");
    return 0;
#else
    if (RunSuite()) {
        std::printf("rt lamps: %d checks failed\n", g_failed);
        return 1;
    }
    std::printf("rt lamps: all tests passed\n");
    return 0;
#endif
}
