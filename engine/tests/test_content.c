// SPDX-License-Identifier: GPL-3.0-or-later
#include "sys/sys.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef int32_t (SYS_ABI *ModuleCall)(uint16_t);
typedef int32_t (SYS_ABI *InitializeCall)(const void *, void *);
typedef int32_t (SYS_ABI *ParamCall)(uint32_t, int32_t *);
typedef int32_t (SYS_ABI *ListCall)(uint32_t, void *, uint32_t, uint32_t *);

typedef struct {
    char label[17];
    char padding[3];
    uint32_t status;
} Addon;

static int failures;
#define CHECK(condition) do { if (!(condition)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); ++failures; } } while (0)
#define CODE(call) ((uint32_t)(call))

static void *find(const char *nid) {
    const SysExport *e = sys_find(nid, NULL);
    return e ? e->function : NULL;
}

static int addons(ModuleCall load, InitializeCall initialize, ListCall list) {
    unsigned char init[32] = {0}, boot[40];
    CHECK(load(0xB4) == 0 && initialize(init, boot) == 0);
    Addon entries[3];
    memset(entries, 0xAA, sizeof(entries));
    uint32_t hits = 0;
    CHECK(list(0, NULL, 0, &hits) == 0);
    printf("hits %u\n", hits);
    CHECK(list(0, entries, 2, &hits) == 0 && hits <= 2);
    for (uint32_t i = 0; i < hits; ++i) printf("addon %s %u\n", entries[i].label, entries[i].status);
    const unsigned char *untouched = (const unsigned char *)&entries[hits];
    for (size_t i = 0; i < sizeof(Addon) * (3 - hits); ++i) CHECK(untouched[i] == 0xAA);
    return failures != 0;
}

static int module_references(ModuleCall load, ModuleCall loaded, ModuleCall unload) {
    CHECK(CODE(loaded(0xB5)) == 0x805A1001);
    CHECK(load(0xB5) == 0 && loaded(0xB5) == 0 && CODE(loaded(0xB4)) == 0x805A1001);
    CHECK(unload(0xB5) == 0 && CODE(loaded(0xB5)) == 0x805A1001);
    if (!failures) puts("PASS: independent module references");
    return failures != 0;
}

int main(int argc, char **argv) {
    ModuleCall load = find("g8cM39EUZ6o"), loaded = find("fMP5NHUOaMk"), unload = find("eR2bZFAAU0Q");
    InitializeCall initialize = find("R9lA82OraNs");
    ParamCall param = find("99b82IKXpH4");
    ListCall list = find("xnd8BJzAxmk");
    if (!load || !loaded || !unload || !initialize || !param || !list) {
        fputs("FAIL: AppContent or Sysmodule export missing\n", stderr);
        return 1;
    }
    const char *user = getenv("TEST_USER_DIR");
    if (user && sys_file_configure(".", user)) {
        fputs("FAIL: the test folders were refused\n", stderr);
        return 1;
    }
    const char *mode = argc > 1 ? argv[1] : "";
    if (!strcmp(mode, "--missing")) {
        load(0xB4);
        return 99;
    }
    const uint32_t profile[] = {1, 13, 0x80000000, 0, 7};
    sys_app_content_configure(profile);
    if (!strcmp(mode, "--addons")) return addons(load, initialize, list);
    if (!strcmp(mode, "--unknown")) return module_references(load, loaded, unload);

    CHECK(CODE(loaded(0)) == 0x805A1000);
    CHECK(CODE(loaded(0xB4)) == 0x805A1001);
    CHECK(CODE(unload(0xB4)) == 0x805A1001);
    CHECK(load(0xB4) == 0 && load(0xB4) == 0 && loaded(0xB4) == 0);

    unsigned char init[32] = {0};
    struct { unsigned char boot[40]; uint32_t canary; } b;
    memset(&b, 0xA5, sizeof(b));
    CHECK(CODE(initialize(NULL, b.boot)) == 0x80D90002);
    init[31] = 1;
    CHECK(CODE(initialize(init, b.boot)) == 0x80D90002);
    init[31] = 0;
    CHECK(b.boot[0] == 0xA5 && initialize(init, b.boot) == 0);
    for (int i = 0; i < 40; ++i) CHECK(b.boot[i] == 0);
    CHECK(b.canary == 0xA5A5A5A5);
    b.boot[0] = 0xCC;
    CHECK(CODE(initialize(init, b.boot)) == 0x80D90003 && b.boot[0] == 0xCC);

    struct { int32_t value; uint32_t canary; } result = {-1, 0x1234};
    for (uint32_t i = 0; i < 5; ++i) CHECK(param(i, &result.value) == 0 && (uint32_t)result.value == profile[i] && result.canary == 0x1234);
    result.value = 77;
    CHECK(CODE(param(5, &result.value)) == 0x80D90002 && result.value == 77);
    CHECK(CODE(param(0, NULL)) == 0x80D90002);

    unsigned char entries[48];
    memset(entries, 0xAA, sizeof(entries));
    struct { uint32_t hits, canary; } h = {99, 0xABCDEF};
    CHECK(list(0, NULL, 0, &h.hits) == 0 && h.hits == 0 && h.canary == 0xABCDEF);
    CHECK(list(0, entries, 2, &h.hits) == 0 && h.hits == 0);
    for (size_t i = 0; i < sizeof(entries); ++i) CHECK(entries[i] == 0xAA);
    CHECK(CODE(list(0, NULL, 0, NULL)) == 0x80D90002);

    CHECK(unload(0xB4) == 0 && loaded(0xB4) == 0);
    CHECK(CODE(initialize(init, b.boot)) == 0x80D90003);
    CHECK(unload(0xB4) == 0 && CODE(loaded(0xB4)) == 0x805A1001);
    CHECK(load(0xB4) == 0 && initialize(init, b.boot) == 0 && unload(0xB4) == 0);
    if (failures) return 1;
    puts("PASS: AppContent lifecycle, profile and output boundaries");
    return 0;
}
