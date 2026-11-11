// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#include <cstdint>
#ifndef _WIN32
#include <pthread.h>
#include <execinfo.h>
#include <sys/resource.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>
#include <cstring>
extern "C" int runtime_memory_vma_info(std::uintptr_t address, int* prot, int* type, std::uintptr_t* end);
#endif

namespace BbPlatform {

struct Memory {
    std::uint64_t private_bytes = 0, working_set = 0, peak_working_set = 0;
    std::uint64_t free_phys = 0, total_phys = 0, commit_left = 0;
};

struct MemoryBreakdown {
    std::uint64_t game_pool = 0, mapped = 0, images = 0, priv = 0, shared = 0;
};

struct Usage {
    std::uint64_t user_us = 0, sys_us = 0, invol_switches = 0, vol_switches = 0, minor_faults = 0;
};

#ifdef _WIN32
bool GetUsage(bool thread, Usage& out);
bool GetMemory(Memory& out);
bool GetMemoryBreakdown(MemoryBreakdown& out);
std::uint32_t CurrentThreadId();
int ThreadCpuClock();
std::uint64_t ReadThreadCpuClockNs(int clock);
void DescribeAddress(const void* address, char* out, std::size_t size);
std::size_t CaptureStack(void** frames, std::size_t count);
bool ReadProcessMemory(const void* source, void* destination, std::size_t size);
[[noreturn]] void TerminateProcessNow(unsigned code);
#else
[[noreturn]] inline void TerminateProcessNow(unsigned code) {
    _exit(static_cast<int>(code));
}
inline bool GetMemory(Memory&) {
    return false;
}
inline bool GetMemoryBreakdown(MemoryBreakdown&) {
    return false;
}

inline bool GetUsage(bool thread, Usage& out) {
    rusage usage{};
    if (getrusage(thread ? RUSAGE_THREAD : RUSAGE_SELF, &usage) != 0) {
        return false;
    }
    out.user_us = std::uint64_t(usage.ru_utime.tv_sec) * 1000000 + usage.ru_utime.tv_usec;
    out.sys_us = std::uint64_t(usage.ru_stime.tv_sec) * 1000000 + usage.ru_stime.tv_usec;
    out.invol_switches = usage.ru_nivcsw;
    out.vol_switches = usage.ru_nvcsw;
    out.minor_faults = usage.ru_minflt;
    return true;
}

inline std::uint32_t CurrentThreadId() {
    return static_cast<std::uint32_t>(gettid());
}

inline std::size_t CaptureStack(void** frames, std::size_t count) {
    return static_cast<std::size_t>(backtrace(frames, static_cast<int>(count)));
}

inline bool ReadProcessMemory(const void* source, void* destination, std::size_t size) {
    iovec local{destination, size}, remote{const_cast<void*>(source), size};
    if (process_vm_readv(getpid(), &local, 1, &remote, 1, 0) == static_cast<ssize_t>(size)) {
        return true;
    }
    int prot = 0, type = 0;
    std::uintptr_t end = 0;
    const auto at = reinterpret_cast<std::uintptr_t>(source);
    if (!at || !runtime_memory_vma_info(at, &prot, &type, &end) || end < at + size) {
        return false;
    }
    std::memcpy(destination, source, size);
    return true;
}

inline int ThreadCpuClock() {
    if (clockid_t clock; pthread_getcpuclockid(pthread_self(), &clock) == 0) {
        return static_cast<int>(clock);
    }
    return -1;
}

inline std::uint64_t ReadThreadCpuClockNs(int clock) {
    timespec ts{};
    clock_gettime(static_cast<clockid_t>(clock), &ts);
    return std::uint64_t(ts.tv_sec) * 1000000000ull + std::uint64_t(ts.tv_nsec);
}
#endif

}
