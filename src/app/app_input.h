// app_input.h — Pointer, keyboard, marquee, and drag/drop input.
#pragma once
#include "app_runtime.h"
#include "app_hover.h"
#include "app_list_selection.h"

namespace pulse {
std::vector<ui::FluentMenuItem> BuildTitleTabOverflowItems(AppState& state);
void ShowTitleTabOverflow(AppState& state, POINT anchor);
bool HandleBrowserNavigation(AppState& s, LPARAM command);
bool DeferFileMouseActivation(AppState& s, POINT client);
bool HandleListCharacter(AppState& s, wchar_t ch);
void TickQuickPreviewSelection(AppState& s, ULONGLONG now);
LRESULT HandleMouseMove(AppState* s, HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
LRESULT HandleMouseLeave(AppState* s, HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
LRESULT HandleLButtonDown(AppState* s, HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
LRESULT HandleLButtonDblClk(AppState* s, HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
LRESULT HandleLButtonUp(AppState* s, HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
LRESULT HandleCaptureChanged(AppState* s, HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
LRESULT HandleRButtonDown(AppState* s, HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
LRESULT HandleRButtonUp(AppState* s, HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
LRESULT HandleMouseWheel(AppState* s, HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
LRESULT HandleKeyDown(AppState* s, HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
bool UpdateSplitterDrag(AppState& s, int mx, int my);
void ClearDropFeedback(AppState& s);
std::wstring BaseName(const std::wstring& path);
bool PointOnPaneHeader(const AppState& s, const ui::WindowViewModel& vm,
                              int pane_index, float x, float y);
std::wstring HeaderDropHint(const std::vector<std::wstring>& sources);
std::wstring ResolveHeaderDropFolder(const std::vector<std::wstring>& sources);
DWORD ResolveDropTarget(AppState& s, const std::vector<std::wstring>& sources,
                               POINT pt, DWORD key_state, DWORD allowed,
                               DWORD preferred_effect);
DWORD DropExecute(AppState& s, const std::vector<std::wstring>& sources,
                         POINT pt, DWORD key_state, DWORD allowed,
                         DWORD preferred_effect);
void StartDragOut(AppState& s);
float MaxScrollForActivePane(AppState& s, ui::PaneViewModel* out = nullptr);
void ClampScroll(AppState& s);
void EnsureRowVisible(AppState& s, app::Tab& tab, int index);
bool PointInList(const AppState& s, int mx, int my);
// 点空白取消选择: a click on blank chrome (toolbar gaps, status bar, sidebar
// blank, the pane title strip, the tab strip) clears the active pane's
// selection; the double click that maximizes from there puts it back.
// `maximize`: the spot maximizes on a double click (toolbar gaps, status
// bar). Restore with `maximizing` only takes back such a click.
bool BlankClickDeselect(AppState& s, POINT client, bool maximize = false);
bool RestoreBlankDeselect(AppState& s, POINT client, bool maximizing = false);
void CancelFileClick(AppState& s);
// 悬停选中 (single click mode): resting on a row selects it. True when it did.
bool TickHoverSelect(AppState& s, ULONGLONG now);
bool PointInHitItemName(AppState& s, const ui::WindowViewModel& vm,
                               const ui::HitTestResult& hit, float x, float y);
void CancelScrollAnimation(AppState& s);
float TagEaseInOutQuad(float t);
void TickTabTransitions(AppState& s);
void StartSmoothScroll(AppState& s, float delta, bool horizontal = false);
void UpdateSmoothScroll(AppState& s, const HoverPointerApi& api = {});
} // namespace pulse
