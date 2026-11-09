// Renders the progress card in each state to
// PNG and checks dismissal, cancellation, and delayed success close. Usage: pulse_ops_dialog_shots [out-dir] [locale]
// Add --copy-pipeline-only as the third argument for eight focused pipeline images.
#include "../common/windows_compat.h"
#include "../common/localization.h"
#include "../ui/file_operation_dialog.h"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <map>
#include <optional>
#include <vector>

namespace pulse::ui {
struct FileOperationWindowTestAccess {
    static void More(FileOperationWindow& window, int command) { window.RunMoreAction(command); }
    static bool MoreShot(FileOperationWindow& window, const std::filesystem::path& path) {
        FluentMenu menu;
        if (!menu.Create(window.hwnd_, &window.compositor_, window.scale_)) return false;
        menu.SetTheme(window.dark_, window.accent_);
        return menu.SaveDebugSnapshot(path.c_str(), window.MoreItems());
    }
    static HWND Handle(FileOperationWindow& window) { return window.hwnd_; }
    static int KeepOpenControl() { return 10; }
    static POINT FindControl(FileOperationWindow& window, int control) {
        RECT client{};
        GetClientRect(window.hwnd_, &client);
        for (int y = 0; y < client.bottom; ++y)
            for (int x = 0; x < client.right; ++x)
                if (window.HitTestControl(static_cast<float>(x), static_cast<float>(y)) == control)
                    return {x, y};
        return {-1, -1};
    }
    static void Size(FileOperationWindow& window, UINT dpi, bool detailed) {
        window.detailed_ = detailed;
        RECT rect{0, 0, MulDiv(440, dpi, 96), MulDiv(detailed ? 384 : 286, dpi, 96)};
        SendMessageW(window.hwnd_, WM_DPICHANGED, MAKELONG(dpi, dpi), reinterpret_cast<LPARAM>(&rect));
    }
    static void Graph(FileOperationWindow& window, float upto, double base, bool by_progress = true) {
        window.graph_.Reset();
        window.graph_.by_progress = by_progress;
        const int count = by_progress ? static_cast<int>(upto * 200.0f) : 90;
        for (int i = 0; i <= count; ++i) {
            const float x = by_progress ? upto * static_cast<float>(i) / static_cast<float>(count) : 0.0f;
            const double t = static_cast<double>(i);
            double v = base * (0.82 + 0.12 * std::sin(t * 0.21) + 0.06 * std::sin(t * 0.77));
            if (i < 8) v *= 0.25 + 0.75 * t / 8.0;  // warm-up ramp
            window.graph_.points.push_back({ x, v });
        }
    }
    static float GraphEnd(const FileOperationWindow& window) {
        return window.graph_.points.empty() ? -1.0f : window.graph_.points.back().x;
    }
    static void Elapsed(FileOperationWindow& window, ULONGLONG ms, bool ended) {
        const ULONGLONG now = GetTickCount64();
        window.task_start_tick_ = now - ms;
        window.task_end_tick_ = ended ? now : 0;
    }
    static const ops::OpStatus& Shown(FileOperationWindow& window) { return window.status_; }
    static void RetryRows(FileOperationWindow& window, const std::vector<size_t>& rows) {
        window.RetryFailureRows(rows);
    }
    static int RowState(FileOperationWindow& window, size_t row) {
        if (!window.board_ || row >= window.board_->rows.size()) return -1;
        return static_cast<int>(window.board_->rows[row].state);
    }
    static bool Board(FileOperationWindow& window) { return window.board_.has_value(); }
    static int Height(FileOperationWindow& window) {
        RECT rect{};
        GetWindowRect(window.hwnd_, &rect);
        return rect.bottom - rect.top;
    }
    static bool CanRetry(FileOperationWindow& window) { return window.FailureView().can_retry; }
    static void Place(FileOperationWindow& window, int x, int y) {
        window.positioned_ = true;
        SetWindowPos(window.hwnd_, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    static bool Snapshot(FileOperationWindow& window, const std::filesystem::path& path) {
        window.backdrop_enabled_ = false;
        window.Render();
        window.Render();
        return window.compositor_.SaveSnapshot(path.c_str());
    }
};
}

namespace {
int failures = 0;
void Check(bool pass, const char* label) {
    std::printf("[%s] %s\n", pass ? "PASS" : "FAIL", label);
    if (!pass) ++failures;
}

void CALLBACK ChooseMoreDestination(HWND, UINT, UINT_PTR timer, DWORD) {
    HWND menu = FindWindowW(L"PulseFluentMenu", nullptr);
    if (!menu || !IsWindowVisible(menu)) return;
    KillTimer(nullptr, timer);
    PostMessageW(menu, WM_KEYDOWN, L'D', 0);
}

void PumpFor(ULONGLONG duration_ms) {
    const ULONGLONG until = GetTickCount64() + duration_ms;
    MSG message{};
    while (GetTickCount64() < until) {
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        Sleep(5);
    }
}

pulse::ops::OpStatus Running(pulse::ops::OpType type, float percent) {
    using namespace pulse;
    ops::OpStatus st;
    st.task_id = 11;
    st.current_item_token = 17;
    st.active = true;
    st.type = type;
    st.phase = ops::OpPhase::Running;
    st.percent = percent;
    st.source_label = L"Downloads";
    st.destination_dir = L"D:\\Backup\\Projects 2026";
    st.destination_label = L"D:\\Backup\\Projects 2026";
    st.current_item = L"raw\\2026-10-17\\DSC_04187.NEF";
    st.total_items = 2003;
    st.completed_items = static_cast<uint64_t>(2003 * percent / 100.0f);
    st.total_bytes = 18ull * 1024 * 1024 * 1024;
    st.transferred_bytes = static_cast<uint64_t>(st.total_bytes * percent / 100.0);
    st.bytes_per_second = 212.0 * 1024 * 1024;
    st.peak_bytes_per_second = 248.0 * 1024 * 1024;
    st.speed_basis = ops::OpSpeedBasis::Bytes;
    st.eta_seconds = 34;
    return st;
}
}

int wmain(int argc, wchar_t** argv) {
    using namespace pulse;
    using Access = ui::FileOperationWindowTestAccess;
    compat::EnableDpiAwareness();
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const wchar_t* locale = argc > 2 ? argv[2] : L"zh-CN";
    l10n::Initialize(GetModuleHandleW(nullptr), locale);
    std::filesystem::path output;
    if (argc > 1) { output = argv[1]; std::filesystem::create_directories(output); }

    int dismissed = 0;
    int cancelled = 0;
    int paused = 0;
    int resumed = 0;
    uint64_t skipped_task = 0, skipped_item = 0;
    uint64_t located_task = 0, menu_task = 0;
    int menu_action = 0;
    ui::FileOperationCallbacks callbacks;
    int reached_status = 0;
    callbacks.dismiss = [&] { ++dismissed; };
    callbacks.reached_status = [&] { ++reached_status; };
    callbacks.cancel = [&] { ++cancelled; };
    callbacks.locate_results = [&](uint64_t task) { located_task = task; };
    callbacks.more_action = [&](uint64_t task, int action) { menu_task = task; menu_action = action; };
    callbacks.pause = [&] { ++paused; };
    callbacks.resume = [&] { ++resumed; };
    callbacks.skip_current = [&](uint64_t task, uint64_t item) {
        skipped_task = task;
        skipped_item = item;
    };
    // Partial failure retries: each call spawns one fake task per call.
    struct RetryCall { uint64_t task; std::vector<size_t> items; };
    std::vector<RetryCall> retry_calls;
    uint64_t next_spawn = 41;
    callbacks.retry_failed = [&](uint64_t task, const std::vector<size_t>& items) {
        retry_calls.push_back({ task, items });
        return std::vector<ops::OpsManager::RetrySpawn>{ { next_spawn++, items } };
    };
    std::map<uint64_t, ops::OpStatus> outcomes;
    callbacks.task_outcome = [&](uint64_t task) -> std::optional<ops::OpStatus> {
        const auto found = outcomes.find(task);
        if (found == outcomes.end()) return std::nullopt;
        return found->second;
    };
    std::vector<std::wstring> opened;
    callbacks.open_folder = [&](const std::wstring& folder) { opened.push_back(folder); };
    ops::OpSpeedGraph saved_history;
    callbacks.speed_history = [&](uint64_t) { return saved_history; };
    ui::FileOperationWindow window;
    Check(window.Create(nullptr, std::move(callbacks)), "create hidden operation window");
    const double mb = 1024.0 * 1024.0;

    auto shot = [&](const std::wstring& name) {
        if (output.empty()) return;
        Check(Access::Snapshot(window, output / (name + L".png")), "save screenshot");
    };

    if (argc > 3 && wcscmp(argv[3], L"--graph-completion-only") == 0) {
        ops::OpSpeedGraph graph;
        graph.Sample(1000, 86.0f, 200.0);
        graph.Complete();
        Check(graph.points.back().x == 1.0f && graph.points.back().v == 200.0,
              "completion bypasses interval and preserves measured speed");
        graph.Complete();
        Check(graph.points.size() == 2, "completion is idempotent");
        graph.Reset();
        graph.Complete();
        Check(graph.points.empty(), "empty history stays empty");
        graph.Sample(1000, -1.0f, 200.0);
        graph.Complete();
        Check(!graph.by_progress && graph.points.size() == 1, "time history stays unchanged");
        for (int mode = 0; mode < 4; ++mode) {
            auto status = Running(ops::OpType::Copy, 86.0f);
            status.task_id = 9000 + mode;
            status.enumeration_complete = true;
            window.Update(status);
            Access::Graph(window, 0.86f, 212.0 * mb);
            status.active = false;
            status.phase = (mode == 0 || mode == 3) ? ops::OpPhase::Completed : ops::OpPhase::Failed;
            if (mode == 1 || mode == 2) status.last_error = mode == 1 ? L"Failed" : L"Cancelled";
            if (mode == 3) status.failed_items = 1;
            if (mode == 0) {
                status.percent = 100.0f;
                status.completed_items = status.total_items;
                status.transferred_bytes = status.total_bytes;
            }
            window.Update(status);
            Check(Access::GraphEnd(window) == (mode == 0 ? 1.0f : 0.86f),
                  "only successful completion closes history");
            shot(L"graph_completion_" + std::to_wstring(mode));
        }
        saved_history.Sample(1000, 0.0f, 120.0 * mb);
        saved_history.Sample(1250, 40.0f, 180.0 * mb);
        saved_history.Sample(1500, 86.0f, 212.0 * mb);
        auto completed = Running(ops::OpType::Copy, 100.0f);
        completed.task_id = 9100;
        completed.active = false;
        completed.phase = ops::OpPhase::Completed;
        completed.enumeration_complete = true;
        window.Update(completed);
        Check(Access::GraphEnd(window) == 1.0f,
              "first completed card closes truncated callback history");
        window.SetTheme(true, ui::HexColor(0x6474C7));
        Access::Size(window, 144, false);
        Access::Place(window, -4000, -4000);
        window.Show(false);
        window.Hide();
        window.Show(false);
        Check(Access::GraphEnd(window) == 1.0f,
              "reopened completed card cannot regress to truncated callback history");
        Access::Elapsed(window, 7000, true);
        shot(L"graph_completion_dark_144dpi_reopened");
        window.Destroy();
        CoUninitialize();
        return failures ? 1 : 0;
    }

    if (argc > 3 && wcscmp(argv[3], L"--copy-pipeline-only") == 0) {
        Check(!output.empty(), "pipeline screenshots require output directory");
        uint64_t pipeline_task = 1000;
        for (const bool dark : {true, false}) {
            window.SetTheme(dark, ui::HexColor(0x6474C7));
            for (const UINT dpi : {96u, 144u}) {
                const std::wstring suffix = std::wstring(dark ? L"-dark-" : L"-light-") + std::to_wstring(dpi);
                Access::Size(window, dpi, false);
                Access::Place(window, -4000, -4000);
                auto unknown = Running(ops::OpType::Copy, 25.0f);
                unknown.task_id = pipeline_task++;
                unknown.active = true;
                unknown.can_pause = true;
                unknown.enumeration_complete = false;
                unknown.percent = -1.0f;
                unknown.eta_seconds = 0;
                unknown.total_items = 640;
                unknown.completed_items = 128;
                unknown.total_bytes = 512ull << 20;
                unknown.transferred_bytes = 128ull << 20;
                unknown.summary = l10n::Get(l10n::StringId::OpScanningAndCopying);
                window.Update(unknown);
                Access::Place(window, -4000, -4000);
                Access::Graph(window, 0.0f, 212.0 * mb, false);
                Access::Elapsed(window, 12000, false);
                const auto& shown_unknown = Access::Shown(window);
                Check(shown_unknown.active && !shown_unknown.enumeration_complete && shown_unknown.percent < 0 &&
                      shown_unknown.eta_seconds == 0 && shown_unknown.total_items > shown_unknown.completed_items &&
                      shown_unknown.completed_items > 0 && shown_unknown.transferred_bytes > 0,
                      "shown scanning-copy status retains unknown totals");
                shot(L"pipeline-unknown" + suffix);

                auto verifying = Running(ops::OpType::Copy, 99.0f);
                verifying.task_id = pipeline_task++;
                verifying.phase = ops::OpPhase::Verifying;
                verifying.active = true;
                verifying.can_pause = true;
                verifying.enumeration_complete = true;
                verifying.percent = 99.0f;
                verifying.eta_seconds = 0;
                verifying.summary = l10n::Get(l10n::StringId::Verifying);
                window.Update(verifying);
                Access::Place(window, -4000, -4000);
                Access::Graph(window, 0.99f, 212.0 * mb);
                Access::Elapsed(window, 61000, false);
                const auto& shown_verifying = Access::Shown(window);
                Check(shown_verifying.active && shown_verifying.enumeration_complete &&
                      shown_verifying.phase == ops::OpPhase::Verifying && shown_verifying.percent == 99.0f,
                      "shown verification stays active at 99 percent");
                shot(L"pipeline-verifying-99" + suffix);
            }
        }
        std::printf("%s\n", failures == 0 ? "ALL PASS" : "FAILURES");
        window.Destroy();
        CoUninitialize();
        return failures == 0 ? 0 : 1;
    }
    struct Scale { UINT dpi; const wchar_t* tag; };
    const Scale scales[] = { {96, L"100"}, {144, L"150"}, {192, L"200"} };
    for (const bool dark : {true, false}) {
        window.SetTheme(dark, ui::HexColor(0x6474C7));
        window.SetAutoClose(true);
        const std::wstring theme = dark ? L"dark" : L"light";
        for (const auto& scale : scales) {
            const bool full = scale.dpi == 96;
            const std::wstring sfx = L"-" + theme + L"-" + scale.tag;
            Access::Size(window, scale.dpi, false);

            auto st = Running(ops::OpType::Copy, 63.0f);
            window.Update(st);
            Access::Graph(window, 0.63f, 212.0 * mb);
            Access::Elapsed(window, 61000, false);
            shot(L"01-running" + sfx);
            if (!full) continue;

            st.pause_requested = true;
            window.Update(st);
            shot(L"02-pausing" + sfx);
            st.phase = ops::OpPhase::Paused;
            window.Update(st);
            shot(L"03-paused" + sfx);
            st.pause_requested = false;
            st.phase = ops::OpPhase::WaitingForConflict;
            window.Update(st);
            shot(L"04-waiting" + sfx);

            auto done = Running(ops::OpType::Copy, 100.0f);
            done.active = false;
            done.phase = ops::OpPhase::Completed;
            done.destination_dir = L"D:\\Backup\\Projects 2026";
            window.Update(done);
            Access::Graph(window, 1.0f, 212.0 * mb);
            Access::Elapsed(window, 88000, true);
            shot(L"05-done-countdown" + sfx);
            shot(L"06-done-hover" + sfx);
            window.Hide();

            auto fail = Running(ops::OpType::Move, 41.0f);
            fail.task_id = 12;
            fail.active = false;
            fail.phase = ops::OpPhase::Failed;
            fail.last_error = L"Access is denied.";
            window.Update(fail);
            Access::Graph(window, 0.41f, 150.0 * mb);
            Access::Elapsed(window, 23000, true);
            shot(L"07-failed" + sfx);

            auto del = Running(ops::OpType::RecycleDelete, 37.0f);
            del.task_id = 13;
            del.destination_label.clear();
            del.speed_basis = ops::OpSpeedBasis::Items;
            del.items_per_second = 1480.0;
            del.peak_items_per_second = 2210.0;
            window.Update(del);
            Access::Graph(window, 0.37f, 1480.0);
            Access::Elapsed(window, 4200, false);
            shot(L"08-delete" + sfx);

            auto move = Running(ops::OpType::Move, 78.0f);
            move.task_id = 14;
            Access::Size(window, scale.dpi, true);
            window.Update(move);
            Access::Graph(window, 0.78f, 180.0 * mb);
            Access::Elapsed(window, 140000, false);
            shot(L"09-move-details" + sfx);
            Access::Size(window, scale.dpi, false);

            auto auth = Running(ops::OpType::Copy, 12.0f);
            auth.task_id = 15;
            auth.authorization = ops::AuthorizationState::ActionRequired;
            auth.can_skip_authorization = true;
            window.Update(auth);
            shot(L"10-authorization" + sfx);

            auto scan = Running(ops::OpType::Copy, -1.0f);
            scan.task_id = 16;
            scan.phase = ops::OpPhase::Scanning;
            scan.total_items = 0;
            scan.speed_basis = ops::OpSpeedBasis::None;
            window.Update(scan);
            shot(L"11-scanning" + sfx);
        }
    }

    // Header dismissal keeps a task running; only the footer cancels it. Success
    // remains visible for 1.2 s before it returns to the status pill.
    {
        auto interactive = Running(ops::OpType::Copy, 50.0f);
        interactive.task_id = 20;
        Access::Size(window, 96, false);
        window.Update(interactive);
        window.Show(false);
        auto click = [&](int control) {
            const POINT point = Access::FindControl(window, control);
            if (point.x < 0) return false;
            SendMessageW(Access::Handle(window), WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(point.x, point.y));
            SendMessageW(Access::Handle(window), WM_LBUTTONUP, 0, MAKELPARAM(point.x, point.y));
            return true;
        };
        Check(click(3) && paused == 1 && resumed == 0, "pause button invokes pause");
        interactive.phase = ops::OpPhase::Paused;
        window.Update(interactive);
        Check(click(3) && resumed == 1, "continue button invokes resume");
        interactive.phase = ops::OpPhase::Running;
        window.Update(interactive);
        Check(click(9) && skipped_task == interactive.task_id && skipped_item == interactive.current_item_token,
              "skip button targets the displayed task and item token");
        const int collapsed = Access::Height(window);
        Check(click(5) && Access::Height(window) > collapsed, "details click expands the window");
        Check(click(5) && Access::Height(window) == collapsed, "details click collapses the window");
        Check(Access::FindControl(window, 11).x >= 0, "more button has a separate hit target");
        const UINT_PTR menu_timer = SetTimer(nullptr, 0, 100, ChooseMoreDestination);
        Check(click(11) && menu_task == interactive.task_id && menu_action == 1,
              "more button opens Fluent menu and keyboard selection invokes destination action");
        KillTimer(nullptr, menu_timer);
        if (!output.empty()) Check(Access::MoreShot(window, output / L"16-more-menu.png"), "save real Fluent more menu");
        for (int command : {1, 2, 3}) {
            Access::More(window, command);
            Check(menu_task == interactive.task_id && menu_action == command, "more action retains exact task identity");
        }
        Access::More(window, 5);
        window.SetAutoClose(true);
        interactive.active = false;
        interactive.phase = ops::OpPhase::Completed;
        interactive.percent = 100;
        window.Update(interactive);
        PumpFor(1400);
        Check(window.IsVisible(), "keep-open menu choice survives host auto-close refresh");
        POINT locate = Access::FindControl(window, 8);
        Check(locate.x >= 0 && click(8) && located_task == interactive.task_id && !window.IsVisible(),
              "completion locate invokes the exact task and dismisses");
        dismissed = 0;
        interactive.task_id = 201;
        interactive.active = true;
        interactive.phase = ops::OpPhase::Running;
        window.Update(interactive);
        window.SetAutoClose(true);
        window.Show(false);
        Access::More(window, 4);
        interactive.active = false;
        interactive.phase = ops::OpPhase::Completed;
        window.Update(interactive);
        PumpFor(1400);
        Check(!window.IsVisible() && located_task == interactive.task_id, "auto-locate runs after the completion hold");
        dismissed = 0;
        interactive.task_id = 202;
        interactive.active = true;
        interactive.phase = ops::OpPhase::Running;
        window.Update(interactive);
        window.Show(false);
        Access::More(window, 4);
        Check(click(1) && !window.IsVisible(), "auto-locate task can be minimized first");
        interactive.active = false;
        interactive.phase = ops::OpPhase::Completed;
        window.Update(interactive);
        Check(located_task == interactive.task_id, "hidden task still auto-locates on completion");
        dismissed = 0;
        interactive.task_id = 203;
        interactive.active = true;
        interactive.phase = ops::OpPhase::Running;
        window.Update(interactive);
        interactive.current_item_token = 0;
        window.Update(interactive);
        Check(Access::FindControl(window, 9).x < 0, "skip is disabled without a current item token");
        auto live = Running(ops::OpType::Copy, 50.0f); live.task_id = 21;
        window.Update(live); window.Show(false);
        SendMessageW(Access::Handle(window), WM_KEYDOWN, VK_ESCAPE, 0);
        Check(!window.IsVisible() && cancelled == 0 && dismissed == 1,
              "Escape hides the live operation without canceling it");
        PumpFor(450);
        Check(reached_status == 1 && !IsWindowVisible(Access::Handle(window)),
              "the card lands in the status pill once it has shrunk away");
        // Reopened from the pill while still shrinking: it comes back whole.
        window.Show(false);
        SendMessageW(Access::Handle(window), WM_KEYDOWN, VK_ESCAPE, 0);
        // Without system animations the card is gone at once; with them it is
        // still shrinking and has not reached the pill.
        const int landed = reached_status;
        window.Show(false, true);
        PumpFor(450);
        Check(window.IsVisible() && IsWindowVisible(Access::Handle(window)) && reached_status == landed,
              "reopening from the pill mid-shrink keeps the card shown");
        Check(SendMessageW(Access::Handle(window), WM_KEYDOWN, VK_ESCAPE, 0) == 0 && !window.IsVisible(),
              "the reopened card hides to the pill again");
        PumpFor(450);
        dismissed = 1;

        window.Show(false);
        POINT close = Access::FindControl(window, 1);
        if (close.x >= 0) {
            SendMessageW(Access::Handle(window), WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(close.x, close.y));
            SendMessageW(Access::Handle(window), WM_LBUTTONUP, 0, MAKELPARAM(close.x, close.y));
        }
        Check(close.x >= 0 && !window.IsVisible() && cancelled == 0 && dismissed == 2,
              "title-bar close hides the live operation without canceling it");

        window.Show(false);
        POINT minimize = Access::FindControl(window, 2);
        if (minimize.x >= 0) {
            SendMessageW(Access::Handle(window), WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(minimize.x, minimize.y));
            SendMessageW(Access::Handle(window), WM_LBUTTONUP, 0, MAKELPARAM(minimize.x, minimize.y));
        }
        Check(minimize.x >= 0 && !window.IsVisible() && cancelled == 0 && dismissed == 3,
              "the other title-bar control also returns the operation to the status area");

        window.Show(false);
        POINT cancel = Access::FindControl(window, 4);
        if (cancel.x >= 0) {
            SendMessageW(Access::Handle(window), WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(cancel.x, cancel.y));
            SendMessageW(Access::Handle(window), WM_LBUTTONUP, 0, MAKELPARAM(cancel.x, cancel.y));
        }
        Check(cancel.x >= 0 && !window.IsVisible() && cancelled == 1 && dismissed == 4,
              "the footer cancel button cancels and dismisses the operation");

        auto live_success = Running(ops::OpType::Copy, 50.0f); live_success.task_id = 23;
        Access::Size(window, 96, true);
        window.Update(live_success); window.Show(false);
        const int expanded_h = Access::Height(window);
        live_success.active = false; live_success.phase = ops::OpPhase::Completed; live_success.percent = 100;
        window.Update(live_success);
        Check(Access::Height(window) < expanded_h, "completion folds the live details panel before showing the result");
        Access::Graph(window, 1.0f, 212.0 * mb);
        Access::Elapsed(window, 88000, true);
        Check(window.IsVisible() && dismissed == 4, "success remains visible during the completion hold");
        shot(L"12-completed-hold");
        PumpFor(1400);
        Check(!window.IsVisible() && dismissed == 5, "success closes after the 1.2-second completion hold");

        auto live_kept = Running(ops::OpType::Copy, 50.0f); live_kept.task_id = 24;
        live_kept.destination_dir = L"D:\\Backup\\Projects 2026";
        window.SetAutoClose(true);
        Access::Size(window, 144, false);
        window.Update(live_kept); window.Show(false);
        live_kept.active = false; live_kept.phase = ops::OpPhase::Completed; live_kept.percent = 100;
        live_kept.completed_items = live_kept.total_items;
        live_kept.transferred_bytes = live_kept.total_bytes;
        window.Update(live_kept);
        Access::Graph(window, 1.0f, 212.0 * mb);
        Access::Elapsed(window, 88000, true);
        shot(L"13-completed-before-keep-150");
        POINT keep = Access::FindControl(window, Access::KeepOpenControl());
        if (keep.x >= 0) {
            SendMessageW(Access::Handle(window), WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(keep.x, keep.y));
            SendMessageW(Access::Handle(window), WM_LBUTTONUP, 0, MAKELPARAM(keep.x, keep.y));
        }
        Check(keep.x >= 0 && window.IsVisible(), "completion offers a keep-open action");
        shot(L"13-completed-keep-open-150");
        PumpFor(1400);
        Check(window.IsVisible() && dismissed == 5, "keep-open action cancels the success countdown");

        auto st = Running(ops::OpType::Copy, 50.0f); st.task_id = 25;
        st.active = false; st.phase = ops::OpPhase::Failed; st.last_error = L"Access denied";
        window.Update(st); window.Show(false); window.Update(st);
        Check(window.IsVisible(), "failure remains visible"); window.Hide();
        st.task_id = 26; st.phase = ops::OpPhase::Completed; st.last_error.clear();
        window.SetAutoClose(false);
        window.Update(st); window.Show(false); window.Update(st);
        Check(window.IsVisible(), "explicit finished summary stays visible"); window.Hide();
    }

    // Partial failure (部分失败): the rest went through; failed items are listed
    // with reasons, retried one by one or together, and never auto-close.
    auto partial_status = [&](uint64_t task, size_t listed, uint64_t failed_count) {
        auto st = Running(ops::OpType::Copy, 100.0f);
        st.task_id = task;
        st.active = false;
        st.phase = ops::OpPhase::Failed;
        st.destination_dir = L"D:\\Backup\\Projects 2026";
        st.completed_items = st.total_items;
        const wchar_t* names[] = { L"\x62a5\x4ef7\x5355_v3.xlsx", L"setup.log", L"pagefile-copy.sys",
                                   L"DSC_04187.NEF", L"meeting-notes-2026-10-17-final-final-long-name.docx" };
        const wchar_t* reasons[] = { L"\x88ab Excel \x5360\x7528", L"\x9700\x8981\x7ba1\x7406\x5458\x6743\x9650",
                                     L"\x7cfb\x7edf\x6587\x4ef6", L"\x88ab Lightroom Classic \x5360\x7528",
                                     L"\x6587\x4ef6\x540d\x592a\x957f" };
        for (size_t i = 0; i < listed; ++i)
            st.failures.push_back({ std::wstring(L"C:\\Users\\User\\Downloads\\") + names[i % 5],
                                    std::wstring(L"D:\\Backup\\Projects 2026\\") + names[i % 5],
                                    reasons[i % 5], E_ACCESSDENIED });
        st.failed_items = failed_count;
        st.last_error = st.failures.empty() ? L"" : st.failures[0].reason + L" | " + st.failures[0].source;
        return st;
    };
    auto finished = [&](uint64_t task, bool ok) {
        auto st = Running(ops::OpType::Copy, 100.0f);
        st.task_id = task;
        st.active = false;
        st.phase = ok ? ops::OpPhase::Completed : ops::OpPhase::Failed;
        return st;
    };
    for (const bool dark : {true, false}) {
        window.SetTheme(dark, ui::HexColor(0x6474C7));
        const std::wstring sfx = dark ? L"-dark-100" : L"-light-100";
        retry_calls.clear();
        Access::Size(window, 96, false);
        const int plain_h = Access::Height(window);
        auto st = partial_status(dark ? 31 : 61, 5, 6);
        window.Update(st);
        window.Show(false); // Verify visibility through failure/retry transitions.
        Access::Graph(window, 1.0f, 212.0 * mb);
        Access::Elapsed(window, 88000, true);
        Check(Access::Board(window), "partial failure lists the failed items");
        Check(Access::Height(window) == plain_h + MulDiv(90 + 4 * 52 + 22 + 2 + 14 - 230, 96, 96),
              "window fits four rows and the more line");
        Check(Access::Shown(window).failed_items == 6, "six items still failed");
        Check(window.IsVisible(), "partial failure does not auto-close");
        Check(Access::CanRetry(window),
              "footer offers to retry the failed items");
        shot(L"12-partial" + sfx);

        Access::RetryRows(window, { 0 });
        Check(retry_calls.size() == 1 && retry_calls[0].task == st.task_id &&
              retry_calls[0].items == std::vector<size_t>{ 0 }, "row retry asks for that item only");
        Check(Access::RowState(window, 0) == 1, "row shows retrying");
        const uint64_t first_spawn = next_spawn - 1;
        Check(window.TracksRetry(first_spawn) && !window.TracksRetry(first_spawn + 100),
              "the app can tell a list retry from another operation");
        auto running = Running(ops::OpType::Copy, 40.0f);
        running.task_id = first_spawn;
        window.Update(running);
        Check(Access::Board(window) && Access::Shown(window).task_id == st.task_id,
              "card stays on the failed task while the retry runs");
        shot(L"13-partial-retrying" + sfx);
        window.Update(finished(first_spawn, true));
        Check(Access::RowState(window, 0) == 2, "row done after its retry succeeds");
        window.Update(finished(first_spawn, true));   // the app repeats the last status
        Check(Access::Board(window) && Access::Shown(window).task_id == st.task_id,
              "a repeated finish of the retry keeps the list");
        Check(Access::Shown(window).failed_items == 5, "one less failed");
        Access::RetryRows(window, { 0 });
        Check(retry_calls.size() == 1, "a done row is not retried again");

        Access::RetryRows(window, { 0, 1, 2, 3, 4 });
        Check(retry_calls.size() == 2 && retry_calls[1].items == std::vector<size_t>({ 1, 2, 3, 4 }),
              "retry all asks for the rows still failed");
        auto again = finished(next_spawn - 1, false);
        again.failures.push_back({ st.failures[2].source, st.failures[2].destination,
                                   L"\x62d2\x7edd\x8bbf\x95ee", E_ACCESSDENIED });
        again.failed_items = 1;
        window.Update(again);
        window.Update(again);
        Check(Access::RowState(window, 2) == 0 && Access::RowState(window, 1) == 2 && Access::RowState(window, 4) == 2,
              "the item that failed again is listed again, the rest are done");
        Check(Access::Shown(window).failed_items == 2, "one listed and one unlisted still failed");
        shot(L"14-partial-after" + sfx);
        Access::RetryRows(window, { 2 });
        Check(retry_calls.size() == 3 && retry_calls[2].task == again.task_id &&
              retry_calls[2].items == std::vector<size_t>{ 0 }, "a second retry goes through the retry's list");
        window.Update(finished(next_spawn - 1, true));
        Check(Access::Shown(window).failed_items == 1 && window.IsVisible(),
              "the unlisted failure keeps the card open");

        auto other = Running(ops::OpType::Copy, 10.0f);
        other.task_id = dark ? 39 : 69;
        window.Update(other);
        Check(!Access::Board(window) && Access::Height(window) == plain_h, "another task clears the list");
    }
    {
        // Everything retried successfully: the completion is briefly visible
        // before the card returns to the status area.
        window.SetTheme(true, ui::HexColor(0x6474C7));
        window.SetAutoClose(true);
        Access::Size(window, 96, false);
        Access::Place(window, -4000, -4000);
        auto st = partial_status(71, 1, 1);
        window.Update(st);
        window.Show(false);
        Access::RetryRows(window, { 0 });
        window.Update(finished(next_spawn - 1, true));
        Check(Access::Shown(window).phase == ops::OpPhase::Completed && Access::Shown(window).failed_items == 0,
              "all retried: shown as completed");
        Check(window.IsVisible(), "all retried: completion stays visible briefly");
        PumpFor(1400);
        Check(!window.IsVisible(), "all retried: completion closes after the hold");
        window.Hide();
        auto global = partial_status(72, 1, 1);
        window.Update(global);
        Access::RetryRows(window, { 0 });
        auto stopped = finished(next_spawn - 1, false);
        stopped.last_error = L"Access is denied. | C:\\x";
        window.Update(stopped);
        Check(Access::RowState(window, 0) == 0, "a retry that stops as a whole leaves the row failed");
        window.Hide();

        // Two retries back to back: only the second one is reported.
        auto pair = partial_status(73, 2, 2);
        window.Update(pair);
        Access::RetryRows(window, { 0 });
        const uint64_t first = next_spawn - 1;
        Access::RetryRows(window, { 1 });
        outcomes[first] = finished(first, true);
        window.Update(finished(next_spawn - 1, true));
        Check(Access::RowState(window, 0) == 2 && Access::RowState(window, 1) == 2 &&
              Access::Shown(window).phase == ops::OpPhase::Completed,
              "a retry finished while another was reported is looked up");
        window.Hide();
    }

    // Acrylic: over one of our windows the card blurs what that window shows,
    // the way menus do; anywhere else it keeps the plain surface.
    {
        ui::Compositor::SetFrameCopyEnabled(true);
        Check(ui::Compositor::GetAcrylicPlateBuilder() != nullptr, "window_material registers the dialog acrylic");
        WNDCLASSW cls{};
        cls.lpfnWndProc = DefWindowProcW;
        cls.hInstance = GetModuleHandleW(nullptr);
        cls.lpszClassName = L"PulseOpsShotsSource";
        RegisterClassW(&cls);
        HWND source_hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, cls.lpszClassName, L"", WS_POPUP,
                                           -4000, -4000, 900, 600, nullptr, nullptr, cls.hInstance, nullptr);
        ShowWindow(source_hwnd, SW_SHOWNOACTIVATE);
        ui::Compositor source;
        const bool source_ok = source_hwnd && source.Init(source_hwnd);
        Check(source_ok, "source window compositor");
        auto paint = [&](bool dark) {
            ID2D1DeviceContext* dc = source.Dc();
            dc->BeginDraw();
            dc->Clear(dark ? ui::HexColor(0x1C1C1C) : ui::HexColor(0xF7F7F7));
            ui::ComPtr<ID2D1SolidColorBrush> brush;
            dc->CreateSolidColorBrush(ui::HexColor(0x000000), &brush);
            const uint32_t colors[] = { 0x0078D4, 0xE3008C, 0xFFB900, 0x107C10, 0x8764B8, 0xD83B01 };
            for (int i = 0; i < 6; ++i) {
                brush->SetColor(ui::HexColor(colors[i]));
                const float x = 60.0f + static_cast<float>(i) * 135.0f;
                dc->FillRectangle(D2D1::RectF(x, 120.0f, x + 110.0f, 300.0f), brush.get());
            }
            brush->SetColor(dark ? ui::HexColor(0xE0E0E0) : ui::HexColor(0x202020));
            for (int row = 0; row < 12; ++row) {
                const float y = 330.0f + static_cast<float>(row) * 22.0f;
                const float w = 220.0f + static_cast<float>((row * 37) % 180);
                dc->FillRectangle(D2D1::RectF(240.0f, y, 240.0f + w, y + 8.0f), brush.get());
            }
            dc->EndDraw();
            source.Present();
        };
        const HWND card_hwnd = Access::Handle(window);
        for (const bool dark : {true, false}) {
            if (!source_ok) break;
            paint(dark);
            window.SetTheme(dark, ui::HexColor(0x6474C7));
            Access::Size(window, 96, false);
            auto st = Running(ops::OpType::Copy, 63.0f);
            st.task_id = dark ? 81 : 82;
            window.Update(st);
            Access::Graph(window, 0.63f, 212.0 * mb);
            Access::Elapsed(window, 61000, false);
            Access::Place(window, -4000 + 230, -4000 + 160);
            SetWindowPos(card_hwnd, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            RECT card{};
            GetWindowRect(card_hwnd, &card);
            Check(ui::Compositor::FrameSourceUnder(card_hwnd, card) == &source,
                  "the window under the card lends its frame");
            shot(L"15-acrylic-" + std::wstring(dark ? L"dark" : L"light"));
        }
        Access::Place(window, -4000 + 2000, -4000);
        RECT away{};
        GetWindowRect(card_hwnd, &away);
        Check(ui::Compositor::FrameSourceUnder(card_hwnd, away) == nullptr, "nothing of ours under the card");
        source.Shutdown();
        if (source_hwnd) DestroyWindow(source_hwnd);
        ui::Compositor::SetFrameCopyEnabled(false);
    }

    std::printf("%s\n", failures == 0 ? "ALL PASS" : "FAILURES");
    window.Destroy();
    CoUninitialize();
    return failures == 0 ? 0 : 1;
}
