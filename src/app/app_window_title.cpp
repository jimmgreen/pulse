#include "app_window_title.h"
#include "app_runtime.h"

namespace pulse {
std::wstring TaskbarWindowTitle(const app::Tab* tab) {
    if (!tab) return L"Pulse";
    const auto name = tab->virtual_title.empty()
        ? app::TabTitle(tab->current_path) : tab->virtual_title;
    return name.empty() ? L"Pulse" : name + L" — Pulse";
}

bool SyncTaskbarWindowTitle(AppState& s) {
    if (!s.hwnd) return false;
    const auto title = TaskbarWindowTitle(ActiveTab(s));
    const int length = GetWindowTextLengthW(s.hwnd);
    if (length == static_cast<int>(title.size())) {
        std::wstring current(static_cast<size_t>(length) + 1, L'\0');
        const int copied = GetWindowTextW(s.hwnd, current.data(), length + 1);
        current.resize(static_cast<size_t>(copied));
        if (current == title) return false;
    }
    return SetWindowTextW(s.hwnd, title.c_str()) != FALSE;
}
} // namespace pulse
