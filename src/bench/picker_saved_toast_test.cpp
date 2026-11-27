#include "../ui/picker_saved_toast.h"
#include "../common/localization.h"
#include "../common/windows_compat.h"

#include <cstdio>
#include <filesystem>

namespace {
constexpr UINT kFirst = WM_APP + 0x101;
constexpr UINT kSecond = WM_APP + 0x102;
int first = 0, second = 0, failed = 0;
WPARAM last = 0;
LRESULT CALLBACK TargetProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == kFirst || message == kSecond) {
        if (message == kFirst) ++first; else ++second;
        last = wparam;
        return 0;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}
void Check(bool condition, const char* name) {
    std::printf("[%s] %s\n", condition ? "PASS" : "FAIL", name);
    if (!condition) ++failed;
}
void Pump(DWORD ms) {
    const ULONGLONG end = GetTickCount64() + ms;
    do {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if (GetTickCount64() >= end) break;
        MsgWaitForMultipleObjectsEx(0, nullptr, 10, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    } while (true);
}
void Click(HWND hwnd, float x, float y) {
    const auto pos = MAKELPARAM(static_cast<int>(x), static_cast<int>(y));
    SendMessageW(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, pos);
    SendMessageW(hwnd, WM_LBUTTONUP, 0, pos);
    Pump(80);
}
HWND Show(HWND target, bool dark = false, bool long_path = false) {
    using namespace pulse;
    using I = l10n::StringId;
    auto replace = [](std::wstring& text, const wchar_t* token, const std::wstring& value) {
        const auto at = text.find(token);
        if (at != std::wstring::npos) text.replace(at, wcslen(token), value);
    };
    std::wstring title = l10n::Get(I::PickerSavedObserved);
    replace(title, L"{name}", L"年度项目总结与客户验收报告（最终修订版本）.docx");
    std::wstring detail = l10n::Get(I::PickerSavedDetail);
    replace(detail, L"{folder}", long_path
        ? L"C:\\工作资料\\2026年\\客户项目交付\\设计审查与验收记录\\第三阶段最终成果"
        : L"C:\\工作资料\\2026年项目交付\\设计审查与验收记录");
    replace(detail, L"{size}", L"1.5 MB");
    replace(detail, L"{app}", L"Microsoft Word");
    return ui::ShowPickerSavedToast(target, dark, ui::HexColor(0x0078d4),
        std::move(title), std::move(detail),
        {{l10n::Get(I::AiAsstShowInPulse), kFirst, 41}, {l10n::Get(I::CopyPath), kSecond, 42}});
}
}

int main() {
    using namespace pulse;
    using namespace ui::picker_saved_toast_test;
    compat::EnableDpiAwareness();
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return 2;
    l10n::Initialize(GetModuleHandleW(nullptr), L"zh-CN");
    ui::Compositor::SetFrameCopyEnabled(true);
    WNDCLASSW wc{};
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"PulsePickerSavedToastTestTarget";
    wc.lpfnWndProc = TargetProc;
    RegisterClassW(&wc);
    HWND target = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, wc.lpszClassName,
        L"", WS_POPUP, 0, 0, 300, 200, nullptr, nullptr, wc.hInstance, nullptr);
    if (!target) return 3;
    std::filesystem::create_directories(L"bench_data/picker-save-followup-20261126");
    const HWND foreground = GetForegroundWindow();
    HWND toast = Show(target);
    Resize(toast, 392, 1.0f);
    Pump(300);
    Check(toast && IsWindowVisible(toast) && !IsWindowVisible(target), "hidden target still shows independent toast");
    Check(GetForegroundWindow() == foreground && GetWindow(toast, GW_OWNER) == nullptr &&
        (GetWindowLongPtrW(toast, GWL_EXSTYLE) & WS_EX_NOACTIVATE), "toast is unowned and never activates");
    Check(SendMessageW(toast, WM_MOUSEACTIVATE, 0, 0) == MA_NOACTIVATE, "mouse activation is suppressed");
    Check(Snapshot(toast, L"bench_data/picker-save-followup-20261126/toast-production-light.png"), "light production labels and compositor PNG");
    auto bounds = Bounds(toast);
    RECT window{};
    GetWindowRect(toast, &window);
    MONITORINFO monitor{sizeof(monitor)};
    GetMonitorInfoW(MonitorFromWindow(target, MONITOR_DEFAULTTONEAREST), &monitor);
    Check(window.right <= monitor.rcWork.right && window.bottom <= monitor.rcWork.bottom &&
        bounds.left >= 0 && bounds.top >= 0, "toast fits target monitor workarea");
    Click(toast, bounds.left + 82, bounds.bottom - 26);
    Check(first == 1 && second == 0 && last == 41 && !IsWindow(toast), "first real button posts its explicit action exactly once");
    toast = Show(target, true);
    Resize(toast, 392, 1.0f);
    Pump(300);
    Check(Snapshot(toast, L"bench_data/picker-save-followup-20261126/toast-production-dark.png"), "dark production labels and compositor PNG");
    bounds = Bounds(toast);
    Click(toast, bounds.left + 205, bounds.bottom - 26);
    Check(first == 1 && second == 1 && last == 42 && !IsWindow(toast), "second real button posts its explicit action exactly once");
    toast = Show(target);
    Resize(toast, 392, 1.0f);
    Pump(250);
    bounds = Bounds(toast);
    Click(toast, bounds.right - 20, bounds.top + 23);
    Check(!IsWindow(toast) && first == 1 && second == 1, "close button dismisses without an action");
    toast = Show(target);
    Pump(200);
    bounds = Bounds(toast);
    Click(toast, bounds.left + 60, bounds.top + 50);
    Check(IsWindow(toast) && first == 1 && second == 1, "body click does not dispatch an action");
    SendMessageW(toast, kFirst, 99, 0);
    SendMessageW(toast, WM_APP + 0x3d1, 0, 0);
    Pump(20);
    Check(first == 1 && second == 1, "unoffered or unarmed messages are never relayed");
    SendMessageW(toast, WM_LBUTTONDOWN, MK_LBUTTON,
        MAKELPARAM(static_cast<int>(bounds.left + 82), static_cast<int>(bounds.bottom - 26)));
    SendMessageW(toast, WM_LBUTTONUP, 0, MAKELPARAM(1, 1));
    Pump(20);
    Check(IsWindow(toast) && first == 1 && second == 1, "drag release outside cancels button action");
    const HWND previous = toast;
    toast = Show(target, true, true);
    Check(IsWindow(toast) && toast != previous && !IsWindow(previous), "new notification replaces the old one");
    Resize(toast, 300, 1.0f);
    Pump(300);
    Check(Snapshot(toast, L"bench_data/picker-save-followup-20261126/toast-production-narrow.png"), "narrow production labels and long Chinese path PNG");
    Expire(toast);
    Pump(20);
    Check(!IsWindow(toast) && first == 1 && second == 1, "deadline dismisses without an action");
    toast = Show(target, false, true);
    Resize(toast, 588, 1.5f);
    Pump(300);
    Check(Snapshot(toast, L"bench_data/picker-save-followup-20261126/toast-production-dpi150.png"), "150 percent DPI production labels and compositor PNG");
    DestroyWindow(target);
    target = nullptr;
    Pump(350);
    Check(!IsWindow(toast) && first == 1 && second == 1, "destroyed target closes toast without stale delivery");
    Check(!Show(target), "invalid target is rejected");
    target = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, wc.lpszClassName,
        L"", WS_POPUP, 0, 0, 300, 200, nullptr, nullptr, wc.hInstance, nullptr);
    toast = Show(target);
    Pump(15300);
    Check(!IsWindow(toast) && first == 1 && second == 1, "real 15-second timeout never invokes either action");
    Check(GetForegroundWindow() == foreground, "foreground remains unchanged throughout live tests");
    DestroyWindow(target);
    CoUninitialize();
    std::printf("RESULT: %d failure(s)\n", failed);
    return failed ? 1 : 0;
}
