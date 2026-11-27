#include "../ui/toolbar_picker.h"
#include "../ui/ui_compositor.h"
#include "../common/localization.h"
#include <commctrl.h>
#include <cstdio>
#include <string>
#include <string_view>

namespace pulse::ui {
HWND ProbeToolbarPicker(const wchar_t* keyword, POINT* center, const wchar_t* snapshot);
bool ProbeToolbarPickerLayout(const wchar_t* report, const wchar_t* snapshot_prefix);
}

namespace {
ULONGLONG started = 0;
HWND picker = nullptr;
unsigned paints = 0, frames = 0, clicks = 0, callbacks = 0;
bool timeout = false, layout_pass = true, visible_when_open = false;
std::wstring layout_prefix;
unsigned long long Cpu() {
    FILETIME c{}, e{}, k{}, u{}; GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u);
    return (static_cast<unsigned long long>(k.dwHighDateTime) << 32) + k.dwLowDateTime +
           (static_cast<unsigned long long>(u.dwHighDateTime) << 32) + u.dwLowDateTime;
}
LRESULT CALLBACK Observe(HWND hwnd, UINT message, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR) {
    if (message == WM_PAINT) ++paints;
    if (message == WM_TIMER && wp == 1) ++frames;
    if (message == WM_NCDESTROY) RemoveWindowSubclass(hwnd, Observe, id);
    return DefSubclassProc(hwnd, message, wp, lp);
}
void CALLBACK Tick(HWND owner, UINT, UINT_PTR id, DWORD) {
    if (!picker) {
        picker = FindWindowW(L"PulseToolbarPicker", nullptr);
        if (picker) { visible_when_open = IsWindowVisible(picker) && !IsIconic(picker); SetWindowSubclass(picker, Observe, 1, 0); }
    }
    if (picker && !layout_prefix.empty()) {
        layout_pass = pulse::ui::ProbeToolbarPickerLayout((layout_prefix + L".txt").c_str(), layout_prefix.c_str());
        KillTimer(owner, id);
        pulse::ui::CloseToolbarPicker();
        return;
    }
    const auto elapsed = GetTickCount64() - started;
    if (picker && clicks < 2 && elapsed >= (clicks == 0 ? 350u : 650u)) {
        POINT point{};
        if (pulse::ui::ProbeToolbarPicker(L"Action 1", &point, nullptr)) {
            const LPARAM at = MAKELPARAM(point.x, point.y);
            SendMessageW(picker, WM_MOUSEMOVE, 0, at);
            SendMessageW(picker, WM_LBUTTONDOWN, MK_LBUTTON, at);
            SendMessageW(picker, WM_LBUTTONUP, 0, at);
            ++clicks;
        }
    }
    if (elapsed >= 1000 && picker) {
        KillTimer(owner, id);
        PostMessageW(picker, WM_CLOSE, 0, 0);
    } else if (GetTickCount64() - started >= 5000) {
        timeout = true; KillTimer(owner, id); pulse::ui::CloseToolbarPicker();
    }
}
}
int wmain(int argc, wchar_t** argv) {
    if (argc == 3 && std::wstring_view(argv[1]) == L"--layout") layout_prefix = argv[2];
    else if (argc != 1) return 2;
    if (!layout_prefix.empty()) pulse::ui::Compositor::SetFrameCopyEnabled(true);
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    std::printf("native_probe_start\n");
    BOOL animations = TRUE; SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &animations, 0);
    std::printf("system_animations=%d\n", animations);
    HDESK original = GetThreadDesktop(GetCurrentThreadId());
    const auto name = L"PulseAnimationPicker_" + std::to_wstring(GetCurrentProcessId());
    HDESK isolated = CreateDesktopW(name.c_str(), nullptr, nullptr, 0, GENERIC_ALL, nullptr);
    if (!isolated || !SetThreadDesktop(isolated)) return 3;
    // Consume the hidden console launch's first-ShowWindow override on a fixture, not the tested popup.
    HWND bootstrap = CreateWindowExW(0, L"STATIC", L"Startup fixture", WS_POPUP, 0, 0, 1, 1,
        nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    ShowWindow(bootstrap, SW_SHOW);
    DestroyWindow(bootstrap);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_STANDARD_CLASSES}; InitCommonControlsEx(&controls);
    pulse::l10n::Initialize(GetModuleHandleW(nullptr), L"en-US");
    HWND owner = CreateWindowExW(0, L"STATIC", L"Isolated picker owner", WS_POPUP | WS_VISIBLE,
        60, 60, 900, 700, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    int failures = 0;
    for (int round = 0; round < (layout_prefix.empty() ? 4 : 1); ++round) {
        pulse::ui::ToolbarCustomizeSpec spec;
        spec.title = L"Customize toolbar"; spec.done_text = L"Done";
        spec.tools_tab = L"Tools"; spec.sel_tab = L"Selection"; spec.apps_tab = L"Apps";
        spec.anchor = RECT{130, 100, 160, 130}; spec.sections = {L"Actions"};
        for (int i = 0; i < 12; ++i) {
            pulse::ui::ToolbarPickerTile tile; tile.label = L"Action " + std::to_wstring(i + 1);
            tile.glyph = L"\xE8A5"; tile.checked = i < 3; tile.keywords = tile.label;
            spec.pins.push_back(std::move(tile));
        }
        started = GetTickCount64(); picker = nullptr; paints = frames = clicks = callbacks = 0; timeout = false;
        spec.on_tools = [](const auto&) { ++callbacks; };
        const auto cpu = Cpu();
        SetTimer(owner, 0xC91, 10, Tick);
        pulse::ui::ShowToolbarPicker(owner, spec, false, D2D1::ColorF(0.2f, 0.4f, 0.8f));
        KillTimer(owner, 0xC91);
        const bool ok = picker && visible_when_open && !timeout && !IsWindow(picker) && !pulse::ui::ToolbarPickerOpen() &&
            (layout_prefix.empty() ? clicks == 2 && callbacks == 2 && spec.pins.front().checked : layout_pass);
        std::printf("[%s] native_picker round=%d paints=%u frame_timer_ticks=%u elapsed_ms=%llu cpu_ms=%.3f closed=%d clicks=%u callbacks=%u restored=%d visible_when_open=%d\n",
            ok ? "PASS" : "FAIL", round, paints, frames, GetTickCount64() - started, (Cpu() - cpu) / 10000.0, !IsWindow(picker), clicks, callbacks, spec.pins.front().checked, visible_when_open);
        if (!ok) ++failures;
    }
    DestroyWindow(owner); CoUninitialize(); SetThreadDesktop(original); CloseDesktop(isolated);
    return failures ? 1 : 0;
}
