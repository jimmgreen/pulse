#pragma once
#include <d2d1.h>
#include <windows.h>
#include <cstdint>
#include <string>

namespace pulse::ui {

// Hosts the system IPreviewHandler (Explorer Alt+P) in a WS_POPUP overlay.
// Office/PDF work stays in prevhost.exe via CLSCTX_LOCAL_SERVER.
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
    // Main window WM_ACTIVATE / WM_ACTIVATEAPP. Overlay is WS_EX_NOACTIVATE so
    // it never receives those; TOPMOST must drop here or it covers other apps.
    void NotifyAppActivate(bool active);

    // enabled=false unloads immediately. bounds are owner-client pixels.
    void Sync(HWND owner, const D2D1_RECT_F& bounds, const std::wstring& path,
              DWORD attrs, uint64_t generation, uint64_t modified, uint64_t size,
              bool dark, const D2D1_COLOR_F& bg, const D2D1_COLOR_F& fg, bool enabled);

    State state() const { return state_; }
    static bool CanHost(const std::wstring& path);

private:
    bool EnsureWindow();
    void PlaceOverlay();
    bool OverlayOwnsForeground() const;
    void Unload();
    bool OpenCurrent();
    void ScheduleOpen();
    std::wstring Identity(const std::wstring& path, uint64_t generation,
                          uint64_t modified, uint64_t size) const;

    HWND hwnd_ = nullptr;
    HWND owner_ = nullptr;
    HWND notify_ = nullptr;
    RECT bounds_{};
    std::wstring path_;
    std::wstring identity_;
    std::wstring pending_identity_;
    DWORD attrs_ = 0;
    bool dark_ = true;
    D2D1_COLOR_F bg_{};
    D2D1_COLOR_F fg_{};
    State state_ = State::Idle;
    bool shown_ = false;
    bool app_active_ = true;
    int placed_x_ = INT_MIN;
    int placed_y_ = INT_MIN;
    int placed_w_ = 0;
    int placed_h_ = 0;
    IUnknown* handler_ = nullptr;
    IUnknown* stream_ = nullptr;
    IUnknown* site_ = nullptr;
};

} // namespace pulse::ui
