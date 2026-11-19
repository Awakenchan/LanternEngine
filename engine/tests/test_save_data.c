// SPDX-License-Identifier: GPL-3.0-or-later
#define _GNU_SOURCE
#include "runtime.h"
#include "sys/sys.h"
#include "sys/save_replace.h"
#include "sys/save_root.h"
#include <errno.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utime.h>
#ifdef _WIN32
#include <process.h>
#else
#include <sys/wait.h>
#endif

enum { MODE_READ_ONLY = 1, MODE_READ_WRITE = 2, MODE_CREATE_OR_OPEN = 32 };
enum { PARAM_ALL = 0, PARAM_TITLE = 1 };
enum { PARAM_SIZE = 1328, EXIT_CODE_BASE = 70, LAST_STEP = 3, JAIL_EXIT_CODE = 79 };
enum { CHILD_OK = 0, CHILD_NO_MOUNT = 2, CHILD_CALL_FAILED = 3 };
enum { SOUND_FLAG_OFFSET = 0x204E, SOUND_FILE_SIZE = 0x40000, SHORT_SOUND_FILE_SIZE = 0x2100 };
enum { MUTANT_ALWAYS_WRITE = 1, MUTANT_IGNORE_BACKUP = 2, MUTANT_ALLOW_DRIVE_ROOT = 3 };
enum { OLD_TIME = 946684800 };

#define TITLE "LNTN00001"
#define BLOODBORNE "CUSA03173"
#define SAVE "SPRJ0005"
#define TEMPORARY_SUFFIX ".lantern-save-tmp"
#ifdef _WIN32
#define LIVE_PID "4"
#else
#define LIVE_PID "1"
#endif
#define DEAD_PID "999999937"

typedef struct { char data[10]; char padding[6]; } TitleId;
typedef struct { char data[32]; } DirName;
typedef struct { char data[16]; } MountPoint;

typedef struct {
    int32_t user, padding;
    const TitleId *title;
    const DirName *dir;
    const void *fingerprint;
    uint64_t blocks;
    uint32_t mode;
    uint8_t reserved[32];
} MountRequest;

typedef struct {
    MountPoint point;
    uint64_t required_blocks;
    uint32_t unused, status;
    uint8_t reserved[28];
    int32_t padding;
} MountResult;

typedef struct {
    const void *buffer;
    uint64_t buffer_size, data_size;
    uint8_t reserved[32];
} SaveIcon;

typedef int32_t (SYS_ABI *InitializeCall)(const void *);
typedef int32_t (SYS_ABI *MountCall)(const MountRequest *, MountResult *);
typedef int32_t (SYS_ABI *UnmountCall)(const MountPoint *);
typedef int32_t (SYS_ABI *SetParamCall)(const MountPoint *, uint32_t, const void *, uint64_t);
typedef int32_t (SYS_ABI *IconCall)(const MountPoint *, const SaveIcon *);

static int failures;
#define EXPECT(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)
#define REQUIRE(c) do { if (!(c)) { printf("FAIL %s:%d: setup: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

static char root[700], self[1024], start_dir[700];

static void *call(const char *nid) {
    const SysExport *e = sys_find(nid, NULL);
    REQUIRE(e != NULL);
    return e->function;
}

static void variable(const char *key, const char *value) {
#ifdef _WIN32
    REQUIRE(!_putenv_s(key, value));
#else
    REQUIRE(!setenv(key, value, 1));
#endif
}

static void forward_slashes(char *path) {
    for (char *at = path; *at; ++at)
        if (*at == '\\') *at = '/';
}

static void configure(const char *title) {
    char app0[800], user[800];
    REQUIRE(root[0] == '/' || root[1] == ':');
    snprintf(app0, sizeof(app0), "%s/app0", root);
    snprintf(user, sizeof(user), "%s/user", root);
    mkdir(app0, 0755);
    REQUIRE(!sys_file_configure(app0, user));
    sys_save_data_configure(title);
}

static void save_path(const char *title, const char *tail, char *out, size_t size) {
    snprintf(out, size, "%s/user/savedata/1/%s/%s", root, title, tail);
}

static void make_folder(const char *path) {
    REQUIRE(!mkdir(path, 0755) || errno == EEXIST);
}

static int mount(uint32_t mode, MountResult *result) {
    static const TitleId title = {TITLE, {0}};
    static const DirName dir = {SAVE};
    MountRequest request = {.user = 1, .title = &title, .dir = &dir, .mode = mode};
    memset(result, 0, sizeof(*result));
    return ((MountCall)call("32HQAQdwM2o"))(&request, result);
}

static int unmount(const MountResult *result) {
    return ((UnmountCall)call("BMR4F-Uek3E"))(&result->point);
}

static int32_t write_details(const char *what, const MountPoint *point, const char *text) {
    if (!strcmp(what, "icon")) {
        SaveIcon icon = {.buffer = text, .buffer_size = strlen(text), .data_size = strlen(text)};
        return ((IconCall)call("c88Yy54Mx0w"))(point, &icon);
    }
    if (!strcmp(what, "paramall")) {
        char whole[PARAM_SIZE] = {0};
        snprintf(whole, 128, "%s", text);
        return ((SetParamCall)call("85zul--eGXs"))(point, PARAM_ALL, whole, sizeof(whole));
    }
    return ((SetParamCall)call("85zul--eGXs"))(point, PARAM_TITLE, text, strlen(text));
}

#ifdef _WIN32
static const unsigned long retried_errors[] = {5, 32, 33};
static const unsigned long other_error = 2;
#else
static const unsigned long retried_errors[] = {EBUSY, ETXTBSY};
static const unsigned long other_error = ENOENT;
#endif

static int replace_attempts(const MountPoint *point, const unsigned long *errors, int count, const char *text) {
    sys_save_data_test_replace_errors(errors, count);
    if (write_details("param", point, text)) return -1;
    return sys_save_data_test_replace_attempts();
}

static int replace_retries_behave(const MountPoint *point) {
    int ok = 1;
    unsigned long busy = retried_errors[1], spend[SAVE_REPLACE_RETRIES + 1];
    for (size_t i = 0; i < sizeof(retried_errors) / sizeof(retried_errors[0]); ++i) {
        unsigned long once[2] = {retried_errors[i], 0};
        ok &= replace_attempts(point, once, 2, "retried") == 2;
    }
    ok &= replace_attempts(point, &other_error, 1, "other") == 1;
    for (int i = 0; i <= SAVE_REPLACE_RETRIES; ++i) spend[i] = busy;
    ok &= replace_attempts(point, spend, SAVE_REPLACE_RETRIES + 1, "spent") == SAVE_REPLACE_RETRIES + 1;
    ok &= replace_attempts(point, &busy, 1, "latched") == 1;
    ok &= replace_attempts(point, NULL, 0, "works") == 1;
    unsigned long again[2] = {busy, 0};
    ok &= replace_attempts(point, again, 2, "again") == 2;
    sys_save_data_test_replace_errors(NULL, 0);
    return ok;
}

static int child(const char *what, const char *text) {
    sys_save_data_test_jail(root);
    if (!strcmp(what, "replace-retries")) {
        configure(TITLE);
        REQUIRE(!((InitializeCall)call("ZkZhskCPXFw"))(NULL));
        MountResult mounted;
        if (mount(MODE_CREATE_OR_OPEN | MODE_READ_WRITE, &mounted)) return CHILD_NO_MOUNT;
        int ok = replace_retries_behave(&mounted.point);
        return ok && !unmount(&mounted) ? CHILD_OK : CHILD_CALL_FAILED;
    }
    if (strcmp(what, "default-root")) configure(TITLE);
    else sys_save_data_configure(TITLE);
    if (!strcmp(what, "seam-root")) {
        sys_save_data_test_root(text);
        sys_save_data_configure(TITLE);
        what = "param";
        text = "seamed";
    }
    REQUIRE(!((InitializeCall)call("ZkZhskCPXFw"))(NULL));
    MountResult mounted;
    if (mount(MODE_CREATE_OR_OPEN | MODE_READ_WRITE, &mounted)) return CHILD_NO_MOUNT;
    int32_t result = write_details(what, &mounted.point, text);
    return result || unmount(&mounted) ? CHILD_CALL_FAILED : CHILD_OK;
}

static int run_child(const char *what, int step, const char *text) {
    char number[16];
    snprintf(number, sizeof(number), "%d", step);
    variable("BB_SAVE_TEST_EXIT_STEP", number);
#ifdef _WIN32
    char quoted_root[800], quoted_text[1200];
    size_t length = strlen(text), slashes = 0;
    while (slashes < length && text[length - 1 - slashes] == '\\') ++slashes;
    snprintf(quoted_root, sizeof(quoted_root), "\"%s\"", root);
    REQUIRE(length + slashes + 3 < sizeof(quoted_text));
    snprintf(quoted_text, sizeof(quoted_text), "\"%s%.*s\"", text, (int)slashes, text + length - slashes);
    const char *const arguments[] = {"save-data-test", "child", quoted_root, what, quoted_text, NULL};
    intptr_t status = _spawnv(_P_WAIT, self, arguments);
    variable("BB_SAVE_TEST_EXIT_STEP", "0");
    return (int)status;
#else
    pid_t pid = fork();
    if (!pid) {
        execl(self, self, "child", root, what, text, (char *)NULL);
        _exit(127);
    }
    int status = 0;
    REQUIRE(pid > 0 && waitpid(pid, &status, 0) == pid);
    variable("BB_SAVE_TEST_EXIT_STEP", "0");
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
}

static long file_size(const char *path) {
    struct stat info;
    return stat(path, &info) ? -1 : (long)info.st_size;
}

static int is_folder(const char *path) {
    struct stat info;
    return !stat(path, &info) && S_ISDIR(info.st_mode);
}

static int read_file(const char *path, char *out, size_t size) {
    FILE *file = fopen(path, "rb");
    if (!file) return -1;
    size_t got = fread(out, 1, size, file);
    EXPECT(!fclose(file));
    return (int)got;
}

static void write_file(const char *path, const void *data, size_t size) {
    FILE *file = fopen(path, "wb");
    REQUIRE(file != NULL);
    REQUIRE(fwrite(data, 1, size, file) == size);
    REQUIRE(!fclose(file));
}

static void param_path(char *out, size_t size) {
    save_path(TITLE, SAVE ".sce_sys/param.bin", out, size);
}

static void expect_title(const char *title) {
    char path[1024], param[PARAM_SIZE + 1] = {0};
    param_path(path, sizeof(path));
    EXPECT(read_file(path, param, sizeof(param)) == PARAM_SIZE);
    EXPECT(!strcmp(param, title));
}

static int count_temporaries(const char *folder) {
    DIR *dir = opendir(folder);
    int found = 0;
    for (struct dirent *entry; dir && (entry = readdir(dir));)
        if (entry->d_name[0] == '.' && strstr(entry->d_name, TEMPORARY_SUFFIX)) ++found;
    if (dir) closedir(dir);
    return found;
}

static int temporaries(void) {
    char path[1024];
    save_path(TITLE, SAVE ".sce_sys", path, sizeof(path));
    return count_temporaries(path);
}

static void write_short_param(void) {
    char path[1024];
    param_path(path, sizeof(path));
    write_file(path, "short", 5);
}

static void killed_param_writes_keep_the_old_file(void) {
    EXPECT(run_child("param", 0, "old") == CHILD_OK);
    expect_title("old");
    EXPECT(temporaries() == 0);
    for (int step = 1; step <= LAST_STEP; ++step) {
        EXPECT(run_child("param", step, "new") == EXIT_CODE_BASE + step);
        expect_title("old");
        EXPECT(temporaries() == 1);
        configure(TITLE);
        EXPECT(temporaries() == 0);
        expect_title("old");
    }
    EXPECT(run_child("param", 0, "new") == CHILD_OK);
    expect_title("new");
    EXPECT(temporaries() == 0);
}

static void killed_icon_writes_keep_the_old_file(void) {
    char path[1024], icon[64] = {0};
    save_path(TITLE, SAVE ".sce_sys/icon0.png", path, sizeof(path));
    EXPECT(run_child("icon", 0, "first icon") == CHILD_OK);
    EXPECT(read_file(path, icon, sizeof(icon)) == 10 && !strcmp(icon, "first icon"));
    EXPECT(run_child("icon", LAST_STEP, "second icon") == EXIT_CODE_BASE + LAST_STEP);
    memset(icon, 0, sizeof(icon));
    EXPECT(read_file(path, icon, sizeof(icon)) == 10 && !strcmp(icon, "first icon"));
    configure(TITLE);
    EXPECT(temporaries() == 0);
}

static void unreadable_param_is_rebuilt(void) {
    write_short_param();
    EXPECT(run_child("param", 0, "rebuilt") == CHILD_OK);
    expect_title("rebuilt");
}

static void param_all_replaces_a_short_param(void) {
    write_short_param();
    EXPECT(run_child("paramall", 0, "whole") == CHILD_OK);
    expect_title("whole");
}

static void missing_meta_folder_is_created(void) {
    char path[1024];
    param_path(path, sizeof(path));
    EXPECT(!remove(path));
    save_path(TITLE, SAVE ".sce_sys/icon0.png", path, sizeof(path));
    EXPECT(!remove(path));
    save_path(TITLE, SAVE ".sce_sys", path, sizeof(path));
    EXPECT(!rmdir(path));
    EXPECT(run_child("param", 0, "imported") == CHILD_OK);
    expect_title("imported");
    EXPECT(run_child("icon", 0, "imported icon") == CHILD_OK);
}

static void failed_replace_keeps_the_old_file_and_the_save_goes_on(void) {
    char path[1024];
    param_path(path, sizeof(path));
    EXPECT(!remove(path));
    make_folder(path);
    EXPECT(run_child("param", 0, "blocked") == CHILD_OK);
    EXPECT(is_folder(path));
    EXPECT(temporaries() == 0);
    EXPECT(!rmdir(path));
    EXPECT(run_child("param", 0, "kept") == CHILD_OK);
    expect_title("kept");
#ifdef _WIN32
    FILE *held = fopen(path, "rb");
    REQUIRE(held != NULL);
    EXPECT(run_child("param", 0, "held") == CHILD_OK);
    EXPECT(!fclose(held));
    expect_title("kept");
    EXPECT(temporaries() == 0);
#endif
}

static void replace_decisions(void) {
    for (size_t i = 0; i < sizeof(retried_errors) / sizeof(retried_errors[0]); ++i)
        EXPECT(save_replace_may_pass_later(retried_errors[i]));
    EXPECT(!save_replace_may_pass_later(other_error));
    EXPECT(!save_replace_may_pass_later(0));
    EXPECT(save_replace_retries(0) == SAVE_REPLACE_RETRIES);
    EXPECT(save_replace_retries(1) == 0);
    EXPECT(save_replace_spent_after(0, retried_errors[0], SAVE_REPLACE_RETRIES) == 1);
    EXPECT(save_replace_spent_after(0, other_error, SAVE_REPLACE_RETRIES) == 0);
    EXPECT(save_replace_spent_after(0, retried_errors[0], 0) == 0);
    EXPECT(save_replace_spent_after(1, retried_errors[0], 0) == 1);
    EXPECT(save_replace_spent_after(1, other_error, 0) == 1);
    EXPECT(save_replace_spent_after(1, 0, 0) == 0);
    EXPECT(save_replace_spent_after(0, 0, SAVE_REPLACE_RETRIES) == 0);
}

static void replace_retries_and_their_latch(void) {
    EXPECT(run_child("replace-retries", 0, "-") == CHILD_OK);
    expect_title("again");
    variable("BB_SAVE_TEST_MUTANT", "4");
    int any_error = run_child("replace-retries", 0, "-") == CHILD_CALL_FAILED;
    variable("BB_SAVE_TEST_MUTANT", "5");
    int latch_kept = run_child("replace-retries", 0, "-") == CHILD_CALL_FAILED;
    variable("BB_SAVE_TEST_MUTANT", "0");
    EXPECT(any_error);
    EXPECT(latch_kept);
    if (any_error && latch_kept) puts("save data mutants caught: anyerror, latchkept");
}

static void live_temporaries_are_kept(void) {
    char folder[1024], live[1200], dead[1200];
    save_path(TITLE, SAVE ".sce_sys", folder, sizeof(folder));
    snprintf(live, sizeof(live), "%s/.param.bin." LIVE_PID TEMPORARY_SUFFIX, folder);
    snprintf(dead, sizeof(dead), "%s/.param.bin." DEAD_PID TEMPORARY_SUFFIX, folder);
    write_file(live, "live", 4);
    write_file(dead, "dead", 4);
    configure(TITLE);
    EXPECT(file_size(live) == 4);
    EXPECT(file_size(dead) == -1);
    EXPECT(!remove(live));
}

static void writable_mounts_are_counted(void) {
    configure(TITLE);
    REQUIRE(!((InitializeCall)call("ZkZhskCPXFw"))(NULL));
    MountResult first, second;
    EXPECT(sys_save_data_writable_mounts() == 0);
    EXPECT(!mount(MODE_READ_ONLY, &first));
    EXPECT(sys_save_data_writable_mounts() == 0);
    EXPECT(!unmount(&first));
    EXPECT(!mount(MODE_READ_WRITE, &second));
    EXPECT(sys_save_data_writable_mounts() == 1);
    EXPECT(!unmount(&second));
    EXPECT(sys_save_data_writable_mounts() == 0);
    EXPECT(unmount(&second) != 0);
    EXPECT(sys_save_data_writable_mounts() == 0);
}

static void sound_paths(char *save, char *meta, char *backup, size_t size) {
    save_path(BLOODBORNE, SAVE "/userdata0010", save, size);
    save_path(BLOODBORNE, SAVE ".sce_sys", meta, size);
    save_path(BLOODBORNE, SAVE ".sce_sys/userdata0010.sound-flag-backup", backup, size);
}

static void write_sound_save(char first_byte, int flag, size_t size) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/user/savedata", root);
    make_folder(path);
    snprintf(path, sizeof(path), "%s/user/savedata/1", root);
    make_folder(path);
    save_path(BLOODBORNE, "", path, sizeof(path));
    make_folder(path);
    save_path(BLOODBORNE, SAVE, path, sizeof(path));
    make_folder(path);
    static char data[SOUND_FILE_SIZE];
    memset(data, 0, sizeof(data));
    data[0] = first_byte;
    if (size > SOUND_FLAG_OFFSET) data[SOUND_FLAG_OFFSET] = (char)flag;
    save_path(BLOODBORNE, SAVE "/userdata0010", path, sizeof(path));
    write_file(path, data, size);
}

static int sound_flag(void) {
    static char data[SOUND_FILE_SIZE + 16];
    char save[1024], meta[1024], backup[1024];
    sound_paths(save, meta, backup, sizeof(save));
    return read_file(save, data, sizeof(data)) == SOUND_FILE_SIZE ? data[SOUND_FLAG_OFFSET] : -1;
}

static char backup_first_byte(void) {
    static char data[SOUND_FILE_SIZE + 16];
    char save[1024], meta[1024], backup[1024];
    sound_paths(save, meta, backup, sizeof(save));
    return read_file(backup, data, sizeof(data)) == SOUND_FILE_SIZE ? data[0] : 0;
}

static void clear_sound_meta(void) {
    char save[1024], meta[1024], backup[1024];
    sound_paths(save, meta, backup, sizeof(save));
    remove(backup);
    if (is_folder(meta)) REQUIRE(!rmdir(meta));
    else remove(meta);
}

static int wanted_save_is_opened_for_writing(void) {
    char save[1024], meta[1024], backup[1024];
    sound_paths(save, meta, backup, sizeof(save));
    write_sound_save('W', 1, SOUND_FILE_SIZE);
    struct utimbuf old = {OLD_TIME, OLD_TIME};
    REQUIRE(!utime(save, &old));
    configure(BLOODBORNE);
    struct stat info;
    REQUIRE(!stat(save, &info));
    return info.st_mtime != OLD_TIME;
}

static int failed_backup_changes_the_save(void) {
    char save[1024], meta[1024], backup[1024];
    sound_paths(save, meta, backup, sizeof(save));
    clear_sound_meta();
    write_file(meta, "not a folder", 12);
    write_sound_save('F', 0, SOUND_FILE_SIZE);
    configure(BLOODBORNE);
    int changed = sound_flag() != 0;
    REQUIRE(!remove(meta));
    return changed;
}

static void sound_flag_is_set_safely(void) {
    clear_sound_meta();
    write_sound_save('A', 0, SOUND_FILE_SIZE);
    configure(BLOODBORNE);
    EXPECT(sound_flag() == 1);
    EXPECT(backup_first_byte() == 'A');
    write_sound_save('B', 0, SOUND_FILE_SIZE);
    configure(BLOODBORNE);
    EXPECT(sound_flag() == 1);
    EXPECT(backup_first_byte() == 'A');
    EXPECT(!wanted_save_is_opened_for_writing());
    EXPECT(!failed_backup_changes_the_save());
    clear_sound_meta();
    write_sound_save('M', 0, SOUND_FILE_SIZE);
    configure(BLOODBORNE);
    EXPECT(sound_flag() == 1);
    EXPECT(backup_first_byte() == 'M');
    write_sound_save('S', 0, SHORT_SOUND_FILE_SIZE);
    configure(BLOODBORNE);
    char save[1024], meta[1024], backup[1024], data[SHORT_SOUND_FILE_SIZE + 16];
    sound_paths(save, meta, backup, sizeof(save));
    EXPECT(read_file(save, data, sizeof(data)) == SHORT_SOUND_FILE_SIZE && data[SOUND_FLAG_OFFSET] == 0);
    variable("BB_SOUND_HACK", "0");
    write_sound_save('C', 0, SOUND_FILE_SIZE);
    configure(BLOODBORNE);
    EXPECT(sound_flag() == 0);
    variable("BB_SOUND_HACK", "1");
}

static void mutants_are_caught(void) {
    variable("BB_SAVE_TEST_MUTANT", "1");
    int always_write = wanted_save_is_opened_for_writing();
    variable("BB_SAVE_TEST_MUTANT", "2");
    int ignore_backup = failed_backup_changes_the_save();
    variable("BB_SAVE_TEST_MUTANT", "0");
    configure(BLOODBORNE);
    EXPECT(always_write);
    EXPECT(ignore_backup);
    if (always_write && ignore_backup) puts("save data mutants caught: alwaysrw, nobackupcheck");
}

static void save_root_kinds(void) {
    char out[64];
    EXPECT(save_root_kind(NULL) == SAVE_ROOT_EMPTY);
    EXPECT(save_root_kind("") == SAVE_ROOT_EMPTY);
    EXPECT(save_root_kind("/") == SAVE_ROOT_DRIVE);
    EXPECT(save_root_kind("/.") == SAVE_ROOT_DRIVE);
    EXPECT(save_root_kind("//") == SAVE_ROOT_DRIVE);
    EXPECT(save_root_kind("/missing/..") == SAVE_ROOT_DRIVE);
    EXPECT(save_root_kind("/missing/../user") == SAVE_ROOT_USABLE);
    EXPECT(save_root_kind("user") == SAVE_ROOT_RELATIVE);
    EXPECT(save_root_kind("./user") == SAVE_ROOT_RELATIVE);
    EXPECT(save_root_kind("/home/hunter/user") == SAVE_ROOT_USABLE);
    EXPECT(save_root_resolve("/missing/..", out, sizeof(out)) == SAVE_ROOT_DRIVE);
#ifdef _WIN32
    EXPECT(save_root_kind("\\") == SAVE_ROOT_DRIVE);
    EXPECT(save_root_kind("/\\") == SAVE_ROOT_DRIVE);
    EXPECT(save_root_kind("E:/") == SAVE_ROOT_DRIVE);
    EXPECT(save_root_kind("E:\\") == SAVE_ROOT_DRIVE);
    EXPECT(save_root_kind("E:") == SAVE_ROOT_RELATIVE);
    EXPECT(save_root_kind("E:user") == SAVE_ROOT_RELATIVE);
    EXPECT(save_root_kind("E:/data/user") == SAVE_ROOT_USABLE);
    EXPECT(save_root_kind("E:\\data\\user") == SAVE_ROOT_USABLE);
    EXPECT(save_root_kind("E:\\.") == SAVE_ROOT_DRIVE);
    EXPECT(save_root_kind("E:\\..") == SAVE_ROOT_DRIVE);
    EXPECT(save_root_kind("E:\\\\") == SAVE_ROOT_DRIVE);
    EXPECT(save_root_kind("E:\\data\\..") == SAVE_ROOT_DRIVE);
    EXPECT(save_root_kind("E:/data/./user") == SAVE_ROOT_USABLE);
    EXPECT(save_root_kind("\\.") == SAVE_ROOT_DRIVE);
    EXPECT(save_root_kind("\\\\?\\E:\\") == SAVE_ROOT_DEVICE);
    EXPECT(save_root_kind("\\\\?\\E:\\data\\user") == SAVE_ROOT_DEVICE);
    EXPECT(save_root_kind("\\\\.\\E:\\") == SAVE_ROOT_DEVICE);
    EXPECT(save_root_kind("//?/E:/") == SAVE_ROOT_DEVICE);
    EXPECT(save_root_kind("\\\\server") == SAVE_ROOT_DRIVE);
    EXPECT(save_root_kind("\\\\server\\share") == SAVE_ROOT_DRIVE);
    EXPECT(save_root_kind("\\\\server\\share\\") == SAVE_ROOT_DRIVE);
    EXPECT(save_root_kind("\\\\server\\share\\.") == SAVE_ROOT_DRIVE);
    EXPECT(save_root_kind("\\\\server\\share\\user") == SAVE_ROOT_USABLE);
    EXPECT(save_root_kind_in("E:\\\x95\\..", 932) == SAVE_ROOT_USABLE);
    EXPECT(save_root_kind_in("E:\\\x95\\..", 1252) == SAVE_ROOT_DRIVE);
    EXPECT(save_root_kind_in("\\\\server\\\x95\\x", 932) == SAVE_ROOT_DRIVE);
    EXPECT(save_root_kind_in("\\\\server\\\x95\\x", 1252) == SAVE_ROOT_USABLE);
    EXPECT(save_root_resolve("\\\\?\\E:\\", out, sizeof(out)) == SAVE_ROOT_DEVICE);
    EXPECT(save_root_resolve("\\\\server\\share\\", out, sizeof(out)) == SAVE_ROOT_DRIVE);
    EXPECT(save_root_resolve("E:/data/user", out, sizeof(out)) == SAVE_ROOT_USABLE);
    EXPECT(!strcmp(out, "E:\\data\\user"));
#else
    EXPECT(save_root_kind("\\") == SAVE_ROOT_RELATIVE);
    EXPECT(save_root_kind("E:/") == SAVE_ROOT_RELATIVE);
    EXPECT(save_root_kind("/\\") == SAVE_ROOT_USABLE);
    EXPECT(save_root_kind("//server/share") == SAVE_ROOT_USABLE);
    EXPECT(save_root_kind("//?/E:/") == SAVE_ROOT_USABLE);
    EXPECT(save_root_kind("\\\\?\\E:\\") == SAVE_ROOT_RELATIVE);
    EXPECT(save_root_resolve("/lantern-missing-root/../user", out, sizeof(out)) == SAVE_ROOT_USABLE);
    EXPECT(!strcmp(out, "/user"));
#endif
}

static void expect_relative_save(const char *folder, const char *user) {
    char path[1200], param[PARAM_SIZE + 1] = {0};
    snprintf(path, sizeof(path), "%s/%s/savedata/1/" TITLE "/" SAVE ".sce_sys/param.bin", folder, user);
    EXPECT(read_file(path, param, sizeof(param)) == PARAM_SIZE);
    EXPECT(!strcmp(param, "relative"));
}

static void expect_seamed_save(const char *user) {
    char path[1200], param[PARAM_SIZE + 1] = {0};
    snprintf(path, sizeof(path), "%s/savedata/1/" TITLE "/" SAVE ".sce_sys/param.bin", user);
    EXPECT(read_file(path, param, sizeof(param)) == PARAM_SIZE);
    EXPECT(!strcmp(param, "seamed"));
}

static void absolute_roots_are_used(void) {
    char user[800], back[800];
    snprintf(user, sizeof(user), "%s/absolute-user", root);
    EXPECT(run_child("seam-root", 0, user) == CHILD_OK);
    expect_seamed_save(user);
    snprintf(back, sizeof(back), "%s/absolute-back", root);
#ifdef _WIN32
    for (char *at = back; *at; ++at)
        if (*at == '/') *at = '\\';
#endif
    EXPECT(run_child("seam-root", 0, back) == CHILD_OK);
    snprintf(back, sizeof(back), "%s/absolute-back", root);
    expect_seamed_save(back);
}

static void drive_root_savedata(char *out, size_t size) {
#ifdef _WIN32
    snprintf(out, size, "%c:/savedata", root[0]);
#else
    snprintf(out, size, "/savedata");
#endif
}

static void unusable_roots_create_nothing(void) {
    char savedata[32], drive[4] = {root[0], ':', '/', 0};
#ifdef _WIN32
    char back[4] = {root[0], ':', '\\', 0}, dot[5] = {root[0], ':', '\\', '.', 0};
    char up[6] = {root[0], ':', '\\', '.', '.', 0}, twice[5] = {root[0], ':', '\\', '\\', 0};
    char device[8] = {'\\', '\\', '?', '\\', root[0], ':', '\\', 0};
    char local[8] = {'\\', '\\', '.', '\\', root[0], ':', '\\', 0};
    const char *roots[] = {"", "/", "\\", drive, back, dot, up, twice, "\\.", "/.", "//", "/\\", device, local,
                           "\\\\server\\share", "\\\\server\\share\\", "/lantern-missing-root/.."};
#else
    const char *roots[] = {"", "/", "/.", "//", "/lantern-missing-root/.."};
#endif
    drive_root_savedata(savedata, sizeof(savedata));
    REQUIRE(!is_folder(savedata) && file_size(savedata) == -1);
    for (size_t i = 0; i < sizeof(roots) / sizeof(roots[0]); ++i) {
        EXPECT(run_child("seam-root", 0, roots[i]) == CHILD_NO_MOUNT);
        EXPECT(file_size(savedata) == -1);
    }
    variable("BB_SAVE_TEST_MUTANT", "3");
#ifdef _WIN32
    int allow_drive_root = run_child("seam-root", 0, drive) == JAIL_EXIT_CODE;
#else
    (void)drive;
    int allow_drive_root = run_child("seam-root", 0, "/") == JAIL_EXIT_CODE;
#endif
    variable("BB_SAVE_TEST_MUTANT", "0");
    EXPECT(allow_drive_root);
    EXPECT(file_size(savedata) == -1);
    if (allow_drive_root) puts("save data mutant caught: allowdriveroot");
}

static void relative_roots_are_made_full_paths(void) {
    char folder[800], app0[800];
    snprintf(folder, sizeof(folder), "%s/cwd-relative", root);
    make_folder(folder);
    REQUIRE(!chdir(folder));
    EXPECT(run_child("default-root", 0, "relative") == CHILD_OK);
    expect_relative_save(folder, "user");
    snprintf(app0, sizeof(app0), "%s/app0", root);
    REQUIRE(!sys_file_configure(app0, "./relative-user"));
    sys_save_data_configure(TITLE);
    REQUIRE(!((InitializeCall)call("ZkZhskCPXFw"))(NULL));
    MountResult mounted;
    EXPECT(!mount(MODE_CREATE_OR_OPEN | MODE_READ_WRITE, &mounted));
    EXPECT(!write_details("param", &mounted.point, "relative"));
    EXPECT(!unmount(&mounted));
    expect_relative_save(folder, "relative-user");
    REQUIRE(!chdir(start_dir));
    configure(TITLE);
}

static void make_root(const char *argv0) {
    REQUIRE(getcwd(start_dir, sizeof(start_dir)) != NULL);
    forward_slashes(start_dir);
    REQUIRE(start_dir[0] == '/' || start_dir[1] == ':');
    char folder[800];
    snprintf(folder, sizeof(folder), "%s/out", start_dir);
    REQUIRE(is_folder(folder));
    snprintf(folder, sizeof(folder), "%s/out/test-tmp", start_dir);
    make_folder(folder);
    int written = snprintf(root, sizeof(root), "%s/save-data-%ld", folder, (long)getpid());
    REQUIRE(written > 0 && (size_t)written < sizeof(root));
    REQUIRE(!mkdir(root, 0755));
    sys_save_data_test_jail(root);
#ifdef _WIN32
    (void)argv0;
    REQUIRE(GetModuleFileNameA(NULL, self, sizeof(self)) > 0);
#else
    REQUIRE(realpath(argv0, self) != NULL);
#endif
}

int main(int argc, char **argv) {
    if (argc == 5 && !strcmp(argv[1], "child")) {
        snprintf(root, sizeof(root), "%s", argv[2]);
        return child(argv[3], argv[4]);
    }
    make_root(argv[0]);
    killed_param_writes_keep_the_old_file();
    killed_icon_writes_keep_the_old_file();
    unreadable_param_is_rebuilt();
    param_all_replaces_a_short_param();
    missing_meta_folder_is_created();
    failed_replace_keeps_the_old_file_and_the_save_goes_on();
    replace_decisions();
    replace_retries_and_their_latch();
    live_temporaries_are_kept();
    writable_mounts_are_counted();
    sound_flag_is_set_safely();
    mutants_are_caught();
    save_root_kinds();
    absolute_roots_are_used();
    relative_roots_are_made_full_paths();
    unusable_roots_create_nothing();
    if (failures) return 1;
    puts("save data tests passed");
    return 0;
}
