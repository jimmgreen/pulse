// builtin_menu_items.h — Pulse's own context-menu rows and list-row hover
// buttons that the 右键菜单 settings page can hide (#41, #44-⑩).
//
// Open and the cut / copy / delete / rename strip always stay; everything
// listed here can be turned off. Hidden items persist by key in
// context_menu.json ("pulse_items"), so the enum order is free to change.
#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace pulse::app {

// Settings-page order.
enum class BuiltinMenuItem : uint8_t {
    OpenInNewTab,
    CopyName,
    CopyPath,
    Terminal,
    QuickAccess,
    PinWorkspace,
    PinNetwork,
    Tags,
    RecentChanges,
    SelectCommands,
    Undo,
    RowNewTab,
    RowStar,
    RowMore,
    View,
    Sort,
    Group,
    Refresh,
    NewFolder,
    NewTextFile,
    Paste,
    Properties,
    FolderProperties,
    Count
};

constexpr int kBuiltinMenuItemCount = static_cast<int>(BuiltinMenuItem::Count);
// User order of a surface's movable rows (fixed rows always stay on top).
// An empty order means the table order.
using BuiltinMenuOrder = std::vector<BuiltinMenuItem>;
static_assert(kBuiltinMenuItemCount <= 32, "builtin_hidden is a 32-bit mask");

constexpr uint32_t BuiltinMenuBit(BuiltinMenuItem item) {
    return 1u << static_cast<uint32_t>(item);
}

// Stable context_menu.json key ("copy_path", "row_star", ...).
std::wstring_view BuiltinMenuKey(BuiltinMenuItem item);
// Settings-page label in the current UI language.
std::wstring BuiltinMenuLabel(BuiltinMenuItem item);

// List-row hover buttons still shown, as the renderer's mask:
// bit 0 star, bit 1 open in new tab, bit 2 more actions.
constexpr uint32_t kRowActionStar = 1u;
constexpr uint32_t kRowActionNewTab = 2u;
constexpr uint32_t kRowActionMore = 4u;
constexpr uint32_t kRowActionsAll = kRowActionStar | kRowActionNewTab | kRowActionMore;
// Bits 3-8 carry the left-to-right order of the three buttons as 2-bit ids
// (0 star, 1 new tab, 2 more); 0 there means the default order.
constexpr uint32_t kRowActionOrderShift = 3u;
uint32_t RowActionMask(uint32_t builtin_hidden, const BuiltinMenuOrder& row_order = {});

// Settings page: the three surfaces the Pulse menu card switches between.
enum class BuiltinMenuSurface : uint8_t { Item, Background, RowButtons, Count };
// Rows that are shown for reference but can never be hidden.
enum class BuiltinFixedRow : uint8_t { None, Open, Strip };

// One row of a surface, in the order the real menu shows it. `item` is
// Count for a fixed row.
struct BuiltinMenuRow {
    BuiltinMenuItem item;
    BuiltinFixedRow fixed;
    const wchar_t* glyph;
    const wchar_t* shortcut;
    bool separator_after;
    bool submenu;
};

struct BuiltinMenuRows {
    const BuiltinMenuRow* rows;
    size_t count;
};
BuiltinMenuRows BuiltinMenuSurfaceRows(BuiltinMenuSurface surface);
// Label of a surface row (fixed rows included).
std::wstring BuiltinMenuRowLabel(const BuiltinMenuRow& row);

// The movable rows of `surface` in table order.
BuiltinMenuOrder BuiltinMenuDefaultOrder(BuiltinMenuSurface surface);
// `saved` with foreign and repeated items dropped and missing ones put back
// after their table predecessor, so new items land where they belong.
BuiltinMenuOrder NormalizeBuiltinMenuOrder(BuiltinMenuSurface surface, const BuiltinMenuOrder& saved);
// The surface rows with the movable rows in `order`. Separators belong to
// the slot, not to the item: a moved row takes the separator of the place
// it lands in, so the groups keep their shape.
std::vector<BuiltinMenuRow> BuiltinMenuOrderedRows(BuiltinMenuSurface surface, const BuiltinMenuOrder& order);
// Rows still shown for `builtin_hidden` in `order`, with separators moved the
// way ApplyBuiltinMenuPrefs moves them (settings preview).
struct BuiltinMenuVisibleRow {
    BuiltinMenuRow row;
    bool separator_after;
};
std::vector<BuiltinMenuVisibleRow> BuiltinMenuVisibleRows(BuiltinMenuSurface surface,
                                                          uint32_t builtin_hidden,
                                                          const BuiltinMenuOrder& order = {});
// True when the item shows in both the file and the blank-area menu, so one
// switch changes both.
bool BuiltinMenuShared(BuiltinMenuItem item);

// One-click presets, as builtin_hidden masks. Full is the default.
enum class BuiltinMenuPreset : uint8_t { Slim, Standard, Full, Count };
uint32_t BuiltinMenuPresetHidden(BuiltinMenuPreset preset);
// The preset whose mask equals `builtin_hidden`, or Count for 自定义.
BuiltinMenuPreset BuiltinMenuPresetFor(uint32_t builtin_hidden);

} // namespace pulse::app
