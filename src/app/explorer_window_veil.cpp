// explorer_window_veil.cpp — see explorer_window_veil.h.
#include "explorer_window_veil.h"

#include <cwchar>

namespace pulse::app {
namespace {

constexpr wchar_t kVeilProperty[] = L"Pulse.ExplorerVeil";
const HANDLE kVeiledWithoutLayer = reinterpret_cast<HANDLE>(1);

bool IsCabinet(HWND hwnd) {
    wchar_t name[32]{};
    return GetClassNameW(hwnd, name, ARRAYSIZE(name)) && wcscmp(name, L"CabinetWClass") == 0;
}

} // namespace

bool IsExplorerWindowVeiled(HWND hwnd) noexcept {
    return hwnd && GetPropW(hwnd, kVeilProperty) == kVeiledWithoutLayer;
}

bool VeilExplorerWindow(HWND hwnd) noexcept {
    if (!hwnd || !IsCabinet(hwnd) || GetPropW(hwnd, kVeilProperty)) return false;
    const LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    // A window that is already layered owns its transparency; leave it alone.
    if (!style || (style & WS_EX_LAYERED)) return false;
    // Mark first: a crash after this point is recoverable by the marker.
    if (!SetPropW(hwnd, kVeilProperty, kVeiledWithoutLayer)) return false;
    // SetWindowLongPtr returns the previous (non-zero) style on success.
    const bool layered = SetWindowLongPtrW(hwnd, GWL_EXSTYLE, style | WS_EX_LAYERED) != 0 &&
                         SetLayeredWindowAttributes(hwnd, 0, 0, LWA_ALPHA);
    if (!layered) {
        UnveilExplorerWindow(hwnd);
        return false;
    }
    return true;
}

void UnveilExplorerWindow(HWND hwnd) noexcept {
    if (!IsExplorerWindowVeiled(hwnd)) return;
    const LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if (style & WS_EX_LAYERED) {
        SetWindowLongPtrW(hwnd, GWL_EXSTYLE, style & ~WS_EX_LAYERED);
        RedrawWindow(hwnd, nullptr, nullptr, RDW_ERASE | RDW_INVALIDATE | RDW_FRAME | RDW_ALLCHILDREN);
    }
    RemovePropW(hwnd, kVeilProperty);
}

void UnveilAbandonedExplorerWindows() noexcept {
    EnumWindows([](HWND hwnd, LPARAM) -> BOOL {
        if (IsExplorerWindowVeiled(hwnd) && IsCabinet(hwnd)) UnveilExplorerWindow(hwnd);
        return TRUE;
    }, 0);
}

} // namespace pulse::app
