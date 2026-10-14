// session.h — Window/session snapshot save/load.
#pragma once
#include "app_model.h"
#include "window_identity.h"
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
    // Index into SessionSnapshot::layout_tabs of the member tab this group last
    // used (see RememberGroupActivation), -1 when nothing is remembered. The
    // live form is a LayoutTab pointer, so the session keeps the position.
    int last_active = -1;
};

struct PaneFolderSnapshot {
    std::wstring path;
    ui::ViewMode view = ui::ViewMode::Details;
    ui::DetailsColumnWidths columns{};
    std::array<float, 4> search_columns{};
    bool column_layout = false;
    std::vector<float> column_widths; // DIP, see ui::ColumnStripWidthDip
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
    // M7k: SplitTreeShape of a custom split tree (then `layout` holds the
    // preset with as many panes, for older versions); empty for a preset.
    std::wstring split_tree;
    std::vector<PaneFolderSnapshot> panes;
};

// The layout around one window's tabs (sidebar and details panel). Session
// version 8 keeps one per window; field meanings as in SessionSnapshot.
struct WindowLayoutSnapshot {
    int sidebar_collapsed = 0;
    int sidebar_hidden = 0;
    std::vector<int> sidebar_order;
    int quick_access_hidden = 0;
    bool starred_expanded = true;
    bool details_panel = false;
    int details_panel_width = 340;
    bool details_preview_only = false;
    bool details_preview = true;
};

// One window of a version-8 session.
struct WindowSessionSnapshot {
    RECT window_rect = {};  // rcNormalPosition: workspace coordinates
    bool maximized = false;
    WindowLayoutSnapshot layout;
    std::vector<LayoutTabSnapshot> layout_tabs;
    std::vector<GroupSessionSnapshot> tab_groups;
    int active_layout_tab = 0;
    WindowIdentity identity;  // M7c: its name and color
};

// A window closed while others stayed open (M7f, plan §7): Ctrl+Shift+T
// reopens it after a restart too. Here `window.window_rect` is the screen
// rectangle it had (app::ClosedWindow::rect), not workspace coordinates.
struct ClosedWindowSessionSnapshot {
    WindowSessionSnapshot window;
    std::wstring name;  // its active tab's title, for the toast
};
// How many closed windows the session keeps (the newest).
constexpr size_t kSessionClosedWindows = 5;

struct SessionSnapshot {
    RECT window_rect = {};
    bool maximized = false;
    bool dark = false;
    std::wstring active_path;
    StagingTray tray;
    std::wstring undo_json; // ops::OpsManager undo stack (serialized array)
    int sidebar_collapsed = 0; // bitmask indexed by SidebarSectionId
    int sidebar_hidden = 0;    // same ids; hidden sections are not drawn at all
    std::vector<int> sidebar_order; // display order, empty = DefaultSidebarOrder()
    int quick_access_hidden = 0;    // bitmask over BuiltinQuickAccess links
    bool starred_expanded = true;
    bool details_panel = false;  // right details panel visible
    int details_panel_width = 340;
    bool details_preview_only = false;
    bool details_preview = true; // preview pane enabled (session remembers it)
    std::array<float, 3> details_column_dividers{}; // version 3 migration only
    std::vector<LayoutTabSnapshot> layout_tabs;
    std::vector<GroupSessionSnapshot> tab_groups;
    int active_layout_tab = 0;
    // Version 8: every window, most recently used first, and the index of the
    // focused one. The single-window fields above mirror windows[focused_window]
    // (the writer fills both), so versions that read one window restore that
    // one. ParseSessionJson turns a file without windows[] into windows[0];
    // SessionToJson leaves windows[] out when it is empty.
    std::vector<WindowSessionSnapshot> windows;
    int focused_window = 0;
    // M7f: windows closed while others stayed open, oldest first. Written
    // after windows[] (the newest kSessionClosedWindows) as "closedWindows".
    std::vector<ClosedWindowSessionSnapshot> closed_windows;
};

std::wstring GetPulseDataDir();
bool SaveSession(const SessionSnapshot& snap);
// Autosave compares the serialized form with the last write and skips unchanged state.
std::wstring SessionToJson(const SessionSnapshot& snap);
bool WriteSessionJson(const std::wstring& json);
bool LoadSession(SessionSnapshot& snap);
// LoadSession's parser, for tests.
bool ParseSessionJson(const std::wstring& json, SessionSnapshot& snap);

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
