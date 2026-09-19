#pragma once
#include <d2d1.h>
#include <windows.h>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

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

#ifdef PULSE_PREVIEW_HANDLER_TESTING
    // The overlay HWND the apartment owns, or null while it has none. Tests use
    // it to measure how quickly the preview follows a moving owner.
    HWND overlay_window_for_test() const;
#endif

private:
    struct WorkerState;
    void EnsureWorker();
    void Publish(bool enabled, HWND owner, const RECT& bounds,
                 const std::wstring& path, const std::wstring& identity,
                 DWORD attrs, bool immediate);
    // One apartment serves every preview, and it also owns the overlay window.
    // A provider that never returns from its open would therefore stall every
    // later preview, so the apartment is retired and the next selection starts a
    // fresh one. The request the pane is still showing is allowed to be slow
    // (starting an Office preview starts its application); anything else, and a
    // request the pane no longer asks for, is given up on early. Returns true
    // when the stalled apartment was retired.
    bool RetireStalledApartment(const std::wstring& requested_identity, bool requested);
    void ReapRetired();
    static DWORD WINAPI WorkerMain(void* parameter);

    std::shared_ptr<WorkerState> worker_;
    // Apartments that were retired while stuck. They keep themselves alive until
    // the provider call they are inside returns, then unload and exit.
    std::vector<std::shared_ptr<WorkerState>> retired_;
    HWND notify_ = nullptr;
    bool app_active_ = true;
    std::wstring last_identity_;
    std::wstring last_path_;
    RECT last_bounds_{};
    HWND last_owner_ = nullptr;
    bool last_enabled_ = false;
};

} // namespace pulse::ui
