#include "../setup/setup_ui.h"
#include <commctrl.h>
#include <cstdio>
#include <string>

namespace {
HWND wizard = nullptr;
ULONGLONG started = 0;
unsigned timers = 0, paints = 0, visible_editors = 0, total_editors = 0, settled_editors = 0;
HWND visible_edit = nullptr;
bool edit_verified = false, settled_checked = false;
bool clicked = false, visible_when_open = false;
LRESULT CALLBACK Observe(HWND hwnd, UINT message, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR) {
    if (GetTickCount64() - started >= 700) {
        if (message == WM_TIMER && wp == 1) ++timers;
        if (message == WM_PAINT) ++paints;
    }
    if (message == WM_NCDESTROY) RemoveWindowSubclass(hwnd, Observe, id);
    return DefSubclassProc(hwnd, message, wp, lp);
}
BOOL CALLBACK CountEdit(HWND child, LPARAM) {
    wchar_t cls[32]{}; GetClassNameW(child, cls, 32);
    if (!lstrcmpiW(cls, L"EDIT")) {
        ++total_editors;
        if (IsWindowVisible(child)) { ++visible_editors; visible_edit = child; }
    }
    return TRUE;
}
void CALLBACK Tick(HWND, UINT, UINT_PTR id, DWORD) {
    if (!wizard) {
        HWND candidate = FindWindowW(L"PulseSetupWindow", nullptr);
        // Initialization can pump messages before RunWizard shows its HWND.
        if (candidate && IsWindowVisible(candidate) && !IsIconic(candidate)) {
            wizard = candidate;
            visible_when_open = IsWindowVisible(wizard) && !IsIconic(wizard);
            SetWindowSubclass(wizard, Observe, 1, 0);
            started = GetTickCount64();
        }
    }
    if (!wizard) return;
    if (!clicked && GetTickCount64() - started >= 100) {
        clicked = true;
        // Force the actual first paint before targeting the bottom-right Custom button.
        RedrawWindow(wizard, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
        RECT client{}; GetClientRect(wizard, &client);
        const LPARAM at = MAKELPARAM(client.right - 55, client.bottom - 32);
        std::printf("setup_client=%ldx%ld click=%ld,%ld\n", client.right, client.bottom, client.right - 55, client.bottom - 32);
        SendMessageW(wizard, WM_MOUSEMOVE, 0, at);
        SendMessageW(wizard, WM_LBUTTONDOWN, MK_LBUTTON, at);
        SendMessageW(wizard, WM_LBUTTONUP, 0, at);
        RedrawWindow(wizard, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
        // Bring the path card into view without waiting for the page transition.
        SendMessageW(wizard, WM_MOUSEWHEEL, MAKEWPARAM(0, static_cast<WORD>(-2 * WHEEL_DELTA)), 0);
        RedrawWindow(wizard, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
        EnumChildWindows(wizard, CountEdit, 0);
        std::printf("setup_immediate total_editors=%u visible_editors=%u\n", total_editors, visible_editors);
    }
    if (!settled_checked && GetTickCount64() - started >= 650) {
        settled_checked = true;
        const auto immediate = visible_editors;
        visible_editors = total_editors = 0;
        EnumChildWindows(wizard, CountEdit, 0);
        settled_editors = visible_editors;
        visible_editors = immediate;
        if (visible_edit) {
            SetFocus(visible_edit);
            SendMessageW(visible_edit, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(L"C:\\PulseAnimationFixtureEdited"));
            SendMessageW(visible_edit, EM_SETSEL, 0, -1);
            wchar_t text[128]{}; GetWindowTextW(visible_edit, text, 128);
            edit_verified = GetFocus() == visible_edit && !lstrcmpW(text, L"C:\\PulseAnimationFixtureEdited");
        }
        std::printf("setup_settled visible_editors=%u edit_roundtrip=%d\n", settled_editors, edit_verified);
    }
    if (GetTickCount64() - started >= 1500) {
        KillTimer(nullptr, id);
        PostMessageW(wizard, WM_CLOSE, 0, 0);
    }
}
}
int wmain() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    std::printf("native_probe_start\n");
    BOOL animations = TRUE; SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &animations, 0);
    std::printf("system_animations=%d\n", animations);
    HDESK original = GetThreadDesktop(GetCurrentThreadId());
    const auto name = L"PulseAnimationSetup_" + std::to_wstring(GetCurrentProcessId());
    HDESK isolated = CreateDesktopW(name.c_str(), nullptr, nullptr, 0, GENERIC_ALL, nullptr);
    if (!isolated || !SetThreadDesktop(isolated)) return 3;
    // Consume the hidden console launch's first-ShowWindow override on a fixture, not the tested popup.
    HWND bootstrap = CreateWindowExW(0, L"STATIC", L"Startup fixture", WS_POPUP, 0, 0, 1, 1,
        nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    ShowWindow(bootstrap, SW_SHOW);
    DestroyWindow(bootstrap);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_STANDARD_CLASSES}; InitCommonControlsEx(&controls);
    SetEnvironmentVariableW(L"PULSE_SETUP_UI_PROBE", L"1");
    SetEnvironmentVariableW(L"PULSE_SETUP_TEST_DPI", L"96");
    pulse::setup::Payload payload;
    pulse::setup::InstallRequest request; request.options.test = true;
    request.app_dir = L"C:\\PulseAnimationFixture"; request.index_path = L"C:\\PulseAnimationFixture\\Index";
    const UINT_PTR timer = SetTimer(nullptr, 0, 10, Tick);
    const auto result = pulse::setup::RunWizard(payload, L"", request, pulse::setup::Lang::En);
    KillTimer(nullptr, timer);
    const bool ok = wizard && visible_when_open && clicked && !IsWindow(wizard) && !result.launch &&
        settled_editors > 0 && edit_verified && (!ANIMATION_EXPECT_IMMEDIATE || visible_editors > 0);
    std::printf("[%s] native_setup immediate_visible_editors=%u settled_timer_ticks=%u settled_paints=%u closed=%d launch=%d settled_editors=%u edit_roundtrip=%d visible_when_open=%d\n",
        ok ? "PASS" : "FAIL", visible_editors, timers, paints, !IsWindow(wizard), result.launch, settled_editors, edit_verified, visible_when_open);
    CoUninitialize(); SetThreadDesktop(original); CloseDesktop(isolated);
    return ok ? 0 : 1;
}
