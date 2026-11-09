#pragma once
#include "app_model.h"

namespace pulse {
struct AppState;
inline constexpr uint64_t kPaneLayoutTransitionMs = 280;
float PaneLayoutProgress(float progress);
void SetPaneLayoutDragSource(app::Pane* pane);
app::Pane* ZoomedPane(const app::LayoutTab& layout);
bool SetPaneZoom(AppState& state, app::Pane* pane);
bool TogglePaneZoom(AppState& state, app::Pane* pane = nullptr);
bool RestorePaneZoom(AppState& state);
// The index comes from HitTestResult::pane_index, not the owning pane vector.
bool TogglePaneZoomAtSlot(AppState& state, int slot_index);
void FillPaneLayoutControls(AppState& state, ui::WindowViewModel& vm);
bool HandlePaneLayoutClick(AppState& state, const ui::HitTestResult& hit);
void HoverPaneLayoutControls(AppState& state, const ui::HitTestResult& hit, bool dragging = false);
bool DismissPaneLayoutControls(AppState& state);
std::wstring PaneLayoutDropPath(AppState& state, const ui::HitTestResult& hit);
}
