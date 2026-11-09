// column_header_drag.cpp — Details header: click sorts, drag reorders columns.
#include "column_header_drag.h"
#include "app_internal.h"
#include "app_navigation.h"
#include "app_runtime.h"
#include "../ui/ui_renderer.h"
#include <cmath>

namespace pulse {
namespace {

using K = ui::MainRenderer::ColumnKind;

bool SlotLayout(AppState& s, const ui::WindowViewModel& vm, int pane,
                ui::MainRenderer::DetailsColumnLayout& out) {
    if (pane < 0 || pane >= static_cast<int>(vm.pane_slots.size())) return false;
    const auto& slot = vm.pane_slots[static_cast<size_t>(pane)];
    if (slot.pane.view_mode != ui::ViewMode::Details || slot.pane.curated_order ||
        slot.pane.column_strip.Active())
        return false;
    out = s.renderer.DetailsColumns(s.renderer.PaneBodyBounds(slot.pane, slot.rect), slot.pane);
    return true;
}

void Reset(AppState& s) {
    s.columnHeaderDrag = {};
}

} // namespace

bool BeginColumnHeaderPress(AppState& s, const ui::WindowViewModel& vm, const ui::HitTestResult& hit,
                            int mx, int my) {
    Reset(s);
    ui::MainRenderer::DetailsColumnLayout layout;
    if (hit.region != ui::HitTestResult::ColumnHeader || !SlotLayout(s, vm, hit.pane_index, layout)) return false;
    const int from = hit.index;
    if (!layout.reorderable || from < layout.FirstOrderable() || from >= layout.count ||
        static_cast<int>(layout.kinds[static_cast<size_t>(from)]) != hit.sub_index)
        return false;
    auto& d = s.columnHeaderDrag;
    d.pending = true;
    d.pane = hit.pane_index;
    d.from = from;
    d.sortable = layout.kinds[static_cast<size_t>(from)] != K::Status;
    d.column = hit.column;
    d.start = POINT{mx, my};
    const float left = layout.DividerX(from - 1);
    d.grab_dx = static_cast<float>(mx) - left;
    d.cursor_x = static_cast<float>(mx);
    SetCapture(s.hwnd);
    return true;
}

bool ColumnHeaderPressTracking(const AppState& s) {
    return s.columnHeaderDrag.pending || s.columnHeaderDrag.active;
}

void UpdateColumnHeaderDrag(AppState& s, int mx, int my) {
    auto& d = s.columnHeaderDrag;
    if (!d.pending && !d.active) return;
    if (!d.active) {
        // Horizontal intent only: a header column moves sideways.
        const float slop = static_cast<float>(GetSystemMetrics(SM_CXDRAG)) * s.scale + 2.0f * s.scale;
        if (std::abs(static_cast<float>(mx - d.start.x)) < slop) {
            (void)my;
            return;
        }
        d.active = true;
        d.pending = false;
    }
    d.cursor_x = static_cast<float>(mx);
    const ui::WindowViewModel vm = BuildVm(s, false);
    ui::MainRenderer::DetailsColumnLayout layout;
    if (!SlotLayout(s, vm, d.pane, layout) || d.from >= layout.count) {
        d.slot = -1;
    } else {
        // The floating header's leading edge picks the slot (its left edge
        // moving left, its right edge moving right), so a wide column does
        // not have to be dragged all the way by the grabbed point.
        const float width = layout.widths[static_cast<size_t>(d.from)];
        const float float_left = static_cast<float>(mx) - d.grab_dx;
        const float probe = float_left < layout.DividerX(d.from - 1) ? float_left : float_left + width;
        d.slot = ui::MainRenderer::ColumnDropSlot(layout, d.from, probe, &d.line_x);
    }
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

void EndColumnHeaderPress(AppState& s) {
    const auto d = s.columnHeaderDrag;
    Reset(s);
    if (GetCapture() == s.hwnd) ReleaseCapture();
    if (d.active) {
        if (d.slot >= 0) {
            const ui::WindowViewModel vm = BuildVm(s, false);
            ui::MainRenderer::DetailsColumnLayout layout;
            if (SlotLayout(s, vm, d.pane, layout)) {
                s.settings.DetailsColumnOrder(ui::MainRenderer::ReorderedColumns(
                    layout, s.appPrefs.details_column_order, d.from, d.slot));
                if (s.renameIndex >= 0) LayoutRenameOverlay(s);
            }
        }
        InvalidateRect(s.hwnd, nullptr, FALSE);
        return;
    }
    if (d.pending && d.sortable) SortBy(s, d.column);
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

bool CancelColumnHeaderPress(AppState& s) {
    if (!ColumnHeaderPressTracking(s)) return false;
    Reset(s);
    if (GetCapture() == s.hwnd) ReleaseCapture();
    InvalidateRect(s.hwnd, nullptr, FALSE);
    return true;
}

} // namespace pulse
