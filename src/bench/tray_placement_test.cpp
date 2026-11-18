#include "../app/tray_controller.h"
#include "../common/localization.h"
#include "../common/runtime_log.h"
#include <cstdio>
#include <filesystem>

// This fixture never opens the tray menu; keep localization out of the placement test.
namespace pulse::l10n { const std::wstring& Get(StringId) { static const std::wstring text; return text; } }
static int failures = 0;
static void Check(bool ok, const char* name) { std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name); failures += !ok; }
static RECT Rect(HWND h) { RECT r{}; GetWindowRect(h, &r); return r; }
static bool Same(RECT a, RECT b) { return EqualRect(&a, &b) != FALSE; }
int main() {
    wchar_t temp[MAX_PATH]{};
    if (!GetTempPathW(ARRAYSIZE(temp), temp)) return 2;
    const std::wstring log_root = std::wstring(temp) + L"PulseTrayPlacement-" +
        std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
    if (!CreateDirectoryW(log_root.c_str(), nullptr)) return 2;
    if (!pulse::diagnostics::runtime::Initialize(log_root, "app")) return 2;
    using SetContext = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
    if (auto set = reinterpret_cast<SetContext>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext")))
        set(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const auto instance = GetModuleHandleW(nullptr);
    HWND h = CreateWindowExW(0, L"STATIC", L"Pulse tray placement test", WS_OVERLAPPEDWINDOW,
                            250, 180, 1000, 700, nullptr, nullptr, instance, nullptr);
    if (!h) return 2;
    pulse::app::TrayController tray;
    tray.Attach(h, instance);
    ShowWindow(h, SW_SHOWNOACTIVATE);
    const RECT original = Rect(h);
    tray.HideWindow(false);
    tray.RestoreWindow();
    Check(Same(original, Rect(h)), "ordinary hide/reveal preserves position");
    tray.HideWindow(false);
    SetWindowPos(h, nullptr, 0, 0, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    tray.HideWindow(false);
    tray.RestoreWindow();
    Check(Same(original, Rect(h)), "hidden position change and repeated hide do not lose original position");
    SetWindowPos(h, nullptr, 300, 200, 900, 650, SWP_NOZORDER | SWP_NOACTIVATE);
    const RECT moved = Rect(h);
    tray.HideWindow(false);
    tray.SetBeforeRestore([h] { SetWindowPos(h, nullptr, 0, 0, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE); });
    tray.RestoreWindow();
    Check(Same(moved, Rect(h)), "restore hook cannot overwrite saved position");
    tray.SetBeforeRestore({});
    ShowWindow(h, SW_MAXIMIZE);
    tray.HideWindow(false);
    tray.RestoreWindow();
    Check(IsZoomed(h) != FALSE, "maximized window remains maximized");
    ShowWindow(h, SW_RESTORE);
    Check(Same(moved, Rect(h)), "unmaximize keeps normal position");
    ShowWindow(h, SW_MINIMIZE);
    tray.HideWindow(false);
    tray.RestoreWindow();
    Check(!IsIconic(h) && Same(moved, Rect(h)), "minimized window restores its normal position");
    ShowWindow(h, SW_MAXIMIZE);
    ShowWindow(h, SW_MINIMIZE);
    tray.HideWindow(false);
    tray.RestoreWindow();
    Check(IsZoomed(h) != FALSE, "minimized maximized window returns maximized");
    ShowWindow(h, SW_RESTORE);
    SetWindowPos(h, nullptr, 325, 225, 1000, 700, SWP_NOZORDER | SWP_NOACTIVATE);
    const RECT latest = Rect(h);
    tray.RestoreWindow();
    Check(Same(latest, Rect(h)), "revealing a visible window does not restore stale coordinates");
    for (int i = 0; i < 5; ++i) {
        tray.HideWindow(false);
        tray.RestoreWindow();
    }
    Check(Same(latest, Rect(h)), "repeated hide/reveal has no coordinate drift");
    tray.HideWindow(false);
    HWND other = CreateWindowExW(0, L"STATIC", L"other", WS_OVERLAPPEDWINDOW, 120, 100, 800, 600, nullptr, nullptr, instance, nullptr);
    const RECT other_rect = Rect(other);
    tray.Retarget(other);
    tray.RestoreWindow();
    Check(Same(other_rect, Rect(other)), "retarget does not transfer another window position");
    tray.Detach();
    DestroyWindow(other);
    DestroyWindow(h);
    HWND startup = CreateWindowExW(0, L"STATIC", L"hidden startup", WS_OVERLAPPEDWINDOW,
                                  210, 160, 900, 650, nullptr, nullptr, instance, nullptr);
    if (!startup) return 2;
    pulse::app::TrayController startup_tray;
    startup_tray.Attach(startup, instance);
    const RECT startup_rect = Rect(startup);
    startup_tray.StartHidden(false, false);
    SetWindowPos(startup, nullptr, 0, 0, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    startup_tray.RestoreWindow();
    Check(Same(startup_rect, Rect(startup)), "initial tray launch preserves position before first reveal");
    ShowWindow(startup, SW_HIDE);
    SetWindowPos(startup, nullptr, 210, 160, 900, 650, SWP_NOZORDER | SWP_NOACTIVATE);
    startup_tray.StartHidden(true, false);
    SetWindowPos(startup, nullptr, 0, 0, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    startup_tray.RestoreWindow();
    Check(IsZoomed(startup) != FALSE, "initial tray launch honors saved maximized state");
    ShowWindow(startup, SW_RESTORE);
    Check(Same(startup_rect, Rect(startup)), "initial maximized tray launch preserves normal rectangle");
    startup_tray.Detach();
    DestroyWindow(startup);
    const bool flushed = pulse::diagnostics::runtime::Flush();
    const auto health = pulse::diagnostics::runtime::GetHealth();
    Check(flushed && !health.dropped_events && !health.write_errors,
          "placement diagnostics fit the runtime schema and are written");
    pulse::diagnostics::runtime::Shutdown();
    std::error_code ignored;
    std::filesystem::remove_all(log_root, ignored);
    return failures ? 1 : 0;
}
