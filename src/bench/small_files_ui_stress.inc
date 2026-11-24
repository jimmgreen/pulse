#include "../app/app_commands.h"
#include "../app/app_navigation.h"
#include "../app/app_ops_ui.h"
#include "../app/app_state.h"
#include <filesystem>
#include <fstream>
#include <functional>

namespace {
bool PumpUntil(const std::function<bool()>& done, ULONGLONG timeout);

HWND StressOperationWindow(HWND owner, const wchar_t* class_name = L"PulseFileOperationWindow") {
    struct Search { HWND owner; const wchar_t* class_name; HWND found = nullptr; } search{owner, class_name};
    EnumThreadWindows(GetCurrentThreadId(), [](HWND window, LPARAM parameter) -> BOOL {
        auto& search = *reinterpret_cast<Search*>(parameter);
        wchar_t name[96]{};
        DWORD process = 0;
        GetWindowThreadProcessId(window, &process);
        if (process == GetCurrentProcessId() && GetWindow(window, GW_OWNER) == search.owner &&
            GetClassNameW(window, name, ARRAYSIZE(name)) && !wcscmp(name, search.class_name)) {
            search.found = window;
            return FALSE;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&search));
    return search.found;
}
}

int RunSmallFilesUiStress(pulse::AppState& state, const std::filesystem::path& private_root,
                         uint64_t heavy_task, std::ofstream& report) {
    using namespace pulse;
    int failures = 0;
    const auto check = [&](bool ok, const char* label) {
        report << (ok ? "[PASS] " : "[FAIL] ") << "UI_STRESS " << label << '\n';
        if (!ok) ++failures;
        report.flush();
        return ok;
    };
    if (!check(state.isolatedTest && !state.appPrefs.persist && !state.places.persist, "isolated nonpersistent application state")) return failures;
    const auto a = fs::NormalizePath((private_root / L"ui-a").wstring());
    const auto b = fs::NormalizePath((private_root / L"ui-b").wstring());
    if (!check(std::filesystem::is_directory(a) && std::filesystem::is_directory(b), "private fixture directories exist")) return failures;
    const auto original_count = state.window_tabs.items.size();
    const auto original_active = state.window_tabs.active;
    const bool copy_enabled = ui::Compositor::FrameCopyEnabled();
    const auto initial_serial = state.compositor.FrameCopySerial();
    const auto started = GetTickCount64();
    const auto active_heavy = [&] {
        const auto status = state.ops.QueryTask(heavy_task);
        return status && status->active;
    };
    check(active_heavy(), "bulk task active at stress entry");
    const auto loaded = [&](const std::wstring& path) {
        const auto* tab = ActiveTab(state);
        return tab && tab->current_path == path && !tab->loading && tab->snapshot && tab->snapshot->size() == 64;
    };
    const auto render = [&] {
        InvalidateRect(state.hwnd, nullptr, FALSE);
        return PumpUntil([&] { return !GetUpdateRect(state.hwnd, nullptr, FALSE); }, 5000);
    };
    OpenTabAt(state, a);
    const auto first = state.window_tabs.active;
    check(PumpUntil([&] { return loaded(a); }, 15000), "first real tab loaded 64 entries");
    OpenTabAt(state, b);
    const auto second = state.window_tabs.active;
    check(PumpUntil([&] { return loaded(b); }, 15000), "second real tab loaded 64 entries");
    unsigned overlapping_rounds = 0;
    for (unsigned round = 0; round < 6; ++round) {
        const auto round_started = GetTickCount64();
        if (active_heavy()) ++overlapping_rounds;
        BOOL client_animation = FALSE;
        const bool animation_known = SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &client_animation, 0) != FALSE;
        const auto animation_evidence = [&](const char* action, ULONGLONG elapsed, bool converged,
                                            bool flight_seen, bool foreground_before, HWND operation) {
            report << "UI_ANIMATION round=" << round << " action=" << action << " elapsed_ms=" << elapsed
                   << " converged=" << converged << " flight_observed=" << flight_seen
                   << " system_client_animation_known=" << animation_known << " system_client_animation=" << client_animation
                   << " main_foreground_before=" << foreground_before
                   << " main_foreground_after=" << (GetForegroundWindow() == state.hwnd)
                   << " operation_foreground=" << (operation && GetForegroundWindow() == operation)
                   << " main_visible=" << IsWindowVisible(state.hwnd) << " main_iconic=" << IsIconic(state.hwnd)
                   << " operation_visible=" << (operation && IsWindowVisible(operation))
                   << " occlusion_not_measured=1 smoothness_not_measured=1\n";
            report.flush();
        };
        const bool restore_foreground = GetForegroundWindow() == state.hwnd;
        const auto restore_started = GetTickCount64();
        PinAndShowOperationWindow(state);
        HWND operation = StressOperationWindow(state.hwnd);
        bool restore_flight = StressOperationWindow(state.hwnd, L"PulseOperationFlight") != nullptr;
        const bool restored = operation && PumpUntil([&] {
            const bool flight = StressOperationWindow(state.hwnd, L"PulseOperationFlight") != nullptr;
            restore_flight = restore_flight || flight;
            return IsWindowVisible(operation) != FALSE && !flight;
        }, 5000);
        const auto restore_elapsed = GetTickCount64() - restore_started;
        animation_evidence("restore", restore_elapsed, restored, restore_flight, restore_foreground, operation);
        check(restored && restore_elapsed <= 1200, "progress restore converged within 1200 ms");
        if (animation_known && client_animation)
            check(restore_flight, "enabled restore animation flight layer actually observed");
        if (operation) {
            const bool minimize_foreground = GetForegroundWindow() == state.hwnd;
            const auto minimize_started = GetTickCount64();
            SendMessageW(operation, WM_SYSCOMMAND, SC_MINIMIZE, 0);
            bool minimize_flight = StressOperationWindow(state.hwnd, L"PulseOperationFlight") != nullptr;
            const bool minimized = PumpUntil([&] {
                const bool flight = StressOperationWindow(state.hwnd, L"PulseOperationFlight") != nullptr;
                minimize_flight = minimize_flight || flight;
                return !IsWindowVisible(operation) && !flight;
            }, 5000);
            const auto minimize_elapsed = GetTickCount64() - minimize_started;
            animation_evidence("minimize", minimize_elapsed, minimized, minimize_flight, minimize_foreground, operation);
            check(minimized && minimize_elapsed <= 1200, "SC_MINIMIZE flight converged within 1200 ms");
            if (animation_known && client_animation)
                check(minimize_flight, "enabled minimize animation flight layer actually observed");
        }
        // Rapid alternation uses application commands, not a second model of tab state.
        for (unsigned switch_index = 0; switch_index < 8; ++switch_index) {
            SwitchTab(state, switch_index % 2 ? second : first);
            ChooseViewMode(state, switch_index % 2 ? ui::ViewMode::Details : ui::ViewMode::MediumIcons);
        }
        check(PumpUntil([&] { return loaded(b) && ActiveTab(state)->view_mode == ui::ViewMode::Details; }, 5000),
              "rapid tabs and view changes converge to expected path mode and snapshot");
        check(render(), "main paint invalidation serviced after rapid view changes");
        report << "UI_STRESS round=" << round << " elapsed_ms=" << GetTickCount64() - round_started
               << " main_frame_serial=" << state.compositor.FrameCopySerial() << " heavy_active=" << active_heavy() << '\n';
    }
    ApplyLayoutPreset(state, app::LayoutPreset::TwoVertical);
    auto* layout = state.window_tabs.Active();
    auto visible = layout->VisiblePanes();
    if (check(visible.size() == 2, "production two-pane layout created")) {
        FocusPane(state, visible[0]);
        NavigateTo(state, a);
        check(PumpUntil([&] { return loaded(a); }, 15000), "left pane fixture loaded");
        FocusPane(state, visible[1]);
        NavigateTo(state, b);
        check(PumpUntil([&] { return loaded(b); }, 15000), "right pane fixture loaded");
        for (unsigned i = 0; i < 12; ++i) FocusPane(state, visible[i % 2]);
        check(loaded(b) && state.pane == visible[1], "rapid pane focus converges to right fixture");
        check(render(), "two-pane paint invalidation serviced with nonempty fixture models");
    }
    ApplyLayoutPreset(state, app::LayoutPreset::Single);
    check(state.window_tabs.Active()->VisiblePanes().size() == 1, "single-pane layout restored");
    // Both created tabs are at the tail, so closing the latter preserves the first index.
    CloseLayoutTab(state, second);
    CloseLayoutTab(state, first);
    if (original_active < state.window_tabs.items.size()) SwitchTab(state, original_active);
    check(state.window_tabs.items.size() == original_count && state.window_tabs.active == original_active,
          "created tabs closed and original active tab restored");
    const auto operation = StressOperationWindow(state.hwnd);
    if (operation && IsWindowVisible(operation)) SendMessageW(operation, WM_SYSCOMMAND, SC_MINIMIZE, 0);
    check(PumpUntil([&] { return (!operation || !IsWindowVisible(operation)) && !StressOperationWindow(state.hwnd, L"PulseOperationFlight"); }, 5000), "no visible progress-window residue");
    check(render(), "restored original tab paint invalidation serviced");
    check(overlapping_rounds > 0, "interaction rounds overlapped real bulk work");
    report << "UI_STRESS elapsed_ms=" << GetTickCount64() - started
           << " existing_frame_copy_enabled=" << copy_enabled
           << " existing_main_frame_copy_delta=" << state.compositor.FrameCopySerial() - initial_serial
           << " overlapping_rounds=" << overlapping_rounds
           << " animation_assessment=convergence_only_not_smoothness failures=" << failures << '\n';
    report.flush();
    return failures;
}


