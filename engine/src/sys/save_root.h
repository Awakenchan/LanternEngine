// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LANTERN_SAVE_ROOT_H
#define LANTERN_SAVE_ROOT_H
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef _WIN32
#include <windows.h>
#endif

typedef enum { SAVE_ROOT_USABLE, SAVE_ROOT_RELATIVE, SAVE_ROOT_EMPTY, SAVE_ROOT_DRIVE, SAVE_ROOT_DEVICE } SaveRootKind;

enum { SAVE_ROOT_MUTANT_ANY_ROOT = 6 };

static inline unsigned save_root_code_page(void) {
#ifdef _WIN32
    return GetACP();
#else
    return 0;
#endif
}

static inline int save_root_separator(char c) {
#ifdef _WIN32
    return c == '/' || c == '\\';
#else
    return c == '/';
#endif
}

static inline const char *save_root_next(const char *at, unsigned code_page) {
#ifdef _WIN32
    if (code_page && IsDBCSLeadByteEx(code_page, (BYTE)*at) && at[1]) return at + 2;
#else
    (void)code_page;
#endif
    return at + 1;
}

static inline const char *save_root_part_end(const char *part, unsigned code_page) {
    while (*part && !save_root_separator(*part)) part = save_root_next(part, code_page);
    return part;
}

static inline int save_root_below(const char *rest, unsigned code_page) {
    size_t depth = 0;
    while (*rest) {
        while (save_root_separator(*rest)) ++rest;
        const char *end = save_root_part_end(rest, code_page);
        size_t length = (size_t)(end - rest);
        if (length == 2 && rest[0] == '.' && rest[1] == '.') depth -= depth > 0;
        else if (length && !(length == 1 && rest[0] == '.')) ++depth;
        rest = end;
    }
    return depth > 0;
}

static inline SaveRootKind save_root_kind_in(const char *root, unsigned code_page) {
    if (!root || !*root) return SAVE_ROOT_EMPTY;
#ifdef _WIN32
    if (save_root_separator(root[0]) && save_root_separator(root[1])) {
        if ((root[2] == '?' || root[2] == '.') && (!root[3] || save_root_separator(root[3]))) return SAVE_ROOT_DEVICE;
        const char *server = root + 2, *server_end = save_root_part_end(server, code_page);
        if (server_end == server || !*server_end) return SAVE_ROOT_DRIVE;
        const char *share = server_end + 1, *share_end = save_root_part_end(share, code_page);
        if (share_end == share) return SAVE_ROOT_DRIVE;
        return save_root_below(share_end, code_page) ? SAVE_ROOT_USABLE : SAVE_ROOT_DRIVE;
    }
    if (root[0] && root[1] == ':') {
        if (!save_root_separator(root[2])) return SAVE_ROOT_RELATIVE;
        return save_root_below(root + 2, code_page) ? SAVE_ROOT_USABLE : SAVE_ROOT_DRIVE;
    }
#endif
    if (!save_root_separator(root[0])) return SAVE_ROOT_RELATIVE;
    return save_root_below(root, code_page) ? SAVE_ROOT_USABLE : SAVE_ROOT_DRIVE;
}

static inline SaveRootKind save_root_kind(const char *root) {
    return save_root_kind_in(root, save_root_code_page());
}

static inline const char *save_root_problem(SaveRootKind kind) {
    return kind == SAVE_ROOT_DRIVE    ? "a drive or share root"
           : kind == SAVE_ROOT_DEVICE ? "a device path"
                                      : "empty or cannot be made a full path";
}

static inline int save_root_fits(int written, size_t size) {
    return written > 0 && (size_t)written < size;
}

#ifndef _WIN32
static inline int save_root_lexical(const char *path, char *out, size_t size) {
    if (size < 2 || path[0] != '/') return -1;
    size_t used = 0;
    for (const char *part = path; *part;) {
        while (*part == '/') ++part;
        const char *end = part;
        while (*end && *end != '/') ++end;
        size_t length = (size_t)(end - part);
        if (length == 2 && part[0] == '.' && part[1] == '.') {
            while (used && out[used - 1] != '/') --used;
            if (used) --used;
        } else if (length && !(length == 1 && part[0] == '.')) {
            if (used + 1 + length >= size) return -1;
            out[used++] = '/';
            memcpy(out + used, part, length);
            used += length;
        }
        part = end;
    }
    if (!used) out[used++] = '/';
    out[used] = 0;
    return 0;
}
#endif

static inline int save_root_absolute(const char *root, char *out, size_t size) {
#ifdef _WIN32
    return _fullpath(out, root, size) ? 0 : -1;
#else
    char *resolved = realpath(root, NULL), joined[PATH_MAX * 2], folder[PATH_MAX];
    int written = resolved ? snprintf(out, size, "%s", resolved) : 0;
    free(resolved);
    if (written) return save_root_fits(written, size) ? 0 : -1;
    written = root[0] == '/'                  ? snprintf(joined, sizeof(joined), "%s", root)
              : getcwd(folder, sizeof(folder)) ? snprintf(joined, sizeof(joined), "%s/%s", folder, root)
                                               : -1;
    return save_root_fits(written, sizeof(joined)) ? save_root_lexical(joined, out, size) : -1;
#endif
}

static inline SaveRootKind save_root_resolve(const char *root, char *out, size_t size) {
    SaveRootKind kind = save_root_kind(root);
    if (kind == SAVE_ROOT_EMPTY || kind == SAVE_ROOT_DEVICE) return kind;
    if (save_root_absolute(root, out, size)) return SAVE_ROOT_EMPTY;
    kind = save_root_kind(out);
    return kind == SAVE_ROOT_RELATIVE ? SAVE_ROOT_EMPTY : kind;
}

#ifdef LANTERN_SAVE_TEST_HOOKS
void sys_save_data_test_root(const char *root);
void sys_save_data_test_jail(const char *jail);
void sys_save_data_test_jail_check(const char *path);
#endif

#endif
