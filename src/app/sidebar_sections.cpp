// sidebar_sections.cpp — Section menu for the empty part of the sidebar.
//
// File Explorer puts a context menu on the navigation-pane background that decides
// which sections the pane shows. Pulse keeps its sections in a fixed order, so the
// same idea becomes one checked entry per section plus a bulk expand/collapse pair.
#include "app_internal.h"
#include "app_commands.h"
#include "../common/localization.h"
#include "../ui/fluent_menu.h"

namespace pulse {
namespace {

using I = l10n::StringId;

constexpr const wchar_t* kIconCheck = L"\xE73E"; // Segoe Fluent Icons: CheckMark

// Menu commands: one toggle per section, then the bulk pair.
constexpr int kToggleBase = 32000;
constexpr int kExpandAll = 32100;
constexpr int kCollapseAll = 32101;
// Section menu (opened on a section header or its empty space).
constexpr int kBuiltinBase = 32200; // + BuiltinQuickAccess
constexpr int kCollapseSection = 32300;
constexpr int kExpandSection = 32301;
constexpr int kHideSection = 32302;

constexpr int kSectionCount = app::kSidebarSectionCount;

// Titles are listed in SidebarSectionId order; the cloud section shows the
// product name, exactly as its sidebar row does.
constexpr I kSectionTitles[] = {
    I::SidebarWorkspaces,
    I::SidebarQuickAccess,
    I::SidebarSavedSearches,
    I::SidebarDrives,
    I::SidebarTags,
    I::SidebarNetworkLocations,
};
// Cloud and Starred are named individually in SectionTitle().
static_assert(std::size(kSectionTitles) ==
                  static_cast<size_t>(app::SidebarSectionId::Cloud),
              "kSectionTitles must cover every header section in id order");

std::wstring SectionTitle(int section) {
    if (section == static_cast<int>(app::SidebarSectionId::Cloud)) return L"OneDrive";
    if (section == static_cast<int>(app::SidebarSectionId::Starred))
        return l10n::Get(I::StarredItems);
    if (section < 0 || section >= static_cast<int>(std::size(kSectionTitles))) return {};
    return l10n::Get(kSectionTitles[section]);
}

bool IsQuickAccessSection(int section) {
    return section == static_cast<int>(app::SidebarSectionId::QuickAccess);
}

} // namespace

void ToggleSidebarSection(AppState& s, int group) {
    if (group < 0 || group >= kSectionCount) return;
    s.sidebarHiddenMask ^= 1u << group;
    // Bringing Quick access back with every built-in link switched off would
    // leave nothing to right-click, so unhiding it also restores the links.
    if (group == static_cast<int>(app::SidebarSectionId::QuickAccess) &&
        ((s.sidebarHiddenMask >> group) & 1u) == 0) {
        s.sidebarQuickAccessHiddenMask = 0;
    }
}

void SetEverySidebarSectionCollapsed(AppState& s, bool collapsed) {
    s.sidebarCollapsedMask = collapsed ? (1u << kSectionCount) - 1u : 0u;
}

void ShowSidebarSectionsMenu(AppState& s, POINT screen_pt) {
    if (!EnsureMenu(s)) return;
    const uint32_t hidden = s.sidebarHiddenMask;
    std::vector<ui::FluentMenuItem> items;
    items.reserve(kSectionCount + 2);
    // Listed in display order, so the menu matches the sidebar after a reorder.
    for (int i : app::NormalizeSidebarOrder(s.sidebarOrder)) {
        ui::FluentMenuItem item;
        item.command = kToggleBase + i;
        item.text = SectionTitle(i);
        item.checked = ((hidden >> i) & 1u) == 0;
        // TrackPopup only paints a checked row when the check arrives as a glyph,
        // which is what TrackDropdown does for its own checked entries.
        if (item.checked) item.glyph = kIconCheck;
        items.push_back(std::move(item));
    }
    items.back().separator_after = true;
    ui::FluentMenuItem expand;
    expand.command = kExpandAll;
    expand.text = l10n::Get(I::SidebarExpandAll);
    items.push_back(std::move(expand));
    ui::FluentMenuItem collapse;
    collapse.command = kCollapseAll;
    collapse.text = l10n::Get(I::SidebarCollapseAll);
    items.push_back(std::move(collapse));

    const int command = s.menu->TrackPopup(screen_pt, std::move(items));
    if (command >= kToggleBase && command < kToggleBase + kSectionCount) {
        ToggleSidebarSection(s, command - kToggleBase);
    } else if (command == kExpandAll) {
        SetEverySidebarSectionCollapsed(s, false);
    } else if (command == kCollapseAll) {
        SetEverySidebarSectionCollapsed(s, true);
    } else {
        return;
    }
    // The sidebar scroll range shrinks with the content, so the layout clamps the
    // offset the same way a group header click does.
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

void ShowSidebarSectionMenu(AppState& s, int section, POINT screen_pt) {
    if (section < 0 || section >= kSectionCount) return;
    if (!EnsureMenu(s)) return;
    std::vector<ui::FluentMenuItem> items;
    if (IsQuickAccessSection(section)) {
        // One entry per built-in link; each toggles just that row. Labels come
        // from the model, so the menu and the sidebar always read the same.
        for (const auto& entry : s.sidebar.quick_access) {
            if (entry.builtin < 0 ||
                entry.builtin >= static_cast<int>(app::BuiltinQuickAccess::Count)) continue;
            ui::FluentMenuItem item;
            item.command = kBuiltinBase + entry.builtin;
            item.text = entry.label;
            item.checked = ((s.sidebarQuickAccessHiddenMask >> entry.builtin) & 1u) == 0;
            if (item.checked) item.glyph = kIconCheck;
            items.push_back(std::move(item));
        }
        if (!items.empty()) items.back().separator_after = true;
    }
    // A header-less section has nothing to fold: its rows are the section.
    if (!app::IsHeaderlessSection(static_cast<app::SidebarSectionId>(section))) {
        const bool collapsed = ((s.sidebarCollapsedMask >> section) & 1u) != 0;
        ui::FluentMenuItem fold;
        fold.command = collapsed ? kExpandSection : kCollapseSection;
        fold.text = l10n::Get(collapsed ? I::SidebarExpandSection : I::SidebarCollapseSection);
        items.push_back(std::move(fold));
    }
    ui::FluentMenuItem hide;
    hide.command = kHideSection;
    hide.text = l10n::Get(I::SidebarHideSection);
    items.push_back(std::move(hide));

    const int command = s.menu->TrackPopup(screen_pt, std::move(items));
    if (command >= kBuiltinBase &&
        command < kBuiltinBase + static_cast<int>(app::BuiltinQuickAccess::Count)) {
        s.sidebarQuickAccessHiddenMask ^= 1u << (command - kBuiltinBase);
    } else if (command == kCollapseSection) {
        s.sidebarCollapsedMask |= 1u << section;
    } else if (command == kExpandSection) {
        s.sidebarCollapsedMask &= ~(1u << section);
    } else if (command == kHideSection) {
        s.sidebarHiddenMask |= 1u << section;
    } else {
        return;
    }
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

} // namespace pulse
