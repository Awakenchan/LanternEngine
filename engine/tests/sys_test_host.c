// SPDX-License-Identifier: GPL-3.0-or-later
#include <stddef.h>
#include <stdint.h>
#include <time.h>
#ifndef _WIN32
#include <pthread.h>
#endif

#define WEAK __attribute__((weak))

typedef struct BbPcPad BbPcPad;

WEAK uint64_t host_realtime_ns(void) { return UINT64_C(1793318400) * 1000000000 + 123456789; }
WEAK uint64_t host_monotonic_ns(void) { return 0; }
WEAK void host_sleep_until_ns(uint64_t deadline) { (void)deadline; }
WEAK int bbgpu_window_in_background(void) { return 0; }
WEAK void bbgpu_save_backups_write(int begin) { (void)begin; }
WEAK int bbgpu_overlay_captures_input(void) { return 0; }
WEAK void bbgpu_anti_lag_before_input(void) {}
WEAK int bbgpu_menu_buttons(uint32_t *confirm, uint32_t *back) { (void)confirm; (void)back; return 0; }
WEAK int bbgpu_pc_input(BbPcPad *pad, const _Bool *keys, int count, int gamepad) { (void)pad; (void)keys; (void)count; (void)gamepad; return 0; }
WEAK int bbgpu_text_input_begin(const char *initial, const char *prompt) { (void)initial; (void)prompt; return 0; }
WEAK int bbgpu_text_input_poll(char *out, uint64_t size) { (void)out; (void)size; return 0; }
WEAK int runtime_block_begin(int kind, uintptr_t object) { (void)kind; (void)object; return 0; }
WEAK void runtime_block_end(int slot) { (void)slot; }
WEAK void runtime_wait_note(int kind, uint64_t ns) { (void)kind; (void)ns; }
WEAK void host_sleep_ns(uint64_t ns) {
    struct timespec t = {(time_t)(ns / 1000000000), (long)(ns % 1000000000)};
    nanosleep(&t, NULL);
}
WEAK void sys_report(void) {}
WEAK uintptr_t bbgpu_resolve(const char *nid) { (void)nid; return 0; }
WEAK void runtime_guest_call_sites(uint64_t out[3]) { out[0] = out[1] = out[2] = 0; }
WEAK void *runtime_low_map(size_t size, int prot) { (void)size; (void)prot; return NULL; }
#ifndef _WIN32
WEAK int host_thread_start(pthread_t *thread, size_t stack, void *(*entry)(void *), void *argument) { (void)stack; return pthread_create(thread, NULL, entry, argument); }
WEAK int host_thread_join(pthread_t thread) { return pthread_join(thread, NULL); }
WEAK void host_thread_detach(pthread_t thread) { pthread_detach(thread); }
#endif
#ifdef _WIN32
WEAK int runtime_win_remove_tree(const char *path) { (void)path; return 0; }
#endif
WEAK void runtime_memory_note_write(uintptr_t address, uint64_t size) { (void)address; (void)size; }
WEAK int runtime_memory_guest_writable(uintptr_t address, uint64_t size) { (void)address; (void)size; return 0; }
