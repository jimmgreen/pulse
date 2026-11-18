#include "task_probe.h"
#include "command_contributions.h"
#include "package_commands.h"
#include "session.h"
#include "../runtime.h"
#include "../../app_runtime.h"
#include "../../app_ops_ui.h"
#include <filesystem>
#include <functional>
#include <ostream>

namespace pulse::ui {
float OperationDisplayPercent(const ops::OpStatus&);
std::wstring ExternalOperationDisplayTitle(const ops::OpStatus&);
struct NativeTaskProbe {
    static bool Capture(FileOperationWindow& view, const std::filesystem::path& path,
                        const ops::OpStatus& expected, std::ostream& report) {
        const bool copied_frames = Compositor::FrameCopyEnabled();
        Compositor::SetFrameCopyEnabled(true);
        struct RestoreFrameCopy {
            bool enabled;
            ~RestoreFrameCopy() { Compositor::SetFrameCopyEnabled(enabled); }
        } restore{copied_frames};
        const auto previous_frame = view.compositor_.FrameCopySerial();
        // Finish only the presentation flight; the worker and its progress are
        // untouched, so snapshots never capture the transient zoom overlay.
        view.FinishShowFromStatus();
        view.Render();
        const auto& shown = view.status_;
        const float percent = OperationDisplayPercent(shown);
        report << "Rendered task " << shown.task_id << ": percent=" << percent << ", status.percent=" << shown.percent
            << ", ops.percent=" << expected.percent << ", outcome=" << static_cast<int>(shown.external_outcome) << '\n';
        const bool terminal_matches = expected.active ||
            (shown.external_outcome == expected.external_outcome && shown.phase == expected.phase &&
             shown.percent == expected.percent && shown.summary == expected.summary && shown.last_error == expected.last_error);
        const bool progress_matches = percent == shown.percent;
        const bool title_matches = !shown.external_title.empty() &&
            ExternalOperationDisplayTitle(shown).starts_with(shown.external_title);
        return view.compositor_.FrameCopy() && view.compositor_.FrameCopySerial() > previous_frame &&
            shown.task_id == expected.task_id && shown.external && terminal_matches && progress_matches && title_matches &&
            view.compositor_.SaveSnapshot(path.c_str());
    }
    static bool Cancel(FileOperationWindow& view, uint64_t id) {
        if (view.status_.task_id != id || !view.status_.active) return false;
        view.RequestCancel();
        return true;
    }
};
}
namespace pulse::extensions::native {
namespace {
bool Wait(const std::function<bool()>& ready) {
    const auto until = GetTickCount64() + 30000;
    do {
        if (ready()) return true;
        MSG message{};
        for (unsigned i = 0; i < 32 && PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE); ++i) {
            if (message.message == WM_QUIT) { PostQuitMessage(static_cast<int>(message.wParam)); return false; }
            TranslateMessage(&message); DispatchMessageW(&message);
        }
        MsgWaitForMultipleObjectsEx(0, nullptr, 1, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    } while (GetTickCount64() < until);
    return ready();
}
}
bool RunArchiveTaskProbe(AppState& owner, const std::wstring& directory, std::ostream& report) {
    const std::filesystem::path output(directory), profile = output.parent_path();
    const auto fixture = profile / "fixture", temporary = profile / "temporary";
    if (!std::filesystem::exists(fixture / "progress.7z")) return true;
    bool passed = true;
    const auto check = [&](bool ok, const char* text) {
        report << (ok ? "[PASS] " : "[FAIL] ") << text << '\n'; report.flush(); passed = passed && ok; return ok;
    };
    const auto clean = [&] {
        for (const auto& entry : std::filesystem::directory_iterator(temporary))
            if (entry.path().filename().wstring().starts_with(L"PulseArchiveVerify-")) return false;
        return true;
    };
    const auto start = [&](const wchar_t* name) -> uint64_t {
        auto* tab = ActiveTab(owner);
        if (!tab) return 0;
        bool selected = false;
        for (size_t i = 0; i < tab->EntryCount(); ++i) if (tab->EntryAt(i).name == name) {
            tab->SelectOnly(static_cast<int>(i)); selected = true; break;
        }
        if (!selected) return 0;
        uint64_t previous = owner.ops.Status().task_id;
        for (const auto& row : CommandRows(owner))
            if (CommandIdentity(owner, row.command) == L"native:sample.native-archive:verify-archive") {
                if (!HandleContributedCommand(owner, row.command)) return 0;
                uint64_t id = 0;
                Wait([&] { const auto status = owner.ops.Status();
                    if (status.external && status.task_id != previous) id = status.task_id;
                    return id != 0; });
                return id;
            }
        return 0;
    };
    const auto finish = [&](uint64_t id, ops::ExternalOutcome outcome) {
        return id && Wait([&] { const auto status = owner.ops.QueryTask(id);
            return status && !status->active && status->external_outcome == outcome; }) && clean();
    };
    const auto capture = [&](uint64_t id, const wchar_t* name) {
        UpdateOperationWindow(owner, false);
        PinAndShowOperationWindow(owner);
        if (!owner.operationWindow) return false;
        owner.operationWindow->SetAutoClose(false);
        const auto expected = owner.ops.QueryTask(id);
        return expected && ui::NativeTaskProbe::Capture(*owner.operationWindow, output / name, *expected, report);
    };
    const auto success = start(L"sample.7z");
    check(finish(success, ops::ExternalOutcome::Completed), "real 7z extraction completes and removes private output");
    const auto completed = owner.ops.QueryTask(success);
    check(completed && completed->transferred_bytes == 13 && completed->completed_items == 1,
        "successful task retains real extracted byte and item counts");
    check(capture(success, L"archive-task-success.png"), "existing operation card displays successful native task");
    const auto rar = start(L"ordinary.rar");
    check(finish(rar, ops::ExternalOutcome::Completed), "real RAR extraction completes and cleans private output");
    const auto failed = start(L"corrupt.7z");
    check(finish(failed, ops::ExternalOutcome::Failed), "corrupt archive fails and cleans private output");
    const auto failure = owner.ops.QueryTask(failed);
    check(failure && !failure->last_error.empty() && capture(failed, L"archive-task-failed.png"), "existing operation card preserves native extraction error");
    const auto running = start(L"progress.7z");
    const bool progress = running && Wait([&] { const auto status = owner.ops.QueryTask(running);
        return status && status->active && status->transferred_bytes > 0 && status->total_bytes > status->transferred_bytes; });
    check(progress, "real archive extraction reports in-flight byte progress");
    if (progress) {
        const auto observed = owner.ops.QueryTask(running);
        report << "Observed extraction bytes: " << observed->transferred_bytes << '/' << observed->total_bytes << '\n';
        UpdateOperationWindow(owner, false);
        RedrawWindow(owner.hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
        check(owner.compositor.SaveSnapshot((output / "archive-task-status-pill.png").c_str()), "capture real native task in main-window status pill");
        check(capture(running, L"archive-task-progress.png"), "existing operation card displays native progress");
    }
    check(finish(running, ops::ExternalOutcome::Completed), "progress screenshot extraction completes and cleans output");
    const auto cancelling = start(L"progress.7z");
    UpdateOperationWindow(owner, false);
    PinAndShowOperationWindow(owner);
    bool requested = false;
    if (cancelling && owner.operationWindow) {
        owner.operationWindow->SetAutoClose(false);
        Wait([&] {
            const auto status = owner.ops.QueryTask(cancelling);
            if (!status || !status->active) return true;
            if (!status->transferred_bytes || status->transferred_bytes >= status->total_bytes) return false;
            report << "Cancel requested at extraction bytes: " << status->transferred_bytes << '/' << status->total_bytes << '\n';
            owner.operationWindow->Update(*status);
            requested = ui::NativeTaskProbe::Cancel(*owner.operationWindow, cancelling);
            return true;
        });
    }
    check(requested && finish(cancelling, ops::ExternalOutcome::Cancelled),
        "operation card cancel handler cancels a separate extraction immediately after first byte progress and cleans output");
    const auto cancelled = owner.ops.QueryTask(cancelling);
    check(cancelled && !cancelled->active && cancelled->external_outcome == ops::ExternalOutcome::Cancelled &&
        cancelled->total_bytes > 0 && cancelled->transferred_bytes < cancelled->total_bytes && cancelled->percent < 100.0f,
        "real mid-extraction cancellation retains incomplete bytes and percentage");
    check(capture(cancelling, L"archive-task-cancelled.png"), "existing operation card displays cancelled native task");
    const auto disabling = start(L"progress.7z");
    const bool active = disabling && Wait([&] { const auto status = owner.ops.QueryTask(disabling);
        return status && status->active && status->transferred_bytes > 0; });
    check(active, "second real extraction is running before package disable");
    HandlePackageCommand(owner, EntriesForPackage(0, L"").back().command);
    check(!PackageEnabled(0) && !PackageActive(owner.hwnd, 0) && finish(disabling, ops::ExternalOutcome::Cancelled),
        "package disable cancels and drains running extraction before unload and removes private output");
    HandlePackageCommand(owner, EntriesForPackage(0, L"").back().command);
    const auto entries = EntriesForPackage(0, L"");
    check(PackageEnabled(0) && !entries.empty() && HandlePackageCommand(owner, entries.front().command), "archive package reopens after task drain");
    if (owner.operationWindow) owner.operationWindow->Hide();
    return passed;
}
}
