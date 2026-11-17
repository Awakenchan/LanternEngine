// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <set>
#include <string>
#include <system_error>
#include <type_traits>

#include "bbport_settings.h"
#include "video_core/renderer_vulkan/vk_fsr_framegen.h"
#include "video_core/renderer_vulkan/vk_fsr_framegen_rules.h"
#include "video_core/renderer_vulkan/vk_fsr_sdk.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_platform.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "video_core/renderer_vulkan/vk_optiscaler.h"
#endif

namespace Vulkan {

bool FsrFrameGen::SelectedAtStart() {
#ifdef _WIN32
    return BbSettings::Get().startup_frame_gen == BbSettings::FrameGenFsr;
#else
    return false;
#endif
}

bool FsrFrameGen::FilesPresent() {
#ifdef _WIN32
    const char* setting = std::getenv("BB_FSR_SDK");
    if (setting && setting[0] == '0') {
        return false;
    }
    std::error_code error;
    const auto dir = FsrSdk::Directory();
    return std::filesystem::is_regular_file(dir / LoaderName, error) &&
           std::filesystem::is_regular_file(dir / DllName, error);
#else
    return false;
#endif
}

void FsrFrameGen::Fallback(const char* reason) {
    auto& s = BbSettings::Get();
    const bool was_running = s.fsr_fg_running.exchange(false, std::memory_order_relaxed);
    if (s.fsr_fg_problem.load(std::memory_order_relaxed) == reason && !was_running) {
        return;
    }
    s.fsr_fg_problem.store(reason, std::memory_order_relaxed);
    s.frame_gen_active.store(false, std::memory_order_relaxed);
    std::printf("AMD FSR Frame Generation: off for this session, the built-in FSR 3.1 frame "
                "generation instead: %s\n",
                reason);
    std::fflush(stdout);
}

#ifdef _WIN32

namespace {

using StructType = u64;
struct Header {
    StructType type;
    Header* next;
};
struct DeviceContext {
    VkDevice device;
    VkPhysicalDevice physical_device;
    PFN_vkGetDeviceProcAddr get_device_proc_addr;
};
struct BackendVk {
    Header header;
    DeviceContext device;
};
struct QueryVersions {
    Header header;
    u64 create_desc_type;
    void* device;
    u64* output_count;
    u64* version_ids;
    const char** version_names;
};
struct QueryProviderVersion {
    Header header;
    u64 version_id;
    const char* version_name;
};
struct VersionDesc {
    Header header;
    u32 version;
};
using SubmitFunction = VkResult (*)(u32 count, const VkSubmitInfo* submits, VkFence fence);
struct QueueInfo {
    VkQueue queue;
    u32 family;
    SubmitFunction submit;
};
struct CreateSwapchainVk {
    Header header;
    VkPhysicalDevice physical_device;
    VkDevice device;
    VkSwapchainKHR* swapchain;
    const VkAllocationCallbacks* allocator;
    VkSwapchainCreateInfoKHR create_info;
    QueueInfo game, async_compute, present, image_acquire;
};
using LastPresentCount = u64 (*)(VkSwapchainKHR);
using CreateSwapchainFfx = VkResult (*)(VkDevice, const VkSwapchainCreateInfoKHR*,
                                        const VkAllocationCallbacks*, VkSwapchainKHR*, void*);
using DestroySwapchainFfx = void (*)(VkDevice, VkSwapchainKHR, const VkAllocationCallbacks*, void*);
struct QueryReplacements {
    Header header;
    CreateSwapchainFfx create;
    DestroySwapchainFfx destroy;
    PFN_vkGetSwapchainImagesKHR get_images;
    PFN_vkAcquireNextImageKHR acquire;
    PFN_vkQueuePresentKHR present;
    PFN_vkSetHdrMetadataEXT set_hdr_metadata;
    LastPresentCount last_present_count;
};
struct WaitForPresents {
    Header header;
};
struct Dimensions {
    u32 width, height;
};
struct Float2 {
    float x, y;
};
struct Rect {
    s32 left, top, width, height;
};
struct ResourceDescription {
    u32 type, format, width, height, depth, mip_count, flags, usage;
};
struct Resource {
    void* resource;
    ResourceDescription description;
    u32 state;
};
struct CreateFrameGen {
    Header header;
    u32 flags;
    Dimensions display_size, max_render_size;
    u32 back_buffer_format;
};
using FrameGenCallback = u32 (*)(Header* params, void* user);
struct ConfigureFrameGen {
    Header header;
    void* swapchain;
    void* present_callback;
    void* present_user;
    FrameGenCallback frame_generation_callback;
    void* frame_generation_user;
    bool enabled;
    bool allow_async;
    Resource hudless;
    u32 flags;
    bool only_present_generated;
    Rect generation_rect;
    u64 frame_id;
};
struct PrepareV2 {
    Header header;
    u64 frame_id;
    u32 flags;
    void* command_list;
    Dimensions render_size;
    Float2 jitter_offset, motion_vector_scale;
    float frame_time_delta;
    bool reset;
    float camera_near, camera_far, camera_fov_vertical, view_space_to_meters;
    Resource depth, motion_vectors;
    float camera_position[3], camera_up[3], camera_right[3], camera_forward[3];
};
static_assert(sizeof(Resource) == 48 && sizeof(CreateFrameGen) == 40);
static_assert(offsetof(ConfigureFrameGen, hudless) == 64 && offsetof(ConfigureFrameGen, flags) == 112 &&
              offsetof(ConfigureFrameGen, generation_rect) == 120 &&
              offsetof(ConfigureFrameGen, frame_id) == 136 && sizeof(ConfigureFrameGen) == 144);
static_assert(offsetof(PrepareV2, command_list) == 32 && offsetof(PrepareV2, frame_time_delta) == 64 &&
              offsetof(PrepareV2, reset) == 68 && offsetof(PrepareV2, depth) == 88 &&
              offsetof(PrepareV2, motion_vectors) == 136 && offsetof(PrepareV2, camera_position) == 184 &&
              offsetof(PrepareV2, camera_forward) == 220 && sizeof(PrepareV2) == 232);
static_assert(sizeof(BackendVk) == 40 && sizeof(QueryVersions) == 56 && sizeof(VersionDesc) == 24);
static_assert(sizeof(QueueInfo) == 24 && offsetof(CreateSwapchainVk, create_info) == 48);
static_assert(sizeof(CreateSwapchainVk) == 48 + sizeof(VkSwapchainCreateInfoKHR) + 4 * 24);
static_assert(sizeof(QueryReplacements) == 72);

constexpr StructType QueryVersionsType = 4u;
constexpr StructType QueryProviderVersionType = 6u;
constexpr StructType BackendVkType = 0x01000003u;
constexpr StructType CreateFrameGenType = 0x00020001u;
constexpr StructType CreateSwapchainVkType = 0x00040001u;
constexpr StructType QueryReplacementsType = 0x00040005u;
constexpr StructType WaitForPresentsType = 0x00040007u;
constexpr StructType SwapchainVersionType = 0x0004000au;
constexpr StructType ConfigureFrameGenType = 0x00020002u;
constexpr StructType PrepareV2Type = 0x0002000cu;
constexpr StructType FrameGenVersionType = 0x0002000eu;
constexpr u32 SwapchainApiVersion = 4u << 22;
constexpr u32 FrameGenApiVersion = (4u << 22) | 2u;
constexpr u32 FlagAsyncWorkload = 1u << 0;
constexpr u32 StateCommon = 1u;
constexpr u32 UsageUav = 1u << 1;
constexpr u32 ResourceTexture2d = 2u;
constexpr u32 FormatR32Float = 28, FormatRg16Float = 18;
constexpr float FarPlane = 3000.0f;
constexpr size_t HudlessImages = 3;
constexpr size_t Slots = 16;

u32 FfxFormat(vk::Format format) {
    switch (format) {
    case vk::Format::eR16G16B16A16Sfloat:
        return 4;
    case vk::Format::eR8G8B8A8Unorm:
        return 10;
    case vk::Format::eR8G8B8A8Srgb:
        return 12;
    case vk::Format::eB8G8R8A8Unorm:
        return 14;
    case vk::Format::eB8G8R8A8Srgb:
        return 15;
    case vk::Format::eA2B10G10R10UnormPack32:
        return 17;
    default:
        return 0;
    }
}

Resource Image2d(vk::Image image, u32 format, u32 width, u32 height, u32 usage) {
    return {VkImage(image), {ResourceTexture2d, format, width, height, 1, 1, 0, usage}, StateCommon};
}
constexpr u32 ReturnOk = 0;

using CreateContext = u32 (*)(void** context, Header* desc, const void* callbacks);
using DestroyContext = u32 (*)(void** context, const void* callbacks);
using Configure = u32 (*)(void** context, const Header* desc);
using Query = u32 (*)(void** context, Header* desc);
using Dispatch = u32 (*)(void** context, const Header* desc);

const char* ReturnName(u32 code) {
    static constexpr std::array<const char*, 8> names{
        "ok",          "error",         "unknown descriptor type", "runtime error",
        "no provider", "out of memory", "invalid parameter",       "descriptor type too new"};
    return code < names.size() ? names[code] : "unknown";
}

VkQueue game_queue = VK_NULL_HANDLE;
Dispatch dispatch_function = nullptr;

u32 DispatchFrameGeneration(Header* params, void* user) {
    return dispatch_function(static_cast<void**>(user), params);
}
thread_local bool holding_submit = false;

VkResult SubmitToGameQueue(u32 count, const VkSubmitInfo* submits, VkFence fence) {
    if (holding_submit) {
        return VULKAN_HPP_DEFAULT_DISPATCHER.vkQueueSubmit(game_queue, count, submits, fence);
    }
    std::scoped_lock lock{Scheduler::submit_mutex};
    return VULKAN_HPP_DEFAULT_DISPATCHER.vkQueueSubmit(game_queue, count, submits, fence);
}

struct SubmitHold {
    std::unique_lock<std::mutex> lock;
    explicit SubmitHold(bool take) {
        if (take) {
            lock = std::unique_lock{Scheduler::submit_mutex};
            holding_submit = true;
        }
    }
    ~SubmitHold() {
        if (lock.owns_lock()) {
            holding_submit = false;
        }
    }
};

const char* Kept(const std::string& text) {
    static std::mutex mutex;
    static std::set<std::string> kept;
    std::scoped_lock lock{mutex};
    return kept.insert(text).first->c_str();
}

std::string Utf8(const std::filesystem::path& path) {
    const auto u8 = path.u8string();
    return {reinterpret_cast<const char*>(u8.data()), u8.size()};
}

}

struct FsrFrameGen::Impl {
    const Instance& instance;
    std::mutex amd_mutex;
    HMODULE module{};
    CreateContext create{};
    DestroyContext destroy{};
    Configure configure{};
    Query query{};
    Dispatch dispatch{};
    bool failed{};
    bool versions_logged{};
    void* swapchain_context{};
    void* fg_context{};
    QueryReplacements functions{};
    VkSwapchainKHR swapchain{};
    std::string version_text, swapchain_version;
    struct Slot {
        VideoCore::UniqueImage depth, motion;
        u32 width = 0, height = 0;
    };
    std::array<Slot, Slots> slots;
    std::array<VideoCore::UniqueImage, HudlessImages> hudless;
    vk::Format hudless_format{};
    vk::Extent2D hudless_extent{};
    vk::Format fg_format{};
    vk::Extent2D fg_display{}, fg_max_render{};
    bool async_queue{};
    FsrFgRules::FrameState state;
    const char* said = "";
    std::chrono::steady_clock::time_point last_game_frame{};
    double interval_ms = 0.0;
    float frame_ms = 16.6f;
    u64 presents = 0, count_base = 0;
    double shown_per_frame = 1.0;
    bool configure_error_logged{}, prepare_error_logged{}, inputs_logged{};

    explicit Impl(const Instance& instance_) : instance{instance_} {}

    ~Impl() {
        End();
        if (module) {
            FreeLibrary(module);
        }
    }

    DeviceContext Device() const {
        return {VkDevice(instance.GetDevice()), VkPhysicalDevice(instance.GetPhysicalDevice()),
                VULKAN_HPP_DEFAULT_DISPATCHER.vkGetDeviceProcAddr};
    }

    bool Load() {
        if (module) {
            return true;
        }
        const auto dir = FsrSdk::Directory();
        const auto own = OptiScaler::PrivateFfxLibrary(dir / DllName, "lantern_fsr_fg_vk.dll");
        if (!own.empty()) {
            module = LoadLibraryExW(own.c_str(), nullptr,
                                    LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
            if (module) {
                std::printf("AMD FSR Frame Generation: runs from its own copy, past OptiScaler's FSR "
                            "hook (BB_OPTISCALER_FFX_PRIVATE)\n");
            }
        }
        const auto loader = dir / LoaderName;
        if (!module) {
            module = LoadLibraryExW(loader.c_str(), nullptr,
                                    LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        }
        if (!module) {
            std::printf("AMD FSR Frame Generation: cannot load %s (error %lu)\n", Utf8(loader).c_str(),
                        static_cast<unsigned long>(GetLastError()));
            return false;
        }
        bool complete = true;
        const auto load = [&](auto& function, const char* name) {
            function = reinterpret_cast<std::remove_reference_t<decltype(function)>>(
                GetProcAddress(module, name));
            complete = complete && function;
        };
        load(create, "ffxCreateContext");
        load(destroy, "ffxDestroyContext");
        load(configure, "ffxConfigure");
        load(query, "ffxQuery");
        load(dispatch, "ffxDispatch");
        dispatch_function = dispatch;
        if (!complete) {
            std::printf("AMD FSR Frame Generation: %s is not the AMD FSR API\n", Utf8(loader).c_str());
            FreeLibrary(module);
            module = nullptr;
            return false;
        }
        return true;
    }

    std::string Versions(StructType type) {
        DeviceContext device = Device();
        u64 count = 0;
        QueryVersions versions{{QueryVersionsType, nullptr}, type, &device, &count, nullptr, nullptr};
        if (query(nullptr, &versions.header) != ReturnOk || count == 0) {
            return "none";
        }
        std::vector<u64> ids(count);
        std::vector<const char*> names(count, nullptr);
        versions.version_ids = ids.data();
        versions.version_names = names.data();
        if (query(nullptr, &versions.header) != ReturnOk) {
            return "none";
        }
        std::string list;
        for (u64 i = 0; i < count; ++i) {
            list += (i ? ", " : "") + std::string{names[i] ? names[i] : "?"};
        }
        return list;
    }

    void LogVersions() {
        if (versions_logged) {
            return;
        }
        versions_logged = true;
        std::printf("AMD FSR Frame Generation: versions for this GPU: frame generation %s; swapchain %s "
                    "(' *': from the driver)\n",
                    Versions(CreateFrameGenType).c_str(), Versions(CreateSwapchainVkType).c_str());
    }

    std::string ProviderName(void** context) {
        QueryProviderVersion name{{QueryProviderVersionType, nullptr}, 0, nullptr};
        std::string shown = query(context, &name.header) == ReturnOk && name.version_name ? name.version_name : "";
        shown.erase(0, shown.find_first_not_of(' '));
        return shown.empty() ? "?" : shown;
    }

    bool CreateSwapchain(const vk::SwapchainCreateInfoKHR& info, vk::SwapchainKHR& out) {
        std::scoped_lock lock{amd_mutex};
        if (failed || !Load()) {
            return false;
        }
        LogVersions();
        const auto& queues = instance.GetFsrFgQueues();
        game_queue = VkQueue(instance.GetGraphicsQueue());
        VkSwapchainKHR made = VK_NULL_HANDLE;
        CreateSwapchainVk desc{};
        desc.header.type = CreateSwapchainVkType;
        desc.physical_device = VkPhysicalDevice(instance.GetPhysicalDevice());
        desc.device = VkDevice(instance.GetDevice());
        desc.swapchain = &made;
        desc.create_info = static_cast<VkSwapchainCreateInfoKHR>(info);
        desc.create_info.oldSwapchain = VK_NULL_HANDLE;
        desc.game = {game_queue, instance.GetGraphicsQueueFamilyIndex(), SubmitToGameQueue};
        desc.async_compute = {VkQueue(queues.async),
                              queues.async ? queues.async_family : instance.GetGraphicsQueueFamilyIndex(),
                              nullptr};
        desc.present = {VkQueue(queues.present), queues.present_family, nullptr};
        desc.image_acquire = {VkQueue(queues.acquire), queues.acquire_family, nullptr};
        VersionDesc version{{SwapchainVersionType, nullptr}, SwapchainApiVersion};
        desc.header.next = &version.header;
        u32 result = create(&swapchain_context, &desc.header, nullptr);
        if (result != ReturnOk) {
            swapchain_context = nullptr;
            desc.header.next = nullptr;
            made = VK_NULL_HANDLE;
            result = create(&swapchain_context, &desc.header, nullptr);
        }
        if (result != ReturnOk || !made) {
            swapchain_context = nullptr;
            std::printf("AMD FSR Frame Generation: AMD's swapchain was not made: %s (%u)\n",
                        ReturnName(result), result);
            return false;
        }
        functions = {};
        functions.header.type = QueryReplacementsType;
        if (query(&swapchain_context, &functions.header) != ReturnOk || !functions.get_images ||
            !functions.acquire || !functions.present || !functions.destroy) {
            std::printf("AMD FSR Frame Generation: AMD's swapchain gave no replacement functions\n");
            destroy(&swapchain_context, nullptr);
            swapchain_context = nullptr;
            return false;
        }
        swapchain = made;
        out = vk::SwapchainKHR(made);
        async_queue = bool(queues.async);
        FsrFgRules::ContextRemade(state);
        const std::string name = ProviderName(&swapchain_context);
        swapchain_version = name;
        version_text = "AMD FSR Frame Generation swapchain " + name;
        BbSettings::Get().fsr_fg_version.store(Kept(version_text), std::memory_order_relaxed);
        BbSettings::Get().fsr_fg_running.store(true, std::memory_order_relaxed);
        BbSettings::Get().fsr_fg_problem.store(nullptr, std::memory_order_relaxed);
        std::printf("AMD FSR Frame Generation: AMD's swapchain %ux%u in place (swapchain %s), %s "
                    "async compute queue\n",
                    info.imageExtent.width, info.imageExtent.height, name.c_str(),
                    queues.async ? "with an" : "without an");
        std::fflush(stdout);
        return true;
    }

    void End() {
        std::scoped_lock lock{amd_mutex};
        if (!swapchain_context) {
            return;
        }
        {
            SubmitHold hold{true};
            WaitForPresents wait{{WaitForPresentsType, nullptr}};
            dispatch(&swapchain_context, &wait.header);
            if (fg_context) {
                destroy(&fg_context, nullptr);
                fg_context = nullptr;
            }
            destroy(&swapchain_context, nullptr);
            swapchain_context = nullptr;
            functions.destroy(VkDevice(instance.GetDevice()), swapchain, nullptr, nullptr);
        }
        swapchain = VK_NULL_HANDLE;
        FsrFgRules::ContextRemade(state);
        BbSettings::Get().frame_gen_active.store(false, std::memory_order_relaxed);
        std::printf("AMD FSR Frame Generation: AMD's swapchain ended\n");
    }

    void DrainPresents() {
        WaitForPresents wait{{WaitForPresentsType, nullptr}};
        dispatch(&swapchain_context, &wait.header);
    }

    bool EnsureContext(vk::Format format, vk::Extent2D extent, u32 render_width, u32 render_height) {
        if (fg_context && fg_format == format && fg_display == extent &&
            render_width <= fg_max_render.width && render_height <= fg_max_render.height) {
            return true;
        }
        if (failed) {
            return false;
        }
        const u32 ffx_format = FfxFormat(format);
        if (!ffx_format) {
            std::printf("AMD FSR Frame Generation: back buffer format %s is not one AMD takes\n",
                        vk::to_string(format).c_str());
            failed = true;
            FsrFrameGen::Fallback("the back buffer format is not one AMD frame generation takes");
            return false;
        }
        if (fg_context) {
            SubmitHold hold{true};
            DrainPresents();
            destroy(&fg_context, nullptr);
            fg_context = nullptr;
            FsrFgRules::ContextRemade(state);
        }
        CreateFrameGen desc{};
        desc.header.type = CreateFrameGenType;
        desc.flags = async_queue ? FlagAsyncWorkload : 0u;
        desc.display_size = {extent.width, extent.height};
        desc.max_render_size = {std::max(extent.width, render_width), std::max(extent.height, render_height)};
        desc.back_buffer_format = ffx_format;
        VersionDesc version{{FrameGenVersionType, nullptr}, FrameGenApiVersion};
        BackendVk backend{{BackendVkType, nullptr}, Device()};
        desc.header.next = &version.header;
        version.header.next = &backend.header;
        u32 result = create(&fg_context, &desc.header, nullptr);
        if (result != ReturnOk) {
            fg_context = nullptr;
            desc.header.next = &backend.header;
            result = create(&fg_context, &desc.header, nullptr);
        }
        if (result != ReturnOk) {
            fg_context = nullptr;
            std::printf("AMD FSR Frame Generation: the frame generation context was not made: %s (%u)\n",
                        ReturnName(result), result);
            failed = true;
            FsrFrameGen::Fallback("the frame generation context could not be made");
            return false;
        }
        fg_format = format;
        fg_display = extent;
        fg_max_render = {desc.max_render_size.width, desc.max_render_size.height};
        const std::string name = ProviderName(&fg_context);
        version_text = "AMD FSR Frame Generation " + name + ", swapchain " + swapchain_version;
        BbSettings::Get().fsr_fg_version.store(Kept(version_text), std::memory_order_relaxed);
        std::printf("AMD FSR Frame Generation: frame generation %s, %ux%u, render sizes up to %ux%u%s\n",
                    name.c_str(), extent.width, extent.height, fg_max_render.width, fg_max_render.height,
                    async_queue ? ", async compute" : "");
        std::fflush(stdout);
        return true;
    }

    void EnsureHudless(vk::Format format, vk::Extent2D extent) {
        if (hudless[0] && hudless_format == format && hudless_extent == extent) {
            return;
        }
        if (hudless[0]) {
            SubmitHold hold{true};
            DrainPresents();
            (void)instance.GetGraphicsQueue().waitIdle();
        }
        const auto& queues = instance.GetFsrFgQueues();
        const std::array<u32, 2> families{instance.GetGraphicsQueueFamilyIndex(), queues.async_family};
        const bool shared = async_queue && queues.async_family != families[0];
        for (auto& image : hudless) {
            image = VideoCore::UniqueImage(instance.GetDevice(), instance.GetAllocator());
            image.Create(vk::ImageCreateInfo{
                .imageType = vk::ImageType::e2D,
                .format = format,
                .extent = {extent.width, extent.height, 1},
                .mipLevels = 1,
                .arrayLayers = 1,
                .samples = vk::SampleCountFlagBits::e1,
                .tiling = vk::ImageTiling::eOptimal,
                .usage = vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst |
                         vk::ImageUsageFlagBits::eTransferSrc,
                .sharingMode = shared ? vk::SharingMode::eConcurrent : vk::SharingMode::eExclusive,
                .queueFamilyIndexCount = shared ? u32(families.size()) : 0u,
                .pQueueFamilyIndices = shared ? families.data() : nullptr,
                .initialLayout = vk::ImageLayout::eUndefined,
            });
        }
        hudless_format = format;
        hudless_extent = extent;
    }

    void Say(const char* why) {
        const char* now = why ? why : "";
        if (now == said) {
            return;
        }
        said = now;
        std::printf("AMD FSR Frame Generation: %s%s\n", why ? "waiting: " : "generating frames", now);
        std::fflush(stdout);
    }

    void RecordHudless(vk::CommandBuffer cmdbuf, vk::Image source, vk::Extent2D frame_extent,
                       vk::Image target, vk::Extent2D extent, vk::Rect2D fit) {
        const auto all = vk::PipelineStageFlagBits2::eAllCommands;
        const auto barrier = [&](vk::Image image, vk::ImageLayout from, vk::ImageLayout to,
                                 vk::AccessFlags2 src, vk::AccessFlags2 dst) {
            return vk::ImageMemoryBarrier2{
                .srcStageMask = all,
                .srcAccessMask = src,
                .dstStageMask = all,
                .dstAccessMask = dst,
                .oldLayout = from,
                .newLayout = to,
                .image = image,
                .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
            };
        };
        const auto write = vk::AccessFlagBits2::eMemoryWrite;
        const auto read = vk::AccessFlagBits2::eMemoryRead;
        const std::array before{
            barrier(source, vk::ImageLayout::eGeneral, vk::ImageLayout::eTransferSrcOptimal, write, read),
            barrier(target, vk::ImageLayout::eUndefined, vk::ImageLayout::eTransferDstOptimal, {}, write),
        };
        cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = u32(before.size()), .pImageMemoryBarriers = before.data()});
        const bool covers = fit.offset.x == 0 && fit.offset.y == 0 && fit.extent == extent;
        if (!covers) {
            const vk::ClearColorValue black{std::array<float, 4>{0.0f, 0.0f, 0.0f, 1.0f}};
            const vk::ImageSubresourceRange range{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
            cmdbuf.clearColorImage(target, vk::ImageLayout::eTransferDstOptimal, black, range);
            const vk::MemoryBarrier2 cleared{.srcStageMask = all, .srcAccessMask = write,
                                             .dstStageMask = all, .dstAccessMask = write};
            cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &cleared});
        }
        const vk::ImageBlit blit{
            .srcSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
            .srcOffsets = std::array{vk::Offset3D{0, 0, 0},
                                     vk::Offset3D{s32(frame_extent.width), s32(frame_extent.height), 1}},
            .dstSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
            .dstOffsets = std::array{vk::Offset3D{fit.offset.x, fit.offset.y, 0},
                                     vk::Offset3D{fit.offset.x + s32(fit.extent.width),
                                                  fit.offset.y + s32(fit.extent.height), 1}},
        };
        cmdbuf.blitImage(source, vk::ImageLayout::eTransferSrcOptimal, target,
                         vk::ImageLayout::eTransferDstOptimal, blit, vk::Filter::eLinear);
        const std::array after{
            barrier(source, vk::ImageLayout::eTransferSrcOptimal, vk::ImageLayout::eGeneral, read, read),
            barrier(target, vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::eGeneral, write, read),
        };
        cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = u32(after.size()), .pImageMemoryBarriers = after.data()});
    }

    void RecordPresent(vk::CommandBuffer cmdbuf, const DlssFrameInfo* frame, vk::Image source,
                       vk::Extent2D frame_extent, vk::Format format, vk::Extent2D extent, vk::Rect2D fit,
                       bool menu_open, bool hdr) {
        std::scoped_lock lock{amd_mutex};
        if (!swapchain_context) {
            return;
        }
        const bool inputs = frame && frame->inputs && source;
        if (inputs) {
            const auto now = std::chrono::steady_clock::now();
            if (last_game_frame != std::chrono::steady_clock::time_point{}) {
                const double dt = std::chrono::duration<double, std::milli>(now - last_game_frame).count();
                if (dt > 1.0 && dt < 200.0) {
                    frame_ms = float(dt);
                    interval_ms = interval_ms > 0.0 ? interval_ms * 0.9 + dt * 0.1 : dt;
                }
            }
            last_game_frame = now;
        }
        FsrFgRules::Off off = FsrFgRules::WhyOff(true, inputs, menu_open, hdr, true);
        if (off == FsrFgRules::Off::None &&
            !EnsureContext(format, extent, frame->render_width, frame->render_height)) {
            off = FsrFgRules::Off::NoContext;
        }
        if (!fg_context) {
            Say(FsrFgRules::OffText(off == FsrFgRules::Off::None ? FsrFgRules::Off::NoContext : off));
            return;
        }
        const Slot& slot = slots[frame ? frame->slot % Slots : 0];
        if (off == FsrFgRules::Off::None &&
            (!slot.depth || slot.width != frame->render_width || slot.height != frame->render_height)) {
            off = FsrFgRules::Off::NoInputs;
        }
        const bool ready = off == FsrFgRules::Off::None;
        const FsrFgRules::FrameDecision decision = FsrFgRules::NextFrame(state, ready);
        vk::Image hudless_image{};
        if (decision.enabled) {
            EnsureHudless(format, extent);
            hudless_image = vk::Image(hudless[decision.frame_id % HudlessImages]);
            RecordHudless(cmdbuf, source, frame_extent, hudless_image, extent, fit);
        }
        ConfigureFrameGen config{};
        config.header.type = ConfigureFrameGenType;
        config.swapchain = swapchain;
        config.frame_generation_callback = DispatchFrameGeneration;
        config.frame_generation_user = &fg_context;
        config.enabled = decision.enabled;
        config.allow_async = async_queue;
        if (decision.enabled) {
            config.hudless = Image2d(hudless_image, FfxFormat(format), extent.width, extent.height, 0);
        }
        config.generation_rect = {fit.offset.x, fit.offset.y, s32(fit.extent.width), s32(fit.extent.height)};
        config.frame_id = decision.frame_id;
        u32 result;
        {
            SubmitHold hold{true};
            result = configure(&fg_context, &config.header);
        }
        if (result != ReturnOk) {
            if (!configure_error_logged) {
                configure_error_logged = true;
                std::printf("AMD FSR Frame Generation: configure failed: %s (%u)\n", ReturnName(result), result);
            }
            FsrFgRules::ContextRemade(state);
            Say("AMD's configure failed");
            return;
        }
        if (!decision.enabled) {
            Say(ready ? "the game frames have to run steadily first" : FsrFgRules::OffText(off));
            return;
        }
        const vk::MemoryBarrier2 inputs_done{
            .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .dstAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
        };
        cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &inputs_done});
        PrepareV2 prepare{};
        prepare.header.type = PrepareV2Type;
        prepare.frame_id = decision.frame_id;
        prepare.command_list = VkCommandBuffer(cmdbuf);
        prepare.render_size = {frame->render_width, frame->render_height};
        prepare.jitter_offset = {frame->jitter[0], frame->jitter[1]};
        prepare.motion_vector_scale = {1.0f, 1.0f};
        prepare.frame_time_delta = frame_ms;
        const auto& camera = frame->camera;
        prepare.reset = decision.reset || frame->reset || !camera.valid;
        prepare.camera_near = std::max(frame->near_plane, 0.001f);
        prepare.camera_far = std::max(FarPlane, prepare.camera_near * 2.0f);
        prepare.camera_fov_vertical = frame->vertical_fov;
        prepare.view_space_to_meters = 1.0f;
        prepare.depth = Image2d(vk::Image(slot.depth), FormatR32Float, slot.width, slot.height, UsageUav);
        prepare.motion_vectors = Image2d(vk::Image(slot.motion), FormatRg16Float, slot.width, slot.height, UsageUav);
        const auto& inv = camera.inv_view;
        const float up_sign = camera.proj[1] < 0.0f ? 1.0f : -1.0f;
        const std::array<float, 3> position{inv[3], inv[7], inv[11]};
        const std::array<float, 3> right{inv[0], inv[4], inv[8]};
        const std::array<float, 3> up{up_sign * inv[1], up_sign * inv[5], up_sign * inv[9]};
        const std::array<float, 3> forward{inv[2], inv[6], inv[10]};
        std::copy(position.begin(), position.end(), prepare.camera_position);
        std::copy(up.begin(), up.end(), prepare.camera_up);
        std::copy(right.begin(), right.end(), prepare.camera_right);
        std::copy(forward.begin(), forward.end(), prepare.camera_forward);
        if (!inputs_logged) {
            inputs_logged = true;
            std::printf("AMD FSR Frame Generation: first prepare: render %ux%u, jitter (%.4f, %.4f) as for "
                        "DLSS-G, motion vector scale (%.1f, %.1f), near %.3f far %.0f, vertical FOV %.4f, up "
                        "sign %+.0f\n",
                        prepare.render_size.width, prepare.render_size.height, prepare.jitter_offset.x,
                        prepare.jitter_offset.y, prepare.motion_vector_scale.x, prepare.motion_vector_scale.y,
                        prepare.camera_near, prepare.camera_far, prepare.camera_fov_vertical, up_sign);
            std::fflush(stdout);
        }
        result = dispatch(&fg_context, &prepare.header);
        cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &inputs_done});
        if (result != ReturnOk) {
            if (!prepare_error_logged) {
                prepare_error_logged = true;
                std::printf("AMD FSR Frame Generation: prepare failed: %s (%u)\n", ReturnName(result), result);
            }
            Say("AMD's prepare failed");
            return;
        }
        Say(nullptr);
    }

    void AfterPresent(bool game_frame) {
        std::scoped_lock lock{amd_mutex};
        auto& s = BbSettings::Get();
        if (!swapchain_context) {
            return;
        }
        s.frame_gen_multiplier.store(2, std::memory_order_relaxed);
        s.frame_gen_active.store(state.on, std::memory_order_relaxed);
        if (!game_frame) {
            return;
        }
        if (functions.last_present_count && ++presents % 60 == 0) {
            const u64 count = functions.last_present_count(swapchain);
            if (count_base && count > count_base) {
                shown_per_frame = double(count - count_base) / 60.0;
            }
            count_base = count ? count : 1;
        }
        if (interval_ms > 0.0) {
            const float game_fps = float(1000.0 / interval_ms);
            s.frame_gen_game_fps.store(game_fps, std::memory_order_relaxed);
            s.frame_gen_shown_fps.store(game_fps * float(std::max(shown_per_frame, 1.0)),
                                        std::memory_order_relaxed);
        }
    }

    void EnsureSlot(u32 index, u32 w, u32 h) {
        Slot& slot = slots[index % Slots];
        if (slot.width == w && slot.height == h && slot.depth) {
            return;
        }
        {
            std::scoped_lock lock{Scheduler::submit_mutex};
            (void)instance.GetGraphicsQueue().waitIdle();
        }
        const auto make = [&](VideoCore::UniqueImage& image, vk::Format format) {
            image = VideoCore::UniqueImage(instance.GetDevice(), instance.GetAllocator());
            image.Create(vk::ImageCreateInfo{
                .imageType = vk::ImageType::e2D,
                .format = format,
                .extent = {w, h, 1},
                .mipLevels = 1,
                .arrayLayers = 1,
                .samples = vk::SampleCountFlagBits::e1,
                .tiling = vk::ImageTiling::eOptimal,
                .usage = vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eStorage |
                         vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eTransferSrc,
                .initialLayout = vk::ImageLayout::eUndefined,
            });
        };
        make(slot.depth, vk::Format::eR32Sfloat);
        make(slot.motion, vk::Format::eR16G16Sfloat);
        slot.width = w;
        slot.height = h;
    }

    void RecordInputs(vk::CommandBuffer cmdbuf, u32 index, vk::Image depth, vk::Image motion, u32 w,
                      u32 h) {
        const Slot& slot = slots[index % Slots];
        if (!slot.depth || slot.width != w || slot.height != h) {
            return;
        }
        const auto all = vk::PipelineStageFlagBits2::eAllCommands;
        const auto to = [&](vk::Image image, vk::ImageLayout from, vk::ImageLayout layout,
                            vk::AccessFlags2 src, vk::AccessFlags2 dst) {
            return vk::ImageMemoryBarrier2{
                .srcStageMask = all,
                .srcAccessMask = src,
                .dstStageMask = all,
                .dstAccessMask = dst,
                .oldLayout = from,
                .newLayout = layout,
                .image = image,
                .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
            };
        };
        const std::array before{
            to(vk::Image(slot.depth), vk::ImageLayout::eUndefined, vk::ImageLayout::eGeneral,
               vk::AccessFlagBits2::eNone, vk::AccessFlagBits2::eTransferWrite),
            to(vk::Image(slot.motion), vk::ImageLayout::eUndefined, vk::ImageLayout::eGeneral,
               vk::AccessFlagBits2::eNone, vk::AccessFlagBits2::eTransferWrite),
        };
        const vk::MemoryBarrier2 captured{
            .srcStageMask = all,
            .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
            .dstStageMask = all,
            .dstAccessMask = vk::AccessFlagBits2::eTransferRead,
        };
        cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &captured,
                                 .imageMemoryBarrierCount = u32(before.size()),
                                 .pImageMemoryBarriers = before.data()});
        const vk::ImageCopy region{
            .srcSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
            .dstSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
            .extent = {w, h, 1},
        };
        cmdbuf.copyImage(depth, vk::ImageLayout::eGeneral, vk::Image(slot.depth), vk::ImageLayout::eGeneral, region);
        cmdbuf.copyImage(motion, vk::ImageLayout::eGeneral, vk::Image(slot.motion), vk::ImageLayout::eGeneral, region);
        const vk::MemoryBarrier2 copied{
            .srcStageMask = all,
            .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .dstStageMask = all,
            .dstAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
        };
        cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &copied});
    }
};

FsrFrameGen::FsrFrameGen(const Instance& instance) : impl{std::make_unique<Impl>(instance)} {}

FsrFrameGen::~FsrFrameGen() = default;

bool FsrFrameGen::Usable() const {
    return !impl->failed && impl->instance.GetFsrFgQueues().present;
}

bool FsrFrameGen::Active() const {
    return impl->swapchain_context != nullptr;
}

void FsrFrameGen::Fail(const char* reason) {
    impl->failed = true;
    Fallback(reason);
}

bool FsrFrameGen::CreateSwapchain(const vk::SwapchainCreateInfoKHR& info, vk::SwapchainKHR& out) {
    if (!Usable()) {
        return false;
    }
    if (!impl->CreateSwapchain(info, out)) {
        Fail("AMD's swapchain could not be made");
        return false;
    }
    return true;
}

void FsrFrameGen::EndSwapchain() {
    impl->End();
}

vk::Result FsrFrameGen::GetImages(std::vector<vk::Image>& images) {
    std::scoped_lock lock{impl->amd_mutex};
    const VkDevice device = VkDevice(impl->instance.GetDevice());
    u32 count = 0;
    VkResult result = impl->functions.get_images(device, impl->swapchain, &count, nullptr);
    if (result != VK_SUCCESS) {
        return vk::Result(result);
    }
    std::vector<VkImage> raw(count);
    result = impl->functions.get_images(device, impl->swapchain, &count, raw.data());
    images.assign(raw.begin(), raw.begin() + count);
    return vk::Result(result);
}

vk::Result FsrFrameGen::Acquire(u64 timeout_ns, vk::Semaphore semaphore, u32& index) {
    return vk::Result(impl->functions.acquire(VkDevice(impl->instance.GetDevice()), impl->swapchain,
                                              timeout_ns, VkSemaphore(semaphore), VK_NULL_HANDLE, &index));
}

vk::Result FsrFrameGen::Present(vk::Queue queue, const vk::PresentInfoKHR& info) {
    std::scoped_lock lock{impl->amd_mutex};
    SubmitHold hold{true};
    const VkPresentInfoKHR raw = static_cast<VkPresentInfoKHR>(info);
    return vk::Result(impl->functions.present(VkQueue(queue), &raw));
}

void FsrFrameGen::EnsureSlot(u32 slot, u32 width, u32 height) {
    impl->EnsureSlot(slot, width, height);
}

void FsrFrameGen::RecordInputs(vk::CommandBuffer cmdbuf, u32 slot, vk::Image depth, vk::Image motion,
                               u32 width, u32 height) {
    impl->RecordInputs(cmdbuf, slot, depth, motion, width, height);
}

void FsrFrameGen::RecordPresent(vk::CommandBuffer cmdbuf, const DlssFrameInfo* frame, vk::Image hudless,
                                vk::Extent2D frame_extent, vk::Format format, vk::Extent2D extent,
                                vk::Rect2D fit, bool menu_open, bool hdr) {
    impl->RecordPresent(cmdbuf, frame, hudless, frame_extent, format, extent, fit, menu_open, hdr);
}

void FsrFrameGen::AfterPresent(bool game_frame) {
    impl->AfterPresent(game_frame);
}

void FsrFrameGen::SetHdrMetadata(const vk::HdrMetadataEXT& metadata) {
    std::scoped_lock lock{impl->amd_mutex};
    if (!impl->functions.set_hdr_metadata || !impl->swapchain) {
        return;
    }
    const VkHdrMetadataEXT raw = static_cast<VkHdrMetadataEXT>(metadata);
    impl->functions.set_hdr_metadata(VkDevice(impl->instance.GetDevice()), 1, &impl->swapchain, &raw);
}

#else

struct FsrFrameGen::Impl {};

FsrFrameGen::FsrFrameGen(const Instance&) {}

FsrFrameGen::~FsrFrameGen() = default;

bool FsrFrameGen::Usable() const {
    return false;
}

bool FsrFrameGen::Active() const {
    return false;
}

void FsrFrameGen::Fail(const char* reason) {
    Fallback(reason);
}

bool FsrFrameGen::CreateSwapchain(const vk::SwapchainCreateInfoKHR&, vk::SwapchainKHR&) {
    return false;
}

void FsrFrameGen::EndSwapchain() {}

vk::Result FsrFrameGen::GetImages(std::vector<vk::Image>&) {
    return vk::Result::eErrorInitializationFailed;
}

vk::Result FsrFrameGen::Acquire(u64, vk::Semaphore, u32&) {
    return vk::Result::eErrorInitializationFailed;
}

vk::Result FsrFrameGen::Present(vk::Queue, const vk::PresentInfoKHR&) {
    return vk::Result::eErrorInitializationFailed;
}

void FsrFrameGen::SetHdrMetadata(const vk::HdrMetadataEXT&) {}

void FsrFrameGen::EnsureSlot(u32, u32, u32) {}

void FsrFrameGen::RecordInputs(vk::CommandBuffer, u32, vk::Image, vk::Image, u32, u32) {}

void FsrFrameGen::RecordPresent(vk::CommandBuffer, const DlssFrameInfo*, vk::Image, vk::Extent2D,
                                vk::Format, vk::Extent2D, vk::Rect2D, bool, bool) {}

void FsrFrameGen::AfterPresent(bool) {}

#endif

}
