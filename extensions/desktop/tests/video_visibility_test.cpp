#include "host/video/video_visibility.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

using namespace pulse::desktop::video;

namespace {
void Check(bool condition, const char* message) {
    if (!condition) {
        std::printf("[FAIL] %s (win32=%lu)\n", message, GetLastError());
        std::exit(1);
    }
}

void TestPolicy() {
    auto signals = MakeVisibilityPauseSignals(false, false, false, false,
                                               VideoPauseReason::Fullscreen, true);
    Check(!signals.AnyPaused(), "disabled auto sleep does not pause fullscreen/occlusion");
    signals = MakeVisibilityPauseSignals(false, true, true, false, VideoPauseReason::None, false);
    Check(signals.session_locked && signals.display_off && ShouldPauseVideoTranscoder(signals),
          "session/display pause remains independent of fullscreen preference");

    VideoPauseStateMachine state;
    signals = MakeVisibilityPauseSignals(true, false, false, true, VideoPauseReason::Fullscreen, false);
    auto step = state.Step(signals, 100);
    Check(step.entered_pause && step.reason == VideoPauseReason::Manual && ShouldPauseVideoTranscoder(signals),
          "manual primary reason does not hide fullscreen from transcoder");
    signals.manual = false;
    step = state.Step(signals, 200);
    Check(step.state == PauseLifecycleState::PausedHot && step.reason == VideoPauseReason::Fullscreen &&
          !step.resumed_from_hot && state.PausedSinceMs() == 100,
          "manual unpause while fullscreen does not resume or reset freeze deadline");
    signals.fullscreen = false;
    signals.desktop_occluded = true;
    step = state.Step(signals, 30099);
    Check(!step.should_freeze_release && ShouldPauseVideoTranscoder(signals),
          "occlusion pauses transcoder and keeps original freeze deadline");
    step = state.Step(signals, 30100);
    Check(step.should_freeze_release && step.state == PauseLifecycleState::FrozenReleased,
          "continuous pause freezes at exactly 30 seconds across reason changes");
    Check(!state.Step(signals, 60100).should_freeze_release, "frozen state does not repeatedly release resources");
    signals.desktop_occluded = false;
    step = state.Step(signals, 60200);
    Check(step.resumed_from_frozen && !ShouldPauseVideoTranscoder(signals),
          "actual visibility recovery resumes frozen player and transcoder");

    Check(!ShouldStartVideoPaused(false, PauseLifecycleState::Playing) &&
          ShouldStartVideoPaused(true, PauseLifecycleState::Playing) &&
          ShouldStartVideoPaused(false, PauseLifecycleState::PausedHot) &&
          ShouldStartVideoPaused(false, PauseLifecycleState::FrozenReleased),
          "replacement video inherits manual/hot/frozen pause state");
    std::puts("[PASS] pause preference, manual/system overlap, source replacement, exact 30-second freeze and recovery");
}

void TestWindowsOnPrivateDesktop(HDESK desktop) {
    Check(SetThreadDesktop(desktop) != FALSE, "assign private desktop without switching visible desktop");
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    Check(GetMonitorInfoW(MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY), &info) != FALSE, "read monitor work area");
    WNDCLASSW cls{};
    cls.lpfnWndProc = DefWindowProcW;
    cls.hInstance = GetModuleHandleW(nullptr);
    cls.lpszClassName = L"PulseVideoVisibilityTest";
    Check(RegisterClassW(&cls) != 0, "register isolated test class");
    auto create = [&](const wchar_t* name, DWORD style) {
        HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, name, L"isolated visibility fixture", style,
            info.rcWork.left, info.rcWork.top, info.rcWork.right - info.rcWork.left,
            info.rcWork.bottom - info.rcWork.top, nullptr, nullptr, cls.hInstance, nullptr);
        Check(hwnd != nullptr, "create isolated window");
        return hwnd;
    };
    HWND popup = create(cls.lpszClassName, WS_POPUP);
    Check(!IsWindowCoveringDesktop(popup, nullptr, nullptr, 0), "hidden full-size window is not occlusion");
    ShowWindow(popup, SW_SHOWNOACTIVATE);
    Check(!IsZoomed(popup) && IsWindowCoveringDesktop(popup, nullptr, nullptr, 0),
          "borderless full-work-area window pauses without maximized state or Shell notification");
    HWND tool = CreateWindowExW(WS_EX_TOOLWINDOW, cls.lpszClassName, L"owned tool fixture", WS_POPUP,
        info.rcWork.left + 100, info.rcWork.top + 100, 250, 180, popup, nullptr, cls.hInstance, nullptr);
    Check(tool != nullptr, "create owned floating tool fixture");
    ShowWindow(tool, SW_SHOWNOACTIVATE);
    Check(!IsSingleWindowCoveringDesktop(tool, nullptr, nullptr, 0) &&
          IsWindowCoveringDesktop(tool, nullptr, nullptr, 0),
          "small foreground-owned tool retains fullscreen same-process owner occlusion");
    Check(!IsWindowCoveringDesktop(popup, popup, nullptr, 0) &&
          !IsWindowCoveringDesktop(popup, nullptr, popup, 0) &&
          !IsWindowCoveringDesktop(popup, nullptr, nullptr, GetCurrentProcessId()),
          "desktop/overlay/host process windows remain excluded");
    SetWindowPos(popup, nullptr, info.rcWork.left, info.rcWork.top,
                 (info.rcWork.right - info.rcWork.left) / 2, info.rcWork.bottom - info.rcWork.top,
                 SWP_NOACTIVATE | SWP_NOZORDER);
    Check(!IsWindowCoveringDesktop(popup, nullptr, nullptr, 0), "ordinary half-width window leaves wallpaper active");
    Check(!IsWindowCoveringDesktop(tool, nullptr, nullptr, 0), "tool owner resized smaller allows visibility recovery");
    DestroyWindow(tool);
    DestroyWindow(popup);

    HWND maximized = create(cls.lpszClassName, WS_OVERLAPPEDWINDOW);
    ShowWindow(maximized, SW_SHOWMAXIMIZED);
    Check(IsZoomed(maximized) && IsWindowCoveringDesktop(maximized, nullptr, nullptr, 0),
          "maximized captioned window covers desktop work area");
    ShowWindow(maximized, SW_SHOWMINNOACTIVE);
    Check(!IsWindowCoveringDesktop(maximized, nullptr, nullptr, 0), "minimized foreground candidate does not suppress desktop");
    DestroyWindow(maximized);

    cls.lpszClassName = L"Windows.UI.Core.CoreWindow";
    Check(RegisterClassW(&cls) != 0, "register synthetic CoreWindow class");
    HWND core = create(cls.lpszClassName, WS_POPUP);
    ShowWindow(core, SW_SHOWNOACTIVATE);
    Check(IsWindowCoveringDesktop(core, nullptr, nullptr, 0), "real app CoreWindow class is not treated as desktop shell");
    Check(!IsForegroundDesktopShell(core, nullptr, nullptr, 0), "normal app CoreWindow is not desktop shell");
    DestroyWindow(core);
    UnregisterClassW(cls.lpszClassName, cls.hInstance);

    // Transparent click-through helper overlays and zero-alpha layered windows must not occlude desktop video.
    HWND passthrough = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_TRANSPARENT, L"PulseVideoVisibilityTest", L"passthrough overlay", WS_POPUP,
        info.rcWork.left, info.rcWork.top, info.rcWork.right - info.rcWork.left,
        info.rcWork.bottom - info.rcWork.top, nullptr, nullptr, cls.hInstance, nullptr);
    Check(passthrough != nullptr, "create click-through transparent helper window");
    ShowWindow(passthrough, SW_SHOWNOACTIVATE);
    Check(!IsWindowCoveringDesktop(passthrough, nullptr, nullptr, 0),
          "click-through transparent tool overlay does not pause desktop video");
    DestroyWindow(passthrough);

    HWND zero_alpha = CreateWindowExW(
        WS_EX_LAYERED, L"PulseVideoVisibilityTest", L"zero alpha layered", WS_POPUP,
        info.rcWork.left, info.rcWork.top, info.rcWork.right - info.rcWork.left,
        info.rcWork.bottom - info.rcWork.top, nullptr, nullptr, cls.hInstance, nullptr);
    Check(zero_alpha != nullptr, "create zero-alpha layered window");
    SetLayeredWindowAttributes(zero_alpha, 0, 0, LWA_ALPHA);
    ShowWindow(zero_alpha, SW_SHOWNOACTIVATE);
    Check(!IsWindowCoveringDesktop(zero_alpha, nullptr, nullptr, 0),
          "zero-alpha layered window does not pause desktop video");
    Check(IsForegroundDesktopShell(zero_alpha, zero_alpha, nullptr, 0) &&
          IsForegroundDesktopShell(zero_alpha, nullptr, zero_alpha, 0) &&
          IsForegroundDesktopShell(zero_alpha, nullptr, nullptr, GetCurrentProcessId()),
          "desktop/overlay/host process windows count as foreground desktop shell");
    DestroyWindow(zero_alpha);

    // Click-through WS_EX_LAYERED | WS_EX_TRANSPARENT overlay (without WS_EX_TOOLWINDOW).
    HWND clickthrough_layered = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TRANSPARENT, L"PulseVideoVisibilityTest", L"clickthrough layered", WS_POPUP,
        info.rcWork.left, info.rcWork.top, info.rcWork.right - info.rcWork.left,
        info.rcWork.bottom - info.rcWork.top, nullptr, nullptr, cls.hInstance, nullptr);
    Check(clickthrough_layered != nullptr, "create click-through layered recording border");
    SetLayeredWindowAttributes(clickthrough_layered, 0, 255, LWA_ALPHA);
    ShowWindow(clickthrough_layered, SW_SHOWNOACTIVATE);
    Check(!IsWindowCoveringDesktop(clickthrough_layered, nullptr, nullptr, 0),
          "WS_EX_LAYERED|WS_EX_TRANSPARENT recording border does not pause desktop video");
    DestroyWindow(clickthrough_layered);

    // Classic color-keyed hollow recording border (LWA_COLORKEY).
    HWND colorkey_border = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOPMOST, L"PulseVideoVisibilityTest", L"colorkey border", WS_POPUP,
        info.rcWork.left, info.rcWork.top, info.rcWork.right - info.rcWork.left,
        info.rcWork.bottom - info.rcWork.top, nullptr, nullptr, cls.hInstance, nullptr);
    Check(colorkey_border != nullptr, "create LWA_COLORKEY recording border");
    SetLayeredWindowAttributes(colorkey_border, RGB(255, 0, 255), 255, LWA_COLORKEY);
    ShowWindow(colorkey_border, SW_SHOWNOACTIVATE);
    Check(!IsWindowCoveringDesktop(colorkey_border, nullptr, nullptr, 0),
          "LWA_COLORKEY hollow recording border does not pause desktop video");
    DestroyWindow(colorkey_border);

    // Per-pixel-alpha (UpdateLayeredWindow) topmost recording frame + owned floating recording toolbar.
    HWND ulw_border = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOPMOST, L"PulseVideoVisibilityTest", L"ulw border", WS_POPUP,
        info.rcWork.left, info.rcWork.top, info.rcWork.right - info.rcWork.left,
        info.rcWork.bottom - info.rcWork.top, nullptr, nullptr, cls.hInstance, nullptr);
    Check(ulw_border != nullptr, "create per-pixel-alpha topmost recording overlay");
    ShowWindow(ulw_border, SW_SHOWNOACTIVATE);
    HWND rec_bar = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST, L"PulseVideoVisibilityTest", L"rec bar", WS_POPUP,
        info.rcWork.left + 80, info.rcWork.top + 20, 260, 44, ulw_border, nullptr, cls.hInstance, nullptr);
    Check(rec_bar != nullptr, "create owned floating recording toolbar");
    ShowWindow(rec_bar, SW_SHOWNOACTIVATE);
    Check(!IsWindowCoveringDesktop(ulw_border, nullptr, nullptr, 0) &&
          !IsWindowCoveringDesktop(rec_bar, nullptr, nullptr, 0),
          "UpdateLayeredWindow topmost recording border and its owned floating toolbar do not pause desktop video");
    DestroyWindow(rec_bar);
    DestroyWindow(ulw_border);

    // Hollow 4-strip SetWindowRgn recording frame vs solid full-screen SetWindowRgn.
    const int ww = info.rcWork.right - info.rcWork.left;
    const int wh = info.rcWork.bottom - info.rcWork.top;
    HWND hollow_rgn_wnd = CreateWindowExW(
        WS_EX_TOPMOST, L"PulseVideoVisibilityTest", L"hollow rgn border", WS_POPUP,
        info.rcWork.left, info.rcWork.top, ww, wh, nullptr, nullptr, cls.hInstance, nullptr);
    Check(hollow_rgn_wnd != nullptr, "create hollow-region recording border");
    HRGN outer_rgn = CreateRectRgn(0, 0, ww, wh);
    HRGN inner_rgn = CreateRectRgn(4, 4, ww - 4, wh - 4);
    CombineRgn(outer_rgn, outer_rgn, inner_rgn, RGN_DIFF);
    DeleteObject(inner_rgn);
    Check(SetWindowRgn(hollow_rgn_wnd, outer_rgn, FALSE) != 0, "apply hollow 4px border region");
    ShowWindow(hollow_rgn_wnd, SW_SHOWNOACTIVATE);
    Check(!IsWindowCoveringDesktop(hollow_rgn_wnd, nullptr, nullptr, 0),
          "hollow 4-strip SetWindowRgn recording border does not pause desktop video");
    HRGN solid_rgn = CreateRectRgn(0, 0, ww, wh);
    Check(SetWindowRgn(hollow_rgn_wnd, solid_rgn, FALSE) != 0, "apply solid full-screen region");
    Check(IsWindowCoveringDesktop(hollow_rgn_wnd, nullptr, nullptr, 0),
          "solid full-screen SetWindowRgn window still pauses desktop video");
    DestroyWindow(hollow_rgn_wnd);

    UnregisterClassW(L"PulseVideoVisibilityTest", cls.hInstance);
    std::puts("[PASS] isolated-desktop Win32 windows: borderless, maximized, owned tool, hidden, minimized, partial, host/overlay, CoreWindow, transparent/colorkey/ULW/hollow-region recording overlays and desktop-shell check");
}
}

int main() {
    TestPolicy();
    Check(CoversDesktopWorkArea({0, 0, 1920, 1080}, {0, 0, 1920, 1040}) &&
          !CoversDesktopWorkArea({1920, 0, 3840, 1080}, {0, 0, 1920, 1040}) &&
          !CoversDesktopWorkArea({0, 0, 1920, 1080}, {0, 0, 0, 0}),
          "coverage handles fullscreen, other monitor and empty work area");
    const std::wstring name = L"PulseVideoVisibility_" + std::to_wstring(GetCurrentProcessId());
    HDESK desktop = CreateDesktopW(name.c_str(), nullptr, nullptr, 0,
        DESKTOP_CREATEWINDOW | DESKTOP_READOBJECTS | DESKTOP_WRITEOBJECTS, nullptr);
    Check(desktop != nullptr, "create private desktop");
    std::thread worker([&] { TestWindowsOnPrivateDesktop(desktop); });
    worker.join();
    Check(CloseDesktop(desktop) != FALSE, "close private desktop");
    return 0;
}
