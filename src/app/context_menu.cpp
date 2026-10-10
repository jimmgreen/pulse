// context_menu.cpp — See context_menu.h.
#include "context_menu.h"
#include "app_model.h"
#include "../common/localization.h"
#include "../common/path_utils.h"
#include "../common/display_path.h"
#include "../fs/fs_enum.h"
#include "../ipc/ctx_menu_util.h"
#include <windows.h>
#include <algorithm>
#include <cwchar>
#include <cwctype>
#include <string_view>
#include <unordered_set>

namespace pulse::app {

namespace {

// Segoe Fluent Icons codepoints (same font strategy as ui_renderer.cpp).
constexpr const wchar_t* kGlyphOpen = L"\xE8E5";
constexpr const wchar_t* kGlyphCut = L"\xE8C6";
constexpr const wchar_t* kGlyphCopy = L"\xE8C8";
constexpr const wchar_t* kGlyphPaste = L"\xE77F";
constexpr const wchar_t* kGlyphDelete = L"\xE74D";
constexpr const wchar_t* kGlyphRename = L"\xE8AC";
constexpr const wchar_t* kGlyphProperties = L"\xE946";
constexpr const wchar_t* kGlyphTerminal = L"\xE756";
constexpr const wchar_t* kGlyphLink = L"\xE71B";
constexpr const wchar_t* kGlyphUndo = L"\xE7A7";
constexpr const wchar_t* kGlyphNewFolder = L"\xE8F4";
constexpr const wchar_t* kGlyphNewFile = L"\xE8A5";
constexpr const wchar_t* kGlyphFolder = L"\xE8B7";
constexpr const wchar_t* kGlyphOpenInNewTab = L"\xE8A7";
constexpr const wchar_t* kGlyphSearch = L"\xE721";
constexpr const wchar_t* kGlyphTag = L"\xE8EC";
constexpr const wchar_t* kGlyphSettings = L"\xE713";
constexpr const wchar_t* kGlyphRecycle = L"\xE75C";
constexpr const wchar_t* kGlyphSelectAll = L"\xE8B3";
constexpr const wchar_t* kGlyphInvert = L"\xE7A1";
constexpr const wchar_t* kGlyphWildcard = L"\xE71C";
constexpr const wchar_t* kGlyphViewMenu = L"\xE8A9";
constexpr const wchar_t* kGlyphSortMenu = L"\xE8CB";
constexpr const wchar_t* kGlyphGroupMenu = L"\xF168";

ui::FluentMenuItem Item(int cmd, const wchar_t* text, const wchar_t* glyph,
                        const wchar_t* shortcut = nullptr, bool enabled = true) {
    ui::FluentMenuItem it;
    it.command = cmd;
    it.text = text;
    it.glyph = glyph;
    if (shortcut) it.shortcut = shortcut;
    it.enabled = enabled;
    it.mnemonic = BuiltinMenuMnemonic(cmd);
    // Command-less flyout headers: 查看(V) / 排序方式(O) / 分组依据(P).
    if (cmd == CmdNone && glyph) {
        if (std::wcscmp(glyph, kGlyphViewMenu) == 0) it.mnemonic = L'V';
        else if (std::wcscmp(glyph, kGlyphSortMenu) == 0) it.mnemonic = L'O';
        else if (std::wcscmp(glyph, kGlyphGroupMenu) == 0) it.mnemonic = L'P';
    }
    return it;
}

ui::FluentMenuItem UndoItem(bool can_undo, const std::wstring& undo_label) {
    auto it = Item(CmdUndo,
        can_undo && !undo_label.empty() ? undo_label.c_str()
                                        : l10n::Get(l10n::StringId::Undo).c_str(),
        kGlyphUndo, L"Ctrl+Z", can_undo);
    it.separator_after = false;
    return it;
}

} // namespace

std::vector<ui::FluentMenuItem> BuildItemMenu(bool can_undo, const std::wstring& undo_label,
                                              bool folder) {
    std::vector<ui::FluentMenuItem> items;
    items.push_back(Item(CmdOpen, l10n::Get(l10n::StringId::Open).c_str(), kGlyphOpen));

    // 剪切/复制/删除/重命名 live on their shortcuts; collapse them into one
    // icon-button row (like the tag swatch strip) to make room for the
    // Explorer verbs merged below.
    ui::FluentMenuItem strip;
    strip.command = CmdNone; // row itself does nothing; buttons carry commands
    strip.enabled = true;
    strip.quick_swatches = {
        { CmdCut,    {}, false, false, kGlyphCut,    BuiltinMenuMnemonic(CmdCut) },
        { CmdCopy,   {}, false, false, kGlyphCopy,   BuiltinMenuMnemonic(CmdCopy) },
        { CmdDelete, {}, false, false, kGlyphDelete, BuiltinMenuMnemonic(CmdDelete) },
        { CmdRename, {}, false, false, kGlyphRename, BuiltinMenuMnemonic(CmdRename) },
    };
    strip.separator_after = true;
    items.push_back(std::move(strip));

    if (folder)
        items.push_back(Item(CmdOpenInNewTab, l10n::Get(l10n::StringId::OpenNewTab).c_str(), kGlyphOpenInNewTab));
    items.push_back(Item(CmdCopyPath, l10n::Get(l10n::StringId::CopyPath).c_str(), kGlyphLink, L"Ctrl+Shift+C"));
    items.push_back(Item(CmdOpenTerminal, l10n::Get(l10n::StringId::OpenTerminalHere).c_str(), kGlyphTerminal));
    items.push_back(Item(CmdProperties, l10n::Get(l10n::StringId::Properties).c_str(), kGlyphProperties, L"Alt+Enter"));
    items.back().separator_after = true;
    items.push_back(Item(CmdPinWorkspace, l10n::Get(l10n::StringId::PinWorkspace).c_str(), kGlyphFolder));
    items.push_back(Item(CmdPinNetwork, l10n::Get(l10n::StringId::PinNetwork).c_str(), kGlyphLink));
    items.back().separator_after = true;
    items.push_back(Item(CmdTags, l10n::Get(l10n::StringId::TagsEllipsis).c_str(), kGlyphTag));
    items.back().separator_after = true;
    items.push_back(UndoItem(can_undo, undo_label));
    return items;
}

BuiltinMenuItem BuiltinItemForCommand(int command) {
    switch (command) {
    case CmdOpenInNewTab: return BuiltinMenuItem::OpenInNewTab;
    case CmdCopyPath: return BuiltinMenuItem::CopyPath;
    case CmdOpenTerminal: return BuiltinMenuItem::Terminal;
    case CmdPinQuickAccess:
    case CmdUnpinQuickAccess: return BuiltinMenuItem::QuickAccess;
    case CmdPinWorkspace: return BuiltinMenuItem::PinWorkspace;
    case CmdPinNetwork: return BuiltinMenuItem::PinNetwork;
    case CmdTags: return BuiltinMenuItem::Tags; // swatch strip and 标签...
    case CmdViewRecentChanges: return BuiltinMenuItem::RecentChanges;
    case CmdSelectAll:
    case CmdInvertSelection:
    case CmdSelectWildcard: return BuiltinMenuItem::SelectCommands;
    case CmdUndo: return BuiltinMenuItem::Undo;
    case CmdRefresh: return BuiltinMenuItem::Refresh;
    case CmdNewFolder: return BuiltinMenuItem::NewFolder;
    case CmdNewTextFile: return BuiltinMenuItem::NewTextFile;
    case CmdPaste: return BuiltinMenuItem::Paste;
    case CmdProperties: return BuiltinMenuItem::Properties;
    case CmdFolderProperties: return BuiltinMenuItem::FolderProperties;
    default: return BuiltinMenuItem::Count;
    }
}

namespace {
// 查看 / 排序方式 / 分组 are command-less flyout headers built by
// AppendBackgroundViewCommands; their glyph is what identifies them.
BuiltinMenuItem BuiltinItemForRow(const ui::FluentMenuItem& item) {
    if (item.command != CmdNone) return BuiltinItemForCommand(item.command);
    if (item.children.empty()) return BuiltinMenuItem::Count;
    if (item.glyph == kGlyphViewMenu) return BuiltinMenuItem::View;
    if (item.glyph == kGlyphSortMenu) return BuiltinMenuItem::Sort;
    if (item.glyph == kGlyphGroupMenu) return BuiltinMenuItem::Group;
    return BuiltinMenuItem::Count;
}
} // namespace

namespace {
// Consecutive rows owned by one movable item (the tag swatches and 标签...,
// the three selection commands) move as one block.
void ReorderBuiltinRows(std::vector<ui::FluentMenuItem>& items, const BuiltinMenuOrder& order) {
    struct Block { size_t first, count, rank; };
    auto rank_of = [&](BuiltinMenuItem item) {
        const auto it = std::find(order.begin(), order.end(), item);
        return it == order.end() ? order.size() : static_cast<size_t>(it - order.begin());
    };
    std::vector<Block> blocks;
    for (size_t i = 0; i < items.size();) {
        const BuiltinMenuItem owner = BuiltinItemForRow(items[i]);
        const size_t rank = owner == BuiltinMenuItem::Count ? order.size() : rank_of(owner);
        size_t end = i + 1;
        if (rank < order.size()) {
            while (end < items.size() && BuiltinItemForRow(items[end]) == owner) ++end;
            blocks.push_back({ i, end - i, rank });
        }
        i = end;
    }
    auto sorted = blocks;
    std::stable_sort(sorted.begin(), sorted.end(), [](const Block& a, const Block& b) { return a.rank < b.rank; });
    bool moved = false;
    for (size_t b = 0; b < blocks.size(); ++b) moved |= sorted[b].first != blocks[b].first;
    if (!moved) return;
    std::vector<bool> slot_separator(blocks.size());
    for (size_t b = 0; b < blocks.size(); ++b)
        slot_separator[b] = items[blocks[b].first + blocks[b].count - 1].separator_after;
    std::vector<ui::FluentMenuItem> out;
    out.reserve(items.size());
    size_t next = 0;
    for (size_t i = 0; i < items.size();) {
        if (next < blocks.size() && i == blocks[next].first) {
            const Block& source = sorted[next];
            for (size_t k = 0; k < source.count; ++k) out.push_back(std::move(items[source.first + k]));
            out.back().separator_after = slot_separator[next];
            i += blocks[next].count;
            ++next;
        } else {
            out.push_back(std::move(items[i++]));
        }
    }
    items = std::move(out);
}
} // namespace

void ApplyBuiltinMenuPrefs(std::vector<ui::FluentMenuItem>& items,
                           const ContextMenuPrefs& prefs,
                           BuiltinMenuSurface surface) {
    if (surface < BuiltinMenuSurface::Count && prefs.BuiltinOrderCustom(surface))
        ReorderBuiltinRows(items, prefs.BuiltinOrder(surface));
    if (prefs.builtin_hidden == 0 || items.empty()) return;
    auto hidden = [&](const ui::FluentMenuItem& item) {
        const BuiltinMenuItem owner = BuiltinItemForRow(item);
        return owner != BuiltinMenuItem::Count && !prefs.BuiltinVisible(owner);
    };
    const bool last_dropped = hidden(items.back());
    std::vector<ui::FluentMenuItem> kept;
    kept.reserve(items.size());
    for (auto& item : items) {
        if (hidden(item)) {
            if (item.separator_after && !kept.empty()) kept.back().separator_after = true;
            continue;
        }
        kept.push_back(std::move(item));
    }
    // The old last row never ended in a separator; keep it that way.
    if (last_dropped && !kept.empty()) kept.back().separator_after = false;
    items = std::move(kept);
}

std::wstring RecentChangesMenuPath(const Tab& tab, bool background) {
    auto filesystem = [](const std::wstring& path) { return !path.empty() && !fs::IsVirtualPath(path); };
    if (background) return filesystem(tab.current_path) ? tab.current_path : std::wstring{};
    if (!tab.snapshot) return {};
    const auto selected = tab.SelectedIndices();
    if (selected.size() != 1 || selected[0] < 0 ||
        static_cast<size_t>(selected[0]) >= tab.EntryCount()) return {};
    const auto& entry = tab.EntryAt(static_cast<size_t>(selected[0]));
    if (entry.change_record_only || !entry.recycle_path.empty()) return {};
    if (!entry.link_target.empty())
        return entry.link_target_is_dir && filesystem(entry.link_target) ? entry.link_target : std::wstring{};
    if (!entry.is_dir) return {};
    if (!entry.full_path.empty()) return filesystem(entry.full_path) ? entry.full_path : std::wstring{};
    if (!filesystem(tab.current_path)) return {};
    auto path = tab.current_path;
    if (!path.ends_with(L"\\")) path += L"\\";
    return path + entry.name;
}

void AppendRecentChangesCommand(std::vector<ui::FluentMenuItem>& items,
                                const std::wstring& path) {
    if (path.empty() || fs::IsVirtualPath(path)) return;
    auto item = Item(CmdViewRecentChanges, l10n::Get(l10n::StringId::ChangeView).c_str(), L"\xE81C");
    // Where the settings preview shows it (builtin_menu_items.cpp): right
    // after the cut / copy / delete / rename strip on an item, right above
    // 撤销 on the blank area, so moved rows land where the preview says.
    const auto strip = std::find_if(items.begin(), items.end(), [](const ui::FluentMenuItem& row) {
        return row.command == CmdNone && row.children.empty() && !row.quick_swatches.empty();
    });
    if (strip != items.end()) { items.insert(strip + 1, std::move(item)); return; }
    const auto undo = std::find_if(items.begin(), items.end(), [](const ui::FluentMenuItem& row) {
        return row.command == CmdUndo;
    });
    items.insert(undo, std::move(item));
}

std::vector<ui::FluentMenuItem> BuildRecycleItemMenu(bool can_undo,
                                                     const std::wstring& undo_label) {
    std::vector<ui::FluentMenuItem> items;
    items.push_back(Item(CmdRestoreRecycle, l10n::Get(l10n::StringId::Restore).c_str(), kGlyphUndo));
    items.push_back(Item(CmdDelete, l10n::Get(l10n::StringId::PermanentDelete).c_str(), kGlyphDelete, L"Del"));
    items.back().separator_after = true;
    items.push_back(Item(CmdCopyPath, l10n::Get(l10n::StringId::CopyPath).c_str(), kGlyphLink, L"Ctrl+Shift+C"));
    items.back().separator_after = true;
    items.push_back(UndoItem(can_undo, undo_label));
    return items;
}

std::vector<ui::FluentMenuItem> BuildRecycleBackgroundMenu(bool can_undo,
                                                           const std::wstring& undo_label,
                                                           bool can_empty) {
    std::vector<ui::FluentMenuItem> items;
    items.push_back(Item(CmdEmptyRecycle, l10n::Get(l10n::StringId::EmptyRecycleBin).c_str(),
                         kGlyphRecycle, nullptr, can_empty));
    items.push_back(Item(CmdRestoreAllRecycle, l10n::Get(l10n::StringId::RestoreAll).c_str(),
                         kGlyphUndo, nullptr, can_empty));
    items.back().separator_after = true;
    items.push_back(UndoItem(can_undo, undo_label));
    return items;
}

std::vector<ui::FluentMenuItem> BuildRecyclePlaceMenu(bool can_empty) {
    std::vector<ui::FluentMenuItem> items;
    items.push_back(Item(CmdOpenRecycle, l10n::Get(l10n::StringId::Open).c_str(), kGlyphOpen));
    items.push_back(Item(CmdEmptyRecycle, l10n::Get(l10n::StringId::EmptyRecycleBin).c_str(),
                         kGlyphRecycle, nullptr, can_empty));
    return items;
}


std::vector<ui::FluentMenuItem> BuildBackgroundMenu(bool can_paste, bool can_undo,
                                                    const std::wstring& undo_label) {
    std::vector<ui::FluentMenuItem> items;
    items.push_back(Item(CmdNewFolder, l10n::Get(l10n::StringId::NewFolder).c_str(), kGlyphNewFolder, L"Ctrl+Shift+N"));
    items.push_back(Item(CmdNewTextFile, l10n::Get(l10n::StringId::NewTextDocument).c_str(), kGlyphNewFile));
    items.back().separator_after = true;
    items.push_back(Item(CmdPaste, l10n::Get(l10n::StringId::Paste).c_str(), kGlyphPaste, L"Ctrl+V", can_paste));
    items.push_back(Item(CmdCopyPath, l10n::Get(l10n::StringId::CopyPath).c_str(), kGlyphLink, L"Ctrl+Shift+C"));
    items.back().separator_after = true;
    items.push_back(Item(CmdSelectAll, l10n::Get(l10n::StringId::SelectAll).c_str(), kGlyphSelectAll, L"Ctrl+A"));
    items.push_back(Item(CmdInvertSelection, l10n::Get(l10n::StringId::InvertSelection).c_str(), kGlyphInvert, L"Ctrl+I"));
    items.push_back(Item(CmdSelectWildcard, l10n::Get(l10n::StringId::SelectWildcard).c_str(), kGlyphWildcard, L"Ctrl+Shift+A"));
    items.back().separator_after = true;
    items.push_back(Item(CmdOpenTerminal, l10n::Get(l10n::StringId::OpenTerminalHere).c_str(), kGlyphTerminal));
    items.back().separator_after = true;
    items.push_back(Item(CmdPinWorkspace, l10n::Get(l10n::StringId::PinWorkspace).c_str(), kGlyphFolder));
    items.push_back(Item(CmdPinNetwork, l10n::Get(l10n::StringId::PinNetwork).c_str(), kGlyphLink));
    items.back().separator_after = true;
    items.push_back(UndoItem(can_undo, undo_label));
    return items;
}

std::vector<ui::FluentMenuItem> BuildBreadcrumbMenu(bool filesystem) {
    std::vector<ui::FluentMenuItem> items;
    items.push_back(Item(CmdOpenInNewTab, l10n::Get(l10n::StringId::OpenNewTab).c_str(), kGlyphOpenInNewTab));
    items.push_back(Item(CmdOpen, l10n::Get(l10n::StringId::Open).c_str(), kGlyphOpen));
    items.back().separator_after = true;
    items.push_back(Item(CmdCopyPath, l10n::Get(l10n::StringId::CopyPath).c_str(), kGlyphLink));
    if (filesystem) {
        items.push_back(Item(CmdCopy, l10n::Get(l10n::StringId::Copy).c_str(), kGlyphCopy));
        items.back().separator_after = true;
        items.push_back(Item(CmdOpenTerminal, l10n::Get(l10n::StringId::OpenTerminalHere).c_str(), kGlyphTerminal));
        items.push_back(Item(CmdProperties, l10n::Get(l10n::StringId::Properties).c_str(), kGlyphProperties));
    }
    return items;
}

std::vector<ui::FluentMenuItem> BuildSortMenu(const BackgroundViewOptions& options) {
    auto sort = Item(CmdNone, l10n::Get(l10n::StringId::SortBy).c_str(), L"\xE8CB",
                     nullptr, options.can_sort);
    struct SortRow { int command; ui::SortColumn column; l10n::StringId label; };
    constexpr SortRow rows[] = {
        { CmdSortName, ui::SortColumn::Name, l10n::StringId::ColumnName },
        { CmdSortModified, ui::SortColumn::Mtime, l10n::StringId::ColumnModified },
        { CmdSortCreated, ui::SortColumn::Created, l10n::StringId::ColumnCreated },
        { CmdSortAccessed, ui::SortColumn::Accessed, l10n::StringId::ColumnAccessed },
        { CmdSortType, ui::SortColumn::Type, l10n::StringId::ColumnType },
        { CmdSortSize, ui::SortColumn::Size, l10n::StringId::ColumnSize },
        { CmdSortPath, ui::SortColumn::Path, l10n::StringId::ColumnPath },
    };
    for (const auto& row : rows) {
        if (row.column == ui::SortColumn::Path && !options.show_path) continue;
        // Search results and the recycle bin carry no creation / access times.
        if ((row.column == ui::SortColumn::Created || row.column == ui::SortColumn::Accessed) &&
            options.show_path) continue;
        auto child = Item(row.command, l10n::Get(row.label).c_str(), L"", nullptr,
                          options.can_sort && !(options.indexed_search &&
                          (row.column == ui::SortColumn::Type || row.column == ui::SortColumn::Path)));
        child.radio_group = true;
        child.radio = options.can_sort && options.sort_column == row.column;
        sort.children.push_back(std::move(child));
    }
    sort.children.back().separator_after = true;
    for (bool ascending : { true, false }) {
        auto child = Item(ascending ? CmdSortAscending : CmdSortDescending,
            l10n::Get(ascending ? l10n::StringId::SortAscending
                               : l10n::StringId::SortDescending).c_str(), L"", nullptr,
            options.can_sort);
        child.radio_group = true;
        child.radio = options.can_sort &&
            (options.sort_direction == ui::SortDirection::Asc) == ascending;
        sort.children.push_back(std::move(child));
    }
    sort.children.back().separator_after = true;
    struct FolderSortRow { int command; int value; l10n::StringId label; };
    constexpr FolderSortRow folder_rows[] = {
        { CmdFolderSortTop, 0, l10n::StringId::FolderSortTop },
        { CmdFolderSortFollow, 1, l10n::StringId::FolderSortFollow },
        { CmdFolderSortMixed, 2, l10n::StringId::FolderSortMixed },
    };
    for (const auto& row : folder_rows) {
        auto child = Item(row.command, l10n::Get(row.label).c_str(), L"", nullptr, options.can_sort);
        child.radio_group = true;
        child.radio = options.can_sort && options.folder_sort == row.value;
        sort.children.push_back(std::move(child));
    }
    return std::move(sort.children);
}

ui::FluentMenuItem BuildShortcutHints() {
    return Item(CmdShortcutHelp, l10n::Get(l10n::StringId::ShortcutHints).c_str(), L"\xE946");
}

ui::FluentMenuItem BuildGroupMenu(const BackgroundViewOptions& options) {
    auto group = Item(CmdNone, l10n::Get(l10n::StringId::GroupBy).c_str(), kGlyphGroupMenu,
                      nullptr, options.can_group);
    struct GroupRow { int command; int value; l10n::StringId label; };
    constexpr GroupRow rows[] = {
        { CmdGroupName, 1, l10n::StringId::GroupByName },
        { CmdGroupDate, 2, l10n::StringId::GroupByDate },
        { CmdGroupType, 3, l10n::StringId::GroupByType },
        { CmdGroupSize, 4, l10n::StringId::GroupBySize },
        { CmdGroupTag, 5, l10n::StringId::GroupByTag },
        { CmdGroupLocation, 6, l10n::StringId::Location },
        { CmdGroupNone, 0, l10n::StringId::GroupNone },
    };
    // Tags need one parent folder; Location only makes sense across folders.
    const int hidden = options.group_virtual ? 5 : 6;
    const int last = options.group_virtual ? 6 : 5;
    for (const auto& row : rows) {
        if (row.value == hidden) continue;
        auto child = Item(row.command, l10n::Get(row.label).c_str(), L"", nullptr, options.can_group);
        child.radio_group = true;
        child.radio = options.can_group && options.group_by == row.value;
        if (row.value == last) child.separator_after = true;
        group.children.push_back(std::move(child));
    }
    if (options.can_apply_group_all && !group.children.empty()) {
        group.children.back().separator_after = true;
        group.children.push_back(Item(CmdApplyGroupToAllFolders,
            l10n::Get(l10n::StringId::ApplyGroupAllFolders).c_str(), L"\xE8B3"));
    }
    return group;
}

void AppendBackgroundViewCommands(std::vector<ui::FluentMenuItem>& items,
                                  const BackgroundViewOptions& options) {
    auto view = Item(CmdNone, l10n::Get(l10n::StringId::View).c_str(), kGlyphViewMenu);
    view.children = BuildViewMenu(options.view_mode, options.details_panel, options.filesystem);
    auto sort = Item(CmdNone, l10n::Get(l10n::StringId::SortBy).c_str(), kGlyphSortMenu, nullptr, options.can_sort);
    sort.children = BuildSortMenu(options);
    auto refresh = Item(CmdRefresh, l10n::Get(l10n::StringId::Refresh).c_str(), L"\xE72C", L"F5");
    refresh.separator_after = true;
    if (options.can_group)
        items.insert(items.begin(), { std::move(view), std::move(sort), BuildGroupMenu(options),
                                      std::move(refresh) });
    else
        items.insert(items.begin(), { std::move(view), std::move(sort), std::move(refresh) });
    if (options.filesystem) {
        if (!items.empty()) items.back().separator_after = true;
        items.push_back(Item(CmdFolderProperties,
            l10n::Get(l10n::StringId::Properties).c_str(), kGlyphProperties));
    }
}

std::vector<ui::FluentMenuItem> BuildNewMenu() {
    std::vector<ui::FluentMenuItem> items;
    items.push_back(Item(CmdNewFolder, l10n::Get(l10n::StringId::Folder).c_str(), kGlyphNewFolder, L"Ctrl+Shift+N"));
    items.push_back(Item(CmdNewTextFile, l10n::Get(l10n::StringId::TextDocument).c_str(), kGlyphNewFile));
    return items;
}

std::vector<ui::FluentMenuItem> BuildSplitMenu(int current_preset) {
    struct Row {
        int cmd;
        const wchar_t* shortcut;
        l10n::StringId label;
        ui::fluent::MenuPictogram pictogram;
    };
    static constexpr Row kRows[] = {
        { CmdLayoutSingle,        L"Ctrl+Alt+1", l10n::StringId::LayoutSingle,     ui::fluent::MenuPictogram::LayoutSingle },
        { CmdLayoutTwoVertical,   L"Ctrl+Alt+2", l10n::StringId::LayoutVertical,   ui::fluent::MenuPictogram::LayoutSideBySide },
        { CmdLayoutTwoHorizontal, nullptr,   l10n::StringId::LayoutHorizontal, ui::fluent::MenuPictogram::LayoutStacked },
        { CmdLayoutThree,         L"Ctrl+Alt+3", l10n::StringId::LayoutThree,      ui::fluent::MenuPictogram::LayoutThree },
        { CmdLayoutFourGrid,      L"Ctrl+Alt+4", l10n::StringId::LayoutFour,       ui::fluent::MenuPictogram::LayoutFour },
    };
    std::vector<ui::FluentMenuItem> items;
    for (const auto& row : kRows) {
        auto it = Item(row.cmd, l10n::Get(row.label).c_str(), L"", row.shortcut);
        it.radio_group = true;
        it.radio = (row.cmd - CmdLayoutSingle == current_preset);
        it.pictogram = row.pictogram;
        items.push_back(std::move(it));
    }
    items.back().separator_after = true;
    items.push_back(Item(CmdCopyToTarget, l10n::Get(l10n::StringId::CopyToTarget).c_str(), kGlyphCopy, L"Ctrl+Alt+C"));
    items.push_back(Item(CmdMoveToTarget, l10n::Get(l10n::StringId::MoveToTarget).c_str(), kGlyphCut, L"Ctrl+Alt+X"));
    return items;
}

std::vector<ui::FluentMenuItem> BuildViewMenu(ui::ViewMode current_mode, bool details_panel,
                                              bool can_apply_all) {
    static constexpr l10n::StringId labels[] = {
        l10n::StringId::ViewExtraLarge, l10n::StringId::ViewLarge,
        l10n::StringId::MediumIcons, l10n::StringId::ViewSmall,
        l10n::StringId::ViewList, l10n::StringId::ViewDetails,
        l10n::StringId::ViewTiles, l10n::StringId::ViewContent,
    };
    static constexpr const wchar_t* glyphs[] = {
        L"\xE7F4", L"\xE7F4", L"\xE7F4", L"\xECA5",
        L"\xEA37", L"\xE8A5", L"\xECA5", L"\xE8FD"
    };
    std::vector<ui::FluentMenuItem> items;
    items.reserve(9);
    // File Explorer's Ctrl+Shift+1..8 (explorer_shortcuts.h).
    static constexpr const wchar_t* keys[] = {
        L"Ctrl+Shift+1", L"Ctrl+Shift+2", L"Ctrl+Shift+3", L"Ctrl+Shift+4",
        L"Ctrl+Shift+5", L"Ctrl+Shift+6", L"Ctrl+Shift+7", L"Ctrl+Shift+8"
    };
    for (int i = 0; i < 8; ++i) {
        auto row = Item(CmdViewBase + i, l10n::Get(labels[i]).c_str(), glyphs[i], keys[i]);
        if (i == 0) row.glyph_scale = 1.16f;
        else if (i == 1) row.glyph_scale = 1.0f;
        else if (i == 2) row.glyph_scale = 0.82f;
        row.radio = ui::ViewModeIndex(current_mode) == i;
        items.push_back(std::move(row));
    }
    items.back().separator_after = true;
    auto panel = Item(CmdDetailsPanel, l10n::Get(l10n::StringId::DetailsPane).c_str(), L"\xE700");
    panel.checked = details_panel;
    items.push_back(std::move(panel));
    if (can_apply_all) {
        items.back().separator_after = true;
        items.push_back(Item(CmdApplyViewToAllFolders,
            l10n::Get(l10n::StringId::ApplyViewAllFolders).c_str(), L"\xE8B3"));
    }
    return items;
}

static std::wstring DisplayPath(const std::wstring& path) {
    return pulse::path::FriendlyPathText(path);
}

static std::wstring FolderTitle(const std::wstring& path) {
    if (path.empty()) return L"This PC";
    std::wstring shown = DisplayPath(path);
    std::wstring_view v = shown;
    if (v.size() > 1 && v.back() == L'\\') v.remove_suffix(1);
    auto pos = v.find_last_of(L"\\/");
    if (pos != std::wstring_view::npos && pos + 1 < v.size())
        return std::wstring(v.substr(pos + 1));
    return std::wstring(v);
}

static bool ContainsI(std::wstring hay, std::wstring needle) {
    for (auto& c : hay) c = static_cast<wchar_t>(std::towlower(c));
    for (auto& c : needle) c = static_cast<wchar_t>(std::towlower(c));
    return needle.empty() || hay.find(needle) != std::wstring::npos;
}

static void TrimInPlace(std::wstring& s) {
    const auto start = s.find_first_not_of(L" \t");
    if (start == std::wstring::npos) {
        s.clear();
        return;
    }
    const auto end = s.find_last_not_of(L" \t");
    s = s.substr(start, end - start + 1);
}

OmnibarQuery ParseOmnibarQuery(const std::wstring& query, bool project_only) {
    OmnibarQuery q;
    q.needle = query;
    if (project_only) {
        q.kind = OmnibarQuery::Kind::Project;
        TrimInPlace(q.needle);
        return q;
    }
    if (!q.needle.empty()) {
        const wchar_t c = q.needle[0];
        if (c == L'>') {
            q.kind = OmnibarQuery::Kind::Command;
            q.prefix = L'>';
            q.needle.erase(0, 1);
        } else if (c == L'?' || c == L'/') {
            q.kind = OmnibarQuery::Kind::Search;
            q.prefix = c;
            q.needle.erase(0, 1);
        }
    }
    TrimInPlace(q.needle);
    return q;
}

bool LooksLikeFilesystemPath(const std::wstring& text) {
    std::wstring t = text;
    TrimInPlace(t);
    if (t.size() >= 2) {
        const wchar_t drive = t[0];
        const bool letter = (drive >= L'A' && drive <= L'Z') || (drive >= L'a' && drive <= L'z');
        if (letter && t[1] == L':') return true;
        if (t[0] == L'\\' && t[1] == L'\\') return true;
    }
    return t.find(L'\\') != std::wstring::npos;
}

std::vector<ui::FluentMenuItem> BuildCommandPalette(const std::vector<std::wstring>& recent_paths) {
    return BuildCommandPalette(L"", recent_paths, {}, false);
}

std::vector<ui::FluentMenuItem> BuildCommandPalette(const std::wstring& query,
                                                    const std::vector<std::wstring>& recent_paths,
                                                    const std::vector<index::Hit>& hits,
                                                    bool project_only,
                                                    size_t total,
                                                    const std::wstring& current_path) {
    std::vector<ui::FluentMenuItem> items;
    const OmnibarQuery parsed = ParseOmnibarQuery(query, project_only);
    const std::wstring& needle = parsed.needle;
    const bool command_mode = parsed.kind == OmnibarQuery::Kind::Command;
    const bool search_mode = parsed.kind == OmnibarQuery::Kind::Search;
    const bool path_like = parsed.kind == OmnibarQuery::Kind::Mixed &&
                           LooksLikeFilesystemPath(needle);
    auto add_cmd = [&](int cmd, const wchar_t* text, const wchar_t* glyph, const wchar_t* shortcut,
                       ui::fluent::MenuPictogram pictogram = ui::fluent::MenuPictogram::None) {
        if (!ContainsI(text, needle)) return;
        items.push_back(Item(cmd, text, glyph, shortcut));
        items.back().pictogram = pictogram;
    };
    if (!project_only && !search_mode && !path_like) {
        add_cmd(CmdLayoutTwoVertical, l10n::Get(l10n::StringId::LayoutVertical).c_str(), L"", L"Ctrl+Alt+2",
                ui::fluent::MenuPictogram::LayoutSideBySide);
        add_cmd(CmdLayoutFourGrid, l10n::Get(l10n::StringId::LayoutFour).c_str(), L"", L"Ctrl+Alt+4",
                ui::fluent::MenuPictogram::LayoutFour);
        add_cmd(CmdCopyToTarget, l10n::Get(l10n::StringId::CopyToTarget).c_str(), kGlyphCopy, L"Ctrl+Alt+C");
        add_cmd(CmdNewFolder, l10n::Get(l10n::StringId::NewFolder).c_str(), kGlyphNewFolder, L"Ctrl+Shift+N");
        add_cmd(CmdPinWorkspace, l10n::Get(l10n::StringId::PinWorkspace).c_str(), kGlyphFolder, nullptr);
        add_cmd(CmdCopyPath, l10n::Get(l10n::StringId::CopyPath).c_str(), kGlyphLink, L"Ctrl+Shift+C");
        add_cmd(CmdInstallFullIndex, l10n::Get(l10n::StringId::EnableFullIndex).c_str(), kGlyphSearch, nullptr);
        add_cmd(CmdSettings, l10n::Get(l10n::StringId::Settings).c_str(), kGlyphSettings, nullptr);
        add_cmd(CmdExitPulse, l10n::Get(l10n::StringId::ExitPulse).c_str(), L"\xE7E8", nullptr);
        add_cmd(CmdOpenRecycle, l10n::Get(l10n::StringId::RecycleBin).c_str(), kGlyphRecycle, nullptr);
        add_cmd(CmdBatchRename, l10n::Get(l10n::StringId::BatchRename).c_str(), kGlyphRename, L"Ctrl+Shift+R");
        add_cmd(CmdAdvancedSearch, l10n::Get(l10n::StringId::AdvancedSearch).c_str(), kGlyphSearch, L"Ctrl+Shift+F");
        add_cmd(CmdSelectAll, l10n::Get(l10n::StringId::SelectAll).c_str(), kGlyphSelectAll, L"Ctrl+A");
        add_cmd(CmdInvertSelection, l10n::Get(l10n::StringId::InvertSelection).c_str(), kGlyphInvert, L"Ctrl+I");
        add_cmd(CmdSelectWildcard, l10n::Get(l10n::StringId::SelectWildcard).c_str(), kGlyphWildcard, L"Ctrl+Shift+A");
    }
    std::unordered_set<std::wstring> seen_paths;
    const bool has_content = index::QueryHasContent(index::ParseQuery(needle));
    if (!command_mode && (!hits.empty() || total > 0 || has_content)) {
        if (!items.empty()) items.back().separator_after = true;
        for (size_t i = 0; i < hits.size() && i < 48; ++i) {
            items.push_back(Item(CmdIndexBase + static_cast<int>(i),
                hits[i].name.c_str(), hits[i].is_dir ? kGlyphFolder : kGlyphNewFile));
            items.back().text = hits[i].name;
            items.back().shortcut = DisplayPath(hits[i].path);
            items.back().shortcut_inline = true;
            const std::wstring n = fs::NormalizePath(hits[i].path);
            if (!n.empty()) seen_paths.insert(n);
        }
        if (!needle.empty() && !project_only) {
            if (!items.empty()) items.back().separator_after = true;
            wchar_t count[64];
            swprintf_s(count, l10n::Get(l10n::StringId::SearchCount).c_str(), total);
            items.push_back(Item(CmdSearchAll,
                l10n::Get(has_content ? l10n::StringId::SearchContentResults
                                      : l10n::StringId::ShowAllResults).c_str(),
                kGlyphSearch, count));
        }
    }

    const std::wstring current_n = current_path.empty() ? L"" : fs::NormalizePath(current_path);
    constexpr size_t kMaxRecent = 6;
    size_t recent_added = 0;
    bool recent_sep = false;
    for (size_t i = 0; i < recent_paths.size() && recent_added < kMaxRecent; ++i) {
        const std::wstring n = fs::NormalizePath(recent_paths[i]);
        if (n.empty() || !seen_paths.insert(n).second) continue;
        if (!current_n.empty() && n == current_n) continue;
        const std::wstring title = FolderTitle(recent_paths[i]);
        if (!ContainsI(title, needle) && !ContainsI(recent_paths[i], needle) &&
            !ContainsI(DisplayPath(recent_paths[i]), needle))
            continue;
        if (!recent_sep) {
            if (!items.empty()) items.back().separator_after = true;
            recent_sep = true;
        }
        items.push_back(Item(CmdRecentBase + static_cast<int>(i), title.c_str(), kGlyphFolder));
        items.back().text = title;
        items.back().badge_text = l10n::Get(l10n::StringId::HistoryPath);
        ++recent_added;
    }
    return items;
}

std::wstring UniqueChildName(const std::wstring& dir, const std::wstring& base,
                             const std::wstring& ext) {
    auto exists = [&](const std::wstring& name) {
        std::wstring full = dir;
        if (!full.empty() && full.back() != L'\\') full += L'\\';
        full += name;
        return pulse::path::Exists(full);
    };
    std::wstring candidate = base + ext;
    for (int i = 2; exists(candidate); ++i) {
        candidate = base + L" (" + std::to_wstring(i) + L")" + ext;
    }
    return candidate;
}

} // namespace pulse::app
