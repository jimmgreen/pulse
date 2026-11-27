int RunPointerMonitorTests() {
    using State = pulse::GlobalSearchWindowTestPeer::State;
    using SetAwareness = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
    const auto set_awareness = reinterpret_cast<SetAwareness>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext"));
    if (set_awareness) set_awareness(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    pulse::l10n::Initialize(GetModuleHandleW(nullptr), L"en-US");
    unsigned checks = 0, failures = 0;
    const auto check = [&](bool ok, const char* label) {
        ++checks; failures += !ok;
        std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
    };
    std::vector<HMONITOR> monitors;
    EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR monitor, HDC, LPRECT, LPARAM param) -> BOOL {
        reinterpret_cast<std::vector<HMONITOR>*>(param)->push_back(monitor); return TRUE;
    }, reinterpret_cast<LPARAM>(&monitors));
    check(!monitors.empty(), "enumerate actual Windows display topology");
    if (monitors.empty()) { CoUninitialize(); return 1; }
    emulate_search_activation = true;
    accept_search_activation_direct = accept_search_activation_retry = false;
    search_fixture_cursor_enabled = true;
    // Do not send any physical input or activate a window on the user's desktop.
    // The existing private-desktop runner isolates the real HWND show path.
    {
        pulse::GlobalSearchWindow window;
        for (const auto monitor : monitors) {
            MONITORINFO info{sizeof(info)};
            check(GetMonitorInfoW(monitor, &info) != FALSE, "read real monitor work area");
            search_fixture_cursor = {info.rcWork.left + 12, info.rcWork.top + 12};
            pulse::GlobalSearchContext context;
            context.show_apps = false; context.positioned = true;
            context.x = 1000000; context.y = 1000000; // removed/other display
            context.width = 800; context.height = 500;
            window.SetContext(context);
            check(window.Show(nullptr, true, 1, L""), "show actual search HWND with a stale saved monitor position");
            auto& state = pulse::GlobalSearchWindowTestPeer::Get(window);
            RECT actual{}; GetWindowRect(state.hwnd, &actual);
            std::printf("DISPLAY work=%ld,%ld,%ld,%ld scale=%.2f actual=%ld,%ld,%ld,%ld\n",
                info.rcWork.left, info.rcWork.top, info.rcWork.right, info.rcWork.bottom, state.scale,
                actual.left, actual.top, actual.right, actual.bottom);
            const int width = actual.right - actual.left, height = actual.bottom - actual.top;
            check(actual.left == info.rcWork.left + (info.rcWork.right - info.rcWork.left - width) / 2 &&
                actual.top == info.rcWork.top + (info.rcWork.bottom - info.rcWork.top - height) / 2,
                "stale or other-screen coordinates center on the pointer monitor instead of clamping to its edge");
            check(MonitorFromWindow(state.hwnd, MONITOR_DEFAULTTONEAREST) == monitor,
                "native search HWND belongs to pointer-selected physical monitor");
            check(actual.left >= info.rcWork.left && actual.top >= info.rcWork.top &&
                actual.right <= info.rcWork.right && actual.bottom <= info.rcWork.bottom,
                "native search rectangle fits real work area");
            const int expected_w = std::min(state.Px(800), static_cast<int>(info.rcWork.right - info.rcWork.left) - state.Px(32));
            const int expected_h = std::min(state.Px(500), static_cast<int>(info.rcWork.bottom - info.rcWork.top) - state.Px(32));
            check(width == expected_w && height == expected_h, "actual HWND size preserves DIP preference at target monitor DPI");
            window.Hide();
            context.x = info.rcWork.left + 24; context.y = info.rcWork.top + 24;
            window.SetContext(context);
            check(window.Show(nullptr, true, 1, L""), "reopen cached search HWND on the same pointer monitor");
            GetWindowRect(state.hwnd, &actual);
            check(actual.left == context.x && actual.top == context.y, "same-monitor reopen retains user placement");
            window.Hide();
        }
        window.Shutdown();
    }
    State geometry;
    geometry.context.positioned = true; geometry.context.x = 120; geometry.context.y = 150;
    geometry.context.width = 800; geometry.context.height = 500;
    // Same assertions can be run against the unchanged pre-edit implementation.
    const auto cross = []<typename T>(T& state, const RECT& work) {
        if constexpr (requires { state.Placement(work, false); }) return state.Placement(work, false);
        else return state.Placement(work);
    };
    for (const float dpi : {1.0f, 1.25f, 1.5f, 2.0f}) {
        geometry.scale = dpi;
        const RECT work{-2560, -1440, 0, 0};
        const auto rect = cross(geometry, work);
        check(rect.right - rect.left == geometry.Px(800) && rect.bottom - rect.top == geometry.Px(500),
            "cross-screen geometry retains DIP dimensions at 100/125/150/200 percent");
        check(rect.left == work.left + (2560 - geometry.Px(800)) / 2 &&
            rect.top == work.top + (1440 - geometry.Px(500)) / 2,
            "negative and above-primary display geometry is centered, not edge-clamped");
    }
    geometry.context.width = 10000; geometry.context.height = 10000;
    const RECT narrow{0, 48, 600, 700};
    const auto rect = cross(geometry, narrow);
    check(rect.left >= narrow.left && rect.top >= narrow.top && rect.right <= narrow.right && rect.bottom <= narrow.bottom,
        "oversized saved dimensions shrink inside small taskbar-adjusted work area");
    search_fixture_cursor_enabled = false;
    emulate_search_activation = false;
    CoUninitialize();
    std::printf("POINTER_MONITOR checks=%u failures=%u physical_monitors=%zu cursor_coordinates=injected real_input_sent=0\n",
        checks, failures, monitors.size());
    return failures ? 1 : 0;
}
