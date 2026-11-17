// SPDX-License-Identifier: GPL-3.0-or-later
#include "sys/sys.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *nid, *symbol;
} ImportName;

static const ImportName import_names[] = {
#include "import_names.inc"
};

enum { kMostOnline = 512, kExpectedOnline = 165 };

static int failures;
static int online_lookups;
#define EXPECT(condition) do { if (!(condition)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); ++failures; } } while (0)

uintptr_t bbgpu_resolve(const char *nid) {
    (void)nid;
    ++online_lookups;
    return 0;
}

static int starts_with(const char *text, const char *prefix) {
    return !strncmp(text, prefix, strlen(prefix));
}

static int online_name(const char *name) {
    static const char *const online[] = {"sceNet", "sceNp", "sceHttp", "sceSsl"};
    static const char *const local[] = {"sceNpTrophy", "sceNpCommerce", "sceNpProfileDialog"};
    int found = 0;
    for (size_t i = 0; i < sizeof(online) / sizeof(*online); ++i)
        if (starts_with(name, online[i])) found = 1;
    for (size_t i = 0; i < sizeof(local) / sizeof(*local); ++i)
        if (starts_with(name, local[i])) found = 0;
    return found;
}

static void set_online(const char *value) {
#ifdef _WIN32
    _putenv_s("BB_ONLINE", value);
#else
    setenv("BB_ONLINE", value, 1);
#endif
}

static char *read_file(const char *path) {
    FILE *file = fopen(path, "rb");
    if (!file) return NULL;
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return NULL;
    }
    const long size = ftell(file);
    if (size < 0 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return NULL;
    }
    char *text = malloc((size_t)size + 1);
    if (!text) {
        fclose(file);
        return NULL;
    }
    const size_t got = fread(text, 1, (size_t)size, file);
    fclose(file);
    size_t kept = 0;
    for (size_t i = 0; i < got; ++i)
        if (text[i] != '\r') text[kept++] = text[i];
    text[kept] = 0;
    return text;
}

int main(int argc, char **argv) {
    const char *golden = argc > 1 ? argv[1] : "tests/golden/offline-contract.txt";
    const int record = argc > 2 && !strcmp(argv[2], "--record");
    const ImportName *online[kMostOnline];
    uintptr_t offline[kMostOnline];
    size_t count = 0;
    for (size_t i = 0; i < sizeof(import_names) / sizeof(*import_names); ++i) {
        if (!online_name(import_names[i].symbol)) continue;
        EXPECT(count < kMostOnline);
        if (count < kMostOnline) online[count++] = &import_names[i];
    }
    EXPECT(count == kExpectedOnline);

    set_online("0");
    online_lookups = 0;
    for (size_t i = 0; i < count; ++i) {
        offline[i] = sys_net_offline_resolve(online[i]->nid);
        if (!offline[i]) printf("FAIL offline: %s (%s) has no offline entry\n", online[i]->symbol, online[i]->nid), ++failures;
        if (sys_resolve(online[i]->nid)) printf("FAIL offline: %s is resolved before the offline table\n", online[i]->symbol), ++failures;
    }
    EXPECT(online_lookups == 0);

    set_online("1");
    online_lookups = 0;
    for (size_t i = 0; i < count; ++i) {
        const uintptr_t resolved = sys_net_offline_resolve(online[i]->nid);
        if (resolved != offline[i]) printf("FAIL online, nothing registered: %s does not keep its offline entry\n", online[i]->symbol), ++failures;
    }
    EXPECT(online_lookups == (int)count);
    set_online("0");

    size_t text_size = count * 96 + 1;
    char *text = malloc(text_size);
    if (!text) return 1;
    size_t used = 0;
    uintptr_t groups[kMostOnline];
    size_t group_count = 0;
    for (size_t i = 0; i < count; ++i) {
        size_t group = 0;
        while (group < group_count && groups[group] != offline[i]) ++group;
        if (group == group_count) groups[group_count++] = offline[i];
        const int n = snprintf(text + used, text_size - used, "%s %s entry%zu\n", online[i]->nid, online[i]->symbol, group);
        if (n < 0 || (size_t)n >= text_size - used) return 1;
        used += (size_t)n;
    }

    if (record) {
        FILE *out = fopen(golden, "wb");
        if (!out || fwrite(text, 1, used, out) != used || fclose(out) != 0) {
            printf("FAIL cannot write %s\n", golden);
            return 1;
        }
        printf("offline-contract-test: recorded %zu imports in %zu offline entries\n", count, group_count);
        free(text);
        return failures ? 1 : 0;
    }
    char *expected = read_file(golden);
    if (!expected) {
        printf("FAIL cannot read %s\n", golden);
        return 1;
    }
    if (strcmp(expected, text) != 0) {
        printf("FAIL the offline contract differs from %s\n", golden);
        ++failures;
    }
    free(expected);
    free(text);
    if (failures) return 1;
    printf("offline-contract-test: ok (%zu imports, %zu offline entries)\n", count, group_count);
    return 0;
}
