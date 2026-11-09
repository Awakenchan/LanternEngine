// SPDX-License-Identifier: GPL-2.0-or-later
#include "option_menu.h"
#include "option_keys.h"
#include "option_rows.h"
#include "engine_state.h"
#include "graphics.h"
#include "menu_memory.h"
#include "bbport_looks.h"
#include "bbport_settings.h"
#include <SDL3/SDL.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

extern "C" int sys_file_translate(const char*, char*, std::size_t);
extern "C" int sys_file_mount(const char*, const char*);
extern "C" void sys_file_unmount(const char*);

namespace BbEngine::Options {
namespace {
#define GUEST __attribute__((sysv_abi))
using OptionRows::Choice;

constexpr std::size_t kSectionNames = 0x53398b0, kSectionSlot = kSectionNames + 6 * 8;
constexpr std::size_t kSectionLoopEnd = 0x1b20e1e;
constexpr std::uint8_t kSectionLoopBytes[] = {0x48, 0x83, 0xfa, 0x06};
constexpr std::size_t kSystemCall = 0x1bb4978, kSystemFinish = 0x1bea630, kSystemPad = 0x1bb4c61;
constexpr std::size_t kTitleCall = 0x1b4adb3, kTitleFinish = 0x1bea820, kTitlePad = 0x1b49ac1;
constexpr std::uint8_t kPadding[] = {0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x2e, 0x0f, 0x1f, 0x84, 0, 0, 0, 0, 0};

constexpr std::uint32_t kEnhancementsRow = 110013, kGraphicsRow = 110007, kEffectsRow = 110010, kColourRow = 110014;
constexpr std::uint32_t kControlsRow = 110008, kKeysRow = 110009, kCameraRow = 110011, kQuitRow = 110015;
constexpr std::uint32_t kLightingRow = 110016, kDetailRow = 110017, kLooksRow = 110018;
constexpr std::uint32_t kTitleQuitRow = 122000, kQuitQuestion = 920001;

char g_section[32] = "PCEnhance";
bool g_installed = false;

enum class Kind { Toggle, Slider, Pick };
struct Setting {
    Kind kind;
    std::uint32_t id;
    std::int32_t def;
    float base = 0, step = 0;
    std::span<const Choice> choices = {};
    bool picture = false;
    std::atomic<bool>* flag = nullptr;
    std::atomic<float>* amount = nullptr;
    std::atomic<int>* number = nullptr;
    std::uint8_t byte = 0;
    std::int32_t word = 0;
    std::int32_t seen = 0;
};

std::int32_t Shown(const Setting& s) { return s.kind == Kind::Pick ? s.word : s.byte; }
std::int32_t Read(const Setting& s) {
    if (s.flag) return s.flag->load() ? 1 : 0;
    if (s.amount) return std::int32_t(std::clamp(std::lround((s.amount->load() - s.base) / s.step), 0l, 10l));
    const int v = s.number->load();
    return s.kind == Kind::Pick ? std::clamp(v, 0, int(s.choices.size()) - 1) : std::clamp(v, 0, 10);
}
void Write(Setting& s, std::int32_t v) {
    v = s.kind == Kind::Pick ? std::clamp(v, 0, int(s.choices.size()) - 1) : std::clamp(v, 0, s.kind == Kind::Toggle ? 1 : 10);
    s.seen = v;
    if (s.flag) *s.flag = v != 0;
    else if (s.amount) *s.amount = s.base + float(v) * s.step;
    else *s.number = v;
}

const Choice kOutputs[] = {{0, 117010},  {1, 117011},  {2, 117012},  {3, 117013},  {4, 117015},
                           {5, 117040},  {6, 117041},  {7, 117042},  {8, 117043},  {9, 117044},
                           {10, 117045}, {11, 117046}, {12, 117047}, {13, 117048}};
static_assert(std::size(kOutputs) == BbSettings::OutputCount);
const Choice kPrompts[] = {{BbSettings::PromptsAuto, 118010}, {BbSettings::PromptsKeyboard, 118011},
                           {BbSettings::PromptsController, 118012}};
static_assert(std::size(kPrompts) == BbSettings::PromptsCount);
const Choice kPadIcons[] = {{BbSettings::PadIconsAuto, 118013}, {BbSettings::PadIconsPlayStation, 118014},
                            {BbSettings::PadIconsXbox, 118015}};
static_assert(std::size(kPadIcons) == BbSettings::PadIconsCount);

const Choice kSunShadows[] = {{0, 127010}, {1, 127011}, {2, 127012}};
const Choice kLampShadows[] = {{0, 127010}, {1, 127013}, {2, 127014}};
const Choice kBlurQuality[] = {{0, 127010}, {1, 127013}};
const Choice kLookChoices[] = {{0, 128010}, {1, 128011}, {2, 128012}, {3, 128013}, {4, 128014}, {5, 128016}, {6, 128015}};
static_assert(std::size(kLookChoices) == BbLooks::PresetCount + 2);

Setting Toggle(std::uint32_t id, std::int32_t def, bool picture = false) { return {Kind::Toggle, id, def, 0, 0, {}, picture}; }
Setting Slider(std::uint32_t id, std::int32_t def, float base = 0, float step = 1, bool picture = false) {
    return {Kind::Slider, id, def, base, step, {}, picture};
}
Setting Pick(std::uint32_t id, std::int32_t def, std::span<const Choice> choices) { return {Kind::Pick, id, def, 0, 0, choices}; }

Setting g_enhancements[] = {Toggle(124001, 0), Toggle(124002, 0), Toggle(124004, 0)};
Setting g_graphics[] = {Toggle(117001, 0, true),           Toggle(117000, 1, true),
                        Slider(117007, 5, 0, 0.2f, true),  Slider(117006, 0, 1, 0.2f, true),
                        Pick(117004, BbSettings::OutputDefault, kOutputs), Slider(117008, 10, 0, 0.1f, true)};
Setting g_effects[] = {Toggle(117005, 0, true), Toggle(117002, 0, true), Toggle(117003, 0, true),
                       Toggle(120001, 1, true), Slider(120000, 10, 0, 0.1f, true)};
Setting g_colour[] = {Toggle(125004, 0),
                      Slider(125000, 5, 0.5f, 0.1f, true), Slider(125001, 5, 0.95f, 0.01f, true),
                      Slider(125002, 5, 0, 0.2f, true),    Slider(125003, 5, -20, 4, true)};
Setting g_controls[] = {Toggle(118000, 1), Slider(118001, 5), Toggle(118002, 0), Toggle(118003, 0),
                        Toggle(116002, 1), Toggle(118005, 0),
                        Pick(118006, BbSettings::PromptsAuto, kPrompts), Pick(118007, BbSettings::PadIconsAuto, kPadIcons)};
Setting g_camera[] = {Slider(118004, 0, 1, 0.05f), Slider(121000, 5, 0.5f, 0.1f), Slider(121001, 5, 0.5f, 0.1f)};
Setting g_lighting[] = {Toggle(126000, 0), Toggle(126001, 0), Toggle(126002, 0), Toggle(126003, 0),
                        Slider(126004, 5, 0, 1.2f), Toggle(126005, 0), Slider(126006, 5, 0, 0.2f),
                        Toggle(126007, 0)};
Setting g_detail[] = {Toggle(127000, 0), Slider(127001, 5, 0, 0.2f), Toggle(127002, 0), Toggle(127003, 0),
                      Slider(127004, 5, 0, 0.2f), Pick(127005, 0, kSunShadows), Pick(127006, 0, kLampShadows),
                      Pick(127007, 0, kBlurQuality)};
Setting g_looks[] = {Pick(128000, 0, kLookChoices)};
constexpr int kGraphicsPick = 4;

struct Section {
    const char* name;
    std::uint32_t row;
    std::span<Setting> rows;
    bool keep_defaults;
    bool shown = false;
};
Section g_sections[] = {
    {"PCEnhance", kEnhancementsRow, g_enhancements, true}, {"PCGraphics", kGraphicsRow, g_graphics, true},
    {"PCEffects", kEffectsRow, g_effects, true},           {"PCColour", kColourRow, g_colour, true},
    {"PCControls", kControlsRow, g_controls, false},       {"PCKeys", kKeysRow, {}, false},
    {"PCCamera", kCameraRow, g_camera, true},
    {"PCLighting", kLightingRow, g_lighting, true},        {"PCDetail", kDetailRow, g_detail, true},
    {"PCLooks", kLooksRow, g_looks, false},
};
enum { kEnhance, kGraphics, kEffects, kColour, kControls, kKeys, kCamera, kLighting, kDetail, kLooks, kSectionCount };
static_assert(std::size(g_sections) == kSectionCount);
bool g_offered[kSectionCount]{};

struct DefaultsCaption {
    std::uint8_t* dialog = nullptr;
    std::uint8_t* choices = nullptr;
    char16_t text[32]{};
    char16_t first = 0;
};
DefaultsCaption g_caption;

void KeepDefaultsCaption(std::uint8_t* dialog) {
    g_caption = DefaultsCaption{};
    const int rows = OptionRows::RowCount(dialog);
    if (rows != int(std::size(g_graphics)) + 1) return;
    const char16_t* label = OptionRows::RowLabel(dialog, rows - 1);
    if (!label || label != OptionRows::Message(OptionRows::kDefaultsRow)) return;
    std::size_t n = 0;
    for (; label[n] && n + 1 < std::size(g_caption.text); ++n) g_caption.text[n] = label[n];
    OptionRows::SetRowLabel(dialog, rows - 1, g_caption.text);
    OptionRows::Layout(dialog);
    g_caption.dialog = dialog;
    g_caption.choices = OptionRows::PickChoices(OptionRows::RowWidget(dialog, kGraphicsPick));
}

void UpdateDefaultsCaption(std::uint8_t* list, bool focused) {
    DefaultsCaption& c = g_caption;
    if (!c.dialog || !focused) return;
    if (list == c.choices && !c.first && c.text[0]) {
        c.first = c.text[0];
        c.text[0] = 0;
        OptionRows::Redraw(OptionRows::RowsList(c.dialog));
    } else if (list == OptionRows::RowsList(c.dialog) && c.first) {
        c.text[0] = c.first;
        c.first = 0;
        OptionRows::Redraw(list);
    }
}

void Build(int index, void* dialog) {
    Section& section = g_sections[index];
    for (Setting& s : section.rows) {
        const std::int32_t v = Read(s);
        s.seen = v;
        switch (s.kind) {
        case Kind::Toggle: s.byte = std::uint8_t(v); OptionRows::AddToggle(dialog, &s.byte, s.id, std::uint8_t(s.def)); break;
        case Kind::Slider: s.byte = std::uint8_t(v); OptionRows::AddSlider(dialog, &s.byte, s.id, std::uint8_t(s.def)); break;
        case Kind::Pick: s.word = v; OptionRows::AddPick(dialog, &s.word, s.id, s.choices, s.def); break;
        }
    }
    section.shown = true;
    const int took = OptionRows::RowCount(static_cast<std::uint8_t*>(dialog));
    host_log("pc-options: %s opened, %d rows%s", section.name, took,
             took == int(section.rows.size()) ? "" : " (not the rows built: they will not line up)");
}

template <int S> void GUEST Handler(void* dialog, void*) { Build(S, dialog); }
template <int S> std::int64_t GUEST Open(void* root, void* params) {
    Section& section = g_sections[S];
    std::snprintf(g_section, sizeof g_section, "%s", section.name);
    g_caption = DefaultsCaption{};
    const OptionRows::Handler handler = S == kKeys ? &OptionKeys::Build : &Handler<S>;
    const std::uint64_t dialog = OptionRows::OpenSection(root, params, g_section, handler);
    auto* d = reinterpret_cast<std::uint8_t*>(static_cast<std::uintptr_t>(dialog));
    if (d && !section.keep_defaults) {
        const int rows = OptionRows::RowCount(d);
        if (rows > 0 && OptionRows::RowLabel(d, rows - 1) == OptionRows::Message(OptionRows::kDefaultsRow))
            OptionRows::DropLastRow(d);
        else
            host_log("pc-options: %s: the last row is not Defaults; left as it is", section.name);
    } else if (d && S == kGraphics) {
        KeepDefaultsCaption(d);
    }
    return std::int64_t(dialog);
}
template <int S> void* GUEST RowOpener(void* out, void* ctx) { return OptionRows::OpenThrough(out, ctx, &Open<S>); }
constexpr OptionRows::Opener kOpeners[] = {&RowOpener<0>, &RowOpener<1>, &RowOpener<2>, &RowOpener<3>,
                                           &RowOpener<4>, &RowOpener<5>, &RowOpener<6>, &RowOpener<7>,
                                           &RowOpener<8>, &RowOpener<9>};
static_assert(std::size(kOpeners) == kSectionCount);
bool HandlersEven() {
    const OptionRows::Handler handlers[] = {&Handler<0>, &Handler<1>, &Handler<2>, &Handler<3>,
                                            &Handler<4>, &OptionKeys::Build, &Handler<6>,
                                            &Handler<7>, &Handler<8>, &Handler<9>};
    for (const auto h : handlers)
        if (reinterpret_cast<std::uintptr_t>(h) & 1) return false;
    return true;
}

void GUEST QuitYes(std::int64_t, std::int64_t) {
    host_log("pc-options: Quit to Desktop");
    SDL_Event quit{};
    quit.type = SDL_EVENT_QUIT;
    SDL_PushEvent(&quit);
}
void* GUEST QuitOpener(void* out, void* factory) { return OptionRows::AskYesNo(out, factory, kQuitQuestion, &QuitYes); }

std::uint64_t g_image = 0;
void* GUEST SystemList(void* out, void* builder, void* functor) {
    for (int s = 0; s < kSectionCount; ++s)
        if (g_offered[s]) OptionRows::AddCommand(builder, g_sections[s].row, kOpeners[s]);
    OptionRows::AddExitCommand(builder, kQuitRow, &QuitOpener);
    Call(g_image + kSystemFinish, out, builder, functor);
    return out;
}
void* GUEST TitleList(void* out, void* builder, void* sprite) {
    OptionRows::AddExitCommand(builder, kTitleQuitRow, &QuitOpener);
    Call(g_image + kTitleFinish, out, builder, sprite);
    return out;
}

bool CallSite(const std::uint8_t* image, std::size_t size, std::size_t call, std::size_t target, std::size_t pad) {
    if (call + 5 > size || pad + sizeof kPadding > size || image[call] != 0xe8) return false;
    std::int32_t rel = 0;
    std::memcpy(&rel, image + call + 1, 4);
    return std::int64_t(call + 5) + rel == std::int64_t(target) && !std::memcmp(image + pad, kPadding, sizeof kPadding);
}
void RouteCall(std::uint8_t* image, std::size_t call, std::size_t pad, const void* host) {
    std::uint8_t* p = image + pad;
    const auto to = reinterpret_cast<std::uint64_t>(host);
    *p++ = 0x49;
    *p++ = 0xbb;
    std::memcpy(p, &to, 8);
    p += 8;
    *p++ = 0x41;
    *p++ = 0xff;
    *p = 0xe3;
    const auto rel = std::int32_t(std::int64_t(pad) - std::int64_t(call + 5));
    std::memcpy(image + call + 1, &rel, 4);
}

std::vector<std::string> g_mounted;
bool MountAssets(const char* assets, const char* game) {
    namespace fs = std::filesystem;
    std::vector<std::pair<std::string, std::string>> files;
    try {
        const fs::path root(assets), dump(game);
        const char* merged_env = std::getenv("BB_GAME_MERGED");
        const fs::path merged = merged_env && *merged_env ? fs::path(merged_env) : dump;
        auto add = [&](const fs::path& relative) {
            const fs::path prepared = root / relative;
            const std::string guest = "/app0/" + relative.generic_string();
            char resolved[512]{};
            std::error_code e;
            if (!fs::is_regular_file(prepared) || sys_file_translate(guest.c_str(), resolved, sizeof resolved) ||
                !(fs::equivalent(resolved, merged / relative, e) || fs::equivalent(resolved, dump / relative, e))) {
                host_log("pc-options off: %s is not the file the PC menus were made from", relative.generic_string().c_str());
                return false;
            }
            files.emplace_back(guest, prepared.string());
            return true;
        };
        if (!add("dvdroot_ps4/menu/optionsetting.gfx")) return false;
        // Widescreen: the same movie with its full-screen dim widened for the UI viewport
        // (start/main.cpp, made from the prepared one).
        const char* wide = std::getenv("BB_WIDE_OPTION_MOVIE");
        std::error_code e;
        if (wide && *wide && fs::is_regular_file(wide, e)) {
            files.back().second = fs::path(wide).string();
            host_log("pc-options: optionsetting.gfx widened for the UI viewport");
        }
        for (const auto& language : fs::directory_iterator(dump / "dvdroot_ps4/msg")) {
            if (!language.is_directory()) continue;
            const fs::path relative = fs::path("dvdroot_ps4/msg") / language.path().filename() / "menu.msgbnd.dcx";
            if (fs::is_regular_file(dump / relative) && !add(relative)) return false;
        }
    } catch (const std::exception& e) {
        host_log("pc-options off: %s", e.what());
        return false;
    }
    if (files.size() < 2 || files.size() > 24) return false;
    for (const auto& [guest, host] : files) {
        if (sys_file_mount(guest.c_str(), host.c_str())) {
            for (const auto& done : g_mounted) sys_file_unmount(done.c_str());
            g_mounted.clear();
            return false;
        }
        g_mounted.push_back(guest);
    }
    return true;
}

int EffectIndex(const char* key) {
    for (int e = 0; e < BbSettings::EffectCount; ++e)
        if (!std::strcmp(BbSettings::Effects[e].key, key)) return e;
    return -1;
}

bool BindSettings() {
    auto& s = BbSettings::Get();
    const int aa = EffectIndex("effect_game_aa"), ao = EffectIndex("effect_ssao"), blur = EffectIndex("effect_motion_blur"),
              dof = EffectIndex("effect_dof"), ca = EffectIndex("effect_chromatic_aberration");
    if (aa < 0 || ao < 0 || blur < 0 || dof < 0 || ca < 0) return false;
    auto effect = [&](Setting& row, int e) {
        row.flag = &s.effects[e];
        row.def = BbSettings::Effects[e].default_on ? 1 : 0;
    };
    g_enhancements[0].flag = &s.change_appearance;
    g_enhancements[1].flag = &s.rebirth;
    g_enhancements[2].flag = &s.camera_controls;
    effect(g_graphics[0], aa);
    effect(g_graphics[1], ao);
    g_graphics[2].amount = &s.graphics_ao_strength;
    g_graphics[3].amount = &s.graphics_shadow_scale;
    g_graphics[4].number = &s.output_res;
    g_graphics[5].amount = &s.graphics_fog;
    effect(g_effects[0], blur);
    effect(g_effects[1], dof);
    effect(g_effects[2], ca);
    g_effects[3].flag = &s.graphics_vignette;
    g_effects[4].amount = &s.graphics_bloom;
    g_colour[0].flag = &s.graphics_controls;
    g_colour[1].amount = &s.graphics_brightness;
    g_colour[2].amount = &s.graphics_contrast;
    g_colour[3].amount = &s.graphics_saturation;
    g_colour[4].amount = &s.graphics_hue;
    g_controls[0].flag = &s.mouse_camera;
    g_controls[1].number = &s.mouse_sensitivity;
    g_controls[2].flag = &s.mouse_invert_x;
    g_controls[3].flag = &s.mouse_invert_y;
    g_controls[4].flag = &s.mouse_menu;
    g_controls[5].flag = &s.mouse_auto_rotation;
    g_controls[6].number = &s.button_prompts;
    g_controls[7].number = &s.pad_icons;
    g_camera[0].amount = &s.camera_fov_scale;
    g_camera[1].amount = &s.camera_distance_scale;
    g_camera[2].amount = &s.camera_height_scale;
    using namespace BbSettings;
    effect(g_lighting[0], EffectGtao);
    effect(g_lighting[1], EffectContactShadows);
    effect(g_lighting[2], EffectPcss);
    effect(g_lighting[3], EffectGi);
    g_lighting[4].amount = &s.graphics_gi_strength;
    effect(g_lighting[5], EffectVolumetricLight);
    g_lighting[6].amount = &s.graphics_volumetric_intensity;
    effect(g_lighting[7], EffectPointFog);
    effect(g_detail[0], EffectModernSsr);
    g_detail[1].amount = &s.graphics_ssr_intensity;
    effect(g_detail[2], EffectPomCavity);
    effect(g_detail[3], EffectPointShadows);
    g_detail[4].amount = &s.graphics_point_fog_intensity;
    g_detail[5].number = &s.graphics_shadow_resolution;
    g_detail[6].number = &s.graphics_local_shadow_resolution;
    g_detail[7].number = &s.graphics_motion_blur_quality;
    g_looks[0].number = &s.look_choice;
    return true;
}
}

bool Install(std::uint8_t* image, std::size_t size) {
    auto& settings = BbSettings::Get();
    if (!settings.pc_menus) {
        host_log("pc-options off: turned off in the launcher (Play, Lantern menus in the game)");
        return false;
    }
    if (!settings.camera_controls && !settings.change_appearance && !settings.rebirth && !settings.graphics_controls &&
        !settings.pc_controls)
        return false;
    const char* assets = std::getenv("BB_PC_MENU_ASSETS");
    const char* game = std::getenv("BB_GAME_DIR");
    if (!assets || !*assets || !game || !*game) return false;

    std::uint64_t slots[2]{};
    if (size < kSectionSlot + sizeof slots) return false;
    std::memcpy(slots, image + kSectionSlot, sizeof slots);
    const bool system = !slots[0] && !slots[1] && kSectionLoopEnd + 4 <= size &&
                        !std::memcmp(image + kSectionLoopEnd, kSectionLoopBytes, 4) &&
                        CallSite(image, size, kSystemCall, kSystemFinish, kSystemPad) && MenuMemory::Compatible(image, size) &&
                        HandlersEven();
    if (!system || !BindSettings()) {
        host_log("pc-options off: the System list's code is not 1.09's");
        return false;
    }
    if (!MountAssets(assets, game)) return false;
    if (!MenuMemory::Install(image, size)) {
        for (const auto& path : g_mounted) sys_file_unmount(path.c_str());
        g_mounted.clear();
        return false;
    }

    g_image = reinterpret_cast<std::uint64_t>(image);
    OptionRows::Bind(g_image);
    g_offered[kEnhance] = true;
    g_offered[kGraphics] = g_offered[kEffects] = g_offered[kColour] = Graphics::Enabled();
    g_offered[kLighting] = g_offered[kDetail] = g_offered[kLooks] = Graphics::Enabled();
    g_offered[kControls] = g_offered[kKeys] = settings.pc_controls;
    g_offered[kCamera] = settings.camera_controls;

    const auto name = reinterpret_cast<std::uint64_t>(g_section);
    std::memcpy(image + kSectionSlot, &name, sizeof name);
    image[kSectionLoopEnd + 3] = 7;
    RouteCall(image, kSystemCall, kSystemPad, reinterpret_cast<const void*>(&SystemList));
    if (CallSite(image, size, kTitleCall, kTitleFinish, kTitlePad))
        RouteCall(image, kTitleCall, kTitlePad, reinterpret_cast<const void*>(&TitleList));
    else
        host_log("pc-options: the title's Quit Game left out (its code is not 1.09's)");
    g_installed = true;
    std::string offered;
    for (int s = 0; s < kSectionCount; ++s)
        if (g_offered[s]) offered += std::string(offered.empty() ? "" : ", ") + g_sections[s].name;
    host_log("pc-options: native System pages installed (%zu prepared files): %s", g_mounted.size(), offered.c_str());
    return true;
}

bool ClickDecides(std::uint8_t* list, int index) { return g_installed && OptionKeys::ClickDecides(list, index); }

void ListUpdate(std::uint8_t* list, bool focused) {
    if (!g_installed) return;
    UpdateDefaultsCaption(list, focused);
    OptionKeys::Update(list, focused);
}

void Tick() {
    if (!g_installed) return;
    bool changed = false, picture = false;
    for (Section& section : g_sections) {
        if (!section.shown) continue;
        for (Setting& s : section.rows) {
            const std::int32_t v = Shown(s);
            if (v == s.seen) continue;
            Write(s, v);
            changed = true;
            picture |= s.picture;
            if (&s == &g_looks[0]) {
                if (v >= 1 && v <= BbLooks::PresetCount) {
                    BbLooks::Apply(v - 1);
                } else if (v == BbLooks::PresetCount + 1) {
                    BbLooks::GoBack();
                }
            }
        }
    }
    auto& settings = BbSettings::Get();
    if (picture && !settings.graphics_controls) {
        settings.graphics_controls = true;
        g_colour[0].byte = 1;
        g_colour[0].seen = 1;
    }
    if (changed) {
        BbSettings::Save();
        host_log("pc-options: a System option changed; saved to lantern.ini");
    }
}
}
