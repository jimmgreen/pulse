// tab_transfer.cpp - Multi-window M5b: moves a live tab between windows.
#include "tab_transfer.h"

#include "app_internal.h"
#include "content_results_ui.h"

namespace pulse {
namespace {
bool OwnsPane(const app::LayoutTab& tab, const app::Pane* pane) {
    for (const auto& owned : tab.panes)
        if (owned.get() == pane) return true;
    return false;
}

bool OwnsView(const app::LayoutTab& tab, const app::Tab* view) {
    for (const auto& pane : tab.panes)
        if (pane->ActiveTab() == view) return true;
    return false;
}

// Ends what `s` was doing with `tab`, which is about to leave the window.
void ReleaseLeavingTab(AppState& s, const app::LayoutTab& tab, bool active) {
    if (active) {
        if (s.renameIndex >= 0) HideRenameOverlay(s, false);
        if (s.addressEditing) HideAddressEditor(s, false);
        if (s.filterEditing) HideFilterEditor(s, true);
        if (s.quickPreview.visible()) s.quickPreview.Close();
    }
    if (OwnsView(tab, static_cast<const app::Tab*>(s.fileClick.Item().tab)) || OwnsPane(tab, static_cast<const app::Pane*>(s.fileClick.Item().pane))) CancelFileClick(s);
    if (OwnsView(tab, s.blankClickTab) || OwnsPane(tab, s.blankClickPane)) {
        s.blankClickPane = nullptr;
        s.blankClickTab = nullptr;
    }
    if (OwnsView(tab, s.blankDoubleTab) || OwnsPane(tab, s.blankDoublePane)) {
        s.blankDoublePane = nullptr;
        s.blankDoubleTab = nullptr;
        s.blankDoublePending = false;
    }
    if (OwnsView(tab, s.listTypeAheadTab)) s.listTypeAheadTab = nullptr;
    if (s.tabFlashKey == &tab) s.tabFlashKey = nullptr;
    for (const auto& pane : tab.panes) CancelContentSelection(s, *pane->ActiveTab());
}

float ScaleRatio(const AppState& src, const AppState& dst) {
    return src.scale > 0.0f && dst.scale > 0.0f ? dst.scale / src.scale : 1.0f;
}

// Takes tab `index` out of `src` for a move into a pane of another window.
std::unique_ptr<app::LayoutTab> TakeLeavingTab(AppState& src, size_t index, bool closing, const AppState& dst) {
    auto& from = src.window_tabs;
    ReleaseLeavingTab(src, *from.items[index], index == from.active);
    RememberLayoutFocus(src);
    std::unique_ptr<app::LayoutTab> tab = from.TakeTab(index, closing);
    if (!tab) return nullptr;
    BindCurrentLayout(src);
    RescaleLayoutTabScroll(*tab, ScaleRatio(src, dst));
    if (!from.items.empty()) {
        RevalidateVisibleFolders(src);
        InvalidateRect(src.hwnd, nullptr, FALSE);
    }
    return tab;
}
}  // namespace

void RescaleLayoutTabScroll(app::LayoutTab& tab, float ratio) {
    if (!(ratio > 0.0f) || ratio == 1.0f) return;
    for (auto& pane : tab.panes) {
        if (!pane) continue;
        app::Tab& view = *pane->ActiveTab();
        view.scroll_x *= ratio;
        view.scroll_y *= ratio;
    }
}

bool MoveLayoutTab(AppState& src, size_t index, AppState& dst, size_t insert_at,
                   TransferSource source) {
    auto& from = src.window_tabs;
    const bool closing = source == TransferSource::Closing;
    if (&src == &dst || index >= from.items.size() || (from.items.size() < 2 && !closing) ||
        !from.items[index])
        return false;
    ReleaseLeavingTab(src, *from.items[index], index == from.active);
    RememberLayoutFocus(src);  // the tab carries its focused and target pane
    std::unique_ptr<app::LayoutTab> tab = from.TakeTab(index, closing);
    if (!tab) return false;
    BindCurrentLayout(src);  // no tab left: no pane, no watches
    RescaleLayoutTabScroll(*tab, ScaleRatio(src, dst));
    if (!from.items.empty()) {
        RevalidateVisibleFolders(src);
        InvalidateRect(src.hwnd, nullptr, FALSE);
    }

    if (dst.addressSearching) HideAddressEditor(dst, false);
    RememberLayoutFocus(dst);
    dst.window_tabs.InsertTab(std::move(tab), insert_at);
    BindCurrentLayout(dst);  // pane, target pane and scroll position follow the tab
    RevalidateVisibleFolders(dst);
    InvalidateRect(dst.hwnd, nullptr, FALSE);
    return true;
}

bool MoveTabIntoPane(AppState& src, size_t index, AppState& dst, app::ContentDrop edge, TransferSource source) {
    const app::LayoutTab* const into = dst.window_tabs.Active();
    const auto& from = src.window_tabs;
    if (!into || index >= from.items.size() || !from.items[index] || !app::CanAddPane(*into, *from.items[index], edge))
        return false;
    return MoveTabIntoPane(src, index, dst, app::EdgePlacement(*into, app::DropPaneCount(*from.items[index]), edge),
                           source);
}

bool MoveTabIntoPane(AppState& src, size_t index, AppState& dst, const app::PanePlacement& placement,
                     TransferSource source) {
    auto& from = src.window_tabs;
    const bool closing = source == TransferSource::Closing;
    app::LayoutTab* const into = dst.window_tabs.Active();
    if (&src == &dst || index >= from.items.size() || (from.items.size() < 2 && !closing) ||
        !from.items[index] || !into || !app::CanPlacePanes(*into, *from.items[index], placement))
        return false;
    std::unique_ptr<app::LayoutTab> tab = TakeLeavingTab(src, index, closing, dst);
    if (!tab) return false;

    if (dst.addressSearching) HideAddressEditor(dst, false);
    RememberLayoutFocus(dst);
    app::AddPaneFromTab(*into, std::move(tab), placement);
    BindCurrentLayout(dst);  // the dropped pane is focused, with its scroll position
    RevalidateVisibleFolders(dst);
    InvalidateRect(dst.hwnd, nullptr, FALSE);
    return true;
}

std::optional<size_t> SplitPaneIntoTab(AppState& s, app::Pane* pane, app::PaneOrigin& origin) {
    app::LayoutTab* const tab = s.window_tabs.Active();
    if (!tab || !pane || IsSettingsTab(ActiveTab(s)) || IsSettingsTab(pane->ActiveTab())) return std::nullopt;
    if (s.addressSearching) HideAddressEditor(s, false);
    RememberLayoutFocus(s);  // the remaining panes keep their focus and target
    std::unique_ptr<app::LayoutTab> own = app::TakePaneAsTab(*tab, pane, origin);
    if (!own) return std::nullopt;
    const size_t index = s.window_tabs.InsertTab(std::move(own), s.window_tabs.active + 1);
    BindCurrentLayout(s);  // the new tab is active, its pane focused
    RevalidateVisibleFolders(s);
    InvalidateRect(s.hwnd, nullptr, FALSE);
    return index;
}

bool ReturnTabAsPane(AppState& src, size_t index, AppState& dst, const app::LayoutTab* into,
                     const app::PaneOrigin& origin, TransferSource source) {
    auto& from = src.window_tabs;
    const bool closing = source == TransferSource::Closing;
    size_t at = dst.window_tabs.items.size();
    for (size_t i = 0; i < dst.window_tabs.items.size(); ++i)
        if (dst.window_tabs.items[i].get() == into) at = i;
    if (&src == &dst || index >= from.items.size() || (from.items.size() < 2 && !closing) ||
        !from.items[index] || at == dst.window_tabs.items.size() ||
        !app::CanPutPaneBack(*dst.window_tabs.items[at], *from.items[index], origin))
        return false;
    std::unique_ptr<app::LayoutTab> tab = TakeLeavingTab(src, index, closing, dst);
    if (!tab) return false;

    if (dst.addressSearching) HideAddressEditor(dst, false);
    RememberLayoutFocus(dst);
    dst.window_tabs.SwitchTab(at);
    app::PutPaneBack(*dst.window_tabs.items[at], std::move(tab), origin);
    BindCurrentLayout(dst);
    RevalidateVisibleFolders(dst);
    InvalidateRect(dst.hwnd, nullptr, FALSE);
    return true;
}
}  // namespace pulse
