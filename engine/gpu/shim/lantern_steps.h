// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "lantern_decide.h"
#include "lantern_switches.h"

#ifndef LANTERN_DIAG_HOT
#define LANTERN_DIAG_HOT 1
#endif

namespace Lantern {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using i32 = std::int32_t;
using i64 = std::int64_t;

enum class CheckKind : u8 { None, Offline, Compare, Sampled };
enum class Stage : u8 { A, B, P, G };
enum class Weight : u8 { Light, Heavy };
enum class Wiring : u8 { Registry, Wired };
enum class Since : u8 { Old, New };
enum class StepState : u8 { Off, On, Compare, Disabled, Unavailable };
enum class OffWhy : u8 { None, Default, Switch, StepsOff };
enum class CheckClass : u8 { Rate, FirstHit };

enum class StepId : u8 {
#define X(id, name, group, sw, check, stage, weight, wiring, since, evidence, reasons) id,
#include "lantern_steps.inc"
#undef X
    kCount
};

inline constexpr int kStepCount = int(StepId::kCount);
inline constexpr int kMaxSteps = 64;
inline constexpr int kMaxReasons = 8;
inline constexpr int kMaxThreads = 128;
inline constexpr int kRecentDraws = 8;
inline constexpr int kNameWords = 3;
inline constexpr u32 kFreeTid = 0;
inline constexpr u32 kClaimingTid = 0xffffffffu;
inline constexpr std::size_t kCacheLine = 64;
inline constexpr u8 kNoStep = 0xff;
static_assert(kStepCount <= kMaxSteps);

struct StepInfo {
    std::string_view name;
    std::string_view group;
    SwitchId sw;
    CheckKind check;
    Stage stage;
    Weight weight;
    Wiring wiring;
    Since since;
    std::string_view evidence;
    std::array<std::string_view, kMaxReasons> reasons;
    std::span<const u64> guest_sites;
};

inline constexpr StepInfo kSteps[] = {
#define LANTERN_REASONS(...) {__VA_ARGS__}
#define X(id, name, group, sw, check, stage, weight, wiring, since, evidence, reasons)                              \
    {name, group, SwitchId::sw, CheckKind::check, Stage::stage, Weight::weight, Wiring::wiring, Since::since, evidence, \
     reasons, {}},
#include "lantern_steps.inc"
#undef X
#undef LANTERN_REASONS
};
static_assert(std::size(kSteps) == std::size_t(kStepCount));

constexpr const StepInfo& Info(StepId id) {
    return kSteps[std::size_t(id)];
}

constexpr Liveness LivenessOf(StepId id) {
    return Row(Info(id).sw).live;
}

constexpr bool Unverified(const StepInfo& s) {
    return s.check == CheckKind::None || s.check == CheckKind::Offline;
}

constexpr bool EvidenceGiven(std::span<const StepInfo> steps) {
    for (const StepInfo& s : steps) {
        if (Unverified(s) && s.evidence.empty()) return false;
    }
    return true;
}

struct Evidence {
    const char* what = "";
    u64 address = 0;
    u64 size = 0;
    u64 old_value[2] = {};
    u64 new_value[2] = {};
    u64 extra = 0;
};

struct CpuContext {
    u64 draw;
    u64 steps;
    u64 vs;
    u64 ps;
    u64 key;
};
static_assert(sizeof(CpuContext) == 40);

enum class CrumbPhase : u8 { Idle, In, Left };
enum class CrumbKind : u8 { None, Draw, Packet, Scope };

constexpr u64 PackNow(u8 step, CrumbPhase phase, CrumbKind kind, u64 number) {
    return (u64(step) << 56) | (u64(phase) << 52) | (u64(kind) << 48) | (number & 0xffffffffffffull);
}
constexpr u8 NowStep(u64 now) {
    return u8(now >> 56);
}
constexpr CrumbPhase NowPhase(u64 now) {
    return CrumbPhase((now >> 52) & 15);
}
constexpr CrumbKind NowKind(u64 now) {
    return CrumbKind((now >> 48) & 15);
}
constexpr u64 NowNumber(u64 now) {
    return now & 0xffffffffffffull;
}

struct alignas(kCacheLine) RecentDraw {
    std::atomic<u64> seq;
    std::atomic<u64> draw, frame, vs, ps, key, steps;
};
static_assert(sizeof(RecentDraw) == kCacheLine);

struct alignas(kCacheLine) ThreadSlot {
    ThreadSlot() = default;
    constexpr explicit ThreadSlot(bool shared_slot) : draw_hash{Decide::kNoHash}, shared{shared_slot} {}
    std::atomic<u64> owner;
    std::atomic<u64> crumb_seq;
    std::atomic<u64> now;
    std::atomic<const char*> site;
    std::atomic<u64> site_frame;
    std::atomic<u64> draw_steps;
    std::atomic<u32> recent_head;
    std::atomic<u32> draw_hash;
    std::atomic<u32> recent_written;
    alignas(kCacheLine) std::atomic<u64> name[kNameWords];
    std::atomic<const char*> vk_call;
    std::atomic<i32> vk_result;
    std::atomic<i64> rate_tokens, first_tokens;
    std::atomic<u64> refill_tsc;
    std::atomic<u32> draw_may_check;
    std::atomic<u64> use_seq;
    std::atomic<u64> other_writers;
    bool shared = false;
    bool has_draws = false;
    std::atomic<u64> min_share_second[kMaxSteps];
    RecentDraw recent[kRecentDraws];
    alignas(kCacheLine) std::atomic<u64> used[kMaxSteps];
    alignas(kCacheLine) std::atomic<u64> covered[kMaxSteps], total[kMaxSteps];
    alignas(kCacheLine) std::atomic<u64> checked[kMaxSteps], differ[kMaxSteps], check_tsc[kMaxSteps],
        skipped_budget[kMaxSteps], inconclusive[kMaxSteps], step_tsc[kMaxSteps];
    alignas(kCacheLine) std::atomic<u64> fell_back[kMaxSteps][kMaxReasons];
};

struct alignas(kCacheLine) Budget {
    std::atomic<i64> earn_rate;
    std::atomic<i64> earn_first;
    std::atomic<i64> earn_scope;
    std::atomic<i64> cap;
    std::atomic<u32> spill;
    std::atomic<u32> writer_check;
};

extern std::atomic<u8> g_state[kMaxSteps];
extern std::atomic<u16> g_sample_mask[kMaxSteps];
extern Budget g_budget;
extern ThreadSlot g_slots[kMaxThreads];
extern ThreadSlot g_dead_slot;
extern ThreadSlot g_overflow_slot;
extern std::atomic<u64> g_threads_without_slot;
extern std::atomic<u64> g_frame;
extern constinit thread_local ThreadSlot* t_slot;
extern constinit thread_local u32 t_tid;

namespace Steps {

ThreadSlot& ClaimSlot() noexcept;
void NoteOtherWriter(ThreadSlot& slot) noexcept;
bool SampleSlow(ThreadSlot& slot, StepId id, u32 hash, u32 mask) noexcept;
bool SampleNewSlow(ThreadSlot& slot, StepId id) noexcept;
u64 Ticks() noexcept;

inline ThreadSlot& Slot() noexcept {
    ThreadSlot* slot = t_slot;
    return slot ? *slot : ClaimSlot();
}

inline ThreadSlot* PeekSlot() noexcept {
    return t_slot;
}

inline Mode Get(StepId id) noexcept {
    const u8 state = g_state[std::size_t(id)].load(std::memory_order_relaxed);
    return state <= u8(StepState::Compare) ? Mode(state) : Mode::Off;
}

inline StepState State(StepId id) noexcept {
    return StepState(g_state[std::size_t(id)].load(std::memory_order_relaxed));
}

inline void Bump(std::atomic<u64>& counter, u64 n = 1) noexcept {
    counter.store(counter.load(std::memory_order_relaxed) + n, std::memory_order_relaxed);
}

inline void CheckWriter(ThreadSlot& slot) noexcept {
    if (g_budget.writer_check.load(std::memory_order_relaxed)) [[unlikely]] {
        NoteOtherWriter(slot);
    }
}

inline void Used(ThreadSlot& slot, StepId id) noexcept {
#if LANTERN_DIAG_HOT
    if (slot.shared) return;
    CheckWriter(slot);
    // written only by: the slot's owner thread
    Bump(slot.used[std::size_t(id)]);
    // written only by: the slot's owner thread
    slot.draw_steps.store(slot.draw_steps.load(std::memory_order_relaxed) | (u64(1) << u8(id)),
                          std::memory_order_relaxed);
#else
    (void)slot;
    (void)id;
#endif
}

inline void Covered(ThreadSlot& slot, StepId id, bool covered) noexcept {
#if LANTERN_DIAG_HOT
    if (slot.shared) return;
    CheckWriter(slot);
    // written only by: the slot's owner thread
    Bump(slot.total[std::size_t(id)]);
    // written only by: the slot's owner thread
    if (covered) Bump(slot.covered[std::size_t(id)]);
#else
    (void)slot;
    (void)id;
    (void)covered;
#endif
}

inline void FellBack(ThreadSlot& slot, StepId id, u8 reason) noexcept {
#if LANTERN_DIAG_HOT
    if (slot.shared) return;
    CheckWriter(slot);
    const u8 r = reason < kMaxReasons ? reason : u8(kMaxReasons - 1);
    // written only by: the slot's owner thread
    Bump(slot.fell_back[std::size_t(id)][r]);
#else
    (void)slot;
    (void)id;
    (void)reason;
#endif
}

inline void Inconclusive(ThreadSlot& slot, StepId id) noexcept {
    if (slot.shared) return;
    CheckWriter(slot);
    // written only by: the slot's owner thread
    Bump(slot.inconclusive[std::size_t(id)]);
}

inline bool Sample(ThreadSlot& slot, StepId id) noexcept {
#if LANTERN_DIAG_HOT
    const u32 hash = slot.draw_hash.load(std::memory_order_relaxed);
    const u32 mask = g_sample_mask[std::size_t(id)].load(std::memory_order_relaxed);
    if (!Decide::MayEnterSlowPath(slot.shared, hash, mask)) return false;
    return SampleSlow(slot, id, hash, mask);
#else
    (void)slot;
    (void)id;
    return false;
#endif
}

inline bool SampleNew(ThreadSlot& slot, StepId id, bool first_hit) noexcept {
#if LANTERN_DIAG_HOT
    if (!first_hit || slot.shared) return false;
    return SampleNewSlow(slot, id);
#else
    (void)slot;
    (void)id;
    (void)first_hit;
    return false;
#endif
}

void Checked(ThreadSlot& slot, StepId id, bool same, const Evidence& evidence, u64 tsc_spent,
             CheckClass cls = CheckClass::Rate) noexcept;

inline void Refill(ThreadSlot& slot, i64 rate_earn, i64 first_earn) noexcept {
    const Decide::Earn earn{rate_earn, first_earn, g_budget.cap.load(std::memory_order_relaxed),
                            g_budget.spill.load(std::memory_order_relaxed) != 0};
    const Decide::Buckets b = Decide::Refill(
        {slot.rate_tokens.load(std::memory_order_relaxed), slot.first_tokens.load(std::memory_order_relaxed)}, earn);
    // written only by: the slot's owner thread
    slot.rate_tokens.store(b.rate, std::memory_order_relaxed);
    // written only by: the slot's owner thread
    slot.first_tokens.store(b.first, std::memory_order_relaxed);
    // written only by: the slot's owner thread
    Bump(slot.refill_tsc);
    // written only by: the slot's owner thread
    slot.draw_may_check.store(b.rate > 0 ? 1u : 0u, std::memory_order_relaxed);
}

inline void SeqBegin(std::atomic<u64>& seq) noexcept {
    const u64 s = seq.load(std::memory_order_relaxed) | 1u;
    seq.store(s, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
}

inline void SeqEnd(std::atomic<u64>& seq) noexcept {
    seq.store((seq.load(std::memory_order_relaxed) | 1u) + 1, std::memory_order_release);
}

}

namespace Crumb {

inline void Draw(ThreadSlot& slot, u64 draw, u64 frame, u64 vs, u64 ps, u64 key) noexcept {
#if LANTERN_DIAG_HOT
    if (slot.shared) return;
    Steps::CheckWriter(slot);
    u32 head = slot.recent_head.load(std::memory_order_relaxed);
    slot.recent[head % kRecentDraws].steps.store(slot.draw_steps.load(std::memory_order_relaxed),
                                                 std::memory_order_relaxed);
    Steps::SeqBegin(slot.crumb_seq);
    slot.now.store(PackNow(kNoStep, CrumbPhase::In, CrumbKind::Draw, draw), std::memory_order_relaxed);
    slot.site_frame.store(frame, std::memory_order_relaxed);
    slot.draw_steps.store(0, std::memory_order_relaxed);
    Steps::SeqEnd(slot.crumb_seq);
    ++head;
    RecentDraw& r = slot.recent[head % kRecentDraws];
    Steps::SeqBegin(r.seq);
    r.draw.store(draw, std::memory_order_relaxed);
    r.frame.store(frame, std::memory_order_relaxed);
    r.vs.store(vs, std::memory_order_relaxed);
    r.ps.store(ps, std::memory_order_relaxed);
    r.key.store(key, std::memory_order_relaxed);
    r.steps.store(0, std::memory_order_relaxed);
    Steps::SeqEnd(r.seq);
    // written only by: the slot's owner thread
    slot.recent_head.store(head, std::memory_order_relaxed);
    const u32 written = slot.recent_written.load(std::memory_order_relaxed);
    // written only by: the slot's owner thread
    if (written < u32(kRecentDraws)) slot.recent_written.store(written + 1, std::memory_order_release);
    slot.draw_hash.store(Decide::MixHash(draw), std::memory_order_relaxed);
    slot.has_draws = true;
    Steps::Refill(slot, g_budget.earn_rate.load(std::memory_order_relaxed),
                  g_budget.earn_first.load(std::memory_order_relaxed));
#else
    (void)slot;
    (void)draw;
    (void)frame;
    (void)vs;
    (void)ps;
    (void)key;
#endif
}

inline void Packet(ThreadSlot& slot, u64 packet) noexcept {
#if LANTERN_DIAG_HOT
    if (slot.shared) return;
    slot.now.store(PackNow(kNoStep, CrumbPhase::In, CrumbKind::Packet, packet), std::memory_order_relaxed);
#else
    (void)slot;
    (void)packet;
#endif
}

inline void Vk(ThreadSlot& slot, const char* call) noexcept {
#if LANTERN_DIAG_HOT
    if (slot.shared) return;
    slot.vk_call.store(call, std::memory_order_relaxed);
#else
    (void)slot;
    (void)call;
#endif
}

class Scope {
public:
    Scope(ThreadSlot& slot, StepId id, const char* site) noexcept : slot_(slot) {
#if LANTERN_DIAG_HOT
        if (slot.shared) return;
        active_ = true;
        Steps::CheckWriter(slot);
        if (!slot.has_draws) {
            // written only by: the slot's owner thread
            const u64 use = slot.use_seq.load(std::memory_order_relaxed) + 1;
            slot.use_seq.store(use, std::memory_order_relaxed);
            slot.draw_hash.store(Decide::MixHash(use), std::memory_order_relaxed);
            const i64 earn = g_budget.earn_scope.load(std::memory_order_relaxed);
            Steps::Refill(slot, earn, earn);
        }
        saved_now_ = slot.now.load(std::memory_order_relaxed);
        saved_site_ = slot.site.load(std::memory_order_relaxed);
        id_ = id;
        Steps::SeqBegin(slot.crumb_seq);
        slot.now.store(PackNow(u8(id), CrumbPhase::In, CrumbKind::Scope, 0), std::memory_order_relaxed);
        slot.site.store(site, std::memory_order_relaxed);
        slot.site_frame.store(g_frame.load(std::memory_order_relaxed), std::memory_order_relaxed);
        Steps::SeqEnd(slot.crumb_seq);
        const u32 hash = slot.draw_hash.load(std::memory_order_relaxed);
        if (((hash >> 10) & 1023u) == 1u) start_ = Steps::Ticks();
#else
        (void)id;
        (void)site;
#endif
    }
    ~Scope() {
#if LANTERN_DIAG_HOT
        if (!active_) return;
        if (start_) {
            // written only by: the slot's owner thread
            Steps::Bump(slot_.step_tsc[std::size_t(id_)], Steps::Ticks() - start_);
        }
        const bool was_idle = NowPhase(saved_now_) != CrumbPhase::In;
        Steps::SeqBegin(slot_.crumb_seq);
        if (was_idle) {
            slot_.now.store(PackNow(u8(id_), CrumbPhase::Left, CrumbKind::Scope, 0), std::memory_order_relaxed);
            slot_.site_frame.store(g_frame.load(std::memory_order_relaxed), std::memory_order_relaxed);
        } else {
            slot_.now.store(saved_now_, std::memory_order_relaxed);
            slot_.site.store(saved_site_, std::memory_order_relaxed);
        }
        Steps::SeqEnd(slot_.crumb_seq);
#endif
    }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

private:
    ThreadSlot& slot_;
    u64 saved_now_ = 0;
    const char* saved_site_ = nullptr;
    u64 start_ = 0;
    StepId id_{};
    bool active_ = false;
};

}

namespace Steps {

struct FlipInput {
    u64 now_ns = 0;
    bool loading_screen_up = false;
    u64 frame_period_ns = 0;
};

struct Totals {
    std::array<u64, kMaxSteps> used{}, covered{}, total{}, checked{}, differ{}, check_tsc{}, skipped_budget{},
        inconclusive{}, step_tsc{};
    std::array<std::array<u64, kMaxReasons>, kMaxSteps> fell_back{};
    u64 draws = 0;
    u64 other_writers = 0;
};

enum class SlotRead : u8 { Free, Changed, Ok };

struct SlotView {
    u32 tid = 0;
    char name[kNameWords * 8 + 1] = {};
    u64 now = 0;
    const char* site = nullptr;
    u64 site_frame = 0;
    u64 draw_steps = 0;
    bool crumbs_changing = false;
    u32 recent_count = 0;
    struct Recent {
        u64 draw, frame, vs, ps, key, steps, seq;
        bool changing;
    } recent[kRecentDraws] = {};
};

struct StartReport {
    std::array<OffWhy, kMaxSteps> off_why{};
    u64 steps_off_named = 0;
    u64 steps_off_not_switched = 0;
    char toggles[256] = {};
    char problems[512] = {};
};

bool Start(const Switches& switches) noexcept;
bool StartOnce() noexcept;
const StartReport& StartInfo() noexcept;
bool SetUnavailable(StepId id, const char* reason) noexcept;
bool SetFromMenu(StepId id, Mode wanted) noexcept;
const char* UnavailableReason(StepId id) noexcept;
Mode StartModeByName(std::string_view switch_name) noexcept;

void SetTicksPerSecond(double hz) noexcept;
void StartCalibration() noexcept;
bool Calibrated() noexcept;
double TicksPerSecond() noexcept;
void OnFlip(const FlipInput& input) noexcept;
u64 Area() noexcept;
bool WarmWindow() noexcept;

void Sum(Totals& out) noexcept;
void AddUnslotted(StepId id, u64 total, u64 covered, int reason, u64 fell_back, u64 used) noexcept;
SlotRead ReadSlot(int index, SlotView& out) noexcept;
SlotRead ReadSlot(const ThreadSlot& slot, SlotView& out) noexcept;
int ReclaimDead(bool (*alive)(u32 tid)) noexcept;
bool ThreadAlive(u32 tid) noexcept;
void ResetForTest() noexcept;

std::size_t FormatStepsOffLine(char* out, std::size_t size) noexcept;
u64 StartOnlyDiffered() noexcept;
u64 TicksReads() noexcept;
bool HoldFirstDifferenceForTest(StepId id) noexcept;
bool FirstDifferenceText(StepId id, char* out, std::size_t size) noexcept;
bool FirstDifferenceReady(StepId id) noexcept;
bool FirstDifferenceClaimed(StepId id) noexcept;

struct Out {
    char* buffer;
    std::size_t size;
    std::size_t used = 0;
    bool truncated = false;
    void Put(std::string_view text) noexcept;
    void Format(const char* format, ...) noexcept __attribute__((format(printf, 2, 3)));
};

void PrintStepsAtStart() noexcept;
void PrintNewFirstDifferences() noexcept;
void PrintMinuteTable(double seconds, bool at_exit) noexcept;

}

struct WarnSite {
    const char* text;
    std::atomic<u64> count{0};
    std::atomic<u32> claimed{0};
    std::atomic<u32> ready{0};
    Evidence first{};
    u64 first_frame = 0;
    u64 first_draw = 0;
    char first_thread[kNameWords * 8 + 1] = {};
    u64 shown = 0;
    WarnSite* next = nullptr;
    constexpr explicit WarnSite(const char* t) : text(t) {}
};

namespace Diag {
void Warn(WarnSite& site, const Evidence& evidence) noexcept;
WarnSite* Warnings() noexcept;
void PrintWarnings() noexcept;
}

namespace VkMessages {
inline constexpr int kEntries = 256;
inline constexpr std::size_t kTextBytes = 192;
bool Note(i32 message_id, const char* text) noexcept;
u64 Count(i32 message_id) noexcept;
u64 Overflow() noexcept;
}

namespace Diagnosis {
struct Inputs {
    long pid = 0;
    const char* started = "";
    const char* engine_id = "";
    const char* os = "";
    const char* ended = "exit";
    u64 minutes_tenths = 0;
    u64 frames = 0;
    u64 areas = 0;
    u64 fps_tenths = 0;
};
void Write(Steps::Out& out, const Inputs& inputs) noexcept;
void WriteRaw(Steps::Out& out, const Inputs& inputs) noexcept;
bool WriteFile(const Inputs& inputs) noexcept;
bool PrepareFile() noexcept;
void Path(char* out, std::size_t size, const char* file) noexcept;
}

}
