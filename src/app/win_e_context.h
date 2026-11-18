#pragma once
#include <windows.h>
#include <atomic>
#include <cstdint>

namespace pulse::app {
// Poll only from the dedicated background observer thread, never the UI/hook
// callback. CurrentContextId may be read from another thread.
class WinEContextObserver {
public:
    uint64_t Poll(HWND foreground, uint64_t now, bool force = false);
    uint64_t CurrentContextId() const noexcept { return context_id_.load(std::memory_order_acquire); }
private:
    friend struct WinEContextTestPeer;
    using OpenProcessFunction = HANDLE (WINAPI*)(DWORD, BOOL, DWORD);
    OpenProcessFunction open_process_ = &OpenProcess;
    std::atomic<uint64_t> context_id_{0};
    HWND foreground_ = nullptr;
    DWORD pid_ = 0;
};
}
