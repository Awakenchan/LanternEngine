// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once
#include <string>

#include <atomic>

namespace BbSettings {

enum Upscaler : int { UpscalerOff = 0, UpscalerFsr3 = 1, UpscalerFsr4 = 2, UpscalerFsr411 = 3,
                      UpscalerTaa = 4, UpscalerDlss = 5, UpscalerFsr = 6, UpscalerXess = 7,
                      UpscalerCount };
inline bool IsFsr4(int upscaler) {
    return upscaler == UpscalerFsr4 || upscaler == UpscalerFsr411 || upscaler == UpscalerFsr;
}
inline bool IsFrameUpscaler(int upscaler) {
    return IsFsr4(upscaler) || upscaler == UpscalerDlss || upscaler == UpscalerXess;
}
enum FrameGen : int { FrameGenOff = 0, FrameGenFsr3 = 1, FrameGenDlss = 2, FrameGenCount };
inline constexpr const char* FrameGenNames[] = {"off", "fsr3", "dlss"};
static_assert(sizeof(FrameGenNames) / sizeof(FrameGenNames[0]) == FrameGenCount);
inline constexpr int FrameGenFramesMax = 5;
enum Reflex : int { ReflexOff = 0, ReflexOn = 1, ReflexBoost = 2, ReflexCount };
inline constexpr const char* ReflexNames[] = {"off", "on", "boost"};
static_assert(sizeof(ReflexNames) / sizeof(ReflexNames[0]) == ReflexCount);
enum Preset : int { NativeAA = 0, Quality, Balanced, Performance, UltraPerformance, PresetCustom, PresetCount };
inline constexpr float RenderScaleMin = 1.0f, RenderScaleMax = 3.0f;
inline constexpr float MipBiasMin = -2.0f, MipBiasMax = 1.0f;
enum FpsPosition : int { FpsTopRight = 0, FpsTopLeft, FpsBottomRight, FpsBottomLeft, FpsTopCentre, FpsPositionCount };
enum DebugView : int { DebugNone = 0, DebugReactive = 1, DebugMotion = 2, DebugViewCount };
enum class MenuLanguage {
    English, Russian, Spanish, PortugueseBrazil, French, German, Italian, Polish, Turkish, ChineseSimplified
};
inline constexpr const char* MenuLanguageCodes[] = {"en", "ru", "es", "pt_BR", "fr", "de", "it", "pl", "tr", "zh_CN"};
inline constexpr int MenuLanguageCount = int(sizeof(MenuLanguageCodes) / sizeof(MenuLanguageCodes[0]));
static_assert(MenuLanguageCount == int(MenuLanguage::ChineseSimplified) + 1);

struct Effect {
    const char* key;
    const char* label;
    const char* label_ru;
    bool default_on;
};
inline constexpr Effect Effects[] = {
    {"effect_chromatic_aberration", "Chromatic aberration", "Хроматическая аберрация", false},
    {"effect_dof", "Depth of field (DoF)", "Глубина резкости (DoF)", false},
    {"effect_motion_blur", "Motion blur", "Размытие в движении", false},
    {"effect_ssao", "Ambient occlusion (SSAO)", "Затенение SSAO", true},
    {"effect_game_aa", "Game's own anti-aliasing", "Собственное сглаживание игры", false},
    {"effect_dynamic_shadows", "Shadows from dynamic lights", "Тени от динамических источников", true},
    {"effect_ssr", "Screen-space reflections (not in original game)", "Отражения SSR (не было в игре)", false},
    {"skip_intro", "Skip startup intros", "Пропуск заставок при запуске", false},
    {"debug_camera", "Free camera (Cross + L3)", "Свободная камера (Cross + L3)", false},
    {"debug_menu", "Debug menu (requires font files)", "Debug menu (нужны файлы шрифтов)", false},
    {"effect_gtao", "Ground-truth ambient occlusion (GTAO, experimental)", "Затенение GTAO (эксперимент)", false},
    {"effect_modern_ssr", "Improved reflections", "Улучшенные отражения", false},
    {"effect_pom_cavity", "Deeper parallax surfaces", "Глубокие параллакс-поверхности", false},
    {"effect_contact_shadows", "Contact shadows", "Контактные тени", false},
    {"effect_pcss", "Soft sun shadows (PCSS)", "Мягкие тени солнца (PCSS)", false},
    {"effect_gi", "Indirect lighting (GI)", "Непрямое освещение (GI)", false},
    {"effect_volumetric_light", "Volumetric sunlight", "Объёмный солнечный свет", false},
    {"effect_point_fog", "Fog around lamps", "Туман вокруг ламп", false},
    {"effect_point_shadows", "Point light shadows (experimental)", "Тени от точечных источников (эксперимент)", false},
    {"effect_ray_tracing", "Ray traced lighting (experimental)", "Трассированное освещение (эксперимент)", false},
};
inline constexpr int EffectCount = int(sizeof(Effects) / sizeof(Effects[0]));
inline constexpr int EffectFirstLighting = 10;
inline constexpr int EffectSsr = 6;
inline constexpr int EffectGtao = 10;
inline constexpr int EffectModernSsr = 11;
inline constexpr int EffectPomCavity = 12;
inline constexpr int EffectContactShadows = 13;
inline constexpr int EffectPcss = 14;
inline constexpr int EffectGi = 15;
inline constexpr int EffectVolumetricLight = 16;
inline constexpr int EffectPointFog = 17;
inline constexpr int EffectPointShadows = 18;
inline constexpr int EffectRayTracing = 19;
inline constexpr bool EffectIsLive(int effect) { return effect >= EffectContactShadows; }
inline constexpr int OutputWidths[] = {1280,1600,1920,2560,3840,2560,3440,3840,3840,5120,5120,7680,0,0};
inline constexpr int OutputHeights[] = {720,900,1080,1440,2160,1080,1440,1600,1080,1440,2160,2160,0,0};
inline constexpr int OutputCount = 14;
inline constexpr int OutputAuto = 12;
inline constexpr int OutputCustom = 13;
bool ValidOutputSize(int w, int h);
inline constexpr int OutputDefault = 2;
static_assert(sizeof(OutputWidths) / sizeof(OutputWidths[0]) == OutputCount &&
              sizeof(OutputHeights) / sizeof(OutputHeights[0]) == OutputCount);
int OutputWidth(int index);
int OutputHeight(int index);
inline constexpr int CheatOff = 0, CheatInfinite = -1, CheatMaxAmount = 99;
inline constexpr int CheatGunRanges[] = {0, 150, 200, 300};
inline constexpr int CheatEchoMultipliers[] = {1, 2, 5, 10};
inline constexpr float HdrPaperWhiteDefault = 200.0f, HdrPaperWhiteMin = 10.0f, HdrPaperWhiteMax = 500.0f;
inline constexpr float HdrPeakDefault = 1000.0f, HdrPeakMin = 10.0f, HdrPeakMax = 4000.0f;
inline constexpr float HdrBoostDefault = 0.25f;
inline constexpr float HdrUiBrightnessMin = 0.5f, HdrUiBrightnessMax = 2.0f, HdrUiColorDefault = 1.0f;
enum HdrOutput : int { HdrOutputOff = 0, HdrOutputHdr10 = 1, HdrOutputScRgb = 2 };
enum HdrMode : int { HdrModeNative = 0, HdrModeExpand = 1, HdrModeCount };
inline constexpr const char* HdrModeNames[] = {"native", "expand"};
static_assert(sizeof(HdrModeNames) / sizeof(HdrModeNames[0]) == HdrModeCount);

inline constexpr int DlssPresetDefault = 0, DlssPresetK = 11, DlssPresetMax = 26;
inline constexpr int DlssPresetAuto = -1, DlssPresetL = 12, DlssPresetM = 13;
int DlssPresetFromText(const char* text);
const char* DlssPresetText(int preset);

enum OptiSubmit : int { OptiSubmitAuto = -1, OptiSubmitOff = 0, OptiSubmitOn = 1 };

enum ButtonPrompts : int { PromptsAuto = 0, PromptsKeyboard = 1, PromptsController = 2, PromptsCount };
inline constexpr const char* ButtonPromptsNames[] = {"auto", "keyboard", "controller"};
static_assert(sizeof(ButtonPromptsNames) / sizeof(ButtonPromptsNames[0]) == PromptsCount);

enum PadIcons : int { PadIconsAuto = 0, PadIconsPlayStation = 1, PadIconsXbox = 2, PadIconsCount };
inline constexpr const char* PadIconsNames[] = {"auto", "playstation", "xbox"};
static_assert(sizeof(PadIconsNames) / sizeof(PadIconsNames[0]) == PadIconsCount);

struct Values {
    std::atomic<MenuLanguage> menu_language{MenuLanguage::English};
    std::atomic<int> upscaler{UpscalerFsr3};
    std::atomic<int> preset{NativeAA};
    std::atomic<int> frame_gen{FrameGenOff};
    std::atomic<bool> frame_gen_cap{true};
    std::atomic<bool> frame_gen_active{false};
    std::atomic<float> frame_gen_shown_fps{0.0f}, frame_gen_game_fps{0.0f};
    std::atomic<int> frame_gen_multiplier{2};
    std::atomic<int> frame_gen_frames{1};
    std::atomic<bool> frame_gen_dynamic{false};
    std::atomic<int> frame_gen_target_fps{0};
    std::atomic<bool> dlss_fg_dynamic_supported{false};
    std::atomic<int> reflex{ReflexOn};
    std::atomic<bool> anti_lag{true}, anti_lag_supported{false};
    std::atomic<bool> streamline_loaded{false}, dlss_fg_supported{false};
    std::atomic<int> dlss_fg_max_frames{0};
    std::atomic<const char*> dlss_fg_problem{nullptr};
    std::atomic<bool> sharpen{true};
    std::atomic<float> sharpness{0.3f};
    std::atomic<bool> jitter{true};
    std::atomic<bool> reactive{false};
    std::atomic<bool> object_motion{true};
    std::atomic<float> reactive_scale{1.0f};
    std::atomic<float> reactive_threshold{0.2f};
    std::atomic<float> reactive_max{0.9f};
    std::atomic<float> fsr3_velocity_factor{1.0f}, fsr3_reactiveness_scale{1.0f},
        fsr3_shading_change_scale{1.0f}, fsr3_accumulation{0.333f}, fsr3_min_disocclusion{-0.333f};
    std::atomic<int> debug_view{DebugNone};
    std::atomic<bool> show_fps{false};
    std::atomic<bool> voice_enabled{true}, voice_push_to_talk{true}, voice_mic_muted{false};
    std::atomic<int> voice_talk_key{25};
    std::atomic<float> voice_volume{1.0f};
    std::atomic<bool> coop_status{true}, coop_notices{true}, coop_warp_requests{true}, coop_hud_ping{true}, coop_open_room{false};
    std::atomic<bool> show_fps_rate{true};
    std::atomic<bool> show_fps_frame_time{true};
    std::atomic<bool> show_fps_upscaler{true};
    std::atomic<bool> show_fps_cpu{false};
    std::atomic<bool> show_fps_gpu{false};
    std::atomic<bool> show_compile_note{true};
    std::atomic<int> fps_position{0};
    std::atomic<float> fps_scale{1.0f};
    std::atomic<unsigned> fps_text_color{0xFFFFFFu};
    std::atomic<unsigned> fps_bg_color{0x0F0F0Fu};
    std::atomic<float> fps_bg_opacity{0.5f};
    std::atomic<float> fps_text_opacity{1.0f};
    std::atomic<bool> fps_border{true};
    std::atomic<bool> hide_cursor{true};
    std::atomic<float> menu_x{-1.0f}, menu_y{-1.0f};
    std::atomic<bool> fsr4_auto_exposure{true};
    std::atomic<bool> fsr4_invert_jitter{false};
    std::atomic<int> active_render_width{1920}, active_render_height{1080};
    std::atomic<bool> motion_blur_held{false};
    std::atomic<bool> effects[EffectCount]{};
    std::atomic<bool> camera_controls{false};
    std::atomic<bool> widescreen{true},pc_controls{true},mouse_camera{true},mouse_menu{true};
    std::atomic<bool> menu_move{false};
    std::atomic<bool> mouse_invert_x{false},mouse_invert_y{false},mouse_auto_rotation{false};
    std::atomic<bool> pc_menus{true};
    std::atomic<int> mouse_sensitivity{5};
    std::atomic<int> button_prompts{PromptsAuto};
    std::atomic<int> pad_icons{PadIconsAuto};
    std::atomic<bool> change_appearance{false}, rebirth{false};
    std::atomic<bool> graphics_controls{false}, graphics_vignette{true};
    std::atomic<float> graphics_ao_strength{1},graphics_shadow_scale{1},graphics_bloom{1},graphics_saturation{1};
    std::atomic<float> graphics_gi_strength{6},graphics_volumetric_intensity{1},graphics_point_fog_intensity{1},graphics_ssr_intensity{1};
    std::atomic<float> graphics_point_fog_glow_size{0.08f}; // Lamp fog inverse-square core / radius, 0.03-0.20.
    std::atomic<int> graphics_shadow_resolution{0},graphics_local_shadow_resolution{0},graphics_motion_blur_quality{0};
    std::atomic<int> look_choice{0};
    std::atomic<float> graphics_fog{1},graphics_brightness{1},graphics_contrast{1},graphics_hue{0};
    std::atomic<float> camera_fov_scale{1.0f}, camera_distance_scale{1.0f}, camera_height_scale{1.0f};
    std::atomic<int> model_lod{0};
    std::atomic<int> output_res{OutputDefault};
    int custom_output_w = 1920, custom_output_h = 1080;
    std::atomic<int> cheat_vials{CheatOff}, cheat_bullets{CheatOff};
    std::atomic<bool> cheat_stamina{false}, cheat_stealth{false}, cheat_keep_echoes{false},
        cheat_no_durability{false}, cheat_consumables{false};
    std::atomic<int> cheat_gun_range{0}, cheat_echo_multiplier{1};
    std::atomic<bool> fullscreen{false};
    std::atomic<bool> hdr{false};
    std::atomic<float> hdr_paper_white{HdrPaperWhiteDefault}, hdr_peak{HdrPeakDefault},
        hdr_boost{HdrBoostDefault};
    std::atomic<bool> hdr_peak_auto{true};
    std::atomic<float> hdr_display_peak{0.0f};
    std::atomic<bool> hdr_compare{false};
    std::atomic<float> hdr_ui_brightness{1.0f}, hdr_ui_color{HdrUiColorDefault};
    std::atomic<int> hdr_mode{HdrModeNative};
    std::atomic<int> hdr_output{HdrOutputOff};
    std::atomic<int> live_resolution{0};
    std::atomic<const char*> fsr4_problem{nullptr};
    std::atomic<bool> fsr4_supported{false}, fsr411_supported{false}, dlss_supported{false},
        fsr_sdk_supported{false}, xess_supported{false};
    std::atomic<int> dlss_preset{DlssPresetK}, dlss_applied_preset{-1};
    std::atomic<bool> dlss_mip_bias{true};
    std::atomic<bool> dlss_auto_exposure{false};
    std::atomic<bool> dlss_jitter_fade{false};
    std::atomic<bool> upscale_before_post{false};
    std::atomic<float> render_scale{1.3f};
    std::atomic<float> mip_bias{0.0f};
    std::atomic<bool> optiscaler_compat{true}, optiscaler_ngx_lock{true}, optiscaler_clean_thread{true};
    std::atomic<int> optiscaler_sync_submit{OptiSubmitAuto};
    struct OptiScalerSwitches {
        bool compat = true, ngx_lock = true, clean_thread = true;
        int sync_submit = OptiSubmitAuto;
    } startup_optiscaler;

    int startup_preset = NativeAA;
    float startup_render_scale = 1.3f;
    int startup_frame_gen = FrameGenOff;
    int startup_upscaler = UpscalerFsr3;
    bool startup_object_motion = true;
    int startup_motion_blur_quality = 0;
    int startup_shadow_resolution = 0;
    int startup_local_shadow_resolution = 0;
    bool startup_effects[EffectCount]{};
    int startup_model_lod = 0;
    int startup_output_res = OutputDefault;
    int startup_live_resolution = 0;
    bool startup_hdr = false;
};

Values& Get();
inline float HdrPeakNits(const Values& v) {
    const float display = v.hdr_display_peak.load(std::memory_order_relaxed);
    const float peak = v.hdr_peak_auto.load(std::memory_order_relaxed) && display >= HdrPeakMin
                           ? display
                           : v.hdr_peak.load(std::memory_order_relaxed);
    return peak < HdrPeakMin ? HdrPeakMin : peak > HdrPeakMax ? HdrPeakMax : peak;
}
void SetValue(const std::string& key, const std::string& value);
std::string SavedValue(const char* key, const char* fallback);
const char* MenuText(const char* english, const char* russian);
inline const char* MenuText(const char* english) { return MenuText(english, english); }

void Load();
void ConfigureUpscalerSupport(bool fsr4, bool fsr411, bool dlss = false, bool fsr_sdk = false,
                              bool fsr4_to_sdk = false, bool xess = false);
bool FixedRenderSession();
int RenderPreset();
bool ResolutionNeedsRestart();
void Save();

float PresetScale(int preset);
const char* PresetName(int preset);
int ModelPreset(int preset);
bool DlssPresetValid(int preset);
const char* UpscalerName(int upscaler);

}
