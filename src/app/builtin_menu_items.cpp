#include "builtin_menu_items.h"

#include "../common/localization.h"
#include "../ui/command_icons.h"

#include <algorithm>
#include <cstddef>
#include <initializer_list>
#include <iterator>

namespace pulse::app {
namespace {

struct BuiltinMenuInfo {
    BuiltinMenuItem item;
    const wchar_t* key;
    l10n::StringId label;
};

constexpr BuiltinMenuInfo kItems[] = {
    { BuiltinMenuItem::OpenInNewTab,   L"open_new_tab",   l10n::StringId::OpenNewTab },
    { BuiltinMenuItem::CopyPath,       L"copy_path",      l10n::StringId::CopyPath },
    { BuiltinMenuItem::Terminal,       L"terminal",       l10n::StringId::OpenTerminalHere },
    { BuiltinMenuItem::QuickAccess,    L"quick_access",   l10n::StringId::PinQuickAccess },
    { BuiltinMenuItem::PinWorkspace,   L"pin_workspace",  l10n::StringId::PinWorkspace },
    { BuiltinMenuItem::PinNetwork,     L"pin_network",    l10n::StringId::PinNetwork },
    { BuiltinMenuItem::Tags,           L"tags",           l10n::StringId::ContextBuiltinTags },
    { BuiltinMenuItem::RecentChanges,  L"recent_changes", l10n::StringId::ChangeView },
    { BuiltinMenuItem::SelectCommands, L"select",         l10n::StringId::ContextBuiltinSelect },
    { BuiltinMenuItem::Undo,           L"undo",           l10n::StringId::Undo },
    { BuiltinMenuItem::RowNewTab,      L"row_new_tab",    l10n::StringId::ContextRowNewTab },
    { BuiltinMenuItem::RowStar,        L"row_star",       l10n::StringId::ContextRowStar },
    { BuiltinMenuItem::RowMore,        L"row_more",       l10n::StringId::ContextRowMore },
    { BuiltinMenuItem::View,           L"view",           l10n::StringId::View },
    { BuiltinMenuItem::Sort,           L"sort",           l10n::StringId::SortBy },
    { BuiltinMenuItem::Group,          L"group",          l10n::StringId::GroupBy },
    { BuiltinMenuItem::Refresh,        L"refresh",        l10n::StringId::Refresh },
    { BuiltinMenuItem::NewFolder,      L"new_folder",     l10n::StringId::NewFolder },
    { BuiltinMenuItem::NewTextFile,    L"new_text",       l10n::StringId::NewTextDocument },
    { BuiltinMenuItem::Paste,          L"paste",          l10n::StringId::Paste },
    { BuiltinMenuItem::Properties,     L"properties",     l10n::StringId::Properties },
    { BuiltinMenuItem::FolderProperties, L"folder_properties", l10n::StringId::ContextFolderProperties },
    { BuiltinMenuItem::OpenPath, L"open_path", l10n::StringId::OpenPath },
    { BuiltinMenuItem::GroupIntoFolder, L"group_into_folder", l10n::StringId::NewFolderWithSelection_2 },
    { BuiltinMenuItem::DissolveFolder, L"dissolve_folder", l10n::StringId::UngroupFolder_2 },
    { BuiltinMenuItem::SetAlias, L"set_alias", l10n::StringId::SetAliasEllipsis },
    { BuiltinMenuItem::StageToTray, L"stage_to_tray", l10n::StringId::CtxAddToTray },
    { BuiltinMenuItem::CopyToTarget, L"copy_to_target", l10n::StringId::CopyToTarget },
    { BuiltinMenuItem::MoveToTarget, L"move_to_target", l10n::StringId::MoveToTarget },
    { BuiltinMenuItem::BatchRename, L"batch_rename", l10n::StringId::BatchRename },
    { BuiltinMenuItem::CompareSideBySide, L"compare_side_by_side", l10n::StringId::CompareSideBySide },
    { BuiltinMenuItem::CloudKeep, L"cloud_keep", l10n::StringId::CloudKeepOnDevice },
    { BuiltinMenuItem::CloudFree, L"cloud_free", l10n::StringId::CloudFreeUpSpace },
    { BuiltinMenuItem::EjectDrive, L"eject_drive", l10n::StringId::Eject },
    { BuiltinMenuItem::DisconnectDrive, L"disconnect_drive", l10n::StringId::DisconnectNetworkDrive },
    { BuiltinMenuItem::UnlockDrive, L"unlock_drive", l10n::StringId::UnlockDrive },
    { BuiltinMenuItem::MapNetworkDrive, L"map_network_drive", l10n::StringId::MapNetworkDrive },
    { BuiltinMenuItem::ForgetNetworkRecent, L"forget_network_recent", l10n::StringId::NetworkForgetRecent },
    { BuiltinMenuItem::DupKeepThis, L"dup_keep_this", l10n::StringId::DupFKeepThis },
    { BuiltinMenuItem::DupReapplyRules, L"dup_reapply_rules", l10n::StringId::DupFReapply },
    { BuiltinMenuItem::FindDuplicates, L"find_duplicates", l10n::StringId::ActFindDuplicates },
    { BuiltinMenuItem::AiOrganizeHere, L"ai_organize_here", l10n::StringId::ActOrganizeHere },
    { BuiltinMenuItem::NewTemplates, L"new_templates", l10n::StringId::New },
    { BuiltinMenuItem::Git, L"git", l10n::StringId::Git },
    { BuiltinMenuItem::ConnectServer, L"connect_server", l10n::StringId::ConnectServerMenu },
    { BuiltinMenuItem::CopyToWindow, L"copy_to_window", l10n::StringId::CopyToWindow },
    { BuiltinMenuItem::MoveToWindow, L"move_to_window", l10n::StringId::MoveToWindow },
};
static_assert(sizeof(kItems) / sizeof(kItems[0]) == kBuiltinMenuItemCount,
              "every built-in item needs a key and a label");

constexpr bool ItemsInEnumOrder() {
    for (size_t i = 0; i < sizeof(kItems) / sizeof(kItems[0]); ++i)
        if (static_cast<size_t>(kItems[i].item) != i) return false;
    return true;
}
static_assert(ItemsInEnumOrder(), "kItems is indexed by BuiltinMenuItem");

const BuiltinMenuInfo& Info(BuiltinMenuItem item) {
    return kItems[static_cast<size_t>(item)];
}

using B = BuiltinMenuItem;
using F = BuiltinFixedRow;
constexpr BuiltinMenuRow kItemRows[] = {
    { B::Count, F::Open, L"\xE8E5", L"Enter", false, false },
    { B::Count, F::Strip, nullptr, nullptr, false, false },
    { B::RecentChanges, F::None, L"\xE81C", nullptr, false, false },
    { B::CompareSideBySide, F::None, ui::command_icons::kGlyphCompare, nullptr, false, false },
    { B::BatchRename, F::None, ui::command_icons::kGlyphBatchRename, nullptr, false, false },
    { B::GroupIntoFolder, F::None, ui::command_icons::kGlyphGroupFolder, L"Ctrl+G", false, false },
    { B::DissolveFolder, F::None, L"\xED25", L"Ctrl+Shift+G", false, false },
    { B::OpenPath, F::None, ui::command_icons::kGlyphOpenPath, nullptr, false, false },
    { B::OpenInNewTab, F::None, L"\xE8A7", nullptr, false, false },
    { B::CopyPath, F::None, ui::command_icons::kGlyphCopyPath, L"Ctrl+Shift+C", false, false },
    { B::SetAlias, F::None, L"\xE8EC", L"Shift+F2", false, false },
    { B::StageToTray, F::None, L"\xE7B8", L"Ctrl+Shift+S", false, false },
    { B::CopyToTarget, F::None, ui::command_icons::kGlyphCopyToPane, L"Ctrl+Alt+C", false, false },
    { B::MoveToTarget, F::None, ui::command_icons::kGlyphMoveToPane, L"Ctrl+Alt+X", false, false },
    { B::CopyToWindow, F::None, L"\xE8C8", nullptr, false, true },
    { B::MoveToWindow, F::None, L"\xE8C6", nullptr, false, true },
    { B::Terminal, F::None, L"\xE756", nullptr, false, false },
    { B::Properties, F::None, L"\xE946", L"Alt+Enter", true, false },
    { B::QuickAccess, F::None, ui::command_icons::kGlyphPinQuickAccess, nullptr, false, false },
    { B::PinWorkspace, F::None, ui::command_icons::kGlyphPinWorkspace, nullptr, false, false },
    { B::PinNetwork, F::None, ui::command_icons::kGlyphPinNetwork, nullptr, true, false },
    { B::EjectDrive, F::None, L"\xF847", nullptr, false, false },
    { B::DisconnectDrive, F::None, L"\xE8CD", nullptr, false, false },
    { B::UnlockDrive, F::None, L"\xE785", nullptr, false, false },
    { B::MapNetworkDrive, F::None, L"\xE8CE", nullptr, false, false },
    { B::ForgetNetworkRecent, F::None, L"\xE711", nullptr, false, false },
    { B::CloudKeep, F::None, L"\xE718", nullptr, false, false },
    { B::CloudFree, F::None, L"\xE753", nullptr, false, false },
    { B::DupKeepThis, F::None, L"\xE73E", nullptr, false, false },
    { B::DupReapplyRules, F::None, L"\xE72C", nullptr, false, false },
    { B::Tags, F::None, L"\xE8EC", nullptr, true, false },
    { B::Undo, F::None, L"\xE7A7", L"Ctrl+Z", false, false },
};
constexpr BuiltinMenuRow kBackgroundRows[] = {
    { B::View, F::None, L"\xE8A9", nullptr, false, true },
    { B::Sort, F::None, L"\xE8CB", nullptr, false, true },
    { B::Group, F::None, L"\xF168", nullptr, false, true },
    { B::Refresh, F::None, L"\xE72C", L"F5", true, false },
    { B::NewFolder, F::None, L"\xE8F4", L"F7", false, false },
    { B::NewTextFile, F::None, L"\xE8A5", nullptr, true, false },
    { B::NewTemplates, F::None, L"\xE710", nullptr, false, true },
    { B::FindDuplicates, F::None, L"\xE8C8", nullptr, false, false },
    { B::AiOrganizeHere, F::None, ui::command_icons::kGlyphSparkle, nullptr, false, false },
    { B::Paste, F::None, L"\xE77F", L"Ctrl+V", false, false },
    { B::CopyPath, F::None, ui::command_icons::kGlyphCopyPath, L"Ctrl+Shift+C", true, false },
    { B::SelectCommands, F::None, L"\xE8B3", L"Ctrl+A", true, false },
    { B::Terminal, F::None, L"\xE756", nullptr, true, false },
    { B::Git, F::None, L"\xE8B7", nullptr, false, true },
    { B::QuickAccess, F::None, ui::command_icons::kGlyphPinQuickAccess, nullptr, false, false },
    { B::PinWorkspace, F::None, ui::command_icons::kGlyphPinWorkspace, nullptr, false, false },
    { B::PinNetwork, F::None, ui::command_icons::kGlyphPinNetwork, nullptr, true, false },
    { B::MapNetworkDrive, F::None, L"\xE8CE", nullptr, false, false },
    { B::ConnectServer, F::None, L"\xE968", nullptr, false, false },
    { B::RecentChanges, F::None, L"\xE81C", nullptr, false, false },
    { B::Undo, F::None, L"\xE7A7", L"Ctrl+Z", true, false },
    { B::FolderProperties, F::None, L"\xE946", nullptr, false, false },
};
constexpr BuiltinMenuRow kRowButtonRows[] = {
    { B::RowNewTab, F::None, L"\xE8A7", nullptr, false, false },
    { B::RowStar, F::None, L"\xE734", nullptr, false, false },
    { B::RowMore, F::None, L"\xE712", nullptr, false, false },
};

constexpr uint64_t Bits(std::initializer_list<B> items) {
    uint64_t bits = 0;
    for (const B item : items) bits |= BuiltinMenuBit(item);
    return bits;
}
constexpr uint64_t kAllBits = ~uint64_t{0} >> (64 - kBuiltinMenuItemCount);
// 精简 keeps the everyday rows; 标准 drops the rarely used pins and
// selection helpers; 完整 (default) shows everything.
constexpr uint64_t kSlimHidden = kAllBits & ~Bits({
    B::CopyPath, B::Undo, B::View, B::Sort, B::Group, B::Refresh, B::NewFolder,
    B::NewTextFile, B::Paste, B::Properties, B::FolderProperties });
constexpr uint64_t kStandardHidden = Bits({ B::PinNetwork, B::RecentChanges, B::SelectCommands });

} // namespace

BuiltinMenuRows BuiltinMenuSurfaceRows(BuiltinMenuSurface surface) {
    switch (surface) {
    case BuiltinMenuSurface::Item: return { kItemRows, std::size(kItemRows) };
    case BuiltinMenuSurface::Background: return { kBackgroundRows, std::size(kBackgroundRows) };
    case BuiltinMenuSurface::RowButtons: return { kRowButtonRows, std::size(kRowButtonRows) };
    default: return { nullptr, 0 };
    }
}

std::wstring BuiltinMenuRowLabel(const BuiltinMenuRow& row) {
    if (row.fixed == F::Open) return l10n::Get(l10n::StringId::Open);
    if (row.fixed == F::Strip) return l10n::Get(l10n::StringId::ContextActionStrip);
    return row.item < B::Count ? BuiltinMenuLabel(row.item) : std::wstring{};
}

BuiltinMenuOrder BuiltinMenuDefaultOrder(BuiltinMenuSurface surface) {
    const auto rows = BuiltinMenuSurfaceRows(surface);
    BuiltinMenuOrder order;
    for (size_t i = 0; i < rows.count; ++i)
        if (rows.rows[i].fixed == F::None && rows.rows[i].item < B::Count) order.push_back(rows.rows[i].item);
    return order;
}

BuiltinMenuOrder NormalizeBuiltinMenuOrder(BuiltinMenuSurface surface, const BuiltinMenuOrder& saved) {
    const auto table = BuiltinMenuDefaultOrder(surface);
    auto has = [](const BuiltinMenuOrder& list, B item) {
        return std::find(list.begin(), list.end(), item) != list.end();
    };
    BuiltinMenuOrder out;
    out.reserve(table.size());
    for (const B item : saved)
        if (has(table, item) && !has(out, item)) out.push_back(item);
    for (size_t i = 0; i < table.size(); ++i) {
        if (has(out, table[i])) continue;
        size_t at = 0;
        for (size_t j = i; j-- > 0;) {
            const auto prev = std::find(out.begin(), out.end(), table[j]);
            if (prev != out.end()) { at = static_cast<size_t>(prev - out.begin()) + 1; break; }
        }
        out.insert(out.begin() + static_cast<std::ptrdiff_t>(at), table[i]);
    }
    return out;
}

std::vector<BuiltinMenuRow> BuiltinMenuOrderedRows(BuiltinMenuSurface surface, const BuiltinMenuOrder& order) {
    const auto rows = BuiltinMenuSurfaceRows(surface);
    std::vector<BuiltinMenuRow> out(rows.rows, rows.rows + rows.count);
    if (order.empty()) return out;
    const auto items = NormalizeBuiltinMenuOrder(surface, order);
    size_t next = 0;
    for (auto& slot : out) {
        if (slot.fixed != F::None || slot.item >= B::Count || next >= items.size()) continue;
        const B item = items[next++];
        for (size_t i = 0; i < rows.count; ++i) {
            if (rows.rows[i].item != item || rows.rows[i].fixed != F::None) continue;
            const bool separator = slot.separator_after;
            slot = rows.rows[i];
            slot.separator_after = separator;
            break;
        }
    }
    return out;
}

std::vector<BuiltinMenuVisibleRow> BuiltinMenuVisibleRows(BuiltinMenuSurface surface,
                                                          uint64_t builtin_hidden,
                                                          const BuiltinMenuOrder& order) {
    const auto rows = BuiltinMenuOrderedRows(surface, order);
    std::vector<BuiltinMenuVisibleRow> kept;
    kept.reserve(rows.size());
    bool last_dropped = false, last_kept = false;
    for (size_t i = 0; i < rows.size(); ++i) {
        const auto& row = rows[i];
        const bool hidden = row.item < B::Count && (builtin_hidden & BuiltinMenuBit(row.item)) != 0;
        last_dropped = hidden;
        if (hidden) {
            if (row.separator_after && !kept.empty()) kept.back().separator_after = true;
            continue;
        }
        kept.push_back({ row, row.separator_after });
        last_kept = i + 1 == rows.size();
    }
    if (!kept.empty() && (last_dropped || last_kept))
        kept.back().separator_after = false;
    return kept;
}

bool BuiltinMenuShared(BuiltinMenuItem item) {
    bool in_item = false, in_background = false;
    for (const auto& row : kItemRows) in_item |= row.item == item;
    for (const auto& row : kBackgroundRows) in_background |= row.item == item;
    return in_item && in_background;
}

uint64_t BuiltinMenuPresetHidden(BuiltinMenuPreset preset) {
    switch (preset) {
    case BuiltinMenuPreset::Slim: return kSlimHidden & ~Bits({B::RowStar, B::RowNewTab, B::RowMore});
    case BuiltinMenuPreset::Standard: return kStandardHidden & ~Bits({B::RowStar, B::RowNewTab, B::RowMore});
    default: return 0;
    }
}

BuiltinMenuPreset BuiltinMenuPresetFor(uint64_t builtin_hidden) {
    builtin_hidden &= kAllBits & ~Bits({B::RowStar, B::RowNewTab, B::RowMore});
    for (int i = 0; i < static_cast<int>(BuiltinMenuPreset::Count); ++i) {
        const auto preset = static_cast<BuiltinMenuPreset>(i);
        if (BuiltinMenuPresetHidden(preset) == builtin_hidden) return preset;
    }
    return BuiltinMenuPreset::Count;
}

std::wstring_view BuiltinMenuKey(BuiltinMenuItem item) {
    return Info(item).key;
}

std::wstring BuiltinMenuLabel(BuiltinMenuItem item) {
    return l10n::Get(Info(item).label);
}

uint32_t RowActionMask(uint64_t builtin_hidden, const BuiltinMenuOrder& row_order) {
    uint32_t mask = kRowActionsAll;
    if (builtin_hidden & BuiltinMenuBit(BuiltinMenuItem::RowStar)) mask &= ~kRowActionStar;
    if (builtin_hidden & BuiltinMenuBit(BuiltinMenuItem::RowNewTab)) mask &= ~kRowActionNewTab;
    if (builtin_hidden & BuiltinMenuBit(BuiltinMenuItem::RowMore)) mask &= ~kRowActionMore;
    const auto order = NormalizeBuiltinMenuOrder(BuiltinMenuSurface::RowButtons, row_order);
    if (row_order.empty() || order == BuiltinMenuDefaultOrder(BuiltinMenuSurface::RowButtons)) return mask;
    uint32_t code = 0;
    for (size_t i = 0; i < order.size() && i < 3; ++i) {
        const uint32_t id = order[i] == B::RowStar ? 0u : order[i] == B::RowNewTab ? 1u : 2u;
        code |= id << (2u * static_cast<uint32_t>(i));
    }
    return mask | (code << kRowActionOrderShift);
}

} // namespace pulse::app
