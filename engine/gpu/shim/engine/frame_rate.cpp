// SPDX-License-Identifier: GPL-3.0-or-later
#include "frame_rate.h"
#include "engine_hooks.h"
#include "engine_state.h"
#include "../lantern_frame_clock.h"
#include "../lantern_meter.h"
#include "../lantern_pacing_stats.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <span>
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

extern "C" std::uint64_t host_realtime_ns(void);

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

#ifdef LANTERN_HOOK_MUTANTS
constexpr bool kMutants = true;
#else
constexpr bool kMutants = false;
#endif
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
constexpr std::uint8_t kTrap = 0xcc;
constexpr std::size_t kFlipper = 0x55404f8;
constexpr std::size_t kLongestStep = 0x4926364;
constexpr float kSlowest = 30.0f;
constexpr int kSettleFrames = 120;
constexpr int kRiseFrames = 30;
constexpr float kDrift = 0.1f;

std::uint8_t* g_image = nullptr;
const std::uint8_t* g_stats_image = nullptr;
constexpr std::size_t kFrameDt = 0x264;
constexpr std::size_t kTimerTarget = 0x18;
constexpr std::size_t kTimerStamp = 0x28;
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
bool g_frame_start = false;
std::int64_t g_last_start = 0;
std::int64_t g_step_carry = 0;

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

class Stores {
public:
    Stores() = default;
    Stores(std::uint8_t* image, std::size_t capacity) : image_(image) {
        values_.reserve(capacity);
        patches_.reserve(capacity);
    }
    void Reset() {
        values_.clear();
        patches_.clear();
    }
    bool Add(std::size_t offset, std::uint32_t bits) {
        std::uint32_t now;
        std::memcpy(&now, image_ + offset, 4);
        if (now == bits) return true;
        if (values_.size() == values_.capacity()) return false;
        values_.push_back({now, bits});
        const auto& v = values_.back();
        patches_.push_back({image_, offset, AsCode(std::span(&v[0], 1)), AsCode(std::span(&v[1], 1))});
        return true;
    }
    void Drop(std::size_t offset, std::size_t size) {
        std::erase_if(patches_, [&](const CodePatch& p) { return p.offset < offset + size && offset < p.offset + p.patched.size(); });
    }
    WriteResult Write() const { return patches_.empty() ? WriteResult::Applied : WriteCode(patches_); }

private:
    std::uint8_t* image_ = nullptr;
    std::vector<std::array<std::uint32_t, 2>> values_;
    std::vector<CodePatch> patches_;
};
Stores g_code_stores;
std::vector<std::size_t> g_data_offsets;

struct ListCounts {
    int written = 0, already = 0, kept = 0;
};

ListCounts ApplyLists(std::uint8_t* image, std::size_t size) {
    bool wanted[4] = {};
    if (g_uncapped) wanted[Uncapped] = wanted[Sprint] = true;
    else if (g_target > 60.0f) wanted[Ninety] = wanted[Sprint] = true;
    else wanted[Sixty] = true;
    int written = 0, already = 0, kept = 0;
    std::vector<CodePatch> patches;
    for (const ListWrite& w : kListWrites) {
        if (!wanted[w.list]) continue;
        if (std::size_t(w.offset) + w.size > size) { ++kept; continue; }
        std::uint8_t* at = image + w.offset;
        const std::span<const std::uint8_t> original(kListBlob + w.at, w.size), patched(kListBlob + w.at + w.size, w.size);
        const auto pending = std::find_if(patches.begin(), patches.end(), [&](const CodePatch& p) {
            return p.offset < w.offset + w.size && w.offset < p.offset + p.patched.size();
        });
        if (pending != patches.end() && !kMutants) {
            if (pending->offset == w.offset && std::ranges::equal(pending->patched, patched)) ++already;
            else ++kept;
        } else if (std::memcmp(at, original.data(), w.size) == 0) patches.push_back({image, std::size_t(w.offset), original, patched});
        else if (std::memcmp(at, patched.data(), w.size) == 0) ++already;
        else ++kept;
    }
    if (const WriteResult result = WriteCode(patches); result == WriteResult::Applied) {
        written = int(patches.size());
    } else {
        kept += int(patches.size());
        host_log("Frame rate: the engine's frame-rate code %s", Describe(result));
    }
    host_log("Frame rate: the engine wrote the frame-rate code for %.0f FPS%s (%d writes, %d already in place, %d left "
             "to another patch)",
             double(g_target), g_uncapped ? " uncapped" : "", written, already, kept);
    return {written, already, kept};
}

int ApplyWallSpeed(std::uint8_t* image, std::size_t size) {
    std::vector<CodePatch> patches;
    for (const ListWrite& w : kListWrites) {
        if (w.list != Sprint || std::size_t(w.offset) + w.size > size) continue;
        const std::span<const std::uint8_t> original(kListBlob + w.at, w.size), patched(kListBlob + w.at + w.size, w.size);
        if (std::memcmp(image + w.offset, original.data(), w.size) == 0) patches.push_back({image, std::size_t(w.offset), original, patched});
    }
    if (const WriteResult result = WriteCode(patches); result != WriteResult::Applied) {
        host_log("Frame rate: the blocked-movement check %s", Describe(result));
        return 0;
    }
    return int(patches.size());
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
    static_assert(sizeof kNop6 <= sizeof kWorldLockFailCode);
    std::vector<std::uint8_t> room_code(kWorldCaveSize, kTrap);
    std::copy(cave.begin(), cave.end(), room_code.begin());
    const std::vector<std::uint8_t> step_code(step, step + jump.size());
    const CodePatch patches[] = {{image, kWorldCave, std::span(game, kWorldCaveSize), room_code},
                                 {image, kWorldLockFail, std::span(kWorldLockFailCode).first(sizeof kNop6), kNop6},
                                 {image, kWorldStep, step_code, jump}};
    if (const WriteResult result = WriteCode(patches); result != WriteResult::Applied) {
        host_log("Frame rate: the physics world step code %s", Describe(result));
        return false;
    }
    return true;
}

void WriteCodeSites(float rate) {
    g_code_stores.Reset();
    for (const Site* s : g_code) g_code_stores.Add(s->offset, Value(s->kind, rate));
    if (const WriteResult result = g_code_stores.Write(); result != WriteResult::Applied) {
        host_log("Frame rate: the steps stay set for %.0f FPS: their code %s", double(g_code_rate), Describe(result));
        g_code.clear();
        return;
    }
    g_code_rate = rate;
}

bool MakeDataWritable() {
    return KeepWritable(g_image, g_data_offsets, sizeof(std::uint32_t)) == WriteResult::Applied;
}

float Rounded(float rate) {
    return std::clamp(std::round(rate), kSlowest, g_target);
}

std::int64_t RealtimeMicros() {
    return std::int64_t(host_realtime_ns() / 1000);
}

std::uintptr_t Timer() {
    std::uintptr_t timer = 0;
    if (g_image) std::memcpy(&timer, g_image + kFlipper, sizeof timer);
    return timer;
}

void PaceFrameStart() {
    const std::uintptr_t timer = Timer();
    LanternFrameClock::Grid grid;
    if (!timer || !LanternFrameClock::Current(grid)) {
        g_last_start = 0;
        return;
    }
    float target;
    std::memcpy(&target, reinterpret_cast<const void*>(timer + kTimerTarget), 4);
    if (!LanternFrameClock::PacesTarget(target, g_target)) {
        g_last_start = 0;
        return;
    }
    const std::int64_t now = LanternFrameClock::SteadyNs();
    const std::int64_t start = LanternFrameClock::NextStart(now, g_last_start, grid);
    if (start > now) {
        Lantern::Meter::NoteCapWait(std::uint64_t(start - now));
        LanternFrameClock::WaitUntil(start);
    }
    const std::int64_t step = LanternFrameClock::Step(start, g_last_start, grid, g_step_carry);
    LanternPacingStats::Paced(start > now ? start - now : 0);
    g_last_start = start;
    const std::int64_t stamp = RealtimeMicros() - LanternFrameClock::StepMicros(step, target);
    std::memcpy(reinterpret_cast<void*>(timer + kTimerStamp), &stamp, sizeof stamp);
}

void RecordStep() {
    float dt = 0.0f, average = 0.0f;
    if (g_stats_image) {
        std::uintptr_t timer;
        std::memcpy(&timer, g_stats_image + kFlipper, sizeof timer);
        if (timer) std::memcpy(&dt, reinterpret_cast<const std::uint8_t*>(timer) + kFrameDt, 4);
        std::memcpy(&average, g_stats_image + kFrameTime, 4);
    }
    LanternPacingStats::Step(dt, average, LanternFrameClock::LockedVblank());
}
}

bool Install(std::uint8_t* image, std::size_t size) {
    g_stats_image = image && size >= kFlipper + 8 && LanternPacingStats::Enabled() ? image : nullptr;
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
    Stores stores(image, std::size(kSites) + std::size(kSlopeSites));
    for (const Site& s : kSites) {
        if (s.offset + 4 > size) { ++skipped; continue; }
        std::uint8_t* at = image + s.offset;
        if ((s.checked && std::memcmp(at - 4, s.prefix, 4) != 0) || !Plausible(s.kind, at)) {
            ++skipped;
            continue;
        }
        if (TargetKind(s.kind)) {
            if (g_uncapped) continue;
            stores.Add(s.offset, Value(s.kind, g_target));
            ++fixed;
            continue;
        }
        if (s.kind == SiteKind::TargetStep) {
            stores.Add(s.offset, Value(s.kind, g_target));
            ++held;
            continue;
        }
        (CodeKind(s.kind) && s.code ? g_code : g_data).push_back(&s);
    }
    if (!g_uncapped && Switch("LANTERN_FRAMERATE_WORLD_STEP", true)) {
        if (ApplyWorldStep(image, size)) {
            if (!kMutants) stores.Drop(kWorldStep, WorldJump().size());
            host_log("Frame rate: the physics world steps with the frame time the characters get, not a fixed 1/%.0f",
                     double(g_target));
        } else
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
    for (const Site* s : g_code) stores.Add(s->offset, Value(s->kind, g_target));
    for (const Site* s : g_data) stores.Add(s->offset, Value(s->kind, g_target));
    if (g_slide_gain) stores.Add(g_slide_gain->offset, Value(SiteKind::SlideGain, g_target));
    if (const WriteResult result = stores.Write(); result != WriteResult::Applied) {
        g_code.clear();
        g_data.clear();
        g_slide_gain = nullptr;
        host_log("Frame rate: native frame rate off: its sites %s", Describe(result));
        return false;
    }
    g_code_stores = Stores(image, g_code.size());
    g_data_offsets.clear();
    for (const Site* s : g_data) g_data_offsets.push_back(s->offset);
    if (g_slide_gain) g_data_offsets.push_back(g_slide_gain->offset);
    g_image = image;
    g_code_rate = g_target;
    g_held = g_target;
    g_frame_start = !g_uncapped && size >= kFlipper + 8;
    if (g_frame_start) Lantern::Meter::SetGameCap(std::uint32_t(g_target));
    g_last_start = 0;
    if (g_frame_start)
        host_log("Frame rate: frame start pacing %s; Insert menu, FPS counter: Frame start pacing switches it during the run",
                 LanternFrameClock::Live() ? "on (default)" : "off (LANTERN_FRAME_START_PACING=0)");
    else if (g_uncapped)
        host_log("Frame rate: frame start pacing off (uncapped frame rate)");
    host_log("Frame rate: native at %.0f FPS%s: %zu code, %zu data, %d target and %d fixed sites (%d left as they were); "
             "%s",
             double(g_target), g_uncapped ? " (uncapped limit)" : "", g_code.size(), g_data.size(), fixed, held, skipped,
             g_follow ? "steps follow the frame rate the game holds" : "steps fixed at the target");
    return true;
}

void Tick() {
    Lantern::Meter::NotePlace();
    if (g_frame_start && LanternFrameClock::Live().load(std::memory_order_relaxed)) PaceFrameStart();
    else g_last_start = 0;
    if (LanternPacingStats::Enabled()) RecordStep();
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
