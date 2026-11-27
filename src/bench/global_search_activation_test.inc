// Include after GlobalSearchWindowTestPeer. The templated setup also compiles
// against the original implementation, so baseline exercises its real proc.
int RunActivationReentryTests() {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    pulse::l10n::Initialize(GetModuleHandleW(nullptr), L"en-US");
    const auto profile = std::filesystem::absolute(L"bench_data/ctrl-search-refactor-20261125/activation/profile");
    std::filesystem::create_directories(profile);
    SetEnvironmentVariableW(L"LOCALAPPDATA", profile.c_str());
    int failures = 0;
    const auto check = [&](bool ok, const char* label) {
        std::cout << (ok ? "[PASS] " : "[FAIL] ") << label << '\n';
        if (!ok) ++failures;
    };
    const auto run = [&]<class State>(State& state) {
        check(state.Create(), "create hidden production search window");
        if (!state.hwnd) return;
        check((GetWindowLongPtrW(state.hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST) == 0,
            "production search has no permanent topmost style");
        uintptr_t token = 0;
        if constexpr (requires { state.activation; }) {
            token = state.activation.Begin(pulse::app::SearchTrigger::DoubleCtrl);
        }
        state.busy = true;
        const auto query_generation = state.generation;
        SendMessageW(state.hwnd, WM_ACTIVATEAPP, FALSE, 0);
        check(state.busy && state.generation == query_generation,
            "reentrant app deactivation preserves opening query before stack unwinds");
        if constexpr (requires { state.activation; }) {
            state.activation.CompleteOpening(token);
            state.opening_foreground = search_activation_other;
            SendMessageW(state.hwnd, WM_APP + 192, token, 0);
            check(state.activation.Visibility() == pulse::app::SearchVisibility::VisibleUnfocused && state.busy,
                "deferred production reconcile preserves activation-denied search");
            state.activation.Activated();
            SendMessageW(state.hwnd, WM_ACTIVATEAPP, FALSE, 0);
            SendMessageW(state.hwnd, WM_APP + 192, token, 0);
            check(!state.activation.Open() && !state.busy && state.generation != query_generation,
                "production external-deactivation reconciliation cancels and hides");
            const auto fresh = state.activation.Begin(pulse::app::SearchTrigger::Hotkey);
            state.busy = true;
            SendMessageW(state.hwnd, WM_APP + 192, token, 0);
            check(state.activation.Generation() == fresh && state.busy && state.activation.Open(),
                "stale production message cannot dismiss reopened search");
            state.Hide();
            state.activation.CompleteOpening(fresh);
            SendMessageW(state.hwnd, WM_APP + 192, fresh, 0);
            check(!state.activation.Open(), "explicit production close invalidates pending reconciliation");
            const auto owned_token = state.activation.Begin(pulse::app::SearchTrigger::Hotkey);
            state.activation.CompleteOpening(owned_token);
            state.activation.Activated();
            state.activation.Reconcile(owned_token, true, false, false);
            state.busy = true;
            const HWND unrelated = search_activation_other;
            const HWND owned = CreateWindowExW(0, L"STATIC", L"Hidden owned popup", WS_POPUP,
                -30000, -30000, 20, 20, state.hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
            check(owned != nullptr, "create hidden owned popup fixture");
            search_activation_other = owned;
            state.result_menu_open = true;
            SendMessageW(state.hwnd, WM_ACTIVATE, WA_INACTIVE, reinterpret_cast<LPARAM>(owned));
            check(state.busy, "same-process deactivation is deferred beyond activation stack");
            SendMessageW(state.hwnd, WM_APP + 192, owned_token, 0);
            check(state.activation.Open() && state.busy, "owned popup retains production search interaction");
            search_activation_other = unrelated;
            DestroyWindow(owned);
            SendMessageW(state.hwnd, WM_ACTIVATE, WA_INACTIVE, reinterpret_cast<LPARAM>(unrelated));
            SendMessageW(state.hwnd, WM_APP + 192, owned_token, 0);
            check(!state.activation.Open() && !state.busy,
                "unrelated same-process foreground dismisses through WM_ACTIVATE even while menu flag is set");
            state.result_menu_open = false;
        }
        if constexpr (requires { state.RequestForeground(uintptr_t{}); }) {
            emulate_search_activation = true;
            const auto prepare = [&](pulse::app::SearchTrigger trigger) {
                accept_search_activation_direct = false;
                accept_search_activation_retry = false;
                search_mouse_pulse_succeeds = search_mouse_payload_zero = true;
                emulated_search_requests = emulated_search_pulses = 0;
                search_held_input = 0;
                search_after_direct = search_after_pulse = {};
                emulated_search_foreground = state.opening_foreground = search_activation_other;
                state.busy = true;
                return state.activation.Begin(trigger);
            };
            for (const bool retry_accepts : {true, false}) {
                const auto retry_token = prepare(pulse::app::SearchTrigger::DoubleCtrl);
                accept_search_activation_retry = retry_accepts;
                const auto query_token = state.generation;
                const auto attempt = state.RequestForeground(retry_token);
                check(!attempt.direct_accepted && attempt.mouse_pulse_sent && attempt.retried &&
                    attempt.accepted == retry_accepts && emulated_search_requests == 2 &&
                    emulated_search_pulses == 1 && search_mouse_payload_zero && attempt.pulse_error == ERROR_SUCCESS,
                    retry_accepts ? "production sends exactly one empty mouse pulse and one successful retry" :
                        "production repeated refusal stops after one empty mouse pulse and one retry");
                check(state.busy && state.generation == query_token && state.activation.Open(),
                    "bounded recovery preserves production opening query");
                if (!retry_accepts) SendMessageW(state.hwnd, WM_ACTIVATE, WA_ACTIVE, 0);
                state.activation.CompleteOpening(retry_token);
                SendMessageW(state.hwnd, WM_APP + 192, retry_token, 0);
                check(state.activation.Visibility() == (retry_accepts ? pulse::app::SearchVisibility::VisibleFocused :
                    pulse::app::SearchVisibility::VisibleUnfocused),
                    "production reconciliation distinguishes recovered focus from final refusal");
                if (!retry_accepts) check(state.busy && state.activation.Open() &&
                    state.activation.Visibility() == pulse::app::SearchVisibility::VisibleUnfocused,
                    "WA_ACTIVE with external foreground does not falsely acquire focus or hide refused opening");
                state.Hide();
            }
            auto request = prepare(pulse::app::SearchTrigger::DoubleCtrl);
            search_mouse_pulse_succeeds = false;
            auto attempt = state.RequestForeground(request);
            check(!attempt.mouse_pulse_sent && !attempt.retried && emulated_search_requests == 1 &&
                emulated_search_pulses == 1 && attempt.pulse_error == ERROR_ACCESS_DENIED && search_mouse_payload_zero,
                "failed mouse injection records error and never retries foreground");
            for (const auto trigger : {pulse::app::SearchTrigger::Hotkey, pulse::app::SearchTrigger::External}) {
                request = prepare(trigger);
                attempt = state.RequestForeground(request);
                check(!attempt.mouse_pulse_sent && !attempt.retried && emulated_search_requests == 1 &&
                    emulated_search_pulses == 0, "hotkey and external triggers never inject mouse pulse");
            }
            request = prepare(pulse::app::SearchTrigger::DoubleCtrl);
            accept_search_activation_direct = true;
            attempt = state.RequestForeground(request);
            check(attempt.direct_accepted && attempt.accepted && !attempt.retried && emulated_search_pulses == 0,
                "direct foreground success never injects mouse pulse");
            for (const int key : {VK_CONTROL, VK_LCONTROL, VK_RCONTROL, VK_MENU, VK_LMENU, VK_RMENU,
                    VK_SHIFT, VK_LSHIFT, VK_RSHIFT, VK_LWIN, VK_RWIN,
                    VK_LBUTTON, VK_RBUTTON, VK_MBUTTON, VK_XBUTTON1, VK_XBUTTON2}) {
                request = prepare(pulse::app::SearchTrigger::DoubleCtrl);
                search_held_input = key;
                attempt = state.RequestForeground(request);
                check(!attempt.mouse_pulse_sent && !attempt.retried && emulated_search_pulses == 0 &&
                    emulated_search_requests == 1, "held modifier or mouse button prevents injection");
            }
            request = prepare(pulse::app::SearchTrigger::DoubleCtrl);
            state.opening_foreground = nullptr;
            attempt = state.RequestForeground(request);
            check(emulated_search_pulses == 0 && !attempt.retried, "foreground changed since opening prevents injection");
            request = prepare(pulse::app::SearchTrigger::DoubleCtrl);
            search_after_direct = [&] { emulated_search_foreground = nullptr; };
            attempt = state.RequestForeground(request);
            check(emulated_search_pulses == 0 && !attempt.retried, "foreground changed during direct request prevents injection");
            request = prepare(pulse::app::SearchTrigger::DoubleCtrl);
            state.Hide();
            attempt = state.RequestForeground(request);
            check(emulated_search_pulses == 0 && emulated_search_requests == 0 && !attempt.retried,
                "stale generation never starts a foreground request");
            request = prepare(pulse::app::SearchTrigger::DoubleCtrl);
            search_after_direct = [&] { state.Hide(); };
            attempt = state.RequestForeground(request);
            check(emulated_search_pulses == 0 && !attempt.retried, "generation cancellation prevents injection");
            request = prepare(pulse::app::SearchTrigger::DoubleCtrl);
            search_after_pulse = [&] { state.Hide(); };
            attempt = state.RequestForeground(request);
            check(emulated_search_pulses == 1 && emulated_search_requests == 1 && !attempt.retried,
                "generation cancellation after injection prevents retry");
            request = prepare(pulse::app::SearchTrigger::DoubleCtrl);
            search_after_pulse = [&] { emulated_search_foreground = nullptr; };
            attempt = state.RequestForeground(request);
            check(emulated_search_pulses == 1 && emulated_search_requests == 1 && !attempt.retried,
                "foreground switch after injection prevents retry");
            request = prepare(pulse::app::SearchTrigger::DoubleCtrl);
            search_after_pulse = [&] { search_held_input = VK_LBUTTON; };
            attempt = state.RequestForeground(request);
            check(emulated_search_pulses == 1 && emulated_search_requests == 1 && !attempt.retried,
                "input pressed after injection prevents retry");
            search_after_direct = search_after_pulse = {};
            emulate_search_activation = false;
        }
    };
    search_activation_other = CreateWindowExW(0, L"STATIC", L"Hidden activation fixture", WS_POPUP,
        -30000, -30000, 20, 20, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    reject_search_activation = true;
    {
        pulse::GlobalSearchWindowTestPeer::State state;
        run(state);
    }
    reject_search_activation = false;
    DestroyWindow(search_activation_other); search_activation_other = nullptr;
    CoUninitialize();
    std::cout << "RESULT " << (failures ? "FAIL" : "PASS") << " activation-reentry failures=" << failures << '\n';
    return failures ? 1 : 0;
}
