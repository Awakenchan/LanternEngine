// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

struct PadState;

enum BindAction : int {

    kBindMoveF, kBindMoveB, kBindMoveL, kBindMoveR, kBindWalk, kBindRoll, kBindLockOn,

    kBindAttack, kBindStrong, kBindStrongMod, kBindTransform, kBindFirearm, kBindBloodVial, kBindUseItem,

    kBindInteract, kBindSwitchItem, kBindSwitchRight, kBindSwitchLeft, kBindDpadUp, kBindGestures, kBindEffects,

    kBindLookU, kBindLookD, kBindLookL, kBindLookR, kBindMenu, kBindConfirm, kBindBack,

    kBindL3, kBindDodge, kBindBackstep, kBindSprint, kBindDebugMenu,
    kBindCount,
};
constexpr int kBindPages = 5, kBindPerPage = 7;
static_assert((kBindPages - 1) * kBindPerPage < kBindCount && kBindCount <= kBindPages * kBindPerPage,
              "the pages hold every action and none is empty");

struct BindingInfo {
    const char* key;
    const char* label;
    const char* help;
    std::uint32_t button;

    const char* default_key;
    int default_mouse;
};
const BindingInfo& host_binding_info(int action);

void host_bindings_load_defaults();

bool host_bindings_parse(const char* name, const char* value);

std::string host_binding_ini_value(int action);

std::uint64_t host_bindings_serial();

int host_binding_key(int action);
int host_binding_mouse(int action);

void host_binding_describe(int action, char* out, std::size_t size);

void host_binding_clear(int action);

void host_bindings_keys_held(const bool* keys, int count, bool held[kBindCount]);
void host_bindings_mouse_held(std::uint32_t inputs, bool held[kBindCount]);

void host_bindings_apply(const bool held[kBindCount], bool strong, PadState& pad);
bool host_bindings_dodge(std::uint64_t now_ms, bool menu, PadState& pad);

void host_bind_capture_begin(int action);
int host_bind_capturing();

bool host_bind_capture_key(int scancode, unsigned sdl_mods = 0);
bool host_bind_capture_key_up(int scancode);
bool host_bind_capture_mouse(int input);

int host_bind_capture_take_done();
bool host_bind_capture_blocking();

void host_bind_capture_note_held(bool any_down);

void host_bind_note_clear_key();
bool host_bind_take_clear_key();
