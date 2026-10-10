#include "app_hover.h"
#include "app_runtime.h"
#include "app_change_tracking.h"

namespace pulse {
bool UpdatePointerHover(AppState& s, const ui::HitTestResult& hit, POINT point) {
    const bool target_changed = static_cast<int>(hit.region) != s.hoverRegion ||
        hit.index != s.hoverControlIndex || hit.sub_index != s.hoverSubIndex ||
        hit.pane_index != s.hoverPaneIndex || hit.path != s.hoverPath || hit.label != s.hoverLabel;
    const int row = (hit.region == ui::HitTestResult::Row ||
        hit.region == ui::HitTestResult::RowStar ||
        hit.region == ui::HitTestResult::RowFolderSize ||
        hit.region == ui::HitTestResult::RowNewTab ||
        hit.region == ui::HitTestResult::ChangeBadge ||
        hit.region == ui::HitTestResult::RowMore) ? hit.index : -1;
    const bool row_changed = row != s.hoverRow || hit.pane_index != s.hoverPaneIndex;
    const int crumb = hit.region == ui::HitTestResult::BreadcrumbSegment ? hit.index : -1;
    const bool crumb_changed = crumb != s.breadcrumbHover;
    const bool sidebar_hot = hit.region == ui::HitTestResult::Scrollbar && hit.sub_index == 2;
    const bool sidebar_changed = sidebar_hot != s.sidebarScrollbarHot;
    s.hoverPoint = point;
    if (target_changed) ApplyHoverTarget(s, hit);
    if (row_changed) {
        s.hoverRow = row;
        s.hoverPaneIndex = hit.pane_index;
        s.ctxHoverSince = row >= 0 ? GetTickCount64() : 0;
        if (row < 0) s.ctxHoverPrefetched.clear();
    }
    s.sidebarScrollbarHot = sidebar_hot;
    s.breadcrumbHover = crumb;
    UpdateChangeHover(s, hit, point);
    return target_changed || row_changed || crumb_changed || sidebar_changed;
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
