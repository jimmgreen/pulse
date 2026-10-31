// app_hosted_edit.h — Address/filter/rename overlay editors.
#pragma once
#include "app_runtime.h"
#include <commctrl.h>

namespace pulse {
std::wstring FormatAddressPath(const std::wstring& text);
// The location typed address-bar text opens (#54): %VAR% and shell: shortcuts
// expand first; This PC (empty) and pulse: views pass through untouched.
// *shortcut is set when the text was such a shortcut.
std::wstring AddressNavigationTarget(const std::wstring& text, bool* shortcut = nullptr);
void PlaceHostedEdit(HWND hwnd, HWND owner, const D2D1_RECT_F& cell, float scale,
                            int left_margin_dip, int right_margin_dip);
HWND CreateHostedEdit(AppState& s, SUBCLASSPROC proc);
void LayoutAddressEditor(AppState& s);
void EnsureEditVisuals(AppState& s);
void ShowAddressEditor(AppState& s);
void HideAddressEditor(AppState& s, bool navigate);
// True when the click chose a 别名 suggestion (and so is consumed).
bool DismissPathEditorOutside(AppState& s, int x, int y);
// Refills the 别名 suggestions from the path editor's text.
void UpdateAddressSuggestions(AppState& s);
void ShowAddressSearch(AppState& s);
void SyncSearchBarWidth(AppState& s);
void EscapeAddressSearch(AppState& s);
void FocusSearchResults(AppState& s, bool select_first);
void BeginSearchEditAt(AppState& s, int x, int y);
bool IsAddressSearchResults(const app::Tab* tab);
void SaveAddressSearchDraft(AppState& s);
void ExitAddressSearch(AppState& s);
void FillAddressSearchView(AppState& s, ui::WindowViewModel& vm);
void ShowAddressSearchScope(AppState& s);
void ShowSearchOptions(AppState& s, bool manage = false);
void SwitchAddressSearchMode(AppState& s, bool content);
void StartIndexedContentSearch(AppState& s, app::Tab& tab, const std::wstring& rest, bool scan = false);
std::wstring ContentIndexStatusText(AppState& s);
void ShowAddressSearchHistory(AppState& s);
void RecordSearchHistory(AppState& s, const std::wstring& path);
void SubmitAddressSearch(AppState& s, bool live = false);
void QueueAddressSearch(AppState& s);
void FlushAddressSearch(AppState& s);
bool TickAddressSearch(AppState& s, ULONGLONG now);
void LayoutFilterEditor(AppState& s);
void ShowFilterEditor(AppState& s, bool select_mode = false);
void ShowWildcardSelect(AppState& s);
void HideFilterEditor(AppState& s, bool commit);
void ClearPaneFilter(AppState& s);
void SyncFilterEditor(AppState& s);
void LayoutRenameOverlay(AppState& s);
void ShowRenameOverlay(AppState& s);
// Shift+F2: the rename field sets the selected item's 别名 instead; clearing
// it (or typing the real name) removes the alias.
void ShowAliasOverlay(AppState& s);
void HideRenameOverlay(AppState& s, bool commit);
bool TagRenameCell(AppState& s, const app::TagId& tag_id, D2D1_RECT_F& cell);
void LayoutWorkspaceRenameOverlay(AppState& s);
void ShowWorkspaceRenameOverlay(AppState& s, int index);
void HideWorkspaceRenameOverlay(AppState& s, bool commit);
LRESULT CALLBACK WorkspaceRenameEditProc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
void LayoutTagRenameOverlay(AppState& s);
void ShowTagRenameOverlay(AppState& s, const app::TagId& tag_id);
void HideTagRenameOverlay(AppState& s, bool commit);
D2D1_COLOR_F HostedEditForeground(const AppState& s);
D2D1_COLOR_F HostedEditBackground(const AppState& s);
IDWriteTextFormat* HostedEditFormat(AppState& s, HWND hwnd);
bool HandleHostedEditMessage(AppState& s, HWND hwnd, UINT msg, WPARAM wParam,
                                    LPARAM lParam, LRESULT& result);
LRESULT CALLBACK AddressEditProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                        UINT_PTR /*uIdSubclass*/, DWORD_PTR dwRefData);
LRESULT CALLBACK FilterEditProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                       UINT_PTR /*uIdSubclass*/, DWORD_PTR dwRefData);
LRESULT CALLBACK RenameEditProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                       UINT_PTR /*uIdSubclass*/, DWORD_PTR dwRefData);
LRESULT CALLBACK TagRenameEditProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                          UINT_PTR /*uIdSubclass*/, DWORD_PTR dwRefData);
} // namespace pulse
