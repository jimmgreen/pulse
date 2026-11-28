// settings_icons.h - The 桌面整理 settings page's row icons: a coloured rounded tile with a white line glyph,
// one per setting (as in the Windows 11 / macOS settings apps). Drawn by the page's CANVAS nodes, so they
// need no icon registration and scale with the page; every path uses the absolute M L H V C A Z commands
// Pulse's canvas accepts.
#pragma once

#include <cstdint>
#include <cstdio>
#include <span>
#include <string>
#include <string_view>

namespace pulse::desktop::plugin {

struct SettingsRowIcon {
    const char* key;
    uint32_t rgb;        // the tile
    const char* glyph;   // canvas lines on a 24x24 grid: a line with fill= is filled, any other is stroked white
};

// Glyph lines may use the tile colour as fill=BASE (a crescent cut out of a white disc).
inline constexpr SettingsRowIcon kSettingsRowIcons[] = {
    // 启用桌面整理: a screen with a group and two tiles on it
    {"desktop", 0x0A7CFF,
     "rect 4.5 5.5 15 10.5 r=2\nline 12 16 12 19\nline 8.5 19 15.5 19\n"
     "rect 7 8 4.2 5.5 r=1 fill=#FFFFFF\nrect 12.6 8 4.4 2.3 r=0.8 fill=#FFFFFF\nrect 12.6 11.2 4.4 2.3 r=0.8 fill=#FFFFFF"},
    // 只显示小组件 / 整理 + 小组件 (the mode cards' heading)
    {"mode", 0x5E5CE6,
     "rect 5 5 6 6 r=1.6\nrect 13 5 6 6 r=1.6 fill=#FFFFFF\nrect 5 13 6 6 r=1.6 fill=#FFFFFF\nrect 13 13 6 6 r=1.6"},
    // AI 桌面收纳: sparkles
    {"ai", 0xA455F0,
     "path d=\"M11.5 4.5 C12.1 8.4 13.1 9.4 17 10 C13.1 10.6 12.1 11.6 11.5 15.5 C10.9 11.6 9.9 10.6 6 10 C9.9 9.4 10.9 8.4 11.5 4.5 Z\" fill=#FFFFFF\n"
     "path d=\"M16.8 14 C17 15.5 17.5 16 19 16.2 C17.5 16.4 17 16.9 16.8 18.4 C16.6 16.9 16.1 16.4 14.6 16.2 C16.1 16 16.6 15.5 16.8 14 Z\" fill=#FFFFFF"},
    // 桌面体检: a shield with a check
    {"checkup", 0x30C05A,
     "path d=\"M12 4.6 L18 6.9 V11.5 C18 15.2 15.4 18 12 19.4 C8.6 18 6 15.2 6 11.5 V6.9 Z\"\npath d=\"M9.3 12 L11.2 13.9 L14.9 10.2\""},
    // 按规则重新整理: files dropping into a tray
    {"reorganize", 0xFF9500,
     "path d=\"M5 13.5 V17 C5 17.8 5.7 18.5 6.5 18.5 H17.5 C18.3 18.5 19 17.8 19 17 V13.5\"\n"
     "path d=\"M5 13.5 H9 L10 15.2 H14 L15 13.5 H19\"\nline 12 5 12 11.4\npath d=\"M9.5 9 L12 11.5 L14.5 9\""},
    // 显示液态玻璃程序坞: three apps on the dock
    {"dock", 0x5856D6,
     "rect 4.5 14 15 4.5 r=2.25\nrect 6.4 8.8 3.4 3.4 r=1 fill=#FFFFFF\nrect 10.3 8.8 3.4 3.4 r=1 fill=#FFFFFF\nrect 14.2 8.8 3.4 3.4 r=1 fill=#FFFFFF"},
    // 停靠位置: the screen with the dock at its foot (and the side places)
    {"dock-pos", 0x3D7BF7,
     "rect 4.8 5 14.4 14 r=2.5\nrect 8.4 15.2 7.2 1.9 r=0.95 fill=#FFFFFF\nrect 6.8 8.5 1.9 5 r=0.95 fill=#FFFFFF opacity=0.5\n"
     "rect 15.3 8.5 1.9 5 r=0.95 fill=#FFFFFF opacity=0.5"},
    // 隐藏方式: an eye, struck through
    {"dock-hide", 0x8E8E93,
     "path d=\"M4.5 12 C6.5 8.4 9 6.8 12 6.8 C15 6.8 17.5 8.4 19.5 12 C17.5 15.6 15 17.2 12 17.2 C9 17.2 6.5 15.6 4.5 12 Z\"\n"
     "circle 12 12 2.3\nline 6 18 18 6"},
    // 窗口收回效果: a window flowing down into the dock
    {"dock-minfx", 0x2FAFC6,
     "rect 4.8 4.8 10 7 r=1.5\npath d=\"M9.8 11.8 C9.8 15.4 12.6 16.6 17 16.6\"\npath d=\"M15 14.6 L17 16.6 L15 18.6\""},
    // 图标大小: a small and a large icon on one baseline
    {"size", 0x0A84FF,
     "rect 4.8 11.6 5.4 5.4 r=1.4\nrect 12 9.4 7.6 7.6 r=1.8 fill=#FFFFFF"},
    // 鼠标悬停放大: a magnifier with a plus
    {"dock-mag", 0xFF2D55,
     "circle 10.5 10.5 5\nline 14.2 14.2 18.6 18.6\nline 10.5 8.4 10.5 12.6\nline 8.4 10.5 12.6 10.5"},
    // 系统状态与托盘区: Wi-Fi waves
    {"dock-sys", 0x636366,
     "path d=\"M5.6 10.4 A9 9 0 0 1 18.4 10.4\"\npath d=\"M8.4 13.2 A5 5 0 0 1 15.6 13.2\"\ncircle 12 16.4 1.2 fill=#FFFFFF"},
    // 壁纸: a landscape picture
    {"wallpaper", 0x2EA8E6,
     "rect 4.5 6 15 12 r=2\npath d=\"M5.2 16.6 L9.5 12.3 L12.4 15.2 L14.5 13.1 L18.8 17.3\"\ncircle 15.4 9.4 1.4 fill=#FFFFFF"},
    // 视频文件: a play button
    {"video", 0xFF3B30,
     "rect 4.5 6.5 15 11 r=2.2\npath d=\"M10.5 9.5 L14.7 12 L10.5 14.5 Z\" fill=#FFFFFF"},
    // 填充方式: the picture filling its frame
    {"video-fill", 0xF26B3A,
     "path d=\"M5 9 V5.5 H8.5\"\npath d=\"M15.5 5.5 H19 V9\"\npath d=\"M19 15 V18.5 H15.5\"\npath d=\"M8.5 18.5 H5 V15\"\n"
     "rect 8.6 8.6 6.8 6.8 r=1.2 fill=#FFFFFF"},
    // 场景: stacked layers
    {"scene", 0xAF52DE,
     "path d=\"M12 4.8 L19 8.4 L12 12 L5 8.4 Z\"\npath d=\"M5 11.9 L12 15.5 L19 11.9\"\npath d=\"M5 15.4 L12 19 L19 15.4\""},
    // 窗口效果: two overlapping translucent panes
    {"material", 0x12A2B8,
     "rect 5 5 10 10 r=2\nrect 9 9 10 10 r=2 fill=#FFFFFF opacity=0.45\nrect 9 9 10 10 r=2"},
    // 液态玻璃风格: a drop with its highlight
    {"glass-mode", 0x3A9FEA,
     "path d=\"M12 4.8 C14.8 8.2 17 10.8 17 13.8 A5 5 0 0 1 7 13.8 C7 10.8 9.2 8.2 12 4.8 Z\"\npath d=\"M9.7 14 A2.3 2.3 0 0 0 12 16.3\""},
    // 动态折射帧率: a speedometer
    {"glass-rate", 0xF08A00,
     "path d=\"M5.6 16.2 A7 7 0 1 1 18.4 16.2\"\nline 12 13.6 15.3 9.6\ncircle 12 13.6 1.4 fill=#FFFFFF"},
    // 不透明度: a half-filled disc
    {"opacity", 0x6D7FA6,
     "circle 12 12 6.8\npath d=\"M12 5.2 A6.8 6.8 0 0 1 12 18.8 Z\" fill=#FFFFFF"},
    // 图标文字: a letter on its plate
    {"label", 0xA2845E,
     "rect 4.5 14 15 4.8 r=1.6 fill=#FFFFFF opacity=0.5\npath d=\"M8.2 12.8 L12 5 L15.8 12.8\"\nline 9.6 10.2 14.4 10.2"},
    // 显示小组件: widgets of different sizes
    {"tiles", 0xFF9500,
     "rect 5 5 6 6 r=1.6\nrect 13 5 6 6 r=1.6 fill=#FFFFFF\nrect 5 13 14 6 r=1.6"},
    // 动态小组件: a live pulse line
    {"live", 0x00B8B0,
     "path d=\"M4.5 12.5 H8 L10 8 L13.5 16.5 L15.5 12.5 H19.5\""},
    // 全屏或节能时暂停: pause
    {"sleep", 0x5856D6,
     "circle 12 12 6.8\nline 10.1 9.4 10.1 14.6\nline 13.9 9.4 13.9 14.6"},
    // 双击空白处隐藏图标: a pointer, clicking
    {"dblhide", 0x7D7D85,
     "path d=\"M8 6 L8 17.6 L10.9 14.9 L12.9 19 L14.6 18.2 L12.6 14.2 L16.6 14.2 Z\"\nline 14.4 4.8 15.4 6.6\nline 17.4 7.6 16 8.6"},
    // 桌面体检提示: a bell
    {"health", 0xFF453A,
     "path d=\"M8 15.6 V11 A4 4 0 0 1 16 11 V15.6 L17.5 17 H6.5 Z\"\npath d=\"M10.4 19 A1.7 1.7 0 0 0 13.6 19\""},
    // 右键使用 Pulse 菜单: a menu
    {"menu", 0x0A7CFF,
     "rect 6 4.8 12 14.4 r=2\nline 9 9 15 9\nline 9 12 15 12\nline 9 15 12.8 15"},
    // 在桌面上打字直接搜索: a magnifier
    {"search", 0x2FAFC6,
     "circle 10.8 10.8 5.2\nline 14.6 14.6 18.6 18.6"},
    // 桌面上的暂存区: a tray
    {"tray", 0xEFA300,
     "path d=\"M5 12.5 L7 6.5 H17 L19 12.5 V17 C19 17.8 18.3 18.5 17.5 18.5 H6.5 C5.7 18.5 5 17.8 5 17 Z\"\n"
     "path d=\"M5 12.5 H9 L10 14.3 H14 L15 12.5 H19\""},
    // 小组件配色: a palette
    {"w-scheme", 0xFF375F,
     "path d=\"M12 5 A7 7 0 1 0 12 19 C13.3 19 13.6 18 13 17 C12.4 16 13 15 14.3 15 H16 A3 3 0 0 0 19 12 C19 8.1 15.9 5 12 5 Z\"\n"
     "circle 8.6 11.2 1.15 fill=#FFFFFF\ncircle 10.9 8.3 1.15 fill=#FFFFFF\ncircle 14.6 8.6 1.15 fill=#FFFFFF"},
    // 着色颜色: three overlapping swatches
    {"w-tint", 0xAF52DE,
     "circle 10 10 3.6\ncircle 14 10 3.6\ncircle 12 13.8 3.6"},
    // 天气用天空的颜色: sun behind a cloud
    {"w-sky", 0x2EA8E6,
     "circle 9.4 9 2.6\npath d=\"M8.6 18 H16 A3 3 0 0 0 16 12 A4.2 4.2 0 0 0 8.3 13.2 A2.4 2.4 0 0 0 8.6 18 Z\" fill=BASE\n"
     "path d=\"M8.6 18 H16 A3 3 0 0 0 16 12 A4.2 4.2 0 0 0 8.3 13.2 A2.4 2.4 0 0 0 8.6 18 Z\""},
    // 叠放自动轮换: two arrows turning
    {"w-rotate", 0x30C05A,
     "path d=\"M17.4 9.4 A6 6 0 0 0 6.7 9\"\npath d=\"M17.7 5.8 V9.6 H13.9\"\npath d=\"M6.6 14.6 A6 6 0 0 0 17.3 15\"\npath d=\"M6.3 18.2 V14.4 H10.1\""},
    // 城市: a map pin
    {"place", 0x0A84FF,
     "path d=\"M12 19 C9 15.6 7 12.9 7 10.2 A5 5 0 0 1 17 10.2 C17 12.9 15 15.6 12 19 Z\"\ncircle 12 10.2 1.8"},
    // 温度单位: a thermometer
    {"temp", 0xFF9500,
     "path d=\"M10.2 13.8 V6.8 A1.8 1.8 0 0 1 13.8 6.8 V13.8 A3.4 3.4 0 1 1 10.2 13.8 Z\"\nline 12 9.4 12 15.4\ncircle 12 16.6 1.3 fill=#FFFFFF"},
    // 更新间隔 / 刷新间隔: a refresh arrow
    {"refresh", 0x30C05A,
     "path d=\"M18 12 A6 6 0 1 1 15.9 7.4\"\npath d=\"M16.4 4.6 L16 7.7 L19.1 8.1\""},
    // 24 小时制: a clock
    {"clock", 0x636366,
     "circle 12 12 6.8\npath d=\"M12 8.3 V12 L14.6 13.6\""},
    // 显示秒针: a stopwatch
    {"stopwatch", 0xFF9500,
     "circle 12 13.2 6\nline 12 13.2 14.6 10.6\nline 10.2 4.8 13.8 4.8\nline 12 4.8 12 7.2"},
    // 夜里表盘变深: a crescent moon
    {"moon", 0x4F4CC9,
     "circle 11.6 12.4 6.4 fill=#FFFFFF\ncircle 14.6 9.6 5.2 fill=BASE"},
    // 世界时钟城市: a globe
    {"globe", 0x2FAFC6,
     "circle 12 12 6.8\nellipse 12 12 2.9 6.8\nline 5.2 12 18.8 12"},
    // 显示农历: a calendar page
    {"calendar", 0xFF3B30,
     "rect 5 6 14 13 r=2\nline 5 10 19 10\nline 9 4.6 9 7.4\nline 15 4.6 15 7.4\ncircle 12 14.6 1.6 fill=#FFFFFF"},
    // 每周第一天: the first column marked
    {"weekstart", 0xFF9500,
     "rect 5 6 14 13 r=2\nline 5 10 19 10\nline 9 4.6 9 7.4\nline 15 4.6 15 7.4\nrect 7.2 12 2.8 5 r=0.8 fill=#FFFFFF"},
    // 显示周数: a page with a week number
    {"weekno", 0x5E5CE6,
     "rect 5 6 14 13 r=2\nline 9 4.6 9 7.4\nline 15 4.6 15 7.4\ntext 7.3 9.4 \"48\" size=7.2 weight=bold color=#FFFFFF w=10 h=9"},
    // 显示哪些项目: a processor
    {"chip", 0x0A7CFF,
     "rect 7.5 7.5 9 9 r=1.6\nrect 10 10 4 4 r=0.6 fill=#FFFFFF\nline 10 5 10 7.5\nline 14 5 14 7.5\nline 10 16.5 10 19\nline 14 16.5 14 19\n"
     "line 5 10 7.5 10\nline 5 14 7.5 14\nline 16.5 10 19 10\nline 16.5 14 19 14"},
    // 占用高时变色: a warning sign
    {"warn", 0xFF6A00,
     "path d=\"M12 5.4 L19.2 18 H4.8 Z\"\nline 12 10 12 13.6\ncircle 12 15.8 1 fill=#FFFFFF"},
    // 照片文件夹: a folder
    {"folder", 0xEFA300,
     "path d=\"M4.8 8 A1.5 1.5 0 0 1 6.3 6.5 H10 L11.6 8.2 H17.7 A1.5 1.5 0 0 1 19.2 9.7 V16.5 A1.5 1.5 0 0 1 17.7 18 H6.3 A1.5 1.5 0 0 1 4.8 16.5 Z\""},
    // 换图间隔: an hourglass
    {"hourglass", 0x2EA8E6,
     "path d=\"M8.5 5 H15.5 C15.5 9 12 10 12 12 C12 14 15.5 15 15.5 19 H8.5 C8.5 15 12 14 12 12 C12 10 8.5 9 8.5 5 Z\""},
    // 随机顺序: shuffle arrows
    {"shuffle", 0xFF2D55,
     "path d=\"M5 8 H8 C11 8 13 16 16 16 H19\"\npath d=\"M5 16 H8 C9.3 16 10.3 14.5 11.2 12.8\"\npath d=\"M12.8 11.2 C13.7 9.5 14.7 8 16 8 H19\"\n"
     "path d=\"M17 6 L19 8 L17 10\"\npath d=\"M17 14 L19 16 L17 18\""},
    // 显示地点和日期: a photo with a caption
    {"caption", 0x30C05A,
     "path d=\"M4.5 13.6 H19.5 V16 A2 2 0 0 1 17.5 18 H6.5 A2 2 0 0 1 4.5 16 Z\" fill=#FFFFFF opacity=0.35\nrect 4.5 6 15 12 r=2\nline 7.4 15.8 13 15.8"},
    // 专注时长: a tomato timer
    {"focus", 0xFF3B30,
     "circle 12 13 6.2\npath d=\"M12 6.8 C12 5.3 13 4.6 14.6 4.8\"\npath d=\"M12 10.2 V13 L14 14.6\""},
    // 短休息: a cup
    {"cup", 0x30C05A,
     "path d=\"M6 9.6 H15 V14 A4 4 0 0 1 11 18 H10 A4 4 0 0 1 6 14 Z\"\npath d=\"M15 10.6 H16.4 A2 2 0 0 1 16.4 14.6 H15\"\n"
     "path d=\"M9 4.8 C8.4 5.8 9.6 6.6 9 7.6\"\npath d=\"M12 4.8 C11.4 5.8 12.6 6.6 12 7.6\""},
    // 长休息: a sofa
    {"sofa", 0x2FAFC6,
     "path d=\"M5 15 V11.6 A1.5 1.5 0 0 1 8 11.6 V13 H16 V11.6 A1.5 1.5 0 0 1 19 11.6 V15 Z\"\n"
     "path d=\"M7 10.4 V9 A2 2 0 0 1 9 7 H15 A2 2 0 0 1 17 9 V10.4\"\nline 6.6 15 6.6 17.4\nline 17.4 15 17.4 17.4"},
    // 结束时提示音: a speaker
    {"sound", 0xFF2D55,
     "path d=\"M5.5 10 H8 L11.5 7 V17 L8 14 H5.5 Z\"\npath d=\"M14.2 9.6 A3.4 3.4 0 0 1 14.2 14.4\"\npath d=\"M16.5 7.4 A6.4 6.4 0 0 1 16.5 16.6\""},
    // 显示蓝牙设备: the Bluetooth rune
    {"bluetooth", 0x0A7CFF,
     "path d=\"M8 8.6 L15.4 15.2 L12 18.6 V5.4 L15.4 8.8 L8 15.4\""},
    // 低电量提醒线: a nearly empty battery
    {"battery", 0xFF453A,
     "rect 4.8 8.5 12.4 7 r=1.8\nline 19.2 10.8 19.2 13.2\nrect 6.8 10.5 2.6 3 r=0.6 fill=#FFFFFF"},
    // 用封面颜色做背景: a record sleeve
    {"album", 0xAF52DE,
     "rect 5 5 14 14 r=2.5\ncircle 12 12 3.3\ncircle 12 12 0.9 fill=#FFFFFF"},
    // 显示播放来源: a music note
    {"music", 0xFF9500,
     "path d=\"M10.1 16.4 V6.6 L17 5 V14.8\"\ncircle 8.3 16.4 1.9\ncircle 15.2 14.8 1.9"},
};

inline const SettingsRowIcon* FindSettingsRowIcon(std::string_view key) noexcept {
    for (const auto& icon : kSettingsRowIcons)
        if (key == icon.key) return &icon;
    return nullptr;
}

// The canvas script for a row icon (empty for an unknown key).
inline std::string SettingsRowIconCanvas(std::string_view key) {
    const SettingsRowIcon* icon = FindSettingsRowIcon(key);
    if (!icon) return {};
    char base[8]{};
    std::snprintf(base, sizeof(base), "#%06X", static_cast<unsigned>(icon->rgb & 0xFFFFFFu));
    std::string out = "viewbox 24 24\n";
    out += "rect 0 0 24 24 r=6 fill=";
    out += base;
    out += "\nrect 0.3 0.3 23.4 23.4 r=5.7 stroke=#000000 width=0.6 opacity=0.08\n";
    std::string_view glyph = icon->glyph;
    while (!glyph.empty()) {
        const size_t nl = glyph.find('\n');
        std::string line(glyph.substr(0, nl));
        glyph = nl == std::string_view::npos ? std::string_view{} : glyph.substr(nl + 1);
        if (line.empty()) continue;
        if (const size_t at = line.find("fill=BASE"); at != std::string::npos) line.replace(at + 5, 4, base);
        if (line.find("fill=") == std::string::npos && line.find("color=") == std::string::npos)
            line += " stroke=#FFFFFF width=1.7";
        out += line;
        out += '\n';
    }
    return out;
}

} // namespace pulse::desktop::plugin
