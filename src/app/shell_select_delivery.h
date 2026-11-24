#pragma once
#include "shell_window_plan.h"
#include <memory>

namespace pulse::app {

// Ownership transfers only when the receiver's queue accepts the request.
inline HRESULT PostShellSelect(HWND window, UINT message, std::unique_ptr<ShellSelectRequest> request,
    BOOL (WINAPI* post)(HWND, UINT, WPARAM, LPARAM) = &PostMessageW) {
    if (!window) return HRESULT_FROM_WIN32(ERROR_INVALID_WINDOW_HANDLE);
    if (!request || !post) return E_INVALIDARG;
    SetLastError(ERROR_SUCCESS);
    if (!post(window, message, 0, reinterpret_cast<LPARAM>(request.get()))) {
        const DWORD error = GetLastError();
        return HRESULT_FROM_WIN32(error ? error : ERROR_GEN_FAILURE);
    }
    request.release();
    return S_OK;
}

} // namespace pulse::app
