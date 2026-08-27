#pragma once

#include <windows.h>
#include <shellapi.h>

namespace pulse::app {

class TrayController {
public:
    enum class CallbackResult { NotHandled, Handled, ExitRequested };
    static constexpr UINT kCallbackMessage = WM_APP + 50;

    TrayController() = default;
    ~TrayController();
    TrayController(const TrayController&) = delete;
    TrayController& operator=(const TrayController&) = delete;

    void Attach(HWND hwnd, HINSTANCE instance);
    void Detach();
    bool SetVisible(bool visible);
    void HideWindow();
    void RestoreWindow();
    CallbackResult HandleCallback(LPARAM event);

    bool IsVisible() const noexcept { return icon_added_; }

private:
    NOTIFYICONDATAW IconData(UINT flags = 0) const;

    HWND hwnd_ = nullptr;
    HINSTANCE instance_ = nullptr;
    bool icon_added_ = false;
};

} // namespace pulse::app
