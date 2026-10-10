// app_input.h — Pointer, keyboard, marquee, and drag/drop input.
#pragma once
#include "app_runtime.h"
#include "app_hover.h"

namespace pulse {
bool HandleBrowserNavigation(AppState& s, LPARAM command);
bool HandleListCharacter(AppState& s, wchar_t ch);
void TickQuickPreviewSelection(AppState& s, ULONGLONG now);
bool HandleDetailsPreviewPointer(AppState* s, HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
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
void ResetMarquee(AppState& s);
void ApplyMarqueeSelection(AppState& s);
// Marquee start in current pane pixels (it moves with the scrolled content).
POINT MarqueeStartOnScreen(const AppState& s);
// The marquee rectangle to draw, in current pane pixels.
D2D1_RECT_F MarqueeDisplayRect(const AppState& s);
// Starts/stops edge auto-scroll for the marquee. Scrolling happens in
// UpdateMarqueeFrame on display frames; Tick (kTimerMarqueeScroll) only asks
// for the next frame while the pointer rests.
void UpdateMarqueeAutoScroll(AppState& s);
void TickMarqueeAutoScroll(AppState& s);
// Render, once per frame: advances edge auto-scroll by elapsed time and
// applies a band the pointer or scrolling changed since the last frame.
void UpdateMarqueeFrame(AppState& s);
void HandleListRowClick(AppState& s, int index, bool ctrl, bool shift);
void FinishListRowClick(AppState& s);
void CancelRenameClick(AppState& s);
bool PointInHitItemName(AppState& s, const ui::WindowViewModel& vm,
                               const ui::HitTestResult& hit, float x, float y);
void CancelScrollAnimation(AppState& s);
float TagEaseOutCubic(float x);
float TagEaseInOutQuad(float t);
void TickTagTransitions(AppState& s);
void TickTabTransitions(AppState& s);
void StartSmoothScroll(AppState& s, float delta, bool horizontal = false);
void UpdateSmoothScroll(AppState& s, const HoverPointerApi& api = {});
} // namespace pulse