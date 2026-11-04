// column_view_model.h — Per-tab listing state of the column (Miller) view.
#pragma once
#include "../fs/fs_snapshot.h"
#include <cstdint>
#include <memory>
#include <map>
#include <cwchar>
#include <string>
#include <vector>

namespace pulse::ui { struct ColumnStripView; }

namespace pulse::app {

struct Tab;

struct ColumnStripColumn {
    std::wstring path;                 // normalized folder path
    std::wstring title;                // caption override (drive label), may be empty
    fs::SnapshotPtr snapshot;          // last listing (may be stale while refreshing)
    std::shared_ptr<const std::vector<int>> rows; // visible source indices
    fs::SnapshotPtr rows_snapshot;     // snapshot `rows` was built from
    bool rows_show_hidden = false;
    bool rows_show_protected = false;
    std::wstring highlight_name;       // entry on the navigation path, if any
    int highlight_row = -1;
    uint64_t generation = 0;           // worker generation still awaited (0 = none)
    uint64_t store_revision = 0;       // SnapshotStore state last observed
    bool requested = false;
    bool error = false;
    float scroll_dip = 0.0f;
    bool auto_scroll = true;
};

// Stable folder identity across column-role transitions and path casing.
struct ColumnPathLess {
    bool operator()(const std::wstring& a, const std::wstring& b) const noexcept {
        return _wcsicmp(a.c_str(), b.c_str()) < 0;
    }
};
using ColumnPathWidths = std::map<std::wstring, float, ColumnPathLess>;

struct ColumnStripState {
    std::wstring for_path;                     // current_path the ancestors belong to
    std::vector<ColumnStripColumn> ancestors;  // farthest -> parent
    ColumnStripColumn child;                   // selected folder (path empty = none)
    float scroll_left_dip = -1.0f;             // whole-chain left scroll; -1 = newest
    bool hold_scroll_on_navigate = false;     // column click preserves the viewport
    ColumnPathWidths widths_by_path;            // only user-set or one-shot fitted widths

    bool Empty() const { return for_path.empty() && ancestors.empty() && child.path.empty(); }
};

// Column view data for the renderer (cheap: shares snapshots and row lists).
void FillColumnStripView(ui::ColumnStripView& out, const Tab& tab);

} // namespace pulse::app
