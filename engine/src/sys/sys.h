// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LANTERN_SYS_H
#define LANTERN_SYS_H
#include <stddef.h>
#include <stdint.h>

#define SYS_ABI __attribute__((sysv_abi))

typedef struct {
    const char *nid;
    const char *name;
    void *function;
} SysExport;

typedef struct {
    const char *library;
    const SysExport *exports;
    size_t count;
} SysLibrary;

#define SYS_LIBRARY(variable, library_name, table) \
    const SysLibrary variable = { library_name, table, sizeof(table) / sizeof(*(table)) }

extern const SysLibrary sys_rtc, sys_user_service, sys_system_service, sys_np_trophy, sys_common_dialog,
    sys_msg_dialog, sys_save_data_dialog, sys_np_profile_dialog, sys_np_commerce, sys_ime_dialog, sys_play_go,
    sys_disc_map, sys_mouse, sys_voice, sys_audio_in, sys_app_content, sys_sysmodule, sys_posix_time, sys_pad,
    sys_audio_out, sys_ajm, sys_save_data, sys_semaphore, sys_rwlock, sys_mutex, sys_posix_mutex, sys_thread, sys_posix_thread, sys_clock, sys_posix_clock, sys_libc_time, sys_process, sys_file, sys_posix_file, sys_memory, sys_start, sys_equeue, sys_video_out, sys_av_player, sys_module_info;
void sys_module_register(const unsigned char *start, uint64_t size, uint64_t init, const char *name);
void sys_module_report(void);

int32_t sys_new_id(void);
void sys_imports_start(uint64_t flags);
void sys_report(void);
uintptr_t sys_import_resolve(const char *name, int is_data);
const char *sys_import_symbol(const char *nid);
const char *sys_import_label(const char *nid);
void sys_start_seed_guard(void);
void sys_start_report(void);
uintptr_t sys_start_data(const char *nid);
uintptr_t sys_net_offline_resolve(const char *nid);
uintptr_t bbgpu_resolve(const char *scoped_nid);
void sys_app_content_configure(const uint32_t values[5]);
void sys_app_content_report(void);
void sys_pad_report(void);
void sys_audio_out_report(void);
void sys_ajm_report(void);
void sys_save_data_configure(const char *title);
void sys_save_data_report(void);
void sys_sema_describe(uint32_t id, char *out, size_t size);
unsigned sys_sema_waiters(uint32_t id);
void sys_sema_census(uint64_t *live, uint64_t *found, uint64_t *steps);
void sys_sema_report(void);
void sys_rwlock_census(uint64_t *live, uint64_t *found, uint64_t *steps);
void sys_rwlock_report(void);
void sys_mutex_describe(uintptr_t object, char *out, size_t size);
void sys_mutex_report(void);
void sys_file_configure(const char *app0, const char *user);
int sys_file_mount(const char *guest, const char *host);
void sys_file_unmount(const char *guest);
int sys_file_translate(const char *guest, char *out, size_t size);
const char *sys_file_user_dir(void);
int64_t sys_file_open(const char *path, int flags, int mode);
int64_t sys_file_close(int fd);
int64_t sys_file_read(int fd, void *buffer, uint64_t size);
void sys_file_report(void);
void sys_file_stream_configure(const char *app0);
int sys_file_stream_sequential(void);
void sys_file_stream_opened(const char *guest, const char *host, uint64_t started);
void sys_file_stream_closed(uint64_t opened);
void sys_file_trace(char op, int fd, int64_t a, int64_t b, int64_t c, const char *path);
uint64_t sys_file_stream_read_begin(int loader);
void sys_file_stream_read_end(int loader, uint64_t started, int64_t bytes);
void sys_file_stream_report(void);
void sys_pad_lock(void);
void sys_pad_unlock(void);
int sys_pad_type(void);
int sys_pad_button(uint32_t button, int right_side);

uintptr_t sys_resolve(const char *nid);
const SysExport *sys_find(const char *nid, const SysLibrary **library);
size_t sys_library_count(void);
const SysLibrary *sys_library(size_t index);

#endif
