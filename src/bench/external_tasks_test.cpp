#include "../ops/ops_manager.h"
#include "../ops/operation_presentation.h"
#include "../ui/file_operation_progress_state.h"
#include <cstdio>
#include <thread>

namespace pulse::ops {
struct OpsManagerAuditAccess {
    static void Builtin(OpsManager& manager, bool active) {
        manager.SetStatus([&](OpStatus& status) {
            status.task_id = 9999;
            status.type = OpType::Copy;
            status.active = active;
            status.phase = active ? OpPhase::Running : OpPhase::Completed;
        });
    }
};
}
int main() {
    using namespace pulse::ops;
    int failures = 0;
    const auto check = [&](bool ok, const char* label) { std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label); if (!ok) ++failures; };
    OpsManager manager;
    const auto first = manager.CreateExternalTask(L"Extract archive", L"Archive");
    const auto second = manager.CreateExternalTask(L"Compress files", L"Archive");
    check(first && second && first != second && manager.HasPendingOperations(), "external IDs are unique and pending blocks update");
    check(!manager.TryPrepareForUpdate(false), "external active tasks block update preparation");
    OpStatus progress; progress.percent = 25; progress.total_bytes = 100; progress.transferred_bytes = 25;
    progress.current_item = L"entry.txt"; progress.can_pause = true; progress.type = OpType::Copy; progress.active = false;
    std::thread worker([&] { manager.UpdateExternalTask(first, progress); }); worker.join();
    auto snapshot = manager.QueryTask(first);
    check(snapshot && snapshot->external && snapshot->active && !snapshot->can_pause && snapshot->type == OpType::External && snapshot->percent == 25,
        "worker progress preserves host identity and controls");
    OpsManagerAuditAccess::Builtin(manager, true);
    check(manager.Status().task_id == 9999, "builtin active operation retains status priority");
    check(manager.CancelTask(second) && manager.ExternalTaskCancelled(second) && !manager.ExternalTaskCancelled(first), "cancel targets exact external ID");
    check(manager.Status().task_id == 9999, "external cancel does not cancel builtin operation");
    OpsManagerAuditAccess::Builtin(manager, false);
    check(manager.Status().task_id == first, "external active task becomes visible after builtin completion");
    manager.CancelCurrent();
    check(manager.ExternalTaskCancelled(first), "cancel current targets the displayed external task");
    check(manager.FinishExternalTask(first, ExternalOutcome::Cancelled, progress), "explicit cancellation publishes terminal snapshot");
    check(!manager.UpdateExternalTask(first, progress) && !manager.FinishExternalTask(first, ExternalOutcome::Completed) && !manager.CancelTask(first), "final snapshot is immutable");
    snapshot = manager.FinalStatus(first);
    check(snapshot && snapshot->external_outcome == ExternalOutcome::Cancelled && !snapshot->active && snapshot->percent == 25 &&
        !pulse::ui::TransferCanAutoClose(*snapshot) && PresentOperationStatus(*snapshot).percent == 25,
        "cancelled terminal never presents fake 100 percent or autoclose");
    check(manager.FinishExternalTask(second, ExternalOutcome::Partial, progress) && !manager.HasPendingOperations(), "partial outcome finishes pending lifecycle");
    check(manager.Status().task_id == second && manager.RecentStatuses().front().task_id == second, "newest external terminal is visible in status and history");
    OpsManagerAuditAccess::Builtin(manager, true); OpsManagerAuditAccess::Builtin(manager, false);
    check(manager.Status().task_id == 9999, "newer builtin terminal takes display priority");
    {
        // A plugin task waiting on a host operation stands in for it while that runs.
        const auto waiting = manager.CreateExternalTask(L"Extract a.7z", L"Archive", L"a.7z", L"D:\\out", {L"Unpack", L"Write"});
        OpStatus step = progress; step.external_step = 2; step.external_linked = 9999; step.external_note = L"undoable";
        manager.UpdateExternalTask(waiting, step);
        OpsManagerAuditAccess::Builtin(manager, true);
        const auto shown = manager.Status();
        check(shown.task_id == waiting && shown.external_step == 2 && shown.source_label == L"a.7z" &&
              shown.destination_label == L"D:\\out" && shown.external_note == L"undoable", "linked plugin task stands in for its running operation");
        step.external_linked = 0; manager.UpdateExternalTask(waiting, step);
        check(manager.Status().task_id == 9999, "unlinked running operation shows itself");
        OpsManagerAuditAccess::Builtin(manager, false);
        int calls = 0; std::string picked;
        manager.SetExternalTaskActionHandler(waiting, [&](const std::string& action) { ++calls; picked = action; });
        check(!manager.InvokeExternalTaskAction(waiting, "retry") && calls == 0, "actions wait until the task has ended");
        OpStatus failed = step; failed.external_actions = {{"retry", L"Retry", true}};
        manager.FinishExternalTask(waiting, ExternalOutcome::Failed, failed);
        const auto final_status = manager.FinalStatus(waiting);
        check(final_status && final_status->external_actions.size() == 1 && final_status->external_linked == 0,
              "terminal snapshot keeps actions and drops the link");
        check(manager.InvokeExternalTaskAction(waiting, "retry") && !manager.InvokeExternalTaskAction(waiting, "retry") &&
              calls == 1 && picked == "retry", "an ended task's action runs once");
    }
    for (int i = 0; i < 20; ++i) { const auto id = manager.CreateExternalTask(L"Task", L"Test"); manager.FinishExternalTask(id, ExternalOutcome::Completed); }
    check(!manager.FinalStatus(first) && manager.RecentStatuses().size() == 16, "external terminal history retains at most sixteen");
    for (int i = 0; i < 32; ++i) check(manager.CreateExternalTask(L"Task", L"Test") != 0, "external active task within bound accepted");
    check(manager.CreateExternalTask(L"Over budget", L"Test") == 0, "external active task limit enforced");
    const auto active = manager.Status().task_id; manager.Stop();
    check(manager.ExternalTaskCancelled(active), "Stop requests external cancellation without callback");
    return failures ? 1 : 0;
}
