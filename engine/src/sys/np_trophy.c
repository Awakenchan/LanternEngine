// SPDX-License-Identifier: GPL-3.0-or-later
#include "sys.h"
#include <stdio.h>
#include <string.h>

#define TROPHY_ERROR_ARGUMENT ((int32_t)0x80551604)
#define NO_PLATINUM (-1)

static SYS_ABI int32_t trophy_create_context(int32_t *context, int32_t user, uint32_t service_label,
                                             uint64_t options) {
    (void)user;
    (void)service_label;
    (void)options;
    if (!context) return TROPHY_ERROR_ARGUMENT;
    *context = sys_new_id();
    return 0;
}

static SYS_ABI int32_t trophy_create_handle(int32_t *handle) {
    if (!handle) return TROPHY_ERROR_ARGUMENT;
    *handle = sys_new_id();
    return 0;
}

static SYS_ABI int32_t trophy_register_context(int32_t context, int32_t handle, uint64_t options) {
    (void)context;
    (void)handle;
    (void)options;
    return 0;
}

static const char *trophy_role_name = "own";

void sys_trophy_role(const char *role) {
    __atomic_store_n(&trophy_role_name, role ? role : "own", __ATOMIC_RELAXED);
}

static SYS_ABI int32_t trophy_unlock_trophy(int32_t context, int32_t handle, int32_t trophy, int32_t *platinum) {
    (void)context;
    (void)handle;
    printf("Runtime: trophy %d unlocked as %s\n", trophy, __atomic_load_n(&trophy_role_name, __ATOMIC_RELAXED));
    if (platinum) *platinum = NO_PLATINUM;
    return 0;
}

static void clear_sized(void *block) {
    if (!block) return;
    uint64_t size;
    memcpy(&size, block, sizeof(size));
    if (size > 8 && size < 4096) memset((char *)block + 8, 0, size - 8);
}

static SYS_ABI int32_t trophy_get_game_info(int32_t context, int32_t handle, void *details, void *data) {
    (void)context;
    (void)handle;
    clear_sized(details);
    clear_sized(data);
    return 0;
}

static SYS_ABI int32_t trophy_get_trophy_info(int32_t context, int32_t handle, int32_t trophy, void *details,
                                              void *data) {
    (void)trophy;
    return trophy_get_game_info(context, handle, details, data);
}

static const SysExport exports[] = {
    {"XbkjbobZlCY", "sceNpTrophyCreateContext", trophy_create_context},
    {"q7U6tEAQf7c", "sceNpTrophyCreateHandle", trophy_create_handle},
    {"YYP3f2W09og", "sceNpTrophyGetGameInfo", trophy_get_game_info},
    {"qqUVGDgQBm0", "sceNpTrophyGetTrophyInfo", trophy_get_trophy_info},
    {"TJCAxto9SEU", "sceNpTrophyRegisterContext", trophy_register_context},
    {"28xmRUFao68", "sceNpTrophyUnlockTrophy", trophy_unlock_trophy},
};

SYS_LIBRARY(sys_np_trophy, "libSceNpTrophy", exports);
