// SPDX-License-Identifier: GPL-3.0-or-later
#include "gx_objects.h"
#include "hook_entries.h"
#include "gx_records.h"
#include "gx_owed_check.h"
#include "gx_whole_route.h"
#include "gx_draws.h"
#include "gx_trace.h"
#include "gx_whole_hooks.h"
#include "engine_hooks.h"
#include "camera.h"
#include "change_appearance.h"
#include "cheats.h"
#include "effect_timing.h"
#include "ground_trace.h"
#include "engine_state.h"
#include "camera_timing.h"
#include "frame_rate.h"
#include "../lantern_ab_flip.h"
#include "../lantern_meter.h"
#include "game_status.h"
#include "job_slots.h"
#include "main_jobs.h"
#include "main_jobs_barrier_hook.h"
#include "graphics.h"
#include "loading.h"
#include "light_list.h"
#include "menu_memory.h"
#include "online_fixes.h"
#include "option_menu.h"
#include "rebirth.h"
#include "render_guard.h"
#include "trophy_rules.h"
#include "trophy_sync.h"
#include "widescreen.h"
#include "../input/input.h"
#include "../face_shot.h"
#include "bbport_settings.h"
#include "../test_sandbox.h"
#include <array>
#include <cstdlib>
#include <cstring>
#include <filesystem>

extern "C" int sys_file_translate(const char*, char*, std::size_t);
extern "C" int sys_file_mount(const char*, const char*);
extern "C" void sys_file_unmount(const char*);
extern "C" void sys_trophy_role(const char*);

namespace BbEngine {
namespace {
void FlipOne(const char* name, std::atomic<int>* value) {
    if (!value || value->load(std::memory_order_relaxed) != 1) return;
    const Lantern::AbFlip::Registered r = Lantern::AbFlip::Register(name, value, 0);
    if (r == Lantern::AbFlip::Registered::Added) host_log("A/B flip: %s starts on and turns off at the flip", name);
    else if (r != Lantern::AbFlip::Registered::Off) host_log("A/B flip: %s could not be added, it stays on", name);
}

void FlipJobs(bool jobs_barrier) {
    FlipOne("job-lock", MainJobs::LockSwitch());
    FlipOne("job-node-cache", MainJobs::CacheSwitch());
    if (jobs_barrier) FlipOne("main-jobs-barrier", &MainJobsBarrier::Switch());
}
constexpr char kVerifiedImage[] = "071df19c8880086d97182dbc057bc8cb37badaca57d9112683836b24a0444c0a";

constexpr std::size_t kStepConstructor = 0x1c1cce0;
constexpr const auto& kStepEntry = HookEntries::kFeaturesStepEntry;
constexpr std::size_t kPacerUpdate = 0x2034770;
constexpr const auto& kPacerEntry = HookEntries::kFeaturesPacerEntry;

constexpr char kDreamLayout[] = "/app0/dvdroot_ps4/map/mapstudio/m21_00_00_00.msb.dcx";
constexpr char kDreamLayoutFile[] = "dvdroot_ps4/map/mapstudio/m21_00_00_00.msb.dcx";

bool Seamless() {
    static const bool on = [] {
        const char* v = std::getenv("BB_NET_BACKEND");
        return v && std::strcmp(v, "room") == 0;
    }();
    return on;
}

std::int64_t __attribute__((sysv_abi)) OnStep(std::uint64_t, std::uint64_t* saved) noexcept {
    menu_steps_notify(saved);
    return 0;
}

std::int64_t __attribute__((sysv_abi)) OnFrame(std::uint64_t, std::uint64_t* saved) noexcept {
    FrameRate::Tick();
    Loading::Tick(saved[5]);
    LightList::Tick();
    ChangeAppearance::Tick();
    BbFaceShot::Tick(ChangeAppearance::PreviewShown() && Seamless());
    Rebirth::Tick();
    Options::Tick();
    BbInput::Tick();
    Camera::Tick();
    Widescreen::Tick();
    Cheats::Tick();
    TrophySync::Tick();
    sys_trophy_role(TrophyRules::RoleName(OnlineFixes::PartyRole()));
    GameStatus::Tick();
    OnlineFixes::Tick();
    MainJobs::Tick();
    MainJobsBarrier::Tick();
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
    LanternSandbox::TheSwitches();
    struct InstallEnd {
        ~InstallEnd() { EndInstall(); }
    } install_end;
    const auto& s = BbSettings::Get();
    const char* mirror_asset = std::getenv("BB_DREAM_MIRROR_ASSET");
    const char* identity = std::getenv("BB_ENGINE_IMAGE_SHA256");
    const bool mirror = mirror_asset && *mirror_asset;
    if (!identity && !mirror && !Set("BB_REBIRTH_ASSET") && !s.camera_controls && !s.graphics_controls &&
        !s.pc_controls && !s.widescreen)
        return;
    if (!identity || std::strcmp(identity, kVerifiedImage) != 0) {
        host_log(identity ? "engine features off (widescreen, Lantern menus, camera controls, native frame rate): the "
                            "game's executable is not the 1.09 one this engine knows; a patched or other version"
                          : "engine features off (widescreen, Lantern menus, camera controls, native frame rate): the "
                            "launcher did not pass the executable's identity (BB_ENGINE_IMAGE_SHA256)");
        return;
    }
    if (mirror && !DreamLayoutIsTheGames()) {
        host_log("mirror off: a mod replaces the Dream's layout");
        mirror_asset = nullptr;
    }
    std::array<Hook, 2> hooks;
    if (!Prepare(hooks[0], image, size, kStepConstructor, SiteKind::Entry, kStepEntry, OnStep) ||
        !Prepare(hooks[1], image, size, kPacerUpdate, SiteKind::Entry, kPacerEntry, OnFrame)) {
        for (auto& h : hooks) Discard(h);
        host_log("engine features off: the game's code is not 1.09's (or no memory for the hooks)");
        return;
    }
    if (const WriteResult result = Commit(hooks); result != WriteResult::Applied) {
        host_log("engine features off: the game's frame and step hooks %s", Describe(result));
        return;
    }
    Loading::Install(image);
    LightList::Install(image);
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
    (void)Lantern::Meter::NoteGuestImage(image, size);
    CameraTiming::Install(image, size);
    const bool slots = JobSlots::Install(image, size);
    MainJobs::Install(image, size);
    const bool jobs_barrier = MainJobsBarrier::Install(image, size) == MainJobsBarrier::Installed::Yes;
    FlipJobs(jobs_barrier);
    const bool status = GameStatus::Install(image, size);
    GxObjects::Install(image, size);
    GxTrace::Install(image, size);
    GxDraws::Install(image, size);
    GxRecords::Install(image, size);
    GxWhole::OwedCheck::Start(GxRecords::WholeMode());
    GxWhole::Hooks::Install(image, size, GxRecords::WholeRequested(), GxRecords::WholeMode());
    GxWholeRoute::Start(GxRecords::WholeMode(), GxRecords::WholeCounting());
    if (!(editor || input || camera || wide || graphics || local_shadows || spot_shadows ||
          directional_shadows || directional_resolution || menus || rebirth || cheats || online || guard || timing || ground_trace ||
          frame_rate || slots || jobs_barrier || status)) {
        host_log("engine features off: none could be prepared");
    }
}
}
