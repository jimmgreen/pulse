#include "column_view_model.h"
#include "app_model.h"
#include "../ui/column_strip_layout.h"

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
    out.scroll_left_dip = state.scroll_left_dip;
    out.eligible = tab.column_layout && !state.for_path.empty() &&
                   state.for_path == tab.current_path;
    out.ancestors.clear();
    out.has_child = false;
    if (!out.eligible) return;
    // Retain directory widths when a column changes ancestor/body/child role.
    // The old slot widths still provide defaults for new paths.
    out.widths_dip = tab.column_widths_dip;
    const auto assign = [&](const std::wstring& path, size_t slot) {
        const auto it = state.widths_by_path.find(path);
        if (it == state.widths_by_path.end()) return;
        if (out.widths_dip.size() <= slot) out.widths_dip.resize(slot + 1, 0.0f);
        out.widths_dip[slot] = it->second;
    };
    out.ancestors.reserve(state.ancestors.size());
    for (size_t i = 0; i < state.ancestors.size(); ++i) {
        assign(state.ancestors[i].path,
               ui::ColumnStripWidthSlot(static_cast<int>(i), state.ancestors.size()));
        out.ancestors.push_back(ToView(state.ancestors[i]));
    }
    assign(state.for_path, ui::kColumnStripBodySlot);
    assign(state.child.path, 0);
    // Only a selected folder gets a column; file properties live in the
    // details panel.
    if (!state.child.path.empty()) {
        out.has_child = true;
        out.child = ToView(state.child);
    }
}

} // namespace pulse::app
