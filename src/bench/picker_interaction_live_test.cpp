#include "file_picker_dialog_test.cpp"
#include "picker_interaction_test_api.h"
#include "../ui/view_zoom.h"
#include <windowsx.h>

int wmain() {
    using namespace pulse::ui;
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return 2;
    pulse::compat::EnableDpiAwareness();
    pulse::l10n::Initialize(GetModuleHandleW(nullptr), L"en-US");
    POINT original_cursor{}; GetCursorPos(&original_cursor);
    int failures = 0;
    const auto check = [&](bool pass, const char* label) {
        std::printf("[%s] %s\n", pass ? "PASS" : "FAIL", label);
        std::fflush(stdout);
        if (!pass) ++failures;
        return pass;
    };
    int remainder = 0; ULONGLONG tick = 0;
    check(!ConsumeIconZoomWheel(60, 1000, remainder, tick) &&
        ConsumeIconZoomWheel(60, 1100, remainder, tick) == 1, "shared touchpad fractions accumulate");
    check(ConsumeIconZoomWheel(1200, 1200, remainder, tick) == 1, "shared fast wheel changes only one size");
    check(ResolveIconZoom(ViewMode::MediumIcons, 32, ViewMode::LargeIcons, 96, -1, 1).mode == ViewMode::Details,
        "shared minimum zoom returns to details");
    const auto restored = ResolveIconZoom(ViewMode::Details, 0, ViewMode::LargeIcons, 110, 1, 1);
    check(restored.mode == ViewMode::LargeIcons && restored.dip == 110, "shared zoom restores last icon size");
    {
        Fixture fixture(true);
        for (const auto mode : {PickerMode::File, PickerMode::Save}) {
            FolderPickerSpec spec; spec.mode = mode; spec.initial_path = fixture.root.wstring();
            spec.persist_layout = false; spec.hover_preview_delay_ms = 300;
            std::printf("[CASE] %s\n", mode == PickerMode::File ? "open" : "save");
            auto outcome = Run(spec, [&](HWND hwnd, const std::atomic<bool>& done) {
                PickerInteractionTest state;
                const auto query = [&](bool folder = false) {
                    state = {}; state.folder = folder;
                    return Send(hwnd, kPickerInteractionTestMessage, 0, reinterpret_cast<LPARAM>(&state));
                };
                if (!Until([&] { return query() && state.ready; }, done)) return false;
                const auto key = [&](UINT value, int edit = 0) {
                    PickerInteractionTest request;
                    request.key = value; request.ctrl = request.shift = true; request.edit_id = edit;
                    return Send(hwnd, kPickerInteractionTestMessage, 0, reinterpret_cast<LPARAM>(&request));
                };
                const auto hover = [&] {
                    SetForegroundWindow(hwnd);
                    SetCursorPos(state.screen_item.x, state.screen_item.y);
                    return Send(hwnd, WM_MOUSEMOVE, 0, MAKELPARAM(state.item.x, state.item.y));
                };
                bool all_views = true;
                for (int i = 0; i < 8; ++i) {
                    all_views = key('1' + i) && query() && state.mode == i && all_views;
                }
                check(all_views, "Ctrl+Shift+1..8 reaches every view");
                check(key('3', kFilenameId) && query() && state.mode == 2,
                    "view shortcut routes from filename edit");
                check(key('2', kSearchId) && query() && state.mode == 1,
                    "view shortcut routes from search edit");
                const float before = state.dip;
                Send(hwnd, WM_MOUSEWHEEL, MAKEWPARAM(MK_CONTROL, 60), MAKELPARAM(state.screen_item.x, state.screen_item.y));
                query();
                check(state.dip == before, "half wheel does not resize early");
                Send(hwnd, WM_MOUSEWHEEL, MAKEWPARAM(MK_CONTROL, 60), MAKELPARAM(state.screen_item.x, state.screen_item.y));
                query();
                check(state.dip > before, "Ctrl+wheel increases actual picker icon size");
                Send(GetDlgItem(hwnd, kFilenameId), WM_MOUSEWHEEL, MAKEWPARAM(MK_CONTROL, -120),
                    MAKELPARAM(state.screen_item.x, state.screen_item.y));
                query();
                check(state.dip < before * 1.2f, "wheel routes from focused edit to picker");
                key('6');
                std::this_thread::sleep_for(std::chrono::milliseconds(350));
                query();
                hover();
                const bool shown = Until([&] { return query() && state.visible && state.card.right > state.card.left; }, done);
                check(shown, "native hover timer opens shared file information card");
                if (!shown) return false;
                check(shown && state.card.bottom <= state.footer.top, "hover card stays above filename and confirmation area");
                if (shown) {
                    const auto shot = std::filesystem::absolute(std::filesystem::path(L"bench_data") /
                        (std::wstring(mode == PickerMode::File ? L"picker-hover-open" : L"picker-hover-save") +
                         (GetEnvironmentVariableW(L"PULSE_PICKER_TEST_DARK", nullptr, 0) ? L"-dark.png" : L".png"))).wstring();
                    SetPropW(hwnd, L"Pulse.PickerTestSnapshot", reinterpret_cast<HANDLE>(const_cast<wchar_t*>(shot.c_str())));
                    Send(hwnd, WM_PAINT);
                    RemovePropW(hwnd, L"Pulse.PickerTestSnapshot");
                }
                Send(hwnd, WM_ACTIVATE, WA_INACTIVE); query();
                check(!state.visible, "deactivation dismisses even a settled card");
                hover();
                check(Until([&] { return query() && state.visible; }, done), "hover reopens after pointer returns");
                RECT bounds{}; GetWindowRect(hwnd, &bounds);
                SetWindowPos(hwnd, nullptr, 0, 0, bounds.right - bounds.left + 24, bounds.bottom - bounds.top,
                    SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
                query();
                check(!state.visible, "layout change cancels stale hover anchor");
                Send(hwnd, WM_MOUSELEAVE); query();
                check(!state.visible, "pointer leave dismisses shared card");
                query(true);
                hover();
                check(Until([&] { return query(true) && state.visible && state.listing_ready; }, done),
                    "folder hover reads children asynchronously");
                key('3'); query();
                check(!state.visible, "view switch cancels old hover candidate");
                std::this_thread::sleep_for(std::chrono::milliseconds(400));
                query();
                check(!state.visible, "late folder response cannot reopen dismissed card");
                Send(hwnd, WM_CLOSE);
                return true;
            });
            check(outcome.driven && outcome.status == FilePickerStatus::Cancelled, "picker cancels cleanly after hover and zoom");
        }
    }
    SetCursorPos(original_cursor.x, original_cursor.y);
    CoUninitialize();
    return failures ? 1 : 0;
}
