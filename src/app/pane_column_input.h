#pragma once
#include "app_model.h"
#include "../ui/ui_renderer.h"
#include "extension_columns_ui.h"
#include <algorithm>

namespace pulse::app {
inline constexpr float kDetailsMinNameDipForResize = 80.0f;   // ui kDetailsMinNameDip

// Extension columns (pulse.columns) sit after every other column and take
// their room from the name. A divider touching one sets that column's user
// width; true when `divider` was such a divider.
inline bool ResizeExtensionColumn(const ui::PaneViewModel& view, const ui::MainRenderer::DetailsColumnLayout& layout,
                                  int divider, float cursor_x, float scale) {
    using R = ui::MainRenderer;
    if (layout.extension_count <= 0 || divider < layout.OrderableEnd() - 1 || divider >= layout.count - 1 ||
        scale <= 0.0f)
        return false;
    const size_t left = static_cast<size_t>(divider);
    const size_t right = left + 1;
    const float x0 = divider == 0 ? layout.left : layout.DividerX(divider - 1);
    const float x1 = layout.DividerX(divider);
    const float x2 = x1 + layout.widths[right];
    const float min_w = 48.0f * scale;
    const auto store = [&](R::ColumnKind kind, float px) {
        const size_t ext = R::ExtensionColumnIndex(kind);
        if (ext < view.extension_columns.size())
            SetExtensionColumnWidth(view.extension_columns[ext].key, std::clamp(px / scale, 48.0f, 480.0f));
    };
    if (!R::IsExtensionColumn(layout.kinds[left])) {
        // First extension column: its right edge stays, the name gives way
        // down to its minimum.
        const float name_spare = std::max(0.0f, layout.widths[0] - (view.media_columns ? 200.0f : kDetailsMinNameDipForResize) * scale);
        const float w = std::clamp(x2 - cursor_x, min_w, std::max(min_w, layout.widths[right] + name_spare));
        store(layout.kinds[right], w);
    } else {
        const float edge = std::clamp(cursor_x, x0 + min_w, std::max(x0 + min_w, x2 - min_w));
        store(layout.kinds[left], edge - x0);
        store(layout.kinds[right], x2 - edge);
    }
    return true;
}

inline void ResizePaneColumn(Tab& tab, const ui::PaneViewModel& view,
                             const ui::MainRenderer& renderer, const D2D1_RECT_F& bounds,
                             int divider, float cursor_x) {
    if (!view.extension_columns.empty() && !view.is_search) {
        const auto layout = renderer.DetailsColumns(bounds, view);
        if (ResizeExtensionColumn(view, layout, divider, cursor_x, renderer.Scale())) return;
        // The other columns sit further left than in the layouts below,
        // which do not have the extension columns.
        cursor_x += ui::MainRenderer::ExtensionColumnsWidth(layout);
    }
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
    if (!view.extension_columns.empty() && !view.is_search) {
        const auto layout = renderer.DetailsColumns(bounds, view);
        if (layout.extension_count > 0 && divider >= layout.OrderableEnd() - 1 && divider < layout.count - 1) {
            // Back to the widths the extensions asked for.
            for (int side : {divider, divider + 1}) {
                const auto kind = layout.kinds[static_cast<size_t>(side)];
                if (!ui::MainRenderer::IsExtensionColumn(kind)) continue;
                const size_t ext = ui::MainRenderer::ExtensionColumnIndex(kind);
                if (ext < view.extension_columns.size()) SetExtensionColumnWidth(view.extension_columns[ext].key, 0.0f);
            }
            return;
        }
    }
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