#include "app_hover.h"
#include "app_internal.h"
#include "app_runtime.h"
#include "app_change_tracking.h"
#include "app_hover_preview.h"
#include "../ui/ui_renderer.h"

namespace pulse {
namespace {
constexpr UINT_PTR kPaneHoverTimer = 0x5048;
constexpr wchar_t kPaneHoverTarget[] = L"Pulse.PaneHoverTarget";
void CancelPaneHover(HWND hwnd) {
    KillTimer(hwnd, kPaneHoverTimer);
    RemovePropW(hwnd, kPaneHoverTarget);
}
int PaneHoverTarget(AppState& s, const ui::WindowViewModel& vm, const ui::HitTestResult& hit,
                    POINT point, WPARAM buttons) {
    if (vm.pane_slots.size() < 2 || hit.pane_index < 0 ||
        hit.pane_index >= static_cast<int>(vm.pane_slots.size()) ||
        GetForegroundWindow() != s.hwnd || GetFocus() != s.hwnd || GetCapture() ||
        (buttons & (MK_LBUTTON|MK_RBUTTON|MK_MBUTTON|MK_XBUTTON1|MK_XBUTTON2)) ||
        s.addressSearching || s.filterEditing || s.renameIndex >= 0 ||
        (s.menu && s.menu->IsOpen())) return -1;
    auto* pane = PaneAtSlot(s, hit.pane_index);
    if (!pane || pane == s.pane) return -1;
    const auto& slot = vm.pane_slots[static_cast<size_t>(hit.pane_index)];
    const auto list = s.renderer.PaneListRect(slot.pane, slot.rect);
    const float inset = 10*s.scale;
    if (point.x < list.left+inset || point.x >= list.right-inset ||
        point.y < list.top+inset || point.y >= list.bottom-inset) return -1;
    using R = ui::HitTestResult;
    switch (hit.region) {
    case R::None: case R::Pane: case R::Row: case R::RowStar: case R::RowTagDot: case R::RowFolderSize:
    case R::RowNewTab: case R::RowMore: case R::RowCheck: case R::ChangeBadge:
        return hit.pane_index;
    default: return -1;
    }
}
void CALLBACK ActivateHoveredPane(HWND hwnd, UINT, UINT_PTR, DWORD) {
    const auto target = reinterpret_cast<INT_PTR>(GetPropW(hwnd, kPaneHoverTarget))-1;
    CancelPaneHover(hwnd);
    auto* s = reinterpret_cast<AppState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!s || target < 0) return;
    POINT point{};
    if (!GetCursorPos(&point) || WindowFromPoint(point) != hwnd || !ScreenToClient(hwnd,&point)) return;
    const auto vm = BuildVm(*s);
    const auto bounds = D2D1::RectF(0,0,static_cast<float>(s->compositor.Width()),static_cast<float>(s->compositor.Height()));
    const auto hit = s->renderer.HitTest(vm,bounds,static_cast<float>(point.x),static_cast<float>(point.y));
    WPARAM buttons = 0;
    for (int key : {VK_LBUTTON,VK_RBUTTON,VK_MBUTTON,VK_XBUTTON1,VK_XBUTTON2})
        if (GetKeyState(key)&0x8000) buttons |= MK_LBUTTON;
    if (PaneHoverTarget(*s,vm,hit,point,buttons) == target) {
        FocusPane(*s,PaneAtSlot(*s,static_cast<int>(target)));
        InvalidateRect(hwnd,nullptr,FALSE);
    }
}
} // namespace
void UpdatePaneHoverActivation(AppState& s, const ui::WindowViewModel& vm,
                               const ui::HitTestResult& hit, POINT point, WPARAM buttons) {
    const int target = PaneHoverTarget(s,vm,hit,point,buttons);
    if (target < 0) { CancelPaneHover(s.hwnd); return; }
    const auto pending = reinterpret_cast<INT_PTR>(GetPropW(s.hwnd,kPaneHoverTarget));
    if (pending == target+1) return;
    CancelPaneHover(s.hwnd);
    if (SetPropW(s.hwnd,kPaneHoverTarget,reinterpret_cast<HANDLE>(static_cast<INT_PTR>(target+1)))) {
        if (!SetTimer(s.hwnd,kPaneHoverTimer,200,ActivateHoveredPane)) RemovePropW(s.hwnd,kPaneHoverTarget);
    }
}
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
        s.listSelection.PendingBand() || s.listSelection.BandActive() || s.listSelection.PendingFile() || s.tabDragging ||
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
