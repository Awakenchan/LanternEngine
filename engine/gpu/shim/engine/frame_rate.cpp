// SPDX-License-Identifier: GPL-3.0-or-later
#include "frame_rate.h"
#include "engine_hooks.h"
#include "engine_state.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#endif

namespace BbEngine::FrameRate {
namespace {
enum class SiteKind {
    Step, ContextStep, TargetStep, HighStep, EffectStep, CountSixth, CountThird, Three, NegThree, EaseDistance,
    EaseChase, FrameModeTime, WaitSleep, LimiterCap, SlideGain, SlideDecay
};

struct Site {
    std::uint32_t offset;
    SiteKind kind;
    bool code;
    bool checked;
    std::uint8_t prefix[4];
};

struct ListWrite {
    std::uint8_t list;
    std::uint32_t offset;
    std::uint16_t size;
    std::uint32_t at;
};

constexpr Site kSites[] = {
#include "frame_rate_sites.inc"
};
#include "frame_rate_lists.inc"

constexpr Site kSlopeSites[] = {
    {0x5128370, SiteKind::SlideGain, false, false, {}},
    {0x5128374, SiteKind::SlideDecay, false, false, {}},
};

enum List : std::uint8_t { Sixty, Ninety, Uncapped, Sprint };

constexpr std::size_t kFrameTime = 0x535a908;
constexpr std::size_t kFrameTimeCave = 0x2018e3d;
constexpr std::uint8_t kFrameTimeCaveCode[] = {0xc5, 0xfa, 0x10, 0x83, 0x64, 0x02, 0x00, 0x00};
constexpr std::size_t kWorldStep = 0x17f9b71;
constexpr std::size_t kWorldStepNext = 0x17f9b78;
constexpr std::uint8_t kWorldStepCode[] = {0xc7, 0x45, 0xb8};
constexpr std::size_t kWorldLockFail = 0x17f9bde;
constexpr std::uint8_t kWorldLockFailCode[] = {0x0f, 0x85, 0xc5, 0x00, 0x00, 0x00};
constexpr std::uint8_t kNop6[] = {0x66, 0x0f, 0x1f, 0x44, 0x00, 0x00};
constexpr std::size_t kWorldCave = 0x17f9ca9;
constexpr std::size_t kWorldCaveSize = 46;
constexpr std::size_t kFlipper = 0x55404f8;
constexpr std::size_t kLongestStep = 0x4926364;
constexpr float kSlowest = 30.0f;
constexpr int kSettleFrames = 120;
constexpr int kRiseFrames = 30;
constexpr float kDrift = 0.1f;

std::uint8_t* g_image = nullptr;
std::vector<const Site*> g_data, g_code;
float g_target = 60.0f;
float g_code_rate = 60.0f;
float g_held = 0.0f;
int g_drift_frames = 0;
int g_adjustments = 0;
bool g_uncapped = false;
bool g_follow = true;
bool g_data_writable = false;
const Site* g_slide_gain = nullptr;
float g_slide_clock = 0.0f;
std::atomic<float> g_shown{0.0f};

float SlideGainNow(float frame) {
    constexpr float tick = 1.0f / 30.0f;
    constexpr float slack = 1e-5f;
    if (frame >= tick - slack) {
        g_slide_clock = 0.0f;
        return 1.0f;
    }
    g_slide_clock += frame;
    if (g_slide_clock < tick - slack) return 0.0f;
    g_slide_clock = std::min(g_slide_clock - tick, tick);
    return 1.0f;
}

bool Switch(const char* name, bool fallback) {
    const char* v = std::getenv(name);
    if (!v || !*v) return fallback;
    return v[0] != '0';
}

bool CodeKind(SiteKind kind) {
    return kind == SiteKind::Step || kind == SiteKind::ContextStep || kind == SiteKind::HighStep ||
           kind == SiteKind::EffectStep || kind == SiteKind::CountSixth || kind == SiteKind::CountThird;
}

bool TargetKind(SiteKind kind) {
    return kind == SiteKind::FrameModeTime || kind == SiteKind::WaitSleep || kind == SiteKind::LimiterCap;
}

std::int32_t LimiterSleep(float rate) {
    return rate <= 60.0f ? 16000 : rate <= 90.0f ? 10000 : rate <= 120.0f ? 8000 : 6500;
}

std::uint32_t Value(SiteKind kind, float rate) {
    float f = 0.0f;
    std::int32_t i = 0;
    bool integer = false;
    switch (kind) {
    case SiteKind::Step:
    case SiteKind::ContextStep:
    case SiteKind::TargetStep:
    case SiteKind::FrameModeTime: f = 1.0f / rate; break;
    case SiteKind::HighStep: f = rate > 60.0f ? 1.0f / rate : 1.0f / 30.0f; break;
    case SiteKind::EffectStep: f = 1.125f / rate; break;
    case SiteKind::CountSixth: i = std::max(5, int(rate / 6.0f)); integer = true; break;
    case SiteKind::CountThird: i = std::max(10, int(rate / 3.0f)); integer = true; break;
    case SiteKind::Three: f = 3.0f / rate; break;
    case SiteKind::NegThree: f = -3.0f / rate; break;
    case SiteKind::EaseDistance: f = float(1.0 - std::pow(0.9, 30.0 / rate)); break;
    case SiteKind::EaseChase: f = float(1.0 - std::pow(1.0 - 1.0 / 30.0, 30.0 / rate)); break;
    case SiteKind::SlideGain: f = 30.0f / rate; break;
    case SiteKind::SlideDecay: f = float(std::pow(0.65, 30.0 / rate)); break;
    case SiteKind::WaitSleep: i = std::int32_t(990000.0f / rate); integer = true; break;
    case SiteKind::LimiterCap: i = LimiterSleep(rate); integer = true; break;
    }
    std::uint32_t bits;
    if (integer) std::memcpy(&bits, &i, 4);
    else std::memcpy(&bits, &f, 4);
    return bits;
}

bool Plausible(SiteKind kind, const std::uint8_t* at) {
    float f;
    std::int32_t i;
    std::memcpy(&f, at, 4);
    std::memcpy(&i, at, 4);
    switch (kind) {
    case SiteKind::Step:
    case SiteKind::ContextStep:
    case SiteKind::TargetStep:
    case SiteKind::HighStep:
    case SiteKind::FrameModeTime: return f > 1.0f / 1000.0f && f < 0.034f;
    case SiteKind::EffectStep: return f > 0.0005f && f < 0.04f;
    case SiteKind::CountSixth: return i >= 5 && i <= 200;
    case SiteKind::CountThird: return i >= 10 && i <= 400;
    case SiteKind::Three: return f > 0.002f && f < 0.11f;
    case SiteKind::NegThree: return f < -0.002f && f > -0.11f;
    case SiteKind::EaseDistance: return f > 0.0f && f < 0.2f;
    case SiteKind::EaseChase: return f > 0.0f && f < 0.07f;
    case SiteKind::SlideGain: return f > 0.0f && f <= 1.0001f;
    case SiteKind::SlideDecay: return f > 0.0f && f < 1.0f;
    case SiteKind::WaitSleep:
    case SiteKind::LimiterCap: return i >= 500 && i <= 40000;
    }
    return false;
}

float TargetRate(bool& uncapped) {
    const char* fps = std::getenv("BB_FPS");
    const std::string v = fps ? fps : "60";
    uncapped = v == "uncap";
    if (uncapped) {
        const char* limit = std::getenv("BB_FPS_LIMIT");
        const int l = limit ? std::atoi(limit) : 0;
        return float(l >= 30 && l <= 1000 ? l : 240);
    }
    const int f = std::atoi(v.c_str());
    return float(f >= 30 && f <= 1000 ? f : 60);
}

bool Unlock(std::uint8_t* at, std::size_t size) {
    const std::uintptr_t first = reinterpret_cast<std::uintptr_t>(at) & ~std::uintptr_t(0xfff);
    const std::uintptr_t end = (reinterpret_cast<std::uintptr_t>(at) + size + 0xfff) & ~std::uintptr_t(0xfff);
#ifdef _WIN32
    DWORD before = 0;
    return VirtualProtect(reinterpret_cast<void*>(first), end - first, PAGE_EXECUTE_READWRITE, &before) != 0;
#else
    return mprotect(reinterpret_cast<void*>(first), end - first, PROT_READ | PROT_WRITE | PROT_EXEC) == 0;
#endif
}

void ApplyLists(std::uint8_t* image, std::size_t size) {
    bool wanted[4] = {};
    if (g_uncapped) wanted[Uncapped] = wanted[Sprint] = true;
    else if (g_target > 60.0f) wanted[Ninety] = wanted[Sprint] = true;
    else wanted[Sixty] = true;
    int written = 0, already = 0, kept = 0;
    for (const ListWrite& w : kListWrites) {
        if (!wanted[w.list]) continue;
        if (std::size_t(w.offset) + w.size > size) { ++kept; continue; }
        std::uint8_t* at = image + w.offset;
        const std::uint8_t* original = kListBlob + w.at;
        const std::uint8_t* patched = original + w.size;
        if (std::memcmp(at, original, w.size) == 0) {
            if (!Unlock(at, w.size)) { ++kept; continue; }
            std::memcpy(at, patched, w.size);
            ++written;
        } else if (std::memcmp(at, patched, w.size) == 0) {
            ++already;
        } else {
            ++kept;
        }
    }
    host_log("Frame rate: the engine wrote the frame-rate code for %.0f FPS%s (%d writes, %d already in place, %d left "
             "to another patch)",
             double(g_target), g_uncapped ? " uncapped" : "", written, already, kept);
}

int ApplyWallSpeed(std::uint8_t* image, std::size_t size) {
    int written = 0;
    for (const ListWrite& w : kListWrites) {
        if (w.list != Sprint || std::size_t(w.offset) + w.size > size) continue;
        std::uint8_t* at = image + w.offset;
        const std::uint8_t* original = kListBlob + w.at;
        if (std::memcmp(at, original, w.size) != 0 || !Unlock(at, w.size)) continue;
        std::memcpy(at, original + w.size, w.size);
        ++written;
    }
    return written;
}

void PutRel(std::vector<std::uint8_t>& code, std::size_t base, std::size_t target) {
    const std::int32_t rel = std::int32_t(std::int64_t(target) - std::int64_t(base + code.size() + 4));
    const auto* b = reinterpret_cast<const std::uint8_t*>(&rel);
    code.insert(code.end(), b, b + 4);
}

std::vector<std::uint8_t> WorldCave() {
    std::vector<std::uint8_t> c = {0x48, 0x8b, 0x05};
    PutRel(c, kWorldCave, kFlipper);
    c.insert(c.end(), {0xc5, 0xfa, 0x10, 0x80, 0x64, 0x02, 0x00, 0x00, 0xc5, 0xfa, 0x5f, 0x40, 0x18, 0xc5, 0xfa, 0x5d, 0x05});
    PutRel(c, kWorldCave, kLongestStep);
    c.insert(c.end(), {0xc5, 0xfa, 0x11, 0x45, 0xb8, 0xe9});
    PutRel(c, kWorldCave, kWorldStepNext);
    return c;
}

std::vector<std::uint8_t> WorldJump() {
    std::vector<std::uint8_t> j = {0xe9};
    PutRel(j, kWorldStep, kWorldCave);
    j.insert(j.end(), {0x66, 0x90});
    return j;
}

const std::uint8_t* GameBytes(std::size_t offset, std::size_t size) {
    for (const ListWrite& w : kListWrites)
        if (w.list == Uncapped && w.offset == offset && w.size == size) return kListBlob + w.at;
    return nullptr;
}

bool ApplyWorldStep(std::uint8_t* image, std::size_t size) {
    if (size < kLongestStep + 4) return false;
    float longest;
    std::memcpy(&longest, image + kLongestStep, 4);
    if (std::fabs(longest - 1.0f / 30.0f) > 1e-6f) return false;
    const std::vector<std::uint8_t> cave = WorldCave(), jump = WorldJump();
    std::uint8_t* step = image + kWorldStep;
    std::uint8_t* fail = image + kWorldLockFail;
    std::uint8_t* room = image + kWorldCave;
    if (std::memcmp(step, jump.data(), jump.size()) == 0 && std::memcmp(room, cave.data(), cave.size()) == 0) return true;
    const std::uint8_t* game = GameBytes(kWorldCave, kWorldCaveSize);
    if (!game || std::memcmp(room, game, kWorldCaveSize) != 0) return false;
    if (std::memcmp(step, kWorldStepCode, sizeof kWorldStepCode) != 0 || !Plausible(SiteKind::TargetStep, step + 3))
        return false;
    if (std::memcmp(fail, kWorldLockFailCode, sizeof kWorldLockFailCode) != 0) return false;
    if (!Unlock(room, kWorldCaveSize) || !Unlock(fail, sizeof kNop6) || !Unlock(step, jump.size())) return false;
    std::memcpy(room, cave.data(), cave.size());
    std::memset(room + cave.size(), 0xcc, kWorldCaveSize - cave.size());
    std::memcpy(fail, kNop6, sizeof kNop6);
    std::memcpy(step, jump.data(), jump.size());
    return true;
}

bool Store(std::uint8_t* at, std::uint32_t bits) {
    std::uint32_t now;
    std::memcpy(&now, at, 4);
    if (now == bits) return true;
    if (!Unlock(at, 4)) return false;
    std::memcpy(at, &bits, 4);
    return true;
}

void WriteCodeSites(float rate) {
    for (const Site* s : g_code) Store(g_image + s->offset, Value(s->kind, rate));
    g_code_rate = rate;
}

bool MakeDataWritable() {
    std::vector<std::uintptr_t> pages;
    std::vector<const Site*> sites = g_data;
    if (g_slide_gain) sites.push_back(g_slide_gain);
    for (const Site* s : sites) {
        const std::uintptr_t page = reinterpret_cast<std::uintptr_t>(g_image + s->offset) & ~std::uintptr_t(0xfff);
        if (std::find(pages.begin(), pages.end(), page) == pages.end()) pages.push_back(page);
    }
    for (const std::uintptr_t page : pages) {
#ifdef _WIN32
        DWORD before = 0;
        if (!VirtualProtect(reinterpret_cast<void*>(page), 0x1000, PAGE_EXECUTE_READWRITE, &before)) return false;
#else
        if (mprotect(reinterpret_cast<void*>(page), 0x1000, PROT_READ | PROT_WRITE | PROT_EXEC)) return false;
#endif
    }
    return true;
}

float Rounded(float rate) {
    return std::clamp(std::round(rate), kSlowest, g_target);
}
}

bool Install(std::uint8_t* image, std::size_t size) {
    if (!Switch("LANTERN_FRAMERATE", true)) {
        host_log("Frame rate: native frame rate off (Engine switches); the start's frame-rate patches are used");
        return false;
    }
    g_target = TargetRate(g_uncapped);
    if (!image || size < kFrameTime + 4) return false;
    if (!g_uncapped && g_target <= 30.0f) {
        host_log("Frame rate: the game's own 30 FPS");
        return false;
    }
    if (Switch("LANTERN_FRAMERATE_FROM_ENGINE", false)) ApplyLists(image, size);
    if (std::memcmp(image + kFrameTimeCave, kFrameTimeCaveCode, sizeof kFrameTimeCaveCode) != 0) {
        host_log("Frame rate: the frame-time code is not in place; the game runs at its own 30 FPS");
        return false;
    }
    if (Switch("LANTERN_FRAMERATE_WALL_SPEED", true)) {
        const int wall = ApplyWallSpeed(image, size);
        if (wall)
            host_log("Frame rate: the blocked-movement check measures speed, not distance a frame, at %.0f FPS%s (%d writes)",
                     double(g_target), g_uncapped ? " uncapped" : "", wall);
    }
    g_follow = Switch("LANTERN_FRAMERATE_FOLLOW", true);
    int skipped = 0, held = 0, fixed = 0;
    for (const Site& s : kSites) {
        if (s.offset + 4 > size) { ++skipped; continue; }
        std::uint8_t* at = image + s.offset;
        if ((s.checked && std::memcmp(at - 4, s.prefix, 4) != 0) || !Plausible(s.kind, at)) {
            ++skipped;
            continue;
        }
        if (TargetKind(s.kind)) {
            if (g_uncapped) continue;
            if (!Store(at, Value(s.kind, g_target))) { ++skipped; continue; }
            ++fixed;
            continue;
        }
        if (s.kind == SiteKind::TargetStep) {
            if (!Store(at, Value(s.kind, g_target))) { ++skipped; continue; }
            ++held;
            continue;
        }
        (CodeKind(s.kind) && s.code ? g_code : g_data).push_back(&s);
    }
    if (!g_uncapped && Switch("LANTERN_FRAMERATE_WORLD_STEP", true)) {
        if (ApplyWorldStep(image, size))
            host_log("Frame rate: the physics world steps with the frame time the characters get, not a fixed 1/%.0f",
                     double(g_target));
        else
            host_log("Frame rate: the physics world step is not the game's code; it stays at 1/%.0f", double(g_target));
    }
    int slope = 0;
    g_slide_gain = nullptr;
    g_slide_clock = 0.0f;
    if (Switch("LANTERN_FRAMERATE_SLOPE", true)) {
        const bool ticked = g_follow && Switch("LANTERN_FRAMERATE_SLOPE_TICK", true);
        for (const Site& s : kSlopeSites) {
            if (s.offset + 4 > size || !Plausible(s.kind, image + s.offset)) continue;
            if (ticked && s.kind == SiteKind::SlideGain) g_slide_gain = &s;
            else g_data.push_back(&s);
            ++slope;
        }
        host_log("Frame rate: the steep-ground slide follows the frame time (%d of %zu values)%s", slope,
                 sizeof kSlopeSites / sizeof kSlopeSites[0],
                 g_slide_gain ? "; it builds on the console's 1/30 s ticks, as the game checks it at 30 FPS" : "");
    }
    g_image = image;
    for (const Site* s : g_code) Store(image + s->offset, Value(s->kind, g_target));
    for (const Site* s : g_data) Store(image + s->offset, Value(s->kind, g_target));
    if (g_slide_gain) Store(image + g_slide_gain->offset, Value(SiteKind::SlideGain, g_target));
    g_code_rate = g_target;
    g_held = g_target;
    host_log("Frame rate: native at %.0f FPS%s: %zu code, %zu data, %d target and %d fixed sites (%d left as they were); "
             "%s",
             double(g_target), g_uncapped ? " (uncapped limit)" : "", g_code.size(), g_data.size(), fixed, held, skipped,
             g_follow ? "steps follow the frame rate the game holds" : "steps fixed at the target");
    return true;
}

void Tick() {
    if (!g_image || !g_follow) return;
    float frame;
    std::memcpy(&frame, g_image + kFrameTime, 4);
    if (!(frame > 0.0f) || !std::isfinite(frame)) return;
    const float rate = std::clamp(1.0f / frame, kSlowest, g_target);
    g_held = g_held + (rate - g_held) * 0.05f;
    g_shown.store(g_held, std::memory_order_relaxed);
    if (!g_data_writable) {
        if (!MakeDataWritable()) {
            host_log("Frame rate: the data sites cannot be made writable; they stay at %.0f FPS", double(g_target));
            g_data.clear();
            g_slide_gain = nullptr;
        }
        g_data_writable = true;
    }
    for (const Site* s : g_data) {
        const std::uint32_t bits = Value(s->kind, rate);
        __atomic_store_n(reinterpret_cast<std::uint32_t*>(g_image + s->offset), bits, __ATOMIC_RELAXED);
    }
    if (g_slide_gain && g_data_writable) {
        const float gain = SlideGainNow(frame);
        std::uint32_t bits;
        std::memcpy(&bits, &gain, 4);
        __atomic_store_n(reinterpret_cast<std::uint32_t*>(g_image + g_slide_gain->offset), bits, __ATOMIC_RELAXED);
    }
    if (g_code.empty()) return;
    const float wanted = Rounded(g_held);
    const bool up = wanted > g_code_rate;
    const bool stall = g_held < kSlowest + 2.0f;
    if (std::fabs(wanted - g_code_rate) > g_code_rate * kDrift && (up || !stall)) {
        if (++g_drift_frames >= (up ? kRiseFrames : kSettleFrames)) {
            ++g_adjustments;
            if (g_adjustments <= 20 || g_adjustments % 50 == 0)
                host_log("Frame rate: the game holds %.0f FPS (target %.0f): steps set for %.0f (%d changes so far)",
                         double(g_held), double(g_target), double(wanted), g_adjustments);
            WriteCodeSites(wanted);
            g_drift_frames = 0;
        }
    } else {
        g_drift_frames = 0;
    }
}

float HeldRate() {
    return g_shown.load(std::memory_order_relaxed);
}
}
