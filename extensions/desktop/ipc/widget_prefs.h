// widget_prefs.h - The 小组件 options (plugin settings › 小组件), shared by the plugin, the IPC and the host.
// They travel and persist as one compact string so the plugin's persisted prefs stay under the SDK's
// 1024-byte cap: "w1;<number>,<number>,...;<city>|<city>". Unknown or missing numbers keep their defaults.
#pragma once
#include <algorithm>
#include <cstdint>
#include <cwchar>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

namespace pulse::desktop {

struct WidgetPrefs {
    uint8_t scheme = 0;                 // 0 跟随 (ink follows what is behind the card), 1 浅色, 2 深色, 3 着色
    bool tint_auto = true;              // 着色: the desktop accent instead of tint_argb
    uint32_t tint_argb = 0xFF0A84FFu;
    bool sky = true;                    // 天气 wears the sky's colour
    bool fahrenheit = false;
    uint8_t weather_refresh_min = 30;   // 15, 30, 60
    bool clock_24h = true;
    bool clock_seconds = true;          // the orange sweep hand
    bool clock_night_face = true;       // a dark face from 18:00 to 6:00 in that city
    bool lunar = true;
    uint8_t week_start = 1;             // 1 Monday, 0 Sunday
    bool week_number = false;
    uint8_t system_items = 0x1F;        // bit 0 CPU, 1 内存, 2 显存, 3 C 盘, 4 网络
    uint8_t system_refresh_s = 2;       // 1, 2, 5
    bool system_warn = true;            // orange from 80%, red from 90%
    uint8_t focus_min = 25;
    uint8_t short_break_min = 5;
    uint8_t long_break_min = 15;
    bool chime = true;
    bool battery_bluetooth = true;
    uint8_t battery_low_pct = 20;
    bool cover_background = true;
    bool show_source = true;
    bool stack_rotate = true;           // 叠放自动轮换
    uint16_t photo_interval_s = 10;     // 照片换图间隔（秒），0=手动
    bool photo_shuffle = false;         // 照片随机顺序
    bool photo_caption = true;          // 照片显示地点和日期
    std::vector<std::wstring> world_cities;  // empty: 北京 东京 伦敦 纽约
    bool operator==(const WidgetPrefs&) const noexcept = default;
};

inline std::wstring EncodeWidgetPrefs(const WidgetPrefs& p) {
    const unsigned nums[] = {p.scheme, p.tint_auto, 0u, p.sky, p.fahrenheit, p.weather_refresh_min, p.clock_24h,
                             p.clock_seconds, p.clock_night_face, p.lunar, p.week_start, p.week_number, p.system_items,
                             p.system_refresh_s, p.system_warn, p.focus_min, p.short_break_min, p.long_break_min, p.chime,
                             p.battery_bluetooth, p.battery_low_pct, p.cover_background, p.show_source,
                             p.stack_rotate, p.photo_interval_s, p.photo_shuffle, p.photo_caption};
    std::wstring out = L"w1;";
    for (size_t i = 0; i < std::size(nums); ++i) {
        if (i) out += L',';
        if (i == 2) {
            wchar_t hex[12]{};
            std::swprintf(hex, 12, L"%08X", static_cast<unsigned>(p.tint_argb));
            out += hex;
        } else {
            out += std::to_wstring(nums[i]);
        }
    }
    out += L';';
    for (size_t i = 0; i < p.world_cities.size() && i < 4; ++i) {
        if (i) out += L'|';
        out += p.world_cities[i].substr(0, 8);
    }
    return out;
}

// False (and `p` untouched) when `s` is not an encoded set.
inline bool DecodeWidgetPrefs(std::wstring_view s, WidgetPrefs& p) {
    if (s.size() < 3 || s.substr(0, 3) != L"w1;") return false;
    s.remove_prefix(3);
    const size_t semi = s.find(L';');
    const std::wstring_view nums = s.substr(0, semi);
    const std::wstring_view cities = semi == std::wstring_view::npos ? std::wstring_view{} : s.substr(semi + 1);
    std::vector<std::wstring> parts;
    for (size_t a = 0; a <= nums.size();) {
        const size_t b = std::min(nums.find(L',', a), nums.size());
        parts.emplace_back(nums.substr(a, b - a));
        a = b + 1;
    }
    WidgetPrefs q = p;
    auto num = [&](size_t i, uint32_t lo, uint32_t hi, uint32_t def, int base = 10) -> uint32_t {
        if (i >= parts.size() || parts[i].empty()) return def;
        wchar_t* end = nullptr;
        const unsigned long v = std::wcstoul(parts[i].c_str(), &end, base);
        if (!end || *end) return def;
        return std::clamp<uint32_t>(static_cast<uint32_t>(v), lo, hi);
    };
    q.scheme = static_cast<uint8_t>(num(0, 0, 3, q.scheme));
    q.tint_auto = num(1, 0, 1, q.tint_auto) != 0;
    q.tint_argb = num(2, 0, 0xFFFFFFFFu, q.tint_argb, 16) | 0xFF000000u;
    q.sky = num(3, 0, 1, q.sky) != 0;
    q.fahrenheit = num(4, 0, 1, q.fahrenheit) != 0;
    q.weather_refresh_min = static_cast<uint8_t>(num(5, 15, 60, q.weather_refresh_min));
    q.clock_24h = num(6, 0, 1, q.clock_24h) != 0;
    q.clock_seconds = num(7, 0, 1, q.clock_seconds) != 0;
    q.clock_night_face = num(8, 0, 1, q.clock_night_face) != 0;
    q.lunar = num(9, 0, 1, q.lunar) != 0;
    q.week_start = static_cast<uint8_t>(num(10, 0, 1, q.week_start));
    q.week_number = num(11, 0, 1, q.week_number) != 0;
    q.system_items = static_cast<uint8_t>(num(12, 1, 0x1F, q.system_items));
    q.system_refresh_s = static_cast<uint8_t>(num(13, 1, 5, q.system_refresh_s));
    q.system_warn = num(14, 0, 1, q.system_warn) != 0;
    q.focus_min = static_cast<uint8_t>(num(15, 5, 90, q.focus_min));
    q.short_break_min = static_cast<uint8_t>(num(16, 1, 30, q.short_break_min));
    q.long_break_min = static_cast<uint8_t>(num(17, 5, 60, q.long_break_min));
    q.chime = num(18, 0, 1, q.chime) != 0;
    q.battery_bluetooth = num(19, 0, 1, q.battery_bluetooth) != 0;
    q.battery_low_pct = static_cast<uint8_t>(num(20, 5, 50, q.battery_low_pct));
    q.cover_background = num(21, 0, 1, q.cover_background) != 0;
    q.show_source = num(22, 0, 1, q.show_source) != 0;
    q.stack_rotate = num(23, 0, 1, q.stack_rotate) != 0;
    q.photo_interval_s = static_cast<uint16_t>(num(24, 0, 3600, q.photo_interval_s));
    q.photo_shuffle = num(25, 0, 1, q.photo_shuffle) != 0;
    q.photo_caption = num(26, 0, 1, q.photo_caption) != 0;
    q.world_cities.clear();
    for (size_t a = 0; a < cities.size() && q.world_cities.size() < 4;) {
        const size_t b = std::min(cities.find(L'|', a), cities.size());
        if (b > a) q.world_cities.emplace_back(cities.substr(a, std::min<size_t>(b - a, 8)));
        a = b + 1;
    }
    p = std::move(q);
    return true;
}

// Compact Dock bar preferences ("d1;<n0>,<n1>,..."), shared by plugin settings, IPC and host.
struct DockPrefs {
    bool enabled = true;            // 启用液态玻璃程序坞
    uint8_t position = 0;           // 0=底部, 1=左侧, 2=右侧
    uint8_t hide_mode = 0;          // 0=常驻, 1=智能隐藏, 2=自动隐藏
    uint8_t minimize_effect = 1;    // 0=关闭, 1=神奇, 2=缩放
    uint8_t icon_size = 48;         // 36..68 (42 / 48 / 56)
    bool magnification = true;      // 悬停放大
    uint16_t max_scale_x100 = 152;  // 100..195 (132 / 152 / 178)
    bool system_area = true;        // 显示系统状态与托盘区 (.dk-sys)
    bool operator==(const DockPrefs&) const noexcept = default;
};

inline std::wstring EncodeDockPrefs(const DockPrefs& d) {
    const unsigned nums[] = {
        d.enabled ? 1u : 0u,
        std::min<uint8_t>(d.position, 2),
        std::min<uint8_t>(d.hide_mode, 2),
        std::min<uint8_t>(d.minimize_effect, 2),
        std::clamp<uint8_t>(d.icon_size, 36, 68),
        d.magnification ? 1u : 0u,
        std::clamp<uint16_t>(d.max_scale_x100, 100, 195),
        d.system_area ? 1u : 0u,
    };
    std::wstring out = L"d1;";
    for (size_t i = 0; i < std::size(nums); ++i) {
        if (i) out += L',';
        out += std::to_wstring(nums[i]);
    }
    return out;
}

inline bool DecodeDockPrefs(std::wstring_view s, DockPrefs& d) {
    if (s.size() < 3 || s.substr(0, 3) != L"d1;") return false;
    s.remove_prefix(3);
    std::vector<std::wstring> parts;
    for (size_t a = 0; a <= s.size();) {
        const size_t b = std::min(s.find(L',', a), s.size());
        parts.emplace_back(s.substr(a, b - a));
        a = b + 1;
    }
    DockPrefs q = d;
    auto num = [&](size_t i, uint32_t lo, uint32_t hi, uint32_t def) -> uint32_t {
        if (i >= parts.size() || parts[i].empty()) return def;
        wchar_t* end = nullptr;
        const unsigned long v = std::wcstoul(parts[i].c_str(), &end, 10);
        if (!end || *end) return def;
        return std::clamp<uint32_t>(static_cast<uint32_t>(v), lo, hi);
    };
    q.enabled = num(0, 0, 1, q.enabled) != 0;
    q.position = static_cast<uint8_t>(num(1, 0, 2, q.position));
    q.hide_mode = static_cast<uint8_t>(num(2, 0, 2, q.hide_mode));
    q.minimize_effect = static_cast<uint8_t>(num(3, 0, 2, q.minimize_effect));
    q.icon_size = static_cast<uint8_t>(num(4, 36, 68, q.icon_size));
    q.magnification = num(5, 0, 1, q.magnification) != 0;
    q.max_scale_x100 = static_cast<uint16_t>(num(6, 100, 195, q.max_scale_x100));
    q.system_area = num(7, 0, 1, q.system_area) != 0;
    d = q;
    return true;
}

} // namespace pulse::desktop
