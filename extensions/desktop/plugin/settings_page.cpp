#include "../glass_control_labels.h"
#include "plugin/settings_page.h"

#include "ipc/desktop_ipc.h"
#include "../../../src/common/localization.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <list>
#include <limits>

namespace pulse::desktop::plugin {
namespace {

constexpr const char* kSettingsKey = "desktop.preferences";

// Persistence schema 1: values follows this immutable order (booleans are 0/1);
// optics follows kGlassOpticalFields, with blur_edge in values. Never reorder either
// array without a schema bump. IPC retains its independent, named-field format.
struct PersistedField { std::string_view key; bool boolean; };
constexpr PersistedField kPersistedFields[] = {
    {"enabled",true},{"hybrid_menu",true},{"organize",true},{"tiles",true},
    {"live_tiles",true},{"type_search",true},{"sync_tray",true},{"dbl_hide",true},
    {"health_check",true},{"material",false},{"icon_size",false},{"scene",false},
    {"label_style",false},{"opacity",false},{"glass_preset",false},
    {"glass_thickness",false},{"glass_ior",false},{"glass_disp_gain",false},
    {"glass_glare",false},{"glass_angle",false},{"glass_frost",false},
    {"glass_rate",false},{"auto_accent",true},{"accent",false},
    {"icons_hidden",true},{"auto_sleep",true},{"restore_on_exit",true},
    {"glass_reference",true},{"glass_blur_radius",false},{"glass_blur_edge",true},
    {"glass2_blur_edge",true}
};

bool ReadPersistedArray(std::string_view json, std::string_view key,
                        size_t count, std::vector<std::string>& values) {
    const auto pos = json.find("\"" + std::string(key) + "\"");
    if (pos == std::string_view::npos) return false;
    size_t cursor = pos + key.size() + 2;
    const auto skip_space = [&] {
        while (cursor < json.size() && (json[cursor] == ' ' || json[cursor] == '\t' ||
               json[cursor] == '\r' || json[cursor] == '\n')) ++cursor;
    };
    skip_space();
    if (cursor == json.size() || json[cursor++] != ':') return false;
    skip_space();
    if (cursor == json.size() || json[cursor++] != '[') return false;
    for (size_t i = 0; i < count; ++i) {
        skip_space();
        const size_t start = cursor;
        double number = 0;
        const auto parsed = std::from_chars(json.data() + cursor, json.data() + json.size(), number);
        if (parsed.ec != std::errc{} || !std::isfinite(number)) return false;
        cursor = static_cast<size_t>(parsed.ptr - json.data());
        values.emplace_back(json.substr(start, cursor - start));
        skip_space();
        if (cursor == json.size() || json[cursor++] != (i + 1 == count ? ']' : ',')) return false;
    }
    return true;
}

bool ExpandPersistedJson(std::string_view json, std::string& expanded) {
    std::vector<std::string> values, optics;
    if (!ReadPersistedArray(json, "values", std::size(kPersistedFields), values) ||
        !ReadPersistedArray(json, "optics", kGlassOpticalFields.size(), optics)) return false;
    expanded = "{\"glass_schema\":2";
    for (size_t i = 0; i < values.size(); ++i) {
        if (kPersistedFields[i].boolean && values[i] != "0" && values[i] != "1") return false;
        int64_t integer = 0;
        const auto parsed = std::from_chars(values[i].data(), values[i].data() + values[i].size(), integer);
        if (parsed.ec != std::errc{} || parsed.ptr != values[i].data() + values[i].size()) return false;
        expanded += ",\"" + std::string(kPersistedFields[i].key) + "\":" +
            (kPersistedFields[i].boolean ? (values[i] == "1" ? "true" : "false") : values[i]);
    }
    for (size_t i = 0; i < optics.size(); ++i)
        expanded += ",\"" + std::string(kGlassOpticalFields[i].key) + "\":" + optics[i];
    std::wstring city;
    if (ipc::JsonFindWide(json, "weather_city", city))
        expanded += ",\"weather_city\":" + ipc::JsonQuoteWide(city.substr(0, 40));
    expanded += "}";
    return true;
}

PulseStringView Sv(std::string_view s) noexcept {
    return PulseStringView{s.empty() ? "" : s.data(), static_cast<uint32_t>(s.size())};
}

std::string CopySv(PulseStringView v) {
    return (v.data && v.length > 0) ? std::string(v.data, v.length) : std::string{};
}

// Self-contained arena builder over C ABI PulseUiModelNode (zero dependency on client.hpp/model_builder.hpp).
struct ModelArena {
    std::list<std::string> strings;
    std::list<std::vector<PulseStringView>> string_slices;
    std::list<std::vector<PulseUiModelNode>> node_slices;
    std::vector<PulseUiState> states;
    std::vector<PulseStringView> commands;

    PulseStringView Intern(std::string_view text) {
        strings.emplace_back(text);
        return Sv(strings.back());
    }

    PulseUiModelString OptStr(std::string_view text) {
        return PulseUiModelString{1u, Intern(text)};
    }

    PulseUiModelBool OptBool(bool val) noexcept {
        return PulseUiModelBool{1u, val ? 1u : 0u};
    }

    PulseUiModelInt OptInt(int64_t val) noexcept {
        return PulseUiModelInt{1u, val};
    }

    PulseUiModelStrings OptOptions(std::initializer_list<std::string_view> items) {
        string_slices.emplace_back();
        auto& vec = string_slices.back();
        vec.reserve(items.size());
        for (auto it : items) {
            vec.push_back(Intern(it));
        }
        return PulseUiModelStrings{1u, vec.data(), static_cast<uint32_t>(vec.size())};
    }

    PulseUiModelNodes MakeChildren(std::initializer_list<PulseUiModelNode> kids) {
        node_slices.emplace_back(kids);
        auto& vec = node_slices.back();
        return PulseUiModelNodes{1u, vec.data(), static_cast<uint32_t>(vec.size())};
    }

    PulseUiModelNode MakeNode(uint32_t type, std::string_view id) {
        PulseUiModelNode n{};
        n.size = sizeof(PulseUiModelNode);
        n.type = type;
        n.id = Intern(id);
        return n;
    }

    void AddState(std::string_view key, std::string_view value) {
        states.push_back(PulseUiState{Intern(key), Intern(value)});
    }

    void AddCommand(std::string_view cmd) {
        commands.push_back(Intern(cmd));
    }
};

constexpr uint8_t kThickStops[5] = {1, 20, 40, 60, 80};
constexpr uint8_t kFrostStops[5] = {0, 15, 35, 60, 90};



uint8_t MapLegacyDispersionToGain(uint64_t legacy) noexcept {
    if (legacy == 0) return 0;
    if (legacy == 1) return 9;
    return 20;
}

uint8_t MapGainToLegacyDispersion(uint8_t gain) noexcept {
    if (gain == 0) return 0;
    if (gain <= 12) return 1;
    return 2;
}

} // namespace

void DesktopPrefs::ApplyGlassPreset(uint8_t preset) noexcept {
    glass_preset = preset <= 5 ? preset : 4u;
    if (preset < 4) glass_reference = false;
    switch (glass_preset) {
    case 5:
        glass_options = {};
        glass_reference = true; glass_thickness = 20; glass_ior_x100 = 140;
        glass_dispersion = 7; glass_glare = 90; glass_glare_angle = -45; glass_frost = 4;
        glass_blur_radius = 1; glass_blur_edge = true;
        break;
    case 0:
        glass_thickness = 28; glass_ior_x100 = 152; glass_dispersion = 9;
        glass_glare = 92; glass_glare_angle = -45; glass_frost = 4;
        break;
    case 1:
        glass_thickness = 24; glass_ior_x100 = 146; glass_dispersion = 7;
        glass_glare = 84; glass_glare_angle = -45; glass_frost = 24;
        break;
    case 2:
        glass_thickness = 44; glass_ior_x100 = 188; glass_dispersion = 22;
        glass_glare = 108; glass_glare_angle = -38; glass_frost = 0;
        break;
    case 3:
        glass_thickness = 20; glass_ior_x100 = 136; glass_dispersion = 4;
        glass_glare = 72; glass_glare_angle = -45; glass_frost = 58;
        break;
    default:
        break;
    }
    if (preset < 4) glass_options = LegacyGlassOptions(glass_thickness,glass_ior_x100,glass_dispersion,glass_glare,glass_glare_angle);
}

std::string DesktopPrefs::ToJson() const {
    char buf[1024]{};
    const uint8_t safe_mat = (material <= 5) ? material : 1u;
    std::snprintf(
        buf, sizeof(buf),
        "{\"enabled\":%s,\"hybrid_menu\":%s,\"organize\":%s,\"tiles\":%s,"
        "\"live_tiles\":%s,\"type_search\":%s,\"sync_tray\":%s,"
        "\"dbl_hide\":%s,\"health_check\":%s,\"material\":%u,\"icon_size\":%u,"
        "\"scene\":%u,\"label_style\":%u,\"opacity\":%u,"
        "\"glass_preset\":%u,\"glass_thickness\":%u,\"glass_ior\":%u,"
        "\"glass_disp_gain\":%u,\"glass_glare\":%u,\"glass_angle\":%d,\"glass_frost\":%u,"
        "\"glass_disp\":%u,\"glass_rate\":%u,"
        "\"auto_accent\":%s,\"accent\":%u,\"icons_hidden\":%s,\"auto_sleep\":%s,\"restore_on_exit\":%s}",
        enabled ? "true" : "false",
        hybrid_menu ? "true" : "false",
        organize ? "true" : "false",
        tiles ? "true" : "false",
        live_tiles ? "true" : "false",
        type_to_search ? "true" : "false",
        sync_tray ? "true" : "false",
        double_click_hide ? "true" : "false",
        health_check ? "true" : "false",
        static_cast<unsigned>(safe_mat),
        static_cast<unsigned>(std::min<uint8_t>(icon_size, 2)),
        static_cast<unsigned>(std::min<uint8_t>(scene, 2)),
        static_cast<unsigned>(std::min<uint8_t>(label_style, 3)),
        static_cast<unsigned>(std::min<uint8_t>(opacity_percent, 100)),
        static_cast<unsigned>(std::min<uint8_t>(glass_preset, 5)),
        static_cast<unsigned>(std::clamp<uint8_t>(glass_thickness, 1, 80)),
        static_cast<unsigned>(std::clamp<uint16_t>(glass_ior_x100, 100, 400)),
        static_cast<unsigned>(std::min<uint8_t>(glass_dispersion, 50)),
        static_cast<unsigned>(std::min<uint8_t>(glass_glare, 120)),
        static_cast<int>(std::clamp<int16_t>(glass_glare_angle, -180, 180)),
        static_cast<unsigned>(std::min<uint8_t>(glass_frost, 100)),
        static_cast<unsigned>(MapGainToLegacyDispersion(glass_dispersion)),
        static_cast<unsigned>(std::min<uint8_t>(glass_live_rate, 2)),
        auto_wallpaper_accent ? "true" : "false",
        accent_argb,
        icons_hidden ? "true" : "false",
        auto_sleep ? "true" : "false",
        restore_on_exit ? "true" : "false");
    std::string out = buf;
    out.pop_back();
    out += GlassOpticalJson(glass_options) + ",\"glass_preview\":" + (glass_preview ? "true" : "false") + "}";
    if (!out.empty() && out.back() == '}') {
        out.pop_back();
        out += ",\"glass_reference\":" + std::string(glass_reference ? "true" : "false") +
            ",\"glass_blur_radius\":" + std::to_string(glass_blur_radius) +
            ",\"glass_blur_edge\":" + (glass_blur_edge ? "true" : "false") + "}";
    }
    if (!out.empty() && out.back() == '}') {
        out.pop_back();
        out += ",\"weather_city\":" + ipc::JsonQuoteWide(weather_city) + "}";
    }
    return out;
}

std::string DesktopPrefs::ToPersistedJson() const {
    const std::string named = ToJson();
    std::string out = "{\"prefs_schema\":1,\"values\":[";
    for (size_t i = 0; i < std::size(kPersistedFields); ++i) {
        if (i) out += ',';
        const auto& field = kPersistedFields[i];
        if (field.boolean) {
            bool value = false;
            ipc::JsonFindBool(named, field.key, value);
            out += value ? '1' : '0';
        } else {
            int64_t value = 0;
            ipc::JsonFindInt64(named, field.key, value);
            out += std::to_string(value);
        }
    }
    out += "],\"optics\":[";
    for (size_t i = 0; i < kGlassOpticalFields.size(); ++i) {
        if (i) out += ',';
        const auto& field = kGlassOpticalFields[i];
        char number[32]{};
        const auto result = std::to_chars(number, number + sizeof(number),
            ClampGlassValue(field, glass_options.*(field.member)),
            std::chars_format::general, std::numeric_limits<double>::max_digits10);
        out.append(number, result.ptr);
    }
    // 31 bounded integral values + 17 doubles (at most 24 bytes each) + at
    // most 240 escaped city bytes keeps this schema below the SDK's 1024 bytes.
    out += "],\"weather_city\":" + ipc::JsonQuoteWide(weather_city.substr(0, 40)) + "}";
    return out;
}

void DesktopPrefs::ApplyJson(std::string_view json) {
    uint64_t schema = 0;
    std::string expanded;
    if (ipc::JsonFindUint64(json, "prefs_schema", schema)) {
        if (schema != 1 || !ExpandPersistedJson(json, expanded)) return;
        json = expanded;
    }
    bool b = false;
    uint64_t u = 0;
    if (ipc::JsonFindBool(json, "enabled", b)) enabled = b;
    if (ipc::JsonFindBool(json, "hybrid_menu", b) || ipc::JsonFindBool(json, "hybrid_context_menu", b)) {
        hybrid_menu = b;
    }
    if (ipc::JsonFindBool(json, "organize", b) || ipc::JsonFindBool(json, "organize_enabled", b)) {
        organize = b;
    }
    if (ipc::JsonFindBool(json, "tiles", b) || ipc::JsonFindBool(json, "tiles_enabled", b)) {
        tiles = b;
    }
    if (ipc::JsonFindBool(json, "live_tiles", b) || ipc::JsonFindBool(json, "live_tiles_enabled", b)) {
        live_tiles = b;
    }
    if (ipc::JsonFindBool(json, "type_search", b) || ipc::JsonFindBool(json, "type_to_search", b)) {
        type_to_search = b;
    }
    if (ipc::JsonFindBool(json, "sync_tray", b) || ipc::JsonFindBool(json, "sync_staging_tray", b)) {
        sync_tray = b;
    }
    if (ipc::JsonFindBool(json, "dbl_hide", b) || ipc::JsonFindBool(json, "double_click_hide_icons", b)) {
        double_click_hide = b;
    }
    if (ipc::JsonFindBool(json, "health_check", b) || ipc::JsonFindBool(json, "health_check_enabled", b)) {
        health_check = b;
    }
    if (ipc::JsonFindUint64(json, "material", u) || ipc::JsonFindUint64(json, "default_material", u)) {
        // Unknown future material values fall back to Acrylic (1) rather than clamping to Solid ("无").
        material = (u <= 5) ? static_cast<uint8_t>(u) : 1u;
    }
    if (ipc::JsonFindUint64(json, "glass_schema", u) && u >= 2) {
        for (const auto& field : kGlassOpticalFields) {
            double value=0;
            if (ipc::JsonFindDouble(json,field.key,value)) glass_options.*(field.member)=ClampGlassValue(field,value);
        }
        if (ipc::JsonFindBool(json,"glass2_blur_edge",b)) glass_options.blur_edge=b;
    }
    if (ipc::JsonFindBool(json, "glass_reference", b)) glass_reference = b;
    else if (ipc::JsonFindUint64(json, "glass_preset", u)) glass_reference = (u == 5);
    if (ipc::JsonFindUint64(json, "glass_blur_radius", u)) glass_blur_radius = static_cast<uint8_t>(std::clamp<uint64_t>(u, 1, 200));
    if (ipc::JsonFindBool(json, "glass_blur_edge", b)) glass_blur_edge = b;
    if (ipc::JsonFindUint64(json, "glass_preset", u)) {
        glass_preset = static_cast<uint8_t>(std::min<uint64_t>(u, 5));
    }
    if (ipc::JsonFindUint64(json, "glass_thickness", u)) {
        glass_thickness = static_cast<uint8_t>(std::clamp<uint64_t>(u, 1, 80));
    }
    if (ipc::JsonFindUint64(json, "glass_ior", u)) {
        glass_ior_x100 = static_cast<uint16_t>(std::clamp<uint64_t>(u, 100, 400));
    }
    if (ipc::JsonFindUint64(json, "glass_disp_gain", u)) {
        glass_dispersion = static_cast<uint8_t>(std::min<uint64_t>(u, 50));
    } else if (ipc::JsonFindUint64(json, "glass_disp", u) || ipc::JsonFindUint64(json, "glass_dispersion", u)) {
        glass_dispersion = MapLegacyDispersionToGain(u);
    }
    if (ipc::JsonFindUint64(json, "glass_glare", u)) {
        glass_glare = static_cast<uint8_t>(std::min<uint64_t>(u, 120));
    }
    if (int64_t s = 0; ipc::JsonFindInt64(json, "glass_angle", s)) {
        glass_glare_angle = static_cast<int16_t>(std::clamp<int64_t>(s, -180, 180));
    }
    if (ipc::JsonFindUint64(json, "glass_frost", u)) {
        glass_frost = static_cast<uint8_t>(std::min<uint64_t>(u, 100));
    }
    uint64_t glass_schema = 0;
    if ((!ipc::JsonFindUint64(json,"glass_schema",glass_schema) || glass_schema < 2) &&
        (ipc::JsonFindUint64(json,"glass_preset",u) || ipc::JsonFindUint64(json,"glass_thickness",u)))
        glass_options = LegacyGlassOptions(glass_thickness,glass_ior_x100,glass_dispersion,glass_glare,glass_glare_angle);
    if (ipc::JsonFindUint64(json, "glass_rate", u) || ipc::JsonFindUint64(json, "glass_live_rate", u)) {
        glass_live_rate = static_cast<uint8_t>(std::min<uint64_t>(u, 2));
    }
    if (ipc::JsonFindUint64(json, "icon_size", u)) {
        icon_size = static_cast<uint8_t>(std::min<uint64_t>(u, 2));
    }
    if (ipc::JsonFindUint64(json, "scene", u) || ipc::JsonFindUint64(json, "active_scene", u)) {
        scene = static_cast<uint8_t>(std::min<uint64_t>(u, 2));
    }
    if (ipc::JsonFindUint64(json, "label_style", u)) {
        label_style = static_cast<uint8_t>(std::min<uint64_t>(u, 3));
    }
    if (ipc::JsonFindUint64(json, "opacity", u) || ipc::JsonFindUint64(json, "card_opacity_percent", u)) {
        opacity_percent = static_cast<uint8_t>(std::min<uint64_t>(u, 100));
    }
    if (ipc::JsonFindBool(json, "auto_accent", b) || ipc::JsonFindBool(json, "auto_wallpaper_accent", b)) {
        auto_wallpaper_accent = b;
    }
    if (ipc::JsonFindUint64(json, "accent", u) || ipc::JsonFindUint64(json, "accent_argb", u)) {
        accent_argb = static_cast<uint32_t>(u);
    }
    if (ipc::JsonFindBool(json, "icons_hidden", b)) {
        icons_hidden = b;
    }
    if (std::wstring city; ipc::JsonFindWide(json, "weather_city", city)) {
        weather_city = city.substr(0, 40);
    }
    if (ipc::JsonFindBool(json, "auto_sleep", b) ||
        ipc::JsonFindBool(json, "pause_on_fullscreen_or_battery", b)) {
        auto_sleep = b;
    }
    if (ipc::JsonFindBool(json, "restore_on_exit", b) || ipc::JsonFindBool(json, "restore", b)) {
        restore_on_exit = b;
    }
}

DesktopPrefs DesktopPrefs::FromJson(std::string_view json) {
    DesktopPrefs p{};
    p.ApplyJson(json);
    return p;
}

DesktopSettingsPage::~DesktopSettingsPage() {
    Close();
}

PulseStatus DesktopSettingsPage::Open(const PulseHostV1* host,
                                      ChangedCallback on_changed,
                                      CommandCallback on_command,
                                      bool ai_organize_enabled) {
    Close();
    if (!host || !host->query_interface) return PULSE_ERROR_ARGUMENT;
    HMODULE strings_module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                      reinterpret_cast<LPCWSTR>(&Sv), &strings_module);
    l10n::Initialize(strings_module, L"system");
    on_changed_ = std::move(on_changed);
    on_command_ = std::move(on_command);
    ai_organize_enabled_ = ai_organize_enabled;

    if (host->query_interface(host->context, Sv(PULSE_RESOURCES_INTERFACE_NAME),
                              PULSE_RESOURCES_VERSION_1, &resources_, sizeof(resources_)) == PULSE_OK &&
        resources_.settings_get && resources_.settings_set) {
        has_resources_ = true;
    }

    if (host->query_interface(host->context, Sv(PULSE_SETTINGS_PAGES_INTERFACE_NAME),
                              PULSE_SETTINGS_PAGES_VERSION_1, &pages_, sizeof(pages_)) != PULSE_OK ||
        !pages_.create) {
        // Headless host without settings UI: still load persisted settings via PulseResourcesV1.
        if (has_resources_) LoadPersisted();
        return PULSE_OK;
    }
    has_pages_ = true;

    ModelArena a;
    auto make_section = [&](std::string_view id, std::string_view text) {
        auto n = a.MakeNode(PULSE_UI_MODEL_TEXT, id);
        n.text = a.OptStr(text);
        n.role = a.OptStr("section");
        return n;
    };
    auto make_caption = [&](std::string_view id, std::string_view text) {
        auto n = a.MakeNode(PULSE_UI_MODEL_TEXT, id);
        n.text = a.OptStr(text);
        n.role = a.OptStr("caption");
        return n;
    };
    auto make_row = [&](std::string id, std::string_view title, std::string_view desc, PulseUiModelNode ctrl) {
        auto t_node = a.MakeNode(PULSE_UI_MODEL_TEXT, id + "-title");
        t_node.text = a.OptStr(title);
        auto d_node = a.MakeNode(PULSE_UI_MODEL_TEXT, id + "-desc");
        d_node.text = a.OptStr(desc);
        d_node.role = a.OptStr("caption");

        auto left = a.MakeNode(PULSE_UI_MODEL_STACK, id + "-left");
        left.gap = a.OptInt(0);
        left.flex = a.OptInt(1);
        left.children = a.MakeChildren({t_node, d_node});

        auto row = a.MakeNode(PULSE_UI_MODEL_STACK, id + "-row");
        row.orientation = a.OptStr("horizontal");
        row.alignment = a.OptStr("center");
        row.gap = a.OptInt(16);
        row.children = a.MakeChildren({left, ctrl});
        return row;
    };
    auto make_switch = [&](std::string_view id, std::string_view state_key) {
        auto sw = a.MakeNode(PULSE_UI_MODEL_SWITCH, id);
        sw.state = a.OptStr(state_key);
        sw.min_width = a.OptInt(48);
        sw.max_width = a.OptInt(48);
        return sw;
    };
    auto make_button = [&](std::string_view id, std::string_view text, std::string_view cmd) {
        auto b = a.MakeNode(PULSE_UI_MODEL_BUTTON, id);
        b.text = a.OptStr(text);
        b.command = a.OptStr(cmd);
        return b;
    };
    auto make_card = [&](std::string_view id, std::initializer_list<PulseUiModelNode> kids) {
        auto g = a.MakeNode(PULSE_UI_MODEL_GROUP, id);
        g.bordered = a.OptBool(true);
        g.gap = a.OptInt(14);
        g.children = a.MakeChildren(kids);
        return g;
    };

    auto make_segmented = [&](std::string_view id, std::string_view state_key,
                              std::initializer_list<std::string_view> opts, int width) {
        auto seg = a.MakeNode(PULSE_UI_MODEL_SEGMENTED, id);
        seg.state = a.OptStr(state_key);
        seg.options = a.OptOptions(opts);
        seg.min_width = a.OptInt(width);
        seg.max_width = a.OptInt(width);
        return seg;
    };
    auto make_slider = [&](std::string_view id, std::string_view state_key,
                           std::initializer_list<std::string_view> stops, int width) {
        auto sl = a.MakeNode(PULSE_UI_MODEL_SLIDER, id);
        sl.state = a.OptStr(state_key);
        sl.options = a.OptOptions(stops);
        sl.stops = a.OptInt(static_cast<int64_t>(stops.size()));
        sl.min_width = a.OptInt(width);
        sl.max_width = a.OptInt(width);
        return sl;
    };
    auto make_hstack = [&](std::string_view id, int gap, std::initializer_list<PulseUiModelNode> kids) {
        auto s = a.MakeNode(PULSE_UI_MODEL_STACK, id);
        s.orientation = a.OptStr("horizontal");
        s.alignment = a.OptStr("center");
        s.gap = a.OptInt(gap);
        s.children = a.MakeChildren(kids);
        return s;
    };

    // Laid out like Pulse's own settings pages (7-Zip): a heading over each card, one setting per row,
    // the control on the right. The package name is already in the selector above, so no title card.
    auto make_action = [&](std::string_view id, std::string_view text, std::string_view cmd, std::string_view glyph,
                           bool primary) {
        auto b = make_button(id, text, cmd);
        b.glyph = a.OptStr(glyph);
        if (primary) b.kind = a.OptStr("primary");
        b.min_width = a.OptInt(112);
        b.max_width = a.OptInt(112);
        return b;
    };
    auto status_node = a.MakeNode(PULSE_UI_MODEL_TEXT, "desktop-status");
    status_node.binding = a.OptStr("state.status");
    status_node.role = a.OptStr("caption");
    auto master_title = a.MakeNode(PULSE_UI_MODEL_TEXT, "master-title");
    master_title.text = a.OptStr("启用桌面整理");
    auto master_desc = make_caption("master-desc", "散落的文件按规则收进分组，常用位置变成磁贴。只改显示位置，文件都还在桌面文件夹里；关掉就回到 Windows 桌面。");
    auto master_left = a.MakeNode(PULSE_UI_MODEL_STACK, "master-left");
    master_left.gap = a.OptInt(2);
    master_left.flex = a.OptInt(1);
    master_left.children = a.MakeChildren({master_title, master_desc, status_node});
    auto master_card = make_card("master-card", {
        make_hstack("master-row", 16, {master_left, make_switch("master-sw", "enabled")}),
    });

    auto ai_row = make_row("ai", "AI 桌面收纳", "按项目和内容把桌面文件归档到文件夹，先给方案，确认后才移动",
                           make_action("btn-ai-organize", "开始收纳", "cmd.ai_organize", "icon:sparkle", true));
    ai_row.visible_binding = a.OptStr("state.ai_organize_enabled");
    auto actions_card = make_card("actions-card", {
        ai_row,
        make_row("checkup", "桌面体检", "找出已装好的安装包、空文件夹和很久没碰的文件；只给建议，不自动删除",
                 make_action("btn-health", "开始体检", "run-health", "icon:shield", false)),
        make_row("reorganize", "按规则重新整理", "把散落的文件重新收进分组；手动拖进分组的不会被移走",
                 make_action("btn-reorganize", "重新整理", "reorganize-desktop", "icon:refresh", false)),
    });

    const auto tr = [](l10n::StringId id) { return ipc::WideToUtf8(l10n::Get(id)); };
    auto make_appearance_row = [&](std::string id, std::string_view title, std::string_view desc, PulseUiModelNode ctrl) {
        ctrl.min_width = {}; ctrl.max_width = {};
        auto label = a.MakeNode(PULSE_UI_MODEL_TEXT, id + "-title");
        label.text = a.OptStr(title);
        auto hint = make_caption(id + "-desc", desc);
        auto row = a.MakeNode(PULSE_UI_MODEL_STACK, id + "-row");
        row.gap = a.OptInt(8);
        row.children = a.MakeChildren({label, hint, ctrl});
        return row;
    };
    auto appearance_card = make_card("appearance-card", {
        make_appearance_row("scene", "场景", "专注只展开「进行中」，其余淡下去；演示隐藏桌面图标和私人磁贴",
                 make_segmented("scene-seg", "scene", {"工作", "专注", "演示"}, 240)),
        make_appearance_row("material", "窗口效果", "分组、磁贴和菜单一起变；系统关闭透明效果时用纯色",
                 make_segmented("material-seg", "material",
                                {"跟随 Pulse", "亚克力", "Mica", "Mica Alt", "无", "液态玻璃"}, 460)),
        make_appearance_row("opacity", "不透明度", "0% 时分组和磁贴只剩内容，壁纸完全露出来；菜单始终保留足够的对比度",
                 make_slider("opacity-slider", "opacity", {"0%", "20%", "40%", "60%", "80%", "100%"}, 300)),
        make_appearance_row("iconsize", "图标大小", "分组里和桌面上的图标一起变",
                 make_segmented("iconsize-seg", "icon_size", {"小", "中", "大"}, 180)),
        make_appearance_row("label", "图标文字", "壁纸很亮或很花时，底板更清楚",
                 make_segmented("label-seg", "label_style",
                                {"阴影", "底板", "悬停显示", ipc::WideToUtf8(l10n::Get(l10n::StringId::DesktopIconLabelNormal))}, 280)),
    });

    auto optics_card = make_card("glass-optics-card", {});
    a.node_slices.emplace_back();
    auto& optics = a.node_slices.back();
    optics.push_back(make_caption("glass-optics-help", tr(l10n::StringId::DesktopGlassOpticsHelp)));
    optics.push_back(make_button("glass-reset", tr(l10n::StringId::DesktopGlassReset), "glass-reference-reset"));
    size_t optical_index=0;
    for(const auto& field:kGlassOpticalFields) {
        auto slider=a.MakeNode(PULSE_UI_MODEL_SLIDER,field.key);
        slider.kind=a.OptStr("continuous");slider.state=a.OptStr(field.key);
        slider.text=a.OptStr(tr(kGlassControlLabels[optical_index]));
        slider.command=a.OptStr("glass-commit");
        slider.options=a.OptOptions({GlassNumber(field.min),GlassNumber(field.max)});
        slider.helper=a.OptStr(ipc::WideToUtf8(kGlassControlUnits[optical_index]));
        optics.push_back(slider);++optical_index;
    }
    auto edge_switch=make_switch("glass-edge-switch","glass2_blur_edge");
    edge_switch.min_width={};edge_switch.max_width={};
    edge_switch.text=a.OptStr(tr(l10n::StringId::DesktopGlassBlurEdge));
    optics.push_back(edge_switch);
    optics_card.children={1u,optics.data(),static_cast<uint32_t>(optics.size())};

    auto organize_card = make_card("organize-card", {
        make_row("organize", "按规则收进分组", "进行中、安装包、截图与图片、未归类；手动拖进去的不受规则影响",
                 make_switch("organize-sw", "organize")),
        make_row("tiles", "磁贴", "照片、下载、最近使用、回收站、磁盘和 Pulse",
                 make_switch("tiles-sw", "tiles")),
        make_row("live", "动态磁贴", "照片轮播，下载和最近使用实时更新",
                 make_switch("live-sw", "live_tiles")),
        make_row("sleep", "全屏或节能时暂停", "玩游戏、看视频或电池节能时，动态磁贴与动态折射自动暂停",
                 make_switch("sleep-sw", "auto_sleep")),
        make_row("dblhide", "双击空白处隐藏图标", "只留壁纸和磁贴，再双击恢复",
                 make_switch("dblhide-sw", "dbl_hide")),
        make_row("health", "桌面体检提示", "发现可以清理的文件时在桌面上提示一次",
                 make_switch("health-sw", "health_check")),
    });

    auto pulse_card = make_card("pulse-card", {
        make_row("menu", "右键使用 Pulse 菜单", "系统和其他软件的菜单项照常出现，标「系统」；关掉后是 Windows 原生菜单",
                 make_switch("menu-sw", "hybrid_menu")),
        make_row("search", "在桌面上打字直接搜索", "打开 Pulse 快速搜索并带上已输入的文字；Ctrl+F 也可以",
                 make_switch("search-sw", "type_search")),
        make_row("tray", "桌面上的暂存区", "和 Pulse 的暂存区是同一份，桌面和 Pulse 窗口之间拖进拖出",
                 make_switch("tray-sw", "sync_tray")),
    });

    // 天气 needs a city: no location API, Open-Meteo's geocoder finds it by name.
    auto city = a.MakeNode(PULSE_UI_MODEL_TEXT_INPUT, "weather-city");
    city.text = a.OptStr("天气城市");   // the accessible label Pulse requires on a text input
    city.state = a.OptStr("weather_city");
    city.placeholder = a.OptStr("例如 北京、上海、Hangzhou");
    city.min_width = a.OptInt(220);
    city.max_width = a.OptInt(220);
    auto widgets_card = make_card("widgets-card", {
        make_row("weather", "天气城市", "天气小组件显示这个城市；数据来自 Open-Meteo，每 30 分钟更新一次", city),
    });

    auto content = a.MakeNode(PULSE_UI_MODEL_STACK, "settings-content");
    content.gap = a.OptInt(8);
    content.children = a.MakeChildren({
        master_card,
        make_section("sec-actions", "整理"),
        actions_card,
        make_section("sec-appearance", "外观与场景"),
        appearance_card,
        make_section("sec-glass-optics", tr(l10n::StringId::DesktopGlassOptics)), optics_card,
        make_section("sec-organize", "分组与磁贴"),
        organize_card,
        make_section("sec-widgets", "小组件"),
        widgets_card,
        make_section("sec-pulse", "与 Pulse 打通"),
        pulse_card,
    });

    auto root = a.MakeNode(PULSE_UI_MODEL_SCROLL_VIEW, "settings-root");
    root.children = a.MakeChildren({content});

    a.AddState("enabled", "true");
    a.AddState("ai_organize_enabled", ai_organize_enabled_ ? "true" : "false");
    a.AddState("hybrid_menu", "true");
    a.AddState("organize", "true");
    a.AddState("tiles", "true");
    a.AddState("live_tiles", "true");
    a.AddState("type_search", "true");
    a.AddState("sync_tray", "true");
    a.AddState("dbl_hide", "true");
    a.AddState("health_check", "true");
    a.AddState("material", "1");
    for(const auto& field:kGlassOpticalFields)a.AddState(field.key,GlassNumber(prefs_.glass_options.*(field.member)));
    a.AddState("glass2_blur_edge",prefs_.glass_options.blur_edge?"true":"false");
    a.AddState("icon_size", "1");
    a.AddState("label_style", "0");
    a.AddState("scene", "0");
    a.AddState("opacity", "3");
    a.AddState("auto_sleep", "true");
    a.AddState("weather_city", "");
    a.AddState("status", "正在运行 · 更改会立即应用到桌面");

    static constexpr const char* kPanelCommands[] = {
        "glass-commit", "glass-reference-reset",
        "cmd.ai_organize",
        "run-health",
        "reorganize-desktop",
    };
    for (const char* cmd : kPanelCommands) {
        a.AddCommand(cmd);
    }

    PulseUiModelTree tree{};
    tree.size = sizeof(tree);
    tree.root = &root;
    tree.states = a.states.data();
    tree.state_count = static_cast<uint32_t>(a.states.size());
    tree.commands = a.commands.data();
    tree.command_count = static_cast<uint32_t>(a.commands.size());

    const PulseStatus st = pages_.create(pages_.context, Sv("桌面整理"), &tree, &page_id_);
    if (st != PULSE_OK) {
        page_id_ = 0;
        if (has_resources_) LoadPersisted();
        return st;
    }

    pages_.query(pages_.context, page_id_, Sv(PULSE_UI_MODEL_INTERFACE),
                 PULSE_UI_MODEL_VERSION, &ui_model_, sizeof(ui_model_));
    pages_.query(pages_.context, page_id_, Sv(PULSE_UI_EVENTS_INTERFACE_NAME),
                 PULSE_UI_EVENTS_VERSION_1, &ui_events_, sizeof(ui_events_));

    if (ui_events_.subscribe_state) {
        ui_events_.subscribe_state(ui_events_.context, page_id_, Sv(""),
                                   PULSE_UI_STATE_USER, &OnUiStateChanged, this, &state_sub_);
    }
    if (ui_model_.subscribe_command) {
        for (const char* cmd : kPanelCommands) {
            uint64_t tok = 0;
            if (ui_model_.subscribe_command(ui_model_.context, page_id_, Sv(cmd),
                                            &OnUiCommandClicked, this, &tok) == PULSE_OK && tok != 0) {
                cmd_subs_.push_back(tok);
            }
        }
    }

    if (has_resources_) {
        LoadPersisted();
    }
    return PULSE_OK;
}

void DesktopSettingsPage::Close() noexcept {
    if (prefs_.glass_preview && has_glass_edit_start_) {
        prefs_.glass_options=glass_edit_start_.glass_options;
        prefs_.glass_reference=glass_edit_start_.glass_reference;
        prefs_.glass_preset=glass_edit_start_.glass_preset;
        prefs_.glass_preview=false;has_glass_edit_start_=false;
        try { if(on_changed_)on_changed_(prefs_,false); } catch (...) {}
    }
    if (pending_req_ != 0 && has_resources_ && resources_.cancel_request) {
        resources_.cancel_request(resources_.context, pending_req_);
        pending_req_ = 0;
    }
    if (state_sub_ != 0 && ui_events_.unsubscribe) {
        ui_events_.unsubscribe(ui_events_.context, state_sub_);
        state_sub_ = 0;
    }
    for (uint64_t tok : cmd_subs_) {
        if (tok != 0 && ui_model_.unsubscribe) {
            ui_model_.unsubscribe(ui_model_.context, tok);
        }
    }
    cmd_subs_.clear();
    if (page_id_ != 0 && has_pages_ && pages_.remove) {
        pages_.remove(pages_.context, page_id_);
        page_id_ = 0;
    }
    has_pages_ = false;
    has_resources_ = false;
    loading_ = false;
    saving_ = false;
    dirty_while_busy_ = false;
    pumping_persistence_ = false;
    reload_requested_ = false;
    save_requested_ = false;
}

void DesktopSettingsPage::UpdateFromHostProcess(const DesktopPrefs& updated) {
    const bool enabled_changed = (updated.enabled != prefs_.enabled);
    prefs_ = updated;
    PushStatesToUi();
    SavePersisted();
    if (enabled_changed && on_changed_) {
        on_changed_(prefs_, true);
    }
}

void DesktopSettingsPage::SetStatusText(std::string_view utf8_status) {
    if (page_id_ == 0 || !ui_model_.set_states) return;
    const PulseUiState s{Sv("status"), Sv(utf8_status)};
    ui_model_.set_states(ui_model_.context, page_id_, &s, 1);
}

bool DesktopSettingsPage::SetAiOrganizerEnabled(bool enabled) {
    if (enabled == ai_organize_enabled_) return true;
    if (page_id_ != 0 && ui_model_.set_states) {
        const PulseUiState state{Sv("ai_organize_enabled"), Sv(enabled ? "true" : "false")};
        if (ui_model_.set_states(ui_model_.context, page_id_, &state, 1) != PULSE_OK) return false;
    }
    ai_organize_enabled_ = enabled;
    return true;
}

void DesktopSettingsPage::PushStatesToUi() {
    if (page_id_ == 0 || !ui_model_.set_states) return;
    const std::string mat_str = std::to_string((prefs_.material <= 5) ? prefs_.material : 1u);
    const std::string isz_str = std::to_string(std::min<uint8_t>(prefs_.icon_size, 2));
    const std::string lbl_str = std::to_string(std::min<uint8_t>(prefs_.label_style, 3));
    const std::string scene_str = std::to_string(std::min<uint8_t>(prefs_.scene, 2));
    // The slider has stops 0 %, 20 % ... 100 %.
    const std::string opacity_str = std::to_string((std::min<uint8_t>(prefs_.opacity_percent, 100) + 10) / 20);
    const std::string city_utf8 = ipc::WideToUtf8(prefs_.weather_city);

    const PulseUiState updates[] = {
        {Sv("enabled"), Sv(prefs_.enabled ? "true" : "false")},
        {Sv("hybrid_menu"), Sv(prefs_.hybrid_menu ? "true" : "false")},
        {Sv("organize"), Sv(prefs_.organize ? "true" : "false")},
        {Sv("tiles"), Sv(prefs_.tiles ? "true" : "false")},
        {Sv("live_tiles"), Sv(prefs_.live_tiles ? "true" : "false")},
        {Sv("type_search"), Sv(prefs_.type_to_search ? "true" : "false")},
        {Sv("sync_tray"), Sv(prefs_.sync_tray ? "true" : "false")},
        {Sv("dbl_hide"), Sv(prefs_.double_click_hide ? "true" : "false")},
        {Sv("health_check"), Sv(prefs_.health_check ? "true" : "false")},
        {Sv("material"), Sv(mat_str)},
        {Sv("icon_size"), Sv(isz_str)},
        {Sv("label_style"), Sv(lbl_str)},
        {Sv("scene"), Sv(scene_str)},
        {Sv("opacity"), Sv(opacity_str)},
        {Sv("auto_sleep"), Sv(prefs_.auto_sleep ? "true" : "false")},
        {Sv("weather_city"), Sv(city_utf8)},
    };
    ui_model_.set_states(ui_model_.context, page_id_, updates, static_cast<uint32_t>(std::size(updates)));
    std::vector<std::string> optical_values;
    optical_values.reserve(kGlassOpticalFields.size());
    for(const auto& field:kGlassOpticalFields)optical_values.push_back(GlassNumber(prefs_.glass_options.*(field.member)));
    std::vector<PulseUiState> optical_states;
    for(size_t i=0;i<kGlassOpticalFields.size();++i)optical_states.push_back({Sv(kGlassOpticalFields[i].key),Sv(optical_values[i])});
    optical_states.push_back({Sv("glass2_blur_edge"),Sv(prefs_.glass_options.blur_edge?"true":"false")});
    ui_model_.set_states(ui_model_.context,page_id_,optical_states.data(),static_cast<uint32_t>(optical_states.size()));
}

void DesktopSettingsPage::LoadPersisted() {
    if (!has_resources_) return;
    reload_requested_ = true;
    PumpPersisted();
}

void DesktopSettingsPage::SavePersisted() {
    if (prefs_.glass_preview) return;
    dirty_while_busy_ = true;
    save_requested_ = true;
    PumpPersisted();
}

void DesktopSettingsPage::PumpPersisted() {
    if (!has_resources_ || pumping_persistence_ || loading_ || saving_) return;
    pumping_persistence_ = true;
    struct PumpGuard {
        bool& active;
        ~PumpGuard() { active = false; }
    } guard{pumping_persistence_};
    // Hosts may complete inline. Bound conflict retries and never recurse through
    // callbacks; unresolved edits remain dirty for the next explicit operation.
    for (unsigned attempt = 0; attempt < 4 && !loading_ && !saving_; ++attempt) {
        PulseResourcesRequestId request = 0;
        PulseStatus status = PULSE_OK;
        if (reload_requested_) {
            reload_requested_ = false;
            loading_ = true;
            status = resources_.settings_get(resources_.context, &OnResourcesCompleted, this, &request);
        } else if (save_requested_ && dirty_while_busy_ && !prefs_.glass_preview) {
            save_requested_ = false;
            const std::string json = prefs_.ToPersistedJson();
            if (json.size() > 1024) break;
            saving_ = true;
            dirty_while_busy_ = false;
            status = resources_.settings_set(resources_.context, Sv(kSettingsKey), Sv(json), 0u,
                settings_version_, &OnResourcesCompleted, this, &request);
        } else {
            break;
        }
        if (status != PULSE_OK) {
            if (saving_) dirty_while_busy_ = true;
            loading_ = false;
            saving_ = false;
            pending_req_ = 0;
            break;
        }
        pending_req_ = (loading_ || saving_) ? request : 0;
    }
}

void PULSE_CALL DesktopSettingsPage::OnResourcesCompleted(void* user, const PulseResourcesReply* reply) noexcept {
    if (!user || !reply) return;
    try {
        static_cast<DesktopSettingsPage*>(user)->HandleResourcesReply(*reply);
    } catch (...) {}
}

void DesktopSettingsPage::HandleResourcesReply(const PulseResourcesReply& reply) {
    pending_req_ = 0;
    const bool was_loading = loading_;
    const bool was_saving = saving_;
    loading_ = false;
    saving_ = false;

    if (reply.status == PULSE_OK) {
        settings_version_ = reply.settings.version;
        if (was_loading && !dirty_while_busy_) {
            const bool prev_enabled = prefs_.enabled;
            for (uint32_t i = 0; i < reply.settings.count; ++i) {
                if (CopySv(reply.settings.values[i].key) == kSettingsKey) {
                    const std::string stored = CopySv(reply.settings.values[i].value);
                    uint64_t schema = 0;
                    std::string expanded;
                    if (ipc::JsonFindUint64(stored, "prefs_schema", schema) &&
                        (schema != 1 || !ExpandPersistedJson(stored, expanded))) break;
                    prefs_ = DesktopPrefs::FromJson(stored);
                    break;
                }
            }
            PushStatesToUi();
            if (on_changed_) {
                on_changed_(prefs_, prefs_.enabled != prev_enabled);
            }
        }
        // A delayed initial read or conflict reload supplies the new version,
        // not permission to replace edits made since that read started.
        if (was_loading && dirty_while_busy_) PushStatesToUi();
        if (dirty_while_busy_) {
            SavePersisted();
        }
    } else {
        if (was_saving) dirty_while_busy_ = true;
        if (CopySv(reply.error) == "version_conflict") {
            dirty_while_busy_ = true;
            LoadPersisted();
        }
    }
}

void PULSE_CALL DesktopSettingsPage::OnUiStateChanged(
    void* user, uint64_t /*panel*/, const PulseUiStateBatch* batch) noexcept {
    if (!user || !batch) return;
    try {
        static_cast<DesktopSettingsPage*>(user)->HandleStateBatch(*batch);
    } catch (...) {}
}

void DesktopSettingsPage::HandleStateBatch(const PulseUiStateBatch& batch) {
    DesktopPrefs next = prefs_;
    bool changed = false;

    bool sync_sliders = false;
    for (uint32_t i = 0; i < batch.count; ++i) {
        const std::string k = CopySv(batch.changes[i].key);
        const std::string v = CopySv(batch.changes[i].value);
        if(k.starts_with("glass2_")) {
            if(k=="glass2_blur_edge"&&(v=="true"||v=="false")) {
                next.glass_options.blur_edge=v=="true";next.glass_reference=true;next.glass_preset=4;next.glass_preview=false;changed=true;
            } else for(const auto& field:kGlassOpticalFields) if(k==field.key) {
                double number=0;const auto parsed=std::from_chars(v.data(),v.data()+v.size(),number);
                if(parsed.ec==std::errc{}&&parsed.ptr==v.data()+v.size()&&std::isfinite(number)&&number>=field.min&&number<=field.max) {
                    next.glass_options.*(field.member)=number;next.glass_reference=true;next.glass_preset=4;next.glass_preview=true;changed=true;
                }
            }
            continue;
        }
        if (k == "enabled" && (v == "true" || v == "false")) {
            next.enabled = (v == "true");
            changed = true;
        } else if (k == "hybrid_menu" && (v == "true" || v == "false")) {
            next.hybrid_menu = (v == "true");
            changed = true;
        } else if (k == "organize" && (v == "true" || v == "false")) {
            next.organize = (v == "true");
            changed = true;
        } else if (k == "tiles" && (v == "true" || v == "false")) {
            next.tiles = (v == "true");
            changed = true;
        } else if (k == "live_tiles" && (v == "true" || v == "false")) {
            next.live_tiles = (v == "true");
            changed = true;
        } else if (k == "type_search" && (v == "true" || v == "false")) {
            next.type_to_search = (v == "true");
            changed = true;
        } else if (k == "sync_tray" && (v == "true" || v == "false")) {
            next.sync_tray = (v == "true");
            changed = true;
        } else if (k == "dbl_hide" && (v == "true" || v == "false")) {
            next.double_click_hide = (v == "true");
            changed = true;
        } else if (k == "health_check" && (v == "true" || v == "false")) {
            next.health_check = (v == "true");
            changed = true;
        } else if (k == "material" && v.size() == 1 && v[0] >= '0' && v[0] <= '5') {
            next.material = static_cast<uint8_t>(v[0] - '0');
            changed = true;
        } else if (k == "glass_preset" && v.size() == 1 && v[0] >= '0' && v[0] <= '5') {
            const uint8_t p = static_cast<uint8_t>(v[0] - '0');
            if (p <= 3 || p == 5) {
                next.ApplyGlassPreset(p);
                sync_sliders = true;
            } else {
                next.glass_preset = 4;
            }
            changed = true;
        } else if (k == "glass_thick" && v.size() == 1 && v[0] >= '0' && v[0] <= '4') {
            next.glass_thickness = kThickStops[v[0] - '0'];
            next.glass_preset = 4;
            sync_sliders = true;
            changed = true;
        } else if (k == "glass_blur" && v.size() == 1 && v[0] >= '0' && v[0] <= '4') {
            static constexpr uint8_t legacy_blur_stops[]{1,10,50,100,200};
            next.glass_blur_radius = legacy_blur_stops[v[0] - '0']; next.glass_preset = 4;
            sync_sliders = true; changed = true;
        } else if (k == "glass_blur_edge" && (v == "true" || v == "false")) {
            next.glass_blur_edge = v == "true"; next.glass_preset = 4;
            sync_sliders = true; changed = true;
        } else if (k == "glass_frost" && v.size() == 1 && v[0] >= '0' && v[0] <= '4') {
            next.glass_frost = kFrostStops[v[0] - '0'];
            next.glass_preset = 4;
            sync_sliders = true;
            changed = true;
        } else if (k == "glass_disp" && v.size() == 1 && v[0] >= '0' && v[0] <= '2') {
            next.glass_dispersion = MapLegacyDispersionToGain(static_cast<uint64_t>(v[0] - '0'));
            next.glass_preset = 4;
            sync_sliders = true;
            changed = true;
        } else if (k == "glass_rate" && v.size() == 1 && v[0] >= '0' && v[0] <= '2') {
            next.glass_live_rate = static_cast<uint8_t>(v[0] - '0');
            changed = true;
        } else if (k == "icon_size" && v.size() == 1 && v[0] >= '0' && v[0] <= '2') {
            next.icon_size = static_cast<uint8_t>(v[0] - '0');
            changed = true;
        } else if (k == "label_style" && v.size() == 1 && v[0] >= '0' && v[0] <= '3') {
            next.label_style = static_cast<uint8_t>(v[0] - '0');
            changed = true;
        } else if (k == "scene" && v.size() == 1 && v[0] >= '0' && v[0] <= '2') {
            next.scene = static_cast<uint8_t>(v[0] - '0');
            changed = true;
        } else if (k == "opacity" && v.size() == 1 && v[0] >= '0' && v[0] <= '5') {
            next.opacity_percent = static_cast<uint8_t>((v[0] - '0') * 20);
            changed = true;
        } else if (k == "weather_city") {
            std::wstring city = ipc::Utf8ToWide(v).substr(0, 40);
            while (!city.empty() && city.back() == L' ') city.pop_back();
            while (!city.empty() && city.front() == L' ') city.erase(city.begin());
            if (city != next.weather_city) { next.weather_city = city; changed = true; }
        } else if (k == "auto_sleep" && (v == "true" || v == "false")) {
            next.auto_sleep = (v == "true");
            changed = true;
        } else if (k == "restore_on_exit" && (v == "true" || v == "false")) {
            next.restore_on_exit = (v == "true");
            changed = true;
        }
    }

    if (changed && !(next == prefs_)) {
        const bool enabled_toggled = (next.enabled != prefs_.enabled);
        if(next.glass_preview && !prefs_.glass_preview) { glass_edit_start_=prefs_;has_glass_edit_start_=true; }
        prefs_ = next;
        if(!prefs_.glass_preview)has_glass_edit_start_=false;
        if (sync_sliders) PushStatesToUi();
        SavePersisted();
        const auto now=GetTickCount64();
        if (on_changed_ && (!prefs_.glass_preview || now-last_glass_preview_ms_>=16)) {
            last_glass_preview_ms_=now;on_changed_(prefs_, enabled_toggled);
        }
    }
}

void PULSE_CALL DesktopSettingsPage::OnUiCommandClicked(
    void* user, uint64_t /*panel*/, PulseStringView command) noexcept {
    if (!user) return;
    try {
        auto* self = static_cast<DesktopSettingsPage*>(user);
        const auto id=CopySv(command);
        if (id == "cmd.ai_organize" && !self->ai_organize_enabled_) return;
        if(id=="glass-reference-reset"||id=="glass-commit") {
            if(id=="glass-reference-reset")self->prefs_.ApplyGlassPreset(5);
            else if(!self->prefs_.glass_preview)return;
            self->prefs_.glass_preview=false;self->has_glass_edit_start_=false;
            self->PushStatesToUi();self->SavePersisted();
            if(self->on_changed_)self->on_changed_(self->prefs_,false);
        } else if (self->on_command_) self->on_command_(id);
    } catch (...) {}
}

} // namespace pulse::desktop::plugin
