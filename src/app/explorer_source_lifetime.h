#pragma once
#include <windows.h>

namespace pulse::app {
inline bool ExplorerSourceDestroyed(HWND window, DWORD process_id, DWORD thread_id) noexcept {
    if (!IsWindow(window)) return true;
    DWORD current_process = 0;
    const DWORD current_thread = GetWindowThreadProcessId(window, &current_process);
    // A missing property is not destruction: Explorer or another component
    // can remove it while the native window remains open. A reused HWND owned
    // by another thread/process does prove the original source has gone.
    return current_thread && current_process &&
        (current_thread != thread_id || current_process != process_id);
}
}
