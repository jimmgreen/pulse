#pragma once
#include <d2d1.h>
#include <windows.h>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

namespace pulse::ui {

// Hosts the system IPreviewHandler (Explorer Alt+P) in a WS_POPUP overlay.
// All third-party COM calls and the overlay HWND live on a dedicated STA.
// The overlay matches the details preview rect so Word/Excel keep their own
// scrollbars inside the pane (Explorer behavior).
class PreviewHandlerHost {
public:
    enum class State { Idle, Loading, Shown, Failed };

    PreviewHandlerHost();
    ~PreviewHandlerHost();
    PreviewHandlerHost(const PreviewHandlerHost&) = delete;
    PreviewHandlerHost& operator=(const PreviewHandlerHost&) = delete;

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    void SetNotifyWindow(HWND hwnd);
    void Hide();
    void Reset();
    // Re-evaluates the popup overlay's screen position after its owner moves.
    // This only wakes the STA worker; it does not reopen the preview content.
    void Reposition();
    // Main window WM_ACTIVATE / WM_ACTIVATEAPP. Overlay is WS_EX_NOACTIVATE so
    // it never receives those; TOPMOST must drop here or it covers other apps.
    void NotifyAppActivate(bool active);

    // enabled=false unloads immediately. bounds are owner-client pixels.
    void Sync(HWND owner, const D2D1_RECT_F& bounds, const std::wstring& path,
              DWORD attrs, uint64_t generation, uint64_t modified, uint64_t size,
              bool dark, const D2D1_COLOR_F& bg, const D2D1_COLOR_F& fg, bool enabled,
              bool immediate = false);

    State state() const;
    static bool CanHost(const std::wstring& path);

private:
    struct WorkerState;
    void EnsureWorker();
    void Publish(bool enabled, HWND owner, const RECT& bounds,
                 const std::wstring& path, const std::wstring& identity,
                 DWORD attrs, bool immediate);
    static DWORD WINAPI WorkerMain(void* parameter);

    std::shared_ptr<WorkerState> worker_;
    HWND notify_ = nullptr;
    bool app_active_ = true;
    std::wstring last_identity_;
    RECT last_bounds_{};
    HWND last_owner_ = nullptr;
    bool last_enabled_ = false;
};

} // namespace pulse::ui
