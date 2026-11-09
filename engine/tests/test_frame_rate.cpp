// SPDX-License-Identifier: GPL-3.0-or-later
#include "gpu/shim/engine/frame_rate.cpp"
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#ifdef _WIN32
#include <malloc.h>
#endif

namespace BbEngine {
void host_log(const char* format, ...) {
    va_list args;
    va_start(args, format);
    std::vprintf(format, args);
    va_end(args);
    std::putchar('\n');
}
}

using namespace BbEngine::FrameRate;

namespace {
int failures = 0;
void Check(bool ok, const char* what) {
    if (!ok) { std::printf("FAIL: %s\n", what); ++failures; }
}

float FloatAt(const std::uint8_t* image, std::uint32_t offset) {
    float f;
    std::memcpy(&f, image + offset, 4);
    return f;
}
std::int32_t IntAt(const std::uint8_t* image, std::uint32_t offset) {
    std::int32_t i;
    std::memcpy(&i, image + offset, 4);
    return i;
}

void SetFloat(std::uint8_t* image, std::uint32_t offset, float f) { std::memcpy(image + offset, &f, 4); }

std::uint8_t* Image(std::size_t size) {
#ifdef _WIN32
    auto* image = static_cast<std::uint8_t*>(_aligned_malloc(size, 0x1000));
#else
    auto* image = static_cast<std::uint8_t*>(std::aligned_alloc(0x1000, size));
#endif
    std::memset(image, 0, size);
    std::memcpy(image + kFrameTimeCave, kFrameTimeCaveCode, sizeof kFrameTimeCaveCode);
    for (const Site& s : kSites) {
        if (s.checked) std::memcpy(image + s.offset - 4, s.prefix, 4);
        const std::uint32_t bits = Value(s.kind, 60.0f);
        std::memcpy(image + s.offset, &bits, 4);
    }
    SetFloat(image, kSlopeSites[0].offset, 1.0f);
    SetFloat(image, kSlopeSites[1].offset, 0.65f);
    return image;
}

const Site* First(SiteKind kind, bool code) {
    for (const Site& s : kSites)
        if (&s != &kSites[0] && s.kind == kind && s.code == code) return &s;
    return nullptr;
}
}

int main() {
    const std::size_t size = 0x5400000;
    Check(std::fabs(BbEngine::FrameRate::HeldRate()) < 1e-6f, "nothing held before install");
    std::uint8_t* blank = static_cast<std::uint8_t*>(std::calloc(size, 1));
    Check(!BbEngine::FrameRate::Install(blank, size), "no frame-rate list: not installed");
    std::free(blank);

    std::uint8_t* image = Image(size);
    const Site* code_step = First(SiteKind::Step, true);
    const Site* data_step = First(SiteKind::Step, false);
    const Site* sixth = First(SiteKind::CountSixth, true);
    const Site* ease = First(SiteKind::EaseDistance, false);
    Check(code_step && data_step && sixth && ease, "the table has code steps, data steps, a count and an ease");
    std::memset(image + kSites[0].offset - 4, 0xcc, kSites[0].checked ? 4 : 0);

#ifdef _WIN32
    _putenv_s("BB_FPS", "120");
    _putenv_s("LANTERN_FRAMERATE_SLOPE_TICK", "1");
#else
    setenv("BB_FPS", "120", 1);
    setenv("LANTERN_FRAMERATE_SLOPE_TICK", "1", 1);
#endif
    Check(BbEngine::FrameRate::Install(image, size), "installed over a 60 FPS list");
    Check(std::fabs(FloatAt(image, code_step->offset) - 1.0f / 120) < 1e-7f, "code step at 1/120");
    Check(std::fabs(FloatAt(image, data_step->offset) - 1.0f / 120) < 1e-7f, "data step at 1/120");
    Check(IntAt(image, sixth->offset) == 20, "a sixth of a second is 20 frames at 120");
    Check(std::fabs(FloatAt(image, ease->offset) - float(1.0 - std::pow(0.9, 0.25))) < 1e-6f, "distance ease for 120");
    if (kSites[0].checked)
        Check(std::fabs(FloatAt(image, kSites[0].offset) - 1.0f / 60) < 1e-7f, "a site whose code differs is left");
    Check(std::fabs(FloatAt(image, kSlopeSites[0].offset) - 0.25f) < 1e-6f, "steep-ground slide gain a quarter until the first tick");
    Check(std::fabs(FloatAt(image, kSlopeSites[1].offset) - float(std::pow(0.65, 0.25))) < 1e-6f,
          "steep-ground slide decay 0.65^(1/4) at 120");

    SetFloat(image, kFrameTime, 1.0f / 120);
    int gains = 0, zeros = 0;
    for (int i = 0; i < 120; ++i) {
        BbEngine::FrameRate::Tick();
        const float gain = FloatAt(image, kSlopeSites[0].offset);
        gains += gain == 1.0f;
        zeros += gain == 0.0f;
    }
    Check(gains == 30 && zeros == 90, "at 120 the slide builds the game's 1.0 on 30 frames a second, 0 between");

    SetFloat(image, kFrameTime, 1.0f / 60);
    BbEngine::FrameRate::Tick();
    Check(std::fabs(FloatAt(image, data_step->offset) - 1.0f / 60) < 1e-7f, "data step follows the frame time at once");
    gains = 0;
    for (int i = 0; i < 60; ++i) {
        BbEngine::FrameRate::Tick();
        gains += FloatAt(image, kSlopeSites[0].offset) == 1.0f;
    }
    Check(gains == 30, "at 60 the slide builds on every second frame");
    Check(std::fabs(FloatAt(image, kSlopeSites[1].offset) - float(std::pow(0.65, 0.5))) < 1e-6f, "slide decay follows it");
    Check(std::fabs(FloatAt(image, code_step->offset) - 1.0f / 120) < 1e-7f, "code step waits for a held rate");
    for (int i = 0; i < 400; ++i) BbEngine::FrameRate::Tick();
    Check(std::fabs(FloatAt(image, code_step->offset) - 1.0f / 60) < 1e-6f, "code step follows a held 60");
    Check(IntAt(image, sixth->offset) == 10, "the count follows a held 60");
    Check(std::fabs(BbEngine::FrameRate::HeldRate() - 60.0f) < 1.0f, "held rate is 60");

    SetFloat(image, kFrameTime, 1.0f / 20);
    for (int i = 0; i < 400; ++i) BbEngine::FrameRate::Tick();
    Check(std::fabs(FloatAt(image, data_step->offset) - 1.0f / 30) < 1e-7f, "no step longer than 1/30");
    Check(FloatAt(image, kSlopeSites[0].offset) == 1.0f && FloatAt(image, kSlopeSites[1].offset) == 0.65f,
          "at 30 the slide is the game's own 1.0 and 0.65");

    SetFloat(image, kFrameTime, 1.0f / 240);
    for (int i = 0; i < 400; ++i) BbEngine::FrameRate::Tick();
    Check(std::fabs(FloatAt(image, code_step->offset) - 1.0f / 120) < 1e-6f, "never past the target");

    std::uint8_t* fresh = static_cast<std::uint8_t*>(std::calloc(size, 1));
    for (const ListWrite& w : kListWrites)
        if (w.list == Ninety || w.list == Sprint) std::memcpy(fresh + w.offset, kListBlob + w.at, w.size);
    g_target = 120.0f;
    g_uncapped = false;
    ApplyLists(fresh, size);
    bool all = true;
    for (const ListWrite& w : kListWrites)
        if ((w.list == Ninety || w.list == Sprint) && std::memcmp(fresh + w.offset, kListBlob + w.at + w.size, w.size) != 0)
            all = false;
    Check(all, "the engine writes the 90 FPS code and the sprint fix over the game's own bytes");
    Check(std::memcmp(fresh + kFrameTimeCave, kFrameTimeCaveCode, sizeof kFrameTimeCaveCode) == 0,
          "the frame-time code is in place after the engine's writes");
    std::free(fresh);

    std::uint8_t* sixty = static_cast<std::uint8_t*>(std::calloc(size, 1));
    int wall_writes = 0;
    for (const ListWrite& w : kListWrites)
        if (w.list == Sprint) { std::memcpy(sixty + w.offset, kListBlob + w.at, w.size); ++wall_writes; }
    g_target = 60.0f;
    Check(wall_writes > 0 && ApplyWallSpeed(sixty, size) == wall_writes, "the wall check is written at 60 FPS");
    bool wall = true;
    for (const ListWrite& w : kListWrites)
        if (w.list == Sprint && std::memcmp(sixty + w.offset, kListBlob + w.at + w.size, w.size) != 0) wall = false;
    Check(wall, "the wall check divides by the frame time at 60 FPS");
    Check(ApplyWallSpeed(sixty, size) == 0, "the wall check is written once");
    std::free(sixty);

    std::uint8_t* world = static_cast<std::uint8_t*>(std::calloc(size, 1));
    const std::uint8_t* game_room = GameBytes(kWorldCave, kWorldCaveSize);
    Check(game_room != nullptr, "the game's bytes of the world step's spare room are known");
    if (game_room) {
        std::memcpy(world + kWorldCave, game_room, kWorldCaveSize);
        std::memcpy(world + kWorldStep, kWorldStepCode, sizeof kWorldStepCode);
        SetFloat(world, kWorldStep + 3, 1.0f / 120);
        std::memcpy(world + kWorldLockFail, kWorldLockFailCode, sizeof kWorldLockFailCode);
        Check(!ApplyWorldStep(world, size), "no world step without the game's longest step");
        SetFloat(world, kLongestStep, 1.0f / 30);
        Check(ApplyWorldStep(world, size), "the world step is written over the game's code");
        auto rel = [&](std::size_t at) {
            std::int32_t r;
            std::memcpy(&r, world + at, 4);
            return std::size_t(std::int64_t(at) + 4 + r);
        };
        Check(world[kWorldStep] == 0xe9 && rel(kWorldStep + 1) == kWorldCave, "the world step jumps to its room");
        Check(rel(kWorldCave + 3) == kFlipper, "the room reads the flipper");
        Check(world[kWorldCave + 7] == 0xc5 && world[kWorldCave + 11] == 0x64 && world[kWorldCave + 12] == 0x02,
              "the room loads the measured frame time");
        Check(world[kWorldCave + 19] == 0x18, "the room keeps the frame's shortest step");
        Check(rel(kWorldCave + 24) == kLongestStep, "the room keeps the game's longest step");
        Check(world[kWorldCave + 28] == 0xc5 && world[kWorldCave + 32] == 0xb8, "the room stores the world's step");
        Check(world[kWorldCave + 33] == 0xe9 && rel(kWorldCave + 34) == kWorldStepNext, "the room returns after the step");
        Check(std::memcmp(world + kWorldLockFail, kNop6, sizeof kNop6) == 0, "the lock failure no longer reaches the room");
        Check(ApplyWorldStep(world, size), "a written world step is accepted again");
        world[kWorldCave + 40] = 0x90;
        std::memcpy(world + kWorldStep, kWorldStepCode, sizeof kWorldStepCode);
        Check(!ApplyWorldStep(world, size), "a world step room that is not the game's is left");
    }
    std::free(world);

    if (failures) return 1;
    std::puts("frame rate: ok");
    return 0;
}
