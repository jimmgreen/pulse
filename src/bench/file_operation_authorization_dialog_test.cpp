#include "../common/windows_compat.h"
#include "../common/localization.h"
#include "../ui/file_operation_dialog.h"
#include "../ui/file_operation_progress_state.h"

#include <cstdio>
#include <filesystem>
#include <cmath>
#include <thread>

namespace pulse::ui {
struct FileOperationWindowTestAccess {
    static bool Indeterminate(FileOperationWindow& window) { return TransferIndeterminate(window.status_); }
    static bool EmptyingSubtitle(FileOperationWindow& window) { return TransferShowsEmptyingSubtitle(window.status_); }
    static void Press(FileOperationWindow& window, bool pressed) { window.pressed_ = pressed ? 1 : 0; }
    static HWND Handle(FileOperationWindow& window) { return window.hwnd_; }
    static void Size(FileOperationWindow& window, UINT dpi) {
        RECT rect{0, 0, MulDiv(468, dpi, 96), MulDiv(348, dpi, 96)};
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

int wmain(int argc, wchar_t** argv) {
    using namespace pulse;
    using Access = ui::FileOperationWindowTestAccess;
    compat::EnableDpiAwareness();
    if (argc > 1 && std::wstring_view(argv[1]) == L"--terminal") {
        const std::filesystem::path output = argc > 2 ? argv[2] : L"";
        const auto name = L"PulseTransferTerminal-" + std::to_wstring(GetCurrentProcessId());
        HDESK desktop = CreateDesktopW(name.c_str(), nullptr, nullptr, 0, GENERIC_ALL, nullptr);
        if (!desktop) return 2;
        int result = 2;
        std::thread worker([&] {
            if (!SetThreadDesktop(desktop)) return;
            CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
            l10n::Initialize(GetModuleHandleW(nullptr), L"zh-CN");
            result = TerminalCases(output);
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
        ui::FileOperationCallbacks callbacks;
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
        Check(!window.IsVisible(), "test never displays or activates a user window");
    }
    CoUninitialize();
    return failures ? 1 : 0;
}
