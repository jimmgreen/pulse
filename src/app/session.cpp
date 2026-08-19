// session.cpp
#include "session.h"
#include "../common/json_utils.h"
#include <commctrl.h>
#include <prsht.h>
#include <shlobj.h>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cmath>

namespace pulse::app {

std::wstring PaneTabsToJson(const std::vector<PaneSessionSnapshot>& panes) {
    std::wstring out = L"[";
    for (size_t i = 0; i < panes.size(); ++i) {
        if (i) out += L",";
        const PaneSessionSnapshot& pane = panes[i];
        out += L"{\"active\":";
        out += std::to_wstring(pane.active);
        out += L",\"groups\":[";
        for (size_t g = 0; g < pane.groups.size(); ++g) {
            if (g) out += L",";
            const GroupSessionSnapshot& grp = pane.groups[g];
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
        out += L"],\"tabs\":[";
        for (size_t t = 0; t < pane.tabs.size(); ++t) {
            if (t) out += L",";
            const TabSessionSnapshot& tab = pane.tabs[t];
            out += L"{\"path\":\"";
            std::wstring escaped;
            pulse::json::Escape(tab.path, escaped);
            out += escaped;
            out += L"\",\"pinned\":";
            out += tab.pinned ? L"true" : L"false";
            out += L",\"group\":" + std::to_wstring(tab.group);
            out += L",\"view\":\"";
            out += ui::ViewModeName(tab.view);
            out += L"\"}";
        }
        out += L"]}";
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

bool ParsePaneTabs(const std::wstring& array_json,
                   std::vector<PaneSessionSnapshot>& out) {
    out.clear();
    if (array_json.size() < 2 || array_json.front() != L'[' ||
        array_json.back() != L']') {
        return false;
    }
    std::vector<std::wstring> paneObjs;
    if (!SplitTopLevelObjects(array_json.substr(1, array_json.size() - 2), paneObjs))
        return false;
    for (const std::wstring& paneJson : paneObjs) {
        PaneSessionSnapshot pane;
        pane.active = pulse::json::ExtractInt(paneJson, L"active");

        // Nested arrays: locate "groups"/"tabs" keys, then bracket-match.
        auto extractArray = [&](const wchar_t* key, std::wstring& body) {
            size_t pos = pulse::json::ValuePosition(paneJson, key);
            if (pos == std::wstring::npos || pos >= paneJson.size() ||
                paneJson[pos] != L'[') {
                return false;
            }
            int depth = 0;
            bool inStr = false;
            for (size_t i = pos; i < paneJson.size(); ++i) {
                wchar_t c = paneJson[i];
                if (inStr) {
                    if (c == L'\\') ++i;
                    else if (c == L'"') inStr = false;
                    continue;
                }
                if (c == L'"') inStr = true;
                else if (c == L'[') ++depth;
                else if (c == L']') {
                    if (--depth == 0) {
                        body = paneJson.substr(pos + 1, i - pos - 1);
                        return true;
                    }
                }
            }
            return false;
        };

        std::wstring groupsBody;
        if (extractArray(L"groups", groupsBody)) {
            std::vector<std::wstring> groupObjs;
            if (!SplitTopLevelObjects(groupsBody, groupObjs)) return false;
            for (const std::wstring& gj : groupObjs) {
                GroupSessionSnapshot grp;
                grp.id = pulse::json::ExtractInt(gj, L"id");
                grp.name = pulse::json::ExtractString(gj, L"name");
                grp.color_rgb = static_cast<uint32_t>(std::max(
                    0, pulse::json::ExtractInt(gj, L"color")));
                grp.collapsed = pulse::json::ExtractBool(gj, L"collapsed");
                pane.groups.push_back(std::move(grp));
            }
        }

        std::wstring tabsBody;
        if (extractArray(L"tabs", tabsBody)) {
            std::vector<std::wstring> tabObjs;
            if (!SplitTopLevelObjects(tabsBody, tabObjs)) return false;
            for (const std::wstring& tj : tabObjs) {
                TabSessionSnapshot tab;
                tab.path = pulse::json::ExtractString(tj, L"path");
                tab.pinned = pulse::json::ExtractBool(tj, L"pinned");
                tab.group = pulse::json::ExtractInt(tj, L"group");
                tab.view = ui::ParseViewMode(
                    pulse::json::ExtractString(tj, L"view"));
                pane.tabs.push_back(std::move(tab));
            }
        }
        out.push_back(std::move(pane));
    }
    return true;
}

std::wstring GetPulseDataDir() {
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
    std::wstring tmp = dir + L"\\session.tmp";
    std::wstring final = dir + L"\\session.json";

    std::wstring trayJson;
    snap.tray.ToJson(trayJson);

    std::wofstream f(tmp, std::wofstream::out | std::wofstream::trunc);
    if (!f) return false;
    f << L"{\n";
    f << L"  \"version\":5,\n";
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
    f << L"  \"layout\":" << snap.layout << L",\n";
    f << L"  \"focused\":" << snap.focused_pane << L",\n";
    f << L"  \"target\":" << snap.target_pane << L",\n";
    f << L"  \"sidebarCollapsed\":" << snap.sidebar_collapsed << L",\n";
    f << L"  \"starredExpanded\":" << (snap.starred_expanded ? L"true" : L"false") << L",\n";
    f << L"  \"detailsPanel\":" << (snap.details_panel ? 1 : 0) << L",\n";
    f << L"  \"detailsPanelWidth\":" << std::clamp(snap.details_panel_width, 300, 480)
      << L",\n";
    f << L"  \"panes\":[";
    for (size_t i = 0; i < snap.pane_paths.size(); ++i) {
        if (i) f << L",";
        f << L"\"";
        escaped.clear();
        pulse::json::Escape(snap.pane_paths[i], escaped);
        f << escaped << L"\"";
    }
    f << L"],\n";
    f << L"  \"paneViews\":[";
    for (size_t i = 0; i < snap.pane_views.size(); ++i) {
        if (i) f << L",";
        f << L"\"" << ui::ViewModeName(snap.pane_views[i]) << L"\"";
    }
    f << L"],\n";
    f << L"  \"paneColumns\":[";
    for (size_t i = 0; i < snap.pane_paths.size(); ++i) {
        if (i) f << L",";
        const std::array<float, 3> edges = i < snap.pane_column_dividers.size()
            ? snap.pane_column_dividers[i] : std::array<float, 3>{};
        f << L"\""
          << static_cast<int>(std::lround(edges[0] * 10000.0f)) << L","
          << static_cast<int>(std::lround(edges[1] * 10000.0f)) << L","
          << static_cast<int>(std::lround(edges[2] * 10000.0f)) << L"\"";
    }
    f << L"],\n";
    f << L"  \"tray\":" << trayJson << L",\n";
    f << L"  \"undo\":" << (snap.undo_json.empty() ? L"[]" : snap.undo_json) << L",\n";
    f << L"  \"paneTabs\":" << PaneTabsToJson(snap.pane_tabs) << L"\n";
    f << L"}\n";
    f.close();
    if (!f) return false;

    // Atomic replace.
    if (!MoveFileExW(tmp.c_str(), final.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        return false;
    return true;
}

static std::wstring ReadFileUtf16(const std::wstring& path) {
    std::wifstream f(path, std::wifstream::binary);
    if (!f) return L"";
    std::wstringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

bool LoadSession(SessionSnapshot& snap) {
    std::wstring dir = GetPulseDataDir();
    if (dir.empty()) return false;
    std::wstring path = dir + L"\\session.json";
    std::wstring json = ReadFileUtf16(path);
    if (json.empty()) return false;

    snap.window_rect.left = pulse::json::ExtractInt(json, L"left");
    snap.window_rect.top = pulse::json::ExtractInt(json, L"top");
    snap.window_rect.right = pulse::json::ExtractInt(json, L"right");
    snap.window_rect.bottom = pulse::json::ExtractInt(json, L"bottom");
    snap.maximized = pulse::json::ExtractBool(json, L"maximized");
    snap.dark = pulse::json::ExtractBool(json, L"dark");
    snap.active_path = pulse::json::ExtractString(json, L"path");
    snap.layout = pulse::json::ExtractInt(json, L"layout");
    snap.focused_pane = pulse::json::ExtractInt(json, L"focused");
    snap.target_pane = pulse::json::ExtractInt(json, L"target");
    if (json.find(L"\"target\"") == std::wstring::npos) snap.target_pane = -1;
    snap.sidebar_collapsed = pulse::json::ExtractInt(json, L"sidebarCollapsed");
    snap.starred_expanded = json.find(L"\"starredExpanded\"") == std::wstring::npos
        ? true : pulse::json::ExtractBool(json, L"starredExpanded");
    snap.details_panel = pulse::json::ExtractInt(json, L"detailsPanel") != 0;
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
    snap.pane_paths = pulse::json::ExtractStringArray(json, L"panes");
    snap.pane_views.clear();
    for (const auto& value : pulse::json::ExtractStringArray(json, L"paneViews"))
        snap.pane_views.push_back(ui::ParseViewMode(value));
    if (snap.pane_paths.empty() && !snap.active_path.empty())
        snap.pane_paths.push_back(snap.active_path);
    while (snap.pane_views.size() < snap.pane_paths.size())
        snap.pane_views.push_back(ui::ViewMode::Details);
    snap.pane_column_dividers.clear();
    for (const auto& value : pulse::json::ExtractStringArray(json, L"paneColumns")) {
        std::array<int, 3> edges{};
        std::array<float, 3> ratios{};
        if (swscanf_s(value.c_str(), L"%d,%d,%d",
                      &edges[0], &edges[1], &edges[2]) == 3 &&
            edges[0] > 0 && edges[0] < edges[1] &&
            edges[1] < edges[2] && edges[2] < 10000) {
            for (size_t i = 0; i < ratios.size(); ++i)
                ratios[i] = static_cast<float>(edges[i]) / 10000.0f;
        }
        snap.pane_column_dividers.push_back(ratios);
    }
    if (snap.pane_column_dividers.empty() &&
        snap.details_column_dividers[0] > 0.0f) {
        snap.pane_column_dividers.assign(
            snap.pane_paths.size(), snap.details_column_dividers);
    }
    while (snap.pane_column_dividers.size() < snap.pane_paths.size())
        snap.pane_column_dividers.push_back({});

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

    snap.pane_tabs.clear();
    size_t paneTabsPos = json.find(L"\"paneTabs\"");
    if (paneTabsPos != std::wstring::npos) {
        size_t start = json.find(L'[', paneTabsPos);
        // The array nests two levels (pane -> groups/tabs -> objects), so
        // bracket-match with depth counting instead of a plain find(']').
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
            if (end != std::wstring::npos) {
                std::vector<PaneSessionSnapshot> parsed;
                if (ParsePaneTabs(json.substr(start, end - start + 1), parsed))
                    snap.pane_tabs = std::move(parsed);
            }
        }
    }
    return true;
}

} // namespace pulse::app
