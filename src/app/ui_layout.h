// ui_layout.h - Interface layouts (ui_modes_mockup): three starting points
// (专注 zen / 标准 standard / 工作台 pro) over one set of window-structure
// options. The options live in AppPrefs as plain fields; a layout is a view of
// them. "Custom, based on X" is the difference from X's preset, never stored.
#pragma once
#include "app_prefs.h"
#include "title_buttons.h"
#include "../ui/details_column_set.h"
#include "../ui/view_layout.h"
#include <string>
#include <string_view>
#include <vector>

namespace pulse::app {

enum class UiLayoutKey : int {
    Sidebar,        // 侧边栏: 0 固定, 1 浮起, 2 隐藏
    SidebarLite,    // 侧边栏内容: 精简
    Tabs,           // 标签页
    AddressInTitle, // 地址栏位置: 并入标题栏
    Commands,       // 命令: 0 完整命令栏, 1 并入地址栏, 2 收起
    ToolbarLabels,  // 按钮文字
    StatusBar,      // 状态栏
    Tray,           // 常驻暂存入口
    SelectionFloat, // 选中操作浮在底部
    Density,        // 行距: 0 紧凑, 1 标准, 2 宽松
    View,           // 默认视图
    Zebra,          // 斑马纹
    MinimalColumns, // 最简列 (名称 + 修改日期)
    Badges,         // 缩略图角标
    Rounded,        // 大圆角
    TitleButtons,   // 标题栏按钮
    Count,
};
inline constexpr int kUiLayoutKeyCount = static_cast<int>(UiLayoutKey::Count);

inline constexpr int kRowHeightCompact = 28, kRowHeightStandard = 34, kRowHeightRoomy = 40;
inline constexpr uint32_t kDetailsColumnsMinimal = ui::kDetailsColumnModified;

struct UiLayout {
    int sidebar_mode = 0;
    bool sidebar_lite = false;
    bool show_tabs = true;
    bool address_in_title = false;
    int command_mode = 0;
    bool toolbar_labels = true;
    bool status_bar = true;
    bool tray_pinned = true;
    bool selection_float = false;
    int density = 1;
    ui::ViewMode view = ui::ViewMode::Details;
    bool zebra = true;
    bool minimal_columns = false;
    bool badges = true;
    bool rounded = false;
    std::wstring title_buttons = L"win,layout,settings,|,theme,-ai";
};

inline bool IsUiModeId(std::wstring_view mode) noexcept {
    return mode == L"zen" || mode == L"standard" || mode == L"pro";
}

// 工作台 is today's Pulse: every value equals the AppPrefs default, so a
// profile from before the modes reads as 工作台 with nothing changed.
inline UiLayout UiPreset(std::wstring_view mode) {
    UiLayout l;
    if (mode == L"zen") {
        l.sidebar_mode = 1; l.sidebar_lite = true; l.show_tabs = false; l.address_in_title = true;
        l.command_mode = 2; l.toolbar_labels = false; l.status_bar = false; l.tray_pinned = false;
        l.selection_float = true; l.density = 2; l.view = ui::ViewMode::MediumIcons; l.zebra = false;
        l.minimal_columns = true; l.badges = false; l.rounded = true;
        l.title_buttons = L"layout,settings,-theme,-win,-ai";
    } else if (mode == L"standard") {
        l.sidebar_lite = true; l.command_mode = 1; l.status_bar = false; l.tray_pinned = false;
        l.selection_float = true; l.zebra = false;
        l.title_buttons = L"layout,settings,|,theme,-win,-ai";
    }
    l.title_buttons = NormalizeTitleButtons(l.title_buttons);
    return l;
}

inline int DensityOfRowHeight(int row_height) noexcept {
    return row_height <= kRowHeightCompact ? 0 : row_height >= kRowHeightRoomy ? 2 : 1;
}
inline int RowHeightOfDensity(int density) noexcept {
    return density <= 0 ? kRowHeightCompact : density >= 2 ? kRowHeightRoomy : kRowHeightStandard;
}

inline UiLayout CurrentUiLayout(const AppPrefs& p) {
    UiLayout l;
    l.sidebar_mode = p.ui_sidebar_mode;
    l.sidebar_lite = p.ui_sidebar_lite;
    l.show_tabs = p.ui_show_tabs;
    l.address_in_title = p.ui_address_in_title;
    l.command_mode = p.ui_command_mode;
    l.toolbar_labels = p.ui_toolbar_labels;
    l.status_bar = p.ui_status_bar;
    l.tray_pinned = !p.tray_hide_when_empty;
    l.selection_float = p.ui_selection_float;
    l.density = DensityOfRowHeight(p.row_height);
    l.view = p.folder_views.Default();
    l.zebra = p.list_zebra_rows;
    l.minimal_columns = p.details_columns == kDetailsColumnsMinimal;
    l.badges = p.list_thumbnail_badges;
    l.rounded = p.ui_rounded;
    l.title_buttons = NormalizeTitleButtons(p.ui_title_buttons);
    return l;
}

inline bool UiLayoutKeyDiffers(const UiLayout& a, const UiLayout& b, UiLayoutKey key) {
    switch (key) {
        case UiLayoutKey::Sidebar: return a.sidebar_mode != b.sidebar_mode;
        case UiLayoutKey::SidebarLite: return a.sidebar_lite != b.sidebar_lite;
        case UiLayoutKey::Tabs: return a.show_tabs != b.show_tabs;
        case UiLayoutKey::AddressInTitle: return a.address_in_title != b.address_in_title;
        case UiLayoutKey::Commands: return a.command_mode != b.command_mode;
        case UiLayoutKey::ToolbarLabels: return a.toolbar_labels != b.toolbar_labels;
        case UiLayoutKey::StatusBar: return a.status_bar != b.status_bar;
        case UiLayoutKey::Tray: return a.tray_pinned != b.tray_pinned;
        case UiLayoutKey::SelectionFloat: return a.selection_float != b.selection_float;
        case UiLayoutKey::Density: return a.density != b.density;
        case UiLayoutKey::View: return a.view != b.view;
        case UiLayoutKey::Zebra: return a.zebra != b.zebra;
        case UiLayoutKey::MinimalColumns: return a.minimal_columns != b.minimal_columns;
        case UiLayoutKey::Badges: return a.badges != b.badges;
        case UiLayoutKey::Rounded: return a.rounded != b.rounded;
        case UiLayoutKey::TitleButtons: return a.title_buttons != b.title_buttons;
        case UiLayoutKey::Count: break;
    }
    return false;
}

// Keys where the current options differ from the preset of prefs.ui_mode,
// in the order above. Like the mockup, the sidebar's lite option does not
// count while the sidebar is hidden, nor button labels without a toolbar.
inline std::vector<UiLayoutKey> UiLayoutDiff(const AppPrefs& p) {
    const UiLayout now = CurrentUiLayout(p), preset = UiPreset(p.ui_mode);
    std::vector<UiLayoutKey> out;
    for (int i = 0; i < kUiLayoutKeyCount; ++i) {
        const auto key = static_cast<UiLayoutKey>(i);
        if (key == UiLayoutKey::SidebarLite && now.sidebar_mode == 2 && preset.sidebar_mode == 2) continue;
        if (key == UiLayoutKey::ToolbarLabels && now.command_mode == 2 && preset.command_mode == 2) continue;
        if (UiLayoutKeyDiffers(now, preset, key)) out.push_back(key);
    }
    return out;
}

// Writes a layout into the preferences. The default view goes to every
// folder (FolderViewPrefs::ApplyToAll) when it changes or `all_folders` asks;
// columns are only reset when leaving or entering the minimal set, so a
// custom column choice survives a preset that does not want minimal columns.
inline void ApplyUiLayout(AppPrefs& p, const UiLayout& l, bool all_folders = false) {
    p.ui_sidebar_mode = std::clamp(l.sidebar_mode, 0, 2);
    p.ui_sidebar_lite = l.sidebar_lite;
    p.ui_show_tabs = l.show_tabs;
    p.ui_address_in_title = l.address_in_title;
    p.ui_command_mode = std::clamp(l.command_mode, 0, 2);
    p.ui_toolbar_labels = l.toolbar_labels;
    p.ui_status_bar = l.status_bar;
    p.tray_hide_when_empty = !l.tray_pinned;
    p.ui_selection_float = l.selection_float;
    if (DensityOfRowHeight(p.row_height) != l.density) p.row_height = RowHeightOfDensity(l.density);
    if (all_folders || p.folder_views.Default() != l.view) p.folder_views.ApplyToAll(l.view);
    p.list_zebra_rows = l.zebra;
    const bool minimal = p.details_columns == kDetailsColumnsMinimal;
    if (l.minimal_columns && !minimal) p.details_columns = kDetailsColumnsMinimal;
    else if (!l.minimal_columns && minimal) p.details_columns = ui::kDetailsColumnsDefault;
    p.list_thumbnail_badges = l.badges;
    p.ui_rounded = l.rounded;
    p.ui_title_buttons = NormalizeTitleButtons(l.title_buttons);
}

// Picking a starting point (installer, 界面 flyout, settings, Alt+1..3):
// every option takes the preset's value, the view in all folders.
inline void ApplyUiPreset(AppPrefs& p, std::wstring_view mode) {
    if (!IsUiModeId(mode)) return;
    p.ui_mode = std::wstring(mode);
    ApplyUiLayout(p, UiPreset(mode), true);
}

} // namespace pulse::app
