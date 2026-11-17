// SPDX-License-Identifier: GPL-2.0-or-later
#include <cstdlib>
#include <chrono>
#include <cstdint>
#include <cstddef>
#include <setjmp.h>
#ifdef _WIN32
struct alignas(16) RuntimeRecoverBuf {
    unsigned char registers[256];
};
#else
typedef sigjmp_buf RuntimeRecoverBuf;
#endif
extern "C" {
void* runtime_low_map(size_t, int) { return nullptr; }
void sys_video_out_set_host(const void*) {}
int sys_video_out_is_open(void) { return 0; }
int sys_video_out_flip_rate(void) { return 0; }
const uint64_t* sys_video_out_labels(void) { static uint64_t labels[16]; return labels; }
int sys_video_out_buffer(int, const void**, uint64_t*) { return 0; }
int sys_video_out_queue_flip(int, int64_t, int) { return 0; }
void sys_video_out_flip_done(int, int64_t, int) {}
int sys_equeue_exists(int64_t) { return 0; }
int sys_equeue_add(int64_t, uint64_t, int16_t, void*) { return -1; }
int sys_equeue_remove(int64_t, uint64_t, int16_t) { return -1; }
int sys_equeue_trigger(int64_t, uint64_t, int16_t, uint64_t) { return -1; }
uint64_t sys_equeue_event_data(const void*) { return 0; }
void sys_equeue_report(double) {}
void sys_av_player_set_decoder(const void*) {}
void sys_trophy_role(const char*) {}
void runtime_memory_invalidate(uintptr_t, uint64_t) {}
#ifndef LANTERN_TEST_FILE_MOUNT
int sys_file_mount(const char*, const char*) { return -1; }
void sys_file_unmount(const char*) {}
#endif
__thread RuntimeRecoverBuf* runtime_fault_recover = nullptr;
int runtime_setjmp(RuntimeRecoverBuf*) { return 0; }
[[noreturn]] void runtime_longjmp(RuntimeRecoverBuf*) { std::abort(); }
uint32_t runtime_disabled_optimizations = 0;
uint64_t runtime_experiment_bits = 0;
uint64_t runtime_tsc_frequency() { return 1000000000; }
#ifndef LANTERN_TEST_FILE_MOUNT
int sys_file_translate(const char*, char*, size_t) { std::abort(); }
#endif
uint64_t runtime_memory_clamp(uintptr_t, uint64_t) { std::abort(); }
uint64_t runtime_process_time_us() { return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
int runtime_memory_region(uintptr_t, uintptr_t*, uintptr_t*, int*) { std::abort(); }
void runtime_memory_set_gpu_hooks(void (*)(uintptr_t, uint64_t),
    void (*)(uintptr_t, uint64_t), void (*)(uintptr_t, uint64_t)) { std::abort(); }
void runtime_thread_attach_host(const char*) {}
#ifndef LANTERN_TEST_GUEST_THREADS
int32_t runtime_thread_spawn(void**, void* (*)(void*), void*, uint64_t, const char*) { std::abort(); }
#endif
int32_t runtime_thread_join_spawned(void*, void**) { std::abort(); }
void runtime_memory_gpu_protect(uintptr_t, uint64_t, int, int) { std::abort(); }
void runtime_restart() { std::abort(); }
uint64_t runtime_process_time_counter() { return runtime_process_time_us() * 1000; }
uint64_t host_realtime_ns() { return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count(); }
int32_t* runtime_errno() { std::abort(); }
int runtime_memory_write_backing(uintptr_t, const void*, uint64_t) { std::abort(); }
void runtime_memory_read_backing(uintptr_t, void*, uint64_t) { std::abort(); }
const uint64_t* runtime_memory_generation() { static const uint64_t generation = 0; return &generation; }
void runtime_memory_note_write(uintptr_t, uint64_t) { std::abort(); }
void runtime_memory_set_note_write_hook(void (*)(uintptr_t, uint64_t)) {}
void runtime_memory_set_cpu_write_hook(void (*)(uintptr_t, uint64_t)) {}
void runtime_memory_set_write_watch(uintptr_t, uint64_t, int) { std::abort(); }
int runtime_memory_restore_access(void*, uintptr_t, int) { return 0; }
void runtime_memory_set_guest_chunk_allocator(int (*)(uint64_t, uint64_t)) {}
void runtime_memory_set_guest_chunk_whole(int) {}
unsigned char* runtime_memory_backing(uint64_t* direct_size) {
    *direct_size = 0;
    return nullptr;
}
#ifdef _WIN32
int win_mem_commit(uint64_t, uint64_t) { return 0; }
#endif
int runtime_memory_vma_info(uintptr_t, int*, int*, uintptr_t*) { std::abort(); }
int runtime_memory_direct_phys(uintptr_t, uint64_t*, uintptr_t*) { std::abort(); }
void runtime_wait_report(double) {}
void runtime_sleep_stats(uint64_t* calls, uint64_t* ns) { *calls = 0; *ns = 0; }
uint64_t runtime_heap_growths() { return 0; }
void sys_rwlock_census(uint64_t* live, uint64_t* found, uint64_t* steps) { *live = *found = *steps = 0; }
void sys_sema_census(uint64_t* live, uint64_t* found, uint64_t* steps) { *live = *found = *steps = 0; }
void runtime_thread_census(uint64_t* made, uint64_t* finished, uint64_t* found, uint64_t* steps) {
    *made = *finished = *found = *steps = 0;
}
uint64_t runtime_memory_regions() { return 0; }
}

