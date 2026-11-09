// SPDX-License-Identifier: GPL-3.0-or-later
#include <vulkan/vulkan.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <random>
#include <vector>
#include "video_core/host_shaders/contact_shadow_comp.h"
#include "video_core/host_shaders/rt_shadow_comp.h"
#include "video_core/host_shaders/rt_shadow_filter_comp.h"
#include "video_core/host_shaders/rt_lab_comp.h"
#include "video_core/host_shaders/point_shadow_ray_comp.h"
#include "video_core/host_shaders/rt_unproject_comp.h"
#include "video_core/host_shaders/ssgi_gather_ray_comp.h"

namespace {
#define CHECK(call) do { if (VkResult r_ = (call); r_ != VK_SUCCESS) { std::fprintf(stderr, "%s failed: %d\n", #call, int(r_)); std::exit(1); } } while (false)
constexpr unsigned W = 1920, H = 1080, Atlas = 4096;
constexpr VkDeviceSize AsAlign = 256;

struct Vec3 { float x, y, z; };
Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3 operator*(Vec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
float Dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec3 Cross(Vec3 a, Vec3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
Vec3 Norm(Vec3 a) { return a * (1.0f / std::sqrt(Dot(a, a))); }

struct Mat4 {
    std::array<float, 16> m{};
    float& at(int r, int c) { return m[r * 4 + c]; }
    float at(int r, int c) const { return m[r * 4 + c]; }
};
Mat4 Identity() { Mat4 r; for (int i = 0; i < 4; ++i) r.at(i, i) = 1; return r; }
Mat4 operator*(const Mat4& a, const Mat4& b) {
    Mat4 r;
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            for (int k = 0; k < 4; ++k) r.at(i, j) += a.at(i, k) * b.at(k, j);
    return r;
}
Mat4 Inverse(const Mat4& a) {
    std::array<double, 32> t{};
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) t[r * 8 + c] = a.at(r, c);
        t[r * 8 + 4 + r] = 1;
    }
    for (int p = 0; p < 4; ++p) {
        int best = p;
        for (int r = p; r < 4; ++r) if (std::abs(t[r * 8 + p]) > std::abs(t[best * 8 + p])) best = r;
        for (int c = 0; c < 8; ++c) std::swap(t[p * 8 + c], t[best * 8 + c]);
        const double d = 1.0 / t[p * 8 + p];
        for (int c = 0; c < 8; ++c) t[p * 8 + c] *= d;
        for (int r = 0; r < 4; ++r) {
            if (r == p) continue;
            const double f = t[r * 8 + p];
            for (int c = 0; c < 8; ++c) t[r * 8 + c] -= f * t[p * 8 + c];
        }
    }
    Mat4 r;
    for (int i = 0; i < 4; ++i)
        for (int c = 0; c < 4; ++c) r.at(i, c) = float(t[i * 8 + 4 + c]);
    return r;
}
Mat4 LookTo(Vec3 eye, Vec3 forward) {
    const Vec3 z = Norm(forward);
    const Vec3 x = Norm(Cross({0, 1, 0}, z));
    const Vec3 y = Cross(z, x);
    Mat4 r = Identity();
    r.at(0, 0) = x.x; r.at(0, 1) = x.y; r.at(0, 2) = x.z; r.at(0, 3) = -Dot(x, eye);
    r.at(1, 0) = y.x; r.at(1, 1) = y.y; r.at(1, 2) = y.z; r.at(1, 3) = -Dot(y, eye);
    r.at(2, 0) = z.x; r.at(2, 1) = z.y; r.at(2, 2) = z.z; r.at(2, 3) = -Dot(z, eye);
    return r;
}

struct Mesh {
    std::vector<Vec3> vertices;
    std::vector<std::uint32_t> indices;
    Vec3 position{};
    bool skinned = false;
    Vec3 scale{1, 1, 1};
};

void AddGrid(Mesh& mesh, Vec3 origin, Vec3 du, Vec3 dv, unsigned n) {
    const auto base = std::uint32_t(mesh.vertices.size());
    for (unsigned j = 0; j <= n; ++j)
        for (unsigned i = 0; i <= n; ++i)
            mesh.vertices.push_back(origin + du * (float(i) / n) + dv * (float(j) / n));
    for (unsigned j = 0; j < n; ++j)
        for (unsigned i = 0; i < n; ++i) {
            const std::uint32_t a = base + j * (n + 1) + i, b = a + 1, c = a + n + 1, d = c + 1;
            mesh.indices.insert(mesh.indices.end(), {a, b, c, b, d, c});
        }
}

Mesh Box(Vec3 size, unsigned triangles) {
    Mesh mesh;
    const auto n = std::max(1u, unsigned(std::sqrt(float(triangles) / 12.0f)));
    const Vec3 x{size.x, 0, 0}, y{0, size.y, 0}, z{0, 0, size.z}, o{-size.x / 2, 0, -size.z / 2};
    AddGrid(mesh, o, x, y, n);
    AddGrid(mesh, o + z, y, x, n);
    AddGrid(mesh, o, y, z, n);
    AddGrid(mesh, o + x, z, y, n);
    AddGrid(mesh, o, z, x, n);
    AddGrid(mesh, o + y, x, z, n);
    return mesh;
}

Mesh Capsule(float height, float radius, unsigned triangles) {
    Mesh mesh;
    const auto n = std::max(4u, unsigned(std::sqrt(float(triangles) / 2.0f)));
    for (unsigned j = 0; j <= n; ++j) {
        const float v = float(j) / n, a = v * 3.14159265f;
        for (unsigned i = 0; i <= n; ++i) {
            const float u = float(i) / n * 6.2831853f;
            const float r = radius * std::sin(a);
            mesh.vertices.push_back({r * std::cos(u), height * (0.5f + 0.5f * std::cos(a)), r * std::sin(u)});
        }
    }
    for (unsigned j = 0; j < n; ++j)
        for (unsigned i = 0; i < n; ++i) {
            const std::uint32_t a = j * (n + 1) + i, b = a + 1, c = a + n + 1, d = c + 1;
            mesh.indices.insert(mesh.indices.end(), {a, c, b, b, c, d});
        }
    return mesh;
}

std::vector<Mesh> MakeScene(unsigned total_triangles, unsigned draws) {
    std::mt19937 rng(7);
    const auto range = [&](float lo, float hi) { return std::uniform_real_distribution<float>(lo, hi)(rng); };
    std::vector<Mesh> scene;
    const unsigned character_triangles = 30000, characters = 6;
    const unsigned static_triangles = total_triangles - std::min(total_triangles, characters * character_triangles);
    const unsigned per_draw = std::max(12u, static_triangles / draws);
    Mesh ground = Box({400, 1, 400}, per_draw);
    ground.position = {0, -1, 60};
    scene.push_back(std::move(ground));
    for (unsigned i = 1; i < draws; ++i) {
        const unsigned kind = i % 10;
        if (kind < 4) {
            Mesh m = Box({range(4, 14), range(6, 30), range(4, 14)}, per_draw);
            const float side = i % 2 ? 1.0f : -1.0f;
            m.position = {side * range(11, 40), 0, range(-20, 220)};
            scene.push_back(std::move(m));
        } else if (kind < 7) {
            Mesh m = Box({0.08f, range(1, 4), 0.08f}, per_draw);
            m.position = {range(-9, 9), 0, range(2, 60)};
            scene.push_back(std::move(m));
        } else if (kind < 8) {
            Mesh m = Box({range(0.8f, 2.5f), range(0.5f, 2.0f), range(0.8f, 2.5f)}, per_draw);
            m.position = {range(-9, 9), 0, range(3, 90)};
            scene.push_back(std::move(m));
        } else if (kind < 9) {
            Mesh m = Box({range(3, 8), 0.2f, range(1, 3)}, per_draw);
            m.position = {range(-10, 10), range(2.5f, 6), range(3, 90)};
            scene.push_back(std::move(m));
        } else {
            Mesh m = Box({0.05f, 0.05f, range(2, 8)}, per_draw);
            m.position = {range(-9, 9), range(0.5f, 3), range(2, 60)};
            scene.push_back(std::move(m));
        }
    }
    for (unsigned i = 0; i < characters; ++i) {
        Mesh m = Capsule(1.8f, 0.3f, character_triangles);
        m.position = {range(-5, 5), 0, 4.0f + 4.0f * float(i)};
        m.skinned = true;
        scene.push_back(std::move(m));
    }
    return scene;
}

struct Buffer {
    VkBuffer buffer{};
    VkDeviceMemory memory{};
    void* data{};
    VkDeviceAddress address{};
    VkDeviceSize size{};
};
struct Image {
    VkImage image{};
    VkDeviceMemory memory{};
    VkImageView view{};
};
struct Pipeline {
    VkDescriptorSetLayout set{};
    VkPipelineLayout layout{};
    VkPipeline pipeline{};
};

struct Vk {
    VkInstance instance{};
    VkPhysicalDevice gpu{};
    VkDevice device{};
    VkQueue queue{};
    VkPhysicalDeviceMemoryProperties memory{};
    VkCommandPool pool{};
    VkCommandBuffer cmd{};
    VkQueryPool stamps{};
    VkQueryPool compact_query{};
    VkSampler sampler{};
    float period = 0;
    std::uint64_t mask = ~std::uint64_t{0};
    PFN_vkCmdPushDescriptorSetKHR push{};
    PFN_vkCreateAccelerationStructureKHR create_as{};
    PFN_vkGetAccelerationStructureBuildSizesKHR sizes{};
    PFN_vkCmdBuildAccelerationStructuresKHR build{};
    PFN_vkGetAccelerationStructureDeviceAddressKHR as_address{};
    PFN_vkCmdWriteAccelerationStructuresPropertiesKHR write_props{};
    PFN_vkDestroyAccelerationStructureKHR destroy_as{};
    char name[256]{};

    unsigned Memory(unsigned bits, VkMemoryPropertyFlags flags) {
        for (unsigned i = 0; i < memory.memoryTypeCount; ++i)
            if ((bits & (1u << i)) && (memory.memoryTypes[i].propertyFlags & flags) == flags) return i;
        std::fprintf(stderr, "no memory type\n");
        std::exit(1);
    }
    Buffer MakeBuffer(VkDeviceSize bytes, VkBufferUsageFlags usage, bool host) {
        Buffer b;
        b.size = bytes;
        VkBufferCreateInfo info{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = bytes,
            .usage = usage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT};
        CHECK(vkCreateBuffer(device, &info, nullptr, &b.buffer));
        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(device, b.buffer, &req);
        VkMemoryAllocateFlagsInfo flags{.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
            .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT};
        VkMemoryAllocateInfo alloc{.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &flags,
            .allocationSize = req.size, .memoryTypeIndex = Memory(req.memoryTypeBits,
                host ? VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
                     : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
        CHECK(vkAllocateMemory(device, &alloc, nullptr, &b.memory));
        CHECK(vkBindBufferMemory(device, b.buffer, b.memory, 0));
        if (host) CHECK(vkMapMemory(device, b.memory, 0, VK_WHOLE_SIZE, 0, &b.data));
        VkBufferDeviceAddressInfo address{.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, .buffer = b.buffer};
        b.address = vkGetBufferDeviceAddress(device, &address);
        return b;
    }
    void Free(Buffer& b) {
        if (!b.buffer) return;
        if (b.data) vkUnmapMemory(device, b.memory);
        vkDestroyBuffer(device, b.buffer, nullptr);
        vkFreeMemory(device, b.memory, nullptr);
        b = {};
    }
    Image MakeImage(unsigned w, unsigned h, VkFormat format) {
        Image i;
        VkImageCreateInfo info{.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .imageType = VK_IMAGE_TYPE_2D, .format = format, .extent = {w, h, 1},
            .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
            .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = VK_IMAGE_USAGE_SAMPLED_BIT |
                VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT};
        CHECK(vkCreateImage(device, &info, nullptr, &i.image));
        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(device, i.image, &req);
        VkMemoryAllocateInfo alloc{.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = req.size, .memoryTypeIndex = Memory(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
        CHECK(vkAllocateMemory(device, &alloc, nullptr, &i.memory));
        CHECK(vkBindImageMemory(device, i.image, i.memory, 0));
        VkImageViewCreateInfo view{.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = i.image, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = format,
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        CHECK(vkCreateImageView(device, &view, nullptr, &i.view));
        return i;
    }
    Pipeline MakePipeline(std::initializer_list<VkDescriptorType> types, unsigned push_bytes,
                          const std::uint32_t* code, std::size_t code_bytes, VkPipelineCreateFlags create_flags = 0) {
        Pipeline p;
        std::vector<VkDescriptorSetLayoutBinding> bindings;
        for (const auto type : types)
            bindings.push_back({unsigned(bindings.size()), type, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr});
        VkDescriptorSetLayoutCreateInfo si{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR,
            .bindingCount = unsigned(bindings.size()), .pBindings = bindings.data()};
        CHECK(vkCreateDescriptorSetLayout(device, &si, nullptr, &p.set));
        VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, push_bytes};
        VkPipelineLayoutCreateInfo li{.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            .setLayoutCount = 1, .pSetLayouts = &p.set, .pushConstantRangeCount = 1, .pPushConstantRanges = &range};
        CHECK(vkCreatePipelineLayout(device, &li, nullptr, &p.layout));
        VkShaderModule module;
        VkShaderModuleCreateInfo mi{.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = code_bytes, .pCode = code};
        CHECK(vkCreateShaderModule(device, &mi, nullptr, &module));
        VkComputePipelineCreateInfo pc{.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
            .flags = create_flags, .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = module, .pName = "main"}, .layout = p.layout};
        CHECK(vkCreateComputePipelines(device, {}, 1, &pc, nullptr, &p.pipeline));
        vkDestroyShaderModule(device, module, nullptr);
        return p;
    }
    Vk() {
        VkApplicationInfo app{.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
            .pApplicationName = "rt-shadow-test", .apiVersion = VK_API_VERSION_1_3};
        VkInstanceCreateInfo ci{.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
        CHECK(vkCreateInstance(&ci, nullptr, &instance));
        unsigned count = 0;
        CHECK(vkEnumeratePhysicalDevices(instance, &count, nullptr));
        std::vector<VkPhysicalDevice> devices(count);
        CHECK(vkEnumeratePhysicalDevices(instance, &count, devices.data()));
        gpu = devices.at(0);
        for (const auto candidate : devices) {
            VkPhysicalDeviceProperties p;
            vkGetPhysicalDeviceProperties(candidate, &p);
            if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) { gpu = candidate; break; }
        }
        vkGetPhysicalDeviceMemoryProperties(gpu, &memory);
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(gpu, &properties);
        std::strncpy(name, properties.deviceName, sizeof(name) - 1);
        period = properties.limits.timestampPeriod;
        unsigned family_count = 0, family = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(gpu, &family_count, nullptr);
        std::vector<VkQueueFamilyProperties> families(family_count);
        vkGetPhysicalDeviceQueueFamilyProperties(gpu, &family_count, families.data());
        while (family < family_count && !(families[family].queueFlags & VK_QUEUE_COMPUTE_BIT)) ++family;
        if (families[family].timestampValidBits < 64) mask = (std::uint64_t{1} << families[family].timestampValidBits) - 1;
        const float priority = 1;
        VkDeviceQueueCreateInfo qi{.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
            .queueFamilyIndex = family, .queueCount = 1, .pQueuePriorities = &priority};
        const std::array<const char*, 4> extensions{
            VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME, VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,
            VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME, VK_KHR_RAY_QUERY_EXTENSION_NAME};
        VkPhysicalDeviceRayQueryFeaturesKHR rq{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR, .rayQuery = VK_TRUE};
        VkPhysicalDeviceAccelerationStructureFeaturesKHR as{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR,
            .pNext = &rq, .accelerationStructure = VK_TRUE};
        VkPhysicalDeviceVulkan12Features v12{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
            .pNext = &as, .scalarBlockLayout = VK_TRUE, .bufferDeviceAddress = VK_TRUE};
        VkPhysicalDeviceFeatures2 f2{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &v12};
        f2.features.shaderStorageImageExtendedFormats = VK_TRUE;
        VkDeviceCreateInfo di{.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &f2,
            .queueCreateInfoCount = 1, .pQueueCreateInfos = &qi,
            .enabledExtensionCount = unsigned(extensions.size()), .ppEnabledExtensionNames = extensions.data()};
        CHECK(vkCreateDevice(gpu, &di, nullptr, &device));
        vkGetDeviceQueue(device, family, 0, &queue);
#define LOAD(field, type, fn) field = reinterpret_cast<type>(vkGetDeviceProcAddr(device, fn))
        LOAD(push, PFN_vkCmdPushDescriptorSetKHR, "vkCmdPushDescriptorSetKHR");
        LOAD(create_as, PFN_vkCreateAccelerationStructureKHR, "vkCreateAccelerationStructureKHR");
        LOAD(sizes, PFN_vkGetAccelerationStructureBuildSizesKHR, "vkGetAccelerationStructureBuildSizesKHR");
        LOAD(build, PFN_vkCmdBuildAccelerationStructuresKHR, "vkCmdBuildAccelerationStructuresKHR");
        LOAD(as_address, PFN_vkGetAccelerationStructureDeviceAddressKHR, "vkGetAccelerationStructureDeviceAddressKHR");
        LOAD(write_props, PFN_vkCmdWriteAccelerationStructuresPropertiesKHR, "vkCmdWriteAccelerationStructuresPropertiesKHR");
        LOAD(destroy_as, PFN_vkDestroyAccelerationStructureKHR, "vkDestroyAccelerationStructureKHR");
#undef LOAD
        VkCommandPoolCreateInfo pi{.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
            .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = family};
        CHECK(vkCreateCommandPool(device, &pi, nullptr, &pool));
        VkCommandBufferAllocateInfo ai{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
        CHECK(vkAllocateCommandBuffers(device, &ai, &cmd));
        VkQueryPoolCreateInfo query{.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
            .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = 2};
        CHECK(vkCreateQueryPool(device, &query, nullptr, &stamps));
        VkSamplerCreateInfo sm{.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
            .magFilter = VK_FILTER_NEAREST, .minFilter = VK_FILTER_NEAREST,
            .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE};
        CHECK(vkCreateSampler(device, &sm, nullptr, &sampler));
    }
    void Begin() {
        CHECK(vkResetCommandBuffer(cmd, 0));
        VkCommandBufferBeginInfo begin{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        CHECK(vkBeginCommandBuffer(cmd, &begin));
        vkCmdResetQueryPool(cmd, stamps, 0, 2);
    }
    void Stamp(unsigned i) { vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, stamps, i); }
    void Barrier() {
        VkMemoryBarrier b{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT, .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT};
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &b, 0, nullptr, 0, nullptr);
    }
    void ToGeneral(VkImage image) {
        VkImageMemoryBarrier b{.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = 0, .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = image, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    }
    double End() {
        CHECK(vkEndCommandBuffer(cmd));
        VkSubmitInfo submit{.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd};
        CHECK(vkQueueSubmit(queue, 1, &submit, {}));
        CHECK(vkQueueWaitIdle(queue));
        std::array<std::uint64_t, 2> t{};
        const VkResult r = vkGetQueryPoolResults(device, stamps, 0, 2, sizeof t, t.data(), sizeof(std::uint64_t),
                                                 VK_QUERY_RESULT_64_BIT);
        return r == VK_SUCCESS ? double((t[1] - t[0]) & mask) * period / 1e6 : 0.0;
    }
};

struct Blas {
    VkAccelerationStructureKHR handle{};
    VkDeviceAddress address{};
    VkDeviceSize offset{}, bytes{}, scratch{}, update_scratch{};
    unsigned triangles{}, vertices{};
    VkDeviceSize vertex_offset{}, index_offset{};
};

struct Scene {
    Vk& vk;
    std::vector<Mesh> meshes;
    Buffer geometry{}, staging{}, storage{}, scratch{};
    std::vector<Blas> blas;
    VkDeviceSize total_bytes = 0, total_scratch = 0;
    unsigned triangles = 0;

    VkAccelerationStructureGeometryKHR Geometry(const Blas& b) const {
        VkAccelerationStructureGeometryKHR g{.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,
            .geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR, .flags = VK_GEOMETRY_OPAQUE_BIT_KHR};
        g.geometry.triangles = {.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR,
            .vertexFormat = VK_FORMAT_R32G32B32_SFLOAT, .vertexStride = sizeof(Vec3),
            .maxVertex = b.vertices - 1, .indexType = VK_INDEX_TYPE_UINT32};
        g.geometry.triangles.vertexData.deviceAddress = geometry.address + b.vertex_offset;
        g.geometry.triangles.indexData.deviceAddress = geometry.address + b.index_offset;
        return g;
    }
    void Create(VkBuildAccelerationStructureFlagsKHR flags) {
        const VkDeviceSize alignment = 16;
        VkDeviceSize bytes = 0;
        blas.resize(meshes.size());
        for (std::size_t i = 0; i < meshes.size(); ++i) {
            blas[i].vertex_offset = bytes;
            bytes += (meshes[i].vertices.size() * sizeof(Vec3) + alignment - 1) & ~(alignment - 1);
            blas[i].index_offset = bytes;
            bytes += (meshes[i].indices.size() * 4 + alignment - 1) & ~(alignment - 1);
            blas[i].vertices = unsigned(meshes[i].vertices.size());
            blas[i].triangles = unsigned(meshes[i].indices.size() / 3);
            triangles += blas[i].triangles;
        }
        staging = vk.MakeBuffer(bytes, 0, true);
        for (std::size_t i = 0; i < meshes.size(); ++i) {
            std::memcpy(static_cast<char*>(staging.data) + blas[i].vertex_offset, meshes[i].vertices.data(), meshes[i].vertices.size() * sizeof(Vec3));
            std::memcpy(static_cast<char*>(staging.data) + blas[i].index_offset, meshes[i].indices.data(), meshes[i].indices.size() * 4);
        }
        geometry = vk.MakeBuffer(bytes, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR, false);
        vk.Begin();
        VkBufferCopy copy{0, 0, bytes};
        vkCmdCopyBuffer(vk.cmd, staging.buffer, geometry.buffer, 1, &copy);
        vk.Barrier();
        vk.Stamp(0); vk.Stamp(1);
        vk.End();
        Sizes(flags);
    }
    void Sizes(VkBuildAccelerationStructureFlagsKHR flags) {
        total_bytes = total_scratch = 0;
        for (auto& b : blas) {
            const auto g = Geometry(b);
            VkAccelerationStructureBuildGeometryInfoKHR info{.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR,
                .type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR, .flags = flags,
                .mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR, .geometryCount = 1, .pGeometries = &g};
            VkAccelerationStructureBuildSizesInfoKHR s{.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
            vk.sizes(vk.device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &info, &b.triangles, &s);
            b.offset = total_bytes;
            b.bytes = s.accelerationStructureSize;
            b.scratch = s.buildScratchSize;
            b.update_scratch = s.updateScratchSize;
            total_bytes += (s.accelerationStructureSize + AsAlign - 1) & ~(AsAlign - 1);
            total_scratch += (std::max(s.buildScratchSize, s.updateScratchSize) + AsAlign - 1) & ~(AsAlign - 1);
        }
    }
    void Allocate() {
        for (auto& b : blas) if (b.handle) { vk.destroy_as(vk.device, b.handle, nullptr); b.handle = {}; }
        vk.Free(storage);
        vk.Free(scratch);
        storage = vk.MakeBuffer(total_bytes, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR, false);
        scratch = vk.MakeBuffer(total_scratch, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, false);
        for (auto& b : blas) {
            VkAccelerationStructureCreateInfoKHR ci{.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR,
                .buffer = storage.buffer, .offset = b.offset, .size = b.bytes,
                .type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR};
            CHECK(vk.create_as(vk.device, &ci, nullptr, &b.handle));
            VkAccelerationStructureDeviceAddressInfoKHR ai{.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR, .accelerationStructure = b.handle};
            b.address = vk.as_address(vk.device, &ai);
        }
    }
    double BuildAll(VkBuildAccelerationStructureFlagsKHR flags) {
        std::vector<VkAccelerationStructureGeometryKHR> geometries;
        geometries.reserve(blas.size());
        std::vector<VkAccelerationStructureBuildGeometryInfoKHR> infos;
        std::vector<VkAccelerationStructureBuildRangeInfoKHR> ranges;
        std::vector<const VkAccelerationStructureBuildRangeInfoKHR*> pointers;
        VkDeviceSize at = 0;
        for (auto& b : blas) {
            geometries.push_back(Geometry(b));
            VkAccelerationStructureBuildGeometryInfoKHR info{.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR,
                .type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR, .flags = flags,
                .mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR, .dstAccelerationStructure = b.handle,
                .geometryCount = 1, .pGeometries = &geometries.back()};
            info.scratchData.deviceAddress = scratch.address + at;
            at += (std::max(b.scratch, b.update_scratch) + AsAlign - 1) & ~(AsAlign - 1);
            infos.push_back(info);
            ranges.push_back({b.triangles, 0, 0, 0});
        }
        for (auto& r : ranges) pointers.push_back(&r);
        vk.Begin();
        vk.Stamp(0);
        vk.build(vk.cmd, unsigned(infos.size()), infos.data(), pointers.data());
        vk.Stamp(1);
        vk.Barrier();
        return vk.End();
    }
    double Refit(VkBuildAccelerationStructureFlagsKHR flags, bool update) {
        std::vector<VkAccelerationStructureGeometryKHR> geometries;
        geometries.reserve(blas.size());
        std::vector<VkAccelerationStructureBuildGeometryInfoKHR> infos;
        std::vector<VkAccelerationStructureBuildRangeInfoKHR> ranges;
        std::vector<const VkAccelerationStructureBuildRangeInfoKHR*> pointers;
        VkDeviceSize at = 0;
        ranges.reserve(blas.size());
        for (std::size_t i = 0; i < blas.size(); ++i) {
            auto& b = blas[i];
            const VkDeviceSize scratch_at = at;
            at += (std::max(b.scratch, b.update_scratch) + AsAlign - 1) & ~(AsAlign - 1);
            if (!meshes[i].skinned) continue;
            geometries.push_back(Geometry(b));
            VkAccelerationStructureBuildGeometryInfoKHR info{.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR,
                .type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR, .flags = flags,
                .mode = update ? VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR : VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR,
                .srcAccelerationStructure = update ? b.handle : VK_NULL_HANDLE, .dstAccelerationStructure = b.handle,
                .geometryCount = 1, .pGeometries = &geometries.back()};
            info.scratchData.deviceAddress = scratch.address + scratch_at;
            infos.push_back(info);
            ranges.push_back({b.triangles, 0, 0, 0});
            pointers.push_back(&ranges.back());
        }
        vk.Begin();
        vk.Stamp(0);
        vk.build(vk.cmd, unsigned(infos.size()), infos.data(), pointers.data());
        vk.Stamp(1);
        vk.Barrier();
        return vk.End();
    }
};

struct Tlas {
    VkAccelerationStructureKHR handle{};
    Buffer instances{}, storage{}, scratch{};
    unsigned count = 0;
};

double Median(std::vector<double> v) {
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

void WriteTlasInstances(Tlas& t, const Scene& scene, const Mat4& view) {
    auto* out = static_cast<VkAccelerationStructureInstanceKHR*>(t.instances.data);
    for (std::size_t i = 0; i < scene.blas.size(); ++i) {
        Mat4 model = Identity();
        model.at(0, 3) = scene.meshes[i].position.x;
        model.at(1, 3) = scene.meshes[i].position.y;
        model.at(2, 3) = scene.meshes[i].position.z;
        const Mat4 m = view * model;
        VkAccelerationStructureInstanceKHR inst{};
        std::memcpy(inst.transform.matrix, m.m.data(), 12 * sizeof(float));
        inst.mask = 0xff;
        inst.accelerationStructureReference = scene.blas[i].address;
        out[i] = inst;
    }
}

double BuildTlas(Vk& vk, Tlas& t, const Scene& scene, const Mat4& view, bool time_upload) {
    if (time_upload || !t.handle) WriteTlasInstances(t, scene, view);
    VkAccelerationStructureGeometryKHR g{.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,
        .geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR, .flags = VK_GEOMETRY_OPAQUE_BIT_KHR};
    g.geometry.instances = {.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR};
    g.geometry.instances.data.deviceAddress = t.instances.address;
    VkAccelerationStructureBuildGeometryInfoKHR info{.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR,
        .type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR, .flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR,
        .mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR, .dstAccelerationStructure = t.handle,
        .geometryCount = 1, .pGeometries = &g};
    info.scratchData.deviceAddress = t.scratch.address;
    VkAccelerationStructureBuildRangeInfoKHR range{t.count, 0, 0, 0};
    const VkAccelerationStructureBuildRangeInfoKHR* pointer = &range;
    vk.Begin();
    vk.Stamp(0);
    vk.build(vk.cmd, 1, &info, &pointer);
    vk.Stamp(1);
    vk.Barrier();
    return vk.End();
}

Tlas CreateTlas(Vk& vk, const Scene& scene) {
    Tlas t;
    t.count = unsigned(scene.blas.size());
    t.instances = vk.MakeBuffer(t.count * sizeof(VkAccelerationStructureInstanceKHR),
                                VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR, true);
    VkAccelerationStructureGeometryKHR g{.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,
        .geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR, .flags = VK_GEOMETRY_OPAQUE_BIT_KHR};
    g.geometry.instances = {.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR};
    VkAccelerationStructureBuildGeometryInfoKHR info{.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR,
        .type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR, .flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR,
        .mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR, .geometryCount = 1, .pGeometries = &g};
    VkAccelerationStructureBuildSizesInfoKHR s{.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    vk.sizes(vk.device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &info, &t.count, &s);
    t.storage = vk.MakeBuffer(s.accelerationStructureSize, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR, false);
    t.scratch = vk.MakeBuffer(s.buildScratchSize + AsAlign, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, false);
    VkAccelerationStructureCreateInfoKHR ci{.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR,
        .buffer = t.storage.buffer, .size = s.accelerationStructureSize, .type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR};
    CHECK(vk.create_as(vk.device, &ci, nullptr, &t.handle));
    return t;
}

struct Views {
    Mat4 view, proj, inv_proj;
    std::array<Mat4, 4> cascade, cascade_inverse;
    std::array<float, 3> splits{};
};

Views MakeViews(Vec3 eye, Vec3 forward, Vec3 sun) {
    Views v;
    v.view = LookTo(eye, forward);
    const float fy = 1.0f / std::tan(0.5f * 0.87f), fx = fy * H / W, near = 0.1f, far = 3000.0f;
    v.proj.at(0, 0) = fx; v.proj.at(1, 1) = fy;
    v.proj.at(2, 2) = far / (far - near); v.proj.at(2, 3) = -near * far / (far - near);
    v.proj.at(3, 2) = 1;
    v.inv_proj = Inverse(v.proj);
    v.splits = {8, 25, 80};
    const std::array<float, 4> half{8, 22, 60, 170};
    const std::array<float, 4> centers{4, 16, 50, 150};
    const Mat4 inv_view = Inverse(v.view);
    for (int c = 0; c < 4; ++c) {
        const Vec3 center = eye + Norm(forward) * centers[c];
        const Mat4 light = LookTo(center + sun * 300.0f, sun * -1.0f);
        Mat4 ortho = Identity();
        ortho.at(0, 0) = 1.0f / half[c]; ortho.at(1, 1) = 1.0f / half[c];
        ortho.at(2, 2) = 1.0f / 800.0f;
        Mat4 tile = Identity();
        tile.at(0, 0) = 0.25f; tile.at(0, 3) = 0.25f + 0.5f * float(c & 1);
        tile.at(1, 1) = 0.25f; tile.at(1, 3) = 0.25f + 0.5f * float(c >> 1);
        const Mat4 view_to_atlas = tile * ortho * light * inv_view;
        v.cascade_inverse[c] = Inverse(view_to_atlas);
        v.cascade[c] = view_to_atlas * v.inv_proj;
    }
    return v;
}

struct Metric { double mean{}, off10{}, off25{}, signed_bias{}; };
Metric Compare(const std::vector<float>& reference, const std::vector<float>& value, const std::vector<float>& depth) {
    Metric m;
    std::size_t counted = 0;
    for (std::size_t i = 0; i < reference.size(); ++i) {
        if (!(depth[i] < 1.0f)) continue;
        const double d = double(value[i]) - reference[i];
        m.mean += std::abs(d);
        m.signed_bias += d;
        m.off10 += std::abs(d) > 0.1;
        m.off25 += std::abs(d) > 0.25;
        ++counted;
    }
    counted = std::max<std::size_t>(counted, 1);
    m.mean /= counted; m.signed_bias /= counted;
    m.off10 *= 100.0 / counted; m.off25 *= 100.0 / counted;
    return m;
}

void WritePgm(const char* path, const std::vector<float>& image) {
    std::ofstream out(path, std::ios::binary);
    out << "P5\n" << W << ' ' << H << "\n255\n";
    std::vector<unsigned char> bytes(image.size());
    for (std::size_t i = 0; i < image.size(); ++i) bytes[i] = (unsigned char)(std::clamp(image[i], 0.0f, 1.0f) * 255.0f + 0.5f);
    out.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
}

struct PcssPush {
    unsigned offsets = 0, frame = 0;
    float width = W, height = H, length = 0.6f, thickness = 0.35f, strength = 0.75f, flip = 1;
    unsigned debug = 0, contact = 0, pcss = 1;
    float light = 0.02f, maximum = 0.3f, minimum = 0.0f, bias = 0.04f;
    unsigned filter = 1;
    float blend = 0.1f;
};
static_assert(sizeof(PcssPush) == 68);
struct RtPush {
    unsigned offsets = 0, frame = 0;
    float width = W, height = H;
    unsigned rays = 1;
    float light = 0.02f, bias = 0.01f, distance = 500.0f;
};
static_assert(sizeof(RtPush) == 32);
struct FilterPush {
    unsigned offsets = 0;
    int radius = 2;
    float width = W, height = H;
    unsigned keep_map = 0;
};
struct LabPush {
    Mat4 to_view;
    float width, height, min_x, min_y, max_x, max_y;
    unsigned perspective;
};

struct Lab {
    Vk& vk;
    Pipeline lab, rt, filter, pcss;
    Buffer cb, ub, readback;
    Image depth, atlas, raw, shadow;
    bool laid_out = false;

    explicit Lab(Vk& v) : vk(v) {
        using enum VkDescriptorType;
        lab = vk.MakePipeline({VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR}, sizeof(LabPush), RT_LAB_COMP, sizeof(RT_LAB_COMP));
        rt = vk.MakePipeline({VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR}, sizeof(RtPush), RT_SHADOW_COMP, sizeof(RT_SHADOW_COMP));
        filter = vk.MakePipeline({VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER}, sizeof(FilterPush), RT_SHADOW_FILTER_COMP, sizeof(RT_SHADOW_FILTER_COMP));
        pcss = vk.MakePipeline({VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER}, sizeof(PcssPush), CONTACT_SHADOW_COMP, sizeof(CONTACT_SHADOW_COMP));
        cb = vk.MakeBuffer(4096, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
        ub = vk.MakeBuffer(4096, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
        readback = vk.MakeBuffer(W * H * 4, 0, true);
        depth = vk.MakeImage(W, H, VK_FORMAT_R32_SFLOAT);
        atlas = vk.MakeImage(Atlas, Atlas, VK_FORMAT_R32_SFLOAT);
        raw = vk.MakeImage(W, H, VK_FORMAT_R8_UNORM);
        shadow = vk.MakeImage(W, H, VK_FORMAT_R8G8B8A8_UNORM);
        auto* u = static_cast<float*>(ub.data);
        u[72] = 2000; u[73] = 1;
    }
    void LayOut() {
        if (laid_out) return;
        laid_out = true;
        for (auto image : {depth.image, atlas.image, raw.image, shadow.image}) vk.ToGeneral(image);
    }
    void SetCamera(const Views& v) {
        auto* c = static_cast<float*>(cb.data);
        for (int r = 0; r < 4; ++r)
            for (int k = 0; k < 4; ++k) {
                c[r * 4 + k] = v.inv_proj.at(r, k);
                for (int cascade = 0; cascade < 4; ++cascade)
                    c[16 + 16 * cascade + r * 4 + k] = v.cascade[cascade].at(r, k);
            }
        c[80] = v.splits[0]; c[81] = v.splits[1]; c[82] = v.splits[2];
    }
    void Lab1(VkAccelerationStructureKHR tlas, Image& target, const Mat4& to_view, float w, float h, std::array<float, 4> region, bool perspective) {
        VkDescriptorImageInfo image{{}, target.view, VK_IMAGE_LAYOUT_GENERAL};
        VkWriteDescriptorSetAccelerationStructureKHR as{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR,
            .accelerationStructureCount = 1, .pAccelerationStructures = &tlas};
        const std::array<VkWriteDescriptorSet, 2> writes{{
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 0, .descriptorCount = 1,
             .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .pImageInfo = &image},
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .pNext = &as, .dstBinding = 1, .descriptorCount = 1,
             .descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR}}};
        LabPush push{to_view, w, h, region[0], region[1], region[2], region[3], perspective ? 1u : 0u};
        vkCmdBindPipeline(vk.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, lab.pipeline);
        vk.push(vk.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, lab.layout, 0, 2, writes.data());
        vkCmdPushConstants(vk.cmd, lab.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
        vkCmdDispatch(vk.cmd, (unsigned(w) + 7) / 8, (unsigned(h) + 7) / 8, 1);
        vk.Barrier();
    }
    void Prepare(VkAccelerationStructureKHR tlas, const Views& v) {
        SetCamera(v);
        vk.Begin();
        LayOut();
        vk.Barrier();
        Lab1(tlas, depth, v.inv_proj, W, H, {0, 0, 1, 1}, true);
        for (int c = 0; c < 4; ++c) {
            const float x = 0.5f * float(c & 1), y = 0.5f * float(c >> 1);
            Lab1(tlas, atlas, v.cascade_inverse[c], Atlas, Atlas, {x, y, x + 0.5f, y + 0.5f}, false);
        }
        vk.Stamp(0); vk.Stamp(1);
        vk.End();
    }
    void Rt(VkAccelerationStructureKHR tlas, RtPush p) {
        VkDescriptorBufferInfo c{cb.buffer, 0, 4096};
        VkDescriptorImageInfo d{vk.sampler, depth.view, VK_IMAGE_LAYOUT_GENERAL}, r{{}, raw.view, VK_IMAGE_LAYOUT_GENERAL};
        VkWriteDescriptorSetAccelerationStructureKHR as{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR,
            .accelerationStructureCount = 1, .pAccelerationStructures = &tlas};
        const std::array<VkWriteDescriptorSet, 4> writes{{
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 0, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &c},
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 1, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &d},
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 2, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .pImageInfo = &r},
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .pNext = &as, .dstBinding = 3, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR}}};
        vkCmdBindPipeline(vk.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, rt.pipeline);
        vk.push(vk.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, rt.layout, 0, 4, writes.data());
        vkCmdPushConstants(vk.cmd, rt.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);
        vkCmdDispatch(vk.cmd, (W + 7) / 8, (H + 7) / 8, 1);
    }
    void Filter(int radius) {
        VkDescriptorBufferInfo c{cb.buffer, 0, 4096}, u{ub.buffer, 0, 4096};
        VkDescriptorImageInfo d{vk.sampler, depth.view, VK_IMAGE_LAYOUT_GENERAL}, r{vk.sampler, raw.view, VK_IMAGE_LAYOUT_GENERAL}, s{{}, shadow.view, VK_IMAGE_LAYOUT_GENERAL};
        const std::array<VkWriteDescriptorSet, 5> writes{{
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 0, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &c},
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 1, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &d},
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 2, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &r},
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 3, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .pImageInfo = &s},
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 4, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &u}}};
        FilterPush p;
        p.radius = radius;
        vkCmdBindPipeline(vk.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, filter.pipeline);
        vk.push(vk.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, filter.layout, 0, 5, writes.data());
        vkCmdPushConstants(vk.cmd, filter.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);
        vkCmdDispatch(vk.cmd, (W + 7) / 8, (H + 7) / 8, 1);
    }
    void Pcss(PcssPush p) {
        VkDescriptorBufferInfo c{cb.buffer, 0, 4096}, u{ub.buffer, 0, 4096};
        VkDescriptorImageInfo d{vk.sampler, depth.view, VK_IMAGE_LAYOUT_GENERAL}, m{vk.sampler, atlas.view, VK_IMAGE_LAYOUT_GENERAL}, s{{}, shadow.view, VK_IMAGE_LAYOUT_GENERAL};
        const std::array<VkWriteDescriptorSet, 5> writes{{
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 0, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &c},
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 1, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &d},
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 2, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .pImageInfo = &s},
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 3, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &m},
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 4, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &u}}};
        vkCmdBindPipeline(vk.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pcss.pipeline);
        vk.push(vk.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pcss.layout, 0, 5, writes.data());
        vkCmdPushConstants(vk.cmd, pcss.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);
        vkCmdDispatch(vk.cmd, (W + 7) / 8, (H + 7) / 8, 1);
    }
    std::vector<float> Read(Image& image, VkFormat format) {
        vk.Begin();
        vk.Barrier();
        VkBufferImageCopy copy{.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {W, H, 1}};
        vkCmdCopyImageToBuffer(vk.cmd, image.image, VK_IMAGE_LAYOUT_GENERAL, readback.buffer, 1, &copy);
        vk.Barrier();
        vk.Stamp(0); vk.Stamp(1);
        vk.End();
        std::vector<float> out(W * H);
        const auto* bytes = static_cast<const unsigned char*>(readback.data);
        for (std::size_t i = 0; i < out.size(); ++i) {
            if (format == VK_FORMAT_R32_SFLOAT) std::memcpy(&out[i], bytes + i * 4, 4);
            else if (format == VK_FORMAT_R8_UNORM) out[i] = bytes[i] / 255.0f;
            else out[i] = bytes[i * 4 + 3] / 255.0f;
        }
        return out;
    }
    void ClearShadow() {
        VkClearColorValue color{};
        color.float32[0] = color.float32[1] = color.float32[2] = color.float32[3] = 1;
        VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(vk.cmd, shadow.image, VK_IMAGE_LAYOUT_GENERAL, &color, 1, &range);
        vk.Barrier();
    }
};
}

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const char* dump = argc > 1 ? argv[1] : nullptr;
    Vk vk;
    std::printf("GPU: %s, %ux%u, atlas %u\n", vk.name, W, H, Atlas);
    Lab lab(vk);
    vk.MakePipeline({VK_DESCRIPTOR_TYPE_STORAGE_BUFFER}, 56, RT_UNPROJECT_COMP, sizeof(RT_UNPROJECT_COMP));
    vk.MakePipeline({VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                     VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                     VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE},
                    64, POINT_SHADOW_RAY_COMP, sizeof(POINT_SHADOW_RAY_COMP));
    vk.MakePipeline({VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                     VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                     VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR},
                    128, SSGI_GATHER_RAY_COMP, sizeof(SSGI_GATHER_RAY_COMP));
    if (argc > 1 && std::strcmp(argv[1], "--pipelines") == 0) {
        std::puts("RT pipelines: built");
        return 0;
    }
    const Vec3 sun = Norm({-0.5f, 0.75f, 0.4f});
    constexpr auto Trace = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    constexpr auto Fast = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR;
    constexpr auto Update = VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR;
    const std::array<std::array<Vec3, 2>, 3> cameras{{
        {{{0, 1.6f, -2}, {0.1f, -0.05f, 1}}},
        {{{-4, 1.7f, 12}, {0.8f, -0.25f, 0.5f}}},
        {{{5, 3, 30}, {-0.5f, -0.5f, 1}}},
    }};
    if (argc > 1 && std::strcmp(argv[1], "--speed") == 0) {
        const unsigned total = argc > 2 ? unsigned(std::strtoul(argv[2], nullptr, 10)) : 1500000u;
        Scene scene{vk, MakeScene(total, total / 1875)};
        scene.Create(Trace | Update);
        scene.Allocate();
        scene.BuildAll(Trace | Update);
        const unsigned n = unsigned(scene.blas.size());
        std::printf("== Speed: %u triangles in %u BLAS\n", scene.triangles, n);
        struct Variant { const char* name; VkBuildAccelerationStructureFlagsKHR flags; bool doubled; };
        const std::array<Variant, 3> variants{{{"TLAS fast build", Fast, false},
                                               {"TLAS fast trace", Trace, false},
                                               {"TLAS fast build, every mesh twice", Fast, true}}};
        const auto make_tlas = [&](unsigned count, VkBuildAccelerationStructureFlagsKHR flags) {
            Tlas t;
            t.count = count;
            t.instances = vk.MakeBuffer(count * sizeof(VkAccelerationStructureInstanceKHR),
                                        VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR, true);
            VkAccelerationStructureGeometryKHR g{.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,
                .geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR, .flags = VK_GEOMETRY_OPAQUE_BIT_KHR};
            g.geometry.instances = {.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR};
            VkAccelerationStructureBuildGeometryInfoKHR info{.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR,
                .type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR, .flags = flags,
                .mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR, .geometryCount = 1, .pGeometries = &g};
            VkAccelerationStructureBuildSizesInfoKHR s{.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
            vk.sizes(vk.device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &info, &t.count, &s);
            t.storage = vk.MakeBuffer(s.accelerationStructureSize, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR, false);
            t.scratch = vk.MakeBuffer(s.buildScratchSize + AsAlign, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, false);
            VkAccelerationStructureCreateInfoKHR ci{.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR,
                .buffer = t.storage.buffer, .size = s.accelerationStructureSize, .type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR};
            CHECK(vk.create_as(vk.device, &ci, nullptr, &t.handle));
            return t;
        };
        const auto build_tlas = [&](Tlas& t, const Mat4& view, VkBuildAccelerationStructureFlagsKHR flags, bool doubled) {
            auto* out = static_cast<VkAccelerationStructureInstanceKHR*>(t.instances.data);
            for (unsigned copy = 0; copy < (doubled ? 2u : 1u); ++copy) {
                for (unsigned i = 0; i < n; ++i) {
                    Mat4 model = Identity();
                    model.at(0, 3) = scene.meshes[i].position.x;
                    model.at(1, 3) = scene.meshes[i].position.y;
                    model.at(2, 3) = scene.meshes[i].position.z;
                    const Mat4 m = view * model;
                    VkAccelerationStructureInstanceKHR inst{};
                    std::memcpy(inst.transform.matrix, m.m.data(), 12 * sizeof(float));
                    inst.mask = doubled ? (copy ? 2 : 1) : 3;
                    inst.accelerationStructureReference = scene.blas[i].address;
                    out[copy * n + i] = inst;
                }
            }
            VkAccelerationStructureGeometryKHR g{.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,
                .geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR, .flags = VK_GEOMETRY_OPAQUE_BIT_KHR};
            g.geometry.instances = {.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR};
            g.geometry.instances.data.deviceAddress = t.instances.address;
            VkAccelerationStructureBuildGeometryInfoKHR info{.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR,
                .type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR, .flags = flags,
                .mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR, .dstAccelerationStructure = t.handle,
                .geometryCount = 1, .pGeometries = &g};
            info.scratchData.deviceAddress = t.scratch.address;
            VkAccelerationStructureBuildRangeInfoKHR range{doubled ? 2 * n : n, 0, 0, 0};
            const VkAccelerationStructureBuildRangeInfoKHR* pointer = &range;
            vk.Begin();
            vk.Stamp(0);
            vk.build(vk.cmd, 1, &info, &pointer);
            vk.Stamp(1);
            vk.Barrier();
            return vk.End();
        };
        struct LampPush {
            unsigned first_light = 0, frame = 0;
            float width = W, height = H, length = 0, thickness = 0, strength = 1, fade_start = 60, fade_end = 100;
            unsigned count = 1, steps = 0;
            float bias = 0.02f;
            std::array<int, 4> rect{};
        };
        static_assert(sizeof(LampPush) == 64);
        Pipeline lamp = vk.MakePipeline({VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                         VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                         VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                         VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                         VK_DESCRIPTOR_TYPE_STORAGE_IMAGE},
                                        64, POINT_SHADOW_RAY_COMP, sizeof(POINT_SHADOW_RAY_COMP),
                                        VK_PIPELINE_CREATE_DISPATCH_BASE_BIT);
        Buffer lights = vk.MakeBuffer(64 * 1024 * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
        Image zero_image = vk.MakeImage(W, H, VK_FORMAT_B10G11R11_UFLOAT_PACK32);
        std::array<Image, 2> history{vk.MakeImage(W, H, VK_FORMAT_R16G16B16A16_SFLOAT),
                                     vk.MakeImage(W, H, VK_FORMAT_R16G16B16A16_SFLOAT)};
        std::array<Image, 4> lamp_images{};
        for (auto& image : lamp_images) image = vk.MakeImage(W, H, VK_FORMAT_B10G11R11_UFLOAT_PACK32);
        vk.Begin();
        for (auto& image : lamp_images) vk.ToGeneral(image.image);
        vk.ToGeneral(zero_image.image);
        for (auto& image : history) vk.ToGeneral(image.image);
        {
            VkClearColorValue zero{};
            const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vkCmdClearColorImage(vk.cmd, zero_image.image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &range);
        }
        vk.Stamp(0); vk.Stamp(1);
        vk.End();
        const auto decode = [](std::uint32_t v) {
            const unsigned e = (v >> 6) & 31u, m = v & 63u;
            return e == 0 ? float(m) / 64.0f * std::ldexp(1.0f, -14) : std::ldexp(1.0f + float(m) / 64.0f, int(e) - 15);
        };
        std::vector<std::array<double, 3>> lamp_ms(variants.size()), sun_ms(variants.size());
        std::array<double, 3> per_tile_ms{}, pick_ms{}, reuse_ms{}, reuse_error{};
        std::array<std::array<double, 2>, 3> pick_error{};
        std::vector<double> tlas_ms(variants.size());
        double lit_sum = 0;
        unsigned tile_sum = 0;
        for (std::size_t cam = 0; cam < cameras.size(); ++cam) {
            const Views views = MakeViews(cameras[cam][0], cameras[cam][1], sun);
            std::vector<LampPush> tiles;
            auto* words = static_cast<float*>(lights.data);
            for (int r = 0; r < 4; ++r)
                for (int k = 0; k < 4; ++k) words[r * 4 + k] = views.inv_proj.at(r, k);
            unsigned used = 16;
            const float radius = 9.0f;
            for (float z = 2.0f; z < 100.0f; z += 6.5f) {
                for (const float x : {-7.0f, 7.0f, 0.0f}) {
                    if (x == 0.0f && int(z) % 3 != 0) continue;
                    const Vec3 world{x, x == 0.0f ? 1.2f : 3.5f, z + (x > 0 ? 3.0f : 0.0f)};
                    Vec3 v{};
                    for (int r = 0; r < 3; ++r) {
                        const float value = views.view.at(r, 0) * world.x + views.view.at(r, 1) * world.y +
                                            views.view.at(r, 2) * world.z + views.view.at(r, 3);
                        (r == 0 ? v.x : r == 1 ? v.y : v.z) = value;
                    }
                    if (v.z < -radius) continue;
                    float x0 = 0, y0 = 0, x1 = float(W), y1 = float(H);
                    if (v.z - radius > 0.1f) {
                        x0 = float(W); y0 = float(H); x1 = 0; y1 = 0;
                        for (int c = 0; c < 8; ++c) {
                            const Vec3 corner{v.x + (c & 1 ? radius : -radius), v.y + (c & 2 ? radius : -radius),
                                              v.z + (c & 4 ? radius : -radius)};
                            const float nx = views.proj.at(0, 0) * corner.x / corner.z;
                            const float ny = views.proj.at(1, 1) * corner.y / corner.z;
                            const float px = (nx * 0.5f + 0.5f) * W, py = (0.5f - ny * 0.5f) * H;
                            x0 = std::min(x0, px); x1 = std::max(x1, px);
                            y0 = std::min(y0, py); y1 = std::max(y1, py);
                        }
                    }
                    LampPush push;
                    push.rect = {std::clamp(int(x0), 0, int(W)), std::clamp(int(y0), 0, int(H)),
                                 std::clamp(int(x1) + 1, 0, int(W)), std::clamp(int(y1) + 1, 0, int(H))};
                    if (push.rect[0] >= push.rect[2] || push.rect[1] >= push.rect[3]) continue;
                    push.first_light = used;
                    const std::array<float, 16> light{v.x, v.y, v.z, 1.0f / (radius * radius), 1.0f, 0.8f, 0.6f};
                    std::memcpy(words + used, light.data(), sizeof(light));
                    used += 16;
                    tiles.push_back(push);
                }
            }
            tile_sum += unsigned(tiles.size());
            const unsigned table = used;
            std::array<int, 4> covered{int(W), int(H), 0, 0};
            for (const LampPush& tile : tiles) {
                const std::array<std::uint32_t, 8> row{unsigned(tile.rect[0]), unsigned(tile.rect[1]), unsigned(tile.rect[2]),
                                                       unsigned(tile.rect[3]), tile.first_light, 1u, 0u, 0u};
                std::memcpy(words + used, row.data(), sizeof(row));
                used += 8;
                covered = {std::min(covered[0], tile.rect[0]), std::min(covered[1], tile.rect[1]),
                           std::max(covered[2], tile.rect[2]), std::max(covered[3], tile.rect[3])};
            }
            for (std::size_t at = 0; at < variants.size(); ++at) {
                const Variant& variant = variants[at];
                Tlas tlas = make_tlas(variant.doubled ? 2 * n : n, variant.flags);
                std::vector<double> builds;
                for (int i = 0; i < 9; ++i) builds.push_back(build_tlas(tlas, views.view, variant.flags, variant.doubled));
                tlas_ms[at] += Median(builds) / cameras.size();
                if (at == 0) {
                    lab.Prepare(tlas.handle, views);
                }
                std::vector<double> sun_samples, lamp_samples;
                for (int i = 0; i < 15; ++i) {
                    vk.Begin();
                    vk.Stamp(0);
                    RtPush p;
                    p.rays = 2;
                    lab.Rt(tlas.handle, p);
                    vk.Stamp(1);
                    vk.Barrier();
                    sun_samples.push_back(vk.End());
                }
                const unsigned history_at = used;
                const auto run_lamps = [&](bool per_tile, unsigned pick = 0, unsigned frame = 0, int parity = -1) {
                    if (parity >= 0) {
                        auto* block = reinterpret_cast<std::uint32_t*>(words) + history_at;
                        block[0] = frame == 0 ? 2u : 3u;
                        block[1] = frame;
                        const std::array<float, 12> identity{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
                        std::memcpy(block + 2, identity.data(), sizeof(identity));
                        std::memcpy(block + 14, views.proj.m.data(), 64);
                    }
                    vk.Begin();
                    VkClearColorValue one{};
                    one.float32[0] = one.float32[1] = one.float32[2] = 1.0f;
                    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                    vkCmdClearColorImage(vk.cmd, lamp_images[0].image, VK_IMAGE_LAYOUT_GENERAL, &one, 1, &range);
                    vkCmdClearColorImage(vk.cmd, lamp_images[2].image, VK_IMAGE_LAYOUT_GENERAL, &one, 1, &range);
                    vk.Barrier();
                    VkDescriptorBufferInfo data{lights.buffer, 0, (used + 32) * 4};
                    VkDescriptorImageInfo depth{vk.sampler, lab.depth.view, VK_IMAGE_LAYOUT_GENERAL};
                    std::array<VkDescriptorImageInfo, 4> images{};
                    for (int k = 0; k < 4; ++k) images[k] = {{}, lamp_images[k].view, VK_IMAGE_LAYOUT_GENERAL};
                    VkWriteDescriptorSetAccelerationStructureKHR as{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR,
                        .accelerationStructureCount = 1, .pAccelerationStructures = &tlas.handle};
                    const int cur_history = parity < 0 ? 0 : parity;
                    VkDescriptorImageInfo history_prev{{}, history[cur_history ^ 1].view, VK_IMAGE_LAYOUT_GENERAL};
                    VkDescriptorImageInfo history_cur{{}, history[cur_history].view, VK_IMAGE_LAYOUT_GENERAL};
                    std::array<VkWriteDescriptorSet, 9> writes{};
                    for (unsigned k = 0; k < 9; ++k) writes[k] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = k, .descriptorCount = 1};
                    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; writes[0].pBufferInfo = &data;
                    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; writes[1].pImageInfo = &depth;
                    for (int k = 0; k < 4; ++k) { writes[2 + k].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; writes[2 + k].pImageInfo = &images[k]; }
                    writes[6].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR; writes[6].pNext = &as;
                    writes[7].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; writes[7].pImageInfo = &history_prev;
                    writes[8].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; writes[8].pImageInfo = &history_cur;
                    const auto snapshot = [&](std::array<int, 4> r) {
                        const VkImageCopy region{.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .srcOffset = {r[0], r[1], 0},
                            .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .dstOffset = {r[0], r[1], 0},
                            .extent = {unsigned(r[2] - r[0]), unsigned(r[3] - r[1]), 1}};
                        vkCmdCopyImage(vk.cmd, zero_image.image, VK_IMAGE_LAYOUT_GENERAL, lamp_images[1].image, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
                        vkCmdCopyImage(vk.cmd, zero_image.image, VK_IMAGE_LAYOUT_GENERAL, lamp_images[3].image, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
                        vk.Barrier();
                    };
                    const auto dispatch = [&](LampPush push) {
                        vkCmdBindPipeline(vk.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, lamp.pipeline);
                        vk.push(vk.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, lamp.layout, 0, 9, writes.data());
                        vkCmdPushConstants(vk.cmd, lamp.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
                        const unsigned gx0 = unsigned(push.rect[0]) / 8, gy0 = unsigned(push.rect[1]) / 8;
                        const unsigned gx1 = (unsigned(push.rect[2]) + 7) / 8, gy1 = (unsigned(push.rect[3]) + 7) / 8;
                        vkCmdDispatchBase(vk.cmd, gx0, gy0, 0, gx1 - gx0, gy1 - gy0, 1);
                        vk.Barrier();
                    };
                    vk.Stamp(0);
                    if (parity >= 0) {
                        VkClearColorValue zero{};
                        vkCmdClearColorImage(vk.cmd, history[cur_history].image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &range);
                        vk.Barrier();
                    }
                    if (per_tile) {
                        for (std::size_t t = 0; t < tiles.size(); ++t) {
                            snapshot(tiles[t].rect);
                            LampPush push = tiles[t];
                            push.first_light = table + unsigned(t) * 8;
                            push.count = 1;
                            dispatch(push);
                        }
                    } else if (!tiles.empty()) {
                        snapshot({0, 0, int(W), int(H)});
                        LampPush push = tiles[0];
                        push.first_light = table;
                        push.count = unsigned(tiles.size());
                        push.rect = covered;
                        push.steps = pick;
                        push.frame = frame;
                        if (parity >= 0) std::memcpy(&push.length, &history_at, 4);
                        dispatch(push);
                    }
                    vk.Stamp(1);
                    vk.Barrier();
                    return vk.End();
                };
                if (at == 1) {
                    std::vector<double> per_tile;
                    for (int i = 0; i < 15; ++i) per_tile.push_back(run_lamps(true));
                    per_tile_ms[cam] = Median(per_tile);
                    const auto lamp_values = [&] {
                        const auto raw = lab.Read(lamp_images[0], VK_FORMAT_R32_SFLOAT);
                        std::vector<float> out(raw.size());
                        for (std::size_t k = 0; k < raw.size(); ++k) {
                            std::uint32_t bits;
                            std::memcpy(&bits, &raw[k], 4);
                            out[k] = decode(bits);
                        }
                        return out;
                    };
                    run_lamps(false);
                    const auto exact = lamp_values();
                    std::vector<double> picked;
                    for (int i = 0; i < 15; ++i) picked.push_back(run_lamps(false, 1, unsigned(i)));
                    pick_ms[cam] = Median(picked);
                    std::vector<float> average(exact.size(), 0.0f);
                    double one_frame = 0, sixteen = 0;
                    for (unsigned f = 0; f < 16; ++f) {
                        run_lamps(false, 1, f);
                        const auto one = lamp_values();
                        for (std::size_t k = 0; k < one.size(); ++k) {
                            average[k] += one[k] / 16.0f;
                            if (f == 0) one_frame += std::abs(double(one[k]) - exact[k]);
                        }
                    }
                    for (std::size_t k = 0; k < exact.size(); ++k) sixteen += std::abs(double(average[k]) - exact[k]);
                    pick_error[cam] = {one_frame / double(exact.size()), sixteen / double(exact.size())};
                    std::vector<double> reused;
                    std::vector<float> reuse_average(exact.size(), 0.0f);
                    for (unsigned f = 0; f < 24; ++f) {
                        const double ms = run_lamps(false, 1, f, int(f & 1u));
                        if (f >= 8) reused.push_back(ms);
                        if (f >= 8) {
                            const auto one = lamp_values();
                            for (std::size_t k = 0; k < one.size(); ++k) reuse_average[k] += one[k] / 16.0f;
                        }
                    }
                    reuse_ms[cam] = Median(reused);
                    double reuse_sum = 0;
                    for (std::size_t k = 0; k < exact.size(); ++k) reuse_sum += std::abs(double(reuse_average[k]) - exact[k]);
                    reuse_error[cam] = reuse_sum / double(exact.size());
                }
                for (int i = 0; i < 15; ++i) lamp_samples.push_back(run_lamps(false));
                sun_ms[at][cam] = Median(sun_samples);
                lamp_ms[at][cam] = Median(lamp_samples);
                if (at == 0) {
                    const auto out = lab.Read(lamp_images[0], VK_FORMAT_R32_SFLOAT);
                    double lit = 0;
                    for (const float f : out) {
                        std::uint32_t bits;
                        std::memcpy(&bits, &f, 4);
                        lit += decode(bits);
                    }
                    lit_sum += lit / double(out.size()) / cameras.size();
                }
                vk.destroy_as(vk.device, tlas.handle, nullptr);
                vk.Free(tlas.instances); vk.Free(tlas.storage); vk.Free(tlas.scratch);
            }
        }
        std::printf("%u lamp tiles over %zu cameras, mean lamp light kept %.4f\n", tile_sum, cameras.size(), lit_sum);
        std::printf("Lamps on the fast-trace TLAS, a snapshot, dispatch and barriers per tile: %.3f %.3f %.3f ms\n",
                    per_tile_ms[0], per_tile_ms[1], per_tile_ms[2]);
        std::printf("Lamps, strongest lamp plus one picked by weight: %.3f %.3f %.3f ms; mean error against every lamp "
                    "%.4f %.4f %.4f in one frame, %.4f %.4f %.4f over 16 frames\n",
                    pick_ms[0], pick_ms[1], pick_ms[2], pick_error[0][0], pick_error[1][0], pick_error[2][0],
                    pick_error[0][1], pick_error[1][1], pick_error[2][1]);
        std::printf("Lamps, picked, reusing still 8x8 blocks (still camera, frames 8-23): %.3f %.3f %.3f ms; mean error "
                    "against every lamp over 16 frames %.4f %.4f %.4f\n",
                    reuse_ms[0], reuse_ms[1], reuse_ms[2], reuse_error[0], reuse_error[1], reuse_error[2]);
        std::printf("%-36s %9s %21s %21s\n", "", "TLAS ms", "sun 2 rays ms", "lamp rays ms");
        for (std::size_t at = 0; at < variants.size(); ++at) {
            std::printf("%-36s %9.3f   %5.3f %5.3f %5.3f   %5.3f %5.3f %5.3f\n", variants[at].name, tlas_ms[at],
                        sun_ms[at][0], sun_ms[at][1], sun_ms[at][2], lamp_ms[at][0], lamp_ms[at][1], lamp_ms[at][2]);
        }
        return 0;
    }
    for (const auto [total, draws] : std::array<std::array<unsigned, 2>, 3>{{{500000, 400}, {1500000, 800}, {4000000, 1200}}}) {
        Scene scene{vk, MakeScene(total, draws)};
        scene.Create(Trace | Update);
        std::printf("\n== Scene: %u triangles in %zu BLAS\n", scene.triangles, scene.blas.size());
        const auto report_builds = [&](const char* name, VkBuildAccelerationStructureFlagsKHR flags) {
            scene.Sizes(flags);
            scene.Allocate();
            std::vector<double> times;
            for (int i = 0; i < 5; ++i) times.push_back(scene.BuildAll(flags));
            std::printf("All BLAS %-22s build %8.2f ms, %7.1f MB, scratch %7.1f MB\n", name, Median(times),
                        double(scene.total_bytes) / 1048576.0, double(scene.total_scratch) / 1048576.0);
        };
        report_builds("fast build", Fast);
        report_builds("fast trace", Trace);
        report_builds("fast build + update", Fast | Update);
        {
            std::vector<double> refit, rebuild;
            for (int i = 0; i < 9; ++i) refit.push_back(scene.Refit(Fast | Update, true));
            for (int i = 0; i < 9; ++i) rebuild.push_back(scene.Refit(Fast | Update, false));
            std::printf("6 characters, 180k triangles: refit %.3f ms, rebuild %.3f ms\n", Median(refit), Median(rebuild));
        }
        for (const bool fast_blas : {false, true}) {
            const VkBuildAccelerationStructureFlagsKHR flags = fast_blas ? Fast : Trace;
            scene.Sizes(flags);
            scene.Allocate();
            scene.BuildAll(flags);
            Tlas tlas = CreateTlas(vk, scene);
            std::array<double, 5> sums{};
            std::array<Metric, 6> metrics{};
            double tlas_ms = 0;
            for (std::size_t cam = 0; cam < cameras.size(); ++cam) {
                const Views views = MakeViews(cameras[cam][0], cameras[cam][1], sun);
                std::vector<double> times;
                for (int i = 0; i < 9; ++i) times.push_back(BuildTlas(vk, tlas, scene, views.view, true));
                tlas_ms += Median(times) / cameras.size();
                lab.Prepare(tlas.handle, views);
                const auto depth = lab.Read(lab.depth, VK_FORMAT_R32_SFLOAT);
                if (fast_blas) continue;
                const auto covered = std::count_if(depth.begin(), depth.end(), [](float d) { return d < 1.0f; });
                if (covered < depth.size() / 4) {
                    std::fprintf(stderr, "camera %zu sees geometry in only %lld pixels\n", cam, (long long)covered);
                    return 1;
                }
                std::vector<float> truth(W * H, 0.0f);
                for (unsigned frame = 0; frame < 64; ++frame) {
                    vk.Begin();
                    RtPush p;
                    p.frame = frame;
                    p.rays = 16;
                    lab.Rt(tlas.handle, p);
                    vk.Barrier();
                    vk.Stamp(0); vk.Stamp(1);
                    vk.End();
                    const auto one = lab.Read(lab.raw, VK_FORMAT_R8_UNORM);
                    for (std::size_t i = 0; i < truth.size(); ++i) truth[i] += one[i] / 64.0f;
                }
                const auto timed = [&](auto&& body) {
                    std::vector<double> samples;
                    for (int i = 0; i < 16; ++i) {
                        vk.Begin();
                        vk.Stamp(0);
                        body();
                        vk.Stamp(1);
                        vk.Barrier();
                        samples.push_back(vk.End());
                    }
                    return Median(samples);
                };
                const auto add = [&](Metric& into, const Metric& m) {
                    into.mean += m.mean / cameras.size(); into.off10 += m.off10 / cameras.size();
                    into.off25 += m.off25 / cameras.size(); into.signed_bias += m.signed_bias / cameras.size();
                };
                vk.Begin(); lab.ClearShadow(); vk.Stamp(0); vk.Stamp(1); vk.End();
                sums[0] += timed([&] { lab.Pcss({}); }) / cameras.size();
                vk.Begin(); lab.ClearShadow(); vk.Stamp(0); vk.Stamp(1); vk.End();
                vk.Begin(); lab.Pcss({}); vk.Barrier(); vk.Stamp(0); vk.Stamp(1); vk.End();
                const auto pcss = lab.Read(lab.shadow, VK_FORMAT_R8G8B8A8_UNORM);
                add(metrics[0], Compare(truth, pcss, depth));
                unsigned at = 1;
                std::vector<float> rt_best;
                for (unsigned rays : {1u, 2u, 4u, 8u}) {
                    RtPush p;
                    p.rays = rays;
                    sums[at] += timed([&] { lab.Rt(tlas.handle, p); }) / cameras.size();
                    vk.Begin(); lab.Rt(tlas.handle, p); vk.Barrier(); vk.Stamp(0); vk.Stamp(1); vk.End();
                    vk.Begin(); lab.ClearShadow(); lab.Filter(2); vk.Barrier(); vk.Stamp(0); vk.Stamp(1); vk.End();
                    const auto out = lab.Read(lab.shadow, VK_FORMAT_R8G8B8A8_UNORM);
                    add(metrics[at], Compare(truth, out, depth));
                    if (rays == 4) rt_best = out;
                    ++at;
                }
                metrics[5].off10 += timed([&] { lab.Filter(2); }) / cameras.size();
                if (dump && total == 1500000 && cam < 3) {
                    const std::string base = std::string(dump) + "/cam" + std::to_string(cam);
                    WritePgm((base + "_truth.pgm").c_str(), truth);
                    WritePgm((base + "_pcss.pgm").c_str(), pcss);
                    WritePgm((base + "_rt4.pgm").c_str(), rt_best);
                }
            }
            std::printf("TLAS %zu instances on %s BLAS: %.3f ms\n", scene.blas.size(), fast_blas ? "fast-build" : "fast-trace", tlas_ms);
            if (fast_blas) {
                const Views views = MakeViews(cameras[0][0], cameras[0][1], sun);
                BuildTlas(vk, tlas, scene, views.view, true);
                lab.Prepare(tlas.handle, views);
                std::vector<double> samples;
                for (int i = 0; i < 16; ++i) {
                    vk.Begin();
                    vk.Stamp(0);
                    RtPush p;
                    p.rays = 2;
                    lab.Rt(tlas.handle, p);
                    vk.Stamp(1);
                    vk.Barrier();
                    samples.push_back(vk.End());
                }
                std::printf("2 rays per pixel on fast-build BLAS, camera 0: %.3f ms\n", Median(samples));
            } else {
                std::printf("%-20s %8s %9s %8s %8s %8s\n", "Against 1024 rays", "GPU ms", "mean err", ">0.10 %", ">0.25 %", "bias");
                std::printf("%-20s %8.3f %9.4f %8.2f %8.2f %+8.4f\n", "PCSS 4096 atlas", sums[0], metrics[0].mean, metrics[0].off10, metrics[0].off25, metrics[0].signed_bias);
                unsigned at = 1;
                for (unsigned rays : {1u, 2u, 4u, 8u}) {
                    std::printf("RT %u rays + filter  %8.3f %9.4f %8.2f %8.2f %+8.4f\n", rays, sums[at] + metrics[5].off10, metrics[at].mean, metrics[at].off10, metrics[at].off25, metrics[at].signed_bias);
                    ++at;
                }
                std::printf("Filter alone: %.3f ms\n", metrics[5].off10);
            }
            vk.destroy_as(vk.device, tlas.handle, nullptr);
            vk.Free(tlas.instances); vk.Free(tlas.storage); vk.Free(tlas.scratch);
        }
        for (auto& b : scene.blas) if (b.handle) vk.destroy_as(vk.device, b.handle, nullptr);
        vk.Free(scene.storage); vk.Free(scene.scratch); vk.Free(scene.geometry); vk.Free(scene.staging);
    }
    return 0;
}
