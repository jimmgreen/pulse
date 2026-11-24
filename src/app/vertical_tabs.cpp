#include "vertical_tabs.h"
#include "app_commands.h"
#include "app_navigation.h"
#include "app_sidebar_refresh.h"
#include "quick_access.h"
#include "last_tab_close.h"
#include "favorites_bar.h"
#include "../common/localization.h"
#include "../ui/ui_renderer.h"
#include "title_buttons.h"
#include "app_runtime.h"
#include "staging_shelf_controller.h"
#include <algorithm>
#include <utility>
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
    // 界面布局: Settings keeps the full chrome (its own navigation and status).
    ui::MainRenderer::UiChromeFlags chrome;
    if (!in_settings) {
        chrome.show_tabs = s.appPrefs.ui_show_tabs;
        chrome.address_in_title = s.appPrefs.ui_address_in_title;
        chrome.command_row = s.appPrefs.ui_command_mode == 0;
        chrome.inline_commands = s.appPrefs.ui_command_mode == 1;
        chrome.toolbar_labels = s.appPrefs.ui_toolbar_labels;
        chrome.status_bar = s.appPrefs.ui_status_bar;
        chrome.sidebar_mode = std::clamp(s.appPrefs.ui_sidebar_mode, 0, 2);
        chrome.selection_float = s.appPrefs.ui_selection_float;
    }
    // 标题栏按钮 (everywhere, settings included).
    chrome.title_buttons.clear();
    for (const auto& button : app::VisibleTitleButtons(s.appPrefs.ui_title_buttons)) {
        if (button.Divider()) { chrome.title_buttons.push_back(-1); continue; }
        for (size_t k = 0; k < app::kTitleButtonIds.size(); ++k)
            if (app::kTitleButtonIds[k] == button.id) chrome.title_buttons.push_back(static_cast<int>(k));
    }
    // 暂存区 leads the group when there is no docked sidebar to hold the tray
    // card and something is staged (ui_modes_mockup pillHTML).
    const int staged = TrayItemTotalCount(s.tray);
    if (!in_settings && chrome.sidebar_mode != 0 && staged > 0) {
        if (!chrome.title_buttons.empty()) chrome.title_buttons.insert(chrome.title_buttons.begin(), -1);
        chrome.title_buttons.insert(chrome.title_buttons.begin(), 5);
        chrome.tray_badge = staged;
        chrome.tray_open = StagingShelfVisible(s) || s.trayView.open;
    }
    s.renderer.SetUiChrome(chrome);
    // 精简侧边栏: places, favorites, this PC, network (and the tabs group);
    // workspaces, saved searches and tags stay in the full sidebar.
    if (!in_settings && s.appPrefs.ui_sidebar_lite) {
        for (auto& group : vm.sidebar) {
            const auto id = static_cast<app::SidebarSectionId>(group.id);
            if (!group.tabs_section && (id == app::SidebarSectionId::Workspaces ||
                id == app::SidebarSectionId::SavedSearches || id == app::SidebarSectionId::Tags))
                group.hidden = true;
        }
    }
    s.renderer.SetSidebarCollapsed(s.appPrefs.sidebar_collapsed);
    if (s.renderer.SidebarPeek() && !s.renderer.SidebarRailVisible(static_cast<float>(s.compositor.Width())))
        CloseSidebarPeek(s);
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
    const float width = static_cast<float>(s.compositor.Width());
    // 浮起 / 隐藏 and the automatic rail: the toggle pins the overlay open or closes it.
    if (s.renderer.SidebarAutoCollapsed(width) || s.renderer.SidebarUndocked()) {
        const bool open = !s.renderer.SidebarPeekVisible(width);
        s.sidebarPeekArmAt = 0;
        if (open) s.renderer.SetSidebarPeek(true, true);
        else CloseSidebarPeek(s);
        InvalidateRect(s.hwnd, nullptr, FALSE);
        return;
    }
    // Pinning a peek keeps the overlay's width, so it expands in place.
    const bool from_peek = s.renderer.SidebarPeekVisible(static_cast<float>(s.compositor.Width()));
    s.sidebarPeekArmAt = 0;
    s.renderer.SetSidebarPeek(false);
    s.appPrefs.sidebar_collapsed = !s.appPrefs.sidebar_collapsed;
    s.renderer.SetSidebarCollapsed(s.appPrefs.sidebar_collapsed, !from_peek);
    if (!s.isolatedTest) s.appPrefs.Save();
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

namespace {
constexpr ULONGLONG kPeekDwellMs = 450;

// 浮起: resting on the window's left edge opens the overlay after a short dwell.
// The outer ~8 DIP are the resize border (WM_NCHITTEST HTLEFT, no client
// mouse moves), so the strip reaches 16 DIP into the client area.
constexpr ULONGLONG kFloatPeekDwellMs = 120;
constexpr float kFloatPeekEdgeDip = 16.0f;

bool SidebarFloats(const AppState& s) { return s.renderer.UiChrome().sidebar_mode == 1 && s.renderer.SidebarUndocked(); }

bool PeekEligible(AppState& s) {
    const float width = static_cast<float>(s.compositor.Width());
    if (!s.appPrefs.sidebar_collapsed && !s.renderer.SidebarAutoCollapsed(width) && !SidebarFloats(s)) return false;
    if (!s.renderer.SidebarAutoCollapsed(width) && s.renderer.SidebarAnimating()) return false;
    if ((GetKeyState(VK_LBUTTON) & 0x8000) != 0) return false;  // drags keep the rail
    if (const app::Tab* tab = ActiveTab(s); tab && tab->current_path.starts_with(L"pulse:settings"))
        return false;
    return true;
}
}

void CloseSidebarPeek(AppState& s) {
    s.sidebarPeekArmAt = 0;
    const bool cooled = s.renderer.SetSidebarEdgeHot(false);
    if (!s.renderer.SidebarPeek()) {
        if (cooled) InvalidateRect(s.hwnd, nullptr, FALSE);
        return;
    }
    s.renderer.SetSidebarPeek(false);
    // The rail itself never scrolls; drop any offset the overlay picked up.
    s.sidebarScroll = 0.0f;
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

void UpdateSidebarPeek(AppState& s, int x, [[maybe_unused]] int y) {
    const float w = static_cast<float>(s.compositor.Width());
    if (!s.appPrefs.sidebar_collapsed && !s.renderer.SidebarAutoCollapsed(w) && !s.renderer.SidebarUndocked()) {
        if (s.sidebarPeekArmAt || s.renderer.SidebarPeek()) CloseSidebarPeek(s);
        return;
    }
    if (s.renderer.SidebarPeek()) {
        if (s.renderer.SidebarPeekPinned()) return;
        // Close once the pointer clears the overlay (the title-bar pin stays
        // reachable because it sits inside the overlay's width).
        if (static_cast<float>(x) > s.renderer.SidebarFullWidth(w) + 12.0f * s.scale)
            CloseSidebarPeek(s);
        return;
    }
    const bool edge = SidebarFloats(s) && static_cast<float>(x) < kFloatPeekEdgeDip * s.scale &&
        static_cast<float>(y) >= s.renderer.SidebarRect(w, static_cast<float>(s.compositor.Height())).top;
    if (s.renderer.SetSidebarEdgeHot(edge)) InvalidateRect(s.hwnd, nullptr, FALSE);
    if (edge) {
        if (!s.sidebarPeekArmAt) s.sidebarPeekArmAt = GetTickCount64();
        return;
    }
    // The rail reads on its own (section titles, drive letters, tooltips), so
    // hovering it no longer opens the full sidebar; the toggle and Ctrl+B do.
    s.sidebarPeekArmAt = 0;
}

void DismissSidebarPeekOutside(AppState& s, int x, int y) {
    const float w = static_cast<float>(s.compositor.Width());
    if (!s.renderer.SidebarPeekVisible(w)) return;
    const auto toggle = s.renderer.SidebarToggleRect(w);
    if (x >= toggle.left && x < toggle.right && y >= toggle.top && y < toggle.bottom) return;
    const auto sidebar = s.renderer.SidebarRect(w, static_cast<float>(s.compositor.Height()));
    if (x < sidebar.left || x >= sidebar.right || y < sidebar.top || y >= sidebar.bottom)
        CloseSidebarPeek(s);
}

void DragSidebarPeek(AppState& s, POINT client) {
    if (!SidebarFloats(s) || s.renderer.SidebarPeekPinned()) return;
    const float w = static_cast<float>(s.compositor.Width());
    const D2D1_RECT_F sb = s.renderer.SidebarRect(w, static_cast<float>(s.compositor.Height()));
    const float x = static_cast<float>(client.x), y = static_cast<float>(client.y);
    if (s.renderer.SidebarPeek()) {
        if (x > s.renderer.SidebarFullWidth(w) + 12.0f * s.scale || y < sb.top - 12.0f * s.scale) CloseSidebarPeek(s);
        return;
    }
    if (x >= 0.0f && x < (kFloatPeekEdgeDip + 4.0f) * s.scale && y >= sb.top) {
        s.sidebarPeekArmAt = 0;
        s.renderer.SetSidebarPeek(true);
        InvalidateRect(s.hwnd, nullptr, FALSE);
    }
}

bool TickSidebarPeek(AppState& s, ULONGLONG now) {
    if (!s.sidebarPeekArmAt || s.renderer.SidebarPeek()) return false;
    if (now - s.sidebarPeekArmAt < (SidebarFloats(s) ? kFloatPeekDwellMs : kPeekDwellMs)) return false;
    s.sidebarPeekArmAt = 0;
    if (!PeekEligible(s)) return false;
    POINT pt{};
    GetCursorPos(&pt);
    ScreenToClient(s.hwnd, &pt);
    const float h = static_cast<float>(s.compositor.Height());
    if (pt.x < 0 || static_cast<float>(pt.x) >= ui::kSidebarRailWidthDip * s.scale ||
        static_cast<float>(pt.y) < s.renderer.SidebarRect(static_cast<float>(s.compositor.Width()), h).top) return false;
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
        ArmTabDoubleClick(s, index);
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

void ArmTabDoubleClick(AppState& s, size_t index) {
    s.tabPressTab = index < s.window_tabs.items.size() ? s.window_tabs.items[index].get() : nullptr;
    s.tabPressTime = GetMessageTime();
}

bool HandleTabDoubleClick(AppState& s, const ui::HitTestResult& hit) {
    using R = ui::HitTestResult;
    const void* pressed = std::exchange(s.tabPressTab, nullptr);
    size_t index = 0;
    if (hit.region == R::Tab && hit.index >= 0) {
        index = static_cast<size_t>(hit.index);
    } else if (hit.region == R::SidebarItem && IsVerticalTabPath(hit.path, &index)) {
    } else {
        return false;
    }
    if (index >= s.window_tabs.items.size()) return false;
    if (!app::TabDoubleClickCloses(s.appPrefs.close_tab_on_double_click, pressed,
            s.window_tabs.items[index].get(),
            static_cast<DWORD>(GetMessageTime() - s.tabPressTime), GetDoubleClickTime()))
        return false;
    // Same as the close button: strip widths hold so the next tab's body
    // stays under the pointer for another double-click.
    if (hit.region == R::Tab)
        s.renderer.FreezeTabWidths(BuildVm(s, false), static_cast<float>(s.compositor.Width()));
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
    // A 收藏夹 group (the bar's button, the sidebar's row): all its folders.
    if ((hit.region == R::FavoritesBarItem || hit.region == R::SidebarItem) && OpenFavoritesGroupTabs(s, hit.path))
        return true;
    if (hit.region == R::Row && hit.index >= 0) {
        app::Pane* pane = PaneAtSlot(s, hit.pane_index);
        app::Tab* tab = pane ? pane->ActiveTab() : nullptr;
        if (!tab || !tab->snapshot || hit.index >= static_cast<int>(tab->EntryCount())) return false;
        if (!tab->EntryAt(static_cast<size_t>(hit.index)).is_dir) return false;
        path = EntryFullPath(*tab, hit.index);
    } else if (hit.region == R::BreadcrumbSegment) {
        path = hit.path;
    } else if (hit.region == R::FavoritesBarItem || hit.region == R::FavoritesBarChevron) {
        path = FavoritesBarMiddleClickPath(s, hit);
        if (path.empty()) return true;   // a starred file, or the bar's empty part
    } else if (hit.region == R::SidebarItem) {
        if (IsVerticalTabPath(hit.path) || hit.path.starts_with(kTabPrefix)) return false;
        const int builtin = app::BuiltinQuickAccessFromKey(
            QuickAccessRowKey(s, vm, hit.sidebar_section, hit.sidebar_item));
        if (OpenSidebarKnownFolder(s, builtin, (GetKeyState(VK_CONTROL) & 0x8000)
                ? SidebarFolderOpenMode::NewTab : SidebarFolderOpenMode::BackgroundTab)) return true;
        // Tag group headers and 所有标签 › are sidebar controls, not places.
        if (app::IsTagGroupPath(hit.path) || hit.path == app::MakeTagManagerPath()) return true;
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