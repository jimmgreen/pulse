// Pure Miller-column row identity and drop target resolution.
#pragma once
#include "column_view_model.h"
#include "../ui/column_strip_layout.h"
#include <cwchar>
#include <string>
namespace pulse::app {
inline const ColumnStripColumn* FindColumnStripColumn(const ColumnStripState& state, int column_id) {
    if (column_id == ui::kColumnStripChildId) return state.child.path.empty() ? nullptr : &state.child;
    if (column_id >= 0 && static_cast<size_t>(column_id) < state.ancestors.size())
        return &state.ancestors[static_cast<size_t>(column_id)];
    return nullptr;
}
inline std::wstring ColumnStripRowItemPath(const ColumnStripState& state, const std::wstring& current_path,
                                          int column_id, int row_index) {
    if (state.for_path.empty() || _wcsicmp(state.for_path.c_str(), current_path.c_str()) != 0) return {};
    const ColumnStripColumn* col = FindColumnStripColumn(state, column_id);
    if (!col || col->path.empty() || !col->rows || !col->rows_snapshot ||
        col->snapshot != col->rows_snapshot || row_index < 0 || static_cast<size_t>(row_index) >= col->rows->size()) return {};
    const int source = (*col->rows)[static_cast<size_t>(row_index)];
    if (source < 0 || static_cast<size_t>(source) >= col->rows_snapshot->size()) return {};
    const fs::DirEntry& entry = (*col->rows_snapshot)[static_cast<size_t>(source)];
    if (entry.change_record_only || !entry.recycle_path.empty() || entry.name.empty() ||
        entry.name == L"." || entry.name == L"..") return {};
    if (!entry.full_path.empty()) return entry.full_path;
    std::wstring path = col->path;
    if (path.back() != L'\\' && path.back() != L'/') path += L'\\';
    return path + entry.name;
}
inline std::wstring ColumnDropDestination(const ColumnStripState& state, const std::wstring& current_path,
                                          int column_id, int row_index, bool row_hit) {
    if (state.for_path.empty() || _wcsicmp(state.for_path.c_str(), current_path.c_str()) != 0) return {};
    const ColumnStripColumn* col = FindColumnStripColumn(state, column_id);
    if (!col || col->path.empty()) return {};
    if (!row_hit) return col->path;
    if (!col->rows || !col->rows_snapshot || col->snapshot != col->rows_snapshot ||
        row_index < 0 || static_cast<size_t>(row_index) >= col->rows->size()) return {};
    const int source = (*col->rows)[static_cast<size_t>(row_index)];
    if (source < 0 || static_cast<size_t>(source) >= col->rows_snapshot->size()) return {};
    const fs::DirEntry& entry = (*col->rows_snapshot)[static_cast<size_t>(source)];
    if ((!entry.is_dir && !entry.link_target_is_dir) || entry.change_record_only ||
        !entry.recycle_path.empty() || entry.name.empty() || entry.name == L"." || entry.name == L"..") return {};
    if (!entry.full_path.empty()) return entry.full_path;
    std::wstring target = col->path;
    if (target.back() != L'\\' && target.back() != L'/') target += L'\\';
    return target + entry.name;
}
} // namespace pulse::app
