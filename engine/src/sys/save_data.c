// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "sys.h"
#include "runtime.h"
#include "../../gpu/bbgpu.h"
#include "save_replace.h"
#include "save_root.h"
#include "gpu/shim/lantern_diag_c.h"
#include "process_alive.h"
#include <dirent.h>
#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#ifndef _WIN32
#include <ftw.h>
#include <signal.h>
#endif

enum {
    SAVE_ERROR_PARAMETER = (int32_t)0x809F0000,
    SAVE_ERROR_NOT_INITIALIZED = (int32_t)0x809F0001,
    SAVE_ERROR_NOT_MOUNTED = (int32_t)0x809F0004,
    SAVE_ERROR_EXISTS = (int32_t)0x809F0007,
    SAVE_ERROR_NOT_FOUND = (int32_t)0x809F0008,
    SAVE_ERROR_INTERNAL = (int32_t)0x809F000B,
    SAVE_ERROR_MOUNT_FULL = (int32_t)0x809F000C,
    SAVE_ERROR_BUSY = (int32_t)0x809F000D,
    SAVE_ERROR_USER = (int32_t)0x809F0011,
    SAVE_ERROR_MEMORY_NOT_READY = (int32_t)0x809F0012,
};

enum { MOUNT_READ_ONLY = 1, MOUNT_CREATE = 4, MOUNT_CREATE_OR_OPEN = 32 };
enum { PARAM_ALL = 0, PARAM_TITLE = 1, PARAM_SUBTITLE = 2, PARAM_DETAIL = 3, PARAM_USER = 4 };
enum { SORT_NAME = 0, SORT_USER_PARAM = 1, SORT_TIME = 3 };

enum { SOUND_FLAG_OFFSET = 0x204E, SOUND_FLAG_ON = 1, SOUND_SAVE_SIZE = 0x40000 };
enum { EXIT_AFTER_OPEN = 1, EXIT_AFTER_WRITE = 2, EXIT_AFTER_CLOSE = 3, TEST_EXIT_CODE_BASE = 70 };
enum { MUTANT_ALWAYS_WRITE = 1, MUTANT_IGNORE_BACKUP = 2, MUTANT_ALLOW_DRIVE_ROOT = 3 };
enum { TEST_JAIL_EXIT_CODE = 79 };
enum { CENSUS_FILES = 32, COPY_CHUNK = 16384, LOAD_RETRIES = 3 };

#define MOUNT_SLOTS 16
#define PATH_SIZE 700
#define TEMPORARY_SUFFIX ".lantern-save-tmp"
#define SOUND_SAVE_FOLDER "SPRJ0005"
#define SOUND_SAVE_FILE "userdata0010"
#define SOUND_BACKUP_FILE "userdata0010.sound-flag-backup"
#define NS_PER_MS UINT64_C(1000000)
#define LOAD_RETRY_WAIT_NS (20 * NS_PER_MS)
#define REPLACE_RETRY_WAIT_NS (50 * NS_PER_MS)
#ifdef LANTERN_SAVE_TEST_HOOKS
#define SAVE_MUTANT(number) (options.mutant == (number))
#else
#define SAVE_MUTANT(number) 0
#endif

typedef enum { LOAD_READ, LOAD_ABSENT, LOAD_UNREADABLE } LoadResult;

typedef struct { char data[10]; char padding[6]; } TitleId;
typedef struct { char data[32]; } DirName;
typedef struct { char data[16]; } MountPoint;

typedef struct {
    char title[128], subtitle[128], detail[1024];
    uint32_t user_param;
    int32_t padding;
    int64_t mtime;
    uint8_t reserved[32];
} SaveParam;

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
    int32_t user, padding;
    const TitleId *title;
    const DirName *dir;
    uint32_t unused;
    uint8_t reserved[32];
    int32_t padding2;
} DeleteRequest;

typedef struct {
    int32_t user, padding;
    const TitleId *title;
    const DirName *dir;
    uint32_t key, order;
    uint8_t reserved[32];
} SearchRequest;

typedef struct {
    uint64_t blocks, free_blocks;
    uint8_t reserved[32];
} SearchInfo;

typedef struct {
    uint32_t hits;
    int32_t padding;
    DirName *names;
    uint32_t names_capacity, set_count;
    SaveParam *params;
    SearchInfo *infos;
    uint8_t reserved[12];
    int32_t padding2;
} SearchResult;

typedef struct {
    const void *buffer;
    uint64_t buffer_size, data_size;
    uint8_t reserved[32];
} SaveIcon;

_Static_assert(sizeof(SaveParam) == 1328, "SaveParam is 1328 bytes");
_Static_assert(sizeof(MountRequest) == 80, "MountRequest is 80 bytes");
_Static_assert(sizeof(MountResult) == 64, "MountResult is 64 bytes");
_Static_assert(sizeof(SearchRequest) == 64 && sizeof(SearchResult) == 56, "search layouts");

typedef struct {
    char name[32];
    SaveParam param;
} Found;

typedef struct {
    char name[32];
    int64_t size, mtime;
} FileStamp;

typedef struct {
    FileStamp files[CENSUS_FILES];
    size_t count;
    int overflow;
} FolderStamp;

typedef struct {
    int changed, created, removed;
    uint64_t bytes;
} FolderChange;

static HostMutex lock = HOST_MUTEX_INIT;
static int initialized;
static char title_id[16] = "UNKNOWN";
static struct {
    int stats, sound, exit_step, mutant;
} options = {0, 1, 0, 0};
#ifdef LANTERN_SAVE_TEST_HOOKS
int save_replace_mutant;
#endif
static struct {
    _Atomic int param_write, icon_write, param_unreadable, user_root, sound_size, replace;
} reported;
static struct {
    _Atomic int replace_retries_spent;
} degraded;
static struct {
    int used, writable;
    int backup_write;
    uint32_t mode;
    uint64_t mounted_ns;
    char folder[PATH_SIZE], meta[PATH_SIZE];
    FolderStamp before;
} mounts[MOUNT_SLOTS];
static char memory_file[720];
static uint64_t memory_size;
static _Atomic uint32_t writable_mounts;
static struct {
    _Atomic uint64_t mounts, memory_writes, memory_bytes, mounts_read_only, mounts_writable, mounts_created;
    _Atomic uint64_t longest_read_only_ns, longest_writable_ns, meta_files, meta_bytes;
    _Atomic uint64_t changed, created, removed, changed_bytes, listing_overflows;
    _Atomic uint64_t meta_failures, param_unreadable;
} census;

static void census_add(_Atomic uint64_t *counter, uint64_t amount) {
    atomic_fetch_add_explicit(counter, amount, memory_order_relaxed);
}

static uint64_t counted(_Atomic uint64_t *counter) {
    return atomic_load_explicit(counter, memory_order_relaxed);
}

static int first_report(_Atomic int *flag) {
    return !atomic_exchange_explicit(flag, 1, memory_order_relaxed);
}

static void read_options(void) {
    const char *sound = getenv("BB_SOUND_HACK");
    options.stats = lantern_switch_c(LANTERN_SW_ID_frame_stats) != 0;
    options.sound = !(sound && sound[0] == '0');
#ifdef LANTERN_SAVE_TEST_HOOKS
    const char *step = getenv("BB_SAVE_TEST_EXIT_STEP");
    const char *mutant = getenv("BB_SAVE_TEST_MUTANT");
    options.exit_step = step ? atoi(step) : 0;
    options.mutant = mutant ? atoi(mutant) : 0;
    save_replace_mutant = options.mutant;
#endif
}

#ifdef LANTERN_SAVE_TEST_HOOKS
static struct {
    unsigned long errors[16];
    int count, next, attempts;
} test_replace;

void sys_save_data_test_replace_errors(const unsigned long *errors, int count) {
    if (count < 0 || count > (int)(sizeof(test_replace.errors) / sizeof(test_replace.errors[0]))) count = 0;
    if (count) memcpy(test_replace.errors, errors, (size_t)count * sizeof(errors[0]));
    test_replace.count = count;
    test_replace.next = 0;
    test_replace.attempts = 0;
}

int sys_save_data_test_replace_attempts(void) {
    return test_replace.attempts;
}

static struct {
    char root[PATH_SIZE], jail[PATH_SIZE];
    int root_set;
} test_paths;

void sys_save_data_test_root(const char *root) {
    snprintf(test_paths.root, sizeof(test_paths.root), "%s", root);
    test_paths.root_set = 1;
}

void sys_save_data_test_jail(const char *jail) {
    snprintf(test_paths.jail, sizeof(test_paths.jail), "%s", jail);
}

static void test_exit(int code) {
    fflush(stdout);
#ifdef _WIN32
    TerminateProcess(GetCurrentProcess(), (UINT)code);
#endif
    _exit(code);
}

static int same_path_char(char a, char b) {
    if (save_root_separator(a) && save_root_separator(b)) return 1;
#ifdef _WIN32
    if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
    if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
#endif
    return a == b;
}

static int inside_jail(const char *path) {
    const char *jail = test_paths.jail;
    if (!*jail || !path) return 0;
    for (; *jail; ++jail, ++path)
        if (!*path || !same_path_char(*path, *jail)) return 0;
    return !*path || save_root_separator(*path) || save_root_separator(jail[-1]);
}
#endif

static void test_exit_at(int step) {
#ifdef LANTERN_SAVE_TEST_HOOKS
    if (options.exit_step == step) test_exit(TEST_EXIT_CODE_BASE + step);
#else
    (void)step;
#endif
}

#ifdef LANTERN_SAVE_TEST_HOOKS
void sys_save_data_test_jail_check(const char *path) {
    if (inside_jail(path)) return;
    printf("save test: %s is outside the test folder\n", path);
    test_exit(TEST_JAIL_EXIT_CODE);
}
#endif

static void test_jail_check(const char *path) {
#ifdef LANTERN_SAVE_TEST_HOOKS
    sys_save_data_test_jail_check(path);
#else
    (void)path;
#endif
}

static const char *configured_root(void) {
#ifdef LANTERN_SAVE_TEST_HOOKS
    if (test_paths.root_set) return test_paths.root;
#endif
    return sys_file_user_dir();
}

static int user_root(char *out, size_t size) {
    const char *root = configured_root();
    SaveRootKind kind = save_root_resolve(root, out, size);
    if (kind == SAVE_ROOT_DRIVE && SAVE_MUTANT(MUTANT_ALLOW_DRIVE_ROOT) &&
        save_root_fits(snprintf(out, size, "%s", root), size))
        kind = SAVE_ROOT_USABLE;
    if (kind == SAVE_ROOT_USABLE) return 0;
    if (first_report(&reported.user_root))
        printf("Runtime: saves are off: the saves folder is %s\n", save_root_problem(kind));
    return -1;
}

static const char *file_name(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static int temporary_path(const char *target, char *out, size_t size) {
    const char *name = file_name(target);
    int written = snprintf(out, size, "%.*s.%s.%ld%s", (int)(name - target), target, name, (long)getpid(),
                           TEMPORARY_SUFFIX);
    return written > 0 && (size_t)written < size ? 0 : -1;
}

static int make_folders(const char *path) {
    char buffer[PATH_SIZE];
    int written = snprintf(buffer, sizeof(buffer), "%s", path);
    if (written < 0 || (size_t)written >= sizeof(buffer)) return -1;
    test_jail_check(buffer);
    for (char *at = buffer + 1; *at; ++at)
        if (*at == '/') {
            *at = 0;
            mkdir(buffer, 0755);
            *at = '/';
        }
    return mkdir(buffer, 0755) && errno != EEXIST ? -1 : 0;
}

static int folder_of(const char *path, char *out, size_t size) {
    const char *name = file_name(path);
    int written = snprintf(out, size, "%.*s", (int)(name - path), path);
    if (written <= 0 || (size_t)written >= size) return -1;
    if (out[written - 1] == '/') out[written - 1] = 0;
    return 0;
}

static unsigned long replace_once(const char *temporary, const char *target) {
#ifdef LANTERN_SAVE_TEST_HOOKS
    ++test_replace.attempts;
    if (test_replace.next < test_replace.count && test_replace.errors[test_replace.next++])
        return test_replace.errors[test_replace.next - 1];
#endif
#ifdef _WIN32
    return MoveFileExA(temporary, target, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) ? 0 : GetLastError();
#else
    return rename(temporary, target) ? (unsigned long)errno : 0;
#endif
}

static int replace_file(const char *temporary, const char *target) {
    unsigned long error = replace_once(temporary, target);
    int spent = atomic_load_explicit(&degraded.replace_retries_spent, memory_order_relaxed);
    int retries = save_replace_retries(spent);
    for (int retry = 0; error && save_replace_may_pass_later(error) && retry < retries; ++retry) {
        host_sleep_ns(REPLACE_RETRY_WAIT_NS);
        error = replace_once(temporary, target);
    }
    int now_spent = save_replace_spent_after(spent, error, retries);
    if (now_spent != spent &&
        atomic_exchange_explicit(&degraded.replace_retries_spent, now_spent, memory_order_relaxed) != now_spent)
        printf(now_spent ? "Runtime: save file replaces kept failing; later replaces are tried once until one works\n"
                         : "Runtime: save file replaces work again; failed replaces are retried again\n");
    if (error && first_report(&reported.replace))
        printf("Runtime: save file %s cannot be replaced (error %lu)\n", file_name(target), error);
    return error ? -1 : 0;
}

typedef int (*FillTemporary)(FILE *file, const void *context);

typedef struct {
    const void *data;
    size_t size;
} Bytes;

static int write_bytes(FILE *file, const void *context) {
    const Bytes *bytes = context;
    return bytes->size && fwrite(bytes->data, 1, bytes->size, file) != bytes->size ? -1 : 0;
}

static int copy_stream(FILE *file, const void *context) {
    FILE *source = (FILE *)context;
    char chunk[COPY_CHUNK];
    for (size_t got; (got = fread(chunk, 1, sizeof(chunk), source));)
        if (fwrite(chunk, 1, got, file) != got) return -1;
    return ferror(source) ? -1 : 0;
}

static int write_replacing(const char *target, FillTemporary fill, const void *context) {
    char temporary[PATH_SIZE + 64], folder[PATH_SIZE + 64];
    if (temporary_path(target, temporary, sizeof(temporary)) || folder_of(target, folder, sizeof(folder)) ||
        make_folders(folder))
        return -1;
    FILE *file = fopen(temporary, "wb");
    if (!file) {
        printf("Runtime: save file %s cannot be written: %s\n", file_name(target), strerror(errno));
        return -1;
    }
    test_exit_at(EXIT_AFTER_OPEN);
    int filled = fill(file, context);
    test_exit_at(EXIT_AFTER_WRITE);
    int flushed = fflush(file) || fsync(fileno(file));
    int closed = fclose(file);
    test_exit_at(EXIT_AFTER_CLOSE);
    if (filled || flushed || closed) {
        printf("Runtime: save file %s was not written completely; the old one is kept\n", file_name(target));
        remove(temporary);
        return -1;
    }
    if (replace_file(temporary, target)) {
        remove(temporary);
        return -1;
    }
    return 0;
}

static int process_alive(long pid) {
    if (pid <= 0 || pid == (long)getpid()) return 0;
    return lantern_process_alive((unsigned long long)pid);
}

static long temporary_owner(const char *name, size_t length, size_t suffix) {
    const char *end = name + length - suffix, *start = end;
    while (start > name && start[-1] >= '0' && start[-1] <= '9') --start;
    if (start == end || start == name || start[-1] != '.') return 0;
    return strtol(start, NULL, 10);
}

static int is_stale_temporary(const char *name) {
    size_t length = strlen(name), suffix = strlen(TEMPORARY_SUFFIX);
    if (name[0] != '.' || length <= suffix + 1 || strcmp(name + length - suffix, TEMPORARY_SUFFIX)) return 0;
    return !process_alive(temporary_owner(name, length, suffix));
}

static size_t remove_stale_in(const char *folder) {
    DIR *dir = opendir(folder);
    size_t removed = 0;
    for (struct dirent *entry; dir && (entry = readdir(dir));) {
        if (!is_stale_temporary(entry->d_name)) continue;
        char path[PATH_SIZE + 64];
        snprintf(path, sizeof(path), "%s/%s", folder, entry->d_name);
        if (!remove(path)) ++removed;
        else printf("Runtime: unfinished save file %s cannot be removed: %s\n", entry->d_name, strerror(errno));
    }
    if (dir) closedir(dir);
    return removed;
}

static int is_meta_folder(const char *name, size_t length) {
    return length > 8 && !strcmp(name + length - 8, ".sce_sys");
}

static size_t remove_stale_for_user(const char *user_folder) {
    char path[PATH_SIZE];
    snprintf(path, sizeof(path), "%s/%s.memory", user_folder, title_id);
    size_t removed = remove_stale_in(path);
    snprintf(path, sizeof(path), "%s/%s", user_folder, title_id);
    DIR *dir = opendir(path);
    for (struct dirent *entry; dir && (entry = readdir(dir));) {
        if (!is_meta_folder(entry->d_name, strlen(entry->d_name))) continue;
        char meta[PATH_SIZE + 64];
        snprintf(meta, sizeof(meta), "%s/%s", path, entry->d_name);
        removed += remove_stale_in(meta);
    }
    if (dir) closedir(dir);
    return removed;
}

static void remove_stale_temporaries(const char *users) {
    DIR *dir = opendir(users);
    size_t removed = 0;
    for (struct dirent *user; dir && (user = readdir(dir));) {
        if (user->d_name[0] == '.') continue;
        char path[PATH_SIZE];
        snprintf(path, sizeof(path), "%s/%s", users, user->d_name);
        removed += remove_stale_for_user(path);
    }
    if (dir) closedir(dir);
    if (removed) printf("Runtime: %zu unfinished save file(s) from an earlier run removed\n", removed);
}

static int is_bloodborne(void) {
    static const char *const titles[] = {"CUSA00207", "CUSA00208", "CUSA00299", "CUSA00900", "CUSA01363",
                                         "CUSA03014", "CUSA03023", "CUSA03173", "CUSA03179"};
    for (size_t i = 0; i < sizeof(titles) / sizeof(*titles); ++i)
        if (!strcmp(title_id, titles[i])) return 1;
    return 0;
}

static int back_up_once(FILE *source, const char *backup) {
    struct stat info;
    if (!stat(backup, &info)) return 0;
    if (fseek(source, 0, SEEK_SET)) return -1;
    return write_replacing(backup, copy_stream, source);
}

static int is_sound_save(FILE *file) {
    struct stat info;
    return !fstat(fileno(file), &info) && S_ISREG(info.st_mode) && info.st_size == SOUND_SAVE_SIZE;
}

static int read_sound_flag(const char *path, const char *user) {
    FILE *file = fopen(path, "rb");
    if (!file) return EOF;
    int sized = is_sound_save(file);
    int was = !sized || fseek(file, SOUND_FLAG_OFFSET, SEEK_SET) ? EOF : fgetc(file);
    if (fclose(file)) return EOF;
    if (!sized && first_report(&reported.sound_size))
        printf("Runtime: Bloodborne sound flag left alone for user %s: %s is not %d bytes\n", user, SOUND_SAVE_FILE,
               (int)SOUND_SAVE_SIZE);
    return was;
}

static const char *sound_failure(int stage) {
    static const char *const text[] = {"the backup could not be made; the save was not changed",
                                       "the byte could not be written; the save was not changed",
                                       "the change may not have reached the disk"};
    return text[stage];
}

static void set_sound_flag(const char *users, const char *user) {
    char path[PATH_SIZE + 64], backup[PATH_SIZE + 64];
    snprintf(path, sizeof(path), "%s/%s/%s/" SOUND_SAVE_FOLDER "/" SOUND_SAVE_FILE, users, user, title_id);
    snprintf(backup, sizeof(backup), "%s/%s/%s/" SOUND_SAVE_FOLDER ".sce_sys/" SOUND_BACKUP_FILE, users, user, title_id);
    int was = read_sound_flag(path, user);
    if (was == EOF || (was == SOUND_FLAG_ON && !SAVE_MUTANT(MUTANT_ALWAYS_WRITE))) return;
    FILE *file = fopen(path, "r+b");
    if (!file) {
        printf("Runtime: Bloodborne sound flag for user %s not set: the save cannot be opened for writing\n", user);
        return;
    }
    int stage = -1;
    if (back_up_once(file, backup) && !SAVE_MUTANT(MUTANT_IGNORE_BACKUP)) stage = 0;
    else if (fseek(file, SOUND_FLAG_OFFSET, SEEK_SET) || fputc(SOUND_FLAG_ON, file) == EOF) stage = 1;
    else if (fflush(file)) stage = 2;
    if (fclose(file) && stage != 0 && stage != 1) stage = 2;
    if (stage < 0) printf("Runtime: Bloodborne sound flag set for user %s (was %d)\n", user, was);
    else printf("Runtime: Bloodborne sound flag for user %s not set safely: %s\n", user, sound_failure(stage));
}

static void turn_sound_on_in_saves(const char *users) {
    if (!options.sound || !is_bloodborne()) return;
    DIR *dir = opendir(users);
    for (struct dirent *user; dir && (user = readdir(dir));)
        if (user->d_name[0] != '.') set_sound_flag(users, user->d_name);
    if (dir) closedir(dir);
}

void sys_save_data_configure(const char *title) {
    if (title && *title) snprintf(title_id, sizeof(title_id), "%s", title);
    read_options();
    char root[PATH_SIZE], users[PATH_SIZE + 16];
    if (user_root(root, sizeof(root))) return;
    snprintf(users, sizeof(users), "%s/savedata", root);
    host_lock(&lock);
    if (!bbgpu_save_backups_write(1)) {
        remove_stale_temporaries(users);
        turn_sound_on_in_saves(users);
        bbgpu_save_backups_write(0);
    }
    host_unlock(&lock);
}

uint32_t sys_save_data_writable_mounts(void) {
    return atomic_load_explicit(&writable_mounts, memory_order_acquire);
}

static unsigned long long in_ms(uint64_t ns) {
    return (unsigned long long)(ns / NS_PER_MS);
}

static void report_failures(void);

void sys_save_data_report(void) {
    printf("Runtime: save data mounts=%llu, memory writes=%llu\n", (unsigned long long)counted(&census.mounts),
           (unsigned long long)counted(&census.memory_writes));
    report_failures();
    if (!options.stats) return;
    printf("Save census: mounts read-only=%llu writable=%llu created=%llu, still writable=%u, longest hold "
           "read-only=%llu ms writable=%llu ms\n",
           (unsigned long long)counted(&census.mounts_read_only), (unsigned long long)counted(&census.mounts_writable),
           (unsigned long long)counted(&census.mounts_created), sys_save_data_writable_mounts(),
           in_ms(counted(&census.longest_read_only_ns)), in_ms(counted(&census.longest_writable_ns)));
    printf("Save census: param/icon files=%llu bytes=%llu, memory writes=%llu bytes=%llu, at unmount files "
           "changed=%llu created=%llu removed=%llu bytes=%llu, folders over %d files=%llu\n",
           (unsigned long long)counted(&census.meta_files), (unsigned long long)counted(&census.meta_bytes),
           (unsigned long long)counted(&census.memory_writes), (unsigned long long)counted(&census.memory_bytes),
           (unsigned long long)counted(&census.changed), (unsigned long long)counted(&census.created),
           (unsigned long long)counted(&census.removed), (unsigned long long)counted(&census.changed_bytes),
           (int)CENSUS_FILES, (unsigned long long)counted(&census.listing_overflows));
}

static void report_failures(void) {
    uint64_t failures = counted(&census.meta_failures), unreadable = counted(&census.param_unreadable);
    if (failures || unreadable)
        printf("Runtime: save details writes failed=%llu, unreadable details files=%llu\n",
               (unsigned long long)failures, (unsigned long long)unreadable);
}

static int title_folder(int32_t user, const char *title, char *out, size_t size) {
    char root[PATH_SIZE];
    if (user_root(root, sizeof(root))) return -1;
    int written = snprintf(out, size, "%s/savedata/%d/%s", root, user, title && *title ? title : title_id);
    return save_root_fits(written, size) ? 0 : -1;
}

static int safe_name(const char *name, size_t limit) {
    size_t n = strnlen(name, limit);
    if (!n || n == limit) return 0;
    for (size_t i = 0; i < n; ++i)
        if (name[i] == '/' || name[i] == '\\' || (name[i] == '.' && (i == 0 || name[i - 1] == '.'))) return 0;
    return 1;
}

static int store_meta_file(const char *meta, const char *name, const void *data, size_t size) {
    char path[PATH_SIZE + 32];
    int written = snprintf(path, sizeof(path), "%s/%s", meta, name);
    const Bytes bytes = {data, size};
    if (written < 0 || (size_t)written >= sizeof(path) || write_replacing(path, write_bytes, &bytes)) {
        census_add(&census.meta_failures, 1);
        return -1;
    }
    census_add(&census.meta_files, 1);
    census_add(&census.meta_bytes, size);
    return 0;
}

static int store_param(const char *meta, const SaveParam *param) {
    SaveParam stamped = *param;
    stamped.mtime = time(NULL);
    return store_meta_file(meta, "param.bin", &stamped, sizeof(stamped));
}

static LoadResult load_param_once(const char *path, SaveParam *param) {
    memset(param, 0, sizeof(*param));
    FILE *file = fopen(path, "rb");
    if (!file) return errno == ENOENT ? LOAD_ABSENT : LOAD_UNREADABLE;
    size_t read = fread(param, sizeof(*param), 1, file);
    int closed = !fclose(file);
    struct stat info;
    if (!stat(path, &info)) param->mtime = info.st_mtime;
    return read == 1 && closed ? LOAD_READ : LOAD_UNREADABLE;
}

static LoadResult load_param(const char *meta, SaveParam *param) {
    char path[PATH_SIZE + 32];
    snprintf(path, sizeof(path), "%s/param.bin", meta);
    LoadResult result = load_param_once(path, param);
    for (int retry = 0; result == LOAD_UNREADABLE && retry < LOAD_RETRIES; ++retry) {
        host_sleep_ns(LOAD_RETRY_WAIT_NS);
        result = load_param_once(path, param);
    }
    if (result == LOAD_UNREADABLE) {
        census_add(&census.param_unreadable, 1);
        if (first_report(&reported.param_unreadable))
            printf("Runtime: a save details file (param.bin) is unreadable; it is rebuilt from the game's next details\n");
    }
    return result;
}

static int save_paths(int32_t user, const char *title, const char *dir, char *base, char *folder, char *meta) {
    if (title_folder(user, title, base, 600)) return -1;
    snprintf(folder, 640, "%s/%s", base, dir);
    snprintf(meta, 680, "%s/%s.sce_sys", base, dir);
    return 0;
}

static int mount_slot(const MountPoint *point) {
    if (!point || strncmp(point->data, "/savedata", 9)) return -1;
    int slot = atoi(point->data + 9);
    return slot >= 0 && slot < MOUNT_SLOTS && mounts[slot].used ? slot : -1;
}

static void stamp_folder(const char *folder, FolderStamp *out) {
    out->count = 0;
    out->overflow = 0;
    DIR *dir = opendir(folder);
    for (struct dirent *entry; dir && (entry = readdir(dir));) {
        if (entry->d_name[0] == '.') continue;
        if (out->count == CENSUS_FILES) {
            out->overflow = 1;
            continue;
        }
        char path[PATH_SIZE + 64];
        snprintf(path, sizeof(path), "%s/%s", folder, entry->d_name);
        struct stat info;
        if (stat(path, &info) || !S_ISREG(info.st_mode)) continue;
        FileStamp *stamp = &out->files[out->count++];
        snprintf(stamp->name, sizeof(stamp->name), "%s", entry->d_name);
        stamp->size = (int64_t)info.st_size;
        stamp->mtime = (int64_t)info.st_mtime;
    }
    if (dir) closedir(dir);
}

static const FileStamp *find_stamp(const FolderStamp *stamps, const char *name) {
    for (size_t i = 0; i < stamps->count; ++i)
        if (!strcmp(stamps->files[i].name, name)) return &stamps->files[i];
    return NULL;
}

static FolderChange compare_stamps(const FolderStamp *before, const FolderStamp *after) {
    FolderChange change = {0};
    for (size_t i = 0; i < after->count; ++i) {
        const FileStamp *now = &after->files[i], *was = find_stamp(before, now->name);
        if (!was) ++change.created;
        else if (was->size != now->size || was->mtime != now->mtime) ++change.changed;
        else continue;
        change.bytes += (uint64_t)now->size;
    }
    for (size_t i = 0; i < before->count; ++i)
        if (!find_stamp(after, before->files[i].name)) ++change.removed;
    return change;
}

static void note_longest(_Atomic uint64_t *longest, uint64_t held) {
    if (held > counted(longest)) atomic_store_explicit(longest, held, memory_order_relaxed);
}

static void claim_slot(int slot, const char *folder, const char *meta, uint32_t mode, int created) {
    mounts[slot].used = 1;
    mounts[slot].mode = mode;
    mounts[slot].writable = !(mode & MOUNT_READ_ONLY);
    mounts[slot].mounted_ns = host_monotonic_ns();
    snprintf(mounts[slot].folder, sizeof(mounts[slot].folder), "%s", folder);
    snprintf(mounts[slot].meta, sizeof(mounts[slot].meta), "%s", meta);
    if (mounts[slot].writable) atomic_fetch_add_explicit(&writable_mounts, 1, memory_order_release);
    census_add(&census.mounts, 1);
    census_add(mounts[slot].writable ? &census.mounts_writable : &census.mounts_read_only, 1);
    if (created) census_add(&census.mounts_created, 1);
    if (options.stats) stamp_folder(folder, &mounts[slot].before);
}

static uint64_t release_slot(int slot, FolderChange *change) {
    uint64_t held = host_monotonic_ns() - mounts[slot].mounted_ns;
    note_longest(mounts[slot].writable ? &census.longest_writable_ns : &census.longest_read_only_ns, held);
    if (options.stats) {
        FolderStamp after;
        stamp_folder(mounts[slot].folder, &after);
        *change = compare_stamps(&mounts[slot].before, &after);
        census_add(&census.changed, (uint64_t)change->changed);
        census_add(&census.created, (uint64_t)change->created);
        census_add(&census.removed, (uint64_t)change->removed);
        census_add(&census.changed_bytes, change->bytes);
        if (mounts[slot].before.overflow || after.overflow) census_add(&census.listing_overflows, 1);
    }
    mounts[slot].used = 0;
    if (mounts[slot].writable) atomic_fetch_sub_explicit(&writable_mounts, 1, memory_order_release);
    return held;
}

static SYS_ABI int32_t save_initialize(const void *param) {
    (void)param;
    initialized = 1;
    return 0;
}

static SYS_ABI int32_t save_terminate(void) {
    if (!initialized) return SAVE_ERROR_NOT_INITIALIZED;
    initialized = 0;
    return 0;
}

static SYS_ABI int32_t save_mount(const MountRequest *request, MountResult *result) {
    if (!request) return SAVE_ERROR_PARAMETER;
    if (!initialized) return SAVE_ERROR_NOT_INITIALIZED;
    if (request->user < 0) return SAVE_ERROR_USER;
    const DirName *dir = request->dir;
    if (!dir || !result || !safe_name(dir->data, sizeof(dir->data))) return SAVE_ERROR_PARAMETER;
    char base[600], folder[640], meta[680];
    if (save_paths(request->user, request->title ? request->title->data : NULL, dir->data, base, folder, meta))
        return SAVE_ERROR_INTERNAL;
    struct stat info;
    int exists = !stat(folder, &info) && S_ISDIR(info.st_mode);
    if ((request->mode & MOUNT_CREATE) && exists) return SAVE_ERROR_EXISTS;
    if (!(request->mode & (MOUNT_CREATE | MOUNT_CREATE_OR_OPEN)) && !exists) return SAVE_ERROR_NOT_FOUND;
    host_lock(&lock);
    int slot = -1;
    for (int i = 0; i < MOUNT_SLOTS; ++i) {
        if (mounts[i].used && !strcmp(mounts[i].folder, folder)) {
            host_unlock(&lock);
            return SAVE_ERROR_BUSY;
        }
        if (!mounts[i].used && slot < 0) slot = i;
    }
    if (slot < 0) {
        host_unlock(&lock);
        return SAVE_ERROR_MOUNT_FULL;
    }
    const int backup_write = !(request->mode & MOUNT_READ_ONLY) || !exists;
    if (backup_write && bbgpu_save_backups_write(1)) {
        host_unlock(&lock);
        return SAVE_ERROR_INTERNAL;
    }
    if (!exists && (make_folders(folder) || make_folders(meta))) {
        int error = errno;
        if (backup_write) bbgpu_save_backups_write(0);
        host_unlock(&lock);
        printf("Runtime: save data '%s' cannot be created in %s: %s\n", dir->data, base, strerror(error));
        return SAVE_ERROR_INTERNAL;
    }
    SaveParam empty = {0};
    int details_lost = !exists && store_param(meta, &empty);
    memset(result, 0, sizeof(*result));
    snprintf(result->point.data, sizeof(result->point.data), "/savedata%d", slot & 15);
    result->status = exists ? 0 : 1;
    if (sys_file_mount(result->point.data, folder)) {
        if (backup_write) bbgpu_save_backups_write(0);
        host_unlock(&lock);
        printf("Runtime: save data '%s' cannot be mounted: the file mount table is full\n", dir->data);
        return SAVE_ERROR_INTERNAL;
    }
    claim_slot(slot, folder, meta, request->mode, !exists);
    mounts[slot].backup_write = backup_write;
    host_unlock(&lock);
    if (details_lost) printf("Runtime: save data '%s' was created without its details file\n", dir->data);
    printf("Runtime: save data '%s' mounted at %s (%s%s)\n", dir->data, result->point.data,
           exists ? "existing" : "created", (request->mode & MOUNT_READ_ONLY) ? ", read-only" : "");
    return 0;
}

static SYS_ABI int32_t save_umount(const MountPoint *point) {
    if (!initialized) return SAVE_ERROR_NOT_INITIALIZED;
    host_lock(&lock);
    int slot = mount_slot(point);
    FolderChange change = {0};
    uint64_t held = 0;
    int writable = 0;
    if (slot >= 0) {
        sys_file_unmount(point->data);
        writable = mounts[slot].writable;
        held = release_slot(slot, &change);
        if (mounts[slot].backup_write) bbgpu_save_backups_write(0);
    }
    host_unlock(&lock);
    if (slot >= 0 && options.stats)
        printf("Save census: unmount %s %s held=%llu ms at=%llu ms files changed=%d created=%d removed=%d "
               "bytes=%llu\n",
               point->data, writable ? "writable" : "read-only", in_ms(held), in_ms(host_monotonic_ns()),
               change.changed, change.created, change.removed, (unsigned long long)change.bytes);
    return slot >= 0 ? 0 : SAVE_ERROR_NOT_FOUND;
}

static int set_text(char *field, size_t capacity, const void *text, uint64_t size) {
    snprintf(field, capacity, "%.*s", (int)size, (const char *)text);
    return 1;
}

static int apply_param(SaveParam *param, uint32_t type, const void *buffer, uint64_t size) {
    switch (type) {
    case PARAM_ALL:
        if (size < sizeof(*param)) return 0;
        memcpy(param, buffer, sizeof(*param));
        return 1;
    case PARAM_TITLE: return set_text(param->title, sizeof(param->title), buffer, size);
    case PARAM_SUBTITLE: return set_text(param->subtitle, sizeof(param->subtitle), buffer, size);
    case PARAM_DETAIL: return set_text(param->detail, sizeof(param->detail), buffer, size);
    case PARAM_USER:
        if (size < 4) return 0;
        memcpy(&param->user_param, buffer, 4);
        return 1;
    }
    return 0;
}

static SYS_ABI int32_t save_set_param(const MountPoint *point, uint32_t type, const void *buffer, uint64_t size) {
    if (!initialized) return SAVE_ERROR_NOT_INITIALIZED;
    if (!buffer) return SAVE_ERROR_PARAMETER;
    host_lock(&lock);
    int slot = mount_slot(point);
    int32_t result = SAVE_ERROR_NOT_MOUNTED;
    int stored = 1;
    if (slot >= 0) {
        SaveParam param = {0};
        if (type != PARAM_ALL) (void)load_param(mounts[slot].meta, &param);
        result = apply_param(&param, type, buffer, size) ? 0 : SAVE_ERROR_PARAMETER;
        if (!result) {
            if (bbgpu_save_backups_write(1)) stored = 0;
            else {
                stored = !store_param(mounts[slot].meta, &param);
                bbgpu_save_backups_write(0);
            }
        }
    }
    host_unlock(&lock);
    if (!stored && first_report(&reported.param_write))
        printf("Runtime: save details (param.bin) could not be written; the old details are kept and the game's save goes on\n");
    return result;
}

static SYS_ABI int32_t save_icon(const MountPoint *point, const SaveIcon *icon) {
    if (!initialized) return SAVE_ERROR_NOT_INITIALIZED;
    if (!icon || !icon->buffer) return SAVE_ERROR_PARAMETER;
    host_lock(&lock);
    int slot = mount_slot(point);
    int32_t result = SAVE_ERROR_NOT_MOUNTED;
    int stored = 1;
    if (slot >= 0) {
        result = 0;
        if (bbgpu_save_backups_write(1)) stored = 0;
        else {
            stored = !store_meta_file(mounts[slot].meta, "icon0.png", icon->buffer, (size_t)icon->data_size);
            bbgpu_save_backups_write(0);
        }
    }
    host_unlock(&lock);
    if (!stored && first_report(&reported.icon_write))
        printf("Runtime: save icon (icon0.png) could not be written; the old icon is kept and the game's save goes on\n");
    return result;
}

#ifndef _WIN32
static int remove_one(const char *path, const struct stat *info, int kind, struct FTW *walk) {
    (void)info;
    (void)kind;
    (void)walk;
    return remove(path);
}
#endif

static void remove_folder(const char *path) {
#ifdef _WIN32
    runtime_win_remove_tree(path);
#else
    nftw(path, remove_one, 16, FTW_DEPTH | FTW_PHYS);
#endif
}

static SYS_ABI int32_t save_delete(const DeleteRequest *request) {
    if (!initialized) return SAVE_ERROR_NOT_INITIALIZED;
    if (!request || !request->dir || !safe_name(request->dir->data, sizeof(request->dir->data)))
        return SAVE_ERROR_PARAMETER;
    char base[600], folder[640], meta[680];
    if (save_paths(request->user, request->title ? request->title->data : NULL, request->dir->data, base, folder, meta))
        return SAVE_ERROR_INTERNAL;
    host_lock(&lock);
    struct stat info;
    if (stat(folder, &info)) { host_unlock(&lock); return SAVE_ERROR_NOT_FOUND; }
    if (bbgpu_save_backups_write(1)) {
        host_unlock(&lock);
        return SAVE_ERROR_INTERNAL;
    }
    remove_folder(folder);
    remove_folder(meta);
    bbgpu_save_backups_write(0);
    host_unlock(&lock);
    printf("Runtime: save data '%s' deleted\n", request->dir->data);
    return 0;
}

static int matches(const char *text, const char *pattern) {
    if (!*pattern) return !*text;
    if (*pattern == '%') {
        for (;; ++text) {
            if (matches(text, pattern + 1)) return 1;
            if (!*text) return 0;
        }
    }
    return *text && (*pattern == '_' || *pattern == *text) && matches(text + 1, pattern + 1);
}

static uint32_t search_key, search_descending;

static int compare_found(const void *a, const void *b) {
    const Found *x = a, *y = b;
    int c;
    if (search_key == SORT_USER_PARAM) c = (x->param.user_param > y->param.user_param) - (x->param.user_param < y->param.user_param);
    else if (search_key == SORT_TIME) c = (x->param.mtime > y->param.mtime) - (x->param.mtime < y->param.mtime);
    else c = strcmp(x->name, y->name);
    return search_descending ? -c : c;
}

static SYS_ABI int32_t save_dir_name_search(const SearchRequest *request, SearchResult *result) {
    if (!initialized) return SAVE_ERROR_NOT_INITIALIZED;
    if (!request || !result) return SAVE_ERROR_PARAMETER;
    char base[600];
    if (title_folder(request->user, request->title ? request->title->data : NULL, base, sizeof(base)))
        return SAVE_ERROR_INTERNAL;
    Found *found = NULL;
    size_t count = 0, capacity = 0;
    DIR *dir = opendir(base);
    for (struct dirent *entry; dir && (entry = readdir(dir));) {
        size_t length = strlen(entry->d_name);
        if (entry->d_name[0] == '.' || length >= 32 || is_meta_folder(entry->d_name, length)) continue;
        if (request->dir && request->dir->data[0] && !matches(entry->d_name, request->dir->data)) continue;
        if (count == capacity) {
            size_t grown_capacity = capacity ? capacity * 2 : 16;
            Found *grown = realloc(found, grown_capacity * sizeof(*found));
            if (!grown) break;
            found = grown;
            capacity = grown_capacity;
        }
        snprintf(found[count].name, sizeof(found[count].name), "%s", entry->d_name);
        char meta[680];
        snprintf(meta, sizeof(meta), "%s/%s.sce_sys", base, entry->d_name);
        (void)load_param(meta, &found[count].param);
        ++count;
    }
    if (dir) closedir(dir);
    search_key = request->key;
    search_descending = request->order;
    if (count) qsort(found, count, sizeof(*found), compare_found);
    result->hits = (uint32_t)count;
    uint32_t filled = count < result->names_capacity ? (uint32_t)count : result->names_capacity;
    for (uint32_t i = 0; i < filled; ++i) {
        if (result->names) memcpy(result->names[i].data, found[i].name, sizeof(result->names[i].data));
        if (result->params) result->params[i] = found[i].param;
        if (result->infos) {
            memset(&result->infos[i], 0, sizeof(SearchInfo));
            result->infos[i].blocks = 32768;
            result->infos[i].free_blocks = 16384;
        }
    }
    result->set_count = filled;
    free(found);
    return 0;
}

static SYS_ABI int32_t save_setup_memory(int32_t user, uint64_t size, const SaveParam *param) {
    if (!initialized) return SAVE_ERROR_NOT_INITIALIZED;
    if (!size) return SAVE_ERROR_PARAMETER;
    char base[600], folder[640];
    if (title_folder(user, NULL, base, sizeof(base))) return SAVE_ERROR_INTERNAL;
    snprintf(folder, sizeof(folder), "%s.memory", base);
    host_lock(&lock);
    if (bbgpu_save_backups_write(1)) {
        host_unlock(&lock);
        return SAVE_ERROR_INTERNAL;
    }
    if (make_folders(folder)) {
        printf("Runtime: save data memory cannot be created in %s: %s\n", folder, strerror(errno));
        bbgpu_save_backups_write(0);
        host_unlock(&lock);
        return SAVE_ERROR_INTERNAL;
    }
    snprintf(memory_file, sizeof(memory_file), "%s/memory.dat", folder);
    FILE *file = fopen(memory_file, "r+b");
    if (!file) file = fopen(memory_file, "w+b");
    int32_t result = SAVE_ERROR_INTERNAL;
    if (file) {
        fseek(file, 0, SEEK_END);
        if ((uint64_t)ftell(file) >= size || !ftruncate(fileno(file), (off_t)size)) {
            memory_size = size;
            result = 0;
        }
        fclose(file);
    }
    int error = errno;
    int details_lost = !result && param && store_param(folder, param);
    bbgpu_save_backups_write(0);
    host_unlock(&lock);
    if (details_lost) printf("Runtime: save data memory details were not written; the memory itself is ready\n");
    if (!result) printf("Runtime: save data memory ready (%llu bytes)\n", (unsigned long long)size);
    else printf("Runtime: save data memory cannot be written in %s: %s\n", folder, strerror(error));
    return result;
}

static int32_t memory_transfer(void *buffer, uint64_t size, int64_t offset, int writing) {
    if (!initialized) return SAVE_ERROR_NOT_INITIALIZED;
    if (!buffer || offset < 0) return SAVE_ERROR_PARAMETER;
    host_lock(&lock);
    if (writing && bbgpu_save_backups_write(1)) {
        host_unlock(&lock);
        return SAVE_ERROR_INTERNAL;
    }
    int32_t result = SAVE_ERROR_INTERNAL;
    if (!memory_size) {
        result = SAVE_ERROR_MEMORY_NOT_READY;
    } else if ((uint64_t)offset + size > memory_size) {
        result = SAVE_ERROR_PARAMETER;
    } else {
        FILE *file = fopen(memory_file, "r+b");
        if (file && !fseek(file, offset, SEEK_SET) &&
            (writing ? fwrite(buffer, 1, size, file) : fread(buffer, 1, size, file)) == size)
            result = 0;
        if (file && fclose(file) && writing) result = SAVE_ERROR_INTERNAL;
        if (!result && writing) {
            census_add(&census.memory_writes, 1);
            census_add(&census.memory_bytes, size);
        }
    }
    if (writing) bbgpu_save_backups_write(0);
    host_unlock(&lock);
    return result;
}

static SYS_ABI int32_t save_get_memory(int32_t user, void *buffer, uint64_t size, int64_t offset) {
    (void)user;
    return memory_transfer(buffer, size, offset, 0);
}

static SYS_ABI int32_t save_set_memory(int32_t user, void *buffer, uint64_t size, int64_t offset) {
    (void)user;
    return memory_transfer(buffer, size, offset, 1);
}

static const SysExport exports[] = {
    {"S1GkePI17zQ", "sceSaveDataDelete", save_delete},
    {"dyIhnXq-0SM", "sceSaveDataDirNameSearch", save_dir_name_search},
    {"7Bt5pBC-Aco", "sceSaveDataGetSaveDataMemory", save_get_memory},
    {"ZkZhskCPXFw", "sceSaveDataInitialize", save_initialize},
    {"32HQAQdwM2o", "sceSaveDataMount", save_mount},
    {"c88Yy54Mx0w", "sceSaveDataSaveIcon", save_icon},
    {"85zul--eGXs", "sceSaveDataSetParam", save_set_param},
    {"h3YURzXGSVQ", "sceSaveDataSetSaveDataMemory", save_set_memory},
    {"v7AAAMo0Lz4", "sceSaveDataSetupSaveDataMemory", save_setup_memory},
    {"yKDy8S5yLA0", "sceSaveDataTerminate", save_terminate},
    {"BMR4F-Uek3E", "sceSaveDataUmount", save_umount},
};

SYS_LIBRARY(sys_save_data, "libSceSaveData", exports);
