#pragma once

#include <windows.h>
#include <string>
#include <string_view>

namespace pulse::app {

class SingleInstanceCoordinator {
public:
    enum class AcquireResult { Primary, Existing, Failed };

    SingleInstanceCoordinator() = default;
    ~SingleInstanceCoordinator();
    SingleInstanceCoordinator(const SingleInstanceCoordinator&) = delete;
    SingleInstanceCoordinator& operator=(const SingleInstanceCoordinator&) = delete;

    AcquireResult Acquire(std::wstring_view mutex_name = {});
    void Release();
    bool ForwardOpenPath(const std::wstring& path, DWORD timeout_ms = 2000) const;
    // The same hand-off to a window the caller already located - a tab dropped
    // on another Pulse window. The target opens the folder as its own tab.
    static bool SendOpenPathToWindow(HWND target, const std::wstring& path,
                                     DWORD timeout_ms = 2000);
    // The last tab of a window, dropped on another one: the source has nothing
    // left to show and closes, so the target also takes over the singleton
    // resources the source is about to release (mutex, tray, hotkey, session).
    static bool SendTabTransfer(HWND target, const std::wstring& path,
                                DWORD timeout_ms = 2000);
    static bool DecodeTabTransfer(const COPYDATASTRUCT* data, std::wstring& path);

    static bool DecodeOpenPath(const COPYDATASTRUCT* data, std::wstring& path);
    static ULONG_PTR OpenPathMessageId() noexcept;
    static const wchar_t* WindowClassName() noexcept;

private:
    HANDLE mutex_ = nullptr;
};

} // namespace pulse::app
