#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
// SPDX-License-Identifier: GPL-2.0-or-later
#include "bbport_settings.h"
#include "bbport_menu_i18n.h"
#include "dlss_rules.h"
#include "input/ini_bindings.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>
#ifdef _WIN32
#include <io.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace BbSettings {

namespace {

const char* Path() {
    const char* env = std::getenv("BB_CONFIG");
    return env && env[0] ? env : "lantern.ini";
}

float Clamp(float v, float lo, float hi) {
    return std::clamp(v, lo, hi);
}

int CheatValue(const std::string& key, const std::string& value) {
    if (value == "off" || value == "0") {
        return CheatOff;
    }
    if (value == "infinite") {
        return CheatInfinite;
    }
    char* end = nullptr;
    const long amount = std::strtol(value.c_str(), &end, 10);
    if (!value.empty() && end && *end == '\0' && amount >= 1 && amount <= CheatMaxAmount) {
        return int(amount);
    }
    std::printf("Settings: unknown %s '%s' (expected off, infinite or 1-%d); off\n", key.c_str(),
                value.c_str(), CheatMaxAmount);
    return CheatOff;
}

std::string CheatText(int value) {
    return value == CheatInfinite ? "infinite"
           : value >= 1 && value <= CheatMaxAmount ? std::to_string(value)
                                                   : "off";
}

template <std::size_t N>
int CheatChoice(const std::string& key, const std::string& value, const int (&choices)[N], const char* expected) {
    if (value == "off") {
        return choices[0];
    }
    char* end = nullptr;
    const long v = std::strtol(value.c_str(), &end, 10);
    if (!value.empty() && end && *end == '\0' && std::find(choices, choices + N, int(v)) != choices + N) {
        return int(v);
    }
    std::printf("Settings: unknown %s '%s' (expected %s); %d\n", key.c_str(), value.c_str(), expected, choices[0]);
    return choices[0];
}

void Set(Values& v, const std::string& key, const std::string& value) {
    const float f = float(std::atof(value.c_str()));
    const int i = std::atoi(value.c_str());
    if (key == "menu_language") {
        if (value == "zh" || value == "zh-CN" || value == "zh_CN") {
            v.menu_language = MenuLanguage::ChineseSimplified;
            return;
        }
        bool known = false;
        for (int l = 0; l < MenuLanguageCount; ++l) {
            if (value == MenuLanguageCodes[l]) {
                v.menu_language = MenuLanguage(l);
                known = true;
            }
        }
        if (!known) {
            std::printf("Settings: unknown menu_language '%s' (expected en, ru, es, pt_BR, fr, de, it, pl "
                        "or tr, zh_CN)\n",
                        value.c_str());
        }
    } else if (key == "upscaler") {
        for (int u = 0; u < UpscalerCount; ++u) {
            if (value == UpscalerName(u)) {
                v.upscaler = u;
            }
        }
    } else if (key == "frame_gen") {
        v.frame_gen = value == "dlss"                     ? FrameGenDlss
                      : value == "fsr"                    ? FrameGenFsr
                      : (value == "fsr3" || value == "1") ? FrameGenFsr3
                                                          : FrameGenOff;
    } else if (key == "frame_gen_even_pacing") {
        v.frame_gen_cap = i != 0;
    } else if (key == "frame_gen_frames") {
        v.frame_gen_frames = std::clamp(i, 1, FrameGenFramesMax);
    } else if (key == "reflex") {
        int mode = -1;
        for (int m = 0; m < ReflexCount; ++m) {
            if (value == ReflexNames[m] || value == std::to_string(m)) {
                mode = m;
            }
        }
        if (mode < 0) {
            std::printf("Settings: unknown reflex '%s' (expected off, on or boost); on\n", value.c_str());
        }
        v.reflex = mode < 0 ? ReflexOn : mode;
    } else if (key == "anti_lag") {
        v.anti_lag = i != 0;
    } else if (key == "render_scale") {
        v.render_scale = Clamp(std::isfinite(f) && f > 0.0f ? f : 1.3f, RenderScaleMin, RenderScaleMax);
    } else if (key == "mip_bias") {
        v.mip_bias = Clamp(std::isfinite(f) ? f : 0.0f, MipBiasMin, MipBiasMax);
    } else if (key == "upscale_before_post") {
        v.upscale_before_post = i != 0;
    } else if (key == "dlss_jitter_fade") {
        v.dlss_jitter_fade = i != 0;
    } else if (key == "dlss_auto_exposure") {
        v.dlss_auto_exposure = i != 0;
    } else if (key == "preset") {
        v.preset = std::clamp(i, 0, PresetCount - 1);
    } else if (key == "sharpen") {
        v.sharpen = i != 0;
    } else if (key == "sharpness") {
        v.sharpness = Clamp(f, 0.0f, 2.0f);
    } else if (key == "jitter") {
        v.jitter = i != 0;
    } else if (key == "reactive") {
        v.reactive = i != 0;
    } else if (key == "object_motion") {
        v.object_motion = i != 0;
    } else if (key == "reactive_scale") {
        v.reactive_scale = Clamp(f, 0.0f, 16.0f);
    } else if (key == "reactive_threshold") {
        v.reactive_threshold = Clamp(f, 0.0f, 1.0f);
    } else if (key == "reactive_max") {
        v.reactive_max = Clamp(f, 0.0f, 1.0f);
    } else if (key == "frame_gen_dynamic") {
        v.frame_gen_dynamic = i != 0;
    } else if (key == "frame_gen_target_fps") {
        v.frame_gen_target_fps = std::clamp(i, 0, 1000);
    } else if (key == "fsr3_velocity_factor") {
        v.fsr3_velocity_factor = Clamp(f, 0.0f, 1.0f);
    } else if (key == "fsr3_reactiveness_scale") {
        v.fsr3_reactiveness_scale = Clamp(f, 0.0f, 4.0f);
    } else if (key == "fsr3_shading_change_scale") {
        v.fsr3_shading_change_scale = Clamp(f, 0.0f, 4.0f);
    } else if (key == "fsr3_accumulation") {
        v.fsr3_accumulation = Clamp(f, 0.0f, 1.0f);
    } else if (key == "fsr3_min_disocclusion") {
        v.fsr3_min_disocclusion = Clamp(f, -1.0f, 1.0f);
    } else if (key == "debug_view") {
    } else if (key == "show_fps") {
        v.show_fps = i != 0;
    } else if (key == "coop_status") {
        v.coop_status = i != 0;
    } else if (key == "coop_hud_ping") {
        v.coop_hud_ping = i != 0;
    } else if (key == "coop_notices") {
        v.coop_notices = i != 0;
    } else if (key == "coop_warp_requests") {
        v.coop_warp_requests = i != 0;
    } else if (key == "coop_open_room") {
        v.coop_open_room = i != 0;
    } else if (key == "voice_enabled") {
        v.voice_enabled = i != 0;
    } else if (key == "voice_push_to_talk") {
        v.voice_push_to_talk = i != 0;
    } else if (key == "voice_mic_muted") {
        v.voice_mic_muted = i != 0;
    } else if (key == "voice_talk_key") {
        if (i != 0 && i > -6 && i < 512) v.voice_talk_key = i;
    } else if (key == "voice_volume") {
        v.voice_volume = Clamp(f, 0.0f, 5.0f);
    } else if (key == "show_fps_rate") {
        v.show_fps_rate = i != 0;
    } else if (key == "show_fps_frame_time") {
        v.show_fps_frame_time = i != 0;
    } else if (key == "show_fps_upscaler") {
        v.show_fps_upscaler = i != 0;
    } else if (key == "show_fps_cpu") {
        v.show_fps_cpu = i != 0;
    } else if (key == "show_fps_gpu") {
        v.show_fps_gpu = i != 0;
    } else if (key == "compile_note") {
        v.show_compile_note = i != 0;
    } else if (key == "fps_position") {
        v.fps_position = std::clamp(i, 0, FpsPositionCount - 1);
    } else if (key == "fps_scale") {
        v.fps_scale = Clamp(f, 0.75f, 2.0f);
    } else if (key == "fps_text_color" || key == "fps_bg_color") {
        const char* hex = value.c_str() + (value.starts_with('#') ? 1 : 0);
        char* end = nullptr;
        const unsigned long rgb = std::strtoul(hex, &end, 16);
        if (end && end != hex && *end == '\0' && rgb <= 0xFFFFFFul) {
            (key == "fps_text_color" ? v.fps_text_color : v.fps_bg_color) = unsigned(rgb);
        }
    } else if (key == "fps_bg_opacity") {
        v.fps_bg_opacity = Clamp(f, 0.0f, 1.0f);
    } else if (key == "fps_text_opacity") {
        v.fps_text_opacity = Clamp(f, 0.1f, 1.0f);
    } else if (key == "fps_border") {
        v.fps_border = i != 0;
    } else if (key == "hide_cursor") {
        v.hide_cursor = i != 0;
    } else if (key == "menu_pos") {
        float x = -1.0f, y = -1.0f;
        if (std::sscanf(value.c_str(), "%f,%f", &x, &y) == 2 && x >= 0.0f && x <= 1.0f && y >= 0.0f &&
            y <= 1.0f) {
            v.menu_x = x;
            v.menu_y = y;
        }
    } else if (key == "dlss_preset") {
        v.dlss_preset = DlssPresetFromText(value.c_str());
    } else if (key == "dlss_mip_bias") {
        v.dlss_mip_bias = i != 0;
    } else if (key == "optiscaler_compat") {
        v.optiscaler_compat = i != 0;
    } else if (key == "optiscaler_ngx_lock") {
        v.optiscaler_ngx_lock = i != 0;
    } else if (key == "optiscaler_clean_thread") {
        v.optiscaler_clean_thread = i != 0;
    } else if (key == "optiscaler_sync_submit") {
        v.optiscaler_sync_submit = value == "1" ? OptiSubmitOn : value == "0" ? OptiSubmitOff : OptiSubmitAuto;
    } else if (key == "fsr4_auto_exposure") {
        v.fsr4_auto_exposure = i != 0;
    } else if (key == "fsr4_invert_jitter") {
        v.fsr4_invert_jitter = i != 0;
    } else if (key == "widescreen") {
        v.widescreen=i!=0;
    } else if (key == "pc_controls") {
        v.pc_controls=i!=0;
    } else if (key == "mouse_camera") {
        v.mouse_camera=i!=0;
    } else if (key == "mouse_menu") {
        v.mouse_menu=i!=0;
    } else if (key == "menu_move") {
        v.menu_move=i!=0;
    } else if (key == "mouse_invert_x") {
        v.mouse_invert_x=i!=0;
    } else if (key == "mouse_invert_y") {
        v.mouse_invert_y=i!=0;
    } else if (key == "mouse_auto_rotation") {
        v.mouse_auto_rotation=i!=0;
    } else if (key == "pc_menus") {
        v.pc_menus=i!=0;
    } else if (key == "button_prompts") {
        int mode = -1;
        for (int m = 0; m < PromptsCount; ++m) {
            if (value == ButtonPromptsNames[m]) {
                mode = m;
            }
        }
        if (mode < 0) {
            std::printf("Settings: unknown button_prompts '%s' (expected auto, keyboard or controller); auto\n",
                        value.c_str());
        }
        v.button_prompts = mode < 0 ? PromptsAuto : mode;
    } else if (key == "pad_icons") {
        int mode = -1;
        for (int m = 0; m < PadIconsCount; ++m) {
            if (value == PadIconsNames[m]) {
                mode = m;
            }
        }
        if (mode < 0) {
            std::printf("Settings: unknown pad_icons '%s' (expected auto, playstation or xbox); auto\n",
                        value.c_str());
        }
        v.pad_icons = mode < 0 ? PadIconsAuto : mode;
    } else if (key == "mouse_sensitivity") {
        v.mouse_sensitivity=std::clamp(i,-10,20);
    } else if (key == "camera_controls") {
        v.camera_controls = i != 0;
    } else if (key == "change_appearance") {
        v.change_appearance = i != 0;
    } else if (key == "rebirth") {
        v.rebirth = i != 0;
    } else if (key == "camera_fov_scale") {
        v.camera_fov_scale = Clamp(std::isfinite(f) ? f : 1.0f, 1.0f, 1.5f);
    } else if (key == "camera_distance_scale") {
        v.camera_distance_scale = Clamp(std::isfinite(f) ? f : 1.0f, 0.5f, 1.5f);
    } else if (key == "camera_height_scale") {
        v.camera_height_scale = Clamp(std::isfinite(f) ? f : 1.0f, 0.5f, 1.5f);
    } else if (key == "graphics_controls") {
        v.graphics_controls=i!=0;
    } else if (key == "graphics_vignette") {
        v.graphics_vignette=i!=0;
    } else if (key == "graphics_ao_strength") {
        v.graphics_ao_strength=Clamp(std::isfinite(f)?f:1.0f,0.0f,5.0f);
    } else if (key == "graphics_gi_strength") {
        v.graphics_gi_strength=Clamp(std::isfinite(f)?f:6.0f,0.0f,12.0f);
    } else if (key == "graphics_volumetric_intensity") {
        v.graphics_volumetric_intensity=Clamp(std::isfinite(f)?f:1.0f,0.0f,3.0f);
    } else if (key == "graphics_point_fog_intensity") {
        v.graphics_point_fog_intensity=Clamp(std::isfinite(f)?f:1.0f,0.0f,3.0f);
    } else if (key == "graphics_point_fog_glow_size") {
        v.graphics_point_fog_glow_size=PointFogGlowSize(f);
    } else if (key == "graphics_point_fog_falloff") {
        if (value != "0" && value != "1") {
            std::printf("Settings: unknown graphics_point_fog_falloff '%s' (expected 0 or 1); classic\n",
                        value.c_str());
        }
        v.graphics_point_fog_falloff = value == "1" ? PointFogFalloffInverseSquare : PointFogFalloffClassic;
    } else if (key == "graphics_ssr_intensity") {
        v.graphics_ssr_intensity=Clamp(std::isfinite(f)?f:1.0f,0.0f,3.0f);
    } else if (key == "graphics_shadow_resolution") {
        v.graphics_shadow_resolution=std::clamp(i,0,2);
    } else if (key == "graphics_local_shadow_resolution") {
        v.graphics_local_shadow_resolution=std::clamp(i,0,2);
    } else if (key == "graphics_motion_blur_quality") {
        v.graphics_motion_blur_quality=std::clamp(i,0,1);
    } else if (key == "graphics_ao_quality") {
        if (value != "0" && value != "1") {
            std::printf("Settings: unknown graphics_ao_quality '%s' (expected 0 or 1); regular GTAO\n",
                        value.c_str());
        }
        v.graphics_ao_quality = value == "1" ? 1 : 0;
    } else if (key == "graphics_shadow_scale") {
        v.graphics_shadow_scale=Clamp(std::isfinite(f)?f:1.0f,0.25f,8.0f);
    } else if (key == "graphics_bloom") {
        v.graphics_bloom=Clamp(std::isfinite(f)?f:1.0f,0.0f,10.0f);
    } else if (key == "graphics_saturation") {
        v.graphics_saturation=Clamp(std::isfinite(f)?f:1.0f,0.0f,5.0f);
    } else if (key == "graphics_fog") {
        v.graphics_fog=Clamp(std::isfinite(f)?f:1.0f,0.0f,5.0f);
    } else if (key == "graphics_brightness") {
        v.graphics_brightness=Clamp(std::isfinite(f)?f:1.0f,0.0f,5.0f);
    } else if (key == "graphics_contrast") {
        v.graphics_contrast=Clamp(std::isfinite(f)?f:1.0f,0.5f,1.5f);
    } else if (key == "graphics_hue") {
        v.graphics_hue=Clamp(std::isfinite(f)?f:0.0f,-180.0f,180.0f);
    } else if (key == "model_lod") {
        v.model_lod = std::clamp(i, -2, 2);
    } else if (key == "fullscreen") {
        v.fullscreen = i != 0;
    } else if (key == "hdr") {
        v.hdr = i != 0;
    } else if (key == "hdr_paper_white") {
        v.hdr_paper_white = Clamp(std::isfinite(f) ? f : HdrPaperWhiteDefault, HdrPaperWhiteMin,
                                  HdrPaperWhiteMax);
    } else if (key == "hdr_peak") {
        v.hdr_peak = Clamp(std::isfinite(f) ? f : HdrPeakDefault, HdrPeakMin, HdrPeakMax);
    } else if (key == "hdr_peak_auto") {
        v.hdr_peak_auto = i != 0;
    } else if (key == "hdr_boost") {
        v.hdr_boost = Clamp(std::isfinite(f) ? f : HdrBoostDefault, 0.0f, 1.0f);
    } else if (key == "hdr_ui_brightness") {
        v.hdr_ui_brightness = Clamp(std::isfinite(f) ? f : 1.0f, HdrUiBrightnessMin, HdrUiBrightnessMax);
    } else if (key == "hdr_ui_color") {
        v.hdr_ui_color = Clamp(std::isfinite(f) ? f : HdrUiColorDefault, 0.0f, 1.0f);
    } else if (key == "hdr_mode") {
        int mode = -1;
        for (int m = 0; m < HdrModeCount; ++m) {
            if (value == HdrModeNames[m]) {
                mode = m;
            }
        }
        if (mode < 0) {
            std::printf("Settings: unknown hdr_mode '%s' (expected native or expand); native\n",
                        value.c_str());
        }
        v.hdr_mode = mode < 0 ? HdrModeNative : mode;
    } else if (key == "live_resolution") {
        v.live_resolution = value == "auto" ? -1 : std::clamp(i, 0, 1);
    } else if (key == "cheat_vials") {
        v.cheat_vials = CheatValue(key, value);
    } else if (key == "cheat_bullets") {
        v.cheat_bullets = CheatValue(key, value);
    } else if (key == "cheat_stamina") {
        v.cheat_stamina = i != 0;
    } else if (key == "cheat_stealth") {
        v.cheat_stealth = i != 0;
    } else if (key == "cheat_keep_echoes") {
        v.cheat_keep_echoes = i != 0;
    } else if (key == "cheat_no_durability") {
        v.cheat_no_durability = i != 0;
    } else if (key == "cheat_consumables") {
        v.cheat_consumables = i != 0;
    } else if (key == "cheat_gun_range") {
        v.cheat_gun_range = CheatChoice(key, value, CheatGunRanges, "off, 150, 200 or 300");
    } else if (key == "cheat_echo_multiplier") {
        v.cheat_echo_multiplier = CheatChoice(key, value, CheatEchoMultipliers, "1, 2, 5 or 10");
    } else if (key == "output_res") {
        int w = 0, h = 0;
        char tail = 0;
        if (value == "auto") {
            v.output_res = OutputAuto;
        } else if (std::sscanf(value.c_str(), "%dx%d%c", &w, &h, &tail) == 2 && ValidOutputSize(w, h)) {
            v.output_res = OutputCustom;
            v.custom_output_w = w & ~1;
            v.custom_output_h = h & ~1;
        }
        for (int r = 0; r < OutputAuto; ++r) {
            if (value == std::to_string(OutputWidths[r]) + "x" + std::to_string(OutputHeights[r])) {
                v.output_res = r;
            }
        }
    } else {
        for (int e = 0; e < EffectCount; ++e) {
            if (key == Effects[e].key) {
                v.effects[e] = i != 0;
            }
        }
    }
}

MenuLanguage LanguageOfLocale(const char* lang) {
    if (!lang || !lang[0] || !lang[1]) {
        return MenuLanguage::English;
    }
    const auto lower = [lang](int i) { return char(std::tolower(static_cast<unsigned char>(lang[i]))); };
    if (lower(0) == 'p' && lower(1) == 't') {
        return (lang[2] == '_' || lang[2] == '-') && lower(3) == 'b' && lower(4) == 'r'
                   ? MenuLanguage::PortugueseBrazil
                   : MenuLanguage::English;
    }
    for (int l = 0; l < MenuLanguageCount; ++l) {
        const char* code = MenuLanguageCodes[l];
        if (code[2] == '\0' && lower(0) == code[0] && lower(1) == code[1]) {
            return MenuLanguage(l);
        }
    }
    return MenuLanguage::English;
}

}

static std::pair<int, int> DisplaySize() {
    static const std::pair<int, int> size = [] {
        int w = 0, h = 0;
        const char* env = std::getenv("BB_DISPLAY_SIZE");
        if (env && std::sscanf(env, "%dx%d", &w, &h) == 2 && w >= 640 && h >= 360 && w <= 16384 &&
            h <= 8640) {
            return std::pair<int, int>{w & ~1, h & ~1};
        }
        return std::pair<int, int>{1920, 1080};
    }();
    return size;
}

bool ValidOutputSize(int w, int h) {
    return w >= 640 && h >= 360 && w <= 16384 && h <= 8640;
}

int OutputWidth(int index) {
    index = std::clamp(index, 0, OutputCount - 1);
    if (index == OutputCustom) return Get().custom_output_w;
    return index == OutputAuto ? DisplaySize().first : OutputWidths[index];
}

int OutputHeight(int index) {
    index = std::clamp(index, 0, OutputCount - 1);
    if (index == OutputCustom) return Get().custom_output_h;
    return index == OutputAuto ? DisplaySize().second : OutputHeights[index];
}

Values& Get() {
    static Values values;
    return values;
}

const char* MenuText(const char* english, const char* russian) {
    switch (const MenuLanguage language = Get().menu_language) {
    case MenuLanguage::English: return english;
    case MenuLanguage::Russian: return russian;
    default: return MenuTranslation(language, english);
    }
}

namespace {

enum class SettingsFileState { Read, Absent, Unreadable };

struct SettingsFileText {
    SettingsFileState state = SettingsFileState::Absent;
    std::string text;
};

constexpr int SettingsReadAttempts = 4;
constexpr int SettingsReplaceAttempts = 5;
constexpr int SettingsLockAttempts = 50;
constexpr std::chrono::milliseconds SettingsRetryPause{2};
constexpr std::size_t SettingsReadChunk = 4096;
constexpr const char* SettingsHeader = "# Lantern Engine settings (in-game menu: Insert / L3+R3)";

constexpr int SaveRetryAttempts = 6;
constexpr std::chrono::milliseconds SaveRetryFirstDelay{50};
constexpr std::chrono::milliseconds ExitSaveBound{4000};
constexpr std::string_view Utf8Bom = "\xEF\xBB\xBF";

using KeyList = std::vector<std::pair<std::string, std::string>>;

KeyList CurrentKeys();

struct SavedKey {
    std::string loaded;
    std::optional<std::string> raw;
};

struct SaveStore {
    std::mutex mutex;
    std::map<std::string, SavedKey, std::less<>> baseline;
    std::map<std::string, std::string, std::less<>> raw;
    bool load_unreadable = false;
};

struct SaveQueue {
    std::mutex mutex;
    std::condition_variable wake;
    std::condition_variable idle;
    KeyList keys;
    bool pending = false;
    bool fresh = false;
    bool busy = false;
    std::once_flag started;
};

SaveStore& g_save_store = *new SaveStore;
SaveQueue& g_save_queue = *new SaveQueue;
std::atomic<bool> g_settings_write_failed{false};
std::atomic<std::uint32_t> g_settings_load_failures{0};
std::atomic<std::uint32_t> g_settings_dropped{0};
std::atomic<std::uint32_t> g_settings_read_failures{0};
std::atomic<std::uint32_t> g_settings_replace_failures{0};
std::atomic<std::uint32_t> g_settings_lock_failures{0};
std::atomic<std::uint32_t> g_settings_temporaries{0};

struct FileClose {
    void operator()(FILE* file) const noexcept { std::fclose(file); }
};
using FilePtr = std::unique_ptr<FILE, FileClose>;

#ifdef _WIN32
struct HandleClose {
    void operator()(HANDLE handle) const noexcept { CloseHandle(handle); }
};
using HandlePtr = std::unique_ptr<std::remove_pointer_t<HANDLE>, HandleClose>;

SettingsFileState ReadSettingsOnce(const char* path, std::string& text) {
    HANDLE raw = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (raw == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ? SettingsFileState::Absent
                                                                                : SettingsFileState::Unreadable;
    }
    const HandlePtr file{raw};
    text.clear();
    char chunk[SettingsReadChunk];
    DWORD got = 0;
    while (true) {
        if (!ReadFile(file.get(), chunk, DWORD(sizeof chunk), &got, nullptr)) {
            return SettingsFileState::Unreadable;
        }
        if (got == 0) {
            return SettingsFileState::Read;
        }
        text.append(chunk, got);
    }
}

bool FlushToDisk(FILE* file) {
    const intptr_t os_handle = _get_osfhandle(_fileno(file));
    return os_handle != -1 && FlushFileBuffers(reinterpret_cast<HANDLE>(os_handle)) != 0;
}

bool FlushFolder(const char*) {
    return true;
}

class SettingsFileLock {
public:
    explicit SettingsFileLock(const std::string& path) {
        for (int attempt = 0; attempt < SettingsLockAttempts; ++attempt) {
            if (attempt > 0) {
                std::this_thread::sleep_for(SettingsRetryPause);
            }
            handle_ = CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
            if (handle_ != INVALID_HANDLE_VALUE) {
                return;
            }
        }
    }
    ~SettingsFileLock() {
        if (held()) {
            CloseHandle(handle_);
        }
    }
    SettingsFileLock(const SettingsFileLock&) = delete;
    SettingsFileLock& operator=(const SettingsFileLock&) = delete;
    [[nodiscard]] bool held() const { return handle_ != INVALID_HANDLE_VALUE; }

private:
    HANDLE handle_ = INVALID_HANDLE_VALUE;
};

bool ReplaceSettingsOnce(const char* from, const char* to) {
    return MoveFileExA(from, to, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}

unsigned long ProcessId() {
    return GetCurrentProcessId();
}
#else
SettingsFileState ReadSettingsOnce(const char* path, std::string& text) {
    const FilePtr file{std::fopen(path, "rb")};
    if (!file) {
        return errno == ENOENT ? SettingsFileState::Absent : SettingsFileState::Unreadable;
    }
    text.clear();
    char chunk[SettingsReadChunk];
    std::size_t got = 0;
    while ((got = std::fread(chunk, 1, sizeof chunk, file.get())) > 0) {
        text.append(chunk, got);
    }
    return std::ferror(file.get()) ? SettingsFileState::Unreadable : SettingsFileState::Read;
}

bool FlushToDisk(FILE* file) {
    return fsync(fileno(file)) == 0;
}

bool FlushFolder(const char* path) {
    std::string folder(path);
    const auto slash = folder.find_last_of('/');
    folder = slash == std::string::npos ? std::string(".") : folder.substr(0, slash == 0 ? 1 : slash);
    const int handle = open(folder.c_str(), O_RDONLY | O_DIRECTORY);
    if (handle < 0) {
        return false;
    }
    const bool synced = fsync(handle) == 0;
    return close(handle) == 0 && synced;
}

bool ReplaceSettingsOnce(const char* from, const char* to) {
    return std::rename(from, to) == 0;
}

class SettingsFileLock {
public:
    explicit SettingsFileLock(const std::string& path) {
        handle_ = open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
        if (handle_ < 0) {
            return;
        }
        for (int attempt = 0; attempt < SettingsLockAttempts; ++attempt) {
            if (attempt > 0) {
                std::this_thread::sleep_for(SettingsRetryPause);
            }
            if (flock(handle_, LOCK_EX | LOCK_NB) == 0) {
                locked_ = true;
                return;
            }
        }
    }
    ~SettingsFileLock() {
        if (handle_ >= 0) {
            close(handle_);
        }
    }
    SettingsFileLock(const SettingsFileLock&) = delete;
    SettingsFileLock& operator=(const SettingsFileLock&) = delete;
    [[nodiscard]] bool held() const { return locked_; }

private:
    int handle_ = -1;
    bool locked_ = false;
};

unsigned long ProcessId() {
    return static_cast<unsigned long>(getpid());
}
#endif

SettingsFileText ReadSettingsFile(const char* path) {
    SettingsFileText result;
    for (int attempt = 0; attempt < SettingsReadAttempts; ++attempt) {
        if (attempt > 0) {
            std::this_thread::sleep_for(SettingsRetryPause);
        }
        result.state = ReadSettingsOnce(path, result.text);
        if (result.state != SettingsFileState::Unreadable) {
            return result;
        }
    }
    result.text.clear();
    return result;
}

std::string_view KeyOf(std::string_view line) {
    const auto eq = line.find('=');
    if (line.empty() || line.front() == '#' || eq == std::string_view::npos) {
        return {};
    }
    std::string_view key = line.substr(0, eq);
    const auto first = key.find_first_not_of(" \t");
    if (first == std::string_view::npos) {
        return {};
    }
    key.remove_prefix(first);
    key = key.substr(0, key.find_last_not_of(" \t") + 1);
    return key;
}

template <typename Visit>
void ForEachSetting(std::string_view text, Visit&& visit) {
    if (text.substr(0, Utf8Bom.size()) == Utf8Bom) {
        text.remove_prefix(Utf8Bom.size());
    }
    std::size_t start = 0;
    while (start < text.size()) {
        auto end = text.find('\n', start);
        if (end == std::string_view::npos) {
            end = text.size();
        }
        std::string_view line = text.substr(start, end - start);
        start = end + 1;
        const auto last = line.find_last_not_of(" \t\r");
        line = last == std::string_view::npos ? std::string_view{} : line.substr(0, last + 1);
        const std::string_view key = KeyOf(line);
        if (!key.empty()) {
            visit(key, line.substr(line.find('=') + 1));
        }
    }
}

std::string MergeSettings(std::string_view before, const KeyList& keys) {
    if (before.substr(0, Utf8Bom.size()) == Utf8Bom) {
        before.remove_prefix(Utf8Bom.size());
    }
    std::string out;
    std::vector<bool> written(keys.size());
    std::size_t start = 0;
    while (start < before.size()) {
        auto end = before.find('\n', start);
        if (end == std::string_view::npos) {
            end = before.size();
        }
        std::string_view line = before.substr(start, end - start);
        start = end + 1;
        while (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        const std::string_view key = KeyOf(line);
        const auto it = key.empty() ? keys.end()
                                    : std::find_if(keys.begin(), keys.end(),
                                                   [&](const auto& k) { return k.first == key; });
        if (it != keys.end()) {
            out += it->first + "=" + it->second + "\n";
            written[std::size_t(it - keys.begin())] = true;
            continue;
        }
        out.append(line);
        out += '\n';
    }
    if (before.empty()) {
        out = std::string(SettingsHeader) + "\n";
    }
    for (std::size_t k = 0; k < keys.size(); ++k) {
        if (!written[k]) {
            out += keys[k].first + "=" + keys[k].second + "\n";
        }
    }
    return out;
}

bool WriteWholeFile(const std::string& path, std::string_view text) {
    FilePtr file{std::fopen(path.c_str(), "wb")};
    if (!file) {
        return false;
    }
    const bool written = std::fwrite(text.data(), 1, text.size(), file.get()) == text.size();
    const bool flushed = std::fflush(file.get()) == 0 && FlushToDisk(file.get());
    const bool closed = std::fclose(file.release()) == 0;
    return written && flushed && closed;
}

bool WriteReplace(const char* target, std::string_view text) {
    const std::string temporary = std::string(target) + "." + std::to_string(ProcessId()) + "." +
                                  std::to_string(g_settings_temporaries.fetch_add(1, std::memory_order_relaxed)) +
                                  ".tmp";
    bool replaced = WriteWholeFile(temporary, text);
    if (replaced) {
        replaced = false;
        for (int attempt = 0; attempt < SettingsReplaceAttempts && !replaced; ++attempt) {
            if (attempt > 0) {
                std::this_thread::sleep_for(SettingsRetryPause);
            }
            replaced = ReplaceSettingsOnce(temporary.c_str(), target);
        }
    }
    if (!replaced) {
        std::remove(temporary.c_str());
        return false;
    }
    return FlushFolder(target);
}

bool Retryable(SaveResult result) {
    return result == SaveResult::Unreadable || result == SaveResult::NotLocked || result == SaveResult::NotReplaced;
}

void NoteSaveResult(bool saved, const char* what) {
    const bool failed_before = g_settings_write_failed.exchange(!saved, std::memory_order_relaxed);
    if (!saved && !failed_before) {
        std::printf("Settings: %s lantern.ini; it keeps what it had and the next save tries again\n", what);
    } else if (saved && failed_before) {
        std::printf("Settings: lantern.ini written again\n");
    }
}

SaveResult SaveKeys(const KeyList& keys) {
    const std::scoped_lock lock{g_save_store.mutex};
    if (g_save_store.load_unreadable) {
        NoteSaveResult(false, "did not read");
        return SaveResult::LoadUnreadable;
    }
    const char* target = Path();
    const SettingsFileLock file_lock{std::string(target) + ".lock"};
    if (!file_lock.held()) {
        g_settings_lock_failures.fetch_add(1, std::memory_order_relaxed);
        NoteSaveResult(false, "cannot lock");
        return SaveResult::NotLocked;
    }
    const SettingsFileText before = ReadSettingsFile(target);
    if (before.state == SettingsFileState::Unreadable) {
        g_settings_read_failures.fetch_add(1, std::memory_order_relaxed);
        NoteSaveResult(false, "cannot read");
        return SaveResult::Unreadable;
    }
    std::map<std::string, std::string, std::less<>> disk;
    ForEachSetting(before.text, [&](std::string_view key, std::string_view value) {
        disk.try_emplace(std::string(key), value);
    });
    KeyList write;
    for (const auto& [key, value] : keys) {
        const auto base = g_save_store.baseline.find(key);
        const auto now = disk.find(key);
        SavedKey late;
        if (base == g_save_store.baseline.end()) {
            const auto raw = g_save_store.raw.find(key);
            late = raw != g_save_store.raw.end() ? SavedKey{raw->second, raw->second} : SavedKey{value, std::nullopt};
        }
        const SavedKey& saved = base != g_save_store.baseline.end() ? base->second : late;
        const bool engine_changed = saved.loaded != value;
        const bool untouched = now == disk.end() ? !saved.raw.has_value() : saved.raw == now->second;
        if (engine_changed || untouched) {
            write.emplace_back(key, value);
        }
    }
    const std::string text = MergeSettings(before.text, write);
    const bool same = before.state == SettingsFileState::Read && text == before.text;
    if (!same) {
        const bool backed_up = before.state != SettingsFileState::Read ||
                               WriteReplace((std::string(target) + ".bak").c_str(), before.text);
        if (!backed_up || !WriteReplace(target, text)) {
            g_settings_replace_failures.fetch_add(1, std::memory_order_relaxed);
            NoteSaveResult(false, "cannot write");
            return SaveResult::NotReplaced;
        }
    }
    for (const auto& [key, value] : write) {
        g_save_store.baseline.insert_or_assign(key, SavedKey{value, value});
    }
    NoteSaveResult(true, "");
    return same ? SaveResult::Unchanged : SaveResult::Written;
}

void SaveWorker() {
    auto& queue = g_save_queue;
    std::unique_lock lock{queue.mutex};
    int attempt = 0;
    while (true) {
        queue.wake.wait(lock, [&] { return queue.pending; });
        if (queue.fresh) {
            queue.fresh = false;
            attempt = 0;
        }
        KeyList keys = std::move(queue.keys);
        queue.keys.clear();
        queue.pending = false;
        queue.busy = true;
        lock.unlock();
        const SaveResult result = SaveKeys(keys);
        lock.lock();
        if (Retryable(result) && attempt < SaveRetryAttempts) {
            const auto delay = SaveRetryFirstDelay * (1 << attempt);
            ++attempt;
            queue.wake.wait_for(lock, delay, [&] { return queue.fresh; });
            if (!queue.fresh) {
                queue.keys = std::move(keys);
            }
            queue.pending = true;
            continue;
        }
        if (Retryable(result)) {
            g_settings_dropped.fetch_add(1, std::memory_order_relaxed);
        }
        attempt = 0;
        queue.busy = false;
        if (!queue.pending) {
            queue.idle.notify_all();
        }
    }
}

void LoadBaseline(const SettingsFileText& file) {
    const std::scoped_lock lock{g_save_store.mutex};
    g_save_store.load_unreadable = file.state == SettingsFileState::Unreadable;
    g_save_store.baseline.clear();
    g_save_store.raw.clear();
    ForEachSetting(file.text, [&](std::string_view key, std::string_view value) {
        g_save_store.raw.try_emplace(std::string(key), value);
    });
    for (auto& [key, value] : CurrentKeys()) {
        const auto found = g_save_store.raw.find(key);
        SavedKey saved{std::move(value), std::nullopt};
        if (found != g_save_store.raw.end()) {
            saved.raw = found->second;
        }
        g_save_store.baseline.insert_or_assign(key, std::move(saved));
    }
}

} // namespace

void SetValue(const std::string& key, const std::string& value) {
    Set(Get(), key, value);
}

std::string SavedValue(const char* key, const char* fallback) {
    std::optional<std::string> found;
    const SettingsFileText file = ReadSettingsFile(Path());
    ForEachSetting(file.text, [&](std::string_view name, std::string_view value) {
        if (!found && name == key) {
            found = std::string(value);
        }
    });
    return found.value_or(fallback);
}

void Load() {
    auto& v = Get();
    for (int e = 0; e < EffectCount; ++e) {
        v.effects[e] = Effects[e].default_on;
    }
    {
        const char* lang = std::getenv("BB_MENU_LANGUAGE");
        if (!lang || !*lang) {
            for (const char* key : {"LC_ALL", "LC_MESSAGES", "LANG", "LANGUAGE"}) {
                if (const char* value = std::getenv(key); value && *value) {
                    lang = value;
                    break;
                }
            }
        }
        v.menu_language = LanguageOfLocale(lang);
    }
    const SettingsFileText file = ReadSettingsFile(Path());
    if (file.state == SettingsFileState::Read) {
        ForEachSetting(file.text, [&](std::string_view key, std::string_view value) {
            Set(v, std::string(key), std::string(value));
        });
        std::printf("Settings: %s\n", Path());
    } else if (file.state == SettingsFileState::Unreadable) {
        g_settings_load_failures.fetch_add(1, std::memory_order_relaxed);
        g_settings_write_failed.store(true, std::memory_order_relaxed);
        std::printf("Settings: lantern.ini could not be read; the defaults are used and nothing is saved this run\n");
    }
    if (const char* env = std::getenv("BB_UPSCALER")) {
        v.upscaler = UpscalerOff;
        for (int u = 0; u < UpscalerCount; ++u) {
            if (std::strcmp(env, UpscalerName(u)) == 0) v.upscaler = u;
        }
    }
    const std::pair<const char*, const char*> env_keys[] = {
        {"BB_FSR_SHARPNESS", "sharpness"},        {"BB_JITTER", "jitter"},
        {"BB_REACTIVE", "reactive"},              {"BB_REACTIVE_SCALE", "reactive_scale"},
        {"BB_REACTIVE_THRESHOLD", "reactive_threshold"}, {"BB_REACTIVE_MAX", "reactive_max"},
        {"BB_UPSCALE_PRESET", "preset"},            {"BB_OBJECT_MOTION", "object_motion"},
        {"BB_HDR", "hdr"},                          {"BB_HDR_PAPER_WHITE", "hdr_paper_white"},
        {"BB_HDR_PEAK", "hdr_peak"},                {"BB_HDR_BOOST", "hdr_boost"},
        {"BB_HDR_MODE", "hdr_mode"},                {"BB_FRAME_GEN", "frame_gen"},
        {"BB_FRAME_GEN_CAP", "frame_gen_even_pacing"},     {"BB_DLSS_PRESET", "dlss_preset"},
        {"BB_DLSS_MIP_BIAS", "dlss_mip_bias"},   {"BB_FRAME_GEN_FRAMES", "frame_gen_frames"},
        {"BB_REFLEX", "reflex"},                   {"BB_RENDER_SCALE", "render_scale"},
        {"BB_MIP_BIAS", "mip_bias"},               {"BB_DLSS_JITTER_FADE", "dlss_jitter_fade"},
    };
    for (const auto& [env, key] : env_keys) {
        if (const char* value = std::getenv(env)) {
            Set(v, key, value);
        }
    }
    if (std::getenv("BB_HDR_PEAK")) {
        v.hdr_peak_auto = false;
    }
    v.startup_preset = v.preset;
    v.startup_render_scale = v.render_scale;
    v.startup_frame_gen = v.frame_gen;
    v.startup_upscaler = v.upscaler;
    v.startup_motion_blur_quality = v.graphics_motion_blur_quality;
    v.startup_ao_quality = v.graphics_ao_quality;
    v.startup_shadow_resolution = v.graphics_shadow_resolution;
    v.startup_local_shadow_resolution = v.graphics_local_shadow_resolution;
    v.startup_object_motion = v.object_motion;
    for (int e = 0; e < EffectCount; ++e) {
        v.startup_effects[e] = v.effects[e];
    }
    v.startup_model_lod = v.model_lod;
    v.startup_output_res = v.output_res;
    v.startup_live_resolution = v.live_resolution;
    v.startup_hdr = v.hdr;
    v.startup_optiscaler = {v.optiscaler_compat, v.optiscaler_ngx_lock, v.optiscaler_clean_thread,
                            v.optiscaler_sync_submit};
    LoadBaseline(file);
}

void ConfigureUpscalerSupport(bool fsr4, bool fsr411, bool dlss, bool fsr_sdk, bool fsr4_to_sdk,
                              bool xess) {
    auto& v = Get();
    v.fsr4_supported = fsr4 && !(fsr4_to_sdk && fsr_sdk);
    v.fsr411_supported = fsr4 && fsr411;
    v.dlss_supported = dlss;
    v.fsr_sdk_supported = fsr_sdk;
    v.xess_supported = xess;
    const int requested = v.upscaler;
    if (requested == UpscalerFsr4 && fsr4_to_sdk && fsr_sdk) {
        std::printf("Upscaler: fsr4 runs through AMD's FSR SDK on this GPU: the built-in FSR 4 model is "
                    "far slower on Radeon (BB_FSR4_BUILTIN_AMD=1 keeps it)\n");
        v.upscaler = UpscalerFsr;
    } else if (requested == UpscalerFsr && !fsr_sdk) {
        v.fsr4_problem = "FSR needs the AMD FSR SDK files (engine/fsr_sdk); using FSR 3.1";
        std::printf("Upscaler: fsr unavailable (AMD FSR SDK files missing or GPU unsupported); falling back to FSR 3.1 before the first frame\n");
        v.upscaler = UpscalerFsr3;
    } else if (requested == UpscalerDlss && !dlss) {
        v.fsr4_problem = "DLSS needs an NVIDIA RTX GPU, its driver's NGX and nvngx_dlss; using FSR 3.1";
        std::printf("Upscaler: dlss unavailable; falling back to FSR 3.1 before the first frame\n");
        v.upscaler = UpscalerFsr3;
    } else if (requested == UpscalerXess && !xess) {
        v.fsr4_problem = "XeSS needs Windows, Intel's libxess.dll (engine/xess) and a GPU it supports; using FSR 3.1";
        std::printf("Upscaler: xess unavailable; falling back to FSR 3.1 before the first frame\n");
        v.upscaler = UpscalerFsr3;
    } else if (requested == UpscalerFsr411 && !v.fsr411_supported && v.fsr4_supported) {
        v.fsr4_problem = "FSR 4.1.1 needs its fsr4_411 files and a GPU that runs its shaders; using FSR 4";
        std::printf("Upscaler: fsr411 unavailable (GPU or fsr4_411 files); falling back to FSR 4 before the first frame\n");
        v.upscaler = UpscalerFsr4;
    } else if ((requested == UpscalerFsr4 && !v.fsr4_supported) ||
        (requested == UpscalerFsr411 && !v.fsr411_supported)) {
        v.fsr4_problem = "GPU does not support the selected FSR 4 shaders; using FSR 3.1";
        std::printf("Upscaler: %s unsupported on this GPU; falling back to FSR 3.1 before the first frame\n",
                    UpscalerName(requested));
        v.upscaler = UpscalerFsr3;
    }
}

bool FixedRenderSession() {
    const char* size = std::getenv("BB_RENDER_RES");
    return size && size[0];
}

int RenderPreset() {
    const auto& v = Get();
    return FixedRenderSession() ? v.startup_preset :
        v.upscaler == UpscalerTaa ? NativeAA : v.preset.load();
}

bool ResolutionNeedsRestart() {
    const auto& v = Get();
    return FixedRenderSession() &&
        (v.preset != v.startup_preset || v.output_res != v.startup_output_res ||
         (v.preset == PresetCustom && v.render_scale != v.startup_render_scale) ||
         (v.upscaler == UpscalerOff) != (v.startup_upscaler == UpscalerOff) ||
         (v.upscaler == UpscalerTaa) != (v.startup_upscaler == UpscalerTaa));
}

namespace {

KeyList CurrentKeys() {
    const auto& v = Get();
    KeyList keys;
    const auto put = [&](const char* key, std::string value) { keys.emplace_back(key, std::move(value)); };
    const auto fixed = [](float value, int digits) {
        char text[32];
        std::snprintf(text, sizeof(text), "%.*f", digits, value);
        return std::string(text);
    };
    const auto flag = [](bool value) { return std::string(value ? "1" : "0"); };
    put("menu_language", MenuLanguageCodes[int(v.menu_language.load())]);
    put("upscaler", UpscalerName(v.upscaler));
    put("preset", std::to_string(v.preset.load()));
    put("frame_gen", FrameGenNames[std::clamp(v.frame_gen.load(), 0, FrameGenCount - 1)]);
    put("frame_gen_even_pacing", flag(v.frame_gen_cap));
    put("frame_gen_frames", std::to_string(std::clamp(v.frame_gen_frames.load(), 1, FrameGenFramesMax)));
    put("reflex", ReflexNames[std::clamp(v.reflex.load(), 0, ReflexCount - 1)]);
    put("anti_lag", flag(v.anti_lag));
    put("render_scale", fixed(v.render_scale, 2));
    put("mip_bias", fixed(v.mip_bias, 2));
    put("sharpen", flag(v.sharpen));
    put("sharpness", fixed(v.sharpness, 2));
    put("jitter", flag(v.jitter));
    put("reactive", flag(v.reactive));
    put("object_motion", flag(v.object_motion));
    put("reactive_scale", fixed(v.reactive_scale, 2));
    put("reactive_threshold", fixed(v.reactive_threshold, 2));
    put("reactive_max", fixed(v.reactive_max, 2));
    put("frame_gen_dynamic", flag(v.frame_gen_dynamic));
    put("frame_gen_target_fps", std::to_string(v.frame_gen_target_fps.load()));
    put("fsr3_velocity_factor", fixed(v.fsr3_velocity_factor, 3));
    put("fsr3_reactiveness_scale", fixed(v.fsr3_reactiveness_scale, 3));
    put("fsr3_shading_change_scale", fixed(v.fsr3_shading_change_scale, 3));
    put("fsr3_accumulation", fixed(v.fsr3_accumulation, 3));
    put("fsr3_min_disocclusion", fixed(v.fsr3_min_disocclusion, 3));
    put("show_fps", flag(v.show_fps));
    put("coop_status", flag(v.coop_status));
    put("coop_hud_ping", flag(v.coop_hud_ping));
    put("coop_notices", flag(v.coop_notices));
    put("coop_warp_requests", flag(v.coop_warp_requests));
    put("coop_open_room", flag(v.coop_open_room));
    put("voice_enabled", flag(v.voice_enabled));
    put("voice_push_to_talk", flag(v.voice_push_to_talk));
    put("voice_mic_muted", flag(v.voice_mic_muted));
    put("voice_talk_key", std::to_string(v.voice_talk_key.load()));
    put("voice_volume", fixed(v.voice_volume, 2));
    put("show_fps_rate", flag(v.show_fps_rate));
    put("show_fps_frame_time", flag(v.show_fps_frame_time));
    put("show_fps_upscaler", flag(v.show_fps_upscaler));
    put("show_fps_cpu", flag(v.show_fps_cpu));
    put("show_fps_gpu", flag(v.show_fps_gpu));
    put("compile_note", flag(v.show_compile_note));
    const auto colour = [](unsigned rgb) {
        char text[8];
        std::snprintf(text, sizeof(text), "#%06X", rgb & 0xFFFFFFu);
        return std::string(text);
    };
    put("fps_position", std::to_string(v.fps_position.load()));
    put("fps_scale", fixed(v.fps_scale, 2));
    put("fps_text_color", colour(v.fps_text_color));
    put("fps_bg_color", colour(v.fps_bg_color));
    put("fps_bg_opacity", fixed(v.fps_bg_opacity, 2));
    put("fps_text_opacity", fixed(v.fps_text_opacity, 2));
    put("fps_border", flag(v.fps_border));
    put("hide_cursor", flag(v.hide_cursor));
    put("fsr4_auto_exposure", flag(v.fsr4_auto_exposure));
    put("fsr4_invert_jitter", flag(v.fsr4_invert_jitter));
    put("dlss_preset", DlssPresetText(v.dlss_preset));
    put("dlss_mip_bias", flag(v.dlss_mip_bias));
    put("dlss_auto_exposure", flag(v.dlss_auto_exposure));
    put("dlss_jitter_fade", flag(v.dlss_jitter_fade));
    put("upscale_before_post", flag(v.upscale_before_post));
    put("optiscaler_compat", flag(v.optiscaler_compat));
    put("optiscaler_ngx_lock", flag(v.optiscaler_ngx_lock));
    put("optiscaler_clean_thread", flag(v.optiscaler_clean_thread));
    put("optiscaler_sync_submit", v.optiscaler_sync_submit == OptiSubmitAuto ? std::string("auto")
                                                                            : flag(v.optiscaler_sync_submit != 0));
    for (int e = 0; e < EffectCount; ++e) {
        put(Effects[e].key, flag(v.effects[e]));
    }
    put("fullscreen", flag(v.fullscreen));
    put("hdr", flag(v.hdr));
    put("hdr_paper_white", fixed(v.hdr_paper_white, 0));
    put("hdr_peak", fixed(v.hdr_peak, 0));
    put("hdr_peak_auto", flag(v.hdr_peak_auto));
    put("hdr_boost", fixed(v.hdr_boost, 2));
    put("hdr_ui_brightness", fixed(v.hdr_ui_brightness, 2));
    put("hdr_ui_color", fixed(v.hdr_ui_color, 2));
    put("hdr_mode", HdrModeNames[std::clamp(v.hdr_mode.load(), 0, HdrModeCount - 1)]);
    put("graphics_controls",flag(v.graphics_controls));
    put("graphics_vignette",flag(v.graphics_vignette));
    put("graphics_ao_strength",fixed(v.graphics_ao_strength,2));
    put("graphics_gi_strength",fixed(v.graphics_gi_strength,2));
    put("graphics_volumetric_intensity",fixed(v.graphics_volumetric_intensity,2));
    put("graphics_point_fog_intensity",fixed(v.graphics_point_fog_intensity,2));
    put("graphics_point_fog_glow_size",fixed(PointFogGlowSize(v.graphics_point_fog_glow_size),3));
    put("graphics_point_fog_falloff",std::to_string(std::clamp(v.graphics_point_fog_falloff.load(),0,1)));
    put("graphics_ssr_intensity",fixed(v.graphics_ssr_intensity,2));
    put("graphics_shadow_resolution",std::to_string(std::clamp(v.graphics_shadow_resolution.load(),0,2)));
    put("graphics_local_shadow_resolution",std::to_string(std::clamp(v.graphics_local_shadow_resolution.load(),0,2)));
    put("graphics_motion_blur_quality",std::to_string(std::clamp(v.graphics_motion_blur_quality.load(),0,1)));
    put("graphics_ao_quality",std::to_string(std::clamp(v.graphics_ao_quality.load(),0,1)));
    put("graphics_shadow_scale",fixed(v.graphics_shadow_scale,2));
    put("graphics_bloom",fixed(v.graphics_bloom,2));
    put("graphics_saturation",fixed(v.graphics_saturation,2));
    put("graphics_fog",fixed(v.graphics_fog,2));
    put("graphics_brightness",fixed(v.graphics_brightness,2));
    put("graphics_contrast",fixed(v.graphics_contrast,2));
    put("graphics_hue",fixed(v.graphics_hue,0));
    put("camera_controls", flag(v.camera_controls));
    put("widescreen",flag(v.widescreen));
    put("pc_controls",flag(v.pc_controls));
    put("mouse_camera",flag(v.mouse_camera));
    put("mouse_menu",flag(v.mouse_menu));
    put("menu_move",flag(v.menu_move));
    put("mouse_invert_x",flag(v.mouse_invert_x));
    put("mouse_invert_y",flag(v.mouse_invert_y));
    put("mouse_auto_rotation",flag(v.mouse_auto_rotation));
    put("pc_menus",flag(v.pc_menus));
    put("mouse_sensitivity",std::to_string(v.mouse_sensitivity.load()));
    put("button_prompts", ButtonPromptsNames[std::clamp(v.button_prompts.load(), 0, PromptsCount - 1)]);
    put("pad_icons", PadIconsNames[std::clamp(v.pad_icons.load(), 0, PadIconsCount - 1)]);
    put("change_appearance", flag(v.change_appearance));
    put("rebirth", flag(v.rebirth));
    put("camera_fov_scale", fixed(v.camera_fov_scale, 2));
    put("camera_distance_scale", fixed(v.camera_distance_scale, 2));
    put("camera_height_scale", fixed(v.camera_height_scale, 2));
    put("model_lod", std::to_string(v.model_lod.load()));
    put("cheat_vials", CheatText(v.cheat_vials));
    put("cheat_bullets", CheatText(v.cheat_bullets));
    put("cheat_stamina", flag(v.cheat_stamina));
    put("cheat_stealth", flag(v.cheat_stealth));
    put("cheat_gun_range", v.cheat_gun_range == 0 ? "off" : std::to_string(v.cheat_gun_range.load()));
    put("cheat_keep_echoes", flag(v.cheat_keep_echoes));
    put("cheat_echo_multiplier", std::to_string(v.cheat_echo_multiplier.load()));
    put("cheat_no_durability", flag(v.cheat_no_durability));
    put("cheat_consumables", flag(v.cheat_consumables));
    put("output_res", v.output_res == OutputAuto ? std::string("auto")
                                                  : std::to_string(OutputWidth(v.output_res)) + "x" +
                                                        std::to_string(OutputHeight(v.output_res)));
    put("live_resolution", v.live_resolution < 0 ? "auto" : flag(v.live_resolution != 0));
    if (v.menu_x >= 0.0f && v.menu_y >= 0.0f) {
        put("menu_pos", fixed(v.menu_x, 4) + "," + fixed(v.menu_y, 4));
    }

    BbInputConfig::Append(keys);
    return keys;
}

} // namespace

void Save() {
    KeyList keys = CurrentKeys();
    auto& queue = g_save_queue;
    std::call_once(queue.started, [] { std::thread(SaveWorker).detach(); });
    {
        const std::scoped_lock lock{queue.mutex};
        queue.keys = std::move(keys);
        queue.pending = true;
        queue.fresh = true;
    }
    queue.wake.notify_one();
}

SaveResult SaveNow() {
    return SaveKeys(CurrentKeys());
}

bool FlushSaves(std::chrono::milliseconds bound) {
    auto& queue = g_save_queue;
    std::unique_lock lock{queue.mutex};
    return queue.idle.wait_for(lock, bound, [&] { return !queue.pending && !queue.busy; });
}

bool FlushForExit() {
#ifdef LANTERN_MUTANT_NO_EXIT_FLUSH
    (void)ExitSaveBound;
    return true;
#else
    if (FlushSaves(ExitSaveBound)) {
        return true;
    }
    std::printf("Settings: lantern.ini save still pending after %lld ms at exit\n",
                static_cast<long long>(ExitSaveBound.count()));
    std::fflush(stdout);
    return false;
#endif
}

bool LastSaveFailed() {
    return g_settings_write_failed.load(std::memory_order_relaxed);
}

SaveFailures SaveFailureCounts() {
    return {g_settings_read_failures.load(std::memory_order_relaxed),
            g_settings_replace_failures.load(std::memory_order_relaxed),
            g_settings_lock_failures.load(std::memory_order_relaxed),
            g_settings_load_failures.load(std::memory_order_relaxed),
            g_settings_dropped.load(std::memory_order_relaxed)};
}

float PresetScale(int preset) {
    static constexpr float scales[PresetCustom] = {1.0f, 1.5f, 1.7f, 2.0f, 3.0f};
    if (preset == PresetCustom) {
        const auto& v = Get();
        const float scale = FixedRenderSession() ? v.startup_render_scale : v.render_scale.load();
        return std::clamp(std::isfinite(scale) ? scale : 1.0f, RenderScaleMin, RenderScaleMax);
    }
    return scales[std::clamp(preset, 0, PresetCustom - 1)];
}

const char* PresetName(int preset) {
    static constexpr const char* names[PresetCount] = {"Native AA", "Quality", "Balanced",
                                                       "Performance", "Ultra Performance", "Custom"};
    return names[std::clamp(preset, 0, PresetCount - 1)];
}

int ModelPreset(int preset) {
    if (preset != PresetCustom) {
        return std::clamp(preset, 0, PresetCustom - 1);
    }
    const float scale = PresetScale(preset);
    int nearest = NativeAA;
    for (int p = NativeAA; p < PresetCustom; ++p) {
        if (std::abs(PresetScale(p) - scale) < std::abs(PresetScale(nearest) - scale)) {
            nearest = p;
        }
    }
    return nearest;
}

bool DlssPresetValid(int preset) {
    return BbDlss::Offered(preset);
}

int DlssPresetFromText(const char* text) {
    if (!text || std::strcmp(text, "default") == 0) {
        return DlssPresetDefault;
    }
    if (std::strcmp(text, "auto") == 0) {
        return DlssPresetAuto;
    }
    const char letter = char(std::tolower(static_cast<unsigned char>(text[0])));
    if (letter >= 'a' && letter <= 'z' && text[1] == '\0') {
        const int preset = letter - 'a' + 1;
        if (BbDlss::Deprecated(preset)) {
            std::printf("Settings: DLSS preset '%s' is deprecated by NVIDIA; auto\n", text);
            return BbDlss::FromSaved(preset);
        }
        if (!DlssPresetValid(preset)) {
            std::printf("Settings: DLSS preset '%s' is not in NVIDIA's DLSS 310.9 (J, K, L, M); k\n", text);
            return DlssPresetK;
        }
        return preset;
    }
    std::printf("Settings: unknown dlss_preset '%s' (expected default or a letter); k\n", text);
    return DlssPresetK;
}

const char* DlssPresetText(int preset) {
    static constexpr const char* letters[DlssPresetMax] = {"a", "b", "c", "d", "e", "f", "g", "h", "i",
                                                           "j", "k", "l", "m", "n", "o", "p", "q", "r",
                                                           "s", "t", "u", "v", "w", "x", "y", "z"};
    if (preset == DlssPresetAuto) {
        return "auto";
    }
    return preset >= 1 && preset <= DlssPresetMax ? letters[preset - 1] : "default";
}

const char* UpscalerName(int upscaler) {
    static constexpr const char* names[UpscalerCount] = {"off", "fsr3", "fsr4", "fsr411", "taa",
                                                                  "dlss", "fsr", "xess"};
    return names[std::clamp(upscaler, 0, UpscalerCount - 1)];
}

}

extern "C" void bbport_settings_flush_for_exit(void) {
    BbSettings::FlushForExit();
}
