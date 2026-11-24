// window_show_watch.h — event-driven discovery for the automatic dialog helpers.
// An out-of-context WinEvent hook reports top-level windows being shown, so a new
// program is checked milliseconds after its first window appears instead of at the
// next EnumWindows sweep. No DLL is loaded into any process. Callbacks run on the
// thread that called Start(), only while it is inside Wait(). One watch per thread.
#pragma once
#include <windows.h>
#include <cstddef>
#include <vector>

namespace pulse::dialog_launcher {

class WindowShowWatch {
public:
    struct Shown {
        DWORD pid = 0;
        bool classified = false;  // the classifier matched one of its shown windows
    };
    // Optional, called on the watch thread for each shown top-level window.
    using Classifier = bool (*)(HWND) noexcept;
    static constexpr size_t kMaxPending = 64;  // more are left to the sweep

    WindowShowWatch() = default;
    ~WindowShowWatch();
    WindowShowWatch(const WindowShowWatch&) = delete;
    WindowShowWatch& operator=(const WindowShowWatch&) = delete;

    // False (with GetLastError) when the hook is unavailable; callers keep polling.
    bool Start(Classifier classify = nullptr) noexcept;
    bool Active() const noexcept { return hook_ != nullptr; }
    // Waits for one of `handles`, a window event or the timeout, then delivers
    // pending events. Without an active hook this is a plain handle wait.
    void Wait(const HANDLE* handles, DWORD count, DWORD timeout_ms) noexcept;
    // Processes with a newly shown top-level window since the last call, oldest first, once each.
    std::vector<Shown> Take();
    // Events seen and accepted since Start (diagnostics and tests).
    size_t Accepted() const noexcept { return accepted_; }
    // The filter: a top-level, non-child window (carets and controls report SHOW too).
    static bool TopLevel(HWND hwnd) noexcept;

private:
    static void CALLBACK OnEvent(HWINEVENTHOOK, DWORD, HWND, LONG, LONG, DWORD, DWORD);
    void Note(HWND hwnd) noexcept;
    HWINEVENTHOOK hook_ = nullptr;
    Classifier classify_ = nullptr;
    std::vector<Shown> pending_;
    size_t accepted_ = 0;
};

} // namespace pulse::dialog_launcher
