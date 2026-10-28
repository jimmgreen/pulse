#include "app_hover.h"
#include "app_runtime.h"
#include "app_change_tracking.h"
#include "app_hover_preview.h"
#include "../ui/ui_renderer.h"

namespace pulse {
bool UpdatePointerHover(AppState& s, const ui::HitTestResult& pointer_hit, POINT point) {
    // On a folder's hover card the card keeps the pointer (its list scrolls):
    // the rows under it get no hover, and its own row stays the hovered one.
    static const ui::HitTestResult kNoHit{};
    const bool on_card = HoverPreviewHoldsPointer(s, point);
    const ui::HitTestResult& hit = on_card ? kNoHit : pointer_hit;
    const bool target_changed = static_cast<int>(hit.region) != s.hoverRegion ||
        hit.index != s.hoverControlIndex || hit.sub_index != s.hoverSubIndex ||
        (!on_card && hit.pane_index != s.hoverPaneIndex) || hit.path != s.hoverPath || hit.label != s.hoverLabel;
    const int row = on_card ? s.hoverRow : (hit.region == ui::HitTestResult::Row ||
        hit.region == ui::HitTestResult::RowStar ||
        hit.region == ui::HitTestResult::RowTagDot ||
        hit.region == ui::HitTestResult::RowFolderSize ||
        hit.region == ui::HitTestResult::RowNewTab ||
        hit.region == ui::HitTestResult::ChangeBadge ||
        hit.region == ui::HitTestResult::RowMore ||
        hit.region == ui::HitTestResult::RowCheck) ? hit.index : -1;
    const int row_pane = on_card ? s.hoverPaneIndex : hit.pane_index;
    const bool row_changed = row != s.hoverRow || row_pane != s.hoverPaneIndex;
    const int crumb = hit.region == ui::HitTestResult::BreadcrumbSegment ? hit.index : -1;
    const bool crumb_changed = crumb != s.breadcrumbHover;
    const bool sidebar_hot = hit.region == ui::HitTestResult::Scrollbar && hit.sub_index == 2;
    const bool sidebar_changed = sidebar_hot != s.sidebarScrollbarHot;
    const bool settings_hot = hit.region == ui::HitTestResult::Scrollbar && hit.sub_index == 3;
    const bool pane_hot = hit.region == ui::HitTestResult::Scrollbar && (hit.sub_index == 0 || hit.sub_index == 1);
    const int pane_hot_index = pane_hot ? hit.pane_index : -1;
    const bool bars_changed = settings_hot != s.settingsScrollbarHot || pane_hot != s.paneScrollbarHot ||
                              pane_hot_index != s.paneScrollbarHotPane;
    s.hoverPoint = point;
    if (target_changed) ApplyHoverTarget(s, hit);
    if (row_changed) {
        s.hoverRow = row;
        s.hoverPaneIndex = row_pane;
        s.ctxHoverSince = row >= 0 ? GetTickCount64() : 0;
        if (row < 0) s.ctxHoverPrefetched.clear();
        NoteHoverPreviewRow(s);
    }
    s.sidebarScrollbarHot = sidebar_hot;
    s.settingsScrollbarHot = settings_hot;
    s.paneScrollbarHot = pane_hot;
    s.paneScrollbarHotPane = pane_hot_index;
    s.breadcrumbHover = crumb;
    UpdateChangeHover(s, hit, point);
    return target_changed || row_changed || crumb_changed || sidebar_changed || bars_changed;
}

bool RefreshScrolledHover(AppState& s, const HoverPointerApi& api) {
    if (!s.hwnd || s.shot.active || s.menushot || api.capture() ||
        s.marqueePending || s.marqueeActive || s.dragPending || s.tabDragging ||
        s.scrollbarDragging || s.columnResizing || s.stripResizing || s.stripHScrolling ||
        s.splitterDragging || s.detailsPanelResizing) return false;
    POINT point{};
    RECT client{};
    if (!api.cursor_position(&point) || api.window_at_point(point) != s.hwnd ||
        !api.to_client(s.hwnd, &point) || !GetClientRect(s.hwnd, &client) ||
        !PtInRect(&client, point)) {
        // A covered window or hosted editor must not gain hover behind the pointer.
        return UpdatePointerHover(s, {}, s.hoverPoint);
    }
    const auto bounds = D2D1::RectF(0, 0, static_cast<float>(s.compositor.Width()),
        static_cast<float>(s.compositor.Height()));
    if (bounds.right <= 0 || bounds.bottom <= 0) return false;
    const auto vm = BuildVm(s, false);
    const auto hit = s.renderer.HitTest(vm, bounds, static_cast<float>(point.x),
        static_cast<float>(point.y));
    return UpdatePointerHover(s, hit, point);
}
} // namespace pulse
