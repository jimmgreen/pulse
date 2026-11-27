#include "../common/windows_compat.h"
#include "../common/localization.h"
#include "../ui/file_operation_dialog.h"
#include "../ui/file_operation_progress_state.h"

#include <cstdio>
#include <fstream>
#include "../common/runtime_log.h"
#include <filesystem>
#include <cmath>
#include <thread>
#include <commctrl.h>
#include <vector>

namespace pulse::ui {
struct FileOperationWindowTestAccess {
    static bool Indeterminate(FileOperationWindow& window) { return TransferIndeterminate(window.status_); }
    static bool EmptyingSubtitle(FileOperationWindow& window) { return TransferShowsEmptyingSubtitle(window.status_); }
    static void Press(FileOperationWindow& window, bool pressed) { window.pressed_ = pressed ? 1 : 0; }
    static HWND Handle(FileOperationWindow& window) { return window.hwnd_; }
    static void RenderFrame(FileOperationWindow& window) { window.Render(); }
    static void NoFlight(FileOperationWindow& window) { window.flight_animations_override_ = false; }
    static uint64_t Task(FileOperationWindow& window) { return window.status_.task_id; }
    static bool Active(FileOperationWindow& window) { return window.status_.active; }
    static ULONGLONG Deadline(FileOperationWindow& window) { return window.success_dismiss_at_; }
    static void ArmOldDeadline(FileOperationWindow& window, uint64_t task) {
        window.success_dismiss_at_ = 1; window.success_dismiss_task_id_ = task;
    }
    static bool SuccessPulse(FileOperationWindow& window, ULONGLONG now = GetTickCount64()) { return window.TickSuccessDismiss(now); }
    static void ClosingForFixture(FileOperationWindow& window, bool automatic) {
        window.flight_.Stop(); window.opening_ = false; window.closing_ = true;
        window.success_dismiss_at_ = 0;
        window.success_hiding_task_ = automatic ? std::optional<uint64_t>(window.status_.task_id) : std::nullopt;
        window.zoom_until_ = GetTickCount64() - 1;
        ShowWindow(window.hwnd_, SW_HIDE);
    }
    static void ZoomPulse(FileOperationWindow& window) { window.TickZoom(); }
    static void PendingLocate(FileOperationWindow& window, uint64_t task) { window.pending_locate_task_ = task; }
    static void Size(FileOperationWindow& window, UINT dpi, int width = 468) {
        RECT rect{0, 0, MulDiv(width, dpi, 96), MulDiv(348, dpi, 96)};
        SendMessageW(window.hwnd_, WM_DPICHANGED, MAKELONG(dpi, dpi), reinterpret_cast<LPARAM>(&rect));
        window.ResizeForDetails(false);   // the height the current state asks for
    }
    static int ClientHeight(FileOperationWindow& window) {
        RECT client{};
        GetClientRect(window.hwnd_, &client);
        return client.bottom;
    }
    static POINT Button(FileOperationWindow& window, int id, int* pixels = nullptr) {
        const int y = static_cast<int>(window.compositor_.Height() - 26 * window.scale_);
        int first = -1, last = -1;
        for (int x = 0; x < static_cast<int>(window.compositor_.Width()); ++x) {
            if (window.HitTestControl(static_cast<float>(x), static_cast<float>(y)) == id) {
                if (first < 0) first = x;
                last = x;
            }
        }
        if (pixels) *pixels = first < 0 ? 0 : last - first + 1;
        return POINT{first < 0 ? -1 : (first + last) / 2, y};
    }
    static float Width(FileOperationWindow& window, l10n::StringId id, bool shield = false) {
        return window.painter_.MeasureButtonWidth(l10n::Get(id), shield ? L"\xEA18" : L"");
    }
    static float EtaWidth(FileOperationWindow& window, const std::wstring& text) {
        ComPtr<IDWriteTextLayout> layout;
        if (FAILED(window.compositor_.DwriteFactory()->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()),
            window.label_format_.get(), 10000, 10000, &layout))) return 10000;
        DWRITE_TEXT_METRICS metrics{};
        if (FAILED(layout->GetMetrics(&metrics))) return 10000;
        return metrics.width / window.scale_;
    }
    static bool Snapshot(FileOperationWindow& window, const std::filesystem::path& path) {
        window.backdrop_enabled_ = false;
        window.Render();
        // Present flips two buffers. Paint both so SaveSnapshot reads this state.
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
void Click(pulse::ui::FileOperationWindow& window, POINT point) {
    if (point.x < 0) return;
    const HWND hwnd = pulse::ui::FileOperationWindowTestAccess::Handle(window);
    SendMessageW(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(point.x, point.y));
    SendMessageW(hwnd, WM_LBUTTONUP, 0, MAKELPARAM(point.x, point.y));
}
template<class Callbacks>
void BindTaskCallbacks(Callbacks& callbacks, uint64_t& paused, uint64_t& resumed, uint64_t& dismissed) {
    if constexpr (requires { callbacks.pause_task; callbacks.resume_task; callbacks.dismiss_task; }) {
        callbacks.pause_task = [&](uint64_t task) { paused = task; };
        callbacks.resume_task = [&](uint64_t task) { resumed = task; };
        callbacks.dismiss_task = [&](uint64_t task) { dismissed = task; };
    }
}
int RecursiveEtaDisplayCases() {
    using namespace pulse;
    using Access = ui::FileOperationWindowTestAccess;
    ui::FileOperationWindow window;
    Check(window.Create(nullptr, {}), "create isolated ETA layout probe");
    Access::NoFlight(window);
    ops::OpStatus status;
    status.task_id = 501; status.type = ops::OpType::RealDelete;
    status.active = true; status.phase = ops::OpPhase::Running; status.speed_basis = ops::OpSpeedBasis::Items;
    status.total_items = 1;
    status.eta_detail = {501, 1000, 12000, 10, 25, ops::EtaStage::Range, ops::EtaSource::RecentCopy, true};
    const auto duration = [](uint64_t seconds) { return std::to_wstring(seconds); };
    for (const wchar_t* language : {L"zh-CN", L"en-US", L"zh-TW"}) {
        l10n::SetLanguage(language);
        status.task_id = 501; status.active = true; status.external = false;
        status.eta_detail = {501, 1000, 12000, 10, 25, ops::EtaStage::Range, ops::EtaSource::RecentCopy, true};
        auto value = ui::TransferAdvisoryEtaText(status, 1100, duration);
        Check(value && value->find(L"\x2013") != std::wstring::npos && *value != l10n::Get(l10n::StringId::OpEstimating),
            "predicted ranges are distinct from learning in all supported locales");
        Check(value && Access::EtaWidth(window, *value) <= 110,
            "seconds range fits the unchanged details value column");
        for (const auto bounds : {std::pair<uint64_t,uint64_t>{80,150}, {3660,7200}}) {
            status.eta_detail.lower_seconds = bounds.first; status.eta_detail.upper_seconds = bounds.second;
            value = ui::TransferAdvisoryEtaText(status, 1100, duration);
            Check(value && Access::EtaWidth(window, *value) <= 110,
                "compact minute and hour ranges fit without repeating long units");
        }
        value = ui::TransferAdvisoryEtaText(status, 5000, duration);
        Check(value && *value == l10n::Get(l10n::StringId::OpEtaUnknownTotal),
            "stale ranges become unknown total rather than an indefinitely frozen countdown");
        status.eta_detail.stage = ops::EtaStage::Learning;
        value = ui::TransferAdvisoryEtaText(status, 5000, duration);
        Check(value && *value == l10n::Get(l10n::StringId::OpEtaUnknownTotal),
            "learning text cannot remain forever after callbacks stop");
        status.eta_detail.stage = ops::EtaStage::Finishing;
        value = ui::TransferAdvisoryEtaText(status, 1100, duration);
        Check(value && *value == l10n::Get(l10n::StringId::OpEtaFinishing) && !ui::TransferCanAutoClose(status),
            "finishing is a presentation phase, never an operation completion signal");
        Check(ui::TransferHasChildWork(status) && status.completed_items == 0 && status.total_items == 1,
            "actual child progress never rewrites selected-root completion counters");
        status.task_id = 502;
        Check(!ui::TransferAdvisoryEtaText(status, 1100, duration) && !ui::TransferHasChildWork(status),
            "ETA presentation cannot leak into a replacement task");
        status.task_id = 501; status.external = true;
        Check(!ui::TransferAdvisoryEtaText(status, 1100, duration), "external plugin ETA remains on its original presentation path");
        status.external = false; status.active = false;
        Check(!ui::TransferAdvisoryEtaText(status, 1100, duration), "terminal summaries retain their authoritative original presentation");
        status.active = true; status.eta_detail.stage = ops::EtaStage::Range; status.eta_detail.lower_seconds = 0;
        value = ui::TransferAdvisoryEtaText(status, 1100, duration);
        Check(value && *value == l10n::Get(l10n::StringId::OpEtaUnknownTotal), "malformed ranges do not display zero seconds remaining");
    }
    std::printf("RECURSIVE_ETA_UI_RESULT failures=%d\n", failures);
    return failures ? 1 : 0;
}
int TaskBindingCases() {
    using namespace pulse;
    using Access = ui::FileOperationWindowTestAccess;
    uint64_t paused = 0, resumed = 0, dismissed = 0;
    constexpr uint64_t displayed = 501, backend_current = 502;
    ui::FileOperationCallbacks callbacks;
    callbacks.pause = [&] { paused = backend_current; };
    callbacks.resume = [&] { resumed = backend_current; };
    callbacks.dismiss = [&] { dismissed = backend_current; };
    BindTaskCallbacks(callbacks, paused, resumed, dismissed);
    ui::FileOperationWindow window;
    Check(window.Create(nullptr, std::move(callbacks)), "create isolated task-binding window");
    ops::OpStatus status;
    status.task_id = displayed; status.active = true; status.type = ops::OpType::Copy;
    status.phase = ops::OpPhase::Running; status.percent = 25; status.total_items = 100;
    window.Update(status); window.Show(false);
    const auto pause = Access::Button(window, 3);
    Check(pause.x >= 0, "displayed transfer exposes pause control");
    Click(window, pause);
    Check(paused == displayed, "pause binds displayed task despite changed backend current task");
    status.phase = ops::OpPhase::Paused; status.pause_requested = true;
    window.Update(status);
    Click(window, Access::Button(window, 3));
    Check(resumed == displayed, "resume binds displayed task despite changed backend current task");
    SendMessageW(Access::Handle(window), WM_SYSCOMMAND, SC_MINIMIZE, 0);
    Check(dismissed == displayed, "minimize dismisses displayed task not backend current task");
    return failures ? 1 : 0;
}
int TaskHandoffCases() {
    using namespace pulse;
    using Access = ui::FileOperationWindowTestAccess;
    constexpr uint64_t copied = 12001, deleting = 12002;
    ops::OpStatus copy;
    copy.task_id = copied; copy.type = ops::OpType::Copy; copy.active = true;
    copy.phase = ops::OpPhase::Running; copy.percent = 40; copy.enumeration_complete = true;
    copy.total_items = 20; copy.completed_items = 8;
    auto complete = copy; complete.active = false; complete.phase = ops::OpPhase::Completed;
    complete.percent = 100; complete.completed_items = complete.total_items;
    auto deletion = copy; deletion.task_id = deleting; deletion.type = ops::OpType::RealDelete;
    deletion.percent = 100; deletion.total_items = 1; deletion.completed_items = 1;
    deletion.enumeration_complete = false; // One selected folder is not proof its children are gone.
    const auto create = [&](ui::FileOperationWindow& window, ui::FileOperationCallbacks callbacks = {}) {
        const bool made = window.Create(nullptr, std::move(callbacks));
        Check(made, "create real progress window for copy-to-delete handoff on private desktop");
        if (made) { Access::NoFlight(window); window.Update(copy); window.Show(false, false); }
        return made;
    };
    Check(!ui::TransferCanAutoClose(deletion), "an active delete at 100 percent cannot be auto-completed");
    Check(!ui::TransferNearDoneSuppressesAutoShow(deletion), "an active folder delete cannot lose progress UI because top-level progress is 100 percent");
    auto recycle = deletion; recycle.type = ops::OpType::RecycleDelete;
    Check(!ui::TransferNearDoneSuppressesAutoShow(recycle), "active recycle deletion also bypasses the nearly-done popup suppression");
    auto active_terminal = deletion; active_terminal.phase = ops::OpPhase::Completed;
    Check(!ui::TransferTerminal(active_terminal), "a terminal-looking phase is not terminal while the task remains active");
    auto almost_copied = copy; almost_copied.percent = 95;
    Check(ui::TransferNearDoneSuppressesAutoShow(almost_copied), "the existing nearly-finished copy popup policy remains unchanged");
    {
        std::vector<uint64_t> dismissed;
        ui::FileOperationCallbacks callbacks;
        callbacks.dismiss_task = [&](uint64_t task) { dismissed.push_back(task); };
        ui::FileOperationWindow window;
        if (!create(window, std::move(callbacks))) return 1;
        window.Update(deletion);
        Access::ArmOldDeadline(window, copied);
        Check(!Access::SuccessPulse(window) && window.IsVisible() && dismissed.empty(),
            "a stale copy close deadline cannot dismiss the running delete task");
        window.Show(false, false); window.Update(deletion); dismissed.clear();
        Access::ArmOldDeadline(window, deleting);
        Check(!Access::SuccessPulse(window) && window.IsVisible() && dismissed.empty(),
            "even a matching deadline cannot auto-close an active delete");
    }
    {
        int reached = 0, located = 0;
        ui::FileOperationCallbacks callbacks; callbacks.reached_status = [&] { ++reached; };
        callbacks.locate_results = [&](uint64_t) { ++located; };
        ui::FileOperationWindow window;
        if (!create(window, std::move(callbacks))) return 1;
        window.Update(complete);
        Access::ClosingForFixture(window, true);
        Access::PendingLocate(window, copied);
        Check(window.CanFollowNextTask(), "a successful card still closing can hand off to the next active operation");
        window.Update(deletion);
        Check(window.IsVisible() && Access::Task(window) == deleting && Access::Active(window),
            "new delete status cancels the previous copy auto-close before advancing its old animation");
        Access::ZoomPulse(window);
        Check(window.IsVisible() && reached == 0 && located == 0,
            "late old zoom ticks cannot hide deletion, pop the old pill, or locate the previous copy");
    }
    {
        ui::FileOperationWindow window;
        if (!create(window)) return 1;
        window.Update(complete);
        Access::ClosingForFixture(window, false);
        Check(!window.CanFollowNextTask(), "a manual hide is not treated as an automatic success handoff");
        window.Update(deletion);
        Check(!window.IsVisible(), "new status does not reverse the user's manual hide intent");
    }
    {
        ui::FileOperationWindow window;
        if (!create(window)) return 1;
        window.Update(complete); Access::ClosingForFixture(window, true);
        auto resumed = copy;
        window.Update(resumed);
        Check(window.IsVisible() && Access::Active(window) && Access::Task(window) == copied,
            "a task that becomes active again cannot be hidden by its previous success animation");
    }
    {
        std::vector<uint64_t> dismissed;
        ui::FileOperationWindow window;
        ui::FileOperationCallbacks callbacks;
        callbacks.dismiss_task = [&](uint64_t task) {
            dismissed.push_back(task);
            if (task == copied) window.Update(deletion);
        };
        if (!create(window, std::move(callbacks))) return 1;
        window.Update(complete); Access::ArmOldDeadline(window, copied);
        Access::SuccessPulse(window);
        Check(window.IsVisible() && Access::Task(window) == deleting && Access::Active(window) &&
            dismissed == std::vector<uint64_t>{copied}, "reentrant completion callbacks cannot close the replacement delete task");
    }
    {
        std::vector<uint64_t> dismissed;
        ui::FileOperationWindow window;
        ui::FileOperationCallbacks callbacks; callbacks.dismiss_task = [&](uint64_t task) { dismissed.push_back(task); };
        if (!create(window, std::move(callbacks))) return 1;
        window.Update(complete); window.SetOtherTasks({deletion});
        Access::ArmOldDeadline(window, copied);
        Check(!Access::SuccessPulse(window) && window.IsVisible() && dismissed.empty(),
            "a completed copy cannot auto-close while its strip still contains active deletion");
        window.Show(false, false); window.SetOtherTasks({});
        const auto deadline = Access::Deadline(window);
        Check(deadline != 0 && !Access::SuccessPulse(window, deadline - 1) && window.IsVisible(),
            "success retains its existing completion hold instead of closing early");
        Check(deadline != 0 && Access::SuccessPulse(window, deadline) && !window.IsVisible() &&
            dismissed == std::vector<uint64_t>{copied}, "automatic close applies only to the completed task after all other work ends");
        Check(!Access::SuccessPulse(window, deadline + 1) && dismissed.size() == 1, "repeated timer ticks never dismiss completion twice");
    }
    {
        int reached = 0;
        ui::FileOperationCallbacks callbacks; callbacks.reached_status = [&] { ++reached; };
        ui::FileOperationWindow window;
        if (!create(window, std::move(callbacks))) return 1;
        auto queued = deletion; queued.phase = ops::OpPhase::Queued; queued.percent = -1;
        window.Update(complete); Access::ClosingForFixture(window, true);
        window.SetOtherTasks({queued});
        Check(window.IsVisible() && Access::Deadline(window) == 0,
            "a queued deletion arriving in the task strip cancels an automatic success flight");
        Access::ZoomPulse(window);
        Check(window.IsVisible() && reached == 0, "a cancelled success flight cannot later hide queued work");
        Access::ClosingForFixture(window, false); window.SetOtherTasks({queued});
        Check(!window.IsVisible(), "task-strip updates do not reverse a manual hide");
    }
    {
        ui::FileOperationWindow window;
        if (!create(window)) return 1;
        auto another_complete = complete; another_complete.task_id = deleting;
        window.Update(another_complete); Access::ArmOldDeadline(window, copied);
        Check(!Access::SuccessPulse(window) && window.IsVisible(),
            "a stale success deadline cannot close a different task even when both tasks completed");
    }
    {
        uint64_t dismissed = 0;
        ui::FileOperationWindow window;
        ui::FileOperationCallbacks callbacks;
        callbacks.dismiss_task = [&](uint64_t task) { dismissed = task; window.Destroy(); };
        if (!create(window, std::move(callbacks))) return 1;
        window.Update(complete); Access::ArmOldDeadline(window, copied);
        Check(Access::SuccessPulse(window) && Access::Handle(window) == nullptr && dismissed == copied,
            "success dismissal safely stops if its callback destroys the progress window");
    }
    {
        ui::FileOperationWindow window;
        ui::FileOperationCallbacks callbacks;
        callbacks.dismiss_task = [&](uint64_t) { window.SetOtherTasks({deletion}); };
        if (!create(window, std::move(callbacks))) return 1;
        window.Update(complete); Access::ArmOldDeadline(window, copied);
        Check(Access::SuccessPulse(window) && window.IsVisible() && Access::Deadline(window) == 0,
            "success dismissal rechecks other active work added by its callback");
    }
    {
        ui::FileOperationWindow window;
        if (!create(window)) return 1;
        auto failed = deletion; failed.active = false; failed.phase = ops::OpPhase::Failed;
        failed.last_error = L"fixture failure"; failed.failed_items = 1;
        window.Update(failed); window.Show(false, false);
        Access::ArmOldDeadline(window, deleting);
        Check(!Access::SuccessPulse(window) && window.IsVisible(), "failed deletion is not hidden by a stale success timer");
    }
    std::printf("TASK_HANDOFF_RESULT failures=%d\n", failures);
    return failures ? 1 : 0;
}
int TerminalCases(const std::filesystem::path& output) {
    using namespace pulse;
    using Access = ui::FileOperationWindowTestAccess;
    l10n::SetLanguage(L"zh-CN");
    if (!output.empty()) std::filesystem::create_directories(output);
    int dismissed = 0, skipped = 0, cancelled = 0;
    uint64_t skip_task = 0, skip_token = 0;
    ui::FileOperationCallbacks callbacks;
    callbacks.dismiss = [&] { ++dismissed; };
    callbacks.cancel = [&] { ++cancelled; };
    callbacks.skip_current = [&](uint64_t task, uint64_t token) { ++skipped; skip_task = task; skip_token = token; };
    ui::FileOperationWindow window;
    Check(window.Create(nullptr, std::move(callbacks)), "create terminal-state window on isolated desktop");
    window.SetAutoClose(true);
    ops::OpStatus status;
    status.task_id = 901; status.type = ops::OpType::EmptyRecycle;
    status.active = true; status.phase = ops::OpPhase::Running;
    status.percent = -1; status.enumeration_complete = false;
    status.total_items = 36;
    window.Update(status); window.Show(false);
    Check(Access::Indeterminate(window) && Access::EmptyingSubtitle(window), "active unknown progress still shows preparation and live subtitle");
    status.active = false; status.phase = ops::OpPhase::Failed; status.completed_items = 36;
    status.last_error = L"Test failure: recycle bin unavailable";
    window.Update(status);
    Check(!Access::Indeterminate(window),
        "failed percent minus one is terminal and never starts auto-close");
    Check(!Access::EmptyingSubtitle(window), "failed empty recycle hides permanently deleting subtitle");
    Check(window.IsVisible() && dismissed == 0, "real failure stays visible after close deadline");
    for (bool dark : {false, true}) {
        window.SetTheme(dark, ui::HexColor(0x0078D4));
        Access::Size(window, 96);
        if (!output.empty()) Check(Access::Snapshot(window, output / (dark ? L"empty-failed-dark.png" : L"empty-failed-light.png")),
            "capture failed empty recycle with 36 of 36 and unknown percent");
    }
    status.phase = ops::OpPhase::Completed; status.last_error.clear(); status.failed_items = 1;
    window.Update(status);
    Check(window.IsVisible(), "completion containing failed items cannot auto-close");
    status.task_id = 902; status.active = true; status.phase = ops::OpPhase::Running;
    status.completed_items = 0; status.failed_items = 0; status.percent = -1;
    window.Update(status);
    status.active = false; status.phase = ops::OpPhase::Completed; status.completed_items = 36;
    window.Update(status);
    Check(!Access::Indeterminate(window) && !window.IsVisible() && dismissed == 1,
        "successful completion closes immediately despite stale enumeration flag");
    Check(!Access::EmptyingSubtitle(window), "successful empty recycle hides permanently deleting subtitle");
    window.Update(status);
    Check(dismissed == 1, "repeated success updates do not dismiss twice");
    for (auto type : {ops::OpType::Copy, ops::OpType::Move, ops::OpType::RealDelete, ops::OpType::RecycleDelete}) {
        ++status.task_id; status.type = type; status.active = true; status.phase = ops::OpPhase::Running;
        window.Update(status); window.Show(false);
        status.active = false; status.phase = ops::OpPhase::Completed;
        Access::Press(window, true);
        window.Update(status);
        Check(!window.IsVisible(), "copy move and delete close immediately even with mouse held");
        Access::Press(window, false);
    }
    ++status.task_id; status.active = true; status.phase = ops::OpPhase::Running;
    window.Update(status); window.Show(false); ShowWindow(Access::Handle(window), SW_MINIMIZE);
    status.active = false; status.phase = ops::OpPhase::Completed; window.Update(status);
    Check(!window.IsVisible(), "success while minimized also closes immediately");
    window.SetAutoClose(false); window.Show(false); window.Update(status);
    Check(window.IsVisible(), "explicit completed summary remains visible");
    status.active = true; status.phase = ops::OpPhase::Running; status.percent = 25;
    status.current_item_token = 123; status.can_pause = true; status.type = ops::OpType::Copy;
    window.Update(status); window.Show(false);
    for (const wchar_t* locale : {L"zh-CN", L"en-US", L"zh-TW"}) {
        l10n::SetLanguage(locale);
        for (UINT dpi : {96u, 144u, 192u}) {
            RECT rectangle{0, 0, MulDiv(440, dpi, 96), MulDiv(272, dpi, 96)};
            SendMessageW(Access::Handle(window), WM_DPICHANGED, MAKELONG(dpi, dpi), reinterpret_cast<LPARAM>(&rectangle));
            for (bool dark : {false, true}) {
                window.SetTheme(dark, ui::HexColor(0x0078D4));
                const auto point = Access::Button(window, 9);
                Check(point.x >= 0, "skip button has a reachable hit target at actual window width");
                int skip_width = 0;
                Access::Button(window, 9, &skip_width);
                Check(skip_width + 2 >= Access::Width(window, l10n::StringId::OpSkipCurrentItem),
                    "skip button is not overlapped by adjacent controls");
                if (!output.empty()) Access::Snapshot(window, output / (std::wstring(L"skip-") + locale + L"-" +
                    std::to_wstring(dpi) + (dark ? L"-dark.png" : L"-light.png")));
            }
        }
    }
    Click(window, Access::Button(window, 9));
    Check(skipped == 1 && cancelled == 0 && skip_task == status.task_id && skip_token == 123,
        "skip dispatches the displayed item identity without cancelling the task");
    status.current_item_token = 0; window.Update(status);
    Check(Access::Button(window, 9).x < 0, "skip is disabled when no single item can be cancelled");
    return failures ? 1 : 0;
}
}

namespace {
constexpr UINT kPresentationLoad = WM_APP + 0x361;
constexpr UINT kPresentationInput = WM_APP + 0x362;
struct PresentationLoad {
    pulse::ui::FileOperationWindow* window;
    pulse::ui::FileOperationWindow* frame_host;
    pulse::ops::OpStatus status;
    ULONGLONG until = 0, last_update = 0;
    unsigned posted = 0, paints = 0, timers = 0, inputs = 0;
    ULONGLONG input_max = 0;
};
LRESULT CALLBACK PresentationLoadProc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR data) {
    auto& load = *reinterpret_cast<PresentationLoad*>(data);
    if (message == kPresentationInput) {
        ++load.inputs;
        load.input_max = (std::max)(load.input_max, GetTickCount64() - static_cast<ULONGLONG>(wp));
        return 0;
    }
    if (message == kPresentationLoad) {
        ++load.posted;
        const auto frame_started = GetTickCount64();
        pulse::ui::FileOperationWindowTestAccess::RenderFrame(*load.frame_host);
        const auto render_elapsed = GetTickCount64() - frame_started;
        if (render_elapsed < 16) Sleep(static_cast<DWORD>(16 - render_elapsed)); // test-only display cadence
        const auto now = GetTickCount64();
        if (now - load.last_update >= 16) {
            load.last_update = now;
            load.status.percent = static_cast<float>((load.posted % 80) + 1);
            load.window->Update(load.status);
        }
        if (now < load.until) {
            PostMessageW(hwnd, kPresentationInput, static_cast<WPARAM>(GetTickCount64()), 0);
            PostMessageW(hwnd, kPresentationLoad, 0, 0);
        }
        return 0;
    }
    if (message == WM_PAINT) ++load.paints;
    if (message == WM_TIMER) ++load.timers;
    return DefSubclassProc(hwnd, message, wp, lp);
}
int PresentationStarvation() {
    using namespace pulse;
    using Access = ui::FileOperationWindowTestAccess;
    ui::FileOperationWindow window;
    Check(window.Create(nullptr, {}), "create real operation window for posted-load regression");
    ops::OpStatus status;
    status.task_id = 876543; status.active = true;
    status.type = ops::OpType::Copy; status.phase = ops::OpPhase::Running; status.percent = 25;
    window.Update(status); window.Show(false, false);
    const auto hwnd = Access::Handle(window);
    UpdateWindow(hwnd);
    ui::FileOperationWindow frame_host;
    Check(frame_host.Create(nullptr, {}), "create independent compositor for finite display-frame load");
    frame_host.Update(status); frame_host.Show(false, false);
    PresentationLoad load{&window, &frame_host, status};
    SetWindowSubclass(hwnd, PresentationLoadProc, 1, reinterpret_cast<DWORD_PTR>(&load));
    load.until = GetTickCount64() + 1200;
    PostMessageW(hwnd, kPresentationLoad, 0, 0);
    while (GetTickCount64() < load.until) {
        MSG message{};
        if (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message); DispatchMessageW(&message);
        } else Sleep(1);
    }
    RemoveWindowSubclass(hwnd, PresentationLoadProc, 1);
    std::printf("POSTED_LOAD messages=%u paints=%u timers=%u duration_ms=1200\n", load.posted, load.paints, load.timers);
    Check(load.posted >= 20 && load.posted <= 90, "bounded display-frame messages remain serviced during finite load");
    Check(load.paints >= 8, "real operation window continues painting during sustained posted load");
    std::printf("POSTED_INPUT samples=%u max_ms=%llu\n", load.inputs, load.input_max);
    Check(load.inputs >= 20 && load.input_max < 250, "presentation pulses leave posted-input service within bounded frame budget");
    load.paints = 0;
    SetWindowSubclass(hwnd, PresentationLoadProc, 1, reinterpret_cast<DWORD_PTR>(&load));
    const auto idle_end = GetTickCount64() + 1200;
    while (GetTickCount64() < idle_end) {
        MSG message{};
        if (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message); DispatchMessageW(&message);
        } else Sleep(1);
    }
    RemoveWindowSubclass(hwnd, PresentationLoadProc, 1);
    std::printf("IDLE_PRESENTATION paints=%u duration_ms=1200\n", load.paints);
    Check(load.paints <= 12, "dirty-driven determinate timer does not flood idle repaints");
    return failures ? 1 : 0;
}
int PresentationDiagnostics(const std::filesystem::path& root) {
    using namespace pulse;
    using Access = ui::FileOperationWindowTestAccess;
    std::filesystem::create_directories(root);
    ui::FileOperationWindow window;
    Check(window.Create(nullptr, {}), "diagnostic real operation window created on private desktop");
    ops::OpStatus status;
    status.task_id = 987654;
    status.type = ops::OpType::Copy;
    status.active = true;
    status.phase = ops::OpPhase::Running;
    status.percent = 25;
    window.Update(status);
    window.Show(false, false);
    const auto pump = [](DWORD duration) {
        const auto end = GetTickCount64() + duration;
        do {
            MSG msg{};
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg); DispatchMessageW(&msg);
            }
            Sleep(1);
        } while (GetTickCount64() < end);
    };
    pump(600); // compositor/font startup is outside the healthy interval
    Check(diagnostics::runtime::Initialize(root.wstring(), "app"), "private diagnostic runtime initialized");
    pump(180);
    const auto flush = [] { std::thread writer([] { diagnostics::runtime::Flush(5000); }); writer.join(); };
    flush();
    const auto read = [&] {
        std::ifstream input(root / L"Diagnostics" / L"Runtime" / (L"app-" + std::to_wstring(GetCurrentProcessId()) + L".jsonl"));
        return std::string(std::istreambuf_iterator<char>(input), {});
    };
    Check(read().find("operation_window_slow") == std::string::npos, "healthy operation-window pumping has no slow-gap report");
    window.Hide();
    Sleep(350);
    window.Show(false, false);
    pump(100);
    flush();
    Check(read().find("\"kind\":1") == std::string::npos && read().find("\"kind\":2") == std::string::npos,
          "hidden interval and reshow do not produce false presentation gaps");
    status.active = false;
    window.Update(status);
    Sleep(350);
    status.active = true;
    window.Update(status);
    pump(100);
    flush();
    Check(read().find("\"kind\":1") == std::string::npos && read().find("\"kind\":2") == std::string::npos,
          "inactive to active transition resets presentation sampling");
    Sleep(350); // deliberate test-only UI blockage, not a production delay
    pump(80);
    flush();
    const auto text = read();
    Check(text.find("operation_window_slow") != std::string::npos && text.find("\"task\":987654") != std::string::npos,
          "injected delay records actual operation-window gap and task identity");
    Check(text.find("\"kind\":1") != std::string::npos, "render-timer starvation is identified separately");
    diagnostics::runtime::Shutdown();
    return failures ? 1 : 0;
}
}

int wmain(int argc, wchar_t** argv) {
    using namespace pulse;
    using Access = ui::FileOperationWindowTestAccess;
    compat::EnableDpiAwareness();
    if (argc > 1 && (std::wstring_view(argv[1]) == L"--recursive-eta" || std::wstring_view(argv[1]) == L"--terminal" || std::wstring_view(argv[1]) == L"--task-binding" || std::wstring_view(argv[1]) == L"--task-handoff" || std::wstring_view(argv[1]) == L"--diagnostics" || std::wstring_view(argv[1]) == L"--starvation")) {
        const std::filesystem::path output = argc > 2 ? argv[2] : L"";
        const auto name = L"PulseTransferTerminal-" + std::to_wstring(GetCurrentProcessId());
        HDESK desktop = CreateDesktopW(name.c_str(), nullptr, nullptr, 0, GENERIC_ALL, nullptr);
        if (!desktop) return 2;
        int result = 2;
        std::thread worker([&] {
            if (!SetThreadDesktop(desktop)) return;
            CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
            l10n::Initialize(GetModuleHandleW(nullptr), L"zh-CN");
            result = std::wstring_view(argv[1]) == L"--recursive-eta" ? RecursiveEtaDisplayCases() :
                std::wstring_view(argv[1]) == L"--task-handoff" ? TaskHandoffCases() :
                std::wstring_view(argv[1]) == L"--starvation" ? PresentationStarvation() :
                std::wstring_view(argv[1]) == L"--diagnostics" ? PresentationDiagnostics(output) :
                std::wstring_view(argv[1]) == L"--task-binding" ? TaskBindingCases() : TerminalCases(output);
            CoUninitialize();
        });
        worker.join(); CloseDesktop(desktop); return result;
    }
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    l10n::Initialize(GetModuleHandleW(nullptr), L"en-US");
    std::filesystem::path output;
    if (argc > 1) { output = argv[1]; std::filesystem::create_directories(output); }
    {
        uint64_t retry = 0, skip = 0;
        int cancel = 0, pause = 0;
        std::wstring handed_off;
        uint64_t stopped_task = 0;
        ui::FileOperationCallbacks callbacks;
        callbacks.open_native_folder = [&](const std::wstring& path) { handed_off = path; };
        callbacks.cancel_task = [&](uint64_t id) { ++cancel; stopped_task = id; };
        callbacks.retry_authorization = [&](uint64_t id) { retry = id; };
        callbacks.skip_authorization = [&](uint64_t id) { skip = id; };
        callbacks.cancel = [&] { ++cancel; };
        callbacks.pause = [&] { ++pause; };
        ui::FileOperationWindow window;
        Check(window.Create(nullptr, std::move(callbacks)), "create isolated hidden operation window");
        ops::OpStatus status;
        status.task_id = 73;
        status.active = true;
        status.type = ops::OpType::Copy;
        status.phase = ops::OpPhase::Running;
        status.source_label = L"Downloads";
        status.destination_label = L"Program Files";
        status.current_item = L"C:\\Program Files\\Example\\settings.json";
        status.authorization = ops::AuthorizationState::ActionRequired;
        status.can_skip_authorization = true;
        for (const wchar_t* language : {L"en-US", L"zh-CN", L"zh-TW"}) {
            l10n::SetLanguage(language);
            for (UINT dpi : {96u, 144u, 192u}) {
                Access::Size(window, dpi);
                for (bool dark : {false, true}) {
                    window.SetTheme(dark, ui::HexColor(0x0078D4));
                    window.Update(status);
                    Check(std::abs(Access::ClientHeight(window) - MulDiv(196, dpi, 96)) <= 2,
                        "the permission card is only as tall as its message (196 DIP)");
                    int retry_pixels = 0, skip_pixels = 0;
                    const auto retry_button = Access::Button(window, 6, &retry_pixels);
                    const auto skip_button = Access::Button(window, 7, &skip_pixels);
                    const auto cancel_button = Access::Button(window, 4);
                    // 跳过此项 on the left, then 重试授权 and 取消 on the right.
                    Check(skip_button.x >= 0 && skip_button.x < retry_button.x &&
                        retry_button.x < cancel_button.x &&
                        retry_pixels >= std::floor(Access::Width(window, l10n::StringId::OpRetryAuthorization)) &&
                        skip_pixels >= std::floor(Access::Width(window, l10n::StringId::OpSkipAuthorization)),
                        "authorization buttons fit minimum width without overlap");
                    Check(Access::Button(window, 3).x < 0 && Access::Button(window, 5).x < 0,
                        "authorization hides pause and details");
                    retry = skip = 0;
                    Click(window, retry_button);
                    Click(window, skip_button);
                    Check(retry == 73 && skip == 73, "retry and skip route the displayed task id");
                    const std::wstring stem = std::wstring(language) + L"-" + std::to_wstring(dpi) +
                        (dark ? L"-dark" : L"-light");
                    if (!output.empty()) Check(Access::Snapshot(window, output / (stem + L"-action.png")), "save action screenshot");
                    status.authorization = ops::AuthorizationState::Requesting;
                    window.Update(status);
                    Check(Access::Button(window, 6).x < 0 && Access::Button(window, 7).x < 0,
                        "requesting cannot retry or skip");
                    if (!output.empty()) Check(Access::Snapshot(window, output / (stem + L"-requesting.png")), "save requesting screenshot");
                    status.authorization = ops::AuthorizationState::ActionRequired;
                }
            }
        }
        status.can_skip_authorization = false;
        window.Update(status);
        Check(Access::Button(window, 7).x < 0 && Access::Button(window, 6).x >= 0,
            "single remaining item can suppress skip");
        const POINT old_retry = Access::Button(window, 6);
        SendMessageW(Access::Handle(window), WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(old_retry.x, old_retry.y));
        ++status.task_id;
        window.Update(status);
        retry = 0;
        SendMessageW(Access::Handle(window), WM_LBUTTONUP, 0, MAKELPARAM(old_retry.x, old_retry.y));
        Check(retry == 0, "task switch invalidates an in-flight authorization click");
        l10n::SetLanguage(L"en-US");
        Access::Size(window, 96);
        status.last_error = L"Administrator authorization was canceled. The pending item has not been changed. "
                            L"You can retry authorization, skip this item, or cancel the operation.";
        window.Update(status);
        if (!output.empty()) Check(Access::Snapshot(window, output / L"en-US-reason.png"), "save detailed authorization reason");
        status.phase = ops::OpPhase::Cancelling;
        window.Update(status);
        Check(Access::Button(window, 6).x < 0 && Access::Button(window, 7).x < 0,
            "cancelling hides authorization actions");
        status.phase = ops::OpPhase::Running;
        status.authorization = ops::AuthorizationState::None;
        status.can_pause = true;
        window.Update(status);
        // A retry that fails again returns within milliseconds; the dialog
        // must not flash the progress view for such a brief exit.
        Check(Access::Button(window, 3).x < 0, "brief authorization exit keeps the authorization view");
        Sleep(450);
        window.Update(status);
        Check(Access::Button(window, 3).x >= 0 && Access::Button(window, 5).x >= 0 &&
            Access::Button(window, 6).x < 0 && Access::Button(window, 7).x < 0,
            "normal operation restores original controls");
        Click(window, Access::Button(window, 3));
        status.authorization = ops::AuthorizationState::ActionRequired;
        window.Update(status);
        Click(window, Access::Button(window, 4));
        Check(pause == 1 && cancel == 1, "normal pause and authorization cancel route unchanged");
        const POINT enabled_retry = Access::Button(window, 6);
        status.authorization_retrying = true;
        window.Update(status);
        retry = 0;
        if (enabled_retry.x >= 0) Click(window, enabled_retry);
        Check(enabled_retry.x >= 0 && retry == 0 && Access::Button(window, 6).x < 0 &&
              Access::Button(window, 3).x < 0,
              "retry in flight keeps the authorization view and ignores clicks");
        status.authorization_retrying = false;
        status.native_handoff_directory = L"C:\\fixture";
        status.can_skip_authorization = true;
        window.Update(status);
        Sleep(450);
        window.Update(status);
        for (const wchar_t* lang : {L"en-US", L"zh-CN", L"zh-TW"}) {
            l10n::SetLanguage(lang);
            for (UINT dpi : {96u, 192u}) {
                Access::Size(window, dpi, 440);
                for (const auto& [id, label] : {
                    std::pair{6, l10n::StringId::OpRetryAuthorization}, {7, l10n::StringId::OpSkipAuthorization},
                    {8, l10n::StringId::SettingsSecExplorer}, {4, l10n::StringId::Cancel}}) {
                    int pixels = 0;
                    Access::Button(window, id, &pixels);
                    Check(pixels >= std::floor(Access::Width(window, label)),
                        "native handoff footer fits all four actions at minimum width");
                }
            }
        }
        Click(window, Access::Button(window, 8));
        Check(handed_off == status.native_handoff_directory && stopped_task == status.task_id,
            "native handoff stops the waiting task and opens its folder without retrying");
        handed_off.clear(); stopped_task = 0;
        status.type = ops::OpType::RecycleDelete;
        status.active = false;
        status.authorization = ops::AuthorizationState::None;
        status.phase = ops::OpPhase::Failed;
        window.Update(status);
        Click(window, Access::Button(window, 8));
        Check(handed_off == status.native_handoff_directory && !stopped_task,
            "terminal deletion hands off source parent even without a destination");
        status.native_handoff_directory.clear();
        window.Update(status);
        Check(Access::Button(window, 8).x < 0, "no native action without a handoff target");
        Check(!window.IsVisible(), "test never displays or activates a user window");
    }
    CoUninitialize();
    return failures ? 1 : 0;
}
