// SPDX-License-Identifier: GPL-3.0-or-later
#include "mouse_camera.h"
#include "../engine/hook_entries.h"
#include "input.h"
#include "mouse_turn.h"
#include "../engine/engine_hooks.h"
#include "../engine/online_fixes.h"
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iterator>
#include <mutex>

#ifdef _WIN32
#define LANTERN_STUB_SYMBOL
#else
#define LANTERN_STUB_SYMBOL __attribute__((visibility("hidden")))
#endif

extern "C" {
LANTERN_STUB_SYMBOL std::uint64_t lantern_free_camera_resume = 0;
LANTERN_STUB_SYMBOL std::uint8_t lantern_free_camera_ran = 0;
LANTERN_STUB_SYMBOL __attribute__((sysv_abi)) void lantern_free_camera_turn(std::uint8_t* camera,
                                                                             std::uint8_t* had_input,
                                                                             float stick_pitch, float stick_yaw);
void lantern_free_camera_site();
void lantern_fast_turn_exit();
}

asm(R"(
    .text
    .p2align 4
    .globl lantern_free_camera_site
lantern_free_camera_site:
    vmovss      %xmm0, 0x140(%r13)
    vextractps  $1, %xmm1, 0x144(%r13)
    push        %rbx
    push        %rax
    push        %rcx
    push        %rdx
    push        %rsi
    push        %rdi
    push        %r8
    push        %r9
    push        %r10
    push        %r11
    mov         %rsp, %rbx
    and         $-32, %rsp
    sub         $512, %rsp
    vmovdqu     %ymm0, 0x000(%rsp)
    vmovdqu     %ymm1, 0x020(%rsp)
    vmovdqu     %ymm2, 0x040(%rsp)
    vmovdqu     %ymm3, 0x060(%rsp)
    vmovdqu     %ymm4, 0x080(%rsp)
    vmovdqu     %ymm5, 0x0a0(%rsp)
    vmovdqu     %ymm6, 0x0c0(%rsp)
    vmovdqu     %ymm7, 0x0e0(%rsp)
    vmovdqu     %ymm8, 0x100(%rsp)
    vmovdqu     %ymm9, 0x120(%rsp)
    vmovdqu     %ymm10, 0x140(%rsp)
    vmovdqu     %ymm11, 0x160(%rsp)
    vmovdqu     %ymm12, 0x180(%rsp)
    vmovdqu     %ymm13, 0x1a0(%rsp)
    vmovdqu     %ymm14, 0x1c0(%rsp)
    vmovdqu     %ymm15, 0x1e0(%rsp)
    mov         %r13, %rdi
    lea         -0x3c4(%rbp), %rsi
    vmovaps     %xmm10, %xmm0
    vmovshdup   %xmm10, %xmm1
    vzeroupper
    call        lantern_free_camera_turn
    vmovdqu     0x000(%rsp), %ymm0
    vmovdqu     0x020(%rsp), %ymm1
    vmovdqu     0x040(%rsp), %ymm2
    vmovdqu     0x060(%rsp), %ymm3
    vmovdqu     0x080(%rsp), %ymm4
    vmovdqu     0x0a0(%rsp), %ymm5
    vmovdqu     0x0c0(%rsp), %ymm6
    vmovdqu     0x0e0(%rsp), %ymm7
    vmovdqu     0x100(%rsp), %ymm8
    vmovdqu     0x120(%rsp), %ymm9
    vmovdqu     0x140(%rsp), %ymm10
    vmovdqu     0x160(%rsp), %ymm11
    vmovdqu     0x180(%rsp), %ymm12
    vmovdqu     0x1a0(%rsp), %ymm13
    vmovdqu     0x1c0(%rsp), %ymm14
    vmovdqu     0x1e0(%rsp), %ymm15
    mov         %rbx, %rsp
    pop         %r11
    pop         %r10
    pop         %r9
    pop         %r8
    pop         %rdi
    pop         %rsi
    pop         %rdx
    pop         %rcx
    pop         %rax
    pop         %rbx
    vmovss      0x140(%r13), %xmm0
    jmp         *lantern_free_camera_resume(%rip)

    .p2align 4
    .globl lantern_fast_turn_exit
lantern_fast_turn_exit:
    vmovss      %xmm0, 0x140(%r13)
    movb        $1, lantern_free_camera_ran(%rip)
    jmp         *lantern_free_camera_resume(%rip)
)");

namespace MouseCamera {
namespace {
using Clock = std::chrono::steady_clock;

constexpr std::size_t kUpdate = 0x143ac60;

constexpr const auto& kUpdateEntry = BbEngine::HookEntries::kMouseCameraUpdateEntry;
constexpr std::size_t kFreeStore = 0x143ce54, kAfterStore = 0x143ce67;

constexpr std::uint8_t kFreeStoreCode[] = {0xc4, 0xc1, 0x7a, 0x11, 0x85, 0x40, 0x01, 0x00, 0x00, 0xc4,
                                           0xc3, 0x79, 0x17, 0x8d, 0x44, 0x01, 0x00, 0x00, 0x01};
struct FastTurnExit {
    std::size_t at;
    std::uint8_t code[14];
};
constexpr FastTurnExit kFastTurnExits[] = {
    {0x143f9bc, {0xc4, 0xc1, 0x7a, 0x11, 0x85, 0x40, 0x01, 0x00, 0x00, 0xe9, 0x9d, 0xd4, 0xff, 0xff}},
    {0x143fabf, {0xc4, 0xc1, 0x7a, 0x11, 0x85, 0x40, 0x01, 0x00, 0x00, 0xe9, 0x9a, 0xd3, 0xff, 0xff}},
};
constexpr std::size_t kPitch = 0x140, kYaw = 0x144, kHighest = 0x1ec, kLowest = 0x1f0;

struct AutoRotationStore {
    std::size_t at;
    std::uint8_t code[9];
    bool held_by_mouse;
};
constexpr AutoRotationStore kAutoRotation[] = {
    {0x143c6e8, {0xc4, 0xc1, 0x7a, 0x11, 0x85, 0x44, 0x01, 0x00, 0x00}, true},
    {0x143c870, {0xc4, 0xc1, 0x7a, 0x11, 0xad, 0x44, 0x01, 0x00, 0x00}, false},
    {0x143c984, {0xc4, 0xc1, 0x7a, 0x11, 0x8d, 0x30, 0x01, 0x00, 0x00}, true},
    {0x143dde6, {0xc4, 0xc1, 0x7a, 0x11, 0x85, 0x94, 0x02, 0x00, 0x00}, true},
};
constexpr std::uint8_t kNops[9] = {0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90};

constexpr float kFlickRate = 3000.0f, kFlickRearmRate = 1500.0f, kFlickCooldown = 0.5f;
constexpr auto kFlickShown = std::chrono::milliseconds(80);

std::uint8_t* g_image = nullptr;
bool g_installed = false;

std::mutex g_counts_lock;
float g_count_x = 0, g_count_y = 0;

std::atomic<float> g_degrees_per_count{MouseTurn::DegreesPerCount(5)};
std::atomic<bool> g_invert_x{false}, g_invert_y{false}, g_hold{false};

float g_update_x = 0, g_update_y = 0;
bool g_auto_rotation_nops = false;
bool g_patched_out = false;
bool g_store_is_games[std::size(kAutoRotation)] = {};
bool g_free_last_update = true;

std::mutex g_flick_lock;
float g_flick_x = 0, g_flick_y = 0;
unsigned g_flick_updates = 0;

Clock::time_point g_last_read{};
float g_cooldown = 0;
float g_shown_x = 0, g_shown_y = 0;
Clock::time_point g_shown_until{};

std::atomic<unsigned> g_turns{0}, g_flicks{0}, g_free_changes{0};

std::uint8_t* At(std::size_t offset) { return g_image + offset; }

void HoldAutoRotation(bool hold) {
    if (g_patched_out || hold == g_auto_rotation_nops) return;
    std::array<BbEngine::CodePatch, std::size(kAutoRotation)> patches;
    std::size_t count = 0;
    for (std::size_t i = 0; i < std::size(kAutoRotation); ++i) {
        const AutoRotationStore& s = kAutoRotation[i];
        if (!s.held_by_mouse || !g_store_is_games[i]) continue;
        patches[count++] = {g_image, s.at, hold ? std::span(s.code) : std::span(kNops), hold ? std::span(kNops) : std::span(s.code)};
    }
    if (const BbEngine::WriteResult result = BbEngine::WriteCode(std::span<const BbEngine::CodePatch>(patches).first(count));
        result != BbEngine::WriteResult::Applied) {
        g_patched_out = true;
        host_log("mouse camera: the game's auto-rotation stays on: its code %s", BbEngine::Describe(result));
        return;
    }
    g_auto_rotation_nops = hold;
}

std::int64_t __attribute__((sysv_abi)) OnCameraUpdate(std::uint64_t, std::uint64_t*) noexcept {
    {
        std::scoped_lock lock(g_counts_lock);
        g_update_x = g_count_x;
        g_update_y = g_count_y;
        g_count_x = g_count_y = 0;
    }
    HoldAutoRotation(g_hold.load(std::memory_order_relaxed));
    const bool free = __atomic_exchange_n(&lantern_free_camera_ran, 0, __ATOMIC_RELAXED) != 0;
    if (!free) {
        std::scoped_lock lock(g_flick_lock);
        g_flick_x += g_update_x;
        g_flick_y += g_update_y;
        ++g_flick_updates;
    }

    static unsigned updates = 0;
    if (++updates > 2 && free != g_free_last_update && g_free_changes.fetch_add(1, std::memory_order_relaxed) < 8)
        host_log("mouse camera: the free camera %s", free ? "is back" : "stopped (locked on, or the game turns it)");
    g_free_last_update = free;
    return 0;
}

constexpr std::size_t kJumpBytes = 14;
using JumpCode = std::array<std::uint8_t, 32>;
JumpCode JumpTo(void (*to)()) {
    JumpCode code;
    code.fill(0xcc);
    const auto target = reinterpret_cast<std::uint64_t>(to);
    code[0] = 0xff;
    code[1] = 0x25;
    std::memset(code.data() + 2, 0, 4);
    std::memcpy(code.data() + 6, &target, 8);
    return code;
}
}

bool Install(std::uint8_t* image, std::size_t size) {
    g_image = image;
    const auto matches = [&](std::size_t at, const std::uint8_t* code, std::size_t n) {
        return at + n <= size && std::memcmp(image + at, code, n) == 0;
    };
    if (!matches(kFreeStore, kFreeStoreCode, sizeof kFreeStoreCode)) {
        host_log("mouse camera: off, the follow camera's store is not 1.09's");
        return false;
    }
    for (const FastTurnExit& exit : kFastTurnExits) {
        if (!matches(exit.at, exit.code, sizeof exit.code)) {
            host_log("mouse camera: off, the follow camera's fast turn is not 1.09's");
            return false;
        }
    }

    bool patched = false;
    for (std::size_t i = 0; i < std::size(kAutoRotation); ++i) {
        const AutoRotationStore& s = kAutoRotation[i];
        g_store_is_games[i] = matches(s.at, s.code, sizeof s.code);
        patched = patched || matches(s.at, kNops, sizeof kNops);
    }
    BbEngine::Hook entry;
    if (!BbEngine::Prepare(entry, image, size, kUpdate, BbEngine::SiteKind::Entry, kUpdateEntry, OnCameraUpdate)) {
        host_log("mouse camera: off, the follow camera's update is not 1.09's");
        return false;
    }
    g_patched_out = patched;
    lantern_free_camera_resume = reinterpret_cast<std::uint64_t>(At(kAfterStore));
    static_assert(sizeof kFreeStoreCode >= kJumpBytes && sizeof kFreeStoreCode <= sizeof(JumpCode));
    static_assert(sizeof FastTurnExit::code >= kJumpBytes && sizeof FastTurnExit::code <= sizeof(JumpCode));
    const JumpCode free_jump = JumpTo(lantern_free_camera_site), turn_jump = JumpTo(lantern_fast_turn_exit);
    std::vector<BbEngine::CodePatch> jumps{{image, kFreeStore, kFreeStoreCode, std::span(free_jump).first(sizeof kFreeStoreCode)}};
    for (const FastTurnExit& exit : kFastTurnExits)
        jumps.push_back({image, exit.at, exit.code, std::span(turn_jump).first(sizeof exit.code)});
    if (const BbEngine::WriteResult result = BbEngine::Commit(std::span(&entry, 1), jumps); result != BbEngine::WriteResult::Applied) {
        host_log("mouse camera: off, the follow camera's code %s", BbEngine::Describe(result));
        return false;
    }
    g_installed = true;
    host_log("mouse camera: %.3f degrees a count at sensitivity 5, after the stick and only without it; a flick "
             "switches the lock-on target%s",
             MouseTurn::DegreesPerCount(5), g_patched_out ? "; a patch holds the camera's auto-rotation already" : "");
    return true;
}

bool Installed() { return g_installed; }

void AddCounts(float dx, float dy) {
    std::scoped_lock lock(g_counts_lock);
    g_count_x += dx;
    g_count_y += dy;
}

void Reset() {
    {
        std::scoped_lock lock(g_counts_lock);
        g_count_x = g_count_y = 0;
    }
    std::scoped_lock lock(g_flick_lock);
    g_flick_x = g_flick_y = 0;
    g_flick_updates = 0;
}

void Publish(int sensitivity, bool invert_x, bool invert_y, bool hold_auto_rotation) {
    g_degrees_per_count.store(MouseTurn::DegreesPerCount(sensitivity), std::memory_order_relaxed);
    g_invert_x.store(invert_x, std::memory_order_relaxed);
    g_invert_y.store(invert_y, std::memory_order_relaxed);

    static unsigned changes = 0;
    if (g_hold.exchange(hold_auto_rotation, std::memory_order_relaxed) != hold_auto_rotation && changes++ < 6)
        host_log("mouse camera: auto-rotation %s",
                 hold_auto_rotation ? "held (the keyboard and mouse in use)" : "the game's own");
}

bool Flick(float& x, float& y) {
    const Clock::time_point now = Clock::now();
    float dt = g_last_read == Clock::time_point{} ? 0.0f : std::chrono::duration<float>(now - g_last_read).count();
    dt = std::min(dt, 0.25f);
    g_last_read = now;
    float dx, dy;
    unsigned updates;
    {
        std::scoped_lock lock(g_flick_lock);
        dx = g_flick_x;
        dy = g_flick_y;
        updates = g_flick_updates;
        g_flick_x = g_flick_y = 0;
        g_flick_updates = 0;
    }
    if (updates && dt > 0) {
        const float distance = std::hypot(dx, dy);
        const float rate = distance / dt;
        g_cooldown = rate < kFlickRearmRate ? 0.0f : std::max(0.0f, g_cooldown - dt);
        if (g_cooldown <= 0 && rate > kFlickRate) {
            g_shown_x = dx / distance;
            g_shown_y = dy / distance;
            g_shown_until = now + kFlickShown;
            g_cooldown = kFlickCooldown;
            if (g_flicks.fetch_add(1, std::memory_order_relaxed) < 4)
                host_log("mouse camera: a flick of %.0f counts (%.0f a second) switches the lock-on target toward "
                         "%+.2f,%+.2f", distance, rate, g_shown_x, g_shown_y);
        }
    }
    if (now >= g_shown_until) return false;
    x = g_shown_x;
    y = g_shown_y;
    return true;
}
}

extern "C" __attribute__((sysv_abi)) void lantern_free_camera_turn(std::uint8_t* camera, std::uint8_t* had_input,
                                                                    float stick_pitch, float stick_yaw) {
    using namespace MouseCamera;
    __atomic_store_n(&lantern_free_camera_ran, 1, __ATOMIC_RELAXED);
    {
        float seen;
        std::memcpy(&seen, camera + kYaw, 4);
        BbEngine::OnlineFixes::SetCameraYaw(seen);
    }
    if (g_update_x == 0 && g_update_y == 0) return;
    if (stick_pitch != 0 || stick_yaw != 0) return;
    float pitch, yaw, highest, lowest;
    std::memcpy(&pitch, camera + kPitch, 4);
    std::memcpy(&yaw, camera + kYaw, 4);
    std::memcpy(&highest, camera + kHighest, 4);
    std::memcpy(&lowest, camera + kLowest, 4);
    const float was_pitch = pitch, was_yaw = yaw;
    const MouseTurn::Settings s{g_degrees_per_count.load(std::memory_order_relaxed),
                                g_invert_x.load(std::memory_order_relaxed), g_invert_y.load(std::memory_order_relaxed)};
    if (!MouseTurn::Turn(g_update_x, g_update_y, s, pitch, yaw, lowest, highest)) return;
    std::memcpy(camera + kPitch, &pitch, 4);
    std::memcpy(camera + kYaw, &yaw, 4);
    *had_input = 1;
    if (g_turns.fetch_add(1, std::memory_order_relaxed) == 0)
        host_log("mouse camera: first turn, %+.0f,%+.0f counts: pitch %.4f -> %.4f, yaw %.4f -> %.4f", g_update_x,
                 g_update_y, was_pitch, pitch, was_yaw, yaw);
}
