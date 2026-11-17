// SPDX-License-Identifier: GPL-3.0-or-later
#include "bbport_sampler.h"

#ifdef _WIN32
#include <windows.h>
#include <tlhelp32.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include "bbport_platform.h"
#include "engine/bottleneck_meter.h"
#include "lantern_meter.h"
#include "lantern_meter_threads.h"

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

namespace BbSampler {
namespace {
constexpr unsigned long long kGuestLo = 0x800000000ull, kGuestHi = 0x810000000ull;

using Site = std::pair<unsigned long long, unsigned long long>;

enum class Role { Other, Main, StageA, StageB, Recorder, Count };
constexpr std::size_t kRoles = static_cast<std::size_t>(Role::Count);
constexpr const char* kRoleNames[kRoles] = {"", "main", "stage A", "stage B", "recorders"};

Role RoleOf(const std::string& name) {
    if (name == "main") return Role::Main;
    if (name == "shadPS4:GpuCommandProcessor") return Role::StageA;
    if (name == "bb:DrawRec") return Role::StageB;
    if (name.rfind("bb:VkRecorder", 0) == 0) return Role::Recorder;
    return Role::Other;
}

struct Target {
    HANDLE handle = nullptr;
    std::string name;
    Role role = Role::Other;
    unsigned long long samples = 0;
    std::map<Site, unsigned long long> sites;
};

struct Placement {
    std::vector<int> core_of;
    unsigned long long main_busy = 0;
    unsigned long long both[kRoles] = {};
    unsigned long long sibling[kRoles] = {};
    unsigned long long same[kRoles] = {};
};

struct TickSample {
    bool busy = false;
    unsigned cpu = 0;
};

std::pair<unsigned long long, unsigned long long> WaitCode() {
    static const auto range = [] {
        const auto base = reinterpret_cast<unsigned long long>(GetModuleHandleW(L"ntdll.dll"));
        if (!base) return std::pair{0ull, 0ull};
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        return std::pair{base, base + nt->OptionalHeader.SizeOfImage};
    }();
    return range;
}

int CoreOf(const Placement& p, unsigned cpu) {
    return cpu < p.core_of.size() ? p.core_of[cpu] : -1;
}

bool Counted(std::size_t role) {
    return role != static_cast<std::size_t>(Role::Main) && role != static_cast<std::size_t>(Role::Other);
}

void CountPlacement(Placement& p, const TickSample& main, const TickSample (&role)[kRoles]) {
    if (!main.busy) return;
    ++p.main_busy;
    const int main_core = CoreOf(p, main.cpu);
    for (std::size_t r = 0; r < kRoles; ++r) {
        if (!Counted(r) || !role[r].busy) continue;
        ++p.both[r];
        if (role[r].cpu == main.cpu) ++p.same[r];
        else if (main_core >= 0 && CoreOf(p, role[r].cpu) == main_core) ++p.sibling[r];
    }
}

constexpr std::size_t kPlacementLine = 1024;

void ReportPlacement(Placement& p) {
    if (!p.main_busy) return;
    char line[kPlacementLine];
    BbEngine::BottleneckMeter::Writer out{line, sizeof line};
    line[0] = '\0';
    out.Put("Placement: main busy in %llu samples;", p.main_busy);
    for (std::size_t r = 0; r < kRoles; ++r) {
        if (!Counted(r)) continue;
        if (!p.both[r]) {
            out.Put(" %s not busy with it,", kRoleNames[r]);
            continue;
        }
        out.Put(" %s busy with it %.0f%%, on main's SMT sibling %.0f%% of those (same CPU, a stale reading, %.0f%%),",
                kRoleNames[r], 100.0 * double(p.both[r]) / double(p.main_busy),
                100.0 * double(p.sibling[r]) / double(p.both[r]), 100.0 * double(p.same[r]) / double(p.both[r]));
    }
    out.Put(" CPU numbers as each thread last noted them");
    Lantern::LogLine(Lantern::LogChannel::Always, line);
    p.main_busy = 0;
    std::fill(std::begin(p.both), std::end(p.both), 0ull);
    std::fill(std::begin(p.sibling), std::end(p.sibling), 0ull);
    std::fill(std::begin(p.same), std::end(p.same), 0ull);
}

std::vector<std::string> Wanted() {
    std::vector<std::string> names;
    const char* env = std::getenv("BB_SAMPLE_THREADS");
    std::string list = env ? env : "";
    if (list == "load") list = "main,GXRenderThread,GXWorker,CSChrThread,Core.Res.,shadPS4:GpuCommandProcessor,bb:DrawRec,bb:VkRecorder";
    size_t start = 0;
    for (;;) {
        const size_t comma = list.find(',', start);
        std::string name = list.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
        if (!name.empty()) names.push_back(name);
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    return names;
}

std::string Describe(HANDLE thread) {
    PWSTR wide = nullptr;
    std::string name;
    if (SUCCEEDED(GetThreadDescription(thread, &wide)) && wide) {
        char text[128] = {};
        WideCharToMultiByte(CP_UTF8, 0, wide, -1, text, sizeof(text) - 1, nullptr, nullptr);
        name = text;
        LocalFree(wide);
    }
    return name;
}

constexpr long long kDefaultPeriodUs = 2000;
constexpr long long kTimerUnitsPerUs = 10;

constexpr DWORD kAccess = THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION;

void Refresh(const std::vector<std::string>& wanted, std::map<DWORD, Target>& targets) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return;
    const DWORD pid = GetCurrentProcessId(), self = GetCurrentThreadId();
    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    std::vector<DWORD> seen;
    for (BOOL ok = Thread32First(snapshot, &entry); ok; ok = Thread32Next(snapshot, &entry)) {
        if (entry.th32OwnerProcessID != pid || entry.th32ThreadID == self) continue;
        seen.push_back(entry.th32ThreadID);
        HANDLE thread = OpenThread(kAccess, FALSE, entry.th32ThreadID);
        if (!thread) continue;
        bool want = false;
        if (!targets.count(entry.th32ThreadID)) {
            const std::string name = Describe(thread);
            for (const auto& w : wanted) want |= !name.empty() && name.rfind(w, 0) == 0;
            if (want) targets[entry.th32ThreadID] = Target{thread, name, RoleOf(name)};
        }
        if (!want) CloseHandle(thread);
    }
    CloseHandle(snapshot);
    for (auto it = targets.begin(); it != targets.end();) {
        if (std::find(seen.begin(), seen.end(), it->first) == seen.end()) {
            CloseHandle(it->second.handle);
            it = targets.erase(it);
        } else {
            ++it;
        }
    }
}

constexpr unsigned long long kHostCaller = 1ull << 63;
std::pair<unsigned long long, unsigned long long> EngineCode() {
    static const auto range = [] {
        const auto base = reinterpret_cast<unsigned long long>(GetModuleHandleW(nullptr));
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        const unsigned long long start = base + nt->OptionalHeader.BaseOfCode;
        return std::pair{start, start + nt->OptionalHeader.SizeOfCode};
    }();
    return range;
}

bool Sample(Target& t, Site* site, bool* busy) {
    if (SuspendThread(t.handle) == static_cast<DWORD>(-1)) return false;
    CONTEXT context{};
    context.ContextFlags = CONTEXT_CONTROL;
    unsigned long long rip = 0, caller = 0;
    if (GetThreadContext(t.handle, &context)) {
        rip = context.Rip;
        if (rip < kGuestLo || rip >= kGuestHi) {
            unsigned long long stack[192] = {};
            if (BbPlatform::ReadProcessMemory(reinterpret_cast<const void*>(context.Rsp), stack, sizeof(stack))) {
                for (const auto value : stack) {
                    if (value >= kGuestLo && value < kGuestHi) {
                        caller = value - kGuestLo;
                        break;
                    }
                }
                const auto [code, code_end] = EngineCode();
                if (!caller && (rip < code || rip >= code_end)) {
                    for (const auto value : stack) {
                        if (value >= code && value < code_end) {
                            caller = value | kHostCaller;
                            break;
                        }
                    }
                }
            }
        }
    }
    ResumeThread(t.handle);
    if (!rip) return false;
    const auto [wait_start, wait_end] = WaitCode();
    *busy = rip < wait_start || rip >= wait_end;
    *site = rip >= kGuestLo && rip < kGuestHi ? Site{rip - kGuestLo, ~0ull} : Site{rip, caller};
    return true;
}

std::string Module(unsigned long long address, bool with_offset) {
    char text[512];
    BbPlatform::DescribeAddress(reinterpret_cast<const void*>(address), text, sizeof(text));
    std::string s = text;
    const size_t slash = s.find_last_of("\\/");
    if (slash != std::string::npos) s = s.substr(slash + 1);
    if (!with_offset) {
        const size_t plus = s.find('+');
        if (plus != std::string::npos) s = s.substr(0, plus);
    }
    return s;
}

void Report(std::map<DWORD, Target>& targets) {
    static const size_t top = [] {
        const char* env = std::getenv("BB_SAMPLE_TOP");
        return env ? std::max<size_t>(1, std::strtoull(env, nullptr, 10)) : size_t{8};
    }();
    for (auto& [id, t] : targets) {
        if (!t.samples) continue;
        std::map<std::string, unsigned long long> modules;
        unsigned long long guest = 0;
        for (const auto& [site, count] : t.sites) {
            if (site.second == ~0ull) guest += count;
            else modules[Module(site.first, false)] += count;
        }
        std::vector<std::pair<Site, unsigned long long>> hot(t.sites.begin(), t.sites.end());
        std::sort(hot.begin(), hot.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
        std::vector<std::pair<std::string, unsigned long long>> mods(modules.begin(), modules.end());
        std::sort(mods.begin(), mods.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
        std::printf("Samples %s (%llu): game code %.0f%%", t.name.c_str(), t.samples, 100.0 * guest / t.samples);
        for (size_t i = 0; i < mods.size() && i < 5; ++i)
            std::printf(", %s %.0f%%", mods[i].first.c_str(), 100.0 * mods[i].second / t.samples);
        std::printf("; hottest:");
        for (size_t i = 0; i < hot.size() && i < top; ++i) {
            const auto& [site, count] = hot[i];
            const double share = 100.0 * count / t.samples;
            const int digits = top > 8 ? 2 : 0;
            if (site.second == ~0ull)
                std::printf(" +%#llx %.*f%%", site.first, digits, share);
            else if (site.second & kHostCaller)
                std::printf(" %s (from %s) %.*f%%", Module(site.first, true).c_str(),
                            Module(site.second & ~kHostCaller, true).c_str(), digits, share);
            else
                std::printf(" %s (from +%#llx) %.*f%%", Module(site.first, true).c_str(), site.second,
                            digits, share);
        }
        std::printf("\n");
        t.samples = 0;
        t.sites.clear();
    }
    std::fflush(stdout);
}

void Run(std::vector<std::string> wanted) {
    std::map<DWORD, Target> targets;
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    auto last_refresh = std::chrono::steady_clock::now() - std::chrono::seconds(10);
    auto last_report = std::chrono::steady_clock::now();
    const long hz = Lantern::Switches::Get().bb_sample_hz;
    const long long period_us = hz > 0 ? 1000000 / hz : kDefaultPeriodUs;
    std::printf("Sampler: BB_SAMPLE_THREADS=%s, every %lld us (BB_SAMPLE_HZ %ld), reported every 5 s\n",
                std::getenv("BB_SAMPLE_THREADS"), period_us, hz);
    Placement placement;
    placement.core_of = Lantern::Meter::Threads::CoreOfCpu();
    {
        int cores = 0;
        for (const int core : placement.core_of) cores = std::max(cores, core + 1);
        Lantern::Log(Lantern::LogChannel::Always, "Placement: %zu logical CPUs in %d cores", placement.core_of.size(), cores);
    }
    for (;;) {
        const auto now = std::chrono::steady_clock::now();
        if (now - last_refresh >= std::chrono::seconds(2)) {
            Refresh(wanted, targets);
            last_refresh = now;
        }
        TickSample main_tick, role_tick[kRoles];
        for (auto& [id, t] : targets) {
            Site site;
            bool busy = false;
            if (!Sample(t, &site, &busy)) continue;
            ++t.samples;
            ++t.sites[site];
            if (t.role == Role::Other || !busy) continue;
            TickSample& tick = t.role == Role::Main ? main_tick : role_tick[static_cast<std::size_t>(t.role)];
            unsigned cpu = 0;
            if (!tick.busy && Lantern::Meter::CpuOfThread(id, &cpu)) {
                tick.busy = true;
                tick.cpu = cpu;
            }
        }
        CountPlacement(placement, main_tick, role_tick);
        if (now - last_report >= std::chrono::seconds(5)) {
            Report(targets);
            ReportPlacement(placement);
            last_report = now;
        }
        if (timer) {
            LARGE_INTEGER due;
            due.QuadPart = -period_us * kTimerUnitsPerUs;
            if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) WaitForSingleObject(timer, INFINITE);
        } else {
            std::this_thread::sleep_for(std::chrono::microseconds(period_us));
        }
    }
}
}

void Start() {
    static bool started = false;
    if (started) return;
    auto wanted = Wanted();
    if (wanted.empty()) return;
    started = true;
    std::thread(Run, std::move(wanted)).detach();
}
}
#else
namespace BbSampler {
void Start() {}
}
#endif
