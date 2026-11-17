// SPDX-License-Identifier: GPL-3.0-or-later
#include "lantern_steps.h"
#include "lantern_scrub.h"

#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <intrin.h>
#else
#include <cerrno>
#include <cpuid.h>
#include <pthread.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <x86intrin.h>
#endif

namespace Lantern {

namespace {

constexpr u16 kWarmMaskLight = 15;
constexpr u16 kWarmMaskHeavy = 63;
constexpr u16 kSteadyMaskLight = 1023;
constexpr u16 kSteadyMaskHeavy = 4095;
constexpr u64 kWarmNs = 5'000'000'000ull;
constexpr u64 kSecondNs = 1'000'000'000ull;
constexpr u64 kMinuteNs = 60'000'000'000ull;
constexpr double kCheckCapNs = 40'000.0;
constexpr double kCheckCapShareOfFrame = 0.005;
constexpr double kEarnPerDrawNs = 15.0;
constexpr int kCalibrationMs = 50;
constexpr std::size_t kFirstTextBytes = 384;
constexpr std::size_t kNextStartBytes = 320;
constexpr int kAttentionMax = 8;
constexpr std::size_t kLineBytes = 1024;
constexpr double kColdBenchNsPerCall = 8.8;
constexpr u32 kCpuidPowerLeaf = 0x80000007u;
constexpr u32 kInvariantTscBit = 1u << 8;

struct FirstDifference {
    std::atomic<u32> claimed{0};
    std::atomic<u32> ready{0};
    std::atomic<u64> frame{0};
    char text[kFirstTextBytes] = {};
    char next_start[kNextStartBytes] = {};
    bool printed = false;
};

struct Registry {
    FirstDifference first[kMaxSteps];
    std::atomic<u64> start_only_differed{0};
    std::atomic<const char*> unavailable[kMaxSteps];
    std::atomic<u8> start_state[kMaxSteps];
    std::atomic<u32> checks_on{1};
    std::atomic<u32> calibrated{0};
    std::atomic<u32> use_tsc{0};
    std::atomic<double> ticks_per_second{1e9};
    std::atomic<u64> second{1};
    std::atomic<u32> frame_in_second{0};
    std::atomic<u32> frames_per_second{0};
    std::atomic<u64> area{0};
    std::atomic<u32> warm{1};
    Steps::StartReport start_report;
    u64 steps_off_named = 0;
    char toggles_named[256] = {};
};

struct FlipClock {
    u64 second_start = 0;
    u64 warm_until = 0;
    u64 next_reclaim = 0;
    u32 frames_this_second = 0;
    u64 period_ns = 0;
    bool loading_seen = false;
    bool started = false;
};

struct MinuteState {
    Steps::Totals last{};
    bool printed_start = false;
};

Registry g_registry;
FlipClock g_flip;
MinuteState g_minute;
std::once_flag g_start_once;
std::atomic<bool> g_started{false};
std::atomic<WarnSite*> g_warnings{nullptr};

struct VkEntry {
    std::atomic<u64> id_plus_one{0};
    std::atomic<u64> count{0};
    std::atomic<u32> ready{0};
    char text[VkMessages::kTextBytes] = {};
};
VkEntry g_vk[VkMessages::kEntries];
char g_diagnosis_text[256 * 1024];
char g_diagnosis_raw[256 * 1024];
std::atomic<u64> g_ticks_reads{0};
std::atomic<u64> g_vk_overflow{0};

static_assert(EvidenceGiven(kSteps), "every step without an in-game comparison names its evidence (rulebook D1)");

constexpr bool OneStepPerSwitch() {
    int step_rows = 0;
    for (const SwitchRow& row : kSwitchRows) step_rows += row.step;
    if (step_rows != kStepCount) return false;
    for (int i = 0; i < kStepCount; ++i) {
        if (!Row(kSteps[i].sw).step) return false;
        for (int j = i + 1; j < kStepCount; ++j) {
            if (kSteps[i].sw == kSteps[j].sw) return false;
        }
    }
    return true;
}
static_assert(OneStepPerSwitch(), "every Step switch row has exactly one step row and the reverse");

constexpr bool CompareRowsMatch() {
    for (const StepInfo& s : kSteps) {
        const SwitchRow& row = Row(s.sw);
        if (row.kind != SwitchKind::Mode || (row.flags & LANTERN_SW_LEGACY)) continue;
        const bool compares = s.check == CheckKind::Compare || s.check == CheckKind::Sampled;
        if (compares != (row.max == 2)) return false;
    }
    return true;
}
static_assert(CompareRowsMatch(), "mode 2 exists exactly for the migrated steps that have a comparison (rulebook D1)");

u32 CurrentTid() noexcept {
#ifdef _WIN32
    return u32(GetCurrentThreadId());
#else
    return u32(syscall(SYS_gettid));
#endif
}

void ReadThreadName(char (&out)[kNameWords * 8]) noexcept {
    std::memset(out, 0, sizeof out);
#ifdef _WIN32
    PWSTR wide = nullptr;
    if (SUCCEEDED(GetThreadDescription(GetCurrentThread(), &wide)) && wide) {
        for (std::size_t i = 0; i < sizeof out - 1 && wide[i]; ++i) out[i] = wide[i] < 128 ? char(wide[i]) : '?';
        LocalFree(wide);
    }
#else
    char name[32] = {};
    if (pthread_getname_np(pthread_self(), name, sizeof name) == 0) std::memcpy(out, name, sizeof out - 1);
#endif
    if (!out[0]) std::snprintf(out, sizeof out, "thread %u", CurrentTid());
}

void MakeEven(std::atomic<u64>& seq) noexcept {
    const u64 s = seq.load(std::memory_order_relaxed);
    if (s & 1u) seq.store(s + 1, std::memory_order_release);
}

void ClearCrumbs(ThreadSlot& slot) noexcept {
    Steps::SeqBegin(slot.crumb_seq);
    slot.now.store(0, std::memory_order_relaxed);
    slot.site.store(nullptr, std::memory_order_relaxed);
    slot.site_frame.store(0, std::memory_order_relaxed);
    slot.draw_steps.store(0, std::memory_order_relaxed);
    Steps::SeqEnd(slot.crumb_seq);
}

struct SlotGuard {
    bool armed = false;
    ~SlotGuard() {
        ThreadSlot* slot = t_slot;
        if (armed && slot >= g_slots && slot < g_slots + kMaxThreads) {
            ClearCrumbs(*slot);
            const u64 o = slot->owner.load(std::memory_order_relaxed);
            slot->owner.store((o & ~u64(0xffffffffu)) | kFreeTid, std::memory_order_release);
        }
        t_slot = &g_dead_slot;
    }
};
thread_local SlotGuard t_guard;

bool IsLive(StepId id) {
    return LivenessOf(id) == Liveness::Live;
}

double Hz() {
    return g_registry.ticks_per_second.load(std::memory_order_relaxed);
}

i64 NsToTicks(double ns) {
    return i64(ns * Hz() / 1e9);
}

void Recompute() noexcept {
    const bool on = g_registry.checks_on.load(std::memory_order_relaxed) &&
                    g_registry.calibrated.load(std::memory_order_acquire);
    const bool warm = g_registry.warm.load(std::memory_order_relaxed) != 0;
    const double period = double(g_flip.period_ns);
    const double cap_ns = period > 0 ? std::min(kCheckCapNs, period * kCheckCapShareOfFrame) : kCheckCapNs;
    const i64 earn = on ? NsToTicks(kEarnPerDrawNs) : 0;
    g_budget.cap.store(on ? NsToTicks(cap_ns / 2) : 0, std::memory_order_relaxed);
    g_budget.earn_first.store(warm ? earn / 2 : earn / 3, std::memory_order_relaxed);
    g_budget.earn_rate.store(warm ? earn - earn / 2 : earn - earn / 3, std::memory_order_relaxed);
    g_budget.earn_scope.store(earn / 4, std::memory_order_relaxed);
    g_budget.spill.store(warm ? 0u : 1u, std::memory_order_relaxed);
    for (int i = 0; i < kStepCount; ++i) {
        const bool heavy = kSteps[i].weight == Weight::Heavy;
        const u16 mask = warm ? (heavy ? kWarmMaskHeavy : kWarmMaskLight) : (heavy ? kSteadyMaskHeavy : kSteadyMaskLight);
        g_sample_mask[i].store(mask, std::memory_order_relaxed);
    }
}

void CopyName(const ThreadSlot& slot, char* out, std::size_t size) {
    char raw[kNameWords * 8 + 1] = {};
    for (int w = 0; w < kNameWords; ++w) {
        const u64 word = slot.name[w].load(std::memory_order_relaxed);
        std::memcpy(raw + 8 * w, &word, 8);
    }
    std::size_t n = 0;
    for (; n + 1 < size && raw[n]; ++n) out[n] = (raw[n] >= 32 && raw[n] < 127) ? raw[n] : '?';
    out[n] = 0;
}

void AppendStepName(char* out, std::size_t size, std::size_t& used, std::string_view name, bool& first) {
    const std::string_view sep = first ? "" : ",";
    first = false;
    for (std::string_view part : {sep, name}) {
        const std::size_t n = std::min(part.size(), size - 1 - std::min(used, size - 1));
        std::memcpy(out + used, part.data(), n);
        used += n;
    }
    out[used] = 0;
}

std::size_t BuildStepsOffLine(char* out, std::size_t size, u64 extra) {
    if (size == 0) return 0;
    std::size_t used = 0;
    out[0] = 0;
    const std::string_view head = "LANTERN_STEPS_OFF=";
    const std::size_t n = std::min(head.size(), size - 1);
    std::memcpy(out, head.data(), n);
    used = n;
    out[used] = 0;
    const u64 named = g_registry.steps_off_named;
    bool first = true;
    for (int i = 0; i < kStepCount; ++i) {
        if (named & (u64(1) << i)) AppendStepName(out, size, used, kSteps[i].name, first);
    }
    for (int i = 0; i < kStepCount; ++i) {
        if ((extra & ~named) & (u64(1) << i)) AppendStepName(out, size, used, kSteps[i].name, first);
    }
    std::string_view toggles = g_registry.toggles_named;
    while (!toggles.empty()) {
        const std::size_t comma = toggles.find(',');
        AppendStepName(out, size, used, toggles.substr(0, comma), first);
        toggles.remove_prefix(comma == std::string_view::npos ? toggles.size() : comma + 1);
    }
    return used;
}

const char* StateName(StepState s) {
    switch (s) {
    case StepState::Off: return "off";
    case StepState::On: return "on";
    case StepState::Compare: return "compare";
    case StepState::Disabled: return "DISABLED";
    case StepState::Unavailable: return "unavailable";
    }
    return "?";
}

void RecordFirst(ThreadSlot& slot, StepId id, const Evidence& e) noexcept {
    FirstDifference& f = g_registry.first[std::size_t(id)];
    const StepInfo& info = Info(id);
    const StepState state = Steps::State(id);
    const bool live = IsLive(id);
    const bool start_only = !live && state != StepState::Compare;
    const bool disable = live && (state == StepState::On || state == StepState::Off);
    if (start_only) g_registry.start_only_differed.fetch_or(u64(1) << u8(id), std::memory_order_acq_rel);
    if (f.claimed.exchange(1, std::memory_order_acq_rel) == 0) {
        char thread[kNameWords * 8 + 1] = "thread without a slot";
        u64 draw = 0, frame = g_frame.load(std::memory_order_relaxed), vs = 0, ps = 0, key = 0;
        if (!slot.shared) {
            CopyName(slot, thread, sizeof thread);
            const RecentDraw& r = slot.recent[slot.recent_head.load(std::memory_order_relaxed) % kRecentDraws];
            draw = r.draw.load(std::memory_order_relaxed);
            vs = r.vs.load(std::memory_order_relaxed);
            ps = r.ps.load(std::memory_order_relaxed);
            key = r.key.load(std::memory_order_relaxed);
        }
        const char* head = disable ? "Step off: " : "Step differs: ";
        const char* how = disable ? "first difference; off for the rest of the session"
                          : state == StepState::Compare  ? "compare mode; stays on"
                                                         : "cannot turn off live";
        std::snprintf(f.text, sizeof f.text,
                      "%s%.*s (%s): frame %llu draw %llu area %llu thread %s; vs %llx ps %llx key %llx; %s guest "
                      "%#llx+%#llx: new %#llx %#llx, old %#llx %#llx; extra %#llx",
                      head, int(info.name.size()), info.name.data(), how, (unsigned long long)frame,
                      (unsigned long long)draw, (unsigned long long)g_registry.area.load(std::memory_order_relaxed),
                      thread, (unsigned long long)vs, (unsigned long long)ps, (unsigned long long)key,
                      e.what ? e.what : "", (unsigned long long)e.address, (unsigned long long)e.size,
                      (unsigned long long)e.new_value[0], (unsigned long long)e.new_value[1],
                      (unsigned long long)e.old_value[0], (unsigned long long)e.old_value[1],
                      (unsigned long long)e.extra);
        if (start_only) {
            char line[kNextStartBytes - 64];
            BuildStepsOffLine(line, sizeof line, g_registry.start_only_differed.load(std::memory_order_acquire));
            std::snprintf(f.next_start, sizeof f.next_start, "Next start, copy this into Extra variables: %s", line);
        }
        f.frame.store(frame, std::memory_order_relaxed);
        f.ready.store(1, std::memory_order_release);
    }
    if (disable) {
        std::atomic<u8>& st = g_state[std::size_t(id)];
        u8 seen = st.load(std::memory_order_relaxed);
        while ((seen == u8(StepState::On) || seen == u8(StepState::Off)) &&
               !st.compare_exchange_weak(seen, u8(StepState::Disabled), std::memory_order_acq_rel,
                                         std::memory_order_relaxed)) {
        }
    }
}

bool TokenIsStep(std::string_view token, int* index) {
    for (int i = 0; i < kStepCount; ++i) {
        if (kSteps[i].name == token) {
            *index = i;
            return true;
        }
    }
    return false;
}

std::string_view TrimToken(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
    return s;
}

bool ParsePart(std::string_view token, int* k, int* n) {
    if (token.size() < 4 || token[0] != '@') return false;
    const std::size_t slash = token.find('/');
    if (slash == std::string_view::npos) return false;
    long a = 0, b = 0;
    char left[12] = {}, right[12] = {};
    const std::string_view l = token.substr(1, slash - 1), r = token.substr(slash + 1);
    if (l.empty() || r.empty() || l.size() >= sizeof left || r.size() >= sizeof right) return false;
    std::memcpy(left, l.data(), l.size());
    std::memcpy(right, r.data(), r.size());
    if (!lantern_parse_number(left, &a) || !lantern_parse_number(right, &b) || b < 1 || b > kMaxSteps || a < 1 || a > b) {
        return false;
    }
    *k = int(a);
    *n = int(b);
    return true;
}

void AddProblem(Steps::StartReport& r, std::string_view text) {
    const std::size_t used = std::strlen(r.problems);
    std::snprintf(r.problems + used, sizeof r.problems - used, "%s%.*s", used ? "; " : "", int(text.size()), text.data());
}

void ResolveStepsOff(const Switches& s, Steps::StartReport& report) {
    std::string_view list = s.steps_off;
    u64 named = 0;
    int part_k = 0, part_n = 0;
    while (!list.empty()) {
        const std::size_t comma = list.find(',');
        const std::string_view token = TrimToken(list.substr(0, comma));
        list.remove_prefix(comma == std::string_view::npos ? list.size() : comma + 1);
        if (token.empty()) continue;
        int index = 0, k = 0, n = 0;
        if (TokenIsStep(token, &index)) {
            named |= u64(1) << index;
        } else if (token.substr(0, 6) == "group:") {
            bool any = false;
            for (int i = 0; i < kStepCount; ++i) {
                if (kSteps[i].group == token.substr(6)) {
                    named |= u64(1) << i;
                    any = true;
                }
            }
            if (!any) AddProblem(report, token);
        } else if (token.substr(0, 7) == "toggle:") {
            const std::size_t used = std::strlen(g_registry.toggles_named);
            std::snprintf(g_registry.toggles_named + used, sizeof g_registry.toggles_named - used, "%s%.*s",
                          used ? "," : "", int(token.size()), token.data());
        } else if (token == "all-new") {
            for (int i = 0; i < kStepCount; ++i) {
                if (kSteps[i].since == Since::New) named |= u64(1) << i;
            }
        } else if (ParsePart(token, &k, &n)) {
            part_k = k;
            part_n = n;
        } else {
            AddProblem(report, token);
        }
    }
    if (part_n) {
        int on_index[kMaxSteps];
        int on_count = 0;
        for (int i = 0; i < kStepCount; ++i) {
            const StepState st = StepState(g_state[i].load(std::memory_order_relaxed));
            if ((st == StepState::On || st == StepState::Compare) && !(named & (u64(1) << i)) &&
                kSteps[i].wiring == Wiring::Wired) {
                on_index[on_count++] = i;
            }
        }
        for (int j = 0; j < on_count; ++j) {
            if (Decide::InPart(j, on_count, part_k, part_n)) named |= u64(1) << on_index[j];
        }
    }
    g_registry.steps_off_named = named;
    report.steps_off_named = named;
    std::snprintf(report.toggles, sizeof report.toggles, "%s", g_registry.toggles_named);
    for (int i = 0; i < kStepCount; ++i) {
        if (!(named & (u64(1) << i))) continue;
        if (kSteps[i].wiring != Wiring::Wired) {
            report.steps_off_not_switched |= u64(1) << i;
            continue;
        }
        g_state[i].store(u8(StepState::Off), std::memory_order_relaxed);
        report.off_why[i] = OffWhy::StepsOff;
    }
}

StepState StartStateOf(const Switches& s, int i) {
    const SwitchRow& row = Row(kSteps[i].sw);
    const long v = s.Number(kSteps[i].sw);
    switch (row.kind) {
    case SwitchKind::Text: return StepState::On;
    case SwitchKind::Enum: return v == 0 ? StepState::Off : v == 2 ? StepState::Compare : StepState::On;
    default: return v == 0 ? StepState::Off : v == 2 ? StepState::Compare : StepState::On;
    }
}

void LogF(const char* format, ...) __attribute__((format(printf, 1, 2)));
void LogF(const char* format, ...) {
    char line[kLineBytes];
    va_list args;
    va_start(args, format);
    const int n = std::vsnprintf(line, sizeof line, format, args);
    va_end(args);
    if (n < 0) return;
    LogLine(LogChannel::Steps, std::string_view(line, std::min<std::size_t>(std::size_t(n), sizeof line - 1)));
}

bool RunCalibration() {
    using Clock = std::chrono::steady_clock;
    const auto t0 = Clock::now();
    const u64 c0 = __rdtsc();
    std::this_thread::sleep_for(std::chrono::milliseconds(kCalibrationMs));
    const u64 c1 = __rdtsc();
    const auto t1 = Clock::now();
    const double ns = double(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
    if (ns <= 0 || c1 <= c0) return false;
    Steps::SetTicksPerSecond(double(c1 - c0) * 1e9 / ns);
    return true;
}

bool InvariantTsc() {
#ifdef _WIN32
    int regs[4] = {};
    __cpuid(regs, int(kCpuidPowerLeaf & 0x80000000u));
    if (u32(regs[0]) < kCpuidPowerLeaf) return false;
    __cpuid(regs, int(kCpuidPowerLeaf));
    return (u32(regs[3]) & kInvariantTscBit) != 0;
#else
    unsigned a = 0, b = 0, c = 0, d = 0;
    if (__get_cpuid_max(0x80000000u, nullptr) < kCpuidPowerLeaf) return false;
    __get_cpuid(kCpuidPowerLeaf, &a, &b, &c, &d);
    return (d & kInvariantTscBit) != 0;
#endif
}

void SumSlot(const ThreadSlot& slot, Steps::Totals& t) {
    for (int i = 0; i < kStepCount; ++i) {
        t.used[i] += slot.used[i].load(std::memory_order_relaxed);
        t.covered[i] += slot.covered[i].load(std::memory_order_relaxed);
        t.total[i] += slot.total[i].load(std::memory_order_relaxed);
        t.checked[i] += slot.checked[i].load(std::memory_order_relaxed);
        t.differ[i] += slot.differ[i].load(std::memory_order_relaxed);
        t.check_tsc[i] += slot.check_tsc[i].load(std::memory_order_relaxed);
        t.skipped_budget[i] += slot.skipped_budget[i].load(std::memory_order_relaxed);
        t.inconclusive[i] += slot.inconclusive[i].load(std::memory_order_relaxed);
        t.step_tsc[i] += slot.step_tsc[i].load(std::memory_order_relaxed);
        for (int r = 0; r < kMaxReasons; ++r) t.fell_back[i][r] += slot.fell_back[i][r].load(std::memory_order_relaxed);
    }
    t.draws += slot.refill_tsc.load(std::memory_order_relaxed);
    t.other_writers += slot.other_writers.load(std::memory_order_relaxed);
}

u64 Delta(u64 now, u64 before) {
    return now >= before ? now - before : 0;
}

const char* SuspectText(Decide::Suspect s) {
    switch (s) {
    case Decide::Suspect::NothingCovered: return "total above 0 and covered 0";
    case Decide::Suspect::CoveredButUnchecked: return "covered above 0 and checked 0";
    case Decide::Suspect::AllDiffer: return "every check differs";
    case Decide::Suspect::None: break;
    }
    return "";
}

bool Checking(StepState s) {
    return s == StepState::On || s == StepState::Compare;
}

}

alignas(kCacheLine) std::atomic<u8> g_state[kMaxSteps];
alignas(kCacheLine) std::atomic<u16> g_sample_mask[kMaxSteps];
Budget g_budget;
ThreadSlot g_slots[kMaxThreads];
constinit ThreadSlot g_dead_slot{true};
constinit ThreadSlot g_overflow_slot{true};
struct UnslottedCounts {
    std::atomic<u64> used[kMaxSteps];
    std::atomic<u64> covered[kMaxSteps];
    std::atomic<u64> total[kMaxSteps];
    std::atomic<u64> fell_back[kMaxSteps][kMaxReasons];
};
constinit UnslottedCounts g_unslotted{};
std::atomic<u64> g_threads_without_slot{0};
std::atomic<u64> g_frame{0};
constinit thread_local ThreadSlot* t_slot = nullptr;
constinit thread_local u32 t_tid = 0;

void Steps::Out::Put(std::string_view text) noexcept {
    if (size == 0) return;
    const std::size_t room = size - 1 - std::min(used, size - 1);
    const std::size_t n = std::min(room, text.size());
    if (n < text.size()) truncated = true;
    std::memcpy(buffer + used, text.data(), n);
    used += n;
    buffer[used] = 0;
}

void Steps::Out::Format(const char* format, ...) noexcept {
    char line[kLineBytes];
    va_list args;
    va_start(args, format);
    const int n = std::vsnprintf(line, sizeof line, format, args);
    va_end(args);
    if (n < 0) return;
    Put(std::string_view(line, std::min<std::size_t>(std::size_t(n), sizeof line - 1)));
}

ThreadSlot& Steps::ClaimSlot() noexcept {
    const u32 tid = CurrentTid();
    t_tid = tid;
    for (ThreadSlot& slot : g_slots) {
        u64 o = slot.owner.load(std::memory_order_relaxed);
        if (u32(o) != kFreeTid) continue;
        const u64 gen = o >> 32;
        if (!slot.owner.compare_exchange_strong(o, (gen << 32) | kClaimingTid, std::memory_order_acquire,
                                                std::memory_order_relaxed)) {
            continue;
        }
        std::atomic_thread_fence(std::memory_order_release);
        MakeEven(slot.crumb_seq);
        for (RecentDraw& r : slot.recent) MakeEven(r.seq);
        char name[kNameWords * 8];
        ReadThreadName(name);
        for (int w = 0; w < kNameWords; ++w) {
            u64 word = 0;
            std::memcpy(&word, name + 8 * w, 8);
            slot.name[w].store(word, std::memory_order_relaxed);
        }
        ClearCrumbs(slot);
        slot.recent_head.store(0, std::memory_order_relaxed);
        slot.recent_written.store(0, std::memory_order_relaxed);
        slot.draw_hash.store(Decide::kNoHash, std::memory_order_relaxed);
        slot.rate_tokens.store(0, std::memory_order_relaxed);
        slot.first_tokens.store(0, std::memory_order_relaxed);
        slot.draw_may_check.store(0, std::memory_order_relaxed);
        slot.vk_call.store(nullptr, std::memory_order_relaxed);
        slot.has_draws = false;
        slot.owner.store(((gen + 1) << 32) | tid, std::memory_order_release);
        t_slot = &slot;
        t_guard.armed = true;
        return slot;
    }
    t_slot = &g_overflow_slot;
    g_threads_without_slot.fetch_add(1, std::memory_order_relaxed);
    return g_overflow_slot;
}

void Steps::NoteOtherWriter(ThreadSlot& slot) noexcept {
    if (slot.shared) return;
    if (u32(slot.owner.load(std::memory_order_relaxed)) != t_tid) {
        slot.other_writers.fetch_add(1, std::memory_order_relaxed);
    }
}

u64 Steps::Ticks() noexcept {
    g_ticks_reads.fetch_add(1, std::memory_order_relaxed);
    if (g_registry.use_tsc.load(std::memory_order_relaxed)) return __rdtsc();
    return u64(std::chrono::duration_cast<std::chrono::nanoseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
                   .count());
}

bool Steps::SampleSlow(ThreadSlot& slot, StepId id, u32 hash, u32 mask) noexcept {
    if (!g_registry.checks_on.load(std::memory_order_relaxed) ||
        !g_registry.calibrated.load(std::memory_order_relaxed)) {
        return false;
    }
    const std::size_t i = std::size_t(id);
    const bool mask_hit = Decide::MaskHit(hash, mask);
    const bool tokens = slot.draw_may_check.load(std::memory_order_relaxed) != 0;
    bool min_share = false;
    if (!tokens && Decide::MinShareDraw(hash)) {
        const u64 second = g_registry.second.load(std::memory_order_relaxed);
        const Decide::MinShare m{second, slot.min_share_second[i].load(std::memory_order_relaxed),
                                 g_registry.frame_in_second.load(std::memory_order_relaxed),
                                 g_registry.frames_per_second.load(std::memory_order_relaxed), int(i)};
        if (Decide::MinShareAllowed(m)) {
            min_share = true;
            // written only by: the slot's owner thread
            slot.min_share_second[i].store(second, std::memory_order_relaxed);
        }
    }
    const Decide::Verdict v = Decide::SampleVerdict(mask_hit, tokens, min_share);
    if (v == Decide::Verdict::Starved) {
        // written only by: the slot's owner thread
        Bump(slot.skipped_budget[i]);
    }
    return v == Decide::Verdict::Check;
}

bool Steps::SampleNewSlow(ThreadSlot& slot, StepId id) noexcept {
    if (!g_registry.checks_on.load(std::memory_order_relaxed) ||
        !g_registry.calibrated.load(std::memory_order_relaxed)) {
        return false;
    }
    if (slot.first_tokens.load(std::memory_order_relaxed) > 0) return true;
    // written only by: the slot's owner thread
    Bump(slot.skipped_budget[std::size_t(id)]);
    return false;
}

void Steps::Checked(ThreadSlot& slot, StepId id, bool same, const Evidence& evidence, u64 tsc_spent,
                    CheckClass cls) noexcept {
    const std::size_t i = std::size_t(id);
    if (!slot.shared) {
        CheckWriter(slot);
        // written only by: the slot's owner thread
        Bump(slot.checked[i]);
        // written only by: the slot's owner thread
        if (!same) Bump(slot.differ[i]);
        // written only by: the slot's owner thread
        Bump(slot.check_tsc[i], tsc_spent);
        if (State(id) != StepState::Compare) {
            std::atomic<i64>& bucket = cls == CheckClass::FirstHit ? slot.first_tokens : slot.rate_tokens;
            // written only by: the slot's owner thread
            bucket.store(Decide::Spend(bucket.load(std::memory_order_relaxed), tsc_spent), std::memory_order_relaxed);
        }
    }
    if (!same) RecordFirst(slot, id, evidence);
}

bool Steps::Start(const Switches& switches) noexcept {
    g_started.store(true, std::memory_order_release);
    StartReport& report = g_registry.start_report;
    report = StartReport{};
    g_registry.steps_off_named = 0;
    g_registry.toggles_named[0] = 0;
    bool any_compare = false;
    for (int i = 0; i < kStepCount; ++i) {
        const StepState st = StartStateOf(switches, i);
        g_state[i].store(u8(st), std::memory_order_relaxed);
        report.off_why[i] = st != StepState::Off                       ? OffWhy::None
                            : switches.IsSet(kSteps[i].sw)             ? OffWhy::Switch
                                                                       : OffWhy::Default;
    }
    ResolveStepsOff(switches, report);
    for (int i = 0; i < kStepCount; ++i) {
        const u8 st = g_state[i].load(std::memory_order_relaxed);
        g_registry.start_state[i].store(st, std::memory_order_relaxed);
        any_compare |= st == u8(StepState::Compare);
    }
    g_registry.checks_on.store(switches.self_check ? 1u : 0u, std::memory_order_relaxed);
    g_budget.writer_check.store(any_compare ? 1u : 0u, std::memory_order_relaxed);
    Recompute();
    return true;
}

bool Steps::StartOnce() noexcept {
    std::call_once(g_start_once, [] {
        if (!g_started.load(std::memory_order_acquire)) Start(Switches::Load());
    });
    return true;
}

const Steps::StartReport& Steps::StartInfo() noexcept {
    return g_registry.start_report;
}

bool Steps::SetUnavailable(StepId id, const char* reason) noexcept {
    const std::size_t i = std::size_t(id);
    u8 expected = g_registry.start_state[i].load(std::memory_order_relaxed);
    g_registry.unavailable[i].store(reason, std::memory_order_relaxed);
    return g_state[i].compare_exchange_strong(expected, u8(StepState::Unavailable), std::memory_order_release,
                                              std::memory_order_relaxed);
}

const char* Steps::UnavailableReason(StepId id) noexcept {
    return g_registry.unavailable[std::size_t(id)].load(std::memory_order_relaxed);
}

bool Steps::SetFromMenu(StepId id, Mode wanted) noexcept {
    if (!IsLive(id)) return false;
    if (wanted == Mode::Compare && Row(Info(id).sw).max < 2) return false;
    std::atomic<u8>& state = g_state[std::size_t(id)];
    u8 seen = state.load(std::memory_order_relaxed);
    while (true) {
        if (seen == u8(StepState::Disabled) || seen == u8(StepState::Unavailable)) return false;
        if (state.compare_exchange_weak(seen, u8(wanted), std::memory_order_acq_rel, std::memory_order_relaxed)) {
            break;
        }
    }
    const std::string_view name = Info(id).name;
    LogF("Step %.*s turned %s from the menu at frame %llu", int(name.size()), name.data(),
         wanted == Mode::Off ? "off" : wanted == Mode::On ? "on" : "to compare",
         (unsigned long long)g_frame.load(std::memory_order_relaxed));
    return true;
}

Mode Steps::StartModeByName(std::string_view switch_name) noexcept {
    SwitchId sw{};
    if (!FindRow(switch_name, &sw)) return Mode::Off;
    StartOnce();
    for (int i = 0; i < kStepCount; ++i) {
        if (kSteps[i].sw == sw) return Get(StepId(i));
    }
    return Switches::Get().ModeOf(sw);
}

void Steps::SetTicksPerSecond(double hz) noexcept {
    g_registry.ticks_per_second.store(hz, std::memory_order_relaxed);
    g_registry.use_tsc.store(1, std::memory_order_relaxed);
    g_registry.calibrated.store(1, std::memory_order_release);
    Recompute();
}

void Steps::StartCalibration() noexcept {
    if (!InvariantTsc()) {
        g_registry.ticks_per_second.store(1e9, std::memory_order_relaxed);
        g_registry.use_tsc.store(0, std::memory_order_relaxed);
        g_registry.calibrated.store(1, std::memory_order_release);
        Recompute();
        LogF("Steps: no invariant TSC; check times use the steady clock");
        return;
    }
    try {
        std::thread([] {
            if (!RunCalibration()) LogF("Steps: TSC calibration failed; self-checks stay off");
        }).detach();
    } catch (const std::system_error&) {
        LogF("Steps: no thread for the TSC calibration; self-checks stay off");
    }
}

bool Steps::Calibrated() noexcept {
    return g_registry.calibrated.load(std::memory_order_acquire) != 0;
}

double Steps::TicksPerSecond() noexcept {
    return Hz();
}

void Steps::OnFlip(const FlipInput& in) noexcept {
    g_frame.fetch_add(1, std::memory_order_relaxed);
    FlipClock& c = g_flip;
    bool changed = false;
    if (!c.started) {
        c.started = true;
        c.second_start = in.now_ns;
        c.warm_until = in.now_ns + kWarmNs;
        c.next_reclaim = in.now_ns + kMinuteNs;
        g_registry.warm.store(1, std::memory_order_relaxed);
        changed = true;
    }
    if (in.loading_screen_up) {
        c.loading_seen = true;
    } else if (c.loading_seen) {
        c.loading_seen = false;
        c.warm_until = in.now_ns + kWarmNs;
        g_registry.area.fetch_add(1, std::memory_order_relaxed);
        if (!g_registry.warm.load(std::memory_order_relaxed)) {
            g_registry.warm.store(1, std::memory_order_relaxed);
            changed = true;
        }
    }
    if (g_registry.warm.load(std::memory_order_relaxed) && in.now_ns >= c.warm_until) {
        g_registry.warm.store(0, std::memory_order_relaxed);
        changed = true;
    }
    ++c.frames_this_second;
    g_registry.frame_in_second.store(c.frames_this_second, std::memory_order_relaxed);
    if (in.now_ns - c.second_start >= kSecondNs) {
        g_registry.frames_per_second.store(c.frames_this_second, std::memory_order_relaxed);
        g_registry.second.fetch_add(1, std::memory_order_relaxed);
        c.frames_this_second = 0;
        c.second_start = in.now_ns;
        c.period_ns = in.frame_period_ns;
        changed = true;
    }
    if (changed) Recompute();
    if (in.now_ns >= c.next_reclaim) {
        c.next_reclaim = in.now_ns + kMinuteNs;
        ReclaimDead(ThreadAlive);
    }
}

u64 Steps::Area() noexcept {
    return g_registry.area.load(std::memory_order_relaxed);
}

bool Steps::WarmWindow() noexcept {
    return g_registry.warm.load(std::memory_order_relaxed) != 0;
}

void Steps::Sum(Totals& out) noexcept {
    out = Totals{};
    for (const ThreadSlot& slot : g_slots) SumSlot(slot, out);
    SumSlot(g_overflow_slot, out);
    for (int i = 0; i < kStepCount; ++i) {
        out.used[i] += g_unslotted.used[i].load(std::memory_order_relaxed);
        out.covered[i] += g_unslotted.covered[i].load(std::memory_order_relaxed);
        out.total[i] += g_unslotted.total[i].load(std::memory_order_relaxed);
        for (int r = 0; r < kMaxReasons; ++r) out.fell_back[i][r] += g_unslotted.fell_back[i][r].load(std::memory_order_relaxed);
    }
}

void Steps::AddUnslotted(StepId id, u64 total, u64 covered, int reason, u64 fell_back, u64 used) noexcept {
    const std::size_t i = std::size_t(id);
    if (i >= std::size_t(kMaxSteps) || reason < 0 || reason >= kMaxReasons) return;
    if (total) g_unslotted.total[i].fetch_add(total, std::memory_order_relaxed);
    if (covered) g_unslotted.covered[i].fetch_add(covered, std::memory_order_relaxed);
    if (fell_back) g_unslotted.fell_back[i][reason].fetch_add(fell_back, std::memory_order_relaxed);
    if (used) g_unslotted.used[i].fetch_add(used, std::memory_order_relaxed);
}

Steps::SlotRead Steps::ReadSlot(const ThreadSlot& slot, SlotView& out) noexcept {
    out = SlotView{};
    const u64 o1 = slot.owner.load(std::memory_order_acquire);
    const u32 tid = u32(o1);
    if (tid == kFreeTid || tid == kClaimingTid) return SlotRead::Free;
    out.tid = tid;
    CopyName(slot, out.name, sizeof out.name);
    const u64 s1 = slot.crumb_seq.load(std::memory_order_acquire);
    out.now = slot.now.load(std::memory_order_relaxed);
    out.site = slot.site.load(std::memory_order_relaxed);
    out.site_frame = slot.site_frame.load(std::memory_order_relaxed);
    out.draw_steps = slot.draw_steps.load(std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_acquire);
    out.crumbs_changing = (s1 & 1u) || slot.crumb_seq.load(std::memory_order_relaxed) != s1;
    const u32 written = slot.recent_written.load(std::memory_order_acquire);
    const u32 head = slot.recent_head.load(std::memory_order_relaxed);
    for (int k = 0; k < int(written); ++k) {
        const RecentDraw& r = slot.recent[(head - u32(k)) % kRecentDraws];
        auto& v = out.recent[k];
        const u64 r1 = r.seq.load(std::memory_order_acquire);
        v.draw = r.draw.load(std::memory_order_relaxed);
        v.frame = r.frame.load(std::memory_order_relaxed);
        v.vs = r.vs.load(std::memory_order_relaxed);
        v.ps = r.ps.load(std::memory_order_relaxed);
        v.key = r.key.load(std::memory_order_relaxed);
        v.steps = r.steps.load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        v.seq = r1;
        v.changing = (r1 & 1u) || r.seq.load(std::memory_order_relaxed) != r1;
        if (r1 == 0) break;
        ++out.recent_count;
    }
    std::atomic_thread_fence(std::memory_order_acquire);
    const u64 o2 = slot.owner.load(std::memory_order_relaxed);
    return o1 == o2 ? SlotRead::Ok : SlotRead::Changed;
}

Steps::SlotRead Steps::ReadSlot(int index, SlotView& out) noexcept {
    if (index < 0 || index >= kMaxThreads) return SlotRead::Free;
    return ReadSlot(g_slots[index], out);
}

bool Steps::ThreadAlive(u32 tid) noexcept {
#ifdef _WIN32
    HANDLE thread = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, tid);
    if (!thread) return true;
    DWORD code = STILL_ACTIVE;
    const BOOL ok = GetExitCodeThread(thread, &code);
    CloseHandle(thread);
    return !ok || code == STILL_ACTIVE;
#else
    if (syscall(SYS_tgkill, getpid(), pid_t(tid), 0) == 0) return true;
    return errno != ESRCH;
#endif
}

int Steps::ReclaimDead(bool (*alive)(u32 tid)) noexcept {
    int reclaimed = 0;
    for (ThreadSlot& slot : g_slots) {
        u64 o = slot.owner.load(std::memory_order_acquire);
        const u32 tid = u32(o);
        if (tid == kFreeTid || tid == kClaimingTid || alive(tid)) continue;
        if (slot.owner.compare_exchange_strong(o, (o & ~u64(0xffffffffu)) | kFreeTid, std::memory_order_release,
                                               std::memory_order_relaxed)) {
            ++reclaimed;
        }
    }
    return reclaimed;
}

void Steps::ResetForTest() noexcept {
    for (ThreadSlot& slot : g_slots) {
        slot.owner.store(0, std::memory_order_relaxed);
        for (int i = 0; i < kMaxSteps; ++i) {
            slot.used[i].store(0, std::memory_order_relaxed);
            slot.covered[i].store(0, std::memory_order_relaxed);
            slot.total[i].store(0, std::memory_order_relaxed);
            slot.checked[i].store(0, std::memory_order_relaxed);
            slot.differ[i].store(0, std::memory_order_relaxed);
            slot.check_tsc[i].store(0, std::memory_order_relaxed);
            slot.skipped_budget[i].store(0, std::memory_order_relaxed);
            slot.inconclusive[i].store(0, std::memory_order_relaxed);
            slot.step_tsc[i].store(0, std::memory_order_relaxed);
            slot.min_share_second[i].store(0, std::memory_order_relaxed);
            for (auto& r : slot.fell_back[i]) r.store(0, std::memory_order_relaxed);
        }
        slot.refill_tsc.store(0, std::memory_order_relaxed);
        slot.other_writers.store(0, std::memory_order_relaxed);
    }
    for (int i = 0; i < kMaxSteps; ++i) {
        g_unslotted.used[i].store(0, std::memory_order_relaxed);
        g_unslotted.covered[i].store(0, std::memory_order_relaxed);
        g_unslotted.total[i].store(0, std::memory_order_relaxed);
        for (auto& r : g_unslotted.fell_back[i]) r.store(0, std::memory_order_relaxed);
    }
    for (FirstDifference& f : g_registry.first) {
        f.claimed.store(0, std::memory_order_relaxed);
        f.ready.store(0, std::memory_order_relaxed);
        f.text[0] = 0;
        f.next_start[0] = 0;
        f.printed = false;
    }
    g_registry.start_only_differed.store(0, std::memory_order_relaxed);
    g_threads_without_slot.store(0, std::memory_order_relaxed);
    g_registry.area.store(0, std::memory_order_relaxed);
    g_flip = FlipClock{};
    g_registry.calibrated.store(0, std::memory_order_relaxed);
    g_registry.use_tsc.store(0, std::memory_order_relaxed);
    g_registry.ticks_per_second.store(1e9, std::memory_order_relaxed);
    g_registry.second.store(1, std::memory_order_relaxed);
    g_registry.frames_per_second.store(0, std::memory_order_relaxed);
    g_registry.frame_in_second.store(0, std::memory_order_relaxed);
    g_registry.warm.store(1, std::memory_order_relaxed);
    g_warnings.store(nullptr, std::memory_order_relaxed);
    for (VkEntry& e : g_vk) {
        e.id_plus_one.store(0, std::memory_order_relaxed);
        e.count.store(0, std::memory_order_relaxed);
        e.ready.store(0, std::memory_order_relaxed);
    }
    g_vk_overflow.store(0, std::memory_order_relaxed);
    g_minute = MinuteState{};
    g_frame.store(0, std::memory_order_relaxed);
    t_slot = nullptr;
}

std::size_t Steps::FormatStepsOffLine(char* out, std::size_t size) noexcept {
    return BuildStepsOffLine(out, size, g_registry.start_only_differed.load(std::memory_order_acquire));
}

u64 Steps::TicksReads() noexcept {
    return g_ticks_reads.load(std::memory_order_relaxed);
}

bool Steps::HoldFirstDifferenceForTest(StepId id) noexcept {
    return g_registry.first[std::size_t(id)].claimed.exchange(1, std::memory_order_acq_rel) == 0;
}

u64 Steps::StartOnlyDiffered() noexcept {
    return g_registry.start_only_differed.load(std::memory_order_acquire);
}

bool Steps::FirstDifferenceClaimed(StepId id) noexcept {
    return g_registry.first[std::size_t(id)].claimed.load(std::memory_order_acquire) != 0;
}

bool Steps::FirstDifferenceReady(StepId id) noexcept {
    return g_registry.first[std::size_t(id)].ready.load(std::memory_order_acquire) != 0;
}

bool Steps::FirstDifferenceText(StepId id, char* out, std::size_t size) noexcept {
    const FirstDifference& f = g_registry.first[std::size_t(id)];
    if (size == 0) return false;
    if (!f.claimed.load(std::memory_order_acquire)) {
        out[0] = 0;
        return false;
    }
    if (!f.ready.load(std::memory_order_acquire)) {
        std::snprintf(out, size, "%.*s: being written", int(Info(id).name.size()), Info(id).name.data());
        return true;
    }
    std::snprintf(out, size, "%s%s%s", f.text, f.next_start[0] ? ". " : "", f.next_start);
    return true;
}

void Steps::PrintStepsAtStart() noexcept {
    char line[kLineBytes];
    Out o{line, sizeof line};
    const StartReport& r = g_registry.start_report;
    const char* const heads[] = {"Steps on: ", "Steps compare (=2): ", "Steps off: ", "Steps unavailable: "};
    const StepState states[] = {StepState::On, StepState::Compare, StepState::Off, StepState::Unavailable};
    for (int h = 0; h < 4; ++h) {
        o.used = 0;
        o.Put(heads[h]);
        bool any = false;
        for (int i = 0; i < kStepCount; ++i) {
            if (State(StepId(i)) != states[h]) continue;
            o.Put(any ? ", " : "");
            any = true;
            o.Put(kSteps[i].name);
            if (states[h] == StepState::Off) {
                o.Put(r.off_why[i] == OffWhy::StepsOff ? " (LANTERN_STEPS_OFF)"
                      : r.off_why[i] == OffWhy::Switch ? " (=0)"
                                                       : " (default)");
            }
            if (states[h] == StepState::Unavailable) {
                const char* why = UnavailableReason(StepId(i));
                o.Format(" (%s)", why ? why : "no reason given");
            }
            if (kSteps[i].wiring == Wiring::Registry) o.Put(" [registry only]");
        }
        if (!any) o.Put("none");
        LogLine(LogChannel::Steps, std::string_view(o.buffer, o.used));
    }
    if (r.steps_off_not_switched) {
        o.used = 0;
        o.Put("Steps: LANTERN_STEPS_OFF names steps this build does not switch yet (they keep running): ");
        bool first = true;
        for (int i = 0; i < kStepCount; ++i) {
            if (!(r.steps_off_not_switched & (u64(1) << i))) continue;
            o.Put(first ? "" : ", ");
            first = false;
            o.Put(kSteps[i].name);
        }
        LogLine(LogChannel::Steps, std::string_view(o.buffer, o.used));
    }
    if (r.toggles[0]) LogF("Steps: LANTERN_STEPS_OFF toggle entries are kept for the bisect line but not applied by this build: %s", r.toggles);
    if (r.problems[0]) LogF("Steps: LANTERN_STEPS_OFF entries not understood, ignored: %s", r.problems);
    LogF("Steps self-check: 1 in %u (heavy 1 in %u) and every new memo entry for %llu s after each load, then 1 in %u "
         "(heavy 1 in %u); at most %.0f us of checks per thread per frame (LANTERN_SELF_CHECK=0 turns the checks off; "
         "counters stay)%s",
         kWarmMaskLight + 1u, kWarmMaskHeavy + 1u, (unsigned long long)(kWarmNs / kSecondNs), kSteadyMaskLight + 1u,
         kSteadyMaskHeavy + 1u, kCheckCapNs / 1000.0,
         g_registry.checks_on.load(std::memory_order_relaxed) ? "" : "; checks are OFF in this run");
}

void Steps::PrintNewFirstDifferences() noexcept {
    for (int i = 0; i < kStepCount; ++i) {
        FirstDifference& f = g_registry.first[i];
        if (f.printed || !f.ready.load(std::memory_order_acquire)) continue;
        f.printed = true;
        LogScrubbed(LogChannel::Steps, f.text);
        if (f.next_start[0]) LogScrubbed(LogChannel::Steps, f.next_start);
    }
}

void Steps::PrintMinuteTable(double seconds, bool at_exit) noexcept {
    Totals now;
    Sum(now);
    const Totals& last = g_minute.last;
    const double secs = seconds > 0 ? seconds : 1.0;
    const double hz = Hz();
    LogF("Steps | %s, frame %llu, area %llu | used/s | fell back/s (top reason) | covered/total | checked | differ | "
         "skipped | check ms/s",
         at_exit ? "session" : "1 min", (unsigned long long)g_frame.load(std::memory_order_relaxed),
         (unsigned long long)Area());
    u64 all_uses = 0;
    double check_ms = 0;
    for (int i = 0; i < kStepCount; ++i) {
        const StepState st = State(StepId(i));
        const u64 used = at_exit ? now.used[i] : Delta(now.used[i], last.used[i]);
        all_uses += used;
        if (st == StepState::Off && now.used[i] == 0 && now.total[i] == 0 && now.checked[i] == 0) continue;
        u64 fell = 0, top = 0;
        int top_reason = 0;
        for (int r = 0; r < kMaxReasons; ++r) {
            const u64 d = at_exit ? now.fell_back[i][r] : Delta(now.fell_back[i][r], last.fell_back[i][r]);
            fell += d;
            if (d > top) {
                top = d;
                top_reason = r;
            }
        }
        const double ms = hz > 0 ? double(Delta(now.check_tsc[i], at_exit ? 0 : last.check_tsc[i])) * 1e3 / hz / secs : 0;
        check_ms += ms;
        const std::string_view reason = kSteps[i].reasons[top_reason];
        LogF("Steps | %-22.*s %-11s | %8.0f | %8.0f (%.*s) | %llu/%llu | %llu%s | %llu | %llu | %.3f",
             int(kSteps[i].name.size()), kSteps[i].name.data(), StateName(st), double(used) / secs,
             double(fell) / secs, int(top ? reason.size() : 4), top ? reason.data() : "none",
             (unsigned long long)now.covered[i], (unsigned long long)now.total[i], (unsigned long long)now.checked[i],
             Unverified(kSteps[i]) ? " unverified" : "", (unsigned long long)now.differ[i],
             (unsigned long long)now.skipped_budget[i], ms);
        const Decide::Suspect s =
            Decide::InstrumentSuspect(Checking(st) && !Unverified(kSteps[i]), now.covered[i], now.total[i], now.checked[i],
                                      now.differ[i]);
        if (s != Decide::Suspect::None) {
            LogF("Steps | INSTRUMENT SUSPECT %.*s: %s", int(kSteps[i].name.size()), kSteps[i].name.data(),
                 SuspectText(s));
        }
    }
    const u64 draws = at_exit ? now.draws : Delta(now.draws, last.draws);
    const double hot_ms = double(all_uses + draws) * kColdBenchNsPerCall / 1e6 / secs;
    LogF("Steps | diagnostics: hot path %.3f ms/s (estimate: calls x %.1f ns, the cold bench's worst thread per call; "
         "not measured in play), checks %.3f ms/s (measured)",
         hot_ms, kColdBenchNsPerCall, check_ms);
    if (const u64 without = g_threads_without_slot.load(std::memory_order_relaxed)) {
        LogF("Steps | totals are lower bounds: %llu threads had no slot", (unsigned long long)without);
    }
    if (now.other_writers) {
        LogF("Steps | other writers: %llu counter writes from a thread that does not own the slot",
             (unsigned long long)now.other_writers);
    }
    g_minute.last = now;
}

void Diag::Warn(WarnSite& site, const Evidence& evidence) noexcept {
    site.count.fetch_add(1, std::memory_order_relaxed);
    if (site.claimed.exchange(1, std::memory_order_acq_rel) != 0) return;
    site.first = evidence;
    site.first_frame = g_frame.load(std::memory_order_relaxed);
    if (ThreadSlot* slot = Steps::PeekSlot(); slot && !slot->shared) {
        CopyName(*slot, site.first_thread, sizeof site.first_thread);
        site.first_draw =
            slot->recent[slot->recent_head.load(std::memory_order_relaxed) % kRecentDraws].draw.load(std::memory_order_relaxed);
    } else {
        std::snprintf(site.first_thread, sizeof site.first_thread, "thread without a slot");
    }
    WarnSite* head = g_warnings.load(std::memory_order_relaxed);
    do {
        site.next = head;
    } while (!g_warnings.compare_exchange_weak(head, &site, std::memory_order_release, std::memory_order_relaxed));
    site.ready.store(1, std::memory_order_release);
}

WarnSite* Diag::Warnings() noexcept {
    return g_warnings.load(std::memory_order_acquire);
}

void Diag::PrintWarnings() noexcept {
    for (WarnSite* s = Warnings(); s; s = s->next) {
        const u64 count = s->count.load(std::memory_order_relaxed);
        if (count == s->shown) continue;
        LogF("Warn | %llux (first frame %llu, %s, draw %llu): %s", (unsigned long long)(count - s->shown),
             (unsigned long long)s->first_frame, s->first_thread, (unsigned long long)s->first_draw, s->text);
        s->shown = count;
    }
}

bool VkMessages::Note(i32 message_id, const char* text) noexcept {
    const u64 stored = u64(u32(message_id)) + 1u;
    u32 slot = (u32(stored) * 0x9E3779B1u) >> 24;
    for (int probe = 0; probe < kEntries; ++probe, slot = (slot + 1) % kEntries) {
        VkEntry& e = g_vk[slot];
        u64 seen = e.id_plus_one.load(std::memory_order_acquire);
        if (seen == 0) {
            if (e.id_plus_one.compare_exchange_strong(seen, stored, std::memory_order_acq_rel)) {
                std::snprintf(e.text, sizeof e.text, "%s", text ? text : "");
                e.ready.store(1, std::memory_order_release);
                e.count.fetch_add(1, std::memory_order_relaxed);
                return true;
            }
        }
        if (seen == stored) {
            e.count.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
    }
    g_vk_overflow.fetch_add(1, std::memory_order_relaxed);
    return false;
}

u64 VkMessages::Count(i32 message_id) noexcept {
    const u64 stored = u64(u32(message_id)) + 1u;
    u32 slot = (u32(stored) * 0x9E3779B1u) >> 24;
    for (int probe = 0; probe < kEntries; ++probe, slot = (slot + 1) % kEntries) {
        const u64 seen = g_vk[slot].id_plus_one.load(std::memory_order_acquire);
        if (seen == 0) return 0;
        if (seen == stored) return g_vk[slot].count.load(std::memory_order_relaxed);
    }
    return 0;
}

u64 VkMessages::Overflow() noexcept {
    return g_vk_overflow.load(std::memory_order_relaxed);
}

namespace {

void WriteThreadLine(Steps::Out& o, const Steps::SlotView& v) {
    o.Format("thread %s: ", v.name);
    const u8 step = NowStep(v.now);
    const CrumbPhase phase = NowPhase(v.now);
    if (v.crumbs_changing) {
        o.Put("(changing)");
    } else if (step < kStepCount && phase != CrumbPhase::Idle) {
        o.Format("%s %.*s %s, frame %llu", phase == CrumbPhase::In ? "in" : "left", int(kSteps[step].name.size()),
                 kSteps[step].name.data(), v.site ? v.site : "", (unsigned long long)v.site_frame);
    } else if (NowKind(v.now) == CrumbKind::Draw) {
        o.Format("draw %llu frame %llu", (unsigned long long)NowNumber(v.now), (unsigned long long)v.site_frame);
    } else if (NowKind(v.now) == CrumbKind::Packet) {
        o.Format("packet %llu", (unsigned long long)NowNumber(v.now));
    } else {
        o.Put("idle");
    }
    if (v.recent_count) {
        o.Put("; recent draws:");
        for (u32 k = 0; k < v.recent_count && k < 3; ++k) {
            const auto& r = v.recent[k];
            if (r.changing) {
                o.Put(" (changing)");
                continue;
            }
            o.Format(" %llu vs %llx ps %llx key %llx steps %#llx;", (unsigned long long)r.draw, (unsigned long long)r.vs,
                     (unsigned long long)r.ps, (unsigned long long)r.key, (unsigned long long)r.steps);
        }
    }
    o.Put("\n");
}

void WriteAttention(Steps::Out& o) {
    o.Put("[attention]\n");
    int shown = 0, more = 0;
    for (int i = 0; i < kStepCount; ++i) {
        const StepState st = Steps::State(StepId(i));
        const bool start_only_differed = (Steps::StartOnlyDiffered() >> i) & 1u;
        if (st != StepState::Disabled && !start_only_differed) continue;
        if (shown == kAttentionMax) {
            ++more;
            continue;
        }
        ++shown;
        char text[kFirstTextBytes + kNextStartBytes];
        Steps::FirstDifferenceText(StepId(i), text, sizeof text);
        if (st == StepState::Disabled) {
            o.Format("STEP TURNED ITSELF OFF: %.*s at frame %llu: %s. The rest of the session ran without it.\n",
                     int(kSteps[i].name.size()), kSteps[i].name.data(),
                     (unsigned long long)g_registry.first[i].frame.load(std::memory_order_relaxed), text);
        } else {
            char line[kNextStartBytes];
            Steps::FormatStepsOffLine(line, sizeof line);
            o.Format("STEP DIFFERED (cannot turn off live): %.*s at frame %llu: %s. Next start, copy this into Extra "
                     "variables: %s\n",
                     int(kSteps[i].name.size()), kSteps[i].name.data(),
                     (unsigned long long)g_registry.first[i].frame.load(std::memory_order_relaxed), g_registry.first[i].text,
                     line);
        }
    }
    if (more) o.Format("and %d more\n", more);
    if (!shown) o.Put("none\n");
}

void WriteSteps(Steps::Out& o) {
    Steps::Totals t;
    Steps::Sum(t);
    o.Put("[steps]\nname | switch | state | used | covered/total | fell back (by reason) | checked | differ | skipped | "
          "inconclusive | check ms\n");
    const Switches& s = Switches::Get();
    for (int i = 0; i < kStepCount; ++i) {
        const StepInfo& info = kSteps[i];
        char value[80];
        FormatSwitchValue(value, sizeof value, s, info.sw);
        const SwitchRow& row = Row(info.sw);
        o.Format("%.*s | %.*s=%s | %s%s | %llu | %llu/%llu | ", int(info.name.size()), info.name.data(),
                 int(row.name.size()), row.name.data(), value, StateName(Steps::State(StepId(i))),
                 info.wiring == Wiring::Registry ? " (registry only)" : "", (unsigned long long)t.used[i],
                 (unsigned long long)t.covered[i], (unsigned long long)t.total[i]);
        bool any = false;
        for (int r = 0; r < kMaxReasons; ++r) {
            if (!t.fell_back[i][r]) continue;
            o.Format("%s%.*s %llu", any ? ", " : "", int(info.reasons[r].size()), info.reasons[r].data(),
                     (unsigned long long)t.fell_back[i][r]);
            any = true;
        }
        if (!any) o.Put("none");
        const double hz = Hz();
        o.Format(" | %llu%s | %llu | %llu | %llu | %.1f\n", (unsigned long long)t.checked[i],
                 Unverified(info) ? " unverified" : "", (unsigned long long)t.differ[i],
                 (unsigned long long)t.skipped_budget[i], (unsigned long long)t.inconclusive[i],
                 hz > 0 ? double(t.check_tsc[i]) * 1e3 / hz : 0.0);
        const Decide::Suspect sus = Decide::InstrumentSuspect(Checking(Steps::State(StepId(i))) && !Unverified(info), t.covered[i], t.total[i],
                                                              t.checked[i], t.differ[i]);
        if (sus != Decide::Suspect::None) {
            o.Format("INSTRUMENT SUSPECT %.*s: %s\n", int(info.name.size()), info.name.data(), SuspectText(sus));
        }
    }
    if (const u64 without = g_threads_without_slot.load(std::memory_order_relaxed)) {
        o.Format("totals are lower bounds: %llu threads had no slot\n", (unsigned long long)without);
    }
    if (t.other_writers) o.Format("other writers: %llu\n", (unsigned long long)t.other_writers);
}

}

void Diagnosis::Write(Steps::Out& clean, const Inputs& in) noexcept {
    Steps::Out o{g_diagnosis_raw, sizeof g_diagnosis_raw};
    WriteRaw(o, in);
    clean.used = Scrub::Text(std::string_view(o.buffer, o.used), clean.buffer, clean.size);
    clean.truncated = o.truncated || clean.used + 1 >= clean.size;
}

void Diagnosis::WriteRaw(Steps::Out& o, const Inputs& in) noexcept {
    o.Put("lantern-diagnosis 1\n");
    o.Format("session: pid %ld, started %s\n", in.pid, in.started);
    WriteAttention(o);
    o.Format("[build]\nengine_id: %s\nos: %s\n", in.engine_id, in.os);
    o.Format("[session]\nended: %s\nminutes: %llu.%llu\nframes: %llu\nareas: %llu\nfps: %llu.%llu average\n", in.ended,
             (unsigned long long)(in.minutes_tenths / 10), (unsigned long long)(in.minutes_tenths % 10),
             (unsigned long long)in.frames, (unsigned long long)in.areas, (unsigned long long)(in.fps_tenths / 10),
             (unsigned long long)(in.fps_tenths % 10));
    o.Put("[switches]\n");
    const Switches& s = Switches::Get();
    for (std::size_t i = 0; i < kSwitchCount; ++i) {
        if (!s.IsSet(SwitchId(i))) continue;
        char value[80];
        FormatSwitchValue(value, sizeof value, s, SwitchId(i));
        o.Format("%.*s=%s\n", int(kSwitchRows[i].name.size()), kSwitchRows[i].name.data(), value);
    }
    WriteSteps(o);
    o.Put("[first-differences]\n");
    bool any = false;
    for (int i = 0; i < kStepCount; ++i) {
        char text[kFirstTextBytes + kNextStartBytes];
        if (!Steps::FirstDifferenceText(StepId(i), text, sizeof text)) continue;
        o.Format("%s\n", text);
        any = true;
    }
    if (!any) o.Put("none\n");
    o.Put("[unverified]\n");
    for (const StepInfo& info : kSteps) {
        if (!Unverified(info)) continue;
        o.Format("%.*s | %s | %.*s\n", int(info.name.size()), info.name.data(),
                 StateName(Steps::State(StepId(&info - kSteps))), int(info.evidence.size()), info.evidence.data());
    }
    o.Put("[warnings]\n");
    any = false;
    for (WarnSite* w = Diag::Warnings(); w; w = w->next) {
        if (!w->ready.load(std::memory_order_acquire)) continue;
        o.Format("%llu | first frame %llu %s draw %llu | %s\n", (unsigned long long)w->count.load(std::memory_order_relaxed),
                 (unsigned long long)w->first_frame, w->first_thread, (unsigned long long)w->first_draw, w->text);
        any = true;
    }
    if (!any) o.Put("none\n");
    o.Put("[vulkan]\n");
    any = false;
    for (const VkEntry& e : g_vk) {
        if (!e.ready.load(std::memory_order_acquire)) continue;
        o.Format("%llu | id %d | %s\n", (unsigned long long)e.count.load(std::memory_order_relaxed),
                 i32(e.id_plus_one.load(std::memory_order_relaxed) - 1u), e.text);
        any = true;
    }
    if (VkMessages::Overflow()) o.Format("overflow: %llu\n", (unsigned long long)VkMessages::Overflow());
    if (!any) o.Put("none\n");
    o.Put("[triage]\nnot written at a clean exit\n[threads]\n");
    for (int i = 0; i < kMaxThreads; ++i) {
        Steps::SlotView v;
        const Steps::SlotRead r = Steps::ReadSlot(i, v);
        if (r == Steps::SlotRead::Free) continue;
        if (r == Steps::SlotRead::Changed) {
            o.Put("(thread changed while read)\n");
            continue;
        }
        WriteThreadLine(o, v);
    }
}

void Diagnosis::Path(char* out, std::size_t size, const char* file) noexcept {
    const std::string_view log = Switches::Get().log_file;
    const std::size_t slash = log.find_last_of("/\\");
    if (slash != std::string_view::npos && slash + std::strlen(file) + 2 < size) {
        std::snprintf(out, size, "%.*s%c%s", int(slash), log.data(), log[slash], file);
    } else if (const char* dir = std::getenv("BB_LOG_DIR"); dir && *dir) {
        std::snprintf(out, size, "%s/%s", dir, file);
    } else {
        std::snprintf(out, size, "logs/%s", file);
    }
}

bool Diagnosis::PrepareFile() noexcept {
    char path[1024], previous[1024], tmp[1024];
    Path(path, sizeof path, "diagnosis.txt");
    Path(previous, sizeof previous, "previous_diagnosis.txt");
    Path(tmp, sizeof tmp, "diagnosis.txt.tmp");
    std::remove(tmp);
#ifdef _WIN32
    const bool moved = MoveFileExA(path, previous, MOVEFILE_REPLACE_EXISTING) != 0;
    const bool missing = !moved && GetLastError() == ERROR_FILE_NOT_FOUND;
#else
    const bool moved = std::rename(path, previous) == 0;
    const bool missing = !moved && errno == ENOENT;
#endif
    if (!moved && !missing) {
        LogF("Diagnosis: the previous diagnosis.txt could not be renamed; it will be replaced at exit");
        return false;
    }
    return true;
}

bool Diagnosis::WriteFile(const Inputs& inputs) noexcept {
    char* const text = g_diagnosis_text;
    Steps::Out o{text, sizeof g_diagnosis_text};
    Write(o, inputs);
    char path[1024], tmp[1024];
    Path(path, sizeof path, "diagnosis.txt");
    Path(tmp, sizeof tmp, "diagnosis.txt.tmp");
    std::FILE* file = std::fopen(tmp, "wb");
    if (!file) {
        LogF("Diagnosis: could not create the diagnosis file; nothing written");
        return false;
    }
    const bool written = std::fwrite(text, 1, o.used, file) == o.used;
    const bool flushed = std::fflush(file) == 0;
    const bool closed = std::fclose(file) == 0;
    if (!written || !flushed || !closed) {
        std::remove(tmp);
        LogF("Diagnosis: writing the diagnosis file failed; nothing written");
        return false;
    }
#ifdef _WIN32
    const bool replaced = MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING) != 0;
#else
    const bool replaced = std::rename(tmp, path) == 0;
#endif
    if (!replaced) {
        LogF("Diagnosis: logs\\diagnosis.txt.tmp (could not replace diagnosis.txt)");
        return false;
    }
    LogF("Diagnosis: logs\\diagnosis.txt%s", o.truncated ? " (truncated)" : "");
    return true;
}

}
