// toolbar_picker.h — the 「自定义工具栏」 popover (toolbar picker T3).
// It hangs under the toolbar's More button and
// shows the toolbar's tools, the selection chip's actions and the pinned apps
// as tiles, one page each. Tiles are dragged to reorder (within their
// section). Every change applies at once through the callbacks, so the
// toolbar behind the popover updates while it is open; there is no Cancel.
#pragma once

#include <d2d1.h>
#include <windows.h>

#include "../common/menu_icon.h"

#include <functional>
#include <string>
#include <vector>

namespace pulse::ui {

struct ToolbarPickerTile {
    std::wstring glyph;          // tools: the command icon
    std::wstring label;
    std::wstring caption;        // apps: the file types it opens (tooltip line)
    std::wstring keywords;       // tools: extra search words (shortcut, stable id)
    common::MenuIconRef icon;    // apps: the program icon (instead of glyph)
    std::wstring badge;          // an accent note under the name (apps: 此文件夹常用; selection: 多选时)
    std::wstring value;          // apps: the program path, returned untouched
    int section = 0;             // tools: index into ToolbarCustomizeSpec::sections
    int tool = -1;               // selection: the same command on the tools page (pins index)
    bool checked = false;        // on the toolbar
    bool toggle = false;         // tools: the tile carries a switch for its setting
    bool toggle_on = false;
    bool hidden = false;         // not offered (kept for its index; never shown)
};
using ToolbarCustomizeRow = ToolbarPickerTile;

struct ToolbarCustomizeSpec {
    std::wstring title;          // 自定义工具栏
    std::wstring done_text;      // 完成
    std::wstring tools_tab;      // 工具
    std::wstring apps_tab;       // 应用
    std::wstring sel_tab;        // 选中时
    std::wstring sel_desc;
    std::wstring on_toolbar_text;   // selection tile whose tool is pinned: 已在工具栏上
    std::wstring tools_full_text;   // the toolbar holds max_tools tools
    std::wstring tools_desc;
    std::wstring apps_desc;
    std::wstring search_hint;    // 搜索工具
    std::wstring empty_text;     // no tool matches the search
    std::wstring apps_full_text; // the toolbar holds max_apps apps
    std::vector<std::wstring> sections;
    std::vector<ToolbarPickerTile> pins;     // tools page
    size_t max_tools = 0;                    // 0: no limit
    // Display order of pins / selection (indices; empty = as listed). The
    // checked tiles in this order are the toolbar's / the chip's order.
    std::vector<size_t> pins_order;
    std::vector<ToolbarPickerTile> selection;   // selection page (选中时)
    std::vector<size_t> selection_order;
    // Apps page (FillToolbarAppRows): pinned apps (checked), then suggestions.
    std::wstring add_app_text;   // 添加软件…
    std::wstring add_app_caption;
    std::vector<ToolbarPickerTile> apps;
    size_t max_apps = 8;
    // 添加软件…: picks a program (owner = the popover). True fills `row`, which is
    // appended checked unless a row with the same value exists (that one is checked).
    std::function<bool(HWND owner, ToolbarPickerTile& row)> add_app;
    // Reading the app rows takes a while (file types, suggestions), so the
    // popover can fill them later: when the apps page is first needed, or
    // once the opening animation has settled. apps_pinned stands in for the
    // tab's count until then.
    std::function<void(ToolbarCustomizeSpec& spec)> load_apps;
    size_t apps_pinned = 0;

    int page = 0;                // 0 tools, 1 selection, 2 apps
    RECT anchor{};               // screen rect of the button the popover hangs from

    // Live edits. on_tools / on_selection: the checked pins / selection
    // indices, in display order (a tile was checked, unchecked or moved).
    // on_switch: flip the setting behind pins[index], returns its new state.
    // on_apps: the app tiles in display order (checked state, order or a new row).
    std::function<void(const std::vector<size_t>& pinned)> on_tools;
    std::function<void(const std::vector<size_t>& shown)> on_selection;
    std::function<bool(size_t index)> on_switch;
    std::function<void(const std::vector<ToolbarPickerTile>& apps)> on_apps;

    // T3c, the toolbar beside the popover.
    // on_preview: the tools page's hovered tile that is not on the toolbar
    // (pins index; -1 none), for a dashed preview pin there.
    std::function<void(int index)> on_preview;
    // Footer row 快捷键 (keys_text, keys_hint the chord): on_shortcuts runs
    // after the popover closes.
    std::wstring keys_text, keys_hint;
    std::function<void()> on_shortcuts;
    // on_link: the hovered (or dragged) tile that is on the toolbar already,
    // so the toolbar can ring its pin (prototype: .tb.linked); page -1 none.
    std::function<void(int page, int index, const std::wstring& value)> on_link;
    // A press on the owner at this screen point (a toolbar pin) keeps the
    // popover open so the pin can be dragged; the owner calls
    // ToolbarPickerActivate() when the press is over.
    std::function<bool(POINT screen)> keep_open;
    // After the toolbar changed outside the popover: refresh the pins'
    // checked and toggle_on and pins_order, the apps' checked (or
    // apps_pinned before they are loaded) and fill apps_order.
    std::function<void(ToolbarCustomizeSpec& spec)> resync;
    std::vector<size_t> apps_order;
    // Separators (tools and apps pages): break_state says whether a separator
    // follows the tile on the toolbar (1), not (0), or none can (-1: not on
    // the toolbar, or its last item); toggle_break adds or removes it.
    // `value` is the tile's value (apps: the program path).
    std::function<int(int page, size_t index, const std::wstring& value)> break_state;
    std::function<void(int page, size_t index, const std::wstring& value)> toggle_break;
    std::wstring add_break_text, remove_break_text;   // shown while the button is hovered
    // The window's theme after on_switch (the 深色主题 tool), so the popover follows.
    std::function<bool()> dark_now;
};

// Runs until the popover closes (Esc, Done, a click elsewhere). Returns at
// once when a picker is already open.
void ShowToolbarPicker(HWND owner, ToolbarCustomizeSpec& spec, bool dark, D2D1_COLOR_F accent);
bool ToolbarPickerOpen() noexcept;
// The owner is done with a press kept by keep_open: resync and take the
// activation back.
void ToolbarPickerActivate();
void CloseToolbarPicker();

} // namespace pulse::ui
