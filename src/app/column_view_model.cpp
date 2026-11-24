#include "column_view_model.h"
#include "app_model.h"
#include "../common/path_utils.h"

namespace pulse::app {

namespace {

ui::ColumnStripColumnView ToView(const ColumnStripColumn& column) {
    ui::ColumnStripColumnView view;
    view.path = column.path;
    view.title = column.title;
    view.snapshot = column.rows ? column.rows_snapshot : nullptr;
    view.rows = view.snapshot ? column.rows : nullptr;
    view.highlight_row = column.highlight_row;
    view.scroll_dip = column.scroll_dip;
    view.auto_scroll = column.auto_scroll;
    view.loading = !column.snapshot && !column.error;
    view.error = column.error && !column.snapshot;
    return view;
}

} // namespace

void FillColumnStripView(ui::ColumnStripView& out, const Tab& tab) {
    const ColumnStripState& state = tab.column_strip;
    out.enabled = tab.column_layout;
    out.widths_dip = tab.column_widths_dip;
    out.scroll_from_right_dip = state.scroll_from_right_dip;
    out.ancestor_viewport_limit_dip.reset();
    out.eligible = tab.column_layout && !state.for_path.empty() &&
                   state.for_path == tab.current_path;
    out.ancestors.clear();
    out.has_child = false;
    if (!out.eligible) return;
    if (state.click_anchor && path::EqualInsensitive(state.click_anchor->path, tab.current_path))
        out.ancestor_viewport_limit_dip = state.click_anchor->viewport_dip;
    out.ancestors.reserve(state.ancestors.size());
    for (const ColumnStripColumn& column : state.ancestors) out.ancestors.push_back(ToView(column));
    // Only a selected folder gets a column; file properties live in the
    // details panel.
    if (!state.child.path.empty()) {
        out.has_child = true;
        out.child = ToView(state.child);
    }
}

} // namespace pulse::app
