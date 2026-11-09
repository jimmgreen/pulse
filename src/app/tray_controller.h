#pragma once

#include <windows.h>
#include <shellapi.h>

#include <functional>

namespace pulse::app {

// Notification-area icon rule (#57). `background`: closing keeps Pulse running
// (keep running after close, or global search). `mode`: 0 always, 1 only while
// the window is hidden, 2 never.
inline bool TrayIconWanted(bool background, int mode, bool window_hidden) {
    if (mode == 2) return false;
    return background && (mode != 1 || window_hidden);
}

class TrayController {
public:
    enum class CallbackResult { NotHandled, Handled, ExitRequested };
    static constexpr UINT kCallbackMessage = WM_APP + 50;

    TrayController() = default;
    ~TrayController();
    TrayController(const TrayController&) = delete;
    TrayController& operator=(const TrayController&) = delete;

    // `window` is the UI window hidden and restored. `icon_window` owns the
    // notification icon, its callback message and menu (Pulse passes its
    // hidden host window, so the icon outlives any one UI window); without
    // it the UI window owns the icon too.
    void Attach(HWND window, HINSTANCE instance, HWND icon_window = nullptr);
    HWND IconWindow() const noexcept { return icon_window_; }
    HWND Window() const noexcept { return hwnd_; }
    // Multi-window (M4a): the closing window hands hide/restore over to the
    // next one. The icon, its visibility and its owner stay as they are.
    void Retarget(HWND window) noexcept {
        if (!hwnd_ || !window) return;
        if (hwnd_ == window) return;
        hwnd_ = window;
        restore_maximized_ = false;
        has_hidden_placement_ = false;
    }
    void Detach();
    bool SetVisible(bool visible);
    // `show_icon` false (icon set to never): hide without an icon; a second
    // launch of Pulse brings the window back.
    void HideWindow(bool show_icon = true);
    void RestoreWindow();
    // Sign-in launch into the tray: add the icon and keep the window hidden.
    // Returns false (caller shows the window) when the icon cannot be added
    // although the taskbar exists; before Explorer is up the icon is added
    // on TaskbarCreated. `maximized`: the first restore shows it maximized.
    // `show_icon` false: stay hidden without an icon (see HideWindow).
    bool StartHidden(bool maximized, bool show_icon = true);
    // Explorer (re)created the taskbar: icons added earlier are gone.
    static UINT TaskbarCreatedMessage();
    void HandleTaskbarCreated();
    bool IconVisible() const { return icon_added_; }
    // Runs inside RestoreWindow() just before a hidden window is shown again
    // (tray click, tray "Open", a second launch), so the app can reset it first.
    void SetBeforeRestore(std::function<void()> hook) { before_restore_ = std::move(hook); }
    CallbackResult HandleCallback(LPARAM event,
        const std::function<int(POINT, HWND)>& show_menu = {});

    bool IsVisible() const noexcept { return icon_added_; }

private:
    NOTIFYICONDATAW IconData(UINT flags = 0) const;

    HWND hwnd_ = nullptr;
    HWND icon_window_ = nullptr;
    HINSTANCE instance_ = nullptr;
    bool icon_added_ = false;
    bool wanted_visible_ = false;   // last SetVisible request, for TaskbarCreated
    bool restore_maximized_ = false;
    WINDOWPLACEMENT hidden_placement_{sizeof(WINDOWPLACEMENT)};
    bool has_hidden_placement_ = false;
    std::function<void()> before_restore_;
};

} // namespace pulse::app
