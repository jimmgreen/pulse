#include "../glass_control_labels.h"
#include "plugin/settings_page.h"
#include "plugin/settings_icons.h"
#include "host/model/world_cities.h"

#include "ipc/desktop_ipc.h"
#include "../../../src/common/localization.h"

#include <algorithm>
#include <charconv>
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
    std::wstring widgets;
    if (ipc::JsonFindWide(json, "widgets", widgets))
        expanded += ",\"widgets\":" + ipc::JsonQuoteWide(widgets.substr(0, 180));
    std::wstring dock;
    if (ipc::JsonFindWide(json, "dock", dock))
        expanded += ",\"dock\":" + ipc::JsonQuoteWide(dock.substr(0, 64));
    uint64_t vw = 0;
    if (ipc::JsonFindUint64(json, "vw", vw)) {
        expanded += ",\"wallpaper_kind\":" + std::to_string((vw >> 1u) & 1u) +
                    ",\"video_fill\":" + std::to_string(vw & 1u);
    }
    expanded += "}";
    return true;
}

PulseStringView Sv(std::string_view s) noexcept {
    return PulseStringView{s.empty() ? "" : s.data(), static_cast<uint32_t>(s.size())};
}

std::string CopySv(PulseStringView v) {
    return (v.data && v.length > 0) ? std::string(v.data, v.length) : std::string{};
}

// ---- 小组件 settings: the UI states behind the 小组件 section, and their parsing ----
constexpr std::string_view kWidgetTabs[] = {"weather", "clock", "calendar", "system", "photos", "pomodoro", "battery", "media"};
constexpr uint32_t kTintColors[] = {0xFF0A84FFu, 0xFF5E5CE6u, 0xFFBF5AF2u, 0xFFFF375Fu, 0xFFFF9F0Au, 0xFF30B0C7u, 0xFF34C759u, 0xFF8E8E93u};
constexpr uint8_t kFocusStops[] = {15, 20, 25, 30, 45, 60};
constexpr uint16_t kPhotoIntervalStops[] = {0, 10, 30, 60, 300, 1800};
constexpr std::string_view kSystemItemKeys[] = {"w_sys_cpu", "w_sys_mem", "w_sys_vram", "w_sys_disk", "w_sys_net"};

std::vector<std::pair<std::string, std::string>> WidgetUiStates(const WidgetPrefs& w, int tab) {
    auto tf = [](bool b) { return std::string(b ? "true" : "false"); };
    std::vector<std::pair<std::string, std::string>> out;
    for (uint8_t i = 0; i < 4; ++i) out.emplace_back("w_scheme_" + std::to_string(i), tf(std::min<uint8_t>(w.scheme, 3) == i));
    out.emplace_back("w_is_tint", tf(w.scheme == 3));
    out.emplace_back("w_sky", tf(w.sky));
    out.emplace_back("w_rotate", tf(w.stack_rotate));
    out.emplace_back("w_tab", std::to_string(std::clamp(tab, 0, static_cast<int>(std::size(kWidgetTabs)) - 1)));
    switch (tab) {
    case 0:
        out.emplace_back("w_unit", w.fahrenheit ? "1" : "0");
        out.emplace_back("w_refresh", w.weather_refresh_min <= 15 ? "0" : w.weather_refresh_min <= 30 ? "1" : "2");
        break;
    case 1:
        out.emplace_back("w_h24", tf(w.clock_24h));
        out.emplace_back("w_seconds", tf(w.clock_seconds));
        out.emplace_back("w_night", tf(w.clock_night_face));
        break;
    case 2:
        out.emplace_back("w_lunar", tf(w.lunar));
        out.emplace_back("w_weekstart", w.week_start == 0 ? "1" : "0");
        out.emplace_back("w_weekno", tf(w.week_number));
        break;
    case 3:
        for (size_t i = 0; i < std::size(kSystemItemKeys); ++i)
            out.emplace_back(std::string(kSystemItemKeys[i]), tf(((w.system_items >> i) & 1u) != 0));
        out.emplace_back("w_sysrate", w.system_refresh_s <= 1 ? "0" : w.system_refresh_s <= 2 ? "1" : "2");
        out.emplace_back("w_syswarn", tf(w.system_warn));
        break;
    case 4: {
        int pi = 1;
        for (size_t i = 0; i < std::size(kPhotoIntervalStops); ++i)
            if (kPhotoIntervalStops[i] == w.photo_interval_s) pi = static_cast<int>(i);
        if (w.photo_interval_s == 0) pi = 0;
        else if (w.photo_interval_s <= 10) pi = 1;
        else if (w.photo_interval_s <= 30) pi = 2;
        else if (w.photo_interval_s <= 60) pi = 3;
        else if (w.photo_interval_s <= 300) pi = 4;
        else pi = 5;
        out.emplace_back("w_photo_int", std::to_string(pi));
        out.emplace_back("w_photo_shuf", tf(w.photo_shuffle));
        out.emplace_back("w_photo_cap", tf(w.photo_caption));
        break;
    }
    case 5: {
        int focus = 2;
        for (size_t i = 0; i < std::size(kFocusStops); ++i)
            if (kFocusStops[i] == w.focus_min) focus = static_cast<int>(i);
        out.emplace_back("w_focus", std::to_string(focus));
        out.emplace_back("w_short", w.short_break_min >= 10 ? "1" : "0");
        out.emplace_back("w_long", w.long_break_min >= 30 ? "2" : w.long_break_min >= 20 ? "1" : "0");
        out.emplace_back("w_chime", tf(w.chime));
        break;
    }
    case 6:
        out.emplace_back("w_bt", tf(w.battery_bluetooth));
        out.emplace_back("w_low", w.battery_low_pct <= 10 ? "0" : w.battery_low_pct <= 15 ? "1" : w.battery_low_pct <= 20 ? "2" : "3");
        break;
    default:
        out.emplace_back("w_cover", tf(w.cover_background));
        out.emplace_back("w_source", tf(w.show_source));
        break;
    }
    return out;
}

// One 小组件 state change into `w` (or which widget's options show). True when the key is a 小组件 key.
bool ApplyWidgetUiState(std::string_view k, std::string_view v, WidgetPrefs& w, int& tab) {
    const bool is_bool = v == "true" || v == "false";
    const bool on = v == "true";
    int n = -1;
    if (!v.empty() && v.size() <= 2 && std::all_of(v.begin(), v.end(), [](char c) { return c >= '0' && c <= '9'; }))
        n = std::stoi(std::string(v));
    if (k.starts_with("w_scheme_") && k.size() == 10 && k[9] >= '0' && k[9] <= '3' && is_bool) {
        if (on) w.scheme = static_cast<uint8_t>(k[9] - '0');  // the option cards are radios: only "true" picks
    } else if (k == "w_sky" && is_bool) {
        w.sky = on;
    } else if (k == "w_rotate" && is_bool) {
        w.stack_rotate = on;
    } else if (k == "w_tab" && n >= 0 && n < static_cast<int>(std::size(kWidgetTabs))) {
        tab = n;
    } else if (k == "w_unit" && n >= 0 && n <= 1) {
        w.fahrenheit = n == 1;
    } else if (k == "w_refresh" && n >= 0 && n <= 2) {
        w.weather_refresh_min = static_cast<uint8_t>(n == 0 ? 15 : n == 1 ? 30 : 60);
    } else if (k == "w_h24" && is_bool) {
        w.clock_24h = on;
    } else if (k == "w_seconds" && is_bool) {
        w.clock_seconds = on;
    } else if (k == "w_night" && is_bool) {
        w.clock_night_face = on;
    } else if (k == "w_lunar" && is_bool) {
        w.lunar = on;
    } else if (k == "w_weekstart" && n >= 0 && n <= 1) {
        w.week_start = static_cast<uint8_t>(n == 0 ? 1 : 0);
    } else if (k == "w_weekno" && is_bool) {
        w.week_number = on;
    } else if (k.starts_with("w_sys_") && is_bool) {
        for (size_t i = 0; i < std::size(kSystemItemKeys); ++i) {
            if (k != kSystemItemKeys[i]) continue;
            const uint8_t bit = static_cast<uint8_t>(1u << i);
            const uint8_t next = on ? static_cast<uint8_t>(w.system_items | bit) : static_cast<uint8_t>(w.system_items & ~bit);
            if (next) w.system_items = next;  // keep at least one
        }
    } else if (k == "w_sysrate" && n >= 0 && n <= 2) {
        w.system_refresh_s = static_cast<uint8_t>(n == 0 ? 1 : n == 1 ? 2 : 5);
    } else if (k == "w_syswarn" && is_bool) {
        w.system_warn = on;
    } else if (k == "w_photo_int" && n >= 0 && n < static_cast<int>(std::size(kPhotoIntervalStops))) {
        w.photo_interval_s = kPhotoIntervalStops[n];
    } else if (k == "w_photo_shuf" && is_bool) {
        w.photo_shuffle = on;
    } else if (k == "w_photo_cap" && is_bool) {
        w.photo_caption = on;
    } else if (k == "w_focus" && n >= 0 && n < static_cast<int>(std::size(kFocusStops))) {
        w.focus_min = kFocusStops[n];
    } else if (k == "w_short" && n >= 0 && n <= 1) {
        w.short_break_min = static_cast<uint8_t>(n == 0 ? 5 : 10);
    } else if (k == "w_long" && n >= 0 && n <= 2) {
        w.long_break_min = static_cast<uint8_t>(n == 0 ? 15 : n == 1 ? 20 : 30);
    } else if (k == "w_chime" && is_bool) {
        w.chime = on;
    } else if (k == "w_bt" && is_bool) {
        w.battery_bluetooth = on;
    } else if (k == "w_low" && n >= 0 && n <= 3) {
        w.battery_low_pct = static_cast<uint8_t>(n == 0 ? 10 : n == 1 ? 15 : n == 2 ? 20 : 30);
    } else if (k == "w_cover" && is_bool) {
        w.cover_background = on;
    } else if (k == "w_source" && is_bool) {
        w.show_source = on;
    } else {
        return k.starts_with("w_");
    }
    return true;
}

constexpr const char* kPanelCommands[] = {
    "glass-commit", "glass-reference-reset",
    "cmd.ai_organize",
    "run-health",
    "reorganize-desktop",
    "pick-photo-folder",
    "pick-video-wallpaper",
};

// 小组件 › 着色颜色 swatches (w-tint-0 is 取自壁纸) and 世界时钟城市 chips (remove a slot / add a city).
const std::vector<std::string>& WidgetCommands() {
    static const std::vector<std::string> cmds = [] {
        std::vector<std::string> v;
        for (size_t i = 0; i <= std::size(kTintColors); ++i) v.push_back("w-tint-" + std::to_string(i));
        for (size_t i = 0; i < 4; ++i) v.push_back("w-city-del-" + std::to_string(i));
        for (size_t i = 0; i < model::kWorldCities.size(); ++i) v.push_back("w-city-add-" + std::to_string(i));
        return v;
    }();
    return cmds;
}

// The world clock's cities as the widget shows them (the defaults until the user picks).
std::vector<std::wstring> ShownWorldCities(const WidgetPrefs& w) {
    std::vector<std::wstring> out;
    if (w.world_cities.empty())
        for (const auto c : model::kDefaultWorldCities) out.emplace_back(c);
    else
        out = w.world_cities;
    if (out.size() > 4) out.resize(4);
    return out;
}

// "+1 小时" for a city's offset from this PC's clock right now (DST included), "" when unknown.
std::string CityOffsetLabel(std::wstring_view tz_key) {
    DYNAMIC_TIME_ZONE_INFORMATION d{};
    bool found = false;
    for (DWORD i = 0; EnumDynamicTimeZoneInformation(i, &d) == ERROR_SUCCESS; ++i)
        if (_wcsicmp(d.TimeZoneKeyName, std::wstring(tz_key).c_str()) == 0) { found = true; break; }
    if (!found) return {};
    SYSTEMTIME utc{}, there{}, here{};
    GetSystemTime(&utc);
    if (!SystemTimeToTzSpecificLocalTimeEx(&d, &utc, &there) || !SystemTimeToTzSpecificLocalTime(nullptr, &utc, &here))
        return {};
    auto minutes = [](const SYSTEMTIME& t) {
        FILETIME ft{};
        SystemTimeToFileTime(&t, &ft);
        return static_cast<int64_t>((static_cast<uint64_t>(ft.dwHighDateTime) << 32 | ft.dwLowDateTime) / 600000000ull);
    };
    const int64_t diff = minutes(there) - minutes(here);
    char buf[32]{};
    if (diff % 60 == 0)
        std::snprintf(buf, sizeof(buf), "%s%lld 小时", diff < 0 ? "\xE2\x88\x92" : "+", static_cast<long long>(std::llabs(diff) / 60));
    else
        std::snprintf(buf, sizeof(buf), "%s%.1f 小时", diff < 0 ? "\xE2\x88\x92" : "+", std::llabs(diff) / 60.0);
    return buf;
}

// Rough label width for a chip (the host sizes tags by their min/max width): CJK ~12.5 DIP, ASCII ~6.5.
int ChipTextWidth(std::string_view utf8) {
    float w = 0.0f;
    for (size_t i = 0; i < utf8.size();) {
        const unsigned char c = static_cast<unsigned char>(utf8[i]);
        const size_t len = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
        w += len >= 3 ? 12.5f : 6.5f;
        i += len;
    }
    return static_cast<int>(std::ceil(w));
}

// 小组件配色 option card art: a calendar and a CPU widget over a wallpaper, drawn in that scheme's veil and
// ink (the values DesktopRenderer uses). 自动 sits the two on a dark and a light half of the wallpaper.
std::string SchemePreviewCanvas(int scheme, uint32_t tint_rgb) {
    SYSTEMTIME now{};
    GetLocalTime(&now);
    static constexpr const char* kWeek[] = {"周日", "周一", "周二", "周三", "周四", "周五", "周六"};
    std::string s = "viewbox 120 60\n";
    if (scheme == 0) {
        s += "clear #2B3A4E\nrect 60 -40 120 140 fill=#DCE5EC\nellipse 18 66 46 22 fill=#3F5A78\nellipse 104 68 44 20 fill=#C3D2DE\n";
    } else {
        s += "clear #3B6FA0\nellipse 20 70 70 34 fill=#5E8FC2\nellipse 112 72 64 30 fill=#2A4F7A\n";
    }
    struct Look { const char* veil; const char* ink; const char* sub; const char* red; };
    char tint_veil[16]{};
    std::snprintf(tint_veil, sizeof(tint_veil), "#B3%06X", tint_rgb & 0xFFFFFFu);
    const Look light{"#E6F5F5F3", "#1D1D1F", "#991D1D1F", "#FF3B30"};
    const Look dark{"#B81C1C1E", "#FFFFFF", "#99FFFFFF", "#FF453A"};
    const Look glass_dark{"#40FFFFFF", "#FFFFFF", "#B3FFFFFF", "#FF6961"};
    const Look glass_light{"#73FFFFFF", "#1D1D1F", "#991D1D1F", "#FF3B30"};
    const Look tint{tint_veil, "#FFFFFF", "#B3FFFFFF", "#FFFFFF"};
    const Look left = scheme == 0 ? glass_dark : scheme == 1 ? light : scheme == 2 ? dark : tint;
    const Look right = scheme == 0 ? glass_light : left;
    char buf[640]{};
    std::snprintf(buf, sizeof(buf),
                  "rect 10 8 44 44 r=10 fill=%s\n"
                  "text 16 11 \"%s\" size=6.5 weight=semibold color=%s h=9\n"
                  "text 16 19 \"%d\" size=20 weight=semibold color=%s h=24\n"
                  "rect 66 8 44 44 r=10 fill=%s\n"
                  "text 72 11 CPU size=6.5 weight=semibold color=%s h=9\n"
                  "text 72 20 23%% size=15 weight=semibold color=%s h=18\n"
                  "rect 72 42 32 3 r=1.5 fill=%s\nrect 72 42 8 3 r=1.5 fill=%s\n",
                  left.veil, kWeek[now.wDayOfWeek % 7], left.red, now.wDay, left.ink,
                  right.veil, right.sub, right.ink, right.sub, right.ink);
    s += buf;
    return s;
}

// 桌面模式 cards: the same wallpaper, with groups and widgets (0) or Windows' own icons and widgets (1).
std::string ModePreviewCanvas(int mode) {
    std::string s = "viewbox 160 64\nclear #3B6FA0\nellipse 24 74 84 36 fill=#5E8FC2\nellipse 144 76 72 32 fill=#2A4F7A\n";
    // Widgets on the right in both modes: a clock, a calendar and a wide weather card.
    s += "rect 112 6 20 20 r=5 fill=#E6F5F5F3\ncircle 122 16 6 stroke=#1D1D1F width=1.2\nline 122 16 122 12.4 stroke=#1D1D1F width=1.2\n"
         "line 122 16 124.6 17.4 stroke=#FF9500 width=1.2\n"
         "rect 136 6 20 20 r=5 fill=#E6F5F5F3\nrect 140 10 12 2.4 r=1.2 fill=#FF3B30\nrect 140 15 12 1.6 r=0.8 fill=#991D1D1F\n"
         "rect 140 19 8 1.6 r=0.8 fill=#991D1D1F\n"
         "rect 112 30 44 20 r=5 fill=#59FFFFFF\ncircle 121 40 4 fill=#FFD60A\nrect 129 36 20 2.4 r=1.2 fill=#FFFFFF\n"
         "rect 129 41 13 1.8 r=0.9 fill=#CCFFFFFF\n";
    if (mode == 0) {
        // Two group cards: a header strip, then rows of icons.
        for (int g = 0; g < 2; ++g) {
            const int x = 6 + g * 52;
            char buf[512]{};
            std::snprintf(buf, sizeof(buf),
                          "rect %d 6 46 44 r=6 fill=#B8FFFFFF\nrect %d 10 18 3 r=1.5 fill=#991D1D1F\n"
                          "rect %d 18 9 9 r=2.4 fill=#%s\nrect %d 18 9 9 r=2.4 fill=#%s\nrect %d 18 9 9 r=2.4 fill=#%s\n"
                          "rect %d 32 9 9 r=2.4 fill=#%s\nrect %d 32 9 9 r=2.4 fill=#%s\n",
                          x, x + 5, x + 5, g ? "34C759" : "0A84FF", x + 18, g ? "30B0C7" : "FF9500",
                          x + 31, g ? "FF2D55" : "AF52DE", x + 5, g ? "FFCC00" : "5856D6", x + 18, g ? "8E8E93" : "FF3B30");
            s += buf;
        }
    } else {
        // Loose desktop icons in Windows' columns, labels under them.
        static constexpr const char* kColors[] = {"0A84FF", "FFCC00", "34C759", "FF9500", "AF52DE", "8E8E93"};
        for (int i = 0; i < 6; ++i) {
            const int col = i / 3, row = i % 3;
            const int x = 9 + col * 22, y = 5 + row * 19;
            char buf[192]{};
            std::snprintf(buf, sizeof(buf), "rect %d %d 10 10 r=2.6 fill=#%s\nrect %d %d 14 2 r=1 fill=#D9FFFFFF\n",
                          x + 2, y, kColors[i], x, y + 12);
            s += buf;
        }
    }
    return s;
}

} // namespace

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

    PulseUiModelStrings OptOptionsV(const std::vector<std::string>& items) {
        string_slices.emplace_back();
        auto& vec = string_slices.back();
        vec.reserve(items.size());
        for (const auto& it : items) vec.push_back(Intern(it));
        return PulseUiModelStrings{1u, vec.data(), static_cast<uint32_t>(vec.size())};
    }

    PulseUiModelNodes MakeChildrenV(const std::vector<PulseUiModelNode>& kids) {
        node_slices.emplace_back(kids);
        auto& vec = node_slices.back();
        return PulseUiModelNodes{1u, vec.data(), static_cast<uint32_t>(vec.size())};
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

    std::list<std::vector<PulseUiModelMenuItem>> menu_slices;
    // A flat command menu: (text, command) pairs.
    PulseUiModelMenus MakeMenu(const std::vector<std::pair<std::string, std::string>>& items) {
        menu_slices.emplace_back();
        auto& vec = menu_slices.back();
        vec.reserve(items.size());
        for (const auto& [text, cmd] : items) {
            PulseUiModelMenuItem m{};
            m.size = sizeof(PulseUiModelMenuItem);
            m.id = OptStr(cmd);
            m.text = OptStr(text);
            m.command = OptStr(cmd);
            vec.push_back(m);
        }
        return PulseUiModelMenus{1u, vec.data(), static_cast<uint32_t>(vec.size())};
    }
};

namespace {

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
        out += ",\"weather_city\":" + ipc::JsonQuoteWide(weather_city) +
               ",\"widgets\":" + ipc::JsonQuoteWide(EncodeWidgetPrefs(widgets)) +
               ",\"dock\":" + ipc::JsonQuoteWide(EncodeDockPrefs(dock)) +
               ",\"wallpaper_kind\":" + std::to_string(std::min<uint8_t>(wallpaper_kind, 1)) +
               ",\"video_fill\":" + std::to_string(std::min<uint8_t>(video_fill, 1)) + "}";
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
    // The 小组件 string is ASCII numbers plus at most four short city names, and dock is ~22 bytes.
    out += "],\"weather_city\":" + ipc::JsonQuoteWide(weather_city.substr(0, 40)) +
           ",\"widgets\":" + ipc::JsonQuoteWide(EncodeWidgetPrefs(widgets)) +
           ",\"dock\":" + ipc::JsonQuoteWide(EncodeDockPrefs(dock));
    const unsigned vw = ((wallpaper_kind ? 1u : 0u) << 1u) | (video_fill ? 1u : 0u);
    if (vw != 0u) {
        out += ",\"vw\":" + std::to_string(vw);
    }
    out += "}";
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
    if (ipc::JsonFindUint64(json, "wallpaper_kind", u)) {
        wallpaper_kind = static_cast<uint8_t>(std::min<uint64_t>(u, 1));
    }
    if (ipc::JsonFindUint64(json, "video_fill", u)) {
        video_fill = static_cast<uint8_t>(std::min<uint64_t>(u, 1));
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
    if (std::wstring enc; ipc::JsonFindWide(json, "widgets", enc)) DecodeWidgetPrefs(enc, widgets);
    if (std::wstring enc; ipc::JsonFindWide(json, "dock", enc)) DecodeDockPrefs(enc, dock);
    if (ipc::JsonFindBool(json, "dock_enabled", b)) dock.enabled = b;
    if (ipc::JsonFindUint64(json, "dock_position", u)) dock.position = static_cast<uint8_t>(std::min<uint64_t>(u, 2));
    if (ipc::JsonFindUint64(json, "dock_hide_mode", u)) dock.hide_mode = static_cast<uint8_t>(std::min<uint64_t>(u, 2));
    if (ipc::JsonFindUint64(json, "dock_minimize_effect", u)) dock.minimize_effect = static_cast<uint8_t>(std::min<uint64_t>(u, 2));
    if (ipc::JsonFindUint64(json, "dock_icon_size", u)) dock.icon_size = static_cast<uint8_t>(std::clamp<uint64_t>(u, 36, 68));
    if (ipc::JsonFindBool(json, "dock_magnification", b)) dock.magnification = b;
    if (ipc::JsonFindUint64(json, "dock_max_scale_x100", u)) dock.max_scale_x100 = static_cast<uint16_t>(std::clamp<uint64_t>(u, 100, 195));
    if (ipc::JsonFindBool(json, "dock_system_area", b)) dock.system_area = b;
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

void DesktopSettingsPage::BuildTree(ModelArena& a, PulseUiModelTree& tree) {
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
    // Every row starts with its icon: a coloured rounded tile with a white line glyph (settings_icons.h).
    constexpr int kRowIcon = 30;
    auto make_icon = [&](const std::string& id, std::string_view key) {
        auto ic = a.MakeNode(PULSE_UI_MODEL_CANVAS, id + "-ic");
        ic.options = a.OptOptionsV({SettingsRowIconCanvas(key)});
        for (auto* f : {&ic.min_width, &ic.max_width, &ic.min_height, &ic.max_height}) *f = a.OptInt(kRowIcon);
        return ic;
    };
    auto make_row = [&](std::string id, std::string_view icon, std::string_view title, std::string_view desc,
                        PulseUiModelNode ctrl) {
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
        row.gap = a.OptInt(12);
        row.children = a.MakeChildren({make_icon(id, icon), left, ctrl});
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
    auto master_desc = make_caption("master-desc", "散落的文件按规则收进分组，也可以只摆小组件。只改显示位置，文件都还在桌面文件夹里；关掉就回到 Windows 桌面。");
    auto master_left = a.MakeNode(PULSE_UI_MODEL_STACK, "master-left");
    master_left.gap = a.OptInt(2);
    master_left.flex = a.OptInt(1);
    master_left.children = a.MakeChildren({master_title, master_desc, status_node});
    // 桌面模式: 整理 + 小组件, or 只显示小组件 for people who keep their own icon layout. The 查看 menu on the
    // desktop shows the same choice; either side changes the other.
    static constexpr const char* kModeNames[] = {"整理 + 小组件", "只显示小组件"};
    static constexpr const char* kModeDetails[] = {"文件按规则收进分组，小组件放在旁边",
                                                   "图标保持 Windows 原来的样子，只多出小组件"};
    std::vector<PulseUiModelNode> mode_cards;
    for (int i = 0; i < 2; ++i) {
        const std::string mid = "mode-" + std::to_string(i);
        auto art = a.MakeNode(PULSE_UI_MODEL_CANVAS, mid + "-art");
        art.options = a.OptOptionsV({ModePreviewCanvas(i)});
        art.min_height = a.OptInt(140);   // a half-width card is about 2.5 : 1, the art's own shape
        art.max_height = a.OptInt(140);
        auto card = a.MakeNode(PULSE_UI_MODEL_OPTION_CARD, mid);
        card.kind = a.OptStr("radio");
        card.state = a.OptStr("view_mode_" + std::to_string(i));
        card.text = a.OptStr(kModeNames[i]);
        card.detail = a.OptStr(kModeDetails[i]);
        card.bordered = a.OptBool(true);
        card.children = a.MakeChildren({art});
        mode_cards.push_back(card);
    }
    auto mode_grid = a.MakeNode(PULSE_UI_MODEL_GRID, "mode-grid");
    mode_grid.columns = a.OptInt(2);
    mode_grid.gap = a.OptInt(10);
    mode_grid.children = a.MakeChildrenV(mode_cards);
    mode_grid.visible_binding = a.OptStr("state.enabled");
    auto master_card = make_card("master-card", {
        make_hstack("master-row", 12, {make_icon("master", "desktop"), master_left, make_switch("master-sw", "enabled")}),
        mode_grid,
    });

    auto ai_row = make_row("ai", "ai", "AI 桌面收纳", "按项目和内容把桌面文件归档到文件夹，先给方案，确认后才移动",
                           make_action("btn-ai-organize", "开始收纳", "cmd.ai_organize", "icon:sparkle", true));
    ai_row.visible_binding = a.OptStr("state.ai_organize_enabled");
    auto actions_card = make_card("actions-card", {
        ai_row,
        make_row("checkup", "checkup", "桌面体检", "找出已装好的安装包、空文件夹和很久没碰的文件；只给建议，不自动删除",
                 make_action("btn-health", "开始体检", "run-health", "icon:shield", false)),
        make_row("reorganize", "reorganize", "按规则重新整理", "把散落的文件重新收进分组；手动拖进分组的不会被移走",
                 make_action("btn-reorganize", "重新整理", "reorganize-desktop", "icon:refresh", false)),
    });

    auto make_appearance_row = [&](std::string id, std::string_view icon, std::string_view title, std::string_view desc,
                                   PulseUiModelNode ctrl) {
        ctrl.min_width = {}; ctrl.max_width = {};
        auto label = a.MakeNode(PULSE_UI_MODEL_TEXT, id + "-title");
        label.text = a.OptStr(title);
        auto hint = make_caption(id + "-desc", desc);
        auto texts = a.MakeNode(PULSE_UI_MODEL_STACK, id + "-left");
        texts.gap = a.OptInt(0);
        texts.flex = a.OptInt(1);
        texts.children = a.MakeChildren({label, hint});
        auto head = make_hstack(id + "-head", 12, {make_icon(id, icon), texts});
        auto row = a.MakeNode(PULSE_UI_MODEL_STACK, id + "-row");
        row.gap = a.OptInt(8);
        row.children = a.MakeChildren({head, ctrl});
        return row;
    };
    auto glass_mode_row = make_appearance_row("glass-mode", "glass-mode", "液态玻璃风格",
        "清透水滴通透明亮，标准液态兼顾文字可读，柔雾凝脂适合多彩壁纸，流光棱镜折射更浓烈",
        make_segmented("glass-mode-seg", "glass_mode", {"清透水滴", "标准液态", "柔雾凝脂", "流光棱镜"}, 360));
    glass_mode_row.visible_binding = a.OptStr("state.is_liquid_glass");
    auto glass_rate_row = make_appearance_row("glass-rate", "glass-rate", "动态折射帧率",
        "动态壁纸下液态玻璃实时折射刷新率；静态最省电",
        make_segmented("glass-rate-seg", "glass_rate", {"静态", "30Hz", "60Hz"}, 240));
    glass_rate_row.visible_binding = a.OptStr("state.is_liquid_glass");

    auto video_pick_btn = make_button("btn-video-pick", "选择视频…", "pick-video-wallpaper");
    video_pick_btn.min_width = a.OptInt(112);
    video_pick_btn.max_width = a.OptInt(112);
    auto video_file_title = a.MakeNode(PULSE_UI_MODEL_TEXT, "video-file-title");
    video_file_title.text = a.OptStr("视频文件");
    auto video_file_status = a.MakeNode(PULSE_UI_MODEL_TEXT, "video-file-status");
    video_file_status.binding = a.OptStr("state.video_status");
    video_file_status.role = a.OptStr("caption");
    auto video_file_left = a.MakeNode(PULSE_UI_MODEL_STACK, "video-file-left");
    video_file_left.gap = a.OptInt(2);
    video_file_left.flex = a.OptInt(1);
    video_file_left.children = a.MakeChildren({video_file_title, video_file_status});
    auto video_file_row = make_hstack("video-file-row", 12, {make_icon("video-file", "video"), video_file_left, video_pick_btn});
    video_file_row.visible_binding = a.OptStr("state.is_video_wallpaper");

    auto video_fill_row = make_appearance_row("video-fill", "video-fill", "填充方式",
        "铺满裁掉黑边，适应保留完整画面；高于屏幕分辨率的视频会在后台自动优化",
        make_segmented("video-fill-seg", "video_fill", {"铺满", "适应"}, 180));
    video_fill_row.visible_binding = a.OptStr("state.is_video_wallpaper");

    auto appearance_card = make_card("appearance-card", {
        make_appearance_row("wallpaper", "wallpaper", "壁纸", "支持系统壁纸，或选一个本地视频作为桌面动态壁纸（始终静音）",
                 make_segmented("wp-kind-seg", "wp_kind", {"系统", "视频"}, 180)),
        video_file_row,
        video_fill_row,
        make_appearance_row("scene", "scene", "场景", "专注只展开「进行中」，其余淡下去；演示隐藏桌面图标和私人磁贴",
                 make_segmented("scene-seg", "scene", {"工作", "专注", "演示"}, 240)),
        make_appearance_row("material", "material", "窗口效果", "分组、磁贴和菜单一起变；系统关闭透明效果时用纯色",
                 make_segmented("material-seg", "material",
                                {"跟随 Pulse", "亚克力", "Mica", "Mica Alt", "无", "液态玻璃"}, 460)),
        glass_mode_row,
        glass_rate_row,
        make_appearance_row("opacity", "opacity", "不透明度", "0% 时分组和磁贴只剩内容，壁纸完全露出来；菜单始终保留足够的对比度",
                 make_slider("opacity-slider", "opacity", {"0%", "20%", "40%", "60%", "80%", "100%"}, 300)),
        make_appearance_row("iconsize", "size", "图标大小", "分组里和桌面上的图标一起变",
                 make_segmented("iconsize-seg", "icon_size", {"小", "中", "大"}, 180)),
        make_appearance_row("label", "label", "图标文字", "壁纸很亮或很花时，底板更清楚",
                 make_segmented("label-seg", "label_style",
                                {"阴影", "底板", "悬停显示", ipc::WideToUtf8(l10n::Get(l10n::StringId::DesktopIconLabelNormal))}, 280)),
    });

    auto make_select = [&](std::string_view id, std::string_view state_key, const std::vector<std::string>& opts, int width) {
        auto sel = a.MakeNode(PULSE_UI_MODEL_SELECT, id);
        sel.state = a.OptStr(state_key);
        sel.options = a.OptOptionsV(opts);
        sel.min_width = a.OptInt(width);
        sel.max_width = a.OptInt(width);
        return sel;
    };
    auto make_check = [&](std::string_view id, std::string_view text, std::string_view state_key) {
        auto c = a.MakeNode(PULSE_UI_MODEL_CHECKBOX, id);
        c.text = a.OptStr(text);
        c.state = a.OptStr(state_key);
        return c;
    };
    // icon | title over its description | the controls, like make_row.
    auto srow = [&](std::string_view id, std::string_view icon, std::string_view title, std::string_view desc,
                    std::vector<PulseUiModelNode> controls) {
        const std::string sid(id);
        auto t_node = a.MakeNode(PULSE_UI_MODEL_TEXT, sid + "-title");
        t_node.text = a.OptStr(title);
        auto left = a.MakeNode(PULSE_UI_MODEL_STACK, sid + "-left");
        left.gap = a.OptInt(0);
        left.flex = a.OptInt(1);
        if (desc.empty()) {
            left.children = a.MakeChildren({t_node});
        } else {
            auto d_node = make_caption(sid + "-desc", desc);
            left.children = a.MakeChildren({t_node, d_node});
        }
        std::vector<PulseUiModelNode> kids{make_icon(sid, icon), left};
        kids.insert(kids.end(), controls.begin(), controls.end());
        auto r = a.MakeNode(PULSE_UI_MODEL_STACK, id);
        r.orientation = a.OptStr("horizontal");
        r.alignment = a.OptStr("center");
        r.gap = a.OptInt(12);
        r.children = a.MakeChildrenV(kids);
        return r;
    };

    auto dock_pos_row = srow("dock-pos", "dock-pos", "停靠位置", "支持底部横排或左侧、右侧竖排停靠",
                             {make_segmented("dock-pos-seg", "dock_pos", {"底部", "左侧", "右侧"}, 210)});
    dock_pos_row.visible_binding = a.OptStr("state.dock_enabled");
    auto dock_hide_row = srow("dock-hide", "dock-hide", "隐藏方式", "智能隐藏仅在有窗口遮挡程序坞时收回；自动隐藏需鼠标贴边唤出",
                              {make_segmented("dock-hide-seg", "dock_hide", {"常驻", "智能隐藏", "自动隐藏"}, 240)});
    dock_hide_row.visible_binding = a.OptStr("state.dock_enabled");
    auto dock_minfx_row = srow("dock-minfx", "dock-minfx", "窗口收回效果", "最小化或还原窗口时的液态玻璃动画",
                               {make_segmented("dock-minfx-seg", "dock_minfx", {"神奇", "缩放", "关闭"}, 210)});
    dock_minfx_row.visible_binding = a.OptStr("state.dock_enabled");
    auto dock_size_row = srow("dock-size", "size", "图标大小", "也可直接拖动程序坞分隔线连续调整尺寸",
                              {make_segmented("dock-size-seg", "dock_size", {"小", "标准", "大"}, 210)});
    dock_size_row.visible_binding = a.OptStr("state.dock_enabled");
    auto dock_mag_row = srow("dock-mag", "dock-mag", "鼠标悬停放大", "鼠标滑过图标时呈流线波形放大",
                             {make_switch("dock-mag-sw", "dock_mag")});
    dock_mag_row.visible_binding = a.OptStr("state.dock_enabled");
    auto dock_sys_row = srow("dock-sys", "dock-sys", "系统状态与托盘区", "右侧统一显示托盘图标、输入法、Wi-Fi、音量、蓝牙、电池与时间面板",
                             {make_switch("dock-sys-sw", "dock_sys")});
    dock_sys_row.visible_binding = a.OptStr("state.dock_enabled");
    auto dock_card = make_card("dock-card", {
        srow("dock-en", "dock", "显示液态玻璃程序坞", "在桌面边缘停靠常用应用、运行窗口、回收站与系统状态区",
             {make_switch("dock-en-sw", "dock_enabled")}),
        dock_pos_row,
        dock_hide_row,
        dock_minfx_row,
        dock_size_row,
        dock_mag_row,
        dock_sys_row,
    });

    auto organize_card = make_card("organize-card", {
        srow("tiles", "tiles", "显示小组件", "天气、时钟、日历、照片等；程序坞上的小组件按钮也能一键显示或隐藏",
             {make_switch("tiles-sw", "tiles")}),
        srow("live", "live", "动态小组件", "照片轮播，下载和最近使用实时更新",
             {make_switch("live-sw", "live_tiles")}),
        srow("sleep", "sleep", "全屏或节能时暂停", "玩游戏、看视频或电池节能时，动态小组件与动态折射自动暂停",
             {make_switch("sleep-sw", "auto_sleep")}),
        srow("dblhide", "dblhide", "双击空白处隐藏图标", "只留壁纸和磁贴，再双击恢复",
             {make_switch("dblhide-sw", "dbl_hide")}),
        srow("health", "health", "桌面体检提示", "发现可以清理的文件时在桌面上提示一次",
             {make_switch("health-sw", "health_check")}),
    });

    auto pulse_card = make_card("pulse-card", {
        srow("menu", "menu", "右键使用 Pulse 菜单", "系统和其他软件的菜单项照常出现，标「系统」；关掉后是 Windows 原生菜单",
             {make_switch("menu-sw", "hybrid_menu")}),
        srow("search", "search", "在桌面上打字直接搜索", "打开 Pulse 快速搜索并带上已输入的文字；Ctrl+F 也可以",
             {make_switch("search-sw", "type_search")}),
        srow("tray", "tray", "桌面上的暂存区", "和 Pulse 的暂存区是同一份，桌面和 Pulse 窗口之间拖进拖出",
             {make_switch("tray-sw", "sync_tray")}),
    });

    // 小组件: one settingsRow per option (two nodes each), and only the chosen widget's options are in the
    // tree — the SDK caps a page at 128 nodes. Choosing another widget rebuilds the page (RebuildTree).
    // 小组件配色: four option cards, each showing two widgets in that scheme (as in the mockup).
    const auto& wp = prefs_.widgets;
    const uint32_t tint_rgb = wp.tint_auto ? prefs_.accent_argb : wp.tint_argb;
    static constexpr const char* kSchemeNames[] = {"自动", "浅色", "深色", "着色"};
    std::vector<PulseUiModelNode> scheme_cards;
    for (int i = 0; i < 4; ++i) {
        const std::string sid = "w-scheme-" + std::to_string(i);
        auto art = a.MakeNode(PULSE_UI_MODEL_CANVAS, sid + "-art");
        art.options = a.OptOptionsV({SchemePreviewCanvas(i, tint_rgb)});
        art.min_height = a.OptInt(64);
        art.max_height = a.OptInt(64);
        auto card = a.MakeNode(PULSE_UI_MODEL_OPTION_CARD, sid);
        card.kind = a.OptStr("radio");
        card.state = a.OptStr("w_scheme_" + std::to_string(i));
        card.text = a.OptStr(kSchemeNames[i]);
        card.bordered = a.OptBool(true);
        card.children = a.MakeChildren({art});
        scheme_cards.push_back(card);
    }
    auto scheme_grid = a.MakeNode(PULSE_UI_MODEL_GRID, "w-scheme-grid");
    scheme_grid.columns = a.OptInt(4);
    scheme_grid.gap = a.OptInt(10);
    scheme_grid.children = a.MakeChildrenV(scheme_cards);

    // 着色颜色: 取自壁纸 plus round swatches; the picked one carries a ring and a white dot (macOS accent picker).
    static constexpr const char* kTintNames[] = {"蓝", "靛蓝", "紫", "粉红", "橙", "青", "绿", "石墨"};
    std::vector<PulseUiModelNode> swatches;
    auto auto_tag = a.MakeNode(PULSE_UI_MODEL_TAG, "w-tint-auto");
    auto_tag.text = a.OptStr("取自壁纸");
    auto_tag.command = a.OptStr("w-tint-0");
    auto_tag.selected = a.OptBool(wp.tint_auto);
    auto_tag.bordered = a.OptBool(true);
    auto_tag.min_width = a.OptInt(84);
    auto_tag.max_width = a.OptInt(84);
    auto_tag.min_height = a.OptInt(28);
    auto_tag.max_height = a.OptInt(28);
    swatches.push_back(auto_tag);
    for (size_t i = 0; i < std::size(kTintColors); ++i) {
        const uint32_t rgb = kTintColors[i] & 0xFFFFFFu;
        const bool picked = !wp.tint_auto && (wp.tint_argb & 0xFFFFFFu) == rgb;
        char ops[256]{};
        if (picked)
            std::snprintf(ops, sizeof(ops),
                          "viewbox 28 28\ncircle 14 14 13 stroke=#%06X width=1.5\ncircle 14 14 10 fill=#%06X\n"
                          "circle 14 14 3.5 fill=#FFFFFF\n", rgb, rgb);
        else
            std::snprintf(ops, sizeof(ops), "viewbox 28 28\ncircle 14 14 10 fill=#%06X\ncircle 14 14 10 stroke=#1F000000 width=1\n",
                          rgb);
        auto sw = a.MakeNode(PULSE_UI_MODEL_CANVAS, "w-tint-" + std::to_string(i + 1));
        sw.text = a.OptStr(kTintNames[i]);
        sw.command = a.OptStr("w-tint-" + std::to_string(i + 1));
        sw.options = a.OptOptionsV({ops});
        for (auto* f : {&sw.min_width, &sw.max_width, &sw.min_height, &sw.max_height}) *f = a.OptInt(28);
        swatches.push_back(sw);
    }
    auto swatch_row = a.MakeNode(PULSE_UI_MODEL_STACK, "w-tint-swatches");
    swatch_row.orientation = a.OptStr("horizontal");
    swatch_row.alignment = a.OptStr("center");
    swatch_row.gap = a.OptInt(8);
    swatch_row.children = a.MakeChildrenV(swatches);
    auto tint_row = make_appearance_row("w-tint", "w-tint", "着色颜色", "“取自壁纸”会在换壁纸后跟着变", swatch_row);
    tint_row.visible_binding = a.OptStr("state.w_is_tint");
    auto widget_look_card = make_card("widget-look-card", {
        make_appearance_row("w-scheme", "w-scheme", "小组件配色",
                            "所有小组件一起变；材质跟着上面的“窗口效果”。自动：按背后壁纸的明暗选深色或浅色字", scheme_grid),
        tint_row,
        srow("w-sky", "w-sky", "天气用天空的颜色", "晴天蓝、阴天灰、夜里深蓝；关掉后和其他小组件同色", {make_switch("w-sky-sw", "w_sky")}),
        srow("w-rotate", "w-rotate", "叠放自动轮换", "把小组件叠在一起后，没在看时自动轮流切换", {make_switch("w-rotate-sw", "w_rotate")}),
    });

    std::vector<PulseUiModelNode> per_widget{
        make_segmented("w-tab-seg", "w_tab", {"天气", "时钟", "日历", "系统", "照片", "番茄钟", "电池", "正在播放"}, 560)};
    switch (widget_tab_) {
    case 0: {
        // 天气 needs a city: no location API, Open-Meteo's geocoder finds it by name.
        auto city = a.MakeNode(PULSE_UI_MODEL_TEXT_INPUT, "weather-city");
        city.text = a.OptStr("天气城市");   // the accessible label Pulse requires on a text input
        city.state = a.OptStr("weather_city");
        city.placeholder = a.OptStr("例如 北京、上海、Hangzhou");
        city.min_width = a.OptInt(220);
        city.max_width = a.OptInt(220);
        per_widget.push_back(srow("weather", "place", "城市", "天气小组件显示这个城市；数据来自 Open-Meteo，不需要账号", {city}));
        per_widget.push_back(srow("w-unit", "temp", "温度单位", "", {make_segmented("w-unit-seg", "w_unit", {"摄氏 °C", "华氏 °F"}, 180)}));
        per_widget.push_back(srow("w-refresh", "refresh", "更新间隔", "全屏或节能时暂停更新",
                                  {make_segmented("w-refresh-seg", "w_refresh", {"15 分钟", "30 分钟", "1 小时"}, 240)}));
        break;
    }
    case 1: {
        // 世界时钟城市: one chip per city with its offset (click to remove), then 添加城市 with the rest.
        const auto shown = ShownWorldCities(wp);
        std::vector<PulseUiModelNode> chips;
        for (size_t i = 0; i < shown.size(); ++i) {
            std::string label = ipc::WideToUtf8(shown[i]);
            const auto* city = model::FindWorldCity(shown[i]);
            if (city)
                if (const auto off = CityOffsetLabel(city->tz_key); !off.empty()) label += "  " + off;
            auto chip = a.MakeNode(PULSE_UI_MODEL_TAG, "w-city-chip-" + std::to_string(i));
            chip.text = a.OptStr(label);
            chip.bordered = a.OptBool(true);
            int w = ChipTextWidth(label) + 24;
            if (shown.size() > 1) {  // the clock keeps at least one city
                chip.command = a.OptStr("w-city-del-" + std::to_string(i));
                chip.glyph = a.OptStr("icon:close");
                w += 19;
            }
            chip.min_width = a.OptInt(w);
            chip.max_width = a.OptInt(w);
            chip.min_height = a.OptInt(30);
            chip.max_height = a.OptInt(30);
            chips.push_back(chip);
        }
        if (shown.size() < 4) {
            std::vector<std::pair<std::string, std::string>> items;
            for (size_t c = 0; c < model::kWorldCities.size(); ++c) {
                const auto label = model::kWorldCities[c].label;
                if (std::find(shown.begin(), shown.end(), label) != shown.end()) continue;
                std::string text = ipc::WideToUtf8(std::wstring(label));
                if (const auto off = CityOffsetLabel(model::kWorldCities[c].tz_key); !off.empty()) text += "  " + off;
                items.emplace_back(std::move(text), "w-city-add-" + std::to_string(c));
            }
            auto add = a.MakeNode(PULSE_UI_MODEL_MENU_BUTTON, "w-city-add");
            add.text = a.OptStr("添加城市");
            add.glyph = a.OptStr("icon:plus");
            add.menu = a.MakeMenu(items);
            add.min_width = a.OptInt(132);
            add.max_width = a.OptInt(132);
            chips.push_back(add);
        }
        auto chip_row = a.MakeNode(PULSE_UI_MODEL_STACK, "w-city-chips");
        chip_row.orientation = a.OptStr("horizontal");
        chip_row.alignment = a.OptStr("center");
        chip_row.gap = a.OptInt(8);
        chip_row.children = a.MakeChildrenV(chips);
        per_widget.push_back(srow("w-h24", "clock", "24 小时制", "关掉后显示“下午 3:05”", {make_switch("w-h24-sw", "w_h24")}));
        per_widget.push_back(srow("w-seconds", "stopwatch", "显示秒针", "表盘上的橙色秒针；关掉后表盘每分钟才重画一次，更省电",
                                  {make_switch("w-seconds-sw", "w_seconds")}));
        per_widget.push_back(srow("w-night", "moon", "夜里表盘变深", "当地 18:00 到 6:00 表盘换成深色，看世界时钟时一眼知道那边是晚上",
                                  {make_switch("w-night-sw", "w_night")}));
        per_widget.push_back(make_appearance_row("w-cities", "globe", "世界时钟城市",
                                                 "4×1 和 4×2 的时钟显示这几个城市，最多 4 个，按这个顺序排", chip_row));
        break;
    }
    case 2:
        per_widget.push_back(srow("w-lunar", "calendar", "显示农历", "日期下面显示农历和节日", {make_switch("w-lunar-sw", "w_lunar")}));
        per_widget.push_back(srow("w-weekstart", "weekstart", "每周第一天", "", {make_segmented("w-weekstart-seg", "w_weekstart", {"周一", "周日"}, 160)}));
        per_widget.push_back(srow("w-weekno", "weekno", "显示周数", "在日期旁边显示“第 48 周”", {make_switch("w-weekno-sw", "w_weekno")}));
        break;
    case 3: {
        // One line of checks at their label width (the host gives unsized children 160 DIP columns).
        auto sized_check = [&](std::string_view id, std::string_view text, std::string_view key) {
            auto c = make_check(id, text, key);
            c.min_width = a.OptInt(84);
            c.max_width = a.OptInt(84);
            return c;
        };
        // Five checks: a settingsRow takes at most 4 inline controls, so they sit on their own line.
        per_widget.push_back(make_appearance_row("w-sys", "chip", "显示哪些项目", "2×2 最多放 4 项，按这个顺序取",
                                  make_hstack("w-sys-checks", 12,
                                  {sized_check("w-sys-cpu", "CPU", "w_sys_cpu"), sized_check("w-sys-mem", "内存", "w_sys_mem"),
                                   sized_check("w-sys-vram", "显存", "w_sys_vram"), sized_check("w-sys-disk", "C 盘", "w_sys_disk"),
                                   sized_check("w-sys-net", "网络", "w_sys_net")})));
        per_widget.push_back(srow("w-sysrate", "refresh", "刷新间隔", "间隔越长越省电",
                                  {make_segmented("w-sysrate-seg", "w_sysrate", {"1 秒", "2 秒", "5 秒"}, 180)}));
        per_widget.push_back(srow("w-syswarn", "warn", "占用高时变色", "超过 80% 变橙，超过 90% 变红；关掉后一直是单色",
                                  {make_switch("w-syswarn-sw", "w_syswarn")}));
        break;
    }
    case 4: {
        auto pick_btn = make_button("w-photo-pick", "更改文件夹…", "pick-photo-folder");
        pick_btn.min_width = a.OptInt(118);
        pick_btn.max_width = a.OptInt(118);
        per_widget.push_back(srow("w-photo-dir", "folder", "照片文件夹", "不指定时默认读取系统「图片」文件夹", {pick_btn}));
        per_widget.push_back(srow("w-photo-int", "hourglass", "换图间隔", "选「手动」时只在点击翻页后切换",
                                  {make_slider("w-photo-int-slider", "w_photo_int", {"手动", "10秒", "30秒", "1分", "5分", "30分"}, 320)}));
        per_widget.push_back(srow("w-photo-shuf", "shuffle", "随机顺序", "关掉后按文件顺序依次轮播", {make_switch("w-photo-shuf-sw", "w_photo_shuf")}));
        per_widget.push_back(srow("w-photo-cap", "caption", "显示地点和日期", "在照片底部显示日期和柔和渐变底", {make_switch("w-photo-cap-sw", "w_photo_cap")}));
        break;
    }
    case 5:
        per_widget.push_back(srow("w-focus", "focus", "专注时长", "下一轮开始时生效",
                                  {make_slider("w-focus-slider", "w_focus", {"15", "20", "25", "30", "45", "60 分钟"}, 300)}));
        per_widget.push_back(srow("w-short", "cup", "短休息", "", {make_segmented("w-short-seg", "w_short", {"5 分钟", "10 分钟"}, 180)}));
        per_widget.push_back(srow("w-long", "sofa", "长休息", "每完成 4 个专注后",
                                  {make_segmented("w-long-seg", "w_long", {"15 分钟", "20 分钟", "30 分钟"}, 240)}));
        per_widget.push_back(srow("w-chime", "sound", "结束时提示音", "用系统的提示声音", {make_switch("w-chime-sw", "w_chime")}));
        break;
    case 6:
        per_widget.push_back(srow("w-bt", "bluetooth", "显示蓝牙设备", "耳机、鼠标、键盘等会报告电量的设备", {make_switch("w-bt-sw", "w_bt")}));
        per_widget.push_back(srow("w-low", "battery", "低电量提醒线", "低于这个电量时圆环变红",
                                  {make_slider("w-low-slider", "w_low", {"10%", "15%", "20%", "30%"}, 240)}));
        break;
    default:
        per_widget.push_back(srow("w-cover", "album", "用封面颜色做背景", "把专辑封面模糊后铺在底下；关掉后和其他小组件同色",
                                  {make_switch("w-cover-sw", "w_cover")}));
        per_widget.push_back(srow("w-source", "music", "显示播放来源", "例如“网易云音乐”“Edge”", {make_switch("w-source-sw", "w_source")}));
        break;
    }
    auto widgets_card = a.MakeNode(PULSE_UI_MODEL_GROUP, "widgets-card");
    widgets_card.bordered = a.OptBool(true);
    widgets_card.gap = a.OptInt(14);
    widgets_card.children = a.MakeChildrenV(per_widget);

    // 只显示小组件 has no groups to tidy: the 整理 actions go with it.
    auto sec_actions = make_section("sec-actions", "整理");
    sec_actions.visible_binding = a.OptStr("state.organize");
    actions_card.visible_binding = a.OptStr("state.organize");
    auto content = a.MakeNode(PULSE_UI_MODEL_STACK, "settings-content");
    content.gap = a.OptInt(8);
    content.children = a.MakeChildren({
        master_card,
        sec_actions,
        actions_card,
        make_section("sec-dock", "程序坞"),
        dock_card,
        make_section("sec-appearance", "外观与场景"),
        appearance_card,
        make_section("sec-organize", "桌面与小组件"),
        organize_card,
        make_section("sec-widgets", "小组件"),
        widget_look_card,
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
    a.AddState("view_mode_0", "true");
    a.AddState("view_mode_1", "false");
    a.AddState("tiles", "true");
    a.AddState("live_tiles", "true");
    a.AddState("type_search", "true");
    a.AddState("sync_tray", "true");
    a.AddState("dbl_hide", "true");
    a.AddState("health_check", "true");
    a.AddState("material", "1");
    a.AddState("is_liquid_glass", "false");
    a.AddState("wp_kind", prefs_.wallpaper_kind ? "1" : "0");
    a.AddState("is_video_wallpaper", prefs_.wallpaper_kind ? "true" : "false");
    a.AddState("video_fill", prefs_.video_fill ? "1" : "0");
    a.AddState("video_status", video_status_text_);
    a.AddState("glass_mode", "0");
    a.AddState("glass_rate", "1");
    a.AddState("icon_size", "1");
    a.AddState("label_style", "0");
    a.AddState("scene", "0");
    a.AddState("opacity", "3");
    a.AddState("auto_sleep", "true");
    a.AddState("weather_city", "");
    a.AddState("dock_enabled", prefs_.dock.enabled ? "true" : "false");
    a.AddState("dock_pos", std::to_string(std::min<uint8_t>(prefs_.dock.position, 2)));
    a.AddState("dock_hide", std::to_string(std::min<uint8_t>(prefs_.dock.hide_mode, 2)));
    a.AddState("dock_minfx", prefs_.dock.minimize_effect == 1 ? "0" : (prefs_.dock.minimize_effect == 2 ? "1" : "2"));
    a.AddState("dock_size", prefs_.dock.icon_size <= 44 ? "0" : (prefs_.dock.icon_size >= 53 ? "2" : "1"));
    a.AddState("dock_mag", prefs_.dock.magnification ? "true" : "false");
    a.AddState("dock_sys", prefs_.dock.system_area ? "true" : "false");
    for (const auto& [key, value] : WidgetUiStates(prefs_.widgets, widget_tab_)) a.AddState(key, value);
    a.AddState("status", "正在运行 · 更改会立即应用到桌面");

    for (const char* cmd : kPanelCommands) {
        a.AddCommand(cmd);
    }
    for (const auto& cmd : WidgetCommands()) a.AddCommand(cmd);

    a.node_slices.emplace_back(std::vector<PulseUiModelNode>{root});
    tree.size = sizeof(tree);
    tree.root = a.node_slices.back().data();
    tree.states = a.states.data();
    tree.state_count = static_cast<uint32_t>(a.states.size());
    tree.commands = a.commands.data();
    tree.command_count = static_cast<uint32_t>(a.commands.size());

}

// Another widget's options were chosen: swap the 小组件 rows (state keys and values are kept by the host).
void DesktopSettingsPage::RebuildTree() {
    if (page_id_ != 0 && ui_model_.update) {
        ModelArena a;
        PulseUiModelTree tree{};
        BuildTree(a, tree);
        if (ui_model_.update(ui_model_.context, page_id_, &tree) == PULSE_OK) {
            PushStatesToUi();
            return;
        }
    }
    PushStatesToUi();
}

// 着色颜色 swatches and 世界时钟城市 chips: apply, save, and redraw them (their look comes from the prefs).
bool DesktopSettingsPage::HandleWidgetCommand(std::string_view id) {
    DesktopPrefs next = prefs_;
    auto& w = next.widgets;
    auto index = [&](std::string_view prefix, size_t limit) -> int {
        if (!id.starts_with(prefix)) return -1;
        const auto rest = id.substr(prefix.size());
        int n = 0;
        const auto r = std::from_chars(rest.data(), rest.data() + rest.size(), n);
        return r.ec == std::errc{} && r.ptr == rest.data() + rest.size() && n >= 0 && static_cast<size_t>(n) < limit ? n : -1;
    };
    if (const int n = index("w-tint-", std::size(kTintColors) + 1); n >= 0) {
        w.tint_auto = n == 0;
        if (n > 0) w.tint_argb = kTintColors[n - 1];
    } else if (const int del = index("w-city-del-", 4); del >= 0) {
        auto cities = ShownWorldCities(w);
        if (cities.size() > 1 && static_cast<size_t>(del) < cities.size()) cities.erase(cities.begin() + del);
        w.world_cities = std::move(cities);
    } else if (const int add = index("w-city-add-", model::kWorldCities.size()); add >= 0) {
        auto cities = ShownWorldCities(w);
        const std::wstring label(model::kWorldCities[static_cast<size_t>(add)].label);
        if (cities.size() < 4 && std::find(cities.begin(), cities.end(), label) == cities.end()) cities.push_back(label);
        w.world_cities = std::move(cities);
    } else {
        return false;
    }
    if (!(next == prefs_)) {
        prefs_ = next;
        SavePersisted();
        if (on_changed_) on_changed_(prefs_, false);
    }
    RebuildTree();
    return true;
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
    PulseUiModelTree tree{};
    BuildTree(a, tree);

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
        for (const auto& cmd : WidgetCommands()) {
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
    RebuildTree();
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

void DesktopSettingsPage::SetVideoWallpaperStatus(std::string_view file_utf8, std::string_view status_utf8) {
    video_file_name_.assign(file_utf8);
    if (!file_utf8.empty() && !status_utf8.empty()) {
        video_status_text_ = std::string(file_utf8) + " · " + std::string(status_utf8);
    } else if (!status_utf8.empty()) {
        video_status_text_.assign(status_utf8);
    } else if (!file_utf8.empty()) {
        video_status_text_.assign(file_utf8);
    } else {
        video_status_text_ = "未选择视频 · 支持 MP4、MOV、MKV";
    }
    if (page_id_ == 0 || !ui_model_.set_states) return;
    const PulseUiState s{Sv("video_status"), Sv(video_status_text_)};
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
    const std::string is_lg_str = (prefs_.material == 5) ? "true" : "false";
    const uint8_t mode_idx = (prefs_.glass_preset == 1) ? 1u
                           : (prefs_.glass_preset == 3) ? 2u
                           : (prefs_.glass_preset == 2) ? 3u : 0u;
    const std::string gmode_str = std::to_string(mode_idx);
    const std::string grate_str = std::to_string(std::min<uint8_t>(prefs_.glass_live_rate, 2));
    const std::string isz_str = std::to_string(std::min<uint8_t>(prefs_.icon_size, 2));
    const std::string lbl_str = std::to_string(std::min<uint8_t>(prefs_.label_style, 3));
    const std::string scene_str = std::to_string(std::min<uint8_t>(prefs_.scene, 2));
    // The slider has stops 0 %, 20 % ... 100 %.
    const std::string opacity_str = std::to_string((std::min<uint8_t>(prefs_.opacity_percent, 100) + 10) / 20);
    const std::string city_utf8 = ipc::WideToUtf8(prefs_.weather_city);
    const std::string dock_pos_str = std::to_string(std::min<uint8_t>(prefs_.dock.position, 2));
    const std::string dock_hide_str = std::to_string(std::min<uint8_t>(prefs_.dock.hide_mode, 2));
    const std::string dock_minfx_str =
        prefs_.dock.minimize_effect == 1 ? "0" : (prefs_.dock.minimize_effect == 2 ? "1" : "2");
    const std::string dock_size_str =
        prefs_.dock.icon_size <= 44 ? "0" : (prefs_.dock.icon_size >= 53 ? "2" : "1");

    const PulseUiState updates[] = {
        {Sv("enabled"), Sv(prefs_.enabled ? "true" : "false")},
        {Sv("hybrid_menu"), Sv(prefs_.hybrid_menu ? "true" : "false")},
        {Sv("organize"), Sv(prefs_.organize ? "true" : "false")},
        {Sv("view_mode_0"), Sv(prefs_.organize ? "true" : "false")},
        {Sv("view_mode_1"), Sv(prefs_.organize ? "false" : "true")},
        {Sv("tiles"), Sv(prefs_.tiles ? "true" : "false")},
        {Sv("live_tiles"), Sv(prefs_.live_tiles ? "true" : "false")},
        {Sv("type_search"), Sv(prefs_.type_to_search ? "true" : "false")},
        {Sv("sync_tray"), Sv(prefs_.sync_tray ? "true" : "false")},
        {Sv("dbl_hide"), Sv(prefs_.double_click_hide ? "true" : "false")},
        {Sv("health_check"), Sv(prefs_.health_check ? "true" : "false")},
        {Sv("material"), Sv(mat_str)},
        {Sv("is_liquid_glass"), Sv(is_lg_str)},
        {Sv("wp_kind"), Sv(prefs_.wallpaper_kind ? "1" : "0")},
        {Sv("is_video_wallpaper"), Sv(prefs_.wallpaper_kind ? "true" : "false")},
        {Sv("video_fill"), Sv(prefs_.video_fill ? "1" : "0")},
        {Sv("video_status"), Sv(video_status_text_)},
        {Sv("glass_mode"), Sv(gmode_str)},
        {Sv("glass_rate"), Sv(grate_str)},
        {Sv("icon_size"), Sv(isz_str)},
        {Sv("label_style"), Sv(lbl_str)},
        {Sv("scene"), Sv(scene_str)},
        {Sv("opacity"), Sv(opacity_str)},
        {Sv("auto_sleep"), Sv(prefs_.auto_sleep ? "true" : "false")},
        {Sv("weather_city"), Sv(city_utf8)},
        {Sv("dock_enabled"), Sv(prefs_.dock.enabled ? "true" : "false")},
        {Sv("dock_pos"), Sv(dock_pos_str)},
        {Sv("dock_hide"), Sv(dock_hide_str)},
        {Sv("dock_minfx"), Sv(dock_minfx_str)},
        {Sv("dock_size"), Sv(dock_size_str)},
        {Sv("dock_mag"), Sv(prefs_.dock.magnification ? "true" : "false")},
        {Sv("dock_sys"), Sv(prefs_.dock.system_area ? "true" : "false")},
    };
    ui_model_.set_states(ui_model_.context, page_id_, updates, static_cast<uint32_t>(std::size(updates)));
    const auto widget_states = WidgetUiStates(prefs_.widgets, widget_tab_);
    std::vector<PulseUiState> wupdates;
    wupdates.reserve(widget_states.size());
    for (const auto& [key, value] : widget_states) wupdates.push_back({Sv(key), Sv(value)});
    ui_model_.set_states(ui_model_.context, page_id_, wupdates.data(), static_cast<uint32_t>(wupdates.size()));
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
            RebuildTree();
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
    std::string host_command;   // 整理 / 小组件 live on the desktop: the host is told, then reports back

    bool sync_sliders = false;
    bool rebuild = false;
    for (uint32_t i = 0; i < batch.count; ++i) {
        const std::string k = CopySv(batch.changes[i].key);
        const std::string v = CopySv(batch.changes[i].value);
        if (k.starts_with("w_")) {
            const int tab_before = widget_tab_;
            const bool was_tint = next.widgets.scheme == 3;
            if (ApplyWidgetUiState(k, v, next.widgets, widget_tab_)) {
                changed = true;
                if (widget_tab_ != tab_before) rebuild = true;
                // Visibility, or a value the parse corrected, goes back to the page.
                if (was_tint != (next.widgets.scheme == 3) || k.starts_with("w_scheme_") || k.starts_with("w_sys_"))
                    sync_sliders = true;
            }
            continue;
        }
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
            host_command = next.organize ? "desktop-organize-on" : "desktop-organize-off";
            sync_sliders = true;
            changed = true;
        } else if ((k == "view_mode_0" || k == "view_mode_1") && (v == "true" || v == "false")) {
            // A radio pair: choosing one card (true) picks its mode; the host echo of the other is ignored.
            const bool organize = (k == "view_mode_0") == (v == "true");
            if (v == "true") {
                next.organize = organize;
                next.tiles = true;
                host_command = organize ? "desktop-view-organize" : "desktop-view-widgets";
            }
            sync_sliders = true;   // the other card and the 整理 sections follow
            changed = true;
        } else if (k == "tiles" && (v == "true" || v == "false")) {
            next.tiles = (v == "true");
            host_command = next.tiles ? "desktop-tiles-on" : "desktop-tiles-off";
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
            sync_sliders = true;
            changed = true;
        } else if (k == "glass_mode" && v.size() == 1 && v[0] >= '0' && v[0] <= '3') {
            static constexpr uint8_t kModeToPreset[4] = {5, 1, 3, 2};
            next.ApplyGlassPreset(kModeToPreset[v[0] - '0']);
            sync_sliders = true;
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
        } else if (k == "wp_kind" && v.size() == 1 && (v[0] == '0' || v[0] == '1')) {
            next.wallpaper_kind = static_cast<uint8_t>(v[0] - '0');
            sync_sliders = true;
            changed = true;
        } else if (k == "video_fill" && v.size() == 1 && (v[0] == '0' || v[0] == '1')) {
            next.video_fill = static_cast<uint8_t>(v[0] - '0');
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
        } else if (k == "dock_enabled" && (v == "true" || v == "false")) {
            next.dock.enabled = (v == "true");
            sync_sliders = true;
            changed = true;
        } else if (k == "dock_pos" && v.size() == 1 && v[0] >= '0' && v[0] <= '2') {
            next.dock.position = static_cast<uint8_t>(v[0] - '0');
            changed = true;
        } else if (k == "dock_hide" && v.size() == 1 && v[0] >= '0' && v[0] <= '2') {
            next.dock.hide_mode = static_cast<uint8_t>(v[0] - '0');
            changed = true;
        } else if (k == "dock_minfx" && v.size() == 1 && v[0] >= '0' && v[0] <= '2') {
            static constexpr uint8_t kMinFxMap[3] = {1, 2, 0};
            next.dock.minimize_effect = kMinFxMap[v[0] - '0'];
            changed = true;
        } else if (k == "dock_size" && v.size() == 1 && v[0] >= '0' && v[0] <= '2') {
            static constexpr uint8_t kDockSizes[3] = {42, 48, 56};
            next.dock.icon_size = kDockSizes[v[0] - '0'];
            changed = true;
        } else if (k == "dock_mag" && (v == "true" || v == "false")) {
            next.dock.magnification = (v == "true");
            changed = true;
        } else if (k == "dock_sys" && (v == "true" || v == "false")) {
            next.dock.system_area = (v == "true");
            changed = true;
        } else if (k == "auto_sleep" && (v == "true" || v == "false")) {
            next.auto_sleep = (v == "true");
            changed = true;
        } else if (k == "restore_on_exit" && (v == "true" || v == "false")) {
            next.restore_on_exit = (v == "true");
            changed = true;
        }
    }

    if (rebuild) RebuildTree();
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
    } else if (sync_sliders && !host_command.empty()) {
        PushStatesToUi();   // a re-click on the chosen mode card: keep the pair consistent
    }
    if (!host_command.empty() && on_command_) on_command_(host_command);
}

void PULSE_CALL DesktopSettingsPage::OnUiCommandClicked(
    void* user, uint64_t /*panel*/, PulseStringView command) noexcept {
    if (!user) return;
    try {
        auto* self = static_cast<DesktopSettingsPage*>(user);
        const auto id=CopySv(command);
        if (id == "cmd.ai_organize" && !self->ai_organize_enabled_) return;
        if (id.starts_with("w-") && self->HandleWidgetCommand(id)) return;
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
