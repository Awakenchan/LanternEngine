// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "sys.h"
#include "runtime.h"
#include "../../gpu/bbgpu.h"
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#ifndef _WIN32
#include <ftw.h>
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

#define MOUNT_SLOTS 16
#define PATH_SIZE 700

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

static HostMutex lock = HOST_MUTEX_INIT;
static int initialized;
static char title_id[16] = "UNKNOWN";
static struct {
    int used;
    int backup_write;
    char folder[PATH_SIZE], meta[PATH_SIZE];
} mounts[MOUNT_SLOTS];
static char memory_file[720];
static uint64_t memory_size;
static size_t mount_count, memory_writes;

static int is_bloodborne(void) {
    static const char *const titles[] = {"CUSA00207", "CUSA00208", "CUSA00299", "CUSA00900", "CUSA01363",
                                         "CUSA03014", "CUSA03023", "CUSA03173", "CUSA03179"};
    for (size_t i = 0; i < sizeof(titles) / sizeof(*titles); ++i)
        if (!strcmp(title_id, titles[i])) return 1;
    return 0;
}

static void turn_sound_on_in_saves(void) {
    const char *setting = getenv("BB_SOUND_HACK");
    if ((setting && setting[0] == '0') || !is_bloodborne()) return;
    char users[PATH_SIZE];
    snprintf(users, sizeof(users), "%s/savedata", sys_file_user_dir());
    DIR *dir = opendir(users);
    if (!dir) return;
    for (struct dirent *user; (user = readdir(dir));) {
        if (user->d_name[0] == '.') continue;
        char path[1100];
        snprintf(path, sizeof(path), "%s/%s/%s/SPRJ0005/userdata0010", users, user->d_name, title_id);
        FILE *file = fopen(path, "r+b");
        if (!file) continue;
        int was = fseek(file, 0x204E, SEEK_SET) ? EOF : fgetc(file);
        if (was != EOF && was != 1 && !fseek(file, 0x204E, SEEK_SET)) {
            fputc(1, file);
            printf("Runtime: Bloodborne sound flag set in %s (was %d)\n", path, was);
        }
        fclose(file);
    }
    closedir(dir);
}

void sys_save_data_configure(const char *title) {
    if (title && *title) snprintf(title_id, sizeof(title_id), "%s", title);
    turn_sound_on_in_saves();
}

void sys_save_data_report(void) {
    printf("Runtime: save data mounts=%zu, memory writes=%zu\n", mount_count, memory_writes);
}

static int make_folders(const char *path) {
    char buffer[PATH_SIZE];
    snprintf(buffer, sizeof(buffer), "%s", path);
    for (char *at = buffer + 1; *at; ++at)
        if (*at == '/') {
            *at = 0;
            mkdir(buffer, 0755);
            *at = '/';
        }
    return mkdir(buffer, 0755) && errno != EEXIST ? -1 : 0;
}

static void title_folder(int32_t user, const char *title, char *out, size_t size) {
    snprintf(out, size, "%s/savedata/%d/%s", sys_file_user_dir(), user, title && *title ? title : title_id);
}

static int safe_name(const char *name, size_t limit) {
    size_t n = strnlen(name, limit);
    if (!n || n == limit) return 0;
    for (size_t i = 0; i < n; ++i)
        if (name[i] == '/' || name[i] == '\\' || (name[i] == '.' && (i == 0 || name[i - 1] == '.'))) return 0;
    return 1;
}

static void store_param(const char *meta, const SaveParam *param) {
    char path[PATH_SIZE];
    snprintf(path, sizeof(path), "%s/param.bin", meta);
    FILE *file = fopen(path, "wb");
    if (!file) return;
    SaveParam stamped = *param;
    stamped.mtime = time(NULL);
    fwrite(&stamped, sizeof(stamped), 1, file);
    fclose(file);
}

static int load_param(const char *meta, SaveParam *param) {
    char path[PATH_SIZE];
    snprintf(path, sizeof(path), "%s/param.bin", meta);
    memset(param, 0, sizeof(*param));
    FILE *file = fopen(path, "rb");
    if (!file) return -1;
    size_t read = fread(param, sizeof(*param), 1, file);
    fclose(file);
    struct stat info;
    if (!stat(path, &info)) param->mtime = info.st_mtime;
    return read == 1 ? 0 : -1;
}

static void save_paths(int32_t user, const char *title, const char *dir, char *base, char *folder, char *meta) {
    title_folder(user, title, base, 600);
    snprintf(folder, 640, "%s/%s", base, dir);
    snprintf(meta, 680, "%s/%s.sce_sys", base, dir);
}

static int mount_slot(const MountPoint *point) {
    if (!point || strncmp(point->data, "/savedata", 9)) return -1;
    int slot = atoi(point->data + 9);
    return slot >= 0 && slot < MOUNT_SLOTS && mounts[slot].used ? slot : -1;
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
    save_paths(request->user, request->title ? request->title->data : NULL, dir->data, base, folder, meta);
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
    if (backup_write) bbgpu_save_backups_write(1);
    if (!exists && (make_folders(folder) || make_folders(meta))) {
        int error = errno;
        if (backup_write) bbgpu_save_backups_write(0);
        host_unlock(&lock);
        printf("Runtime: save data '%s' cannot be created in %s: %s\n", dir->data, base, strerror(error));
        return SAVE_ERROR_INTERNAL;
    }
    if (!exists) {
        SaveParam empty = {0};
        store_param(meta, &empty);
    }
    mounts[slot].used = 1;
    mounts[slot].backup_write = backup_write;
    snprintf(mounts[slot].folder, sizeof(mounts[slot].folder), "%s", folder);
    snprintf(mounts[slot].meta, sizeof(mounts[slot].meta), "%s", meta);
    memset(result, 0, sizeof(*result));
    snprintf(result->point.data, sizeof(result->point.data), "/savedata%d", slot & 15);
    result->status = exists ? 0 : 1;
    sys_file_mount(result->point.data, folder);
    ++mount_count;
    host_unlock(&lock);
    printf("Runtime: save data '%s' mounted at %s (%s%s)\n", dir->data, result->point.data,
           exists ? "existing" : "created", (request->mode & MOUNT_READ_ONLY) ? ", read-only" : "");
    return 0;
}

static SYS_ABI int32_t save_umount(const MountPoint *point) {
    if (!initialized) return SAVE_ERROR_NOT_INITIALIZED;
    host_lock(&lock);
    int slot = mount_slot(point);
    if (slot >= 0) {
        sys_file_unmount(point->data);
        mounts[slot].used = 0;
        if (mounts[slot].backup_write) bbgpu_save_backups_write(0);
    }
    host_unlock(&lock);
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
    if (slot >= 0) {
        SaveParam param;
        load_param(mounts[slot].meta, &param);
        result = apply_param(&param, type, buffer, size) ? 0 : SAVE_ERROR_PARAMETER;
        if (!result) store_param(mounts[slot].meta, &param);
    }
    host_unlock(&lock);
    return result;
}

static SYS_ABI int32_t save_icon(const MountPoint *point, const SaveIcon *icon) {
    if (!initialized) return SAVE_ERROR_NOT_INITIALIZED;
    if (!icon || !icon->buffer) return SAVE_ERROR_PARAMETER;
    host_lock(&lock);
    int slot = mount_slot(point);
    int32_t result = SAVE_ERROR_NOT_MOUNTED;
    if (slot >= 0) {
        char path[PATH_SIZE];
        snprintf(path, sizeof(path), "%s/icon0.png", mounts[slot].meta);
        FILE *file = fopen(path, "wb");
        result = file && fwrite(icon->buffer, 1, icon->data_size, file) == icon->data_size ? 0 : SAVE_ERROR_INTERNAL;
        if (file) fclose(file);
    }
    host_unlock(&lock);
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
    save_paths(request->user, request->title ? request->title->data : NULL, request->dir->data, base, folder, meta);
    host_lock(&lock);
    struct stat info;
    if (stat(folder, &info)) { host_unlock(&lock); return SAVE_ERROR_NOT_FOUND; }
    bbgpu_save_backups_write(1);
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

static int is_meta_folder(const char *name, size_t length) {
    return length > 8 && !strcmp(name + length - 8, ".sce_sys");
}

static SYS_ABI int32_t save_dir_name_search(const SearchRequest *request, SearchResult *result) {
    if (!initialized) return SAVE_ERROR_NOT_INITIALIZED;
    if (!request || !result) return SAVE_ERROR_PARAMETER;
    char base[600];
    title_folder(request->user, request->title ? request->title->data : NULL, base, sizeof(base));
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
        load_param(meta, &found[count].param);
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
    title_folder(user, NULL, base, sizeof(base));
    snprintf(folder, sizeof(folder), "%s.memory", base);
    host_lock(&lock);
    bbgpu_save_backups_write(1);
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
    if (!result && param) store_param(folder, param);
    int error = errno;
    bbgpu_save_backups_write(0);
    host_unlock(&lock);
    if (!result) printf("Runtime: save data memory ready (%llu bytes)\n", (unsigned long long)size);
    else printf("Runtime: save data memory cannot be written in %s: %s\n", folder, strerror(error));
    return result;
}

static int32_t memory_transfer(void *buffer, uint64_t size, int64_t offset, int writing) {
    if (!initialized) return SAVE_ERROR_NOT_INITIALIZED;
    if (!buffer || offset < 0) return SAVE_ERROR_PARAMETER;
    host_lock(&lock);
    if (writing) bbgpu_save_backups_write(1);
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
        if (file) fclose(file);
        if (!result && writing) ++memory_writes;
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
