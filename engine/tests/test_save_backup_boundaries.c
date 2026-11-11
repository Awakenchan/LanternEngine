// SPDX-License-Identifier: GPL-3.0-or-later
// Exercise the runtime's actual save entry points without loading a game or renderer.
#include "../src/sys/save_data.c"
#include <assert.h>

static char test_user[1024];
static int active_writes, begins, ends;
const char *sys_file_user_dir(void) { return test_user; }
int sys_file_mount(const char *guest, const char *host) { (void)guest; (void)host; return 0; }
void sys_file_unmount(const char *guest) { (void)guest; }
void bbgpu_save_backups_write(int begin) {
    if (begin) { ++active_writes; ++begins; }
    else { --active_writes; ++ends; }
    assert(active_writes >= 0);
}
#ifdef _WIN32
int runtime_win_mkdir(const char *path, int mode) { (void)mode; return _mkdir(path); }
int runtime_win_remove_tree(const char *path) { (void)path; return 0; }
#endif

int main(int argc, char **argv) {
    assert(argc == 2);
    snprintf(test_user, sizeof(test_user), "%s", argv[1]);
    assert(save_initialize(NULL) == 0);
    DirName dir = {"SPRJ0005"};
    MountRequest request = {.user = 1, .dir = &dir, .mode = MOUNT_CREATE_OR_OPEN};
    MountResult result;
    assert(save_mount(&request, &result) == 0);
    assert(active_writes == 1);
    assert(save_mount(&request, &result) == SAVE_ERROR_BUSY);
    assert(active_writes == 1); // an unsuccessful mount must not leak a second lock
    assert(save_umount(&result.point) == 0);
    assert(active_writes == 0);
    request.mode = MOUNT_READ_ONLY;
    assert(save_mount(&request, &result) == 0);
    assert(active_writes == 0); // a long-lived read-only mount must not block backups
    assert(save_umount(&result.point) == 0);
    assert(save_setup_memory(1, 16, NULL) == 0);
    char data[4] = {1, 2, 3, 4};
    assert(save_set_memory(1, data, sizeof(data), 0) == 0);
    assert(active_writes == 0);
    DeleteRequest deleting = {.user = 1, .dir = &dir};
    assert(save_delete(&deleting) == 0);
    assert(active_writes == 0 && begins == ends && begins >= 4);
    puts("PASS: runtime writable/read-only mount, failed mount, unmount, save memory and delete boundaries");
    return 0;
}
