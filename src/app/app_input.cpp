#include "reopen_closed.h"
#include "git_ui.h"
#include "ai_settings_ui.h"
#include "ai_assistant.h"
#include "folder_alias.h"
#include "app_wall_hint.h"
#include "arrival_select.h"
#include "pane_column_input.h"
#include "../ui/ui_renderer.h"
// app_input.cpp — extracted from app_main.cpp.
#include "quick_access.h"
#include "tag_manager.h"
#include "tag_view.h"
#include "filter_tag_complete.h"
#include "sidebar_tag_drag.h"
#include "app_prompts.h"
#include "vertical_tabs.h"
#include "favorites_bar.h"
#include "tab_detach.h"
#include "tab_spring.h"
#include "tab_shortcuts.h"
#include "explorer_shortcuts.h"
#include "window_overview_ui.h"
#include "app_updates.h"
#include "app_internal.h"
#include "global_search_controller.h"
#include "selection_caps.h"
#include "app_input.h"
#include "app_hover.h"
#include "partial_redraw.h"
#include "app_hover_preview.h"
#include "app_column_view.h"
#include "app_details_tree.h"
#include "details_column_menu.h"
#include "../ui/lumatext_renderer.h"
#include "../ui/fluent_menu.h"
#include "../ui/toolbar_picker.h"
#include "../ui/drag_drop.h"
#include "../ui/file_operation_dialog.h"
#include "../ui/batch_rename_dialog.h"
#include "../ui/quick_preview_window.h"
#include "../ui/typography.h"
#include "../ui/color_picker.h"
#include "../common/localization.h"
#include "../common/text_format.h"
#include "../common/path_utils.h"
#include "../common/diagnostics_exporter.h"
#include "../common/runtime_log.h"
#include "snapshot_patch.h"
#include "session.h"
#include "context_menu.h"
#include "app_network_page.h"
#include "batch_rename.h"
#include "blank_pane_click.h"
#include "drop_staging.h"
#include "link_resolve.h"
#include "toolbar_apps.h"
#include "resource.h"
#include "../ops/clipboard.h"
#include "../ipc/ctx_menu_util.h"
#include "../fs/device_path.h"
#include "device_registry.h"
#include "window_identity_ui.h"
#include "window_switcher_ui.h"
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <uxtheme.h>
#include <psapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shlwapi.h>
#include <algorithm>
#include <cmath>
#include <cwctype>
#include <thread>
#include <unordered_set>
#include <utility>

using namespace pulse;

namespace pulse {
// The split New button. On the network page and a server's share list it is
// 连接服务器 (main part: the dialog, chevron: its menu).
static void PressNewButton(AppState& s, bool arrow) {
    const app::Tab* tab = ActiveTab(s);
    const bool connect = tab && IsConnectServerPage(tab->current_path);
    if (connect) {
        if (arrow) ShowConnectServerMenu(s);
        else RunConnectServerDialog(s);
        return;
    }
    if (arrow) ShowNewDropdown(s);
    else CreateNewItem(s, ToolbarNewCreatesFolder(s));
}
// Ctrl+Shift+N: 新建文件夹, or 连接服务器 where nothing can be created (the
// network page, a server's share list) - the New button's slot either way.
static void NewFolderOrConnect(AppState& s) {
    const app::Tab* tab = ActiveTab(s);
    if (tab && IsConnectServerPage(tab->current_path)) RunConnectServerDialog(s);
    else CreateNewItem(s, true);
}
// Settings toggles that belong to the network page (40-42, 46) are handled
// by app_network_page; the rest by the settings controller.
static void ToggleSetting(AppState& s, int index) {
    if (HandleNetworkSettingToggle(s, index)) return;
    s.settings.ToggleUi(index);
}
bool HandleBrowserNavigation(AppState& s, LPARAM command) {
    switch (GET_APPCOMMAND_LPARAM(command)) {
    case APPCOMMAND_BROWSER_BACKWARD:
        GoBack(s);
        return true;
    case APPCOMMAND_BROWSER_FORWARD:
        GoForward(s);
        return true;
    default:
        return false;
    }
}

bool UpdateSplitterDrag(AppState& s, int mx, int my) {
    if (!s.splitterDragging) return false;
    if ((GetKeyState(VK_LBUTTON) & 0x8000) == 0) {
        s.splitterDragging = false;
        s.splitterDragIndex = -1;
        return true;
    }
    if (s.splitterDragIndex < 0 ||
        s.splitterDragIndex >= static_cast<int>(s.splitterNodes.size())) {
        return true;
    }
    app::SplitContainer* node = s.splitterNodes[static_cast<size_t>(s.splitterDragIndex)];
    if (!node) return true;
    app::ApplySplitRatio(*node, s.splitterParentBounds, 8.0f * s.scale,
                         static_cast<float>(mx), static_cast<float>(my));
    // Motion 4b: the divider holds at thirds and the middle within 12 px,
    // unless the panes' minimum sizes keep it from getting there.
    s.splitterSnap = 0.0f;
    {
        const float gap = 8.0f * s.scale;
        const D2D1_RECT_F& b = s.splitterParentBounds;
        const bool vertical = node->orientation == app::SplitOrientation::Vertical;
        const float span = (vertical ? b.right - b.left : b.bottom - b.top) - gap;
        const float origin = (vertical ? b.left : b.top) + gap * 0.5f;
        const float free_ratio = node->ratio;
        for (const float snap : {1.0f / 3.0f, 0.5f, 2.0f / 3.0f}) {
            if (span <= 1.0f || std::fabs(free_ratio - snap) * span >= 12.0f * s.scale) continue;
            const float at = origin + snap * span;
            app::ApplySplitRatio(*node, b, gap, vertical ? at : 0.0f, vertical ? 0.0f : at);
            if (std::fabs(node->ratio - snap) < 1e-4f) {
                s.splitterSnap = snap;
            } else {
                node->ratio = free_ratio;
            }
            break;
        }
    }
    s.hoverRegion = static_cast<int>(ui::HitTestResult::Splitter);
    s.hoverControlIndex = s.splitterDragIndex;
    return true;
}
void ClearDropFeedback(AppState& s) {
    s.dropLaunchProgram.clear();
    s.dropLaunchShortcut.clear();
    s.toolbarDropMode = false;
    s.toolbarDropSources.clear();
    s.toolbarDropProgram.clear();
    s.dropToolbarApp = -1;
    s.dropToolbarForce = false;
    s.dropToolbarPin = false;
    s.dropRow = -1;
    s.dropPaneIndex = -1;
    s.dropHeader = false;
    s.dropBreadcrumb = -1;
    s.dropSidebar = -1;
    s.dropTray = false;
    s.dragFilesOver = false;
    s.dropTrayMove = false;
    s.dropQuickAccess = false;
    s.dropFavorite = -1;
    s.dropFavoriteAdd = false;
    SetFavoritesBarDropGroup({});
    s.dropBadge.clear();
    s.dropBadgeMove = false;
    s.dropDestDir.clear();
    s.springRow = -1;
    s.springStart = 0;
    s.springTab = -1;
    s.springTabStart = 0;
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

std::wstring BaseName(const std::wstring& path) {
    std::wstring_view v = path;
    if (v.size() > 1 && v.back() == L'\\') v.remove_suffix(1);
    auto pos = v.find_last_of(L"\\/");
    return std::wstring(pos != std::wstring::npos ? v.substr(pos + 1) : v);
}

bool PointOnPaneHeader(const AppState& s, const ui::WindowViewModel& vm,
                              int pane_index, float x, float y) {
    D2D1_RECT_F rc{};
    if (pane_index >= 0 && pane_index < static_cast<int>(vm.pane_slots.size())) {
        rc = vm.pane_slots[static_cast<size_t>(pane_index)].rect;
    } else if (vm.pane_slots.empty() && pane_index == 0) {
        rc = s.renderer.ContentRect(static_cast<float>(s.compositor.Width()),
                                    static_cast<float>(s.compositor.Height()));
    } else {
        return false;
    }
    const float header = s.renderer.PaneHeaderHeight();
    return x >= rc.left && x < rc.right && y >= rc.top && y < rc.top + header;
}

// Folder path, or a .lnk that DropExecute will resolve. No COM on drag-over.
std::wstring HeaderDropHint(const std::vector<std::wstring>& sources) {
    std::wstring folder = ui::FirstDroppableFolder(sources);
    if (!folder.empty()) return folder;
    for (const auto& source : sources) {
        if (ui::LooksLikeFolderShortcut(source)) return source;
    }
    return {};
}

std::wstring ResolveHeaderDropFolder(const std::vector<std::wstring>& sources) {
    std::wstring folder = ui::FirstDroppableFolder(sources);
    if (!folder.empty()) return fs::NormalizePath(folder);
    for (const auto& source : sources) {
        if (!ui::LooksLikeFolderShortcut(source)) continue;
        fs::DirEntry entry;
        if (app::ResolveLink(source, entry) && entry.link_target_is_dir &&
            !entry.link_target.empty())
            return fs::NormalizePath(entry.link_target);
    }
    return {};
}

// The program a file row hands dropped items to: an .exe/.com/.bat/.cmd, or a
// shortcut to one, as in Explorer. Shortcut targets were resolved by the
// listing worker, so DragOver reads only the snapshot.
static std::wstring DropLaunchProgramAt(const app::Tab& tab, int index) {
    const auto& entry = tab.EntryAt(index);
    if (entry.is_dir || entry.change_record_only || !entry.recycle_path.empty()) return {};
    if (ui::LooksLikeFolderShortcut(entry.name)) {
        return !entry.link_target.empty() && !entry.link_target_is_dir &&
                ui::IsDropLaunchProgram(entry.link_target)
            ? entry.link_target : std::wstring();
    }
    std::wstring full = EntryFullPath(tab, index);
    return ui::IsDropLaunchProgram(full) ? full : std::wstring();
}

// Resolves the drop target under pt (client coords), updates feedback state,
// and returns the DROPEFFECT_* to report back. Also drives the 800ms
// spring-loaded folder enter and Esc-back.
static DWORD FolderDropEffect(DWORD key_state, const std::vector<std::wstring>& sources,
                              const std::wstring& dest, DWORD allowed, DWORD preferred_effect);

// The phone folder Pulse's own drag-out started in (StartDragOut): its rows
// released over that same folder are not a target, as in Explorer.
static std::wstring g_drag_device_dir;

// A phone folder that takes a drop: the shell copies into it (MTP), run in
// the host by DropExecute. False for a phone's root, a read-only storage, an
// iPhone and folders whose shell item is not known yet.
static bool DeviceDropFolder(const std::wstring& dest) {
    if (!fs::IsDevicePath(dest)) return false;
    const auto& devices = app::DeviceRegistry::Instance();
    return devices.FolderWritable(dest) && !devices.FolderShellId(dest).empty();
}

// Drag of a phone's items (shell item ids, no CF_HDROP): only the shell can
// read them, so they land in a folder and nowhere else.
static bool ShellItemDrag(const std::vector<std::wstring>& sources) {
    return !sources.empty() && ui::IsShellItemId(sources.front());
}

// Drops the target ResolveDropTargetImpl picked; the toolbar keeps its mode.
static DWORD RejectDropTarget(AppState& s) {
    s.dropRow = -1;
    s.dropPaneIndex = -1;
    s.dropHeader = false;
    s.dropBreadcrumb = -1;
    s.dropSidebar = -1;
    s.dropTray = false;
    s.dropQuickAccess = false;
    s.dropFavorite = -1;
    s.dropFavoriteAdd = false;
    SetFavoritesBarDropGroup({});
    s.dropToolbarApp = -1;
    s.dropToolbarPin = false;
    s.dropDestDir.clear();
    s.dropLaunchProgram.clear();
    s.dropLaunchShortcut.clear();
    s.dropBadge.clear();
    s.dropBadgeMove = false;
    InvalidateRect(s.hwnd, nullptr, FALSE);
    return DROPEFFECT_NONE;
}

static DWORD ResolveDropTargetImpl(AppState& s, const std::vector<std::wstring>& sources,
                                   POINT pt, DWORD key_state, DWORD allowed,
                                   DWORD preferred_effect);

DWORD ResolveDropTarget(AppState& s, const std::vector<std::wstring>& sources,
                        POINT pt, DWORD key_state, DWORD allowed,
                        DWORD preferred_effect) {
    const DWORD effect = ResolveDropTargetImpl(s, sources, pt, key_state, allowed, preferred_effect);
    if (effect == DROPEFFECT_NONE) return effect;
    if (!g_drag_device_dir.empty() && _wcsicmp(g_drag_device_dir.c_str(), s.dropDestDir.c_str()) == 0)
        return RejectDropTarget(s);
    if (!ShellItemDrag(sources)) return effect;
    // A phone's items: copied or moved into a folder by the shell, never
    // staged, starred, pinned, tagged, opened with an app or as a pane title.
    const bool folder = !s.dropDestDir.empty() && !s.dropTray && !s.dropQuickAccess &&
        !s.dropFavoriteAdd && FavoritesBarDropGroup().empty() && !s.dropHeader &&
        s.dropLaunchProgram.empty() && s.dropToolbarApp < 0 && !s.dropToolbarPin &&
        (!fs::IsVirtualPath(s.dropDestDir) || DeviceDropFolder(s.dropDestDir));
    if (!folder || (effect != DROPEFFECT_COPY && effect != DROPEFFECT_MOVE)) return RejectDropTarget(s);
    return effect;
}

static DWORD ResolveDropTargetImpl(AppState& s, const std::vector<std::wstring>& sources,
                                   POINT pt, DWORD key_state, DWORD allowed,
                                   DWORD preferred_effect) {
    s.dropRow = -1;
    s.dropPaneIndex = -1;
    s.dropHeader = false;
    s.dropBreadcrumb = -1;
    s.dropSidebar = -1;
    s.dropTray = false;
    s.dropQuickAccess = false;
    s.dropFavorite = -1;
    s.dropFavoriteAdd = false;
    SetFavoritesBarDropGroup({});
    s.dropDestDir.clear();
    s.dropLaunchProgram.clear();
    s.dropLaunchShortcut.clear();
    s.dropBadge.clear();
    s.dropBadgeMove = false;
    // Files from outside (or another pane) are over the window: the shelf
    // opens its 复制暂存 / 移动暂存 halves. Shift picks the move half anywhere.
    const bool shift = (key_state & MK_SHIFT) != 0;
    if (!s.trayDragOut && !sources.empty() && !ShellItemDrag(sources)) {
        if (!s.dragFilesOver) InvalidateRect(s.hwnd, nullptr, FALSE);
        s.dragFilesOver = true;
        s.dropTrayMove = shift && (allowed & DROPEFFECT_MOVE) != 0;
    }
    if (sources.empty() || !(allowed & (DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK)))
        return DROPEFFECT_NONE;

    // Esc after a spring-loaded enter: go back instead of cancelling (self drags).
    if ((GetAsyncKeyState(VK_ESCAPE) & 0x8000) && s.springEntered) {
        GoBack(s);
        s.springEntered = false;
        return DROPEFFECT_NONE;
    }

    // The toolbar holds the pinned apps while files are dragged over the
    // window; the layout (and so the hit test) depends on it.
    UpdateToolbarDropMode(s, sources);
    ui::WindowViewModel vm = BuildVm(s);
    D2D1_RECT_F rect = D2D1::RectF(0, 0, (float)s.compositor.Width(), (float)s.compositor.Height());
    ui::HitTestResult hit = s.renderer.HitTest(vm, rect, (float)pt.x, (float)pt.y);
    // Near either end of a long path in the 地址栏: page it that way.
    if ((hit.region == ui::HitTestResult::AddressBar || hit.region == ui::HitTestResult::BreadcrumbSegment) &&
        s.renderer.DragScrollBreadcrumb(vm, rect.right, (float)pt.x)) {
        InvalidateRect(s.hwnd, nullptr, FALSE);
        hit = s.renderer.HitTest(vm, rect, (float)pt.x, (float)pt.y);
    }
    DWORD toolbar_effect = DROPEFFECT_NONE;
    if (ResolveToolbarAppDrop(s, vm, hit, sources, pt, key_state, allowed, toolbar_effect)) {
        s.springRow = -1;
        return toolbar_effect;
    }

    // Resting on a tab of the strip opens it (M6e); the tab takes the drop
    // into the folder it shows.
    const bool on_strip_tab = hit.region == ui::HitTestResult::Tab && hit.index >= 0 &&
                              hit.index < static_cast<int>(s.window_tabs.items.size());
    if (app::AdvanceTabSpring(s.springTab, s.springTabStart, on_strip_tab ? hit.index : -1, s.window_tabs.active,
                              GetTickCount64()))
        SwitchTab(s, static_cast<size_t>(hit.index));

    app::Pane* hitPane = PaneAtSlot(s, hit.pane_index);
    app::Tab* tab = hitPane ? hitPane->ActiveTab() : ActiveTab(s);
    if (!tab) return DROPEFFECT_NONE;

    std::wstring destName;
    const bool file_row = hit.region == ui::HitTestResult::Row && hit.index >= 0 &&
        tab->snapshot && hit.index < (int)tab->EntryCount() && !tab->EntryAt(hit.index).is_dir;
    const std::wstring launch_program = file_row ? DropLaunchProgramAt(*tab, hit.index) : std::wstring();
    if (hit.region == ui::HitTestResult::Row && hit.index >= 0 &&
        tab->snapshot && hit.index < (int)tab->EntryCount() &&
        tab->EntryAt(hit.index).is_dir) {
        if (tab->EntryAt(hit.index).change_record_only ||
            !tab->EntryAt(hit.index).recycle_path.empty()) return DROPEFFECT_NONE;
        std::wstring full = EntryFullPath(*tab, hit.index);
        if (full.empty()) return DROPEFFECT_NONE;
        // The dragged folder's own row neither accepts the drop nor
        // spring-opens: pausing on it right after the drag starts is common.
        if (ui::DropIntoOwnFolder(sources, full)) {
            s.springRow = -1;
            InvalidateRect(s.hwnd, nullptr, FALSE);
            return DROPEFFECT_NONE;
        }
        s.dropDestDir = full;
        s.dropRow = hit.index;
        s.dropPaneIndex = hit.pane_index;
        destName = tab->EntryAt(hit.index).name;

        // Spring-loaded: hover 800ms on a folder row enters it (ui.md §7.8).
        if (s.springRow != hit.index) {
            s.springRow = hit.index;
            s.springStart = GetTickCount64();
        } else if (!s.springEntered && GetTickCount64() - s.springStart >= 800) {
            s.springEntered = true;
            s.springRow = -1;
            if (hitPane && hitPane != s.pane) FocusPane(s, hitPane);
            NavigateTo(s, full);
            return DROPEFFECT_NONE;
        }
    } else if (!launch_program.empty()) {
        // Items dropped on a program open with it ("用 X 打开"); nothing is
        // copied or moved, so the source never deletes on a COPY result.
        s.springRow = -1;
        const DWORD effect = (allowed & DROPEFFECT_COPY) ? DROPEFFECT_COPY
            : (allowed & DROPEFFECT_LINK) ? DROPEFFECT_LINK : DROPEFFECT_NONE;
        // A program is not handed itself (its row or its shortcut's target).
        if (effect == DROPEFFECT_NONE || ui::DropIntoOwnFolder(sources, launch_program) ||
            ui::DropIntoOwnFolder(sources, EntryFullPath(*tab, hit.index))) {
            InvalidateRect(s.hwnd, nullptr, FALSE);
            return DROPEFFECT_NONE;
        }
        s.dropLaunchProgram = launch_program;
        s.dropLaunchShortcut = ui::LooksLikeFolderShortcut(tab->EntryAt(hit.index).name)
            ? EntryFullPath(*tab, hit.index) : std::wstring();
        s.dropRow = hit.index;
        s.dropPaneIndex = hit.pane_index;
        s.dropBadge = std::wstring(l10n::Get(l10n::StringId::OpenWith).c_str()) +
            fs::StripLnkSuffix(tab->EntryAt(hit.index).name) + l10n::Get(l10n::StringId::Text2758).c_str();
        s.dropBadgeX = (float)pt.x;
        s.dropBadgeY = (float)pt.y;
        InvalidateRect(s.hwnd, nullptr, FALSE);
        return effect;
    } else if (on_strip_tab) {
        s.springRow = -1;
        const app::Tab* view = s.window_tabs.items[static_cast<size_t>(hit.index)]->ActiveFolder();
        if (view && !view->current_path.empty() && !fs::IsVirtualPath(view->current_path)) {
            s.dropDestDir = view->current_path;
            destName = BaseName(view->current_path);
        }
    } else if ((hit.region == ui::HitTestResult::FavoritesBarItem ||
                hit.region == ui::HitTestResult::FavoritesBarChevron) && hit.index < 0) {
        // 收藏栏's empty part: what is dropped there gets starred.
        s.springRow = -1;
        s.dropFavoriteAdd = true;
        s.dropBadge = l10n::Get(l10n::StringId::FavoritesBarDropAdd).c_str();
        s.dropBadgeX = (float)pt.x;
        s.dropBadgeY = (float)pt.y;
        InvalidateRect(s.hwnd, nullptr, FALSE);
        if (allowed & DROPEFFECT_LINK) return DROPEFFECT_LINK;
        if (allowed & DROPEFFECT_COPY) return DROPEFFECT_COPY;
        return DROPEFFECT_NONE;
    } else if (hit.region == ui::HitTestResult::FavoritesBarItem ||
               hit.region == ui::HitTestResult::FavoritesBarChevron) {
        // A group takes folders: they get starred into it (files cannot join
        // a group, and a menu cannot open during the drag).
        if (const std::wstring group = FavoritesBarGroupAt(s, hit); !group.empty()) {
            s.springRow = -1;
            const app::StarredGroup* info = s.places.FindStarredGroup(group);
            bool any_folder = false;
            for (const auto& source : sources) {
                const DWORD attrs = GetFileAttributesW(source.c_str());
                any_folder = any_folder || (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY));
            }
            s.dropBadgeX = (float)pt.x;
            s.dropBadgeY = (float)pt.y;
            if (!any_folder || !info) {
                s.dropBadge = l10n::Get(l10n::StringId::FavoritesBarNotFolder).c_str();
                InvalidateRect(s.hwnd, nullptr, FALSE);
                return DROPEFFECT_NONE;
            }
            SetFavoritesBarDropGroup(group);
            s.dropFavorite = hit.index;
            wchar_t badge[256]{};
            swprintf_s(badge, l10n::Get(l10n::StringId::FavoritesBarDropGroupFormat).c_str(), info->name.c_str());
            s.dropBadge = badge;
            InvalidateRect(s.hwnd, nullptr, FALSE);
            if (allowed & DROPEFFECT_LINK) return DROPEFFECT_LINK;
            if (allowed & DROPEFFECT_COPY) return DROPEFFECT_COPY;
            return DROPEFFECT_NONE;
        }
        // A favorite folder takes the drop like a breadcrumb; a file does not.
        bool offline = false;
        const std::wstring folder = FavoritesBarDropFolder(s, hit, &offline);
        // Spring-loaded like a folder row (ui.md §7.8): resting 800ms on it
        // opens it here, Esc goes back. springRow holds bar entries below -1.
        const int spring_key = -2 - hit.index;
        if (folder.empty() || ui::DropIntoOwnFolder(sources, folder)) {
            s.springRow = -1;
        } else if (s.springRow != spring_key) {
            s.springRow = spring_key;
            s.springStart = GetTickCount64();
        } else if (!s.springEntered && GetTickCount64() - s.springStart >= 800) {
            s.springEntered = true;
            s.springRow = -1;
            NavigateTo(s, folder);
            return DROPEFFECT_NONE;
        }
        if (folder.empty()) {
            s.dropBadge = l10n::Get(offline ? l10n::StringId::FavoritesBarUnreachable
                                            : l10n::StringId::FavoritesBarNotFolder).c_str();
            s.dropBadgeX = (float)pt.x;
            s.dropBadgeY = (float)pt.y;
            InvalidateRect(s.hwnd, nullptr, FALSE);
            return DROPEFFECT_NONE;
        }
        s.dropDestDir = folder;
        s.dropFavorite = hit.index;
        destName = BaseName(folder);
    } else if (hit.region == ui::HitTestResult::BreadcrumbSegment && !hit.path.empty()) {
        s.dropDestDir = hit.path;
        s.dropBreadcrumb = hit.index;
        destName = BaseName(hit.path);
        s.springRow = -1;
    } else if ((hit.region == ui::HitTestResult::SidebarHeader ||
                hit.region == ui::HitTestResult::SidebarHeaderAction) &&
               hit.index >= 0 && hit.index < static_cast<int>(vm.sidebar.size()) &&
               vm.sidebar[static_cast<size_t>(hit.index)].add_action ==
                   ui::SidebarAddAction::AddQuickAccess) {
        // Folders dropped on the Quick access header (or its +) get pinned,
        // as in Explorer. Pinned rows themselves stay move/copy targets.
        s.springRow = -1;
        if (HeaderDropHint(sources).empty()) {
            InvalidateRect(s.hwnd, nullptr, FALSE);
            return DROPEFFECT_NONE;
        }
        s.dropQuickAccess = true;
        s.dropBadge = l10n::Get(l10n::StringId::PinQuickAccess).c_str();
        s.dropBadgeX = (float)pt.x;
        s.dropBadgeY = (float)pt.y;
        InvalidateRect(s.hwnd, nullptr, FALSE);
        if (allowed & DROPEFFECT_LINK) return DROPEFFECT_LINK;
        if (allowed & DROPEFFECT_COPY) return DROPEFFECT_COPY;
        return DROPEFFECT_NONE;
    } else if (hit.region == ui::HitTestResult::SidebarItem && !hit.path.empty()) {
        if (hit.path.starts_with(L"pulse:tag:")) {
            if (!(allowed & DROPEFFECT_COPY)) return DROPEFFECT_NONE;
            s.dropDestDir = hit.path;
            s.dropSidebar = hit.index;
            destName.clear();
            s.springRow = -1;
            s.dropBadge = l10n::Get(l10n::StringId::DropTag).c_str();
            s.dropBadgeX = (float)pt.x;
            s.dropBadgeY = (float)pt.y;
            InvalidateRect(s.hwnd, nullptr, FALSE);
            return DROPEFFECT_COPY;
        }
        if (hit.path.starts_with(L"pulse:workspace:")) {
            std::wstring kind, rest;
            app::ParsePulsePath(hit.path, &kind, &rest);
            int wi = _wtoi(rest.c_str());
            if (wi >= 0 && wi < static_cast<int>(s.places.workspaces.size()))
                s.dropDestDir = s.places.workspaces[static_cast<size_t>(wi)].root;
            else
                s.dropDestDir.clear();
        } else if (hit.path.starts_with(L"pulse:")) {
            s.dropDestDir.clear();
        } else {
            s.dropDestDir = hit.path;
        }
        s.dropSidebar = hit.index;
        destName = BaseName(s.dropDestDir.empty() ? hit.path : s.dropDestDir);
        s.springRow = -1;
    } else if (hit.pane_index >= 0 &&
               PointOnPaneHeader(s, vm, hit.pane_index,
                                 static_cast<float>(pt.x), static_cast<float>(pt.y))) {
        // Drag a folder onto a pane title bar (path + nav/view) to open it there.
        const std::wstring folder = HeaderDropHint(sources);
        if (!folder.empty()) {
            s.dropHeader = true;
            s.dropPaneIndex = hit.pane_index;
            s.dropDestDir = folder;
            destName = BaseName(folder);
            s.springRow = -1;
            s.dropBadge = l10n::Get(l10n::StringId::DropOpen).c_str() + destName;
            s.dropBadgeX = (float)pt.x;
            s.dropBadgeY = (float)pt.y;
            InvalidateRect(s.hwnd, nullptr, FALSE);
            if (allowed & DROPEFFECT_COPY) return DROPEFFECT_COPY;
            if (allowed & DROPEFFECT_LINK) return DROPEFFECT_LINK;
            return DROPEFFECT_NONE;
        }
        s.dropDestDir = tab->current_path;
        s.dropPaneIndex = hit.pane_index;
        destName = BaseName(tab->current_path);
        s.springRow = -1;
    } else if (hit.pane_index >= 0 ||
               hit.region == ui::HitTestResult::Row ||
               hit.region == ui::HitTestResult::Pane ||
               hit.region == ui::HitTestResult::PaneHeader ||
               hit.region == ui::HitTestResult::ColumnHeader ||
               hit.region == ui::HitTestResult::FilterBox ||
               (hit.region == ui::HitTestResult::Scrollbar && hit.sub_index != 3)) {
        s.dropDestDir = tab->current_path;
        s.dropPaneIndex = hit.pane_index;
        destName = BaseName(tab->current_path);
        s.springRow = -1;
    }
    // Tray zone: bottom card of the sidebar = staging, not a move.
    D2D1_RECT_F sb = s.renderer.SidebarRect(rect.right, rect.bottom);
    if (s.dropDestDir.empty() && pt.x < sb.right && !s.trayDragOut) {
        ui::WindowViewModel trayVm = BuildVm(s);
        D2D1_RECT_F trayRc = s.renderer.StagingTrayRect(trayVm, rect.right, rect.bottom);
        if (pt.x >= trayRc.left && pt.x < trayRc.right &&
            pt.y >= trayRc.top && pt.y < trayRc.bottom) {
            if (!(allowed & DROPEFFECT_COPY)) return DROPEFFECT_NONE;
            s.dropTray = true;
            s.springRow = -1;
            // Left half stages a copy batch, right half a move batch; Shift
            // flips the half under the cursor. The source is never touched
            // here, so the OLE effect stays COPY.
            const bool right_half = pt.x >= (trayRc.left + trayRc.right) * 0.5f;
            const bool stage_move = (right_half != shift) && (allowed & DROPEFFECT_MOVE) != 0;
            s.dropTrayMove = stage_move;
            s.dropBadge = l10n::Get(stage_move ? l10n::StringId::TrayGhostMove
                                               : l10n::StringId::TrayGhostCopy).c_str();
            s.dropBadgeMove = stage_move;
            s.dropBadgeX = (float)pt.x;
            s.dropBadgeY = (float)pt.y;
            InvalidateRect(s.hwnd, nullptr, FALSE);
            return DROPEFFECT_COPY; // staging never moves the source
        }
    }
    if (s.dropDestDir.empty()) {
        InvalidateRect(s.hwnd, nullptr, FALSE);
        return DROPEFFECT_NONE;
    }
    // A combined tag view takes a drop as its 且 tags (none: no drop).
    if (s.dropDestDir.starts_with(L"pulse:tags:")) {
        if (!(allowed & DROPEFFECT_COPY) || !TagViewDropTags(s, s.dropDestDir, nullptr)) {
            s.dropPaneIndex = -1;
            s.dropDestDir.clear();
            InvalidateRect(s.hwnd, nullptr, FALSE);
            return DROPEFFECT_NONE;
        }
        s.dropBadge = l10n::Get(l10n::StringId::DropTag).c_str();
        s.dropBadgeX = (float)pt.x;
        s.dropBadgeY = (float)pt.y;
        InvalidateRect(s.hwnd, nullptr, FALSE);
        return DROPEFFECT_COPY;
    }
    // A view with no folder behind it (search results, Recent, This PC, the
    // Recycle Bin) takes nothing: show "no drop" rather than a copy badge for
    // a drop that DropExecute refuses. Tag views take tags; a writable phone
    // folder takes files through the shell.
    if (fs::IsVirtualPath(s.dropDestDir) && !s.dropDestDir.starts_with(L"pulse:tag:") &&
        !DeviceDropFolder(s.dropDestDir)) {
        return RejectDropTarget(s);
    }
    // A folder released on its own row (or into its own subtree) is not a
    // target: Explorer shows "no drop" there, and the move would fail.
    if (!s.dropDestDir.starts_with(L"pulse:") && ui::DropIntoOwnFolder(sources, s.dropDestDir)) {
        s.dropRow = -1;
        s.dropPaneIndex = -1;
        s.dropBreadcrumb = -1;
        s.dropSidebar = -1;
        s.dropFavorite = -1;
        s.dropDestDir.clear();
        InvalidateRect(s.hwnd, nullptr, FALSE);
        return DROPEFFECT_NONE;
    }

    DWORD effect = FolderDropEffect(key_state, sources, s.dropDestDir, allowed, preferred_effect);
    s.dropBadge = (effect == DROPEFFECT_LINK ? l10n::Get(l10n::StringId::DropLink).c_str() :
                   effect == DROPEFFECT_MOVE ? l10n::Get(l10n::StringId::DropMove).c_str() :
                   l10n::Get(l10n::StringId::DropCopy).c_str()) + destName;
    // Say how to switch before the drop, not after (Explorer rules: Ctrl copies, Shift moves).
    if (effect == DROPEFFECT_MOVE && (allowed & DROPEFFECT_COPY))
        s.dropBadge += l10n::Get(l10n::StringId::DropAltCopy);
    else if (effect == DROPEFFECT_COPY && (allowed & DROPEFFECT_MOVE))
        s.dropBadge += l10n::Get(l10n::StringId::DropAltMove);
    s.dropBadgeMove = effect == DROPEFFECT_MOVE;
    s.dropBadgeX = (float)pt.x;
    s.dropBadgeY = (float)pt.y;
    InvalidateRect(s.hwnd, nullptr, FALSE);
    return effect;
}

// Pulse's own drag-out is running (StartDragOut): its sources are never an
// archive manager's temporary extraction (#55).
static bool g_internal_drag = false;

// Alt-drop (or Ctrl+Shift, Explorer's keys): a shortcut to each item in
// `dest`, named "<name> - 快捷方式.lnk" and numbered when that is taken;
// nothing is copied or moved. True when at least one was created.
static bool CreateDropShortcuts(AppState& s, const std::vector<std::wstring>& sources,
                                const std::wstring& dest) {
    const std::wstring dir = path::StripExtendedPathPrefix(fs::NormalizePath(dest));
    size_t created = 0, failed = 0;
    for (const auto& source : sources) {
        if (source.empty() || fs::IsVirtualPath(source)) continue;
        const std::wstring target = path::StripExtendedPathPrefix(fs::NormalizePath(source));
        std::wstring name = fs::StripLnkSuffix(BaseName(target));
        if (name.empty() && target.size() >= 2 && target[1] == L':') name = target.substr(0, 1);
        if (name.empty()) { ++failed; continue; }
        wchar_t base[MAX_PATH]{};
        swprintf_s(base, l10n::Get(l10n::StringId::ShortcutNameFormat).c_str(), name.c_str());
        std::wstring file = dir + L"\\" + base + L".lnk";
        for (int n = 2; GetFileAttributesW(file.c_str()) != INVALID_FILE_ATTRIBUTES && n < 1000; ++n)
            file = dir + L"\\" + base + L" (" + std::to_wstring(n) + L").lnk";
        IShellLinkW* link = nullptr;
        bool ok = SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                             IID_PPV_ARGS(&link)));
        if (ok) {
            ok = SUCCEEDED(link->SetPath(target.c_str()));
            const DWORD attrs = GetFileAttributesW(target.c_str());
            if (ok && attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY))
                link->SetWorkingDirectory(fs::ParentPath(target).c_str());
            IPersistFile* persist = nullptr;
            ok = ok && SUCCEEDED(link->QueryInterface(IID_PPV_ARGS(&persist)));
            ok = ok && SUCCEEDED(persist->Save(file.c_str(), TRUE));
            if (persist) persist->Release();
            link->Release();
        }
        ok ? ++created : ++failed;
    }
    if (failed && s.hwnd)
        s.notification_toast.ShowError(s.hwnd, l10n::Get(l10n::StringId::CannotCreateShortcut),
                                       l10n::Get(l10n::StringId::OperationFailedMessage));
    return created > 0;
}

// The effect of a drop into a folder: Alt or Ctrl+Shift links (shortcuts in
// the folder) when the source allows it, otherwise Ctrl copies, Shift moves.
static DWORD FolderDropEffect(DWORD key_state, const std::vector<std::wstring>& sources,
                              const std::wstring& dest, DWORD allowed, DWORD preferred_effect) {
    const bool link = (key_state & MK_ALT) != 0 ||
        ((key_state & MK_CONTROL) != 0 && (key_state & MK_SHIFT) != 0);
    if (link && (allowed & DROPEFFECT_LINK) && !dest.empty() && !fs::IsVirtualPath(dest) &&
        !ShellItemDrag(sources))
        return DROPEFFECT_LINK;
    return ui::ComputeDropEffect(key_state, sources.front(), dest,
                                 allowed & (DROPEFFECT_COPY | DROPEFFECT_MOVE), preferred_effect);
}

DWORD DropExecute(AppState& s, const std::vector<std::wstring>& sources,
                         POINT pt, DWORD key_state, DWORD allowed,
                         DWORD preferred_effect) {
    // Resolve once more for the final position.
    // Tray drag-out is copy-only: the staged originals stay where they are.
    if (s.trayDragOut) allowed &= DROPEFFECT_COPY;
    DWORD effect = ResolveDropTarget(s, sources, pt, key_state, allowed, preferred_effect);
    std::wstring dest = s.dropDestDir;
    const std::wstring launch_program = s.dropLaunchProgram;
    const std::wstring launch_shortcut = s.dropLaunchShortcut;
    bool tray = s.dropTray;
    const bool tray_move = s.dropTrayMove;
    const bool header = s.dropHeader;
    const int header_pane = s.dropPaneIndex;
    const bool pin_quick_access = s.dropQuickAccess;
    const bool add_favorite = s.dropFavoriteAdd;
    const std::wstring add_group = FavoritesBarDropGroup();
    const int toolbar_app = s.dropToolbarApp;
    const bool toolbar_pin = s.dropToolbarPin;
    const std::wstring toolbar_program = s.toolbarDropProgram;
    ClearDropFeedback(s);
    s.springEntered = false;
    if (effect == DROPEFFECT_NONE || !(effect & allowed)) return DROPEFFECT_NONE;

    if (toolbar_pin) {
        // A program dropped on the label joins the pinned apps; a shortcut
        // only when it starts a program.
        const app::ToolbarAppInfo info = app::DescribeToolbarAppNow(toolbar_program);
        if (info.exe.empty() || !ui::IsDropLaunchProgram(info.exe)) return DROPEFFECT_NONE;
        PinToolbarApp(s, toolbar_program);
        return effect;   // nothing is copied; the source keeps its file
    }
    if (toolbar_app >= 0) {
        // Shift hands over every item, also types the app does not list.
        DropOnToolbarApp(s, static_cast<size_t>(toolbar_app), sources, (key_state & MK_SHIFT) != 0);
        return effect;
    }

    if (!launch_program.empty() && !launch_shortcut.empty()) {
        // Dropped on a program's shortcut: started as the shortcut would be,
        // its own arguments before the items (read on the ops open thread).
        s.ops.OpenShortcutProgram(launch_shortcut, launch_program, ui::DropLaunchArguments(sources));
        return effect;
    }
    if (!launch_program.empty()) {
        // Launched on the ops open thread, in the program's own folder.
        std::wstring folder = fs::ParentPath(fs::NormalizePath(launch_program));
        s.ops.OpenProgramIn(launch_program, folder, ui::DropLaunchArguments(sources));
        return effect;
    }

    if (!add_group.empty()) {
        // Dropped on a 收藏栏 group: its folders are starred into it (a starred
        // one moves there).
        bool changed = false;
        for (const auto& source : sources) {
            const DWORD attrs = GetFileAttributesW(source.c_str());
            if (attrs == INVALID_FILE_ATTRIBUTES || !(attrs & FILE_ATTRIBUTE_DIRECTORY)) continue;
            if (!s.places.IsStarred(source)) ToggleStarred(s, source, app::PlaceItemKind::Folder);
            changed = s.places.SetStarredItemGroup(source, add_group) || changed;
        }
        if (changed) RefreshStarredViews(s);
        InvalidateRect(s.hwnd, nullptr, FALSE);
        return changed ? effect : DROPEFFECT_NONE;  // nothing is copied
    }
    if (add_favorite) {
        // Dropped on the 收藏栏's empty part: starred (already starred stays).
        bool added = false;
        for (const auto& source : sources) {
            const DWORD attrs = GetFileAttributesW(source.c_str());
            if (attrs == INVALID_FILE_ATTRIBUTES || s.places.IsStarred(source)) continue;
            ToggleStarred(s, source, (attrs & FILE_ATTRIBUTE_DIRECTORY) ? app::PlaceItemKind::Folder
                                                                        : app::PlaceItemKind::File);
            added = true;
        }
        InvalidateRect(s.hwnd, nullptr, FALSE);
        return added ? effect : DROPEFFECT_NONE;  // nothing is copied; the source keeps its files
    }

    if (pin_quick_access) {
        std::vector<std::wstring> folders;
        for (const auto& source : sources) {
            const DWORD attrs = GetFileAttributesW(source.c_str());
            if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY)) {
                folders.push_back(fs::NormalizePath(source));
            } else if (ui::LooksLikeFolderShortcut(source)) {
                std::wstring target = ResolveHeaderDropFolder({source});
                if (!target.empty()) folders.push_back(std::move(target));
            }
        }
        if (folders.empty()) return DROPEFFECT_NONE;
        s.places.SetQuickAccessPinned(folders, true);
        InvalidateRect(s.hwnd, nullptr, FALSE);
        return effect;  // nothing is copied; the source keeps its files
    }

    if (header) {
        const std::wstring folder = ResolveHeaderDropFolder(sources);
        if (folder.empty()) {
            // Shortcut resolved to a file: fall through to copy/move into the pane.
        } else {
            if (app::Pane* pane = PaneAtSlot(s, header_pane)) {
                if (pane != s.pane) FocusPane(s, pane);
                app::Tab* tab = pane->ActiveTab();
                if (!tab || _wcsicmp(tab->current_path.c_str(), folder.c_str()) != 0)
                    NavigateTo(s, folder);
                else
                    InvalidateRect(s.hwnd, nullptr, FALSE);
            }
            return effect;
        }
        if (app::Pane* pane = PaneAtSlot(s, header_pane)) {
            if (app::Tab* tab = pane->ActiveTab()) dest = tab->current_path;
        }
        if (dest.empty() || fs::IsVirtualPath(dest)) return DROPEFFECT_NONE;
        effect = FolderDropEffect(key_state, sources, dest, allowed, preferred_effect);
    }

    if (tray) {
        // Staging leaves the clipboard alone; the half sets the release mode.
        StagePaths(s, sources, tray_move);
        return DROPEFFECT_COPY;
    }
    if (dest.starts_with(L"pulse:tags:")) {
        std::vector<std::wstring> tag_ids;
        if (!TagViewDropTags(s, dest, &tag_ids)) return DROPEFFECT_NONE;
        ApplyDropTags(s, tag_ids, sources);
        return DROPEFFECT_COPY;
    }
    if (dest.starts_with(L"pulse:tag:")) {
        std::wstring kind, rest;
        app::ParsePulsePath(dest, &kind, &rest);
        const int ti = s.places.FindTagIndex(s.places.ResolveTagRef(rest));
        if (ti >= 0) ApplyDropTags(s, {s.places.tags[static_cast<size_t>(ti)].id}, sources);
        return DROPEFFECT_COPY;
    }
    if (effect == DROPEFFECT_LINK && !dest.empty() && !fs::IsVirtualPath(dest) && (allowed & DROPEFFECT_LINK)) {
        // Alt / Ctrl+Shift: shortcuts in dest; the source keeps its items.
        if (!CreateDropShortcuts(s, sources, dest)) return DROPEFFECT_NONE;
        InvalidateRect(s.hwnd, nullptr, FALSE);
        return DROPEFFECT_LINK;
    }
    const bool device_dest = DeviceDropFolder(dest);
    if ((device_dest || ShellItemDrag(sources)) && (effect == DROPEFFECT_COPY || effect == DROPEFFECT_MOVE) &&
        (effect & allowed)) {
        // Into a phone, or a phone's items into a folder: a Pulse task run by
        // Explorer's engine (WPD), with Pulse's progress and conflict dialog.
        // The source never deletes: a move is reported as optimized (the
        // task moves the items itself).
        if (!device_dest && (dest.empty() || fs::IsVirtualPath(dest))) return DROPEFFECT_NONE;
        if (ui::DropIntoOwnFolder(sources, dest)) return DROPEFFECT_NONE;
        uint64_t task = 0;
        if (!SubmitDeviceTransfer(s, sources, dest, effect == DROPEFFECT_MOVE, &task)) {
            // Foreign shell items (not a phone's): the folder's own drop target.
            if (!device_dest) return DROPEFFECT_NONE;
            std::vector<std::wstring> items;
            items.reserve(sources.size() + 1);
            items.push_back(app::DeviceRegistry::Instance().FolderShellId(dest));
            for (const auto& source : sources) items.push_back(ClipboardPath(source));
            if (!s.ops.RunShellVerb(std::move(items), effect == DROPEFFECT_MOVE ? L"drop:move" : L"drop:copy",
                                    true, s.hwnd))
                return DROPEFFECT_NONE;
            return effect;
        }
        if (!device_dest) ArmArrivalSelection(s, nullptr, task, 0);
        return effect;
    }
    if (dest.empty() || fs::IsVirtualPath(dest) || !(effect & allowed) ||
        (effect != DROPEFFECT_COPY && effect != DROPEFFECT_MOVE)) return DROPEFFECT_NONE;
    if (ui::DropIntoOwnFolder(sources, dest)) return DROPEFFECT_NONE;

    // Archive managers (7-Zip, WinRAR, Bandizip) drag out of a temporary
    // folder that they delete as soon as the drop returns, while conflicts are
    // resolved later (#55). Such items are copied unless the user or the
    // source asks for a move, and staged before this returns.
    const std::wstring temp_dir =
        g_internal_drag || s.trayDragOut ? std::wstring() : app::TempDirectory();
    const std::wstring stage_root = app::DropStageRoot();
    const bool from_temp = !temp_dir.empty() && !sources.empty() &&
        std::all_of(sources.begin(), sources.end(), [&](const std::wstring& p) {
            return app::IsTemporaryDropSource(p, temp_dir, stage_root);
        });
    if (from_temp && effect == DROPEFFECT_MOVE && (allowed & DROPEFFECT_COPY) &&
        (key_state & MK_SHIFT) == 0 && preferred_effect != DROPEFFECT_MOVE)
        effect = DROPEFFECT_COPY;
    bool submitted = false;
    uint64_t drop_task = 0;
    auto submit = [&](const std::vector<std::wstring>& prepared) {
        ops::OpRequest req;
        req.type = (effect == DROPEFFECT_MOVE) ? ops::OpType::Move : ops::OpType::Copy;
        req.dest_dir = fs::NormalizePath(dest);
        for (const auto& p : prepared) req.sources.push_back(fs::NormalizePath(p));
        submitted = SubmitWithConflictResolution(s, std::move(req), &drop_task);
    };
    if (!temp_dir.empty() && effect == DROPEFFECT_COPY) {
        const auto result = app::SubmitStagedDrop(sources, temp_dir, stage_root, submit);
        if (result.error) {
            wchar_t error[128]{};
            swprintf_s(error, l10n::Get(l10n::StringId::ErrorCodeFormat).c_str(), result.error);
            s.notification_toast.Show(s.hwnd, l10n::Get(l10n::StringId::OpCopyFailed),
                result.source + L"\n" + error, false);
            s.notification_toast.SetKind(ui::fluent::InfoBarKind::Error);
            return DROPEFFECT_NONE;
        }
    } else submit(sources);
    if (!submitted) return DROPEFFECT_NONE;
    if (s.trayDragOut) RememberTrayDest(s, dest);
    // Dropped into the folder a pane shows: the new rows get selected there.
    {
        const app::Pane* pane = PaneAtSlot(s, header_pane);
        if (!pane) pane = s.pane;
        const app::Tab* tab = pane ? pane->ActiveTab() : nullptr;
        if (tab && _wcsicmp(fs::NormalizePath(tab->current_path).c_str(), fs::NormalizePath(dest).c_str()) == 0)
            ArmArrivalSelection(s, pane, drop_task, 0);
    }
    // Dropped into the list that shows `dest` (not onto a folder row): the
    // drag's cards wait and land on the new rows once they appear.
    if (g_internal_drag && !header && ui::motion::SystemAnimationsEnabled()) {
        const app::Pane* pane = PaneAtSlot(s, header_pane);
        const app::Tab* tab = pane ? pane->ActiveTab() : ActiveTab(s);
        if (tab && _wcsicmp(fs::NormalizePath(tab->current_path).c_str(), fs::NormalizePath(dest).c_str()) == 0) {
            s.dragLanding = {};
            s.dragLanding.dir = tab->current_path;
            s.dragLanding.pane = pane ? pane : s.pane;
            for (const auto& p : sources) {
                std::wstring leaf = p;
                while (leaf.size() > 3 && (leaf.back() == L'\\' || leaf.back() == L'/')) leaf.pop_back();
                const size_t slash = leaf.find_last_of(L"\\/");
                s.dragLanding.names.push_back(slash == std::wstring::npos ? leaf : leaf.substr(slash + 1));
            }
            s.dragLanding.since = ui::motion::NowMs();
            ui::ExpectDragLanding();
        }
    }
    return effect;
}

// Starts the modal OLE drag-out for the selected entries.
void StartDragOut(AppState& s) {
    if(DeferContentSelection(s,[](AppState& v){if(GetKeyState(VK_LBUTTON)&0x8000) StartDragOut(v);})) return;
    app::Tab* tab = ActiveTab(s);
    if (!tab || !tab->snapshot || IsRecycleTab(tab)) return;
    const app::SelectionCaps caps = app::ComputeSelectionCaps(*tab, &s.places);
    if (!caps.copy && !caps.cut) return;
    const bool device = IsDeviceTab(tab);
    // A phone's rows drag their shell items (Explorer's MTP data object), so
    // the desktop, Explorer and Pulse's own panes all copy them over WPD.
    std::vector<std::wstring> paths = device ? SelectedShellIds(*tab) : SelectedFullPaths(*tab);
    if (paths.empty()) return;
    if (!device)
        for (auto& path : paths) path = ClipboardPath(path);

    s.clickCollapseIndex = -1;
    CancelScrollAnimation(s); // DoDragDrop's modal loop coexists with on-demand render
    g_internal_drag = true;
    if (device) g_drag_device_dir = tab->current_path;
    // A phone's read-only storage (or its root) gives a copy only.
    const DWORD allowed = device && !caps.cut ? DROPEFFECT_COPY : DROPEFFECT_COPY | DROPEFFECT_MOVE;
    DWORD effect = ui::DoFileDragDrop(paths, allowed,
        [&s] { return s.springEntered; }, tab->current_path); // Esc = 退回 when spring-entered
    g_internal_drag = false;
    g_drag_device_dir.clear();
    s.springEntered = false;
    ClearDropFeedback(s);
    if (effect == DROPEFFECT_MOVE) {
        // The target took the file; refresh the listing.
        s.store.MarkDirty(tab->current_path);
        RefreshActiveTab(s, RefreshReason::OperationCompleted);
    }
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

// Drags every staged item that still exists out of the tray (other apps,
// panes, sidebar folders). Always a copy, so the tray keeps its items.
// `only` limits the drag to those item ids (open view rows).
static void StartTrayDragOut(AppState& s, const std::vector<uint64_t>* only = nullptr) {
    std::vector<std::wstring> paths;
    for (const auto& batch : s.tray.batches())
        for (const auto& item : batch.items) {
            if (only && std::find(only->begin(), only->end(), item.id) == only->end()) continue;
            if (GetFileAttributesW(item.path.c_str()) != INVALID_FILE_ATTRIBUTES)
                paths.push_back(ClipboardPath(item.path));
        }
    if (paths.empty()) return;
    CancelScrollAnimation(s);
    s.trayDragOut = true;
    ui::DoFileDragDrop(paths, DROPEFFECT_COPY, [&s] { return s.springEntered; });
    s.trayDragOut = false;
    s.springEntered = false;
    ClearDropFeedback(s);
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

// Open view row click: toggle the check of the staged item shown at path.
static void ToggleTrayOpenCheck(AppState& s, const std::wstring& path) {
    if (path.empty()) return;
    for (const auto& b : s.tray.batches())
        for (const auto& item : b.items) {
            if (_wcsicmp(ClipboardPath(item.path).c_str(), path.c_str()) != 0) continue;
            auto& checked = s.trayView.checked;
            const auto at = std::find(checked.begin(), checked.end(), item.id);
            if (at != checked.end()) checked.erase(at);
            else if (item.exists) checked.push_back(item.id);
            if (checked.size() != 2) s.trayView.compare = false;
            return;
        }
}

// Staging shelf press (pulse_tray_redesign.html). The fan drags the whole
// stack out; the main button releases into the current folder; ▾ and ⋯ open
// their menus; chips send without opening the folder.
static void HandleTrayShelfPress(AppState& s, HWND hwnd, const ui::HitTestResult& hit, int mx, int my) {
    POINT sp{mx, my};
    ClientToScreen(hwnd, &sp);
    const bool move = TrayEffectiveMove(s);
    switch (static_cast<ui::TrayShelfHit>(hit.index)) {
    case ui::TrayShelfHit::Fan:
        // DragDetect returns at once on release, so a plain click is a no-op.
        if (DragDetect(hwnd, sp)) StartTrayDragOut(s);
        break;
    case ui::TrayShelfHit::Release:
        if (const app::Tab* tab = ActiveTab(s); tab && !tab->current_path.empty())
            ReleaseTrayTo(s, tab->current_path, move);
        break;
    case ui::TrayShelfHit::ReleaseMenu:
        ShowTrayReleaseMenu(s, sp);
        break;
    case ui::TrayShelfHit::More:
        ShowTrayBatchMenu(s, sp);
        break;
    case ui::TrayShelfHit::Zip:
        TrayShelfZip(s);
        break;
    case ui::TrayShelfHit::Rename: {
        const std::vector<std::wstring> paths = TrayLivePaths(s);
        if (!paths.empty()) ShowBatchRenamePaths(s, paths);
        break;
    }
    case ui::TrayShelfHit::Chip:
        if (!hit.path.empty()) TraySendTo(s, hit.path, move);
        break;
    case ui::TrayShelfHit::Clip:
        TakeTrayClip(s);
        break;
    case ui::TrayShelfHit::Expand:
        s.trayView = {};
        s.trayView.open = true;
        s.trayView.opened_at = ui::motion::SystemAnimationsEnabled() ? ui::motion::NowMs() : 0;
        break;
    case ui::TrayShelfHit::Collapse:
        s.trayView.open = false;
        s.trayView.compare = false;
        break;
    case ui::TrayShelfHit::Filter:
        if (hit.sub_index >= 0 && hit.sub_index < 4) {
            s.trayView.filter = hit.sub_index;
            s.trayView.scroll = 0.0f;
        }
        break;
    case ui::TrayShelfHit::Row:
        // Dragging a row takes the checked items along when it is one of
        // them, else just that row; a click toggles its check.
        if (DragDetect(hwnd, sp)) {
            std::vector<uint64_t> ids;
            for (const auto& b : s.tray.batches())
                for (const auto& item : b.items)
                    if (!hit.path.empty() && _wcsicmp(ClipboardPath(item.path).c_str(), hit.path.c_str()) == 0)
                        ids.push_back(item.id);
            const auto& checked = s.trayView.checked;
            if (!ids.empty() && std::find(checked.begin(), checked.end(), ids.front()) != checked.end()) ids = checked;
            if (!ids.empty()) StartTrayDragOut(s, &ids);
        } else {
            ToggleTrayOpenCheck(s, hit.path);
        }
        break;
    case ui::TrayShelfHit::GroupOpen:
        if (!hit.path.empty()) NavigateTo(s, hit.path);
        break;
    case ui::TrayShelfHit::SelNone:
        s.trayView.checked.clear();
        s.trayView.compare = false;
        break;
    case ui::TrayShelfHit::Compare:
        s.trayView.compare = !s.trayView.compare;
        s.trayView.compare_at = ui::motion::SystemAnimationsEnabled() ? ui::motion::NowMs() : 0;
        break;
    case ui::TrayShelfHit::CardClose:
        s.trayView.compare = false;
        break;
    case ui::TrayShelfHit::CardAction:
        if (hit.sub_index == 2) TrayOpenTextDiff(s);
        else TrayStartContentCompare(s);
        break;
    case ui::TrayShelfHit::Pick: {
        std::wstring folder;
        if (PickFolder(s, folder, l10n::Get(l10n::StringId::TrayPickFolder).c_str(), L"tray_send") && !folder.empty())
            TraySendTo(s, folder, move);
        break;
    }
    default:
        break;
    }
    InvalidateRect(hwnd, nullptr, FALSE);
}

float MaxScrollForActivePane(AppState& s, ui::PaneViewModel* out) {
    if (!s.pane || !s.pane->ActiveTab()) return 0.0f;
    ui::PaneViewModel pane;
    app::FillPaneViewModel(pane, *s.pane, &s.places);
    float max_scroll = s.renderer.MaxScrollForPane(pane, FocusedPaneRect(s));
    // A rename card below the list's end made it taller for now.
    if (s.renameIndex >= 0) max_scroll += s.renameScrollRoom;
    max_scroll += s.selectScrollRoom;
    if (out) *out = std::move(pane);
    return max_scroll;
}

void ClampScroll(AppState& s) {
    app::Tab* tab = ActiveTab(s);
    if (!tab) return;
    const float maxScroll = MaxScrollForActivePane(s);
    tab->scroll_y = std::clamp(tab->scroll_y, 0.0f, maxScroll);
    s.scrollTargetY = std::clamp(s.scrollTargetY, 0.0f, maxScroll);
    ui::PaneViewModel pane;
    app::FillPaneViewModel(pane, *s.pane, &s.places);
    const float maxScrollX = s.renderer.MaxScrollXForPane(pane, FocusedPaneRect(s));
    tab->scroll_x = std::clamp(tab->scroll_x, 0.0f, maxScrollX);
    s.scrollTargetX = std::clamp(s.scrollTargetX, 0.0f, maxScrollX);
}

void EnsureRowVisible(AppState& s, app::Tab& tab, int index) {
    if (index < 0) return;
    int viewRow = index;
    ui::PaneViewModel pane;
    MaxScrollForActivePane(s, &pane);
    const D2D1_RECT_F list = s.renderer.PaneListRect(pane, FocusedPaneRect(s));
    if (pane.filter_map || !pane.filter_text.empty()) {
        viewRow = pane.ViewIndex(index);
        if (viewRow < 0) return;
    }
    const D2D1_RECT_F item = s.renderer.ItemRectInPane(pane, FocusedPaneRect(s), viewRow);
    // 滚动时固定上级: a row under the pinned folders is out of sight too.
    const float top = s.renderer.StickyTreeRows(pane, list).Bottom();
    if (item.top < top) tab.scroll_y += item.top - top;
    else if (item.bottom > list.bottom) tab.scroll_y += item.bottom - list.bottom;
    if (item.left < list.left) tab.scroll_x += item.left - list.left;
    else if (item.right > list.right) tab.scroll_x += item.right - list.right;
    ClampScroll(s);
}

bool PointInList(const AppState& s, int mx, int my) {
    const D2D1_RECT_F list = ListRect(s);
    return mx >= list.left && mx < list.right && my >= list.top && my < list.bottom;
}

void ResetMarquee(AppState& s) {
    if (s.marqueeAutoScroll && s.hwnd) KillTimer(s.hwnd, kTimerMarqueeScroll);
    s.marqueeAutoScroll = false;
    s.marqueeSelectionDirty = false;
    s.marqueePending = false;
    s.marqueeActive = false;
    s.marqueeAdditive = false;
    s.marqueeToggle = false;
    s.marqueeRowIndex = -1;
    s.marqueeBase.clear();
    s.marqueeBefore.clear();
    s.blankClickPane = nullptr;
    s.blankClickTab = nullptr;
    s.blankClickGeneration = 0;
}

POINT MarqueeStartOnScreen(const AppState& s) {
    POINT start = s.marqueeStart;
    if (const app::Tab* tab = s.pane ? s.pane->ActiveTab() : nullptr) {
        start.x -= static_cast<LONG>(std::lround(tab->scroll_x - s.marqueeOriginScrollX));
        start.y -= static_cast<LONG>(std::lround(tab->scroll_y - s.marqueeOriginScrollY));
    }
    return start;
}

D2D1_RECT_F MarqueeDisplayRect(const AppState& s) {
    const POINT start = MarqueeStartOnScreen(s);
    return D2D1::RectF(static_cast<float>(std::min(start.x, s.marqueeCur.x)),
                       static_cast<float>(std::min(start.y, s.marqueeCur.y)),
                       static_cast<float>(std::max(start.x, s.marqueeCur.x)),
                       static_cast<float>(std::max(start.y, s.marqueeCur.y)));
}

// Bug 4: items are tested in content space, so rows scrolled out of view stay
// selected and the start stays anchored to the item where the drag began.
void ApplyMarqueeSelection(AppState& s) {
    s.marqueeSelectionDirty = false;
    app::Tab* tab = ActiveTab(s);
    if (!tab || !tab->snapshot) return;
    const int n = static_cast<int>(tab->EntryCount());
    const D2D1_RECT_F list = ListRect(s);
    const POINT start = MarqueeStartOnScreen(s);
    // The pointer end stops at the list edge (auto-scroll brings more in);
    // the start end may lie far off screen.
    const float curX = std::clamp(static_cast<float>(s.marqueeCur.x), list.left, list.right);
    const float curY = std::clamp(static_cast<float>(s.marqueeCur.y), list.top, list.bottom);
    const float left = std::min(static_cast<float>(start.x), curX);
    const float top = std::min(static_cast<float>(start.y), curY);
    // A straight vertical (or horizontal) drag still crosses the rows under
    // it, as in File Explorer: give the band at least 1 px of extent.
    const float right = std::max(std::max(static_cast<float>(start.x), curX), left + 1.0f);
    const float bottom = std::max(std::max(static_cast<float>(start.y), curY), top + 1.0f);

    ++tab->selection_revision;
    tab->all_selected = false;
    tab->selected.clear();
    if (s.marqueeAdditive) tab->selected = s.marqueeBase;

    ui::PaneViewModel pane;
    app::FillPaneViewModel(pane, *s.pane, &s.places);
    if (n > 0 && right > left && bottom > top) {
        std::vector<int> hits;
        s.renderer.ItemsInRectInPane(pane, FocusedPaneRect(s), D2D1::RectF(left, top, right, bottom), hits);
        for (const int source : hits) {
            if (IsDetailsTreePlaceholder(*tab, source)) continue;
            // Ctrl (File Explorer): the band inverts what it covers.
            if (s.marqueeToggle && s.marqueeBase.contains(source)) tab->selected.erase(source);
            else tab->selected.insert(source);
        }
    }

    if (tab->selected.empty()) {
        tab->selected_index = -1;
        return;
    }
    int focus = s.renderer.ItemFromPointInPane(pane, FocusedPaneRect(s), curX, curY);
    if (tab->selected.contains(focus)) tab->selected_index = focus;
    else tab->selected_index = *tab->selected.begin();
    if (tab->selection_anchor < 0) tab->selection_anchor = tab->selected_index;
    if (static_cast<int>(tab->selected.size()) == n) {
        tab->all_selected = true;
        tab->selected.clear();
    }
}

namespace {
// Scroll speed in pixels per millisecond for a pointer `beyond` pixels past
// the edge zone: about 125 px/s at the edge, up to 3750 px/s far outside.
// Time-based, so uneven frame or timer intervals do not change the speed.
float MarqueeScrollSpeed(float beyond, float scale) {
    return std::clamp(beyond * 0.022f, 0.125f * scale, 3.75f * scale);
}

// Signed distance past the auto-scroll zone on each axis (0 = inside).
void MarqueeEdgeOverflow(const AppState& s, float& dx, float& dy) {
    const D2D1_RECT_F list = ListRect(s);
    const float zone = 12.0f * s.scale;   // a maximized window has no room below the list
    const float x = static_cast<float>(s.marqueeCur.x), y = static_cast<float>(s.marqueeCur.y);
    dx = dy = 0.0f;
    if (y < list.top + zone) dy = y - (list.top + zone);
    else if (y > list.bottom - zone) dy = y - (list.bottom - zone);
    if (x < list.left + zone) dx = x - (list.left + zone);
    else if (x > list.right - zone) dx = x - (list.right - zone);
}

void StopMarqueeAutoScroll(AppState& s) {
    if (s.marqueeAutoScroll && s.hwnd) KillTimer(s.hwnd, kTimerMarqueeScroll);
    s.marqueeAutoScroll = false;
}

// Requests the next frame: on the display clock when the pump runs.
void RequestMarqueeFrame(AppState& s) {
    if (s.framePump.Running()) s.framePump.Arm();
    else if (s.hwnd) InvalidateRect(s.hwnd, nullptr, FALSE);
}
} // namespace

void UpdateMarqueeAutoScroll(AppState& s) {
    float dx = 0.0f, dy = 0.0f;
    if (s.marqueeActive) MarqueeEdgeOverflow(s, dx, dy);
    const app::Tab* tab = ActiveTab(s);
    const bool horizontal = tab && tab->view_mode == ui::ViewMode::List;
    const bool want = s.marqueeActive && (horizontal ? dx != 0.0f : dy != 0.0f);
    if (want == s.marqueeAutoScroll || !s.hwnd) return;
    if (!want) { StopMarqueeAutoScroll(s); return; }
    s.marqueeAutoScroll = true;
    s.marqueeScrollLast = std::chrono::steady_clock::now();
    // The timer keeps frames coming while the pointer rests (no mouse moves).
    SetTimer(s.hwnd, kTimerMarqueeScroll, 16, nullptr);
    RequestMarqueeFrame(s);
}

void TickMarqueeAutoScroll(AppState& s) {
    if (!s.marqueeActive || !ActiveTab(s) || (GetKeyState(VK_LBUTTON) & 0x8000) == 0) {
        StopMarqueeAutoScroll(s);
        return;
    }
    RequestMarqueeFrame(s);
}

// Called by Render once per frame before the view model is built.
void UpdateMarqueeFrame(AppState& s) {
    if (s.marqueeAutoScroll) {
        app::Tab* tab = ActiveTab(s);
        if (!s.marqueeActive || !tab || (GetKeyState(VK_LBUTTON) & 0x8000) == 0) {
            StopMarqueeAutoScroll(s);
        } else {
            // The pointer may rest outside the window; follow it without mouse moves.
            POINT cursor{};
            if (GetCursorPos(&cursor) && ScreenToClient(s.hwnd, &cursor)) s.marqueeCur = cursor;
            const auto now = std::chrono::steady_clock::now();
            const float elapsed = static_cast<float>(std::clamp(
                std::chrono::duration<double, std::milli>(now - s.marqueeScrollLast).count(), 0.0, 50.0));
            s.marqueeScrollLast = now;
            float dx = 0.0f, dy = 0.0f;
            MarqueeEdgeOverflow(s, dx, dy);
            const bool horizontal = tab->view_mode == ui::ViewMode::List;
            const float over = horizontal ? dx : dy;
            if (over == 0.0f) {
                StopMarqueeAutoScroll(s);
            } else {
                if (s.scrollAnimating) CancelScrollAnimation(s);
                float& position = horizontal ? tab->scroll_x : tab->scroll_y;
                const float before = position;
                const float step = MarqueeScrollSpeed(std::abs(over), s.scale) * elapsed;
                position += over < 0.0f ? -step : step;
                ClampScroll(s);
                s.scrollTargetX = tab->scroll_x;
                s.scrollTargetY = tab->scroll_y;
                if (position != before) {
                    s.marqueeSelectionDirty = true;
                    RefreshScrolledHover(s);
                    MaybePrefetchSearchPage(s);
                }
            }
        }
    }
    if (s.marqueeSelectionDirty && s.marqueeActive) ApplyMarqueeSelection(s);
}

void HandleListRowClick(AppState& s, int index, bool ctrl, bool shift) {
    app::Tab* tab = ActiveTab(s);
    if (!tab) return;
    s.clickCollapseIndex = -1;
    s.clickToggleOnRelease = false;
    if (shift) {
        const int anchor = tab->selection_anchor >= 0 ? tab->selection_anchor
            : (tab->selected_index >= 0 ? tab->selected_index : index);
        ui::PaneViewModel view;
        app::FillPaneViewModel(view, *s.pane, &s.places);
        // Ctrl+Shift adds the range to what is already selected (Explorer).
        if (ctrl) tab->AddRange(anchor, index, &view);
        else tab->SelectRange(anchor, index, &view);
    } else if (ctrl) {
        // A selected item remains part of the drag payload. Only a completed
        // Ctrl-click (without a drag) removes it from the selection.
        if (tab->IsSelected(index)) {
            tab->selected_index = index;
            s.clickCollapseIndex = index;
            s.clickToggleOnRelease = true;
        } else {
            tab->ToggleSelect(index);
        }
    } else if (tab->IsSelected(index) && tab->SelectedCount() > 1) {
        tab->selected_index = index;
        s.clickCollapseIndex = index;
    } else {
        tab->SelectOnly(index);
    }
}

void FinishListRowClick(AppState& s) {
    if (app::Tab* tab = ActiveTab(s); tab && s.clickCollapseIndex >= 0) {
        if (s.clickToggleOnRelease) tab->ToggleSelect(s.clickCollapseIndex);
        else tab->SelectOnly(s.clickCollapseIndex);
    }
    s.clickCollapseIndex = -1;
    s.clickToggleOnRelease = false;
}

// Keyboard navigation to a view row, the way File Explorer treats modifiers:
// plain keys select it, Shift extends from the anchor, Ctrl moves only the
// focus (Ctrl+Space then toggles it), Ctrl+Shift adds the range.
void MoveKeyboardFocus(AppState& s, app::Tab& tab, const ui::PaneViewModel& pane, int view_index,
                       bool ctrl, bool shift) {
    const int index = pane.SourceIndex(view_index);
    if (index < 0) return;
    if (ctrl && !shift) {
        tab.SetCaret(index);
    } else if (ctrl) {
        const int focus = tab.FocusIndex();
        const int anchor = tab.selection_anchor >= 0 ? tab.selection_anchor : (focus >= 0 ? focus : index);
        tab.AddRange(anchor, index, &pane);
        tab.SetCaret(index);
    } else {
        tab.MoveFocus(index, shift, &pane);
    }
    EnsureRowVisible(s, tab, index);
}

// ---- Sidebar section header drag: reorders the sections ---------------------
// The order lives in AppState::sidebarOrder (logical ids); the masks stay keyed
// by id, so a reorder never shuffles what is hidden or folded.

// A press must travel this far before it becomes a reorder; below it the
// gesture stays a click (fold a section, open a row).
constexpr int kSidebarDragDeadzonePx = 4;

void ResetSidebarGroupDrag(AppState& s) {
    s.groupDragPending = false;
    s.groupDragActive = false;
    s.groupDragId = -1;
    s.groupDragToIndex = -1;
    s.groupGapVisible = false;
    s.groupGapLineY = 0.0f;
    s.groupDragPath.clear();
    s.groupDragNavigate = false;
}

// ---- Quick-access pin drag: reorders the pinned folders ---------------------

void ResetSidebarPinDrag(AppState& s) {
    s.pinDragPending = false;
    s.pinDragActive = false;
    s.pinDragPath.clear();
    s.pinDragKey.clear();
    s.pinDragRun = -1;
    s.pinDragToIndex = -1;
    s.pinGapVisible = false;
    s.pinGapLineY = 0.0f;
}

// Insertion slot for the dragged quick-access row, measured against the rows
// that move: built-in links and pins (not the Git project row).
void UpdateSidebarPinDrag(AppState& s, int my) {
    if (!s.pinDragActive || s.pinDragPath.empty()) return;
    if (UpdateVerticalTabDrag(s, my)) return;  // tab rows share this gesture
    if (UpdateSidebarTagDrag(s, my)) return;   // so do tag rows
    ui::WindowViewModel vm = BuildVm(s, false);
    const float w = static_cast<float>(s.compositor.Width());
    const float h = static_cast<float>(s.compositor.Height());
    const int section = static_cast<int>(app::SidebarSectionId::QuickAccess);

    std::vector<D2D1_RECT_F> rows;
    const int group = app::SidebarSectionIndex(vm, section);
    if (group >= 0) {
        const auto keys = QuickAccessGroupRowKeys(s, vm.sidebar[static_cast<size_t>(group)]);
        rows.reserve(keys.size());
        for (int i = 0; i < static_cast<int>(keys.size()); ++i) {
            if (keys[static_cast<size_t>(i)].empty()) continue;
            D2D1_RECT_F rc{};
            if (s.renderer.SidebarRowRect(vm, w, h, section, i, &rc)) rows.push_back(rc);
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
}

// Insertion slot under the cursor plus the indicator line for it. Sections the
// bands do not cover (hidden, or scrolled past) keep their order.
void UpdateSidebarGroupDrag(AppState& s, int my) {
    if (!s.groupDragActive || s.groupDragId < 0) return;
    ui::WindowViewModel vm = BuildVm(s, false);
    std::vector<ui::SidebarGroupBand> bands;
    s.renderer.SidebarGroupBands(vm, static_cast<float>(s.compositor.Width()),
                                 static_cast<float>(s.compositor.Height()), bands);

    std::vector<int> rest;
    rest.reserve(s.sidebarOrder.size());
    for (int id : s.sidebarOrder)
        if (id != s.groupDragId) rest.push_back(id);

    const float cursor = static_cast<float>(my);
    int insert_at = static_cast<int>(rest.size());
    float line_y = 0.0f;
    for (const auto& band : bands) {
        if (band.group < 0 || band.group >= static_cast<int>(vm.sidebar.size())) continue;
        const int id = vm.sidebar[static_cast<size_t>(band.group)].id;
        if (id == s.groupDragId) continue;
        const auto it = std::find(rest.begin(), rest.end(), id);
        if (it == rest.end()) continue;
        if (cursor < (band.top + band.bottom) * 0.5f) {
            insert_at = static_cast<int>(it - rest.begin());
            line_y = band.top;
            break;
        }
    }
    if (line_y <= 0.0f && !bands.empty()) line_y = bands.back().bottom;
    s.groupDragToIndex = insert_at;
    s.groupGapLineY = line_y;
    s.groupGapVisible = line_y > 0.0f;
}

void CommitSidebarGroupDrag(AppState& s) {
    if (!s.groupDragActive || s.groupDragId < 0) return;
    std::vector<int> rest;
    rest.reserve(s.sidebarOrder.size());
    for (int id : s.sidebarOrder)
        if (id != s.groupDragId) rest.push_back(id);
    if (s.groupDragToIndex < 0 || s.groupDragToIndex > static_cast<int>(rest.size()))
        s.groupDragToIndex = static_cast<int>(rest.size());
    rest.insert(rest.begin() + s.groupDragToIndex, s.groupDragId);
    s.sidebarOrder = app::NormalizeSidebarOrder(rest);
}

void CancelRenameClick(AppState& s) {
    s.renameClickCandidate = false;
    s.renameClickPane = nullptr;
    s.renameClickTab = nullptr;
    s.renameClickIndex = -1;
    s.renameClickDue = 0;
    s.renameClickStarted = 0;
    s.renameClickPath.clear();
}

bool TickHoverSelect(AppState& s, ULONGLONG now) {
    if (!s.appPrefs.single_click_open || !s.appPrefs.hover_select) return false;
    if (s.hoverRow < 0 || s.ctxHoverSince == 0 || s.hoverSelectFired == s.ctxHoverSince ||
        now - s.ctxHoverSince < 500) return false;
    if (s.dragPending || s.marqueePending || s.marqueeActive || s.renameIndex >= 0 || GetCapture() ||
        (GetKeyState(VK_LBUTTON) & 0x8000) != 0 || (GetKeyState(VK_RBUTTON) & 0x8000) != 0) return false;
    s.hoverSelectFired = s.ctxHoverSince;
    if (s.hwnd && GetForegroundWindow() != GetAncestor(s.hwnd, GA_ROOT)) return false;
    app::Pane* pane = s.hoverPaneIndex >= 0 ? PaneAtSlot(s, s.hoverPaneIndex) : s.pane;
    if (!pane || pane != s.pane) return false;
    app::Tab* tab = ActiveTab(s);
    const int row = s.hoverRow;
    if (!tab || IsSettingsTab(tab) || row >= tab->CountBound()) return false;
    if ((GetKeyState(VK_CONTROL) & 0x8000) != 0) {
        if (tab->IsSelected(row)) return false;
        tab->ToggleSelect(row);
        return true;
    }
    if (tab->SelectedCount() == 1 && tab->selected_index == row) return false;
    tab->SelectOnly(row);
    return true;
}

// Item check boxes: a row's box toggles it (Shift: the range from the anchor
// joins), the header's selects everything shown or nothing.
static bool HandleItemCheckPress(AppState& s, const ui::HitTestResult& hit) {
    app::Tab* tab = ActiveTab(s);
    if (!tab || IsSettingsTab(tab)) return false;
    CancelRenameClick(s);
    if (hit.region == ui::HitTestResult::HeaderCheck) {
        std::vector<int> matches;
        app::CollectFilterMatches(*tab, &s.places, matches);
        if (!matches.empty() && static_cast<size_t>(tab->SelectedCount()) >= matches.size()) tab->ClearSelection();
        else tab->SelectIndices(matches);
        return true;
    }
    if (hit.index < 0 || hit.index >= tab->CountBound()) return false;
    if ((GetKeyState(VK_SHIFT) & 0x8000) != 0) HandleListRowClick(s, hit.index, true, true);
    else tab->ToggleSelect(hit.index);
    return true;
}

// Single click opens: arms the open a plain press on a row does on release.
static void ArmSingleOpen(AppState& s, const ui::WindowViewModel& vm, const ui::HitTestResult& hit,
                          app::Tab* tab, bool ctrl, bool shift, bool activates) {
    s.singleOpenCandidate = false;
    if (!s.appPrefs.single_click_open || !tab || IsSettingsTab(tab) || ctrl || shift || activates ||
        (GetKeyState(VK_MENU) & 0x8000) != 0 || hit.index < 0 || hit.index >= tab->CountBound()) return;
    // Column view: selecting a folder already lists it in the next column.
    const ui::PaneViewModel& pane = hit.pane_index >= 0 && hit.pane_index < static_cast<int>(vm.pane_slots.size())
        ? vm.pane_slots[static_cast<size_t>(hit.pane_index)].pane : vm.pane;
    if (pane.column_strip.Active() && tab->snapshot && hit.index < static_cast<int>(tab->EntryCount()) &&
        tab->EntryAt(hit.index).is_dir) return;
    s.singleOpenCandidate = true;
    s.singleOpenPane = s.pane;
    s.singleOpenTab = tab;
    s.singleOpenIndex = hit.index;
}

static void OpenSingleClickRow(AppState& s, int index) {
    app::Tab* tab = ActiveTab(s);
    if (!tab || tab != s.singleOpenTab || s.pane != s.singleOpenPane || index >= tab->CountBound()) return;
    tab->SelectOnly(index);
    s.singleOpenTick = GetTickCount64();
    OpenSelected(s);
}

bool PointInHitItemName(AppState& s, const ui::WindowViewModel& vm,
                               const ui::HitTestResult& hit, float x, float y) {
    if (hit.region != ui::HitTestResult::Row || hit.index < 0) return false;
    if (hit.pane_index >= 0 &&
        hit.pane_index < static_cast<int>(vm.pane_slots.size())) {
        const auto& slot = vm.pane_slots[static_cast<size_t>(hit.pane_index)];
        return s.renderer.PointInItemName(slot.pane, slot.rect, hit.index, x, y);
    }
    return s.renderer.PointInItemName(vm.pane, FocusedPaneRect(s), hit.index, x, y);
}

static bool PointInHitItemGrip(AppState& s, const ui::WindowViewModel& vm,
                               const ui::HitTestResult& hit, float x, float y) {
    if (hit.region != ui::HitTestResult::Row || hit.index < 0) return true;
    // Icon grids: a selected item drags from its whole picture.
    const app::Tab* tab = ActiveTab(s);
    const bool selected = tab && tab->IsSelected(hit.index);
    if (hit.pane_index >= 0 &&
        hit.pane_index < static_cast<int>(vm.pane_slots.size())) {
        const auto& slot = vm.pane_slots[static_cast<size_t>(hit.pane_index)];
        return s.renderer.PointInItemGrip(slot.pane, slot.rect, hit.index, x, y, selected);
    }
    return s.renderer.PointInItemGrip(vm.pane, FocusedPaneRect(s), hit.index, x, y, selected);
}

// How far a press in the file list moves before it becomes a marquee or a
// drag. SM_CXDRAG stays 4 px at every DPI, a hand's tremor at 150 %; scale
// it, and give dragging the files away 2 DIP more than the marquee.
static int ListDragSlop(const AppState& s, int metric, bool drag) {
    const float px = static_cast<float>(GetSystemMetrics(metric)) * s.scale + (drag ? 2.0f * s.scale : 0.0f);
    return (std::max)(1, static_cast<int>(std::lround(px)));
}

// Esc during a marquee puts this selection back.
static void RememberSelectionForMarquee(AppState& s, app::Tab* tab) {
    s.marqueeBefore.clear();
    s.marqueeBeforeAll = tab && tab->all_selected;
    s.marqueeBeforeIndex = tab ? tab->selected_index : -1;
    if (tab && !tab->all_selected) s.marqueeBefore = tab->selected;
}

static bool CancelMarquee(AppState& s) {
    if (!s.marqueeActive && !s.marqueePending) return false;
    if (app::Tab* tab = ActiveTab(s); tab && s.marqueeActive) {
        ++tab->selection_revision;
        tab->all_selected = s.marqueeBeforeAll;
        tab->selected = s.marqueeBefore;
        tab->selected_index = s.marqueeBeforeIndex;
    }
    ResetMarquee(s);
    if (s.hwnd && GetCapture() == s.hwnd) ReleaseCapture();
    return true;
}

void CancelScrollAnimation(AppState& s) {
    s.scrollAnimating = false;
}

float TagEaseInOutQuad(float t) { // used by the window-tab slide animation
    const float u = -2.0f * t + 2.0f;
    return t < 0.5f ? 2.0f * t * t : 1.0f - u * u * 0.5f;
}

void TickTabTransitions(AppState& s) {
    std::unordered_set<const app::LayoutTab*> live;
    if (s.pane) {
        for (const auto& t : s.window_tabs.items) live.insert(t.get());
    }
    const auto now = std::chrono::steady_clock::now();
    for (auto it = s.tabTracks.begin(); it != s.tabTracks.end();) {
        if (!live.count(it->first)) {
            s.tabOffsets.erase(it->first);
            it = s.tabTracks.erase(it);
            continue;
        }
        const float t = std::chrono::duration<float, std::milli>(now - it->second.t0).count()
            / static_cast<float>(it->second.durationMs);
        if (t >= 1.0f) {
            s.tabOffsets.erase(it->first);
            it = s.tabTracks.erase(it);
        } else {
            s.tabOffsets[it->first] = it->second.start * (1.0f - TagEaseInOutQuad(t));
            ++it;
        }
    }
    for (auto it = s.tabOffsets.begin(); it != s.tabOffsets.end();) {
        if (!live.count(it->first)) it = s.tabOffsets.erase(it);
        else ++it;
    }
    // Chip slide channel: same easing, px units, keyed by group id.
    std::unordered_set<int> liveGroups;
    if (s.pane) {
        for (const auto& g : s.window_tabs.tab_groups) liveGroups.insert(g.id);
    }
    for (auto it = s.chipTracks.begin(); it != s.chipTracks.end();) {
        if (!liveGroups.count(it->first)) {
            s.chipOffsets.erase(it->first);
            it = s.chipTracks.erase(it);
            continue;
        }
        const float t = std::chrono::duration<float, std::milli>(now - it->second.t0).count()
            / static_cast<float>(it->second.durationMs);
        if (t >= 1.0f) {
            s.chipOffsets.erase(it->first);
            it = s.chipTracks.erase(it);
        } else {
            s.chipOffsets[it->first] = it->second.start * (1.0f - TagEaseInOutQuad(t));
            ++it;
        }
    }
    for (auto it = s.chipOffsets.begin(); it != s.chipOffsets.end();) {
        if (!liveGroups.count(it->first)) it = s.chipOffsets.erase(it);
        else ++it;
    }
}

void StartSmoothScroll(AppState& s, float delta, bool horizontal) {
    app::Tab* tab = ActiveTab(s);
    if (!tab) return;
    ClampScroll(s);

    if (s.scrollAnimating && s.scrollHorizontal != horizontal) CancelScrollAnimation(s);
    s.scrollHorizontal = horizontal;

    // Preserve the distance from earlier wheel pulses. Restarting from the
    // partially animated position discards most of a fast wheel gesture.
    if (s.scrollAnimating) {
        UpdateSmoothScroll(s);
    } else {
        s.scrollTargetY = tab->scroll_y;
        s.scrollTargetX = tab->scroll_x;
        s.scrollLastUpdateTime = std::chrono::steady_clock::now();
    }

    if (horizontal) {
        ui::PaneViewModel pane;
        app::FillPaneViewModel(pane, *s.pane, &s.places);
        const float maximum = s.renderer.MaxScrollXForPane(pane, FocusedPaneRect(s));
        s.scrollTargetX = std::clamp(s.scrollTargetX + delta, 0.0f, maximum);
    } else {
        const float maximum = MaxScrollForActivePane(s);
        s.scrollTargetY = std::clamp(s.scrollTargetY + delta, 0.0f, maximum);
    }
    s.scrollAnimating = true;
    if (s.framePump.Running()) s.framePump.Arm();
    else if (s.hwnd) InvalidateRect(s.hwnd, nullptr, FALSE);
    MaybePrefetchSearchPage(s);
}

void UpdateSmoothScroll(AppState& s, const HoverPointerApi& api) {
    if (!s.scrollAnimating) return;
    app::Tab* tab = ActiveTab(s);
    if (!tab) { s.scrollAnimating = false; return; }
    auto now = std::chrono::steady_clock::now();
    const double elapsed = std::clamp(
        std::chrono::duration<double, std::milli>(now - s.scrollLastUpdateTime).count(),
        0.0, 50.0);
    s.scrollLastUpdateTime = now;

    float& position = s.scrollHorizontal ? tab->scroll_x : tab->scroll_y;
    const float previous = position;
    const float target = s.scrollHorizontal ? s.scrollTargetX : s.scrollTargetY;
    const float remaining = target - position;
    if (std::abs(remaining) <= 0.35f) {
        position = target;
        s.scrollAnimating = false;
    } else {
        // Exponential response is independent of timer jitter and accepts a
        // moving target without resetting its easing curve on every pulse.
        const float response = 1.0f - static_cast<float>(
            std::exp(-elapsed / AppState::kScrollResponseMs));
        position += remaining * response;
    }
    ClampScroll(s);
    if (position != previous) {
        // Wheel during a marquee: the band grows with the scrolled content.
        if (s.marqueeActive) s.marqueeSelectionDirty = true;
        RefreshScrolledHover(s, api);
    }
}

namespace {
// Blank chrome moves the window (空白处拖动窗口): a press on the toolbar gaps,
// the sidebar gaps, a lone pane's title strip or the status bar keeps its old
// meaning, but once the pointer travels past the drag threshold the window
// moves as from the title bar (snap included). The list's blank space is not
// part of it: there a drag draws the selection band.
enum class BlankChrome { None, Drag, DragMaximize };

struct BlankWindowDrag {
    HWND hwnd = nullptr;
    POINT start{};
};
BlankWindowDrag g_blank_drag;

BlankChrome BlankChromeAt(AppState& s, const ui::HitTestResult& hit, int mx, int my) {
    if (!s.hwnd) return BlankChrome::None;
    switch (hit.region) {
    case ui::HitTestResult::SidebarBlank:
        return BlankChrome::Drag;
    case ui::HitTestResult::StatusBar:
        return BlankChrome::DragMaximize;
    case ui::HitTestResult::PaneHeader: {
        // index 0 is the empty strip right of the title; in a split, a drag
        // there takes the pane out as a window instead (ArmPaneDrag).
        const app::LayoutTab* const tab = s.window_tabs.Active();
        if (hit.index != 0 || hit.pane_index < 0 || !tab || tab->VisiblePanes().size() >= 2)
            return BlankChrome::None;
        return BlankChrome::Drag;
    }
    case ui::HitTestResult::None: {
        // The title bar is a caption already (WM_NCHITTEST); the content
        // area's blank space belongs to the list.
        const float x = static_cast<float>(mx);
        const float y = static_cast<float>(my);
        if (y < s.renderer.TitleBarHeight()) return BlankChrome::None;
        const D2D1_RECT_F content = s.renderer.ContentRect(
            static_cast<float>(s.compositor.Width()), static_cast<float>(s.compositor.Height()));
        if (x >= content.left && x < content.right && y >= content.top && y < content.bottom)
            return BlankChrome::None;
        return BlankChrome::DragMaximize;
    }
    default:
        return BlankChrome::None;
    }
}

void ArmBlankWindowDrag(AppState& s, const ui::HitTestResult& hit, int mx, int my) {
    g_blank_drag = BlankWindowDrag{};
    if (BlankChromeAt(s, hit, mx, my) == BlankChrome::None) return;
    g_blank_drag.hwnd = s.hwnd;
    g_blank_drag.start = POINT{mx, my};
}

// True once the window move has started (the system move loop has run).
bool TryBlankWindowDrag(HWND hwnd, WPARAM wParam, int mx, int my) {
    if (!g_blank_drag.hwnd || g_blank_drag.hwnd != hwnd) return false;
    if (!(wParam & MK_LBUTTON)) {
        g_blank_drag = BlankWindowDrag{};
        return false;
    }
    if (std::abs(mx - g_blank_drag.start.x) < GetSystemMetrics(SM_CXDRAG) &&
        std::abs(my - g_blank_drag.start.y) < GetSystemMetrics(SM_CYDRAG))
        return false;
    g_blank_drag = BlankWindowDrag{};
    POINT screen{mx, my};
    ClientToScreen(hwnd, &screen);
    if (GetCapture() == hwnd) ReleaseCapture();
    SendMessageW(hwnd, WM_NCLBUTTONDOWN, HTCAPTION, MAKELPARAM(screen.x, screen.y));
    return true;
}

// The size tooltip names the old size: drop it while the size changes; it
// returns with the new one once the pointer rests.
void ResetSizeTooltip(AppState& s) {
    s.tooltipText.clear();
    s.hoverSince = GetTickCount64();
}

// Pane header size slider (P1e): the size under x for the pane being dragged.
void DragPaneSize(AppState& s, int mx) {
    const ui::WindowViewModel vm = BuildVm(s, false);
    const int slot = s.paneSizeDragPane;
    if (slot < 0 || slot >= static_cast<int>(vm.pane_slots.size())) return;
    const auto& pane = vm.pane_slots[static_cast<size_t>(slot)];
    const float dip = s.renderer.PaneSizeSliderValueAt(pane.pane, pane.rect, static_cast<float>(mx));
    if (dip > 0.0f) SetIconSize(s, dip);
    ResetSizeTooltip(s);
}

void EndPaneSizeDrag(AppState& s) {
    if (!s.paneSizeDragging) return;
    s.paneSizeDragging = false;
    s.paneSizeDragPane = -1;
    if (s.paneSizeSavePending) {
        s.paneSizeSavePending = false;
        s.appPrefs.Save();
    }
}

// The slider follows the press; a narrow pane's size button opens or closes it.
void PressPaneSize(AppState& s, HWND hwnd, const ui::HitTestResult& hit, int mx) {
    if (hit.region == ui::HitTestResult::PaneSizeButton) {
        if (app::Pane* pane = PaneAtSlot(s, hit.pane_index)) pane->size_slider_open = !pane->size_slider_open;
    } else {
        s.dragPending = false;
        s.paneSizeDragging = true;
        s.paneSizeDragPane = hit.pane_index;
        SetCapture(hwnd);
        DragPaneSize(s, mx);
    }
    InvalidateRect(hwnd, nullptr, FALSE);
}
} // namespace

namespace {
// 点空白取消选择. A press on blank chrome arms it; the release clears the
// selection only when the pointer has not moved (a drag there moves the
// window or takes a pane out instead).
struct BlankDeselect {
    bool armed = false;
    bool maximize = false;   // a double click there maximizes
    POINT start{};
};
BlankDeselect g_blank_deselect;

// What the last blank click cleared, for the double click that maximizes.
struct BlankDeselectUndo {
    app::Tab* tab = nullptr;   // compared with the active tab, never read on its own
    app::Pane* pane = nullptr;
    uint64_t generation = 0;
    DWORD tick = 0;
    POINT point{};
    bool maximize = false;
    std::unordered_set<int> selected;
    bool all = false;
    int index = -1;
};
BlankDeselectUndo g_blank_undo;

bool BlankDeselectRegion(AppState& s, const ui::HitTestResult& hit, int mx, int my) {
    if (BlankChromeAt(s, hit, mx, my) != BlankChrome::None) return true;
    // The pane title strip in a split too (a drag there takes the pane out).
    return hit.region == ui::HitTestResult::PaneHeader && hit.index == 0 && hit.pane_index >= 0;
}

void ArmBlankDeselect(AppState& s, const ui::HitTestResult& hit, int mx, int my, bool activates) {
    g_blank_deselect = BlankDeselect{};
    if (activates || !BlankDeselectRegion(s, hit, mx, my)) return;
    g_blank_deselect.armed = true;
    g_blank_deselect.maximize = BlankChromeAt(s, hit, mx, my) == BlankChrome::DragMaximize;
    g_blank_deselect.start = POINT{mx, my};
}

bool BlankDeselectMoved(POINT a, int mx, int my) {
    return std::abs(mx - a.x) >= GetSystemMetrics(SM_CXDRAG) || std::abs(my - a.y) >= GetSystemMetrics(SM_CYDRAG);
}
} // namespace

bool BlankClickDeselect(AppState& s, POINT client, bool maximize) {
    app::Tab* tab = ActiveTab(s);
    if (!tab || IsSettingsTab(tab) || (!tab->all_selected && tab->selected.empty())) return false;
    g_blank_undo = BlankDeselectUndo{};
    g_blank_undo.tab = tab;
    g_blank_undo.pane = s.pane;
    g_blank_undo.generation = tab->view_generation;
    g_blank_undo.tick = GetTickCount();
    g_blank_undo.point = client;
    g_blank_undo.maximize = maximize;
    g_blank_undo.selected = tab->selected;
    g_blank_undo.all = tab->all_selected;
    g_blank_undo.index = tab->selected_index;
    tab->ClearSelection();
    if (s.hwnd) InvalidateRect(s.hwnd, nullptr, FALSE);
    return true;
}

bool RestoreBlankDeselect(AppState& s, POINT client, bool maximizing) {
    if (maximizing && !g_blank_undo.maximize) return false;
    BlankDeselectUndo undo = std::move(g_blank_undo);
    g_blank_undo = BlankDeselectUndo{};
    app::Tab* tab = ActiveTab(s);
    if (!undo.tab || tab != undo.tab || s.pane != undo.pane || tab->view_generation != undo.generation) return false;
    if (GetTickCount() - undo.tick > GetDoubleClickTime() ||
        std::abs(client.x - undo.point.x) > GetSystemMetrics(SM_CXDOUBLECLK) / 2 ||
        std::abs(client.y - undo.point.y) > GetSystemMetrics(SM_CYDOUBLECLK) / 2)
        return false;
    ++tab->selection_revision;
    tab->all_selected = undo.all;
    tab->selected = std::move(undo.selected);
    tab->selected_index = undo.index;
    if (s.hwnd) InvalidateRect(s.hwnd, nullptr, FALSE);
    return true;
}

LRESULT HandleMouseMove(AppState* s, HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        if (!s) return DefWindowProcW(hwnd, msg, wParam, lParam);
        int mx = GET_X_LPARAM(lParam);
        int my = GET_Y_LPARAM(lParam);
        if (AiAssistantMouseMove(*s, mx, my)) {   // Pulse 助手 owns its panel
            if (s->hoverRegion != 0 || s->hoverRow >= 0 || !s->tooltipText.empty())
                HandleMouseLeave(s, hwnd, WM_MOUSELEAVE, 0, 0);
            return 0;
        }
        if (g_blank_deselect.armed && BlankDeselectMoved(g_blank_deselect.start, mx, my))
            g_blank_deselect = BlankDeselect{};   // a drag, not a click
        if (TryBlankWindowDrag(hwnd, wParam, mx, my)) return 0;  // blank chrome moves the window
        if (ToolbarPinDragging()) {   // picker T3c: no hover plate or tooltip under the dragged pin
            MoveToolbarPinDrag(*s, POINT{mx, my});
            if (s->hoverRegion != 0 || !s->tooltipText.empty()) HandleMouseLeave(s, hwnd, WM_MOUSELEAVE, 0, 0);
            return 0;
        }
        if (FavoritesBarDragging()) {   // 收藏栏: the same, under a dragged favorite
            if (MoveFavoritesBarDrag(*s, POINT{mx, my}) &&
                (s->hoverRegion != 0 || !s->tooltipText.empty()))
                HandleMouseLeave(s, hwnd, WM_MOUSELEAVE, 0, 0);
            return 0;
        }
        s->hoverPoint = POINT{ mx, my };
        s->bloom_accent.SetPointer(static_cast<float>(mx), static_cast<float>(my), true);
        if (static_cast<float>(my) >= s->renderer.TitleBarHeight() && s->renderer.ReleaseTabWidths())
            InvalidateRect(hwnd, nullptr, FALSE);
        if (s->paneSizeDragging) {
            if (!(wParam & MK_LBUTTON)) {
                EndPaneSizeDrag(*s);  // capture lost without WM_LBUTTONUP
            } else {
                DragPaneSize(*s, mx);
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
        }
        if (s->settings.slider_drag() >= 0) {
            if (!(wParam & MK_LBUTTON)) {
                s->settings.EndSlider(); // capture lost without WM_LBUTTONUP
            } else {
                const int which = s->settings.slider_drag();
                ui::WindowViewModel vm = BuildVm(*s, false);
                const D2D1_RECT_F rect = D2D1::RectF(0, 0, (float)s->compositor.Width(),
                                                     (float)s->compositor.Height());
                if (s->settings.SliderValue(which,
                        s->renderer.SettingsSliderValueAt(vm, rect, which, (float)mx)))
                    InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
        }

        // 系统右键菜单 preview item held: follow the pointer.
        if (SysCtxPreviewDragMove(*s, mx, my, wParam)) return 0;

        // Pulse menu preview row held on the 右键菜单 page: follow the pointer.
        if (s->settings.menu_drag_item() >= 0) {
            if (!(wParam & MK_LBUTTON)) {
                s->settings.EndMenuDrag(); // capture lost without WM_LBUTTONUP
                InvalidateRect(hwnd, nullptr, FALSE);
            } else {
                const bool horizontal = s->settings.ContextTab() == 2;
                if (s->settings.MenuDragMove(static_cast<float>(horizontal ? mx : my)))
                    InvalidateRect(hwnd, nullptr, FALSE);
                // Captured: no WM_SETCURSOR arrives, so set the drag cursor here.
                if (s->settings.menu_drag_live())
                    SetCursor(LoadCursorW(nullptr, horizontal ? IDC_SIZEWE : IDC_SIZENS));
                return 0;
            }
        }

        // Press-and-hold on the top staging-tray card: the card follows the
        // pointer; releasing decides between a fling to the back and a spring.
        if (s->trayDrag.pending || s->trayDrag.active) {
            if ((GetKeyState(VK_LBUTTON) & 0x8000) == 0) {
                ReleaseTrayDrag(*s, false);
                if (GetCapture() == hwnd) ReleaseCapture();
                return 0;
            }
            const float sc = std::max(0.5f, s->scale);
            const float dx = static_cast<float>(mx - s->trayDrag.x0) / sc;
            const float dy = static_cast<float>(my - s->trayDrag.y0) / sc;
            if (!s->trayDrag.active && std::abs(dx) + std::abs(dy) > 3.0f)
                s->trayDrag.active = true;
            const ULONGLONG now = ui::motion::NowMs();
            const float dt = static_cast<float>(std::max<ULONGLONG>(1, now - s->trayDrag.last_t));
            const float inst = static_cast<float>(mx - s->trayDrag.last_x) / sc / dt;
            s->trayDrag.vx = s->trayDrag.vx * 0.4f + inst * 0.6f;
            s->trayDrag.last_x = mx;
            s->trayDrag.last_t = now;
            s->trayDrag.dx = dx;
            s->trayDrag.dy = dy;
            if (s->trayDrag.active) {
                // Leaving the sidebar, or pulling well above the tray, turns the
                // gesture into a real drag-out; the card springs back meanwhile.
                RECT crc{};
                GetClientRect(hwnd, &crc);
                const float w = static_cast<float>(crc.right);
                const float h = static_cast<float>(crc.bottom);
                const D2D1_RECT_F sb = s->renderer.SidebarRect(w, h);
                const D2D1_RECT_F trayRc = s->renderer.StagingTrayRect(BuildVm(*s), w, h);
                if (static_cast<float>(mx) >= sb.right ||
                    static_cast<float>(my) < trayRc.top - 16.0f * s->scale) {
                    ReleaseTrayDrag(*s, false);
                    if (GetCapture() == hwnd) ReleaseCapture();
                    StartTrayDragOut(*s);
                    return 0;
                }
            }
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        if (s->stripResizing || s->stripHScrolling) {
            if ((GetKeyState(VK_LBUTTON) & 0x8000) == 0) {
                EndColumnStripResize(*s);
                if (GetCapture() == hwnd) ReleaseCapture();
            } else {
                UpdateColumnStripResize(*s, mx);
            }
            return 0;
        }

        if (s->columnResizing) {
            if ((GetKeyState(VK_LBUTTON) & 0x8000) == 0) {
                s->columnResizing = false;
                s->columnResizeIndex = -1;
                s->columnResizePane = -1;
                if (GetCapture() == hwnd) ReleaseCapture();
            } else {
                ui::WindowViewModel resizeVm = BuildVm(*s);
                D2D1_RECT_F paneRect = s->renderer.ContentRect(
                    static_cast<float>(s->compositor.Width()),
                    static_cast<float>(s->compositor.Height()));
                const ui::PaneViewModel* resizeView = &resizeVm.pane;
                if (s->columnResizePane >= 0 &&
                    s->columnResizePane < static_cast<int>(resizeVm.pane_slots.size())) {
                    const auto& resizeSlot = resizeVm.pane_slots[
                        static_cast<size_t>(s->columnResizePane)];
                    resizeView = &resizeSlot.pane;
                    paneRect = s->renderer.PaneBodyBounds(resizeSlot.pane, resizeSlot.rect);
                } else {
                    paneRect = s->renderer.PaneBodyBounds(resizeVm.pane, paneRect);
                }
                app::Pane* resizePane = PaneAtSlot(*s, s->columnResizePane);
                app::Tab* resizeTab = resizePane ? resizePane->ActiveTab() : nullptr;
                if (resizeTab) {
                    app::ResizePaneColumn(*resizeTab, *resizeView, s->renderer, paneRect,
                                          s->columnResizeIndex, static_cast<float>(mx));
                }
                s->hoverRegion = static_cast<int>(ui::HitTestResult::ColumnDivider);
                s->hoverControlIndex = s->columnResizeIndex;
                s->hoverPaneIndex = s->columnResizePane;
                if (s->renameIndex >= 0) LayoutRenameOverlay(*s);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }

        if (s->detailsPanelResizing) {
            if ((GetKeyState(VK_LBUTTON) & 0x8000) == 0) {
                s->detailsPanelResizing = false;
                if (GetCapture() == hwnd) ReleaseCapture();
            } else {
                const float width = (static_cast<float>(s->compositor.Width())
                    - s->renderer.Margin() - static_cast<float>(mx)) / s->scale;
                // Explorer-style limit: the window decides how far the splitter goes.
                const float max_width = s->renderer.DetailsMaxWidthDip(
                    static_cast<float>(s->compositor.Width()));
                s->detailsPanelWidth = std::clamp(width, ui::kDetailsMinWidthDip, max_width);
                s->renderer.SetDetailsPanelWidth(s->detailsPanelWidth);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }

        if (s->splitterDragging) {
            if (UpdateSplitterDrag(*s, mx, my)) {
                if (!s->splitterDragging && GetCapture() == hwnd) ReleaseCapture();
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
        }

        if (s->scrollbarDragging) {
            app::Tab* tab = ActiveTab(*s);
            if ((!tab && !s->scrollbarSettings) || (GetKeyState(VK_LBUTTON) & 0x8000) == 0) {
                s->scrollbarDragging = false;
                s->scrollbarHorizontal = false;
                s->scrollbarSidebar = false;
                s->scrollbarSettings = false;
                if (GetCapture() == hwnd) ReleaseCapture();
            } else {
                ui::WindowViewModel dragVm = BuildVm(*s);
                D2D1_RECT_F track{}, thumb{};
                float maxScroll = 0.0f;
                if (s->scrollbarSettings) {
                    if (s->renderer.SettingsScrollbarGeometry(
                            dragVm, static_cast<float>(s->compositor.Width()),
                            static_cast<float>(s->compositor.Height()), track, thumb, maxScroll)) {
                        const float travel = std::max(1.0f,
                            (track.bottom - track.top) - (thumb.bottom - thumb.top));
                        s->settings.SetScroll(s->scrollbarDragStartScroll +
                            (my - s->scrollbarDragStartY) * maxScroll / travel, maxScroll);
                        InvalidateRect(hwnd, nullptr, FALSE);
                    }
                } else if (s->scrollbarSidebar && s->renderer.SidebarScrollbarGeometry(
                        dragVm, static_cast<float>(s->compositor.Width()),
                        static_cast<float>(s->compositor.Height()), track, thumb, maxScroll)) {
                    const float travel = std::max(1.0f,
                        (track.bottom - track.top) - (thumb.bottom - thumb.top));
                    s->sidebarScroll = std::clamp(s->scrollbarDragStartScroll +
                        (my - s->scrollbarDragStartY) * maxScroll / travel, 0.0f, maxScroll);
                    InvalidateRect(hwnd, nullptr, FALSE);
                } else if (!s->scrollbarSidebar && s->scrollbarHorizontal &&
                    HorizontalScrollbarGeometry(*s, dragVm.pane, track, thumb, maxScroll)) {
                    const float travel = std::max(1.0f,
                        (track.right - track.left) - (thumb.right - thumb.left));
                    tab->scroll_x = std::clamp(s->scrollbarDragStartScroll +
                        (mx - s->scrollbarDragStartX) * maxScroll / travel, 0.0f, maxScroll);
                    InvalidateRect(hwnd, nullptr, FALSE);
                } else if (!s->scrollbarSidebar && !s->scrollbarHorizontal &&
                           ScrollbarGeometry(*s, dragVm.pane, track, thumb, maxScroll)) {
                    const float travel = std::max(1.0f,
                        (track.bottom - track.top) - (thumb.bottom - thumb.top));
                    tab->scroll_y = std::clamp(
                        (my - track.top - std::min(s->scrollbarGrabOffset, thumb.bottom - thumb.top))
                            * maxScroll / travel,
                        0.0f, maxScroll);
                    s->scrollTargetY = tab->scroll_y;
                    MaybePrefetchSearchPage(*s);
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
                return 0;
            }
        }

        if (TryDetachDraggedPane(*s, mx, my)) return 0;  // the pane is a window now (M6d)
        if (s->tabDragPending) {
            if ((GetKeyState(VK_LBUTTON) & 0x8000) == 0) {
                s->tabDragPending = false;
                s->tabDragging = false;
                s->tabDragIndex = -1;
                s->tabDragRunPos = 0;
                s->tabDragRunLen = 1;
                s->tabDragFromChip = false;
                s->tabDragGroupId = 0;
                s->tabDragSlots = 1.0f;
                s->tabOrder.clear();
                if (GetCapture() == hwnd) ReleaseCapture();
            } else if (TryDetachDraggedTab(*s, mx, my)) {
                return 0;  // off the strip: the tab is a window now (M6a)
            } else if (s->tabDragging ||
                       (s->pane && s->window_tabs.items.size() >= 2 &&
                        std::abs(mx - s->tabDragStartPt.x) >= 2)) {
                if (!s->tabDragging && s->pane &&
                    s->tabDragIndex >= 0 &&
                    s->tabDragIndex < static_cast<int>(s->window_tabs.items.size())) {
                    ui::WindowViewModel vm0 = BuildVm(*s);
                    const float w0 = static_cast<float>(s->compositor.Width());
                    const int n0 = static_cast<int>(vm0.tabs.size());
                    D2D1_RECT_F first{}, last{}, self{};
                    bool haveEnds = false;
                    for (int i = 0; i < n0; ++i) {
                        D2D1_RECT_F rc{};
                        if (!s->renderer.TabItemRect(vm0, w0, i, &rc)) continue; // hidden
                        if (!haveEnds) first = rc;
                        last = rc;
                        haveEnds = true;
                    }
                    bool selfOk = s->renderer.TabItemRect(vm0, w0, s->tabDragIndex, &self);
                    bool chipBlock = false;
                    if (!selfOk && s->tabDragFromChip) {
                        // Collapsed group: the chip itself is the drag block.
                        for (int gi = 0; gi < static_cast<int>(vm0.tab_groups.size()); ++gi) {
                            if (vm0.tab_groups[static_cast<size_t>(gi)].id == s->tabDragGroupId &&
                                s->renderer.TabGroupChipRect(vm0, w0, gi, &self)) {
                                selfOk = true;
                                chipBlock = true;
                                break;
                            }
                        }
                    }
                    if (n0 >= 2 && n0 == static_cast<int>(s->window_tabs.items.size()) &&
                        haveEnds && selfOk) {
                        s->tabDragging = true;
                        s->tabFlowLeft = first.left;
                        s->tabFlowRight = last.right;
                        s->tabSlotW = self.right - self.left;
                        // Chrome ConstrainMoveIndex: pinned and unpinned tabs
                        // each stay within their own region while dragging.
                        {
                            const bool dragPinned = s->tabDragFromChip
                                ? false // groups never contain pinned tabs
                                : s->window_tabs.items[static_cast<size_t>(s->tabDragIndex)]->pinned;
                            int firstUnpinned = -1;
                            for (int i = 0; i < n0; ++i)
                                if (!s->window_tabs.items[static_cast<size_t>(i)]->pinned) {
                                    firstUnpinned = i;
                                    break;
                                }
                            if (firstUnpinned > 0) {
                                D2D1_RECT_F b{};
                                if (s->renderer.TabItemRect(vm0, w0, firstUnpinned, &b)) {
                                    if (dragPinned) s->tabFlowRight = b.left;
                                    else s->tabFlowLeft = b.left;
                                }
                            }
                        }
                        // Uniform pitch excludes group-chip offsets; rest
                        // rects come from TabItemRect where chips matter.
                        s->tabPitch = s->renderer.TabPitchPx(vm0, w0);
                        s->tabDragPressLeft = self.left;
                        s->tabDragFloatLeft = std::clamp(
                            self.left + static_cast<float>(mx - s->tabDragStartPt.x),
                            first.left, last.left);
                        s->tabDragLastX = mx;
                        s->tabTracks.clear();
                        s->tabOffsets.clear();
                        s->chipTracks.clear();
                        s->chipOffsets.clear();
                        s->tabOrder.resize(static_cast<size_t>(n0));
                        for (int i = 0; i < n0; ++i) s->tabOrder[static_cast<size_t>(i)] = i;
                        // Whole-group drags start from the chip; member tabs
                        // always drag solo (Chromium semantics).
                        s->tabDragRunPos = s->tabDragIndex;
                        s->tabDragRunLen = 1;
                        s->tabDragSlots = 1.0f;
                        s->tabDragBlockW = 0.0f;
                        if (s->tabDragFromChip) {
                            const int dragGroup =
                                s->window_tabs.items[static_cast<size_t>(s->tabDragIndex)]->tab_group;
                            int pos = s->tabDragIndex;
                            while (pos > 0 &&
                                   s->window_tabs.items[static_cast<size_t>(pos - 1)]->tab_group == dragGroup)
                                --pos;
                            int end = s->tabDragIndex;
                            while (end + 1 < n0 &&
                                   s->window_tabs.items[static_cast<size_t>(end + 1)]->tab_group == dragGroup)
                                ++end;
                            s->tabDragRunPos = pos;
                            s->tabDragRunLen = end - pos + 1;
                            if (chipBlock) {
                                s->tabDragSlots = s->tabPitch > 0.0f
                                    ? (self.right - self.left) / s->tabPitch : 1.0f;
                                // Collapsed group: the chip alone is the drag
                                // block; its width is px, not slots.
                                s->tabDragBlockW = app::CollapsedChipBlockW(
                                    self.right - self.left, 4.0f * s->scale);
                            } else {
                                s->tabDragSlots = static_cast<float>(s->tabDragRunLen);
                                if (s->tabDragRunLen > 1) {
                                    D2D1_RECT_F runRc{};
                                    if (s->renderer.TabItemRect(vm0, w0, pos, &runRc))
                                        s->tabDragPressLeft = runRc.left;
                                }
                            }
                        }
                    } else {
                        s->tabDragPending = false; // geometry unavailable: plain click
                    }
                }
                if (s->tabDragging) {
                    const int dx = mx - s->tabDragLastX;
                    // Collapsed chip drags clamp by the px block width; slot
                    // math only holds for tab-sized blocks.
                    const float dragBlockW = s->tabDragBlockW > 0.0f
                        ? s->tabDragBlockW : s->tabSlotW * s->tabDragSlots;
                    s->tabDragFloatLeft = std::clamp(
                        s->tabDragPressLeft + static_cast<float>(mx - s->tabDragStartPt.x),
                        s->tabFlowLeft,
                        std::max(s->tabFlowLeft, s->tabFlowRight - dragBlockW));
                    if (std::abs(dx) >= 2 && s->tabDragFromChip) {
                        // Whole-group drag: rotate the run as a block when its
                        // leading/trailing edge crosses the neighbor's center.
                        s->tabDragLastX = mx;
                        const int n = static_cast<int>(s->tabOrder.size());
                        const int runPos = s->tabDragRunPos;
                        const int runLen = s->tabDragRunLen;
                        int blockShift = 0; // -1 left, +1 right, 0 none
                        int siblingPos = -1;
                        if (dx < 0 && runPos > 0) siblingPos = runPos - 1;
                        else if (dx > 0 && runPos + runLen < n) siblingPos = runPos + runLen;
                        if (siblingPos >= 0 && s->tabDragBlockW > 0.0f) {
                            // Collapsed group: the chip alone is the drag
                            // block, so all geometry here is px-based (chips
                            // don't occupy whole slots).
                            const int adjIdx = s->tabOrder[static_cast<size_t>(siblingPos)];
                            const app::LayoutTab* adjTab =
                                (adjIdx >= 0 && adjIdx < static_cast<int>(s->window_tabs.items.size()))
                                    ? s->window_tabs.items[static_cast<size_t>(adjIdx)].get() : nullptr;
                            ui::WindowViewModel vmNow = BuildVm(*s);
                            const float ww = static_cast<float>(s->compositor.Width());
                            float adjRest = 0.0f; // neighbor rest left (px)
                            float adjCur = 0.0f;  // neighbor slide offset (px)
                            float adjW = 0.0f;    // neighbor visual width (px)
                            int adjChipGi = -1;   // vm.tab_groups index when the
                                                  // neighbor is another chip
                            int adjChipGid = 0;   // app::TabGroup::id of it
                            D2D1_RECT_F adjRc{};
                            if (s->renderer.TabItemRect(vmNow, ww, siblingPos, &adjRc)) {
                                adjRest = adjRc.left;
                                adjW = adjRc.right - adjRc.left;
                                if (adjTab) {
                                    const auto oit = s->tabOffsets.find(adjTab);
                                    if (oit != s->tabOffsets.end())
                                        adjCur = oit->second * s->tabPitch;
                                }
                            } else if (adjTab && adjTab->tab_group != 0) {
                                // Hidden member of another collapsed group:
                                // measure and animate that group's chip.
                                for (int gi = 0;
                                     gi < static_cast<int>(vmNow.tab_groups.size()); ++gi) {
                                    if (vmNow.tab_groups[static_cast<size_t>(gi)].id
                                            != adjTab->tab_group)
                                        continue;
                                    D2D1_RECT_F chipRc{};
                                    if (s->renderer.TabGroupChipRect(vmNow, ww, gi, &chipRc)) {
                                        adjChipGi = gi;
                                        adjChipGid = adjTab->tab_group;
                                        adjRest = chipRc.left;
                                        adjW = chipRc.right - chipRc.left;
                                        const auto oit = s->chipOffsets.find(adjChipGid);
                                        if (oit != s->chipOffsets.end()) adjCur = oit->second;
                                    }
                                    break;
                                }
                            }
                            if (adjTab && adjW > 0.0f) {
                                const float adjCenter = adjRest + adjCur + adjW * 0.5f;
                                if (app::ChipBlockCrossed(s->tabDragFloatLeft,
                                        s->tabDragBlockW, adjCenter, dx))
                                    blockShift = dx < 0 ? -1 : 1;
                            }
                            if (blockShift != 0 && adjTab) {
                                s->tabDragRunPos = app::MoveTabRun(
                                    s->tabOrder, runPos, runLen, blockShift);
                                // Measure the displaced unit's NEW rest with
                                // the post-move order; the track covers the
                                // difference from where it visibly sits now.
                                ui::WindowViewModel vmAfter = BuildVm(*s);
                                float newRest = 0.0f;
                                bool haveNew = false;
                                D2D1_RECT_F newRc{};
                                if (adjChipGi >= 0) {
                                    // Group index is stable: pane.tab_groups
                                    // order does not change with tab order.
                                    haveNew = s->renderer.TabGroupChipRect(
                                        vmAfter, ww, adjChipGi, &newRc);
                                } else {
                                    haveNew = s->renderer.TabItemRect(vmAfter, ww,
                                        siblingPos - blockShift * runLen, &newRc);
                                }
                                if (haveNew) newRest = newRc.left;
                                if (haveNew) {
                                    const float startPx = app::DisplacedRestDelta(
                                        adjRest, adjCur, newRest);
                                    if (adjChipGi >= 0) {
                                        if (std::abs(startPx) >= 1.0f) {
                                            s->chipTracks[adjChipGid] = AppState::TabTrack{
                                                startPx, 150, std::chrono::steady_clock::now() };
                                            s->chipOffsets[adjChipGid] = startPx;
                                        }
                                    } else if (s->tabPitch > 0.0f) {
                                        const float start = startPx / s->tabPitch;
                                        if (std::abs(start) >= 0.01f) {
                                            s->tabTracks[adjTab] = AppState::TabTrack{
                                                start, 150, std::chrono::steady_clock::now() };
                                            s->tabOffsets[adjTab] = start;
                                        }
                                    }
                                }
                            }
                        } else if (siblingPos >= 0) {
                            const int adjIdx = s->tabOrder[static_cast<size_t>(siblingPos)];
                            const app::LayoutTab* adjTab =
                                (adjIdx >= 0 && adjIdx < static_cast<int>(s->window_tabs.items.size()))
                                    ? s->window_tabs.items[static_cast<size_t>(adjIdx)].get() : nullptr;
                            const float adjOff = [&]() {
                                if (!adjTab) return 0.0f;
                                const auto oit = s->tabOffsets.find(adjTab);
                                return oit != s->tabOffsets.end() ? oit->second : 0.0f;
                            }();
                            float adjRest = s->tabFlowLeft
                                + static_cast<float>(siblingPos) * s->tabPitch;
                            {
                                ui::WindowViewModel vmNow = BuildVm(*s);
                                D2D1_RECT_F adjRc{};
                                if (s->renderer.TabItemRect(vmNow,
                                        static_cast<float>(s->compositor.Width()),
                                        siblingPos, &adjRc))
                                    adjRest = adjRc.left;
                            }
                            const float adjCenter = adjRest + adjOff * s->tabPitch
                                + s->tabSlotW * 0.5f;
                            if (dx < 0 && s->tabDragFloatLeft < adjCenter) blockShift = -1;
                            else if (dx > 0 && s->tabDragFloatLeft
                                     + s->tabDragSlots * s->tabSlotW > adjCenter)
                                blockShift = 1;
                            if (blockShift != 0 && adjTab) {
                                s->tabDragRunPos = app::MoveTabRun(
                                    s->tabOrder, runPos, runLen, blockShift);
                                const float start = static_cast<float>(-blockShift * runLen)
                                    + adjOff;
                                if (std::abs(start) >= 0.01f) {
                                    s->tabTracks[adjTab] = AppState::TabTrack{
                                        start, 150, std::chrono::steady_clock::now() };
                                    s->tabOffsets[adjTab] = start;
                                }
                            }
                        }
                        InvalidateRect(hwnd, nullptr, FALSE);
                        return 0;
                    }
                    if (std::abs(dx) >= 2) { // 2px deadzone against jitter, as in TabBar
                        s->tabDragLastX = mx;
                        int cur = -1;
                        for (int p = 0; p < static_cast<int>(s->tabOrder.size()); ++p)
                            if (s->tabOrder[p] == s->tabDragIndex) { cur = p; break; }
                        const int n = static_cast<int>(s->tabOrder.size());
                        const int adj = dx < 0 ? cur - 1 : cur + 1;
                        int swapWith = -1;
                        if (cur >= 0 && adj >= 0 && adj < n) {
                            const int adjIdx = s->tabOrder[adj];
                            const app::LayoutTab* adjTab =
                                (adjIdx >= 0 && adjIdx < static_cast<int>(s->window_tabs.items.size()))
                                    ? s->window_tabs.items[static_cast<size_t>(adjIdx)].get() : nullptr;
                            const int dragGid =
                                s->window_tabs.items[static_cast<size_t>(s->tabDragIndex)]->tab_group;
                            const int adjGid = adjTab ? adjTab->tab_group : 0;
                            if (dragGid != 0 && adjGid == 0) {
                                // A group member crossing a free tab moves the
                                // free tab across the whole group. This keeps
                                // the group contiguous without silently
                                // removing the dragged member from it.
                                ui::WindowViewModel vmNow = BuildVm(*s);
                                const float ww = static_cast<float>(s->compositor.Width());
                                D2D1_RECT_F adjRc{};
                                if (s->renderer.TabItemRect(vmNow, ww, adj, &adjRc)) {
                                    const auto off = s->tabOffsets.find(adjTab);
                                    const float adjOff = off != s->tabOffsets.end()
                                        ? off->second : 0.0f;
                                    const float adjCenter = adjRc.left + adjOff * s->tabPitch
                                        + s->tabSlotW * 0.5f;
                                    const bool crossed = dx < 0
                                        ? s->tabDragFloatLeft < adjCenter
                                        : s->tabDragFloatLeft + s->tabSlotW > adjCenter;
                                    if (crossed) {
                                        const std::vector<int> oldOrder = s->tabOrder;
                                        std::vector<int> groupOf(s->window_tabs.items.size());
                                        for (size_t i = 0; i < s->window_tabs.items.size(); ++i)
                                            groupOf[i] = s->window_tabs.items[i]->tab_group;
                                        D2D1_RECT_F oldChip{};
                                        int chipIndex = -1;
                                        for (int gi = 0;
                                             gi < static_cast<int>(vmNow.tab_groups.size()); ++gi) {
                                            if (vmNow.tab_groups[static_cast<size_t>(gi)].id == dragGid) {
                                                if (s->renderer.TabGroupChipRect(
                                                        vmNow, ww, gi, &oldChip))
                                                    chipIndex = gi;
                                                break;
                                            }
                                        }
                                        if (app::MoveTabGroupAcrossFreeTab(
                                                s->tabOrder, groupOf, cur, dx < 0 ? -1 : 1)) {
                                            ui::WindowViewModel vmAfter = BuildVm(*s);
                                            for (int newPos = 0; newPos < n; ++newPos) {
                                                const int tabIndex = s->tabOrder[
                                                    static_cast<size_t>(newPos)];
                                                if (tabIndex == s->tabDragIndex) continue;
                                                const auto old = std::find(oldOrder.begin(),
                                                    oldOrder.end(), tabIndex);
                                                if (old == oldOrder.end()) continue;
                                                const int oldPos = static_cast<int>(
                                                    std::distance(oldOrder.begin(), old));
                                                if (oldPos == newPos) continue;
                                                D2D1_RECT_F oldRc{}, newRc{};
                                                if (!s->renderer.TabItemRect(vmNow, ww, oldPos, &oldRc) ||
                                                    !s->renderer.TabItemRect(vmAfter, ww, newPos, &newRc))
                                                    continue;
                                                const app::LayoutTab* tab = s->window_tabs.items[
                                                    static_cast<size_t>(tabIndex)].get();
                                                const auto current = s->tabOffsets.find(tab);
                                                const float currentPx = current != s->tabOffsets.end()
                                                    ? current->second * s->tabPitch : 0.0f;
                                                const float start = (oldRc.left + currentPx - newRc.left)
                                                    / s->tabPitch;
                                                s->tabTracks[tab] = AppState::TabTrack{
                                                    start, 150, std::chrono::steady_clock::now()};
                                                s->tabOffsets[tab] = start;
                                            }
                                            if (chipIndex >= 0) {
                                                D2D1_RECT_F newChip{};
                                                if (s->renderer.TabGroupChipRect(
                                                        vmAfter, ww, chipIndex, &newChip)) {
                                                    const auto current = s->chipOffsets.find(dragGid);
                                                    const float currentPx = current != s->chipOffsets.end()
                                                        ? current->second : 0.0f;
                                                    const float start = oldChip.left + currentPx
                                                        - newChip.left;
                                                    s->chipTracks[dragGid] = AppState::TabTrack{
                                                        start, 150,
                                                        std::chrono::steady_clock::now()};
                                                    s->chipOffsets[dragGid] = start;
                                                }
                                            }
                                        }
                                    }
                                }
                            } else if (adjGid != 0 && adjGid != dragGid) {
                                // Group neighbor: hop the WHOLE run (expanded
                                // or collapsed) past the dragged tab, so the
                                // tab can never land between group members.
                                std::vector<int> groupOf(s->window_tabs.items.size());
                                for (size_t i = 0; i < s->window_tabs.items.size(); ++i)
                                    groupOf[i] = s->window_tabs.items[i]->tab_group;
                                const app::GroupRun run =
                                    app::FindGroupRun(s->tabOrder, groupOf, adj, adjGid);
                                if (run.len > 0) {
                                    ui::WindowViewModel vmNow = BuildVm(*s);
                                    const float ww = static_cast<float>(s->compositor.Width());
                                    float blockLeft = 0.0f, blockRight = 0.0f, blockOff = 0.0f;
                                    bool haveBlock = false;
                                    int chipGi = -1; // collapsed: vm.tab_groups index
                                    D2D1_RECT_F rcFirst{}, rcLast{};
                                    if (s->renderer.TabItemRect(vmNow, ww, run.pos, &rcFirst) &&
                                        s->renderer.TabItemRect(vmNow, ww,
                                            run.pos + run.len - 1, &rcLast)) {
                                        blockLeft = rcFirst.left;
                                        blockRight = rcLast.right;
                                        const int fIdx = s->tabOrder[static_cast<size_t>(run.pos)];
                                        if (fIdx >= 0 &&
                                            fIdx < static_cast<int>(s->window_tabs.items.size())) {
                                            const auto oit = s->tabOffsets.find(
                                                s->window_tabs.items[static_cast<size_t>(fIdx)].get());
                                            if (oit != s->tabOffsets.end())
                                                blockOff = oit->second * s->tabPitch;
                                        }
                                        haveBlock = true;
                                    } else {
                                        // Collapsed group: the chip is the block.
                                        for (int gi = 0;
                                             gi < static_cast<int>(vmNow.tab_groups.size()); ++gi) {
                                            if (vmNow.tab_groups[static_cast<size_t>(gi)].id
                                                    != adjGid)
                                                continue;
                                            D2D1_RECT_F chipRc{};
                                            if (s->renderer.TabGroupChipRect(
                                                    vmNow, ww, gi, &chipRc)) {
                                                chipGi = gi;
                                                blockLeft = chipRc.left;
                                                blockRight = chipRc.right;
                                                const auto oit = s->chipOffsets.find(adjGid);
                                                if (oit != s->chipOffsets.end())
                                                    blockOff = oit->second;
                                                haveBlock = true;
                                            }
                                            break;
                                        }
                                    }
                                    if (haveBlock &&
                                        app::ChipBlockCrossed(s->tabDragFloatLeft, s->tabSlotW,
                                            (blockLeft + blockRight) * 0.5f + blockOff, dx)) {
                                        // The run slides one slot against the
                                        // drag direction (MoveTabRun keeps the
                                        // group contiguous by construction).
                                        const int dir = dx < 0 ? 1 : -1;
                                        const int newRunPos = app::MoveTabRun(
                                            s->tabOrder, run.pos, run.len, dir);
                                        ui::WindowViewModel vmAfter = BuildVm(*s);
                                        if (chipGi >= 0) {
                                            D2D1_RECT_F newRc{};
                                            if (s->renderer.TabGroupChipRect(
                                                    vmAfter, ww, chipGi, &newRc)) {
                                                const float startPx = app::DisplacedRestDelta(
                                                    blockLeft, blockOff, newRc.left);
                                                if (std::abs(startPx) >= 1.0f) {
                                                    s->chipTracks[adjGid] = AppState::TabTrack{
                                                        startPx, 150,
                                                        std::chrono::steady_clock::now() };
                                                    s->chipOffsets[adjGid] = startPx;
                                                }
                                            }
                                        } else if (s->tabPitch > 0.0f) {
                                            // Every visible member slides one
                                            // slot; track each from where it
                                            // visibly sits now.
                                            for (int p = newRunPos;
                                                 p < newRunPos + run.len && p < n; ++p) {
                                                const int tIdx = s->tabOrder[static_cast<size_t>(p)];
                                                if (tIdx < 0 ||
                                                    tIdx >= static_cast<int>(s->window_tabs.items.size()))
                                                    continue;
                                                const app::LayoutTab* member =
                                                    s->window_tabs.items[static_cast<size_t>(tIdx)].get();
                                                float oldRest = s->tabFlowLeft
                                                    + static_cast<float>(p - dir) * s->tabPitch;
                                                D2D1_RECT_F oldRc{}, newRc{};
                                                if (s->renderer.TabItemRect(
                                                        vmNow, ww, p - dir, &oldRc))
                                                    oldRest = oldRc.left;
                                                if (!s->renderer.TabItemRect(
                                                        vmAfter, ww, p, &newRc))
                                                    continue; // hidden: no visual to animate
                                                float memberOff = 0.0f;
                                                const auto oit = s->tabOffsets.find(member);
                                                if (oit != s->tabOffsets.end())
                                                    memberOff = oit->second * s->tabPitch;
                                                const float startPx = app::DisplacedRestDelta(
                                                    oldRest, memberOff, newRc.left);
                                                const float start = startPx / s->tabPitch;
                                                if (std::abs(start) >= 0.01f) {
                                                    s->tabTracks[member] = AppState::TabTrack{
                                                        start, 150,
                                                        std::chrono::steady_clock::now() };
                                                    s->tabOffsets[member] = start;
                                                }
                                            }
                                        }
                                    }
                                }
                            } else {
                                const float adjOff = [&]() {
                                    if (!adjTab) return 0.0f;
                                    const auto oit = s->tabOffsets.find(adjTab);
                                    return oit != s->tabOffsets.end() ? oit->second : 0.0f;
                                }();
                                // Chip offsets make slots non-uniform: ask the
                                // renderer for this slot's rest rect instead of
                                // extrapolating from the pitch.
                                float adjRest = s->tabFlowLeft
                                    + static_cast<float>(adj) * s->tabPitch;
                                {
                                    ui::WindowViewModel vmNow = BuildVm(*s);
                                    D2D1_RECT_F adjRc{};
                                    if (s->renderer.TabItemRect(vmNow,
                                            static_cast<float>(s->compositor.Width()),
                                            adj, &adjRc))
                                        adjRest = adjRc.left;
                                }
                                const float adjCenter = adjRest + adjOff * s->tabPitch
                                    + s->tabSlotW * 0.5f;
                                if ((dx < 0 && s->tabDragFloatLeft < adjCenter) ||
                                    (dx > 0 && s->tabDragFloatLeft + s->tabSlotW > adjCenter))
                                    swapWith = adj;
                            }
                        }
                        if (swapWith >= 0) {
                            const int siblingIdx = s->tabOrder[swapWith];
                            std::swap(s->tabOrder[cur], s->tabOrder[swapWith]);
                            const app::LayoutTab* sibling =
                                (siblingIdx >= 0 && siblingIdx < static_cast<int>(s->window_tabs.items.size()))
                                    ? s->window_tabs.items[static_cast<size_t>(siblingIdx)].get() : nullptr;
                            if (sibling) {
                                const auto off = s->tabOffsets.find(sibling);
                                const float curOff = off != s->tabOffsets.end() ? off->second : 0.0f;
                                const float start = static_cast<float>(swapWith) + curOff
                                    - static_cast<float>(cur);
                                if (std::abs(start) >= 0.01f) {
                                    s->tabTracks[sibling] = AppState::TabTrack{
                                        start, 150, std::chrono::steady_clock::now() };
                                    s->tabOffsets[sibling] = start;
                                }
                            }
                        }
                    }
                    InvalidateRect(hwnd, nullptr, FALSE);
                    return 0;
                }
            }
        }

        if (s->marqueePending || s->marqueeActive) {
            if ((wParam & MK_LBUTTON) == 0) {
                if (s->marqueeActive) ApplyMarqueeSelection(*s);
                ResetMarquee(*s);
            } else {
                s->marqueeCur = POINT{ mx, my };
                if ((GetKeyState(VK_CONTROL) & 0x8000) != 0 ||
                    (GetKeyState(VK_SHIFT) & 0x8000) != 0 ||
                    (GetKeyState(VK_MENU) & 0x8000) != 0)
                    s->blankClickTab = nullptr;
                if (!s->marqueeActive &&
                    (std::abs(mx - s->marqueeStart.x) >= ListDragSlop(*s, SM_CXDRAG, false) ||
                     std::abs(my - s->marqueeStart.y) >= ListDragSlop(*s, SM_CYDRAG, false))) {
                    s->marqueeActive = true;
                    s->marqueePending = false;
                    s->tooltipText.clear();  // the hover tip of the pressed row would ride along
                    // A marquee from a row clears the selection only now that
                    // it really is a marquee (a plain click selects the row).
                    if (s->marqueeRowIndex >= 0) {
                        if (!s->marqueeAdditive) {
                            if (app::Tab* tab = ActiveTab(*s)) tab->ClearSelection();
                        }
                        s->marqueeRowIndex = -1;
                    }
                }
                // Applied once per frame in Render (UpdateMarqueeFrame).
                if (s->marqueeActive) s->marqueeSelectionDirty = true;
                UpdateMarqueeAutoScroll(*s);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }

        // Drag-out threshold (1B-2): left button held after a row click.
        if (s->dragPending) {
            if ((GetKeyState(VK_LBUTTON) & 0x8000) == 0) {
                s->dragPending = false;
                if (s->renameClickCandidate && s->renameClickDue == 0)
                    CancelRenameClick(*s);
            } else if (std::abs(mx - s->dragStartPt.x) >= ListDragSlop(*s, SM_CXDRAG, true) ||
                       std::abs(my - s->dragStartPt.y) >= ListDragSlop(*s, SM_CYDRAG, true)) {
                s->dragPending = false;
                CancelRenameClick(*s);
                StartDragOut(*s);
                return 0;
            }
        }

        if (s->starDragPending || s->starDragActive) {
            if ((GetKeyState(VK_LBUTTON) & 0x8000) == 0) {
                s->starDragPending = false;
                s->starDragActive = false;
                s->starDragPath.clear();
                s->dropSidebar = -1;
            } else {
                if (!s->starDragActive &&
                    (std::abs(mx - s->starDragStartPt.x) >= GetSystemMetrics(SM_CXDRAG) ||
                     std::abs(my - s->starDragStartPt.y) >= GetSystemMetrics(SM_CYDRAG))) {
                    s->starDragActive = true;
                    s->starDragPending = false;
                }
                if (s->starDragActive) {
                    ui::WindowViewModel dragVm = BuildVm(*s);
                    const D2D1_RECT_F dragRect = D2D1::RectF(
                        0, 0, static_cast<float>(s->compositor.Width()),
                        static_cast<float>(s->compositor.Height()));
                    const D2D1_RECT_F sidebar = s->renderer.SidebarRect(
                        dragRect.right, dragRect.bottom);
                    const float max_scroll = s->renderer.SidebarMaxScroll(
                        dragVm, dragRect.right, dragRect.bottom);
                    if (my < sidebar.top + 28.0f * s->scale) {
                        s->sidebarScroll = std::max(
                            0.0f, s->sidebarScroll - 8.0f * s->scale);
                        dragVm = BuildVm(*s);
                    } else if (my > sidebar.bottom - 170.0f * s->scale) {
                        s->sidebarScroll = std::min(
                            max_scroll, s->sidebarScroll + 8.0f * s->scale);
                        dragVm = BuildVm(*s);
                    }
                    const ui::HitTestResult target = s->renderer.HitTest(
                        dragVm, dragRect, static_cast<float>(mx), static_cast<float>(my));
                    const app::StarredItem* item = s->places.FindStarred(target.path);
                    if (target.region == ui::HitTestResult::SidebarItem && item &&
                        item->kind == app::PlaceItemKind::Folder) {
                        const auto folders = s->places.StarredFolderPaths();
                        const auto found = std::find_if(
                            folders.begin(), folders.end(), [&](const auto& path) {
                                return _wcsicmp(path.c_str(), target.path.c_str()) == 0;
                            });
                        if (found != folders.end()) {
                            s->starDragTarget = static_cast<size_t>(found - folders.begin());
                            s->starDragGroup = item->group;
                            s->dropSidebar = target.index;
                        }
                    } else if (target.region == ui::HitTestResult::SidebarItem &&
                               app::IsStarredGroupPath(target.path)) {
                        // A group's row: the folder joins the group.
                        s->starDragTarget = SIZE_MAX;
                        s->starDragGroup = app::StarredGroupIdFromPath(target.path);
                        s->dropSidebar = target.index;
                    } else {
                        s->dropSidebar = -1;
                    }
                    InvalidateRect(hwnd, nullptr, FALSE);
                    return 0;
                }
            }
        }

        // Tag reorder drag (QFluentKit TabBar model): the dragged tag follows
        // cursor deltas 1:1 and swaps with a sibling on center crossing.
        // Sidebar section header drag: past a small deadzone the press turns into
        // a reorder and the insertion line follows the cursor.
        if (s->groupDragPending || s->groupDragActive) {
            if ((GetKeyState(VK_LBUTTON) & 0x8000) == 0) {
                ResetSidebarGroupDrag(*s);
                if (GetCapture() == hwnd) ReleaseCapture();
                InvalidateRect(hwnd, nullptr, FALSE);
            } else {
                if (!s->groupDragActive && s->groupDragId >= 0 &&
                    std::abs(my - s->groupDragStartPt.y) >= kSidebarDragDeadzonePx) {
                    s->groupDragActive = true;
                }
                if (s->groupDragActive) {
                    UpdateSidebarGroupDrag(*s, my);
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
            }
        }

        // Quick-access pin drag: past the deadzone the press becomes a reorder.
        if (s->pinDragPending || s->pinDragActive) {
            if ((GetKeyState(VK_LBUTTON) & 0x8000) == 0) {
                ResetSidebarPinDrag(*s);
                if (GetCapture() == hwnd) ReleaseCapture();
                InvalidateRect(hwnd, nullptr, FALSE);
            } else {
                if (!s->pinDragActive && !s->pinDragPath.empty() &&
                    std::abs(my - s->pinDragStartPt.y) >= kSidebarDragDeadzonePx) {
                    s->pinDragActive = true;
                }
                if (s->pinDragActive) {
                    UpdateSidebarPinDrag(*s, my);
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
            }
        }

        TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, hwnd, 0 };
        TrackMouseEvent(&tme);

        ui::WindowViewModel vm = BuildVm(*s);
        D2D1_RECT_F rect = D2D1::RectF(0, 0, (float)s->compositor.Width(), (float)s->compositor.Height());
        ui::HitTestResult hit = s->renderer.HitTest(vm, rect, (float)mx, (float)my);
        const bool target_changed = static_cast<int>(hit.region) != s->hoverRegion ||
            hit.index != s->hoverControlIndex || hit.sub_index != s->hoverSubIndex ||
            hit.pane_index != s->hoverPaneIndex;
        if (target_changed && hit.region == ui::HitTestResult::SidebarItem && fs::IsUncPath(hit.path))
            RequestUncProbe(*s, hit.path);
        // A move from one row to another of the same details list repaints
        // those rows and the plate's path only (partial redraw).
        const auto row_region = [](int region) {
            using R = ui::HitTestResult;
            switch (static_cast<R::Region>(region)) {
            case R::Row: case R::RowStar: case R::RowTagDot: case R::RowFolderSize:
            case R::RowNewTab: case R::ChangeBadge: case R::RowMore: case R::RowCheck:
                return true;
            default:
                return false;
            }
        };
        const int prev_row = s->hoverRow, prev_pane = s->hoverPaneIndex, prev_region = s->hoverRegion;
        const int prev_crumb = s->breadcrumbHover;
        const bool prev_sidebar_hot = s->sidebarScrollbarHot;
        if (UpdatePointerHover(*s, hit, POINT{mx, my})) {
            D2D1_RECT_F rows{};
            if (row_region(prev_region) && row_region(s->hoverRegion) && prev_pane == s->hoverPaneIndex &&
                prev_crumb == s->breadcrumbHover && prev_sidebar_hot == s->sidebarScrollbarHot &&
                s->renderer.HoverRowDamage(prev_pane, prev_row, s->hoverRow, &rows))
                app::InvalidatePartial(hwnd, app::DamagePixels(rows));
            else
                InvalidateRect(hwnd, nullptr, FALSE);
        }
        D2D1_RECT_F content = s->renderer.ContentRect((float)s->compositor.Width(), (float)s->compositor.Height());
        float extra = 0.0f;
        if (app::Tab* tab = ActiveTab(*s); tab) {
            if (!tab->banner_message.empty()) {
                const float drawn = s->renderer.FocusedPaneBannerHeight();  // one or two lines
                extra = drawn > 0.0f ? drawn : 36.0f * s->scale;
            }
            std::wstring kind;
            if (app::ParsePulsePath(tab->current_path, &kind, nullptr) && kind == L"recent")
                extra += 40.0f * s->scale;
        }
        float listTop = content.top + s->renderer.PaneHeaderHeight() + extra + s->renderer.ColumnHeaderHeight();
        bool sbHit = (mx >= content.right - 14 * s->scale && mx < content.right &&
                      my >= listTop && my < content.bottom);
        if (sbHit != s->scrollbarHovered) {
            s->scrollbarHovered = sbHit;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
}

LRESULT HandleMouseLeave(AppState* s, HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        (void)msg;
        (void)wParam;
        (void)lParam;
        if (s) {
            AiAssistantMouseLeave(*s);
            if (s->renderer.ReleaseTabWidths()) InvalidateRect(hwnd, nullptr, FALSE);
            s->scrollbarHovered = false;
            s->sidebarScrollbarHot = false;
            s->hoverRow = -1;
            s->hoverPaneIndex = -1;
            s->ctxHoverSince = 0;
            s->ctxHoverPrefetched.clear();
            s->breadcrumbHover = -1;
            s->hoverRegion = 0;
            s->hoverControlIndex = -1;
            s->hoverSubIndex = -1;
            s->hoverPath.clear();
            s->hoverLabel.clear();
            s->hoverSince = 0;
            s->tooltipText.clear();
            s->bloom_accent.SetPointer(0.0f, 0.0f, false);
            if (GetCapture() != hwnd) s->dragPending = false;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
}

// 滚动时固定上级: a pinned folder glides back to its own row at the top.
static void ClickTreeSticky(AppState& s, int pane_slot, int view_row) {
    app::Pane* pane = PaneAtSlot(s, pane_slot);
    if (!pane) return;
    if (pane != s.pane) FocusPane(s, pane);
    if (!ActiveTab(s)) return;
    if (s.scrollAnimating) CancelScrollAnimation(s);
    ui::PaneViewModel vm;
    app::FillPaneViewModel(vm, *s.pane, &s.places);
    const D2D1_RECT_F bounds = FocusedPaneRect(s);
    const float row_top = s.renderer.ItemRectInPane(vm, bounds, view_row).top;
    const float list_top = s.renderer.PaneListRect(vm, bounds).top;
    if (row_top < list_top - 0.5f) StartSmoothScroll(s, row_top - list_top, false);
}

// "Group by" header: toggle collapse, or select every row of the group.
static void ClickGroupHeader(AppState& s, int pane_slot, int group, bool select) {
    app::Pane* pane = PaneAtSlot(s, pane_slot);
    if (!pane) return;
    if (pane != s.pane) FocusPane(s, pane);
    app::Tab* tab = pane->ActiveTab();
    if (!tab) return;
    ui::PaneViewModel vm;
    app::FillPaneViewModel(vm, *pane, &s.places);
    const ui::ListGroups* groups = vm.Groups();
    if (!groups || group < 0 || group >= static_cast<int>(groups->size())) return;
    const ui::ListGroup& g = (*groups)[static_cast<size_t>(group)];
    if (select) {
        std::vector<int> indices;
        indices.reserve(static_cast<size_t>(g.count));
        for (int row = g.first; row < g.first + g.count; ++row) {
            const int src = vm.SourceIndex(row);
            if (src >= 0) indices.push_back(src);
        }
        tab->SelectIndices(indices);
    } else {
        // A pinned header belongs to a group scrolled above the top; bring its
        // header to the top first so collapsing keeps the user in place.
        const D2D1_RECT_F bounds = FocusedPaneRect(s);
        const D2D1_RECT_F list = s.renderer.PaneListRect(vm, bounds);
        const float header_top = s.renderer.ItemRectInPane(vm, bounds, g.first).top -
                                 ui::kGroupHeaderDip * s.scale;
        if (header_top < list.top - 0.5f) {
            tab->scroll_y = std::max(0.0f, tab->scroll_y - (list.top - header_top));
            s.scrollTargetY = tab->scroll_y;
            s.scrollAnimating = false;
        }
        auto& collapsed = tab->collapsed_groups[tab->current_path];
        if (!collapsed.erase(g.key)) collapsed.insert(g.key);
        ++tab->group_collapse_rev;
        ClampScroll(s);
    }
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

LRESULT HandleLButtonDown(AppState* s, HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        if (!s) return DefWindowProcW(hwnd, msg, wParam, lParam);
        s->blankDoubleTab = nullptr;
        s->blankDoublePending = false;
        int mx = GET_X_LPARAM(lParam);
        int my = GET_Y_LPARAM(lParam);
        if (AiAssistantMouseDown(*s, mx, my)) return 0;   // Pulse 助手
        if (DismissPathEditorOutside(*s, mx, my)) return 0;  // chose a 别名 suggestion
        if (ClickFilterTagSuggestion(*s, mx, my)) return 0;  // the filter box's # completion
        CancelRenameClick(*s);
        CancelScrollAnimation(*s);
        // Commit explicitly before changing pane/selection; a click on a
        // non-focusable part of the owner need not produce WM_KILLFOCUS.
        if (s->renameIndex >= 0) HideRenameOverlay(*s, true);
        ui::WindowViewModel vm = BuildVm(*s, false);
        D2D1_RECT_F rect = D2D1::RectF(0, 0, (float)s->compositor.Width(), (float)s->compositor.Height());
        ui::HitTestResult hit = s->renderer.HitTest(vm, rect, (float)mx, (float)my);
        // A narrow pane's opened size slider closes on any press outside it.
        if (hit.region != ui::HitTestResult::PaneSizeSlider && hit.region != ui::HitTestResult::PaneSizeButton) {
            for (int slot = 0; slot < static_cast<int>(vm.pane_slots.size()); ++slot)
                if (app::Pane* pane = PaneAtSlot(*s, slot)) pane->size_slider_open = false;
        }
        // Picker T3c: with 自定义工具栏 open, a toolbar pin can be dragged (or
        // clicked); a press anywhere else closes the picker first.
        if (ui::ToolbarPickerOpen()) {
            if (hit.region == ui::HitTestResult::ToolbarPin) {
                BeginToolbarPinDrag(*s, hit.index, POINT{mx, my});
                return 0;
            }
            ui::CloseToolbarPicker();
        }
        if (s->addressSearching && hit.region != ui::HitTestResult::AddressSearchInput &&
            hit.region != ui::HitTestResult::AddressSearchScope &&
            hit.region != ui::HitTestResult::AddressSearchMode &&
            hit.region != ui::HitTestResult::AddressSearchContent &&
            hit.region != ui::HitTestResult::AddressSearchOptions &&
            hit.region != ui::HitTestResult::ContentIndexManage &&
            hit.region != ui::HitTestResult::NetworkIndexAdd &&
            hit.region != ui::HitTestResult::PaneBannerButton &&
            hit.region != ui::HitTestResult::AddressSearchClear &&
            hit.region != ui::HitTestResult::AddressSearchClose) {
            HideAddressEditor(*s, false);
        }
        if (s->filterEditing && hit.region != ui::HitTestResult::FilterBox &&
            hit.region != ui::HitTestResult::FilterClear) {
            HideFilterEditor(*s, true);
        }
        // Single click opens: a press that switches panes or brings the
        // window forward only selects.
        s->singleOpenCandidate = false;
        const bool pressActivates =
            (hit.pane_index >= 0 && PaneAtSlot(*s, hit.pane_index) != nullptr &&
             PaneAtSlot(*s, hit.pane_index) != s->pane) ||
            (s->activateClickTick != 0 && GetTickCount64() - s->activateClickTick < 500);
        s->activateClickTick = 0;
        if (hit.pane_index >= 0) {
            if (app::Pane* p = PaneAtSlot(*s, hit.pane_index)) FocusPane(*s, p);
        }
        if (hit.region == ui::HitTestResult::RowCheck || hit.region == ui::HitTestResult::HeaderCheck) {
            if (HandleItemCheckPress(*s, hit)) InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        if (hit.region == ui::HitTestResult::TreeSticky) {
            ClickTreeSticky(*s, hit.pane_index, hit.index);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        ArmPaneDrag(*s, hit, mx, my);  // the press still does what it did; a drag takes the pane out (M6d)
        ArmBlankWindowDrag(*s, hit, mx, my);  // likewise: a drag on blank chrome moves the window
        ArmBlankDeselect(*s, hit, mx, my, pressActivates);  // ...and a click there clears the selection
        if (HandleChangeClick(*s, hit, POINT{mx, my})) {
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        if (HandleWindowIdentityPress(*s, hit, POINT{mx, my})) return 0;  // M7c
        if (HandlePaneBannerPress(*s, hit)) return 0;  // retry, go up, reconnect... on a pane banner
        if (HandleRowTagDotPress(*s, hit)) return 0;   // a row's tag dot: only that tag here
        if (hit.region == ui::HitTestResult::PaneWallHint) {  // photo wall suggestion card
            if (hit.index == 1) WallHintSwitch(*s);
            else if (hit.index == 2) WallHintNever(*s);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        if (hit.region == ui::HitTestResult::TrayShelf) {
            s->dragPending = false;
            HandleTrayShelfPress(*s, hwnd, hit, mx, my);
            return 0;
        }
        if (hit.region == ui::HitTestResult::StagedBadge) {
            // "已暂存" pill on a row: take that item off the shelf.
            s->dragPending = false;
            if (!hit.path.empty()) UnstagePath(*s, hit.path);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        if (hit.region == ui::HitTestResult::TrayCard && hit.index == 0) {
            const auto top = TrayDeckEntries(s->tray, static_cast<size_t>(TrayStackTop(*s)), 1);
            if (!top.empty()) {
                s->dragPending = false;
                s->trayDrag = AppState::TrayDrag{};
                s->trayDrag.pending = true;
                s->trayDrag.path = top.front().item->path;
                s->trayDrag.x0 = mx;
                s->trayDrag.y0 = my;
                s->trayDrag.last_x = mx;
                s->trayDrag.last_t = ui::motion::NowMs();
                SetCapture(hwnd);
                return 0;
            }
        }
        if (hit.region == ui::HitTestResult::TrayPrev) {
            s->dragPending = false;
            TrayStepBack(*s);
            return 0;
        }
        if (hit.region == ui::HitTestResult::TrayNext) {
            s->dragPending = false;
            ThrowTrayTop(*s, -1.0f, -30.0f, -10.0f);
            return 0;
        }
        if (HandleColumnStripMouseDown(*s, vm, hit, mx)) {
            s->dragPending = false;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        if (hit.region == ui::HitTestResult::DetailsResize) {
            s->dragPending = false;
            s->detailsPanelResizing = true;
            SetCapture(hwnd);
            return 0;
        }
        if (hit.region == ui::HitTestResult::DetailsCloudDownload) {
            s->dragPending = false;
            StartCloudPreviewDownload(*s, vm.details.path, vm.details.size_value);
            return 0;
        }
        if (hit.region == ui::HitTestResult::DetailsPreviewEnable) {
            s->detailsPreviewEnabled = !s->detailsPreviewEnabled;
            s->detailsScroll = 0;
            if (!s->detailsPreviewEnabled) {
                s->detailsPreviewPanning = false;
                s->renderer.EndDetailsPreviewPan();
            }
            s->dragPending = false;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        if (hit.region == ui::HitTestResult::DetailsPreviewToggle) {
            s->detailsPreviewOnly = !s->detailsPreviewOnly;
            s->detailsPreviewExpansionFrom = s->detailsPreviewExpansion;
            s->detailsPreviewFoldStart = GetTickCount64();
            s->detailsScroll = 0;
            s->dragPending = false;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        // Tag rows that are not places, and the tag view condition bar (kept
        // out of the click chain below, which is at MSVC's nesting limit).
        if (hit.region == ui::HitTestResult::SidebarItem && app::IsTagGroupPath(hit.path)) {
            // A tag group header only folds; 未分组 (no id) does nothing.
            ToggleTagGroupRow(*s, hit.path);
            return 0;
        }
        if (hit.region == ui::HitTestResult::SidebarItem && hit.path == app::MakeTagManagerPath()) {
            POINT sp{ mx, my };
            ClientToScreen(hwnd, &sp);
            ShowTagManager(*s, sp);
            return 0;
        }
        if (HandleFavoritesBarPress(*s, hit)) return 0;   // 收藏栏
        if (hit.region == ui::HitTestResult::TagQueryChip) {
            POINT sp{ static_cast<LONG>(std::lround(hit.control_bounds.left)),
                      static_cast<LONG>(std::lround(hit.control_bounds.bottom)) };
            ClientToScreen(hwnd, &sp);
            HandleTagQueryChip(*s, hit.index, sp);
            return 0;
        }
        if (hit.region == ui::HitTestResult::TagQueryChipRemove) {
            HandleTagQueryChipRemove(*s, hit.index);
            return 0;
        }
        if (hit.region == ui::HitTestResult::TagQueryRefine) {
            HandleTagQueryRefine(*s, hit.index, (GetKeyState(VK_SHIFT) & 0x8000) != 0);
            return 0;
        }
        if (hit.region == ui::HitTestResult::StatusBarAi) {   // outside the chain below: C1061
            pulse::AiChipClicked(*s);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        if (hit.region == ui::HitTestResult::DetailsPreview) {
            s->dragPending = false;
            return 0;
        } else if (hit.region == ui::HitTestResult::ColumnDivider) {
            if (s->renameIndex >= 0) HideRenameOverlay(*s, true);
            s->dragPending = false;
            s->columnResizing = true;
            if (auto* resizePane = PaneAtSlot(*s, hit.pane_index)) {
                if (auto* tab = resizePane->ActiveTab(); tab && hit.pane_index >= 0 &&
                    hit.pane_index < static_cast<int>(vm.pane_slots.size())) {
                    const auto& slot = vm.pane_slots[static_cast<size_t>(hit.pane_index)];
                    const auto columns = s->renderer.DetailsColumns(
                        s->renderer.PaneBodyBounds(slot.pane, slot.rect), slot.pane);
                    using K = ui::MainRenderer::ColumnKind;
                    for (int i = 0; i < columns.count; ++i) {
                        const K kind = columns.kinds[static_cast<size_t>(i)];
                        const int stored = ui::MainRenderer::ManualColumnSlot(kind, slot.pane.is_search);
                        const float width = columns.widths[static_cast<size_t>(i)] / s->scale;
                        // The search name width is only stored next to a path column.
                        if (stored < 0 || (kind == K::Name && !columns.Has(K::Path))) continue;
                        if (slot.pane.is_search) tab->search_column_dividers[static_cast<size_t>(stored)] = width;
                        else tab->details_column_dividers[static_cast<size_t>(stored)] = width;
                    }
                }
            }
            s->columnResizeIndex = hit.index;
            s->columnResizePane = hit.pane_index;
            s->hoverRegion = static_cast<int>(ui::HitTestResult::ColumnDivider);
            s->hoverControlIndex = hit.index;
            s->hoverPaneIndex = hit.pane_index;
            SetCapture(hwnd);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        } else if (hit.region == ui::HitTestResult::Splitter) {
            s->dragPending = false;
            if (hit.index >= ui::kSplitterSwapIndex) {
                SwapSplitPanes(*s);
                return 0;
            }
            s->splitterDragging = true;
            s->splitterDragIndex = hit.index;
            s->splitterSnap = 0.0f;
            s->tooltipText.clear();   // motion 4b: the ratio badge takes over
            if (hit.index >= 0 && hit.index < static_cast<int>(vm.splitters.size())) {
                s->splitterParentBounds = vm.splitters[static_cast<size_t>(hit.index)].parent_bounds;
                s->splitterOrientation = vm.splitters[static_cast<size_t>(hit.index)].vertical
                    ? app::SplitOrientation::Vertical : app::SplitOrientation::Horizontal;
            }
            SetCapture(hwnd);
            InvalidateRect(hwnd, nullptr, FALSE);
        } else if (hit.region == ui::HitTestResult::Scrollbar) {
            s->dragPending = false;
            D2D1_RECT_F track{}, thumb{};
            float maxScroll = 0.0f;
            app::Tab* tab = ActiveTab(*s);
            s->scrollbarHorizontal = hit.sub_index == 1;
            s->scrollbarSidebar = hit.sub_index == 2;
            s->scrollbarSettings = hit.sub_index == 3;
            const auto& scrollPane = hit.pane_index >= 0 && hit.pane_index < static_cast<int>(vm.pane_slots.size())
                ? vm.pane_slots[static_cast<size_t>(hit.pane_index)].pane : vm.pane;
            const bool hasGeometry = s->scrollbarSettings
                ? s->renderer.SettingsScrollbarGeometry(vm, rect.right, rect.bottom, track, thumb, maxScroll)
                : s->scrollbarSidebar
                ? s->renderer.SidebarScrollbarGeometry(vm, rect.right, rect.bottom,
                                                       track, thumb, maxScroll)
                : s->scrollbarHorizontal
                ? HorizontalScrollbarGeometry(*s, scrollPane, track, thumb, maxScroll)
                : ScrollbarGeometry(*s, scrollPane, track, thumb, maxScroll);
            if ((tab || s->scrollbarSettings) && hasGeometry) {
                const bool outside = s->scrollbarHorizontal
                    ? (mx < thumb.left || mx >= thumb.right)
                    : (my < thumb.top || my >= thumb.bottom);
                if (outside) {
                    const float travel = std::max(1.0f,
                        s->scrollbarHorizontal
                            ? (track.right-track.left)-(thumb.right-thumb.left)
                            : (track.bottom-track.top)-(thumb.bottom-thumb.top));
                    const float pointer = s->scrollbarHorizontal ? mx-track.left : my-track.top;
                    const float thumbExtent = s->scrollbarHorizontal
                        ? thumb.right-thumb.left : thumb.bottom-thumb.top;
                    const float value = std::clamp((pointer-thumbExtent*0.5f)*maxScroll/travel,
                                                   0.0f,maxScroll);
                    if (s->scrollbarSettings) s->settings.SetScroll(value, maxScroll);
                    else if (s->scrollbarSidebar) s->sidebarScroll=value;
                    else if (s->scrollbarHorizontal) tab->scroll_x=value;
                    else { tab->scroll_y=value; s->scrollTargetY=value; MaybePrefetchSearchPage(*s); }
                }
                s->scrollbarGrabOffset = outside ? (thumb.bottom - thumb.top) * 0.5f : my - thumb.top;
                s->scrollbarDragging = true;
                s->scrollbarDragStartX = mx;
                s->scrollbarDragStartY = my;
                s->scrollbarDragStartScroll = s->scrollbarSettings ? s->settings.scroll()
                    : s->scrollbarSidebar ? s->sidebarScroll
                    : s->scrollbarHorizontal ? tab->scroll_x : tab->scroll_y;
                SetCapture(hwnd);
                InvalidateRect(hwnd, nullptr, FALSE);
            } else {
                s->scrollbarSettings = false;
            }
        } else if (hit.region == ui::HitTestResult::Row && hit.index >= 0) {
            // Details tree: chevrons (Alt: with subfolders), "more" buttons, placeholders.
            if (HandleDetailsTreeMouseDown(*s, vm, hit, mx, my, (GetKeyState(VK_MENU) & 0x8000) != 0)) {
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
            app::Tab* tab = ActiveTab(*s);
            // File Explorer: in Details / List only the icon and the name drag
            // the item; the rest of the row starts a marquee (a click there
            // still selects the row, on release).
            if (tab && !IsSettingsTab(tab) && !PointInHitItemGrip(*s, vm, hit, static_cast<float>(mx), static_cast<float>(my))) {
                RememberSelectionForMarquee(*s, tab);
                s->marqueePending = true;
                s->marqueeActive = false;
                s->marqueeAdditive = ctrl || shift;
                s->marqueeToggle = ctrl;
                s->marqueeRowIndex = hit.index;
                s->marqueeRowCtrl = ctrl;
                s->marqueeRowShift = shift;
                ArmSingleOpen(*s, vm, hit, tab, ctrl, shift, pressActivates);
                s->blankClickTab = nullptr;
                s->blankClickPane = s->pane;
                s->blankClickGeneration = tab->view_generation;
                s->marqueeStart = s->marqueeCur = POINT{ mx, my };
                s->marqueeOriginScrollX = tab->scroll_x;
                s->marqueeOriginScrollY = tab->scroll_y;
                s->marqueeBase.clear();
                if (ctrl || shift) {
                    tab->MaterializeSelection();
                    s->marqueeBase = tab->selected;
                }
                SetCapture(hwnd);
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            // Single click opens: no slow-click rename (F2 still renames).
            const bool renameCandidate = !s->appPrefs.single_click_open && !ctrl && !shift && tab && !tab->net_readonly &&
                s->renameIndex < 0 && tab->SelectedCount() == 1 &&
                tab->selected_index == hit.index && tab->IsSelected(hit.index) &&
                PointInHitItemName(*s, vm, hit, static_cast<float>(mx), static_cast<float>(my));
            HandleListRowClick(*s, hit.index, ctrl, shift);
            ArmSingleOpen(*s, vm, hit, tab, ctrl, shift, pressActivates);
            if (renameCandidate && tab == ActiveTab(*s)) {
                s->renameClickCandidate = true;
                s->renameClickStarted = GetTickCount64();
                s->renameClickPane = s->pane;
                s->renameClickTab = tab;
                s->renameClickIndex = hit.index;
                s->renameClickPath = EntryFullPath(*tab, hit.index);
            }
            // Potential drag-out start; resolved by movement in WM_MOUSEMOVE.
            s->dragPending = true;
            s->dragStartPt = POINT{ mx, my };
            SetCapture(hwnd);
            InvalidateRect(hwnd, nullptr, FALSE);
        } else if (hit.region == ui::HitTestResult::TabGroup) {
            // Group chip (Chromium behavior): press arms a whole-group drag;
            // a plain release toggles collapse; right-click opens the editor.
            if (hit.index >= 0 && hit.index < static_cast<int>(vm.tab_groups.size()) &&
                s->pane) {
                const int gid = vm.tab_groups[static_cast<size_t>(hit.index)].id;
                int first = -1;
                for (int i = 0; i < static_cast<int>(s->window_tabs.items.size()); ++i)
                    if (s->window_tabs.items[static_cast<size_t>(i)]->tab_group == gid) {
                        first = i;
                        break;
                    }
                if (first >= 0) {
                    s->tabDragPending = true;
                    s->tabDragging = false;
                    s->tabDragFromChip = true;
                    s->tabDragGroupId = gid;
                    s->tabDragIndex = first;
                    s->tabDragStartPt = POINT{ mx, my };
                    SetCapture(hwnd);
                }
            }
        } else if (hit.region == ui::HitTestResult::Tab && hit.index >= 0) {
            pulse::ArmTabDoubleClick(*s, static_cast<size_t>(hit.index));
            SwitchTab(*s, hit.index);
            s->tabDragPending = true;
            s->tabDragging = false;
            s->tabDragFromChip = false;
            s->tabDragIndex = hit.index;
            s->tabDragStartPt = POINT{ mx, my };
            SetCapture(hwnd);
        } else if (hit.region == ui::HitTestResult::TabClose && hit.index >= 0) {
            s->renderer.FreezeTabWidths(BuildVm(*s, false), static_cast<float>(s->compositor.Width()));
            CloseLayoutTab(*s, static_cast<size_t>(hit.index));
            InvalidateRect(hwnd, nullptr, FALSE);
        } else if (hit.region == ui::HitTestResult::TabNew || hit.region == ui::HitTestResult::TabScroll) {
            // Shares a branch: this else-if chain is at the compiler's nesting limit.
            if (hit.region == ui::HitTestResult::TabNew) {
                OpenNewTab(*s);
            } else {
                s->renderer.ScrollTabStripPage(BuildVm(*s, false), static_cast<float>(s->compositor.Width()), hit.index);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
        } else if (hit.region == ui::HitTestResult::ThemeToggle) {
            ToggleTheme(*s);
        } else if (hit.region == ui::HitTestResult::SettingsButton) {
            // The button is settings' tab: it opens where you left off and
            // closes again on a second click.
            if (IsSettingsTab(ActiveTab(*s))) CloseSettings(*s);
            else OpenSettingsTab(*s, s->settings.page());
        } else if (hit.region == ui::HitTestResult::SettingsNav ||
                   hit.region == ui::HitTestResult::SettingsCtxPivot) {
            OpenSettingsTab(*s, hit.index);
        } else if (HandleSettingsControl(*s, hit)) {
        } else if (hit.region == ui::HitTestResult::SettingsToggle) {
            // Search master switch: asks before turning off, so it is handled here.
            if (hit.index == 38) ToggleSearchSwitch(*s);
            else if (hit.index == 39) {
                // 别名旁显示小图标: kept with the aliases (aliases.json), not app prefs.
                auto& aliases = app::AliasStore::Instance();
                aliases.SetShowMark(!aliases.ShowMark());
            } else ToggleSetting(*s, hit.index);
            InvalidateRect(hwnd, nullptr, FALSE);
        } else if (hit.region == ui::HitTestResult::SettingsAccent) {
            s->bloom_accent.SetPressed(hit.index);
            SetCapture(hwnd);
            InvalidateRect(hwnd, nullptr, FALSE);
        } else if (hit.region == ui::HitTestResult::SettingsEffect) {
            if (hit.index >= 0 && hit.index < ui::kWindowEffectCount)
                s->settings.WindowEffect(
                    ui::WindowEffectId(static_cast<ui::WindowEffect>(hit.index)));
            InvalidateRect(hwnd, nullptr, FALSE);
        } else if (hit.region == ui::HitTestResult::SettingsDensity) {
            s->settings.RowHeight(hit.index);
            InvalidateRect(hwnd, nullptr, FALSE);
        } else if (hit.region == ui::HitTestResult::SettingsTextRender) {
            s->settings.TextRendering(hit.index);
            InvalidateRect(hwnd, nullptr, FALSE);
        } else if (hit.region == ui::HitTestResult::SettingsFolderSort ||
                   hit.region == ui::HitTestResult::SettingsMtimeStyle ||
                   hit.region == ui::HitTestResult::SettingsMtimeRange ||
                   hit.region == ui::HitTestResult::SettingsMtimePalette ||
                   hit.region == ui::HitTestResult::SettingsClickMode ||
                   hit.region == ui::HitTestResult::SettingsNameBand ||
                   hit.region == ui::HitTestResult::SettingsUiFontSize ||
                   hit.region == ui::HitTestResult::SettingsStartupOpen ||
                   hit.region == ui::HitTestResult::SettingsNewTabOpen ||
                   hit.region == ui::HitTestResult::SettingsExternalOpen ||
                   hit.region == ui::HitTestResult::SettingsNotifyIcon ||
                   hit.region == ui::HitTestResult::SettingsBlankClick ||
                   hit.region == ui::HitTestResult::SettingsHomeFolder ||
                   hit.region == ui::HitTestResult::SettingsLanCache ||
                   hit.region == ui::HitTestResult::SettingsThumbCache) {
            // One branch on purpose: this else-if chain sits at MSVC's block
            // nesting limit (C1061), so new settings must not lengthen it.
            switch (hit.region) {
            case ui::HitTestResult::SettingsUiFontSize: s->settings.UiFontSize(hit.index); break;
            case ui::HitTestResult::SettingsStartupOpen: s->settings.StartupOpen(hit.index); break;
            case ui::HitTestResult::SettingsNewTabOpen: s->settings.NewTabOpen(hit.index); break;
            case ui::HitTestResult::SettingsExternalOpen: s->settings.ExternalOpen(hit.index); break;
            case ui::HitTestResult::SettingsNotifyIcon: s->settings.NotifyIcon(hit.index); break;
            case ui::HitTestResult::SettingsBlankClick: s->settings.BlankClick(hit.index); break;
            case ui::HitTestResult::SettingsHomeFolder: s->settings.HomeFolder(hit.index); break;
            case ui::HitTestResult::SettingsLanCache: SetLanCacheMode(*s, hit.index); break;
            case ui::HitTestResult::SettingsThumbCache: s->settings.ClearThumbCache(); break;
            case ui::HitTestResult::SettingsMtimeStyle: s->settings.MtimeStyle(hit.index); break;
            case ui::HitTestResult::SettingsMtimeRange: s->settings.MtimeRange(hit.index); break;
            case ui::HitTestResult::SettingsMtimePalette: s->settings.MtimePalette(hit.index); break;
            case ui::HitTestResult::SettingsClickMode: s->settings.ClickMode(hit.index); break;
            case ui::HitTestResult::SettingsNameBand: s->settings.ClickMode(2 + hit.index); break;
            default: s->settings.FolderSort(hit.index); break;
            }
            InvalidateRect(hwnd, nullptr, FALSE);
        } else if (hit.region == ui::HitTestResult::SettingsTrayIcon || hit.region == ui::HitTestResult::SettingsDoubleCtrlGap) {
            if (hit.region == ui::HitTestResult::SettingsTrayIcon) s->settings.TrayIconSize(hit.index);
            else s->settings.DoubleCtrlGap(hit.index);
            InvalidateRect(hwnd, nullptr, FALSE);
        } else if (hit.region == ui::HitTestResult::SettingsWallpaperLook ||
                   hit.region == ui::HitTestResult::SettingsWallpaperBlur ||
                   hit.region == ui::HitTestResult::SettingsFrameLook ||
                   hit.region == ui::HitTestResult::SettingsAccentSlider) {
            const int which = hit.region == ui::HitTestResult::SettingsWallpaperLook ? 0
                : hit.region == ui::HitTestResult::SettingsWallpaperBlur ? 1
                : hit.region == ui::HitTestResult::SettingsFrameLook ? 5 : 2 + std::clamp(hit.sub_index, 0, 2);
            s->settings.BeginSlider(which);
            s->settings.SliderValue(which, hit.index);
            SetCapture(hwnd);
            InvalidateRect(hwnd, nullptr, FALSE);
        } else if (hit.region == ui::HitTestResult::SettingsLanguage) {
            static constexpr const wchar_t* languages[] = {L"system", L"zh-CN", L"zh-TW", L"en-US"};
            if (hit.index >= 0 && hit.index < static_cast<int>(std::size(languages)))
                s->settings.Language(languages[hit.index]);
        } else if (hit.region == ui::HitTestResult::SettingsWallpaper) {
            s->settings.Wallpaper(hit.index);
            InvalidateRect(hwnd, nullptr, FALSE);
        } else if (hit.region == ui::HitTestResult::SettingsIndexVolume) {
            s->settings.ToggleVolume(hit.index);
            InvalidateRect(hwnd, nullptr, FALSE);
        } else if (hit.region == ui::HitTestResult::SettingsIndexAction) {
            s->settings.IndexAction(hit.index);
            InvalidateRect(hwnd, nullptr, FALSE);
        } else if (hit.region == ui::HitTestResult::SettingsIndexExcludeAction) {
            if (hit.index == 0) s->settings.AddExclude();
            else s->settings.SystemExclusionAction(hit.index);
            InvalidateRect(hwnd, nullptr, FALSE);
        } else if (hit.region == ui::HitTestResult::SettingsIndexExcludeRemove) {
            s->settings.RemoveExclude(hit.index);
            InvalidateRect(hwnd, nullptr, FALSE);
        } else if (hit.region == ui::HitTestResult::SettingsNetworkAction) {
            s->settings.NetworkAction(hit.index);
            InvalidateRect(hwnd, nullptr, FALSE);
        } else if (hit.region == ui::HitTestResult::SettingsNetworkRemove) {
            s->settings.RemoveNetwork(hit.index);
            InvalidateRect(hwnd, nullptr, FALSE);
        } else if (hit.region == ui::HitTestResult::SettingsRestore) {
            s->ctxMenuPrefs.ResetToDefaults();
            s->ctxMenuPrefs.Save();
            s->renderer.SetRowActions(app::RowActionMask(s->ctxMenuPrefs.builtin_hidden));
            InvalidateRect(hwnd, nullptr, FALSE);
        } else if (hit.region == ui::HitTestResult::SettingsDiagnosticsAction) {
            s->settings.DiagnosticsAction(hit.index);
            InvalidateRect(hwnd, nullptr, FALSE);
        } else if (hit.region == ui::HitTestResult::SettingsUpdateAction) {
            if (hit.index == 0) CheckForUpdates(*s);
            else if (hit.index == 1) InstallUpdate(*s);
        } else if (hit.region == ui::HitTestResult::SettingsDupScope) {
            if (!s->duplicateScan.scanning && hit.index >= 0 && hit.index <= 2) {
                s->duplicateScan.scope = static_cast<app::DuplicateScanScope>(hit.index);
                s->duplicateScan.minimum_file_bytes =
                    app::DuplicateScanSession::DefaultMinimumBytes(s->duplicateScan.scope);
                PersistDuplicateScanPrefs(*s);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
        } else if (hit.region == ui::HitTestResult::SettingsDupDrive) {
            if (!s->duplicateScan.scanning && hit.index >= 0) {
                int n = 0;
                for (const auto& volume : s->dup_volume_cache) {
                    if (volume.mount_point.empty()) continue;
                    if (n == hit.index) {
                        s->duplicateScan.drive_root =
                            app::DuplicateScanSession::NormalizeDriveRoot(volume.mount_point);
                        PersistDuplicateScanPrefs(*s);
                        InvalidateRect(hwnd, nullptr, FALSE);
                        break;
                    }
                    ++n;
                }
            }
        } else if (hit.region == ui::HitTestResult::SettingsDupBrowse) {
            if (!s->duplicateScan.scanning) {
                std::wstring path;
                if (PickFolder(*s, path, l10n::Get(l10n::StringId::DupFolderPlaceholder).c_str(), L"duplicates")) {
                    s->duplicateScan.folder_path = std::move(path);
                    PersistDuplicateScanPrefs(*s);
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
            }
        } else if (hit.region == ui::HitTestResult::SettingsDupMinSize) {
            static constexpr uint64_t kBytes[] = {1024ull, 1024ull * 1024ull, 10ull * 1024ull * 1024ull};
            if (!s->duplicateScan.scanning && hit.index >= 0 && hit.index < 3) {
                s->duplicateScan.minimum_file_bytes = kBytes[hit.index];
                InvalidateRect(hwnd, nullptr, FALSE);
            }
        } else if (hit.region == ui::HitTestResult::SettingsDupScan) {
            StartDuplicateScan(*s);
        } else if (hit.region == ui::HitTestResult::SettingsDupCancel) {
            CancelDuplicateScan(*s);
        } else if (hit.region == ui::HitTestResult::SettingsDupKeep) {
            if (!s->duplicateScan.scanning)
                s->duplicateScan.SetKeep(static_cast<size_t>(hit.index),
                                         static_cast<size_t>(hit.sub_index));
            InvalidateRect(hwnd, nullptr, FALSE);
        } else if (hit.region == ui::HitTestResult::SettingsDupOpen) {
            OpenDuplicateLocation(*s, static_cast<size_t>(hit.index),
                                  static_cast<size_t>(hit.sub_index));
        } else if (hit.region == ui::HitTestResult::SettingsDupGroupDelete) {
            RecycleDuplicateGroup(*s, static_cast<size_t>(hit.index));
        } else if (hit.region == ui::HitTestResult::SettingsDupDeleteAll) {
            RecycleAllDuplicateExtras(*s);
        } else if (hit.region == ui::HitTestResult::NavBack) {
            GoBack(*s);
        } else if (hit.region == ui::HitTestResult::NavForward) {
            GoForward(*s);
        } else if (hit.region == ui::HitTestResult::NavUp) {
            GoUp(*s);
        } else if (hit.region == ui::HitTestResult::NavRefresh) {
            RefreshActiveTab(*s);
        } else if (hit.region == ui::HitTestResult::NewButton ||
                   hit.region == ui::HitTestResult::NewButtonArrow) {
            // Split New button: main part creates a folder (inline rename), chevron opens the menu.
            // One branch on purpose: this else-if chain is at MSVC's nesting limit (C1061).
            PressNewButton(*s, hit.region == ui::HitTestResult::NewButtonArrow);
        } else if (hit.region == ui::HitTestResult::BreadcrumbSegment) {
            // The trailing search segment edits the query instead of reloading it.
            const app::Tab* active = ActiveTab(*s);
            if (active && hit.path == active->current_path && IsAddressSearchResults(active)) ShowAddressSearch(*s);
            // Empty path is the This PC segment; NavigateTo handles it.
            else NavigateTo(*s, hit.path);
        } else if (hit.region == ui::HitTestResult::Copy ||
                   hit.region == ui::HitTestResult::ToolbarSelAction ||
                   hit.region == ui::HitTestResult::ToolbarPin) {
            // One branch on purpose: this else-if chain is at MSVC's nesting limit (C1061).
            if (hit.region == ui::HitTestResult::Copy) CollectToTray(*s, false);
            else RunToolbarAction(*s, hit.region == ui::HitTestResult::ToolbarPin, hit.index);
        } else if (hit.region == ui::HitTestResult::Cut) {
            CollectToTray(*s, true);
        } else if (hit.region == ui::HitTestResult::Paste) {
            PasteIntoCurrent(*s);
        } else if (hit.region == ui::HitTestResult::Rename) {
            RenameSelected(*s);
        } else if (hit.region == ui::HitTestResult::Delete) {
            DeleteSelected(*s, (GetKeyState(VK_SHIFT) & 0x8000) != 0);
        } else if (hit.region == ui::HitTestResult::GroupHeader ||
                   hit.region == ui::HitTestResult::GroupSelect) {
            ClickGroupHeader(*s, hit.pane_index, hit.index,
                             hit.region == ui::HitTestResult::GroupSelect);
        } else if (hit.region == ui::HitTestResult::TrayCompare) {
            if (hit.index == 0) TrayToggleCompare(*s);
            else if (hit.index == 2) TrayOpenTextDiff(*s);
            else TrayStartContentCompare(*s);
        } else if (hit.region == ui::HitTestResult::TrayStale) {
            if (hit.index == 0) TrayFindStale(*s);
            else TrayRemoveStale(*s);
        } else if (hit.region == ui::HitTestResult::TrayDest) {
            // Recent destination chip: copy the whole tray there; Shift moves.
            SendTrayToDest(*s, hit.path, (GetKeyState(VK_SHIFT) & 0x8000) != 0);
        } else if (hit.region == ui::HitTestResult::TrayRelease) {
            ReleaseTrayBatch(*s, (size_t)hit.index);
        } else if (hit.region == ui::HitTestResult::CompareDiffToggle) {
            ToggleCompareDiffOnly(*s);
        } else if (hit.region == ui::HitTestResult::CompareExit) {
            SetFolderCompare(*s, false);
        } else if (hit.region == ui::HitTestResult::TrayIntent) {
            const auto& batches = s->tray.batches();
            if (hit.index >= 0 && hit.index < static_cast<int>(batches.size())) {
                const bool to_move = !batches[(size_t)hit.index].move_intent;
                s->tray.SetMoveIntent((size_t)hit.index, to_move);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
        } else if (hit.region == ui::HitTestResult::TrayClose) {
            s->tray.RemoveBatch((size_t)hit.index);
            InvalidateRect(hwnd, nullptr, FALSE);
        } else if (hit.region == ui::HitTestResult::TrayItemRemove) {
            // Dismiss: the card tumbles off in a puff of smoke.
            const auto& batches = s->tray.batches();
            if (hit.index >= 0 && hit.index < static_cast<int>(batches.size()) &&
                hit.sub_index >= 0 &&
                hit.sub_index < static_cast<int>(batches[(size_t)hit.index].items.size())) {
                MarkTrayExit(*s, { batches[(size_t)hit.index].items[(size_t)hit.sub_index].path },
                             false);
                SpawnTrayPuffs(*s);
            }
            s->tray.RemoveItem((size_t)hit.index, (size_t)hit.sub_index);
            InvalidateRect(hwnd, nullptr, FALSE);
        } else if (hit.region == ui::HitTestResult::TrayClear) {
            // Clear: the visible cards tumble off one after another.
            std::vector<std::wstring> all;
            for (const auto& b : s->tray.batches())
                for (const auto& item : b.items) all.push_back(item.path);
            if (!all.empty()) {
                MarkTrayExit(*s, all, true);
                SpawnTrayPuffs(*s);
            }
            s->tray.Clear();
            InvalidateRect(hwnd, nullptr, FALSE);
        } else if (hit.region == ui::HitTestResult::RowFolderSize && hit.index >= 0) {
            if (app::Tab* tab = ActiveTab(*s)) {
                // The label is part of the item (tiles, icons): the press
                // selects it too, and a second press opens it (DblClk).
                const bool modifier = (GetKeyState(VK_CONTROL) & 0x8000) != 0 || (GetKeyState(VK_SHIFT) & 0x8000) != 0;
                if (!modifier && !tab->IsSelected(hit.index)) tab->SelectOnly(hit.index);
                const auto path = EntryFullPath(*tab, hit.index);
                const auto work = s->folderSizes.GetWork(path);
                s->folderSizeClickPath.clear();
                if (work.manual && work.Running()) s->folderSizes.Cancel(path);
                else {
                    s->folderSizes.Calculate(path);
                    s->folderSizeClickPath = path;
                    s->folderSizeClickTime = GetMessageTime();
                }
                s->tooltipText.clear();
                InvalidateRect(hwnd, nullptr, FALSE);
            }
        } else if (hit.region == ui::HitTestResult::RowStar && hit.index >= 0) {
            if (app::Tab* tab = ActiveTab(*s)) {
                const std::wstring p = EntryFullPath(*tab, hit.index);
                if (!p.empty() && tab->snapshot &&
                    hit.index < static_cast<int>(tab->EntryCount())) {
                    const auto& entry = tab->EntryAt(static_cast<size_t>(hit.index));
                    ToggleStarred(*s, p, entry.is_dir
                        ? app::PlaceItemKind::Folder : app::PlaceItemKind::File);
                }
            }
        } else if (hit.region == ui::HitTestResult::RowNewTab && hit.index >= 0) {
            if (app::Tab* tab = ActiveTab(*s)) {
                const std::wstring p = EntryFullPath(*tab, hit.index);
                if (!p.empty()) OpenFolderTab(*s, p);
            }
        } else if (hit.region == ui::HitTestResult::RowMore && hit.index >= 0) {
            if (app::Tab* tab = ActiveTab(*s)) {
                if (!tab->IsSelected(hit.index)) tab->SelectOnly(hit.index);
                POINT point{ mx, my };
                ClientToScreen(hwnd, &point);
                ShowItemContextMenu(*s, point);
            }
        } else if (hit.region == ui::HitTestResult::TeachPrimary) {
            HandleTeachButton(*s, 0);
        } else if (hit.region == ui::HitTestResult::TeachDismiss) {
            HandleTeachButton(*s, 1);
        } else if (hit.region == ui::HitTestResult::TeachNever) {
            HandleTeachButton(*s, 2);
        } else if (hit.region == ui::HitTestResult::TeachBubble) {
        } else if (hit.region == ui::HitTestResult::FilterEmptyAction) {
            if (hit.index == 0) SearchFilterInSubfolders(*s);
            else ClearPaneFilter(*s);
            InvalidateRect(hwnd, nullptr, FALSE);
        } else if (hit.region == ui::HitTestResult::SearchEmptyAction) {
            if (!hit.path.empty()) NavigateTo(*s, app::MakeSearchPath(hit.path));
        } else if (hit.region == ui::HitTestResult::PaneEmptyNewFolder) {
            CreateNewItem(*s, true);
        } else if (hit.region == ui::HitTestResult::DetailsOpen) {
            OpenSelected(*s);
        } else if (hit.region == ui::HitTestResult::DetailsStar) {
            const std::wstring p = SelectedFullPath(*s);
            if (!p.empty()) ToggleStarred(*s, p, vm.details.is_dir
                ? app::PlaceItemKind::Folder : app::PlaceItemKind::File);
        } else if (hit.region == ui::HitTestResult::DetailsMore) {
            if (EnsureMenu(*s)) {
                std::vector<ui::FluentMenuItem> items;
                ui::FluentMenuItem terminal;
                terminal.command = app::CmdOpenTerminal;
                terminal.text = l10n::Get(l10n::StringId::OpenInTerminal);
                terminal.glyph = L"\xE756";
                items.push_back(std::move(terminal));
                if (vm.details.is_dir) {
                    ui::FluentMenuItem size;
                    size.command = app::CmdDetailsComputeSize;
                    size.text = l10n::Get(l10n::StringId::ComputeSize);
                    size.glyph = L"\xE8EF";
                    items.push_back(std::move(size));
                }
                ui::FluentMenuItem props;
                props.command = app::CmdProperties;
                props.text = l10n::Get(l10n::StringId::Properties);
                props.glyph = L"\xE946";
                props.separator_after = true;
                items.push_back(std::move(props));
                ui::FluentMenuItem shell;
                shell.command = app::CmdDetailsShellMenu;
                shell.text = l10n::Get(l10n::StringId::SystemMenu);
                shell.glyph = L"\xE712";
                items.push_back(std::move(shell));
                POINT point{ mx, my };
                ClientToScreen(hwnd, &point);
                const int cmd = s->menu->TrackPopup(point, std::move(items));
                if (cmd == app::CmdOpenTerminal) {
                    std::wstring p = vm.details.path;
                    if (!p.empty() && !vm.details.is_dir) p = fs::ParentPath(p);
                    if (!p.empty()) s->ops.OpenTerminal(ClipboardPath(p), s->appPrefs.terminal_program);
                } else if (cmd == app::CmdDetailsComputeSize) {
                    if (!vm.details.path.empty()) StartDetailsSizeWalk(*s, vm.details.path);
                    InvalidateRect(hwnd, nullptr, FALSE);
                } else if (cmd == app::CmdProperties) {
                    DispatchMenuCommand(*s, app::CmdProperties);
                } else if (cmd == app::CmdDetailsShellMenu) {
                    ShowItemContextMenu(*s, point);
                }
            }
        } else if (hit.region == ui::HitTestResult::DetailsNewTab) {
            if (!vm.details.path.empty()) {
                const std::wstring target = vm.details.path;
                if (vm.details.is_dir) {
                    OpenFolderTab(*s, target);
                } else {
                    OpenFolderTab(*s, fs::ParentPath(target));
                    if (app::Tab* tab = ActiveTab(*s)) {
                        std::wstring leaf = target;
                        if (leaf.starts_with(L"\\\\?\\UNC\\")) leaf = L"\\\\" + leaf.substr(8);
                        else if (leaf.starts_with(L"\\\\?\\")) leaf = leaf.substr(4);
                        const auto slash = leaf.find_last_of(L"\\/");
                        if (slash != std::wstring::npos) leaf = leaf.substr(slash + 1);
                        SelectNameInTab(*s, *tab, leaf);
                    }
                }
            }
        } else if (hit.region == ui::HitTestResult::DetailsCopyPath) {
            if (!vm.details.path.empty()) {
                ops::WriteClipboardText(ClipboardPath(vm.details.path));
                s->renderer.NotifyCopied(static_cast<int>(ui::HitTestResult::DetailsCopyPath), 0);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
        } else if (hit.region == ui::HitTestResult::DetailsSection) {
            if (hit.index >= 0 && hit.index < 32) {
                s->detailsCollapsedMask ^= (1u << hit.index);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
        } else if (hit.region == ui::HitTestResult::DetailsAttrToggle) {
            if (hit.index == 2) {
                DispatchMenuCommand(*s, app::CmdProperties);
            } else if (!vm.details.path.empty() &&
                       (hit.index == 0 || hit.index == 1)) {
                const DWORD flag = hit.index == 0 ? FILE_ATTRIBUTE_READONLY
                                                  : FILE_ATTRIBUTE_HIDDEN;
                DWORD attrs = GetFileAttributesW(vm.details.path.c_str());
                if (attrs != INVALID_FILE_ATTRIBUTES) {
                    attrs = (attrs & flag) ? (attrs & ~flag) : (attrs | flag);
                    if (SetFileAttributesW(vm.details.path.c_str(), attrs))
                        RefreshActiveTab(*s, RefreshReason::FileChange);
                }
            }
        } else if (hit.region == ui::HitTestResult::DetailsSecurityChange) {
            DispatchMenuCommand(*s, app::CmdProperties);
        } else if (hit.region == ui::HitTestResult::DetailsRename) {
            RenameSelected(*s);
        } else if (hit.region == ui::HitTestResult::DetailsTagAdd) {
            POINT point{ mx, my };
            ClientToScreen(hwnd, &point);
            ShowTagPicker(*s, point);
        } else if (hit.region == ui::HitTestResult::DetailsPresetTag) {
            if (hit.index >= 0 && hit.index < static_cast<int>(s->places.tags.size()) &&
                !vm.details.path.empty()) {
                const bool assigned = s->places.PathHasTag(vm.details.path, hit.index);
                std::vector<app::TagAdsUpdate> ads_updates;
                s->places.SetTaggedBatch(hit.index, { vm.details.path }, !assigned,
                                         &ads_updates);
                QueueTagAds(*s, std::move(ads_updates));
                InvalidateRect(hwnd, nullptr, FALSE);
            }
        } else if (hit.region == ui::HitTestResult::SplitButton) {
            ShowSplitDropdown(*s);
        } else if (hit.region == ui::HitTestResult::DetailsToggle) {
            s->showDetailsPanel = !s->showDetailsPanel;
            s->renderer.SetDetailsPanelVisible(s->showDetailsPanel, true);
            InvalidateRect(hwnd, nullptr, FALSE);
        } else if (hit.region == ui::HitTestResult::PaneMediumIcons ||
                   hit.region == ui::HitTestResult::PaneWall ||
                   hit.region == ui::HitTestResult::PaneSizeSlider ||
                   hit.region == ui::HitTestResult::PaneSizeButton) {
            // One branch (this chain is at MSVC's nesting limit, see Sort/Group).
            // Icons / photo wall buttons: the size used last in this tab (Medium
            // at first), as a grid or as a photo wall.
            if (hit.region == ui::HitTestResult::PaneMediumIcons || hit.region == ui::HitTestResult::PaneWall)
                ShowIconWall(*s, hit.region == ui::HitTestResult::PaneWall);
            else PressPaneSize(*s, hwnd, hit, mx);
        } else if (hit.region == ui::HitTestResult::PaneDetails) {
            SetViewMode(*s, ui::ViewMode::Details);
        } else if (hit.region == ui::HitTestResult::ToolbarSort ||
                   hit.region == ui::HitTestResult::ToolbarGroup ||
                   hit.region == ui::HitTestResult::ToolbarGroupClear) {
            // One branch for the Sort/Group pair: this else-if chain sits at
            // MSVC's nesting limit (C1061).
            if (hit.region == ui::HitTestResult::ToolbarSort) ShowSortDropdown(*s);
            else if (hit.region == ui::HitTestResult::ToolbarGroup) ShowGroupDropdown(*s);
            else SetGroupBy(*s, 0);
        } else if (hit.region == ui::HitTestResult::ToolbarMore) {
            ShowToolbarMore(*s);
        } else if (hit.region == ui::HitTestResult::PaneViewButton) {
            ShowViewDropdown(*s, hit.pane_index);
        } else if (hit.region == ui::HitTestResult::FilterBox) {
            ShowFilterEditor(*s);
        } else if (hit.region == ui::HitTestResult::FilterClear) {
            ClearPaneFilter(*s);
        } else if (hit.region == ui::HitTestResult::AddressSearch) {
            ShowAddressSearch(*s);
        } else if (hit.region == ui::HitTestResult::AddressSearchScope) {
            if (!s->addressSearching) ShowAddressSearch(*s);
            ShowAddressSearchScope(*s);
        } else if (hit.region == ui::HitTestResult::AddressSearchMode ||
                   hit.region == ui::HitTestResult::AddressSearchContent) {
            // The mode chip toggles; the dedicated content target (if any) forces content.
            if (!s->addressSearching) ShowAddressSearch(*s);
            SwitchAddressSearchMode(*s, hit.region == ui::HitTestResult::AddressSearchContent ||
                                        !s->addressSearchContent);
        } else if (hit.region == ui::HitTestResult::AddressSearchOptions) {
            if (!s->addressSearching) ShowAddressSearch(*s);
            ShowSearchOptions(*s);
        } else if (hit.region == ui::HitTestResult::ContentIndexManage) {
            ShowSearchOptions(*s, true);
        } else if (hit.region == ui::HitTestResult::NetworkIndexAdd) {
            app::Pane* pane = PaneAtSlot(*s, hit.index);
            if (app::Tab* tab = pane ? pane->ActiveTab() : nullptr) AddLiveNetworkRoot(*s, *tab);
        } else if (hit.region == ui::HitTestResult::SettingsContentIndex) {
            ShowSearchOptions(*s);
        } else if (hit.region == ui::HitTestResult::AddressSearchClear) {
            if (!s->addressSearching) ShowAddressSearch(*s);
            SetWindowTextW(s->hwndAddressEdit, L"");
            SetForegroundWindow(GetAncestor(s->hwndAddressEdit, GA_ROOT));
            SetFocus(s->hwndAddressEdit);

        } else if (hit.region == ui::HitTestResult::AddressSearchClose) {
            ExitAddressSearch(*s);
        } else if (hit.region == ui::HitTestResult::AddressSearchInput) {
            // A results page shows the query without a live editor: open it at the click.
            if (!s->addressSearching) BeginSearchEditAt(*s, mx, my);
            else SetFocus(s->hwndAddressEdit);
        } else if (hit.region == ui::HitTestResult::AddressBar) {
            if (s->addressSearching) {
                SetForegroundWindow(GetAncestor(s->hwndAddressEdit, GA_ROOT));
                SetFocus(s->hwndAddressEdit);
            }
            else ShowOmnibar(*s, OmnibarMode::Path);
        } else if (hit.region == ui::HitTestResult::SearchFilter) {
            POINT corners[] = {
                {static_cast<LONG>(std::lround(hit.control_bounds.left)),
                 static_cast<LONG>(std::lround(hit.control_bounds.top))},
                {static_cast<LONG>(std::lround(hit.control_bounds.right)),
                 static_cast<LONG>(std::lround(hit.control_bounds.bottom))}};
            MapWindowPoints(hwnd, nullptr, corners, 2);
            ShowSearchFilterMenu(*s, hit.index,
                {corners[0].x, corners[0].y, corners[1].x, corners[1].y});
        } else if (hit.region == ui::HitTestResult::RecentFilter) {
            if (app::Tab* tab = ActiveTab(*s)) {
                const int filter = std::clamp(hit.index, 0, 2);
                if (tab->recent_filter != filter) {
                    tab->recent_filter = filter;
                    LoadVirtualView(*s, *tab, tab->current_path);
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
            }
        } else if (hit.region == ui::HitTestResult::RecentClear) {
            if (ConfirmClearRecent(*s) && s->places.ClearRecent()) {
                RefreshRecentViews(*s);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
        } else if (hit.region == ui::HitTestResult::ColumnHeader) {
            SortBy(*s, hit.column);
        } else if (HandleVerticalTabPress(*s, hit)) {
            // Tab rows, the tabs header and the sidebar toggle.
        } else if (hit.region == ui::HitTestResult::SidebarHeaderAction) {
            if (hit.sidebar_action == ui::SidebarAddAction::AddNetwork) {
                RunConnectServerDialog(*s);  // 连接服务器 (pulse_network_design.html §02)
            } else if (hit.sidebar_action == ui::SidebarAddAction::CreateTag) {
                POINT point{ mx, my };
                ClientToScreen(hwnd, &point);
                ShowCreateTagPicker(*s, point);
            } else if (hit.sidebar_action == ui::SidebarAddAction::AddQuickAccess) {
                POINT point{ mx, my };
                ClientToScreen(hwnd, &point);
                ShowQuickAccessAddMenu(*s, point);
            }
        } else if (hit.region == ui::HitTestResult::SidebarHeader) {
            // Arm the header drag; a release without movement still folds the
            // section, which keeps the header behaving like a plain toggle.
            if (hit.index >= 0 && hit.index < static_cast<int>(vm.sidebar.size())) {
                s->groupDragPending = true;
                s->groupDragActive = false;
                s->groupDragId = vm.sidebar[static_cast<size_t>(hit.index)].id;
                // #80: the This PC title opens This PC (the empty path) on a
                // plain click; the chevron and the rest of the header fold.
                s->groupDragNavigate = hit.sub_index == 1 &&
                    vm.sidebar[static_cast<size_t>(hit.index)].navigable;
                s->groupDragPath = s->groupDragNavigate
                    ? vm.sidebar[static_cast<size_t>(hit.index)].navigation_path : std::wstring{};
                s->groupDragStartPt = POINT{ mx, my };
                s->groupDragToIndex = -1;
                s->groupGapVisible = false;
                s->groupGapLineY = 0.0f;
                SetCapture(hwnd);
            }
        } else if (hit.region == ui::HitTestResult::SidebarItemAction) {
            std::wstring kind, rest;
            if (app::ParsePulsePath(hit.path, &kind, &rest) && kind == L"workspace")
                s->places.UnpinWorkspace(_wtoi(rest.c_str()));
            InvalidateRect(hwnd, nullptr, FALSE);
        } else if (hit.region == ui::HitTestResult::SidebarItemExpand) {
            if (hit.path == app::MakeStarredPath()) {
                s->starredExpanded = !s->starredExpanded;
                InvalidateRect(hwnd, nullptr, FALSE);
            } else if (app::IsTagGroupPath(hit.path)) {
                ToggleTagGroupRow(*s, hit.path);
            }
        } else if (hit.region == ui::HitTestResult::SidebarItem) {
            // A section whose rows are the section itself (the starred root, the
            // OneDrive accounts) has no header to grab: dragging a row moves the
            // whole section, and a plain click still navigates on release.
            const bool headerless_row =
                hit.sidebar_section == static_cast<int>(app::SidebarSectionId::Cloud) ||
                (hit.sidebar_section == static_cast<int>(app::SidebarSectionId::Starred) &&
                 hit.path == app::MakeStarredPath());
            const app::StarredItem* starred = s->places.FindStarred(hit.path);
            const bool in_starred =
                hit.sidebar_section == static_cast<int>(app::SidebarSectionId::Starred);
            if (headerless_row) {
                s->groupDragPending = true;
                s->groupDragActive = false;
                s->groupDragId = hit.sidebar_section;
                s->groupDragPath = hit.path;
                s->groupDragStartPt = POINT{ mx, my };
                s->groupDragToIndex = -1;
                s->groupGapVisible = false;
                s->groupGapLineY = 0.0f;
                SetCapture(hwnd);
            } else if (in_starred && starred && starred->kind == app::PlaceItemKind::Folder) {
                // A starred folder nested under the root row: reorder inside the
                // starred list (only when the row really is in that section).
                s->starDragPending = true;
                s->starDragActive = false;
                s->starDragStartPt = POINT{ mx, my };
                s->starDragPath = hit.path;
                const auto folders = s->places.StarredFolderPaths();
                const auto found = std::find_if(folders.begin(), folders.end(), [&](const auto& p) {
                    return _wcsicmp(p.c_str(), hit.path.c_str()) == 0;
                });
                s->starDragTarget = found == folders.end()
                    ? 0 : static_cast<size_t>(found - folders.begin());
                SetCapture(hwnd);
            } else if (std::wstring qa_row_key = QuickAccessRowKey(*s, vm, hit.sidebar_section,
                                                                   hit.sidebar_item);
                       !qa_row_key.empty()) {
                // Quick-access rows (built-in links and pins) reorder inside the
                // section; navigation waits for the release so a press can
                // become a drag.
                s->pinDragPending = true;
                s->pinDragActive = false;
                s->pinDragStartPt = POINT{ mx, my };
                s->pinDragPath = hit.path;
                s->pinDragKey = std::move(qa_row_key);
                s->pinDragRun = hit.index;
                s->pinDragToIndex = -1;
                s->pinGapVisible = false;
                s->pinGapLineY = 0.0f;
                SetCapture(hwnd);
            } else if (IsSidebarTagPath(hit.path)) {
                // Tags defer navigation to release too: a press may become the
                // same reorder drag as the quick-access rows above.
                s->pinDragPending = true;
                s->pinDragActive = false;
                s->pinDragStartPt = POINT{ mx, my };
                s->pinDragPath = hit.path;
                s->pinDragKey.clear();
                s->pinDragRun = hit.index;
                s->pinDragToIndex = -1;
                s->pinGapVisible = false;
                s->pinGapLineY = 0.0f;
                SetCapture(hwnd);
            } else if (hit.path.starts_with(L"pulse:workspace:")) {
                std::wstring kind, rest;
                app::ParsePulsePath(hit.path, &kind, &rest);
                OpenWorkspace(*s, _wtoi(rest.c_str()));
            } else if (!hit.path.empty()) {
                NavigateTo(*s, hit.path);
            }
        } else if (hit.region == ui::HitTestResult::StatusBarCancelSearch) {
            if (auto* tab = ActiveTab(*s)) CancelActiveContentSearch(*s, *tab);
            InvalidateRect(hwnd, nullptr, FALSE);
        } else if (hit.region == ui::HitTestResult::StatusHintAction) {
            // Re-derived at click time, so the action always matches the current state.
            switch (CurrentStatusHintAction(*s)) {
            case StatusHintAction::Tray: StageSelection(*s); break;
            case StatusHintAction::Compare: DispatchMenuCommand(*s, app::CmdCompareToggle); break;
            case StatusHintAction::DiffOnly:
            case StatusHintAction::ShowAll: DispatchMenuCommand(*s, app::CmdCompareDiffOnly); break;
            case StatusHintAction::Advanced: DispatchMenuCommand(*s, app::CmdAdvancedSearch); break;
            case StatusHintAction::Shortcuts: DispatchMenuCommand(*s, app::CmdShortcutHelp); break;
            case StatusHintAction::OpenPath: DispatchMenuCommand(*s, app::CmdOpenPath); break;
            case StatusHintAction::SearchSubfolders: SearchFilterInSubfolders(*s); break;
            case StatusHintAction::LanRescan: DispatchMenuCommand(*s, app::CmdLanRescan); break;
            default: break;
            }
            InvalidateRect(hwnd, nullptr, FALSE);
        } else if (hit.region == ui::HitTestResult::StatusBarTask) {
            const ops::OpStatus finished = s->ops.Status();
            if (hit.sub_index == 0) {
                // 撤销 on the finished pill: only the operation it names.
                if (s->ops.CanUndoTask(finished.task_id)) s->ops.Undo();
            } else if (hit.sub_index == 1) {
                if (!finished.destination_dir.empty()) NavigateTo(*s, finished.destination_dir);
            } else {
                PinAndShowOperationWindow(*s);
            }
            InvalidateRect(hwnd, nullptr, FALSE);
        } else if (hit.region == ui::HitTestResult::StatusBar) {
            // Left/right status text is not a transfer control.
        } else if (!IsSettingsTab(ActiveTab(*s)) &&
                   (hit.region == ui::HitTestResult::Pane ||
                   (PointInList(*s, mx, my) && hit.region == ui::HitTestResult::None))) {
            app::Tab* tab = ActiveTab(*s);
            const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
            // A selection-clearing first click still counts toward the
            // blank double click, so the second click navigates back.
            // Shift keeps the selection (the band adds), Ctrl inverts.
            RememberSelectionForMarquee(*s, tab);
            if (tab && !ctrl && !shift) tab->ClearSelection();
            s->marqueePending = true;
            s->marqueeActive = false;
            s->marqueeAdditive = ctrl || shift;
            s->marqueeToggle = ctrl;
            s->marqueeRowIndex = -1;
            s->blankClickPane = s->pane;
            s->blankClickTab = s->appPrefs.blank_click_action != app::kBlankClickOff && tab &&
                !IsAddressSearchResults(tab) && !ctrl && PointInList(*s, mx, my) &&
                (GetKeyState(VK_SHIFT) & 0x8000) == 0 &&
                (GetKeyState(VK_MENU) & 0x8000) == 0 ? tab : nullptr;
            s->blankClickGeneration = tab ? tab->view_generation : 0;
            s->marqueeStart = s->marqueeCur = POINT{ mx, my };
            s->marqueeOriginScrollX = tab ? tab->scroll_x : 0.0f;
            s->marqueeOriginScrollY = tab ? tab->scroll_y : 0.0f;
            s->marqueeBase.clear();
            if (tab && (ctrl || shift)) {
                tab->MaterializeSelection();
                s->marqueeBase = tab->selected;
            }
            SetCapture(hwnd);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
}

// The blank double-click action (设置 > 双击列表空白处).
static void RunBlankDoubleClick(AppState& s, app::Tab& tab) {
    if (app::BlankClickGoesBack(s.appPrefs.blank_click_action, tab.CanGoBack())) {
        GoBack(s);
    } else if (!fs::IsVirtualPath(tab.current_path)) {
        // Same as the Up button: a drive or share root goes on
        // to This PC, where it stops (#68).
        GoUp(s);
    }
}

// The empty strip of the pane title bar is list blank space too.
static bool HandlePaneHeaderBlankDoubleClick(AppState& s, const ui::HitTestResult& hit) {
    if (hit.region != ui::HitTestResult::PaneHeader || hit.index != 0 || hit.pane_index < 0) return false;
    app::Tab* tab = ActiveTab(s);
    if (s.appPrefs.blank_click_action == app::kBlankClickOff || !tab || IsSettingsTab(tab) ||
        IsAddressSearchResults(tab) || PaneAtSlot(s, hit.pane_index) != s.pane ||
        (GetKeyState(VK_CONTROL) & 0x8000) || (GetKeyState(VK_SHIFT) & 0x8000) ||
        (GetKeyState(VK_MENU) & 0x8000))
        return false;
    s.paneDragPending = false;
    RunBlankDoubleClick(s, *tab);
    return true;
}

LRESULT HandleLButtonDblClk(AppState* s, HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        if (!s) return DefWindowProcW(hwnd, msg, wParam, lParam);
        // Pulse 助手: a double click is a second click on its buttons.
        if (AiAssistantMouseOther(*s, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam))) {
            AiAssistantMouseDown(*s, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            return 0;
        }
        if (ColumnStripSwallowDoubleClick(*s)) {
            s->stripClickTick = 0;
            return 0;
        }
        // The second click on a toolbar gap or the status bar after the first
        // cleared the selection: the toolbar and the status hints have changed
        // under the pointer since, so do not hit-test again. Put the selection
        // back and maximize, as the double click there means.
        if (RestoreBlankDeselect(*s, POINT{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)}, true)) {
            g_blank_drag = BlankWindowDrag{};
            SendMessageW(hwnd, WM_SYSCOMMAND, IsZoomed(hwnd) ? SC_RESTORE : SC_MAXIMIZE, 0);
            return 0;
        }
        app::Tab* double_tab = ActiveTab(*s);
        const bool blank_double = s->blankDoubleTab && double_tab == s->blankDoubleTab &&
            s->pane == s->blankDoublePane && double_tab->view_generation == s->blankDoubleGeneration &&
            static_cast<DWORD>(GetMessageTime() - s->blankDoubleTime) <= GetDoubleClickTime() &&
            abs(GET_X_LPARAM(lParam) - s->blankDoublePoint.x) <= GetSystemMetrics(SM_CXDOUBLECLK) / 2 &&
            abs(GET_Y_LPARAM(lParam) - s->blankDoublePoint.y) <= GetSystemMetrics(SM_CYDOUBLECLK) / 2;
        s->blankDoubleTab = nullptr;
        s->blankClickTab = nullptr;
        int mx = GET_X_LPARAM(lParam);
        int my = GET_Y_LPARAM(lParam);
        CancelRenameClick(*s);
        s->dragPending = false;
        // Hit-test only: do not probe details (SHGetFileInfo) before OpenWith.
        ui::WindowViewModel vm = BuildVm(*s, false);
        D2D1_RECT_F rect = D2D1::RectF(0, 0, (float)s->compositor.Width(), (float)s->compositor.Height());
        ui::HitTestResult hit = s->renderer.HitTest(vm, rect, (float)mx, (float)my);
        pulse::diagnostics::runtime::Event("file_double_click", {
            {"row_hit", hit.region == ui::HitTestResult::Row},
            {"details_view", double_tab && double_tab->view_mode == ui::ViewMode::Details},
            {"column_layout", double_tab && double_tab->column_layout},
            {"has_index", hit.index >= 0},
            {"item_index", hit.index >= 0 ? static_cast<uint64_t>(hit.index) : 0}});
        g_blank_drag = BlankWindowDrag{};
        if (BlankChromeAt(*s, hit, mx, my) == BlankChrome::DragMaximize) {
            // Toolbar gaps and the status bar act as the title bar here too.
            SendMessageW(hwnd, WM_SYSCOMMAND, IsZoomed(hwnd) ? SC_RESTORE : SC_MAXIMIZE, 0);
            return 0;
        }
        if (hit.region == ui::HitTestResult::PaneSizeSlider) {  // back to the 96 DIP preset
            SetViewMode(*s, ui::ViewMode::LargeIcons);
            return 0;
        }
        if (HandleWindowIdentityDoubleClick(*s, hit, POINT{mx, my})) return 0;  // M7c: rename, recolor
        if (pulse::HandleTabDoubleClick(*s, hit)) return 0;
        if (HandlePaneHeaderBlankDoubleClick(*s, hit)) {
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        // Buttons have no double-click meaning: a quick second press (inside
        // the system double-click time - 1 s on some machines) is just another
        // press. Without this the theme toggle and friends swallowed it, so
        // they sometimes needed two clicks.
        switch (hit.region) {
        case ui::HitTestResult::ThemeToggle:
        case ui::HitTestResult::SettingsButton:
        case ui::HitTestResult::SidebarToggle:
        case ui::HitTestResult::TabNew:
        case ui::HitTestResult::TabClose:
        case ui::HitTestResult::NavBack:
        case ui::HitTestResult::NavForward:
        case ui::HitTestResult::NavUp:
        case ui::HitTestResult::NavRefresh:
        case ui::HitTestResult::Cut:
        case ui::HitTestResult::Copy:
        case ui::HitTestResult::Paste:
        case ui::HitTestResult::Delete:
        case ui::HitTestResult::DetailsToggle:
        case ui::HitTestResult::PaneMediumIcons:
        case ui::HitTestResult::PaneWall:
        case ui::HitTestResult::PaneSizeButton:
        case ui::HitTestResult::PaneViewButton:
        case ui::HitTestResult::PaneDetails:
        case ui::HitTestResult::ToolbarPin:
        case ui::HitTestResult::FilterClear:
        case ui::HitTestResult::CompareDiffToggle:
        case ui::HitTestResult::DetailsAttrToggle:
        case ui::HitTestResult::DetailsPreviewToggle:
        case ui::HitTestResult::SettingsToggle:
        case ui::HitTestResult::RowCheck:
        case ui::HitTestResult::HeaderCheck:
        case ui::HitTestResult::TreeSticky:
            return HandleLButtonDown(s, hwnd, WM_LBUTTONDOWN, wParam, lParam);
        default:
            break;
        }
        // Single click opens: this press is the second half of a double click
        // whose first click already opened the item.
        if (s->singleOpenTick != 0 && GetTickCount64() - s->singleOpenTick <= GetDoubleClickTime()) {
            s->singleOpenTick = 0;
            return 0;
        }
        // A second press on a chevron toggles again instead of opening the folder.
        if (HandleDetailsTreeMouseDown(*s, vm, hit, mx, my, (GetKeyState(VK_MENU) & 0x8000) != 0)) {
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        if ((hit.region == ui::HitTestResult::Pane || hit.region == ui::HitTestResult::None) &&
            PointInList(*s, mx, my) &&
            (hit.pane_index < 0 || PaneAtSlot(*s, hit.pane_index) == s->pane)) {
            HandleLButtonDown(s, hwnd, WM_LBUTTONDOWN, wParam, lParam);
            s->blankDoublePending = blank_double && s->blankClickTab != nullptr;
            return 0;
        }
        if (hit.region == ui::HitTestResult::ColumnStripDivider) {
            ResetColumnStripWidth(*s, hit);
            if (GetCapture() == hwnd) ReleaseCapture();
            return 0;
        }
        if (hit.region == ui::HitTestResult::DetailsPreview) {
            s->renderer.ToggleDetailsPreviewFit(static_cast<float>(mx), static_cast<float>(my));
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        if (hit.region == ui::HitTestResult::ColumnDivider) {
            // Double-click a column divider: both neighbours return to their
            // content-fitted widths.
            s->columnResizing = false;
            s->columnResizeIndex = -1;
            s->columnResizePane = -1;
            if (GetCapture() == hwnd) ReleaseCapture();
            app::Pane* fitPane = PaneAtSlot(*s, hit.pane_index);
            app::Tab* fitTab = fitPane ? fitPane->ActiveTab() : nullptr;
            if (fitTab && hit.pane_index >= 0 &&
                hit.pane_index < static_cast<int>(vm.pane_slots.size())) {
                const auto& fitView = vm.pane_slots[static_cast<size_t>(hit.pane_index)].pane;
                s->renderer.NoteColumnAutoFit(hit.pane_index);   // motion v2 "colresize"
                app::AutoFitPaneColumn(*fitTab, fitView, s->renderer,
                    s->renderer.PaneBodyBounds(fitView,
                        vm.pane_slots[static_cast<size_t>(hit.pane_index)].rect), hit.index);
                if (s->renameIndex >= 0) LayoutRenameOverlay(*s);
            }
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        if (hit.region == ui::HitTestResult::SidebarItem &&
            hit.path.starts_with(L"pulse:tag:")) {
            ResetSidebarPinDrag(*s);
            if (GetCapture() == hwnd) ReleaseCapture();
            std::wstring kind, rest;
            app::ParsePulsePath(hit.path, &kind, &rest);
            ShowTagRenameOverlay(*s, s->places.ResolveTagRef(rest));
        } else if ((hit.region == ui::HitTestResult::Row || hit.region == ui::HitTestResult::RowFolderSize) &&
                   hit.index >= 0) {
            app::Tab* tab = ActiveTab(*s);
            if (tab && hit.region == ui::HitTestResult::RowFolderSize) {
                // The first press on the size label started a calculation;
                // the folder is being opened instead, so the old result stays.
                const auto path = EntryFullPath(*tab, hit.index);
                if (!path.empty() && path == s->folderSizeClickPath &&
                    static_cast<DWORD>(GetMessageTime() - s->folderSizeClickTime) <= GetDoubleClickTime())
                    s->folderSizes.Cancel(path);
            }
            s->folderSizeClickPath.clear();
            if (tab) tab->SelectOnly(hit.index);
            OpenSelected(*s, (wParam & MK_SHIFT) != 0);
        } else if (hit.region == ui::HitTestResult::AddressBar ||
                   hit.region == ui::HitTestResult::BreadcrumbSegment) {
            ShowOmnibar(*s, OmnibarMode::Path);
        }
        return 0;
}

LRESULT HandleLButtonUp(AppState* s, HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        (void)msg;
        (void)wParam;
        g_blank_drag = BlankWindowDrag{};
        if (const BlankDeselect armed = std::exchange(g_blank_deselect, BlankDeselect{});
            s && armed.armed && !BlankDeselectMoved(armed.start, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)))
            BlankClickDeselect(*s, POINT{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)}, armed.maximize);
        if (s && AiAssistantMouseUp(*s, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam))) return 0;   // Pulse 助手
        if (s && ToolbarPinDragging()) {   // picker T3c
            EndToolbarPinDrag(*s, false);
            return 0;
        }
        if (s && FavoritesBarDragging()) {   // 收藏栏: a click opens, a drag reorders
            EndFavoritesBarDrag(*s, false);
            return 0;
        }
        if (s) {
            s->paneDragPending = false;
            if (s->paneSizeDragging) {
                EndPaneSizeDrag(*s);
                if (GetCapture() == hwnd) ReleaseCapture();
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (s->settings.slider_drag() >= 0) {
                s->settings.EndSlider();
                if (GetCapture() == hwnd) ReleaseCapture();
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (SysCtxPreviewDragEnd(*s, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam))) return 0;
            if (s->settings.menu_drag_item() >= 0) {
                if (s->settings.menu_drag_live()) {
                    const ui::WindowViewModel vm = BuildVm(*s, false);
                    const D2D1_RECT_F rect = D2D1::RectF(0, 0, (float)s->compositor.Width(),
                                                         (float)s->compositor.Height());
                    const auto order = s->renderer.SettingsMenuDropOrder(vm, rect,
                        (float)GET_X_LPARAM(lParam), (float)GET_Y_LPARAM(lParam));
                    s->settings.EndMenuDrag();
                    if (!order.empty()) s->settings.SetMenuOrder(order);
                } else {
                    s->settings.EndMenuDrag();
                }
                if (GetCapture() == hwnd) ReleaseCapture();
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (s->trayDrag.pending || s->trayDrag.active) {
                ReleaseTrayDrag(*s, true);
                if (GetCapture() == hwnd) ReleaseCapture();
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (s->bloom_accent.Pressed() >= 0) {
                const int pressed = s->bloom_accent.Pressed();
                s->bloom_accent.SetPressed(-1);
                const int mx = GET_X_LPARAM(lParam);
                const int my = GET_Y_LPARAM(lParam);
                ui::WindowViewModel vm = BuildVm(*s);
                D2D1_RECT_F rect = D2D1::RectF(0, 0,
                    static_cast<float>(s->compositor.Width()),
                    static_cast<float>(s->compositor.Height()));
                const ui::HitTestResult hit =
                    s->renderer.HitTest(vm, rect, static_cast<float>(mx), static_cast<float>(my));
                if (hit.region == ui::HitTestResult::SettingsAccent && hit.index == pressed)
                    if (pressed >= 0 && pressed < ui::kBloomDotCount)
                        s->settings.AccentChoice(pressed == 0,
                            pressed == 0 ? 0 : ui::BloomDotRgb(pressed));
                if (GetCapture() == hwnd) ReleaseCapture();
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (s->groupDragPending || s->groupDragActive) {
                const bool was_active = s->groupDragActive;
                const std::wstring path = s->groupDragPath;
                const bool navigate = s->groupDragNavigate;
                const int section = s->groupDragId;
                if (was_active) CommitSidebarGroupDrag(*s);
                ResetSidebarGroupDrag(*s);
                if (GetCapture() == hwnd) ReleaseCapture();
                if (!was_active) {
                    // A row of a header-less section navigates on release; a
                    // header folds its section (masks are keyed by id).
                    if (navigate || !path.empty()) NavigateTo(*s, path);
                    else if (section >= 0) s->sidebarCollapsedMask ^= 1u << section;
                }
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (s->pinDragPending || s->pinDragActive) {
                const bool was_active = s->pinDragActive;
                const std::wstring path = s->pinDragPath;
                const bool tab_row = IsVerticalTabPath(path);
                const std::wstring qa_drag_key = s->pinDragKey;
                if (was_active && s->pinDragToIndex >= 0) {
                    if (tab_row)
                        CommitVerticalTabDrag(*s, path, static_cast<size_t>(s->pinDragToIndex));
                    else if (IsSidebarTagPath(path))
                        CommitSidebarTagDrag(*s, path, static_cast<size_t>(s->pinDragToIndex));
                    else if (!qa_drag_key.empty())
                        CommitQuickAccessDrag(*s, qa_drag_key,
                                              static_cast<size_t>(s->pinDragToIndex));
                }
                ResetSidebarPinDrag(*s);
                if (GetCapture() == hwnd) ReleaseCapture();
                // Ctrl / Shift on a tag row combines it into the open tag view;
                // Ctrl on a quick-access row opens it in a new tab (#58-2), as the
                // middle button does.
                const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
                if (!was_active && !path.empty() && !tab_row && ctrl && !qa_drag_key.empty() &&
                    !IsSidebarTagPath(path))
                    OpenFolderTab(*s, path);
                else if (!was_active && !path.empty() && !tab_row &&
                    !(IsSidebarTagPath(path) && CombineSidebarTagClick(*s, path)))
                    NavigateTo(*s, path);
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (s->starDragPending || s->starDragActive) {
                const bool was_active = s->starDragActive;
                const std::wstring path = s->starDragPath;
                if (was_active && s->dropSidebar >= 0) {
                    // The target row's group comes along: onto a member joins
                    // that group, onto a top-level folder leaves one.
                    if (s->starDragTarget == SIZE_MAX) {
                        s->places.SetStarredItemGroup(path, s->starDragGroup);
                    } else {
                        s->places.ReorderStarredFolder(path, s->starDragTarget);
                        s->places.SetStarredItemGroup(path, s->starDragGroup, false);
                    }
                    RefreshStarredViews(*s);
                }
                s->starDragGroup.clear();
                s->starDragPending = false;
                s->starDragActive = false;
                s->starDragPath.clear();
                s->dropSidebar = -1;
                if (GetCapture() == hwnd) ReleaseCapture();
                if (!was_active && !path.empty()) NavigateTo(*s, path);
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (s->tabDragPending || s->tabDragging) {
                const bool wasActive = s->tabDragging;
                if (wasActive && s->pane && s->tabDragIndex >= 0 &&
                    s->tabDragIndex < static_cast<int>(s->window_tabs.items.size())) {
                    int cur = -1;
                    for (int p = 0; p < static_cast<int>(s->tabOrder.size()); ++p)
                        if (s->tabOrder[p] == s->tabDragIndex) { cur = p; break; }
                    if (cur >= 0 && s->tabPitch > 0.0f) {
                        // Collapsed chip drag: the drop target is the chip's
                        // new rest rect, and the settle animation runs on the
                        // px chip channel (hidden members can't carry it).
                        const bool chipDrop = s->tabDragFromChip && s->tabDragBlockW > 0.0f;
                        float targetLeft = s->tabFlowLeft + static_cast<float>(cur) * s->tabPitch;
                        {
                            ui::WindowViewModel vmDrop = BuildVm(*s);
                            const float wwDrop = static_cast<float>(s->compositor.Width());
                            if (chipDrop) {
                                for (int gi = 0;
                                     gi < static_cast<int>(vmDrop.tab_groups.size()); ++gi) {
                                    if (vmDrop.tab_groups[static_cast<size_t>(gi)].id
                                            != s->tabDragGroupId)
                                        continue;
                                    D2D1_RECT_F chipRc{};
                                    if (s->renderer.TabGroupChipRect(vmDrop, wwDrop, gi, &chipRc))
                                        targetLeft = chipRc.left;
                                    break;
                                }
                            } else {
                                D2D1_RECT_F curRc{};
                                if (s->renderer.TabItemRect(vmDrop, wwDrop, cur, &curRc))
                                    targetLeft = curRc.left;
                            }
                        }
                        const float dist = std::abs(s->tabDragFloatLeft - targetLeft);
                        // chipDrop: tabSlotW holds the chip width, so scale the
                        // duration by the uniform tab pitch instead.
                        const float durSlot = chipDrop ? s->tabPitch : s->tabSlotW;
                        const int dur = static_cast<int>(
                            dist * 250.0f / std::max(1.0f, durSlot));
                        if (dur >= 50) {
                            if (chipDrop) {
                                const float start = s->tabDragFloatLeft - targetLeft; // px
                                s->chipTracks[s->tabDragGroupId] = AppState::TabTrack{
                                    start, dur, std::chrono::steady_clock::now() };
                                s->chipOffsets[s->tabDragGroupId] = start;
                            } else {
                                const app::LayoutTab* key =
                                    s->window_tabs.items[static_cast<size_t>(s->tabDragIndex)].get();
                                const float start = (s->tabDragFloatLeft - targetLeft) / s->tabPitch;
                                s->tabTracks[key] = AppState::TabTrack{
                                    start, dur, std::chrono::steady_clock::now() };
                                s->tabOffsets[key] = start;
                            }
                        }
                    }
                    if (s->tabOrder.size() == s->window_tabs.items.size() && !s->tabOrder.empty()) {
                        bool changed = false;
                        for (size_t i = 0; i < s->tabOrder.size(); ++i)
                            if (s->tabOrder[i] != static_cast<int>(i)) { changed = true; break; }
                        if (changed) {
                            std::vector<std::unique_ptr<app::LayoutTab>> reordered;
                            reordered.reserve(s->window_tabs.items.size());
                            size_t newActive = s->window_tabs.active;
                            for (int i = 0; i < static_cast<int>(s->tabOrder.size()); ++i) {
                                if (s->tabOrder[static_cast<size_t>(i)] ==
                                    static_cast<int>(s->window_tabs.active))
                                    newActive = static_cast<size_t>(i);
                            }
                            for (int idx : s->tabOrder) {
                                if (idx >= 0 && idx < static_cast<int>(s->window_tabs.items.size()))
                                    reordered.push_back(
                                        std::move(s->window_tabs.items[static_cast<size_t>(idx)]));
                            }
                            if (reordered.size() == s->window_tabs.items.size()) {
                                s->window_tabs.items = std::move(reordered);
                                s->window_tabs.active = newActive;
                            }
                        }
                    }
                    // A free tab dropped inside a same-group run (or against
                    // its end) joins it. Group members remain grouped; crossing
                    // a free tab moves that tab across the whole group above.
                    if (!s->tabDragFromChip && cur >= 0 && cur < static_cast<int>(s->window_tabs.items.size())) {
                        app::LayoutTab* moved = s->window_tabs.items[static_cast<size_t>(cur)].get();
                        const int prevG = cur > 0
                            ? s->window_tabs.items[static_cast<size_t>(cur - 1)]->tab_group : 0;
                        const int nextG = cur + 1 < static_cast<int>(s->window_tabs.items.size())
                            ? s->window_tabs.items[static_cast<size_t>(cur + 1)]->tab_group : 0;
                        int joined = 0;
                        if (prevG != 0 && prevG == nextG) joined = prevG;
                        else if (moved->tab_group == 0) {
                            if (nextG == 0) joined = prevG;      // run's trailing edge
                            else if (prevG == 0) joined = nextG; // run's leading edge
                        }
                        if (joined != 0) moved->tab_group = joined;
                        if (moved->tab_group != 0) app::NormalizeGroupRuns(s->window_tabs);
                        // Groups with no members left disappear.
                        auto& groups = s->window_tabs.tab_groups;
                        for (auto git = groups.begin(); git != groups.end();) {
                            bool used = false;
                            for (const auto& t : s->window_tabs.items)
                                if (t->tab_group == git->id) { used = true; break; }
                            if (used) ++git; else git = groups.erase(git);
                        }
                    }
                }
                // Plain chip click (press without drag): toggle collapse.
                if (s->tabDragFromChip && !wasActive && s->tabDragGroupId != 0) {
                    s->tabs.ToggleGroupCollapse(s->window_tabs, s->tabDragGroupId);
                    BindCurrentLayout(*s);
                }
                s->tabDragPending = false;
                s->tabDragging = false;
                s->tabDragIndex = -1;
                s->tabDragRunPos = 0;
                s->tabDragRunLen = 1;
                s->tabDragFromChip = false;
                s->tabDragGroupId = 0;
                s->tabDragSlots = 1.0f;
                s->tabOrder.clear();
                if (GetCapture() == hwnd) ReleaseCapture();
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            int singleOpenRow = -1;
            if (s->marqueeActive || s->marqueePending) {
                const int mx = GET_X_LPARAM(lParam);
                const int my = GET_Y_LPARAM(lParam);
                app::Tab* tab = ActiveTab(*s);
                app::BlankPaneClickRelease click;
                click.pending = s->marqueePending;
                click.marquee_active = s->marqueeActive;
                click.owns_capture = GetCapture() == hwnd;
                // Stored pointers are identity tokens only; dereference the live active tab.
                click.same_context = s->pane == s->blankClickPane &&
                    tab && tab == s->blankClickTab &&
                    tab->view_generation == s->blankClickGeneration;
                click.modified = (GetKeyState(VK_CONTROL) & 0x8000) != 0 ||
                    (GetKeyState(VK_SHIFT) & 0x8000) != 0 ||
                    (GetKeyState(VK_MENU) & 0x8000) != 0;
                click.delta_x = mx - s->marqueeStart.x;
                click.delta_y = my - s->marqueeStart.y;
                click.drag_width = GetSystemMetrics(SM_CXDRAG);
                click.drag_height = GetSystemMetrics(SM_CYDRAG);
                click.blank_list_hit = PointInList(*s, mx, my);
                bool goBack = s->appPrefs.blank_click_action != app::kBlankClickOff &&
                    !IsAddressSearchResults(tab) && app::IsBlankPaneBackClick(click);
                if (goBack) {
                    const ui::WindowViewModel vm = BuildVm(*s, false);
                    const D2D1_RECT_F rect = D2D1::RectF(
                        0, 0, static_cast<float>(s->compositor.Width()),
                        static_cast<float>(s->compositor.Height()));
                    const ui::HitTestResult hit = s->renderer.HitTest(
                        vm, rect, static_cast<float>(mx), static_cast<float>(my));
                    goBack = (hit.region == ui::HitTestResult::Pane ||
                              hit.region == ui::HitTestResult::None) &&
                        (hit.pane_index < 0 || PaneAtSlot(*s, hit.pane_index) == s->pane);
                }
                const bool double_click = s->blankDoublePending;
                s->blankDoublePending = false;
                s->blankDoubleTab = goBack && !double_click ? tab : nullptr;
                s->blankDoublePane = s->pane;
                s->blankDoubleGeneration = tab ? tab->view_generation : 0;
                s->blankDoubleTime = static_cast<DWORD>(GetMessageTime());
                s->blankDoublePoint = s->marqueeStart;
                goBack = goBack && double_click;
                const int row_click = s->marqueePending ? s->marqueeRowIndex : -1;
                const bool row_ctrl = s->marqueeRowCtrl, row_shift = s->marqueeRowShift;
                if (s->marqueeActive) ApplyMarqueeSelection(*s);
                ResetMarquee(*s);
                if (row_click >= 0) {
                    // No movement: the press on the row was a click on it.
                    HandleListRowClick(*s, row_click, row_ctrl, row_shift);
                    FinishListRowClick(*s);
                    goBack = false;
                    if (s->singleOpenCandidate && row_click == s->singleOpenIndex) singleOpenRow = row_click;
                }
                if (goBack) RunBlankDoubleClick(*s, *tab);
            } else if (s->clickCollapseIndex >= 0) {
                FinishListRowClick(*s);
            }
            if (s->renameClickCandidate && s->renameClickDue == 0) {
                const int mx = GET_X_LPARAM(lParam);
                const int my = GET_Y_LPARAM(lParam);
                ui::WindowViewModel vm = BuildVm(*s, false);
                D2D1_RECT_F rect = D2D1::RectF(
                    0, 0, static_cast<float>(s->compositor.Width()),
                    static_cast<float>(s->compositor.Height()));
                const ui::HitTestResult hit = s->renderer.HitTest(
                    vm, rect, static_cast<float>(mx), static_cast<float>(my));
                app::Tab* tab = ActiveTab(*s);
                const bool valid = s->pane == s->renameClickPane &&
                    tab == s->renameClickTab && tab && tab->SelectedCount() == 1 &&
                    tab->selected_index == s->renameClickIndex &&
                    hit.index == s->renameClickIndex &&
                    PointInHitItemName(*s, vm, hit, static_cast<float>(mx), static_cast<float>(my));
                if (valid) {
                    // Double-click recognition starts on press, not release.
                    s->renameClickDue = s->renameClickStarted + GetDoubleClickTime();
                } else {
                    CancelRenameClick(*s);
                }
            }
            s->clickCollapseIndex = -1;
            if (s->singleOpenCandidate && s->dragPending) singleOpenRow = s->singleOpenIndex;
            s->singleOpenCandidate = false;
            s->dragPending = false;
            s->scrollbarDragging = false;
            s->scrollbarHorizontal = false;
            s->scrollbarSidebar = false;
            s->scrollbarSettings = false;
            s->splitterDragging = false;
            s->detailsPanelResizing = false;
            s->columnResizing = false;
            s->columnResizeIndex = -1;
            s->columnResizePane = -1;
            EndColumnStripResize(*s);
            s->splitterDragIndex = -1;
            s->tabDragPending = false;
            s->tabDragging = false;
            s->tabDragIndex = -1;
            if (GetCapture() == hwnd) ReleaseCapture();
            if (singleOpenRow >= 0) OpenSingleClickRow(*s, singleOpenRow);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
}

LRESULT HandleCaptureChanged(AppState* s, HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        (void)msg;
        (void)wParam;
        (void)lParam;
        if (s && ToolbarPinDragging() && reinterpret_cast<HWND>(lParam) != hwnd) EndToolbarPinDrag(*s, true);
        if (s && FavoritesBarDragging() && reinterpret_cast<HWND>(lParam) != hwnd) EndFavoritesBarDrag(*s, true);
        if (s) {
            s->columnResizing = false;
            s->columnResizeIndex = -1;
            s->columnResizePane = -1;
            if (reinterpret_cast<HWND>(lParam) != hwnd) EndPaneSizeDrag(*s);
            if (s->trayDrag.pending || s->trayDrag.active) {
                // Capture lost mid-gesture: the card springs back home.
                ReleaseTrayDrag(*s, false);
            }
            EndColumnStripResize(*s);
            if (s->renameClickCandidate && s->renameClickDue == 0)
                CancelRenameClick(*s);
            if (s->starDragPending || s->starDragActive) {
                s->starDragPending = false;
                s->starDragActive = false;
                s->starDragPath.clear();
                s->dropSidebar = -1;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            if (s->groupDragPending || s->groupDragActive) {
                // Capture lost mid-gesture: cancel the reorder, keep the order.
                ResetSidebarGroupDrag(*s);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            if (s->pinDragPending || s->pinDragActive) {
                // Same for the pin reorder: abandon it, keep the stored order.
                ResetSidebarPinDrag(*s);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            if (s->tabDragPending || s->tabDragging) {
                s->tabDragPending = false;
                s->tabDragging = false;
                s->tabDragIndex = -1;
                s->tabDragRunPos = 0;
                s->tabDragRunLen = 1;
                s->tabDragFromChip = false;
                s->tabDragGroupId = 0;
                s->tabDragSlots = 1.0f;
                s->tabOrder.clear();
                s->tabTracks.clear();
                s->tabOffsets.clear();
                s->chipTracks.clear();
                s->chipOffsets.clear();
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            if (s->marqueeActive) ApplyMarqueeSelection(*s);
            ResetMarquee(*s);
            s->clickCollapseIndex = -1;
            s->singleOpenCandidate = false;
            s->dragPending = false;
            s->scrollbarDragging = false;
            s->scrollbarHorizontal = false;
            s->scrollbarSidebar = false;
            s->scrollbarSettings = false;
            s->splitterDragging = false;
            s->splitterDragIndex = -1;
            s->tabDragPending = false;
            s->tabDragging = false;
            s->tabDragIndex = -1;
        }
        return 0;
}

LRESULT HandleRButtonDown(AppState* s, HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        if (!s) return DefWindowProcW(hwnd, msg, wParam, lParam);
        if (AiAssistantMouseOther(*s, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam))) return 0;   // Pulse 助手
        DismissPathEditorOutside(*s, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        if (s->filterEditing) HideFilterEditor(*s, true);
        s->blankClickTab = nullptr;
        CancelRenameClick(*s);
        // Prefetch the Explorer verbs for the menu that WM_RBUTTONUP will
        // open: pulse_shell builds the COM menu during the press + fade-in.
        int mx = GET_X_LPARAM(lParam);
        int my = GET_Y_LPARAM(lParam);
        ui::WindowViewModel vm = BuildVm(*s);
        D2D1_RECT_F rect = D2D1::RectF(0, 0, (float)s->compositor.Width(), (float)s->compositor.Height());
        ui::HitTestResult hit = s->renderer.HitTest(vm, rect, (float)mx, (float)my);
        app::Pane* pane = hit.pane_index >= 0 ? PaneAtSlot(*s, hit.pane_index) : s->pane;
        app::Tab* tab = pane ? pane->ActiveTab() : nullptr;
        if (hit.region == ui::HitTestResult::Row && hit.index >= 0 && tab) {
            // Selection changes on WM_RBUTTONUP; predict what it will be.
            std::vector<std::wstring> paths;
            std::wstring ext;
            if (tab->IsSelected(hit.index) && !tab->content_results) {
                paths = SelectedFullPaths(*tab);
                ext = StaticVerbKey(*tab, tab->SelectedIndices());
            } else {
                std::wstring one = EntryFullPath(*tab, hit.index);
                if (!one.empty()) paths.push_back(std::move(one));
                ext = StaticVerbKey(*tab, { hit.index });
            }
            if (!paths.empty()) StartCtxQuery(*s, std::move(paths), false, ext);
        } else if (tab && !tab->current_path.empty() &&
                   !fs::IsVirtualPath(tab->current_path) &&
                   (hit.region == ui::HitTestResult::Pane ||
                    hit.region == ui::HitTestResult::PaneHeader ||
                    hit.region == ui::HitTestResult::None)) {
            D2D1_RECT_F content = s->renderer.ContentRect(rect.right, rect.bottom);
            if (hit.pane_index >= 0 ||
                (mx >= content.left && mx < content.right && my >= content.top && my < content.bottom)) {
                StartCtxQuery(*s, { tab->current_path }, true, ipc::kBackgroundVerbKey);
            }
        }
        return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT HandleRButtonUp(AppState* s, HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        if (!s) return DefWindowProcW(hwnd, msg, wParam, lParam);
        if (AiAssistantMouseOther(*s, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam))) return 0;   // Pulse 助手
        int mx = GET_X_LPARAM(lParam);
        int my = GET_Y_LPARAM(lParam);
        ui::WindowViewModel vm = BuildVm(*s);
        D2D1_RECT_F rect = D2D1::RectF(0, 0, (float)s->compositor.Width(), (float)s->compositor.Height());
        ui::HitTestResult hit = s->renderer.HitTest(vm, rect, (float)mx, (float)my);
        if (hit.pane_index >= 0) {
            if (app::Pane* p = PaneAtSlot(*s, hit.pane_index)) FocusPane(*s, p);
        }
        POINT sp{ mx, my };
        ClientToScreen(hwnd, &sp);
        // Toolbar selection chip: the same Pulse item menu as a right-clicked row.
        if (hit.region == ui::HitTestResult::ToolbarSelAction) {
            ShowItemContextMenu(*s, sp);
            return 0;
        }
        // Column view: a row of another column is selected like a click, then
        // gets the same item menu as a list row.
        if (hit.region == ui::HitTestResult::ColumnStripRow) {
            if (SelectColumnStripRow(*s, hit)) {
                InvalidateRect(hwnd, nullptr, FALSE);
                ShowItemContextMenu(*s, sp);
            }
            return 0;
        }
        if (HandleFavoritesBarContextMenu(*s, hit, sp)) return 0;   // 收藏栏
        // A pinned tool: separators, remove, 自定义工具栏 (the preview pin and
        // the apps fall through to the popover).
        if (hit.region == ui::HitTestResult::ToolbarPin && hit.index >= 0 && !ui::ToolbarPickerOpen() &&
            hit.index < static_cast<int>(app::ToolbarToolsOnly(s->appPrefs.toolbar_tools).size())) {
            ShowToolbarPinMenu(*s, hit.index, sp);
            return 0;
        }
        if (hit.region == ui::HitTestResult::ToolbarPin && hit.index >= 0 && !ui::ToolbarPickerOpen() &&
            !s->toolbarDropMode) {
            const int pins = static_cast<int>(ToolbarPinActions(*s).size());
            if (hit.index >= pins && hit.index - pins < static_cast<int>(s->appPrefs.toolbar_apps.size())) {
                ShowToolbarAppPinMenu(*s, static_cast<size_t>(hit.index - pins), sp);
                return 0;
            }
        }
        // Toolbar tools / apps / More: the customize popover, on the page of
        // the part clicked.
        if (hit.region == ui::HitTestResult::ToolbarPin || hit.region == ui::HitTestResult::ToolbarPinLabel ||
            hit.region == ui::HitTestResult::ToolbarMore) {
            const bool app_page = hit.region == ui::HitTestResult::ToolbarPinLabel ||
                (hit.region == ui::HitTestResult::ToolbarPin &&
                 hit.index >= static_cast<int>(ToolbarPinActions(*s).size()));
            ShowToolbarCustomize(*s, app_page ? 2 : 0);
            return 0;
        }
        bool shown = false;
        if (hit.region == ui::HitTestResult::WindowIdentity) {
            ShowWindowIdentityMenu(*s, sp);  // M7c
            shown = true;
        } else if (hit.region == ui::HitTestResult::TabGroup && hit.index >= 0 &&
            hit.index < static_cast<int>(vm.tab_groups.size())) {
            // Group chip right-click: the Edge-style editor bubble.
            if (s->pane && EnsureMenu(*s)) {
                s->tabs.ShowGroupMenu(s->window_tabs,
                    vm.tab_groups[static_cast<size_t>(hit.index)].id, sp, *s->menu);
                BindCurrentLayout(*s);
            }
            shown = true;
        } else if (hit.region == ui::HitTestResult::Tab && hit.index >= 0) {
            // Every tab gets the Edge-style tab menu; group editing lives on
            // the chip (right-click) and in the editor bubble.
            if (s->pane && EnsureMenu(*s)) {
                s->tabs.ShowTabMenu(s->window_tabs, hit.index, sp, *s->menu);
                BindCurrentLayout(*s);
            }
            shown = true;
        } else if (hit.region == ui::HitTestResult::BreadcrumbSegment && !hit.path.empty()) {
            ShowBreadcrumbMenu(*s, hit.path, sp);
            shown = true;
        } else if (hit.region == ui::HitTestResult::TrayShelf &&
                   static_cast<ui::TrayShelfHit>(hit.index) != ui::TrayShelfHit::Strip &&
                   !s->tray.batches().empty()) {
            ShowTrayBatchMenu(*s, sp);
            shown = true;
        } else if (hit.region == ui::HitTestResult::TrayCard ||
                   hit.region == ui::HitTestResult::TrayIntent ||
                   hit.region == ui::HitTestResult::TrayItemRemove ||
                   hit.region == ui::HitTestResult::TrayPrev ||
                   hit.region == ui::HitTestResult::TrayNext) {
            ShowTrayBatchMenu(*s, sp);
            shown = true;
        } else if (hit.region == ui::HitTestResult::Row && hit.index >= 0) {
            app::Tab* tab = ActiveTab(*s);
            if (tab) {
                if (!tab->IsSelected(hit.index)) tab->SelectOnly(hit.index);
                else tab->selected_index = hit.index;
            }
            InvalidateRect(hwnd, nullptr, FALSE);
            std::wstring virtual_kind;
            const bool curated = tab && app::ParsePulsePath(
                tab->current_path, &virtual_kind, nullptr) &&
                (virtual_kind == L"starred" || virtual_kind == L"recent");
            if (curated) {
                ShowCuratedItemMenu(*s, EntryFullPath(*tab, hit.index),
                                    virtual_kind == L"recent", sp);
            } else {
                ShowItemContextMenu(*s, sp);
            }
            shown = true;
        } else if (hit.region == ui::HitTestResult::SidebarItem &&
                   hit.path.starts_with(L"pulse:tag:")) {
            std::wstring kind, rest;
            app::ParsePulsePath(hit.path, &kind, &rest);
            const app::TagId id = s->places.ResolveTagRef(rest);
            if (!id.empty()) ShowTagSidebarMenu(*s, id, sp);
            shown = true;
        } else if (hit.region == ui::HitTestResult::SidebarItem && app::IsTagGroupPath(hit.path)) {
            const std::wstring group = app::TagGroupIdFromPath(hit.path);
            if (!group.empty()) ShowTagGroupMenu(*s, group, sp);
            else ShowSidebarSectionMenu(*s, static_cast<int>(app::SidebarSectionId::Tags), sp);
            shown = true;
        } else if (hit.region == ui::HitTestResult::SidebarItem && hit.path == app::MakeTagManagerPath()) {
            ShowSidebarSectionMenu(*s, static_cast<int>(app::SidebarSectionId::Tags), sp);
            shown = true;
        } else if (hit.region == ui::HitTestResult::SidebarItem &&
                   hit.path == app::MakeRecyclePath()) {
            ShowRecyclePlaceMenu(*s, sp);
            shown = true;
        } else if (hit.region == ui::HitTestResult::SidebarItem &&
                   hit.path.starts_with(L"pulse:workspace:")) {
            std::wstring kind, rest;
            app::ParsePulsePath(hit.path, &kind, &rest);
            ShowWorkspaceMenu(*s, _wtoi(rest.c_str()), sp);
            shown = true;
        } else if (HandleVerticalTabContextMenu(*s, hit, sp)) {
            shown = true;
        } else if (hit.region == ui::HitTestResult::SidebarItem &&
                   hit.sidebar_section == static_cast<int>(app::SidebarSectionId::Networks) &&
                   s->places.FindNetwork(hit.path) >= 0) {
            // Pinned network locations had no menu, so they could not be unpinned (#236).
            ShowNetworkPlaceMenu(*s, hit.path, sp);
            shown = true;
        } else if (hit.region == ui::HitTestResult::SidebarItem &&
                   hit.sidebar_section == static_cast<int>(app::SidebarSectionId::Cloud)) {
            // OneDrive has no header to right-click (#80): its rows carry the menu.
            ShowCloudPlaceMenu(*s, hit.path, sp);
            shown = true;
        } else if (hit.region == ui::HitTestResult::SidebarItem) {
            // The 项目 row of the repository the tab is in: its Git menu.
            if (ShowGitProjectMenu(*s, hit.path, sp)) {
                s->context_menu.Close();
                return 0;
            }
            if (s->places.IsQuickAccessPinned(hit.path)) {
                ShowQuickAccessMenu(*s, hit.path, sp);
                s->context_menu.Close();
                return 0;
            }
            const app::StarredItem* starred = s->places.FindStarred(hit.path);
            const app::SidebarEntry* quick_access = QuickAccessEntryForPath(*s, hit.path);
            if ((starred && starred->kind == app::PlaceItemKind::Folder) || quick_access) {
                ShowCuratedItemMenu(*s, hit.path, false, sp);
                shown = true;
            }
        } else if (hit.region == ui::HitTestResult::SidebarHeader ||
                   hit.region == ui::HitTestResult::SidebarBlank) {
            // The menu must match what the cursor is over: a section header, or
            // the empty space inside a section, opens that section's own menu.
            // Only the space below every section opens the pane-wide menu.
            int section = -1;
            if (hit.index >= 0 && hit.index < static_cast<int>(vm.sidebar.size()))
                section = vm.sidebar[static_cast<size_t>(hit.index)].id;
            if (section >= 0) ShowSidebarSectionMenu(*s, section, sp);
            else ShowSidebarSectionsMenu(*s, sp);
            shown = true;
        } else if (!IsSettingsTab(ActiveTab(*s)) &&
                   (hit.region == ui::HitTestResult::Pane ||
                   hit.region == ui::HitTestResult::PaneHeader ||
                   hit.region == ui::HitTestResult::FilterBox ||
                   hit.region == ui::HitTestResult::ColumnHeader ||
                   hit.region == ui::HitTestResult::None)) {
            D2D1_RECT_F content = s->renderer.ContentRect(rect.right, rect.bottom);
            // The Details header picks the shown columns, as in File Explorer.
            if (hit.region == ui::HitTestResult::ColumnHeader && hit.pane_index >= 0) {
                s->context_menu.Close();
                uint32_t visible = 0xFFFFFFFFu;
                bool search = false;
                if (hit.pane_index < static_cast<int>(vm.pane_slots.size())) {
                    const auto& slot = vm.pane_slots[static_cast<size_t>(hit.pane_index)];
                    const auto columns = s->renderer.DetailsColumns(
                        s->renderer.PaneBodyBounds(slot.pane, slot.rect), slot.pane);
                    visible = 0;
                    for (int i = 0; i < columns.count; ++i)
                        visible |= 1u << static_cast<uint32_t>(columns.kinds[static_cast<size_t>(i)]);
                    search = slot.pane.is_search;
                }
                ShowDetailsColumnMenu(*s, sp, visible, search);
                shown = true;
            } else if (hit.pane_index >= 0 ||
                (mx >= content.left && mx < content.right && my >= content.top && my < content.bottom)) {
                ShowBackgroundContextMenu(*s, sp);
                shown = true;
            }
        }
        // The press may have prefetched a session for a menu that never
        // opened (released over the sidebar, drag, …): free the host thread.
        if (!shown) s->context_menu.Close();
        return 0;
}

LRESULT HandleMouseWheel(AppState* s, HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        if (!s) return DefWindowProcW(hwnd, msg, wParam, lParam);
        {
            POINT wp{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            ScreenToClient(hwnd, &wp);
            if (AiAssistantWheel(*s, wp.x, wp.y, msg == WM_MOUSEWHEEL ? GET_WHEEL_DELTA_WPARAM(wParam) : 0))
                return 0;   // Pulse 助手's conversation scrolls
        }
        {
            // 地址栏 out of edit mode: a path longer than the bar scrolls under
            // the wheel, and under a touchpad's sideways swipe.
            POINT ap{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            ScreenToClient(hwnd, &ap);
            const float width = static_cast<float>(s->compositor.Width());
            const D2D1_RECT_F addr = s->renderer.AddressBarRect(width);
            const float notches = static_cast<float>(GET_WHEEL_DELTA_WPARAM(wParam)) / static_cast<float>(WHEEL_DELTA);
            if (static_cast<float>(ap.y) >= addr.top && static_cast<float>(ap.y) < addr.bottom &&
                static_cast<float>(ap.x) >= addr.left && static_cast<float>(ap.x) < addr.right &&
                s->renderer.ScrollBreadcrumb(BuildVm(*s, false), width, static_cast<float>(ap.x),
                                             static_cast<float>(ap.y), msg == WM_MOUSEHWHEEL ? -notches : notches)) {
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
        }
        if (msg == WM_MOUSEHWHEEL) return DefWindowProcW(hwnd, msg, wParam, lParam);
        s->blankClickTab = nullptr;
        CancelRenameClick(*s);
        {
            // Overflowing tab strip scrolls under the wheel, on any page.
            POINT tp{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            ScreenToClient(hwnd, &tp);
            if (tp.y >= 0 && static_cast<float>(tp.y) < s->renderer.TitleBarHeight() &&
                s->renderer.ScrollTabStrip(BuildVm(*s, false), static_cast<float>(s->compositor.Width()),
                    static_cast<float>(tp.x), static_cast<float>(tp.y),
                    static_cast<float>(GET_WHEEL_DELTA_WPARAM(wParam)) / static_cast<float>(WHEEL_DELTA))) {
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
        }
        if (IsSettingsTab(ActiveTab(*s))) {
            const int delta = GET_WHEEL_DELTA_WPARAM(wParam);
            ui::WindowViewModel svm = BuildVm(*s);
            const float max_scroll = s->renderer.SettingsMaxScroll(
                svm, static_cast<float>(s->compositor.Width()),
                static_cast<float>(s->compositor.Height()));
            s->settings.ScrollBy(static_cast<float>(delta), s->scale, max_scroll);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        POINT pt{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        ScreenToClient(hwnd, &pt);
        ui::WindowViewModel wheelVm = BuildVm(*s);
        D2D1_RECT_F wheelRect = D2D1::RectF(0, 0, (float)s->compositor.Width(), (float)s->compositor.Height());
        ui::HitTestResult wheelHit = s->renderer.HitTest(wheelVm, wheelRect, (float)pt.x, (float)pt.y);
        if (HandleColumnStripWheel(*s, wheelVm, wheelHit, GET_WHEEL_DELTA_WPARAM(wParam))) {
            RefreshScrolledHover(*s);
            return 0;
        }
        const D2D1_RECT_F sidebarRc = s->renderer.SidebarRect(wheelRect.right, wheelRect.bottom);
        // Tray stack first: the panel lives inside the sidebar rect, so the
        // sidebar branch below would swallow every wheel event over it.
        // Wheel down sends the top card to the back, wheel up brings the
        // last one back on top (cyclic).
        if (TrayItemTotalCount(s->tray) > 0) {
            const D2D1_RECT_F tray_rc = s->renderer.StagingTrayRect(
                wheelVm, wheelRect.right, wheelRect.bottom);
            if (pt.x >= tray_rc.left && pt.x < tray_rc.right &&
                pt.y >= tray_rc.top && pt.y < tray_rc.bottom) {
                // Shelf redesign: the open view's list scrolls (FillTrayOpen
                // clamps it); the other layers just keep the sidebar still.
                if (s->trayView.open) {
                    const float steps = static_cast<float>(GET_WHEEL_DELTA_WPARAM(wParam)) /
                                        static_cast<float>(WHEEL_DELTA);
                    s->trayView.scroll = std::max(0.0f, s->trayView.scroll - steps * 48.0f);
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
                return 0;
            }
        }
        if (pt.x >= sidebarRc.left && pt.x < sidebarRc.right &&
            pt.y >= sidebarRc.top && pt.y < sidebarRc.bottom &&
            // SidebarRect covers the hover-peek overlay, so it scrolls too.
            sidebarRc.right - sidebarRc.left > 60.0f * s->scale) {
            const float max_scroll = s->renderer.SidebarMaxScroll(
                wheelVm, wheelRect.right, wheelRect.bottom);
            const float steps = static_cast<float>(GET_WHEEL_DELTA_WPARAM(wParam)) /
                                static_cast<float>(WHEEL_DELTA);
            s->sidebarScroll = std::clamp(
                s->sidebarScroll - steps * 48.0f * s->scale, 0.0f, max_scroll);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        const D2D1_RECT_F detailsRc = s->renderer.DetailsPanelRect(
            wheelRect.right, wheelRect.bottom);
        if (detailsRc.right > detailsRc.left && pt.x >= detailsRc.left &&
            pt.x < detailsRc.right && pt.y >= detailsRc.top && pt.y < detailsRc.bottom) {
            const float steps = static_cast<float>(GET_WHEEL_DELTA_WPARAM(wParam)) /
                                static_cast<float>(WHEEL_DELTA);
            if (wheelVm.details.multi_count <= 1 && !wheelVm.details.is_dir &&
                wheelHit.region == ui::HitTestResult::DetailsPreview) {
                s->renderer.ScrollDetailsPreview(steps, static_cast<float>(pt.x),
                    static_cast<float>(pt.y));
            } else {
                const float visibleDip = (detailsRc.bottom - detailsRc.top) / s->scale;
                const float contentDip = s->renderer.DetailsContentHeightDip(
                    wheelVm, wheelRect.right, wheelRect.bottom);
                s->detailsScroll = std::clamp(s->detailsScroll - steps * 48.0f,
                    0.0f, std::max(0.0f, contentDip - visibleDip));
            }
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        // Tray deck: wheel over the panel pages the icon window (handled above,
        // ahead of the sidebar branch that contains it).
        // A marquee belongs to the focused pane: the wheel scrolls it there.
        const bool marquee = s->marqueeActive || s->marqueePending;
        if (wheelHit.pane_index >= 0 && !marquee) {
            if (app::Pane* p = PaneAtSlot(*s, wheelHit.pane_index)) FocusPane(*s, p);
        }
        int delta = GET_WHEEL_DELTA_WPARAM(wParam);
        app::Tab* wheelTab = ActiveTab(*s);
        // The header size slider takes the plain wheel too, within the icon sizes.
        const bool sizeWheel = wheelHit.region == ui::HitTestResult::PaneSizeSlider;
        if (((GET_KEYSTATE_WPARAM(wParam) & MK_CONTROL) != 0 || sizeWheel) && !marquee) {
            if (wheelTab && delta != 0) {
                const ULONGLONG now = GetTickCount64();
                if (now - s->zoomWheelTick > 400 || (s->zoomWheelRemainder > 0) != (delta > 0))
                    s->zoomWheelRemainder = 0;
                s->zoomWheelTick = now;
                s->zoomWheelRemainder += delta;
                // At most one size per message: every step relayouts and
                // requests thumbnails, and a fast spin should not skip a stop.
                const int step = std::clamp(s->zoomWheelRemainder / WHEEL_DELTA, -1, 1);
                if (step != 0) {
                    s->zoomWheelRemainder = 0;
                    ZoomIconSize(*s, step, sizeWheel ? nullptr : &pt, !sizeWheel);
                    if (sizeWheel) ResetSizeTooltip(*s);
                }
            }
            return 0;
        }
        if (wheelTab && wheelTab->view_mode == ui::ViewMode::List) {
            StartSmoothScroll(*s,
                -(static_cast<float>(delta) / WHEEL_DELTA) * 220.0f * s->scale, true);
            return 0;
        }
        UINT wheelLines = 3;
        SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &wheelLines, 0);
        float distance = 0.0f;
        if (wheelLines == WHEEL_PAGESCROLL) {
            D2D1_RECT_F list = ListRect(*s);
            distance = (list.bottom - list.top) * 0.9f;
        } else {
            distance = s->renderer.RowHeight() * static_cast<float>(wheelLines);
        }
        StartSmoothScroll(*s,
            -(static_cast<float>(delta) / static_cast<float>(WHEEL_DELTA)) * distance);
        return 0;
}

// Where a keyboard-opened menu appears (screen coordinates): under the focused
// item when something is selected, otherwise at the list's top-left corner.
static POINT KeyboardMenuPoint(AppState& s) {
    app::Tab* tab = ActiveTab(s);
    const D2D1_RECT_F list = ListRect(s);
    POINT pt{ static_cast<LONG>(list.left + 24.0f * s.scale), static_cast<LONG>(list.top + 12.0f * s.scale) };
    if (tab && !SelectedFullPaths(*tab).empty() && tab->selected_index >= 0) {
        EnsureRowVisible(s, *tab, tab->selected_index);
        ui::PaneViewModel pane;
        app::FillPaneViewModel(pane, *s.pane, &s.places);
        const int view = pane.ViewIndex(tab->selected_index);
        if (view >= 0) {
            const D2D1_RECT_F item = s.renderer.ItemRectInPane(pane, FocusedPaneRect(s), view);
            if (item.bottom > item.top) {
                pt.x = static_cast<LONG>(std::clamp(item.left + 24.0f * s.scale, list.left, list.right));
                pt.y = static_cast<LONG>(std::clamp(item.bottom, list.top, list.bottom));
            }
        }
    }
    ClientToScreen(s.hwnd, &pt);
    return pt;
}

// Shift+F10 / Apps key: the item menu under the focused item, otherwise the
// folder background menu at the list's top-left corner.
void ShowKeyboardContextMenu(AppState& s) {
    app::Tab* tab = ActiveTab(s);
    if (!tab || !s.hwnd) return;
    const POINT pt = KeyboardMenuPoint(s);
    if (!SelectedFullPaths(*tab).empty()) ShowItemContextMenu(s, pt);
    else ShowBackgroundContextMenu(s, pt);
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

LRESULT HandleKeyDown(AppState* s, HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        if (!s) return DefWindowProcW(hwnd, msg, wParam, lParam);
        if (wParam == VK_ESCAPE && AiAssistantEscape(*s)) return 0;   // Pulse 助手's model menu
        if (wParam == VK_ESCAPE && SysCtxPreviewDragCancel(*s)) return 0;
        if (wParam == VK_ESCAPE && CancelMarquee(*s)) {
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        if (wParam == VK_ESCAPE && s->settings.menu_drag_item() >= 0) {
            // Esc puts a dragged Pulse menu preview row back.
            s->settings.EndMenuDrag();
            if (GetCapture() == hwnd) ReleaseCapture();
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        if (HandleWindowSwitcherKey(*s, lParam)) return 0;  // M7d: Ctrl+`
        if (HandleFavoritesBarKey(*s, wParam)) return 0;    // 收藏栏 holding the F6 focus
        s->blankClickTab = nullptr;
        CancelRenameClick(*s);
        app::Tab* tab = ActiveTab(*s);
        if (!tab) return DefWindowProcW(hwnd, msg, wParam, lParam);
        ui::WindowViewModel vm = BuildVm(*s);
        bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        bool alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
        bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        bool handled = true;

        // Settings has no tab of its own: Esc closes it like a second click on
        // the settings button. Popovers, recorders, the shortcut search box and
        // drags took their Esc before this; editors keep theirs.
        if (wParam == VK_ESCAPE && !ctrl && !alt && !shift && IsSettingsTab(tab) &&
            !s->addressSearching && !s->filterEditing && s->renameIndex < 0 &&
            (GetFocus() == hwnd || GetFocus() == nullptr)) {
            CloseSettings(*s);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        // Open tray view (pulse_tray_redesign.html): Esc collapses (the compare
        // card first), Del takes the checked items off the tray, Enter
        // releases them into this folder. Without checks Del / Enter keep
        // acting on the list.
        if (s->trayView.open && !ctrl && !alt && !s->addressSearching && !s->filterEditing &&
            s->renameIndex < 0 && (GetFocus() == hwnd || GetFocus() == nullptr)) {
            if (wParam == VK_ESCAPE) {
                if (s->trayView.compare) s->trayView.compare = false;
                else s->trayView.open = false;
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (wParam == VK_DELETE && !shift && !s->trayView.checked.empty()) {
                s->trayView.removing = s->trayView.checked;
                s->trayView.remove_at = ui::motion::NowMs();
                s->trayView.compare = false;
                if (!ui::motion::SystemAnimationsEnabled()) s->trayView.remove_at -= 320;
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (wParam == VK_RETURN && !s->trayView.checked.empty()) {
                if (tab && !tab->current_path.empty()) ReleaseTrayTo(*s, tab->current_path, TrayEffectiveMove(*s));
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
        }

        auto switchToTab = [&](std::optional<size_t> target) {
            if (!target || *target == s->window_tabs.active) return;
            if (s->renameIndex >= 0) HideRenameOverlay(*s, false);
            if (!s->tagRenameId.empty()) HideTagRenameOverlay(*s, false);
            if (s->filterEditing) HideFilterEditor(*s, true);
            HideAddressEditor(*s, false);
            SetFocus(s->hwnd);
            SwitchTab(*s, *target);
        };
        // Bug 3: File Explorer's keys first (explorer_shortcuts.h).
        const app::ExplorerKeyCommand explorer =
            app::ExplorerShortcut(static_cast<UINT>(wParam), ctrl, shift, alt);

        if (app::IsTabShortcut(static_cast<UINT>(wParam), ctrl, shift, alt)) {
            switchToTab(app::TabShortcutTarget(s->window_tabs, shift));
        } else if (explorer.action != app::ExplorerKeyAction::None) {
            using A = app::ExplorerKeyAction;
            switch (explorer.action) {
            case A::Delete: DeleteSelected(*s, explorer.shift); break;
            case A::MarkTarget: MarkTargetPane(*s); break;
            case A::NewFolder: NewFolderOrConnect(*s); break;
            case A::Search:
                if (IsAddressSearchResults(tab)) ShowAddressSearch(*s);
                else ShowFilterEditor(*s);
                break;
            case A::TabNumber:
                switchToTab(app::NthVisibleTab(app::VisibleTabIndices(s->window_tabs), explorer.index));
                break;
            case A::Layout: {
                static constexpr app::LayoutPreset kPresets[] = {
                    app::LayoutPreset::Single, app::LayoutPreset::TwoVertical,
                    app::LayoutPreset::Three, app::LayoutPreset::FourGrid};
                ApplyLayoutPreset(*s, kPresets[explorer.index]);
                break;
            }
            case A::ViewMode: ChooseViewMode(*s, ui::ViewModeFromIndex(explorer.index)); break;
            case A::Tag: {
                // Alt+N follows the favorite slot, not the sidebar order.
                const int index = s->places.FavoriteTagIndex(explorer.index + 1);
                if (index >= 0) {
                    const auto tag_id=s->places.tags[static_cast<size_t>(index)].id;
                    auto apply=[tag_id](AppState& v){if(auto* current=ActiveTab(v)) ToggleTagWithUndo(v,tag_id,SelectedFullPaths(*current));};
                    if(!DeferContentSelection(*s,apply)) apply(*s);
                }
                break;
            }
            case A::ClearTags: {
                auto apply=[](AppState& v){if(auto* current=ActiveTab(v)) ClearTagsWithUndo(v,SelectedFullPaths(*current));};
                if(!DeferContentSelection(*s,apply)) apply(*s);
                break;
            }
            case A::TagPanel:
                if (tab->SelectedCount() > 0) ShowTagPicker(*s, KeyboardMenuPoint(*s));
                break;
            case A::Back: GoBack(*s); break;
            case A::ContextMenu: ShowKeyboardContextMenu(*s); break;
            case A::Maximize: ShowWindow(hwnd, IsZoomed(hwnd) ? SW_RESTORE : SW_MAXIMIZE); break;
            // Posted: the window must not be created or destroyed under this handler.
            case A::NewWindow: PostMessageW(hwnd, WM_NEW_WINDOW, 0, 0); break;
            case A::CloseWindow: PostMessageW(hwnd, WM_CLOSE, 0, 0); break;
            case A::MoveTabToNewWindow: PostMessageW(hwnd, WM_MOVE_TAB_TO_NEW_WINDOW, 0, 0); break;
            // The other windows go away; this one (whose handler runs) stays.
            case A::MergeWindows: MergeAllWindows(*s); break;
            // The commands check the selection themselves (as their menu rows).
            case A::GroupIntoFolder: DispatchMenuCommand(*s, app::CmdGroupIntoFolder); break;
            case A::DissolveFolder: DispatchMenuCommand(*s, app::CmdDissolveFolder); break;
            case A::None: break;
            }
        } else if (ctrl && !shift && !alt && wParam == L'J') {
            AiAssistantHotkey(*s);   // Pulse 助手: open + focus, focus, or collapse
        } else if (ctrl && !shift && !alt && wParam == L'B') {
            ToggleSidebarCollapsed(*s);
        } else if (ctrl && shift && !alt && wParam == L'B') {
            ToggleFavoritesBar(*s);
        } else if (ctrl && !shift && !alt && wParam == VK_OEM_2) {
            DispatchMenuCommand(*s, app::CmdShortcutHelp);  // 键盘快捷键 (toolbar More)
        } else if (ctrl && shift && !alt && wParam == L'T') {
            ReopenLastClosed(*s);  // the tabs or window closed last (M4c, M7f)
        } else if (ctrl && !shift && wParam == L'T') {
            OpenNewTab(*s);
        } else if (ctrl && wParam == L'K') {
            if (shift) ShowOmnibar(*s, OmnibarMode::Command);  // 命令栏: the ">" palette
            else ShowAddressSearch(*s);
        } else if (ctrl && wParam == L'P') {
            ShowOmnibar(*s, OmnibarMode::Project);
        } else if (ctrl && shift && wParam == L'F') {
            ShowAdvancedSearch(*s);
        } else if (ctrl && wParam == L'F') {
            if (IsAddressSearchResults(tab)) ShowAddressSearch(*s);
            else ShowFilterEditor(*s);
        } else if (wParam == VK_F6) {
            if (!CycleFavoritesBarFocus(*s)) CycleFocus(*s);   // panes, then 收藏栏
        } else if (ctrl && alt && wParam == L'C') {
            TransferToTarget(*s, false);
        } else if (ctrl && alt && wParam == L'X') {
            TransferToTarget(*s, true);
        } else if (ctrl && !shift && wParam == L'W') {
            CloseActiveTab(*s);  // Ctrl+Shift+W closes the window (ExplorerShortcut)
        } else if (ctrl && wParam == L'L') {
            ShowOmnibar(*s, OmnibarMode::Path);
        } else if (wParam == VK_F4) {
            ShowOmnibar(*s, OmnibarMode::Path);
        } else if (ctrl && shift && wParam == L'S') {
            StageSelection(*s);  // 加入暂存区: the clipboard is left alone
        } else if (ctrl && shift && wParam == L'C') {
            // As the 复制路径 rows that show this key: the selection, or the
            // folder when nothing is selected (background menu).
            DispatchMenuCommand(*s, app::CmdCopyPath);
        } else if (ctrl && wParam == L'C') {
            CollectToTray(*s, false);
        } else if (ctrl && wParam == L'X') {
            CollectToTray(*s, true);
        } else if (ctrl && wParam == L'V') {
            PasteIntoCurrent(*s);
        } else if (ctrl && wParam == L'Z') {
            if (s->ops.CanUndo()) s->ops.Undo();
            else InvalidateRect(hwnd, nullptr, FALSE);
        } else if (ctrl && shift && wParam == L'R') {
            ShowBatchRename(*s);
        } else if (shift && !ctrl && wParam == VK_F2) {
            ShowAliasOverlay(*s);   // 别名
        } else if (wParam == VK_F2) {
            RenameSelected(*s);
        } else if (wParam == VK_F7) {
            CreateNewItem(*s, true); // 新建文件夹并进入重命名（ui.md §7.9）
        } else if (alt && wParam == VK_RETURN) {
            DispatchMenuCommand(*s, app::CmdProperties);
        } else if (wParam == VK_DELETE) {
            DeleteSelected(*s, (GetKeyState(VK_SHIFT) & 0x8000) != 0);
        } else if (wParam == VK_RETURN) {
            if (!HandleDetailsTreeKey(*s, vm.pane, VK_RETURN)) OpenSelected(*s);
        } else if (alt && wParam == VK_UP) {
            GoUp(*s);
        } else if (alt && wParam == VK_LEFT) {
            GoBack(*s);
        } else if (alt && wParam == VK_RIGHT) {
            GoForward(*s);
        } else if (wParam == VK_F5) {
            DispatchMenuCommand(*s, app::CmdRefresh);
        } else if (wParam == VK_F1) {
            s->appPrefs.show_status_performance = !s->appPrefs.show_status_performance;
            s->showFps = s->forceStatusPerformance || s->appPrefs.show_status_performance;
            s->appPrefs.Save();
            InvalidateRect(hwnd, nullptr, FALSE);
        } else if (wParam == VK_SPACE && (ctrl || shift) && !alt && vm.pane.EntryCount() > 0) {
            // Ctrl+Space toggles the focused item; Shift+Space selects from
            // the anchor to it. Plain Space stays Quick Look.
            const int focus = tab->HasFocusHint() ? vm.pane.SourceIndex(0) : tab->FocusIndex();
            if (focus >= 0) {
                if (ctrl && !shift) {
                    tab->ToggleSelect(focus);
                } else {
                    const int anchor = tab->selection_anchor >= 0 ? tab->selection_anchor : focus;
                    if (ctrl) tab->AddRange(anchor, focus, &vm.pane);
                    else tab->SelectRange(anchor, focus, &vm.pane);
                    tab->SetCaret(focus);
                }
                EnsureRowVisible(*s, *tab, focus);
            }
        } else if (wParam == VK_SPACE) {
            // A hover card up: Space opens its file, not the selection.
            if (!OpenHoverPreviewInQuickLook(*s)) ToggleQuickPreview(*s);
        } else if (ctrl && shift && wParam == L'A') {
            DispatchMenuCommand(*s, app::CmdSelectWildcard);
        } else if (ctrl && wParam == L'A') {
            DispatchMenuCommand(*s, app::CmdSelectAll);
        } else if (ctrl && wParam == L'I') {
            DispatchMenuCommand(*s, app::CmdInvertSelection);
        } else if (wParam == VK_ESCAPE) {
            s->contentSelectionAction.reset();
            if (tab->search_content_active || tab->search_awaiting_content || tab->search_live_generation) {
                CancelActiveContentSearch(*s, *tab);
                InvalidateRect(hwnd, nullptr, FALSE);
            } else {
                tab->ClearSelection();
                ResetMarquee(*s);
                s->clickCollapseIndex = -1;
            }
        } else if (!shift && !ctrl && (((wParam == VK_LEFT || wParam == VK_RIGHT) &&
                               HandleColumnArrowKey(*s, wParam == VK_RIGHT)) ||
                              HandleDetailsTreeKey(*s, vm.pane, wParam))) {
            CancelScrollAnimation(*s);
        } else if (wParam == VK_DOWN || wParam == VK_UP ||
                   wParam == VK_LEFT || wParam == VK_RIGHT) {
            CancelScrollAnimation(*s);
            {
                const int n = static_cast<int>(vm.pane.EntryCount());
                if (n > 0) {
                    int view = vm.pane.ViewIndex(tab->FocusIndex());
                    if (view < 0) view = 0;
                    const int dx = wParam == VK_LEFT ? -1 : (wParam == VK_RIGHT ? 1 : 0);
                    const int dy = wParam == VK_UP ? -1 : (wParam == VK_DOWN ? 1 : 0);
                    // The first arrow press selects the focused (not yet
                    // selected) first row instead of stepping past it.
                    // Ctrl moves the focus itself, so it steps on.
                    const int next = tab->HasFocusHint() && !ctrl ? 0
                        : SkipDetailsTreePlaceholders(vm.pane,
                              s->renderer.MoveViewIndex(vm.pane, FocusedPaneRect(*s), view, dx, dy),
                              dy != 0 ? dy : dx);
                    MoveKeyboardFocus(*s, *tab, vm.pane, next, ctrl, shift);
                }
            }
        } else if (wParam == VK_NEXT) {
            CancelScrollAnimation(*s);
            {
                const int n = static_cast<int>(vm.pane.EntryCount());
                const int page = s->renderer.PageDelta(vm.pane, FocusedPaneRect(*s));
                if (n > 0) {
                    int view = vm.pane.ViewIndex(tab->FocusIndex());
                    if (view < 0) view = 0;
                    const int next = SkipDetailsTreePlaceholders(vm.pane, std::min(n - 1, view + page), 1);
                    MoveKeyboardFocus(*s, *tab, vm.pane, next, ctrl, shift);
                }
            }
        } else if (wParam == VK_PRIOR) {
            CancelScrollAnimation(*s);
            {
                const int n = static_cast<int>(vm.pane.EntryCount());
                const int page = s->renderer.PageDelta(vm.pane, FocusedPaneRect(*s));
                if (n > 0) {
                    int view = vm.pane.ViewIndex(tab->FocusIndex());
                    if (view < 0) view = 0;
                    const int next = SkipDetailsTreePlaceholders(vm.pane, std::max(0, view - page), -1);
                    MoveKeyboardFocus(*s, *tab, vm.pane, next, ctrl, shift);
                }
            }
        } else if (wParam == VK_HOME) {
            CancelScrollAnimation(*s);
            if (vm.pane.EntryCount() > 0) {
                MoveKeyboardFocus(*s, *tab, vm.pane, 0, ctrl, shift);
                tab->scroll_y = 0;
            }
        } else if (wParam == VK_END) {
            CancelScrollAnimation(*s);
            if (vm.pane.EntryCount() > 0) {
                MoveKeyboardFocus(*s, *tab, vm.pane, SkipDetailsTreePlaceholders(
                    vm.pane, static_cast<int>(vm.pane.EntryCount()) - 1, -1), ctrl, shift);
                s->scrollTargetY = tab->scroll_y;
                MaybePrefetchSearchPage(*s);
            }
        } else {
            handled = false;
        }

        if (handled) {
            ClampScroll(*s);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace pulse
