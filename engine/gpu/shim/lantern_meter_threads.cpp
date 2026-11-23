// SPDX-License-Identifier: GPL-3.0-or-later
#include "lantern_meter_threads.h"
#include <algorithm>
#include <cstdio>
#include <cstring>

#ifdef _WIN32
#include <windows.h>
#include <tlhelp32.h>
#else
#include <dirent.h>
#include <pthread.h>
#include <sched.h>
#include <unistd.h>
#include <cstdlib>
#endif

namespace Lantern::Meter::Threads {
namespace {
constexpr const char* kMainName = "main";
constexpr const char* kRenderName = "GXRenderThread";
constexpr const char* kWorkerPrefix = "GXWorker";
constexpr std::size_t kThreadName = 64;

#ifdef _WIN32
constexpr u32 kGroupCpus = 64;  // a Windows processor group
constexpr u64 kFileTimeNs = 100;

u64 FileTimeTicks(const FILETIME& t) {
    return (u64(t.dwHighDateTime) << 32) | t.dwLowDateTime;
}

bool Describe(HANDLE thread, char* out, std::size_t room) {
    PWSTR wide = nullptr;
    if (FAILED(GetThreadDescription(thread, &wide)) || !wide) return false;
    const int n = WideCharToMultiByte(CP_UTF8, 0, wide, -1, out, int(room), nullptr, nullptr);
    LocalFree(wide);
    if (n <= 0) return false;
    out[room - 1] = '\0';
    return true;
}

bool ReadCpuNs(u32, void* handle, u64* ns) {
    DWORD code = 0;
    if (!GetExitCodeThread(static_cast<HANDLE>(handle), &code) || code != STILL_ACTIVE) return false;
    FILETIME creation, exit, kernel, user;
    if (!GetThreadTimes(static_cast<HANDLE>(handle), &creation, &exit, &kernel, &user)) return false;
    *ns = (FileTimeTicks(kernel) + FileTimeTicks(user)) * kFileTimeNs;
    return true;
}
#else
bool ReadSmallFile(const char* path, char* out, std::size_t room) {
    FILE* file = std::fopen(path, "r");
    if (!file) return false;
    const std::size_t n = std::fread(out, 1, room - 1, file);
    const bool failed = std::ferror(file) != 0;
    std::fclose(file);
    if (failed) return false;
    out[n] = '\0';
    return true;
}

bool ReadCpuNs(u32 tid, void*, u64* ns) {
    char path[64], text[1024];
    std::snprintf(path, sizeof path, "/proc/self/task/%u/stat", tid);
    if (!ReadSmallFile(path, text, sizeof text)) return false;
    const char* p = std::strrchr(text, ')');
    if (!p) return false;
    constexpr int kFieldsBeforeUtime = 12;
    for (int field = 0; field < kFieldsBeforeUtime; ++field) {
        p = std::strchr(p + 1, ' ');
        if (!p) return false;
    }
    char* end = nullptr;
    const unsigned long long utime = std::strtoull(p + 1, &end, 10);
    if (end == p + 1) return false;
    const unsigned long long stime = std::strtoull(end, nullptr, 10);
    const long hz = sysconf(_SC_CLK_TCK);
    if (hz <= 0) return false;
    *ns = u64(utime + stime) * (1'000'000'000ull / u64(hz));
    return true;
}
#endif
}

bool Wanted(const char* name, Kind* kind) {
    if (!name || !name[0]) return false;
    if (std::strcmp(name, kMainName) == 0) {
        *kind = Kind::Main;
        return true;
    }
    if (std::strcmp(name, kRenderName) == 0) {
        *kind = Kind::Render;
        return true;
    }
    if (std::strncmp(name, kWorkerPrefix, std::strlen(kWorkerPrefix)) == 0) {
        *kind = Kind::Worker;
        return true;
    }
    return false;
}

#ifdef _WIN32
u32 CurrentCpu() {
    PROCESSOR_NUMBER number{};
    GetCurrentProcessorNumberEx(&number);
    return u32(number.Group) * kGroupCpus + number.Number;
}

u32 CurrentTid() {
    return u32(GetCurrentThreadId());
}

bool CurrentThreadName(char* out, std::size_t room) {
    if (!room) return false;
    out[0] = '\0';
    return Describe(GetCurrentThread(), out, room);
}

std::vector<int> CoreOfCpu() {
    std::vector<int> core_of;
    DWORD bytes = 0;
    GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &bytes);
    if (!bytes) return core_of;
    std::vector<unsigned char> buffer(bytes);
    auto* info = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buffer.data());
    if (!GetLogicalProcessorInformationEx(RelationProcessorCore, info, &bytes)) return core_of;
    int core = 0;
    for (DWORD at = 0; at < bytes;) {
        const auto* entry = reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buffer.data() + at);
        if (!entry->Size) break;
        if (entry->Relationship == RelationProcessorCore) {
            for (WORD g = 0; g < entry->Processor.GroupCount; ++g) {
                const GROUP_AFFINITY& group = entry->Processor.GroupMask[g];
                for (u32 bit = 0; bit < kGroupCpus; ++bit) {
                    if (!(group.Mask & (KAFFINITY(1) << bit))) continue;
                    const u32 cpu = u32(group.Group) * kGroupCpus + bit;
                    if (core_of.size() <= cpu) core_of.resize(cpu + 1, -1);
                    core_of[cpu] = core;
                }
            }
            ++core;
        }
        at += entry->Size;
    }
    return core_of;
}

static void CloseEntry(void* handle, void* sample_handle) {
    CloseHandle(static_cast<HANDLE>(handle));
    if (sample_handle) CloseHandle(static_cast<HANDLE>(sample_handle));
}

GuestThreads::~GuestThreads() {
    for (Entry& e : entries) CloseEntry(e.handle, e.sample_handle);
}

bool GuestThreads::SampleMain(u64* rip, u64* stopped_ns) {
    for (Entry& e : entries) {
        if (e.kind != Kind::Main || !e.sample_handle) continue;
        const HANDLE thread = static_cast<HANDLE>(e.sample_handle);
        LARGE_INTEGER before{}, after{}, frequency{};
        QueryPerformanceCounter(&before);
        if (SuspendThread(thread) == static_cast<DWORD>(-1)) return false;
        CONTEXT context{};
        context.ContextFlags = CONTEXT_CONTROL;
        const bool read = GetThreadContext(thread, &context) != 0;
        ResumeThread(thread);
        QueryPerformanceCounter(&after);
        QueryPerformanceFrequency(&frequency);
        if (!read) return false;
        *rip = context.Rip;
        *stopped_ns = frequency.QuadPart ? u64(double(after.QuadPart - before.QuadPart) * 1e9 / double(frequency.QuadPart)) : 0;
        return true;
    }
    return false;
}

bool GuestThreads::Refresh() {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return false;
    const DWORD pid = GetCurrentProcessId();
    for (Entry& e : entries) e.seen = false;
    std::vector<u32> still_rejected;
    THREADENTRY32 thread{};
    thread.dwSize = sizeof(thread);
    for (BOOL ok = Thread32First(snapshot, &thread); ok; ok = Thread32Next(snapshot, &thread)) {
        if (thread.th32OwnerProcessID != pid) continue;
        const u32 tid = thread.th32ThreadID;
        const auto known = std::find_if(entries.begin(), entries.end(), [&](const Entry& e) { return e.tid == tid; });
        if (known != entries.end()) {
            known->seen = true;
            continue;
        }
        if (std::find(rejected.begin(), rejected.end(), tid) != rejected.end()) {
            still_rejected.push_back(tid);
            continue;
        }
        HANDLE handle = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, tid);
        if (!handle) continue;
        char name[kThreadName] = {};
        Kind kind{};
        if (Describe(handle, name, sizeof name) && Wanted(name, &kind)) {
            void* sample_handle = kind == Kind::Main
                                      ? OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, tid)
                                      : nullptr;
            entries.push_back({tid, kind, handle, sample_handle, 0, false, true});
        } else {
            CloseHandle(handle);
            if (name[0]) still_rejected.push_back(tid);
        }
    }
    CloseHandle(snapshot);
    rejected.swap(still_rejected);
    for (auto it = entries.begin(); it != entries.end();) {
        if (!it->seen) {
            CloseEntry(it->handle, it->sample_handle);
            it = entries.erase(it);
        } else {
            ++it;
        }
    }
    return true;
}

#else
u32 CurrentTid() {
    return u32(gettid());
}

u32 CurrentCpu() {
    const int cpu = sched_getcpu();
    return cpu < 0 ? kUnknownCpu : u32(cpu);
}

bool CurrentThreadName(char* out, std::size_t room) {
    if (!room) return false;
    out[0] = '\0';
    return pthread_getname_np(pthread_self(), out, room) == 0;
}

std::vector<int> CoreOfCpu() {
    return {};
}

GuestThreads::~GuestThreads() = default;

bool GuestThreads::SampleMain(u64*, u64*) {
    return false;
}

bool GuestThreads::Refresh() {
    DIR* dir = opendir("/proc/self/task");
    if (!dir) return false;
    for (Entry& e : entries) e.seen = false;
    std::vector<u32> still_rejected;
    while (const dirent* d = readdir(dir)) {
        char* end = nullptr;
        const unsigned long value = std::strtoul(d->d_name, &end, 10);
        if (end == d->d_name || *end) continue;
        const u32 tid = u32(value);
        const auto known = std::find_if(entries.begin(), entries.end(), [&](const Entry& e) { return e.tid == tid; });
        if (known != entries.end()) {
            known->seen = true;
            continue;
        }
        if (std::find(rejected.begin(), rejected.end(), tid) != rejected.end()) {
            still_rejected.push_back(tid);
            continue;
        }
        char path[64], name[kThreadName] = {};
        std::snprintf(path, sizeof path, "/proc/self/task/%u/comm", tid);
        if (!ReadSmallFile(path, name, sizeof name)) continue;
        if (char* newline = std::strchr(name, '\n')) *newline = '\0';
        Kind kind{};
        if (Wanted(name, &kind)) {
            entries.push_back({tid, kind, nullptr, nullptr, 0, false, true});
        } else if (name[0]) {
            still_rejected.push_back(tid);
        }
    }
    closedir(dir);
    rejected.swap(still_rejected);
    std::erase_if(entries, [](const Entry& e) { return !e.seen; });
    return true;
}

#endif

Times GuestThreads::Take() {
    Times t;
    for (Entry& e : entries) {
        u64 now = 0;
        if (!ReadCpuNs(e.tid, e.handle, &now)) {
            ++t.gone;
            continue;
        }
        const u64 spent = e.primed && now >= e.last_ns ? now - e.last_ns : 0;
        const bool counted = e.primed;
        e.last_ns = now;
        e.primed = true;
        if (!counted) continue;
        switch (e.kind) {
        case Kind::Main:
            t.main_found = true;
            t.main_ns += spent;
            break;
        case Kind::Render:
            t.render_found = true;
            t.render_ns += spent;
            break;
        case Kind::Worker:
            ++t.workers;
            t.worker_sum_ns += spent;
            t.worker_max_ns = std::max(t.worker_max_ns, spent);
            break;
        }
    }
    return t;
}
}
