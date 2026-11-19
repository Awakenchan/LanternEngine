// SPDX-License-Identifier: GPL-3.0-or-later
#include "graphics.h"
#include "fog_scale.h"
#include "hook_entries.h"
#include "engine_hooks.h"
#include "bbport_platform.h"
#include "bbport_settings.h"
#include "bbport_toggles.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

namespace BbEngine::Graphics {
namespace {
#define GUEST_ABI __attribute__((sysv_abi))

constexpr std::size_t kRenderView = 0x229e990;
constexpr const auto& kRenderViewEntry = HookEntries::kGraphicsRenderViewEntry;
constexpr std::size_t kSsaoSwitch = 0x2968, kBlurSwitch = 0x2969, kAaSwitch = 0x296a;

constexpr std::size_t kSsaoDraw = 0xe669d0;
constexpr const auto& kSsaoDrawEntry = HookEntries::kGraphicsSsaoDrawEntry;
constexpr std::size_t kSsaoScale = 0x14c + 0x44, kSsaoOffset = 0x14c + 0x48;

constexpr std::size_t kShadowSetup = 0xeac210;
constexpr const auto& kShadowSetupEntry = HookEntries::kGraphicsShadowSetupEntry;
constexpr std::size_t kShadowDistances[] = {0x38, 0x3c, 0x44, 0x48};
constexpr std::size_t kCascadeRange = 0xeac540, kCascadeRangeTail = 0xeac657;
constexpr std::uint8_t kCascadeRangeEntry[] = {
    0xc5, 0xf8, 0x28, 0x02, 0xc5, 0xf8, 0x29, 0x06, 0xc5, 0xf8, 0x28, 0x42, 0x10,
    0xc5, 0xf8, 0x29, 0x46, 0x10};
constexpr const auto& kCascadeRangeStores = HookEntries::kGraphicsCascadeRangeStores;
constexpr float kCascadeBlend = 0.1f;
constexpr std::size_t kDirectionalSizeTable = 0x48a9668, kDirectionalSizeDefault = 0xeac238;
constexpr std::uint32_t kDirectionalSizes[] = {2048, 1024, 2048, 4096};
constexpr std::uint8_t kDirectionalSizeCode[] = {
    0xb8, 0x00, 0x08, 0x00, 0x00, 0x48, 0x83, 0xf9, 0x03, 0x77, 0x0a,
    0x48, 0x8d, 0x05, 0x1e, 0xd4, 0x9f, 0x03, 0x8b, 0x04, 0x88};
std::atomic<bool> g_cascade_overlap{false};

constexpr std::size_t kLocalShadowPool = 0x22440b0, kLocalShadowSize = 0x224443f;
constexpr std::uint8_t kLocalShadowPoolEntry[] = {
    0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53,
    0x48, 0x81, 0xe4, 0xe0, 0xff, 0xff, 0xff, 0x48, 0x81, 0xec, 0x20, 0x01, 0x00, 0x00};
constexpr std::uint8_t kLocalShadowSizeRead[] = {
    0x41, 0x8b, 0x44, 0x24, 0x14, 0x4d, 0x89, 0xe6, 0x4c, 0x89, 0x74, 0x24, 0x58,
    0x89, 0x44, 0x24, 0x44};
constexpr std::size_t kLocalShadowTier = 0x2244cb0;
constexpr std::uint8_t kLocalShadowTierCode[] = {
    0x41, 0xd1, 0xed, 0x48, 0xff, 0xc1, 0x48, 0x83, 0xf9, 0x04};
constexpr std::size_t kSpotShadowSize = 0x22440ed, kSpotShadowTier = 0x224442a;
constexpr std::uint8_t kSpotShadowSizeRead[] = {
    0x41, 0x8b, 0x04, 0x24, 0x89, 0x44, 0x24, 0x68, 0x48, 0x8d, 0x4b, 0x10,
    0x48, 0x89, 0x4c, 0x24, 0x50, 0x49, 0x89, 0xdd, 0x45, 0x31, 0xff, 0x89, 0x44, 0x24, 0x60};
constexpr std::uint8_t kSpotShadowTierCode[] = {
    0xd1, 0x6c, 0x24, 0x68, 0xd1, 0x6c, 0x24, 0x60, 0x49, 0xff, 0xc7, 0x49, 0x83, 0xff, 0x04};
constexpr std::size_t kPointShadowRadius = 0x4929c80, kSpotShadowRadius = 0x4929c84;
constexpr std::size_t kPointShadowRadiusLoad = 0x22b7011;
constexpr std::uint8_t kPointShadowRadiusCode[] = {0xc5, 0xfa, 0x10, 0x05, 0x67, 0x2c, 0x67, 0x02};
constexpr std::size_t kSpotShadowRadiusLoad = 0x22b7220, kSpotShadowRadiusLoadFallback = 0x22b7298;
constexpr std::uint8_t kSpotShadowRadiusCode[] = {0xc5, 0xfa, 0x10, 0x05, 0x5c, 0x2a, 0x67, 0x02};
constexpr std::uint8_t kSpotShadowRadiusFallbackCode[] = {0xc5, 0xfa, 0x10, 0x05, 0xe4, 0x29, 0x67, 0x02};
constexpr float kOriginalLocalShadowRadius = 3.2f;

int LocalShadowQuality() {
    return std::clamp(BbSettings::Get().graphics_local_shadow_resolution.load(std::memory_order_relaxed), 0, 2);
}

constexpr std::size_t kYebisRecord = 0x21d4860;
constexpr const auto& kYebisRecordEntry = HookEntries::kGraphicsYebisRecordEntry;
constexpr std::size_t kYebisBlockLoad = 0x21d4888;
constexpr std::uint8_t kLeaRax[] = {0x48, 0x8d, 0x05};
constexpr std::size_t kHue = 0xc4, kSaturation = 0xc8, kValue = 0xcc, kVignette = 0xd0, kGlare = 0xdc,
                      kContrast = 0x248;

constexpr std::size_t kFogRebuild = 0x22da690;
constexpr const auto& kFogRebuildEntry = HookEntries::kGraphicsFogRebuildEntry;
constexpr std::size_t kFogJump = 0x22daadf, kFogPadding = 0x22daaf3, kTableUpload = 0xe2e6b0;
constexpr std::uint8_t kPadding[] = {0x66, 0x66, 0x66, 0x66, 0x2e, 0x0f, 0x1f, 0x84, 0, 0, 0, 0, 0};
constexpr std::size_t kFogMap = 0x248, kFogTable = 0x250, kFogCount = 0x258;
constexpr std::size_t kUploadBuffer = 0x70, kUploadSize = 0x2c, kUploadKind = 0x38, kUploadFlags = 0x3b;
constexpr std::uint8_t kUploadCopy = 1, kUploadOther = 4;

bool g_enabled = false;
std::uint8_t* g_post_block = nullptr;
std::atomic<std::uint64_t> g_yebis_records{0};
thread_local std::uint64_t t_fog_manager = 0;
thread_local std::vector<std::uint8_t> t_fog_scratch;
std::atomic<bool> g_fog_kind_said{false}, g_fog_size_said{false};

template <typename T> bool Peek(std::uint64_t at, T* out) {
    return at && BbPlatform::ReadProcessMemory(reinterpret_cast<const void*>(at), out, sizeof(T));
}
template <typename T> void Poke(std::uint64_t at, T value) {
    std::memcpy(reinterpret_cast<void*>(at), &value, sizeof value);
}
bool On() { return BbSettings::Get().graphics_controls.load(std::memory_order_relaxed); }
float Setting(const std::atomic<float>& v, float lo, float hi) {
    const float f = v.load(std::memory_order_relaxed);
    return std::isfinite(f) ? std::clamp(f, lo, hi) : 1.0f;
}

struct Written {
    std::uint64_t at = 0;
    float source = 0, value = 0;
};
std::array<Written, 256> g_written;
std::mutex g_written_mutex;

template <typename F> void Tune(std::uint64_t at, bool active, F&& f) {
    float now = 0;
    if (!at || !Peek(at, &now) || !std::isfinite(now)) return;
    std::scoped_lock lock(g_written_mutex);
    Written* own = nullptr;
    Written* empty = nullptr;
    for (auto& w : g_written) {
        if (w.at == at) { own = &w; break; }
        if (!empty && !w.at) empty = &w;
    }
    const bool ours = own && now == own->value;
    if (!active) {
        if (ours) Poke(at, own->source);
        if (own) *own = {};
        return;
    }
    const float source = ours ? own->source : now;
    const float value = f(source);
    Poke(at, value);
    if (Written* slot = own ? own : empty) *slot = {at, source, value};
}

struct Renderer {
    std::uint64_t at = 0;
    std::uint8_t ssao = 0, blur = 0, aa = 0;
};
std::array<Renderer, 8> g_renderers;
std::mutex g_renderer_mutex;
int g_blur_said = -1;

GUEST_ABI std::int64_t BeforeRenderView(std::uint64_t, std::uint64_t* saved) noexcept {
    const std::uint64_t r = saved[0];
    if (!r) return 0;
    std::scoped_lock lock(g_renderer_mutex);
    Renderer* own = nullptr;
    for (auto& x : g_renderers) if (x.at == r) { own = &x; break; }
    if (!own) {
        for (auto& x : g_renderers) {
            if (x.at) continue;
            if (!Peek(r + kSsaoSwitch, &x.ssao) || !Peek(r + kBlurSwitch, &x.blur) || !Peek(r + kAaSwitch, &x.aa)) return 0;
            x.at = r;
            own = &x;
            break;
        }
        if (!own) return 0;
    }
    const auto& s = BbSettings::Get();
    Poke<std::uint8_t>(r + kSsaoSwitch, own->ssao && s.effects[3].load(std::memory_order_relaxed));
    Poke<std::uint8_t>(r + kAaSwitch, own->aa && s.effects[4].load(std::memory_order_relaxed));
    const bool held = s.motion_blur_held.load(std::memory_order_relaxed);
    Poke<std::uint8_t>(r + kBlurSwitch, held ? 0 : own->blur);
    if (own->blur && g_blur_said != int(held)) {
        g_blur_said = int(held);
        std::printf(held ? "Engine: motion blur held off while the upscaler takes the finished frame (output "
                           "other than 1080p), the setting unchanged (BB_UPSCALER_NO_BLUR)\n"
                         : "Engine: motion blur on (the game's own)\n");
    }
    return 0;
}

GUEST_ABI std::int64_t BeforeSsaoDraw(std::uint64_t, std::uint64_t* saved) noexcept {
    const std::uint64_t object = saved[5];
    if (!object) return 0;
    const float k = Setting(BbSettings::Get().graphics_ao_strength, 0.0f, 5.0f);
    const bool active = On() && k != 1;
    Tune(object + kSsaoScale, active, [k](float v) { return v * k; });
    Tune(object + kSsaoOffset, active, [k](float v) { return v * k + (1 - k); });
    return 0;
}

GUEST_ABI std::int64_t BeforeShadowSetup(std::uint64_t, std::uint64_t* saved) noexcept {
    const std::uint64_t block = saved[4];
    if (!block) return 0;
    const float k = Setting(BbSettings::Get().graphics_shadow_scale, 0.25f, 8.0f);
    const bool active = On() && k != 1;
    for (const std::size_t at : kShadowDistances) Tune(block + at, active, [k](float v) { return v * k; });
    return 0;
}

GUEST_ABI std::int64_t BeforeCascadeRangeStores(std::uint64_t, std::uint64_t* saved) noexcept {
    const unsigned cascade = unsigned(saved[1]);
    if (cascade == 0 || cascade > 3 || !saved[5] || !saved[3]) return 0;
    float near = 0, previous = 0, camera_near = 0;
    std::memcpy(&near, SavedXmm(saved, 0), sizeof near);
    if (cascade > 1 && !Peek(saved[5] + 0x6dc + 4 * (cascade - 2), &previous)) return 0;
    if (!Peek(saved[3] + 0x54, &camera_near) ||
        !std::isfinite(near) || !std::isfinite(previous) || !std::isfinite(camera_near) ||
        near <= previous || camera_near > near) return 0;
    near = std::max(camera_near, near - kCascadeBlend * (near - previous));
    std::memcpy(SavedXmm(saved, 0), &near, sizeof near);
    return 0;
}

GUEST_ABI std::int64_t BeforeYebisRecord(std::uint64_t, std::uint64_t*) noexcept {
    if (BbStats::enabled) g_yebis_records.fetch_add(1, std::memory_order_relaxed);
    if (!g_post_block) return 0;
    const bool on = On();
    const auto& s = BbSettings::Get();
    const auto block = reinterpret_cast<std::uint64_t>(g_post_block);
    const float bloom = Setting(s.graphics_bloom, 0.0f, 10.0f), saturation = Setting(s.graphics_saturation, 0.0f, 5.0f);
    const float bright = Setting(s.graphics_brightness, 0.0f, 5.0f), contrast = Setting(s.graphics_contrast, 0.5f, 1.5f);
    const float hue = std::clamp(std::isfinite(s.graphics_hue.load()) ? s.graphics_hue.load() : 0.0f, -180.0f, 180.0f);
    const bool vignette = s.graphics_vignette.load(std::memory_order_relaxed);
    Tune(block + kGlare, on && bloom != 1, [bloom](float v) { return v * bloom; });
    Tune(block + kSaturation, on && saturation != 1, [saturation](float v) { return v * saturation; });
    Tune(block + kValue, on && bright != 1, [bright](float v) { return v * bright; });
    Tune(block + kContrast, on && contrast != 1, [contrast](float v) { return v * contrast; });
    Tune(block + kHue, on && hue != 0, [hue](float v) { return v + hue; });
    Tune(block + kVignette, on && !vignette, [](float) { return 0.0f; });
    return 0;
}

GUEST_ABI std::int64_t BeforeFogRebuild(std::uint64_t, std::uint64_t* saved) noexcept {
    t_fog_manager = saved[5];
    return 0;
}

GUEST_ABI std::int64_t BeforeFogUpload(std::uint64_t, std::uint64_t* saved) noexcept {
    const std::uint64_t manager = t_fog_manager;
    t_fog_manager = 0;
    const float k = Setting(BbSettings::Get().graphics_fog, 0.0f, 5.0f);
    if (FogScale::Untouched(On(), k)) return 0;
    std::uint64_t map = 0, table = 0, buffer = 0, view = 0;
    std::uint32_t count = 0, upload = 0;
    std::uint8_t kind = 0, flags = 0;
    if (!Peek(manager + kFogMap, &map) || !Peek(manager + kFogTable, &table) || !Peek(manager + kFogCount, &count) ||
        map != saved[5] || table != saved[3] || !table || !count || count > FogScale::kMaxBlocks ||
        !Peek(map + kUploadBuffer, &buffer) || !Peek(buffer, &view) || !view) return 0;
    if (!Peek(view + kUploadSize, &upload) || !Peek(view + kUploadKind, &kind) || !Peek(view + kUploadFlags, &flags) ||
        (!(flags & kUploadOther) && kind != kUploadCopy)) {
        if (!g_fog_kind_said.exchange(true)) std::printf("Engine: fog left at the game's own for an upload of kind %u\n", unsigned(kind));
        return 0;
    }
    const std::size_t bytes = FogScale::CopyBytes(count, upload);
    if (!bytes) {
        if (!g_fog_size_said.exchange(true))
            std::printf("Engine: fog left at the game's own: %u blocks, %u bytes is over the copy's %zu\n", count, upload,
                        FogScale::kMaxBytes);
        return 0;
    }
    if (t_fog_scratch.size() < bytes) t_fog_scratch.resize(bytes);
    saved[3] = reinterpret_cast<std::uint64_t>(
        FogScale::ScaleCopy(reinterpret_cast<const std::uint8_t*>(table), count, bytes, k, t_fog_scratch.data()));
    return 0;
}

bool Matches(const std::uint8_t* image, std::size_t size, std::size_t at, const std::uint8_t* bytes, std::size_t n) {
    return at <= size && n <= size - at && std::memcmp(image + at, bytes, n) == 0;
}

bool RedirectFogUpload(std::uint8_t* image, std::size_t size, std::uint8_t** stub) {
    *stub = nullptr;
    if (!Matches(image, size, kFogPadding, kPadding, sizeof kPadding) || image[kFogJump] != 0xe9) return false;
    std::int32_t rel = 0;
    std::memcpy(&rel, image + kFogJump + 1, 4);
    if (std::int64_t(kFogJump + 5) + rel != std::int64_t(kTableUpload)) return false;
    *stub = TailStub(BeforeFogUpload, reinterpret_cast<std::uint64_t>(image + kTableUpload));
    return *stub != nullptr;
}
struct FogUpload {
    std::array<std::uint8_t, sizeof kPadding> jump{};
    std::array<std::uint8_t, 4> rel{}, old_rel{};
    std::array<CodePatch, 2> patches{};
};
void FogUploadPatches(std::uint8_t* image, std::uint8_t* stub, FogUpload& fog) {
    const auto to = reinterpret_cast<std::uint64_t>(stub);
    fog.jump = {0x49, 0xbb, 0, 0, 0, 0, 0, 0, 0, 0, 0x41, 0xff, 0xe3};
    std::memcpy(fog.jump.data() + 2, &to, 8);
    const auto rel = std::int32_t(std::int64_t(kFogPadding) - std::int64_t(kFogJump + 5));
    std::memcpy(fog.rel.data(), &rel, 4);
    std::memcpy(fog.old_rel.data(), image + kFogJump + 1, 4);
    fog.patches = {CodePatch{image, kFogPadding, kPadding, fog.jump}, CodePatch{image, kFogJump + 1, fog.old_rel, fog.rel}};
}
}

bool Enabled() { return g_enabled; }

std::uint64_t YebisRecords() { return g_yebis_records.load(std::memory_order_relaxed); }

float DirectionalShadowBlend() {
    return g_cascade_overlap.load(std::memory_order_relaxed) ? kCascadeBlend : 0.0f;
}

bool InstallDirectionalShadowResolution(std::uint8_t* image, std::size_t size) {
    if (!image ||
        !Matches(image, size, kDirectionalSizeTable,
                 reinterpret_cast<const std::uint8_t*>(kDirectionalSizes), sizeof kDirectionalSizes) ||
        !Matches(image, size, kDirectionalSizeDefault, kDirectionalSizeCode, sizeof kDirectionalSizeCode)) {
        std::printf("Engine: directional shadow resolution unavailable: allocation code differs from 1.09\n");
        return false;
    }
    const int quality = std::clamp(
        BbSettings::Get().graphics_shadow_resolution.load(std::memory_order_relaxed), 0, 2);
    if (quality == 0) return false;
    const std::uint32_t atlas = 2048u << quality;
    const std::array<std::uint32_t, 4> sizes{atlas, atlas, atlas, atlas};
    const CodePatch patches[] = {
        {image, kDirectionalSizeTable, AsCode(std::span(kDirectionalSizes)), AsCode(std::span(sizes))},
        {image, kDirectionalSizeDefault + 1, std::span(kDirectionalSizeCode).subspan(1, sizeof atlas), AsCode(std::span(&atlas, 1))}};
    if (const WriteResult result = WriteCode(patches); result != WriteResult::Applied) {
        std::printf("Engine: directional shadow resolution off: native allocation settings %s\n", Describe(result));
        return false;
    }
    std::printf("Engine: directional shadow atlas %ux%u, four %ux%u cascades (restart to change)\n",
                atlas, atlas, atlas / 2, atlas / 2);
    return true;
}

bool InstallDirectionalShadows(std::uint8_t* image, std::size_t size) {
    const auto& settings = BbSettings::Get();
    if (!settings.startup_effects[BbSettings::EffectPcss] &&
        !settings.startup_effects[BbSettings::EffectContactShadows]) {
        return false;
    }
    Hook hook;
    if (!image || !Matches(image, size, kCascadeRange, kCascadeRangeEntry, sizeof kCascadeRangeEntry) ||
        !Prepare(hook, image, size, kCascadeRangeTail, SiteKind::Inside, kCascadeRangeStores, BeforeCascadeRangeStores)) {
        std::printf("Engine: directional overlap off: cascade code does not match 1.09 or hook unavailable\n");
        return false;
    }
    if (const WriteResult result = Commit(std::span<Hook>{&hook, 1}); result != WriteResult::Applied) {
        std::printf("Engine: directional overlap off: cascade code %s\n", Describe(result));
        return false;
    }
    g_cascade_overlap.store(true, std::memory_order_relaxed);
    std::printf("Engine: directional cascades overlap by 10%%; authored splits and atlas size retained\n");
    return true;
}

bool InstallLocalShadowSoftness(std::uint8_t* image, std::size_t size, bool point, bool spot) {
    if ((!point && !spot) || LocalShadowQuality() == 0) return true;
    const auto radius_matches = [&](std::size_t at) {
        return Matches(image, size, at, reinterpret_cast<const std::uint8_t*>(&kOriginalLocalShadowRadius),
                       sizeof kOriginalLocalShadowRadius);
    };
    if (!image ||
        (point && (!radius_matches(kPointShadowRadius) ||
                   !Matches(image, size, kPointShadowRadiusLoad, kPointShadowRadiusCode,
                            sizeof kPointShadowRadiusCode))) ||
        (spot && (!radius_matches(kSpotShadowRadius) ||
                  !Matches(image, size, kSpotShadowRadiusLoad, kSpotShadowRadiusCode,
                           sizeof kSpotShadowRadiusCode) ||
                  !Matches(image, size, kSpotShadowRadiusLoadFallback, kSpotShadowRadiusFallbackCode,
                           sizeof kSpotShadowRadiusFallbackCode)))) {
        std::printf("Engine: local-shadow filter tuning unavailable: the filter code is not 1.09's\n");
        return false;
    }
    const float radius = LocalShadowQuality() == 1 ? 3.2f : 6.4f;
    const auto original = AsCode(std::span(&kOriginalLocalShadowRadius, 1));
    const auto wanted = AsCode(std::span(&radius, 1));
    std::array<CodePatch, 2> patches;
    std::size_t count = 0;
    if (point) patches[count++] = {image, kPointShadowRadius, original, wanted};
    if (spot) patches[count++] = {image, kSpotShadowRadius, original, wanted};
    if (const WriteResult result = WriteCode(std::span<const CodePatch>(patches).first(count)); result != WriteResult::Applied) {
        std::printf("Engine: local-shadow filter tuning off: filter radius %s\n", Describe(result));
        return false;
    }
    std::printf("Engine: local-shadow filter radius %.1f texels (point %s, spotlight %s); high-resolution maps kept\n",
                radius, point ? "on" : "unchanged", spot ? "on" : "unchanged");
    return true;
}

bool InstallLocalShadows(std::uint8_t* image, std::size_t size) {
    const int quality = LocalShadowQuality();
    if (quality == 0) return false;
    if (!image ||
        !Matches(image, size, kLocalShadowPool, kLocalShadowPoolEntry, sizeof kLocalShadowPoolEntry) ||
        !Matches(image, size, kLocalShadowSize, kLocalShadowSizeRead, sizeof kLocalShadowSizeRead) ||
        !Matches(image, size, kLocalShadowTier, kLocalShadowTierCode, sizeof kLocalShadowTierCode)) {
        std::printf("Engine: higher-resolution point shadows off: the pool code is not 1.09's\n");
        return false;
    }
    const std::uint32_t base = quality == 1 ? 1024u : 2048u;
    std::array<std::uint8_t, 5> resolution{0xb8};
    std::memcpy(resolution.data() + 1, &base, sizeof base);
    const CodePatch pool{image, kLocalShadowSize, std::span(kLocalShadowSizeRead).first(resolution.size()), resolution};
    if (const WriteResult result = WriteCode(pool); result != WriteResult::Applied) {
        std::printf("Engine: higher-resolution point shadows off: the pool size %s\n", Describe(result));
        return false;
    }
    std::printf("Engine: native point-shadow faces %u/%u/%u/%u; other pools untouched by point patch\n",
                base, base / 2, base / 4, base / 8);
    return true;
}

bool InstallSpotShadows(std::uint8_t* image, std::size_t size) {
    const int quality = LocalShadowQuality();
    if (quality == 0) return false;
    if (!image ||
        !Matches(image, size, kLocalShadowPool, kLocalShadowPoolEntry, sizeof kLocalShadowPoolEntry) ||
        !Matches(image, size, kSpotShadowSize, kSpotShadowSizeRead, sizeof kSpotShadowSizeRead) ||
        !Matches(image, size, kSpotShadowTier, kSpotShadowTierCode, sizeof kSpotShadowTierCode)) {
        std::printf("Engine: higher-resolution spotlight shadows off: the pool code is not 1.09's\n");
        return false;
    }
    const std::uint8_t resolution[] = {0x31, 0xc0, 0xb4, std::uint8_t(quality == 1 ? 8 : 16)};
    const CodePatch pool{image, kSpotShadowSize, std::span(kSpotShadowSizeRead).first(sizeof resolution), resolution};
    if (const WriteResult result = WriteCode(pool); result != WriteResult::Applied) {
        std::printf("Engine: higher-resolution spotlight shadows off: the pool size %s\n", Describe(result));
        return false;
    }
    const unsigned base = quality == 1 ? 2048u : 4096u;
    std::printf("Engine: native spotlight shadow maps %u/%u/%u/%u; directional pool unchanged\n",
                base, base / 2, base / 4, base / 8);
    return true;
}

bool Install(std::uint8_t* image, std::size_t size) {
    const std::size_t lea_end = kYebisBlockLoad + sizeof kLeaRax + 4;
    if (!Matches(image, size, kYebisBlockLoad, kLeaRax, sizeof kLeaRax)) {
        std::printf("Engine: picture tuning off: the game's code is not 1.09's\n");
        return false;
    }
    std::int32_t d = 0;
    std::memcpy(&d, image + kYebisBlockLoad + sizeof kLeaRax, 4);
    const std::size_t block = std::size_t(std::int64_t(lea_end) + d);
    if (block > size - (kContrast + 4)) return false;

    std::array<Hook, 5> hooks;
    const bool ready = Prepare(hooks[0], image, size, kRenderView, SiteKind::Entry, kRenderViewEntry, BeforeRenderView) &&
                       Prepare(hooks[1], image, size, kYebisRecord, SiteKind::Entry, kYebisRecordEntry, BeforeYebisRecord) &&
                       Prepare(hooks[2], image, size, kSsaoDraw, SiteKind::Entry, kSsaoDrawEntry, BeforeSsaoDraw) &&
                       Prepare(hooks[3], image, size, kShadowSetup, SiteKind::Entry, kShadowSetupEntry, BeforeShadowSetup);
    if (!ready) {
        for (auto& h : hooks) Discard(h);
        std::printf("Engine: picture tuning off: the game's code is not 1.09's\n");
        return false;
    }
    std::uint8_t* fog_stub = nullptr;
    const bool fog = Prepare(hooks[4], image, size, kFogRebuild, SiteKind::Entry, kFogRebuildEntry, BeforeFogRebuild) &&
                     RedirectFogUpload(image, size, &fog_stub);
    if (!fog) {
        Discard(hooks[4]);
        FreeStub(fog_stub);
    }
    g_post_block = image + block;
    FogUpload upload;
    if (fog) FogUploadPatches(image, fog_stub, upload);
    const WriteResult result = Commit(std::span(hooks).first(fog ? hooks.size() : hooks.size() - 1),
                                      fog ? std::span<const CodePatch>(upload.patches) : std::span<const CodePatch>{});
    if (result != WriteResult::Applied) {
        if (result == WriteResult::NotApplied) {
            FreeStub(fog_stub);
            g_post_block = nullptr;
        }
        std::printf("Engine: picture tuning off: the game's code %s\n", Describe(result));
        return false;
    }
    g_enabled = true;
    std::printf("Engine: picture tuning ready (%s, live): AA, SSAO, AO strength, shadow distance, bloom, saturation, "
                "vignette, brightness, contrast, hue%s\n",
                On() ? "on" : "off", fog ? " and fog" : "; fog left alone, its code is not 1.09's");
    return true;
}
}
