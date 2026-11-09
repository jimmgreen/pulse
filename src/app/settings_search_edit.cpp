#include "app_hosted_edit.h"
#include "app_navigation.h"
#include "../ui/ui_renderer.h"
#include <algorithm>
namespace pulse {
void SyncSettingsSearch(AppState& s) {
    const int n = GetWindowTextLengthW(s.hwndSettingsSearchEdit);
    std::wstring text(static_cast<size_t>(n) + 1, L'\0');
    GetWindowTextW(s.hwndSettingsSearchEdit, text.data(), n + 1);
    text.resize(n);
    s.settingsSearchQuery = std::move(text);
    s.settingsSearchSelected = 0;
    s.settings.SetScroll(0, 0);
    InvalidateRect(s.hwnd, nullptr, FALSE);
}
static LRESULT CALLBACK SearchProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR data) {
    auto& s = *reinterpret_cast<AppState*>(data);
    if (msg == WM_KILLFOCUS) {
        ShowWindow(hwnd, SW_HIDE);
        InvalidateRect(s.hwnd, nullptr, FALSE);
    }
    if (msg == WM_KEYDOWN) {
        if (wp == VK_ESCAPE) {
            SetWindowTextW(hwnd, L""); ShowWindow(hwnd, SW_HIDE); SetFocus(s.hwnd); return 0;
        }
        if (wp == VK_RETURN) {
            OpenSettingsSearchResult(s, s.settingsSearchSelected);
            return 0;
        }
        if (wp == VK_UP || wp == VK_DOWN) {
            auto vm = BuildVm(s, false);
            const int count = static_cast<int>(vm.settings_search_results.size());
            if (count) {
                s.settingsSearchSelected = (s.settingsSearchSelected + (wp == VK_UP ? count-1 : 1)) % count;
                const float top = (76+72*s.settingsSearchSelected)*s.scale;
                const float bottom = top+64*s.scale;
                const float viewport = static_cast<float>(s.compositor.Height())-s.renderer.TitleBarHeight()-32*s.scale;
                float scroll = s.settings.scroll();
                if (top < scroll) scroll = top;
                if (bottom > scroll+viewport) scroll = bottom-viewport;
                s.settings.SetScroll(scroll, s.renderer.SettingsMaxScroll(vm, static_cast<float>(s.compositor.Width()), static_cast<float>(s.compositor.Height())));
            }
            InvalidateRect(s.hwnd, nullptr, FALSE); return 0;
        }
    }
    LRESULT result = 0;
    if (HandleHostedEditMessage(s, hwnd, msg, wp, lp, result)) return result;
    return DefSubclassProc(hwnd, msg, wp, lp);
}
void LayoutSettingsSearch(AppState& s) {
    if (!s.hwndSettingsSearchEdit) return;
    if (!IsSettingsTab(ActiveTab(s))) { ShowWindow(s.hwndSettingsSearchEdit, SW_HIDE); return; }
    if (!IsWindowVisible(s.hwndSettingsSearchEdit)) return;
    auto vm = BuildVm(s, false);
    const float left = vm.settings_inset_left;
    const float width = static_cast<float>(s.compositor.Width()) - left;
    const float nav = (width < 760*s.scale ? 260.0f : width < 1000*s.scale ? 200.0f : 220.0f)*s.scale;
    EnsureEditVisuals(s);
    SendMessageW(s.hwndSettingsSearchEdit, WM_SETFONT, reinterpret_cast<WPARAM>(s.editFont), FALSE);
    PlaceHostedEdit(s.hwndSettingsSearchEdit, s.hwnd, D2D1::RectF(left+42*s.scale, s.renderer.TitleBarHeight()+10*s.scale, (std::min)(left+nav-36*s.scale, static_cast<float>(s.compositor.Width())-36*s.scale), s.renderer.TitleBarHeight()+46*s.scale), s.scale, 2, 2);
}
void StartSettingsSearch(AppState& s) {
    if (!IsSettingsTab(ActiveTab(s))) return;
    if (!s.hwndSettingsSearchEdit) s.hwndSettingsSearchEdit = CreateHostedEdit(s, SearchProc);
    if (!s.hwndSettingsSearchEdit) return;
    ShowWindow(s.hwndSettingsSearchEdit, SW_SHOW);
    LayoutSettingsSearch(s);
    SetFocus(s.hwndSettingsSearchEdit);
    SendMessageW(s.hwndSettingsSearchEdit, EM_SETSEL, 0, -1);
}
}
