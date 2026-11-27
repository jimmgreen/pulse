#include "../app/app_internal.h"
#include "../app/app_input.h"
#include "../app/single_instance_coordinator.h"
#include "../ui/ui_renderer.h"
#include <cstdio>
#include <clocale>
#include <filesystem>
#include <functional>
#include <optional>

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int);
void Render(pulse::AppState&);

namespace {
using namespace pulse;
using H = ui::HitTestResult;
int failed = 0, passed = 0, menus = 0;
ULONGLONG started = 0;
std::filesystem::path base, fixture;
void Check(bool value, const char* label) {
    std::printf("[%s] %s\n", value ? "PASS" : "FAIL", label);
    std::fflush(stdout);
    if (value) ++passed; else ++failed;
}
bool Wait(const std::function<bool()>& done, DWORD timeout = 5000) {
    const auto until = GetTickCount64() + timeout;
    do {
        if (done()) return true;
        MSG msg{};
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) return false;
            TranslateMessage(&msg); DispatchMessageW(&msg);
        }
        Sleep(10);
    } while (GetTickCount64() < until);
    return done();
}
void Input(HWND hwnd, UINT message, POINT point) {
    const WPARAM keys = message == WM_LBUTTONDOWN ||
        (message == WM_MOUSEMOVE && GetCapture() == hwnd) ? MK_LBUTTON : 0;
    SendMessageW(hwnd, message, keys, MAKELPARAM(point.x, point.y));
}
void Frame(AppState& s) { Render(s); }
std::optional<POINT> Find(AppState& s, H::Region region, int index = -1,
                          int pane = -1, int sub = -1) {
    const auto vm = BuildVm(s, false);
    const auto bounds = D2D1::RectF(0, 0, static_cast<float>(s.compositor.Width()),
                                  static_cast<float>(s.compositor.Height()));
    for (int y = 4; y < s.compositor.Height(); y += 4)
        for (int x = 4; x < s.compositor.Width(); x += 4) {
            const auto hit = s.renderer.HitTest(vm, bounds, static_cast<float>(x), static_cast<float>(y));
            if (hit.region == region && (index < 0 || hit.index == index) &&
                (pane < 0 || hit.pane_index == pane) && (sub < 0 || hit.sub_index == sub)) return POINT{x, y};
        }
    return {};
}
bool Ready(app::Tab* tab) { return tab && !tab->loading && tab->snapshot && tab->EntryCount() >= 3; }
void Home(AppState& s, int pane) {
    SendMessageW(s.hwnd, WM_CANCELMODE, 0, 0);
    SwitchTab(s, 0);
    while (s.window_tabs.items.size() > 1) CloseLayoutTab(s, s.window_tabs.items.size() - 1);
    FocusPane(s, PaneAtSlot(s, pane));
    auto* tab = ActiveTab(s);
    if (tab && tab->current_path != fs::NormalizePath(fixture.wstring())) NavigateTo(s, fixture.wstring());
    Check(Wait([&] { return Ready(ActiveTab(s)); }), "real fixture directory snapshot is ready");
    if (auto* active = ActiveTab(s)) {
        active->view_mode = ui::ViewMode::Details;
        active->ClearSelection();
    }
    s.hoverRow = -1; s.hoverPaneIndex = -1;
    Frame(s);
}
std::optional<POINT> HoverAction(AppState& s, H::Region action, int pane, int index = 0) {
    const auto row = Find(s, H::Row, index, pane);
    if (!row) return {};
    Input(s.hwnd, WM_MOUSEMOVE, *row); Frame(s);
    auto point = Find(s, action, index, pane);
    if (point) Input(s.hwnd, WM_MOUSEMOVE, *point);
    Frame(s);
    return point;
}
void Gesture(AppState& s, POINT point, int kind) {
    Input(s.hwnd, WM_LBUTTONDOWN, point);
    if (kind == 1 || kind == 2) Input(s.hwnd, WM_MOUSEMOVE, {-20, -20});
    if (kind == 2) Input(s.hwnd, WM_MOUSEMOVE, point);
    if (kind == 3) ReleaseCapture();
    if (kind == 4) SendMessageW(s.hwnd, WM_CANCELMODE, 0, 0);
    if (kind == 5) {
        NavigateTo(s, (fixture / L"B_Beta").wstring());
        Wait([&] { return ActiveTab(s) && !ActiveTab(s)->loading; });
    }
    if (kind == 6) Input(s.hwnd, WM_MOUSEMOVE, point);
    if (kind == 7) SendMessageW(s.hwnd, WM_KEYDOWN, VK_ESCAPE, 0);
    if (kind == 8) SendMessageW(s.hwnd, WM_ACTIVATE, WA_INACTIVE, 0);
    Frame(s);
    Input(s.hwnd, WM_LBUTTONUP, kind == 1 ? POINT{-20, -20} : point);
}
bool Activates(int kind) { return kind == 0 || kind == 2 || kind == 6; }
void CALLBACK CloseMenu(HWND, UINT, UINT_PTR, DWORD) {
    EnumThreadWindows(GetCurrentThreadId(), [](HWND hwnd, LPARAM) -> BOOL {
        wchar_t cls[100]{}; GetClassNameW(hwnd, cls, 100);
        if (wcscmp(cls, L"PulseFluentMenu") == 0 && IsWindowVisible(hwnd)) {
            ++menus; PostMessageW(hwnd, WM_KEYDOWN, VK_ESCAPE, 0);
        }
        return TRUE;
    }, 0);
}
void Exercise(AppState& s) {
    Check(s.isolatedTest && !s.isolatedTestPersist, "real Pulse window uses isolated nonpersistent configuration");
    s.appPrefs.show_hints = false;
    s.renderer.SetRowActions(7);
    SetWindowPos(s.hwnd, nullptr, 20, 20, 1500, 850, SWP_NOZORDER | SWP_NOACTIVATE);
    ApplyLayoutPreset(s, app::LayoutPreset::TwoVertical);
    for (int pane = 0; pane < 2; ++pane) {
        FocusPane(s, PaneAtSlot(s, pane)); NavigateTo(s, fixture.wstring());
        Check(Wait([&] { return Ready(ActiveTab(s)); }), "split pane loads real directory");
    }
    for (int pane = 0; pane < 2; ++pane) for (int kind = 0; kind < 9; ++kind) {
        Home(s, pane);
        auto* tab = ActiveTab(s);
        const auto target = EntryFullPath(*tab, 0);
        if (kind == 0) tab->SelectOnly(0);
        if (kind == 6) tab->SelectIndices({0, 1});
        const auto point = HoverAction(s, H::RowNewTab, pane);
        std::printf("[CASE] new-tab pane=%d gesture=%d\n", pane, kind);
        Check(point.has_value(), "painted row new-tab has a real hit target");
        if (!point) continue;
        const auto before = s.window_tabs.items.size();
        Gesture(s, *point, kind);
        const bool opened = s.window_tabs.items.size() == before + 1 && ActiveTab(s)->current_path == target;
        Check(opened == Activates(kind) && s.window_tabs.items.size() == before + (Activates(kind) ? 1 : 0),
              "row new-tab release activates exactly once or cancels as specified");
        const auto after = s.window_tabs.items.size();
        Input(s.hwnd, WM_LBUTTONUP, *point);
        Check(s.window_tabs.items.size() == after, "extra release never opens another tab");
    }
    for (const auto action : {H::RowMore, H::RowStar}) for (int kind : {0, 1, 2, 3, 5, 6}) {
        Home(s, 1);
        auto* tab = ActiveTab(s);
        if (kind == 6) tab->SelectOnly(1);
        const auto path = EntryFullPath(*tab, 0);
        const bool starred = s.places.IsStarred(path);
        const auto point = HoverAction(s, action, 1);
        std::printf("[CASE] action=%d gesture=%d\n", static_cast<int>(action), kind);
        Check(point.has_value(), "hover-only row action has a real hit target");
        if (!point) continue;
        menus = 0;
        const auto timer = SetTimer(nullptr, 0, 30, CloseMenu);
        Gesture(s, *point, kind);
        KillTimer(nullptr, timer);
        const bool changed = action == H::RowMore ? menus > 0 : s.places.IsStarred(path) != starred;
        Check(changed == Activates(kind), "row More opens actual menu / Star changes actual place only on valid release");
    }
    for (const UINT dpi : {96u, 144u, 192u}) {
        Home(s, 1);
        RECT rect{20, 20, 20 + static_cast<LONG>(1100 * dpi / 96), 20 + static_cast<LONG>(650 * dpi / 96)};
        SendMessageW(s.hwnd, WM_DPICHANGED, MAKEWPARAM(dpi, dpi), reinterpret_cast<LPARAM>(&rect));
        s.darkMode = dpi != 144;
        Frame(s);
        const auto point = HoverAction(s, H::RowNewTab, 1);
        Check(point.has_value(), "new-tab remains reachable at 100/150/200 percent DPI");
        if (!point) continue;
        const auto before = s.window_tabs.items.size();
        Input(s.hwnd, WM_LBUTTONDOWN, *point);
        const bool was_shot = s.shot.active;
        s.shot.active = true;
        SetEnvironmentVariableW(L"PULSE_TEST_HIDDEN_SHOT", L"1");
        Frame(s);
        const auto image = base / (L"pressed-" + std::to_wstring(dpi) + L".png");
        Check(s.compositor.SaveSnapshot(image.c_str()), "actual pressed-state compositor snapshot saved");
        s.shot.active = was_shot;
        SetEnvironmentVariableW(L"PULSE_TEST_HIDDEN_SHOT", nullptr);
        Input(s.hwnd, WM_LBUTTONUP, *point);
        Check(s.window_tabs.items.size() == before + 1, "DPI/theme case opens one real tab on release");
    }
    {
        RECT rect{20, 20, 1520, 870};
        SendMessageW(s.hwnd, WM_DPICHANGED, MAKEWPARAM(96, 96), reinterpret_cast<LPARAM>(&rect));
    }
    Home(s, 0);
    ops::OpRequest req; req.type = ops::OpType::CreateFolder;
    req.sources = {(fixture / L"UndoFixture").wstring()};
    const auto task = s.ops.Submit(req);
    Check(task && Wait([&] { return !s.ops.HasPendingOperations() && std::filesystem::exists(fixture / L"UndoFixture"); }),
          "real folder creation produces an undoable operation");
    NavigateTo(s, (fixture / L"A_Alpha").wstring());
    Wait([&] { return ActiveTab(s) && !ActiveTab(s)->loading; });
    s.undoHistoryOpen = true; s.undoHistoryOffset = 0; Frame(s);
    auto row = Find(s, H::UndoHistoryRow, 0);
    Check(row.has_value(), "actual undo-history row exists");
    if (row) {
        Input(s.hwnd, WM_MOUSEMOVE, *row); Frame(s);
        const auto locate = Find(s, H::UndoHistoryRow, 0, -1, 1);
        Check(locate.has_value(), "history locate sub-button exists for elsewhere operation");
        if (locate) {
            const auto undo_other = Find(s, H::UndoHistoryRow, 0, -1, 0);
            if (undo_other) {
                Input(s.hwnd, WM_LBUTTONDOWN, *locate);
                Input(s.hwnd, WM_LBUTTONUP, *undo_other);
                Check(s.undoHistoryOpen && std::filesystem::exists(fixture / L"UndoFixture") &&
                      ActiveTab(s)->current_path == fs::NormalizePath((fixture / L"A_Alpha").wstring()),
                      "history locate press and undo release activate neither sub-button");
            }
            Input(s.hwnd, WM_MOUSEMOVE, *row); Frame(s);
            Gesture(s, *locate, 0);
            Check(!s.undoHistoryOpen && ActiveTab(s)->current_path == fs::NormalizePath(fixture.wstring()),
                  "history locate release navigates to actual operation folder");
        }
        Check(Wait([&] { return ActiveTab(s) && !ActiveTab(s)->loading; }),
              "locate navigation settles before next history gesture");
        s.undoHistoryOpen = true; Frame(s);
        row = Find(s, H::UndoHistoryRow, 0);
        if (row) { Input(s.hwnd, WM_MOUSEMOVE, *row); Frame(s); }
        const auto undo = Find(s, H::UndoHistoryRow, 0, -1, 0);
        Check(undo.has_value(), "history undo sub-button exists");
        if (undo) {
            Gesture(s, *undo, 1);
            Check(std::filesystem::exists(fixture / L"UndoFixture"), "history outside release does not undo");
            if (row) Input(s.hwnd, WM_MOUSEMOVE, *row);
            Frame(s);
            Gesture(s, *undo, 2);
            const auto progress_before_wait = s.ops.HistoryProgress();
            std::printf("[UNDO] pending=%d busy=%d sequence=%llu\n",
                        s.ops.HasPendingOperations(), progress_before_wait.busy,
                        static_cast<unsigned long long>(progress_before_wait.completed_sequence));
            Check(Wait([&] { return !std::filesystem::exists(fixture / L"UndoFixture"); }, 10000),
                  "history undo after reentry removes the real test folder");
            const auto status = s.ops.Status();
            std::printf("[UNDO_STATUS] type=%d phase=%d active=%d pause=%d authorization=%d wait=%d error=%ls item=%ls\n",
                        static_cast<int>(status.type), static_cast<int>(status.phase), status.active,
                        status.pause_requested, static_cast<int>(status.authorization), static_cast<int>(status.wait_reason),
                        status.last_error.c_str(), status.current_item.c_str());
            const auto progress = s.ops.HistoryProgress();
            std::printf("[UNDO] completed=%llu steps=%zu success=%d busy=%d\n",
                        static_cast<unsigned long long>(progress.completed_sequence), progress.completed_steps,
                        progress.completed_success, progress.busy);
        }
    }
    s.undoHistoryOpen = false;
    // Keep the recycle failure above visible, then exercise an independent
    // history operation without recycling or changing the user's files.
    if (s.ops.HasPendingOperations()) s.ops.CancelCurrent();
    Check(Wait([&] { return !s.ops.HasPendingOperations() && !s.ops.HistoryProgress().busy; }),
          "previous history operation settles before independent rename case");
    const auto rename_before = fixture / L"RenameBefore";
    const auto rename_after = fixture / L"RenameAfter";
    std::filesystem::create_directory(rename_before);
    ops::OpRequest rename; rename.type = ops::OpType::Rename;
    rename.sources = {rename_before.wstring()}; rename.new_name = L"RenameAfter";
    const auto rename_task = s.ops.Submit(rename);
    Check(rename_task && Wait([&] { return !s.ops.HasPendingOperations() &&
          std::filesystem::exists(rename_after) && !std::filesystem::exists(rename_before); }),
          "real rename produces an independent undoable operation");
    for (const bool redo : {false, true}) {
        const auto prior_sequence = s.ops.HistoryProgress().completed_sequence;
        const bool prior_state = std::filesystem::exists(redo ? rename_before : rename_after) &&
                                 !std::filesystem::exists(redo ? rename_after : rename_before);
        s.undoHistoryOpen = true; Frame(s);
        const auto history_row = Find(s, H::UndoHistoryRow, 0);
        if (history_row) { Input(s.hwnd, WM_MOUSEMOVE, *history_row); Frame(s); }
        const auto action = Find(s, H::UndoHistoryRow, 0, -1, 0);
        Check(action.has_value(), "rename history undo/redo button is reachable");
        if (action) Gesture(s, *action, redo ? 0 : 2);
        Check(Wait([&] { return prior_state && !s.ops.HasPendingOperations() && !s.ops.HistoryProgress().busy &&
              s.ops.HistoryProgress().completed_sequence > prior_sequence &&
              std::filesystem::exists(redo ? rename_after : rename_before) &&
              !std::filesystem::exists(redo ? rename_before : rename_after); }),
              redo ? "history redo restores real renamed directory" : "history undo restores real original directory");
    }
    s.undoHistoryOpen = false; Home(s, 0);
    while (s.window_tabs.items.size() < 20) s.window_tabs.NewTab(fixture.wstring());
    SwitchTab(s, 0); Frame(s);
    for (int kind : {0, 2}) {
        s.hoverRegion = 0; s.hoverControlIndex = -1;
        auto point = Find(s, H::Tab, 1);
        Check(point.has_value(), "crowded inactive tab is reachable");
        if (!point) continue;
        Input(s.hwnd, WM_MOUSEMOVE, *point); Frame(s);
        auto close = Find(s, H::TabClose, 1);
        Check(close.has_value(), "crowded inactive tab close appears on hover");
        if (!close) continue;
        const auto before = s.window_tabs.items.size();
        Gesture(s, *close, kind);
        Check(s.window_tabs.items.size() == before - 1, "existing TabClose preservation still works");
    }
}
void CALLBACK Probe(HWND, UINT, UINT_PTR timer, DWORD) {
    HWND window = nullptr;
    EnumThreadWindows(GetCurrentThreadId(), [](HWND candidate, LPARAM ctx) -> BOOL {
        wchar_t name[100]{}; GetClassNameW(candidate, name, 100);
        if (wcscmp(name, app::SingleInstanceCoordinator::WindowClassName()) == 0) {
            *reinterpret_cast<HWND*>(ctx) = candidate; return FALSE;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&window));
    auto* s = window ? GetAppState(window) : nullptr;
    if (s && s->startupComplete && s->first_present_reported) {
        KillTimer(nullptr, timer); Exercise(*s);
        std::printf("ROW_ACTION_RESULT pass=%d fail=%d\n", passed, failed); std::fflush(stdout);
        if (IsWindow(window)) DestroyWindow(window);
        PostQuitMessage(failed ? 1 : 0);
    } else if (GetTickCount64() - started > 30000) {
        KillTimer(nullptr, timer); Check(false, "real Pulse startup within timeout"); PostQuitMessage(2);
    }
}
}
LONG CALLBACK DiagnosticException(EXCEPTION_POINTERS* info) {
    const auto code = info->ExceptionRecord->ExceptionCode;
    if (code != 0x406D1388 && code != 0x80000003) {
        HMODULE module = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          reinterpret_cast<LPCWSTR>(info->ExceptionRecord->ExceptionAddress), &module);
        wchar_t name[MAX_PATH]{}; GetModuleFileNameW(module, name, MAX_PATH);
        std::printf("[EXCEPTION] code=%08lx module=%ls offset=%llx\n", code, name,
            static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(info->ExceptionRecord->ExceptionAddress) -
                                            reinterpret_cast<uintptr_t>(module)));
        std::fflush(stdout);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
int wmain() {
    std::setlocale(LC_ALL, ".UTF8");
    AddVectoredExceptionHandler(1, DiagnosticException);
    setvbuf(stdout, nullptr, _IONBF, 0);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    base = std::filesystem::current_path() / L"bench_data" / L"row-action-click-20261125" /
           (L"run-" + std::to_wstring(GetCurrentProcessId()));
    fixture = base / L"files";
    for (const auto* name : {L"A_Alpha", L"B_Beta", L"C_Gamma"}) std::filesystem::create_directories(fixture / name);
    std::filesystem::create_directories(base / L"profile");
    SetEnvironmentVariableW(L"PULSE_TEST_DATA_DIR", (base / L"profile").c_str());
    SetEnvironmentVariableW(L"LOCALAPPDATA", (base / L"profile").c_str());
    SetEnvironmentVariableW(L"APPDATA", (base / L"profile").c_str());
    const auto desktop_name = L"PulseRowActions-" + std::to_wstring(GetCurrentProcessId());
    const auto desktop = CreateDesktopW(desktop_name.c_str(), nullptr, nullptr, 0, GENERIC_ALL, nullptr);
    if (!desktop || !SetThreadDesktop(desktop)) { Check(false, "isolated desktop creation"); return 2; }
    std::printf("[INFO] full production Pulse window on private desktop; real filesystem; no desktop switch or physical input\n");
    started = GetTickCount64(); SetTimer(nullptr, 0, 50, Probe);
    return wWinMain(GetModuleHandleW(nullptr), nullptr, GetCommandLineW(), SW_SHOWNOACTIVATE);
}
