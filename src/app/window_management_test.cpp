#include "window_management_test.h"
#include "app_internal.h"
#include "explorer_shortcuts.h"
#include "keymap_input.h"
#include "open_routing.h"
#include "external_open_dispatch.h"
#include "window_arrange_ui.h"
#include "window_factory.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <objbase.h>

namespace pulse {
namespace {
struct Report {
    FILE *file = nullptr;
    int failed = 0;
    void Check(bool ok, const char *name) {
        std::fprintf(file, "[%s] %s\n", ok ? "PASS" : "FAIL", name);
        if (!ok)
            ++failed;
        std::fflush(file);
    }
    void Skip(const char *name) {
        std::fprintf(file, "[SKIP] %s: work area cannot fit minimum window sizes\n", name);
    }
};
RECT Bounds(HWND hwnd) {
    RECT r{};
    GetWindowRect(hwnd, &r);
    return r;
}
bool Same(RECT a, RECT b) {
    return EqualRect(&a, &b) != FALSE;
}
std::vector<HWND> Windows(AppState &s) {
    std::vector<HWND> result;
    s.core.windows.ForEach([&](HWND hwnd, AppState &) { result.push_back(hwnd); });
    return result;
}
void Undo(AppState &s) {
    UndoWindowArrange(s, WindowArrangeGeneration());
}
} // namespace
int RunWindowManagementTest(AppState &original, const std::wstring &report_path) {
    Report log;
    if (_wfopen_s(&log.file, report_path.c_str(), L"wb") != 0 || !log.file)
        return 2;
    if (!original.isolatedTest || !GetEnvironmentVariableW(L"PULSE_TEST_DATA_DIR", nullptr, 0)) {
        log.Check(false, "requires isolated test instance and PULSE_TEST_DATA_DIR");
        std::fclose(log.file);
        return 2;
    }
    wchar_t selected[2]{};
    const bool external_only = GetEnvironmentVariableW(L"PULSE_TEST_EXTERNAL_OPEN_ONLY", selected, ARRAYSIZE(selected)) == 1 && selected[0] == L'1';
    wchar_t inspect_value[2]{};
    const bool notice_inspect = GetEnvironmentVariableW(L"PULSE_TEST_EXTERNAL_OPEN_NOTICE_INSPECT", inspect_value,
        ARRAYSIZE(inspect_value)) == 1 && inspect_value[0] == L'1';
    std::filesystem::path inspect_directory;
    if (notice_inspect) {
        wchar_t directory[32768]{};
        const DWORD length = GetEnvironmentVariableW(L"PULSE_TEST_DATA_DIR", directory, ARRAYSIZE(directory));
        const DWORD attributes = length && length < ARRAYSIZE(directory) ? GetFileAttributesW(directory) : INVALID_FILE_ATTRIBUTES;
        inspect_directory = directory;
        if (!external_only || !length || length >= ARRAYSIZE(directory) || !inspect_directory.is_absolute() ||
            attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY)) {
            log.Check(false, "notice inspection requires external-only and an existing absolute isolated data directory");
            std::fclose(log.file); return 2;
        }
    }
    std::fprintf(log.file, "Mode: %s\n", external_only ? "external-open-only" : "full-window-management");
    if (external_only) std::fprintf(log.file, "[SKIP] keymap/menu/arrangement cases excluded by explicit external-open-only filter\n");
    if (!external_only) {
        app::Keymap keys;
        const app::KeyChord chord{static_cast<uint8_t>(app::kKeyCtrl | app::kKeyAlt), VK_F8};
        keys.SetBindings(app::KeyAction::ArrangeColumns, {chord});
        log.Check(app::RouteKey(keys, chord, 0).action == app::KeyAction::ArrangeColumns,
                  "unbound arrangement action routes after assignment");
        log.Check(app::ExplorerShortcut(L'N', true, false, false).action == app::ExplorerKeyAction::NewWindow,
                  "Ctrl+N opens a new window");
        ui::FluentMenuItem heading;
        heading.heading = true;
        ui::FluentMenuItem row;
        ui::FluentMenuSwatch tile;
        tile.command = 1;
        tile.label = L"Layout";
        row.quick_swatches.push_back(tile);
        ui::FluentMenuModel model;
        model.SetItems({heading, row});
        model.Layout(nullptr, 1.0f);
        log.Check(model.RowHeightPx(1) == 72.0f && model.FirstEnabled() == 1,
                  "layout tiles have space and keyboard skips heading");
    }
    const auto before_windows = Windows(original);
    if (before_windows.size() != 1) {
        log.Check(false, "fixture requires one initial isolated window");
        std::fclose(log.file);
        return 2;
    }
    const auto prefs = original.appPrefs;
    WINDOWPLACEMENT original_placement{sizeof(original_placement)};
    GetWindowPlacement(original.hwnd, &original_placement);
    const bool original_visible = IsWindowVisible(original.hwnd) != FALSE;
    original.appPrefs.persist = false;
    original.appPrefs.arrange_include_minimized = false;
    original.appPrefs.arrange_gap = 0;
    SetWindowArrangeAnimations(false);
    ShowWindow(original.hwnd, SW_HIDE);
    MONITORINFO monitor{sizeof(monitor)};
    const HMONITOR target_monitor = MonitorFromWindow(original.hwnd, MONITOR_DEFAULTTONEAREST);
    GetMonitorInfoW(target_monitor, &monitor);
    const auto make_window = [&]() {
        WindowInit init;
        init.monitor = target_monitor;
        init.rect = RECT{monitor.rcWork.left + 20, monitor.rcWork.top + 20, monitor.rcWork.left + 720,
                         monitor.rcWork.top + 560};
        HWND hwnd = OpenPulseWindow(original, init);
        if (auto *state = original.core.windows.Lookup(hwnd)) {
            state->isolatedTest = true;
            // WM_CREATE establishes the target monitor DPI. A 540px fixture is
            // below the 630px minimum at 150%, and SetWindowPlacement correctly
            // grows that invalid baseline when undo restores it.
            MINMAXINFO minimum{};
            SendMessageW(hwnd, WM_GETMINMAXINFO, 0, reinterpret_cast<LPARAM>(&minimum));
            const RECT bounds = Bounds(hwnd);
            SetWindowPos(hwnd, nullptr, bounds.left, bounds.top,
                         (std::max)(bounds.right - bounds.left, minimum.ptMinTrackSize.x),
                         (std::max)(bounds.bottom - bounds.top, minimum.ptMinTrackSize.y),
                         SWP_NOZORDER | SWP_NOACTIVATE);
        }
        return hwnd;
    };
    const HWND first = make_window();
    AppState *fixture = original.core.windows.Lookup(first);
    log.Check(fixture != nullptr, "create isolated fixture window");
    if (fixture) {
        {
            const auto routing_prefs = original.appPrefs;
            const auto base_count = original.core.windows.Count();
            const auto base_tabs = fixture->window_tabs.items.size();
            original.core.windows.Touch(first);
            original.appPrefs.open_pulse_icon = 2;
            const auto activated = OpenExternalFolder(original, L"");
            log.Check(activated.status == ExternalOpenStatus::Dispatched && (activated.target ? activated.target->hwnd : nullptr) == first && original.core.windows.Count() == base_count &&
                          fixture->window_tabs.items.size() == base_tabs,
                      "icon activate selects MRU without adding window or tab");
            const HWND peer = make_window();
            if (auto *target = original.core.windows.Lookup(peer)) {
                original.appPrefs.open_pulse_icon = 1;
                original.appPrefs.new_window_shows = 1;
                for (const int show : {SW_HIDE, SW_MINIMIZE}) {
                    ShowWindow(peer, show);
                    original.core.windows.Touch(peer);
                    log.Check(original.core.windows.MostRecent() == target,
                              "launch fixture hidden or minimized peer is MRU before routing");
                    const auto old_tabs = target->window_tabs.items.size();
                    const HWND before_mru = original.core.windows.MostRecentHwnd();
                    const int before_iconic = IsIconic(peer);
                    const int before_visible = IsWindowVisible(peer);
                    const auto opened = OpenExternalFolder(*fixture, L"");
                    std::fprintf(log.file,
                        "[DIAG] launch show=%d peer=%p mru_before=%p actual=%p tabs_before=%zu tabs_after=%zu "
                        "iconic_before=%d iconic_after=%d visible_before=%d visible_after=%d rename=%d enabled=%d capture=%p\n",
                        show, static_cast<void*>(peer), static_cast<void*>(before_mru), static_cast<void*>((opened.target ? opened.target->hwnd : nullptr)),
                        old_tabs, target->window_tabs.items.size(), before_iconic, IsIconic(peer), before_visible,
                        IsWindowVisible(peer), target->renameIndex, IsWindowEnabled(peer), static_cast<void*>(GetCapture()));
                    log.Check(opened.status == ExternalOpenStatus::Dispatched && (opened.target ? opened.target->hwnd : nullptr) == peer && target->window_tabs.items.size() == old_tabs + 1 &&
                                  IsWindowVisible(peer) && !IsIconic(peer),
                              "icon new tab restores hidden or minimized MRU target");
                }
                DestroyWindow(peer);
            } else
                log.Check(false, "create launch routing MRU peer");
            wchar_t test_directory[32768]{};
            GetEnvironmentVariableW(L"PULSE_TEST_DATA_DIR", test_directory, ARRAYSIZE(test_directory));
            OpenTabAt(*fixture, test_directory);
            const auto *current = ActiveTab(*fixture);
            const std::wstring current_path = current ? current->current_path : std::wstring();
            original.appPrefs.open_pulse_icon = 0;
            original.appPrefs.new_window_shows = 2;
            original.core.windows.Touch(first);
            const auto current_window = OpenExternalFolder(*fixture, L"");
            const HWND current_hwnd = (current_window.target ? current_window.target->hwnd : nullptr);
            const auto *current_folder = current_window.target ? ActiveTab(*current_window.target) : nullptr;
            log.Check(current_window.status == ExternalOpenStatus::Dispatched && current_hwnd && current_hwnd != first && original.core.windows.Count() == base_count + 1 && current_folder &&
                          current_folder->current_path == current_path,
                      "icon new window uses current folder preference");
            if (current_hwnd != first && current_hwnd != original.hwnd)
                DestroyWindow(current_hwnd);
            original.appPrefs.new_window_shows = 1;
            original.core.windows.Touch(first);
            const auto pc_window = OpenExternalFolder(*fixture, L"");
            const HWND pc_hwnd = (pc_window.target ? pc_window.target->hwnd : nullptr);
            const auto *pc_folder = pc_window.target ? ActiveTab(*pc_window.target) : nullptr;
            log.Check(pc_window.status == ExternalOpenStatus::Dispatched && pc_hwnd && pc_hwnd != first && pc_folder && pc_folder->current_path.empty(),
                      "icon new window uses This PC preference");
            if (pc_hwnd != first && pc_hwnd != original.hwnd)
                DestroyWindow(pc_hwnd);
            original.appPrefs.open_pulse_icon = 2;
            original.appPrefs.new_window_shows = 2;
            original.core.windows.Touch(first);
            const auto explicit_pc = OpenExternalFolder(*fixture, L"pulse:this-pc");
            const HWND explicit_hwnd = (explicit_pc.target ? explicit_pc.target->hwnd : nullptr);
            const auto *explicit_folder = explicit_pc.target ? ActiveTab(*explicit_pc.target) : nullptr;
            log.Check(explicit_pc.status == ExternalOpenStatus::Dispatched && explicit_hwnd && explicit_hwnd != first && explicit_folder && explicit_folder->current_path.empty(),
                      "explicit This PC overrides icon activation and current folder preference");
            if (explicit_hwnd != first && explicit_hwnd != original.hwnd)
                DestroyWindow(explicit_hwnd);
            original.appPrefs.open_pulse_icon = 1;
            original.core.windows.Touch(first);
            const auto busy_count = original.core.windows.Count();
            const auto busy_tabs = fixture->window_tabs.items.size();
            const auto original_tabs = original.window_tabs.items.size();
            const bool first_enabled = IsWindowEnabled(first) != FALSE;
            const bool original_enabled = IsWindowEnabled(original.hwnd) != FALSE;
            HWND modal = CreateWindowW(L"STATIC", L"Pulse external-open test modal", WS_POPUP | WS_CAPTION | WS_VISIBLE,
                monitor.rcWork.left + 60, monitor.rcWork.top + 60, 300, 100, first, nullptr, GetModuleHandleW(nullptr), nullptr);
            log.Check(modal != nullptr, "create owned modal fixture");
            if (modal) {
                EnableWindow(first, FALSE); EnableWindow(original.hwnd, FALSE);
                const auto busy = OpenExternalFolder(*fixture, L"");
                log.Check(busy.status == ExternalOpenStatus::Deferred && !busy.target &&
                    original.core.windows.Count() == busy_count && fixture->window_tabs.items.size() == busy_tabs &&
                    original.window_tabs.items.size() == original_tabs && IsWindowVisible(modal) && IsWindowEnabled(modal) &&
                    !IsWindowEnabled(first) && !IsWindowEnabled(original.hwnd) && GetWindow(modal, GW_OWNER) == first,
                    "all busy launch defers without new window tab or interruption of owned modal");
                using Coordinator = app::SingleInstanceCoordinator;
                Coordinator::OpenRequest request;
                GUID id{};
                const HRESULT generated = CoCreateGuid(&id);
                if (SUCCEEDED(generated)) std::memcpy(request.id.data(), &id, sizeof(id));
                request.deadline = GetTickCount64() + 10000;
                request.path = L"";
                request.delivery = std::make_shared<std::atomic<Coordinator::OpenDeliveryState>>(Coordinator::OpenDeliveryState::Pending);
                const auto submitted = SUCCEEDED(generated) ? original.single_instance.SubmitOpenRequest(request,
                    GetTickCount64(), original.core.host.Hwnd(), WM_OPEN_EXTERNAL) : Coordinator::OpenSubmission::Invalid;
                log.Check(submitted == Coordinator::OpenSubmission::Accepted, "owned dispatcher accepts one bounded request while all windows busy");
                if (submitted == Coordinator::OpenSubmission::Accepted) {
                    DispatchQueuedExternalOpen(original);
                    log.Check(request.delivery->load() == Coordinator::OpenDeliveryState::Queued &&
                        original.core.windows.Count() == busy_count && fixture->window_tabs.items.size() == busy_tabs &&
                        original.window_tabs.items.size() == original_tabs && IsWindowVisible(modal) && IsWindowEnabled(modal),
                        "real dispatcher retains busy request without completing or disrupting modal");
                }
                DestroyWindow(modal);
                EnableWindow(first, first_enabled); EnableWindow(original.hwnd, original_enabled);
                original.core.windows.Touch(first);
                if (submitted == Coordinator::OpenSubmission::Accepted) {
                    DispatchQueuedExternalOpen(original);
                    log.Check(request.delivery->load() == Coordinator::OpenDeliveryState::Dispatched &&
                        original.core.windows.Count() == busy_count && fixture->window_tabs.items.size() == busy_tabs + 1,
                        "real dispatcher resumes exactly one tab after modal releases owner");
                    const auto duplicate = original.single_instance.SubmitOpenRequest(request, GetTickCount64(),
                        original.core.host.Hwnd(), WM_OPEN_EXTERNAL);
                    DispatchQueuedExternalOpen(original);
                    log.Check(duplicate == Coordinator::OpenSubmission::Duplicate &&
                        original.core.windows.Count() == busy_count && fixture->window_tabs.items.size() == busy_tabs + 1,
                        "same owned request and extra wake do not dispatch a second time");
                }
            }
            const auto explicit_before = original.core.windows.Count();
            const auto explicit_one = OpenExternalFolder(*fixture, L"pulse:new-window");
            const auto explicit_two = OpenExternalFolder(*fixture, L"pulse:new-window");
            const HWND explicit_one_hwnd = explicit_one.target ? explicit_one.target->hwnd : nullptr;
            const HWND explicit_two_hwnd = explicit_two.target ? explicit_two.target->hwnd : nullptr;
            log.Check(explicit_one.status == ExternalOpenStatus::Dispatched && explicit_two.status == ExternalOpenStatus::Dispatched &&
                explicit_one_hwnd && explicit_two_hwnd && explicit_one_hwnd != explicit_two_hwnd &&
                explicit_one_hwnd != first && explicit_two_hwnd != first && original.core.windows.Count() == explicit_before + 2,
                "explicit new-window requests remain distinct rather than being merged");
            for (HWND opened : {explicit_one_hwnd, explicit_two_hwnd})
                if (opened && opened != first && opened != original.hwnd && IsWindow(opened)) DestroyWindow(opened);
            while (fixture->window_tabs.items.size() > base_tabs)
                CloseLayoutTab(*fixture, fixture->window_tabs.items.size() - 1);
            original.appPrefs = routing_prefs;
            ShowWindow(original.hwnd, SW_HIDE);
            ShowWindow(first, SW_SHOWNOACTIVATE);
            original.core.windows.Touch(first);
            log.Check(original.core.windows.Count() == base_count && fixture->window_tabs.items.size() == base_tabs,
                      "launch routing fixtures restore window and tab counts");
        }
        if (notice_inspect) {
            // Visual fixture inputs, not synthetic queue-delivery success. The
            // normal Tick path selects production localized text and draws Toast.
            const auto deadline = GetTickCount64() + 60000;
            bool seeded = false, captured = false, painted = false, quit = false;
            uint64_t visible_since = 0;
            const auto screenshot = inspect_directory / (L"external-open-notice-" + std::to_wstring(GetCurrentProcessId()) +
                L"-" + std::to_wstring(GetTickCount64()) + L".png");
            SetWindowTextW(first, L"Pulse external-open notice visual inspection");
            ShowWindow(first, SW_SHOW);
            original.core.windows.Touch(first);
            std::fprintf(log.file, "[INSPECT] up to 60 seconds; close only this fixture to finish early; fixture counts timeout=2 failure=1\n");
            std::fflush(log.file);
            while (GetTickCount64() < deadline && IsWindow(first) && !quit) {
                // Lookup afresh: user WM_CLOSE may destroy the AppState.
                auto* current_fixture = original.core.windows.Lookup(first);
                if (!current_fixture) break;
                if (!seeded && !current_fixture->notification_toast.IsVisible()) {
                    original.core.open_failure_notice.TimedOut(false);
                    original.core.open_failure_notice.TimedOut(false);
                    original.core.open_failure_notice.Failed(false);
                    seeded = true;
                    TickExternalOpen(*current_fixture);
                }
                if (seeded && current_fixture->notification_toast.IsVisible() && !visible_since) {
                    visible_since = GetTickCount64();
                    std::fprintf(log.file, "[INSPECT] actual mixed-failure toast visible; dpi_scale=%.3f\n", current_fixture->scale);
                    std::fflush(log.file);
                }
                if (!captured && painted && visible_since && GetTickCount64() - visible_since >= 500 &&
                    current_fixture->notification_toast.IsVisible()) {
                    const auto bounds = current_fixture->notification_toast.Bounds();
                    const bool saved = current_fixture->compositor.SaveSnapshot(screenshot.c_str());
                    captured = true;
                    log.Check(saved, "save actual rendered notice surface (visual review still required)");
                    std::fprintf(log.file, "[INSPECT] bounds=(%.1f,%.1f,%.1f,%.1f); snapshot path recorded below as UTF-16 code units\n",
                        bounds.left, bounds.top, bounds.right, bounds.bottom);
                    for (wchar_t ch : screenshot.wstring()) std::fprintf(log.file, "\\u%04X", static_cast<unsigned>(ch));
                    std::fprintf(log.file, "\n"); std::fflush(log.file);
                }
                MSG message{};
                unsigned pumped = 0;
                while (pumped++ < 64 && GetTickCount64() < deadline && PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                    if (message.message == WM_QUIT) { PostQuitMessage(static_cast<int>(message.wParam)); quit = true; break; }
                    const bool own_paint = message.hwnd == first && message.message == WM_PAINT;
                    TranslateMessage(&message); DispatchMessageW(&message);
                    if (own_paint && visible_since) painted = true;
                    if (!IsWindow(first)) break;
                }
                if (!quit && IsWindow(first)) MsgWaitForMultipleObjectsEx(0, nullptr, 50, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
            }
            log.Check(seeded && visible_since != 0, "normal TickExternalOpen displayed mixed timeout/failure fixture");
            if (!captured) std::fprintf(log.file, "[INSPECT] no surface captured before close/deadline; not visual acceptance\n");
            std::fflush(log.file);
            fixture = original.core.windows.Lookup(first);
        }
        if (!external_only) {
            const auto positions = [&](app::WindowArrangeLayout layout, size_t count) {
                MINMAXINFO minimum{};
                SendMessageW(first, WM_GETMINMAXINFO, 0, reinterpret_cast<LPARAM>(&minimum));
                const int dpi = static_cast<int>(96.0f * fixture->scale);
                return app::CalculateWindowArrangement(layout, monitor.rcWork, count, 0, minimum.ptMinTrackSize.x,
                                                       minimum.ptMinTrackSize.y, MulDiv(32, dpi, 96));
            };
            const auto destroy_extras = [&]() {
                CleanupWindowArrange();
                for (HWND hwnd : Windows(original))
                    if (hwnd != original.hwnd && hwnd != first)
                        DestroyWindow(hwnd);
            };
            const auto layout = app::WindowArrangeLayout::SideBySide;
            if (!positions(layout, 2).empty()) {
                const auto count = original.core.windows.Count();
                const auto *initial_tab = fixture->window_tabs.Active();
                log.Check(ArrangeWindows(*fixture, layout), "single folder duplicates into second window");
                log.Check(original.core.windows.Count() == count + 1, "copy creates exactly one window");
                Undo(*fixture);
                log.Check(original.core.windows.Count() == count && fixture->window_tabs.Active() == initial_tab,
                          "copy undo closes copy and preserves original tab");
                destroy_extras();
                OpenTabAt(*fixture, L"");
                std::vector<const app::LayoutTab *> tabs;
                for (const auto &tab : fixture->window_tabs.items)
                    tabs.push_back(tab.get());
                log.Check(ArrangeWindows(*fixture, layout), "single window splits existing tabs");
                log.Check(fixture->window_tabs.items.size() == 1, "split moves an existing tab");
                Undo(*fixture);
                bool restored = fixture->window_tabs.items.size() == tabs.size();
                for (size_t i = 0; restored && i < tabs.size(); ++i)
                    restored = fixture->window_tabs.items[i].get() == tabs[i];
                log.Check(restored && original.core.windows.Count() == count, "split undo restores tab identity and order");
                destroy_extras();
            } else
                log.Skip("copy and split undo");
            const HWND second = make_window();
            const HWND third = make_window();
            const std::vector<HWND> order{first, second, third};
            if (second && third) {
                struct Foreign {
                    HWND hwnd = nullptr;
                    RECT rect{};
                } foreign;
                EnumWindows(
                    [](HWND hwnd, LPARAM parameter) -> BOOL {
                        DWORD pid = 0;
                        GetWindowThreadProcessId(hwnd, &pid);
                        if (pid != GetCurrentProcessId() && IsWindowVisible(hwnd) && !IsIconic(hwnd)) {
                            auto &value = *reinterpret_cast<Foreign *>(parameter);
                            value.hwnd = hwnd;
                            GetWindowRect(hwnd, &value.rect);
                            return FALSE;
                        }
                        return TRUE;
                    },
                    reinterpret_cast<LPARAM>(&foreign));
                for (const auto kind : {app::WindowArrangeLayout::SideBySide, app::WindowArrangeLayout::Stacked,
                                        app::WindowArrangeLayout::Columns, app::WindowArrangeLayout::Tile,
                                        app::WindowArrangeLayout::Main, app::WindowArrangeLayout::Cascade}) {
                    std::fprintf(log.file, "Layout: %d\n", static_cast<int>(kind));
                    const auto expected = positions(kind, order.size());
                    if (expected.empty()) {
                        log.Skip("layout");
                        continue;
                    }
                    std::vector<RECT> initial;
                    for (HWND hwnd : order)
                        initial.push_back(Bounds(hwnd));
                    log.Check(ArrangeWindows(*fixture, kind, false, order), "arrange real windows");
                    bool actual_matches = true;
                    for (size_t i = 0; i < order.size(); ++i)
                        actual_matches &= Same(Bounds(order[i]), i < expected.size() ? expected[i] : initial[i]);
                    log.Check(actual_matches, "GetWindowRect matches arrangement algorithm");
                    Undo(*fixture);
                    bool undo_matches = true;
                    for (size_t i = 0; i < order.size(); ++i) {
                        const RECT actual = Bounds(order[i]);
                        const RECT before = initial[i];
                        const bool matches = Same(actual, before);
                        if (!matches) {
                            WINDOWPLACEMENT placement{sizeof(placement)};
                            GetWindowPlacement(order[i], &placement);
                            const RECT normal = placement.rcNormalPosition;
                            std::fprintf(log.file, "Undo window %zu expected=(%ld,%ld,%ld,%ld) actual=(%ld,%ld,%ld,%ld) normal=(%ld,%ld,%ld,%ld) show=%u flags=%u\n",
                                i, before.left, before.top, before.right, before.bottom,
                                actual.left, actual.top, actual.right, actual.bottom,
                                normal.left, normal.top, normal.right, normal.bottom,
                                placement.showCmd, placement.flags);
                        }
                        undo_matches &= matches;
                    }
                    log.Check(undo_matches, "undo restores real window rectangles");
                }
                if (foreign.hwnd && IsWindow(foreign.hwnd))
                    log.Check(original.core.windows.Lookup(foreign.hwnd) == nullptr &&
                                  Same(Bounds(foreign.hwnd), foreign.rect),
                              "foreign process window excluded and unchanged");
                else
                    std::fprintf(log.file, "[SKIP] no stable visible foreign process window\n");
                if (!positions(layout, 2).empty()) {
                    ShowWindow(third, SW_MINIMIZE);
                    log.Check(ArrangeWindows(*fixture, layout, false, order), "arrange excludes minimized window");
                    log.Check(IsIconic(third) != FALSE, "excluded window remains minimized");
                    Undo(*fixture);
                    if (!positions(app::WindowArrangeLayout::Tile, 4).empty()) {
                        // force_include includes the hidden original fixture owner too.
                        log.Check(ArrangeWindows(*fixture, app::WindowArrangeLayout::Tile, false, order, true),
                                  "force include arrangement");
                        log.Check(!IsIconic(third) && IsWindowVisible(third), "force include restores minimized window");
                        Undo(*fixture);
                        log.Check(IsIconic(third) != FALSE && !IsWindowVisible(original.hwnd),
                                  "undo restores minimized and hidden state");
                    } else
                        log.Skip("force include");
                    ShowWindow(third, SW_RESTORE);
                    if (!positions(layout, order.size()).empty()) {
                        ShowWindow(first, SW_MAXIMIZE);
                        const RECT maximized = Bounds(first);
                        log.Check(ArrangeWindows(*fixture, layout, false, order), "arrange maximized window");
                        Undo(*fixture);
                        log.Check(IsZoomed(first) && Same(Bounds(first), maximized),
                                  "undo restores maximized state and rectangle");
                    } else
                        log.Skip("maximized undo");
                }
            } else
                log.Check(false, "create arrangement peers");
        }
    }
    CleanupWindowArrange();
    for (HWND hwnd : Windows(original))
        if (std::find(before_windows.begin(), before_windows.end(), hwnd) == before_windows.end())
            DestroyWindow(hwnd);
    original.appPrefs = prefs;
    SetWindowPlacement(original.hwnd, &original_placement);
    if (!original_visible)
        ShowWindow(original.hwnd, SW_HIDE);
    SetWindowArrangeAnimations(true);
    log.Check(Windows(original).size() == before_windows.size(), "cleanup removes all generated windows");
    std::fprintf(log.file, "Failures: %d\n", log.failed);
    std::fclose(log.file);
    return log.failed ? 1 : 0;
}
} // namespace pulse



