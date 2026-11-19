// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stddef.h>
#include <stdint.h>
#ifndef __cplusplus
#include <stdbool.h>
#endif

#ifdef __cplusplus
#include "pad.h"
union SDL_Event;
struct SDL_Window;

enum class InputDevice { KeyboardMouse, Pad };

InputDevice host_input_device();

void host_log(const char* format, ...) __attribute__((format(printf, 1, 2)));

namespace BbInput {

bool Install(uint8_t* image, size_t size);
bool Enabled();

void Event(const SDL_Event& event, bool blocked);

void Pump(SDL_Window* window, bool text_active);

void Tick();

bool MouseIsMenuPointer();
bool PointerOnStage(float& x, float& y);
bool TextEntryOpen();

PadState LastPad();
}

extern "C" {
#endif

typedef struct {
    uint32_t buttons;
    uint8_t lx, ly, rx, ry, l2, r2, touch_count;
    struct { uint16_t x, y; uint8_t id; } touch[2];
    uint8_t actions;
} BbPcPad;

int bbgpu_pc_input(BbPcPad* pad, const bool* keys, int nkeys, int gamepad_present);

int bbgpu_menu_buttons(uint32_t* confirm, uint32_t* back);

int sys_pad_type(void);
int sys_pad_button(uint32_t ps_button, int right_side);

#ifdef __cplusplus
}
#endif
