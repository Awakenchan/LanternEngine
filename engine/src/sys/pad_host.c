// SPDX-License-Identifier: GPL-3.0-or-later
#include "sys.h"
#include "pad.h"
#include "gpu/bbgpu.h"
#include "gpu/shim/input/input.h"
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    IN_CROSS, IN_CIRCLE, IN_SQUARE, IN_TRIANGLE, IN_L1, IN_R1, IN_L2, IN_R2, IN_L3, IN_R3,
    IN_OPTIONS, IN_TOUCHPAD, IN_TOUCHPAD_RIGHT, IN_UP, IN_DOWN, IN_LEFT, IN_RIGHT,
    IN_MOVE_UP, IN_MOVE_DOWN, IN_MOVE_LEFT, IN_MOVE_RIGHT, IN_LOOK_UP, IN_LOOK_DOWN, IN_LOOK_LEFT, IN_LOOK_RIGHT,
    IN_MENU_CONFIRM, IN_MENU_BACK, IN_COUNT
};

static const char *const input_names[IN_COUNT] = {
    "cross", "circle", "square", "triangle", "l1", "r1", "l2", "r2", "l3", "r3", "options", "touchpad",
    "touchpad_right", "up", "down", "left", "right", "move_up", "move_down", "move_left", "move_right",
    "look_up", "look_down", "look_left", "look_right", "menu_confirm", "menu_back",
};

static const uint32_t input_buttons[IN_COUNT] = {
    PAD_CROSS, PAD_CIRCLE, PAD_SQUARE, PAD_TRIANGLE, PAD_L1, PAD_R1, PAD_L2, PAD_R2, PAD_L3, PAD_R3,
    PAD_OPTIONS, PAD_TOUCHPAD, 0, PAD_UP, PAD_DOWN, PAD_LEFT, PAD_RIGHT,
};

#define MAX_PER_INPUT 4
#define SPARE_PADS 4
#define PRESSED 30
enum {
    LEFT_TRIGGER = SDL_GAMEPAD_BUTTON_COUNT, RIGHT_TRIGGER,
    LEFT_PAD_UP, LEFT_PAD_DOWN, LEFT_PAD_LEFT, LEFT_PAD_RIGHT, LEFT_PAD_TOUCH,
    RIGHT_PAD_UP, RIGHT_PAD_DOWN, RIGHT_PAD_LEFT, RIGHT_PAD_RIGHT, RIGHT_PAD_TOUCH
};
enum { TRACK_AUTO, TRACK_OFF, TRACK_TOUCH, TRACK_CAMERA, TRACK_MOVE };

typedef struct {
    int down;
    float x, y, vx, vy;
    uint64_t moved;
} TrackPad;

typedef struct {
    int key_count, pad_count;
    SDL_Scancode keys[MAX_PER_INPUT];
    int pad[MAX_PER_INPUT];
} Binding;

static Binding bindings[IN_COUNT];
static int bindings_loaded;
static int swap_sticks, invert_look_x, invert_look_y, deadzone_percent;
static int track_mode[2], track_sensitivity = 100, gyro_camera, gyro_sensitivity = 100;
static TrackPad track[2];
static int track_pads;

static SDL_Gamepad *gamepad;
static SDL_Gamepad *spares[SPARE_PADS];
static SDL_JoystickID spare_ids[SPARE_PADS];
static int sdl_state;
static uint8_t connections;
static int gamepad_type;

static uint8_t stick_byte(int16_t value) {
    int v = (value + 32768) >> 8;
    return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
}

static uint8_t trigger_byte(int16_t value) {
    int v = value >> 7;
    return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
}

static uint16_t touch_coordinate(float value, int max) {
    return (uint16_t)(value <= 0.0f ? 0 : value >= 1.0f ? max : (int)(value * max + 0.5f));
}

static const char *chosen_gamepad(void) {
    static const char *name;
    static int read;
    if (!read) {
        name = getenv("BB_GAMEPAD");
        if (name && !*name) name = NULL;
        read = 1;
    }
    return name;
}

static int is_chosen(SDL_JoystickID id, const char *choice) {
    char guid[33];
    SDL_GUIDToString(SDL_GetGamepadGUIDForID(id), guid, sizeof(guid));
    const char *name = SDL_GetGamepadNameForID(id);
    return !SDL_strcasecmp(guid, choice) || (name && SDL_strcasestr(name, choice));
}

static int in_use(SDL_Gamepad *pad) {
    for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT; ++b)
        if (SDL_GetGamepadButton(pad, (SDL_GamepadButton)b)) return 1;
    for (int a = 0; a < SDL_GAMEPAD_AXIS_COUNT; ++a) {
        int v = SDL_GetGamepadAxis(pad, (SDL_GamepadAxis)a);
        if (v > 20000 || v < -20000) return 1;
    }
    return 0;
}

static void switch_to_active_spare(void) {
    static uint64_t last_look;
    uint64_t now = pad_now_us();
    if (now - last_look < 200000) return;
    last_look = now;
    for (int s = 0; s < SPARE_PADS; ++s)
        if (spares[s] && !SDL_GamepadConnected(spares[s])) {
            SDL_CloseGamepad(spares[s]);
            spares[s] = NULL;
        }
    if (gamepad && in_use(gamepad)) return;
    int count = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&count);
    if (!ids) return;
    for (int i = 0; count > 1 && i < count; ++i) {
        if (gamepad && SDL_GetGamepadID(gamepad) == ids[i]) continue;
        int slot = -1, empty = -1;
        for (int s = 0; s < SPARE_PADS; ++s) {
            if (spares[s] && spare_ids[s] == ids[i]) slot = s;
            else if (!spares[s] && empty < 0) empty = s;
        }
        if (slot < 0 && empty >= 0 && (spares[empty] = SDL_OpenGamepad(ids[i]))) {
            spare_ids[empty] = ids[i];
            slot = empty;
        }
        if (slot >= 0 && in_use(spares[slot])) {
            SDL_Gamepad *previous = gamepad;
            gamepad = spares[slot];
            spares[slot] = previous;
            spare_ids[slot] = previous ? SDL_GetGamepadID(previous) : 0;
            printf("Runtime: gamepad in use: %s\n", SDL_GetGamepadName(gamepad));
            break;
        }
    }
    SDL_free(ids);
}

static SDL_Gamepad *current_gamepad(void) {
    static int on_choice;
    static uint64_t last_scan;
    static SDL_JoystickID typed_id;
    if (!sdl_state) {
        const char *steam = getenv("BB_STEAM_HIDAPI");
        if (!steam || strcmp(steam, "0")) {
            SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_STEAM, "1");
            SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_STEAMDECK, "1");
            SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_STEAM_HORI, "1");
        }
        sdl_state = SDL_WasInit(SDL_INIT_GAMEPAD) || SDL_InitSubSystem(SDL_INIT_GAMEPAD) ? 1 : -1;
    }
    if (sdl_state < 0) return NULL;
    if (gamepad && !SDL_GamepadConnected(gamepad)) {
        SDL_CloseGamepad(gamepad);
        gamepad = NULL;
    }
    const char *choice = chosen_gamepad();
    uint64_t now = pad_now_us();
    if (!gamepad || (choice && !on_choice && now - last_scan > 1000000)) {
        last_scan = now;
        int count = 0, pick = -1;
        SDL_JoystickID *ids = SDL_GetGamepads(&count);
        for (int i = 0; choice && ids && i < count && pick < 0; ++i)
            if (is_chosen(ids[i], choice)) pick = i;
        if (pick < 0 && !gamepad && ids && count > 0) {
            const char *index = getenv("BB_GAMEPAD_INDEX");
            pick = index && *index ? atoi(index) : 0;
            if (pick < 0 || pick >= count) pick = 0;
        }
        if (pick >= 0 && (!gamepad || SDL_GetGamepadID(gamepad) != ids[pick])) {
            if (gamepad) SDL_CloseGamepad(gamepad);
            gamepad = SDL_OpenGamepad(ids[pick]);
            on_choice = choice && gamepad && is_chosen(ids[pick], choice);
            if (gamepad) {
                ++connections;
                printf("Runtime: gamepad connected: %s%s\n", SDL_GetGamepadName(gamepad),
                       !choice ? "" : on_choice ? " (the chosen one)" : " (the chosen one is not connected)");
                printf("Runtime: gamepad has %d touchpad(s), gyro %s, paddles %s\n", SDL_GetNumGamepadTouchpads(gamepad),
                       SDL_GamepadHasSensor(gamepad, SDL_SENSOR_GYRO) ? "yes" : "no",
                       SDL_GamepadHasButton(gamepad, SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1) ? "yes" : "no");
            }
        }
        SDL_free(ids);
    }
    if (!choice) switch_to_active_spare();
    SDL_JoystickID id = gamepad ? SDL_GetGamepadID(gamepad) : 0;
    if (id != typed_id) {
        typed_id = id;
        __atomic_store_n(&gamepad_type, gamepad ? (int)SDL_GetGamepadType(gamepad) : (int)SDL_GAMEPAD_TYPE_UNKNOWN,
                         __ATOMIC_RELAXED);
    }
    return gamepad;
}

static void default_bindings(void) {
    static const struct { int input; SDL_Scancode key; } keys[] = {
        {IN_CROSS, SDL_SCANCODE_SPACE}, {IN_CIRCLE, SDL_SCANCODE_LSHIFT}, {IN_SQUARE, SDL_SCANCODE_E},
        {IN_TRIANGLE, SDL_SCANCODE_Q}, {IN_L1, SDL_SCANCODE_1}, {IN_R1, SDL_SCANCODE_3},
        {IN_L2, SDL_SCANCODE_R}, {IN_R2, SDL_SCANCODE_F}, {IN_L3, SDL_SCANCODE_Z}, {IN_R3, SDL_SCANCODE_C},
        {IN_OPTIONS, SDL_SCANCODE_RETURN}, {IN_TOUCHPAD, SDL_SCANCODE_TAB}, {IN_TOUCHPAD_RIGHT, SDL_SCANCODE_BACKSPACE},
        {IN_UP, SDL_SCANCODE_I}, {IN_DOWN, SDL_SCANCODE_K}, {IN_LEFT, SDL_SCANCODE_J}, {IN_RIGHT, SDL_SCANCODE_L},
        {IN_MOVE_UP, SDL_SCANCODE_W}, {IN_MOVE_DOWN, SDL_SCANCODE_S}, {IN_MOVE_LEFT, SDL_SCANCODE_A},
        {IN_MOVE_RIGHT, SDL_SCANCODE_D}, {IN_LOOK_UP, SDL_SCANCODE_UP}, {IN_LOOK_DOWN, SDL_SCANCODE_DOWN},
        {IN_LOOK_LEFT, SDL_SCANCODE_LEFT}, {IN_LOOK_RIGHT, SDL_SCANCODE_RIGHT},
    };
    static const struct { int input, button; } buttons[] = {
        {IN_CROSS, SDL_GAMEPAD_BUTTON_SOUTH}, {IN_CIRCLE, SDL_GAMEPAD_BUTTON_EAST},
        {IN_SQUARE, SDL_GAMEPAD_BUTTON_WEST}, {IN_TRIANGLE, SDL_GAMEPAD_BUTTON_NORTH},
        {IN_L1, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER}, {IN_R1, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER},
        {IN_L2, LEFT_TRIGGER}, {IN_R2, RIGHT_TRIGGER},
        {IN_L3, SDL_GAMEPAD_BUTTON_LEFT_STICK}, {IN_R3, SDL_GAMEPAD_BUTTON_RIGHT_STICK},
        {IN_OPTIONS, SDL_GAMEPAD_BUTTON_START},
        {IN_TOUCHPAD, SDL_GAMEPAD_BUTTON_BACK}, {IN_TOUCHPAD, SDL_GAMEPAD_BUTTON_TOUCHPAD},
        {IN_TOUCHPAD_RIGHT, SDL_GAMEPAD_BUTTON_MISC2},
        {IN_UP, SDL_GAMEPAD_BUTTON_DPAD_UP}, {IN_DOWN, SDL_GAMEPAD_BUTTON_DPAD_DOWN},
        {IN_LEFT, SDL_GAMEPAD_BUTTON_DPAD_LEFT}, {IN_RIGHT, SDL_GAMEPAD_BUTTON_DPAD_RIGHT},
    };
    memset(bindings, 0, sizeof(bindings));
    for (size_t i = 0; i < sizeof(keys) / sizeof(*keys); ++i) {
        Binding *b = &bindings[keys[i].input];
        b->keys[b->key_count++] = keys[i].key;
    }
    for (size_t i = 0; i < sizeof(buttons) / sizeof(*buttons); ++i) {
        Binding *b = &bindings[buttons[i].input];
        b->pad[b->pad_count++] = buttons[i].button;
    }
}

static int gamepad_button_named(const char *name) {
    if (!SDL_strcasecmp(name, "lefttrigger")) return LEFT_TRIGGER;
    if (!SDL_strcasecmp(name, "righttrigger")) return RIGHT_TRIGGER;
    static const char *const pad_names[] = {"leftpad_up",  "leftpad_down",  "leftpad_left",  "leftpad_right",  "leftpad_touch",
                                            "rightpad_up", "rightpad_down", "rightpad_left", "rightpad_right", "rightpad_touch"};
    for (int i = 0; i < 10; ++i)
        if (!SDL_strcasecmp(name, pad_names[i])) return LEFT_PAD_UP + i;
    SDL_GamepadButton button = SDL_GetGamepadButtonFromString(name);
    return button == SDL_GAMEPAD_BUTTON_INVALID ? -1 : (int)button;
}

static int track_mode_named(const char *value) {
    static const char *const names[] = {"auto", "off", "touch", "camera", "move"};
    for (int i = 0; i < 5; ++i)
        if (!strncmp(value, names[i], strlen(names[i]))) return i;
    return TRACK_AUTO;
}

static int read_option(const char *line, const char *value) {
    int v = atoi(value);
    if (!strncmp(line, "pad_trackpad_left=", 18)) track_mode[0] = track_mode_named(value);
    else if (!strncmp(line, "pad_trackpad_right=", 19)) track_mode[1] = track_mode_named(value);
    else if (!strncmp(line, "pad_trackpad_sensitivity=", 25)) track_sensitivity = v < 10 ? 10 : v > 400 ? 400 : v;
    else if (!strncmp(line, "pad_gyro_camera=", 16)) gyro_camera = v != 0;
    else if (!strncmp(line, "pad_gyro_sensitivity=", 21)) gyro_sensitivity = v < 10 ? 10 : v > 400 ? 400 : v;
    if (!strncmp(line, "pad_swap_sticks=", 16)) swap_sticks = v != 0;
    else if (!strncmp(line, "pad_invert_look_x=", 18)) invert_look_x = v != 0;
    else if (!strncmp(line, "pad_invert_look_y=", 18)) invert_look_y = v != 0;
    else if (!strncmp(line, "pad_deadzone=", 13)) deadzone_percent = v < 0 ? 0 : v > 50 ? 50 : v;
    return 1;
}

static void bind_names(Binding *b, int keyboard, char *list, const char *input) {
    if (keyboard) b->key_count = 0;
    else b->pad_count = 0;
    for (char *name = strtok(list, ",\r\n"); name; name = strtok(NULL, ",\r\n")) {
        while (*name == ' ') ++name;
        for (char *end = name + strlen(name); end > name && end[-1] == ' ';) *--end = 0;
        if (!*name) continue;
        if (keyboard) {
            SDL_Scancode key = SDL_GetScancodeFromName(name);
            if (key == SDL_SCANCODE_UNKNOWN) printf("Runtime: controls: unknown key \"%s\" for %s\n", name, input);
            else if (b->key_count < MAX_PER_INPUT) b->keys[b->key_count++] = key;
        } else {
            int button = gamepad_button_named(name);
            if (button < 0) printf("Runtime: controls: unknown gamepad button \"%s\" for %s\n", name, input);
            else if (b->pad_count < MAX_PER_INPUT) b->pad[b->pad_count++] = button;
        }
    }
}

static void load_bindings(void) {
    default_bindings();
    const char *path = getenv("BB_CONFIG");
    FILE *file = path ? fopen(path, "r") : NULL;
    if (!file) return;
    char line[512];
    while (fgets(line, sizeof(line), file)) {
        int keyboard = !strncmp(line, "key.", 4), pad = !strncmp(line, "pad.", 4);
        char *equals = strchr(line, '=');
        if (!strncmp(line, "pad_", 4) && equals && read_option(line, equals + 1)) continue;
        if ((!keyboard && !pad) || !equals) continue;
        *equals = 0;
        int input = -1;
        for (int i = 0; i < IN_COUNT; ++i)
            if (!strcmp(line + 4, input_names[i])) input = i;
        int pad_only_stick = pad && input >= IN_MOVE_UP && input < IN_MENU_CONFIRM;
        int keyboard_menu = keyboard && input >= IN_MENU_CONFIRM;
        if (input < 0 || pad_only_stick || keyboard_menu) {
            printf("Runtime: controls: unknown input %s\n", line);
            continue;
        }
        bind_names(&bindings[input], keyboard, equals + 1, line + 4);
    }
    fclose(file);
}

static void ensure_bindings(void) {
    if (!bindings_loaded) {
        load_bindings();
        bindings_loaded = 1;
    }
}

static int key_held(const bool *keys, int input) {
    for (int i = 0; i < bindings[input].key_count; ++i)
        if (keys[bindings[input].keys[i]]) return 1;
    return 0;
}

static int menu_owns(int button) {
    for (int m = IN_MENU_CONFIRM; m <= IN_MENU_BACK; ++m)
        for (int i = 0; i < bindings[m].pad_count; ++i)
            if (bindings[m].pad[i] == button) return 1;
    return 0;
}

static int track_button(int button) {
    const TrackPad *t = &track[button >= RIGHT_PAD_UP];
    if (track_pads < 2 || !t->down) return 0;
    int which = (button - LEFT_PAD_UP) % 5;
    float dx = t->x - 0.5f, dy = t->y - 0.5f, ax = SDL_fabsf(dx), ay = SDL_fabsf(dy);
    int on = which == 4   ? 1
             : which == 0 ? dy < -0.18f && ay >= ax
             : which == 1 ? dy > 0.18f && ay >= ax
             : which == 2 ? dx < -0.18f && ax > ay
                          : dx > 0.18f && ax > ay;
    return on ? 255 : 0;
}

static void update_trackpads(SDL_Gamepad *g) {
    track_pads = SDL_GetNumGamepadTouchpads(g);
    uint64_t now = pad_now_us();
    for (int p = 0; p < 2; ++p) {
        TrackPad *t = &track[p];
        bool down = false;
        float x = 0, y = 0;
        if (track_pads < 2 || !SDL_GetGamepadTouchpadFinger(g, p, 0, &down, &x, &y, NULL) || !down) {
            t->down = 0;
            t->vx = t->vy = 0;
            continue;
        }
        if (!t->down) {
            t->vx = t->vy = 0;
            t->moved = now;
        } else if (x != t->x || y != t->y) {
            float dt = (float)(now - t->moved) / 1000000.0f;
            if (dt < 0.002f) dt = 0.002f;
            t->vx = 0.5f * t->vx + 0.5f * (x - t->x) / dt;
            t->vy = 0.5f * t->vy + 0.5f * (y - t->y) / dt;
            t->moved = now;
        } else if (now - t->moved > 40000) {
            t->vx = t->vy = 0;
        }
        t->down = 1;
        t->x = x;
        t->y = y;
    }
}

static int track_mode_of(int p) {
    return track_mode[p] != TRACK_AUTO ? track_mode[p] : p ? TRACK_CAMERA : TRACK_TOUCH;
}

static uint8_t offset_byte(uint8_t value, float offset) {
    float v = value + offset * 127.0f;
    return (uint8_t)(v < 0.0f ? 0 : v > 255.0f ? 255 : (int)(v + 0.5f));
}

static void apply_trackpads(PadData *pad) {
    for (int p = 0; p < 2 && track_pads >= 2; ++p) {
        const TrackPad *t = &track[p];
        int mode = track_mode_of(p);
        if (!t->down) continue;
        if (mode == TRACK_CAMERA) {
            float k = 0.45f * track_sensitivity / 100.0f;
            pad->right_x = offset_byte(pad->right_x, (invert_look_x ? -t->vx : t->vx) * k);
            pad->right_y = offset_byte(pad->right_y, (invert_look_y ? -t->vy : t->vy) * k);
        } else if (mode == TRACK_MOVE) {
            float x = (t->x - 0.5f) * 2.4f, y = (t->y - 0.5f) * 2.4f, length = SDL_sqrtf(x * x + y * y);
            if (length < 0.15f) continue;
            if (length > 1.0f) {
                x /= length;
                y /= length;
            }
            pad->left_x = offset_byte(128, x);
            pad->left_y = offset_byte(128, y);
        } else if (mode == TRACK_TOUCH && pad->touch_count < 2) {
            pad->touches[pad->touch_count++] =
                (PadTouch){.x = touch_coordinate((p ? 0.5f : 0.0f) + t->x * 0.5f, PAD_TOUCH_WIDTH - 1),
                           .y = touch_coordinate(t->y, PAD_TOUCH_HEIGHT - 1),
                           .id = (uint8_t)p};
        }
    }
}

static void apply_gyro(SDL_Gamepad *g, PadData *pad) {
    static SDL_JoystickID enabled_on;
    if (!gyro_camera || !SDL_GamepadHasSensor(g, SDL_SENSOR_GYRO)) return;
    SDL_JoystickID id = SDL_GetGamepadID(g);
    if (id != enabled_on) {
        SDL_SetGamepadSensorEnabled(g, SDL_SENSOR_GYRO, true);
        enabled_on = id;
    }
    float rate[3] = {0};
    if (!SDL_GetGamepadSensorData(g, SDL_SENSOR_GYRO, rate, 3)) return;
    float k = 0.6f * gyro_sensitivity / 100.0f, yaw = -rate[1], pitch = -rate[0];
    if (SDL_fabsf(yaw) < 0.03f) yaw = 0;
    if (SDL_fabsf(pitch) < 0.03f) pitch = 0;
    pad->right_x = offset_byte(pad->right_x, (invert_look_x ? -yaw : yaw) * k);
    pad->right_y = offset_byte(pad->right_y, (invert_look_y ? -pitch : pitch) * k);
}

static int gamepad_value(SDL_Gamepad *g, int input, int skip_menu_buttons) {
    int value = 0;
    for (int i = 0; i < bindings[input].pad_count; ++i) {
        int b = bindings[input].pad[i];
        if (skip_menu_buttons && menu_owns(b)) continue;
        int v = b >= LEFT_PAD_UP     ? track_button(b)
                : b == LEFT_TRIGGER  ? trigger_byte(SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFT_TRIGGER))
                : b == RIGHT_TRIGGER ? trigger_byte(SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER))
                : SDL_GetGamepadButton(g, (SDL_GamepadButton)b) ? 255
                                                                  : 0;
        if (v > value) value = v;
    }
    return value;
}

static uint8_t keys_to_axis(uint8_t value, int negative, int positive) {
    if (!negative && !positive) return value;
    return (uint8_t)(128 - (negative ? 128 : 0) + (positive ? 127 : 0));
}

static void apply_keyboard(PadData *pad, const bool *keys) {
    for (int i = IN_CROSS; i <= IN_RIGHT; ++i)
        if (i != IN_TOUCHPAD && i != IN_TOUCHPAD_RIGHT && key_held(keys, i)) pad->buttons |= input_buttons[i];
    if (key_held(keys, IN_TOUCHPAD)) pad_touch_click(pad, 0);
    if (key_held(keys, IN_TOUCHPAD_RIGHT)) pad_touch_click(pad, 1);
    if (key_held(keys, IN_L2)) pad->l2 = 255;
    if (key_held(keys, IN_R2)) pad->r2 = 255;
    pad->left_x = keys_to_axis(pad->left_x, key_held(keys, IN_MOVE_LEFT), key_held(keys, IN_MOVE_RIGHT));
    pad->left_y = keys_to_axis(pad->left_y, key_held(keys, IN_MOVE_UP), key_held(keys, IN_MOVE_DOWN));
    pad->right_x = keys_to_axis(pad->right_x, key_held(keys, IN_LOOK_LEFT), key_held(keys, IN_LOOK_RIGHT));
    pad->right_y = keys_to_axis(pad->right_y, key_held(keys, IN_LOOK_UP), key_held(keys, IN_LOOK_DOWN));
}

static void apply_deadzone(uint8_t *x, uint8_t *y) {
    float zone = deadzone_percent * 1.28f, dx = *x - 128.0f, dy = *y - 128.0f, length = SDL_sqrtf(dx * dx + dy * dy);
    if (length <= zone) {
        *x = *y = 128;
        return;
    }
    float scale = (length - zone) * 128.0f / (128.0f - zone) / length;
    float nx = 128.0f + dx * scale, ny = 128.0f + dy * scale;
    *x = (uint8_t)(nx < 0.0f ? 0 : nx > 255.0f ? 255 : (int)(nx + 0.5f));
    *y = (uint8_t)(ny < 0.0f ? 0 : ny > 255.0f ? 255 : (int)(ny + 0.5f));
}

static void read_sticks(SDL_Gamepad *g, PadData *pad) {
    pad->left_x = stick_byte(SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTX));
    pad->left_y = stick_byte(SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTY));
    pad->right_x = stick_byte(SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_RIGHTX));
    pad->right_y = stick_byte(SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_RIGHTY));
    if (deadzone_percent) {
        apply_deadzone(&pad->left_x, &pad->left_y);
        apply_deadzone(&pad->right_x, &pad->right_y);
    }
    if (swap_sticks) {
        uint8_t t = pad->left_x;
        pad->left_x = pad->right_x;
        pad->right_x = t;
        t = pad->left_y;
        pad->left_y = pad->right_y;
        pad->right_y = t;
    }
    if (invert_look_x) pad->right_x = (uint8_t)(255 - pad->right_x);
    if (invert_look_y) pad->right_y = (uint8_t)(255 - pad->right_y);
}

static void read_touchpad(SDL_Gamepad *g, PadData *pad) {
    if (track_pads != 1) return;
    int fingers = SDL_GetNumGamepadTouchpadFingers(g, 0);
    for (int finger = 0; finger < fingers && pad->touch_count < 2; ++finger) {
        bool down = false;
        float x = 0, y = 0;
        if (SDL_GetGamepadTouchpadFinger(g, 0, finger, &down, &x, &y, NULL) && down)
            pad->touches[pad->touch_count++] = (PadTouch){.x = touch_coordinate(x, PAD_TOUCH_WIDTH - 1),
                                                         .y = touch_coordinate(y, PAD_TOUCH_HEIGHT - 1),
                                                         .id = (uint8_t)finger};
    }
}

static void read_gamepad(SDL_Gamepad *g, PadData *pad) {
    static int menu_buttons_held;
    uint32_t menu_confirm = 0, menu_back = 0;
    int in_menu = 0, menu_down = 0, touch_right = 0;
    update_trackpads(g);
    if (bindings[IN_MENU_CONFIRM].pad_count || bindings[IN_MENU_BACK].pad_count) {
        in_menu = bbgpu_menu_buttons(&menu_confirm, &menu_back);
        menu_down = (gamepad_value(g, IN_MENU_CONFIRM, 0) > PRESSED ? 1 : 0) |
                    (gamepad_value(g, IN_MENU_BACK, 0) > PRESSED ? 2 : 0);
        menu_buttons_held = in_menu ? menu_down : menu_buttons_held & menu_down;
    }
    for (int i = IN_CROSS; i <= IN_RIGHT; ++i) {
        int v = gamepad_value(g, i, in_menu || menu_buttons_held);
        if (i == IN_L2) pad->l2 = (uint8_t)v;
        if (i == IN_R2) pad->r2 = (uint8_t)v;
        if (i == IN_TOUCHPAD_RIGHT) touch_right = v > PRESSED;
        else if (v > PRESSED) pad->buttons |= input_buttons[i];
    }
    if (in_menu) {
        if (menu_down & 1) pad->buttons |= menu_confirm;
        if (menu_down & 2) pad->buttons |= menu_back;
    }
    read_sticks(g, pad);
    read_touchpad(g, pad);
    apply_trackpads(pad);
    apply_gyro(g, pad);
    if ((pad->buttons & PAD_TOUCHPAD) && !pad->touch_count) pad_touch_click(pad, 0);
    if (touch_right) pad_touch_click(pad, 1);
}

static void pc_layer(PadData *pad, const bool *keys, int gamepad_present) {
    int key_count = 0;
    if (keys) SDL_GetKeyboardState(&key_count);
    BbPcPad pc = {.buttons = pad->buttons, .lx = pad->left_x, .ly = pad->left_y, .rx = pad->right_x,
                  .ry = pad->right_y, .l2 = pad->l2, .r2 = pad->r2, .touch_count = pad->touch_count};
    for (int i = 0; i < pad->touch_count && i < 2; ++i) {
        pc.touch[i].x = pad->touches[i].x;
        pc.touch[i].y = pad->touches[i].y;
        pc.touch[i].id = pad->touches[i].id;
    }
    if (bbgpu_pc_input(&pc, keys, key_count, gamepad_present)) {
        pad->buttons = pc.buttons;
        pad->left_x = pc.lx;
        pad->left_y = pc.ly;
        pad->right_x = pc.rx;
        pad->right_y = pc.ry;
        pad->l2 = pc.l2;
        pad->r2 = pc.r2;
        pad->touch_count = pc.touch_count;
        for (int i = 0; i < pc.touch_count && i < 2; ++i)
            pad->touches[i] = (PadTouch){.x = pc.touch[i].x, .y = pc.touch[i].y, .id = pc.touch[i].id};
    } else if (keys) {
        apply_keyboard(pad, keys);
    }
}

typedef struct {
    uint16_t size, version;
    uint32_t reserved;
    uint64_t most_from_system, from_system, most_in_use, in_use;
} GuestHeapStats;

static void report_guest_heap(void) {
    static int enabled = -1;
    static uint64_t last;
    if (enabled < 0) enabled = getenv("BB_FRAME_STATS") != NULL;
    uint64_t now = pad_now_us();
    if (!enabled || now - last < 5000000) return;
    last = now;
    const uint8_t *stub = (const uint8_t *)(UINT64_C(0x800000000) + 0x56E0000 + 0x1F8C0);
    if (stub[0] != 0xFF || stub[1] != 0x25) return;
    SYS_ABI int (*malloc_stats)(GuestHeapStats *) = (SYS_ABI int (*)(GuestHeapStats *))(uintptr_t)stub;
    GuestHeapStats m = {.size = sizeof(m), .version = 1};
    int result = malloc_stats(&m);
    if (result) {
        static int told;
        if (!told++) printf("Guest heap: malloc_stats returned %#x\n", (unsigned)result);
        return;
    }
    printf("Guest heap: %.1f MB in use (most %.1f), %.1f MB from the system (most %.1f)\n", m.in_use / 1048576.0,
           m.most_in_use / 1048576.0, m.from_system / 1048576.0, m.most_from_system / 1048576.0);
}

void pad_host_sample(PadData *pad) {
    report_guest_heap();
    memset(pad, 0, sizeof(*pad));
    pad->left_x = pad->left_y = pad->right_x = pad->right_y = 128;
    pad->orientation[3] = 1.0f;
    pad->connected = 1;
    pad->connected_count = connections ? connections : 1;
    pad->timestamp = pad_now_us();
    SDL_Gamepad *g = current_gamepad();
    ensure_bindings();
    if (bbgpu_overlay_captures_input()) return;
    const bool *keys = SDL_WasInit(SDL_INIT_VIDEO) ? SDL_GetKeyboardState(NULL) : NULL;
    if (g) read_gamepad(g, pad);
    pc_layer(pad, keys, g != NULL);
}

void pad_host_refresh(void) {
    current_gamepad();
}

uint8_t pad_host_connected_count(void) {
    return connections;
}

int pad_host_sdl_state(void) {
    return sdl_state;
}

void pad_host_rumble(uint8_t large, uint8_t small) {
    SDL_Gamepad *g = current_gamepad();
    if (g) SDL_RumbleGamepad(g, (uint16_t)(large * 257), (uint16_t)(small * 257), 1000);
}

const char *pad_host_gamepad_name(void) {
    return gamepad ? SDL_GetGamepadName(gamepad) : "none";
}

int sys_pad_type(void) {
    return __atomic_load_n(&gamepad_type, __ATOMIC_RELAXED);
}

int sys_pad_button(uint32_t button, int right_side) {
    int input = -1, found = -1;
    if (button == PAD_TOUCHPAD) input = right_side ? IN_TOUCHPAD_RIGHT : IN_TOUCHPAD;
    else
        for (int i = IN_CROSS; i <= IN_RIGHT && input < 0; ++i)
            if (input_buttons[i] == button) input = i;
    if (input < 0) return -1;
    sys_pad_lock();
    ensure_bindings();
    const Binding *b = &bindings[input];
    for (int i = 0; i < b->pad_count && found < 0; ++i) {
        int p = b->pad[i];
        int present = p >= LEFT_PAD_UP        ? 0
                      : !gamepad              ? 1
                      : p == LEFT_TRIGGER     ? SDL_GamepadHasAxis(gamepad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER)
                      : p == RIGHT_TRIGGER    ? SDL_GamepadHasAxis(gamepad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER)
                                              : SDL_GamepadHasButton(gamepad, (SDL_GamepadButton)p);
        if (present) found = p;
    }
    for (int i = 0; i < b->pad_count && found < 0; ++i)
        if (b->pad[i] < LEFT_PAD_UP) found = b->pad[i];
    sys_pad_unlock();
    return found;
}
