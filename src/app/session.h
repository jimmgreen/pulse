// session.h — Window/session snapshot save/load.
#pragma once
#include "app_model.h"
#include <array>
#include <string>
#include <windows.h>

namespace pulse::app {

struct TabSessionSnapshot {
    std::wstring path;
    bool pinned = false;
    int group = 0; // TabGroup::id of the owning group; 0 = none
    ui::ViewMode view = ui::ViewMode::Details;
    std::array<float, 3> columns{}; // details column divider ratios
};

struct GroupSessionSnapshot {
    int id = 0;
    std::wstring name;
    uint32_t color_rgb = 0;
    bool collapsed = false;
};

struct PaneSessionSnapshot {
    int active = 0; // index into tabs
    std::vector<GroupSessionSnapshot> groups;
    std::vector<TabSessionSnapshot> tabs;
};

struct SessionSnapshot {
    RECT window_rect = {};
    bool maximized = false;
    bool dark = false;
    std::wstring active_path;
    int layout = 0;
    int focused_pane = 0;
    int target_pane = -1;
    std::vector<std::wstring> pane_paths;
    std::vector<ui::ViewMode> pane_views;
    std::vector<std::array<float, 3>> pane_column_dividers;
    StagingTray tray;
    std::wstring undo_json; // ops::OpsManager undo stack (serialized array)
    int sidebar_collapsed = 0; // bitmask over WindowViewModel::sidebar group order
    bool starred_expanded = true;
    bool details_panel = false;  // right details panel visible
    int details_panel_width = 340;
    std::array<float, 3> details_column_dividers{}; // version 3 migration only
    std::vector<PaneSessionSnapshot> pane_tabs; // version 5: full tab/group state per pane
    std::vector<float> split_ratios; // preorder non-leaf splitter ratios
};

std::wstring GetPulseDataDir();
bool SaveSession(const SessionSnapshot& snap);
bool LoadSession(SessionSnapshot& snap);

// Serialize/parse the per-pane tab+group array ("paneTabs" key). Pure
// functions so the selftest can round-trip them without a window.
std::wstring PaneTabsToJson(const std::vector<PaneSessionSnapshot>& panes);
bool ParsePaneTabs(const std::wstring& array_json,
                   std::vector<PaneSessionSnapshot>& out);

} // namespace pulse::app
