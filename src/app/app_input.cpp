// app_input.cpp — extracted from app_main.cpp.
#include "app_internal.h"
#include "../ui/lumatext_renderer.h"
#include "../ui/fluent_menu.h"
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
#include "snapshot_patch.h"
#include "session.h"
#include "context_menu.h"
#include "batch_rename.h"
#include "link_resolve.h"
#include "resource.h"
#include "../ops/clipboard.h"
#include "../ipc/ctx_menu_util.h"
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

using namespace pulse;

namespace pulse {
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
    s.hoverRegion = static_cast<int>(ui::HitTestResult::Splitter);
    s.hoverControlIndex = s.splitterDragIndex;
    return true;
}
void ClearDropFeedback(AppState& s) {
    s.dropRow = -1;
    s.dropPaneIndex = -1;
    s.dropHeader = false;
    s.dropBreadcrumb = -1;
    s.dropSidebar = -1;
    s.dropTray = false;
    s.dropBadge.clear();
    s.dropDestDir.clear();
    s.springRow = -1;
    s.springStart = 0;
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

// Resolves the drop target under pt (client coords), updates feedback state,
// and returns the DROPEFFECT_* to report back. Also drives the 800ms
// spring-loaded folder enter and Esc-back.
DWORD ResolveDropTarget(AppState& s, const std::vector<std::wstring>& sources,
                               POINT pt, DWORD key_state, DWORD allowed) {
    s.dropRow = -1;
    s.dropPaneIndex = -1;
    s.dropHeader = false;
    s.dropBreadcrumb = -1;
    s.dropSidebar = -1;
    s.dropTray = false;
    s.dropDestDir.clear();
    s.dropBadge.clear();

    // Esc after a spring-loaded enter: go back instead of cancelling (self drags).
    if ((GetAsyncKeyState(VK_ESCAPE) & 0x8000) && s.springEntered) {
        GoBack(s);
        s.springEntered = false;
        return DROPEFFECT_NONE;
    }

    ui::WindowViewModel vm = BuildVm(s);
    D2D1_RECT_F rect = D2D1::RectF(0, 0, (float)s.compositor.Width(), (float)s.compositor.Height());
    ui::HitTestResult hit = s.renderer.HitTest(vm, rect, (float)pt.x, (float)pt.y);

    app::Pane* hitPane = PaneAtSlot(s, hit.pane_index);
    app::Tab* tab = hitPane ? hitPane->ActiveTab() : ActiveTab(s);
    if (!tab) return DROPEFFECT_NONE;

    std::wstring destName;
    if (hit.region == ui::HitTestResult::Row && hit.index >= 0 &&
        tab->snapshot && hit.index < (int)tab->snapshot->size() &&
        (*tab->snapshot)[hit.index].is_dir) {
        std::wstring full = tab->current_path;
        if (!full.ends_with(L"\\")) full += L"\\";
        full += (*tab->snapshot)[hit.index].name;
        s.dropDestDir = full;
        s.dropRow = hit.index;
        s.dropPaneIndex = hit.pane_index;
        destName = (*tab->snapshot)[hit.index].name;

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
    } else if (hit.region == ui::HitTestResult::BreadcrumbSegment && !hit.path.empty()) {
        s.dropDestDir = hit.path;
        s.dropBreadcrumb = hit.index;
        destName = BaseName(hit.path);
        s.springRow = -1;
    } else if (hit.region == ui::HitTestResult::SidebarItem && !hit.path.empty()) {
        if (hit.path.starts_with(L"pulse:tag:")) {
            s.dropDestDir = hit.path;
            s.dropSidebar = hit.index;
            destName.clear();
            s.springRow = -1;
            s.dropBadge = L"打标签";
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
            s.dropBadge = L"打开 " + destName;
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
               hit.region == ui::HitTestResult::Scrollbar) {
        s.dropDestDir = tab->current_path;
        s.dropPaneIndex = hit.pane_index;
        destName = BaseName(tab->current_path);
        s.springRow = -1;
    }
    // Tray zone: bottom card of the sidebar = staging, not a move.
    D2D1_RECT_F sb = s.renderer.SidebarRect(rect.right, rect.bottom);
    if (s.dropDestDir.empty() && pt.x < sb.right) {
        ui::WindowViewModel trayVm = BuildVm(s);
        D2D1_RECT_F trayRc = s.renderer.StagingTrayRect(trayVm, rect.right, rect.bottom);
        if (pt.x >= trayRc.left && pt.x < trayRc.right &&
            pt.y >= trayRc.top && pt.y < trayRc.bottom) {
            s.dropTray = true;
            s.springRow = -1;
            s.dropBadge = L"暂存到托盘";
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

    DWORD effect = ui::ComputeDropEffect(key_state, sources.front(), s.dropDestDir, allowed);
    s.dropBadge = (effect == DROPEFFECT_MOVE ? L"移动到 " : L"复制到 ") + destName;
    s.dropBadgeX = (float)pt.x;
    s.dropBadgeY = (float)pt.y;
    InvalidateRect(s.hwnd, nullptr, FALSE);
    return effect;
}

DWORD DropExecute(AppState& s, const std::vector<std::wstring>& sources,
                         POINT pt, DWORD key_state, DWORD preferred) {
    // Resolve once more for the final position.
    DWORD effect = ResolveDropTarget(s, sources, pt, key_state, DROPEFFECT_COPY | DROPEFFECT_MOVE);
    std::wstring dest = s.dropDestDir;
    bool tray = s.dropTray;
    const bool header = s.dropHeader;
    const int header_pane = s.dropPaneIndex;
    ClearDropFeedback(s);
    s.springEntered = false;

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
            return DROPEFFECT_COPY;
        }
        if (app::Pane* pane = PaneAtSlot(s, header_pane)) {
            if (app::Tab* tab = pane->ActiveTab()) dest = tab->current_path;
        }
        if (dest.empty() || fs::IsVirtualPath(dest)) return DROPEFFECT_NONE;
        effect = ui::ComputeDropEffect(key_state, sources.front(), dest,
                                       DROPEFFECT_COPY | DROPEFFECT_MOVE);
    }

    if (tray) {
        std::vector<std::wstring> paths;
        for (auto& p : sources) paths.push_back(fs::NormalizePath(p));
        s.tray.Collect(paths, (preferred & DROPEFFECT_MOVE) != 0);
        ops::WriteClipboard(sources, (preferred & DROPEFFECT_MOVE) != 0);
        InvalidateRect(s.hwnd, nullptr, FALSE);
        return DROPEFFECT_COPY;
    }
    if (dest.starts_with(L"pulse:tag:")) {
        std::wstring kind, rest;
        app::ParsePulsePath(dest, &kind, &rest);
        const int ti = s.places.FindTagIndex(s.places.ResolveTagRef(rest));
        std::vector<app::TagAdsUpdate> ads_updates;
        s.places.SetTaggedBatch(ti, sources, true, &ads_updates);
        QueueTagAds(s, std::move(ads_updates));
        InvalidateRect(s.hwnd, nullptr, FALSE);
        return DROPEFFECT_COPY;
    }
    if (dest.empty() || effect == DROPEFFECT_NONE || fs::IsVirtualPath(dest)) return DROPEFFECT_NONE;

    ops::OpRequest req;
    req.type = (effect == DROPEFFECT_MOVE) ? ops::OpType::Move : ops::OpType::Copy;
    req.dest_dir = fs::NormalizePath(dest);
    for (auto& p : sources) req.sources.push_back(fs::NormalizePath(p));
    return SubmitWithConflictResolution(s, std::move(req)) ? effect : DROPEFFECT_NONE;
}

// Starts the modal OLE drag-out for the selected entries.
void StartDragOut(AppState& s) {
    app::Tab* tab = ActiveTab(s);
    if (!tab || !tab->snapshot) return;
    std::vector<std::wstring> paths = SelectedFullPaths(*tab);
    if (paths.empty()) return;
    for (auto& path : paths) path = ClipboardPath(path);

    s.clickCollapseIndex = -1;
    CancelScrollAnimation(s); // DoDragDrop's modal loop coexists with on-demand render
    DWORD effect = ui::DoFileDragDrop(paths, DROPEFFECT_COPY | DROPEFFECT_MOVE,
        [&s] { return s.springEntered; }); // Esc = 退回 when spring-entered
    s.springEntered = false;
    ClearDropFeedback(s);
    if (effect == DROPEFFECT_MOVE) {
        // The target took the file; refresh the listing.
        s.store.MarkDirty(tab->current_path);
        RefreshActiveTab(s);
    }
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

float MaxScrollForActivePane(AppState& s, ui::PaneViewModel* out) {
    if (!s.pane || !s.pane->ActiveTab()) return 0.0f;
    ui::PaneViewModel pane;
    app::FillPaneViewModel(pane, *s.pane, &s.places);
    const float max_scroll = s.renderer.MaxScrollForPane(pane, FocusedPaneRect(s));
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
}

void EnsureRowVisible(AppState& s, app::Tab& tab, int index) {
    if (index < 0) return;
    const D2D1_RECT_F list = ListRect(s);
    int viewRow = index;
    ui::PaneViewModel pane;
    MaxScrollForActivePane(s, &pane);
    if (!pane.filter_text.empty()) {
        viewRow = pane.ViewIndex(index);
        if (viewRow < 0) return;
    }
    const D2D1_RECT_F item = s.renderer.ItemRectInPane(pane, FocusedPaneRect(s), viewRow);
    if (item.top < list.top) tab.scroll_y += item.top - list.top;
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
    s.marqueePending = false;
    s.marqueeActive = false;
    s.marqueeAdditive = false;
    s.marqueeBase.clear();
}

void ApplyMarqueeSelection(AppState& s) {
    app::Tab* tab = ActiveTab(s);
    if (!tab || !tab->snapshot) return;
    const int n = static_cast<int>(tab->snapshot->size());
    const D2D1_RECT_F list = ListRect(s);
    const float left = static_cast<float>(std::min(s.marqueeStart.x, s.marqueeCur.x));
    const float top = static_cast<float>(std::min(s.marqueeStart.y, s.marqueeCur.y));
    const float right = static_cast<float>(std::max(s.marqueeStart.x, s.marqueeCur.x));
    const float bottom = static_cast<float>(std::max(s.marqueeStart.y, s.marqueeCur.y));
    const float clipL = std::max(left, list.left);
    const float clipT = std::max(top, list.top);
    const float clipR = std::min(right, list.right);
    const float clipB = std::min(bottom, list.bottom);

    tab->all_selected = false;
    tab->selected.clear();
    if (s.marqueeAdditive) tab->selected = s.marqueeBase;

    if (n > 0 && clipR > clipL && clipB > clipT) {
        ui::PaneViewModel pane;
        app::FillPaneViewModel(pane, *s.pane, &s.places);
        const auto [first, last] = s.renderer.VisibleRangeInPane(pane, FocusedPaneRect(s));
        const D2D1_RECT_F marquee = D2D1::RectF(clipL, clipT, clipR, clipB);
        for (int view = first; view >= 0 && view <= last; ++view) {
            const D2D1_RECT_F item = s.renderer.ItemRectInPane(pane, FocusedPaneRect(s), view);
            if (item.right <= marquee.left || item.left >= marquee.right ||
                item.bottom <= marquee.top || item.top >= marquee.bottom) continue;
            const int source = pane.SourceIndex(view);
            if (source >= 0) tab->selected.insert(source);
        }
    }

    if (tab->selected.empty()) {
        tab->selected_index = -1;
        return;
    }
    ui::PaneViewModel focusPane;
    app::FillPaneViewModel(focusPane, *s.pane, &s.places);
    int focus = s.renderer.ItemFromPointInPane(focusPane, FocusedPaneRect(s),
                                               static_cast<float>(s.marqueeCur.x),
                                               static_cast<float>(s.marqueeCur.y));
    if (tab->selected.contains(focus)) tab->selected_index = focus;
    else tab->selected_index = *tab->selected.begin();
    if (tab->selection_anchor < 0) tab->selection_anchor = tab->selected_index;
    if (static_cast<int>(tab->selected.size()) == n) {
        tab->all_selected = true;
        tab->selected.clear();
    }
}

void HandleListRowClick(AppState& s, int index, bool ctrl, bool shift) {
    app::Tab* tab = ActiveTab(s);
    if (!tab) return;
    s.clickCollapseIndex = -1;
    if (shift) {
        const int anchor = tab->selection_anchor >= 0 ? tab->selection_anchor
            : (tab->selected_index >= 0 ? tab->selected_index : index);
        tab->SelectRange(anchor, index);
    } else if (ctrl) {
        tab->ToggleSelect(index);
    } else if (tab->IsSelected(index) && tab->SelectedCount() > 1) {
        tab->selected_index = index;
        s.clickCollapseIndex = index;
    } else {
        tab->SelectOnly(index);
    }
}

void CancelRenameClick(AppState& s) {
    s.renameClickCandidate = false;
    s.renameClickPane = nullptr;
    s.renameClickTab = nullptr;
    s.renameClickIndex = -1;
    s.renameClickDue = 0;
    s.renameClickPath.clear();
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

void CancelScrollAnimation(AppState& s) {
    s.scrollAnimating = false;
}

// --- Tag slide animation ------------------------------------------------------
// SortableJS-style reorder motion: a quick easeOutCubic with no overshoot.
// On 32px rows an overshoot spring (the old Pivot indicator curve) reads as
// wobble, especially when fast drags restart the track mid-flight.
float TagEaseOutCubic(float x) {
    const float u = 1.0f - x;
    return 1.0f - u * u * u;
}

float TagEaseInOutQuad(float t) { // used by the window-tab slide animation
    const float u = -2.0f * t + 2.0f;
    return t < 0.5f ? 2.0f * t * t : 1.0f - u * u * 0.5f;
}

void TickTagTransitions(AppState& s) {
    const auto now = std::chrono::steady_clock::now();
    for (auto it = s.tagTracks.begin(); it != s.tagTracks.end();) {
        const float t = std::chrono::duration<float, std::milli>(now - it->second.t0).count()
            / static_cast<float>(it->second.durationMs);
        if (t >= 1.0f) {
            s.tagOffsets.erase(it->first);
            it = s.tagTracks.erase(it);
        } else {
            s.tagOffsets[it->first] = it->second.start * (1.0f - TagEaseOutCubic(t));
            ++it;
        }
    }
    // Insertion indicator chase (same 180ms easeOutCubic as the slides).
    if (s.tagGapFrom != s.tagGapTo) {
        const float t = std::chrono::duration<float, std::milli>(now - s.tagGapT0).count()
            / 180.0f;
        if (t >= 1.0f) {
            s.tagGapLineY = s.tagGapTo;
            s.tagGapFrom = s.tagGapTo;
        } else {
            s.tagGapLineY = s.tagGapFrom +
                (s.tagGapTo - s.tagGapFrom) * TagEaseOutCubic(t);
        }
    }
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

void UpdateSmoothScroll(AppState& s);

void StartSmoothScroll(AppState& s, float delta) {
    app::Tab* tab = ActiveTab(s);
    if (!tab) return;
    ClampScroll(s);

    // Preserve the distance from earlier wheel pulses. Restarting from the
    // partially animated position discards most of a fast wheel gesture.
    if (s.scrollAnimating) {
        UpdateSmoothScroll(s);
    } else {
        s.scrollTargetY = tab->scroll_y;
        s.scrollLastUpdateTime = std::chrono::steady_clock::now();
    }

    const float maxScroll = MaxScrollForActivePane(s);
    s.scrollTargetY = std::clamp(s.scrollTargetY + delta, 0.0f, maxScroll);
    s.scrollAnimating = true;
    MaybePrefetchSearchPage(s);
}

void UpdateSmoothScroll(AppState& s) {
    if (!s.scrollAnimating) return;
    app::Tab* tab = ActiveTab(s);
    if (!tab) { s.scrollAnimating = false; return; }
    auto now = std::chrono::steady_clock::now();
    const double elapsed = std::clamp(
        std::chrono::duration<double, std::milli>(now - s.scrollLastUpdateTime).count(),
        0.0, 50.0);
    s.scrollLastUpdateTime = now;

    const float remaining = s.scrollTargetY - tab->scroll_y;
    if (std::abs(remaining) <= 0.35f) {
        tab->scroll_y = s.scrollTargetY;
        s.scrollAnimating = false;
    } else {
        // Exponential response is independent of timer jitter and accepts a
        // moving target without resetting its easing curve on every pulse.
        const float response = 1.0f - static_cast<float>(
            std::exp(-elapsed / AppState::kScrollResponseMs));
        tab->scroll_y += remaining * response;
    }
    ClampScroll(s);
}

LRESULT HandleMouseMove(AppState* s, HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        if (!s) return DefWindowProcW(hwnd, msg, wParam, lParam);
        int mx = GET_X_LPARAM(lParam);
        int my = GET_Y_LPARAM(lParam);
        s->hoverPoint = POINT{ mx, my };
        s->bloom_accent.SetPointer(static_cast<float>(mx), static_cast<float>(my), true);

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
                if (s->columnResizePane >= 0 &&
                    s->columnResizePane < static_cast<int>(resizeVm.pane_slots.size())) {
                    paneRect = resizeVm.pane_slots[
                        static_cast<size_t>(s->columnResizePane)].rect;
                }
                app::Pane* resizePane = PaneAtSlot(*s, s->columnResizePane);
                app::Tab* resizeTab = resizePane ? resizePane->ActiveTab() : nullptr;
                if (resizeTab) {
                    std::wstring kind;
                    app::ParsePulsePath(resizeTab->current_path, &kind, nullptr);
                    if (kind == L"search") {
                        resizeTab->search_column_dividers =
                            s->renderer.ResizeSearchColumnDivider(
                                paneRect, resizeTab->search_column_dividers,
                                s->columnResizeIndex, static_cast<float>(mx));
                    } else {
                        resizeTab->details_column_dividers =
                            s->renderer.ResizeDetailsColumnDivider(
                                paneRect, resizeTab->details_column_dividers,
                                s->columnResizeIndex, static_cast<float>(mx));
                    }
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
                s->detailsPanelWidth = std::clamp(width, 300.0f, 480.0f);
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
            if (!tab || (GetKeyState(VK_LBUTTON) & 0x8000) == 0) {
                s->scrollbarDragging = false;
                s->scrollbarHorizontal = false;
                if (GetCapture() == hwnd) ReleaseCapture();
            } else {
                ui::WindowViewModel dragVm = BuildVm(*s);
                D2D1_RECT_F track{}, thumb{};
                float maxScroll = 0.0f;
                if (s->scrollbarHorizontal &&
                    HorizontalScrollbarGeometry(*s, dragVm.pane, track, thumb, maxScroll)) {
                    const float travel = std::max(1.0f,
                        (track.right - track.left) - (thumb.right - thumb.left));
                    tab->scroll_x = std::clamp(s->scrollbarDragStartScroll +
                        (mx - s->scrollbarDragStartX) * maxScroll / travel, 0.0f, maxScroll);
                    InvalidateRect(hwnd, nullptr, FALSE);
                } else if (!s->scrollbarHorizontal &&
                           ScrollbarGeometry(*s, dragVm.pane, track, thumb, maxScroll)) {
                    const float travel = std::max(1.0f,
                        (track.bottom - track.top) - (thumb.bottom - thumb.top));
                    tab->scroll_y = std::clamp(
                        s->scrollbarDragStartScroll
                            + (my - s->scrollbarDragStartY) * maxScroll / travel,
                        0.0f, maxScroll);
                    s->scrollTargetY = tab->scroll_y;
                    MaybePrefetchSearchPage(*s);
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
                return 0;
            }
        }

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
            if ((GetKeyState(VK_LBUTTON) & 0x8000) == 0) {
                if (s->marqueeActive) ApplyMarqueeSelection(*s);
                ResetMarquee(*s);
            } else {
                s->marqueeCur = POINT{ mx, my };
                if (!s->marqueeActive &&
                    (std::abs(mx - s->marqueeStart.x) >= GetSystemMetrics(SM_CXDRAG) ||
                     std::abs(my - s->marqueeStart.y) >= GetSystemMetrics(SM_CYDRAG))) {
                    s->marqueeActive = true;
                    s->marqueePending = false;
                }
                if (s->marqueeActive) ApplyMarqueeSelection(*s);
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
            } else if (std::abs(mx - s->dragStartPt.x) >= GetSystemMetrics(SM_CXDRAG) ||
                       std::abs(my - s->dragStartPt.y) >= GetSystemMetrics(SM_CYDRAG)) {
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
                            s->dropSidebar = target.index;
                        }
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
        if (s->tagDragPending || s->tagDragActive) {
            if ((GetKeyState(VK_LBUTTON) & 0x8000) == 0) {
                s->tagDragPending = false;
                s->tagDragActive = false;
                s->tagDragTag = -1;
                s->tagDragPath.clear();
                s->tagOrder.clear();
                s->tagGapVisible = false;
                s->tagGapFrom = s->tagGapTo = s->tagGapLineY = 0.0f;
            } else {
                if (!s->tagDragActive &&
                    std::abs(my - s->tagDragStartPt.y) >= 2 && // TabBar starts on a 2px delta
                    s->tagDragTag >= 0 &&
                    s->tagDragTag < static_cast<int>(s->places.tags.size()) &&
                    s->renderer.EffectiveSidebarWidth(static_cast<float>(s->compositor.Width()))
                        > 60.0f * s->scale) {
                    // Capture the tag flow geometry once so the gesture can map
                    // cursor deltas 1:1 and evaluate sibling-center crossings.
                    ui::WindowViewModel vm0 = BuildVm(*s);
                    const float w0 = static_cast<float>(s->compositor.Width());
                    const float h0 = static_cast<float>(s->compositor.Height());
                    int g0 = -1;
                    for (int g = 0; g < static_cast<int>(vm0.sidebar.size()); ++g) {
                        if (!vm0.sidebar[g].items.empty() && vm0.sidebar[g].items[0].is_tag) {
                            g0 = g;
                            break;
                        }
                    }
                    const int n0 = g0 >= 0 ? static_cast<int>(vm0.sidebar[g0].items.size()) : 0;
                    D2D1_RECT_F first{}, last{}, self{};
                    if (n0 == static_cast<int>(s->places.tags.size()) &&
                        s->renderer.TagItemRect(vm0, w0, h0, g0, 0, &first) &&
                        s->renderer.TagItemRect(vm0, w0, h0, g0, n0 - 1, &last) &&
                        s->renderer.TagItemRect(vm0, w0, h0, g0, s->tagDragTag, &self)) {
                        s->tagDragActive = true;
                        s->tagFlowTop = first.top;
                        s->tagFlowBottom = last.bottom;
                        s->tagSlotH = self.bottom - self.top;
                        s->tagPitch = n0 >= 2 ? (last.top - first.top) / (n0 - 1) : s->tagSlotH;
                        s->tagDragFloatTop = self.top;
                        s->tagDragGrabDy = static_cast<float>(my) - self.top;
                        s->tagDragY = (self.top + self.bottom) * 0.5f;
                        s->tagDragLastY = my;
                        s->tagTracks.clear();
                        s->tagOffsets.clear();
                        s->tagOrder.resize(s->places.tags.size());
                        for (int i = 0; i < static_cast<int>(s->tagOrder.size()); ++i)
                            s->tagOrder[i] = i;
                        // The indicator starts at the grabbed slot, no chase yet.
                        s->tagGapVisible = true;
                        s->tagGapLineY = self.top;
                        s->tagGapFrom = s->tagGapTo = self.top;
                        s->tagGapT0 = std::chrono::steady_clock::now();
                    } else {
                        s->tagDragPending = false; // geometry unavailable: plain click
                    }
                }
                if (s->tagDragActive) {
                    const int dy = my - s->tagDragLastY;
                    if (std::abs(dy) >= 2) { // 2px deadzone against jitter, as in TabBar
                        s->tagDragLastY = my;
                        // Grab-relative position keeps the tag glued to the cursor
                        // even after clamping at a flow edge (TabBar edge behavior).
                        s->tagDragFloatTop = std::clamp(
                            static_cast<float>(my) - s->tagDragGrabDy, s->tagFlowTop,
                            std::max(s->tagFlowTop, s->tagFlowBottom - s->tagSlotH));
                        s->tagDragY = s->tagDragFloatTop + s->tagSlotH * 0.5f;
                        int cur = -1;
                        for (int p = 0; p < static_cast<int>(s->tagOrder.size()); ++p)
                            if (s->tagOrder[p] == s->tagDragTag) { cur = p; break; }
                        const int n = static_cast<int>(s->tagOrder.size());
                        if (cur >= 0 && s->tagPitch > 0.0f) {
                            // SortableJS-style insertion target: the dragged card's
                            // center maps straight to a flow slot, so a fast flick
                            // crosses several rows in a single mouse event.
                            const int target = std::clamp(
                                static_cast<int>((s->tagDragY - s->tagFlowTop) / s->tagPitch),
                                0, n - 1);
                            if (target != cur) {
                                const auto now = std::chrono::steady_clock::now();
                                std::vector<int> next = s->tagOrder;
                                const int dragged = next[static_cast<size_t>(cur)];
                                next.erase(next.begin() + cur);
                                next.insert(next.begin() + target, dragged);
                                // FLIP: every shifted sibling slides from its current
                                // (possibly mid-flight) visual position to its new slot.
                                for (int pos = 0; pos < n; ++pos) {
                                    const int idx = next[static_cast<size_t>(pos)];
                                    if (idx == dragged) continue;
                                    int oldPos = -1;
                                    for (int p = 0; p < n; ++p)
                                        if (s->tagOrder[static_cast<size_t>(p)] == idx) {
                                            oldPos = p;
                                            return DefWindowProcW(hwnd, msg, wParam, lParam);
                                        }
                                    if (oldPos < 0 || oldPos == pos) continue;
                                    const std::wstring& label =
                                        s->places.tags[static_cast<size_t>(idx)].name;
                                    const auto off = s->tagOffsets.find(label);
                                    const float curOff =
                                        off != s->tagOffsets.end() ? off->second : 0.0f;
                                    const float start = static_cast<float>(oldPos) + curOff
                                        - static_cast<float>(pos);
                                    if (std::abs(start) >= 0.01f) {
                                        s->tagTracks[label] = AppState::TagTrack{
                                            start, 180, now };
                                        s->tagOffsets[label] = start;
                                    }
                                }
                                s->tagOrder = std::move(next);
                                // The insertion indicator chases the new gap.
                                const float lineTarget =
                                    s->tagFlowTop + static_cast<float>(target) * s->tagPitch;
                                if (lineTarget != s->tagGapTo) {
                                    s->tagGapFrom = s->tagGapLineY;
                                    s->tagGapTo = lineTarget;
                                    s->tagGapT0 = now;
                                }
                            }
                        }
                    }
                    InvalidateRect(hwnd, nullptr, FALSE);
                    return 0;
                }
            }
        }

        TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, hwnd, 0 };
        TrackMouseEvent(&tme);

        ui::WindowViewModel vm = BuildVm(*s);
        D2D1_RECT_F rect = D2D1::RectF(0, 0, (float)s->compositor.Width(), (float)s->compositor.Height());
        ui::HitTestResult hit = s->renderer.HitTest(vm, rect, (float)mx, (float)my);
        const int newRegion = static_cast<int>(hit.region);
        if (newRegion != s->hoverRegion || hit.index != s->hoverControlIndex ||
            hit.pane_index != s->hoverPaneIndex) {
            s->hoverRegion = newRegion;
            s->hoverControlIndex = hit.index;
            s->hoverSubIndex = hit.sub_index;
            s->hoverSince = GetTickCount64();
            s->tooltipText.clear();
            if (hit.region == ui::HitTestResult::SidebarItem && fs::IsUncPath(hit.path))
                RequestUncProbe(*s, hit.path);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        int newHover = (hit.region == ui::HitTestResult::Row ||
                        hit.region == ui::HitTestResult::RowStar ||
                        hit.region == ui::HitTestResult::RowNewTab ||
                        hit.region == ui::HitTestResult::RowMore) ? hit.index : -1;
        if (newHover != s->hoverRow || hit.pane_index != s->hoverPaneIndex) {
            s->hoverRow = newHover;
            s->hoverPaneIndex = hit.pane_index;
            s->ctxHoverSince = GetTickCount64();
            if (newHover < 0) s->ctxHoverPrefetched.clear();
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        int newCrumb = (hit.region == ui::HitTestResult::BreadcrumbSegment) ? hit.index : -1;
        if (newCrumb != s->breadcrumbHover) {
            s->breadcrumbHover = newCrumb;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        D2D1_RECT_F content = s->renderer.ContentRect((float)s->compositor.Width(), (float)s->compositor.Height());
        float extra = 0.0f;
        if (app::Tab* tab = ActiveTab(*s); tab) {
            if (!tab->banner_message.empty()) extra = 36.0f * s->scale;
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
            s->scrollbarHovered = false;
            s->hoverRow = -1;
            s->hoverPaneIndex = -1;
            s->ctxHoverSince = 0;
            s->ctxHoverPrefetched.clear();
            s->breadcrumbHover = -1;
            s->hoverRegion = 0;
            s->hoverControlIndex = -1;
            s->hoverSubIndex = -1;
            s->hoverPath.clear();
            s->hoverSince = 0;
            s->tooltipText.clear();
            s->bloom_accent.SetPointer(0.0f, 0.0f, false);
            if (GetCapture() != hwnd) s->dragPending = false;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
}

LRESULT HandleLButtonDown(AppState* s, HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        if (!s) return DefWindowProcW(hwnd, msg, wParam, lParam);
        int mx = GET_X_LPARAM(lParam);
        int my = GET_Y_LPARAM(lParam);
        CancelRenameClick(*s);
        CancelScrollAnimation(*s);
        ui::WindowViewModel vm = BuildVm(*s);
        D2D1_RECT_F rect = D2D1::RectF(0, 0, (float)s->compositor.Width(), (float)s->compositor.Height());
        ui::HitTestResult hit = s->renderer.HitTest(vm, rect, (float)mx, (float)my);
        if (hit.pane_index >= 0) {
            if (app::Pane* p = PaneAtSlot(*s, hit.pane_index)) FocusPane(*s, p);
        }
        if (hit.region == ui::HitTestResult::DetailsResize) {
            s->dragPending = false;
            s->detailsPanelResizing = true;
            SetCapture(hwnd);
            return 0;
        }
        if (hit.region == ui::HitTestResult::DetailsPreview) {
            s->dragPending = false;
            return 0;
        } else if (hit.region == ui::HitTestResult::ColumnDivider) {
            if (s->renameIndex >= 0) HideRenameOverlay(*s, true);
            s->dragPending = false;
            s->columnResizing = true;
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
            s->splitterDragging = true;
            s->splitterDragIndex = hit.index;
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
            const bool hasGeometry = s->scrollbarHorizontal
                ? HorizontalScrollbarGeometry(*s, vm.pane, track, thumb, maxScroll)
                : ScrollbarGeometry(*s, vm.pane, track, thumb, maxScroll);
            if (tab && hasGeometry) {
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
                    if (s->scrollbarHorizontal) tab->scroll_x=value;
                    else { tab->scroll_y=value; s->scrollTargetY=value; MaybePrefetchSearchPage(*s); }
                }
                s->scrollbarDragging = true;
                s->scrollbarDragStartX = mx;
                s->scrollbarDragStartY = my;
                s->scrollbarDragStartScroll = s->scrollbarHorizontal ? tab->scroll_x : tab->scroll_y;
                SetCapture(hwnd);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
        } else if (hit.region == ui::HitTestResult::Row && hit.index >= 0) {
            const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
            app::Tab* tab = ActiveTab(*s);
            const bool renameCandidate = !ctrl && !shift && tab && !tab->net_readonly &&
                s->renameIndex < 0 && tab->SelectedCount() == 1 &&
                tab->selected_index == hit.index && tab->IsSelected(hit.index) &&
                PointInHitItemName(*s, vm, hit, static_cast<float>(mx), static_cast<float>(my));
            HandleListRowClick(*s, hit.index, ctrl, shift);
            if (renameCandidate && tab == ActiveTab(*s)) {
                s->renameClickCandidate = true;
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
                        return DefWindowProcW(hwnd, msg, wParam, lParam);
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
            SwitchTab(*s, hit.index);
            s->tabDragPending = true;
            s->tabDragging = false;
            s->tabDragFromChip = false;
            s->tabDragIndex = hit.index;
            s->tabDragStartPt = POINT{ mx, my };
            SetCapture(hwnd);
        } else if (hit.region == ui::HitTestResult::TabClose && hit.index >= 0) {
            CloseLayoutTab(*s, static_cast<size_t>(hit.index));
            InvalidateRect(hwnd, nullptr, FALSE);
        } else if (hit.region == ui::HitTestResult::TabNew) {
            NewTab(*s, NewTabPath(*s));
        } else if (hit.region == ui::HitTestResult::ThemeToggle) {
            ToggleTheme(*s);
        } else if (hit.region == ui::HitTestResult::SettingsButton) {
            OpenSettingsTab(*s, 0);
        } else if (hit.region == ui::HitTestResult::SettingsNav) {
            OpenSettingsTab(*s, hit.index);
        } else if (hit.region == ui::HitTestResult::SettingsToggle) {
            s->settings.ToggleUi(hit.index);
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
        } else if (hit.region == ui::HitTestResult::SettingsTrayIcon) {
            s->settings.TrayIconSize(hit.index);
            InvalidateRect(hwnd, nullptr, FALSE);
        } else if (hit.region == ui::HitTestResult::SettingsLanguage) {
            static constexpr const wchar_t* languages[] = {L"system", L"zh-CN", L"en-US"};
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
            s->settings.AddExclude();
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
            InvalidateRect(hwnd, nullptr, FALSE);
        } else if (hit.region == ui::HitTestResult::SettingsDiagnosticsAction) {
            s->settings.DiagnosticsAction(hit.index);
            InvalidateRect(hwnd, nullptr, FALSE);
        } else if (hit.region == ui::HitTestResult::SettingsUpdateAction) {
            if (hit.index == 0 && app::UpdateChecker::Enabled()) {
                if (s->update_checker.CheckAsync(hwnd, WM_UPDATE_RESULT))
                    s->update_result_ready = false;
                InvalidateRect(hwnd, nullptr, FALSE);
            } else if (hit.index == 1 && s->update_result_ready &&
                       s->update_result.update_available &&
                       s->update_result.download_page.starts_with(L"https://")) {
                ShellExecuteW(hwnd, L"open", s->update_result.download_page.c_str(),
                              nullptr, nullptr, SW_SHOWNORMAL);
            }
        } else if (hit.region == ui::HitTestResult::NavBack) {
            GoBack(*s);
        } else if (hit.region == ui::HitTestResult::NavForward) {
            GoForward(*s);
        } else if (hit.region == ui::HitTestResult::NavUp) {
            GoUp(*s);
        } else if (hit.region == ui::HitTestResult::NavRefresh) {
            RefreshActiveTab(*s);
        } else if (hit.region == ui::HitTestResult::NewButton) {
            ShowNewDropdown(*s);
        } else if (hit.region == ui::HitTestResult::BreadcrumbSegment) {
            // Empty path is the This PC segment; NavigateTo handles it.
            NavigateTo(*s, hit.path);
        } else if (hit.region == ui::HitTestResult::Copy) {
            CollectToTray(*s, false);
        } else if (hit.region == ui::HitTestResult::Cut) {
            CollectToTray(*s, true);
        } else if (hit.region == ui::HitTestResult::Paste) {
            PasteIntoCurrent(*s);
        } else if (hit.region == ui::HitTestResult::Rename) {
            ShowRenameOverlay(*s);
        } else if (hit.region == ui::HitTestResult::Delete) {
            DeleteSelected(*s, (GetKeyState(VK_SHIFT) & 0x8000) != 0);
        } else if (hit.region == ui::HitTestResult::TrayRelease) {
            ReleaseTrayBatch(*s, (size_t)hit.index);
        } else if (hit.region == ui::HitTestResult::TrayClose) {
            s->tray.RemoveBatch((size_t)hit.index);
            InvalidateRect(hwnd, nullptr, FALSE);
        } else if (hit.region == ui::HitTestResult::TrayItemRemove) {
            s->tray.RemoveItem((size_t)hit.index, (size_t)hit.sub_index);
            InvalidateRect(hwnd, nullptr, FALSE);
        } else if (hit.region == ui::HitTestResult::TrayClear) {
            s->tray.Clear();
            InvalidateRect(hwnd, nullptr, FALSE);
        } else if (hit.region == ui::HitTestResult::RowStar && hit.index >= 0) {
            if (app::Tab* tab = ActiveTab(*s)) {
                const std::wstring p = EntryFullPath(*tab, hit.index);
                if (!p.empty() && tab->snapshot &&
                    hit.index < static_cast<int>(tab->snapshot->size())) {
                    const auto& entry = (*tab->snapshot)[static_cast<size_t>(hit.index)];
                    ToggleStarred(*s, p, entry.is_dir
                        ? app::PlaceItemKind::Folder : app::PlaceItemKind::File);
                }
            }
        } else if (hit.region == ui::HitTestResult::RowNewTab && hit.index >= 0) {
            if (app::Tab* tab = ActiveTab(*s)) {
                const std::wstring p = EntryFullPath(*tab, hit.index);
                if (!p.empty()) NewTab(*s, p);
            }
        } else if (hit.region == ui::HitTestResult::RowMore && hit.index >= 0) {
            if (app::Tab* tab = ActiveTab(*s)) {
                if (!tab->IsSelected(hit.index)) tab->SelectOnly(hit.index);
                POINT point{ mx, my };
                ClientToScreen(hwnd, &point);
                ShowItemContextMenu(*s, point);
            }
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
                    if (!p.empty()) s->ops.OpenTerminal(ClipboardPath(p));
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
                    NewTab(*s, target);
                } else {
                    NewTab(*s, fs::ParentPath(target));
                    if (app::Tab* tab = ActiveTab(*s)) {
                        std::wstring leaf = target;
                        if (leaf.starts_with(L"\\\\?\\UNC\\")) leaf = L"\\\\" + leaf.substr(8);
                        else if (leaf.starts_with(L"\\\\?\\")) leaf = leaf.substr(4);
                        const auto slash = leaf.find_last_of(L"\\/");
                        if (slash != std::wstring::npos) leaf = leaf.substr(slash + 1);
                        tab->pending_selected_name = leaf;
                        tab->pending_selected_names = { leaf };
                    }
                }
            }
        } else if (hit.region == ui::HitTestResult::DetailsCopyPath) {
            if (!vm.details.path.empty())
                ops::WriteClipboardText(ClipboardPath(vm.details.path));
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
                        RefreshActiveTab(*s);
                }
            }
        } else if (hit.region == ui::HitTestResult::DetailsSecurityChange) {
            DispatchMenuCommand(*s, app::CmdProperties);
        } else if (hit.region == ui::HitTestResult::DetailsRename) {
            ShowRenameOverlay(*s);
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
            s->renderer.SetDetailsPanelVisible(s->showDetailsPanel);
            InvalidateRect(hwnd, nullptr, FALSE);
        } else if (hit.region == ui::HitTestResult::PaneMediumIcons) {
            SetViewMode(*s, ui::ViewMode::MediumIcons);
        } else if (hit.region == ui::HitTestResult::PaneViewButton) {
            ShowViewDropdown(*s, hit.pane_index);
        } else if (hit.region == ui::HitTestResult::FilterBox) {
            ShowFilterEditor(*s);
        } else if (hit.region == ui::HitTestResult::AddressBar) {
            ShowOmnibar(*s, OmnibarMode::Path);
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
            if (MessageBoxW(hwnd, L"确定清空全部最近使用记录吗？", L"清空最近使用",
                            MB_OKCANCEL | MB_ICONWARNING | MB_DEFBUTTON2) == IDOK &&
                s->places.ClearRecent()) {
                RefreshRecentViews(*s);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
        } else if (hit.region == ui::HitTestResult::ColumnHeader) {
            SortBy(*s, hit.column);
        } else if (hit.region == ui::HitTestResult::SidebarHeaderAction) {
            // Sidebar groups are pushed in a fixed order; group 4 is 网络位置.
            if (hit.index == 4) {
                s->settings.NetworkAction(0, true);
            } else if (hit.index == 3) {
                POINT point{ mx, my };
                ClientToScreen(hwnd, &point);
                ShowCreateTagPicker(*s, point);
            } else {
                POINT point{ mx, my };
                ClientToScreen(hwnd, &point);
                ShowTagPicker(*s, point);
            }
        } else if (hit.region == ui::HitTestResult::SidebarHeader) {
            if (hit.index >= 0 && hit.index < 32) {
                s->sidebarCollapsedMask ^= (1u << hit.index);
                InvalidateRect(hwnd, nullptr, FALSE);
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
            }
        } else if (hit.region == ui::HitTestResult::SidebarItem) {
            const app::StarredItem* starred = s->places.FindStarred(hit.path);
            if (starred && starred->kind == app::PlaceItemKind::Folder) {
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
            } else if (hit.path.starts_with(L"pulse:tag:")) {
                // Tags defer navigation to release; a press may become a reorder drag.
                s->tagDragPending = true;
                s->tagDragStartPt = POINT{ mx, my };
                s->tagDragPath = hit.path;
                std::wstring kind, rest;
                app::ParsePulsePath(hit.path, &kind, &rest);
                s->tagDragTag = s->places.FindTagIndex(s->places.ResolveTagRef(rest));
                SetCapture(hwnd);
            } else if (hit.path.starts_with(L"pulse:workspace:")) {
                std::wstring kind, rest;
                app::ParsePulsePath(hit.path, &kind, &rest);
                OpenWorkspace(*s, _wtoi(rest.c_str()));
            } else if (!hit.path.empty()) {
                NavigateTo(*s, hit.path);
            }
        } else if (hit.region == ui::HitTestResult::StatusBar) {
            const ops::OpStatus status = s->ops.Status();
            if (s->operationWindow &&
                (status.active || !status.summary.empty() || !status.last_error.empty())) {
                s->operationWindow->Update(status);
                s->operationWindow->Show(true);
            }
        } else if (!IsSettingsTab(ActiveTab(*s)) &&
                   (hit.region == ui::HitTestResult::Pane ||
                   (PointInList(*s, mx, my) && hit.region == ui::HitTestResult::None))) {
            app::Tab* tab = ActiveTab(*s);
            const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            if (tab && !ctrl) tab->ClearSelection();
            s->marqueePending = true;
            s->marqueeActive = false;
            s->marqueeAdditive = ctrl;
            s->marqueeStart = s->marqueeCur = POINT{ mx, my };
            s->marqueeBase.clear();
            if (tab && ctrl) {
                tab->MaterializeSelection();
                s->marqueeBase = tab->selected;
            }
            SetCapture(hwnd);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
}

LRESULT HandleLButtonDblClk(AppState* s, HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        if (!s) return DefWindowProcW(hwnd, msg, wParam, lParam);
        int mx = GET_X_LPARAM(lParam);
        int my = GET_Y_LPARAM(lParam);
        CancelRenameClick(*s);
        s->dragPending = false;
        // Hit-test only: do not probe details (SHGetFileInfo) before OpenWith.
        ui::WindowViewModel vm = BuildVm(*s, false);
        D2D1_RECT_F rect = D2D1::RectF(0, 0, (float)s->compositor.Width(), (float)s->compositor.Height());
        ui::HitTestResult hit = s->renderer.HitTest(vm, rect, (float)mx, (float)my);
        if (hit.region == ui::HitTestResult::SidebarItem &&
            hit.path.starts_with(L"pulse:tag:")) {
            s->tagDragPending = false;
            s->tagDragActive = false;
            s->tagDragTag = -1;
            s->tagDragPath.clear();
            if (GetCapture() == hwnd) ReleaseCapture();
            std::wstring kind, rest;
            app::ParsePulsePath(hit.path, &kind, &rest);
            ShowTagRenameOverlay(*s, s->places.ResolveTagRef(rest));
        } else if (hit.region == ui::HitTestResult::Row && hit.index >= 0) {
            app::Tab* tab = ActiveTab(*s);
            if (tab) tab->SelectOnly(hit.index);
            OpenSelected(*s);
        } else if (hit.region == ui::HitTestResult::AddressBar ||
                   hit.region == ui::HitTestResult::BreadcrumbSegment) {
            ShowOmnibar(*s, OmnibarMode::Path);
        }
        return 0;
}

LRESULT HandleLButtonUp(AppState* s, HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        if (s) {
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
            if (s->starDragPending || s->starDragActive) {
                const bool was_active = s->starDragActive;
                const std::wstring path = s->starDragPath;
                if (was_active && s->dropSidebar >= 0)
                    s->places.ReorderStarredFolder(path, s->starDragTarget);
                s->starDragPending = false;
                s->starDragActive = false;
                s->starDragPath.clear();
                s->dropSidebar = -1;
                if (GetCapture() == hwnd) ReleaseCapture();
                if (!was_active && !path.empty()) NavigateTo(*s, path);
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (s->tagDragPending || s->tagDragActive) {
                const bool wasActive = s->tagDragActive;
                const std::wstring path = s->tagDragPath;
                if (wasActive && s->tagDragTag >= 0 &&
                    s->tagDragTag < static_cast<int>(s->places.tags.size())) {
                    // Slide the dragged tag home; duration scales with distance
                    // (250 ms per slot height, snap when < 50 ms), as in TabBar.
                    int cur = -1;
                    for (int p = 0; p < static_cast<int>(s->tagOrder.size()); ++p)
                        if (s->tagOrder[p] == s->tagDragTag) { cur = p; break; }
                    if (cur >= 0 && s->tagPitch > 0.0f) {
                        const float targetTop = s->tagFlowTop + cur * s->tagPitch;
                        const float dist = std::abs(s->tagDragFloatTop - targetTop);
                        const int raw = static_cast<int>(
                            dist * 250.0f / std::max(1.0f, s->tagSlotH));
                        // Snap tiny gaps; otherwise ease home (easeOutCubic via
                        // TickTagTransitions), fast enough to feel magnetic.
                        const int dur = raw < 120 ? 0 : std::clamp(raw, 160, 320);
                        if (dur > 0) {
                            const std::wstring label =
                                s->places.tags[static_cast<size_t>(s->tagDragTag)].name;
                            const float start =
                                (s->tagDragFloatTop - targetTop) / s->tagPitch;
                            s->tagTracks[label] = AppState::TagTrack{
                                start, dur, std::chrono::steady_clock::now() };
                            s->tagOffsets[label] = start;
                        }
                    }
                }
                if (wasActive && s->tagOrder.size() == s->places.tags.size() &&
                    !s->tagOrder.empty()) {
                    bool changed = false;
                    for (size_t i = 0; i < s->tagOrder.size(); ++i)
                        if (s->tagOrder[i] != static_cast<int>(i)) { changed = true; break; }
                    if (changed) {
                        std::vector<app::ColorTag> reordered;
                        reordered.reserve(s->places.tags.size());
                        for (int idx : s->tagOrder)
                            if (idx >= 0 && idx < static_cast<int>(s->places.tags.size()))
                                reordered.push_back(s->places.tags[static_cast<size_t>(idx)]);
                        if (reordered.size() == s->places.tags.size()) {
                            s->places.tags = std::move(reordered);
                            s->places.TagsReordered();
                        }
                    }
                }
                s->tagDragPending = false;
                s->tagDragActive = false;
                s->tagDragTag = -1;
                s->tagDragPath.clear();
                s->tagOrder.clear();
                s->tagGapVisible = false;
                s->tagGapFrom = s->tagGapTo = s->tagGapLineY = 0.0f;
                if (GetCapture() == hwnd) ReleaseCapture();
                if (!wasActive && !path.empty()) NavigateTo(*s, path); // plain click
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
                                    return DefWindowProcW(hwnd, msg, wParam, lParam);
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
            if (s->marqueeActive || s->marqueePending) {
                if (s->marqueeActive) ApplyMarqueeSelection(*s);
                ResetMarquee(*s);
            } else if (s->clickCollapseIndex >= 0) {
                app::Tab* tab = ActiveTab(*s);
                if (tab) tab->SelectOnly(s->clickCollapseIndex);
            }
            if (s->renameClickCandidate && s->renameClickDue == 0) {
                const int mx = GET_X_LPARAM(lParam);
                const int my = GET_Y_LPARAM(lParam);
                ui::WindowViewModel vm = BuildVm(*s);
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
                    s->renameClickDue = GetTickCount64() + GetDoubleClickTime();
                } else {
                    CancelRenameClick(*s);
                }
            }
            s->clickCollapseIndex = -1;
            s->dragPending = false;
            s->scrollbarDragging = false;
            s->scrollbarHorizontal = false;
            s->splitterDragging = false;
            s->detailsPanelResizing = false;
            s->columnResizing = false;
            s->columnResizeIndex = -1;
            s->columnResizePane = -1;
            s->splitterDragIndex = -1;
            s->tabDragPending = false;
            s->tabDragging = false;
            s->tabDragIndex = -1;
            if (GetCapture() == hwnd) ReleaseCapture();
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
}

LRESULT HandleCaptureChanged(AppState* s, HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        (void)msg;
        (void)wParam;
        (void)lParam;
        if (s) {
            s->columnResizing = false;
            s->columnResizeIndex = -1;
            s->columnResizePane = -1;
            if (s->renameClickCandidate && s->renameClickDue == 0)
                CancelRenameClick(*s);
            if (s->starDragPending || s->starDragActive) {
                s->starDragPending = false;
                s->starDragActive = false;
                s->starDragPath.clear();
                s->dropSidebar = -1;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            if (s->tagDragPending || s->tagDragActive) {
                // Capture lost mid-gesture: cancel the reorder, keep places.tags.
                s->tagDragPending = false;
                s->tagDragActive = false;
                s->tagDragTag = -1;
                s->tagDragPath.clear();
                s->tagOrder.clear();
                s->tagGapVisible = false;
                s->tagGapFrom = s->tagGapTo = s->tagGapLineY = 0.0f;
                s->tagTracks.clear();
                s->tagOffsets.clear();
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            if (s->tabDragPending || s->tabDragging) {
                s->tabDragPending = false;
                s->tabDragging = false;
                s->tabDragIndex = -1;
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
            s->dragPending = false;
            s->scrollbarDragging = false;
            s->scrollbarHorizontal = false;
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
            if (tab->IsSelected(hit.index)) {
                paths = SelectedFullPaths(*tab);
                ext = CommonExtension(*tab, tab->SelectedIndices());
            } else {
                std::wstring one = EntryFullPath(*tab, hit.index);
                if (!one.empty()) paths.push_back(std::move(one));
                ext = CommonExtension(*tab, { hit.index });
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
                StartCtxQuery(*s, { tab->current_path }, true, L"");
            }
        }
        return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT HandleRButtonUp(AppState* s, HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        if (!s) return DefWindowProcW(hwnd, msg, wParam, lParam);
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
        bool shown = false;
        if (hit.region == ui::HitTestResult::TabGroup && hit.index >= 0 &&
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
        } else if (hit.region == ui::HitTestResult::SidebarItem &&
                   hit.path == app::MakeRecyclePath()) {
            ShowRecyclePlaceMenu(*s, sp);
            shown = true;
        } else if (hit.region == ui::HitTestResult::SidebarItem) {
            const app::StarredItem* starred = s->places.FindStarred(hit.path);
            const app::SidebarEntry* quick_access = QuickAccessEntryForPath(*s, hit.path);
            if ((starred && starred->kind == app::PlaceItemKind::Folder) || quick_access) {
                ShowCuratedItemMenu(*s, hit.path, false, sp);
                shown = true;
            }
        } else if (!IsSettingsTab(ActiveTab(*s)) &&
                   (hit.region == ui::HitTestResult::Pane ||
                   hit.region == ui::HitTestResult::PaneHeader ||
                   hit.region == ui::HitTestResult::FilterBox ||
                   hit.region == ui::HitTestResult::ColumnHeader ||
                   hit.region == ui::HitTestResult::None)) {
            D2D1_RECT_F content = s->renderer.ContentRect(rect.right, rect.bottom);
            if (hit.pane_index >= 0 ||
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
        CancelRenameClick(*s);
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
        const D2D1_RECT_F sidebarRc = s->renderer.SidebarRect(wheelRect.right, wheelRect.bottom);
        // Tray deck first: the panel lives inside the sidebar rect, so the
        // sidebar branch below would swallow every wheel event over it.
        const int tray_cap = TrayDeckCap(*s);
        if (TrayItemTotalCount(s->tray) > tray_cap) {
            const D2D1_RECT_F tray_rc = s->renderer.StagingTrayRect(
                wheelVm, wheelRect.right, wheelRect.bottom);
            if (pt.x >= tray_rc.left && pt.x < tray_rc.right &&
                pt.y >= tray_rc.top && pt.y < tray_rc.bottom) {
                const int wheel_delta = GET_WHEEL_DELTA_WPARAM(wParam);
                s->trayWheelAccum += wheel_delta;
                int steps = 0;
                while (s->trayWheelAccum <= -WHEEL_DELTA) { s->trayWheelAccum += WHEEL_DELTA; ++steps; }
                while (s->trayWheelAccum >= WHEEL_DELTA) { s->trayWheelAccum -= WHEEL_DELTA; --steps; }
                if (steps != 0) {
                    const int max_off = std::max(0, TrayItemTotalCount(s->tray) - tray_cap);
                    const int next = std::clamp(s->trayDeckOffset + steps, 0, max_off);
                    if (next != s->trayDeckOffset) {
                        s->trayDeckOffset = next;
                        InvalidateRect(hwnd, nullptr, FALSE);
                    }
                }
                return 0;
            }
        }
        if (pt.x >= sidebarRc.left && pt.x < sidebarRc.right &&
            pt.y >= sidebarRc.top && pt.y < sidebarRc.bottom &&
            s->renderer.EffectiveSidebarWidth(wheelRect.right) > 60.0f * s->scale) {
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
                s->detailsPreviewScroll = std::clamp(
                    s->detailsPreviewScroll - steps * 48.0f, 0.0f, 4000.0f);
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
        if (wheelHit.pane_index >= 0) {
            if (app::Pane* p = PaneAtSlot(*s, wheelHit.pane_index)) FocusPane(*s, p);
        }
        int delta = GET_WHEEL_DELTA_WPARAM(wParam);
        app::Tab* wheelTab = ActiveTab(*s);
        if ((GET_KEYSTATE_WPARAM(wParam) & MK_CONTROL) != 0) {
            if (wheelTab) {
                const int direction = delta > 0 ? -1 : 1;
                const int next = std::clamp(ui::ViewModeIndex(wheelTab->view_mode) + direction, 0, 7);
                SetViewMode(*s, ui::ViewModeFromIndex(next));
            }
            return 0;
        }
        if (wheelTab && wheelTab->view_mode == ui::ViewMode::List) {
            ui::PaneViewModel pane;
            app::FillPaneViewModel(pane, *s->pane, &s->places);
            const float maxX = s->renderer.MaxScrollXForPane(pane, FocusedPaneRect(*s));
            wheelTab->scroll_x = std::clamp(wheelTab->scroll_x -
                (static_cast<float>(delta) / WHEEL_DELTA) * 220.0f * s->scale,
                0.0f, maxX);
            InvalidateRect(hwnd, nullptr, FALSE);
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

LRESULT HandleKeyDown(AppState* s, HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        if (!s) return DefWindowProcW(hwnd, msg, wParam, lParam);
        CancelRenameClick(*s);
        app::Tab* tab = ActiveTab(*s);
        if (!tab) return DefWindowProcW(hwnd, msg, wParam, lParam);
        ui::WindowViewModel vm = BuildVm(*s);
        bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        bool alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
        bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        bool handled = true;

        if (ctrl && wParam == L'T') {
            NewTab(*s, NewTabPath(*s));
        } else if (ctrl && wParam == L'K') {
            ShowOmnibar(*s, OmnibarMode::Command);
        } else if (ctrl && wParam == L'P') {
            ShowOmnibar(*s, OmnibarMode::Project);
        } else if (ctrl && shift && wParam >= L'1' && wParam <= L'7') {
            app::Tab* t = ActiveTab(*s);
            const int tag_index = static_cast<int>(wParam - L'1');
            if (t && tag_index < static_cast<int>(s->places.tags.size()))
                ToggleTagForSelection(*s, s->places.tags[static_cast<size_t>(tag_index)].id,
                                      SelectedFullPaths(*t));
        } else if (ctrl && wParam == L'F') {
            ShowFilterEditor(*s);
        } else if (ctrl && wParam == L'D') {
            MarkTargetPane(*s);
        } else if (wParam == VK_F6) {
            CycleFocus(*s);
        } else if (ctrl && wParam == L'1') {
            ApplyLayoutPreset(*s, app::LayoutPreset::Single);
        } else if (ctrl && wParam == L'2') {
            ApplyLayoutPreset(*s, app::LayoutPreset::TwoVertical);
        } else if (ctrl && wParam == L'3') {
            ApplyLayoutPreset(*s, app::LayoutPreset::Three);
        } else if (ctrl && wParam == L'4') {
            ApplyLayoutPreset(*s, app::LayoutPreset::FourGrid);
        } else if (ctrl && alt && wParam == L'C') {
            TransferToTarget(*s, false);
        } else if (ctrl && alt && wParam == L'X') {
            TransferToTarget(*s, true);
        } else if (ctrl && wParam == L'W') {
            CloseActiveTab(*s);
        } else if (ctrl && wParam == VK_TAB) {
            if (!s->window_tabs.items.empty()) {
                SwitchTab(*s, (s->window_tabs.active + 1) % s->window_tabs.items.size());
            }
        } else if (ctrl && wParam == L'L') {
            ShowOmnibar(*s, OmnibarMode::Path);
        } else if (wParam == VK_F4) {
            ShowOmnibar(*s, OmnibarMode::Path);
        } else if (ctrl && shift && wParam == L'C') {
            CopySelectedPath(*s);
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
        } else if (wParam == VK_F2) {
            ShowRenameOverlay(*s);
        } else if (wParam == VK_F7) {
            CreateNewItem(*s, true); // 新建文件夹并进入重命名（ui.md §7.9）
        } else if (alt && wParam == VK_RETURN) {
            DispatchMenuCommand(*s, app::CmdProperties);
        } else if (wParam == VK_DELETE) {
            DeleteSelected(*s, (GetKeyState(VK_SHIFT) & 0x8000) != 0);
        } else if (wParam == VK_RETURN) {
            OpenSelected(*s);
        } else if (wParam == VK_BACK || (alt && wParam == VK_UP)) {
            GoUp(*s);
        } else if (alt && wParam == VK_LEFT) {
            GoBack(*s);
        } else if (alt && wParam == VK_RIGHT) {
            GoForward(*s);
        } else if (wParam == VK_F5) {
            s->store.MarkDirty(tab->current_path);
            RefreshActiveTab(*s);
        } else if (wParam == VK_F1) {
            s->showFps = !s->showFps;
            InvalidateRect(hwnd, nullptr, FALSE);
        } else if (wParam == VK_SPACE) {
            ToggleQuickPreview(*s);
        } else if (ctrl && shift && wParam == L'A') {
            DispatchMenuCommand(*s, app::CmdSelectWildcard);
        } else if (ctrl && wParam == L'A') {
            DispatchMenuCommand(*s, app::CmdSelectAll);
        } else if (ctrl && wParam == L'I') {
            DispatchMenuCommand(*s, app::CmdInvertSelection);
        } else if (wParam == VK_ESCAPE) {
            tab->ClearSelection();
            ResetMarquee(*s);
            s->clickCollapseIndex = -1;
        } else if (wParam == VK_DOWN || wParam == VK_UP ||
                   wParam == VK_LEFT || wParam == VK_RIGHT) {
            CancelScrollAnimation(*s);
            {
                const int n = static_cast<int>(vm.pane.EntryCount());
                if (n > 0) {
                    int view = vm.pane.ViewIndex(tab->selected_index);
                    if (view < 0) view = 0;
                    const int dx = wParam == VK_LEFT ? -1 : (wParam == VK_RIGHT ? 1 : 0);
                    const int dy = wParam == VK_UP ? -1 : (wParam == VK_DOWN ? 1 : 0);
                    const int next = s->renderer.MoveViewIndex(vm.pane, FocusedPaneRect(*s),
                                                                view, dx, dy);
                    tab->MoveFocus(vm.pane.SourceIndex(next), shift);
                    EnsureRowVisible(*s, *tab, tab->selected_index);
                }
            }
        } else if (wParam == VK_NEXT) {
            CancelScrollAnimation(*s);
            {
                const int n = static_cast<int>(vm.pane.EntryCount());
                const int page = s->renderer.PageDelta(vm.pane, FocusedPaneRect(*s));
                if (n > 0) {
                    int view = vm.pane.ViewIndex(tab->selected_index);
                    if (view < 0) view = 0;
                    const int next = std::min(n - 1, view + page);
                    tab->MoveFocus(vm.pane.SourceIndex(next), shift);
                    EnsureRowVisible(*s, *tab, tab->selected_index);
                }
            }
        } else if (wParam == VK_PRIOR) {
            CancelScrollAnimation(*s);
            {
                const int n = static_cast<int>(vm.pane.EntryCount());
                const int page = s->renderer.PageDelta(vm.pane, FocusedPaneRect(*s));
                if (n > 0) {
                    int view = vm.pane.ViewIndex(tab->selected_index);
                    if (view < 0) view = 0;
                    const int next = std::max(0, view - page);
                    tab->MoveFocus(vm.pane.SourceIndex(next), shift);
                    EnsureRowVisible(*s, *tab, tab->selected_index);
                }
            }
        } else if (wParam == VK_HOME) {
            CancelScrollAnimation(*s);
            if (vm.pane.EntryCount() > 0) {
                tab->MoveFocus(vm.pane.SourceIndex(0), shift);
                tab->scroll_y = 0;
            }
        } else if (wParam == VK_END) {
            CancelScrollAnimation(*s);
            if (vm.pane.EntryCount() > 0) {
                tab->MoveFocus(vm.pane.SourceIndex(static_cast<int>(vm.pane.EntryCount()) - 1), shift);
                EnsureRowVisible(*s, *tab, tab->selected_index);
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
