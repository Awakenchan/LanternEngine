// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "sys.h"
#include "runtime.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef _WIN32
typedef struct _stat64 HostStat;
#define host_stat _stat64
#define host_fstat _fstat64
#define host_seek _lseeki64
#else
typedef struct stat HostStat;
#define host_stat stat
#define host_fstat fstat
#define host_seek lseek
#endif

#define KERNEL_ERROR(e) ((int32_t)(UINT32_C(0x80020000) | (uint32_t)(e)))

enum {
    FILE_LIMIT = 1024,
    MOUNT_LIMIT = 64,
    MOUNT_GUEST = 64,
    MOUNT_HOST = 512,
    PATH_SIZE = 1024,
    GUEST_READ_ONLY_FLAGS = 3 | 0x8 | 0x200 | 0x400 | 0x800,
    BOUNCE_SIZE = 1 << 20,
};

typedef struct {
    int64_t seconds, nanoseconds;
} GuestTimespec;

typedef struct {
    uint32_t dev, ino;
    uint16_t mode, nlink;
    uint32_t uid, gid, rdev;
    GuestTimespec atime, mtime, ctime;
    int64_t size, blocks;
    uint32_t blksize, flags, gen;
    int32_t spare;
    GuestTimespec birthtime;
} GuestStat;
_Static_assert(sizeof(GuestStat) == 120, "FreeBSD stat layout");

typedef struct {
    char *name;
    unsigned char type;
} DirEntry;

typedef struct {
    DirEntry *entries;
    size_t count;
} Listing;

typedef struct {
    int host, references, loader;
    Listing *listing;
    size_t position;
    uint64_t opened_ns;
    char guest[512];
} File;

typedef struct {
    char guest[MOUNT_GUEST], host[MOUNT_HOST];
    size_t length;
} Mount;

typedef struct {
    char *key, *host;
} ModFile;

static HostMutex files_lock = HOST_MUTEX_INIT;
static HostRwlock mounts_lock = HOST_RWLOCK_INIT;
static File *files[FILE_LIMIT];
static int lowest_free = 3;
static File standard_files[3] = {{.host = 0, .references = 1}, {.host = 1, .references = 1}, {.host = 2, .references = 1}};
static Mount mounts[MOUNT_LIMIT];
static size_t mount_count;
static ModFile *mod_files;
static size_t mod_file_count;
static char user_dir[512] = "user";
static uint64_t count_opens, count_reads, count_read_bytes, count_writes, count_missing;

int sys_file_mount(const char *guest, const char *host) {
    int result = 0;
    host_write_lock(&mounts_lock);
    size_t i = 0;
    while (i < mount_count && strcmp(mounts[i].guest, guest)) ++i;
    if (i == mount_count) {
        if (i == MOUNT_LIMIT || strlen(guest) >= MOUNT_GUEST || strlen(host) >= MOUNT_HOST) result = -1;
        else {
            snprintf(mounts[i].guest, MOUNT_GUEST, "%s", guest);
            mounts[i].length = strlen(guest);
            ++mount_count;
        }
    }
    if (!result) snprintf(mounts[i].host, MOUNT_HOST, "%s", host);
    host_write_unlock(&mounts_lock);
    return result;
}

void sys_file_unmount(const char *guest) {
    host_write_lock(&mounts_lock);
    for (size_t i = 0; i < mount_count; ++i) {
        if (!strcmp(mounts[i].guest, guest)) {
            mounts[i] = mounts[--mount_count];
            break;
        }
    }
    host_write_unlock(&mounts_lock);
}

const char *sys_file_user_dir(void) {
    return user_dir;
}

static void fold_path(char *path) {
    for (; *path; ++path) {
        if (*path == '\\') *path = '/';
        else if (*path >= 'A' && *path <= 'Z') *path = (char)(*path - 'A' + 'a');
    }
}

static int compare_mod_files(const void *a, const void *b) {
    return strcmp(((const ModFile *)a)->key, ((const ModFile *)b)->key);
}

static char *split_list_line(char *line) {
    line[strcspn(line, "\r\n")] = 0;
    char *tab = strchr(line, '\t');
    if (!tab || tab == line || !tab[1]) return NULL;
    *tab = 0;
    return tab + 1;
}

static FILE *open_list(const char *variable, const char **name) {
    *name = getenv(variable);
    return *name && **name ? fopen(*name, "rb") : NULL;
}

static void load_mod_files(void) {
    const char *name;
    FILE *list = open_list("BB_MOD_FILES", &name);
    if (!list) return;
    char line[2048];
    size_t capacity = 0;
    while (fgets(line, sizeof(line), list)) {
        char *host = split_list_line(line);
        if (!host) continue;
        if (mod_file_count == capacity) {
            capacity = capacity ? capacity * 2 : 256;
            ModFile *grown = realloc(mod_files, capacity * sizeof(*grown));
            if (!grown) break;
            mod_files = grown;
        }
        ModFile *entry = &mod_files[mod_file_count];
        entry->key = strdup(line);
        entry->host = strdup(host);
        if (!entry->key || !entry->host) break;
        fold_path(entry->key);
        ++mod_file_count;
    }
    fclose(list);
    qsort(mod_files, mod_file_count, sizeof(*mod_files), compare_mod_files);
    printf("Runtime: %zu mod files read in place (%s)\n", mod_file_count, name);
}

static const char *find_mod_file(const char *guest) {
    if (!mod_file_count) return NULL;
    const char *rest = !strncmp(guest, "/app0/", 6) ? guest + 6 : !strncmp(guest, "/hostapp/", 9) ? guest + 9 : NULL;
    if (!rest || strlen(rest) >= PATH_SIZE) return NULL;
    char key[PATH_SIZE];
    snprintf(key, sizeof(key), "%s", rest);
    fold_path(key);
    const ModFile wanted = {key, NULL};
    const ModFile *found = bsearch(&wanted, mod_files, mod_file_count, sizeof(*mod_files), compare_mod_files);
    return found ? found->host : NULL;
}

static void load_file_fixes(void) {
    const char *name;
    FILE *list = open_list("BB_FILE_FIXES", &name);
    if (!list) return;
    char line[2048], guest[2064];
    int mounted = 0;
    while (fgets(line, sizeof(line), list)) {
        char *host = split_list_line(line);
        if (!host) continue;
        snprintf(guest, sizeof(guest), "/app0/%s", line);
        if (!sys_file_mount(guest, host)) ++mounted;
        else printf("Runtime: the fixed %s cannot be mounted\n", line);
    }
    fclose(list);
    if (mounted) printf("Runtime: %d game files fixed (BB_FILE_FIXES)\n", mounted);
}

void sys_file_configure(const char *app0, const char *user) {
    sys_file_stream_configure(app0);
    load_mod_files();
    snprintf(user_dir, sizeof(user_dir), "%s", user);
    sys_file_mount("/app0", app0);
    sys_file_mount("/hostapp", app0);
    load_file_fixes();
    const char *language = getenv("BB_LANGUAGE");
    if (language && !strcmp(language, "17"))
        puts("Runtime: Brazilian Portuguese requested (native assets; game region unchanged)");
    mkdir(user, 0755);
    char path[600];
    snprintf(path, sizeof(path), "%s/.write-test", user);
    FILE *probe = fopen(path, "wb");
    if (probe) {
        fclose(probe);
        remove(path);
    } else {
        printf("Saves: the saves folder %s cannot be written (%s): the game cannot create or keep "
               "saves. Pick another saves folder in the launcher%s\n", user, strerror(errno),
#ifdef _WIN32
               ", or allow Lantern Engine in Windows Security > Ransomware protection > "
               "Controlled folder access"
#else
               ""
#endif
        );
    }
    static const char *const writable[] = {"temp0", "download0", "data"};
    for (int i = 0; i < 3; ++i) {
        snprintf(path, sizeof(path), "%s/%s", user, writable[i]);
        mkdir(path, 0755);
        char guest[32];
        snprintf(guest, sizeof(guest), "/%s", writable[i]);
        sys_file_mount(guest, path);
    }
}

static int climbs_up(const char *path) {
    for (const char *part = path; part;) {
        if (part[0] == '.' && part[1] == '.' && (!part[2] || part[2] == '/')) return 1;
        part = strchr(part, '/');
        if (part) ++part;
    }
    return 0;
}

static int translate_mounted(const char *full, char *out, size_t size) {
    const char *mod = find_mod_file(full);
    if (mod) return (size_t)snprintf(out, size, "%s", mod) >= size ? ENAMETOOLONG : 0;
    const Mount *best = NULL;
    int result = ENOENT;
    host_read_lock(&mounts_lock);
    for (size_t i = 0; i < mount_count; ++i) {
        const Mount *mount = &mounts[i];
        if (!strncmp(full, mount->guest, mount->length) && (full[mount->length] == '/' || !full[mount->length]) &&
            (!best || mount->length > best->length))
            best = mount;
    }
    if (best) result = (size_t)snprintf(out, size, "%s%s", best->host, full + best->length) >= size ? ENAMETOOLONG : 0;
    host_read_unlock(&mounts_lock);
    return result;
}

/* CUSA03173 can fall back to another language despite system language 17. Select the
 * existing Brazilian assets without changing the game's region (used by online play).
 * Rewrite guest paths, not host paths, so mods and per-file mounts still apply. */
static int brazilian_path(const char *full, char localized[PATH_SIZE]) {
    const char *relative = !strncmp(full, "/app0/", 6) ? full + 6 :
                           !strncmp(full, "/hostapp/", 9) ? full + 9 : NULL;
    if (!relative || strncmp(relative, "dvdroot_ps4/", 12)) return 0;
    const char *part = relative + 12;
    const char *folder = !strncmp(part, "msg/", 4) ? part + 4 :
                         !strncmp(part, "menu/", 5) ? part + 5 : NULL;
    if (folder) {
        static const char *const languages[] = {
            "engus", "enggb", "rusru", "jpnjp", "frafr", "spaes", "spaar", "deude", "itait", "porpt",
        };
        for (size_t i = 0; i < sizeof(languages) / sizeof(*languages); ++i) {
            if (strncmp(folder, languages[i], 5) || folder[5] != '/') continue;
            strcpy(localized, full);
            memcpy(localized + (folder - full), "porbr", 5);
            return 1;
        }
    } else if (!strncmp(part, "sound/", 6)) {
        const char *suffix = strrchr(part + 6, '_');
        static const char *const voices[] = {"eng", "ded", "esa", "ese", "frf", "iti", "jaj"};
        if (!suffix || strlen(suffix) != 8 || strcmp(suffix + 4, ".fsb")) return 0;
        for (size_t i = 0; i < sizeof(voices) / sizeof(*voices); ++i) {
            if (strncmp(suffix + 1, voices[i], 3)) continue;
            strcpy(localized, full);
            memcpy(localized + (suffix + 1 - full), "ptb", 3);
            return 1;
        }
    }
    return 0;
}

int sys_file_translate(const char *guest, char *out, size_t size) {
    if (!guest || !*guest) return ENOENT;
    char full[PATH_SIZE];
    if ((size_t)snprintf(full, sizeof(full), "%s%s", guest[0] == '/' ? "" : "/app0/", guest) >= sizeof(full))
        return ENAMETOOLONG;
    if (climbs_up(full)) return EACCES;
    const char *language = getenv("BB_LANGUAGE");
    char localized[PATH_SIZE], host[PATH_SIZE];
    HostStat st;
    if (language && !strcmp(language, "17") && brazilian_path(full, localized) &&
        !translate_mounted(localized, host, sizeof(host)) && !host_stat(host, &st) && S_ISREG(st.st_mode))
        return (size_t)snprintf(out, size, "%s", host) >= size ? ENAMETOOLONG : 0;
    int result = translate_mounted(full, out, size);
    if (result == ENOENT) fprintf(stderr, "Runtime: no mount for guest path %s\n", guest);
    return result;
}

static int under_mount(const char *path, const char *mount) {
    size_t length = strlen(mount);
    return !strncmp(path, mount, length) && (!path[length] || path[length] == '/');
}

static int is_game_path(const char *path) {
    if (!path || !*path) return 0;
    return *path != '/' || under_mount(path, "/app0") || under_mount(path, "/hostapp");
}

static int to_host_flags(int flags) {
    static const struct { int guest, host; } map[] = {
        {0x4, O_NONBLOCK}, {0x8, O_APPEND}, {0x80, O_SYNC}, {0x200, O_CREAT},
        {0x400, O_TRUNC}, {0x800, O_EXCL}, {0x20000, O_DIRECTORY},
    };
    int access_mode = flags & 3;
    int host = access_mode == 0 ? O_RDONLY : access_mode == 1 ? O_WRONLY : O_RDWR;
    for (size_t i = 0; i < sizeof(map) / sizeof(*map); ++i)
        if (flags & map[i].guest) host |= map[i].host;
#ifdef _WIN32
    host |= O_BINARY;
#endif
    return host | O_CLOEXEC;
}

static void free_listing(Listing *listing) {
    if (!listing) return;
    for (size_t i = 0; i < listing->count; ++i) free(listing->entries[i].name);
    free(listing->entries);
    free(listing);
}

static unsigned char entry_type(DIR *dir, const char *path, const struct dirent *entry) {
#ifdef _WIN32
    (void)dir;
    char child[1300];
    HostStat info;
    snprintf(child, sizeof(child), "%s/%s", path, entry->d_name);
    if (host_stat(child, &info)) return 0;
    return S_ISDIR(info.st_mode) ? 4 : S_ISREG(info.st_mode) ? 8 : 0;
#else
    (void)path;
    unsigned char type = entry->d_type;
    HostStat info;
    if ((type == DT_LNK || type == DT_UNKNOWN) && !fstatat(dirfd(dir), entry->d_name, &info, 0))
        type = S_ISDIR(info.st_mode) ? DT_DIR : S_ISREG(info.st_mode) ? DT_REG : type;
    return type == DT_DIR ? 4 : type == DT_REG ? 8 : type == DT_LNK ? 10 : 0;
#endif
}

static Listing *list_directory(const char *path) {
    DIR *dir = opendir(path);
    if (!dir) return NULL;
    Listing *listing = calloc(1, sizeof(*listing));
    size_t capacity = 0;
    int failed = !listing;
    for (struct dirent *entry; !failed && (entry = readdir(dir));) {
        if (listing->count == capacity) {
            capacity = capacity ? capacity * 2 : 64;
            DirEntry *grown = realloc(listing->entries, capacity * sizeof(*grown));
            if (!grown) { failed = 1; break; }
            listing->entries = grown;
        }
        char *name = strdup(entry->d_name);
        if (!name) { failed = 1; break; }
        listing->entries[listing->count++] = (DirEntry){name, entry_type(dir, path, entry)};
    }
    closedir(dir);
    if (failed) {
        fputs("Out of memory listing directory\n", stderr);
        exit(1);
    }
    return listing;
}

static void to_guest_stat(const HostStat *from, GuestStat *to) {
    memset(to, 0, sizeof(*to));
    to->dev = (uint32_t)from->st_dev;
    to->ino = (uint32_t)from->st_ino;
    to->nlink = (uint16_t)from->st_nlink;
    to->size = from->st_size;
#ifdef _WIN32
    int owner = from->st_mode & 0700;
    to->mode = (uint16_t)((S_ISDIR(from->st_mode) ? 0040000 : 0100000) | (owner * 0111 & 0777));
    to->blocks = (from->st_size + 511) / 512;
    to->blksize = 65536;
    to->atime = (GuestTimespec){from->st_atime, 0};
    to->mtime = (GuestTimespec){from->st_mtime, 0};
    to->ctime = (GuestTimespec){from->st_ctime, 0};
#else
    to->mode = (uint16_t)from->st_mode;
    to->blocks = from->st_blocks;
    to->blksize = (uint32_t)from->st_blksize;
    to->atime = (GuestTimespec){from->st_atim.tv_sec, from->st_atim.tv_nsec};
    to->mtime = (GuestTimespec){from->st_mtim.tv_sec, from->st_mtim.tv_nsec};
    to->ctime = (GuestTimespec){from->st_ctim.tv_sec, from->st_ctim.tv_nsec};
#endif
    to->birthtime = to->ctime;
}

static File *pin(int fd) {
    if (fd >= 0 && fd < 3) return &standard_files[fd];
    if (fd < 3 || fd >= FILE_LIMIT) return NULL;
    host_lock(&files_lock);
    File *file = files[fd];
    if (file) ++file->references;
    host_unlock(&files_lock);
    return file;
}

static void unpin(File *file) {
    if (file >= standard_files && file < standard_files + 3) return;
    host_lock(&files_lock);
    int left = --file->references;
    host_unlock(&files_lock);
    if (left) return;
    if (file->host >= 0) close(file->host);
    free_listing(file->listing);
    free(file);
}

static File *pin_opened(int fd) {
    return fd >= 3 ? pin(fd) : NULL;
}

static int trace_audio(void) {
    static int value = -1;
    if (value < 0) {
        const char *setting = getenv("BB_AUDIO_TRACE");
        value = setting && setting[0] == '1';
    }
    return value;
}

static void trace_mod_open(const char *guest, const char *path) {
    const char *trace = getenv("BB_MOD_TRACE"), *root_setting = getenv("BB_MODS_DIR");
    if (!trace || trace[0] != '1' || !root_setting) return;
    char actual[PATH_MAX], root[PATH_MAX];
    static unsigned traced;
    if (!realpath(path, actual) || !realpath(root_setting, root)) return;
    size_t length = strlen(root);
    if (!strncmp(actual, root, length) && (actual[length] == '/' || actual[length] == '\\') &&
        __atomic_fetch_add(&traced, 1, __ATOMIC_RELAXED) < 32)
        printf("Mods: open %s -> %s\n", guest, actual);
}

static int64_t do_open(const char *guest, int flags, int mode) {
    if (is_game_path(guest) && (flags & GUEST_READ_ONLY_FLAGS)) return -EROFS;
    char path[PATH_SIZE];
    int e = sys_file_translate(guest, path, sizeof(path));
    if (e) return -e;
    HostStat info;
    Listing *listing = NULL;
    int host = -1;
    const int game_read = is_game_path(guest) && !(flags & 3);
    const int loader = game_read && runtime_thread_is_loader();
    const uint64_t started = host_monotonic_ns();
    int host_flags = to_host_flags(flags);
#ifdef _O_SEQUENTIAL
    if (game_read && sys_file_stream_sequential()) host_flags |= _O_SEQUENTIAL;
#endif
    host = open(path, host_flags, mode ? mode : 0644);
    if (host < 0) {
        e = errno;
#ifdef _WIN32
        if (e == EACCES && !host_stat(path, &info) && S_ISDIR(info.st_mode)) {
            if (flags & 3) return -EISDIR;
            listing = list_directory(path);
            if (!listing) return -errno;
        } else
#endif
        {
            if (e == ENOENT) {
                __atomic_add_fetch(&count_missing, 1, __ATOMIC_RELAXED);
                printf("Runtime: open(%s) -> not found\n", guest);
            }
            return -e;
        }
    }
#ifndef _WIN32
    else if (!host_fstat(host, &info) && S_ISDIR(info.st_mode)) {
        listing = list_directory(path);
    }
#endif
    if (loader && !listing) sys_file_stream_opened(guest, path, started);
    File *file = calloc(1, sizeof(*file));
    if (!file) {
        if (host >= 0) close(host);
        free_listing(listing);
        return -ENOMEM;
    }
    file->host = host;
    file->references = 1;
    file->listing = listing;
    file->opened_ns = started;
    file->loader = loader && !listing;
    snprintf(file->guest, sizeof(file->guest), "%s", guest);
    int fd = -1;
    host_lock(&files_lock);
    for (int i = lowest_free; i < FILE_LIMIT && fd < 0; ++i) if (!files[i]) fd = i;
    if (fd >= 0) {
        files[fd] = file;
        lowest_free = fd + 1;
    }
    host_unlock(&files_lock);
    if (fd < 0) {
        if (host >= 0) close(host);
        free_listing(listing);
        free(file);
        return -EMFILE;
    }
    if (file->loader) sys_file_trace('O', fd, 0, 0, 0, path);
    __atomic_add_fetch(&count_opens, 1, __ATOMIC_RELAXED);
    if (trace_audio() && strstr(guest, "sound/") && !host_stat(path, &info))
        printf("Audio trace: open(%s) -> fd %d, %lld bytes\n", guest, fd, (long long)info.st_size);
    trace_mod_open(guest, path);
    return fd;
}

static int64_t do_close(int fd) {
    if (fd >= 0 && fd < 3) return 0;
    if (fd < 3 || fd >= FILE_LIMIT) return -EBADF;
    host_lock(&files_lock);
    File *file = files[fd];
    files[fd] = NULL;
    if (file && fd < lowest_free) lowest_free = fd;
    host_unlock(&files_lock);
    if (!file) return -EBADF;
    if (file->loader) {
        sys_file_trace('C', fd, 0, 0, 0, NULL);
        sys_file_stream_closed(file->opened_ns);
    }
    unpin(file);
    return 0;
}

static void touch_pages(void *buffer, uint64_t size) {
    uintptr_t address = (uintptr_t)buffer, end = address + size;
    while (address < end) {
        volatile unsigned char *byte = (volatile unsigned char *)address;
        *byte = *byte;
        address = (address & ~(uintptr_t)4095) + 4096;
    }
}

static int64_t read_through_bounce(int host, void *buffer, uint64_t size, int64_t offset) {
    unsigned char *bounce = malloc(size < BOUNCE_SIZE ? size : BOUNCE_SIZE);
    if (!bounce) {
        errno = ENOMEM;
        return -1;
    }
    uint64_t done = 0;
    while (done < size) {
        const uint64_t chunk = size - done < BOUNCE_SIZE ? size - done : BOUNCE_SIZE;
        const int64_t got = offset < 0 ? read(host, bounce, chunk) : pread(host, bounce, chunk, offset + (int64_t)done);
        if (got < 0) {
            if (done) break;
            int e = errno;
            free(bounce);
            errno = e;
            return -1;
        }
        memcpy((unsigned char *)buffer + done, bounce, (size_t)got);
        done += (uint64_t)got;
        if ((uint64_t)got < chunk) break;
    }
    free(bounce);
    static unsigned notes;
    if (__atomic_fetch_add(&notes, 1, __ATOMIC_RELAXED) < 4)
        printf("Runtime: a file read into guest memory failed (a page protected again); read through a host buffer\n");
    return (int64_t)done;
}

static int64_t read_file(int fd, void *buffer, uint64_t size, int64_t offset, int positional) {
    File *file = pin(fd);
    if (!file) return -EBADF;
    if (file->host < 0) {
        unpin(file);
        return -EISDIR;
    }
    runtime_memory_note_write((uintptr_t)buffer, size);
    touch_pages(buffer, size);
    const uint64_t started = sys_file_stream_read_begin(file->loader);
    int64_t got = positional ? pread(file->host, buffer, size, offset) : read(file->host, buffer, size);
    if (got < 0 && size && (!positional || offset >= 0) && runtime_memory_guest_writable((uintptr_t)buffer, size))
        got = read_through_bounce(file->host, buffer, size, offset);
    if (file->loader) sys_file_trace('R', fd, positional ? offset : -1, (int64_t)size, got, NULL);
    sys_file_stream_read_end(file->loader, started, got);
    if (got < 0) {
        int e = errno;
        static unsigned noted;
        if (__atomic_fetch_add(&noted, 1, __ATOMIC_RELAXED) < 16)
            printf("Runtime: reading %s (%llu bytes at %lld) failed: %s\n", fd < 3 ? "?" : file->guest,
                   (unsigned long long)size, (long long)offset, strerror(e));
        unpin(file);
        return -e;
    }
    unpin(file);
    if (got > 0) runtime_memory_note_write((uintptr_t)buffer, (uint64_t)got);
    __atomic_add_fetch(&count_reads, 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&count_read_bytes, (uint64_t)got, __ATOMIC_RELAXED);
    return got;
}

static int64_t do_read(int fd, void *buffer, uint64_t size) {
    return read_file(fd, buffer, size, -1, 0);
}

static int64_t do_pread(int fd, void *buffer, uint64_t size, int64_t offset) {
    return read_file(fd, buffer, size, offset, 1);
}

static File *pin_host(int fd) {
    File *file = pin(fd);
    if (file && file->host < 0) {
        unpin(file);
        return NULL;
    }
    return file;
}

static int64_t do_write(int fd, const void *buffer, uint64_t size) {
    File *file = pin_host(fd);
    if (!file) return -EBADF;
    int64_t written = write(file->host, buffer, size);
    int e = errno;
    unpin(file);
    if (written < 0) return -e;
    __atomic_add_fetch(&count_writes, 1, __ATOMIC_RELAXED);
    return written;
}

static int64_t do_pwrite(int fd, const void *buffer, uint64_t size, int64_t offset) {
    File *file = pin_host(fd);
    if (!file) return -EBADF;
    int64_t written = pwrite(file->host, buffer, size, offset);
    int e = errno;
    unpin(file);
    return written < 0 ? -e : written;
}

static int64_t do_lseek(int fd, int64_t offset, int whence) {
    File *file = pin_opened(fd);
    if (!file) return -EBADF;
    int64_t result;
    if (whence < 0 || whence > 2) result = -EINVAL;
    else if (file->listing) {
        result = -EINVAL;
        if (whence == 0 && offset >= 0) {
            host_lock(&files_lock);
            file->position = (size_t)offset;
            host_unlock(&files_lock);
            result = offset;
        }
    } else {
        result = host_seek(file->host, offset, whence);
        if (result < 0) result = -errno;
        if (file->loader) sys_file_trace('S', fd, offset, whence, result, NULL);
    }
    unpin(file);
    return result;
}

static int64_t do_stat(const char *guest, GuestStat *out) {
    char path[PATH_SIZE];
    HostStat info;
    int e = sys_file_translate(guest, path, sizeof(path));
    if (e) return -e;
    if (!out) return -EFAULT;
    if (host_stat(path, &info)) return -errno;
    to_guest_stat(&info, out);
    return 0;
}

static int64_t do_fstat(int fd, GuestStat *out) {
    File *file = pin(fd);
    if (!file) return -EBADF;
    int64_t result = 0;
    if (file->host < 0) result = do_stat(file->guest, out);
    else if (!out) result = -EFAULT;
    else {
        HostStat info;
        if (host_fstat(file->host, &info)) result = -errno;
        else to_guest_stat(&info, out);
    }
    unpin(file);
    return result;
}

static int64_t do_getdents(int fd, char *buffer, uint64_t size, int64_t *base) {
    File *file = pin_opened(fd);
    if (!file) return -EBADF;
    int64_t result = 0;
    host_lock(&files_lock);
    if (!file->listing) result = -EINVAL;
    else if (!buffer) result = -EFAULT;
    else if (size < 512) result = -EINVAL;
    else {
        if (base) *base = (int64_t)file->position;
        uint64_t written = 0;
        while (file->position < file->listing->count) {
            const DirEntry *entry = &file->listing->entries[file->position];
            size_t length = strlen(entry->name);
            if (length > 255) length = 255;
            uint16_t record = (uint16_t)((8 + length + 1 + 7) & ~(size_t)7);
            if (written + record > size) break;
            char *out = buffer + written;
            uint32_t ino = (uint32_t)file->position + 1;
            memset(out, 0, record);
            memcpy(out, &ino, 4);
            memcpy(out + 4, &record, 2);
            out[6] = (char)entry->type;
            out[7] = (char)length;
            memcpy(out + 8, entry->name, length);
            written += record;
            ++file->position;
        }
        result = (int64_t)written;
    }
    host_unlock(&files_lock);
    unpin(file);
    return result;
}

static int64_t writable_path(const char *guest, char *path) {
    if (is_game_path(guest)) return -EROFS;
    return -sys_file_translate(guest, path, PATH_SIZE);
}

static int64_t do_mkdir(const char *guest, int mode) {
    char path[PATH_SIZE];
    int64_t e = writable_path(guest, path);
    if (e) return e;
    return mkdir(path, mode ? mode : 0755) ? -errno : 0;
}

static int64_t do_rmdir(const char *guest) {
    char path[PATH_SIZE];
    int64_t e = writable_path(guest, path);
    if (e) return e;
    return rmdir(path) ? -errno : 0;
}

static int64_t do_unlink(const char *guest) {
    char path[PATH_SIZE];
    int64_t e = writable_path(guest, path);
    if (e) return e;
    return unlink(path) ? -errno : 0;
}

static int64_t do_rename(const char *from, const char *to) {
    if (is_game_path(from) || is_game_path(to)) return -EROFS;
    char host_from[PATH_SIZE], host_to[PATH_SIZE];
    int e = sys_file_translate(from, host_from, sizeof(host_from));
    if (!e) e = sys_file_translate(to, host_to, sizeof(host_to));
    if (e) return -e;
    return rename(host_from, host_to) ? -errno : 0;
}

static int64_t do_truncate(const char *guest, int64_t length) {
    char path[PATH_SIZE];
    int64_t e = writable_path(guest, path);
    if (e) return e;
    return truncate(path, length) ? -errno : 0;
}

static int64_t do_ftruncate(int fd, int64_t length) {
    File *file = pin_host(fd);
    if (!file) return -EBADF;
    int result = ftruncate(file->host, length);
    int e = errno;
    unpin(file);
    return result ? -e : 0;
}

static int64_t do_fsync(int fd) {
    File *file = pin_host(fd);
    if (!file) return -EBADF;
    int result = fsync(file->host);
    int e = errno;
    unpin(file);
    return result ? -e : 0;
}

int64_t sys_file_open(const char *path, int flags, int mode) { return do_open(path, flags, mode); }
int64_t sys_file_close(int fd) { return do_close(fd); }
int64_t sys_file_read(int fd, void *buffer, uint64_t size) { return do_read(fd, buffer, size); }

void sys_file_report(void) {
    printf("Runtime: files opened=%zu, reads=%zu (%llu bytes), writes=%zu, not found=%zu\n",
           (size_t)count_opens, (size_t)count_reads, (unsigned long long)count_read_bytes, (size_t)count_writes,
           (size_t)count_missing);
}

static int64_t kernel_result(int64_t r) {
    return r < 0 ? KERNEL_ERROR(runtime_guest_errno((int)-r)) : r;
}

static int64_t posix_result(int64_t r) {
    if (r >= 0) return r;
    *runtime_errno() = runtime_guest_errno((int)-r);
    return -1;
}

static SYS_ABI int64_t kernel_open(const char *p, int f, int m) { return kernel_result(do_open(p, f, m)); }
static SYS_ABI int64_t kernel_close(int fd) { return kernel_result(do_close(fd)); }
static SYS_ABI int64_t kernel_read(int fd, void *b, uint64_t n) { return kernel_result(do_read(fd, b, n)); }
static SYS_ABI int64_t kernel_pread(int fd, void *b, uint64_t n, int64_t o) { return kernel_result(do_pread(fd, b, n, o)); }
static SYS_ABI int64_t kernel_write(int fd, const void *b, uint64_t n) { return kernel_result(do_write(fd, b, n)); }
static SYS_ABI int64_t kernel_pwrite(int fd, const void *b, uint64_t n, int64_t o) { return kernel_result(do_pwrite(fd, b, n, o)); }
static SYS_ABI int64_t kernel_lseek(int fd, int64_t o, int w) { return kernel_result(do_lseek(fd, o, w)); }
static SYS_ABI int64_t kernel_fstat(int fd, GuestStat *s) { return kernel_result(do_fstat(fd, s)); }
static SYS_ABI int64_t kernel_stat(const char *p, GuestStat *s) { return kernel_result(do_stat(p, s)); }
static SYS_ABI int64_t kernel_getdents(int fd, char *b, uint64_t n, int64_t *base) { return kernel_result(do_getdents(fd, b, n, base)); }
static SYS_ABI int64_t kernel_rename(const char *a, const char *b) { return kernel_result(do_rename(a, b)); }
static SYS_ABI int64_t kernel_ftruncate(int fd, int64_t l) { return kernel_result(do_ftruncate(fd, l)); }
static SYS_ABI int64_t kernel_truncate(const char *p, int64_t l) { return kernel_result(do_truncate(p, l)); }
static SYS_ABI int64_t kernel_fsync(int fd) { return kernel_result(do_fsync(fd)); }
static SYS_ABI int64_t kernel_mkdir(const char *p, int m) { return kernel_result(do_mkdir(p, m)); }
static SYS_ABI int64_t kernel_rmdir(const char *p) { return kernel_result(do_rmdir(p)); }
static SYS_ABI int64_t kernel_unlink(const char *p) { return kernel_result(do_unlink(p)); }
static SYS_ABI int64_t posix_read(int fd, void *b, uint64_t n) { return posix_result(do_read(fd, b, n)); }
static SYS_ABI int64_t posix_write(int fd, const void *b, uint64_t n) { return posix_result(do_write(fd, b, n)); }
static SYS_ABI int64_t posix_fstat(int fd, GuestStat *s) { return posix_result(do_fstat(fd, s)); }
static SYS_ABI int64_t posix_stat(const char *p, GuestStat *s) { return posix_result(do_stat(p, s)); }
static SYS_ABI int64_t posix_mkdir(const char *p, int m) { return posix_result(do_mkdir(p, m)); }
static SYS_ABI int64_t posix_rmdir(const char *p) { return posix_result(do_rmdir(p)); }
static SYS_ABI int64_t posix_rename(const char *a, const char *b) { return posix_result(do_rename(a, b)); }

static const SysExport kernel_exports[] = {
    {"1G3lF1Gg1k8", "sceKernelOpen", kernel_open},
    {"UK2Tl2DWUns", "sceKernelClose", kernel_close},
    {"Cg4srZ6TKbU", "sceKernelRead", kernel_read},
    {"+r3rMFwItV4", "sceKernelPread", kernel_pread},
    {"4wSze92BhLI", "sceKernelWrite", kernel_write},
    {"nKWi-N2HBV4", "sceKernelPwrite", kernel_pwrite},
    {"oib76F-12fk", "sceKernelLseek", kernel_lseek},
    {"kBwCPsYX-m4", "sceKernelFstat", kernel_fstat},
    {"eV9wAD2riIA", "sceKernelStat", kernel_stat},
    {"taRWhTJFTgE", "sceKernelGetdirentries", kernel_getdents},
    {"52NcYU9+lEo", "sceKernelRename", kernel_rename},
    {"VW3TVZiM4-E", "sceKernelFtruncate", kernel_ftruncate},
    {"WlyEA-sLDf0", "sceKernelTruncate", kernel_truncate},
    {"fTx66l5iWIA", "sceKernelFsync", kernel_fsync},
    {"1-LFLmRFxxM", "sceKernelMkdir", kernel_mkdir},
    {"naInUjYt3so", "sceKernelRmdir", kernel_rmdir},
    {"AUXVxWeJU-A", "sceKernelUnlink", kernel_unlink},
    {"DRuBt2pvICk", "_read", posix_read},
    {"FxVZqBAA7ks", "_write", posix_write},
    {"mqQMh1zPPT8", "fstat", posix_fstat},
};

static const SysExport posix_exports[] = {
    {"E6ao34wPw+U", "stat", posix_stat},
    {"JGMio+21L4c", "mkdir", posix_mkdir},
    {"c7ZnT7V1B98", "rmdir", posix_rmdir},
    {"NN01qLRhiqU", "rename", posix_rename},
};

SYS_LIBRARY(sys_file, "libkernel", kernel_exports);
SYS_LIBRARY(sys_posix_file, "libScePosix", posix_exports);
