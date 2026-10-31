#include "workspace_sidebar.h"
#include "app_navigation.h"
#include "quick_access.h"
#include "../ui/ui_renderer.h"

namespace pulse {
int WorkspaceIndexFromPath(const std::wstring& path) {
    std::wstring kind, rest;
    if (!app::ParsePulsePath(path, &kind, &rest) || kind != L"workspace" || rest.empty()) return -1;
    wchar_t* end = nullptr;
    const long value = wcstol(rest.c_str(), &end, 10);
    return end && *end == 0 && value >= 0 && value <= INT_MAX ? static_cast<int>(value) : -1;
}
void FillWorkspaceSidebarState(AppState& s, ui::WindowViewModel& vm) {
    const auto paths = CollectPanePaths(s);
    const auto views = CollectPaneViews(s);
    const int layout = static_cast<int>(WorkspaceLayoutOf(s));
    for (auto& group : vm.sidebar) for (auto& item : group.items) {
        item.editing = !s.workspaceRenameRoot.empty() && s.places.FindWorkspace(s.workspaceRenameRoot) == item.workspace_index;
        if (!item.workspace_active || item.workspace_index < 0 ||
            item.workspace_index >= static_cast<int>(s.places.workspaces.size())) continue;
        const auto& saved = s.places.workspaces[static_cast<size_t>(item.workspace_index)];
        const auto expected = saved.pane_paths.empty() ? std::vector<std::wstring>{saved.root} : saved.pane_paths;
        bool same = layout == saved.layout && paths.size() == expected.size();
        for (size_t i = 0; same && i < paths.size(); ++i)
            same = _wcsicmp(fs::NormalizePath(paths[i]).c_str(), fs::NormalizePath(expected[i]).c_str()) == 0 &&
                (i >= saved.pane_views.size() || (i < views.size() && views[i] == saved.pane_views[i]));
        item.workspace_dirty = !same;
    }
}
bool UpdateWorkspaceDrag(AppState& s, int y) {
    if (WorkspaceIndexFromPath(s.pinDragPath) < 0) return false;
    const auto vm = BuildVm(s, false);
    const int section = static_cast<int>(app::SidebarSectionId::Workspaces);
    s.pinGapVisible = false;
    for (const auto& group : vm.sidebar) {
        if (group.id != section) continue;
        for (int i = 0; i < static_cast<int>(group.items.size()); ++i) {
            const auto& item = group.items[static_cast<size_t>(i)];
            if (item.workspace_index < 0) continue;
            D2D1_RECT_F r{};
            if (!s.renderer.SidebarRowRect(vm, static_cast<float>(s.compositor.Width()),
                static_cast<float>(s.compositor.Height()), section, i, &r)) continue;
            const bool above = y < (r.top + r.bottom) * 0.5f;
            s.pinDragToIndex = item.workspace_index + (above ? 0 : 1);
            s.pinGapLineY = above ? r.top : r.bottom;
            s.pinGapVisible = true;
            if (above) break;
        }
    }
    return true;
}
void CommitWorkspaceDrag(AppState& s, const std::wstring& path, int gap) {
    const int from = WorkspaceIndexFromPath(path);
    if (from < 0 || gap < 0 || s.places.workspaces.empty()) return;
    const int to = std::clamp(gap > from ? gap - 1 : gap, 0, static_cast<int>(s.places.workspaces.size()) - 1);
    MoveWorkspace(s, from, to);
}
}
