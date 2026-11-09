#include "pane_layout_controls.h"
#include "app_internal.h"
#include <algorithm>
#include <shlwapi.h>
#include "layout_pane_selection.h"

namespace pulse {
namespace { app::Pane* drag_source_pane = nullptr; }
void SetPaneLayoutDragSource(app::Pane* pane) { drag_source_pane = pane; }
float PaneLayoutProgress(float progress) {
    if (progress <= 0) return 0;
    if (progress >= 1) return 1;
    const auto curve = [](float t, float a, float b) {
        const float u = 1.0f - t;
        return 3*u*u*t*a + 3*u*t*t*b + t*t*t;
    };
    float lo = 0, hi = 1;
    for (int i = 0; i < 18; ++i) {
        const float mid = (lo + hi) * .5f;
        if (curve(mid, .2f, .2f) < progress) lo = mid;
        else hi = mid;
    }
    return curve((lo + hi) * .5f, .8f, 1.0f);
}
app::Pane* ZoomedPane(const app::LayoutTab& layout) {
    if (layout.zoomed_pane_index < 0 ||
        static_cast<size_t>(layout.zoomed_pane_index) >= layout.panes.size()) return nullptr;
    const auto visible = layout.VisiblePanes();
    if (visible.size() < 2) return nullptr;
    app::Pane* pane = layout.panes[static_cast<size_t>(layout.zoomed_pane_index)].get();
    return std::find(visible.begin(), visible.end(), pane) != visible.end() ? pane : nullptr;
}

bool RestorePaneZoom(AppState& state) {
    auto* layout = state.window_tabs.Active();
    if (!layout || layout->zoomed_pane_index < 0) return false;
    DismissPaneLayoutControls(state);
    layout->zoomed_pane_index = -1;
    state.hoverPaneIndex = -1;
    InvalidateRect(state.hwnd, nullptr, FALSE);
    return true;
}

bool SetPaneZoom(AppState& state, app::Pane* pane) {
    if (!pane) return RestorePaneZoom(state);
    auto* layout = state.window_tabs.Active();
    if (!layout) return false;
    const auto visible = layout->VisiblePanes();
    if (visible.size() < 2 || std::find(visible.begin(), visible.end(), pane) == visible.end()) return false;
    const auto owner = std::find_if(layout->panes.begin(), layout->panes.end(),
        [pane](const auto& item) { return item.get() == pane; });
    if (owner == layout->panes.end()) return false;
    const int index = static_cast<int>(owner - layout->panes.begin());
    if (layout->zoomed_pane_index == index) return false;
    DismissPaneLayoutControls(state);
    FocusPane(state, pane);
    layout->zoomed_pane_index = index;
    state.hoverPaneIndex = -1;
    InvalidateRect(state.hwnd, nullptr, FALSE);
    return true;
}

bool TogglePaneZoom(AppState& state, app::Pane* pane) {
    auto* layout = state.window_tabs.Active();
    if (!layout) return false;
    if (ZoomedPane(*layout)) return RestorePaneZoom(state);
    return SetPaneZoom(state, pane ? pane : state.pane);
}

bool TogglePaneZoomAtSlot(AppState& state, int slot_index) {
    if (slot_index < 0) return false;
    return TogglePaneZoom(state, PaneAtSlot(state, slot_index));
}

namespace {
constexpr UINT_PTR kPaneLayoutHoverTimer = 0x504c;
void CALLBACK RepaintPaneLayout(HWND hwnd, UINT, UINT_PTR, DWORD) {
    InvalidateRect(hwnd, nullptr, FALSE);
}
bool IsLayoutRegion(ui::HitTestResult::Region region) {
    using H = ui::HitTestResult;
    return region == H::PaneLayoutPreset || region == H::PaneLayoutZoom ||
        region == H::PaneLayoutRestore || region == H::PaneLayoutCell ||
        region == H::PaneLayoutTray || region == H::PaneLayoutCompare || region == H::PaneLayoutPeek;
}
app::Pane* PaneAtTreeIndex(AppState& state, int index) {
    const auto* layout = state.window_tabs.Active();
    if (!layout) return nullptr;
    const auto panes = layout->VisiblePanes();
    return index >= 0 && static_cast<size_t>(index) < panes.size() ? panes[static_cast<size_t>(index)] : nullptr;
}
}

bool DismissPaneLayoutControls(AppState& state) {
    auto* layout = state.window_tabs.Active();
    if (!layout) return false;
    const bool changed = layout->layout_tray_open || layout->layout_peek_index >= 0 || layout->layout_preview_preset >= 0;
    layout->layout_tray_open = false;
    layout->layout_peek_index = layout->layout_preview_preset = -1;
    layout->layout_hover_region = 0;
    layout->layout_hover_index = -1;
    layout->layout_hover_since = layout->layout_leave_at = 0;
    KillTimer(state.hwnd, kPaneLayoutHoverTimer);
    if (changed) InvalidateRect(state.hwnd, nullptr, FALSE);
    return changed;
}

void HoverPaneLayoutControls(AppState& state, const ui::HitTestResult& hit, bool dragging) {
    auto* layout = state.window_tabs.Active();
    if (!layout) return;
    const uint64_t now = GetTickCount64();
    if (!IsLayoutRegion(hit.region)) {
        if (!layout->layout_leave_at && (layout->layout_tray_open || layout->layout_peek_index >= 0 ||
                                        layout->layout_preview_preset >= 0)) layout->layout_leave_at = now + 220;
        layout->layout_hover_since = 0;
        layout->layout_hover_region = 0;
        if (layout->layout_leave_at) SetTimer(state.hwnd, kPaneLayoutHoverTimer, 16, RepaintPaneLayout);
        return;
    }
    layout->layout_leave_at = 0;
    if (layout->layout_hover_region != static_cast<int>(hit.region) || layout->layout_hover_index != hit.index ||
        layout->layout_hover_drag != dragging) {
        layout->layout_hover_region = static_cast<int>(hit.region);
        layout->layout_hover_index = hit.index;
        layout->layout_hover_drag = dragging;
        layout->layout_hover_since = now;
        if (hit.region != ui::HitTestResult::PaneLayoutPeek) layout->layout_peek_index = -1;
        if (hit.region != ui::HitTestResult::PaneLayoutPreset) layout->layout_preview_preset = -1;
    }
    SetTimer(state.hwnd, kPaneLayoutHoverTimer, 16, RepaintPaneLayout);
}

void FillPaneLayoutControls(AppState& state, ui::WindowViewModel& vm) {
    auto* layout = state.window_tabs.Active();
    auto& out = vm.layout_controls;
    out = {};
    if (!layout) { out.visible = false; return; }
    out.visible = !IsSettingsTab(ActiveTab(state));
    const uint64_t now = GetTickCount64();
    if (layout->layout_leave_at && now >= layout->layout_leave_at) DismissPaneLayoutControls(state);
    using H = ui::HitTestResult;
    bool pending = layout->layout_leave_at != 0;
    if (layout->layout_hover_since) {
        const auto region = static_cast<H::Region>(layout->layout_hover_region);
        const uint64_t delay = region == H::PaneLayoutPreset ? 120 : region == H::PaneLayoutTray ? 140 :
            layout->layout_hover_drag ? 300 : 200;
        if (now - layout->layout_hover_since >= delay) {
            if (region == H::PaneLayoutPreset) layout->layout_preview_preset = layout->layout_hover_index;
            if (region == H::PaneLayoutTray) layout->layout_tray_open = true;
            if (region == H::PaneLayoutCell && ZoomedPane(*layout) &&
                PaneAtTreeIndex(state, layout->layout_hover_index) != ZoomedPane(*layout))
                layout->layout_peek_index = layout->layout_hover_index;
            layout->layout_hover_since = 0;
        } else pending = true;
    }
    if (!pending) KillTimer(state.hwnd, kPaneLayoutHoverTimer);
    out.preset = static_cast<int>(layout->layout);
    out.compare = layout->compare;
    out.preview_preset = layout->layout_preview_preset;
    out.peek_index = layout->layout_peek_index;
    out.tray_open = layout->layout_tray_open;
    const auto panes = layout->VisiblePanes();
    const auto* zoomed = ZoomedPane(*layout);
    for (size_t i = 0; i < panes.size(); ++i) {
        const auto* pane = panes[i];
        if (pane == zoomed) out.zoom_index = static_cast<int>(i);
        if (pane == state.pane) out.focus_index = static_cast<int>(i);
        if (pane == state.targetPane) out.target_index = static_cast<int>(i);
        const auto* tab = pane ? pane->ActiveTab() : nullptr;
        const std::wstring path = tab ? tab->current_path : std::wstring();
        out.pane_paths.push_back(path);
        out.pane_names.push_back(app::TabTitle(path));
        std::vector<std::wstring> preview, sizes;
        std::vector<bool> directories;
        const size_t count = tab && tab->snapshot ? tab->EntryCount() : 0;
        out.pane_counts.push_back(count);
        for (size_t row = 0; row < std::min<size_t>(6, count); ++row) {
            const auto entry = tab->EntryAt(row);
            preview.push_back(entry.name);
            directories.push_back(entry.is_dir);
            wchar_t size[64]{};
            if (!entry.is_dir) StrFormatByteSizeW(static_cast<LONGLONG>(entry.size), size, ARRAYSIZE(size));
            sizes.emplace_back(size);
        }
        out.pane_preview_sizes.push_back(std::move(sizes));
        out.pane_preview_dirs.push_back(std::move(directories));
        out.pane_previews.push_back(std::move(preview));
    }
    if (out.preview_preset >= 0 && out.preview_preset <= 4) {
        const size_t count = app::LayoutPresetCount(static_cast<app::LayoutPreset>(out.preview_preset));
        const auto selected = app::SelectLayoutPanes(layout->panes, nullptr, count);
        for (size_t i = 0; i < selected.size(); ++i) {
            const auto* tab = selected[i] ? selected[i]->ActiveTab() : nullptr;
            out.preview_names.push_back(app::TabTitle(tab ? tab->current_path : std::wstring()));
            if (selected[i] == state.targetPane) out.preview_target_index = static_cast<int>(i);
        }
        const auto* focused = state.pane ? state.pane->ActiveTab() : nullptr;
        while (out.preview_names.size() < count)
            out.preview_names.push_back(app::TabTitle(focused ? focused->current_path : L"C:\\"));
    }
}

bool HandlePaneLayoutClick(AppState& state, const ui::HitTestResult& hit) {
    using H = ui::HitTestResult;
    auto* layout = state.window_tabs.Active();
    if (!layout) return false;
    switch (hit.region) {
    case H::PaneExpand: TogglePaneZoomAtSlot(state, hit.pane_index); return true;
    case H::PaneLayoutPreset:
        if (hit.index >= 0 && hit.index <= 4) { DismissPaneLayoutControls(state); ApplyLayoutPreset(state, static_cast<app::LayoutPreset>(hit.index)); }
        return true;
    case H::PaneLayoutZoom: TogglePaneZoom(state); return true;
    case H::PaneLayoutRestore: RestorePaneZoom(state); return true;
    case H::PaneLayoutCompare:
        SetFolderCompare(state, !layout->compare); DismissPaneLayoutControls(state); return true;
    case H::PaneLayoutTray:
        layout->layout_tray_open = !layout->layout_tray_open;
        layout->layout_hover_since = 0;
        InvalidateRect(state.hwnd, nullptr, FALSE); return true;
    case H::PaneLayoutCell:
        if (auto* pane = PaneAtTreeIndex(state, hit.index)) {
            if (ZoomedPane(*layout)) SetPaneZoom(state, pane);
            else FocusPane(state, pane);
            DismissPaneLayoutControls(state);
        }
        return true;
    case H::PaneLayoutPeek: return true;
    default: DismissPaneLayoutControls(state); return false;
    }
}

std::wstring PaneLayoutDropPath(AppState& state, const ui::HitTestResult& hit) {
    if (hit.region != ui::HitTestResult::PaneLayoutCell && hit.region != ui::HitTestResult::PaneLayoutPeek) return {};
    const auto* pane = PaneAtTreeIndex(state, hit.index);
    if (pane && pane == drag_source_pane) return {};
    const auto* tab = pane ? pane->ActiveTab() : nullptr;
    return tab ? tab->current_path : std::wstring();
}
}
