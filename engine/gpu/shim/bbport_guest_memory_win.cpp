// SPDX-License-Identifier: GPL-3.0-or-later
#ifdef _WIN32
#include "bbport_guest_memory.h"

#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <windows.h>
#include "video_core/renderer_vulkan/vk_instance.h"

extern "C" unsigned char* runtime_memory_backing(uint64_t* direct_size);
extern "C" int win_mem_commit(uint64_t phys, uint64_t size);

namespace BbGuestMemory {
namespace {
using u64 = std::uint64_t;
constexpr u64 ChunkSize = u64(256) << 20;
constexpr std::size_t MaxChunks = 256;

const Vulkan::Instance* vk_instance = nullptr;
std::uint32_t import_types = 0;
std::mutex mutex;
std::array<Chunk*, MaxChunks> chunks{};
std::array<bool, MaxChunks> failed{};
std::atomic<u64> chunk_bytes{0};

std::uint32_t PickType(std::uint32_t bits) {
    const auto props = vk_instance->GetPhysicalDevice().getMemoryProperties();
    for (std::uint32_t i = 0; i < props.memoryTypeCount; ++i) {
        if ((bits & (1u << i)) &&
            (props.memoryTypes[i].propertyFlags & vk::MemoryPropertyFlagBits::eHostVisible)) {
            return i;
        }
    }
    return ~0u;
}

bool Import(void* pointer, u64 size, vk::Buffer& buffer, vk::DeviceMemory& memory,
            const char** failed_step) {
    const vk::Device dev = vk_instance->GetDevice();
    const auto [props_result, props] = dev.getMemoryHostPointerPropertiesEXT(
        vk::ExternalMemoryHandleTypeFlagBits::eHostAllocationEXT, pointer);
    const std::uint32_t type = props_result == vk::Result::eSuccess
                                   ? PickType(props.memoryTypeBits & (import_types ? import_types : ~0u))
                                   : ~0u;
    if (type == ~0u) {
        *failed_step = "the driver's host pointer check";
        return false;
    }
    const vk::ExternalMemoryBufferCreateInfo external{
        .handleTypes = vk::ExternalMemoryHandleTypeFlagBits::eHostAllocationEXT,
    };
    const auto [buffer_result, made] = dev.createBuffer({
        .pNext = &external,
        .size = size,
        .usage = ChunkUsage,
        .sharingMode = vk::SharingMode::eExclusive,
    });
    if (buffer_result != vk::Result::eSuccess) {
        *failed_step = "buffer creation";
        return false;
    }
    const vk::ImportMemoryHostPointerInfoEXT import_info{
        .handleType = vk::ExternalMemoryHandleTypeFlagBits::eHostAllocationEXT,
        .pHostPointer = pointer,
    };
    const auto [memory_result, imported] = dev.allocateMemory({
        .pNext = &import_info,
        .allocationSize = size,
        .memoryTypeIndex = type,
    });
    if (memory_result != vk::Result::eSuccess) {
        dev.destroyBuffer(made);
        *failed_step = "the import";
        return false;
    }
    if (dev.bindBufferMemory(made, imported, 0) != vk::Result::eSuccess) {
        dev.freeMemory(imported);
        dev.destroyBuffer(made);
        *failed_step = "binding";
        return false;
    }
    buffer = made;
    memory = imported;
    return true;
}
}

vk::ExternalMemoryHandleTypeFlagBits ChunkHandleType() {
    return vk::ExternalMemoryHandleTypeFlagBits::eHostAllocationEXT;
}

bool PcModelGpu(const Vulkan::Instance& instance) {
    static const bool ok = [&] {
        if (instance.GetVendorID() == 0x1002 && !instance.IsVertexInputDynamicState()) {
            const char* force = std::getenv("BB_GUEST_IN_PLACE_LEGACY_AMD");
            if (!(force && force[0] == '1')) {
                std::printf("Guest memory: AMD's legacy driver (Polaris, Vega, older APUs) draws "
                            "wrong lights and vertices with the PC memory model "
                            "(BB_GUEST_IN_PLACE_LEGACY_AMD=1 keeps it on)\n");
                return false;
            }
        }
        const char* any = std::getenv("BB_PC_MODEL_ANY_GPU");
        if (instance.GetVendorID() != 0x10de && !(any && any[0] == '1')) {
            std::printf("Guest memory: the PC memory model stays off on this GPU (vendor 0x%04x): players "
                        "with AMD and Intel GPUs reported broken lighting and a falling frame rate with "
                        "it (BB_PC_MODEL_ANY_GPU=1 turns it on)\n",
                        instance.GetVendorID());
            return false;
        }
        if (const char* user = std::getenv("BB_GPU_USER_DIR"); user && *user && !(any && any[0] == '1')) {
            if (std::FILE* mark = std::fopen((std::string(user) + "/pc-model-hung").c_str(), "r")) {
                std::fclose(mark);
                std::printf("Guest memory: the PC memory model stays off on this PC: the GPU stopped "
                            "answering with it in an earlier session (delete user\\pc-model-hung or set "
                            "BB_PC_MODEL_ANY_GPU=1 to try it again)\n");
                return false;
            }
        }
        return true;
    }();
    return ok;
}

bool Usable(const Vulkan::Instance& instance) {
    static const bool usable = [&] {
        if (!instance.IsGuestMemoryImportSupported()) {
            std::printf("Guest memory: the driver cannot import system memory "
                        "(VK_EXT_external_memory_host)\n");
            return false;
        }
        vk_instance = &instance;
        constexpr u64 Size = 2 << 20;
        HANDLE section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                            DWORD(Size), nullptr);
        void* view = section ? MapViewOfFile(section, FILE_MAP_ALL_ACCESS, 0, 0, Size) : nullptr;
        vk::Buffer buffer;
        vk::DeviceMemory memory;
        const char* step = "mapping a test section";
        const bool imported = view && Import(view, Size, buffer, memory, &step);
        if (imported) {
            const vk::Device dev = instance.GetDevice();
            const auto [arena_result, arena] = dev.createBuffer({
                .flags = vk::BufferCreateFlagBits::eSparseBinding |
                         vk::BufferCreateFlagBits::eSparseResidency,
                .size = u64(1) << 30,
                .usage = ChunkUsage,
                .sharingMode = vk::SharingMode::eExclusive,
            });
            if (arena_result == vk::Result::eSuccess) {
                const auto arena_types = dev.getBufferMemoryRequirements(arena).memoryTypeBits;
                const auto [props_result, props] = dev.getMemoryHostPointerPropertiesEXT(
                    vk::ExternalMemoryHandleTypeFlagBits::eHostAllocationEXT, view);
                import_types = props_result == vk::Result::eSuccess ? props.memoryTypeBits : 0;
                if ((arena_types & import_types) == 0) {
                    std::printf("Guest memory: the arena lists no memory type a host import may "
                                "use (0x%x, imports 0x%x): its blocks are bound to imports anyway, "
                                "outside the Vulkan spec\n",
                                arena_types, import_types);
                }
                dev.destroyBuffer(arena);
            }
            dev.destroyBuffer(buffer);
            dev.freeMemory(memory);
        } else {
            std::printf("Guest memory: importing a section view failed at %s\n", step);
            vk_instance = nullptr;
        }
        if (view) {
            UnmapViewOfFile(view);
        }
        if (section) {
            CloseHandle(section);
        }
        return imported;
    }();
    return usable;
}

void Install(const Vulkan::Instance& instance) {
    const char* in_place = std::getenv("BB_GUEST_IN_PLACE");
    if (!(in_place && in_place[0] == '1') || !PcModelGpu(instance) || !Usable(instance)) {
        return;
    }
    std::printf("Guest memory: the GPU imports the game's memory in %llu MiB chunks as it uses "
                "them (VK_EXT_external_memory_host)\n",
                (unsigned long long)(ChunkSize >> 20));
}

namespace {
std::array<bool, MaxChunks> queued{};

void ImportChunk(u64 n) {
    u64 direct_size = 0;
    unsigned char* backing = runtime_memory_backing(&direct_size);
    const u64 size = std::min(ChunkSize, direct_size - n * ChunkSize);
    vk::Buffer buffer;
    vk::DeviceMemory memory;
    const char* step = "committing its pages";
    if (win_mem_commit(n * ChunkSize, size) != 0 ||
        !Import(backing + n * ChunkSize, size, buffer, memory, &step)) {
        {
            std::scoped_lock lk{mutex};
            failed[n] = true;
        }
        static std::atomic<int> reports{0};
        if (reports++ < 4) {
            std::fprintf(stderr, "Guest memory: importing direct memory %#llx+%llu MiB failed at "
                         "%s; it stays copied as before\n",
                         (unsigned long long)(n * ChunkSize), (unsigned long long)(size >> 20), step);
        }
        return;
    }
    auto* chunk = new Chunk{n * ChunkSize, size, buffer, memory, 0};
    {
        std::scoped_lock lk{mutex};
        static std::uint32_t next_index = 0;
        chunk->index = next_index++;
        chunks[n] = chunk;
    }
    const u64 total = chunk_bytes.fetch_add(size) + size;
    std::printf("Guest memory: direct memory %#llx+%llu MiB imported for the GPU, %llu MiB so far\n",
                (unsigned long long)(n * ChunkSize), (unsigned long long)(size >> 20),
                (unsigned long long)(total >> 20));
}
}

const Chunk* Find(std::uint64_t phys) {
    if (!vk_instance) {
        return nullptr;
    }
    const u64 n = phys / ChunkSize;
    if (n >= MaxChunks) {
        return nullptr;
    }
    std::scoped_lock lk{mutex};
    if (chunks[n] || failed[n] || queued[n]) {
        return chunks[n];
    }
    u64 direct_size = 0;
    if (!runtime_memory_backing(&direct_size) || n * ChunkSize >= direct_size) {
        return nullptr;
    }
    queued[n] = true;
    std::thread([n] { ImportChunk(n); }).detach();
    return nullptr;
}
}
#endif
