#ifndef BB_RUNTIME_H
#define BB_RUNTIME_H
#include <time.h>
#include <stdint.h>
#include <stddef.h>
#ifdef _WIN32
#include "win32_compat.h"
#endif
#include "host_sync.h"
#ifdef _WIN32
typedef struct { _Alignas(16) unsigned char registers[256]; } RuntimeRecoverBuf;
int runtime_setjmp(RuntimeRecoverBuf *buf) __attribute__((returns_twice));
__attribute__((noreturn)) void runtime_longjmp(RuntimeRecoverBuf *buf);
#define RUNTIME_RECOVER_SET(buf) runtime_setjmp(&(buf))
#define RUNTIME_RECOVER_JUMP(buf) runtime_longjmp(&(buf))
#else
#include <setjmp.h>
typedef sigjmp_buf RuntimeRecoverBuf;
#define RUNTIME_RECOVER_SET(buf) sigsetjmp(buf, 0)
#define RUNTIME_RECOVER_JUMP(buf) siglongjmp(buf, 1)
#endif
extern __thread RuntimeRecoverBuf *runtime_fault_recover;
void runtime_restart(void);
void bbport_settings_flush_for_exit(void) __attribute__((weak));
static inline void runtime_flush_settings(void) {
    if (bbport_settings_flush_for_exit) bbport_settings_flush_for_exit();
}
#define ABI __attribute__((sysv_abi))
typedef void (ABI *GuestCallback)(void);
void runtime_finalize(void *dso);
void runtime_memory_report(void);
uint64_t runtime_memory_regions(void);
int runtime_memory_is_mapped(uintptr_t address, uint64_t size);
int runtime_memory_guest_writable(uintptr_t address, uint64_t size);
int runtime_memory_restore_access(void *context, uintptr_t address, int repeated);
void runtime_memory_note_write(uintptr_t address, uint64_t size);
void runtime_memory_note_cpu_write(uintptr_t address, uint64_t size);
void runtime_wait_note(int kind, uint64_t ns);
void runtime_wait_report(double frames);
void runtime_guest_call_sites(uint64_t out[3]);
uint64_t runtime_heap_growths(void);
uint64_t runtime_process_time_us(void);
static inline uint64_t runtime_wait_clock(void) { return host_monotonic_ns(); }
void runtime_set_libc_tls(const void *data, uint64_t filesz, uint64_t memsz);
void runtime_set_module_tls(uint64_t module, const void *data, uint64_t filesz, uint64_t memsz);
void runtime_set_procparam(void *param);
void **runtime_application_heap_api(void);
void runtime_thread_report(void);
void runtime_thread_census(uint64_t *made, uint64_t *finished, uint64_t *found, uint64_t *steps);
void runtime_set_main_tls(const void *data, uint64_t filesz, uint64_t memsz, uint64_t align);
void runtime_thread_attach_main(void);
int runtime_thread_has_record(void);
int runtime_thread_is_loader(void);
int32_t *runtime_errno(void);
enum { RUNTIME_BLOCK_MUTEX = 1, RUNTIME_BLOCK_SEMA = 2, RUNTIME_BLOCK_COND = 3 };
int runtime_block_begin(int kind, uintptr_t object);
void runtime_block_end(int slot);
void runtime_blocked_report(void);
int32_t runtime_guest_errno(int host_errno);
void *runtime_low_map(size_t size, int prot);
void runtime_thread_attach_host(const char *name);
#endif
