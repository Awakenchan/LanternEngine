// SPDX-License-Identifier: GPL-3.0-or-later
#define _GNU_SOURCE
#include "runtime.h"
#include "sys/sys.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures;
#define EXPECT(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

static void variable(const char *key, const char *value) {
#ifdef _WIN32
    EXPECT(!_putenv_s(key, value));
#else
    EXPECT(!setenv(key, value, 1));
#endif
}

static void put(const char *path, const char *text) {
    FILE *file = fopen(path, "wb");
    EXPECT(file != NULL);
    if (file) { fputs(text, file); EXPECT(!fclose(file)); }
}

static void resolves(const char *guest, const char *expected) {
    char out[1024];
    EXPECT(!sys_file_translate(guest, out, sizeof(out)));
    EXPECT(!strcmp(out, expected));
}

int main(void) {
    char root[512], path[1024], expected[1024], list[1024], custom[1024];
#ifdef _WIN32
    char temp[MAX_PATH];
    EXPECT(GetTempPathA(sizeof(temp), temp));
    EXPECT(GetTempFileNameA(temp, "lng", 0, root));
    EXPECT(DeleteFileA(root));
    EXPECT(!mkdir(root, 0755));
#else
    snprintf(root, sizeof(root), "/tmp/lantern-language-XXXXXX");
    EXPECT(mkdtemp(root) != NULL);
#endif
    static const char *const folders[] = {
        "dvdroot_ps4", "dvdroot_ps4/msg", "dvdroot_ps4/msg/engus", "dvdroot_ps4/msg/porbr",
        "dvdroot_ps4/menu", "dvdroot_ps4/menu/porbr", "dvdroot_ps4/sound", "user",
    };
    for (size_t i = 0; i < sizeof(folders) / sizeof(*folders); ++i) {
        snprintf(path, sizeof(path), "%s/%s", root, folders[i]);
        EXPECT(!mkdir(path, 0755));
    }
    static const char *const assets[] = {
        "dvdroot_ps4/msg/porbr/menu.msgbnd.dcx", "dvdroot_ps4/msg/engus/item.msgbnd.dcx",
        "dvdroot_ps4/menu/porbr/font.gfx", "dvdroot_ps4/sound/sprj_c1020_ptb.fsb",
    };
    for (size_t i = 0; i < sizeof(assets) / sizeof(*assets); ++i) {
        snprintf(path, sizeof(path), "%s/%s", root, assets[i]);
        put(path, "fictional language fixture");
    }
    EXPECT(!sys_file_mount("/app0", root));
    EXPECT(!sys_file_mount("/hostapp", root));
    variable("BB_LANGUAGE", "17");
    const char *const sources[] = {"engus", "enggb", "rusru", "jpnjp", "frafr", "spaes", "spaar", "deude", "itait", "porpt"};
    snprintf(expected, sizeof(expected), "%s/dvdroot_ps4/msg/porbr/menu.msgbnd.dcx", root);
    for (size_t i = 0; i < sizeof(sources) / sizeof(*sources); ++i) {
        snprintf(path, sizeof(path), "/app0/dvdroot_ps4/msg/%s/menu.msgbnd.dcx", sources[i]);
        resolves(path, expected);
    }
    resolves("dvdroot_ps4/msg/engus/menu.msgbnd.dcx", expected);
    resolves("/hostapp/dvdroot_ps4/msg/porpt/menu.msgbnd.dcx", expected);
    snprintf(expected, sizeof(expected), "%s/dvdroot_ps4/menu/porbr/font.gfx", root);
    resolves("/app0/dvdroot_ps4/menu/porpt/font.gfx", expected);
    snprintf(expected, sizeof(expected), "%s/dvdroot_ps4/sound/sprj_c1020_ptb.fsb", root);
    const char *const voices[] = {"eng", "ded", "esa", "ese", "frf", "iti", "jaj", "ptb"};
    for (size_t i = 0; i < sizeof(voices) / sizeof(*voices); ++i) {
        snprintf(path, sizeof(path), "/app0/dvdroot_ps4/sound/sprj_c1020_%s.fsb", voices[i]);
        resolves(path, expected);
    }
    /* Missing Brazilian files retain the game's requested asset, never another bank. */
    snprintf(expected, sizeof(expected), "%s/dvdroot_ps4/msg/engus/item.msgbnd.dcx", root);
    resolves("/app0/dvdroot_ps4/msg/engus/item.msgbnd.dcx", expected);
    snprintf(expected, sizeof(expected), "%s/dvdroot_ps4/sound/sprj_c9999_eng.fsb", root);
    resolves("/app0/dvdroot_ps4/sound/sprj_c9999_eng.fsb", expected);
    snprintf(expected, sizeof(expected), "%s/dvdroot_ps4/sound/sprj_smain.fsb", root);
    resolves("/app0/dvdroot_ps4/sound/sprj_smain.fsb", expected);
    snprintf(expected, sizeof(expected), "%s/dvdroot_ps4/menu/eu/common.tpf.dcx", root);
    resolves("/app0/dvdroot_ps4/menu/eu/common.tpf.dcx", expected);
    snprintf(expected, sizeof(expected), "%s/dvdroot_ps4/sound/sprj_c1020_eng.fev", root);
    resolves("/app0/dvdroot_ps4/sound/sprj_c1020_eng.fev", expected);
    /* No language remapping outside the game mounts. */
    EXPECT(!sys_file_mount("/data", root));
    snprintf(expected, sizeof(expected), "%s/dvdroot_ps4/msg/engus/menu.msgbnd.dcx", root);
    resolves("/data/dvdroot_ps4/msg/engus/menu.msgbnd.dcx", expected);
    for (int i = 0; i < 3; ++i) {
        variable("BB_LANGUAGE", i == 0 ? "1" : i == 1 ? "3" : "");
        resolves("/app0/dvdroot_ps4/msg/engus/menu.msgbnd.dcx", expected);
    }
    variable("BB_LANGUAGE", "17");
    char small[2];
    EXPECT(sys_file_translate("/app0/dvdroot_ps4/msg/engus/menu.msgbnd.dcx", small, sizeof(small)) == ENAMETOOLONG);
    EXPECT(sys_file_translate("/app0/../dvdroot_ps4/msg/engus/menu.msgbnd.dcx", path, sizeof(path)) == EACCES);
    char long_path[1100];
    memset(long_path, 'x', sizeof(long_path) - 1);
    long_path[sizeof(long_path) - 1] = 0;
    EXPECT(sys_file_translate(long_path, path, sizeof(path)) == ENAMETOOLONG);
    /* A mod on the selected Brazilian asset keeps precedence over the game copy. */
    snprintf(custom, sizeof(custom), "%s/custom-menu.bin", root);
    put(custom, "fictional mod fixture");
    snprintf(list, sizeof(list), "%s/mods.txt", root);
    FILE *file = fopen(list, "wb");
    EXPECT(file != NULL);
    if (file) { fprintf(file, "dvdroot_ps4/msg/porbr/menu.msgbnd.dcx\t%s\n", custom); fclose(file); }
    variable("BB_MOD_FILES", list);
    snprintf(path, sizeof(path), "%s/user", root);
    sys_file_configure(root, path);
    resolves("/app0/dvdroot_ps4/msg/engus/menu.msgbnd.dcx", custom);
    /* A per-file mount for the selected language is resolved as usual too. */
    EXPECT(!sys_file_mount("/app0/dvdroot_ps4/sound/sprj_c1020_ptb.fsb", custom));
    resolves("/app0/dvdroot_ps4/sound/sprj_c1020_ese.fsb", custom);
    sys_file_unmount("/app0/dvdroot_ps4/sound/sprj_c1020_ptb.fsb");
    variable("BB_LANGUAGE", "");
    variable("BB_MOD_FILES", "");
    remove(custom);
    remove(list);
    for (size_t i = 0; i < sizeof(assets) / sizeof(*assets); ++i) {
        snprintf(path, sizeof(path), "%s/%s", root, assets[i]); remove(path);
    }
    const char *const writable[] = {"user/temp0", "user/download0", "user/data"};
    for (size_t i = 0; i < sizeof(writable) / sizeof(*writable); ++i) {
        snprintf(path, sizeof(path), "%s/%s", root, writable[i]); rmdir(path);
    }
    for (size_t i = sizeof(folders) / sizeof(*folders); i-- > 0;) {
        snprintf(path, sizeof(path), "%s/%s", root, folders[i]); rmdir(path);
    }
    EXPECT(!rmdir(root));
    puts(failures ? "Brazilian game language: FAILED" : "Brazilian game language: text, voices, fallback, mods and mounts PASS");
    return failures != 0;
}
