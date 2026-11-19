// SPDX-License-Identifier: GPL-3.0-or-later
#include "light_list.h"
#include "bbport_platform.h"
#include "bbport_settings.h"
#include "../lantern_switches.h"
#include "video_core/renderer_vulkan/rt_lamp_rules.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <unordered_map>
#include <utility>
#include <vector>

namespace BbEngine::LightList {
namespace {
constexpr std::size_t kRenderSystem = 0x5540dd8, kLightManager = 0x5870, kListBegin = 0x18, kListEnd = 0x20;
constexpr std::size_t kDiffuse = 0x70, kStrength = 0x94, kKind = 0xa8, kEnabled = 0xb0, kCastsShadow = 0xb1,
                      kPosition = 0x110;
constexpr std::uint64_t kMaxLights = 8192;
constexpr int kRawReports = 3;
constexpr std::size_t kRawLights = 8;
constexpr float kMovedMetres = 0.05f;
constexpr auto kSample = std::chrono::seconds(1), kReport = std::chrono::seconds(5), kFeedReport = std::chrono::seconds(10);
constexpr std::size_t kSpanFirst = kDiffuse, kSpanBytes = kPosition + 16 - kDiffuse;
constexpr std::uint64_t kFeedList = 4096;
constexpr std::size_t kPerFrame = 96, kMovingPerFrame = 64;
constexpr std::size_t kNotPublished = ~std::size_t(0);
constexpr float kMovingMetres = 0.01f;
constexpr std::uint64_t kMovingFrames = 120;

using Clock = std::chrono::steady_clock;

struct Light {
    std::array<float, 4> position{};
    std::array<float, 4> diffuse{};
    float strength = 0;
    std::uint32_t kind = 0;
    std::uint8_t enabled = 0, casts_shadow = 0;
};

std::uint8_t* g_image = nullptr;
Clock::time_point g_last_sample, g_last_report;
std::unordered_map<std::uint64_t, std::array<float, 3>> g_previous;
std::uint64_t g_samples = 0, g_unreadable = 0;
bool g_shown_first = false;
bool g_log = false, g_feed = false;

struct Seen {
    Vulkan::RtLamps::WorldLight light{};
    std::uint64_t moved_frame = 0;
    std::size_t published = kNotPublished;
    bool usable = false, mover = false;
};
std::unordered_map<std::uint64_t, Seen> g_seen;
std::vector<std::uint64_t> g_pointers, g_listed, g_movers;
bool g_rebuild = true;
std::size_t g_mover_cursor = 0;
std::uint64_t g_feed_rebuilds = 0;
std::vector<Vulkan::RtLamps::WorldLight> g_published;
std::size_t g_cursor = 0;
Clock::time_point g_last_feed_report;
std::uint64_t g_feed_frame = 0;
std::uint64_t g_feed_frames = 0, g_feed_ns = 0, g_feed_max_ns = 0, g_feed_reads = 0, g_feed_unreadable = 0;
std::size_t g_feed_listed = 0;

template <typename T> bool Peek(std::uint64_t at, T* out) {
    return at && BbPlatform::ReadProcessMemory(reinterpret_cast<const void*>(at), out, sizeof(T));
}

std::uint64_t Follow(std::uint64_t at) {
    std::uint64_t v = 0;
    return Peek(at, &v) ? v : 0;
}

std::uint64_t VtableOffset(std::uint64_t light) {
    return Follow(light) - reinterpret_cast<std::uint64_t>(g_image);
}

bool Valid(std::uint64_t light, std::uint32_t kind) {
    return Vulkan::RtLamps::GameLightEntry(VtableOffset(light), kind);
}

int g_raw_reports = 0;

bool Read(std::uint64_t light, Light& out) {
    return Peek(light + kPosition, &out.position) && Peek(light + kDiffuse, &out.diffuse) &&
           Peek(light + kStrength, &out.strength) && Peek(light + kKind, &out.kind) &&
           Peek(light + kEnabled, &out.enabled) && Peek(light + kCastsShadow, &out.casts_shadow);
}

void Show(const char* what, std::uint64_t light, const Light& l) {
    std::printf("  %s 0x%llx: at (%.2f, %.2f, %.2f) radius %.2f, colour (%.2f, %.2f, %.2f, %.2f), enabled %u, "
                "casts shadow %u, kind 0x%x, shadow strength %.2f\n",
                what, static_cast<unsigned long long>(light), l.position[0], l.position[1], l.position[2],
                l.position[3], l.diffuse[0], l.diffuse[1], l.diffuse[2], l.diffuse[3], l.enabled, l.casts_shadow,
                l.kind, l.strength);
}

void Sample(bool report) {
    ++g_samples;
    const std::uint64_t base = reinterpret_cast<std::uint64_t>(g_image);
    const std::uint64_t render = Follow(base + kRenderSystem);
    const std::uint64_t manager = render ? Follow(render + kLightManager) : 0;
    const std::uint64_t begin = Follow(manager + kListBegin), end = Follow(manager + kListEnd);
    if (!render || !begin || end < begin || (end - begin) % 8 != 0 || (end - begin) / 8 > kMaxLights) {
        ++g_unreadable;
        if (report) {
            std::printf("Light list: not readable (render system 0x%llx, list 0x%llx..0x%llx; %llu of %llu samples)\n",
                        static_cast<unsigned long long>(render), static_cast<unsigned long long>(begin),
                        static_cast<unsigned long long>(end), static_cast<unsigned long long>(g_unreadable),
                        static_cast<unsigned long long>(g_samples));
        }
        return;
    }
    const std::uint64_t count = (end - begin) / 8;
    std::unordered_map<std::uint64_t, std::array<float, 3>> now;
    now.reserve(count);
    std::map<std::uint32_t, std::uint64_t> kinds;
    std::uint64_t enabled = 0, shadowed = 0, bad = 0;
    std::vector<std::pair<std::uint64_t, Light>> moved;
    for (std::uint64_t i = 0; i < count; ++i) {
        const std::uint64_t light = Follow(begin + i * 8);
        Light l;
        if (!light || !Read(light, l) || !Valid(light, l.kind)) {
            ++bad;
            continue;
        }
        if (report && g_raw_reports < kRawReports && i < kRawLights) {
            std::uint8_t flags[3]{};
            Peek(light + kEnabled, &flags);
            std::printf("  raw light %llu: vtable +0x%llx, kind %u, flags %u %u %u, +0x110 (%.2f, %.2f, %.2f, %.2f)\n",
                        static_cast<unsigned long long>(i),
                        static_cast<unsigned long long>(Follow(light) - base), l.kind, flags[0], flags[1], flags[2],
                        l.position[0], l.position[1], l.position[2], l.position[3]);
        }
        ++kinds[l.kind];
        enabled += l.enabled ? 1 : 0;
        shadowed += l.casts_shadow ? 1 : 0;
        now[light] = {l.position[0], l.position[1], l.position[2]};
        if (const auto it = g_previous.find(light); it != g_previous.end()) {
            const float dx = l.position[0] - it->second[0], dy = l.position[1] - it->second[1],
                        dz = l.position[2] - it->second[2];
            if (std::sqrt(dx * dx + dy * dy + dz * dz) > kMovedMetres) {
                moved.emplace_back(light, l);
            }
        }
        if (!g_shown_first && i < 3) {
            Show("light", light, l);
        }
    }
    g_shown_first = g_shown_first || count > 0;
    g_raw_reports += report && count ? 1 : 0;
    g_previous = std::move(now);
    if (!report) {
        return;
    }
    std::printf("Light list: %llu lights, %llu enabled, %llu cast shadows, %llu unreadable; kinds",
                static_cast<unsigned long long>(count), static_cast<unsigned long long>(enabled),
                static_cast<unsigned long long>(shadowed), static_cast<unsigned long long>(bad));
    for (const auto& [kind, n] : kinds) {
        std::printf(" 0x%x:%llu", kind, static_cast<unsigned long long>(n));
    }
    std::printf("; %zu moved in the last second\n", moved.size());
    for (std::size_t i = 0; i < moved.size() && i < 6; ++i) {
        Show("moved", moved[i].first, moved[i].second);
    }
}
void Update(std::uint64_t pointer, Seen& seen) {
    std::array<std::uint8_t, kSpanBytes> raw{};
    ++g_feed_reads;
    const bool was = seen.usable;
    Vulkan::RtLamps::WorldLight now{};
    const std::uint64_t vtable_before = pointer ? VtableOffset(pointer) : 0;
    if (!pointer ||
        !BbPlatform::ReadProcessMemory(reinterpret_cast<const void*>(pointer + kSpanFirst), raw.data(), raw.size()) ||
        VtableOffset(pointer) != vtable_before) {
        seen.usable = false;
    } else {
        std::array<float, 4> diffuse{}, position{};
        std::uint32_t kind = 0;
        std::memcpy(diffuse.data(), raw.data() + (kDiffuse - kSpanFirst), sizeof(diffuse));
        std::memcpy(position.data(), raw.data() + (kPosition - kSpanFirst), sizeof(position));
        std::memcpy(&kind, raw.data() + (kKind - kSpanFirst), sizeof(kind));
        seen.usable = Vulkan::RtLamps::GameLightFeeds(vtable_before, kind) &&
                      Vulkan::RtLamps::Usable(kind, raw[kEnabled - kSpanFirst], position, diffuse, now);
    }
    if (seen.usable != was) {
        g_rebuild = true;
    }
    if (!seen.usable) {
        return;
    }
    const float dx = now.position[0] - seen.light.position[0], dy = now.position[1] - seen.light.position[1],
                dz = now.position[2] - seen.light.position[2];
    if (was && dx * dx + dy * dy + dz * dz > kMovingMetres * kMovingMetres) {
        seen.moved_frame = g_feed_frame;
        if (!seen.mover) {
            seen.mover = true;
            g_movers.push_back(pointer);
        }
    }
    seen.light = now;
    if (seen.published < g_published.size()) {
        g_published[seen.published] = now;
    }
}

void Rebuild() {
    g_published.clear();
    std::unordered_map<std::uint64_t, Seen> kept;
    kept.reserve(g_pointers.size());
    for (const std::uint64_t pointer : g_pointers) {
        const auto it = g_seen.find(pointer);
        if (it == g_seen.end()) {
            continue;
        }
        Seen seen = it->second;
        seen.published = kNotPublished;
        if (seen.usable && g_published.size() < Vulkan::RtLamps::FeedCapacity) {
            seen.published = g_published.size();
            g_published.push_back(seen.light);
        }
        kept.emplace(pointer, seen);
    }
    g_seen = std::move(kept);
    std::erase_if(g_movers, [](std::uint64_t pointer) { return !g_seen.contains(pointer); });
    g_rebuild = false;
}

void Feed(Clock::time_point now) {
    const auto start = Clock::now();
    ++g_feed_frame;
    const std::uint64_t base = reinterpret_cast<std::uint64_t>(g_image);
    const std::uint64_t render = Follow(base + kRenderSystem);
    const std::uint64_t manager = render ? Follow(render + kLightManager) : 0;
    const std::uint64_t begin = Follow(manager + kListBegin), end = Follow(manager + kListEnd);
    if (!render || !begin || end < begin || (end - begin) % 8 != 0 || (end - begin) / 8 > kMaxLights) {
        ++g_feed_unreadable;
        return;
    }
    const std::size_t count = std::size_t(std::min<std::uint64_t>((end - begin) / 8, kFeedList));
    g_listed.resize(count);
    if (count && !BbPlatform::ReadProcessMemory(reinterpret_cast<const void*>(begin), g_listed.data(), count * 8)) {
        ++g_feed_unreadable;
        return;
    }
    if (g_listed != g_pointers) {
        g_pointers.swap(g_listed);
        g_rebuild = true;
    }
    const std::size_t window = std::min(count, kPerFrame);
    for (std::size_t k = 0; k < window; ++k) {
        const std::uint64_t pointer = g_pointers[(g_cursor + k) % count];
        Update(pointer, g_seen[pointer]);
    }
    g_cursor = count ? (g_cursor + window) % count : 0;
    std::erase_if(g_movers, [](std::uint64_t pointer) {
        const auto it = g_seen.find(pointer);
        if (it == g_seen.end()) {
            return true;
        }
        if (g_feed_frame - it->second.moved_frame >= kMovingFrames) {
            it->second.mover = false;
            return true;
        }
        return false;
    });
    const std::size_t movers = g_movers.size();
    const std::size_t update = std::min(movers, kMovingPerFrame);
    const std::size_t first = movers ? g_mover_cursor % movers : 0;
    const std::vector<std::uint64_t> due = [&] {
        std::vector<std::uint64_t> out;
        out.reserve(update);
        for (std::size_t k = 0; k < update; ++k) {
            out.push_back(g_movers[(first + k) % movers]);
        }
        return out;
    }();
    g_mover_cursor = movers ? (first + update) % movers : 0;
    for (const std::uint64_t pointer : due) {
        if (const auto it = g_seen.find(pointer); it != g_seen.end()) {
            Update(pointer, it->second);
        }
    }
    if (g_rebuild) {
        Rebuild();
        ++g_feed_rebuilds;
    }
    const auto stamp = std::uint64_t(
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count());
    Vulkan::RtLamps::SharedFeed().Publish(g_published, stamp);
    const auto spent = std::uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count());
    ++g_feed_frames;
    g_feed_ns += spent;
    g_feed_max_ns = std::max(g_feed_max_ns, spent);
    g_feed_listed = count;
    if (now - g_last_feed_report >= kFeedReport) {
        g_last_feed_report = now;
        std::printf("Game lights for ray traced lamps: %zu published of %zu in the list, %.1f lights read a frame, "
                    "%.3f ms a frame (max %.3f), %zu moving, %llu list changes, %llu frames unreadable\n",
                    g_published.size(), g_feed_listed,
                    g_feed_frames ? double(g_feed_reads) / double(g_feed_frames) : 0.0,
                    g_feed_frames ? double(g_feed_ns) / double(g_feed_frames) / 1e6 : 0.0, double(g_feed_max_ns) / 1e6,
                    g_movers.size(), static_cast<unsigned long long>(g_feed_rebuilds),
                    static_cast<unsigned long long>(g_feed_unreadable));
        g_feed_frames = g_feed_ns = g_feed_max_ns = g_feed_reads = g_feed_rebuilds = 0;
    }
}
}

void Install(std::uint8_t* image) {
    const auto& switches = Lantern::Switches::Get();
    g_log = switches.rt_light_list_log;
    g_feed = switches.rt_game_lights && BbSettings::Get().startup_effects[BbSettings::EffectRayTracing];
    if (!g_log && !g_feed) {
        return;
    }
    g_image = image;
    g_last_sample = g_last_report = g_last_feed_report = Clock::now();
    if (g_feed) {
        std::printf("Game lights for ray traced lamps: on (LANTERN_RT_GAME_LIGHTS=1), the game's light list read "
                    "every frame for the lamp pass\n");
    }
    if (!g_log) {
        return;
    }
    std::printf("Light list log: on (LANTERN_RT_LIGHT_LIST_LOG=1), the game's light list every 5 s and the lights "
                "that moved in the last second\n");
}

void Tick() {
    if (!g_image) {
        return;
    }
    const auto now = Clock::now();
    if (g_feed) {
        Feed(now);
    }
    if (!g_log || now - g_last_sample < kSample) {
        return;
    }
    g_last_sample = now;
    const bool report = now - g_last_report >= kReport;
    if (report) {
        g_last_report = now;
    }
    Sample(report);
}
}
