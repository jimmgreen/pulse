std::string FocusTestDesktopName(HDESK desktop) {
    wchar_t name[256]{};
    if (!desktop || !GetUserObjectInformationW(desktop, UOI_NAME, name, sizeof(name), nullptr)) return "<unavailable>";
    char text[768]{};
    WideCharToMultiByte(CP_UTF8, 0, name, -1, text, sizeof(text), nullptr, nullptr);
    return text;
}
DWORD FocusTestIntegrity(DWORD process_id) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process_id);
    if (!process) return 0;
    HANDLE token = nullptr;
    const bool opened = OpenProcessToken(process, TOKEN_QUERY, &token) != FALSE;
    CloseHandle(process);
    if (!opened) return 0;
    DWORD bytes = 0, integrity = 0;
    GetTokenInformation(token, TokenIntegrityLevel, nullptr, 0, &bytes);
    std::vector<BYTE> storage(bytes);
    if (bytes && GetTokenInformation(token, TokenIntegrityLevel, storage.data(), bytes, &bytes)) {
        const auto* label = reinterpret_cast<const TOKEN_MANDATORY_LABEL*>(storage.data());
        if (IsValidSid(label->Label.Sid)) {
            const auto count = *GetSidSubAuthorityCount(label->Label.Sid);
            if (count) integrity = *GetSidSubAuthority(label->Label.Sid, count - 1);
        }
    }
    CloseHandle(token);
    return integrity;
}
bool DescribeFocusTestEnvironment(const char* phase) {
    const HDESK thread_desktop = GetThreadDesktop(GetCurrentThreadId());
    const HDESK input_desktop = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
    BOOL receiving_input = FALSE;
    const bool queried_input = GetUserObjectInformationW(thread_desktop, UOI_IO,
        &receiving_input, sizeof(receiving_input), nullptr) != FALSE;
    const HWND foreground = GetForegroundWindow();
    DWORD foreground_process = 0;
    const DWORD foreground_thread = foreground ? GetWindowThreadProcessId(foreground, &foreground_process) : 0;
    GUITHREADINFO own_gui{sizeof(own_gui)}, foreground_gui{sizeof(foreground_gui)};
    const bool own_gui_ok = GetGUIThreadInfo(GetCurrentThreadId(), &own_gui) != FALSE;
    const bool foreground_gui_ok = foreground_thread && GetGUIThreadInfo(foreground_thread, &foreground_gui);
    std::cout << "[ENV] phase=" << phase << " thread_desktop=" << FocusTestDesktopName(thread_desktop)
        << " input_desktop=" << FocusTestDesktopName(input_desktop)
        << " input_query=" << queried_input << " receiving_input=" << (receiving_input != FALSE)
        << " pid=" << GetCurrentProcessId() << " integrity=" << FocusTestIntegrity(GetCurrentProcessId())
        << " foreground=" << foreground << " foreground_pid=" << foreground_process
        << " foreground_integrity=" << (foreground_process ? FocusTestIntegrity(foreground_process) : 0) << '\n';
    std::cout << "[GUI] phase=" << phase << " own_ok=" << own_gui_ok << " own_active=" << own_gui.hwndActive
        << " own_focus=" << own_gui.hwndFocus << " own_flags=" << own_gui.flags
        << " foreground_ok=" << foreground_gui_ok << " foreground_active=" << foreground_gui.hwndActive
        << " foreground_focus=" << foreground_gui.hwndFocus << " foreground_flags=" << foreground_gui.flags << '\n';
    if (input_desktop) CloseDesktop(input_desktop);
    return queried_input && receiving_input != FALSE;
}
int RunActivationDeniedTests(bool diagnose_legacy_direct) {
    // SetThreadDesktop does not switch the input desktop. The live focus
    // assertion is meaningful only where the user input queue can reach it.
    // https://learn.microsoft.com/windows/win32/api/winuser/nf-winuser-getuserobjectinformationw
    const bool input_desktop = DescribeFocusTestEnvironment("before");
    if (!diagnose_legacy_direct && !input_desktop) {
        std::cout << "[ENVIRONMENT] live foreground verification requires the input desktop; no live assertions ran\n"
            << "RESULT ENVIRONMENT activation-denied exit=2\n";
        return 2;
    }
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    pulse::l10n::Initialize(GetModuleHandleW(nullptr), L"en-US");
    const auto root = std::filesystem::absolute(L"bench_data/search-activation-denied");
    std::filesystem::create_directories(root / L"profile");
    SetEnvironmentVariableW(L"LOCALAPPDATA", (root / L"profile").c_str());
    pulse::diagnostics::runtime::Initialize((root / L"profile").wstring(), "search-activation-test");
    int failures = 0;
    const auto check = [&](bool ok, const char* label) {
        std::cout << (ok ? "[PASS] " : "[FAIL] ") << label << '\n';
        if (!ok) ++failures;
    };
    const HWND previous_foreground = GetForegroundWindow();
    log_search_activation_api = true;
    search_activation_other = CreateWindowExW(0, L"STATIC", L"Activation fixture", WS_POPUP,
        0, 0, 100, 100, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    check(search_activation_other != nullptr, "create independent foreground fixture");
    {
        pulse::GlobalSearchWindow window;
        pulse::GlobalSearchContext context;
        context.show_apps = false;
        window.SetContext(std::move(context));
        reject_search_activation = true;
        const bool shown = window.Show(nullptr, true, 1, root.wstring());
        check(shown, "activation refusal is not a search-window open failure");
        check(window.Visible(), "activation refusal keeps the search window available to click");
        reject_search_activation = false;
        DescribeFocusTestEnvironment("after_forced_denial");
        auto& state = pulse::GlobalSearchWindowTestPeer::Get(window);
        // Removing the test override does not grant Windows foreground rights.
        // Exercise the actual double-Ctrl recovery; diagnostics retain the old
        // direct-call scenario and its unchanged, strict focus assertion.
        const auto trigger = diagnose_legacy_direct ? pulse::app::SearchTrigger::Hotkey : pulse::app::SearchTrigger::DoubleCtrl;
        const bool reopened = window.Show(nullptr, true, 1, root.wstring(), true, {}, trigger);
        DescribeFocusTestEnvironment("after_recovery");
        std::cout << "[WINDOW] search=" << state.hwnd << " edit=" << state.edit
            << " visible=" << window.Visible() << " trigger=" << static_cast<unsigned>(trigger) << '\n';
        check(reopened && window.Visible() && GetForegroundWindow() == state.hwnd && GetFocus() == state.edit,
            "subsequent permitted activation focuses the existing search edit");
        const bool restore_foreground = GetForegroundWindow() == state.hwnd;
        window.Hide();
        check(!window.Visible(), "explicit dismissal still hides search");
        if (restore_foreground && previous_foreground && IsWindow(previous_foreground)) SetForegroundWindow(previous_foreground);
    }
    DestroyWindow(search_activation_other); search_activation_other = nullptr;
    log_search_activation_api = false;
    pulse::diagnostics::runtime::Shutdown();
    CoUninitialize();
    std::cout << "RESULT " << (failures ? "FAIL" : "PASS")
        << (diagnose_legacy_direct ? " activation-denied-diagnose\n" : " activation-denied\n");
    return failures ? 1 : 0;
}
