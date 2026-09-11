// session.cpp
#include "session.h"
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

std::wstring FormatScaled3(const std::array<float, 3>& edges) {
    return std::to_wstring(static_cast<int>(std::lround(edges[0] * 10000.0f))) + L","
         + std::to_wstring(static_cast<int>(std::lround(edges[1] * 10000.0f))) + L","
         + std::to_wstring(static_cast<int>(std::lround(edges[2] * 10000.0f)));
}

std::array<float, 3> ParseScaled3(const std::wstring& value) {
    std::array<int, 3> edges{};
    std::array<float, 3> ratios{};
    if (swscanf_s(value.c_str(), L"%d,%d,%d",
                  &edges[0], &edges[1], &edges[2]) == 3 &&
        edges[0] > 0 && edges[0] < edges[1] &&
        edges[1] < edges[2] && edges[2] < 10000) {
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
        edges[0] > 0 && edges[0] < edges[1] &&
        edges[1] < edges[2] && edges[2] < edges[3] &&
        edges[3] < 10000) {
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
            out += FormatScaled3(pane.columns);
            out += L"\",\"searchCols\":\"";
            out += FormatScaled4(pane.search_columns);
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
        out += L"}";
    }
    out += L"]";
    return out;
}

// Extract the contents of each top-level {...} object inside an array body
// (without the outer brackets). Handles nested objects/arrays and string
// escapes; returns false on unbalanced input.
static bool SplitTopLevelObjects(const std::wstring& body,
                                 std::vector<std::wstring>& out) {
    int depth = 0;
    bool inStr = false;
    size_t start = std::wstring::npos;
    for (size_t i = 0; i < body.size(); ++i) {
        wchar_t c = body[i];
        if (inStr) {
            if (c == L'\\') ++i;
            else if (c == L'"') inStr = false;
            continue;
        }
        if (c == L'"') inStr = true;
        else if (c == L'{' || c == L'[') {
            if (depth == 0 && c == L'{') start = i;
            ++depth;
        } else if (c == L'}' || c == L']') {
            --depth;
            if (depth < 0) return false;
            if (depth == 0 && start != std::wstring::npos && c == L'}') {
                out.push_back(body.substr(start, i - start + 1));
                start = std::wstring::npos;
            }
        }
    }
    return depth == 0 && !inStr;
}

static bool ExtractJsonArray(const std::wstring& json, const wchar_t* key,
                             std::wstring& body) {
    size_t pos = pulse::json::ValuePosition(json, key);
    if (pos == std::wstring::npos || pos >= json.size() || json[pos] != L'[')
        return false;
    int depth = 0;
    bool inStr = false;
    for (size_t i = pos; i < json.size(); ++i) {
        wchar_t c = json[i];
        if (inStr) {
            if (c == L'\\') ++i;
            else if (c == L'"') inStr = false;
            continue;
        }
        if (c == L'"') inStr = true;
        else if (c == L'[') ++depth;
        else if (c == L']') {
            if (--depth == 0) {
                body = json.substr(pos + 1, i - pos - 1);
                return true;
            }
        }
    }
    return false;
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

        std::wstring panesBody;
        if (ExtractJsonArray(tabJson, L"panes", panesBody)) {
            std::vector<std::wstring> paneObjs;
            if (!SplitTopLevelObjects(panesBody, paneObjs)) return false;
            for (const std::wstring& pj : paneObjs) {
                PaneFolderSnapshot pane;
                pane.path = pulse::json::ExtractString(pj, L"path");
                pane.view = ui::ParseViewMode(pulse::json::ExtractString(pj, L"view"));
                pane.columns = ParseScaled3(pulse::json::ExtractString(pj, L"cols"));
                pane.search_columns = ParseScaled4(
                    pulse::json::ExtractString(pj, L"searchCols"));
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

bool SaveSession(const SessionSnapshot& snap) {
    std::wstring dir = GetPulseDataDir();
    if (dir.empty()) return false;

    std::wstring trayJson;
    snap.tray.ToJson(trayJson);

    std::wostringstream f;
    f << L"{\n";
    f << L"  \"version\":6,\n";
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
    f << L"  \"starredExpanded\":" << (snap.starred_expanded ? L"true" : L"false") << L",\n";
    f << L"  \"detailsPanel\":" << (snap.details_panel ? 1 : 0) << L",\n";
    f << L"  \"detailsPanelWidth\":" << std::clamp(snap.details_panel_width, 300, 480)
      << L",\n";
    f << L"  \"detailsPreviewOnly\":" << (snap.details_preview_only ? L"true" : L"false") << L",\n";
    f << L"  \"tray\":" << trayJson << L",\n";
    f << L"  \"undo\":" << (snap.undo_json.empty() ? L"[]" : snap.undo_json) << L",\n";
    f << L"  \"activeTab\":" << snap.active_layout_tab << L",\n";
    f << L"  \"tabGroups\":" << TabGroupsToJson(snap.tab_groups) << L",\n";
    f << L"  \"layoutTabs\":" << LayoutTabsToJson(snap.layout_tabs) << L"\n";
    f << L"}\n";
    return WriteUtf8FileAtomic(dir + L"\\session.json", f.str());
}

bool LoadSession(SessionSnapshot& snap) {
    std::wstring dir = GetPulseDataDir();
    if (dir.empty()) return false;
    std::wstring json;
    if (!ReadUtf8File(dir + L"\\session.json", json) || json.empty()) return false;

    snap.window_rect.left = pulse::json::ExtractInt(json, L"left");
    snap.window_rect.top = pulse::json::ExtractInt(json, L"top");
    snap.window_rect.right = pulse::json::ExtractInt(json, L"right");
    snap.window_rect.bottom = pulse::json::ExtractInt(json, L"bottom");
    snap.maximized = pulse::json::ExtractBool(json, L"maximized");
    snap.dark = pulse::json::ExtractBool(json, L"dark");
    snap.active_path = pulse::json::ExtractString(json, L"path");
    snap.sidebar_collapsed = pulse::json::ExtractInt(json, L"sidebarCollapsed");
    snap.starred_expanded = json.find(L"\"starredExpanded\"") == std::wstring::npos
        ? true : pulse::json::ExtractBool(json, L"starredExpanded");
    snap.details_panel = pulse::json::ExtractInt(json, L"detailsPanel") != 0;
    snap.details_preview_only = pulse::json::ExtractBool(json, L"detailsPreviewOnly", false);
    snap.details_panel_width = pulse::json::ExtractInt(json, L"detailsPanelWidth");
    if (snap.details_panel_width < 300 || snap.details_panel_width > 480)
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

    size_t trayPos = json.find(L"\"tray\"");
    if (trayPos != std::wstring::npos) {
        size_t start = json.find(L'[', trayPos);
        size_t end = json.find(L']', start);
        if (start != std::wstring::npos && end != std::wstring::npos && end > start) {
            std::wstring trayJson = json.substr(start, end - start + 1);
            snap.tray.FromJson(trayJson);
        }
    }

    size_t undoPos = json.find(L"\"undo\"");
    if (undoPos != std::wstring::npos) {
        size_t start = json.find(L'[', undoPos);
        // Find the matching closing bracket (array may nest one level of "src":[...]).
        if (start != std::wstring::npos) {
            int depth = 0;
            size_t end = std::wstring::npos;
            bool inStr = false;
            for (size_t i = start; i < json.size(); ++i) {
                wchar_t c = json[i];
                if (inStr) {
                    if (c == L'\\') ++i;
                    else if (c == L'"') inStr = false;
                    continue;
                }
                if (c == L'"') inStr = true;
                else if (c == L'[') ++depth;
                else if (c == L']') { if (--depth == 0) { end = i; break; } }
            }
            if (end != std::wstring::npos)
                snap.undo_json = json.substr(start, end - start + 1);
        }
    }

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
    return true;
}

} // namespace pulse::app
