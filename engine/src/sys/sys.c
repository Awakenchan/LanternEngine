// SPDX-License-Identifier: GPL-3.0-or-later
#include "sys.h"
#include <stdatomic.h>
#include <string.h>

static const SysLibrary *const libraries[] = {
    &sys_rtc, &sys_user_service, &sys_system_service, &sys_np_trophy, &sys_common_dialog, &sys_msg_dialog,
    &sys_save_data_dialog, &sys_np_profile_dialog, &sys_np_commerce, &sys_ime_dialog, &sys_play_go,
    &sys_disc_map, &sys_mouse, &sys_voice, &sys_audio_in, &sys_app_content, &sys_sysmodule, &sys_posix_time, &sys_pad,
    &sys_audio_out, &sys_ajm, &sys_save_data, &sys_semaphore, &sys_rwlock, &sys_mutex, &sys_posix_mutex, &sys_thread, &sys_posix_thread, &sys_clock, &sys_posix_clock, &sys_libc_time, &sys_process, &sys_file, &sys_posix_file, &sys_memory, &sys_start, &sys_equeue, &sys_video_out, &sys_av_player, &sys_module_info,
};

static _Atomic int32_t last_id;

int32_t sys_new_id(void) {
    return atomic_fetch_add(&last_id, 1) + 1;
}

static size_t nid_length(const char *nid) {
    const char *hash = strchr(nid, '#');
    return hash ? (size_t)(hash - nid) : strlen(nid);
}

const SysExport *sys_find(const char *nid, const SysLibrary **library) {
    size_t length = nid_length(nid);
    for (size_t i = 0; i < sizeof(libraries) / sizeof(*libraries); ++i) {
        const SysLibrary *lib = libraries[i];
        for (size_t j = 0; j < lib->count; ++j) {
            const char *candidate = lib->exports[j].nid;
            if (strlen(candidate) == length && !memcmp(candidate, nid, length)) {
                if (library) *library = lib;
                return &lib->exports[j];
            }
        }
    }
    return NULL;
}

uintptr_t sys_resolve(const char *nid) {
    const SysExport *found = sys_find(nid, NULL);
    return found ? (uintptr_t)found->function : 0;
}

size_t sys_library_count(void) {
    return sizeof(libraries) / sizeof(*libraries);
}

const SysLibrary *sys_library(size_t index) {
    return index < sys_library_count() ? libraries[index] : NULL;
}
