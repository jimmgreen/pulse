// background_url_edit.cpp — 设置 › 背景图's 网络地址 / API 指针 inline fields.
#include "../common/windows_compat.h"
#include "app_internal.h"
#include "../ui/ui_renderer.h"

#include <algorithm>
#include <string>

namespace pulse {
namespace {

std::wstring FieldText(HWND hwnd) {
    const int length = GetWindowTextLengthW(hwnd);
    std::wstring text(static_cast<size_t>(length) + 1, L'\0');
    GetWindowTextW(hwnd, text.data(), length + 1);
    text.resize(static_cast<size_t>(length));
    return text;
}

// The editor covers the right part of its row so the label keeps the left.
D2D1_RECT_F FieldRect(const D2D1_RECT_F& row, float scale) {
    const float height = 32.0f * scale;
    const float top = row.top + (std::max)(0.0f, (row.bottom - row.top - height) * 0.5f);
    const float left = (std::max)(row.left + 16.0f * scale,
                                  row.right - 16.0f * scale - 360.0f * scale);
    return D2D1::RectF(left, top, row.right - 16.0f * scale, top + height);
}

LRESULT CALLBACK BackgroundUrlEditProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                       UINT_PTR, DWORD_PTR data) {
    auto& s = *reinterpret_cast<AppState*>(data);
    if (msg == WM_KILLFOCUS) {
        CommitBackgroundUrl(s);
        ShowWindow(hwnd, SW_HIDE);
        InvalidateRect(s.hwnd, nullptr, FALSE);
    } else if (msg == WM_KEYDOWN) {
        if (wParam == VK_ESCAPE) {
            ShowWindow(hwnd, SW_HIDE);
            SetFocus(s.hwnd);
            InvalidateRect(s.hwnd, nullptr, FALSE);
            return 0;
        }
        if (wParam == VK_RETURN) {
            CommitBackgroundUrl(s);
            ShowWindow(hwnd, SW_HIDE);
            SetFocus(s.hwnd);
            InvalidateRect(s.hwnd, nullptr, FALSE);
            return 0;
        }
    }
    LRESULT result = 0;
    if (HandleHostedEditMessage(s, hwnd, msg, wParam, lParam, result)) return result;
    return DefSubclassProc(hwnd, msg, wParam, lParam);
}

} // namespace

void CommitBackgroundUrl(AppState& s) {
    if (!s.hwndBackgroundUrlEdit) return;
    const std::wstring text = FieldText(s.hwndBackgroundUrlEdit);
    if (s.backgroundUrlEditPointer) s.settings.BackgroundSetApiPointer(text);
    else s.settings.BackgroundSetUrl(text);
}

void LayoutBackgroundUrlEdit(AppState& s) {
    if (!s.hwndBackgroundUrlEdit) return;
    if (!IsSettingsTab(ActiveTab(s)) || !IsWindowVisible(s.hwndBackgroundUrlEdit)) {
        ShowWindow(s.hwndBackgroundUrlEdit, SW_HIDE);
        return;
    }
    // Row cached by the settings page draw; empty until that row has been drawn.
    const D2D1_RECT_F row =
        s.renderer.SettingsBackgroundFieldRect(s.backgroundUrlEditPointer ? 1 : 0);
    if (row.bottom <= row.top) {
        ShowWindow(s.hwndBackgroundUrlEdit, SW_HIDE);
        return;
    }
    if (row.bottom <= s.renderer.TitleBarHeight() ||
        row.top >= static_cast<float>(s.compositor.Height())) {
        // Scrolled out: commit instead of leaving the field over another row.
        CommitBackgroundUrl(s);
        ShowWindow(s.hwndBackgroundUrlEdit, SW_HIDE);
        return;
    }
    EnsureEditVisuals(s);
    SendMessageW(s.hwndBackgroundUrlEdit, WM_SETFONT, reinterpret_cast<WPARAM>(s.editFont), FALSE);
    PlaceHostedEdit(s.hwndBackgroundUrlEdit, s.hwnd, FieldRect(row, s.scale), s.scale, 2, 2);
}

void StartBackgroundUrlEdit(AppState& s, bool pointer_row) {
    if (!IsSettingsTab(ActiveTab(s))) return;
    if (!s.hwndBackgroundUrlEdit)
        s.hwndBackgroundUrlEdit = CreateHostedEdit(s, BackgroundUrlEditProc);
    if (!s.hwndBackgroundUrlEdit) return;
    s.backgroundUrlEditPointer = pointer_row;
    const std::wstring& current = pointer_row ? s.appPrefs.background_api_pointer
                                              : s.appPrefs.background_url;
    SetWindowTextW(s.hwndBackgroundUrlEdit, current.c_str());
    ShowWindow(s.hwndBackgroundUrlEdit, SW_SHOW);
    LayoutBackgroundUrlEdit(s);
    SetFocus(s.hwndBackgroundUrlEdit);
    SendMessageW(s.hwndBackgroundUrlEdit, EM_SETSEL, 0, -1);
}

} // namespace pulse
