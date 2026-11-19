// SPDX-License-Identifier: GPL-3.0-or-later
#define _GNU_SOURCE
#include "runtime.h"
#include "sys/sys.h"
#include "sys/save_root.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef _WIN32
#include <process.h>
#else
#include <sys/wait.h>
#endif

enum { JAIL_EXIT_CODE = 79, CHILD_REFUSED = 10, CHILD_ACCEPTED = 11 };

static int failures;
#define EXPECT(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)
#define REQUIRE(c) do { if (!(c)) { printf("FAIL %s:%d: setup: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

static char root[700], self[1024], start_dir[700];

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

static int is_folder(const char *path) {
    struct stat info;
    return !stat(path, &info) && S_ISDIR(info.st_mode);
}

static int exists(const char *path) {
    struct stat info;
    return !stat(path, &info);
}

static int folder_is_empty(const char *path) {
    DIR *dir = opendir(path);
    if (!dir) return 0;
    int entries = 0;
    for (struct dirent *entry; (entry = readdir(dir));)
        if (strcmp(entry->d_name, ".") && strcmp(entry->d_name, "..")) ++entries;
    closedir(dir);
    return entries == 0;
}

static int same_path(const char *a, const char *b) {
#ifdef _WIN32
    return !_stricmp(a, b);
#else
    return !strcmp(a, b);
#endif
}

static void make_folder(const char *path) {
    REQUIRE(!mkdir(path, 0755));
}

static void path_in_root(const char *tail, char *out, size_t size) {
    REQUIRE(save_root_fits(snprintf(out, size, "%s/%s", root, tail), size));
}

static int run_child(const char *mode, const char *game, const char *saves) {
#ifdef _WIN32
    char quoted[3][1200];
    const char *texts[3] = {root, game, saves};
    for (int i = 0; i < 3; ++i) {
        size_t length = strlen(texts[i]), slashes = 0;
        while (slashes < length && texts[i][length - 1 - slashes] == '\\') ++slashes;
        REQUIRE(length + slashes + 3 < sizeof(quoted[i]));
        snprintf(quoted[i], sizeof(quoted[i]), "\"%s%.*s\"", texts[i], (int)slashes, texts[i] + length - slashes);
    }
    const char *const arguments[] = {"file-root-test", mode, quoted[0], quoted[1], quoted[2], NULL};
    return (int)_spawnv(_P_WAIT, self, arguments);
#else
    pid_t pid = fork();
    if (!pid) {
        execl(self, self, mode, root, game, saves, (char *)NULL);
        _exit(127);
    }
    int status = 0;
    REQUIRE(pid > 0 && waitpid(pid, &status, 0) == pid);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
}

static int child(int jailed, const char *game, const char *saves) {
    if (jailed) sys_save_data_test_jail(root);
    char cwd[800];
    path_in_root("cwd-child", cwd, sizeof(cwd));
    REQUIRE(!chdir(cwd));
    return sys_file_configure(game, saves) ? CHILD_REFUSED : CHILD_ACCEPTED;
}

static void expect_refused(const char *game, const char *saves, const char *cwd, const char *fresh) {
    EXPECT(sys_file_configure(game, saves) != 0);
    EXPECT(!strcmp(sys_file_user_dir(), ""));
    EXPECT(folder_is_empty(cwd));
    EXPECT(!exists(fresh));
}

static void unusable_roots_are_refused(void) {
    char cwd[800], game[800], fresh[800];
    path_in_root("cwd-refused", cwd, sizeof(cwd));
    path_in_root("game", game, sizeof(game));
    path_in_root("never-made", fresh, sizeof(fresh));
    make_folder(cwd);
    REQUIRE(!chdir(cwd));
#ifdef _WIN32
    char drive[4] = {root[0], ':', '/', 0}, back[4] = {root[0], ':', '\\', 0}, up[6] = {root[0], ':', '/', '.', '.', 0};
    char dot[5] = {root[0], ':', '\\', '.', 0}, twice[5] = {root[0], ':', '\\', '\\', 0};
    char device[8] = {'\\', '\\', '?', '\\', root[0], ':', '\\', 0};
    char local[8] = {'\\', '\\', '.', '\\', root[0], ':', '\\', 0};
    const char *roots[] = {"", "/", "\\", "/..", drive, back, up, dot, twice, "\\.", "/.", "//", "/\\", device, local,
                           "\\\\server\\share", "\\\\server\\share\\", "/lantern-missing-root/.."};
    const char *games[] = {"", device, local};
#else
    const char *roots[] = {"", "/", "/..", "/.", "//", "/lantern-missing-root/.."};
    const char *games[] = {""};
#endif
    for (size_t i = 0; i < sizeof(roots) / sizeof(roots[0]); ++i) expect_refused(game, roots[i], cwd, fresh);
    for (size_t i = 0; i < sizeof(games) / sizeof(games[0]); ++i) expect_refused(games[i], fresh, cwd, fresh);
    expect_refused(NULL, fresh, cwd, fresh);
    expect_refused(game, NULL, cwd, fresh);
    REQUIRE(!chdir(start_dir));
}

static void relative_roots_are_made_full_paths(void) {
    char cwd[800], expected[900], temp[900];
    path_in_root("cwd-relative", cwd, sizeof(cwd));
    make_folder(cwd);
    REQUIRE(!chdir(cwd));
    make_folder("game");
    EXPECT(!sys_file_configure("game", "./user"));
    REQUIRE(save_root_fits(snprintf(expected, sizeof(expected), "%s/user", cwd), sizeof(expected)));
    REQUIRE(save_root_fits(snprintf(temp, sizeof(temp), "%s/temp0", expected), sizeof(temp)));
    char full[900];
    REQUIRE(save_root_fits(snprintf(full, sizeof(full), "%s", sys_file_user_dir()), sizeof(full)));
    forward_slashes(full);
    EXPECT(same_path(full, expected));
    EXPECT(is_folder(temp));
    char translated[1024];
    EXPECT(!sys_file_translate("/temp0", translated, sizeof(translated)));
    forward_slashes(translated);
    EXPECT(same_path(translated, temp));
    REQUIRE(!chdir(start_dir));
}

static void game_at_a_root_is_mounted(void) {
    char saves[800], temp[900], translated[1024];
    path_in_root("root-game-saves", saves, sizeof(saves));
    REQUIRE(save_root_fits(snprintf(temp, sizeof(temp), "%s/temp0", saves), sizeof(temp)));
#ifdef _WIN32
    char drive[4] = {root[0], ':', '/', 0}, prefix[4] = {root[0], ':', '\\', 0};
#else
    const char *drive = "/", *prefix = "/";
#endif
    EXPECT(!sys_file_configure(drive, saves));
    EXPECT(is_folder(temp));
    EXPECT(!sys_file_translate("/app0/eboot.bin", translated, sizeof(translated)));
    EXPECT(!strncmp(translated, prefix, strlen(prefix)));
}

static void saves_keep_their_separators(void) {
#ifdef _WIN32
    char game[800], saves[800];
    path_in_root("game", game, sizeof(game));
    path_in_root("back-saves", saves, sizeof(saves));
    for (char *at = saves; *at; ++at)
        if (*at == '/') *at = '\\';
    EXPECT(!sys_file_configure(game, saves));
    EXPECT(!strcmp(sys_file_user_dir(), saves));
    EXPECT(strchr(sys_file_user_dir(), '/') == NULL);
#endif
}

static void mutant_is_caught(void) {
    char cwd[800], game[800];
    path_in_root("cwd-child", cwd, sizeof(cwd));
    path_in_root("game", game, sizeof(game));
    make_folder(cwd);
    EXPECT(run_child("child", game, "/") == CHILD_REFUSED);
    EXPECT(run_child("child", game, "") == CHILD_REFUSED);
    char number[8];
    snprintf(number, sizeof(number), "%d", SAVE_ROOT_MUTANT_ANY_ROOT);
    variable("BB_SAVE_TEST_MUTANT", number);
    int drive_caught = run_child("child", game, "/") == JAIL_EXIT_CODE;
    int empty_caught = run_child("child", game, "") == JAIL_EXIT_CODE;
    variable("BB_SAVE_TEST_MUTANT", "0");
    EXPECT(drive_caught);
    EXPECT(empty_caught);
    EXPECT(folder_is_empty(cwd));
    if (drive_caught && empty_caught) puts("file root mutant caught: anyroot");
    char saves[800];
    path_in_root("unjailed-saves", saves, sizeof(saves));
    EXPECT(run_child("unjailed", game, saves) == JAIL_EXIT_CODE);
    EXPECT(!exists(saves));
}

static void make_root(const char *argv0) {
    REQUIRE(getcwd(start_dir, sizeof(start_dir)) != NULL);
    forward_slashes(start_dir);
    REQUIRE(start_dir[0] == '/' || start_dir[1] == ':');
    char folder[800];
    REQUIRE(save_root_fits(snprintf(folder, sizeof(folder), "%s/out", start_dir), sizeof(folder)));
    REQUIRE(is_folder(folder));
    REQUIRE(save_root_fits(snprintf(folder, sizeof(folder), "%s/out/test-tmp", start_dir), sizeof(folder)));
    REQUIRE(!mkdir(folder, 0755) || is_folder(folder));
    REQUIRE(save_root_fits(snprintf(root, sizeof(root), "%s/file-root-%ld", folder, (long)getpid()), sizeof(root)));
    REQUIRE(!mkdir(root, 0755));
    sys_save_data_test_jail(root);
    char game[800];
    path_in_root("game", game, sizeof(game));
    make_folder(game);
#ifdef _WIN32
    (void)argv0;
    REQUIRE(GetModuleFileNameA(NULL, self, sizeof(self)) > 0);
#else
    REQUIRE(realpath(argv0, self) != NULL);
#endif
}

int main(int argc, char **argv) {
    if (argc == 5 && (!strcmp(argv[1], "child") || !strcmp(argv[1], "unjailed"))) {
        snprintf(root, sizeof(root), "%s", argv[2]);
        return child(!strcmp(argv[1], "child"), argv[3], argv[4]);
    }
    make_root(argv[0]);
    unusable_roots_are_refused();
    mutant_is_caught();
    relative_roots_are_made_full_paths();
    saves_keep_their_separators();
    game_at_a_root_is_mounted();
    if (failures) return 1;
    puts("file root tests passed");
    return 0;
}
