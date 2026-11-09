#pragma once
#include "app_model.h"
#include "../ui/ui_renderer.h"

namespace pulse::app {
inline void ResizePaneColumn(Tab& tab, const ui::PaneViewModel& view,
                             const ui::MainRenderer& renderer, const D2D1_RECT_F& bounds,
                             int divider, float cursor_x) {
    if (view.is_search)
        tab.search_column_dividers = renderer.ResizeSearchColumnDivider(
            bounds, tab.search_column_dividers, divider, cursor_x);
    else if (view.media_columns >= 1 && view.media_columns <= 3) {
        auto current = view;
        current.media_column_widths = tab.media_column_widths;
        current.details_column_dividers = tab.details_column_dividers;
        tab.media_column_widths[view.media_columns - 1] =
            renderer.ResizeMediaColumnDivider(bounds, current, divider, cursor_x);
        tab.details_column_dividers = renderer.ResizeMediaDetailsColumnDivider(bounds, current, divider, cursor_x);
    } else
        tab.details_column_dividers = renderer.ResizeDetailsColumnDivider(
            bounds, tab.details_column_dividers, divider, cursor_x, view.cloud_status_column);
}

inline void AutoFitPaneColumn(Tab& tab, const ui::PaneViewModel& view,
                              const ui::MainRenderer& renderer, const D2D1_RECT_F& bounds,
                              int divider) {
    if (!view.is_search && view.media_columns >= 1 && view.media_columns <= 3) {
        const auto layout = renderer.DetailsColumns(bounds, view);
        if (divider < 0 || divider >= layout.count - 1) return;
        for (int side : {divider, divider + 1}) {
            const int slot = ui::MainRenderer::MediaColumnSlot(layout.kinds[side], view.media_columns);
            if (slot >= 0 && ui::MainRenderer::IsMediaColumn(layout.kinds[side]))
                tab.media_column_widths[view.media_columns - 1][slot] = 0.0f;
            else if (const int normal = ui::MainRenderer::ManualColumnSlot(layout.kinds[side], false); normal >= 0)
                tab.details_column_dividers[normal] = 0.0f;
        }
        return;
    }
    renderer.AutoFitColumnDivider(bounds, tab.details_column_dividers,
                                   view.is_search, tab.search_column_dividers, divider,
                                   view.cloud_status_column);
}
}
