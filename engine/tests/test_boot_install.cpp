// SPDX-License-Identifier: GPL-3.0-or-later
#include "gpu/shim/engine/engine_hooks.h"
#include "gpu/shim/engine/cheats.h"
#include "gpu/shim/engine/frame_rate.h"
#include "gpu/shim/engine/gx_objects.h"
#include "gpu/shim/engine/gx_records.h"
#include "gpu/shim/engine/gx_whole_hook_sites.h"
#include "gpu/shim/engine/loading.h"
#include "gpu/shim/engine/hook_entries.h"
#include "gpu/shim/engine/main_jobs.h"
#include "gpu/shim/engine/main_jobs_barrier.h"
#include "gpu/shim/engine/main_jobs_barrier_hook.h"
#include "gpu/shim/lantern_steps.h"
#include "gpu/shim/input/mouse_camera.h"
#include "gpu/shim/bbport_settings.h"
#include "gpu/shim/test_sandbox.h"
#include "tools/engine/sha256.h"
#include "tests/eboot_image.h"
#include <SDL3/SDL.h>
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <span>
#include <string>
#include <thread>
#include <vector>
#ifdef _WIN32
#include <io.h>
#include <process.h>
#include <windows.h>
#else
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;
using EbootImage::MapImage;
using EbootImage::Relocate;
using EbootImage::kEbootSha;

extern "C" int sys_file_mount(const char*, const char*) { return 0; }
extern "C" void sys_file_unmount(const char*) {}
extern "C" [[noreturn]] void runtime_thread_exit(void*) { std::abort(); }
extern "C" void runtime_blocked_report() {}
extern "C" int sys_pad_type() { return 0; }
extern "C" int sys_pad_button(std::uint32_t, int) { return 0; }
extern "C" int sys_file_translate(const char* guest, char* out, std::size_t size) {
    const char* game = std::getenv("BB_GAME_DIR");
    constexpr const char* kApp = "/app0/";
    if (!game || !guest || std::strncmp(guest, kApp, std::strlen(kApp)) != 0) return -1;
    const std::string host = (fs::path(game) / (guest + std::strlen(kApp))).string();
    if (host.size() + 1 > size) return -1;
    std::memcpy(out, host.c_str(), host.size() + 1);
    return 0;
}

namespace {
constexpr const char* kTest = "boot-install-test";
constexpr const char* kImageSwitch = "BB_BOOT_INSTALL_IMAGE";
constexpr const char* kAssetsSwitch = "BB_BOOT_INSTALL_ASSETS";
constexpr const char* kOnlySwitch = "BB_BOOT_INSTALL_ONLY";
constexpr const char* kMutantSwitch = "BB_BOOT_INSTALL_MUTANT";
constexpr int kSkipCode = 77;
constexpr const char* kImageSha = "071df19c8880086d97182dbc057bc8cb37badaca57d9112683836b24a0444c0a";
constexpr std::size_t kKeepEchoes = 0x13d2bd2;
constexpr std::uint8_t kKeepEchoesCode[] = {0x45, 0x2b, 0xa5, 0x94, 0x00, 0x00, 0x00, 0x44,
                                            0x89, 0x63, 0x38, 0x0f, 0x1f, 0x40, 0x00};
constexpr std::size_t kWorldStep = 0x17f9b71;
constexpr std::uint8_t kWorldJump[] = {0xe9, 0x33, 0x01, 0x00, 0x00, 0x66, 0x90};

constexpr std::size_t kLoadAsked = 0x556286b;
constexpr std::size_t kLoadClearedSlots[] = {0x553e8b8, 0x5540058};
constexpr std::size_t kPacerTargets[] = {0x2034845, 0x203485f, 0x2034883, 0x20348b8};
constexpr float kLoadingTarget = 1.0f / 30.0f;
constexpr std::size_t kPacerBytes = 0x40;

constexpr std::size_t kCameraUpdate = 0x143ac60;
constexpr std::uint8_t kAbsoluteJump[] = {0xff, 0x25, 0x00, 0x00, 0x00, 0x00};
constexpr std::uint8_t kMovRax[] = {0x48, 0xb8};
constexpr std::uint8_t kCallRax[] = {0xff, 0xd0};
constexpr std::size_t kStubScan = 256;
constexpr std::size_t kSavedSlots = 40, kSavedAt = 4;
struct AutoRotation {
    std::size_t at;
    std::array<std::uint8_t, 9> code;
    bool held;
};
constexpr AutoRotation kAutoRotation[] = {
    {0x143c6e8, {0xc4, 0xc1, 0x7a, 0x11, 0x85, 0x44, 0x01, 0x00, 0x00}, true},
    {0x143c870, {0xc4, 0xc1, 0x7a, 0x11, 0xad, 0x44, 0x01, 0x00, 0x00}, false},
    {0x143c984, {0xc4, 0xc1, 0x7a, 0x11, 0x8d, 0x30, 0x01, 0x00, 0x00}, true},
    {0x143dde6, {0xc4, 0xc1, 0x7a, 0x11, 0x85, 0x94, 0x02, 0x00, 0x00}, true},
};
constexpr std::uint8_t kNop = 0x90;

struct Rate {
    const char* fps;
    const char* limit;
    const char* lists;
    const char* native;
};
constexpr Rate k120{"120", "120",
                    "Engine: Frame rate: the engine wrote the frame-rate code for 120 FPS (181 writes, 1 already in place, 0 left to "
                    "another patch)",
                    "Engine: Frame rate: native at 120 FPS: 61 code, 25 data, 7 target and 5 fixed sites (0 left as they were)"};
constexpr Rate k60{"60", "60",
                   "Engine: Frame rate: the engine wrote the frame-rate code for 60 FPS (158 writes, 1 already in place, 0 left to "
                   "another patch)",
                   "Engine: Frame rate: native at 60 FPS: 60 code, 25 data, 7 target and 5 fixed sites (1 left as they were)"};
constexpr Rate k90{"90", "90",
                   "Engine: Frame rate: the engine wrote the frame-rate code for 90 FPS (181 writes, 1 already in place, 0 left to "
                   "another patch)",
                   "Engine: Frame rate: native at 90 FPS: 61 code, 25 data, 7 target and 5 fixed sites (0 left as they were)"};
constexpr Rate k144{"144", "144",
                    "Engine: Frame rate: the engine wrote the frame-rate code for 144 FPS (181 writes, 1 already in place, 0 left to "
                    "another patch)",
                    "Engine: Frame rate: native at 144 FPS: 61 code, 25 data, 7 target and 5 fixed sites (0 left as they were)"};
constexpr Rate kUncapped{"uncap", nullptr,
                         "Engine: Frame rate: the engine wrote the frame-rate code for 240 FPS uncapped (198 writes, 1 already in "
                         "place, 0 left to another patch)",
                         "Engine: Frame rate: native at 240 FPS (uncapped limit): 60 code, 25 data, 0 target and 4 fixed sites (6 "
                         "left as they were)"};

enum class Net { Offline, Thd, Room };
enum class GxStack : std::uint8_t { None, Whole, WholeNoRecords, WholeBindsOff };
struct Variant {
    const char* name;
    Net net;
    const Rate* rate;
    GxStack gx = GxStack::None;
    bool jobs = false;
    bool jobs_barrier = false;
};
constexpr Variant kVariants[] = {
    {"offline", Net::Offline, &k120},      {"online-thd", Net::Thd, &k120}, {"online-room", Net::Room, &k120},
    {"offline-60", Net::Offline, &k60},    {"offline-90", Net::Offline, &k90}, {"offline-144", Net::Offline, &k144},
    {"offline-uncapped", Net::Offline, &kUncapped},
    {"offline-gx-whole", Net::Offline, &k120, GxStack::Whole},
    {"offline-gx-whole-no-records", Net::Offline, &k120, GxStack::WholeNoRecords},
    {"offline-gx-whole-binds-off", Net::Offline, &k120, GxStack::WholeBindsOff},
    {"offline-jobs", Net::Offline, &k120, GxStack::None, true},
    {"offline-main-jobs-barrier", Net::Offline, &k120, GxStack::None, false, true},
};

constexpr const char* kJobsBarrierSwitch = "LANTERN_MAIN_JOBS_BARRIER";
constexpr const char* kJobsBarrierLine = "Engine: Main jobs barrier: on, the main thread takes the GX workers' queued jobs";
constexpr std::size_t kJobsBarrierStep = 0x21b9980;
constexpr std::size_t kStackGuardSlot = 0x53e3be0;
constexpr std::size_t kReadTscSlot = 0x53e5b28;
constexpr std::size_t kJobsCodePages[][2] = {{0xe47000, 0xe48000}, {0x21b9000, 0x21ba000}, {0x2bc0000, 0x2bc1000}, {0x2929000, 0x292a000}};

const std::vector<const char*> kGxWhole = {
    "Engine: GX whole hooks: on, 6 GX draw flushes hooked at start (LANTERN_GX_WHOLE=1)",
    "Engine: GX objects: on",
    "Engine: GX records: on",
    "Engine: GX whole: the record carries every field a covered draw reads",
    "Engine: GX objects: static GX buffers are sealed, watched only, for the whole draw's vertex and index binds (LANTERN_GX_WHOLE=1)",
};
constexpr const char* kSealedLine = "Engine: GX objects: static GX buffers are sealed";
constexpr const char* kVersionedLine =
    "Engine: GX records: buffers versioned by GX's updates for the whole draw's vertex and index binds (LANTERN_GX_WHOLE=1), still bound by address";
constexpr const char* kByObjectLine = "vertex and index buffers bound by object";

constexpr const char* kBindsOffLine =
    "Engine: GX whole binds off (LANTERN_GX_BINDS=0): no seals and no versions for the whole draw's vertex and index binds";

constexpr const char* kGxSwitches[] = {"LANTERN_GX_WHOLE", "LANTERN_GX_DRAWS", "LANTERN_GX_RECORDS", "LANTERN_GX_BINDS"};
constexpr const char* kGxOn[] = {"1", "1", "1", nullptr};
constexpr const char* kGxNoRecords[] = {"1", "1", nullptr, nullptr};
constexpr const char* kGxBindsOff[] = {"1", "1", "1", "0"};

const std::vector<const char*> kEngine = {
    "Input: keyboard and mouse play on, the mouse camera ready",
    "Engine: camera adjustments enabled (distance, height and FOV)",
    "Engine: widescreen enabled: native camera aspect and 15 Scaleform stage pins",
    "Engine: picture tuning ready",
    "Engine: Job slots: a Havok job that retries or ends returns its thread-memory slot (cutscene freezes)",
    "Engine: Game status: area, boss, level, Blood Echoes and Insight for the launcher's Discord status",
    "Loading: fast loading on (loading screens paced at 30 FPS)",
    "Engine: Frame rate: the steep-ground slide follows the frame time (2 of 2 values)",
};

const std::vector<const char*> kCheats = {
    "Cheats: keep Blood Echoes on death on",
    "Cheats: stealth on (Player Hide, Player Silence)",
    "Cheats: weapon durability kept full on",
};

const std::vector<const char*> kMenus = {
    "Engine: Scaleform menu heaps 50/98 MiB, MENU 178 MiB (native option pages)",
    "Engine: pc-options: native System pages installed (",
    "Engine: Dream mirror enabled: native appearance editor and ChrMake_BG preview",
};

const std::vector<const char*> kBellRules = {
    "the bell's X is gone in every area and on every floor",
    "bells after the area boss is dead",
    "the party stays after a boss",
    "faster summons and joins",
    "bell signs go up 0.5 s after the bell",
    "multiplayer messages hold 1.5 s",
    "bells without Insight",
    "a guest gets a kill's full blood echoes",
    "no Insight or echoes for another hunter's death or leaving",
    "a guest gets the host's Insight for a boss, not the cooperator's 1",
};

const std::vector<const char*> kRoom = {
    "Engine: online: seamless: up to 5 players in one world (host and 4 cooperators)",
    "Engine: online: seamless: bosses scale with the game's own rows for 3 and 4 cooperators",
    "Engine: online: seamless: deaths keep the party: ",
    "Engine: online: seamless: lamps work in a party: ",
    "Engine: online: seamless: each hunter has its own loot, drops and NPC progress in the host's world",
    "Engine: online: seamless: shared progress: ",
    "Engine: online: seamless: chalice dungeons with the party: ",
    "Engine: online: seamless: boss rewards: every hunter in the host's world gets a boss's drop once",
    "Engine: online: seamless: the game's multiplayer messages (bells, summons, joins) left out",
    "Engine: online: seamless: a guest's copy of the host's world follows the host's flags",
    "Voice: off (test sandbox)",
};

const char* const kRefused[] = {
    "Engine: code at image offset",
    "Engine: game code writes could not all be undone",
    "more code sites failed",
    "unavailable",
    "nothing written",
    "native frame rate off",
    "engine features off",
    "is not the game's code",
    "is not in place",
    "pc-options off",
    "mirror off",
    "fast loading off",
    "auto-rotation stays on",
    " left off",
    " left alone",
    "stay the game's",
    "left as the game has them",
    "messages kept",
    "spectator camera is off",
    "fix off",
    "Voice: microphone",
    "Voice: no microphone",
    " refused (",
    "binds to loopback",
    "GX whole hooks off: GX's",
    "GX objects off",
    "GX draws off",
    "GX records off",
};

int failures = 0;
void Fail(const std::string& what) {
    std::printf("FAIL: %s\n", what.c_str());
    ++failures;
}

std::string Hex(std::size_t at) {
    char text[32];
    std::snprintf(text, sizeof text, "%#zx", at);
    return text;
}

void SetEnv(const char* key, const char* value) {
#ifdef _WIN32
    _putenv_s(key, value ? value : "");
#else
    if (value) setenv(key, value, 1);
    else unsetenv(key);
#endif
}

bool Mutant(const char* name) {
    const char* mutant = std::getenv(kMutantSwitch);
    return mutant && !std::strcmp(mutant, name);
}

class Capture {
public:
    explicit Capture(const fs::path& path) : path_(path) {
        std::fflush(stdout);
#ifdef _WIN32
        saved_ = _dup(_fileno(stdout));
#else
        saved_ = dup(fileno(stdout));
#endif
        file_ = std::fopen(path.string().c_str(), "wb");
        if (saved_ < 0 || !file_) return;
#ifdef _WIN32
        ok_ = _dup2(_fileno(file_), _fileno(stdout)) == 0;
#else
        ok_ = dup2(fileno(file_), fileno(stdout)) >= 0;
#endif
    }
    bool Ok() const { return ok_; }
    std::vector<std::string> Lines() {
        std::fflush(stdout);
        if (saved_ >= 0) {
#ifdef _WIN32
            _dup2(saved_, _fileno(stdout));
            _close(saved_);
#else
            dup2(saved_, fileno(stdout));
            close(saved_);
#endif
            saved_ = -1;
        }
        if (file_) std::fclose(file_);
        file_ = nullptr;
        std::vector<std::string> lines;
        std::ifstream in(path_, std::ios::binary);
        for (std::string line; std::getline(in, line);) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            lines.push_back(line);
        }
        return lines;
    }
    ~Capture() { Lines(); }

private:
    fs::path path_;
    FILE* file_ = nullptr;
    int saved_ = -1;
    bool ok_ = false;
};

bool Has(const std::vector<std::string>& lines, const std::string& text) {
    return std::any_of(lines.begin(), lines.end(), [&](const std::string& l) { return l.find(text) != std::string::npos; });
}

bool HasLine(const std::vector<std::string>& lines, const std::string& text) {
    return std::find(lines.begin(), lines.end(), text) != lines.end();
}

void WriteSettings(const fs::path& ini, bool menus) {
    std::ofstream out(ini, std::ios::binary);
    out << "camera_controls=1\ngraphics_controls=0\npc_controls=1\nwidescreen=1\nmouse_camera=1\nmouse_menu=1\n"
        << "change_appearance=" << (menus ? 1 : 0) << "\n"
        << "rebirth=0\nonline_enabled=0\nonline_connected=0\noutput_res=1920x1080\n"
           "cheat_vials=infinite\ncheat_bullets=infinite\ncheat_stamina=0\ncheat_stealth=1\ncheat_gun_range=off\n"
           "cheat_keep_echoes=1\ncheat_echo_multiplier=1\ncheat_no_durability=1\ncheat_consumables=0\n";
}

void Bytes(std::uint8_t* to, const void* from, std::size_t n) { std::memcpy(to, from, n); }

void DriveLoadingPace(const char* variant, std::uint8_t* image) {
    std::array<std::array<std::uint8_t, 4>, std::size(kPacerTargets)> game{};
    for (std::size_t i = 0; i < std::size(kPacerTargets); ++i) Bytes(game[i].data(), image + kPacerTargets[i], 4);
    for (const std::size_t slot : kLoadClearedSlots) std::memset(image + slot, 0, sizeof(std::uint64_t));
    alignas(16) std::array<std::uint8_t, kPacerBytes> pacer{};
    const auto pacer_at = reinterpret_cast<std::uint64_t>(pacer.data());
    image[kLoadAsked] = 1;
    BbEngine::Loading::Tick(pacer_at);
    for (const std::size_t at : kPacerTargets) {
        float now = 0;
        std::memcpy(&now, image + at, sizeof now);
        if (now != kLoadingTarget) Fail(std::string(variant) + ": the loading pace was not written at " + Hex(at));
    }
    if (Mutant("loading-restore-byte")) image[kPacerTargets[0]] ^= 0x01;
    image[kLoadAsked] = 0;
    BbEngine::Loading::Tick(pacer_at);
    for (std::size_t i = 0; i < std::size(kPacerTargets); ++i)
        if (std::memcmp(image + kPacerTargets[i], game[i].data(), 4) != 0)
            Fail(std::string(variant) + ": the loading pace was not put back at " + Hex(kPacerTargets[i]));
}

BbEngine::Callback CameraCallback(const char* variant, const std::uint8_t* image) {
    const std::uint8_t* site = image + kCameraUpdate;
    if (std::memcmp(site, kAbsoluteJump, sizeof kAbsoluteJump) != 0) {
        Fail(std::string(variant) + ": the mouse camera's hook is not a jump at " + Hex(kCameraUpdate));
        return nullptr;
    }
    std::uint64_t stub_at = 0;
    std::memcpy(&stub_at, site + sizeof kAbsoluteJump, sizeof stub_at);
    const auto* stub = reinterpret_cast<const std::uint8_t*>(stub_at);
    if (!stub) {
        Fail(std::string(variant) + ": the mouse camera's hook has no stub");
        return nullptr;
    }
    constexpr std::size_t kImm = sizeof(std::uint64_t);
    for (std::size_t i = 0; i + sizeof kMovRax + kImm + sizeof kCallRax <= kStubScan; ++i) {
        if (std::memcmp(stub + i, kMovRax, sizeof kMovRax) != 0 ||
            std::memcmp(stub + i + sizeof kMovRax + kImm, kCallRax, sizeof kCallRax) != 0)
            continue;
        std::uint64_t callback = 0;
        std::memcpy(&callback, stub + i + sizeof kMovRax, kImm);
        return reinterpret_cast<BbEngine::Callback>(callback);
    }
    Fail(std::string(variant) + ": the mouse camera's stub calls no callback");
    return nullptr;
}

void ExpectAutoRotation(const char* variant, const std::uint8_t* image, bool held, const char* what) {
    for (const AutoRotation& s : kAutoRotation) {
        std::array<std::uint8_t, 9> want = s.code;
        if (held && s.held) want.fill(kNop);
        if (std::memcmp(image + s.at, want.data(), want.size()) != 0)
            Fail(std::string(variant) + ": the auto-rotation store " + what + " at " + Hex(s.at));
    }
}

void DriveAutoRotation(const char* variant, std::uint8_t* image) {
    if (!MouseCamera::Installed()) {
        Fail(std::string(variant) + ": the mouse camera is not installed");
        return;
    }
    const BbEngine::Callback update = CameraCallback(variant, image);
    if (!update) return;
    ExpectAutoRotation(variant, image, false, "is not the game's before the hold");
    std::array<std::uint64_t, kSavedSlots> frame{};
    MouseCamera::Publish(5, false, false, true);
    update(0, frame.data() + kSavedAt);
    ExpectAutoRotation(variant, image, true, "was not held");
    if (Mutant("auto-rotation-byte")) image[kAutoRotation[0].at] = 0xcc;
    MouseCamera::Publish(5, false, false, false);
    update(0, frame.data() + kSavedAt);
    ExpectAutoRotation(variant, image, false, "was not put back");
}


namespace JobsDrive {
namespace L = BbEngine::MainJobsBarrier::Layout;
constexpr std::size_t kParticipants = 3;
constexpr std::uint64_t kDequeContainer = 0x00, kDequeMap = 0x08, kDequeMapSize = 0x10, kDequeStart = 0x18, kDequeCount = 0x20, kDequeLock = 0x38, kStealLock = 0x50;
constexpr std::uint64_t kLockAt = 0x18, kUnlockAt = 0x28;
constexpr std::uint64_t kGuard = 0x5eed5eed5eed5eedull, kTsc = 0x10001;
constexpr std::size_t kMapSize = 4, kJobs = 4, kVictim = 2;

struct Counts {
    int locks[kParticipants][2] = {};
    int runs = 0;
    std::uint32_t run_index = 0xffffffffu;
    std::uint64_t last_job = 0;
    int tsc = 0;
};
Counts g_counts;
alignas(16) std::uint8_t g_memory[64 + kParticipants * L::kParticipantBytes];
std::uint64_t g_lock_vtable[8], g_job_vtable[4], g_jobs[kJobs][1], g_blocks[kParticipants][kMapSize][2],
    g_maps[kParticipants][kMapSize], g_proxies[kParticipants];
std::uint64_t g_guard = kGuard;
std::uint64_t g_base = 0;

int LockSlot(std::uint64_t lock, std::size_t& who) {
    for (std::size_t i = 0; i < kParticipants; ++i) {
        const std::uint64_t deque = g_base + i * L::kParticipantBytes + L::kDeque;
        if (lock == deque + kDequeLock) {
            who = i;
            return 0;
        }
        if (lock == deque + kStealLock) {
            who = i;
            return 1;
        }
    }
    return -1;
}
std::int64_t __attribute__((sysv_abi)) Lock(std::uint64_t lock, std::uint32_t) {
    std::size_t who = 0;
    const int kind = LockSlot(lock, who);
    if (kind >= 0) ++g_counts.locks[who][kind];
    return 0;
}
std::int64_t __attribute__((sysv_abi)) Unlock(std::uint64_t) { return 0; }
std::int64_t __attribute__((sysv_abi)) RunJob(std::uint64_t job, std::uint32_t index) {
    ++g_counts.runs;
    g_counts.last_job = job;
    g_counts.run_index = index;
    return 0;
}
std::int64_t __attribute__((sysv_abi)) KeepWhole(std::uint64_t, std::uint64_t) { return 0; }
std::uint64_t __attribute__((sysv_abi)) ReadTsc() {
    ++g_counts.tsc;
    return kTsc;
}

void Put(std::uint64_t at, std::uint64_t v) { std::memcpy(reinterpret_cast<void*>(at), &v, sizeof v); }
std::uint64_t Get(std::uint64_t at) {
    std::uint64_t v = 0;
    std::memcpy(&v, reinterpret_cast<const void*>(at), sizeof v);
    return v;
}
std::uint64_t Participant(std::size_t i) { return g_base + i * L::kParticipantBytes; }

void Lay(bool own_job, std::size_t victim_jobs = 0) {
    std::memset(g_memory, 0, sizeof g_memory);
    g_counts = {};
    const std::uint64_t system = reinterpret_cast<std::uint64_t>(g_memory);
    g_base = system + 64;
    Put(system + L::kParticipants, g_base);
    const std::uint32_t n = kParticipants;
    std::memcpy(reinterpret_cast<void*>(system + L::kCount), &n, sizeof n);
    g_lock_vtable[kLockAt / 8] = reinterpret_cast<std::uint64_t>(&Lock);
    g_lock_vtable[kUnlockAt / 8] = reinterpret_cast<std::uint64_t>(&Unlock);
    g_job_vtable[0] = reinterpret_cast<std::uint64_t>(&RunJob);
    g_job_vtable[1] = reinterpret_cast<std::uint64_t>(&KeepWhole);
    g_job_vtable[2] = reinterpret_cast<std::uint64_t>(&KeepWhole);
    std::memset(g_blocks, 0, sizeof g_blocks);
    for (auto& j : g_jobs) j[0] = reinterpret_cast<std::uint64_t>(g_job_vtable);
    if (own_job) g_blocks[0][0][0] = reinterpret_cast<std::uint64_t>(g_jobs[0]);
    for (std::size_t k = 0; k < victim_jobs && k + 1 < kJobs; ++k)
        g_blocks[kVictim][k / 2][k % 2] = reinterpret_cast<std::uint64_t>(g_jobs[k + 1]);
    for (std::size_t i = 0; i < kParticipants; ++i) {
        const std::uint64_t p = Participant(i), deque = p + L::kDeque;
        Put(p + L::kSystem, system);
        const std::uint32_t index = std::uint32_t(i);
        std::memcpy(reinterpret_cast<void*>(p + L::kIndex), &index, sizeof index);
        g_proxies[i] = deque;
        for (std::size_t m = 0; m < kMapSize; ++m) g_maps[i][m] = reinterpret_cast<std::uint64_t>(g_blocks[i][m]);
        Put(deque + kDequeContainer, reinterpret_cast<std::uint64_t>(&g_proxies[i]));
        Put(deque + kDequeMap, reinterpret_cast<std::uint64_t>(g_maps[i]));
        Put(deque + kDequeMapSize, kMapSize);
        Put(deque + kDequeLock, reinterpret_cast<std::uint64_t>(g_lock_vtable));
        Put(deque + kStealLock, reinterpret_cast<std::uint64_t>(g_lock_vtable));
        Put(deque + kDequeStart, 0);
        Put(deque + kDequeCount, i == 0 && own_job ? 1 : i == kVictim ? victim_jobs : 0);
    }
}

bool Executable(std::uint8_t* image) {
    for (const auto& r : kJobsCodePages) {
#ifdef _WIN32
        DWORD before = 0;
        if (!VirtualProtect(image + r[0], r[1] - r[0], PAGE_EXECUTE_READWRITE, &before)) return false;
#else
        if (mprotect(image + r[0], r[1] - r[0], PROT_READ | PROT_WRITE | PROT_EXEC)) return false;
#endif
    }
    return true;
}

void Expect(const char* variant, bool ok, const std::string& what) {
    if (!ok) Fail(std::string(variant) + ": main jobs barrier on the 1.09 step: " + what);
}

void Run(const char* variant, std::uint8_t* image, bool own_job, bool on) {
    Lay(own_job);
    using Step = void(__attribute__((sysv_abi)) *)(std::uint64_t);
    reinterpret_cast<Step>(image + kJobsBarrierStep)(Participant(0));
    const std::string what = std::string(own_job ? "own job" : "own deque empty") + (on ? ", on" : ", off");
    Expect(variant, g_counts.runs == (own_job ? 1 : 0), what + ": the job ran " + std::to_string(g_counts.runs) + " times");
    if (own_job) Expect(variant, g_counts.run_index == 0, what + ": the job ran with another index");
    Expect(variant, Get(Participant(0) + L::kDequeCount) == 0, what + ": its own deque still holds a job");
    Expect(variant, g_counts.tsc == (own_job ? 0 : 1),
           what + ": the game's time stamp was read " + std::to_string(g_counts.tsc) + " times");
    const int game_sweeps = own_job ? 0 : 1, ours = on ? 1 : 0;
    for (std::size_t i = 1; i < kParticipants; ++i)
        Expect(variant, g_counts.locks[i][0] == game_sweeps + ours,
               what + ": participant " + std::to_string(i) + "'s deque was swept " + std::to_string(g_counts.locks[i][0]) +
                   " times");
    Expect(variant, g_counts.locks[0][1] == int(kParticipants - 1) * (game_sweeps + ours),
           what + ": the steal lock of its own deque was taken " + std::to_string(g_counts.locks[0][1]) + " times");
}

void RunSteal(const char* variant, std::uint8_t* image, bool on) {
    Lay(false, 2);
    using Step = void(__attribute__((sysv_abi)) *)(std::uint64_t);
    reinterpret_cast<Step>(image + kJobsBarrierStep)(Participant(0));
    const std::string what = std::string("a worker's deque holds 2 jobs") + (on ? ", on" : ", off");
    const std::uint64_t first = reinterpret_cast<std::uint64_t>(g_jobs[1]), second = reinterpret_cast<std::uint64_t>(g_jobs[2]);
    Expect(variant, g_counts.runs == 1 && g_counts.last_job == first && g_counts.run_index == 0,
           what + ": the game's step did not run the worker's front job once on participant 0");
    const std::uint64_t own = Get(Participant(0) + L::kDequeCount), victim = Get(Participant(kVictim) + L::kDequeCount);
    Expect(variant, own == (on ? 1u : 0u), what + ": its own deque holds " + std::to_string(own) + " jobs after the step");
    Expect(variant, victim == (on ? 0u : 1u), what + ": the worker's deque holds " + std::to_string(victim) + " jobs after the step");
    if (!on) return;
    Expect(variant, g_blocks[0][0][0] == second, what + ": the added sweep did not move the worker's second job to its own deque");
    reinterpret_cast<Step>(image + kJobsBarrierStep)(Participant(0));
    Expect(variant, g_counts.runs == 2 && g_counts.last_job == second,
           what + ": the next step did not run the job the added sweep moved");
    Expect(variant, Get(Participant(0) + L::kDequeCount) == 0 && Get(Participant(kVictim) + L::kDequeCount) == 0,
           what + ": a deque still holds a job after the next step");
}

void RunCensus(const char* variant, std::uint8_t* image) {
    const std::size_t id = std::size_t(Lantern::StepId::main_jobs_barrier);
    BbEngine::MainJobsBarrier::Tick();
    Lantern::Steps::Totals before;
    Lantern::Steps::Sum(before);
    Lay(false, 2);
    std::thread barrier([image] {
        using Step = void(__attribute__((sysv_abi)) *)(std::uint64_t);
        reinterpret_cast<Step>(image + kJobsBarrierStep)(Participant(0));
        reinterpret_cast<Step>(image + kJobsBarrierStep)(Participant(0));
    });
    barrier.join();
    Expect(variant, g_counts.runs == 2, "on a thread without a step slot: the barrier's step did not run both jobs");
    if (!Mutant("jobs-barrier-census")) BbEngine::MainJobsBarrier::Tick();
    Lantern::Steps::Totals after;
    Lantern::Steps::Sum(after);
    const std::uint64_t total = after.total[id] - before.total[id], covered = after.covered[id] - before.covered[id],
                        used = after.used[id] - before.used[id];
    Expect(variant, total == 2 && covered == 2 && used == 1,
           "the census of a barrier run on another thread did not reach the registry at the frame tick (total " +
               std::to_string(total) + ", covered " + std::to_string(covered) + ", used " + std::to_string(used) + ")");
}

void Drive(const char* variant, std::uint8_t* image) {
    if (!BbEngine::MainJobsBarrier::LatchedOn()) {
        Fail(std::string(variant) + ": the main jobs barrier is not on after the install");
        return;
    }
    if (!Executable(image)) {
        Fail(std::string(variant) + ": the job code pages could not be made executable");
        return;
    }
    Put(reinterpret_cast<std::uint64_t>(image + kStackGuardSlot), reinterpret_cast<std::uint64_t>(&g_guard));
    Put(reinterpret_cast<std::uint64_t>(image + kReadTscSlot), reinterpret_cast<std::uint64_t>(&ReadTsc));
    Run(variant, image, true, true);
    Run(variant, image, false, true);
    RunSteal(variant, image, !Mutant("jobs-barrier-steal"));
    RunCensus(variant, image);
    if (!Lantern::Steps::SetFromMenu(Lantern::StepId::main_jobs_barrier, Lantern::Mode::Off))
        Fail(std::string(variant) + ": the steps menu could not switch the main jobs barrier off");
    if (!BbEngine::MainJobsBarrier::LatchedOn()) Fail(std::string(variant) + ": the switch took effect before the frame boundary");
    BbEngine::MainJobsBarrier::Tick();
    if (BbEngine::MainJobsBarrier::LatchedOn()) Fail(std::string(variant) + ": still on after the frame boundary");
    Run(variant, image, true, false);
    Run(variant, image, false, false);
    RunSteal(variant, image, false);
    if (!Lantern::Steps::SetFromMenu(Lantern::StepId::main_jobs_barrier, Lantern::Mode::On))
        Fail(std::string(variant) + ": the steps menu could not switch the main jobs barrier back on");
    BbEngine::MainJobsBarrier::Tick();
    Run(variant, image, false, true);
    std::atomic<int>& flip = BbEngine::MainJobsBarrier::Switch();
    if (flip.load() != 1) Fail(std::string(variant) + ": the runtime switch does not read 1 while the step is on");
    flip.store(Mutant("jobs-barrier-flip") ? 1 : 0);
    if (!BbEngine::MainJobsBarrier::LatchedOn()) Fail(std::string(variant) + ": the runtime switch took effect before the frame boundary");
    BbEngine::MainJobsBarrier::Tick();
    if (BbEngine::MainJobsBarrier::LatchedOn()) Fail(std::string(variant) + ": the runtime switch at 0 left the step on after the frame boundary");
    Run(variant, image, true, false);
    Run(variant, image, false, false);
    flip.store(1);
    BbEngine::MainJobsBarrier::Tick();
    if (!BbEngine::MainJobsBarrier::LatchedOn()) Fail(std::string(variant) + ": the runtime switch at 1 did not turn the step back on");
    Run(variant, image, false, true);
    Lay(true);
    Put(Participant(0) + L::kIndex, Mutant("jobs-barrier-shape") ? 0 : 1);
    using Step = void(__attribute__((sysv_abi)) *)(std::uint64_t);
    reinterpret_cast<Step>(image + kJobsBarrierStep)(Participant(0));
    if (BbEngine::MainJobsBarrier::LatchedOn() || g_counts.runs != 1 || g_counts.locks[1][0] != 0)
        Fail(std::string(variant) + ": a participant whose index differs from its place did not fall back to the game's step");
    BbEngine::MainJobsBarrier::Tick();
    if (BbEngine::MainJobsBarrier::LatchedOn()) Fail(std::string(variant) + ": back on after the refusal");
}

void CheckSite(const Variant& v, const std::uint8_t* image) {
    const std::uint8_t* at = image + kJobsBarrierStep;
    const auto& entry = BbEngine::HookEntries::kMainJobsBarrierStepEntry;
    const bool untouched = !std::memcmp(at, entry, sizeof entry);
    const bool jump = at[0] == 0xff && at[1] == 0x25;
    if (v.jobs_barrier && !jump) Fail(std::string(v.name) + ": the barrier's step at " + Hex(kJobsBarrierStep) + " was not hooked");
    if (!v.jobs_barrier && !untouched)
        Fail(std::string(v.name) + ": the barrier's step at " + Hex(kJobsBarrierStep) + " changed with the switch off");
}
}

std::vector<std::string> Required(const Variant& v, bool menus) {
    std::vector<std::string> out(kEngine.begin(), kEngine.end());
    out.push_back(v.rate->lists);
    out.push_back(v.rate->native);
    if (v.rate != &kUncapped)
        out.push_back(std::string("Engine: Frame rate: the physics world steps with the frame time the characters get, not a fixed 1/") +
                      v.rate->fps);
    else
        out.push_back("Engine: Frame rate: frame start pacing off (uncapped frame rate)");
    if (menus) out.insert(out.end(), kMenus.begin(), kMenus.end());
    if (v.net == Net::Offline) out.insert(out.end(), kCheats.begin(), kCheats.end());
    if (v.net != Net::Offline) out.push_back("Engine: online: a summon sign entry times out after 30 s");
    if (v.net == Net::Room) out.insert(out.end(), kRoom.begin(), kRoom.end());
    if (v.jobs_barrier) out.push_back(kJobsBarrierLine);
    return out;
}

void SetVariantEnv(const Variant& v, const fs::path& ini, const char* image_path, const char* assets) {
    SetEnv(LanternSandbox::kSwitch, "1");
    SetEnv(LanternSandbox::kLoopbackSwitch, nullptr);
    SetEnv("BB_CONFIG", ini.string().c_str());
    SetEnv("BB_ENGINE_IMAGE_SHA256", kImageSha);
    SetEnv("LANTERN_FRAMERATE_FROM_ENGINE", "1");
    SetEnv("BB_FAST_LOADING", "1");
    SetEnv("BB_FPS", v.rate->fps);
    SetEnv("BB_FPS_LIMIT", v.rate->limit);
    SetEnv("BB_ONLINE", v.net == Net::Offline ? "0" : "1");
    SetEnv("BB_NET_BACKEND", v.net == Net::Thd ? "thd" : v.net == Net::Room ? "room" : nullptr);
    SetEnv("BB_SIGN_TIMEOUT", v.net == Net::Offline ? nullptr : "30");
    for (const char* key : {"BB_DREAM_MIRROR_ASSET", "BB_REBIRTH_ASSET", "BB_PC_MENU_ASSETS", "BB_ROOM_SERVER", "BB_ROOM_NAME",
                            "BB_ROOM_HOST", "BB_THD_SERVER", "BB_THD_TOKEN", "BB_SHADNET_SERVER", "BB_SHADNET_NPID",
                            "LANTERN_FRAMERATE", "BB_VOICE", "BB_BELL_TRACE", "BB_PERSONAL_WORLD", "BB_WORLD_COPY",
                            "BB_GAME_MP_MESSAGES", "BB_GUEST_REWARDS_ALL", "BB_MATCHING_REGION"})
        SetEnv(key, nullptr);
    for (std::size_t i = 0; i < std::size(kGxSwitches); ++i)
        SetEnv(kGxSwitches[i], v.gx == GxStack::Whole           ? kGxOn[i]
                               : v.gx == GxStack::WholeNoRecords ? kGxNoRecords[i]
                               : v.gx == GxStack::WholeBindsOff  ? kGxBindsOff[i]
                                                                 : nullptr);
    SetEnv("LANTERN_JOB_LOCK", v.jobs ? "1" : "0");
    SetEnv("LANTERN_JOB_NODE_CACHE", v.jobs ? "1" : "0");
    SetEnv(kJobsBarrierSwitch, v.jobs_barrier ? "1" : "0");
    SetEnv("BB_GAME_DIR", fs::path(image_path).parent_path().string().c_str());
    if (assets) {
        SetEnv("BB_PC_MENU_ASSETS", (fs::path(assets) / "pc-menus").string().c_str());
        SetEnv("BB_DREAM_MIRROR_ASSET",
               (fs::path(assets) / "dream-mirror/dvdroot_ps4/map/mapstudio/m21_00_00_00.msb.dcx").string().c_str());
    }
}

void CheckGxWhole(const Variant& v, const std::vector<std::string>& lines) {
    if (v.gx == GxStack::Whole) {
        for (const char* required : kGxWhole)
            if (!Has(lines, required)) Fail(std::string(v.name) + ": no line \"" + required + "\"");
        if (Has(lines, "Engine: GX whole off")) Fail(std::string(v.name) + ": GX whole is off with the hooks on");
    }
    if (v.gx == GxStack::WholeBindsOff) {
        for (std::size_t i = 0; i + 1 < kGxWhole.size(); ++i)
            if (!Has(lines, kGxWhole[i])) Fail(std::string(v.name) + ": no line \"" + kGxWhole[i] + "\"");
        if (!Has(lines, kBindsOffLine)) Fail(std::string(v.name) + ": no line \"" + kBindsOffLine + "\"");
    }
    if (v.gx != GxStack::WholeBindsOff && Has(lines, kBindsOffLine)) Fail(std::string(v.name) + ": the binds off line without LANTERN_GX_BINDS=0");
    if (v.gx == GxStack::None && !Has(lines, "Engine: GX whole hooks: off (LANTERN_GX_WHOLE=1 with GX records"))
        Fail(std::string(v.name) + ": no GX whole hooks off line at the default");
    if (v.gx == GxStack::WholeNoRecords && !Has(lines, "Engine: GX whole hooks off: needs GX records"))
        Fail(std::string(v.name) + ": no line saying the hooks need GX records");
    if (v.gx == GxStack::WholeNoRecords && Has(lines, "Engine: GX whole hooks: on"))
        Fail(std::string(v.name) + ": the hooks say on without GX records");
    if (v.gx != GxStack::Whole && Has(lines, kSealedLine)) Fail(std::string(v.name) + ": GX buffers are sealed without GX whole and records");
    if (v.gx == GxStack::Whole && !Has(lines, kVersionedLine)) Fail(std::string(v.name) + ": no line \"" + kVersionedLine + "\"");
    if (v.gx != GxStack::Whole && Has(lines, "Engine: GX records: buffers versioned"))
        Fail(std::string(v.name) + ": GX buffers are versioned without GX whole and records");
    for (const std::string& line : lines)
        if (line.find(kByObjectLine) != std::string::npos) Fail(std::string(v.name) + ": buffers bound by object: " + line);
    if (BbEngine::GxRecords::KeysCaptured() != (v.gx == GxStack::Whole) || BbEngine::GxRecords::BindsByObject())
        Fail(std::string(v.name) + ": GX object keys must be captured only with GX whole and records, and the register path must bind by address");
    if (BbEngine::GxObjects::SealingOn() != (v.gx == GxStack::Whole))
        Fail(std::string(v.name) + (v.gx == GxStack::Whole ? ": no seal registry with GX whole and records" : ": a seal registry without GX whole and records"));
}

void CheckGxSites(const Variant& v, const std::uint8_t* image) {
    for (const BbEngine::GxWhole::Hooks::Site& site : BbEngine::GxWhole::Hooks::kSites) {
        const std::uint8_t* at = image + site.offset;
        const bool untouched = !std::memcmp(at, site.entry.data(), site.entry.size());
        const bool jump = at[0] == 0xff && at[1] == 0x25;
        const bool hooked = v.gx == GxStack::Whole || v.gx == GxStack::WholeBindsOff;
        if (hooked && !jump) Fail(std::string(v.name) + ": GX's " + site.name + " at " + Hex(site.offset) + " was not hooked");
        if (!hooked && !untouched)
            Fail(std::string(v.name) + ": GX's " + site.name + " at " + Hex(site.offset) + " changed without the hooks on");
    }
}

struct JobSite {
    const char* name;
    std::size_t offset;
    std::span<const std::uint8_t> entry;
    const char* on;
    const char* off;
};
const JobSite kJobSites[] = {
    {"Lock", BbEngine::MainJobs::kLock, BbEngine::HookEntries::kMainJobsLockEntry,
     "Engine: Job lock: on (LANTERN_JOB_LOCK=1)", "Engine: Job lock: the game's own (LANTERN_JOB_LOCK=0)"},
    {"ProcessOne", BbEngine::MainJobs::kProcessOne, BbEngine::HookEntries::kMainJobsProcessOneEntry,
     "Engine: Job node cache: on (LANTERN_JOB_NODE_CACHE=1)", "Engine: Job node cache: the game's own (LANTERN_JOB_NODE_CACHE=0)"},
};

void CheckJobs(const Variant& v, const std::vector<std::string>& lines, const std::uint8_t* image) {
    for (const JobSite& site : kJobSites) {
        const std::uint8_t* at = image + site.offset;
        const bool untouched = !std::memcmp(at, site.entry.data(), site.entry.size());
        const bool jump = at[0] == 0xff && at[1] == 0x25;
        if (v.jobs && !jump) Fail(std::string(v.name) + ": the game's " + site.name + " at " + Hex(site.offset) + " was not hooked");
        if (!v.jobs && !untouched) Fail(std::string(v.name) + ": the game's " + site.name + " at " + Hex(site.offset) + " changed with its switch at 0");
        const char* want = v.jobs ? site.on : site.off;
        if (!Has(lines, want)) Fail(std::string(v.name) + ": no line \"" + want + "\"");
        if (Has(lines, v.jobs ? site.off : site.on)) Fail(std::string(v.name) + ": the " + site.name + " line says the other state");
    }
}

void CheckLines(const Variant& v, const std::vector<std::string>& lines, bool menus) {
    for (const std::string& line : lines)
        for (const char* refused : kRefused)
            if (line.find(refused) != std::string::npos) {
                Fail(std::string(v.name) + ": " + line);
                break;
            }
    for (const std::string& required : Required(v, menus))
        if (!Has(lines, required)) Fail(std::string(v.name) + ": no line \"" + required + "\"");
    CheckGxWhole(v, lines);
    if (!v.jobs_barrier && Has(lines, "Main jobs barrier")) Fail(std::string(v.name) + ": a main jobs barrier line with the switch off");
    if (v.net == Net::Room)
        for (const char* rule : kBellRules)
            if (!HasLine(lines, std::string("Engine: online: seamless: ") + rule))
                Fail(std::string(v.name) + ": the bell rule \"" + rule + "\" was not written");
}

void CheckSandbox(const Variant& v) {
    const auto& c = LanternSandbox::counters;
    if (c.recording_opens.load()) Fail(std::string(v.name) + ": a recording device was opened");
    if (SDL_WasInit(SDL_INIT_AUDIO)) Fail(std::string(v.name) + ": SDL audio was started");
    if (c.sockets.load()) Fail(std::string(v.name) + ": " + std::to_string(c.sockets.load()) + " socket(s) were created");
    if (c.refused.load()) Fail(std::string(v.name) + ": " + std::to_string(c.refused.load()) + " connection(s) were attempted");
    if (v.net == Net::Room && !c.voice_refused.load()) Fail(std::string(v.name) + ": voice was not started and refused");
}

int Child(const char* path, const char* variant_name, const fs::path& scratch) {
    const Variant* variant = nullptr;
    for (const Variant& v : kVariants)
        if (!std::strcmp(v.name, variant_name)) variant = &v;
    if (!variant) {
        Fail(std::string("unknown variant ") + variant_name);
        return 1;
    }
    std::ifstream in(path, std::ios::binary);
    const std::vector<std::uint8_t> elf((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const std::string sha = sha256_hex(elf.data(), elf.size());
    if (std::none_of(std::begin(kEbootSha), std::end(kEbootSha), [&](const char* s) { return sha == s; })) {
        std::printf("%s: SKIPPED: %s is not the 1.09 image (sha256 %s)\n", kTest, path, sha.c_str());
        return kSkipCode;
    }
    std::size_t size = 0;
    std::uint8_t* image = MapImage(elf, size);
    if (!image) {
        Fail(std::string("the image's load segments could not be mapped from ") + path);
        return 1;
    }
    if (variant->gx != GxStack::None) {
        const std::size_t relocated = Relocate(elf, image, size);
        std::printf("%s %s: %zu relative relocations applied for GX objects\n", kTest, variant->name, relocated);
        if (!relocated) {
            Fail(std::string(variant->name) + ": no relative relocations could be applied");
            return 1;
        }
    }
    const char* assets_env = std::getenv(kAssetsSwitch);
    const char* assets = assets_env && *assets_env ? assets_env : nullptr;
    const fs::path ini = scratch / "lantern.ini";
    WriteSettings(ini, assets != nullptr);
    SetVariantEnv(*variant, ini, path, assets);
    {
        Lantern::SwitchInput input;
        input.env = [](const char* name, void*) -> const char* { return std::getenv(name); };
        Lantern::ReloadSwitches(input);
        Lantern::Steps::Start(Lantern::Switches::Get());
    }
    LanternSandbox::Reload();
    if (Mutant("expected-byte")) image[kKeepEchoes + 1] ^= 0xff;
    if (Mutant("jobs-lock-body") && variant->jobs) image[BbEngine::MainJobs::kLock + 0x40] ^= 0xff;
    if (Mutant("gx-whole-byte") && variant->gx == GxStack::Whole) image[BbEngine::GxWhole::Hooks::kSites[0].offset + 1] ^= 0xff;
    std::vector<std::string> lines;
    {
        Capture capture(scratch / "install.log");
        if (!capture.Ok()) {
            Fail("the install's output could not be captured");
            return 1;
        }
        BbSettings::Load();
        BbEngine::Install(image, size);
        BbEngine::Cheats::Tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(600));
        BbEngine::Cheats::Tick();
        DriveLoadingPace(variant->name, image);
        DriveAutoRotation(variant->name, image);
        if (variant->jobs_barrier) JobsDrive::Drive(variant->name, image);
        lines = capture.Lines();
    }
    std::printf("%s %s: %zu install lines%s\n", kTest, variant->name, lines.size(),
                assets ? "" : "; the Lantern menus and the Dream mirror not covered (BB_BOOT_INSTALL_ASSETS not set)");
    for (const std::string& line : lines)
        if (line.find("Frame rate: native at") != std::string::npos ||
            line.find("Frame rate: the engine wrote") != std::string::npos || line.rfind("Voice:", 0) == 0)
            std::printf("  %s\n", line.c_str());
    CheckLines(*variant, lines, assets != nullptr);
    CheckSandbox(*variant);
    CheckGxSites(*variant, image);
    CheckJobs(*variant, lines, image);
    JobsDrive::CheckSite(*variant, image);
    if (variant->rate != &kUncapped && std::memcmp(image + kWorldStep, kWorldJump, sizeof kWorldJump) != 0)
        Fail(std::string(variant->name) + ": the world step jump is not at 0x17f9b71");
    if (variant->net == Net::Offline && std::memcmp(image + kKeepEchoes, kKeepEchoesCode, sizeof kKeepEchoesCode) != 0)
        Fail(std::string(variant->name) + ": keep echoes did not write its code at 0x13d2bd2");
    if (failures) {
        std::printf("%s %s: the install's lines follow\n", kTest, variant->name);
        for (const std::string& line : lines) std::printf("  %s\n", line.c_str());
    }
    return failures ? 1 : 0;
}
}

std::string Self(const char* argv0) {
#ifdef _WIN32
    char buffer[MAX_PATH * 4] = {};
    const DWORD n = GetModuleFileNameA(nullptr, buffer, DWORD(sizeof buffer));
    if (n > 0 && n < sizeof buffer) return buffer;
#endif
    std::error_code error;
    return fs::absolute(argv0, error).string();
}

bool Launch(const std::vector<std::string>& args, int& code, std::string& why) {
    std::fflush(stdout);
#ifdef _WIN32
    std::vector<std::wstring> plain, wide;
    for (const std::string& a : args) {
        const int n = MultiByteToWideChar(CP_UTF8, 0, a.c_str(), -1, nullptr, 0);
        std::wstring w(n > 0 ? n - 1 : 0, L'\0');
        if (n > 0) MultiByteToWideChar(CP_UTF8, 0, a.c_str(), -1, w.data(), n);
        std::wstring quoted = L"\"";
        std::size_t slashes = 0;
        for (wchar_t c : w) {
            if (c == L'\\') {
                ++slashes;
                continue;
            }
            quoted.append(c == L'"' ? slashes * 2 + 1 : slashes, L'\\');
            slashes = 0;
            quoted += c;
        }
        quoted.append(slashes * 2, L'\\');
        quoted += L'"';
        plain.push_back(w);
        wide.push_back(quoted);
    }
    std::vector<const wchar_t*> argv;
    for (const std::wstring& w : wide) argv.push_back(w.c_str());
    argv.push_back(nullptr);
    errno = 0;
    SetLastError(0);
    const intptr_t rc = _wspawnv(_P_WAIT, plain[0].c_str(), argv.data());
    if (rc == -1) {
        why = "_wspawnv returned -1, errno " + std::to_string(errno) + " (" + std::strerror(errno) + "), GetLastError " +
              std::to_string(GetLastError());
        return false;
    }
    code = int(rc);
    return true;
#else
    std::vector<char*> argv;
    for (const std::string& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    const pid_t pid = fork();
    if (pid < 0) {
        why = "fork failed, errno " + std::to_string(errno) + " (" + std::strerror(errno) + ")";
        return false;
    }
    if (pid == 0) {
        execv(argv[0], argv.data());
        std::fprintf(stderr, "execv %s failed, errno %d (%s)\n", argv[0], errno, std::strerror(errno));
        _exit(127);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) != pid) {
        why = "waitpid failed, errno " + std::to_string(errno) + " (" + std::strerror(errno) + ")";
        return false;
    }
    if (WIFEXITED(status) && WEXITSTATUS(status) == 127) {
        why = "execv failed in the child, exit code 127";
        return false;
    }
    code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
    return true;
#endif
}

int main(int argc, char** argv) {
    if (argc == 5 && !std::strcmp(argv[1], "--child")) return Child(argv[2], argv[3], argv[4]);
    if (argc != 2) {
        std::printf("FAIL: %s needs its scratch folder (inside the worktree) as its one argument\n", kTest);
        return 1;
    }
    const char* image_env = std::getenv(kImageSwitch);
    if (!image_env || !*image_env) {
        std::printf("%s: SKIPPED: %s is not set (the path of the 1.09 eboot.elf, read only)\n", kTest, kImageSwitch);
        return 0;
    }
    std::error_code error;
    if (!fs::is_regular_file(image_env, error)) {
        std::printf("%s: SKIPPED: %s names no file\n", kTest, kImageSwitch);
        return 0;
    }
    const std::string path = image_env;
    const fs::path root = fs::absolute(argv[1], error);
    if (error || root.empty()) {
        std::printf("FAIL: %s: no scratch folder from %s\n", kTest, argv[1]);
        return 1;
    }
#ifdef _WIN32
    const int pid = _getpid();
#else
    const int pid = getpid();
#endif
    const fs::path scratch = root / ("boot-install-" + std::to_string(pid));
    fs::create_directories(scratch, error);
    if (error || !fs::is_directory(scratch)) {
        std::printf("FAIL: %s: the scratch folder %s could not be made\n", kTest, scratch.string().c_str());
        return 1;
    }
    const char* assets_env = std::getenv(kAssetsSwitch);
    const bool assets = assets_env && *assets_env && fs::is_directory(fs::path(assets_env) / "pc-menus", error);
    if (assets_env && *assets_env && !assets)
        std::printf("%s: %s has no pc-menus folder; the Lantern menus are not covered\n", kTest, kAssetsSwitch);
    SetEnv(kAssetsSwitch, assets ? assets_env : nullptr);
    const char* only = std::getenv(kOnlySwitch);
    int result = 0, ran = 0, skipped = 0;
    const std::string self = Self(argv[0]);
    for (const Variant& variant : kVariants) {
        if (only && *only && std::strcmp(only, variant.name) != 0) continue;
        const fs::path folder = scratch / variant.name;
        fs::create_directories(folder, error);
        if (error) {
            std::printf("FAIL: %s: the scratch folder %s could not be made\n", kTest, folder.string().c_str());
            return 1;
        }
        SetEnv("LANTERN_JOB_LOCK", variant.jobs ? "1" : "0");
        SetEnv("LANTERN_JOB_NODE_CACHE", variant.jobs ? "1" : "0");
        const std::vector<std::string> args = {self, "--child", path, variant.name, folder.string()};
        int code = 0;
        std::string why;
        if (!Launch(args, code, why)) {
            std::printf("FAIL: %s %s: could not start the child (%s):", kTest, variant.name, why.c_str());
            for (const std::string& a : args) std::printf(" \"%s\"", a.c_str());
            std::printf("\n");
            result = 1;
        } else if (code == kSkipCode) {
            ++skipped;
        } else if (code != 0) {
            std::printf("%s %s: FAILED (child exit code %d)\n", kTest, variant.name, code);
            result = 1;
        }
        ++ran;
    }
    fs::remove_all(scratch, error);
    if (!ran) {
        std::printf("FAIL: %s: %s=%s names no variant\n", kTest, kOnlySwitch, only);
        return 1;
    }
    if (result) return result;
    if (skipped) {
        std::printf("%s: SKIPPED: %d of %d variants found no 1.09 image\n", kTest, skipped, ran);
        return 0;
    }
    std::printf("%s: ok (%d variants)\n", kTest, ran);
    return 0;
}
