// app_hosted_edit.h — Address/filter/rename overlay editors.
#pragma once
#include "app_runtime.h"
#include <commctrl.h>

namespace pulse {
std::wstring FormatAddressPath(const std::wstring& text);
void PlaceHostedEdit(HWND hwnd, HWND owner, const D2D1_RECT_F& cell, float scale,
                            int left_margin_dip, int right_margin_dip);
HWND CreateHostedEdit(AppState& s, SUBCLASSPROC proc);
void LayoutAddressEditor(AppState& s);
void EnsureEditVisuals(AppState& s);
void ShowAddressEditor(AppState& s);
void HideAddressEditor(AppState& s, bool navigate);
void ShowAddressSearch(AppState& s);
bool IsAddressSearchResults(const app::Tab* tab);
void SaveAddressSearchDraft(AppState& s);
void ExitAddressSearch(AppState& s);
void FillAddressSearchView(AppState& s, ui::WindowViewModel& vm);
void ShowAddressSearchScope(AppState& s);
void SubmitAddressSearch(AppState& s);
bool TickAddressSearch(AppState& s, ULONGLONG now);
void LayoutFilterEditor(AppState& s);
void ShowFilterEditor(AppState& s, bool select_mode = false);
void ShowWildcardSelect(AppState& s);
void HideFilterEditor(AppState& s, bool commit);
void LayoutRenameOverlay(AppState& s);
void ShowRenameOverlay(AppState& s);
void HideRenameOverlay(AppState& s, bool commit);
bool TagRenameCell(AppState& s, const app::TagId& tag_id, D2D1_RECT_F& cell);
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
