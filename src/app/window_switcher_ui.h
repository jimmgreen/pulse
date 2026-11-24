#pragma once
// window_switcher_ui.h - Multi-window M7d (plan section 8, P3): the Ctrl+`
// window switcher on screen. Holding Ctrl, the key below Esc opens a card
// per window over the current one (most recent first, the previous window
// selected) and moves on with each press, Shift going back; releasing Ctrl
// switches to the selected window. Esc, a click outside the cards or losing
// focus keeps the current window. Rules: window_switcher.h; drawing:
// ui/window_switcher_view.h.
#include <windows.h>

namespace pulse {
struct AppState;

// WM_KEYDOWN: Ctrl+` (Ctrl+Shift+` backwards) opens the switcher or steps it.
// False when it is not that key or there is only one window.
bool HandleWindowSwitcherKey(AppState& s, LPARAM key_lparam);
// While open the switcher takes the window's keyboard and mouse input.
// True when the message was consumed.
bool WindowSwitcherMessage(AppState& s, HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

// Ctrl+Tab "最近使用的标签" (ui modes step 7, rules in tab_mru.h): opens the
// recent tabs panel on the previous tab (Shift: the oldest row) or steps it;
// releasing Ctrl opens the selected tab. While open, WindowSwitcherMessage
// routes the window's input to it. False when there is nothing to switch to.
bool HandleTabMruKey(AppState& s, bool back);
// Activates a tab from the keyboard as Ctrl+1..9 do (app_input.cpp): editors
// close first and the list takes the focus.
void SwitchTabFromKeyboard(AppState& s, size_t index);

}  // namespace pulse