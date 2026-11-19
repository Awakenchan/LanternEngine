// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif

namespace EbootImage {
constexpr std::size_t kPage = 0x4000;
inline constexpr const char* kEbootSha[] = {"941f887a562aae054fac35af8cc8f27cf075f3d4cc2e029fb5ae2a663aaa5ae7",
                                            "cec1b276e7f9e4db978e57f524f41fbaac594530a3437b002e23f3fab14b4f86"};

inline std::uint8_t* MapImage(const std::vector<std::uint8_t>& elf, std::size_t& size, bool executable = false) {
    auto load = [&](std::size_t at, auto* out) {
        if (at + sizeof(*out) > elf.size()) return false;
        std::memcpy(out, elf.data() + at, sizeof(*out));
        return true;
    };
    std::uint64_t phoff = 0;
    std::uint16_t entsize = 0, count = 0;
    if (!load(32, &phoff) || !load(54, &entsize) || !load(56, &count) || entsize != 56 || !count) return nullptr;
    struct Load { std::uint64_t offset, va, filesz, memsz; };
    std::vector<Load> loads;
    for (std::uint16_t i = 0; i < count; ++i) {
        const std::size_t at = phoff + std::size_t(i) * 56;
        std::uint32_t type = 0;
        Load l{};
        if (!load(at, &type) || !load(at + 8, &l.offset) || !load(at + 16, &l.va) || !load(at + 32, &l.filesz) ||
            !load(at + 40, &l.memsz))
            return nullptr;
        if (type != 1) continue;
        if (l.filesz > l.memsz || l.offset + l.filesz > elf.size()) return nullptr;
        loads.push_back(l);
    }
    size = 0;
    for (const Load& l : loads) size = std::max<std::size_t>(size, l.va + l.memsz);
    size = (size + kPage - 1) / kPage * kPage;
    if (!size) return nullptr;
#ifdef _WIN32
    auto* image = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, size, MEM_COMMIT | MEM_RESERVE, executable ? PAGE_EXECUTE_READWRITE : PAGE_READWRITE));
#else
    void* mapped = mmap(nullptr, size, PROT_READ | PROT_WRITE | (executable ? PROT_EXEC : 0), MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    auto* image = mapped == MAP_FAILED ? nullptr : static_cast<std::uint8_t*>(mapped);
#endif
    if (!image) return nullptr;
    for (const Load& l : loads) std::memcpy(image + l.va, elf.data() + l.offset, l.filesz);
    return image;
}

inline std::size_t Relocate(const std::vector<std::uint8_t>& elf, std::uint8_t* image, std::size_t size) {
    constexpr std::uint32_t kDynamic = 2, kDynlib = 0x61000000;
    constexpr std::int64_t kRelaOffset = 0x6100002f, kRelaSize = 0x61000031;
    constexpr std::uint32_t kRelative = 8;
    constexpr std::size_t kTags = 4096, kRela = 24;
    auto load = [&](std::size_t at, auto* out) {
        if (at + sizeof(*out) > elf.size()) return false;
        std::memcpy(out, elf.data() + at, sizeof(*out));
        return true;
    };
    std::uint64_t phoff = 0, dynamic = 0, dynlib = 0;
    std::uint16_t count = 0;
    if (!load(32, &phoff) || !load(56, &count)) return 0;
    for (std::uint16_t i = 0; i < count; ++i) {
        std::uint32_t type = 0;
        std::uint64_t offset = 0;
        if (!load(phoff + std::size_t(i) * 56, &type) || !load(phoff + std::size_t(i) * 56 + 8, &offset)) return 0;
        if (type == kDynamic) dynamic = offset;
        if (type == kDynlib) dynlib = offset;
    }
    if (!dynamic || !dynlib) return 0;
    std::uint64_t rela = 0, rela_size = 0;
    for (std::size_t i = 0; i < kTags; ++i) {
        std::int64_t tag = 0;
        std::uint64_t value = 0;
        if (!load(dynamic + 16 * i, &tag) || !load(dynamic + 16 * i + 8, &value) || tag == 0) break;
        if (tag == kRelaOffset && !rela) rela = value;
        if (tag == kRelaSize && !rela_size) rela_size = value;
    }
    std::size_t applied = 0;
    for (std::uint64_t at = dynlib + rela; rela_size && at + kRela <= dynlib + rela + rela_size; at += kRela) {
        std::uint64_t target = 0, info = 0, addend = 0;
        if (!load(at, &target) || !load(at + 8, &info) || !load(at + 16, &addend)) return applied;
        if ((info & 0xffffffffu) != kRelative || target + 8 > size) continue;
        const std::uint64_t value = reinterpret_cast<std::uint64_t>(image) + addend;
        std::memcpy(image + target, &value, sizeof value);
        ++applied;
    }
    return applied;
}

}
