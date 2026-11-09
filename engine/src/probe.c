#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <cpuid.h>
#include "runtime.h"
#include "sys/sys.h"
#include "gpu/bbgpu.h"
#include "respawn_patch.h"
#include "isa_rewrite.h"
#if !defined(__x86_64__) || !defined(__GNUC__)
#error This prototype requires x86-64 GCC or Clang (including MinGW).
#endif
#ifdef _WIN32
#include <windows.h>
#include <dbghelp.h>
#include <timeapi.h>
#include <process.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <wchar.h>
#else
#include <sys/mman.h>
#include <malloc.h>
#include <unistd.h>
#include <signal.h>
#include <ucontext.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <execinfo.h>
#include <pthread.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#endif

typedef struct { uint64_t address, size, flags; } Segment;
typedef struct { uint64_t target, kind, value, addend; } Reloc;
static char (*names)[128];
static uint64_t import_count;
static unsigned char *image;
static size_t page_size;
typedef struct { uint64_t base, size, init, tls_address, tls_memsz, tls_filesz, tls_module; } LinkedModule;
static LinkedModule modules[16];
static uint64_t module_count;
static int entered_game;
static int gpu_enabled;
static unsigned isa_missing_sets;
static IsaTable *isa_table;
static unsigned char *isa_stubs;
static size_t isa_stubs_size;
int vulkan_smoke(void);

static __attribute__((noreturn)) void fail(const char *message) { fprintf(stderr, "ERROR: %s\n", message); exit(1); }

static void check_cpu(void) {
    const char *skip = getenv("BB_SKIP_CPU_CHECK");
    unsigned a, b, c, d, leaf1_c = 0, leaf7_b = 0, ext1_c = 0;
    if (__get_cpuid(1, &a, &b, &c, &d)) leaf1_c = c;
    if (__get_cpuid_count(7, 0, &a, &b, &c, &d)) leaf7_b = b;
    if (__get_cpuid(0x80000001u, &a, &b, &c, &d)) ext1_c = c;
    int avx = (leaf1_c & bit_AVX) && (leaf1_c & bit_OSXSAVE);
    if (avx) {
        unsigned lo, hi;
        __asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
        avx = (lo & 6) == 6;
    }

    const struct { int present; const char *name; } fatal_features[] = {
        {avx, "AVX"}, {(leaf1_c & bit_POPCNT) != 0, "POPCNT"},
        {(leaf1_c & bit_SSE4_2) != 0, "SSE4.2"},
    };
    char missing[64] = "";
    for (unsigned i = 0; i < sizeof(fatal_features) / sizeof(fatal_features[0]); ++i) {
        if (fatal_features[i].present) continue;
        if (missing[0]) strcat(missing, ", ");
        strcat(missing, fatal_features[i].name);
    }
    if (missing[0] && (!skip || strcmp(skip, "1"))) {
        fprintf(stderr,
                "ERROR: this CPU lacks %s. Bloodborne's baseline instruction set requires AVX, POPCNT "
                "and SSE4.2. BB_SKIP_CPU_CHECK=1 bypasses this baseline check.\n", missing);
        exit(1);
    }
    if (missing[0])
        fprintf(stderr, "WARNING: BB_SKIP_CPU_CHECK=1 bypasses missing baseline features: %s\n", missing);

    isa_missing_sets = 0;
    if (!(leaf1_c & bit_MOVBE)) isa_missing_sets |= CPU_SET_MOVBE;
    if (!(leaf7_b & bit_BMI)) isa_missing_sets |= CPU_SET_BMI1;
    if (!(ext1_c & bit_LZCNT)) isa_missing_sets |= CPU_SET_LZCNT;

    if (isa_missing_sets) {
        char absent[32] = "";
        if (isa_missing_sets & CPU_SET_MOVBE) strcat(absent, "MOVBE");
        if (isa_missing_sets & CPU_SET_BMI1) strcat(absent, absent[0] ? ", BMI1" : "BMI1");
        if (isa_missing_sets & CPU_SET_LZCNT) strcat(absent, absent[0] ? ", LZCNT" : "LZCNT");
        printf("CPU: missing %s; enabling instruction compatibility rewriting\n", absent);
    } else {
        puts("CPU: native MOVBE, BMI1 and LZCNT; instruction rewriting disabled");
    }
}
static uint64_t read64(FILE *f) {
    unsigned char b[8];
    if (fread(b, 1, 8, f) != 8) fail("truncated boot file");
    uint64_t n = 0;
    for (int i = 7; i >= 0; --i) n = (n << 8) | b[i];
    return n;
}
static size_t round_page(size_t size) { return (size + page_size - 1) & ~(page_size - 1); }
static void *allocate(size_t size) {
    void *low=runtime_low_map(size,PROT_READ|PROT_WRITE);
    if (low) return low;
#ifdef _WIN32
    void *p = VirtualAlloc(NULL, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!p) fail("VirtualAlloc failed");
#else
    void *p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) fail("mmap failed");
#endif
    return p;
}
static void protect(void *p, size_t size, unsigned flags) {
#ifdef _WIN32
    DWORD old, mode = PAGE_NOACCESS;
    if (flags & 1) mode = (flags & 2) ? PAGE_EXECUTE_READWRITE : PAGE_EXECUTE_READ;
    else if (flags & 2) mode = PAGE_READWRITE;
    else if (flags & 4) mode = PAGE_READONLY;
    if (!VirtualProtect(p, size, mode, &old)) fail("VirtualProtect failed");
    FlushInstructionCache(GetCurrentProcess(), p, size);
#else
    int mode = ((flags & 4) ? PROT_READ : 0) | ((flags & 2) ? PROT_WRITE : 0) | ((flags & 1) ? PROT_EXEC : 0);
    if (mprotect(p, size, mode)) fail("mprotect failed");
#endif
}
static ABI __attribute__((noreturn)) void unresolved(uint32_t id, uintptr_t argument) {
    if (id >= import_count) fail("bad import trap index");
    printf("STOP: first unsupported PS4 import: %s (index %u)\n", names[id], id);
    printf("API: %s\n", sys_import_label(names[id]));
    printf("Caller return offset: 0x%" PRIxPTR "; first argument: 0x%" PRIxPTR "\n",
           (uintptr_t)__builtin_return_address(0) - (uintptr_t)image, argument);
    uintptr_t caller=(uintptr_t)__builtin_return_address(0)-(uintptr_t)image;
    for (uint64_t m=0;m<module_count;++m)
        if (caller>=modules[m].base && caller-modules[m].base<modules[m].size)
            printf("Caller in linked module %" PRIu64 " (%s): +0x%" PRIxPTR "\n",m,m==0 ? "libc.prx" : "system module",caller-modules[m].base);
    sys_report();
    puts(entered_game ? "Original guest entry instructions executed; game initialization is incomplete." :
                        "Native libc initialization is incomplete; game entry has not run.");
    fflush(NULL);
    _exit(20);
}
#ifndef _WIN32
void enter_on_stack(void *entry, void *arg0, void *arg1, void *top);
__asm__(".text\n.globl enter_on_stack\nenter_on_stack:\n"
        " push %rbp\n mov %rsp,%rbp\n and $-16,%rcx\n mov %rcx,%rsp\n"
        " mov %rdi,%rax\n mov %rsi,%rdi\n mov %rdx,%rsi\n call *%rax\n"
        " mov %rbp,%rsp\n pop %rbp\n ret\n");
#endif
static ABI void guest_exit(void) { puts("Runtime: process finalizer callback reached"); }
#ifndef _WIN32
static void fault(int sig, siginfo_t *info, void *context) {
    if (gpu_enabled && sig == SIGSEGV && bbgpu_handle_fault(context, info->si_addr)) return;
    if ((sig == SIGSEGV || sig == SIGBUS) && runtime_fault_recover) {
        sigjmp_buf *recover = runtime_fault_recover;
        runtime_fault_recover = NULL;
        sigset_t unblock;
        sigemptyset(&unblock);
        sigaddset(&unblock, sig);
        pthread_sigmask(SIG_UNBLOCK, &unblock, NULL);
        siglongjmp(*recover, 1);
    }
    ucontext_t *uc = context;
    uintptr_t rip = (uintptr_t)uc->uc_mcontext.gregs[REG_RIP];
    if (sig == SIGTRAP && isa_table && rip) {
        IsaRegs regs = {0};
        greg_t *g = uc->uc_mcontext.gregs;
        regs.rflags = (uint64_t)g[REG_EFL];
        regs.gpr[0] = (uint64_t)g[REG_RAX]; regs.gpr[1] = (uint64_t)g[REG_RCX];
        regs.gpr[2] = (uint64_t)g[REG_RDX]; regs.gpr[3] = (uint64_t)g[REG_RBX];
        regs.gpr[4] = (uint64_t)g[REG_RSP]; regs.gpr[5] = (uint64_t)g[REG_RBP];
        regs.gpr[6] = (uint64_t)g[REG_RSI]; regs.gpr[7] = (uint64_t)g[REG_RDI];
        regs.gpr[8] = (uint64_t)g[REG_R8]; regs.gpr[9] = (uint64_t)g[REG_R9];
        regs.gpr[10] = (uint64_t)g[REG_R10]; regs.gpr[11] = (uint64_t)g[REG_R11];
        regs.gpr[12] = (uint64_t)g[REG_R12]; regs.gpr[13] = (uint64_t)g[REG_R13];
        regs.gpr[14] = (uint64_t)g[REG_R14]; regs.gpr[15] = (uint64_t)g[REG_R15];
        uintptr_t next_rip = 0;
        if (isa_rewrite_trap(isa_table, rip - 1, &regs, &next_rip)) {
            g[REG_RAX] = (greg_t)regs.gpr[0]; g[REG_RCX] = (greg_t)regs.gpr[1];
            g[REG_RDX] = (greg_t)regs.gpr[2]; g[REG_RBX] = (greg_t)regs.gpr[3];
            g[REG_RSP] = (greg_t)regs.gpr[4]; g[REG_RBP] = (greg_t)regs.gpr[5];
            g[REG_RSI] = (greg_t)regs.gpr[6]; g[REG_RDI] = (greg_t)regs.gpr[7];
            g[REG_R8] = (greg_t)regs.gpr[8]; g[REG_R9] = (greg_t)regs.gpr[9];
            g[REG_R10] = (greg_t)regs.gpr[10]; g[REG_R11] = (greg_t)regs.gpr[11];
            g[REG_R12] = (greg_t)regs.gpr[12]; g[REG_R13] = (greg_t)regs.gpr[13];
            g[REG_R14] = (greg_t)regs.gpr[14]; g[REG_R15] = (greg_t)regs.gpr[15];
            g[REG_EFL] = (greg_t)regs.rflags;
            g[REG_RIP] = (greg_t)next_rip;
            return;
        }
    }
    char line[512];
    Dl_info where;
    if (rip - (uintptr_t)image < 0x10000000)
        snprintf(line, sizeof(line), "Guest fault (signal %d) at guest offset 0x%lx, address %p\n",
                 sig, (unsigned long)(rip - (uintptr_t)image), info->si_addr);
    else if (dladdr((void *)rip, &where) && where.dli_fname)
        snprintf(line, sizeof(line), "Host fault (signal %d) in %s+0x%lx (%s), address %p\n", sig, where.dli_fname,
                 (unsigned long)(rip - (uintptr_t)where.dli_fbase), where.dli_sname ? where.dli_sname : "?", info->si_addr);
    else
        snprintf(line, sizeof(line), "Fault (signal %d) at RIP %p, address %p\n", sig, (void *)rip, info->si_addr);
    { ssize_t written_=write(2, line, strlen(line)); (void)written_; }
    if (gpu_enabled) bbgpu_dump_guest_writes(context);
    if (rip - (uintptr_t)image >= 0x10000000 && !(dladdr((void *)rip, &where) && where.dli_fname)) {
        char thread[32] = "?";
        pthread_getname_np(pthread_self(), thread, sizeof(thread));
        snprintf(line, sizeof(line), "  thread %s; mapping of RIP: ", thread);
        { ssize_t written_=write(2, line, strlen(line)); (void)written_; }
        FILE *maps = fopen("/proc/self/maps", "r");
        int found = 0;
        while (maps && fgets(line, sizeof(line), maps)) {
            unsigned long from, to;
            if (sscanf(line, "%lx-%lx", &from, &to) == 2 && rip >= from && rip < to) {
                ssize_t written_=write(2, line, strlen(line)); (void)written_;
                found = 1;
                break;
            }
        }
        if (maps) fclose(maps);
        if (!found) { ssize_t written_=write(2, "none\n", 5); (void)written_; }
    }
    void *frames[32];
    int depth = backtrace(frames, 32);
    for (int i = 2; i < depth; ++i) {
        if (dladdr(frames[i], &where) && where.dli_fname)
            snprintf(line, sizeof(line), "  #%d %s+0x%lx (%s)\n", i, where.dli_fname,
                     (unsigned long)((uintptr_t)frames[i] - (uintptr_t)where.dli_fbase), where.dli_sname ? where.dli_sname : "?");
        else
            snprintf(line, sizeof(line), "  #%d %p\n", i, frames[i]);
        ssize_t written_=write(2, line, strlen(line)); (void)written_;
    }
    _exit(128 + sig);
}
#endif
#ifdef _WIN32
static unsigned char *traps_base;
static size_t traps_size, image_size;
static void recover_jump(void) {
    RuntimeRecoverBuf *recover = runtime_fault_recover;
    runtime_fault_recover = NULL;
    RUNTIME_RECOVER_JUMP(*recover);
}
static int guest_address(uintptr_t a) {
    return (image && a - (uintptr_t)image < image_size) || (traps_base && a - (uintptr_t)traps_base < traps_size);
}
static void describe(char *out, size_t size, uintptr_t a) {
    MEMORY_BASIC_INFORMATION info;
    char path[MAX_PATH];
    if (image && a - (uintptr_t)image < image_size) snprintf(out, size, "guest+0x%llx", (unsigned long long)(a - (uintptr_t)image));
    else if (VirtualQuery((void *)a, &info, sizeof(info)) && info.Type == MEM_IMAGE &&
             GetMappedFileNameA(GetCurrentProcess(), (void *)a, path, sizeof(path))) {
        const char *name = strrchr(path, '\\');
        snprintf(out, size, "%s+0x%llx", name ? name + 1 : path, (unsigned long long)(a - (uintptr_t)info.AllocationBase));
    } else snprintf(out, size, "0x%llx", (unsigned long long)a);
}
static __attribute__((noreturn)) void terminate(unsigned code) {
    fflush(NULL);
    TerminateProcess(GetCurrentProcess(), code);
    _exit((int)code);
}
static void report_exception(EXCEPTION_POINTERS *e) {
    static volatile LONG reporting;
    if (InterlockedExchange(&reporting, 1)) { Sleep(INFINITE); }
    EXCEPTION_RECORD *r = e->ExceptionRecord;
    CONTEXT *c = e->ContextRecord;
    char where[512];
    describe(where, sizeof(where), (uintptr_t)c->Rip);
    fprintf(stderr, "%s fault 0x%08lx at %s", guest_address((uintptr_t)c->Rip) ? "Guest" : "Host", r->ExceptionCode, where);
    if (r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && r->NumberParameters >= 2)
        fprintf(stderr, ", %s address %p", r->ExceptionInformation[0] == 1 ? "writing" : r->ExceptionInformation[0] == 8 ? "executing" : "reading",
                (void *)r->ExceptionInformation[1]);
    PWSTR description = NULL;
    char thread_name[64] = "";
    if (SUCCEEDED(GetThreadDescription(GetCurrentThread(), &description)) && description) {
        WideCharToMultiByte(CP_UTF8, 0, description, -1, thread_name, sizeof(thread_name), NULL, NULL);
        LocalFree(description);
    }
    fprintf(stderr, " (thread %lu %s)\n", GetCurrentThreadId(), thread_name);
    fprintf(stderr, "  rax=%016llx rbx=%016llx rcx=%016llx rdx=%016llx\n  rsi=%016llx rdi=%016llx rbp=%016llx rsp=%016llx\n",
            c->Rax, c->Rbx, c->Rcx, c->Rdx, c->Rsi, c->Rdi, c->Rbp, c->Rsp);
    uintptr_t rbp = c->Rbp;
    for (int depth = 0; depth < 24 && rbp; ++depth) {
        uintptr_t frame[2];
        SIZE_T n = 0;
        if (!ReadProcessMemory(GetCurrentProcess(), (void *)rbp, frame, sizeof(frame), &n) || n != sizeof(frame) || frame[0] <= rbp) break;
        describe(where, sizeof(where), frame[1]);
        fprintf(stderr, "  #%d %s\n", depth, where);
        rbp = frame[0];
    }
    void *frames[48];
    USHORT depth = RtlCaptureStackBackTrace(0, 48, frames, NULL);
    for (USHORT i = 0; i < depth; ++i) {
        describe(where, sizeof(where), (uintptr_t)frames[i]);
        fprintf(stderr, "  host #%u %s\n", i, where);
    }
    if (gpu_enabled && r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) bbgpu_dump_guest_writes(e);
    fflush(NULL);
}
enum { DUMP_THREADS = 512 };
static struct { DWORD id; CONTEXT context; } dumped[DUMP_THREADS];
static void dump_chain(CONTEXT c) {
    char where[512];
    for (int depth = 0; depth < 32 && c.Rip; ++depth) {
        describe(where, sizeof(where), (uintptr_t)c.Rip);
        fprintf(stderr, "  #%d %s\n", depth, where);
        DWORD64 base = 0;
        RUNTIME_FUNCTION *function = guest_address((uintptr_t)c.Rip) ? NULL : RtlLookupFunctionEntry(c.Rip, &base, NULL);
        if (function) {
            void *handler_data;
            DWORD64 frame;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, c.Rip, function, &c, &handler_data, &frame, NULL);
        } else if (guest_address((uintptr_t)c.Rip) && c.Rbp > c.Rsp) {
            uintptr_t *frame = (uintptr_t *)c.Rbp;
            c.Rip = frame[1]; c.Rsp = c.Rbp + 16; c.Rbp = frame[0];
        } else {
            c.Rip = *(DWORD64 *)c.Rsp; c.Rsp += 8;
        }
    }
}
static unsigned __stdcall dump_thread(void *event) {
    for (;;) {
        WaitForSingleObject(event, INFINITE);
        DWORD self = GetCurrentThreadId(), pid = GetCurrentProcessId();
        unsigned count = 0;
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        THREADENTRY32 entry = {.dwSize = sizeof(entry)};
        for (BOOL more = Thread32First(snapshot, &entry); more && count < DUMP_THREADS; more = Thread32Next(snapshot, &entry)) {
            if (entry.th32OwnerProcessID != pid || entry.th32ThreadID == self) continue;
            HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, entry.th32ThreadID);
            if (!thread) continue;
            dumped[count].id = entry.th32ThreadID;
            dumped[count].context.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
            if (SuspendThread(thread) != (DWORD)-1) {
                if (GetThreadContext(thread, &dumped[count].context)) ++count;
                ResumeThread(thread);
            }
            CloseHandle(thread);
        }
        CloseHandle(snapshot);
        fprintf(stderr, "Thread dump: %u threads\n", count);
        for (unsigned i = 0; i < count; ++i) {
            char name[64] = "";
            HANDLE thread = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, dumped[i].id);
            PWSTR description = NULL;
            if (thread && SUCCEEDED(GetThreadDescription(thread, &description)) && description) {
                WideCharToMultiByte(CP_UTF8, 0, description, -1, name, sizeof(name), NULL, NULL);
                LocalFree(description);
            }
            if (thread) CloseHandle(thread);
            CONTEXT *c = &dumped[i].context;
            fprintf(stderr, "thread %lu %s: rcx=%llx rdx=%llx rdi=%llx rsi=%llx\n", dumped[i].id, name, c->Rcx, c->Rdx, c->Rdi, c->Rsi);
            RuntimeRecoverBuf recover;
            if (!RUNTIME_RECOVER_SET(recover)) {
                runtime_fault_recover = &recover;
                dump_chain(*c);
            } else fprintf(stderr, "  (bad frame)\n");
            runtime_fault_recover = NULL;
        }
        fprintf(stderr, "Thread dump end\n");
        fflush(NULL);
    }
    return 0;
}
static void start_dump_thread(void) {
    wchar_t name[64];
    swprintf(name, 64, L"Local\\bbport-dump-%lu", GetCurrentProcessId());
    HANDLE event = CreateEventW(NULL, FALSE, FALSE, name);
    if (event) CloseHandle((HANDLE)_beginthreadex(NULL, 0, dump_thread, event, 0, NULL));
}
static volatile uintptr_t hw_watch;
static volatile char hw_mode = 'w';
static const char *hw_watch_file;
static volatile LONG hw_hits;
static uintptr_t hw_rips[64];
static void hw_watch_hit(CONTEXT *c) {
    uintptr_t rip = (uintptr_t)c->Rip;
    LONG n = hw_hits < 64 ? hw_hits : 64;
    for (LONG i = 0; i < n; ++i) if (hw_rips[i] == rip) return;
    LONG slot = InterlockedIncrement(&hw_hits) - 1;
    if (slot >= 64) return;
    hw_rips[slot] = rip;
    char where[512];
    describe(where, sizeof(where), rip);
    fprintf(stderr, "HW watch: write to %#llx by %s (after the store), now %08x, thread %lu\n"
            "  rax=%llx rbx=%llx rcx=%llx rdx=%llx rsi=%llx rdi=%llx r8=%llx r12=%llx r13=%llx r14=%llx r15=%llx\n",
            (unsigned long long)hw_watch, where, *(volatile unsigned *)hw_watch, GetCurrentThreadId(),
            c->Rax, c->Rbx, c->Rcx, c->Rdx, c->Rsi, c->Rdi, c->R8, c->R12, c->R13, c->R14, c->R15);
    uintptr_t rbp = c->Rbp;
    for (int depth = 0; depth < 6 && rbp > c->Rsp; ++depth) {
        uintptr_t frame[2];
        SIZE_T got = 0;
        if (!ReadProcessMemory(GetCurrentProcess(), (void *)rbp, frame, sizeof(frame), &got) || got != sizeof(frame) || frame[0] <= rbp) break;
        describe(where, sizeof(where), frame[1]);
        fprintf(stderr, "  #%d %s\n", depth, where);
        rbp = frame[0];
    }
    fflush(stderr);
}
static unsigned __stdcall hw_watch_thread(void *unused) {
    (void)unused;
    for (;;) {
        unsigned long long address = 0;
        char mode = 'w';
        FILE *f = fopen(hw_watch_file, "r");
        if (f) {
            int c = fgetc(f);
            if (c == 'r' || c == 'x') mode = (char)c; else if (c != EOF) ungetc(c, f);
            if (fscanf(f, "%llx", &address) != 1) address = 0;
            fclose(f);
        }
        if (address != hw_watch || mode != hw_mode) {
            hw_hits = 0; hw_mode = mode; hw_watch = (uintptr_t)address;
            fprintf(stderr, "HW watch: %#llx (%s)\n", address, mode == 'r' ? "reads and writes" : "writes");
        }
        DWORD self = GetCurrentThreadId(), pid = GetCurrentProcessId();
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        THREADENTRY32 entry = {.dwSize = sizeof(entry)};
        for (BOOL more = Thread32First(snapshot, &entry); more; more = Thread32Next(snapshot, &entry)) {
            if (entry.th32OwnerProcessID != pid || entry.th32ThreadID == self) continue;
            HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT, FALSE, entry.th32ThreadID);
            if (!thread) continue;
            if (SuspendThread(thread) != (DWORD)-1) {
                CONTEXT c = {.ContextFlags = CONTEXT_DEBUG_REGISTERS};
                DWORD64 dr7 = !hw_watch ? 0 : hw_mode == 'x' ? 1 : 1 | ((hw_mode == 'r' ? 3u : 1u) << 16) | (3u << 18);
                if (GetThreadContext(thread, &c) && (c.Dr0 != hw_watch || (c.Dr7 & 0xf0003) != dr7)) {
                    c.Dr0 = hw_watch;
                    c.Dr7 = (c.Dr7 & ~(DWORD64)0xf0003) | dr7;
                    SetThreadContext(thread, &c);
                }
                ResumeThread(thread);
            }
            CloseHandle(thread);
        }
        CloseHandle(snapshot);
        Sleep(200);
    }
    return 0;
}
static void start_hw_watch(void) {
    if (!(hw_watch_file = getenv("BB_HW_WATCH"))) return;
    CloseHandle((HANDLE)_beginthreadex(NULL, 0, hw_watch_thread, NULL, 0, NULL));
}
static int fatal(DWORD code) {
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION: case EXCEPTION_ILLEGAL_INSTRUCTION: case EXCEPTION_PRIV_INSTRUCTION:
    case EXCEPTION_INT_DIVIDE_BY_ZERO: case EXCEPTION_INT_OVERFLOW: case EXCEPTION_STACK_OVERFLOW:
    case EXCEPTION_IN_PAGE_ERROR: case EXCEPTION_DATATYPE_MISALIGNMENT: case EXCEPTION_BREAKPOINT:
        return 1;
    default:
        return 0;
    }
}
static void note_host_fault(EXCEPTION_POINTERS *e) {
    static volatile LONG notes;
    EXCEPTION_RECORD *r = e->ExceptionRecord;
    if (r->ExceptionCode == EXCEPTION_BREAKPOINT) return;
    if (!runtime_thread_has_record() && InterlockedIncrement(&notes) > 4) return;
    char line[1024], where[512], thread_name[64] = "";
    describe(where, sizeof(where), (uintptr_t)e->ContextRecord->Rip);
    PWSTR description = NULL;
    if (SUCCEEDED(GetThreadDescription(GetCurrentThread(), &description)) && description) {
        WideCharToMultiByte(CP_UTF8, 0, description, -1, thread_name, sizeof(thread_name), NULL, NULL);
        LocalFree(description);
    }
    int n = snprintf(line, sizeof(line), "Host fault 0x%08lx at %s", r->ExceptionCode, where);
    if (r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && r->NumberParameters >= 2)
        n += snprintf(line + n, sizeof(line) - n, ", %s address %p",
                      r->ExceptionInformation[0] == 1 ? "writing" : r->ExceptionInformation[0] == 8 ? "executing" : "reading",
                      (void *)r->ExceptionInformation[1]);
    n += snprintf(line + n, sizeof(line) - n, " (thread %lu %s; if the game closes now, this is why)\n",
                  GetCurrentThreadId(), thread_name);
    HANDLE out = GetStdHandle(STD_ERROR_HANDLE);
    DWORD written;
    WriteFile(out, line, (DWORD)strlen(line), &written, NULL);
    void *frames[32];
    USHORT depth = RtlCaptureStackBackTrace(0, 32, frames, NULL);
    for (USHORT i = 0; i < depth; ++i) {
        describe(where, sizeof(where), (uintptr_t)frames[i]);
        snprintf(line, sizeof(line), "  host #%u %s\n", i, where);
        WriteFile(out, line, (DWORD)strlen(line), &written, NULL);
    }
}
static char own_dir[MAX_PATH];
static void find_own_dir(void) {
    if (!GetMappedFileNameA(GetCurrentProcess(), (void *)find_own_dir, own_dir, sizeof(own_dir))) own_dir[0] = 0;
    char *slash = strrchr(own_dir, '\\');
    if (slash) slash[1] = 0;
}
static int host_fault_dead_end(const CONTEXT *fault) {
    if (!own_dir[0]) return 0;
    CONTEXT c = *fault;
    RuntimeRecoverBuf recover;
    if (RUNTIME_RECOVER_SET(recover)) return 0;
    runtime_fault_recover = &recover;
    int dead_end = 0;
    for (int depth = 0; depth < 64 && c.Rip; ++depth) {
        if (guest_address((uintptr_t)c.Rip)) { dead_end = 1; break; }
        DWORD64 base = 0;
        RUNTIME_FUNCTION *function = RtlLookupFunctionEntry(c.Rip, &base, NULL);
        if (!function) { c.Rip = *(DWORD64 *)c.Rsp; c.Rsp += 8; continue; }
        void *handler_data;
        DWORD64 frame;
        PEXCEPTION_ROUTINE handler = RtlVirtualUnwind(UNW_FLAG_EHANDLER, base, c.Rip, function, &c, &handler_data, &frame, NULL);
        char path[MAX_PATH];
        if (handler && !(GetMappedFileNameA(GetCurrentProcess(), (void *)handler, path, sizeof(path)) &&
                         !_strnicmp(path, own_dir, strlen(own_dir)))) break;
    }
    runtime_fault_recover = NULL;
    return dead_end;
}
static LONG WINAPI unhandled_filter(EXCEPTION_POINTERS *e);
static HANDLE crash_dump_request, crash_dump_done;
static EXCEPTION_POINTERS *volatile crash_dump_exception;
static volatile DWORD crash_dump_thread_id;
static volatile LONG crash_dump_written;
static char crash_dump_dir[MAX_PATH], crash_dump_path[MAX_PATH];
typedef BOOL (WINAPI *MiniDumpWriteDumpFn)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION,
                                          PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);
static unsigned __stdcall crash_helper_thread(void *unused) {
    (void)unused;
    for (DWORD waited; (waited = WaitForSingleObject(crash_dump_request, 1000)) != WAIT_OBJECT_0;) {
        if (waited != WAIT_TIMEOUT) { SetEvent(crash_dump_done); return 0; }
        LPTOP_LEVEL_EXCEPTION_FILTER previous = SetUnhandledExceptionFilter(unhandled_filter);
        static int noted;
        if (previous != unhandled_filter && !noted) {
            noted = 1;
            fprintf(stderr, "Crash reports: another module replaced the unhandled exception filter (an overlay?); set again\n");
        }
    }
    if (crash_dump_dir[0]) CreateDirectoryA(crash_dump_dir, NULL);
    HMODULE dbghelp = LoadLibraryW(L"dbghelp.dll");
    MiniDumpWriteDumpFn write_dump = dbghelp ? (MiniDumpWriteDumpFn)(void *)GetProcAddress(dbghelp, "MiniDumpWriteDump") : NULL;
    HANDLE file = write_dump ? CreateFileA(crash_dump_path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL)
                             : INVALID_HANDLE_VALUE;
    if (file != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION exception = {crash_dump_thread_id, crash_dump_exception, FALSE};
        const MINIDUMP_TYPE type = (MINIDUMP_TYPE)(MiniDumpNormal | MiniDumpWithThreadInfo | MiniDumpWithIndirectlyReferencedMemory);
        if (write_dump(GetCurrentProcess(), GetCurrentProcessId(), file, type, &exception, NULL, NULL)) crash_dump_written = 1;
        CloseHandle(file);
    }
    SetEvent(crash_dump_done);
    return 0;
}
static void start_crash_helper(void) {
    const char *log = getenv("BB_LOG_FILE");
    const char *slash = log ? strrchr(log, '\\') : NULL, *other = log ? strrchr(log, '/') : NULL;
    if (other > slash) slash = other;
    if (slash && (size_t)(slash - log) + sizeof("\\crash.dmp") < sizeof(crash_dump_path)) {
        snprintf(crash_dump_path, sizeof(crash_dump_path), "%.*s\\crash.dmp", (int)(slash - log), log);
    } else {
        snprintf(crash_dump_dir, sizeof(crash_dump_dir), "logs");
        snprintf(crash_dump_path, sizeof(crash_dump_path), "logs\\crash.dmp");
    }
    crash_dump_request = CreateEventW(NULL, FALSE, FALSE, NULL);
    crash_dump_done = CreateEventW(NULL, TRUE, FALSE, NULL);
    HANDLE thread = crash_dump_request && crash_dump_done ? (HANDLE)_beginthreadex(NULL, 0, crash_helper_thread, NULL, 0, NULL) : NULL;
    if (thread) CloseHandle(thread);
    else crash_dump_request = NULL;
}
static void write_crash_dump(EXCEPTION_POINTERS *e) {
    if (!crash_dump_request) return;
    crash_dump_exception = e;
    crash_dump_thread_id = GetCurrentThreadId();
    SetEvent(crash_dump_request);
    const DWORD waited = WaitForSingleObject(crash_dump_done, 30000);
    char line[MAX_PATH + 64];
    snprintf(line, sizeof(line), waited == WAIT_OBJECT_0 && crash_dump_written ? "Crash dump: %s\n" : "Crash dump: not written (%s)\n",
             crash_dump_path);
    DWORD written;
    WriteFile(GetStdHandle(STD_ERROR_HANDLE), line, (DWORD)strlen(line), &written, NULL);
}
static LONG CALLBACK vectored_handler(EXCEPTION_POINTERS *e) {
    EXCEPTION_RECORD *r = e->ExceptionRecord;
    if (r->ExceptionCode == EXCEPTION_SINGLE_STEP && hw_watch_file && (e->ContextRecord->Dr6 & 1)) {
        if (hw_watch && e->ContextRecord->Dr0 == hw_watch) hw_watch_hit(e->ContextRecord);
        e->ContextRecord->Dr6 = 0;
        if (!(e->ContextRecord->Dr7 & (3u << 16))) e->ContextRecord->EFlags |= 0x10000;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (r->ExceptionCode == EXCEPTION_BREAKPOINT && isa_table && e->ContextRecord->Rip) {
        CONTEXT *c = e->ContextRecord;
        IsaRegs regs = {0};
        regs.rflags = c->EFlags;
        regs.gpr[0] = c->Rax; regs.gpr[1] = c->Rcx; regs.gpr[2] = c->Rdx; regs.gpr[3] = c->Rbx;
        regs.gpr[4] = c->Rsp; regs.gpr[5] = c->Rbp; regs.gpr[6] = c->Rsi; regs.gpr[7] = c->Rdi;
        regs.gpr[8] = c->R8; regs.gpr[9] = c->R9; regs.gpr[10] = c->R10; regs.gpr[11] = c->R11;
        regs.gpr[12] = c->R12; regs.gpr[13] = c->R13; regs.gpr[14] = c->R14; regs.gpr[15] = c->R15;
        uintptr_t next_rip = 0;
        uintptr_t rip = (uintptr_t)c->Rip;
        /* Windows reports the breakpoint RIP at the INT3 byte on some paths, while
         * other paths report the following byte. Accept either convention. */
        int handled = isa_rewrite_trap(isa_table, rip - 1, &regs, &next_rip);
        if (!handled) handled = isa_rewrite_trap(isa_table, rip, &regs, &next_rip);
        if (handled) {
            c->Rax = regs.gpr[0]; c->Rcx = regs.gpr[1]; c->Rdx = regs.gpr[2]; c->Rbx = regs.gpr[3];
            c->Rsp = regs.gpr[4]; c->Rbp = regs.gpr[5]; c->Rsi = regs.gpr[6]; c->Rdi = regs.gpr[7];
            c->R8 = regs.gpr[8]; c->R9 = regs.gpr[9]; c->R10 = regs.gpr[10]; c->R11 = regs.gpr[11];
            c->R12 = regs.gpr[12]; c->R13 = regs.gpr[13]; c->R14 = regs.gpr[14]; c->R15 = regs.gpr[15];
            c->EFlags = (DWORD)regs.rflags;
            c->Rip = (DWORD64)next_rip;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    if (r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && r->NumberParameters >= 2) {
        if (gpu_enabled && bbgpu_handle_fault(e, (void *)r->ExceptionInformation[1])) return EXCEPTION_CONTINUE_EXECUTION;
        if (runtime_fault_recover) {
            CONTEXT *c = e->ContextRecord;
            c->Rsp = ((c->Rsp - 512) & ~(DWORD64)15) - 8;
            c->Rip = (DWORD64)(uintptr_t)recover_jump;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    if (!fatal(r->ExceptionCode)) return EXCEPTION_CONTINUE_SEARCH;
    if (!guest_address((uintptr_t)e->ContextRecord->Rip) && !host_fault_dead_end(e->ContextRecord)) {
        note_host_fault(e);
        return EXCEPTION_CONTINUE_SEARCH;
    }
    report_exception(e);
    write_crash_dump(e);
    terminate(128 + 11);
}
static LONG WINAPI unhandled_filter(EXCEPTION_POINTERS *e) {
    report_exception(e);
    write_crash_dump(e);
    terminate(128 + 11);
}
static uint64_t patch_tls_reads(const Segment *segments, uint64_t count) {
    static const unsigned char gs_load[9] = {0x65, 0x48, 0x8b, 0x04, 0x25, 0, 0, 0, 0};
    const uint32_t slot = runtime_win_tls_slot();
    unsigned char **sites = NULL;
    size_t found = 0, capacity = 0;
    for (uint64_t i = 0; i < count; ++i) {
        if (!(segments[i].flags & 1) || segments[i].size < sizeof(gs_load)) continue;
        unsigned char *at = image + segments[i].address, *last = at + segments[i].size - sizeof(gs_load);
        while (at <= last && (at = memchr(at, 0x65, (size_t)(last - at) + 1))) {
            if (memcmp(at, gs_load, sizeof(gs_load))) { ++at; continue; }
            if (found == capacity) {
                capacity = capacity ? capacity * 2 : 4096;
                if (!(sites = realloc(sites, capacity * sizeof(*sites)))) fail("allocation failed");
            }
            sites[found++] = at;
            at += sizeof(gs_load);
        }
    }
    if (slot < 64) {
        uint32_t displacement = 0x1480 + slot * 8;
        for (size_t i = 0; i < found; ++i) memcpy(sites[i] + 5, &displacement, 4);
    } else if (found) {
        enum { STUB = 21 };
        static const unsigned char expansion[9] = {0x65, 0x48, 0x8b, 0x04, 0x25, 0x80, 0x17, 0, 0};
        size_t bytes = round_page(found * STUB);
        unsigned char *stubs = allocate(bytes);
        uint32_t index = (slot - 64) * 8;
        for (size_t i = 0; i < found; ++i) {
            unsigned char *stub = stubs + i * STUB, *site = sites[i];
            int64_t back = (site + 9) - (stub + STUB), there = stub - (site + 5);
            if (back != (int32_t)back || there != (int32_t)there) fail("TLS stub out of jump range");
            int32_t back32 = (int32_t)back, there32 = (int32_t)there;
            memcpy(stub, expansion, 9);
            stub[9] = 0x48; stub[10] = 0x8b; stub[11] = 0x80; memcpy(stub + 12, &index, 4);
            stub[16] = 0xe9; memcpy(stub + 17, &back32, 4);
            site[0] = 0xe9; memcpy(site + 1, &there32, 4); memset(site + 5, 0x90, 4);
        }
        protect(stubs, bytes, 5);
    }
    free(sites);
    return found;
}
#else
static uintptr_t exe_base;
static void write_hex(char *out, uint64_t v) {
    const char digits[] = "0123456789abcdef";
    for (int i = 0; i < 16; ++i) out[i] = digits[(v >> (60 - i * 4)) & 15];
}
static void dump_frames(ucontext_t *uc) {
    char line[] = "  tid=0000000000000000 rip=0000000000000000 image-relative=0000000000000000 host-relative=0000000000000000\n";
    uintptr_t rip=(uintptr_t)uc->uc_mcontext.gregs[REG_RIP], rbp=(uintptr_t)uc->uc_mcontext.gregs[REG_RBP];
    uint64_t tid=(uint64_t)gettid();
    char arg[]="  tid=0000000000000000 rdi=0000000000000000\n";
    write_hex(arg+6,tid); write_hex(arg+27,(uint64_t)uc->uc_mcontext.gregs[REG_RDI]);
    { ssize_t written_=write(2,arg,sizeof(arg)-1); (void)written_; }
    for (int depth=0; depth<24; ++depth) {
        write_hex(line+6,tid); write_hex(line+27,rip); write_hex(line+59,rip-(uintptr_t)image); write_hex(line+90,rip-exe_base);
        { ssize_t written_=write(2,line,sizeof(line)-1); (void)written_; }
        uintptr_t frame[2];
        struct iovec local={frame,sizeof(frame)}, remote={(void *)rbp,sizeof(frame)};
        if (!rbp || process_vm_readv(getpid(),&local,1,&remote,1,0)!=(ssize_t)sizeof(frame)) break;
        if (frame[0]<=rbp) break;
        rbp=frame[0]; rip=frame[1];
    }
}
static void thread_dump(int sig, siginfo_t *info, void *context) { (void)sig; (void)info; dump_frames(context); }
static void watchdog(int sig, siginfo_t *info, void *context) {
    (void)info;
    const char head[]="STOP: watchdog timeout; thread stacks:\n";
    { ssize_t written_=write(2,head,sizeof(head)-1); (void)written_; }
    dump_frames(context);
    int dir=open("/proc/self/task",O_RDONLY|O_DIRECTORY);
    char buffer[4096];
    long n;
    pid_t self=gettid();
    while (dir>=0 && (n=syscall(SYS_getdents64,dir,buffer,sizeof(buffer)))>0)
        for (long at=0; at<n;) {
            struct { uint64_t ino; int64_t off; unsigned short reclen; unsigned char type; char name[]; } *d=(void *)(buffer+at);
            pid_t tid=(pid_t)strtol(d->name,NULL,10);
            if (tid>0 && tid!=self) { syscall(SYS_tgkill,getpid(),tid,SIGUSR2); usleep(20000); }
            at+=d->reclen;
        }
    usleep(100000);
    _exit(128 + sig);
}
#endif
static int sfo_value(const char *path, const char *key, char *text, size_t text_size, uint32_t *number) {
    FILE *f=fopen(path,"rb");
    if (!f) return 0;
    unsigned char data[65536];
    size_t n=fread(data,1,sizeof(data),f); fclose(f);
    if (n<20 || memcmp(data,"\0PSF",4)) return 0;
    uint32_t keys, values, count;
    memcpy(&keys,data+8,4); memcpy(&values,data+12,4); memcpy(&count,data+16,4);
    for (uint32_t i=0;i<count && 20+i*16+16<=n;++i) {
        const unsigned char *e=data+20+i*16;
        uint16_t key_offset, format; uint32_t length, offset;
        memcpy(&key_offset,e,2); memcpy(&format,e+2,2); memcpy(&length,e+4,4); memcpy(&offset,e+12,4);
        if (keys+key_offset>=n || values+offset+length>n || strcmp((const char *)data+keys+key_offset,key)) continue;
        if (format==0x0404 && number && length>=4) { memcpy(number,data+values+offset,4); return 1; }
        if (text && text_size) {
            size_t copy=length<text_size-1 ? length : text_size-1;
            memcpy(text,data+values+offset,copy); text[copy]=0;
        }
        return 1;
    }
    return 0;
}
static int mapped(Segment *segments, uint64_t count, uint64_t address, uint64_t bytes) {
    for (uint64_t i = 0; i < count; ++i)
        if (address >= segments[i].address && bytes <= segments[i].size &&
            address - segments[i].address <= segments[i].size - bytes) return 1;
    return 0;
}
static void apply_patches(const char *path, Segment *segments, uint64_t ns, const Reloc *relocs, uint64_t nr) {
    FILE *f=fopen(path,"rb");
    char magic[8];
    if (!f || fread(magic,1,8,f)!=8 || memcmp(magic,"BBPATCH2",8)) fail("invalid patch file");
    uint64_t base=read64(f), count=read64(f), bytes=0, rebased=0;
    unsigned char data[4096];
    for (uint64_t i=0;i<count;++i) {
        uint64_t offset=read64(f), length=read64(f);
        if (!length || length>sizeof(data) || !mapped(segments,ns,offset,length) || fread(data,1,length,f)!=length)
            fail("bad patch entry");
        uint64_t slots[sizeof(data)/8]; size_t nslots=0;
        for (uint64_t r=0;r<nr;++r) {
            if (!(relocs[r].target<offset+length && offset<relocs[r].target+8)) continue;
            uint64_t target=relocs[r].target, value;
            if (relocs[r].kind || target<offset || target+8>offset+length) fail("patch overlaps a relocation");
            memcpy(&value,data+(target-offset),8);
            if (value<base || !mapped(segments,ns,value-base,1)) fail("patch writes a pointer outside the image");
            slots[nslots++]=target;
        }
        memcpy(image+offset,data,length);
        for (size_t s=0;s<nslots;++s) {
            uint64_t value;
            memcpy(&value,image+slots[s],8);
            value=(uint64_t)(uintptr_t)image+(value-base);
            memcpy(image+slots[s],&value,8);
        }
        rebased+=nslots;
        bytes+=length;
    }
    if (fgetc(f)!=EOF) fail("trailing data in patch file");
    fclose(f);
    printf("Patches: %" PRIu64 " writes, %" PRIu64 " bytes applied, %" PRIu64 " pointers rebased\n",count,bytes,rebased);
}
volatile int runtime_restarting;
void runtime_restart(void) {
    fflush(NULL);
#ifdef _WIN32
    const char *command = getenv("BB_RESTART_COMMAND");
    puts("Runtime: restarting through the launcher");
    if (!command) { fputs("runtime_restart: BB_RESTART_COMMAND is not set\n", stderr); _exit(1); }
    char line[32768];
    snprintf(line, sizeof(line), "%s", command);
    STARTUPINFOA startup = {.cb = sizeof(startup)};
    PROCESS_INFORMATION process;
    if (!CreateProcessA(NULL, line, NULL, NULL, FALSE, 0, NULL, NULL, &startup, &process)) {
        fprintf(stderr, "runtime_restart: CreateProcess failed (%lu)\n", GetLastError());
        _exit(1);
    }
    _exit(0);
#else
    puts("Runtime: restarting through run.sh");
    runtime_restarting = 1;
    __sync_synchronize();
    syscall(SYS_close_range, 3u, ~0u, 0u);
    execlp("bash", "bash", "run.sh", (char *)NULL);
    perror("runtime_restart: exec");
    _exit(1);
#endif
}

#ifdef _WIN32
static int loader_main(int argc, char **argv);
static int main_argc;
static char **main_argv;
static unsigned __stdcall loader_thread(void *unused) {
    (void)unused;
    runtime_win_set_thread_name("main");
    return (unsigned)loader_main(main_argc, main_argv);
}
int main(int argc, char **argv) {
    timeBeginPeriod(1);
    PROCESS_POWER_THROTTLING_STATE throttling = {PROCESS_POWER_THROTTLING_CURRENT_VERSION,
                                                 PROCESS_POWER_THROTTLING_EXECUTION_SPEED, 0};
    SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &throttling, sizeof(throttling));
    find_own_dir();
    AddVectoredExceptionHandler(1, vectored_handler);
    SetUnhandledExceptionFilter(unhandled_filter);
    start_crash_helper();
    start_dump_thread();
    start_hw_watch();
    main_argc = argc; main_argv = argv;
    HANDLE thread = (HANDLE)_beginthreadex(NULL, 64u << 20, loader_thread, NULL, 0, NULL);
    if (!thread) fail("cannot start the loader thread");
    WaitForSingleObject(thread, INFINITE);
    DWORD code = 1;
    GetExitCodeThread(thread, &code);
    return (int)code;
}
#define main loader_main
#endif

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
#ifndef _WIN32
    mallopt(M_ARENA_MAX,1);
    mallopt(M_MMAP_THRESHOLD,32*1024*1024);
#endif
    if (argc == 2 && !strcmp(argv[1], "--vulkan-only")) return vulkan_smoke();
    check_cpu();
    int cpu_only = 0, strict_imports = 0;
    unsigned timeout_seconds = 10;
    const char *content_profile=NULL, *app0=NULL, *user_dir=NULL, *patch_file=NULL;
    for (int i = 2; i < argc; ++i) {
        if (!strcmp(argv[i], "--cpu-only")) cpu_only = 1;
        else if (!strcmp(argv[i], "--strict-imports")) strict_imports = 1;
        else if (!strcmp(argv[i], "--content-profile") && i+1<argc) content_profile=argv[++i];
        else if (!strcmp(argv[i], "--app0") && i+1<argc) app0=argv[++i];
        else if (!strcmp(argv[i], "--user") && i+1<argc) user_dir=argv[++i];
        else if (!strcmp(argv[i], "--patches") && i+1<argc) patch_file=argv[++i];
        else if (!strcmp(argv[i], "--timeout") && i+1<argc) timeout_seconds=(unsigned)strtoul(argv[++i],NULL,10);
        else { fprintf(stderr, "Unknown option: %s\n", argv[i]); return 1; }
    }
    if (argc < 2) {
        fprintf(stderr, "Usage: %s boot.bin [--cpu-only] [--strict-imports] [--content-profile file] [--app0 dir] [--user dir] [--patches file] [--timeout seconds] | --vulkan-only\n", argv[0]);
        return 1;
    }
    if (content_profile) {
        FILE *profile=fopen(content_profile,"rb");
        unsigned char data[28];
        if (!profile) fail("cannot open content profile");
        if (fread(data,1,sizeof(data),profile)!=sizeof(data) || fgetc(profile)!=EOF || memcmp(data,"BBCONT01",8)) fail("invalid content profile");
        fclose(profile);
        uint32_t values[5];
        for (unsigned i=0;i<5;++i) {
            unsigned char *p=data+8+i*4;
            values[i]=(uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;
        }
        sys_app_content_configure(values);
    }
    if (app0) {
        sys_file_configure(app0, user_dir ? user_dir : "user");
        char sfo[4096], id[16]="";
        snprintf(sfo,sizeof(sfo),"%s/sce_sys/param.sfo",app0);
        if (sfo_value(sfo,"INSTALL_DIR_SAVEDATA",id,sizeof(id),NULL) || sfo_value(sfo,"TITLE_ID",id,sizeof(id),NULL))
            sys_save_data_configure(id);
    }
    bbgpu_register_kernel();
#ifdef _WIN32
    SYSTEM_INFO system_info; GetSystemInfo(&system_info); page_size = system_info.dwPageSize;
    (void)timeout_seconds;
#else
    page_size = (size_t)sysconf(_SC_PAGESIZE);
    struct sigaction sa = {0}; sa.sa_sigaction = fault; sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL); sigaction(SIGILL, &sa, NULL); sigaction(SIGBUS, &sa, NULL); sigaction(SIGTRAP, &sa, NULL);
    Dl_info self_info;
    if (dladdr((void *)main,&self_info)) exe_base=(uintptr_t)self_info.dli_fbase;
    struct sigaction dump = {0}; dump.sa_sigaction = thread_dump; dump.sa_flags = SA_SIGINFO|SA_RESTART;
    sigemptyset(&dump.sa_mask); sigaction(SIGUSR2, &dump, NULL);
    struct sigaction alarm_action = {0}; alarm_action.sa_sigaction = watchdog; alarm_action.sa_flags = SA_SIGINFO;
    sigemptyset(&alarm_action.sa_mask); sigaction(SIGALRM, &alarm_action, NULL);
    alarm(timeout_seconds);
#endif
    FILE *f = fopen(argv[1], "rb");
    if (!f) fail("cannot open boot file; run prepare.py first");
    char magic[8];
    if (fread(magic, 1, 8, f) != 8 || (memcmp(magic, "BBPROBE1", 8) && memcmp(magic, "BBPROBE2", 8) && memcmp(magic,"BBPROBE3",8) && memcmp(magic,"BBPROBE4",8) && memcmp(magic,"BBPROBE5",8))) fail("bad boot file signature");
    uint64_t size = read64(f), entry = read64(f), ns = read64(f), nr = read64(f);
    import_count = read64(f);
    uint64_t capabilities = memcmp(magic, "BBPROBE1", 8) ? read64(f) : 0;
    if (capabilities & ~UINT64_C(1)) fail("unknown runtime capabilities");
    sys_imports_start(strict_imports ? 0 : capabilities);
    if (!size || size > 512*1024*1024 || entry >= size || !ns || ns > 64 || nr > 1000000 || import_count > 100000)
        fail("boot file limits exceeded");
    int multi=!memcmp(magic,"BBPROBE5",8);
    int linked=!memcmp(magic,"BBPROBE3",8) || !memcmp(magic,"BBPROBE4",8) || multi;
    int native_libc=linked && !strict_imports && (capabilities&1);
    uint64_t main_tls[4]={0};
    uint64_t nb=0,procparam=0;
    uint64_t *bindings=calloc(import_count ? import_count : 1,sizeof(*bindings));
    uint64_t *binding_kinds=calloc(import_count ? import_count : 1,sizeof(*binding_kinds));
    if (!bindings || !binding_kinds) fail("allocation failed");
    if (multi) {
        procparam=read64(f);
        for (int i=0;i<4;++i) main_tls[i]=read64(f);
        module_count=read64(f);
        if (!module_count || module_count>sizeof(modules)/sizeof(*modules)) fail("invalid module count");
        for (uint64_t m=0;m<module_count;++m) {
            LinkedModule *x=&modules[m];
            x->base=read64(f); x->size=read64(f); x->init=read64(f); x->tls_address=read64(f);
            x->tls_memsz=read64(f); x->tls_filesz=read64(f); x->tls_module=read64(f);
        }
        nb=read64(f);
    } else if (linked) {
        LinkedModule *x=&modules[0];
        module_count=1;
        x->base=read64(f); x->size=read64(f); x->init=read64(f);
        x->tls_address=read64(f); x->tls_memsz=read64(f); x->tls_filesz=read64(f); x->tls_module=2; nb=read64(f);
        procparam=read64(f);
        if (!memcmp(magic,"BBPROBE4",8)) for (int i=0;i<4;++i) main_tls[i]=read64(f);
    }
    if (linked) {
        if (main_tls[1]>main_tls[2] || main_tls[2]>1024*1024 || main_tls[0]>size ||
            main_tls[1]>size-main_tls[0] || (main_tls[3] & (main_tls[3]-1)) || main_tls[3]>4096)
            fail("invalid eboot TLS metadata");
        for (uint64_t m=0;m<module_count;++m) {
            LinkedModule *x=&modules[m];
            if (x->base>=size || !x->size || x->size>size-x->base || x->init<x->base || x->init-x->base>=x->size ||
                (x->tls_module && (x->tls_address>=size || x->tls_memsz>size-x->tls_address || x->tls_memsz>1024*1024 ||
                                   x->tls_filesz>x->tls_memsz || x->tls_module<2 || x->tls_module>7)))
                fail("invalid linked module metadata");
        }
        if (nb>import_count) fail("invalid binding count");
        for (uint64_t i=0;i<nb;++i) {
            uint64_t index=read64(f),address=read64(f),kind=read64(f);
            int inside=0;
            for (uint64_t m=0;m<module_count;++m)
                if (address>=modules[m].base && address-modules[m].base<modules[m].size) inside=1;
            if (index>=import_count || !inside || (kind!=1 && kind!=2) || bindings[index]) fail("invalid native binding");
            bindings[index]=address; binding_kinds[index]=kind;
        }
    }
    Segment *segments = calloc(ns, sizeof(*segments));
    Reloc *relocs = calloc(nr ? nr : 1, sizeof(*relocs));
    names = calloc(import_count ? import_count : 1, sizeof(*names));
    if (!segments || !relocs || !names) fail("allocation failed");
    for (uint64_t i = 0; i < ns; ++i) {
        segments[i].address = read64(f);
        segments[i].size = read64(f);
        segments[i].flags = read64(f);
        if (segments[i].address > size || segments[i].size > size - segments[i].address ||
            segments[i].address % page_size || segments[i].flags > 7) fail("bad segment");
    }
    if (fread(names, 128, import_count, f) != import_count) fail("truncated import names");
    for (uint64_t i = 0; i < import_count; ++i)
        if (!memchr(names[i], 0, 128)) fail("unterminated import name");
    for (uint64_t i = 0; i < nr; ++i) {
        relocs[i].target = read64(f);
        relocs[i].kind = read64(f);
        relocs[i].value = read64(f);
        relocs[i].addend = read64(f);
        if (!mapped(segments, ns, relocs[i].target, 8) || relocs[i].kind > 2 ||
            (relocs[i].kind && relocs[i].value >= import_count) ||
            (relocs[i].kind != 2 && relocs[i].addend) || relocs[i].addend >= page_size) fail("bad relocation");
    }
    for (uint64_t m=0;m<module_count;++m)
        if (!mapped(segments,ns,modules[m].init,1) || (modules[m].tls_module && !mapped(segments,ns,modules[m].tls_address,modules[m].tls_memsz)))
            fail("unmapped module metadata");
    if (module_count && !mapped(segments,ns,procparam,64)) fail("unmapped procparam");
    for (uint64_t i=0;i<import_count;++i) {
        if (!bindings[i]) continue;
        if (!mapped(segments,ns,bindings[i],1)) fail("unmapped native export");
        if (binding_kinds[i]==1) {
            int executable=0;
            for (uint64_t s=0;s<ns;++s)
                if ((segments[s].flags&1) && bindings[i]>=segments[s].address &&
                    bindings[i]-segments[s].address<segments[s].size) executable=1;
            if (!executable) fail("native function is not executable");
        }
    }
    image = allocate(round_page(size));
#ifdef _WIN32
    image_size = round_page(size);
#endif
    if (fread(image, 1, size, f) != size || fgetc(f) != EOF) fail("incorrect memory image size");
    fclose(f);
    if (isa_missing_sets) {
        char table_path[4096];
        const char *slash = strrchr(argv[1], '/');
#ifdef _WIN32
        const char *backslash = strrchr(argv[1], '\\');
        if (!slash || (backslash && backslash > slash)) slash = backslash;
#endif
        if (slash) snprintf(table_path, sizeof(table_path), "%.*s%cisa-rewrite.bin",
                            (int)(slash - argv[1]), argv[1], *slash);
        else snprintf(table_path, sizeof(table_path), "isa-rewrite.bin");
        isa_table = isa_table_load(table_path);
        if (!isa_table) fail("CPU needs MOVBE/BMI1/LZCNT emulation but the ISA rewrite table is unavailable");
        if (isa_table_matches(isa_table, image, size))
            fail("ISA rewrite table does not match the linked boot image");
    }
    if (!cpu_only) {
        char title[128]="Bloodborne", serial[16]="UNKNOWN", sfo[4096];
        uint32_t attributes=0;
        snprintf(sfo,sizeof(sfo),"%s/sce_sys/param.sfo",app0 ? app0 : ".");
        sfo_value(sfo,"TITLE",title,sizeof(title),NULL);
        sfo_value(sfo,"TITLE_ID",serial,sizeof(serial),NULL);
        sfo_value(sfo,"ATTRIBUTE",NULL,0,&attributes);
        uint64_t sdk=0;
        if (procparam) memcpy(&sdk,image+procparam+16,8);
        BbGpuConfig gpu={title,serial,user_dir ? user_dir : "user",(uint32_t)sdk,attributes,1920,1080};
        gpu_enabled=1;
        if (bbgpu_init(&gpu)) fail("GPU initialization failed");
        printf("GPU: window and Vulkan presenter ready; SDK 0x%08x, %u HLE symbols\n",(unsigned)sdk,bbgpu_symbol_count());
    }
    unsigned char *traps = allocate(round_page((import_count + 1) * 32));
#ifdef _WIN32
    traps_base = traps; traps_size = round_page((import_count + 1) * 32);
#endif
    unsigned char *data_traps = allocate((import_count + 1) * page_size);
    protect(data_traps, (import_count + 1) * page_size, 0);
    for (uint64_t i = 0; i < import_count; ++i) {
        unsigned char *t = traps + i * 32;
        uint32_t index = (uint32_t)i;
        uintptr_t handler = (uintptr_t)unresolved;
        t[0] = 0x48; t[1] = 0x89; t[2] = 0xfe;
        t[3] = 0xbf; memcpy(t + 4, &index, 4);
        t[8] = 0x48; t[9] = 0xb8; memcpy(t + 10, &handler, 8);
        t[18] = 0xff; t[19] = 0xe0;
    }
    for (uint64_t i = 0; i < nr; ++i) {
        uintptr_t value = relocs[i].kind == 2 ? (uintptr_t)(data_traps + page_size * relocs[i].value + relocs[i].addend)
                        : relocs[i].kind == 1 ? (uintptr_t)(traps + 32 * relocs[i].value)
                        : (uintptr_t)image + relocs[i].value;
        if (relocs[i].kind) {
            uintptr_t resolved = sys_import_resolve(names[relocs[i].value], relocs[i].kind == 2);
            if (resolved) value = resolved + relocs[i].addend;
            else if (native_libc && bindings[relocs[i].value]) {
                uint64_t address=bindings[relocs[i].value];
                if (binding_kinds[relocs[i].value]!=relocs[i].kind || !mapped(segments,ns,address,relocs[i].addend+1)) fail("native export kind/range mismatch");
                value=(uintptr_t)image+address+relocs[i].addend;
            }
        }
        memcpy(image + relocs[i].target, &value, 8);
    }
    if (patch_file) apply_patches(patch_file, segments, ns, relocs, nr);
    if (!cpu_only) {
        const char *respawn_fix = getenv("BB_RESPAWN_DELAY_FIX");
        if (respawn_fix && !strcmp(respawn_fix, "0")) {
            puts("Respawn loading: original minimum delay requested (BB_RESPAWN_DELAY_FIX=0)");
        } else {
            int patched = bb_patch_respawn_delay(image, (size_t)size);
            if (patched == BB_RESPAWN_APPLIED || patched == BB_RESPAWN_ALREADY_APPLIED)
                printf("Respawn loading: 12-second minimum delay removed%s; readiness checks preserved\n",
                       patched == BB_RESPAWN_ALREADY_APPLIED ? " (already patched)" : "");
            else
                puts("Respawn loading: unsupported or modified instructions; minimum-delay fix skipped");
        }
    }
#ifdef _WIN32
    printf("Guest thread pointer reads: %" PRIu64 " use TEB TLS slot %u\n", patch_tls_reads(segments, ns), runtime_win_tls_slot());
#endif
    if (!cpu_only) bbgpu_patch_image(image, size);
    if (isa_table) {
        isa_stubs_size = round_page(isa_stub_area_size(isa_table));
        if (isa_stubs_size) isa_stubs = allocate(isa_stubs_size);
        IsaReport report = isa_rewrite_apply(isa_table, image, isa_stubs, isa_missing_sets);
        if (report.unsupported_needed)
            fail("the linked image contains MOVBE/BMI1/LZCNT encodings that this compatibility layer cannot rewrite");
        if (isa_stubs && isa_stubs_size) protect(isa_stubs, isa_stubs_size, 5);
        printf("ISA: rewrote %u sites (%u jmp, %u relocated, %u int3); %u changed sites skipped\n",
               report.rewritten, report.jmp, report.relocated, report.int3, report.skipped);
    }
    protect(traps, round_page((import_count + 1) * 32), 5);
    protect(image, round_page(size), 0);
    int executable_entry = 0;
    for (uint64_t i = 0; i < ns; ++i) {
        protect(image + segments[i].address, round_page(segments[i].size), (unsigned)segments[i].flags);
        if ((segments[i].flags & 1) && entry >= segments[i].address && entry - segments[i].address < segments[i].size)
            executable_entry = 1;
    }
    if (!executable_entry) fail("entry is not executable");
    printf("Mapped %" PRIu64 " bytes, %" PRIu64 " segments; applied %" PRIu64 " relocations\n", size, ns, nr);
    if (native_libc) {
        for (uint64_t m=0;m<module_count;++m) {
            int init_executable=0;
            for (uint64_t i=0;i<ns;++i)
                if ((segments[i].flags&1) && modules[m].init>=segments[i].address && modules[m].init-segments[i].address<segments[i].size) init_executable=1;
            if (!init_executable) fail("module init is not executable");
            if (modules[m].tls_module)
                runtime_set_module_tls(modules[m].tls_module,image+modules[m].tls_address,modules[m].tls_filesz,modules[m].tls_memsz);
        }
        runtime_set_main_tls(image+main_tls[0],main_tls[1],main_tls[2],main_tls[3]);
        runtime_thread_attach_main();
        runtime_set_procparam(image+procparam);
        for (uint64_t m=0;m<module_count;++m) {
            printf("Starting linked module %" PRIu64 " at image offset 0x%" PRIx64 "; native bindings=%" PRIu64 "\n",m,modules[m].init,nb);
            typedef int (ABI *ModuleInit)(uint64_t,void *,void *);
            int result=((ModuleInit)(image+modules[m].init))(0,NULL,NULL);
            printf("Module %" PRIu64 " initializer returned %d\n",m,result);
            if (result) fail("module initializer failed");
        }
    }
#ifdef _WIN32
    SetUnhandledExceptionFilter(unhandled_filter);
#endif
    if (!cpu_only) bbgpu_start_online();
    printf("Entering original x86-64 code at guest offset 0x%" PRIx64 "\n", entry);
    entered_game=1;
    struct { uint64_t argc; const char *argv[2]; } params = {1, {"/app0/eboot.bin", NULL}};
#ifdef _WIN32
    typedef void (ABI *Entry)(void *, void (ABI *)(void));
    ((Entry)(image + entry))(&params, guest_exit);
#else
    enum { MAIN_STACK=8*1024*1024 };
    unsigned char *stack=runtime_low_map(MAIN_STACK,PROT_READ|PROT_WRITE);
    if (!stack) fail("cannot allocate guest main stack");
    enter_on_stack(image+entry,&params,(void *)guest_exit,stack+MAIN_STACK-64);
#endif
    fail("entry unexpectedly returned");
}
