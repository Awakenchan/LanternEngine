// SPDX-License-Identifier: GPL-3.0-or-later
#define _GNU_SOURCE
#include "runtime.h"
#include "sys/sys.h"
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifdef _WIN32
#define setenv(name, value, overwrite) _putenv_s(name, value)
#endif

#define CODE(e) ((int64_t)(int32_t)(UINT32_C(0x80020000) | (e)))

static int failures;
#define EXPECT(condition) do { if (!(condition)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); ++failures; } } while (0)

uint64_t host_monotonic_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000 + (uint64_t)t.tv_nsec;
}

static void *fn(const char *name) {
    const SysLibrary *libraries[] = {&sys_file, &sys_posix_file};
    for (size_t l = 0; l < 2; ++l)
        for (size_t i = 0; i < libraries[l]->count; ++i)
            if (!strcmp(libraries[l]->exports[i].name, name)) return libraries[l]->exports[i].function;
    printf("FAIL missing %s\n", name);
    ++failures;
    return NULL;
}

static void put(const char *path, const char *text) {
    FILE *f = fopen(path, "wb");
    EXPECT(f != NULL);
    if (!f) return;
    fputs(text, f);
    fclose(f);
}

static int64_t (SYS_ABI *open_)(const char *, int, int);
static int64_t (SYS_ABI *close_)(int);
static int64_t (SYS_ABI *read_)(int, void *, uint64_t);
static int64_t (SYS_ABI *pread_)(int, void *, uint64_t, int64_t);

static _Atomic int stop_readers;
static _Atomic long reads_done, reads_wrong;

static void *read_while_closing(void *argument) {
    int fd = *(int *)argument;
    char data[8];
    while (!stop_readers) {
        int64_t got = pread_(fd, data, 4, 0);
        if (got == 4 && memcmp(data, "aaaa", 4) && memcmp(data, "bbbb", 4)) ++reads_wrong;
        else if (got != 4 && got != CODE(9)) ++reads_wrong;
        ++reads_done;
    }
    return NULL;
}

int main(void) {
    char root[128];
    snprintf(root, sizeof(root), "%s/lantern-sys-file-XXXXXX", getenv("TEMP") ? getenv("TEMP") : "/tmp");
    EXPECT(mkdtemp(root) != NULL);
    char game[128], user[128], path[256], list[256], mod[256];
    snprintf(game, sizeof(game), "%s/game", root);
    snprintf(user, sizeof(user), "%s/user", root);
    mkdir(game, 0755);
    snprintf(path, sizeof(path), "%s/a.bin", game);
    put(path, "aaaa");
    snprintf(path, sizeof(path), "%s/b.bin", game);
    put(path, "bbbb");
    snprintf(mod, sizeof(mod), "%s/mod.bin", root);
    put(mod, "in place");
    snprintf(list, sizeof(list), "%s/mods.txt", root);
    FILE *f = fopen(list, "wb");
    if (f) {
        fprintf(f, "Param/Added.bin\t%s\n", mod);
        fclose(f);
    }
    setenv("BB_MOD_FILES", list, 1);
    EXPECT(!sys_file_configure(game, user));

    EXPECT(sys_file.count + sys_posix_file.count == 24);
    open_ = fn("sceKernelOpen");
    close_ = fn("sceKernelClose");
    read_ = fn("sceKernelRead");
    pread_ = fn("sceKernelPread");
    int64_t (SYS_ABI *write_)(int, const void *, uint64_t) = fn("sceKernelWrite");
    int64_t (SYS_ABI *lseek_)(int, int64_t, int) = fn("sceKernelLseek");
    int64_t (SYS_ABI *getdents)(int, char *, uint64_t, int64_t *) = fn("sceKernelGetdirentries");
    int64_t (SYS_ABI *unlink_)(const char *) = fn("sceKernelUnlink");
    int64_t (SYS_ABI *posix_read)(int, void *, uint64_t) = fn("_read");
    int64_t (SYS_ABI *posix_mkdir)(const char *, int) = fn("mkdir");

    char data[1024] = {0}, out[512];
    int fd = (int)open_("/app0/a.bin", 0, 0);
    EXPECT(fd >= 3);
    EXPECT(read_(fd, data, 2) == 2 && lseek_(fd, 0, 1) == 2);
    EXPECT(pread_(fd, data, 4, 0) == 4 && !memcmp(data, "aaaa", 4) && lseek_(fd, 0, 1) == 2);
    EXPECT(write_(fd, "x", 1) == CODE(9));
    EXPECT(getdents(fd, data, sizeof(data), NULL) == CODE(22));
    EXPECT(close_(fd) == 0 && close_(fd) == CODE(9));
    EXPECT(open_("/app0/a.bin", 2, 0) == CODE(30));
    EXPECT(open_("a.bin", 0x400, 0) == CODE(30));
    EXPECT(open_("/app0/../a.bin", 0, 0) == CODE(13));
    EXPECT(open_("/nowhere/a.bin", 0, 0) == CODE(2));
    EXPECT(unlink_("/app0/a.bin") == CODE(30));
    EXPECT(close_(1) == 0 && lseek_(1, 0, 0) == CODE(9));

    fd = (int)open_("/hostapp/PARAM/added.BIN", 0, 0);
    EXPECT(fd >= 3 && read_(fd, data, 8) == 8 && !memcmp(data, "in place", 8));
    close_(fd);

    EXPECT(!sys_file_mount("/deep", "outer") && !sys_file_mount("/deep/er", "inner"));
    EXPECT(!sys_file_translate("/deep/er/x", out, sizeof(out)) && !strcmp(out, "inner/x"));
    EXPECT(!sys_file_translate("/deep/erx", out, sizeof(out)) && !strcmp(out, "outer/erx"));
    sys_file_unmount("/deep/er");
    EXPECT(!sys_file_translate("/deep/er/x", out, sizeof(out)) && !strcmp(out, "outer/er/x"));
    sys_file_unmount("/deep");

    fd = (int)open_("/app0", 0, 0);
    EXPECT(fd >= 3);
    int64_t base = -1, count = getdents(fd, data, 511, &base);
    EXPECT(count == CODE(22));
    count = getdents(fd, data, sizeof(data), &base);
    EXPECT(count > 0 && base == 0);
    int found = 0;
    for (int64_t p = 0; p < count;) {
        uint16_t length;
        memcpy(&length, data + p + 4, 2);
        if (!strcmp(data + p + 8, "b.bin") && data[p + 6] == 8) found = 1;
        p += length ? length : count;
    }
    EXPECT(found);
    EXPECT(lseek_(fd, -1, 0) == CODE(22) && lseek_(fd, 0, 0) == 0);
    close_(fd);

    *runtime_errno() = 0;
    EXPECT(posix_read(9999, data, 1) == -1 && *runtime_errno() == 9);
    EXPECT(posix_mkdir("/app0/nope", 0755) == -1 && *runtime_errno() == 30);
    EXPECT(!posix_mkdir("/data/made", 0755));

    int opened = 0, last = -1;
    for (;;) {
        int64_t r = open_("/app0/a.bin", 0, 0);
        if (r < 0) {
            EXPECT(r == CODE(24));
            break;
        }
        last = (int)r;
        ++opened;
    }
    EXPECT(opened == 1021 && last == 1023);
    for (int i = 3; i < 1024; ++i) close_(i);

    int shared = (int)open_("/app0/a.bin", 0, 0);
    pthread_t readers[4];
    for (int i = 0; i < 4; ++i) pthread_create(&readers[i], NULL, read_while_closing, &shared);
    for (int i = 0; i < 20000; ++i) {
        close_(shared);
        int reopened = (int)open_(i & 1 ? "/app0/a.bin" : "/app0/b.bin", 0, 0);
        EXPECT(reopened == shared);
    }
    stop_readers = 1;
    for (int i = 0; i < 4; ++i) pthread_join(readers[i], NULL);
    EXPECT(reads_done > 0 && reads_wrong == 0);
    close_(shared);

    char command[300];
    snprintf(command, sizeof(command), "rm -rf %s", root);
    if (system(command)) ++failures;
    puts(failures ? "sys file test FAILED" : "Lantern file library: paths, mounts, mods, listings, errors, fd limit, close during reads PASS");
    return failures != 0;
}
