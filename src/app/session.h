// session.h — Window/session snapshot save/load.
#pragma once
#include "app_model.h"
#include <array>
#include <string>
#include <windows.h>

namespace pulse::app {

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
    bool details_panel = false;  // right details panel visible
    int details_panel_width = 340;
    int details_preview_height = 420;
    std::array<float, 3> details_column_dividers{}; // version 3 migration only
};

std::wstring GetPulseDataDir();
bool SaveSession(const SessionSnapshot& snap);
bool LoadSession(SessionSnapshot& snap);

} // namespace pulse::app
