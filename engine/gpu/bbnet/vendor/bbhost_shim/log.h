// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
void bbhost_np_log(const char* format, ...) __attribute__((format(printf, 1, 2)));
#define host_log bbhost_np_log
