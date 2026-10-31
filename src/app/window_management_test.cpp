#include "window_management_test.h"
#include "app_internal.h"
#include "explorer_shortcuts.h"
#include "keymap_input.h"
#include "open_routing.h"
#include "window_arrange_ui.h"
#include "window_factory.h"
#include <algorithm>
#include <cstdio>

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
    {
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
            AppState &activated = OpenExternalFolder(original, L"");
            log.Check(activated.hwnd == first && original.core.windows.Count() == base_count &&
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
                    AppState &opened = OpenExternalFolder(*fixture, L"");
                    std::fprintf(log.file,
                        "[DIAG] launch show=%d peer=%p mru_before=%p actual=%p tabs_before=%zu tabs_after=%zu "
                        "iconic_before=%d iconic_after=%d visible_before=%d visible_after=%d rename=%d enabled=%d capture=%p\n",
                        show, static_cast<void*>(peer), static_cast<void*>(before_mru), static_cast<void*>(opened.hwnd),
                        old_tabs, target->window_tabs.items.size(), before_iconic, IsIconic(peer), before_visible,
                        IsWindowVisible(peer), target->renameIndex, IsWindowEnabled(peer), static_cast<void*>(GetCapture()));
                    log.Check(opened.hwnd == peer && target->window_tabs.items.size() == old_tabs + 1 &&
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
            AppState &current_window = OpenExternalFolder(*fixture, L"");
            const HWND current_hwnd = current_window.hwnd;
            const auto *current_folder = ActiveTab(current_window);
            log.Check(current_hwnd != first && original.core.windows.Count() == base_count + 1 && current_folder &&
                          current_folder->current_path == current_path,
                      "icon new window uses current folder preference");
            if (current_hwnd != first && current_hwnd != original.hwnd)
                DestroyWindow(current_hwnd);
            original.appPrefs.new_window_shows = 1;
            original.core.windows.Touch(first);
            AppState &pc_window = OpenExternalFolder(*fixture, L"");
            const HWND pc_hwnd = pc_window.hwnd;
            const auto *pc_folder = ActiveTab(pc_window);
            log.Check(pc_hwnd != first && pc_folder && pc_folder->current_path.empty(),
                      "icon new window uses This PC preference");
            if (pc_hwnd != first && pc_hwnd != original.hwnd)
                DestroyWindow(pc_hwnd);
            original.appPrefs.open_pulse_icon = 2;
            original.appPrefs.new_window_shows = 2;
            original.core.windows.Touch(first);
            AppState &explicit_pc = OpenExternalFolder(*fixture, L"pulse:this-pc");
            const HWND explicit_hwnd = explicit_pc.hwnd;
            const auto *explicit_folder = ActiveTab(explicit_pc);
            log.Check(explicit_hwnd != first && explicit_folder && explicit_folder->current_path.empty(),
                      "explicit This PC overrides icon activation and current folder preference");
            if (explicit_hwnd != first && explicit_hwnd != original.hwnd)
                DestroyWindow(explicit_hwnd);
            original.appPrefs.open_pulse_icon = 1;
            original.core.windows.Touch(first);
            const int previous_rename = fixture->renameIndex;
            fixture->renameIndex = 0;
            AppState &busy_window = OpenExternalFolder(*fixture, L"");
            fixture->renameIndex = previous_rename;
            const HWND busy_hwnd = busy_window.hwnd;
            log.Check(busy_hwnd != first, "busy tab target falls back to new window");
            if (busy_hwnd != first && busy_hwnd != original.hwnd)
                DestroyWindow(busy_hwnd);
            while (fixture->window_tabs.items.size() > base_tabs)
                CloseLayoutTab(*fixture, fixture->window_tabs.items.size() - 1);
            original.appPrefs = routing_prefs;
            ShowWindow(original.hwnd, SW_HIDE);
            ShowWindow(first, SW_SHOWNOACTIVATE);
            original.core.windows.Touch(first);
            log.Check(original.core.windows.Count() == base_count && fixture->window_tabs.items.size() == base_tabs,
                      "launch routing fixtures restore window and tab counts");
        }
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



