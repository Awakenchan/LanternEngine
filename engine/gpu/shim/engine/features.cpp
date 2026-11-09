// SPDX-License-Identifier: GPL-3.0-or-later
#include "gx_objects.h"
#include "gx_records.h"
#include "gx_draws.h"
#include "gx_trace.h"
#include "engine_hooks.h"
#include "camera.h"
#include "change_appearance.h"
#include "cheats.h"
#include "effect_timing.h"
#include "ground_trace.h"
#include "engine_state.h"
#include "camera_timing.h"
#include "frame_rate.h"
#include "game_status.h"
#include "job_slots.h"
#include "graphics.h"
#include "loading.h"
#include "menu_memory.h"
#include "online_fixes.h"
#include "option_menu.h"
#include "rebirth.h"
#include "render_guard.h"
#include "trophy_sync.h"
#include "widescreen.h"
#include "../input/input.h"
#include "../face_shot.h"
#include "bbport_settings.h"
#include <array>
#include <cstdlib>
#include <cstring>
#include <filesystem>

extern "C" int sys_file_translate(const char*, char*, std::size_t);
extern "C" int sys_file_mount(const char*, const char*);
extern "C" void sys_file_unmount(const char*);

namespace BbEngine {
namespace {
constexpr char kVerifiedImage[] = "071df19c8880086d97182dbc057bc8cb37badaca57d9112683836b24a0444c0a";

constexpr std::size_t kStepConstructor = 0x1c1cce0;
constexpr std::uint8_t kStepEntry[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56,
                                       0x53, 0x50, 0x4d, 0x89, 0xce, 0x48, 0x89, 0xfb};
constexpr std::size_t kPacerUpdate = 0x2034770;
constexpr std::uint8_t kPacerEntry[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41,
                                        0x55, 0x41, 0x54, 0x53, 0x48, 0x83, 0xec, 0x38};

constexpr char kDreamLayout[] = "/app0/dvdroot_ps4/map/mapstudio/m21_00_00_00.msb.dcx";
constexpr char kDreamLayoutFile[] = "dvdroot_ps4/map/mapstudio/m21_00_00_00.msb.dcx";

bool Seamless() {
    static const bool on = [] {
        const char* v = std::getenv("BB_NET_BACKEND");
        return v && std::strcmp(v, "room") == 0;
    }();
    return on;
}

std::int64_t __attribute__((sysv_abi)) OnStep(std::uint64_t, const std::uint64_t* saved) {
    menu_steps_notify(saved);
    return 0;
}

std::int64_t __attribute__((sysv_abi)) OnFrame(std::uint64_t, const std::uint64_t* saved) {
    FrameRate::Tick();
    Loading::Tick(saved[5]);
    ChangeAppearance::Tick();
    BbFaceShot::Tick(ChangeAppearance::PreviewShown() && Seamless());
    Rebirth::Tick();
    Options::Tick();
    BbInput::Tick();
    Camera::Tick();
    Widescreen::Tick();
    Cheats::Tick();
    TrophySync::Tick();
    GameStatus::Tick();
    OnlineFixes::Tick();
    return 0;
}

bool Set(const char* name) {
    const char* v = std::getenv(name);
    return v && *v;
}

bool DreamLayoutIsTheGames() {
    const char* game = std::getenv("BB_GAME_DIR");
    char resolved[512]{};
    std::error_code error;
    return game && !sys_file_translate(kDreamLayout, resolved, sizeof resolved) &&
           std::filesystem::equivalent(resolved, std::filesystem::path(game) / kDreamLayoutFile, error);
}
}

void Install(std::uint8_t* image, std::size_t size) {
    const auto& s = BbSettings::Get();
    const char* mirror_asset = std::getenv("BB_DREAM_MIRROR_ASSET");
    const char* identity = std::getenv("BB_ENGINE_IMAGE_SHA256");
    const bool mirror = mirror_asset && *mirror_asset;
    if (!identity && !mirror && !Set("BB_REBIRTH_ASSET") && !s.camera_controls && !s.graphics_controls &&
        !s.pc_controls && !s.widescreen)
        return;
    if (!identity || std::strcmp(identity, kVerifiedImage) != 0) {
        host_log("engine features off: the executable was not verified");
        return;
    }
    if (mirror && !DreamLayoutIsTheGames()) {
        host_log("mirror off: a mod replaces the Dream's layout");
        mirror_asset = nullptr;
    }
    std::array<Hook, 2> hooks;
    if (!Prepare(hooks[0], image, size, kStepConstructor, kStepEntry, OnStep) ||
        !Prepare(hooks[1], image, size, kPacerUpdate, kPacerEntry, OnFrame)) {
        for (auto& h : hooks) Discard(h);
        host_log("engine features off: the game's code is not 1.09's (or no memory for the hooks)");
        return;
    }
    Loading::Install(image);
    StateInitialize(reinterpret_cast<std::uint64_t>(image));

    std::error_code error;
    const bool mounted = mirror_asset && std::filesystem::is_regular_file(mirror_asset, error) && !error &&
                         !sys_file_mount(kDreamLayout, mirror_asset);
    const bool input = BbInput::Install(image, size);
    const bool camera = Camera::Install(image, size);
    const bool wide = Widescreen::Install(image, size);
    const bool graphics = Graphics::Install(image, size);
    const bool local_shadows = Graphics::InstallLocalShadows(image, size);
    const bool spot_shadows = Graphics::InstallSpotShadows(image, size);
    Graphics::InstallLocalShadowSoftness(image, size, local_shadows, spot_shadows);
    const bool directional_shadows = Graphics::InstallDirectionalShadows(image, size);
    const bool directional_resolution = Graphics::InstallDirectionalShadowResolution(image, size);
    const bool menus = Options::Install(image, size);
    bool editor = false;
    const bool list = ChangeAppearance::Watch(image);
    if (mounted && (menus || MenuMemory::Install(image, size)) && list) {
        ChangeAppearance::EnableEditor();
        editor = true;
        host_log("Dream mirror enabled: native appearance editor and ChrMake_BG preview; %s", mirror_asset);
    } else if (mounted) {
        sys_file_unmount(kDreamLayout);
        host_log("mirror off: the menu heaps are not 1.09's");
    }
    const bool rebirth = menus && Rebirth::Install(image, size);
    const bool cheats = Cheats::Install(image, size);
    const bool online = OnlineFixes::Install(image, size);
    const bool guard = RenderGuard::Install(image, size);
    const bool timing = EffectTiming::Install(image, size);
    const bool ground_trace = GroundTrace::Install(image, size);
    const bool frame_rate = FrameRate::Install(image, size);
    CameraTiming::Install(image, size);
    const bool slots = JobSlots::Install(image, size);
    const bool status = GameStatus::Install(image, size);
    GxObjects::Install(image, size);
    GxTrace::Install(image, size);
    GxDraws::Install(image, size);
    GxRecords::Install(image, size);
    if (!(editor || input || camera || wide || graphics || local_shadows || spot_shadows ||
          directional_shadows || directional_resolution || menus || rebirth || cheats || online || guard || timing || ground_trace ||
          frame_rate || slots || status)) {
        for (auto& h : hooks) Discard(h);
        host_log("engine features off: none could be prepared");
        return;
    }
    Commit(hooks);
}
}
