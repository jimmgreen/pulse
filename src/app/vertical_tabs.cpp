#include "vertical_tabs.h"
#include "app_commands.h"
#include "app_navigation.h"
#include "../common/localization.h"
#include <algorithm>
#include <cwchar>

namespace pulse {
namespace {
constexpr wchar_t kTabPrefix[] = L"pulse:tab:";
constexpr size_t kTabPrefixLen = sizeof(kTabPrefix) / sizeof(kTabPrefix[0]) - 1;
constexpr wchar_t kNewTabRowPath[] = L"pulse:tab:new";
}

bool IsVerticalTabPath(const std::wstring& path, size_t* index) {
    if (path.size() <= kTabPrefixLen || path.compare(0, kTabPrefixLen, kTabPrefix) != 0)
        return false;
    wchar_t* end = nullptr;
    const unsigned long value = std::wcstoul(path.c_str() + kTabPrefixLen, &end, 10);
    if (!end || *end != L'\0') return false;
    if (index) *index = static_cast<size_t>(value);
    return true;
}

void ApplyVerticalTabs(AppState& s, ui::WindowViewModel& vm) {
    // Settings replaces the sidebar with its own navigation, so the tab strip
    // comes back there. Read the active tab directly: BuildVm calls this before
    // FillPaneSlots sets vm.settings_open (the scroll clamp needs the tabs group).
    const app::Tab* active = ActiveTab(s);
    const bool in_settings = vm.settings_open ||
        (active && active->current_path.starts_with(L"pulse:settings"));
    const bool vertical = s.appPrefs.vertical_tabs && !in_settings;
    s.renderer.SetVerticalTabs(vertical);
    s.renderer.SetSidebarCollapsed(s.appPrefs.sidebar_collapsed);
    if (!vertical || vm.tabs.empty()) return;

    ui::SidebarGroup group;
    group.id = kVerticalTabsSectionId;
    group.header = l10n::Get(l10n::StringId::SidebarTabs);
    group.icon_glyph = L"\xE7C4";  // task view: stacked windows
    group.add_action = ui::SidebarAddAction::NewTab;
    group.collapsed = ((s.sidebarCollapsedMask >> kVerticalTabsSectionId) & 1u) != 0;
    group.tabs_section = true;
    group.items.reserve(vm.tabs.size() + 1);
    const std::wstring settings_title = l10n::Get(l10n::StringId::Settings);
    for (size_t i = 0; i < vm.tabs.size(); ++i) {
        const ui::TabView& tab = vm.tabs[i];
        ui::SidebarItem item;
        item.label = tab.title;
        item.path = kTabPrefix + std::to_wstring(i);
        item.tab_row = true;
        item.tab_active = tab.active;
        item.flash = tab.flash;
        item.tab_number = static_cast<int>(i) + 1;
        item.fallback_text = L"Tab";
        if (tab.title == settings_title) {
            item.icon_glyph = L"\xE713";
            item.icon_color = ui::HexColor(0x94A3B8);
        } else {
            item.icon_glyph = L"\xE8B7";
            item.icon_color = ui::HexColor(0xFBBF24);
        }
        // Group color, then the tab's own marker, tints the active card's bar.
        const uint32_t tint = tab.color_rgb ? tab.color_rgb : tab.marker_rgb;
        if (tint) item.tag_dot = ui::HexColor(tint);  // accent bar color
        if (i < s.window_tabs.items.size()) {
            const size_t panes = s.window_tabs.items[i]->panes.size();
            if (panes > 1) {
                wchar_t badge[32]{};
                swprintf_s(badge, l10n::Get(l10n::StringId::WorkspacePanes).c_str(),
                           static_cast<int>(panes));
                item.badge = badge;
            }
        }
        group.items.push_back(std::move(item));
    }
    // Trailing "new tab" row, as in the tab strip's ＋.
    ui::SidebarItem add;
    add.label = l10n::Get(l10n::StringId::TooltipNewTab);
    add.path = kNewTabRowPath;
    add.icon_glyph = L"\xE710";
    add.icon_color = ui::HexColor(0x8A8A93);  // neutral in both themes
    add.fallback_text = L"+";
    add.badge = L"Ctrl+T";
    add.badge_color = ui::HexColor(0x94A3B8);  // quiet hint, not an accent pill
    group.items.push_back(std::move(add));
    vm.sidebar.insert(vm.sidebar.begin(), std::move(group));
}

void ToggleSidebarCollapsed(AppState& s) {
    // Pinning a peek keeps the overlay's width, so it expands in place.
    const bool from_peek = s.renderer.SidebarPeekVisible(static_cast<float>(s.compositor.Width()));
    s.sidebarPeekArmAt = 0;
    s.renderer.SetSidebarPeek(false);
    s.appPrefs.sidebar_collapsed = !s.appPrefs.sidebar_collapsed;
    s.renderer.SetSidebarCollapsed(s.appPrefs.sidebar_collapsed, !from_peek);
    s.appPrefs.Save();
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

namespace {
constexpr ULONGLONG kPeekDwellMs = 450;

bool PeekEligible(AppState& s) {
    if (!s.appPrefs.sidebar_collapsed || s.renderer.SidebarAnimating()) return false;
    if (static_cast<float>(s.compositor.Width()) / s.scale < ui::kSidebarRailWindowDip) return false;
    if ((GetKeyState(VK_LBUTTON) & 0x8000) != 0) return false;  // drags keep the rail
    if (const app::Tab* tab = ActiveTab(s); tab && tab->current_path.starts_with(L"pulse:settings"))
        return false;
    return true;
}
}

void CloseSidebarPeek(AppState& s) {
    s.sidebarPeekArmAt = 0;
    if (!s.renderer.SidebarPeek()) return;
    s.renderer.SetSidebarPeek(false);
    // The rail itself never scrolls; drop any offset the overlay picked up.
    s.sidebarScroll = 0.0f;
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

void UpdateSidebarPeek(AppState& s, int x, int y) {
    if (!s.appPrefs.sidebar_collapsed) {
        if (s.sidebarPeekArmAt || s.renderer.SidebarPeek()) CloseSidebarPeek(s);
        return;
    }
    const float w = static_cast<float>(s.compositor.Width());
    if (s.renderer.SidebarPeek()) {
        // Close once the pointer clears the overlay (the title-bar pin stays
        // reachable because it sits inside the overlay's width).
        if (static_cast<float>(x) > s.renderer.SidebarFullWidth(w) + 12.0f * s.scale)
            CloseSidebarPeek(s);
        return;
    }
    const bool in_rail = static_cast<float>(x) < ui::kSidebarRailWidthDip * s.scale &&
                         static_cast<float>(y) >= s.renderer.TitleBarHeight();
    if (!in_rail || !PeekEligible(s)) {
        s.sidebarPeekArmAt = 0;
        return;
    }
    if (!s.sidebarPeekArmAt) s.sidebarPeekArmAt = GetTickCount64();
}

bool TickSidebarPeek(AppState& s, ULONGLONG now) {
    if (!s.sidebarPeekArmAt || s.renderer.SidebarPeek()) return false;
    if (now - s.sidebarPeekArmAt < kPeekDwellMs) return false;
    s.sidebarPeekArmAt = 0;
    if (!PeekEligible(s)) return false;
    POINT pt{};
    GetCursorPos(&pt);
    ScreenToClient(s.hwnd, &pt);
    if (pt.x < 0 || static_cast<float>(pt.x) >= ui::kSidebarRailWidthDip * s.scale ||
        static_cast<float>(pt.y) < s.renderer.TitleBarHeight()) return false;
    s.renderer.SetSidebarPeek(true);
    return true;
}

bool HandleVerticalTabPress(AppState& s, const ui::HitTestResult& hit) {
    using R = ui::HitTestResult;
    size_t index = 0;
    if (hit.region == R::SidebarToggle) {
        ToggleSidebarCollapsed(s);
        return true;
    }
    if (hit.region == R::SidebarHeaderAction &&
        hit.sidebar_action == ui::SidebarAddAction::NewTab) {
        OpenNewTab(s);
        InvalidateRect(s.hwnd, nullptr, FALSE);
        return true;
    }
    if (hit.region == R::SidebarHeader && hit.sidebar_section == kVerticalTabsSectionId) {
        // Plain fold toggle: the synthesized section never joins the drag order.
        s.sidebarCollapsedMask ^= 1u << kVerticalTabsSectionId;
        InvalidateRect(s.hwnd, nullptr, FALSE);
        return true;
    }
    if (hit.region == R::SidebarItemAction && IsVerticalTabPath(hit.path, &index)) {
        if (index < s.window_tabs.items.size()) CloseLayoutTab(s, index);
        InvalidateRect(s.hwnd, nullptr, FALSE);
        return true;
    }
    if (hit.region == R::SidebarItem && hit.path == kNewTabRowPath) {
        OpenNewTab(s);
        InvalidateRect(s.hwnd, nullptr, FALSE);
        return true;
    }
    if (hit.region == R::SidebarItem && IsVerticalTabPath(hit.path, &index)) {
        // Switch on press (like the strip); the press may also become a reorder.
        if (index < s.window_tabs.items.size())
            s.tabDoubleClickTarget = s.window_tabs.items[index].get();
        if (index < s.window_tabs.items.size() && index != s.window_tabs.active)
            SwitchTab(s, index);
        POINT pt{};
        GetCursorPos(&pt);
        ScreenToClient(s.hwnd, &pt);
        s.pinDragPending = true;
        s.pinDragActive = false;
        s.pinDragStartPt = pt;
        s.pinDragPath = hit.path;
        s.pinDragRun = hit.index;
        s.pinDragToIndex = -1;
        s.pinGapVisible = false;
        s.pinGapLineY = 0.0f;
        SetCapture(s.hwnd);
        InvalidateRect(s.hwnd, nullptr, FALSE);
        return true;
    }
    return false;
}

bool UpdateVerticalTabDrag(AppState& s, int my) {
    if (!IsVerticalTabPath(s.pinDragPath)) return false;
    const ui::WindowViewModel vm = BuildVm(s, false);
    const float w = static_cast<float>(s.compositor.Width());
    const float h = static_cast<float>(s.compositor.Height());
    std::vector<D2D1_RECT_F> rows;
    const int group = app::SidebarSectionIndex(vm, kVerticalTabsSectionId);
    if (group >= 0) {
        const auto& items = vm.sidebar[static_cast<size_t>(group)].items;
        for (int i = 0; i < static_cast<int>(items.size()); ++i) {
            if (!items[static_cast<size_t>(i)].tab_row) continue;
            D2D1_RECT_F rc{};
            if (s.renderer.SidebarRowRect(vm, w, h, kVerticalTabsSectionId, i, &rc)) rows.push_back(rc);
        }
    }
    const float cursor = static_cast<float>(my);
    int insert_at = static_cast<int>(rows.size());
    float line_y = rows.empty() ? 0.0f : rows.back().bottom;
    for (size_t i = 0; i < rows.size(); ++i) {
        if (cursor < (rows[i].top + rows[i].bottom) * 0.5f) {
            insert_at = static_cast<int>(i);
            line_y = rows[i].top;
            break;
        }
    }
    s.pinDragToIndex = insert_at;
    s.pinGapLineY = line_y;
    s.pinGapVisible = line_y > 0.0f;
    return true;
}

void CommitVerticalTabDrag(AppState& s, const std::wstring& path, size_t insert_at) {
    size_t from = 0;
    if (!IsVerticalTabPath(path, &from) || from >= s.window_tabs.items.size()) return;
    size_t to = insert_at > from ? insert_at - 1 : insert_at;
    to = (std::min)(to, s.window_tabs.items.size() - 1);
    if (to == from) return;
    s.window_tabs.MoveTab(from, to);
    // Grouped tabs stay contiguous; a group left without members disappears.
    app::NormalizeGroupRuns(s.window_tabs);
    auto& groups = s.window_tabs.tab_groups;
    for (auto git = groups.begin(); git != groups.end();) {
        bool used = false;
        for (const auto& t : s.window_tabs.items)
            if (t->tab_group == git->id) { used = true; break; }
        if (used) ++git; else git = groups.erase(git);
    }
    s.tabOffsets.clear();
}

bool HandleVerticalTabContextMenu(AppState& s, const ui::HitTestResult& hit, POINT screen) {
    using R = ui::HitTestResult;
    size_t index = 0;
    if ((hit.region == R::SidebarItem || hit.region == R::SidebarItemAction) &&
        IsVerticalTabPath(hit.path, &index)) {
        if (index < s.window_tabs.items.size() && s.pane && EnsureMenu(s)) {
            s.tabs.ShowTabMenu(s.window_tabs, static_cast<int>(index), screen, *s.menu);
            BindCurrentLayout(s);
            InvalidateRect(s.hwnd, nullptr, FALSE);
        }
        return true;
    }
    if (hit.region == R::SidebarItem && hit.path == kNewTabRowPath) return true;
    // The tabs header has no section menu (it is not a stored section).
    return (hit.region == R::SidebarHeader || hit.region == R::SidebarHeaderAction) &&
           hit.sidebar_section == kVerticalTabsSectionId;
}

bool HandleTabMiddleClick(AppState& s, int x, int y) {
    using R = ui::HitTestResult;
    const ui::WindowViewModel vm = BuildVm(s, false);
    const D2D1_RECT_F bounds = D2D1::RectF(0, 0, static_cast<float>(s.compositor.Width()),
                                           static_cast<float>(s.compositor.Height()));
    const ui::HitTestResult hit =
        s.renderer.HitTest(vm, bounds, static_cast<float>(x), static_cast<float>(y));
    size_t index = 0;
    if ((hit.region == R::Tab || hit.region == R::TabClose) && hit.index >= 0) {
        index = static_cast<size_t>(hit.index);
    } else if ((hit.region == R::SidebarItem || hit.region == R::SidebarItemAction) &&
               IsVerticalTabPath(hit.path, &index)) {
    } else {
        return false;
    }
    if (index >= s.window_tabs.items.size()) return true;
    CloseLayoutTab(s, index);
    InvalidateRect(s.hwnd, nullptr, FALSE);
    return true;
}

bool HandleFolderMiddleClick(AppState& s, int x, int y) {
    using R = ui::HitTestResult;
    const ui::WindowViewModel vm = BuildVm(s, false);
    const D2D1_RECT_F bounds = D2D1::RectF(0, 0, static_cast<float>(s.compositor.Width()),
                                           static_cast<float>(s.compositor.Height()));
    const ui::HitTestResult hit =
        s.renderer.HitTest(vm, bounds, static_cast<float>(x), static_cast<float>(y));
    std::wstring path;
    if (hit.region == R::Row && hit.index >= 0) {
        app::Pane* pane = PaneAtSlot(s, hit.pane_index);
        app::Tab* tab = pane ? pane->ActiveTab() : nullptr;
        if (!tab || !tab->snapshot || hit.index >= static_cast<int>(tab->EntryCount())) return false;
        if (!tab->EntryAt(static_cast<size_t>(hit.index)).is_dir) return false;
        path = EntryFullPath(*tab, hit.index);
    } else if (hit.region == R::BreadcrumbSegment) {
        path = hit.path;
    } else if (hit.region == R::SidebarItem) {
        if (IsVerticalTabPath(hit.path) || hit.path.starts_with(kTabPrefix)) return false;
        path = hit.path;
        if (!path.empty() && !fs::IsVirtualPath(path)) {
            // Starred files and other non-folder places have nothing to browse.
            const DWORD attrs = GetFileAttributesW(path.c_str());
            if (attrs == INVALID_FILE_ATTRIBUTES || !(attrs & FILE_ATTRIBUTE_DIRECTORY)) return false;
        }
    } else {
        return false;
    }
    if (path.empty()) return false;
    if (GetKeyState(VK_CONTROL) & 0x8000) OpenFolderTab(s, path);
    else NewBackgroundTab(s, path);
    return true;
}
}
