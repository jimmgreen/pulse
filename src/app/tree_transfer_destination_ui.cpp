#include "tree_transfer_destination.h"
#include "app_internal.h"
#include "app_ops_ui.h"
#include "arrival_select.h"
#include "../common/localization.h"
#include "../ui/confirm_dialog.h"

namespace pulse {
app::TreeTransferDestinations TreeTransferDestinationsForTab(const app::Tab& tab) {
    std::vector<app::TreeDestinationRow> rows;
    if (tab.TreeEntries()) for (int index : tab.SelectedIndices()) {
        if (index < 0 || static_cast<size_t>(index) >= tab.EntryCount()) continue;
        const auto& entry = tab.EntryAt(static_cast<size_t>(index));
        rows.push_back({EntryFullPath(tab, index), entry.is_dir, tab.tree->IsPlaceholder(static_cast<size_t>(index))});
    }
    return app::ResolveTreeTransferDestinations(tab.TreeEntries() != nullptr, tab.current_path, rows);
}
bool SubmitTreeTransfer(AppState& s, const std::vector<std::wstring>& sources,
                        const app::TreeTransferDestinations& targets, bool move,
                        app::Pane* pane, bool* moved) {
    if (moved) *moved = false;
    using I = l10n::StringId;
    FILETIME now{}; GetSystemTimeAsFileTime(&now);
    const uint64_t group = (static_cast<uint64_t>(now.dwHighDateTime) << 32) | now.dwLowDateTime;
    auto plan = app::PlanTreeTransfer(sources, targets, move, true, group);
    if (!plan.invalid_target.empty()) {
        ui::ConfirmDialogSpec error;
        error.title = l10n::Get(I::Copy);
        error.message = targets.invalid || plan.invalid_target == L"?"
            ? l10n::Get(I::TreeTransferInvalidTargets)
            : l10n::Get(I::CannotCopyOrMoveAFolderIntoItself) + plan.invalid_target;
        error.single_button = true;
        ui::ShowConfirmDialog(s.hwnd, error, s.darkMode, s.accentColor);
        return false;
    }
    if (plan.requests.empty()) return false;
    const bool fanout = plan.requests.size() > 1;
    const auto submitted = app::SubmitTreeTransferPlan(std::move(plan), [&] {
        const auto confirm = BuildTreeTransferConfirmation(targets);
        return ui::ShowConfirmDialog(s.hwnd, confirm, s.darkMode, s.accentColor);
    }, [&](ops::OpRequest request) {
        if (fanout) request.undo_label = l10n::Get(I::TreeTransferCopyAllAction);
        uint64_t task = 0;
        if (!SubmitWithConflictResolution(s, std::move(request), &task)) return false;
        ArmArrivalSelection(s, pane, task, 0);
        WatchQueuedTransfer(s, task);
        return true;
    });
    if (moved) *moved = submitted && move && targets.folders.size() == 1;
    return submitted != 0;
}
}
