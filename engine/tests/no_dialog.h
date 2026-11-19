// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LANTERN_TESTS_NO_DIALOG_H
#define LANTERN_TESTS_NO_DIALOG_H
#ifdef _WIN32
#include <stdlib.h>
#ifdef _DEBUG
#include <crtdbg.h>
#endif
#ifdef __cplusplus
extern "C" {
#endif
__declspec(dllimport) unsigned int __stdcall SetErrorMode(unsigned int mode);
#ifdef __cplusplus
}
#endif
__attribute__((constructor(101), used)) static void lantern_no_dialog(void) {
    SetErrorMode(0x0001u | 0x0002u | 0x8000u);
    _set_error_mode(_OUT_TO_STDERR);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#ifdef _DEBUG
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_WARN, _CRTDBG_FILE_STDERR);
#endif
}
#endif
#endif
