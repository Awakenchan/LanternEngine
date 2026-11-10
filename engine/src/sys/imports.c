// SPDX-License-Identifier: GPL-3.0-or-later
#include "sys.h"
#include "runtime.h"
#include <stdio.h>
#include <string.h>

typedef struct {
    const char *nid, *symbol;
} ImportName;

static const ImportName import_names[] = {
#include "import_names.inc"
};
static uint64_t capabilities;

void sys_imports_start(uint64_t flags) {
    capabilities = flags;
    if (flags & 1) sys_start_seed_guard();
}

void sys_report(void) {
    sys_start_report();
    sys_module_report();
    runtime_thread_report();
    sys_mutex_report();
    sys_rwlock_report();
    sys_sema_report();
    sys_app_content_report();
    runtime_memory_report();
    sys_ajm_report();
    sys_audio_out_report();
    sys_pad_report();
    sys_save_data_report();
    sys_file_report();
}

uintptr_t sys_import_resolve(const char *name, int is_data) {
    if (!(capabilities & 1)) return 0;
    if (is_data) return sys_start_data(name);
    uintptr_t own = sys_resolve(name);
    if (own) return own;
    uintptr_t offline = sys_net_offline_resolve(name);
    return offline ? offline : bbgpu_resolve(name);
}

const char *sys_import_symbol(const char *nid) {
    for (size_t i = 0; i < sizeof(import_names) / sizeof(*import_names); ++i)
        if (!strcmp(nid, import_names[i].nid)) return import_names[i].symbol;
    return NULL;
}

const char *sys_import_label(const char *nid) {
    const char *symbol = sys_import_symbol(nid);
    return symbol ? symbol : "name not resolved; see analysis.json import_name_hints";
}
