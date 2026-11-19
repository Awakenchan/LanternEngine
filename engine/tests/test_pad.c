// SPDX-License-Identifier: GPL-3.0-or-later
#include "sys/sys.h"
#include "sys/pad.h"
#include "gpu/shim/input/input.h"
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int capture, menu, failures;
static uint8_t actions;
static bool keyboard[SDL_SCANCODE_COUNT];

#define CHECK(condition) do { if (!(condition)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); ++failures; } } while (0)

uint64_t host_monotonic_ns(void) {
    return SDL_GetTicksNS();
}
int bbgpu_overlay_captures_input(void) { return capture; }
void bbgpu_anti_lag_before_input(void) {}
int bbgpu_pc_input(BbPcPad *pad, const bool *keys, int count, int gamepad) {
    actions = pad->actions;
    (void)keys;
    (void)count;
    (void)gamepad;
    return 0;
}
int bbgpu_menu_buttons(uint32_t *confirm, uint32_t *back) {
    *confirm = PAD_CROSS;
    *back = PAD_CIRCLE;
    return menu;
}
const bool *SDL_GetKeyboardState(int *count) {
    if (count) *count = SDL_SCANCODE_COUNT;
    return keyboard;
}

typedef int32_t (SYS_ABI *Init)(void);
typedef int32_t (SYS_ABI *Open)(int32_t, int32_t, int32_t, const void *);
typedef int32_t (SYS_ABI *Read)(int32_t, PadData *);

static void *find(const char *nid) {
    const SysExport *e = sys_find(nid, NULL);
    return e ? e->function : NULL;
}

static void write_file(const char *path, const char *text) {
    FILE *file = fopen(path, "w");
    CHECK(file != NULL);
    if (!file) return;
    fputs(text, file);
    fclose(file);
    SDL_Delay(25);
}

static void temp_path(char *out, size_t size, const char *name) {
    const char *dir = getenv("TMPDIR");
    if (!dir) dir = getenv("TEMP");
    if (!dir) dir = "/tmp";
    snprintf(out, size, "%s/lantern-%s-%u", dir, name, (unsigned)SDL_GetTicks());
}

static void set_env(const char *name, const char *value) {
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

static void update(void) {
    SDL_UpdateJoysticks();
    SDL_UpdateGamepads();
}

int main(void) {
    char pad_file[512], config[512];
    temp_path(pad_file, sizeof(pad_file), "pad-file");
    temp_path(config, sizeof(config), "pad-config");
    write_file(pad_file, "");
    write_file(config, "upscaler=fsr3\npad.cross=b\npad.circle=a\npad.r2=rightshoulder\n"
                       "pad.r1=righttrigger\nkey.cross=X, Space\npad.bogus=a\npad.menu_back=b\n"
                       "pad.l1=\npad.sprint=leftshoulder\npad.dodge=paddle1\npad.backstep=b\nkey.sprint=N\n");
    set_env("BB_PAD_FILE", pad_file);
    set_env("BB_CONFIG", config);
    set_env("SDL_VIDEODRIVER", "dummy");
    SDL_SetHint(SDL_HINT_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT, "0x1d50/0x6189");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    Init init = find("hv1luiJrqQM");
    Open open = find("xk0AcarP3V4");
    Read read = find("YndgXqQVV7c");
    CHECK(init && open && read);
    if (failures) return 1;
    CHECK(init() == 0 && open(1, 0, 0, NULL) == 1);
    PadData data;

    write_file(pad_file, "cross l3 touchpad_left");
    CHECK(read(1, &data) == 0);
    CHECK((data.buttons & (PAD_CROSS | PAD_L3 | PAD_TOUCHPAD)) == (PAD_CROSS | PAD_L3 | PAD_TOUCHPAD));
    CHECK(data.touch_count == 1 && data.touches[0].x == 480 && data.touches[0].y == 471 && data.touches[0].id == 1);
    write_file(pad_file, "touchpad_right");
    CHECK(read(1, &data) == 0 && data.touch_count == 1 && data.touches[0].x == 1440);
    write_file(pad_file, "");
    CHECK(read(1, &data) == 0 && data.buttons == 0 && data.touch_count == 0);

    keyboard[SDL_SCANCODE_X] = true;
    CHECK(read(1, &data) == 0 && data.buttons == PAD_CROSS);
    keyboard[SDL_SCANCODE_X] = false;
    keyboard[SDL_SCANCODE_SPACE] = true;
    CHECK(read(1, &data) == 0 && data.buttons == PAD_CROSS);
    keyboard[SDL_SCANCODE_SPACE] = false;

    SDL_VirtualJoystickTouchpadDesc touch = {.nfingers = 2};
    SDL_VirtualJoystickDesc desc;
    SDL_INIT_INTERFACE(&desc);
    desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
    desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    desc.button_mask = (1u << SDL_GAMEPAD_BUTTON_COUNT) - 1;
    desc.axis_mask = (1u << SDL_GAMEPAD_AXIS_COUNT) - 1;
    desc.name = "lantern test controller";
    desc.vendor_id = 0x1d50;
    desc.product_id = 0x6189;
    desc.ntouchpads = 1;
    desc.touchpads = &touch;
    SDL_JoystickID id = SDL_AttachVirtualJoystick(&desc);
    CHECK(id != 0);
    SDL_Joystick *joystick = SDL_OpenJoystick(id);
    CHECK(joystick != NULL);
    if (failures) return 1;

    CHECK(SDL_SetJoystickVirtualTouchpad(joystick, 0, 0, true, 0.75f, 0.5f, 1.0f));
    CHECK(SDL_SetJoystickVirtualTouchpad(joystick, 0, 1, true, 0.25f, 1.0f, 1.0f));
    CHECK(SDL_SetJoystickVirtualButton(joystick, SDL_GAMEPAD_BUTTON_TOUCHPAD, true));
    update();
    CHECK(read(1, &data) == 0 && data.touch_count == 2 && (data.buttons & PAD_TOUCHPAD));
    CHECK(data.touches[0].x == 1439 && data.touches[0].y == 471 && data.touches[0].id == 2);
    CHECK(data.touches[1].x == 480 && data.touches[1].y == 942 && data.touches[1].id == 3);
    capture = 1;
    CHECK(read(1, &data) == 0 && data.touch_count == 0 && data.buttons == 0);
    capture = 0;
    CHECK(SDL_SetJoystickVirtualTouchpad(joystick, 0, 0, false, 0, 0, 0));
    CHECK(SDL_SetJoystickVirtualTouchpad(joystick, 0, 1, false, 0, 0, 0));
    update();
    CHECK(read(1, &data) == 0 && data.touch_count == 1 && data.touches[0].x == 480);
    CHECK(SDL_SetJoystickVirtualButton(joystick, SDL_GAMEPAD_BUTTON_TOUCHPAD, false));
    update();
    CHECK(read(1, &data) == 0 && data.buttons == 0);

    CHECK(read(1, &data) == 0 && actions == 0);
    CHECK(SDL_SetJoystickVirtualButton(joystick, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, true));
    update();
    CHECK(read(1, &data) == 0 && actions == 4 && (data.buttons & (PAD_L1 | PAD_CIRCLE)) == PAD_CIRCLE);
    CHECK(SDL_SetJoystickVirtualButton(joystick, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, false));
    CHECK(SDL_SetJoystickVirtualButton(joystick, SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1, true));
    update();
    CHECK(read(1, &data) == 0 && actions == 1 && data.buttons == PAD_CIRCLE);
    CHECK(SDL_SetJoystickVirtualButton(joystick, SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1, false));
    update();
    CHECK(read(1, &data) == 0 && actions == 0 && data.buttons == 0);
    keyboard[SDL_SCANCODE_N] = true;
    CHECK(read(1, &data) == 0 && actions == 0 && data.buttons == 0);
    keyboard[SDL_SCANCODE_N] = false;

    CHECK(SDL_SetJoystickVirtualButton(joystick, SDL_GAMEPAD_BUTTON_EAST, true));
    CHECK(SDL_SetJoystickVirtualButton(joystick, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, true));
    CHECK(SDL_SetJoystickVirtualAxis(joystick, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 32767));
    update();
    CHECK(read(1, &data) == 0 && data.buttons == (PAD_CROSS | PAD_R2 | PAD_R1 | PAD_CIRCLE) && data.r2 == 255 && actions == 2);
    menu = 1;
    CHECK(read(1, &data) == 0 && (data.buttons & (PAD_CROSS | PAD_CIRCLE)) == PAD_CIRCLE && actions == 0);
    menu = 0;
    CHECK(read(1, &data) == 0 && !(data.buttons & (PAD_CROSS | PAD_CIRCLE)) && actions == 0);
    CHECK(sys_pad_button(PAD_CROSS, 0) == SDL_GAMEPAD_BUTTON_EAST);

    SDL_CloseJoystick(joystick);
    SDL_DetachVirtualJoystick(id);
    SDL_Quit();
    remove(pad_file);
    remove(config);
    if (failures) return 1;
    puts("PASS: pad ABI, left/right clicks, SDL touch coordinates, overlay capture, controls, menu buttons, dodge/backstep/sprint buttons");
    return 0;
}
