// session.cpp
#include "session.h"
#include "split_tree_shape.h"
#include "../common/config_json.h"
#include "../ui/panel_metrics.h"
#include "../common/json_utils.h"
#include "../common/utf8_file.h"
#include <commctrl.h>
#include <prsht.h>
#include <shlobj.h>
#include <sstream>
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace pulse::app {

namespace {

// Session file version. Bump when a stored field changes meaning: the loader
// migrates (or discards) values written by older versions.
//   6 -> 7: BuiltinQuickAccess lost its reserved "starred" bit, so the stored
//           quick-access mask is dropped for files written before 7.
//   7 -> 8: windows[] holds every window (multi-window). The single-window
//           fields stay and mirror the focused window, so version 7 readers
//           (which accept any version) still restore that window. windows[]
//           is written last: the readers find keys by their first occurrence.
constexpr int kSessionVersion = 8;
// First version whose quickAccessHidden uses the current bit positions.
constexpr int kQuickAccessBitsVersion = 7;

// Folder-view column widths: modified, type, size[, created, accessed].
// The two date columns are only written when set, so the value stays the
// three numbers older versions parse (they read the first three of five too).
std::wstring FormatDetailsWidths(const ui::DetailsColumnWidths& edges) {
    const size_t count = edges[3] > 0.0f || edges[4] > 0.0f ? 5 : 3;
    std::wstring out;
    for (size_t i = 0; i < count; ++i) {
        if (i) out += L",";
        out += std::to_wstring(static_cast<int>(std::lround(edges[i] * 10000.0f)));
    }
    return out;
}

ui::DetailsColumnWidths ParseDetailsWidths(const std::wstring& value) {
    std::array<int, 5> edges{};
    ui::DetailsColumnWidths ratios{};
    // Values <= 1 are pre-1.0.39 divider ratios (read back as "automatic");
    // larger values are manual column widths in DIP. 0 = automatic.
    const int read = swscanf_s(value.c_str(), L"%d,%d,%d,%d,%d",
                               &edges[0], &edges[1], &edges[2], &edges[3], &edges[4]);
    if (read != 3 && read != 5) return ratios;
    if (read == 3) edges[3] = edges[4] = 0;
    if (std::all_of(edges.begin(), edges.end(), [](int v) { return v >= 0 && v <= 40000000; })) {
        for (size_t i = 0; i < ratios.size(); ++i)
            ratios[i] = static_cast<float>(edges[i]) / 10000.0f;
    }
    return ratios;
}

std::wstring FormatScaled4(const std::array<float, 4>& edges) {
    return std::to_wstring(static_cast<int>(std::lround(edges[0] * 10000.0f))) + L","
         + std::to_wstring(static_cast<int>(std::lround(edges[1] * 10000.0f))) + L","
         + std::to_wstring(static_cast<int>(std::lround(edges[2] * 10000.0f))) + L","
         + std::to_wstring(static_cast<int>(std::lround(edges[3] * 10000.0f)));
}

std::array<float, 4> ParseScaled4(const std::wstring& value) {
    std::array<int, 4> edges{};
    std::array<float, 4> ratios{};
    if (swscanf_s(value.c_str(), L"%d,%d,%d,%d",
                  &edges[0], &edges[1], &edges[2], &edges[3]) == 4 &&
        std::all_of(edges.begin(), edges.end(), [](int v) { return v >= 0 && v <= 40000000; })) {
        for (size_t i = 0; i < ratios.size(); ++i)
            ratios[i] = static_cast<float>(edges[i]) / 10000.0f;
    }
    return ratios;
}

std::wstring FormatScaledList(const std::vector<float>& values) {
    std::wstring out;
    for (size_t i = 0; i < values.size(); ++i) {
        if (i) out += L",";
        out += std::to_wstring(static_cast<int>(std::lround(values[i] * 10000.0f)));
    }
    return out;
}

std::vector<float> ParseScaledList(const std::wstring& value) {
    std::vector<float> out;
    size_t pos = 0;
    while (pos < value.size()) {
        const size_t comma = value.find(L',', pos);
        const std::wstring token = value.substr(
            pos, comma == std::wstring::npos ? std::wstring::npos : comma - pos);
        const int scaled = _wtoi(token.c_str());
        if (scaled > 0 && scaled < 10000)
            out.push_back(static_cast<float>(scaled) / 10000.0f);
        if (comma == std::wstring::npos) break;
        pos = comma + 1;
    }
    return out;
}

// Column view widths: whole DIP values, 0 = default width for that slot.
std::wstring FormatDipList(const std::vector<float>& values) {
    std::wstring out;
    for (size_t i = 0; i < values.size(); ++i) {
        if (i) out += L",";
        out += std::to_wstring(std::max(0, static_cast<int>(std::lround(values[i]))));
    }
    return out;
}

std::vector<float> ParseDipList(const std::wstring& value) {
    std::vector<float> out;
    size_t pos = 0;
    while (pos < value.size() && out.size() < 64) {
        const size_t comma = value.find(L',', pos);
        const std::wstring token = value.substr(
            pos, comma == std::wstring::npos ? std::wstring::npos : comma - pos);
        const int dip = _wtoi(token.c_str());
        out.push_back(dip > 0 && dip < 4096 ? static_cast<float>(dip) : 0.0f);
        if (comma == std::wstring::npos) break;
        pos = comma + 1;
    }
    while (!out.empty() && out.back() <= 0.0f) out.pop_back();
    return out;
}

// Section order travels as "0,1,2,6,3,4,5"; anything malformed falls back to
// the default order (NormalizeSidebarOrder rebuilds a full permutation).
std::vector<int> ParseSidebarOrder(const std::wstring& order_text) {
    std::vector<int> order;
    size_t start = 0;
    while (start < order_text.size()) {
        size_t end = order_text.find(L',', start);
        if (end == std::wstring::npos) end = order_text.size();
        int value = 0;
        bool digits = end > start;
        for (size_t i = start; i < end && digits; ++i) {
            const wchar_t c = order_text[i];
            if (c < L'0' || c > L'9') { digits = false; break; }
            value = value * 10 + static_cast<int>(c - L'0');
        }
        if (digits && value < kSidebarSectionCount) order.push_back(value);
        start = end + 1;
    }
    // Sessions written before the starred section existed lack its id; put it
    // where the default order has it instead of appending it at the very end.
    const int starred_id = static_cast<int>(SidebarSectionId::Starred);
    if (!order.empty() && std::find(order.begin(), order.end(), starred_id) == order.end()) {
        const auto cloud = std::find(order.begin(), order.end(),
                                     static_cast<int>(SidebarSectionId::Cloud));
        order.insert(cloud == order.end() ? order.begin() : cloud + 1, starred_id);
    }
    return NormalizeSidebarOrder(order);
}

std::wstring FormatSidebarOrder(const std::vector<int>& order) {
    std::wstring out;
    for (size_t i = 0; i < order.size(); ++i) {
        if (i != 0) out += L",";
        out += std::to_wstring(order[i]);
    }
    return out;
}

int ClampDetailsPanelWidth(int width) {
    return std::clamp(width, static_cast<int>(ui::kDetailsMinWidthDip),
                      static_cast<int>(ui::kPanelWidthMaxDip));
}

// One windows[] element; same key names as the single-window fields.
std::wstring WindowToJson(const WindowSessionSnapshot& w) {
    const WindowLayoutSnapshot& l = w.layout;
    std::wostringstream f;
    f << L"    {\"left\":" << w.window_rect.left << L",\"top\":" << w.window_rect.top
      << L",\"right\":" << w.window_rect.right << L",\"bottom\":" << w.window_rect.bottom
      << L",\"maximized\":" << (w.maximized ? L"true" : L"false") << L",\n";
    f << L"     \"sidebarCollapsed\":" << l.sidebar_collapsed
      << L",\"sidebarHidden\":" << l.sidebar_hidden
      << L",\"sidebarOrder\":\"" << FormatSidebarOrder(l.sidebar_order) << L"\""
      << L",\"quickAccessHidden\":" << l.quick_access_hidden
      << L",\"starredExpanded\":" << (l.starred_expanded ? L"true" : L"false") << L",\n";
    f << L"     \"detailsPanel\":" << (l.details_panel ? 1 : 0)
      << L",\"detailsPanelWidth\":" << ClampDetailsPanelWidth(l.details_panel_width)
      << L",\"detailsPreviewOnly\":" << (l.details_preview_only ? L"true" : L"false")
      << L",\"detailsPreview\":" << (l.details_preview ? L"true" : L"false") << L",\n";
    // M7c: "windowName" "" = named after its folders; unique keys, since
    // the lookups below also see the nested groups' "name" and "color".
    std::wstring name;
    pulse::json::Escape(w.identity.name, name);
    f << L"     \"windowName\":\"" << name << L"\",\"windowColor\":" << w.identity.color << L",\n";
    f << L"     \"activeTab\":" << w.active_layout_tab << L",\n";
    f << L"     \"tabGroups\":" << TabGroupsToJson(w.tab_groups) << L",\n";
    f << L"     \"layoutTabs\":" << LayoutTabsToJson(w.layout_tabs) << L"}";
    return f.str();
}

} // namespace

std::wstring LayoutTabsToJson(const std::vector<LayoutTabSnapshot>& tabs) {
    std::wstring out = L"[";
    for (size_t i = 0; i < tabs.size(); ++i) {
        if (i) out += L",";
        const LayoutTabSnapshot& tab = tabs[i];
        out += L"{\"pinned\":";
        out += tab.pinned ? L"true" : L"false";
        out += L",\"group\":" + std::to_wstring(tab.group);
        out += L",\"marker_rgb\":" + std::to_wstring(tab.marker_rgb);
        out += L",\"title\":\"";
        std::wstring escaped;
        pulse::json::Escape(tab.title, escaped);
        out += escaped;
        out += L"\",\"layout\":" + std::to_wstring(tab.layout);
        out += L",\"focused\":" + std::to_wstring(tab.focused);
        out += L",\"target\":" + std::to_wstring(tab.target);
        out += L",\"splitRatios\":\"";
        out += FormatScaledList(tab.split_ratios);
        if (!tab.split_tree.empty()) {
            out += L"\",\"splitTree\":\"";
            out += tab.split_tree;  // only P, V and H: nothing to escape
        }
        out += L"\",\"panes\":[";
        for (size_t p = 0; p < tab.panes.size(); ++p) {
            if (p) out += L",";
            const PaneFolderSnapshot& pane = tab.panes[p];
            out += L"{\"path\":\"";
            escaped.clear();
            pulse::json::Escape(pane.path, escaped);
            out += escaped;
            out += L"\",\"view\":\"";
            out += ui::ViewModeName(pane.view);
            out += L"\",\"cols\":\"";
            out += FormatDetailsWidths(pane.columns);
            out += L"\",\"searchCols\":\"";
            out += FormatScaled4(pane.search_columns);
            out += L"\",\"colLayout\":";
            out += pane.column_layout ? L"true" : L"false";
            out += L",\"colWidths\":\"";
            out += FormatDipList(pane.column_widths);
            out += L"\"}";
        }
        out += L"]}";
    }
    out += L"]";
    return out;
}

std::wstring TabGroupsToJson(const std::vector<GroupSessionSnapshot>& groups) {
    std::wstring out = L"[";
    for (size_t g = 0; g < groups.size(); ++g) {
        if (g) out += L",";
        const GroupSessionSnapshot& grp = groups[g];
        out += L"{\"id\":" + std::to_wstring(grp.id);
        out += L",\"name\":\"";
        std::wstring escaped;
        pulse::json::Escape(grp.name, escaped);
        out += escaped;
        out += L"\",\"color\":" + std::to_wstring(grp.color_rgb);
        out += L",\"collapsed\":";
        out += grp.collapsed ? L"true" : L"false";
        out += L",\"lastActive\":" + std::to_wstring(grp.last_active);
        out += L"}";
    }
    out += L"]";
    return out;
}

// Extract the contents of each top-level {...} object inside an array body
// (without the outer brackets); returns false on unbalanced input.
static bool SplitTopLevelObjects(const std::wstring& body,
                                 std::vector<std::wstring>& out) {
    return pulse::json::ForEachElement(L"[" + body + L"]", [&](const std::wstring& element) {
        if (!element.empty() && element.front() == L'{') out.push_back(element);
    });
}

// The body (without brackets) of key's array value.
static bool ExtractJsonArray(const std::wstring& json, const wchar_t* key,
                             std::wstring& body) {
    const std::wstring array = pulse::json::ExtractArray(json, key);
    if (array.size() < 2) return false;
    body = array.substr(1, array.size() - 2);
    return true;
}

bool ParseTabGroups(const std::wstring& array_json,
                    std::vector<GroupSessionSnapshot>& out) {
    out.clear();
    if (array_json.size() < 2 || array_json.front() != L'[' ||
        array_json.back() != L']') {
        return false;
    }
    std::vector<std::wstring> objs;
    if (!SplitTopLevelObjects(array_json.substr(1, array_json.size() - 2), objs))
        return false;
    for (const std::wstring& gj : objs) {
        GroupSessionSnapshot grp;
        grp.id = pulse::json::ExtractInt(gj, L"id");
        grp.name = pulse::json::ExtractString(gj, L"name");
        grp.color_rgb = static_cast<uint32_t>(std::max(
            0, pulse::json::ExtractInt(gj, L"color")));
        grp.collapsed = pulse::json::ExtractBool(gj, L"collapsed");
        grp.last_active = pulse::json::ExtractInt(gj, L"lastActive", -1);
        out.push_back(std::move(grp));
    }
    return true;
}

bool ParseLayoutTabs(const std::wstring& array_json,
                     std::vector<LayoutTabSnapshot>& out) {
    out.clear();
    if (array_json.size() < 2 || array_json.front() != L'[' ||
        array_json.back() != L']') {
        return false;
    }
    std::vector<std::wstring> tabObjs;
    if (!SplitTopLevelObjects(array_json.substr(1, array_json.size() - 2), tabObjs))
        return false;
    for (const std::wstring& tabJson : tabObjs) {
        LayoutTabSnapshot tab;
        tab.pinned = pulse::json::ExtractBool(tabJson, L"pinned");
        tab.group = pulse::json::ExtractInt(tabJson, L"group");
        tab.title = pulse::json::ExtractString(tabJson, L"title");
        tab.marker_rgb = static_cast<uint32_t>(std::clamp(
            pulse::json::ExtractInt(tabJson, L"marker_rgb"), 0, 0xFFFFFF));
        tab.layout = pulse::json::ExtractInt(tabJson, L"layout");
        tab.focused = pulse::json::ExtractInt(tabJson, L"focused");
        tab.target = pulse::json::ExtractInt(tabJson, L"target");
        if (tabJson.find(L"\"target\"") == std::wstring::npos) tab.target = -1;
        tab.split_ratios = ParseScaledList(
            pulse::json::ExtractString(tabJson, L"splitRatios"));
        tab.split_tree = pulse::json::ExtractString(tabJson, L"splitTree");
        if (ShapePaneCount(tab.split_tree) == 0) tab.split_tree.clear();

        std::wstring panesBody;
        if (ExtractJsonArray(tabJson, L"panes", panesBody)) {
            std::vector<std::wstring> paneObjs;
            if (!SplitTopLevelObjects(panesBody, paneObjs)) return false;
            for (const std::wstring& pj : paneObjs) {
                PaneFolderSnapshot pane;
                pane.path = pulse::json::ExtractString(pj, L"path");
                pane.view = ui::ParseViewMode(pulse::json::ExtractString(pj, L"view"));
                pane.columns = ParseDetailsWidths(pulse::json::ExtractString(pj, L"cols"));
                pane.search_columns = ParseScaled4(
                    pulse::json::ExtractString(pj, L"searchCols"));
                pane.column_layout = pulse::json::ExtractBool(pj, L"colLayout");
                pane.column_widths = ParseDipList(pulse::json::ExtractString(pj, L"colWidths"));
                tab.panes.push_back(std::move(pane));
            }
        }
        out.push_back(std::move(tab));
    }
    return true;
}

std::wstring GetPulseDataDir() {
#ifdef PULSE_WITH_SELFTEST
    wchar_t test_dir[32768]{};
    const DWORD length = GetEnvironmentVariableW(L"PULSE_TEST_DATA_DIR", test_dir, ARRAYSIZE(test_dir));
    if (length > 0 && length < ARRAYSIZE(test_dir)) {
        CreateDirectoryW(test_dir, nullptr);
        return test_dir;
    }
#endif
    wchar_t path[MAX_PATH] = {};
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, path))) {
        std::wstring dir = std::wstring(path) + L"\\Pulse";
        CreateDirectoryW(dir.c_str(), nullptr);
        return dir;
    }
    return L"";
}

std::wstring SessionToJson(const SessionSnapshot& snap) {
    std::wstring trayJson;
    snap.tray.ToJson(trayJson);

    std::wostringstream f;
    f << L"{\n";
    f << L"  \"version\":" << kSessionVersion << L",\n";
    f << L"  \"left\":" << snap.window_rect.left << L",\n";
    f << L"  \"top\":" << snap.window_rect.top << L",\n";
    f << L"  \"right\":" << snap.window_rect.right << L",\n";
    f << L"  \"bottom\":" << snap.window_rect.bottom << L",\n";
    f << L"  \"maximized\":" << (snap.maximized ? L"true" : L"false") << L",\n";
    f << L"  \"dark\":" << (snap.dark ? L"true" : L"false") << L",\n";
    f << L"  \"path\":\"";
    std::wstring escaped;
    pulse::json::Escape(snap.active_path, escaped);
    f << escaped << L"\",\n";
    f << L"  \"sidebarCollapsed\":" << snap.sidebar_collapsed << L",\n";
    f << L"  \"sidebarHidden\":" << snap.sidebar_hidden << L",\n";
    f << L"  \"sidebarOrder\":\"";
    for (size_t i = 0; i < snap.sidebar_order.size(); ++i) {
        if (i != 0) f << L",";
        f << snap.sidebar_order[i];
    }
    f << L"\",\n";
    f << L"  \"quickAccessHidden\":" << snap.quick_access_hidden << L",\n";
    f << L"  \"starredExpanded\":" << (snap.starred_expanded ? L"true" : L"false") << L",\n";
    f << L"  \"detailsPanel\":" << (snap.details_panel ? 1 : 0) << L",\n";
    f << L"  \"detailsPanelWidth\":" << std::clamp(snap.details_panel_width,
        static_cast<int>(ui::kDetailsMinWidthDip),
        static_cast<int>(ui::kPanelWidthMaxDip))
      << L",\n";
    f << L"  \"detailsPreviewOnly\":" << (snap.details_preview_only ? L"true" : L"false") << L",\n";
    f << L"  \"detailsPreview\":" << (snap.details_preview ? L"true" : L"false") << L",\n";
    f << L"  \"tray\":" << trayJson << L",\n";
    f << L"  \"undo\":" << (snap.undo_json.empty() ? L"[]" : snap.undo_json) << L",\n";
    f << L"  \"activeTab\":" << snap.active_layout_tab << L",\n";
    f << L"  \"tabGroups\":" << TabGroupsToJson(snap.tab_groups) << L",\n";
    f << L"  \"layoutTabs\":" << LayoutTabsToJson(snap.layout_tabs);
    if (!snap.windows.empty()) {
        f << L",\n  \"focusedWindow\":"
          << std::clamp(snap.focused_window, 0, static_cast<int>(snap.windows.size()) - 1);
        f << L",\n  \"windows\":[\n";
        for (size_t i = 0; i < snap.windows.size(); ++i)
            f << (i ? L",\n" : L"") << WindowToJson(snap.windows[i]);
        f << L"\n  ]";
    }
    if (!snap.closed_windows.empty()) {
        // M7f: the newest closed windows, oldest first; each is a windows[]
        // element plus its "closedName".
        const size_t count = snap.closed_windows.size();
        const size_t first = count > kSessionClosedWindows ? count - kSessionClosedWindows : 0;
        f << L",\n  \"closedWindows\":[\n";
        for (size_t i = first; i < count; ++i) {
            std::wstring element = WindowToJson(snap.closed_windows[i].window);
            std::wstring name;
            pulse::json::Escape(snap.closed_windows[i].name, name);
            element.insert(element.size() - 1, L",\n     \"closedName\":\"" + name + L"\"");
            f << (i > first ? L",\n" : L"") << element;
        }
        f << L"\n  ]";
    }
    f << L"\n}\n";
    return f.str();
}

bool WriteSessionJson(const std::wstring& json) {
    const std::wstring dir = GetPulseDataDir();
    if (dir.empty()) return false;
    return WriteUtf8FileAtomic(dir + L"\\session.json", json);
}

bool SaveSession(const SessionSnapshot& snap) {
    return WriteSessionJson(SessionToJson(snap));
}

bool LoadSession(SessionSnapshot& snap) {
    std::wstring dir = GetPulseDataDir();
    if (dir.empty()) return false;
    std::wstring json;
    if (!ReadUtf8File(dir + L"\\session.json", json) || json.empty()) return false;
    return ParseSessionJson(json, snap);
}

// One windows[] or closedWindows[] element (version 8).
static WindowSessionSnapshot ParseWindowJson(const std::wstring& wj) {
    WindowSessionSnapshot w;
    w.window_rect = {pulse::json::ExtractInt(wj, L"left"), pulse::json::ExtractInt(wj, L"top"),
                     pulse::json::ExtractInt(wj, L"right"), pulse::json::ExtractInt(wj, L"bottom")};
    w.maximized = pulse::json::ExtractBool(wj, L"maximized");
    WindowLayoutSnapshot& l = w.layout;
    l.sidebar_collapsed = pulse::json::ExtractInt(wj, L"sidebarCollapsed");
    l.sidebar_hidden = pulse::json::ExtractInt(wj, L"sidebarHidden");
    l.sidebar_order = ParseSidebarOrder(pulse::json::ExtractString(wj, L"sidebarOrder"));
    l.quick_access_hidden = pulse::json::ExtractInt(wj, L"quickAccessHidden");
    l.starred_expanded = pulse::json::ExtractBool(wj, L"starredExpanded", true);
    l.details_panel = pulse::json::ExtractInt(wj, L"detailsPanel") != 0;
    l.details_panel_width = pulse::json::ExtractInt(wj, L"detailsPanelWidth");
    if (l.details_panel_width != ClampDetailsPanelWidth(l.details_panel_width))
        l.details_panel_width = 340;
    l.details_preview_only = pulse::json::ExtractBool(wj, L"detailsPreviewOnly", false);
    l.details_preview = pulse::json::ExtractBool(wj, L"detailsPreview", true);
    w.active_layout_tab = pulse::json::ExtractInt(wj, L"activeTab");
    w.identity.name = NormalizeWindowName(pulse::json::ExtractString(wj, L"windowName"));
    w.identity.color = pulse::json::ExtractInt(wj, L"windowColor", -1);
    if (!ValidWindowColor(w.identity.color)) w.identity.color = -1;
    std::wstring body;
    if (ExtractJsonArray(wj, L"tabGroups", body)) ParseTabGroups(L"[" + body + L"]", w.tab_groups);
    if (ExtractJsonArray(wj, L"layoutTabs", body)) ParseLayoutTabs(L"[" + body + L"]", w.layout_tabs);
    return w;
}

bool ParseSessionJson(const std::wstring& json, SessionSnapshot& snap) {
    if (!pulse::json::ValidConfigObject(json)) return false;
    snap.window_rect.left = pulse::json::ExtractInt(json, L"left");
    snap.window_rect.top = pulse::json::ExtractInt(json, L"top");
    snap.window_rect.right = pulse::json::ExtractInt(json, L"right");
    snap.window_rect.bottom = pulse::json::ExtractInt(json, L"bottom");
    snap.maximized = pulse::json::ExtractBool(json, L"maximized");
    snap.dark = pulse::json::ExtractBool(json, L"dark");
    snap.active_path = pulse::json::ExtractString(json, L"path");
    snap.sidebar_collapsed = pulse::json::ExtractInt(json, L"sidebarCollapsed");
    snap.sidebar_hidden = pulse::json::ExtractInt(json, L"sidebarHidden");
    snap.sidebar_order = ParseSidebarOrder(pulse::json::ExtractString(json, L"sidebarOrder"));
    // Bit positions changed in version 7 (the reserved starred bit is gone), so
    // anything older restarts with every built-in link visible.
    snap.quick_access_hidden = pulse::json::ExtractInt(json, L"version") >= kQuickAccessBitsVersion
        ? pulse::json::ExtractInt(json, L"quickAccessHidden") : 0;
    snap.starred_expanded = json.find(L"\"starredExpanded\"") == std::wstring::npos
        ? true : pulse::json::ExtractBool(json, L"starredExpanded");
    snap.details_panel = pulse::json::ExtractInt(json, L"detailsPanel") != 0;
    snap.details_preview_only = pulse::json::ExtractBool(json, L"detailsPreviewOnly", false);
    // Missing in sessions written before the switch existed: keep the old
    // always-on behaviour.
    snap.details_preview = json.find(L"\"detailsPreview\"") == std::wstring::npos
        ? true : pulse::json::ExtractBool(json, L"detailsPreview", true);
    snap.details_panel_width = pulse::json::ExtractInt(json, L"detailsPanelWidth");
    // The stored value is the user's intent; the window caps it while drawing.
    if (snap.details_panel_width < static_cast<int>(ui::kDetailsMinWidthDip) ||
        snap.details_panel_width > static_cast<int>(ui::kPanelWidthMaxDip))
        snap.details_panel_width = 340;
    const std::array<int, 3> columnEdges{
        pulse::json::ExtractInt(json, L"detailsColumn0"),
        pulse::json::ExtractInt(json, L"detailsColumn1"),
        pulse::json::ExtractInt(json, L"detailsColumn2") };
    if (columnEdges[0] > 0 && columnEdges[0] < columnEdges[1] &&
        columnEdges[1] < columnEdges[2] && columnEdges[2] < 10000) {
        for (size_t i = 0; i < columnEdges.size(); ++i)
            snap.details_column_dividers[i] =
                static_cast<float>(columnEdges[i]) / 10000.0f;
    }

    // Both hold nested arrays (each tray batch has its own "items" list).
    if (const std::wstring tray = pulse::json::ExtractArray(json, L"tray"); !tray.empty())
        snap.tray.FromJson(tray);
    if (std::wstring undo = pulse::json::ExtractArray(json, L"undo"); !undo.empty())
        snap.undo_json = std::move(undo);

    snap.active_layout_tab = pulse::json::ExtractInt(json, L"activeTab");
    snap.tab_groups.clear();
    std::wstring groupsBody;
    if (ExtractJsonArray(json, L"tabGroups", groupsBody)) {
        std::vector<GroupSessionSnapshot> parsed;
        if (ParseTabGroups(L"[" + groupsBody + L"]", parsed))
            snap.tab_groups = std::move(parsed);
    }
    snap.layout_tabs.clear();
    std::wstring layoutBody;
    if (ExtractJsonArray(json, L"layoutTabs", layoutBody)) {
        std::vector<LayoutTabSnapshot> parsed;
        if (ParseLayoutTabs(L"[" + layoutBody + L"]", parsed))
            snap.layout_tabs = std::move(parsed);
    }

    // Version 8: windows[] (written after every single-window field, so the
    // lookups above found the top-level keys). Each element uses the same
    // key names; elements that do not parse are skipped.
    snap.windows.clear();
    std::wstring windowsBody;
    std::vector<std::wstring> windowObjs;
    if (ExtractJsonArray(json, L"windows", windowsBody) &&
        SplitTopLevelObjects(windowsBody, windowObjs)) {
        for (const std::wstring& wj : windowObjs) snap.windows.push_back(ParseWindowJson(wj));
    }
    if (snap.windows.empty()) {
        // Version 7 and older (or an unreadable windows[]): the single window.
        WindowSessionSnapshot w;
        w.window_rect = snap.window_rect;
        w.maximized = snap.maximized;
        w.layout = {snap.sidebar_collapsed, snap.sidebar_hidden, snap.sidebar_order,
                    snap.quick_access_hidden, snap.starred_expanded, snap.details_panel,
                    snap.details_panel_width, snap.details_preview_only, snap.details_preview};
        w.layout_tabs = snap.layout_tabs;
        w.tab_groups = snap.tab_groups;
        w.active_layout_tab = snap.active_layout_tab;
        snap.windows.push_back(std::move(w));
        snap.focused_window = 0;
    } else {
        snap.focused_window = std::clamp(pulse::json::ExtractInt(json, L"focusedWindow"), 0,
                                         static_cast<int>(snap.windows.size()) - 1);
    }

    // M7f: closed windows, oldest first; one without tabs has nothing to bring back.
    snap.closed_windows.clear();
    std::wstring closedBody;
    std::vector<std::wstring> closedObjs;
    if (ExtractJsonArray(json, L"closedWindows", closedBody) && SplitTopLevelObjects(closedBody, closedObjs)) {
        for (const std::wstring& cj : closedObjs) {
            ClosedWindowSessionSnapshot closed;
            closed.window = ParseWindowJson(cj);
            closed.name = pulse::json::ExtractString(cj, L"closedName");
            if (!closed.window.layout_tabs.empty()) snap.closed_windows.push_back(std::move(closed));
        }
        if (snap.closed_windows.size() > kSessionClosedWindows)
            snap.closed_windows.erase(snap.closed_windows.begin(),
                                      snap.closed_windows.end() - static_cast<ptrdiff_t>(kSessionClosedWindows));
    }
    return true;
}

} // namespace pulse::app
