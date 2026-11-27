#include "../ui/fluent_menu.h"
#include "../common/localization.h"
#include <windows.h>
#include <cstdio>
#include <chrono>
#include <string>
#include <vector>

namespace pulse::ui {
struct FluentMenuTestPeer {
    static inline FluentMenu* current = nullptr;
    static inline std::chrono::steady_clock::time_point selected;
    static inline bool fired = false, visible_at_select = false, motion_at_select = false;
    static void CALLBACK Select(HWND owner, UINT, UINT_PTR id, DWORD) {
        KillTimer(owner, id);
        fired = true;
        visible_at_select = current && IsWindowVisible(current->hwnd_) && !IsIconic(current->hwnd_);
        motion_at_select = current && current->motion_;
        selected = std::chrono::steady_clock::now();
        // The menu modal loop handles the same key message as keyboard input.
        PostMessageW(owner, WM_KEYDOWN, VK_RETURN, 0);
    }
    static int Run(HWND owner, const wchar_t* output) {
        Compositor compositor;
        if (!compositor.Init(owner)) return 2;
        compositor.RecreateTextFormats(1.0f);
        FluentMenu menu;
        if (!menu.Create(owner, &compositor, 1.0f)) return 2;
        menu.SetTheme(false, D2D1::ColorF(0.2f, 0.4f, 0.8f));
        std::vector<FluentMenuItem> items;
        for (int i = 0; i < 12; ++i) {
            FluentMenuItem item; item.command = i + 1; item.text = L"Readable action " + std::to_wstring(i + 1);
            if (IsHighContrast() && i == 1) {
                item.glyph = L"\xE8A5"; item.shortcut = L"Ctrl+2"; item.detail = L"Selected detail";
                FluentMenuItem child; child.command = 202; child.text = L"Nested action";
                item.children.push_back(std::move(child));
            }
            if (IsHighContrast() && i == 11) item.enabled = false;
            items.push_back(std::move(item));
        }
        const bool system_motion = motion::SystemAnimationsEnabled();
        SetEnvironmentVariableW(L"PULSE_MENUSHOT_OPEN_MS", system_motion ? L"0" : nullptr);
        const bool captured = menu.SaveDebugSnapshot(output, items, 1);
        SetEnvironmentVariableW(L"PULSE_MENUSHOT_OPEN_MS", nullptr);
        std::printf("first_frame_snapshot=%d\n", captured);
        if (!captured) return 2;
        int failures = 0;
        current = &menu;
        for (int round = 0; round < 8; ++round) {
            fired = false;
            SetForegroundWindow(owner);
            SetTimer(owner, 0xA91, 160, Select);
            const auto start = std::chrono::steady_clock::now();
            const int result = menu.TrackPopup(POINT{150, 150}, items);
            const auto end = std::chrono::steady_clock::now();
            KillTimer(owner, 0xA91);
            const double latency = fired ? std::chrono::duration<double, std::milli>(end - selected).count() : -1;
            const double total = std::chrono::duration<double, std::milli>(end - start).count();
            const bool ok = fired && visible_at_select && result == 1 && !IsWindowVisible(menu.hwnd_) && (system_motion || !motion_at_select);
            std::printf("[%s] native_menu round=%d command=%d select_return_ms=%.3f total_ms=%.3f hidden=%d visible_at_select=%d motion_at_select=%d\n",
                ok ? "PASS" : "FAIL", round, result, latency, total, !IsWindowVisible(menu.hwnd_), visible_at_select, motion_at_select);
            if (!ok) ++failures;
        }
        return failures ? 1 : 0;
    }
};
}

int wmain(int argc, wchar_t** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    std::printf("native_probe_start\n");
    BOOL animations = TRUE; SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &animations, 0);
    std::printf("system_animations=%d\n", animations);
    if (argc != 2) return 2;
    const auto name = L"PulseAnimationDialogs_" + std::to_wstring(GetCurrentProcessId());
    HDESK original = GetThreadDesktop(GetCurrentThreadId());
    HDESK desktop = CreateDesktopW(name.c_str(), nullptr, nullptr, 0, GENERIC_ALL, nullptr);
    if (!desktop || !SetThreadDesktop(desktop)) return 3;
    // Consume the hidden console launch's first-ShowWindow override on a fixture, not the tested popup.
    HWND bootstrap = CreateWindowExW(0, L"STATIC", L"Startup fixture", WS_POPUP, 0, 0, 1, 1,
        nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    ShowWindow(bootstrap, SW_SHOW);
    DestroyWindow(bootstrap);
    const HRESULT apartment = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    pulse::l10n::Initialize(GetModuleHandleW(nullptr), L"en-US");
    WNDCLASSW wc{}; wc.lpfnWndProc = DefWindowProcW; wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"PulseAnimationDialogOwner"; RegisterClassW(&wc);
    HWND owner = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP, wc.lpszClassName, L"Isolated animation test",
        WS_POPUP | WS_VISIBLE, 80, 80, 720, 540, nullptr, nullptr, wc.hInstance, nullptr);
    const int result = owner ? pulse::ui::FluentMenuTestPeer::Run(owner, argv[1]) : 2;
    if (owner) DestroyWindow(owner);
    if (SUCCEEDED(apartment)) CoUninitialize();
    SetThreadDesktop(original);
    CloseDesktop(desktop);
    std::printf("result=%d isolated_desktop=1\n", result);
    return result;
}
