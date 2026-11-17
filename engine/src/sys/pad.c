// SPDX-License-Identifier: GPL-3.0-or-later
#include "sys.h"
#include "pad.h"
#include "gpu/bbgpu.h"
#include "host_sync.h"
#include <stdio.h>
#include <string.h>

enum {
    PAD_ERROR_ARGUMENT = (int32_t)0x80920001,
    PAD_ERROR_HANDLE = (int32_t)0x80920003,
    PAD_ERROR_OPENED = (int32_t)0x80920004,
    PAD_ERROR_NOT_INITIALIZED = (int32_t)0x80920005,
};

#define THE_HANDLE 1
#define THE_USER 1
#define PAD_TYPE_STANDARD 0
#define PAD_TYPE_SPECIAL 2

typedef struct {
    float pixel_density;
    uint16_t resolution_x, resolution_y;
    uint8_t dead_zone_left, dead_zone_right, connection_type, connected_count;
    uint8_t connected, padding[3];
    int32_t device_class;
    uint8_t reserved[8];
} PadControllerInfo;
_Static_assert(sizeof(PadControllerInfo) == 28, "PadControllerInfo is 28 bytes");

static HostMutex lock = HOST_MUTEX_INIT;
static int initialized, opened, holding_after_overlay;
static size_t reads;

uint64_t pad_now_us(void) {
    return host_monotonic_ns() / 1000;
}

void pad_touch_click(PadData *pad, int right_half) {
    pad->buttons |= PAD_TOUCHPAD;
    pad->touch_count = 1;
    pad->touches[0] = (PadTouch){.x = right_half ? 1440 : 480, .y = 471, .id = 0};
}

static void number_touches(PadData *pad) {
    static uint8_t next = 1, ids[2];
    static int down[2];
    static uint64_t first_down;
    for (int i = 0; i < 2; ++i) {
        int touching = i < pad->touch_count;
        if (touching && !down[i]) {
            ids[i] = next;
            next = next == 127 ? 1 : next + 1;
        }
        down[i] = touching;
        if (touching) pad->touches[i].id = ids[i];
    }
    if (!pad->touch_count) first_down = 0;
    else if (!first_down) first_down = pad->timestamp;
    pad->touch_held_time = pad->touch_count ? (uint32_t)(pad->timestamp - first_down) : 0;
}

static void sample(PadData *pad) {
    pad_host_sample(pad);
    if (bbgpu_overlay_captures_input()) {
        holding_after_overlay = 1;
        return;
    }
    pad_script_record(pad);
    pad_script_buttons(pad);
    if (holding_after_overlay) {
        if (pad->buttons) {
            pad->buttons = 0;
            pad->l2 = pad->r2 = 0;
        } else {
            holding_after_overlay = 0;
        }
    }
    pad_script_overrides(pad);
    number_touches(pad);
}

static int handle_ok(int32_t handle) {
    return handle == THE_HANDLE && opened;
}

static SYS_ABI int32_t pad_init(void) {
    host_lock(&lock);
    initialized = 1;
    host_unlock(&lock);
    return 0;
}

static SYS_ABI int32_t pad_open(int32_t user, int32_t type, int32_t index, const void *param) {
    (void)param;
    if (!initialized) return PAD_ERROR_NOT_INITIALIZED;
    if (user != THE_USER || (type != PAD_TYPE_STANDARD && type != PAD_TYPE_SPECIAL) || index) return PAD_ERROR_ARGUMENT;
    host_lock(&lock);
    int was_open = opened;
    opened = 1;
    host_unlock(&lock);
    if (was_open) return PAD_ERROR_OPENED;
    puts("Runtime: pad opened for user 1 (SDL gamepad or keyboard)");
    return THE_HANDLE;
}

static SYS_ABI int32_t pad_close(int32_t handle) {
    if (!handle_ok(handle)) return PAD_ERROR_HANDLE;
    opened = 0;
    return 0;
}

static SYS_ABI int32_t pad_read_state(int32_t handle, PadData *pad) {
    if (!handle_ok(handle)) return PAD_ERROR_HANDLE;
    if (!pad) return PAD_ERROR_ARGUMENT;
    bbgpu_anti_lag_before_input();
    host_lock(&lock);
    sample(pad);
    ++reads;
    host_unlock(&lock);
    bbgpu_pacing_pad_read();
    return 0;
}

static SYS_ABI int32_t pad_get_controller_information(int32_t handle, PadControllerInfo *info) {
    if (!handle_ok(handle)) return PAD_ERROR_HANDLE;
    if (!info) return PAD_ERROR_ARGUMENT;
    memset(info, 0, sizeof(*info));
    info->pixel_density = 44.86f;
    info->resolution_x = PAD_TOUCH_WIDTH;
    info->resolution_y = PAD_TOUCH_HEIGHT;
    info->dead_zone_left = info->dead_zone_right = 2;
    info->connected = 1;
    host_lock(&lock);
    pad_host_refresh();
    uint8_t count = pad_host_connected_count();
    info->connected_count = count ? count : 1;
    host_unlock(&lock);
    return 0;
}

static SYS_ABI int32_t pad_set_vibration(int32_t handle, const uint8_t *motors) {
    if (!handle_ok(handle)) return PAD_ERROR_HANDLE;
    if (!motors) return PAD_ERROR_ARGUMENT;
    host_lock(&lock);
    pad_host_rumble(motors[0], motors[1]);
    host_unlock(&lock);
    return 0;
}

static SYS_ABI int32_t pad_accept(int32_t handle) {
    return handle_ok(handle) ? 0 : PAD_ERROR_HANDLE;
}

static SYS_ABI int32_t pad_accept_flag(int32_t handle, uint8_t flag) {
    (void)flag;
    return pad_accept(handle);
}

void sys_pad_lock(void) {
    host_lock(&lock);
}

void sys_pad_unlock(void) {
    host_unlock(&lock);
}

void sys_pad_report(void) {
    printf("Runtime: pad reads=%zu, gamepad=%s\n", reads, pad_host_gamepad_name());
}

static const SysExport exports[] = {
    {"6ncge5+l5Qs", "scePadClose", pad_close},
    {"gjP9-KQzoUk", "scePadGetControllerInformation", pad_get_controller_information},
    {"hv1luiJrqQM", "scePadInit", pad_init},
    {"xk0AcarP3V4", "scePadOpen", pad_open},
    {"YndgXqQVV7c", "scePadReadState", pad_read_state},
    {"rIZnR6eSpvk", "scePadResetOrientation", pad_accept},
    {"r44mAxdSG+U", "scePadSetAngularVelocityDeadbandState", pad_accept_flag},
    {"vDLMoJLde8I", "scePadSetTiltCorrectionState", pad_accept_flag},
    {"yFVnOdGxvZY", "scePadSetVibration", pad_set_vibration},
};

SYS_LIBRARY(sys_pad, "libScePad", exports);
