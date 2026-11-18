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
    for (int i = 0; i < 20; ++i) { const auto id = manager.CreateExternalTask(L"Task", L"Test"); manager.FinishExternalTask(id, ExternalOutcome::Completed); }
    check(!manager.FinalStatus(first) && manager.RecentStatuses().size() == 16, "external terminal history retains at most sixteen");
    for (int i = 0; i < 32; ++i) check(manager.CreateExternalTask(L"Task", L"Test") != 0, "external active task within bound accepted");
    check(manager.CreateExternalTask(L"Over budget", L"Test") == 0, "external active task limit enforced");
    const auto active = manager.Status().task_id; manager.Stop();
    check(manager.ExternalTaskCancelled(active), "Stop requests external cancellation without callback");
    return failures ? 1 : 0;
}
