// SPDX-License-Identifier: GPL-3.0-or-later
#include "loading.h"
#include "engine_hooks.h"
#include "bbport_platform.h"
#include "bbport_settings.h"
#include "dlss_rules.h"
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace BbEngine::Loading {
namespace {
constexpr std::size_t kLoadAsked = 0x556286b;
constexpr std::size_t kNowLoadingHelper = 0x553e8b8;
constexpr std::size_t kWorldShownA = 0x50, kWorldShownB = 0x51;
constexpr std::size_t kRemo = 0x5540058, kRemoManager = 8, kRemoPlaying = 0x168;
constexpr std::size_t kPacerMode = 0xc, kPacerVblanks = 0x10, kPacerTarget = 0x18;

constexpr std::size_t kTargetStores[] = {0x2034840, 0x203485a, 0x203487e, 0x20348b3};
constexpr std::uint8_t kTargetStore[] = {0x41, 0xc7, 0x44, 0x24, 0x18};
float g_loading_target = 1.0f / 30.0f;

constexpr std::size_t kLampWarp = 0x13cdf30, kLampLookup = 0x1f26b40, kLampRowVtable = 0x5320b40;
constexpr std::uint32_t kLampFirst = 2000000, kLampLast = 7000000;
constexpr auto kBenchSettled = std::chrono::seconds(5), kBenchWarp = std::chrono::seconds(20);

using Clock = std::chrono::steady_clock;
std::vector<std::uint32_t> g_bench_lamps;
std::size_t g_bench_next = 0;
bool g_bench_on = false, g_bench_list = false, g_bench_said_settled = false;
Clock::time_point g_bench_since;
std::uint8_t* g_image = nullptr;
std::atomic<bool> g_up{false};
bool g_asked = false;
Clock::time_point g_started, g_load_began;
std::uint64_t g_frames = 0;
bool g_world_seen = false;
bool g_fast_allowed = false, g_fast_set = false, g_fast_this_load = false;
float g_game_targets[4]{};
float g_target_before = 0;

template <typename T> bool Peek(std::uint64_t at, T* out) {
    return at && BbPlatform::ReadProcessMemory(reinterpret_cast<const void*>(at), out, sizeof(T));
}
std::uint64_t Follow(std::uint64_t at) {
    std::uint64_t v = 0;
    return Peek(at, &v) ? v : 0;
}
std::uint64_t Image() { return reinterpret_cast<std::uint64_t>(g_image); }

bool CutscenePlaying() {
    const std::uint64_t manager = Follow(Follow(Image() + kRemo) + kRemoManager);
    std::uint32_t state = 0;
    return manager && Peek(manager + kRemoPlaying, &state) && (state & 1);
}

void SetLoadingPace(bool on) {
    if (on == g_fast_set || !g_fast_allowed) return;
    std::array<CodePatch, std::size(kTargetStores)> patches;
    for (std::size_t i = 0; i < std::size(kTargetStores); ++i) {
        const std::size_t imm = kTargetStores[i] + sizeof kTargetStore;
        if (on) std::memcpy(&g_game_targets[i], g_image + imm, sizeof(float));
        const auto loading = AsCode(std::span(&g_loading_target, 1)), game = AsCode(std::span(&g_game_targets[i], 1));
        patches[i] = {g_image, imm, on ? game : loading, on ? loading : game};
    }
    if (const WriteResult result = WriteCode(patches); result != WriteResult::Applied) {
        g_fast_allowed = false;
        std::printf("Loading: fast loading off: the frame pacer's targets %s\n", Describe(result));
        return;
    }
    g_fast_set = on;
    g_fast_this_load |= on;
}

struct Pace {
    std::uint32_t mode = 0, vblanks = 0;
    float target = 0;
};
Pace PaceOf(std::uint64_t pacer) {
    Pace p;
    Peek(pacer + kPacerMode, &p.mode);
    Peek(pacer + kPacerVblanks, &p.vblanks);
    Peek(pacer + kPacerTarget, &p.target);
    return p;
}

struct LampRow {
    std::uint64_t vtable;
    std::uint32_t id;
    const std::uint16_t* row;
};

void ListLamps() {
    for (std::uint32_t lamp = kLampFirst; lamp < kLampLast; ++lamp) {
        LampRow found{Image() + kLampRowVtable, 0xffffffffu, nullptr};
        Call(Image() + kLampLookup, &found, lamp);
        if (found.row) std::printf("Load bench: lamp %u is in map %u_%u\n", lamp, found.row[0], found.row[1]);
    }
}

void BenchTick(bool loading, bool ended, Clock::time_point now) {
    if (g_bench_list) {
        g_bench_list = false;
        ListLamps();
    }
    if (g_bench_next >= g_bench_lamps.size()) return;
    if (loading) return;
    if (ended || g_bench_since == Clock::time_point{}) {
        g_bench_since = now;
        g_bench_said_settled = false;
    }
    if (!g_bench_said_settled && now - g_bench_since >= kBenchSettled) {
        g_bench_said_settled = true;
        std::printf("Load bench: settled\n");
        std::fflush(stdout);
    }
    if (now - g_bench_since < kBenchWarp) return;
    const std::uint32_t lamp = g_bench_lamps[g_bench_next++];
    std::printf("Load bench: to lamp %u\n", lamp);
    std::fflush(stdout);
    g_bench_since = now;
    g_bench_said_settled = true;
    Call(Image() + kLampWarp, lamp);
}
}

void Install(std::uint8_t* image) {
    g_image = image;
    g_started = Clock::now();
    if (const char* pace = std::getenv("LANTERN_LOADING_PACE_MS"); pace && std::atof(pace) > 0) {
        g_loading_target = static_cast<float>(std::atof(pace) / 1000.0);
        std::printf("Loading: loading screens paced at %.1f ms (LANTERN_LOADING_PACE_MS)\n", g_loading_target * 1000.0f);
    }
    if (const char* bench = std::getenv("LANTERN_LOAD_BENCH")) {
        g_bench_on = true;
        g_bench_list = !std::strcmp(bench, "list");
        for (const char* at = bench; !g_bench_list && *at;) {
            char* end = nullptr;
            const unsigned long lamp = std::strtoul(at, &end, 10);
            if (end == at) break;
            g_bench_lamps.push_back(static_cast<std::uint32_t>(lamp));
            at = *end == ',' ? end + 1 : end;
        }
    }
    const char* fast = std::getenv("BB_FAST_LOADING");
    const char* fps = std::getenv("BB_FPS");
    if (!fast || std::strcmp(fast, "1") != 0) return;
    if (fps && !std::strcmp(fps, "30")) {
        std::printf("Loading: fast loading not needed at 30 FPS\n");
        return;
    }
    for (const std::size_t at : kTargetStores) {
        if (std::memcmp(image + at, kTargetStore, sizeof kTargetStore) != 0) {
            std::printf("Loading: fast loading off: the frame pacer's code is not 1.09's\n");
            return;
        }
    }
    g_fast_allowed = true;
    std::printf("Loading: fast loading on (loading screens paced at 30 FPS)\n");
}

bool ScreenUp() { return g_up.load(std::memory_order_relaxed); }

void Tick(std::uint64_t pacer) {
    if (!g_image) return;
    std::uint8_t asked = 0, shown_a = 0, shown_b = 0;
    Peek(Image() + kLoadAsked, &asked);
    const std::uint64_t helper = Follow(Image() + kNowLoadingHelper);
    const bool world = helper && ((Peek(helper + kWorldShownA, &shown_a) && shown_a) ||
                                  (Peek(helper + kWorldShownB, &shown_b) && shown_b));
    const bool loading = asked && !world;
    if (BbDlss::LoadArrived(g_asked, asked != 0)) BbSettings::Get().history_cuts.fetch_add(1, std::memory_order_relaxed);
    g_asked = asked != 0;
    const auto now = Clock::now();
    ++g_frames;
    const bool was = g_up.exchange(loading, std::memory_order_relaxed);
    if (g_fast_allowed) SetLoadingPace(loading && !CutscenePlaying());

    if (loading && !was) {
        g_load_began = now;
        g_frames = 0;
        const Pace p = PaceOf(pacer);
        if (p.target != g_loading_target) g_target_before = p.target;
        std::printf("Loading: begins; pacer mode %u, %u vblank(s) a frame, target %.1f ms\n", p.mode, p.vblanks,
                    p.target * 1000.0f);
    } else if (!loading && was) {
        const double seconds = std::chrono::duration<double>(now - g_load_began).count();
        const bool cutscene = CutscenePlaying();
        float live = 0;
        if (g_fast_this_load && !cutscene && g_target_before > 0 && Peek(pacer + kPacerTarget, &live) &&
            live == g_loading_target) {
            *reinterpret_cast<volatile float*>(pacer + kPacerTarget) = g_target_before;
            std::printf("Loading: the frame pace back to %.1f ms (the loading pace stayed set)\n",
                        g_target_before * 1000.0f);
        }
        g_fast_this_load = false;
        const Pace p = PaceOf(pacer);
        std::printf("Loading: %.2f s, %llu frames (%.1f ms each)%s; pacer mode %u, %u vblank(s), target %.1f ms\n",
                    seconds, static_cast<unsigned long long>(g_frames),
                    g_frames ? seconds * 1000.0 / double(g_frames) : 0.0, cutscene ? ", a cutscene follows" : "",
                    p.mode, p.vblanks, p.target * 1000.0f);
    }
    if (world && !g_world_seen) {
        g_world_seen = true;
        std::printf("Loading: the first in-game frame, %.1f s after the game started\n",
                    std::chrono::duration<double>(now - g_started).count());
    }
    if (g_bench_on && g_world_seen) BenchTick(loading, !loading && was, now);
}
}
