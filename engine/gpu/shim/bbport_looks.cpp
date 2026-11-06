// SPDX-License-Identifier: GPL-3.0-or-later
#include "bbport_looks.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "bbport_settings.h"

namespace BbLooks {
namespace {
struct Knob {
    const char* key;
    const char* def;
};
constexpr Knob kKnobs[] = {
    {"graphics_controls", "0"},
    {"graphics_vignette", "1"},
    {"graphics_brightness", "1"},
    {"graphics_contrast", "1"},
    {"graphics_saturation", "1"},
    {"graphics_hue", "0"},
    {"graphics_bloom", "1"},
    {"graphics_fog", "1"},
    {"graphics_ao_strength", "1"},
    {"graphics_shadow_scale", "1"},
    {"effect_chromatic_aberration", "0"},
    {"effect_dof", "0"},
    {"effect_motion_blur", "0"},
    {"effect_ssao", "1"},
    {"effect_game_aa", "0"},
    {"effect_dynamic_shadows", "1"},
    {"effect_ssr", "0"},
    {"effect_gtao", "0"},
    {"effect_modern_ssr", "0"},
    {"effect_pom_cavity", "0"},
    {"effect_contact_shadows", "0"},
    {"effect_pcss", "0"},
    {"effect_gi", "0"},
    {"effect_volumetric_light", "0"},
    {"effect_point_fog", "0"},
    {"effect_point_shadows", "0"},
    {"effect_ray_tracing", "0"},
    {"graphics_gi_strength", "6"},
    {"graphics_ssr_intensity", "1"},
    {"graphics_volumetric_intensity", "1"},
    {"graphics_point_fog_intensity", "1"},
    {"graphics_point_fog_glow_size", "0.08"},
    {"graphics_shadow_resolution", "0"},
    {"graphics_local_shadow_resolution", "0"},
    {"graphics_motion_blur_quality", "0"},
};

struct Value {
    const char* key;
    const char* value;
};
constexpr Value kPerformance[] = {{"graphics_controls", "1"},
                                  {"graphics_shadow_scale", "0.75"},
                                  {"effect_dynamic_shadows", "0"},
                                  {"effect_motion_blur", "0"},
                                  {"effect_ssr", "0"}};
constexpr Value kBetterShadows[] = {
    {"effect_contact_shadows", "1"}, {"effect_pcss", "1"}};
constexpr Value kRemastered[] = {{"effect_ssr", "1"},
                                 {"effect_modern_ssr", "1"},
                                 {"effect_pom_cavity", "1"},
                                 {"effect_contact_shadows", "1"},
                                 {"effect_pcss", "1"},
                                 {"effect_gi", "1"},
                                 {"effect_volumetric_light", "1"},
                                 {"effect_point_fog", "1"},
                                 {"graphics_shadow_resolution", "1"},
                                 {"graphics_local_shadow_resolution", "1"}};

struct Preset {
    const char* name;
    const char* name_ru;
    const Value* values;
    std::size_t count;
};
constexpr Preset kPresets[PresetCount] = {
    {"Original look", "Исходный вид", nullptr, 0},
    {"Performance", "Производительность", kPerformance, std::size(kPerformance)},
    {"Better shadows", "Лучшие тени", kBetterShadows, std::size(kBetterShadows)},
    {"BB Remastered", "BB Remastered", kRemastered, std::size(kRemastered)},
};

std::filesystem::path PreviousFile() {
    const char* env = std::getenv("BB_CONFIG");
    const std::filesystem::path ini = env && env[0] ? env : "lantern.ini";
    return ini.parent_path() / "looks-previous.json";
}

void KeepPrevious() {
    const auto file = PreviousFile();
    std::error_code error;
    if (std::filesystem::exists(file, error)) {
        return;
    }
    BbSettings::Save();
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out << "{\"values\":{";
    bool first = true;
    for (const Knob& knob : kKnobs) {
        out << (first ? "" : ",") << '"' << knob.key << "\":\"" << BbSettings::SavedValue(knob.key, knob.def) << '"';
        first = false;
    }
    out << "}}\n";
}

std::map<std::string, std::string> ReadPrevious() {
    std::ifstream in(PreviousFile(), std::ios::binary);
    std::stringstream text;
    text << in.rdbuf();
    const std::string json = text.str();
    std::map<std::string, std::string> values;
    for (const Knob& knob : kKnobs) {
        const std::string name = std::string("\"") + knob.key + "\":";
        const auto at = json.find(name);
        if (at == std::string::npos) {
            continue;
        }
        const auto open = at + name.size();
        if (open < json.size() && json[open] == '"') {
            const auto close = json.find('"', open + 1);
            if (close != std::string::npos && close - open <= 32) {
                values[knob.key] = json.substr(open + 1, close - open - 1);
            }
        }
    }
    return values;
}
}

const char* PresetName(int preset) {
    return kPresets[std::clamp(preset, 0, PresetCount - 1)].name;
}

const char* PresetNameRu(int preset) {
    return kPresets[std::clamp(preset, 0, PresetCount - 1)].name_ru;
}

void Apply(int preset) {
    if (preset < 0 || preset >= PresetCount) {
        return;
    }
    KeepPrevious();
    const Preset& look = kPresets[preset];
    for (const Knob& knob : kKnobs) {
        const char* value = knob.def;
        for (std::size_t i = 0; i < look.count; ++i) {
            if (!std::strcmp(look.values[i].key, knob.key)) {
                value = look.values[i].value;
            }
        }
        BbSettings::SetValue(knob.key, value);
    }
    BbSettings::Save();
    std::printf("Looks: %s in use%s\n", look.name, NeedsRestart() ? "; some of it applies at the next start" : "");
}

bool GoBack() {
    if (!HasPrevious()) {
        return false;
    }
    const auto values = ReadPrevious();
    for (const Knob& knob : kKnobs) {
        const auto it = values.find(knob.key);
        BbSettings::SetValue(knob.key, it != values.end() ? it->second : knob.def);
    }
    BbSettings::Save();
    std::error_code error;
    std::filesystem::remove(PreviousFile(), error);
    std::printf("Looks: back to the previous values\n");
    return true;
}

bool HasPrevious() {
    std::error_code error;
    return std::filesystem::exists(PreviousFile(), error);
}

bool NeedsRestart() {
    const auto& s = BbSettings::Get();
    for (int e = 0; e < BbSettings::EffectCount; ++e) {
        if (!BbSettings::EffectIsLive(e) && s.effects[e] != s.startup_effects[e]) {
            return true;
        }
    }
    return s.graphics_motion_blur_quality != s.startup_motion_blur_quality ||
           s.graphics_shadow_resolution != s.startup_shadow_resolution ||
           s.graphics_local_shadow_resolution != s.startup_local_shadow_resolution;
}
}
