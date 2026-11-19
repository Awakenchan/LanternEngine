// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "sys.h"
#include "runtime.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifndef _WIN32
#include <sys/random.h>
#include <sys/resource.h>
#endif

#define PAGE 16384

typedef struct {
    int64_t seconds, microseconds;
} GuestTimeval;

typedef struct {
    uint32_t bits[4];
} GuestSigset;

typedef struct {
    GuestTimeval user, system;
    int64_t rest[14];
} GuestRusage;

static _Thread_local GuestSigset signal_mask;
static uintptr_t handlers[128];

int32_t runtime_guest_errno(int e) {
    switch (e) {
    case 0: return 0;
    case EPERM: return 1;
    case ENOENT: return 2;
    case ESRCH: return 3;
    case EINTR: return 4;
    case EIO: return 5;
    case ENXIO: return 6;
    case E2BIG: return 7;
    case ENOEXEC: return 8;
    case EBADF: return 9;
    case ECHILD: return 10;
    case EDEADLK: return 11;
    case ENOMEM: return 12;
    case EACCES: return 13;
    case EFAULT: return 14;
    case EBUSY: return 16;
    case EEXIST: return 17;
    case EXDEV: return 18;
    case ENODEV: return 19;
    case ENOTDIR: return 20;
    case EISDIR: return 21;
    case EINVAL: return 22;
    case ENFILE: return 23;
    case EMFILE: return 24;
    case ENOTTY: return 25;
    case EFBIG: return 27;
    case ENOSPC: return 28;
    case ESPIPE: return 29;
    case EROFS: return 30;
    case EMLINK: return 31;
    case EPIPE: return 32;
    case ERANGE: return 34;
    case EAGAIN: return 35;
    case ETIMEDOUT: return 60;
    case ELOOP: return 62;
    case ENAMETOOLONG: return 63;
    case ENOTEMPTY: return 66;
    case ENOSYS: return 78;
    case EOVERFLOW: return 84;
    case ECANCELED: return 85;
    default: return 5;
    }
}

static int32_t fail_posix(int e) {
    *runtime_errno() = runtime_guest_errno(e);
    return -1;
}

static SYS_ABI int32_t get_pagesize(void) {
    return PAGE;
}

static SYS_ABI int32_t get_pid(void) {
    return 1000;
}

static SYS_ABI int32_t yield(void) {
    host_yield();
    return 0;
}

static SYS_ABI __attribute__((noreturn)) void hard_exit(int status) {
    printf("Runtime: guest requested _exit(%d)\n", status);
    runtime_flush_settings();
    sys_report();
    fflush(stdout);
    _exit(status);
}

static SYS_ABI __attribute__((noreturn)) void raise_exception(uint32_t code, uint64_t argument) {
    fprintf(stderr, "STOP: guest raised debug exception code=0x%x argument=0x%llx\n", code,
            (unsigned long long)argument);
    sys_report();
    exit(23);
}

static SYS_ABI int32_t print_backtrace(void) {
    fputs("Runtime: guest requested a backtrace (not available)\n", stderr);
    return 0;
}

static SYS_ABI uintptr_t guest_signal(int sig, uintptr_t handler) {
    if (sig <= 0 || sig >= 128) {
        *runtime_errno() = 22;
        return (uintptr_t)-1;
    }
    uintptr_t old = handlers[sig];
    handlers[sig] = handler;
    printf("Runtime: guest signal(%d) handler recorded (host signals are not forwarded)\n", sig);
    return old;
}

static SYS_ABI int32_t guest_sigprocmask(int how, const GuestSigset *set, GuestSigset *old) {
    if (old) *old = signal_mask;
    if (!set) return 0;
    for (int i = 0; i < 4; ++i) {
        if (how == 1) signal_mask.bits[i] |= set->bits[i];
        else if (how == 2) signal_mask.bits[i] &= ~set->bits[i];
        else if (how == 3) signal_mask.bits[i] = set->bits[i];
        else return fail_posix(EINVAL);
    }
    return 0;
}

static SYS_ABI int32_t guest_sigfillset(GuestSigset *set) {
    if (!set) return fail_posix(EINVAL);
    memset(set, 0xff, sizeof(*set));
    return 0;
}

static SYS_ABI int32_t guest_getrusage(int who, GuestRusage *out) {
    if (!out || (who != 0 && who != 1)) return fail_posix(EINVAL);
    memset(out, 0, sizeof(*out));
#ifdef _WIN32
    int64_t user, system;
    runtime_cpu_times(who == 1, &user, &system);
    out->user = (GuestTimeval){user / 1000000, user % 1000000};
    out->system = (GuestTimeval){system / 1000000, system % 1000000};
#else
    struct rusage r;
    getrusage(who == 0 ? RUSAGE_SELF : RUSAGE_THREAD, &r);
    out->user = (GuestTimeval){r.ru_utime.tv_sec, r.ru_utime.tv_usec};
    out->system = (GuestTimeval){r.ru_stime.tv_sec, r.ru_stime.tv_usec};
#endif
    return 0;
}

static SYS_ABI int32_t guest_sysctl(const int32_t *name, uint32_t namelen, void *old, uint64_t *oldlen,
                                    const void *new_value, uint64_t newlen) {
    (void)newlen;
    if (!name || namelen < 2 || new_value) return fail_posix(EINVAL);
    if (name[0] == 1 && name[1] == 37) {
        if (!old || !oldlen) return fail_posix(EINVAL);
#ifdef _WIN32
        if (runtime_random(old, (size_t)*oldlen) < 0) return fail_posix(errno);
#else
        if (getrandom(old, (size_t)*oldlen, 0) < 0) return fail_posix(errno);
#endif
        return 0;
    }
    if (name[0] == 6 && (name[1] == 7 || name[1] == 3)) {
        if (!oldlen) return fail_posix(EINVAL);
        int32_t value = name[1] == 7 ? PAGE : 7;
        if (old) {
            if (*oldlen < 4) return fail_posix(ENOMEM);
            memcpy(old, &value, 4);
        }
        *oldlen = 4;
        return 0;
    }
    static unsigned notes;
    if (__atomic_fetch_add(&notes, 1, __ATOMIC_RELAXED) < 8) {
        fprintf(stderr, "Runtime: unsupported sysctl mib");
        for (uint32_t i = 0; i < namelen && i < 8; ++i) fprintf(stderr, " %d", name[i]);
        fputs(" (ENOENT)\n", stderr);
    }
    return fail_posix(ENOENT);
}

static const SysExport kernel_exports[] = {
    {"k+AXqu2-eBc", "getpagesize", get_pagesize},
    {"HoLVWNanBBc", "getpid", get_pid},
    {"6XG4B33N09g", "sched_yield", yield},
    {"6Z83sYWFlA8", "_exit", hard_exit},
    {"OMDRKKAZ8I4", "sceKernelDebugRaiseException", raise_exception},
    {"zE-wXIZjLoM", "sceKernelDebugRaiseExceptionOnReleaseMode", raise_exception},
    {"Wl2o5hOVZdw", "sceKernelPrintBacktraceWithModuleInfo", print_backtrace},
    {"VADc3MNQ3cM", "signal", guest_signal},
    {"aPcyptbOiZs", "sigprocmask", guest_sigprocmask},
    {"6xVpy0Fdq+I", "_sigprocmask", guest_sigprocmask},
    {"VkTAsrZDcJ0", "sigfillset", guest_sigfillset},
    {"hHlZQUnlxSM", "getrusage", guest_getrusage},
    {"DFmMT80xcNI", "sysctl", guest_sysctl},
};

SYS_LIBRARY(sys_process, "libkernel", kernel_exports);
