#include "../app/tray_controller.h"
#include "../app/tray_reveal.h"
#include "../common/localization.h"
#include "../common/runtime_log.h"
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <vector>

// This fixture never opens the tray menu; keep localization out of the placement test.
namespace pulse::l10n { const std::wstring& Get(StringId) { static const std::wstring text; return text; } }
static int failures = 0, checks = 0;
static void Check(bool ok, const char* name) { std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name); ++checks; failures += !ok; }
static RECT Rect(HWND h) { RECT r{}; GetWindowRect(h, &r); return r; }
static bool Same(RECT a, RECT b) { return EqualRect(&a, &b) != FALSE; }

struct PrivateDesktop {
    HDESK original = GetThreadDesktop(GetCurrentThreadId());
    HDESK desktop = nullptr;
    bool selected = false;
    PrivateDesktop() {
        const auto name = L"PulseTrayPlacement-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
        desktop = CreateDesktopW(name.c_str(), nullptr, nullptr, 0,
            DESKTOP_CREATEWINDOW | DESKTOP_READOBJECTS | DESKTOP_WRITEOBJECTS | DESKTOP_ENUMERATE, nullptr);
        selected = desktop && SetThreadDesktop(desktop);
    }
    ~PrivateDesktop() {
        if (selected) SetThreadDesktop(original);
        if (desktop) CloseDesktop(desktop);
    }
};

struct ShowGuard { bool block = false, destroy = false; unsigned attempts = 0; };
static LRESULT CALLBACK GuardedWindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_NCCREATE) {
        auto create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }
    auto guard = reinterpret_cast<ShowGuard*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (guard && message == WM_WINDOWPOSCHANGING) {
        auto position = reinterpret_cast<WINDOWPOS*>(lparam);
        if (position->flags & SWP_SHOWWINDOW) {
            ++guard->attempts;
            if (guard->destroy) { DestroyWindow(window); return 0; }
        }
    }
    const bool hide_after_show = guard && guard->block && message == WM_WINDOWPOSCHANGED &&
        (reinterpret_cast<WINDOWPOS*>(lparam)->flags & SWP_SHOWWINDOW);
    const LRESULT result = DefWindowProcW(window, message, wparam, lparam);
    if (hide_after_show && IsWindow(window)) ShowWindow(window, SW_HIDE);
    return result;
}

static void FailureCases(HINSTANCE instance) {
    const auto create = [&](const wchar_t* title) {
        return CreateWindowExW(0, L"STATIC", title, WS_OVERLAPPEDWINDOW,
                              210, 160, 900, 650, nullptr, nullptr, instance, nullptr);
    };
    HWND original = create(L"retarget source"), replacement = create(L"retarget destination");
    Check(original && replacement, "create restore-callback fixtures");
    if (!original || !replacement) {
        if (original) DestroyWindow(original);
        if (replacement) DestroyWindow(replacement);
        return;
    }
    pulse::app::TrayController tray;
    tray.Attach(original, instance);
    tray.StartHidden(false, false);
    tray.SetBeforeRestore([&] { tray.Retarget(replacement); });
    tray.RestoreWindow();
    Check(!IsWindowVisible(original) && !IsWindowVisible(replacement),
          "a retargeting callback cannot reveal a different window");
    tray.SetBeforeRestore({}); tray.Detach();
    DestroyWindow(original); DestroyWindow(replacement);

    HWND detached = create(L"detached restore");
    Check(detached != nullptr, "create detach fixture");
    if (detached) {
        tray.Attach(detached, instance);
        tray.SetBeforeRestore([&] { tray.Detach(); });
        tray.RestoreWindow();
        Check(!tray.Window() && !IsWindowVisible(detached), "a detaching callback cancels restore safely");
        tray.SetBeforeRestore({}); tray.Detach(); DestroyWindow(detached);
    }
    HWND destroyed = create(L"destroyed restore");
    Check(destroyed != nullptr, "create destroy fixture");
    if (destroyed) {
        tray.Attach(destroyed, instance);
        tray.SetBeforeRestore([&] { DestroyWindow(destroyed); });
        tray.RestoreWindow();
        Check(!pulse::app::InspectWindowRevealState(destroyed).Restored(), "destroyed callback target is never a successful reveal");
        tray.SetBeforeRestore({});
        tray.RestoreWindow();
        tray.Detach();
    }
    HWND throwing = create(L"throwing restore");
    Check(throwing != nullptr, "create throwing callback fixture");
    if (throwing) {
        tray.Attach(throwing, instance);
        tray.SetBeforeRestore([] { throw std::runtime_error("fixture"); });
        bool threw = false;
        try { tray.RestoreWindow(); } catch (...) { threw = true; }
        Check(!threw && !IsWindowVisible(throwing), "restore callback exception is contained without a false reveal");
        tray.SetBeforeRestore({}); tray.Detach(); DestroyWindow(throwing);
    }

    WNDCLASSW type{}; type.lpfnWndProc = GuardedWindowProc; type.hInstance = instance; type.lpszClassName = L"PulseTrayShowGuard";
    const bool registered = RegisterClassW(&type) != 0;
    Check(registered, "register actual Win32 show-refusal fixture");
    if (!registered) return;
    ShowGuard blocked; blocked.block = true;
    HWND blocked_window = CreateWindowExW(0, type.lpszClassName, L"blocked show", WS_OVERLAPPEDWINDOW,
        220, 170, 900, 650, nullptr, nullptr, instance, &blocked);
    Check(blocked_window != nullptr, "create show-refusal window");
    if (blocked_window) {
        tray.Attach(blocked_window, instance);
        const RECT saved = Rect(blocked_window);
        tray.StartHidden(false, false);
        tray.RestoreWindow();
        Check(blocked.attempts > 0 && !pulse::app::InspectWindowRevealState(blocked_window).Restored(),
              "a refused Win32 show is not a successful reveal");
        SetWindowPos(blocked_window, nullptr, 0, 0, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        blocked.block = false;
        tray.RestoreWindow();
        Check(pulse::app::InspectWindowRevealState(blocked_window).Restored() && Same(saved, Rect(blocked_window)),
              "a failed restore retains placement for the next user request");
        tray.Detach(); DestroyWindow(blocked_window);
    }
    ShowGuard destroyed_on_show; destroyed_on_show.destroy = true;
    HWND during_show = CreateWindowExW(0, type.lpszClassName, L"destroy during show", WS_OVERLAPPEDWINDOW,
        220, 170, 900, 650, nullptr, nullptr, instance, &destroyed_on_show);
    Check(during_show != nullptr, "create destroy-during-show window");
    if (during_show) {
        tray.Attach(during_show, instance);
        tray.StartHidden(false, false);
        tray.RestoreWindow();
        Check(destroyed_on_show.attempts > 0 && !IsWindow(during_show) &&
              !pulse::app::InspectWindowRevealState(during_show).Restored(), "window destroyed by show processing remains a failed reveal");
        tray.Detach();
        if (IsWindow(during_show)) DestroyWindow(during_show);
    }
    UnregisterClassW(type.lpszClassName, instance);
}

static void CheckDiagnostics(const std::wstring& root) {
    std::vector<std::string> lines;
    for (const auto& item : std::filesystem::recursive_directory_iterator(root)) {
        if (!item.is_regular_file() || item.path().extension() != L".jsonl") continue;
        std::ifstream stream(item.path(), std::ios::binary);
        for (std::string line; std::getline(stream, line);) lines.push_back(std::move(line));
    }
    const auto record = [&](const char* event, std::initializer_list<const char*> fields) {
        const auto key = std::string("\"event\":\"") + event + "\"";
        for (const auto& line : lines) {
            if (line.find(key) == std::string::npos) continue;
            bool matches = true;
            for (const auto* field : fields) matches = matches && line.find(field) != std::string::npos;
            if (matches) return true;
        }
        return false;
    };
    Check(record("tray_start_hidden", {"\"applied\":0"}), "legacy hidden-start applied field keeps its placement-only meaning");
    Check(record("tray_start_hidden_result", {"\"requested_show\":0", "\"visible_after\":0", "\"hide_ok\":1"}),
          "hidden-start outcome explicitly records a successful hide request");
    Check(record("tray_restore_result", {"\"restore_ok\":1", "\"visible_after\":1", "\"minimized_after\":0"}),
          "restore diagnostics report actual visible and nonminimized state");
    Check(record("tray_restore_result", {"\"restore_ok\":1", "\"show_called\":1", "\"show_previous_visible\":0"}),
          "a zero ShowWindow return is not logged as restoration failure");
    Check(record("tray_restore_result", {"\"reason\":\"target_changed\"", "\"restore_ok\":0"}), "retarget and detach failures are diagnosed");
    Check(record("tray_restore_result", {"\"reason\":\"window_destroyed\"", "\"restore_ok\":0"}), "callback destruction is diagnosed");
    Check(record("tray_restore_result", {"\"reason\":\"invalid_window\"", "\"window_valid\":0"}), "invalid restore handles are diagnosed");
    Check(record("tray_restore_result", {"\"reason\":\"before_restore_exception\"", "\"restore_ok\":0"}), "callback exceptions are diagnosed without user data");
    Check(record("tray_restore_result", {"\"reason\":\"not_visible\"", "\"restore_ok\":0"}), "refused shows are diagnosed as unsuccessful");
    Check(record("tray_restore_api", {"\"placement_attempted\":1", "\"show_called\":1"}), "placement fallback is recorded separately from actual visibility");
}

int main() {
    PrivateDesktop desktop;
    Check(desktop.selected, "run real window tests on a private desktop without switching the input desktop");
    if (!desktop.selected) { std::printf("desktop error=%lu\n", GetLastError()); return 2; }
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
    Check(!pulse::app::InspectWindowRevealState(nullptr).Restored() && !pulse::app::InspectWindowRevealState(h).Restored(),
          "invalid and hidden windows do not satisfy external reveal success");
    Check(pulse::app::WindowRevealState{true, true, false, false}.Restored(), "denied foreground activation is not a visibility failure");
    // Consume inherited STARTUPINFO before checking an explicit show command.
    ShowWindow(h, SW_HIDE); ShowWindow(h, SW_HIDE);
    const BOOL previously_visible = ShowWindow(h, SW_SHOWNOACTIVATE);
    Check(!previously_visible && pulse::app::InspectWindowRevealState(h).Restored(), "ShowWindow can return zero while successfully showing a hidden window");
    const RECT original = Rect(h);
    tray.HideWindow(false);
    tray.RestoreWindow();
    Check(pulse::app::InspectWindowRevealState(h).Restored(), "ordinary restore really shows the window");
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
    Check(!pulse::app::InspectWindowRevealState(h).Restored(), "a visible minimized window is not a completed external reveal");
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
    Check(!pulse::app::InspectWindowRevealState(startup).Restored(), "hidden startup remains deliberately hidden until requested");
    SetWindowPos(startup, nullptr, 0, 0, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    startup_tray.RestoreWindow();
    Check(pulse::app::InspectWindowRevealState(startup).Restored() && Same(startup_rect, Rect(startup)), "initial tray launch preserves position before first reveal");
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
    FailureCases(instance);
    const bool flushed = pulse::diagnostics::runtime::Flush();
    const auto health = pulse::diagnostics::runtime::GetHealth();
    Check(flushed && !health.dropped_events && !health.write_errors,
          "placement diagnostics fit the runtime schema and are written");
    pulse::diagnostics::runtime::Shutdown();
    CheckDiagnostics(log_root);
    std::error_code ignored;
    std::filesystem::remove_all(log_root, ignored);
    std::printf("TRAY_PLACEMENT_RESULT checks=%d failed=%d\n", checks, failures);
    return failures ? 1 : 0;
}
