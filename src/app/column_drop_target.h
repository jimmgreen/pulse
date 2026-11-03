// Pure Miller-column drop target selection. A column row is never the
// active tab's current-path row: its index belongs to that column's snapshot.
#pragma once
#include "column_view_model.h"
#include "../ui/column_strip_layout.h"
#include <cwchar>
#include <string>

namespace pulse::app {

// row_hit=false -> the background of this column; row_hit=true -> its folder
// row. Return empty on any uncertain target rather than guessing a directory.
inline std::wstring ColumnDropDestination(const ColumnStripState& state,
                                          const std::wstring& current_path,
                                          int column_id, int row_index,
                                          bool row_hit) {
    if (state.for_path.empty() ||
        _wcsicmp(state.for_path.c_str(), current_path.c_str()) != 0) return {};
    const ColumnStripColumn* col = nullptr;
    if (column_id == ui::kColumnStripChildId) {
        if (!state.child.path.empty()) col = &state.child;
    } else if (column_id >= 0 && static_cast<size_t>(column_id) < state.ancestors.size()) {
        col = &state.ancestors[static_cast<size_t>(column_id)];
    }
    if (!col || col->path.empty()) return {};
    if (!row_hit) return col->path;
    // On a row, use exactly the displayed row mapping. Do not use the active
    // tab's selected_index or an out-of-date list while the column refreshes.
    if (row_index < 0 || !col->rows || !col->rows_snapshot ||
        col->rows_snapshot != col->snapshot ||
        static_cast<size_t>(row_index) >= col->rows->size()) return {};
    const int source_index = (*col->rows)[static_cast<size_t>(row_index)];
    if (source_index < 0 ||
        static_cast<size_t>(source_index) >= col->rows_snapshot->size()) return {};
    const fs::DirEntry& entry = (*col->rows_snapshot)[static_cast<size_t>(source_index)];
    if (!entry.is_dir || entry.change_record_only || !entry.recycle_path.empty() ||
        entry.name.empty() || entry.name == L"." || entry.name == L"..") return {};
    std::wstring target = col->path;
    if (target.back() != L'\\' && target.back() != L'/') target += L'\\';
    return target + entry.name;
}

} // namespace pulse::app
