// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LANTERN_PROCESS_ALIVE_H
#define LANTERN_PROCESS_ALIVE_H
#ifdef _WIN32
#ifndef _WINDOWS_
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#else
#include <errno.h>
#include <signal.h>
#include <sys/types.h>
#endif

static inline int lantern_process_alive(unsigned long long pid) {
    if (pid == 0) return 0;
#ifdef _WIN32
    if (pid > 0xFFFFFFFFull) return 0;
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, (DWORD)pid);
    if (!process) return GetLastError() == ERROR_ACCESS_DENIED;
    DWORD code = 0;
    int alive = GetExitCodeProcess(process, &code) && code == STILL_ACTIVE;
    CloseHandle(process);
    return alive;
#else
    if (pid > 0x7FFFFFFFull) return 0;
    return !kill((pid_t)pid, 0) || errno == EPERM;
#endif
}
#endif
