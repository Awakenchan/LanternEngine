// SPDX-License-Identifier: GPL-3.0-or-later
#include "sys.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define MODULE_LIMIT 17
#define INFO_SIZE 0x1a8
#define E_SEARCH ((int32_t)0x80020003)
#define E_FAULT ((int32_t)0x8002000e)

typedef struct {
    const unsigned char *start;
    uint64_t size, init;
    char name[32];
    int searched;
    const unsigned char *frame_header, *frames;
    uint32_t frame_header_size, frames_size;
} Module;

static Module modules[MODULE_LIMIT];
static unsigned module_total;
static uint64_t lookups, misses;

void sys_module_register(const unsigned char *start, uint64_t size, uint64_t init, const char *name) {
    if (module_total >= MODULE_LIMIT || !start || !size) return;
    Module *m = &modules[module_total++];
    memset(m, 0, sizeof(*m));
    m->start = start;
    m->size = size;
    m->init = init;
    snprintf(m->name, sizeof(m->name), "%s", name);
}

static uint32_t read32(const unsigned char *p) {
    uint32_t value;
    memcpy(&value, p, 4);
    return value;
}

static int inside(const Module *m, const unsigned char *p, uint64_t bytes) {
    return p >= m->start && (uint64_t)(p - m->start) <= m->size && bytes <= m->size - (uint64_t)(p - m->start);
}

static int table_entry(const Module *m, const unsigned char *header, uint32_t index, const unsigned char **code) {
    const unsigned char *entry = header + 12 + (uint64_t)index * 8;
    const unsigned char *location = header + (int32_t)read32(entry);
    const unsigned char *frame = header + (int32_t)read32(entry + 4);
    if (!inside(m, location, 1) || !inside(m, frame, 8)) return 0;
    uint32_t length = read32(frame);
    if (!length || length == 0xffffffffu || !inside(m, frame, (uint64_t)length + 4)) return 0;
    *code = location;
    return 1;
}

static int header_ok(const Module *m, const unsigned char *header) {
    if (!inside(m, header, 12)) return 0;
    uint32_t count = read32(header + 8);
    if (!count || !inside(m, header, 12 + (uint64_t)count * 8)) return 0;
    const unsigned char *frames = header + 4 + (int32_t)read32(header + 4);
    if (!inside(m, frames, 8)) return 0;
    const unsigned char *first, *middle, *last;
    if (!table_entry(m, header, 0, &first) || !table_entry(m, header, count / 2, &middle) ||
        !table_entry(m, header, count - 1, &last))
        return 0;
    return first <= middle && middle <= last;
}

static void search(Module *m) {
    static const unsigned char signature[4] = {0x01, 0x1b, 0x03, 0x3b};
    m->searched = 1;
    if (m->size < 16) return;
    for (uint64_t at = 0; at + 12 <= m->size; at += 4) {
        const unsigned char *header = m->start + at;
        if (memcmp(header, signature, 4) || !header_ok(m, header)) continue;
        m->frame_header = header;
        m->frame_header_size = 12 + read32(header + 8) * 8;
        m->frames = header + 4 + (int32_t)read32(header + 4);
        const unsigned char *p = m->frames;
        while (inside(m, p, 4)) {
            uint32_t length = read32(p);
            if (!length) {
                p += 4;
                break;
            }
            if (length == 0xffffffffu || !inside(m, p, (uint64_t)length + 4)) break;
            p += (uint64_t)length + 4;
        }
        m->frames_size = (uint32_t)(p - m->frames);
        return;
    }
}

static Module *module_at(uint64_t address) {
    for (unsigned i = 0; i < module_total; ++i) {
        Module *m = &modules[i];
        if (address >= (uint64_t)(uintptr_t)m->start && address - (uint64_t)(uintptr_t)m->start < m->size) {
            if (!m->searched) search(m);
            return m;
        }
    }
    return NULL;
}

static void put32(unsigned char *out, uint32_t value) {
    memcpy(out, &value, 4);
}

static void put64(unsigned char *out, uint64_t value) {
    memcpy(out, &value, 8);
}

static void note(uint64_t address, const Module *m) {
    if (lookups > 6) return;
    if (lookups == 1)
        printf("Runtime: the game raised a C++ exception; its error handling looks up the code on the stack:\n");
    if (m)
        printf("Runtime:   %s+0x%llx%s\n", m->name, (unsigned long long)(address - (uint64_t)(uintptr_t)m->start),
               m->frame_header ? "" : " (no unwind table found in this module)");
    else
        printf("Runtime:   0x%llx, outside every module\n", (unsigned long long)address);
    fflush(stdout);
}

static SYS_ABI int32_t module_info_from_address(uint64_t address, int32_t flags, unsigned char *info) {
    (void)flags;
    ++lookups;
    if (!info) return E_FAULT;
    Module *m = module_at(address);
    note(address, m);
    if (!m) {
        ++misses;
        return E_SEARCH;
    }
    uint64_t wanted;
    memcpy(&wanted, info, 8);
    unsigned char full[INFO_SIZE];
    memset(full, 0, sizeof(full));
    put64(full, wanted >= 0x160 && wanted <= INFO_SIZE ? wanted : INFO_SIZE);
    snprintf((char *)full + 8, 256, "%s", m->name);
    put32(full + 0x108, (uint32_t)(m - modules));
    put64(full + 0x128, m->init);
    put64(full + 0x148, (uint64_t)(uintptr_t)m->frame_header);
    put64(full + 0x150, (uint64_t)(uintptr_t)m->frames);
    put32(full + 0x158, m->frame_header_size);
    put32(full + 0x15c, m->frames_size);
    put64(full + 0x160, (uint64_t)(uintptr_t)m->start);
    put32(full + 0x168, (uint32_t)m->size);
    put32(full + 0x16c, 5);
    put32(full + 0x1a0, 1);
    put32(full + 0x1a4, 1);
    uint64_t size;
    memcpy(&size, full, 8);
    memcpy(info, full, size);
    return 0;
}

static SYS_ABI int32_t module_info_for_unwind(uint64_t address, int32_t flags, unsigned char *info) {
    (void)flags;
    ++lookups;
    if (!info) return E_FAULT;
    Module *m = module_at(address);
    note(address, m);
    if (!m) {
        ++misses;
        return E_SEARCH;
    }
    unsigned char full[0x130];
    memset(full, 0, sizeof(full));
    put64(full, sizeof(full));
    snprintf((char *)full + 8, 256, "%s", m->name);
    put64(full + 0x108, (uint64_t)(uintptr_t)m->frame_header);
    put64(full + 0x110, (uint64_t)(uintptr_t)m->frames);
    put64(full + 0x118, m->frames_size);
    put64(full + 0x120, (uint64_t)(uintptr_t)m->start);
    put64(full + 0x128, m->size);
    memcpy(info, full, sizeof(full));
    return 0;
}

void sys_module_report(void) {
    if (!lookups) return;
    printf("Runtime: the game's error handling asked for module information %llu times (%llu for an address outside "
           "every module)\n",
           (unsigned long long)lookups, (unsigned long long)misses);
}

static const SysExport module_exports[] = {
    {"f7KBOafysXo", "sceKernelGetModuleInfoFromAddr", module_info_from_address},
    {"RpQJJVKTiFM", "sceKernelGetModuleInfoForUnwind", module_info_for_unwind},
};
SYS_LIBRARY(sys_module_info, "libkernel", module_exports);
