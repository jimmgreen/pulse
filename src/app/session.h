// session.h — Window/session snapshot save/load.
#pragma once
#include "app_model.h"
#include <array>
#include <functional>
#include <string>
#include <windows.h>

namespace pulse::app {

struct GroupSessionSnapshot {
    int id = 0;
    std::wstring name;
    uint32_t color_rgb = 0;
    bool collapsed = false;
};

struct PaneFolderSnapshot {
    std::wstring path;
    ui::ViewMode view = ui::ViewMode::Details;
    std::array<float, 3> columns{};
    std::array<float, 4> search_columns{};
};

struct LayoutTabSnapshot {
    std::wstring title;
    uint32_t marker_rgb = 0;
    bool pinned = false;
    int group = 0;
    int layout = 0;
    int focused = 0;
    int target = -1;
    std::vector<float> split_ratios;
    std::vector<PaneFolderSnapshot> panes;
};

struct SessionSnapshot {
    RECT window_rect = {};
    bool maximized = false;
    bool dark = false;
    std::wstring active_path;
    StagingTray tray;
    std::wstring undo_json; // ops::OpsManager undo stack (serialized array)
    int sidebar_collapsed = 0; // bitmask over WindowViewModel::sidebar group order
    bool starred_expanded = true;
    bool details_panel = false;  // right details panel visible
    int details_panel_width = 340;
    bool details_preview_only = false;
    std::array<float, 3> details_column_dividers{}; // version 3 migration only
    std::vector<LayoutTabSnapshot> layout_tabs;
    std::vector<GroupSessionSnapshot> tab_groups;
    int active_layout_tab = 0;
};

std::wstring GetPulseDataDir();
bool SaveSession(const SessionSnapshot& snap);
bool LoadSession(SessionSnapshot& snap);

std::wstring LayoutTabsToJson(const std::vector<LayoutTabSnapshot>& tabs);
bool ParseLayoutTabs(const std::wstring& array_json,
                     std::vector<LayoutTabSnapshot>& out);
std::wstring TabGroupsToJson(const std::vector<GroupSessionSnapshot>& groups);
bool ParseTabGroups(const std::wstring& array_json,
                    std::vector<GroupSessionSnapshot>& out);

using SessionTabLoader = std::function<void(Tab&, const std::wstring&)>;
LayoutTabSnapshot CaptureLayoutTab(const LayoutTab& tab);
void RestoreLayoutTab(LayoutTab& tab, const LayoutTabSnapshot& snapshot,
                      const SessionTabLoader& load_tab);
void RestoreWindowTabs(WindowTabs& tabs,
                       const std::vector<LayoutTabSnapshot>& layout_tabs,
                       const std::vector<GroupSessionSnapshot>& groups,
                       int active_index,
                       const SessionTabLoader& load_tab);

} // namespace pulse::app
