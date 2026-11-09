#include "workspace_sidebar.h"
#include "app_navigation.h"
#include "quick_access.h"
#include "../ui/ui_renderer.h"

namespace pulse {
const ui::SidebarItem* WorkspacePaneRow(const ui::WindowViewModel& vm, const ui::HitTestResult& hit) {
    if (hit.region != ui::HitTestResult::SidebarItem || hit.sidebar_item < 0) return nullptr;
    for (const auto& group : vm.sidebar) {
        if (group.id != hit.sidebar_section || hit.sidebar_item >= static_cast<int>(group.items.size())) continue;
        const auto& row = group.items[static_cast<size_t>(hit.sidebar_item)];
        return row.workspace_parent_index >= 0 && row.workspace_child_number > 0 ? &row : nullptr;
    }
    return nullptr;
}

void MoveWorkspacePane(AppState& s, int workspace, int from, int to) {
    if (workspace < 0 || workspace >= static_cast<int>(s.places.workspaces.size())) return;
    auto& saved = s.places.workspaces[static_cast<size_t>(workspace)];
    const int count = static_cast<int>(saved.pane_paths.size());
    if (from < 0 || to < 0 || from >= count || to >= count || from == to) return;
    auto panes = LiveLayout(s).VisiblePanes();
    const bool move_live = s.places.active_workspace == workspace &&
        CollectPanePaths(s) == saved.pane_paths && static_cast<int>(panes.size()) == count;
    const auto move = [from, to](auto& values) {
        auto value = std::move(values[static_cast<size_t>(from)]);
        values.erase(values.begin() + from);
        values.insert(values.begin() + to, std::move(value));
    };
    saved.pane_views.resize(saved.pane_paths.size(), ui::ViewMode::Details);
    move(saved.pane_paths);
    move(saved.pane_views);
    if (move_live) {
        move(panes);
        size_t next = 0;
        const auto assign = [&](auto&& self, app::SplitContainer* node) -> void {
            if (!node) return;
            if (node->is_leaf) { if (node->pane) node->pane = panes[next++]; return; }
            self(self, node->first.get());
            self(self, node->second.get());
        };
        assign(assign, LiveLayout(s).root.get());
    }
    s.places.Save();
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

void ActivateWorkspacePane(AppState& s, int workspace, int pane) {
    if (workspace < 0 || workspace >= static_cast<int>(s.places.workspaces.size()) || pane < 0) return;
    const auto& saved = s.places.workspaces[static_cast<size_t>(workspace)];
    const size_t count = saved.pane_paths.empty() ? 1 : saved.pane_paths.size();
    if (static_cast<size_t>(pane) >= count) return;
    auto visible = LiveLayout(s).VisiblePanes();
    if (s.places.active_workspace != workspace || static_cast<size_t>(pane) >= visible.size()) {
        OpenWorkspace(s, workspace);
        visible = LiveLayout(s).VisiblePanes();
    }
    if (static_cast<size_t>(pane) < visible.size()) FocusPane(s, visible[static_cast<size_t>(pane)]);
}

int WorkspaceIndexFromPath(const std::wstring& path) {
    std::wstring kind, rest;
    if (!app::ParsePulsePath(path, &kind, &rest) || kind != L"workspace" || rest.empty()) return -1;
    wchar_t* end = nullptr;
    const long value = wcstol(rest.c_str(), &end, 10);
    return end && *end == 0 && value >= 0 && value <= INT_MAX ? static_cast<int>(value) : -1;
}
void FillWorkspaceSidebarState(AppState& s, ui::WindowViewModel& vm) {
    const auto paths = CollectPanePaths(s);
    const int layout = static_cast<int>(WorkspaceLayoutOf(s));
    for (auto& group : vm.sidebar) for (auto& item : group.items) {
        item.editing = !s.workspaceRenameRoot.empty() && s.places.FindWorkspace(s.workspaceRenameRoot) == item.workspace_index;
        if (!item.workspace_active || item.workspace_index < 0 ||
            item.workspace_index >= static_cast<int>(s.places.workspaces.size())) continue;
        const auto& saved = s.places.workspaces[static_cast<size_t>(item.workspace_index)];
        const auto expected = saved.pane_paths.empty() ? std::vector<std::wstring>{saved.root} : saved.pane_paths;
        // Browsing folders and changing view mode do not change the pane layout.
        item.workspace_dirty = layout != saved.layout || paths.size() != expected.size();
    }
}
bool UpdateWorkspaceDrag(AppState& s, int y) {
    if (s.workspacePaneDragParent >= 0) {
        const auto vm = BuildVm(s, false);
        s.pinGapVisible = false;
        s.pinDragToIndex = -1;
        for (const auto& group : vm.sidebar) for (int i = 0; i < static_cast<int>(group.items.size()); ++i) {
            const auto& row = group.items[static_cast<size_t>(i)];
            if (row.workspace_parent_index != s.workspacePaneDragParent || row.workspace_child_number <= 0) continue;
            D2D1_RECT_F r{};
            if (!s.renderer.SidebarRowRect(vm, static_cast<float>(s.compositor.Width()),
                static_cast<float>(s.compositor.Height()), group.id, i, &r)) continue;
            const bool above = y < (r.top + r.bottom) * 0.5f;
            s.pinDragToIndex = row.workspace_child_number - (above ? 1 : 0);
            s.pinGapLineY = above ? r.top : r.bottom;
            s.pinGapVisible = true;
            if (above) return true;
        }
        return true;
    }
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
