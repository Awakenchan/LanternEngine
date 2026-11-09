// SPDX-License-Identifier: GPL-3.0-or-later
// bbport: NVIDIA DLSS Frame Generation through NVIDIA Streamline (vk_streamline.h).

#include "video_core/renderer_vulkan/vk_streamline.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <type_traits>
#include <mutex>
#include <string>

#ifdef _WIN32
#include <windows.h>
#include "sl.h"
#include "sl_dlss_g.h"
#include "sl_pcl.h"
#include "sl_reflex.h"
#include "sl_security.h"
#endif

#include "bbport_overlay.h"
#include "bbport_settings.h"
#include "video_core/host_shaders/dlss_ui_alpha_comp.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_optiscaler.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"

namespace Vulkan {

namespace {

// The scene's far plane (the scene constants give near and the field of view; the upscaler and
// FSR 3.1 frame generation pass the same).
constexpr float FarPlane = 3000.0f;
// A pixel is UI where the back buffer differs from the HUD-less frame by more than this (the
// two are bit-identical where no UI is drawn; this only ignores rounding).
constexpr float UiThreshold = 1.5f / 255.0f;

std::atomic<const char*> problem{nullptr};

void SetProblem(const char* why) {
    problem.store(why, std::memory_order_relaxed);
    BbSettings::Get().dlss_fg_problem.store(why, std::memory_order_relaxed);
    std::printf("DLSS Frame Generation: %s\n", why);
    std::fflush(stdout);
}

#ifdef _WIN32

HMODULE module = nullptr;
bool initialized = false;
bool device_ready = false;

PFun_slInit* sl_init = nullptr;
PFun_slShutdown* sl_shutdown = nullptr;
PFun_slIsFeatureSupported* sl_is_feature_supported = nullptr;
PFun_slGetFeatureFunction* sl_get_feature_function = nullptr;
PFun_slGetNewFrameToken* sl_get_new_frame_token = nullptr;
PFun_slSetTagForFrame* sl_set_tag_for_frame = nullptr;
PFun_slSetConstants* sl_set_constants = nullptr;
PFun_slDLSSGSetOptions* sl_dlssg_set_options = nullptr;
PFun_slDLSSGGetState* sl_dlssg_get_state = nullptr;
PFun_slReflexSetOptions* sl_reflex_set_options = nullptr;
PFun_slReflexSleep* sl_reflex_sleep = nullptr;
PFun_slPCLSetMarker* sl_pcl_set_marker = nullptr;

std::atomic<VkResult> last_api_error{VK_SUCCESS};

const char* ResultName(sl::Result r) {
    switch (r) {
    case sl::Result::eOk: return "ok";
    case sl::Result::eErrorDriverOutOfDate: return "the NVIDIA driver is too old";
    case sl::Result::eErrorOSOutOfDate: return "Windows is too old (Windows 10 20H1 or newer)";
    case sl::Result::eErrorOSDisabledHWS:
        return "Hardware-accelerated GPU scheduling is off (Windows Settings > Display > Graphics)";
    case sl::Result::eErrorNoSupportedAdapterFound:
    case sl::Result::eErrorAdapterNotSupported: return "this GPU has no DLSS Frame Generation (RTX 40 or 50)";
    case sl::Result::eErrorNoPlugins:
    case sl::Result::eErrorFeatureMissing:
    case sl::Result::eErrorFeatureFailedToLoad: return "a Streamline plugin is missing or failed to load";
    case sl::Result::eErrorFeatureNotSupported: return "DLSS Frame Generation is not supported here";
    case sl::Result::eErrorFeatureMissingDependency: return "a Streamline plugin's dependency is missing";
    case sl::Result::eErrorNGXFailed: return "NVIDIA NGX failed";
    case sl::Result::eErrorVulkanAPI: return "a Vulkan call failed";
    case sl::Result::eWarnOutOfVRAM: return "out of video memory";
    default: return "Streamline error";
    }
}

void Log(sl::LogType type, const char* message) {
    // Streamline's own lines; warnings and errors always, the rest with BB_STREAMLINE_LOG=1.
    static const bool verbose = [] {
        const char* env = std::getenv("BB_STREAMLINE_LOG");
        return env && env[0] == '1';
    }();
    if (type == sl::LogType::eInfo && !verbose) {
        return;
    }
    std::printf("Streamline: %s%s", message,
                *message && message[std::strlen(message) - 1] == '\n' ? "" : "\n");
}

void OnApiError(const sl::APIError& error) {
    // On Streamline's present thread: kept, not handled here (it must not block).
    last_api_error.store(error.vkRes, std::memory_order_relaxed);
}

template <typename Fn>
bool Feature(sl::Feature feature, const char* name, Fn*& fn) {
    void* function = nullptr;
    if (sl_get_feature_function(feature, name, function) != sl::Result::eOk || !function) {
        std::printf("Streamline: %s missing\n", name);
        return false;
    }
    fn = reinterpret_cast<Fn*>(function);
    return true;
}

sl::FrameToken* Token(u32 index) {
    sl::FrameToken* token = nullptr;
    if (sl_get_new_frame_token(token, &index) != sl::Result::eOk) {
        return nullptr;
    }
    return token;
}

void Marker(sl::PCLMarker marker, u32 index) {
    if (sl_pcl_set_marker) {
        if (sl::FrameToken* token = Token(index)) {
            sl_pcl_set_marker(marker, *token);
        }
    }
}

std::wstring EngineDir() {
    if (const char* env = std::getenv("BB_STREAMLINE_DIR"); env && *env) {
        return std::filesystem::path(env).wstring();
    }
    wchar_t exe[MAX_PATH];
    const DWORD length = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) {
        return {};
    }
    return std::filesystem::path(exe).parent_path().wstring();
}

#endif // _WIN32

// Matrices for Streamline's constants: row-major, row vectors (p' = p * M), D3D clip space (ndc
// +y up, depth 0..1). The engine's camera (CameraMotion::Matrices): column vectors, view z
// forward, ndc +y down.
using Mat4 = std::array<std::array<float, 4>, 4>;

Mat4 Identity() {
    Mat4 m{};
    for (int i = 0; i < 4; ++i) m[i][i] = 1.0f;
    return m;
}

Mat4 Multiply(const Mat4& a, const Mat4& b) {
    Mat4 r{};
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) {
            float sum = 0.0f;
            for (int k = 0; k < 4; ++k) sum += a[i][k] * b[k][j];
            r[i][j] = sum;
        }
    }
    return r;
}

/// General 4x4 inverse (cofactors); the identity for a singular matrix.
Mat4 Inverse(const Mat4& m) {
    const float* a = &m[0][0];
    float inv[16];
    inv[0] = a[5] * a[10] * a[15] - a[5] * a[11] * a[14] - a[9] * a[6] * a[15] + a[9] * a[7] * a[14] + a[13] * a[6] * a[11] - a[13] * a[7] * a[10];
    inv[4] = -a[4] * a[10] * a[15] + a[4] * a[11] * a[14] + a[8] * a[6] * a[15] - a[8] * a[7] * a[14] - a[12] * a[6] * a[11] + a[12] * a[7] * a[10];
    inv[8] = a[4] * a[9] * a[15] - a[4] * a[11] * a[13] - a[8] * a[5] * a[15] + a[8] * a[7] * a[13] + a[12] * a[5] * a[11] - a[12] * a[7] * a[9];
    inv[12] = -a[4] * a[9] * a[14] + a[4] * a[10] * a[13] + a[8] * a[5] * a[14] - a[8] * a[6] * a[13] - a[12] * a[5] * a[10] + a[12] * a[6] * a[9];
    inv[1] = -a[1] * a[10] * a[15] + a[1] * a[11] * a[14] + a[9] * a[2] * a[15] - a[9] * a[3] * a[14] - a[13] * a[2] * a[11] + a[13] * a[3] * a[10];
    inv[5] = a[0] * a[10] * a[15] - a[0] * a[11] * a[14] - a[8] * a[2] * a[15] + a[8] * a[3] * a[14] + a[12] * a[2] * a[11] - a[12] * a[3] * a[10];
    inv[9] = -a[0] * a[9] * a[15] + a[0] * a[11] * a[13] + a[8] * a[1] * a[15] - a[8] * a[3] * a[13] - a[12] * a[1] * a[11] + a[12] * a[3] * a[9];
    inv[13] = a[0] * a[9] * a[14] - a[0] * a[10] * a[13] - a[8] * a[1] * a[14] + a[8] * a[2] * a[13] + a[12] * a[1] * a[10] - a[12] * a[2] * a[9];
    inv[2] = a[1] * a[6] * a[15] - a[1] * a[7] * a[14] - a[5] * a[2] * a[15] + a[5] * a[3] * a[14] + a[13] * a[2] * a[7] - a[13] * a[3] * a[6];
    inv[6] = -a[0] * a[6] * a[15] + a[0] * a[7] * a[14] + a[4] * a[2] * a[15] - a[4] * a[3] * a[14] - a[12] * a[2] * a[7] + a[12] * a[3] * a[6];
    inv[10] = a[0] * a[5] * a[15] - a[0] * a[7] * a[13] - a[4] * a[1] * a[15] + a[4] * a[3] * a[13] + a[12] * a[1] * a[7] - a[12] * a[3] * a[5];
    inv[14] = -a[0] * a[5] * a[14] + a[0] * a[6] * a[13] + a[4] * a[1] * a[14] - a[4] * a[2] * a[13] - a[12] * a[1] * a[6] + a[12] * a[2] * a[5];
    inv[3] = -a[1] * a[6] * a[11] + a[1] * a[7] * a[10] + a[5] * a[2] * a[11] - a[5] * a[3] * a[10] - a[9] * a[2] * a[7] + a[9] * a[3] * a[6];
    inv[7] = a[0] * a[6] * a[11] - a[0] * a[7] * a[10] - a[4] * a[2] * a[11] + a[4] * a[3] * a[10] + a[8] * a[2] * a[7] - a[8] * a[3] * a[6];
    inv[11] = -a[0] * a[5] * a[11] + a[0] * a[7] * a[9] + a[4] * a[1] * a[11] - a[4] * a[3] * a[9] - a[8] * a[1] * a[7] + a[8] * a[3] * a[5];
    inv[15] = a[0] * a[5] * a[10] - a[0] * a[6] * a[9] - a[4] * a[1] * a[10] + a[4] * a[2] * a[9] + a[8] * a[1] * a[6] - a[8] * a[2] * a[5];
    const float det = a[0] * inv[0] + a[1] * inv[4] + a[2] * inv[8] + a[3] * inv[12];
    if (!std::isfinite(det) || std::abs(det) < 1e-20f) {
        return Identity();
    }
    Mat4 r{};
    for (int i = 0; i < 16; ++i) (&r[0][0])[i] = inv[i] / det;
    return r;
}

/// A 3x4 rows world-to-view (column vectors) as a row-vector 4x4: the transpose.
Mat4 RowVectorView(const std::array<float, 12>& v) {
    Mat4 m{};
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 4; ++col) m[col][row] = v[row * 4 + col];
    }
    m[3][3] = 1.0f;
    return m;
}

/// The projection (x, y, z scale, z offset; clip w = view z) as a row-vector view-to-clip with
/// ndc +y up (the engine's y scale is for +y down).
Mat4 RowVectorProjection(const std::array<float, 4>& p) {
    Mat4 m{};
    m[0][0] = p[0];
    m[1][1] = -p[1];
    m[2][2] = p[2];
    m[2][3] = 1.0f;
    m[3][2] = p[3];
    return m;
}

#ifdef _WIN32
sl::float4x4 ToSl(const Mat4& m) {
    sl::float4x4 r;
    for (int i = 0; i < 4; ++i) r.setRow(i, sl::float4(m[i][0], m[i][1], m[i][2], m[i][3]));
    return r;
}

sl::Resource Image(vk::Image image, vk::ImageView view, vk::Format format, u32 w, u32 h,
                   vk::ImageUsageFlags usage) {
    sl::Resource r{sl::ResourceType::eTex2d, static_cast<VkImage>(image), nullptr,
                   static_cast<VkImageView>(view), u32(VK_IMAGE_LAYOUT_GENERAL)};
    r.width = w;
    r.height = h;
    r.nativeFormat = u32(format);
    r.mipLevels = 1;
    r.arrayLayers = 1;
    r.flags = 0;
    r.usage = u32(static_cast<VkImageUsageFlags>(usage));
    return r;
}
#endif

} // namespace

namespace Streamline {

PFN_vkGetInstanceProcAddr LoadBeforeVulkan() {
    if (BbSettings::Get().startup_frame_gen != BbSettings::FrameGenDlss) {
        return nullptr;
    }
#ifdef _WIN32
    if (initialized) {
        // Another instance in the same process (a tool): the same loader.
        return reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(module, "vkGetInstanceProcAddr"));
    }
#endif
#ifndef _WIN32
    SetProblem("not available on Linux (NVIDIA ships Streamline for Windows only)");
    return nullptr;
#else
    if (OptiScaler::Detect().loaded) {
        SetProblem("OptiScaler is loaded (it hooks the same Vulkan calls): remove it to use DLSS "
                   "Frame Generation");
        return nullptr;
    }
    const std::wstring dir = EngineDir();
    const std::wstring interposer = dir + L"\\sl.interposer.dll";
    if (dir.empty() || !std::filesystem::exists(interposer)) {
        SetProblem("sl.interposer.dll is not next to the engine (tools/fetch_streamline.sh)");
        return nullptr;
    }
    if (!sl::security::verifyEmbeddedSignature(interposer.c_str())) {
        SetProblem("sl.interposer.dll is not signed by NVIDIA: not loaded");
        return nullptr;
    }
    module = LoadLibraryW(interposer.c_str());
    if (!module) {
        SetProblem("sl.interposer.dll could not be loaded");
        return nullptr;
    }
    const auto load = [](auto& fn, const char* name) {
        fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(GetProcAddress(module, name));
        return fn != nullptr;
    };
    PFN_vkGetInstanceProcAddr get_instance_proc_addr = nullptr;
    if (!load(sl_init, "slInit") || !load(sl_shutdown, "slShutdown") ||
        !load(sl_is_feature_supported, "slIsFeatureSupported") ||
        !load(sl_get_feature_function, "slGetFeatureFunction") ||
        !load(sl_get_new_frame_token, "slGetNewFrameToken") ||
        !load(sl_set_tag_for_frame, "slSetTagForFrame") ||
        !load(sl_set_constants, "slSetConstants") ||
        !load(get_instance_proc_addr, "vkGetInstanceProcAddr")) {
        SetProblem("sl.interposer.dll lacks Streamline's entry points");
        FreeLibrary(module);
        module = nullptr;
        return nullptr;
    }
    static const std::wstring plugin_dir = dir;
    static const wchar_t* plugin_paths[] = {plugin_dir.c_str()};
    static const sl::Feature features[] = {sl::kFeatureDLSS_G, sl::kFeatureReflex, sl::kFeaturePCL};
    sl::Preferences preferences{};
    preferences.showConsole = false;
    preferences.logLevel = std::getenv("BB_STREAMLINE_LOG") ? sl::LogLevel::eVerbose
                                                            : sl::LogLevel::eDefault;
    preferences.pathsToPlugins = plugin_paths;
    preferences.numPathsToPlugins = 1;
    preferences.logMessageCallback = &Log;
    // No over-the-air updates (nothing is downloaded at run time); tags per frame; the engine
    // restores its own command buffer state.
    preferences.flags = sl::PreferenceFlags::eDisableCLStateTracking |
                        sl::PreferenceFlags::eDisableDebugText |
                        sl::PreferenceFlags::eUseFrameBasedResourceTagging;
    preferences.featuresToLoad = features;
    preferences.numFeaturesToLoad = u32(std::size(features));
    preferences.applicationId = 231313132; // NVIDIA's id for unregistered applications (as NGX)
    preferences.engine = sl::EngineType::eCustom;
    preferences.engineVersion = "Lantern Engine";
    if (const char* use_project_id = std::getenv("BB_DLSS_PROJECT_ID"); use_project_id && use_project_id[0] == '1') {
        preferences.engineVersion = "0.9.40";
        preferences.projectId = "5c1f3a9e-7b42-4d86-9e31-0f6a2b7cd481";
    }
    preferences.renderAPI = sl::RenderAPI::eVulkan;
    if (const sl::Result r = sl_init(preferences, sl::kSDKVersion); r != sl::Result::eOk) {
        static std::string why;
        why = std::string("Streamline did not start: ") + ResultName(r);
        SetProblem(why.c_str());
        FreeLibrary(module);
        module = nullptr;
        return nullptr;
    }
    initialized = true;
    BbSettings::Get().streamline_loaded = true;
    std::printf("DLSS Frame Generation: Streamline %d.%d.%d loaded; the Vulkan instance and device "
                "go through it\n",
                SL_VERSION_MAJOR, SL_VERSION_MINOR, SL_VERSION_PATCH);
    std::fflush(stdout);
    return get_instance_proc_addr;
#endif
}

bool Loaded() {
#ifdef _WIN32
    return initialized;
#else
    return false;
#endif
}

void OnDeviceCreated(vk::PhysicalDevice physical) {
#ifdef _WIN32
    if (!initialized) {
        return;
    }
    sl::AdapterInfo adapter{};
    adapter.vkPhysicalDevice = static_cast<VkPhysicalDevice>(physical);
    if (const sl::Result r = sl_is_feature_supported(sl::kFeatureDLSS_G, adapter);
        r != sl::Result::eOk) {
        static std::string why;
        why = ResultName(r);
        SetProblem(why.c_str());
    } else if (!Feature(sl::kFeatureDLSS_G, "slDLSSGSetOptions", sl_dlssg_set_options) ||
               !Feature(sl::kFeatureDLSS_G, "slDLSSGGetState", sl_dlssg_get_state)) {
        SetProblem("its DLSS-G plugin lacks its functions");
    } else {
        device_ready = true;
    }
    // Reflex and its markers work on any GPU (PCL); low latency only on NVIDIA's.
    Feature(sl::kFeatureReflex, "slReflexSetOptions", sl_reflex_set_options);
    Feature(sl::kFeatureReflex, "slReflexSleep", sl_reflex_sleep);
    Feature(sl::kFeaturePCL, "slPCLSetMarker", sl_pcl_set_marker);
    if (!device_ready) {
        return;
    }
    sl::DLSSGState state{};
    const sl::ViewportHandle viewport{0};
    int max_frames = 1;
    bool dynamic = false;
    if (sl_dlssg_get_state(viewport, state, nullptr) == sl::Result::eOk) {
        max_frames = std::clamp(int(state.numFramesToGenerateMax), 1, BbSettings::FrameGenFramesMax);
        dynamic = state.bIsDynamicMFGSupported == sl::Boolean::eTrue;
    }
    auto& s = BbSettings::Get();
    s.dlss_fg_max_frames = max_frames;
    s.dlss_fg_dynamic_supported = dynamic && max_frames > 1;
    s.dlss_fg_supported = true;
    s.dlss_fg_problem = nullptr;
    problem = nullptr;
    std::printf("DLSS Frame Generation: available, up to %d generated frame%s per real one (%dx)%s\n",
                max_frames, max_frames == 1 ? "" : "s", max_frames + 1,
                s.dlss_fg_dynamic_supported ? ", dynamic multi frame generation too" : "");
    std::fflush(stdout);
#else
    (void)physical;
#endif
}

void Shutdown() {
#ifdef _WIN32
    if (!initialized) {
        return;
    }
    initialized = false;
    device_ready = false;
    BbSettings::Get().dlss_fg_supported = false;
    BbSettings::Get().streamline_loaded = false;
    sl_shutdown();
#endif
}

} // namespace Streamline

DlssFrameGen::DlssFrameGen(const Instance& instance_, u32 slot_count) : instance{instance_} {
    slots.resize(slot_count);
}

DlssFrameGen::~DlssFrameGen() = default;

bool DlssFrameGen::Available() const {
#ifdef _WIN32
    return Streamline::Loaded() && device_ready && !failed;
#else
    return false;
#endif
}

u32 DlssFrameGen::CurrentFrame() {
#ifdef _WIN32
    if (!Streamline::Loaded()) {
        return 0;
    }
    if (current_frame == 0) {
        current_frame = 1;
        if (sl_reflex_sleep) {
            if (sl::FrameToken* token = Token(current_frame)) sl_reflex_sleep(*token);
        }
        Marker(sl::PCLMarker::eSimulationStart, current_frame);
        Marker(sl::PCLMarker::eSimulationEnd, current_frame);
        Marker(sl::PCLMarker::eRenderSubmitStart, current_frame);
    }
    return current_frame;
#else
    return 0;
#endif
}

void DlssFrameGen::OnFrameSubmitted() {
#ifdef _WIN32
    if (!Streamline::Loaded() || current_frame == 0) {
        return;
    }
    Marker(sl::PCLMarker::eRenderSubmitEnd, current_frame);
    // The next game frame starts here on this thread: Reflex holds it back until the GPU is
    // about to need it (its sleep), then its simulation and render submission begin. The
    // guest's own simulation runs ahead in the command queue; these mark where its commands are
    // taken up.
    ++current_frame;
    if (sl_reflex_sleep) {
        if (sl::FrameToken* token = Token(current_frame)) sl_reflex_sleep(*token);
    }
    Marker(sl::PCLMarker::eSimulationStart, current_frame);
    Marker(sl::PCLMarker::eSimulationEnd, current_frame);
    Marker(sl::PCLMarker::eRenderSubmitStart, current_frame);
#endif
}

void DlssFrameGen::EnsureSlot(u32 index, u32 w, u32 h) {
    Slot& slot = slots[index % slots.size()];
    if (slot.width == w && slot.height == h && slot.depth_view) {
        return;
    }
    // Streamline may still read the old images for an earlier present: the presenting queue
    // waits for DLSS-G's work, so once it is idle they are free.
    {
        std::scoped_lock lock{Scheduler::submit_mutex};
        (void)instance.GetGraphicsQueue().waitIdle();
    }
    const auto device = instance.GetDevice();
    const auto make = [&](VideoCore::UniqueImage& image, vk::UniqueImageView& view,
                          vk::Format format) {
        view.reset();
        image = VideoCore::UniqueImage(device, instance.GetAllocator());
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
        view = Check(device.createImageViewUnique({
            .image = vk::Image(image),
            .viewType = vk::ImageViewType::e2D,
            .format = format,
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        }));
    };
    make(slot.depth, slot.depth_view, vk::Format::eR32Sfloat);
    make(slot.motion, slot.motion_view, vk::Format::eR16G16Sfloat);
    slot.width = w;
    slot.height = h;
}

void DlssFrameGen::RecordInputs(vk::CommandBuffer cmdbuf, u32 index, vk::Image depth,
                                vk::Image motion, u32 w, u32 h) const {
    const Slot& slot = slots[index % slots.size()];
    if (!slot.depth_view || slot.width != w || slot.height != h) {
        return;
    }
    const auto all = vk::PipelineStageFlagBits2::eAllCommands;
    const auto to = [&](vk::Image image, vk::ImageLayout old_layout, vk::ImageLayout new_layout,
                        vk::AccessFlags2 src, vk::AccessFlags2 dst) {
        return vk::ImageMemoryBarrier2{
            .srcStageMask = all,
            .srcAccessMask = src,
            .dstStageMask = all,
            .dstAccessMask = dst,
            .oldLayout = old_layout,
            .newLayout = new_layout,
            .image = image,
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        };
    };
    // The captures were written by this frame's commands before; the slot's previous contents
    // are not needed.
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
    cmdbuf.copyImage(depth, vk::ImageLayout::eGeneral, vk::Image(slot.depth),
                     vk::ImageLayout::eGeneral, region);
    cmdbuf.copyImage(motion, vk::ImageLayout::eGeneral, vk::Image(slot.motion),
                     vk::ImageLayout::eGeneral, region);
    const vk::MemoryBarrier2 copied{
        .srcStageMask = all,
        .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .dstStageMask = all,
        .dstAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
    };
    cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &copied});
}

void DlssFrameGen::CreatePipeline() {
    if (ui_pipeline) {
        return;
    }
    const auto device = instance.GetDevice();
    const std::array<vk::DescriptorSetLayoutBinding, 3> bindings{{
        {.binding = 0, .descriptorType = vk::DescriptorType::eSampledImage, .descriptorCount = 1,
         .stageFlags = vk::ShaderStageFlagBits::eCompute},
        {.binding = 1, .descriptorType = vk::DescriptorType::eSampledImage, .descriptorCount = 1,
         .stageFlags = vk::ShaderStageFlagBits::eCompute},
        {.binding = 2, .descriptorType = vk::DescriptorType::eStorageImage, .descriptorCount = 1,
         .stageFlags = vk::ShaderStageFlagBits::eCompute},
    }};
    desc_layout = Check(device.createDescriptorSetLayoutUnique({
        .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
        .bindingCount = u32(bindings.size()),
        .pBindings = bindings.data(),
    }));
    const vk::PushConstantRange push{vk::ShaderStageFlagBits::eCompute, 0, sizeof(float)};
    pipeline_layout = Check(device.createPipelineLayoutUnique({
        .setLayoutCount = 1,
        .pSetLayouts = &*desc_layout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &push,
    }));
    const auto module = CompileSPV(DLSS_UI_ALPHA_COMP, device);
    ui_pipeline = Check(device.createComputePipelineUnique(
        {}, vk::ComputePipelineCreateInfo{
                .stage = {.stage = vk::ShaderStageFlagBits::eCompute, .module = module,
                          .pName = "main"},
                .layout = *pipeline_layout,
            }));
    device.destroyShaderModule(module);
}

void DlssFrameGen::EnsurePresentImages(vk::Format format, vk::Extent2D extent) {
    if (ui_view && present_format == format && present_extent == extent) {
        return;
    }
    {
        std::scoped_lock lock{Scheduler::submit_mutex};
        (void)instance.GetGraphicsQueue().waitIdle();
    }
    CreatePipeline();
    const auto device = instance.GetDevice();
    const auto make = [&](VideoCore::UniqueImage& image, vk::UniqueImageView& view,
                          vk::Format image_format, vk::ImageUsageFlags usage) {
        view.reset();
        image = VideoCore::UniqueImage(device, instance.GetAllocator());
        image.Create(vk::ImageCreateInfo{
            .imageType = vk::ImageType::e2D,
            .format = image_format,
            .extent = {extent.width, extent.height, 1},
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = vk::SampleCountFlagBits::e1,
            .tiling = vk::ImageTiling::eOptimal,
            .usage = usage,
            .initialLayout = vk::ImageLayout::eUndefined,
        });
        view = Check(device.createImageViewUnique({
            .image = vk::Image(image),
            .viewType = vk::ImageViewType::e2D,
            .format = image_format,
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        }));
    };
    const auto color = vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst |
                       vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eStorage;
    make(hudless_image, hudless_view, format, color);
    make(final_image, final_view, format, color);
    // R16F: Streamline's Vulkan format table has no R8_UNORM (a UI alpha in it was refused on
    // every present).
    make(ui_image, ui_view, vk::Format::eR16Sfloat,
         vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eStorage);
    present_format = format;
    present_extent = extent;
}

void DlssFrameGen::RecordPresent(vk::CommandBuffer cmdbuf, const DlssFrameInfo& info,
                                 vk::Image hudless, vk::ImageView hudless_frame_view,
                                 vk::Extent2D frame_extent,
                                 vk::Image backbuffer, vk::Format format, vk::Extent2D extent,
                                 vk::Rect2D fit) {
    tag_hudless = nullptr;
    tag_hudless_view = nullptr;
    tag_ui = false;
    if (!Available() || !info.inputs || !hudless) {
        return;
    }
    EnsurePresentImages(format, extent);
    const auto all = vk::PipelineStageFlagBits2::eAllCommands;
    const auto barrier = [&](vk::Image image, vk::ImageLayout old_layout,
                             vk::ImageLayout new_layout, vk::AccessFlags2 src,
                             vk::AccessFlags2 dst) {
        return vk::ImageMemoryBarrier2{
            .srcStageMask = all,
            .srcAccessMask = src,
            .dstStageMask = all,
            .dstAccessMask = dst,
            .oldLayout = old_layout,
            .newLayout = new_layout,
            .image = image,
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        };
    };
    const auto write = vk::AccessFlagBits2::eMemoryWrite;
    const auto read = vk::AccessFlagBits2::eMemoryRead;
    // The HUD-less frame as the back buffer holds the frame: the same fit and black bars.
    const bool covers = fit.offset.x == 0 && fit.offset.y == 0 && fit.extent == extent &&
                        frame_extent == extent;
    {
        std::array<vk::ImageMemoryBarrier2, 4> before{
            barrier(backbuffer, vk::ImageLayout::ePresentSrcKHR,
                    vk::ImageLayout::eTransferSrcOptimal, write, read),
            barrier(vk::Image(final_image), vk::ImageLayout::eUndefined,
                    vk::ImageLayout::eTransferDstOptimal, {}, write),
            barrier(hudless, vk::ImageLayout::eGeneral, vk::ImageLayout::eTransferSrcOptimal,
                    write, read),
            barrier(vk::Image(hudless_image), vk::ImageLayout::eUndefined,
                    vk::ImageLayout::eTransferDstOptimal, {}, write),
        };
        cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = covers ? 2u : 4u,
                                 .pImageMemoryBarriers = before.data()});
    }
    const vk::ImageCopy whole{
        .srcSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
        .dstSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
        .extent = {extent.width, extent.height, 1},
    };
    cmdbuf.copyImage(backbuffer, vk::ImageLayout::eTransferSrcOptimal, vk::Image(final_image),
                     vk::ImageLayout::eTransferDstOptimal, whole);
    if (!covers) {
        const vk::ClearColorValue black{std::array<float, 4>{0.0f, 0.0f, 0.0f, 1.0f}};
        const vk::ImageSubresourceRange range{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
        cmdbuf.clearColorImage(vk::Image(hudless_image), vk::ImageLayout::eTransferDstOptimal,
                               black, range);
        const vk::MemoryBarrier2 cleared{.srcStageMask = all, .srcAccessMask = write,
                                         .dstStageMask = all, .dstAccessMask = write};
        cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &cleared});
        const vk::ImageBlit blit{
            .srcSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
            .srcOffsets = std::array{vk::Offset3D{0, 0, 0},
                                     vk::Offset3D{s32(frame_extent.width),
                                                  s32(frame_extent.height), 1}},
            .dstSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
            .dstOffsets = std::array{vk::Offset3D{fit.offset.x, fit.offset.y, 0},
                                     vk::Offset3D{fit.offset.x + s32(fit.extent.width),
                                                  fit.offset.y + s32(fit.extent.height), 1}},
        };
        cmdbuf.blitImage(hudless, vk::ImageLayout::eTransferSrcOptimal, vk::Image(hudless_image),
                         vk::ImageLayout::eTransferDstOptimal, blit, vk::Filter::eLinear);
    }
    {
        std::array<vk::ImageMemoryBarrier2, 5> after{
            barrier(backbuffer, vk::ImageLayout::eTransferSrcOptimal,
                    vk::ImageLayout::ePresentSrcKHR, read, {}),
            barrier(vk::Image(final_image), vk::ImageLayout::eTransferDstOptimal,
                    vk::ImageLayout::eGeneral, write, read),
            barrier(vk::Image(ui_image), vk::ImageLayout::eUndefined, vk::ImageLayout::eGeneral,
                    {}, write),
            barrier(hudless, vk::ImageLayout::eTransferSrcOptimal, vk::ImageLayout::eGeneral,
                    read, read),
            barrier(vk::Image(hudless_image), vk::ImageLayout::eTransferDstOptimal,
                    vk::ImageLayout::eGeneral, write, read),
        };
        cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = covers ? 3u : 5u,
                                 .pImageMemoryBarriers = after.data()});
    }
    // Where the back buffer differs from the HUD-less frame: the UI. A frame that fills the
    // back buffer gives its own HUD-less image; a letterboxed one the copy made above.
    tag_hudless = covers ? hudless : vk::Image(hudless_image);
    tag_hudless_view = covers ? hudless_frame_view : *hudless_view;
    const vk::DescriptorImageInfo final_info{.imageView = *final_view,
                                             .imageLayout = vk::ImageLayout::eGeneral};
    const vk::DescriptorImageInfo hudless_info{.imageView = tag_hudless_view,
                                               .imageLayout = vk::ImageLayout::eGeneral};
    const vk::DescriptorImageInfo ui_info{.imageView = *ui_view,
                                          .imageLayout = vk::ImageLayout::eGeneral};
    const std::array<vk::WriteDescriptorSet, 3> writes{{
        {.dstBinding = 0, .descriptorCount = 1, .descriptorType = vk::DescriptorType::eSampledImage,
         .pImageInfo = &final_info},
        {.dstBinding = 1, .descriptorCount = 1, .descriptorType = vk::DescriptorType::eSampledImage,
         .pImageInfo = &hudless_info},
        {.dstBinding = 2, .descriptorCount = 1, .descriptorType = vk::DescriptorType::eStorageImage,
         .pImageInfo = &ui_info},
    }};
    const float threshold = UiThreshold;
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, *ui_pipeline);
    cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, *pipeline_layout, 0, writes);
    cmdbuf.pushConstants(*pipeline_layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(threshold),
                         &threshold);
    cmdbuf.dispatch((extent.width + 7) / 8, (extent.height + 7) / 8, 1);
    const vk::MemoryBarrier2 done{
        .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
        .dstStageMask = all,
        .dstAccessMask = read,
    };
    cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &done});
    tag_ui = true;
}

void DlssFrameGen::SetMode(bool on) {
#ifdef _WIN32
    const int frames = std::clamp(BbSettings::Get().frame_gen_frames.load(std::memory_order_relaxed),
                                  1, std::max(1, BbSettings::Get().dlss_fg_max_frames.load()));
    // bbport: dynamic multi frame generation: up to `frames` generated frames, as many as the
    // target needs (the driver decides per moment).
    const bool dynamic = BbSettings::Get().frame_gen_dynamic.load(std::memory_order_relaxed) &&
                         BbSettings::Get().dlss_fg_dynamic_supported.load(std::memory_order_relaxed) &&
                         frames > 1;
    const int target_fps =
        dynamic ? BbSettings::Get().frame_gen_target_fps.load(std::memory_order_relaxed) : 0;
    // Reflex: the menu's choice while frame generation is off, at least On while it runs.
    int reflex = std::clamp(BbSettings::Get().reflex.load(std::memory_order_relaxed), 0,
                            BbSettings::ReflexCount - 1);
    if (on) {
        reflex = std::max(reflex, int(BbSettings::ReflexOn));
    }
    if (sl_reflex_set_options && reflex != applied_reflex) {
        sl::ReflexOptions options{};
        options.mode = reflex == BbSettings::ReflexBoost ? sl::ReflexMode::eLowLatencyWithBoost
                       : reflex == BbSettings::ReflexOn  ? sl::ReflexMode::eLowLatency
                                                         : sl::ReflexMode::eOff;
        if (sl_reflex_set_options(options) == sl::Result::eOk) {
            applied_reflex = reflex;
            std::printf("DLSS Frame Generation: Reflex %s\n", BbSettings::ReflexNames[reflex]);
        }
    }
    if (!sl_dlssg_set_options ||
        (on == mode_on && (!on || (frames == applied_frames && dynamic == applied_dynamic &&
                                   target_fps == applied_target_fps && options_format == present_format &&
                                   options_extent == present_extent &&
                                   options_applied_render == options_render)))) {
        return;
    }
    sl::DLSSGOptions options{};
    options.mode = !on ? sl::DLSSGMode::eOff : dynamic ? sl::DLSSGMode::eDynamic : sl::DLSSGMode::eOn;
    options.numFramesToGenerate = u32(frames);
    options.dynamicTargetFrameRate = float(target_fps); // 0: the display's refresh rate
    // The inputs' formats and sizes (Streamline checks them on every present; left undefined it
    // logged an error for each).
    options.colorBufferFormat = u32(present_format);
    options.hudLessBufferFormat = u32(present_format);
    options.uiBufferFormat = u32(VK_FORMAT_R16_SFLOAT);
    options.mvecBufferFormat = u32(VK_FORMAT_R16G16_SFLOAT);
    options.depthBufferFormat = u32(VK_FORMAT_R32_SFLOAT);
    options.colorWidth = present_extent.width;
    options.colorHeight = present_extent.height;
    options.mvecDepthWidth = options_render.width;
    options.mvecDepthHeight = options_render.height;
    // Off keeps its resources: menus and loading screens come and go without a stutter.
    options.flags = sl::DLSSGFlags::eRetainResourcesWhenOff;
    options.onErrorCallback = &OnApiError;
    const sl::ViewportHandle viewport{0};
    if (const sl::Result r = sl_dlssg_set_options(viewport, options); r != sl::Result::eOk) {
        std::printf("DLSS Frame Generation: options refused: %s\n", ResultName(r));
        return;
    }
    if (on != mode_on) {
        std::printf("DLSS Frame Generation: %s\n", on ? "on" : "off");
    }
    if (on && (frames != applied_frames || dynamic != applied_dynamic || target_fps != applied_target_fps)) {
        if (dynamic && target_fps) {
            std::printf("DLSS Frame Generation: dynamic, up to %d generated frames per real one, "
                        "target %d FPS\n", frames, target_fps);
        } else if (dynamic) {
            std::printf("DLSS Frame Generation: dynamic, up to %d generated frames per real one, "
                        "target the refresh rate\n", frames);
        } else {
            std::printf("DLSS Frame Generation: %d generated frame%s per real one\n", frames,
                        frames == 1 ? "" : "s");
        }
    }
    std::fflush(stdout);
    mode_on = on;
    applied_frames = frames;
    applied_dynamic = dynamic;
    applied_target_fps = target_fps;
    options_format = present_format;
    options_extent = present_extent;
    options_applied_render = options_render;
    // Dynamic: the game's limit as for 2x; the driver adds frames when the game is slower.
    BbSettings::Get().frame_gen_multiplier.store(dynamic ? 2 : frames + 1, std::memory_order_relaxed);
    // Shown frames per game frame while it runs (not decided per present: Streamline counts
    // since its last state query, 1 or 3 now and then, and the FPS counter and the flip limit
    // followed that back and forth).
    BbSettings::Get().frame_gen_active.store(on, std::memory_order_relaxed);
#else
    (void)on;
#endif
}

bool DlssFrameGenHdr() {
    static const bool on = [] {
        const char* env = std::getenv("BB_DLSSG_HDR");
        return !(env && env[0] == '0');
    }();
    return on;
}

void DlssFrameGen::BeforePresent(const DlssFrameInfo* info, bool menu_open, bool hdr) {
#ifdef _WIN32
    if (!Streamline::Loaded() || !info || info->index == 0) {
        return;
    }
    sl::FrameToken* token = Token(info->index);
    if (!token) {
        return;
    }
    if (!Available()) {
        // Reflex alone (DLSS-G not on this GPU): its options and the present markers.
        SetMode(false);
        Marker(sl::PCLMarker::ePresentStart, info->index);
        return;
    }
    auto& s = BbSettings::Get();
    const bool wanted = s.frame_gen.load(std::memory_order_relaxed) == BbSettings::FrameGenDlss;
    const bool on = wanted && info->inputs && tag_ui && !menu_open && (!hdr || DlssFrameGenHdr());
    if (info->inputs) {
        options_render = {info->render_width, info->render_height};
    }
    const sl::ViewportHandle viewport{0};
    if (on) {
        const auto& cam = info->camera;
        const Mat4 view_to_clip = RowVectorProjection(cam.proj);
        const Mat4 clip_to_view = Inverse(view_to_clip);
        // Current view to previous view (row vectors): inverse(current view) * previous view.
        const Mat4 view_to_prev_view =
            Multiply(Inverse(RowVectorView(cam.view)), RowVectorView(cam.prev_view));
        const Mat4 clip_to_prev_clip =
            Multiply(Multiply(clip_to_view, view_to_prev_view), RowVectorProjection(cam.prev_proj));
        sl::Constants constants{};
        constants.cameraViewToClip = ToSl(view_to_clip);
        constants.clipToCameraView = ToSl(clip_to_view);
        constants.clipToLensClip = ToSl(Identity());
        constants.clipToPrevClip = ToSl(clip_to_prev_clip);
        constants.prevClipToClip = ToSl(Inverse(clip_to_prev_clip));
        constants.jitterOffset = sl::float2(info->jitter[0], info->jitter[1]);
        // Motion vectors in render pixels, previous minus current: [-1, 1] after this scale.
        constants.mvecScale = sl::float2(1.0f / float(std::max(info->render_width, 1u)),
                                         1.0f / float(std::max(info->render_height, 1u)));
        constants.cameraPinholeOffset = sl::float2(0.0f, 0.0f);
        // The camera in the world: the view-to-world matrix's columns (x right, y down in view
        // space with this projection's +y-down ndc, so up is its negated y).
        const auto& inv = cam.inv_view;
        constants.cameraPos = sl::float3(inv[3], inv[7], inv[11]);
        constants.cameraRight = sl::float3(inv[0], inv[4], inv[8]);
        const float up_sign = cam.proj[1] < 0.0f ? 1.0f : -1.0f;
        constants.cameraUp = sl::float3(up_sign * inv[1], up_sign * inv[5], up_sign * inv[9]);
        constants.cameraFwd = sl::float3(inv[2], inv[6], inv[10]);
        constants.cameraNear = std::max(info->near_plane, 0.001f);
        constants.cameraFar = std::max(FarPlane, constants.cameraNear * 2.0f);
        constants.cameraFOV = info->vertical_fov;
        constants.cameraAspectRatio = float(info->render_width) / float(std::max(info->render_height, 1u));
        constants.depthInverted = sl::Boolean::eFalse;
        constants.cameraMotionIncluded = sl::Boolean::eTrue;
        constants.motionVectors3D = sl::Boolean::eFalse;
        constants.reset = info->reset || !cam.valid ? sl::Boolean::eTrue : sl::Boolean::eFalse;
        constants.orthographicProjection = sl::Boolean::eFalse;
        constants.motionVectorsDilated = sl::Boolean::eFalse;
        constants.motionVectorsJittered = sl::Boolean::eFalse;
        sl_set_constants(constants, *token, viewport);

        const Slot& slot = slots[info->slot % slots.size()];
        sl::Resource depth = Image(vk::Image(slot.depth), *slot.depth_view, vk::Format::eR32Sfloat,
                                   slot.width, slot.height,
                                   vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eStorage);
        sl::Resource motion = Image(vk::Image(slot.motion), *slot.motion_view,
                                    vk::Format::eR16G16Sfloat, slot.width, slot.height,
                                    vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eStorage);
        sl::Resource hudless = Image(tag_hudless, tag_hudless_view, present_format,
                                     present_extent.width, present_extent.height,
                                     vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferSrc);
        sl::Resource ui = Image(vk::Image(ui_image), *ui_view, vk::Format::eR16Sfloat,
                                present_extent.width, present_extent.height,
                                vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eStorage);
        const sl::Extent render{0, 0, slot.width, slot.height};
        const sl::Extent display{0, 0, present_extent.width, present_extent.height};
        const std::array tags{
            sl::ResourceTag{&depth, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilPresent, &render},
            sl::ResourceTag{&motion, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilPresent, &render},
            sl::ResourceTag{&hudless, sl::kBufferTypeHUDLessColor, sl::ResourceLifecycle::eValidUntilPresent, &display},
            sl::ResourceTag{&ui, sl::kBufferTypeUIAlpha, sl::ResourceLifecycle::eValidUntilPresent, &display},
        };
        sl_set_tag_for_frame(*token, viewport, tags.data(), u32(tags.size()), nullptr);
    } else if (mode_on) {
        // Nothing valid to interpolate (a menu, a movie, loading): the tags go too.
        const std::array tags{
            sl::ResourceTag{nullptr, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilPresent},
            sl::ResourceTag{nullptr, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilPresent},
            sl::ResourceTag{nullptr, sl::kBufferTypeHUDLessColor, sl::ResourceLifecycle::eValidUntilPresent},
            sl::ResourceTag{nullptr, sl::kBufferTypeUIAlpha, sl::ResourceLifecycle::eValidUntilPresent},
        };
        sl_set_tag_for_frame(*token, viewport, tags.data(), u32(tags.size()), nullptr);
    }
    SetMode(on);
    Marker(sl::PCLMarker::ePresentStart, info->index);
#else
    (void)info;
    (void)menu_open;
    (void)hdr;
#endif
}

void DlssFrameGen::AfterPresent(const DlssFrameInfo* info) {
#ifdef _WIN32
    if (!Streamline::Loaded() || !info || info->index == 0) {
        return;
    }
    Marker(sl::PCLMarker::ePresentEnd, info->index);
    if (!Available()) {
        return;
    }
    auto& s = BbSettings::Get();
    const auto now = std::chrono::steady_clock::now();
    if (last_present != std::chrono::steady_clock::time_point{}) {
        const double dt = std::chrono::duration<double, std::milli>(now - last_present).count();
        if (dt > 1.0 && dt < 100.0) {
            interval_ms = interval_ms > 0.0 ? interval_ms * 0.9 + dt * 0.1 : dt;
        }
    }
    last_present = now;
    if (!mode_on) {
        return;
    }
    sl::DLSSGState state{};
    const sl::ViewportHandle viewport{0};
    if (sl_dlssg_get_state(viewport, state, nullptr) != sl::Result::eOk) {
        return;
    }
    if (state.status != sl::DLSSGStatus::eOk) {
        if (status_logged != u32(state.status)) {
            status_logged = u32(state.status);
            std::printf("DLSS Frame Generation: status %#x (%s)\n", unsigned(state.status),
                        (state.status & sl::DLSSGStatus::eFailResolutionTooLow) ? "the output is too small"
                        : (state.status & sl::DLSSGStatus::eFailReflexNotDetectedAtRuntime) ? "Reflex not detected"
                        : (state.status & sl::DLSSGStatus::eFailHDRFormatNotSupported) ? "HDR format not supported"
                        : (state.status & sl::DLSSGStatus::eFailCommonConstantsInvalid) ? "constants invalid"
                                                                                          : "see the Streamline log");
            std::fflush(stdout);
        }
    }
    if (const VkResult error = last_api_error.exchange(VK_SUCCESS); error != VK_SUCCESS) {
        std::printf("DLSS Frame Generation: present returned %d\n", int(error));
    }
    // bbport: dynamic multi frame generation as the running feature reports it. At device
    // creation, before DLSS-G has a swapchain, an RTX 5080 (beta 7) got up to 5 generated frames
    // but no dynamic mode, and the menu's switch stayed disabled.
    if (state.bIsDynamicMFGSupported == sl::Boolean::eTrue && s.dlss_fg_max_frames.load() > 1 &&
        !s.dlss_fg_dynamic_supported.exchange(true)) {
        std::printf("DLSS Frame Generation: dynamic multi frame generation available\n");
        std::fflush(stdout);
    }
    // Frames presented per game frame, smoothed (Streamline counts since the last query).
    presented_avg = presented_avg * 0.95 + double(state.numFramesActuallyPresented) * 0.05;
    if (interval_ms > 0.0) {
        const float game_fps = float(1000.0 / interval_ms);
        s.frame_gen_game_fps.store(game_fps, std::memory_order_relaxed);
        s.frame_gen_shown_fps.store(game_fps * float(std::max(presented_avg, 1.0)),
                                    std::memory_order_relaxed);
    }
    // BB_FRAME_STATS: what DLSS-G did over the last 5 seconds.
    static const bool stats = [] {
        const char* env = std::getenv("BB_FRAME_STATS");
        return env && env[0] == '1';
    }();
    if (stats) {
        ++stats_frames;
        stats_presented += state.numFramesActuallyPresented;
        if (stats_start == std::chrono::steady_clock::time_point{}) {
            stats_start = now;
        }
        const double window = std::chrono::duration<double>(now - stats_start).count();
        if (window >= 5.0) {
            std::printf("DLSS Frame Generation: %.1f s: game %.1f FPS, shown %.1f FPS (%.2f per game frame), "
                        "status %#x, max %u generated%s\n",
                        window, stats_frames / window, stats_presented / window,
                        stats_frames ? double(stats_presented) / stats_frames : 0.0,
                        unsigned(state.status), state.numFramesToGenerateMax,
                        state.bIsVsyncSupportAvailable == sl::Boolean::eTrue ? ", VSync available" : "");
            std::fflush(stdout);
            stats_start = now;
            stats_frames = stats_presented = 0;
        }
    }
#else
    (void)info;
#endif
}

void DlssFrameGen::BeforeSwapchainChange() {
    if (mode_on) {
        SetMode(false);
    }
}

} // namespace Vulkan
