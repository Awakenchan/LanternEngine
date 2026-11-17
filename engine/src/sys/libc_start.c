// SPDX-License-Identifier: GPL-3.0-or-later
#define _CRT_RAND_S
#include "sys.h"
#include "runtime.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MODULE_SLOTS 8

typedef struct {
    union {
        void (SYS_ABI *plain)(void);
        void (SYS_ABI *with_argument)(void *);
    } call;
    void *argument, *dso;
    int takes_argument, pending;
} ExitEntry;

typedef struct {
    const void *image;
    uint64_t file_bytes, memory_bytes;
} ModuleTls;

static HostMutex exit_lock = HOST_MUTEX_INIT;
static ExitEntry *exit_entries;
static size_t exit_count, exit_capacity;
static _Atomic size_t init_calls, atexit_calls, cxa_calls, guards_taken, guards_done;
static uint64_t stack_guard;
static int32_t need_libc_internal = 1;
static ModuleTls module_tls[MODULE_SLOTS];
static _Thread_local unsigned char *thread_tls[MODULE_SLOTS];
static _Atomic uint32_t last_guard_owner;
static _Thread_local uint32_t guard_owner;
static void *process_param;
static void **heap_api;

void runtime_set_procparam(void *param) {
    process_param = param;
    const uint64_t *fields = param;
    if (!fields || fields[0] < 0x38 || !fields[6]) return;
    const uint64_t *libc = (const uint64_t *)fields[6];
    printf("Runtime: libc param size %#llx, entries %u", (unsigned long long)libc[0], (unsigned)libc[1]);
    static const char *const labels[] = {"heap size", "delayed alloc", "extended alloc", "initial size"};
    for (unsigned i = 0; i < 4 && (uint64_t)(2 + i) * 8 < libc[0]; ++i) {
        const void *field = (const void *)libc[2 + i];
        if (!field) printf(", %s -", labels[i]);
        else if (i == 0 || i == 3) printf(", %s %#llx", labels[i], (unsigned long long)*(const uint64_t *)field);
        else printf(", %s %u", labels[i], *(const uint32_t *)field);
    }
    printf("\n");
}

static SYS_ABI void *get_process_param(void) {
    return process_param;
}

static SYS_ABI void set_heap_api(void **api) {
    heap_api = api;
    puts("Runtime: application heap API registered");
}

void **runtime_application_heap_api(void) {
    return heap_api;
}

void runtime_set_module_tls(uint64_t module, const void *data, uint64_t filesz, uint64_t memsz) {
    if (module < 2 || module >= MODULE_SLOTS) {
        fputs("STOP: unsupported TLS module id\n", stderr);
        exit(21);
    }
    module_tls[module] = (ModuleTls){data, filesz, memsz};
}

void runtime_set_libc_tls(const void *data, uint64_t filesz, uint64_t memsz) {
    runtime_set_module_tls(2, data, filesz, memsz);
}

static SYS_ABI void *tls_get_addr(const uint64_t *index) {
    uint64_t module = index ? index[0] : 0;
    if (module < 2 || module >= MODULE_SLOTS || !module_tls[module].image || index[1] >= module_tls[module].memory_bytes) {
        fputs("STOP: unsupported TLS module/offset\n", stderr);
        exit(21);
    }
    unsigned char *block = thread_tls[module];
    if (!block) {
        block = calloc(1, module_tls[module].memory_bytes);
        if (!block) {
            fputs("Cannot allocate module TLS\n", stderr);
            exit(1);
        }
        memcpy(block, module_tls[module].image, module_tls[module].file_bytes);
        thread_tls[module] = block;
    }
    return block + index[1];
}

void sys_start_seed_guard(void) {
#ifdef _WIN32
    unsigned int parts[2];
    if (rand_s(&parts[0]) || rand_s(&parts[1])) {
        fputs("Cannot initialize stack canary\n", stderr);
        exit(1);
    }
    memcpy(&stack_guard, parts, sizeof(stack_guard));
#else
    FILE *source = fopen("/dev/urandom", "rb");
    if (!source || fread(&stack_guard, sizeof(stack_guard), 1, source) != 1) {
        fputs("Cannot initialize stack canary\n", stderr);
        exit(1);
    }
    fclose(source);
#endif
    stack_guard &= ~UINT64_C(255);
}

void sys_start_report(void) {
    printf("Runtime: _init_env=%zu, atexit=%zu, __cxa_atexit=%zu, registered handlers=%zu\n", atomic_load(&init_calls),
           atomic_load(&atexit_calls), atomic_load(&cxa_calls), exit_count);
    printf("Runtime: static guards acquired=%zu, released=%zu\n", atomic_load(&guards_taken), atomic_load(&guards_done));
}

static SYS_ABI void init_env(void) {
    atomic_fetch_add(&init_calls, 1);
    puts("Runtime: _init_env returned (verified libc implementation: RET)");
}

static int add_exit_entry(ExitEntry entry) {
    host_lock(&exit_lock);
    if (exit_count == exit_capacity) {
        size_t capacity = exit_capacity ? exit_capacity * 2 : 64;
        ExitEntry *grown = capacity > 1024 * 1024 ? NULL : realloc(exit_entries, capacity * sizeof(*grown));
        if (!grown) {
            host_unlock(&exit_lock);
            return -1;
        }
        exit_entries = grown;
        exit_capacity = capacity;
    }
    entry.pending = 1;
    exit_entries[exit_count++] = entry;
    host_unlock(&exit_lock);
    return 0;
}

static SYS_ABI int guest_atexit(void (SYS_ABI *call)(void)) {
    if (!call) return -1;
    atomic_fetch_add(&atexit_calls, 1);
    return add_exit_entry((ExitEntry){.call.plain = call});
}

static SYS_ABI int guest_cxa_atexit(void (SYS_ABI *call)(void *), void *argument, void *dso) {
    if (!call) return -1;
    atomic_fetch_add(&cxa_calls, 1);
    return add_exit_entry((ExitEntry){.call.with_argument = call, .argument = argument, .dso = dso, .takes_argument = 1});
}

void runtime_finalize(void *dso) {
    for (;;) {
        host_lock(&exit_lock);
        size_t i = exit_count;
        while (i && (!exit_entries[i - 1].pending || (dso && exit_entries[i - 1].dso != dso))) --i;
        if (!i) {
            host_unlock(&exit_lock);
            return;
        }
        ExitEntry entry = exit_entries[i - 1];
        exit_entries[i - 1].pending = 0;
        host_unlock(&exit_lock);
        if (entry.takes_argument) entry.call.with_argument(entry.argument);
        else entry.call.plain();
    }
}

static SYS_ABI void guest_finalize(void *dso) {
    runtime_finalize(dso);
}

static SYS_ABI int guard_acquire(uint64_t *guard) {
    _Atomic uint64_t *state = (_Atomic uint64_t *)guard;
    if (atomic_load_explicit(state, memory_order_acquire) & 1) return 0;
    if (!guard_owner) guard_owner = atomic_fetch_add(&last_guard_owner, 1) + 1;
    for (;;) {
        uint64_t seen = 0;
        if (atomic_compare_exchange_strong_explicit(state, &seen, (uint64_t)guard_owner << 32, memory_order_acq_rel,
                                                    memory_order_acquire))
            break;
        if (seen & 1) return 0;
        if ((uint32_t)(seen >> 32) == guard_owner) {
            fputs("STOP: recursive/concurrent static initialization is not supported yet\n", stderr);
            exit(21);
        }
        host_yield();
    }
    atomic_fetch_add(&guards_taken, 1);
    return 1;
}

static SYS_ABI void guard_release(uint64_t *guard) {
    atomic_store_explicit((_Atomic uint64_t *)guard, 1, memory_order_release);
    atomic_fetch_add(&guards_done, 1);
}

static SYS_ABI void guard_abort(uint64_t *guard) {
    atomic_fetch_and_explicit((_Atomic uint64_t *)guard, UINT64_C(0xffffffff), memory_order_release);
}

static SYS_ABI __attribute__((noreturn)) void stack_check_failed(void) {
    fputs("STOP: guest stack protector detected corruption\n", stderr);
    exit(22);
}

static SYS_ABI void *guest_memset(void *target, int value, size_t size) {
    memset(target, value, size);
    runtime_memory_note_cpu_write((uintptr_t)target, size);
    return target;
}

static SYS_ABI void *guest_memcpy(void *target, const void *source, size_t size) {
    memcpy(target, source, size);
    runtime_memory_note_cpu_write((uintptr_t)target, size);
    return target;
}

static SYS_ABI void *guest_memmove(void *target, const void *source, size_t size) {
    memmove(target, source, size);
    runtime_memory_note_cpu_write((uintptr_t)target, size);
    return target;
}

static SYS_ABI int guest_memcmp(const void *a, const void *b, size_t size) {
    return memcmp(a, b, size);
}

static SYS_ABI size_t guest_strlen(const char *text) {
    return strlen(text);
}

static SYS_ABI __attribute__((noreturn)) void guest_exit(int status) {
    printf("Runtime: guest requested exit(%d)\n", status);
    runtime_flush_settings();
    runtime_finalize(NULL);
    sys_report();
    exit(status);
}

static const SysExport start_exports[] = {
    {"bzQExy189ZI", "_init_env", init_env},
    {"8G2LB+A3rzg", "atexit", guest_atexit},
    {"tsvEmnenz48", "__cxa_atexit", guest_cxa_atexit},
    {"uMei1W9uyNo", "exit", guest_exit},
    {"H2e8t5ScQGc", "__cxa_finalize", guest_finalize},
    {"3GPpjQdAMTw", "__cxa_guard_acquire", guard_acquire},
    {"9rAeANT2tyE", "__cxa_guard_release", guard_release},
    {"2emaaluWzUw", "__cxa_guard_abort", guard_abort},
    {"Ou3iL1abvng", "__stack_chk_fail", stack_check_failed},
    {"8zTFvBIAIN8", "memset", guest_memset},
    {"Q3VBxCXhUHs", "memcpy", guest_memcpy},
    {"+P6FRGH4LfA", "memmove", guest_memmove},
    {"DfivPArhucg", "memcmp", guest_memcmp},
    {"j4ViWNHEgww", "strlen", guest_strlen},
    {"vNe1w4diLCs", "__tls_get_addr", tls_get_addr},
    {"959qrazPIrg", "sceKernelGetProcParam", get_process_param},
    {"p5EcQeEeJAE", "_sceKernelRtldSetApplicationHeapAPI", set_heap_api},
};
SYS_LIBRARY(sys_start, "libc", start_exports);

static const struct {
    const char *nid, *name;
    void *address;
} start_data[] = {
    {"f7uOxY9mM1U", "__stack_chk_guard", &stack_guard},
    {"ZT4ODD2Ts9o", "Need_sceLibcInternal", &need_libc_internal},
};

uintptr_t sys_start_data(const char *nid) {
    size_t length = strcspn(nid, "#");
    for (size_t i = 0; i < sizeof(start_data) / sizeof(*start_data); ++i)
        if (strlen(start_data[i].nid) == length && !memcmp(start_data[i].nid, nid, length)) return (uintptr_t)start_data[i].address;
    return 0;
}
