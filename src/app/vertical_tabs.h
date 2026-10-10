#pragma once
#include "app_runtime.h"

// Vertical tabs + collapsible sidebar. With tabs in the sidebar the window
// tabs become the first sidebar section (rows reuse the sidebar row painter),
// the address row moves up into the title bar, and Ctrl+B folds the sidebar
// into its icon rail. Settings keeps the top strip so tabs stay reachable.
namespace pulse {
// Not a SidebarSectionId: the section is synthesized per frame and never
// enters the persisted order. Its fold bit lives in sidebarCollapsedMask.
inline constexpr int kVerticalTabsSectionId = 30;

bool IsVerticalTabPath(const std::wstring& path, size_t* index = nullptr);
// Pushes the renderer flags and prepends the tabs section. Called by BuildVm.
void ApplyVerticalTabs(AppState& s, ui::WindowViewModel& vm);
void ToggleSidebarCollapsed(AppState& s);
// Mouse handlers; each returns true when it consumed the hit.
bool HandleVerticalTabPress(AppState& s, const ui::HitTestResult& hit);
bool HandleVerticalTabContextMenu(AppState& s, const ui::HitTestResult& hit, POINT screen);
// Tab-row reorder rides the quick-access pin drag state (pinDrag*): the press
// arms it, these measure the insertion slot and commit the move.
bool UpdateVerticalTabDrag(AppState& s, int my);
void CommitVerticalTabDrag(AppState& s, const std::wstring& path, size_t insert_at);
// Hover peek on the collapsed rail: mouse moves arm it, the UI tick opens it
// after a short dwell, leaving the overlay (or the window) closes it.
void UpdateSidebarPeek(AppState& s, int x, int y);
bool TickSidebarPeek(AppState& s, ULONGLONG now);
void CloseSidebarPeek(AppState& s);
// Middle click closes a tab (title strip or sidebar row).
bool HandleTabMiddleClick(AppState& s, int x, int y);
// Double-click close (Settings > Startup and close): a press on a tab arms it,
// the double-click on that same tab closes it. Returns true when it closed one.
void ArmTabDoubleClick(AppState& s, size_t index);
bool HandleTabDoubleClick(AppState& s, const ui::HitTestResult& hit);
// Middle click on a folder (list row, sidebar place, breadcrumb) opens it in a
// background tab; Ctrl+middle click opens it in the foreground.
bool HandleFolderMiddleClick(AppState& s, int x, int y);
}
