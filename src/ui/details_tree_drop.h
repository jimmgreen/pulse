#pragma once
#include "details_tree_rows.h"
namespace pulse::ui {
// Child file rows belong to the expanded folder, not to the pane's root.
inline int DetailsTreeDropFolderRow(const DetailsTreeRows& tree, int view_row) {
    const auto* row = tree.At(view_row);
    if (!row) return -1;
    if (row->kind == TreeRowKind::Entry && row->expandable) return view_row;
    return tree.ParentRow(view_row);
}
}
