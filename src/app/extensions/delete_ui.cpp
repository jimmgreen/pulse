#include "delete_ui.h"
#include "audit_log.h"
#include "fileops_request_id.h"
#include "../app_ops_ui.h"
#include "../app_runtime.h"
#include "../delete_preflight.h"
#include <algorithm>
#include <map>
#include <unordered_map>

namespace pulse::extensions {
namespace {
struct DeleteRequestRecord {
    CallerBinding principal;
    std::shared_ptr<DeletePreflight> preflight;
    bool cancel_requested = false;
};
std::unordered_map<HWND, std::map<uint64_t, DeleteRequestRecord>> delete_history;
}

bool BeginRecycleDeleteSelection(AppState& state, const CallerBinding& principal, uint64_t* request_id) {
    if (!request_id) return false;
    *request_id = 0;
    if (!ValidateCallerBinding(principal) || principal.Owner().key != reinterpret_cast<uintptr_t>(state.hwnd) ||
        state.delete_preflight)
        return false;
    auto* tab = ActiveTab(state);
    if (!tab || tab->loading || tab->content_results || tab->search_entries || tab->current_path.empty() ||
        tab->snapshot_path != tab->current_path || tab->SelectedCount() <= 0 || tab->SelectedCount() > 64)
        return false;
    std::vector<int> selected;
    selected.reserve(static_cast<size_t>(tab->SelectedCount()));
    if (tab->all_selected) {
        for (size_t i = 0; i < tab->EntryCount(); ++i) {
            if (!tab->IsSelected(static_cast<int>(i))) continue;
            selected.push_back(static_cast<int>(i));
            if (selected.size() > 64) return false;
        }
    } else selected.assign(tab->selected.begin(), tab->selected.end());
    std::sort(selected.begin(), selected.end());
    std::vector<std::wstring> paths;
    paths.reserve(selected.size());
    for (int index : selected) {
        if (index < 0 || static_cast<size_t>(index) >= tab->EntryCount() || !tab->IsSelected(index) ||
            tab->EntryAt(static_cast<size_t>(index)).is_dir || tab->EntryAt(static_cast<size_t>(index)).is_reparse)
            return false;
        auto path = EntryFullPath(*tab, index);
        if (path.empty() || ops::IsShellItemId(path)) return false;
        paths.push_back(std::move(path));
    }
    if (paths.empty()) return false;
    auto& history = delete_history[state.hwnd];
    for (auto at = history.begin(); at != history.end();) {
        const auto& record = at->second;
        const auto& preflight = record.preflight;
        if (!ValidateCallerBinding(record.principal) || !preflight) {
            if (preflight && preflight->task_id) {
                state.ops.CancelTask(preflight->task_id);
                state.ops.ReleaseTask(preflight->task_id);
            } else if (preflight) preflight->cancel = true;
            at = history.erase(at);
        }
        else ++at;
    }
    if (history.size() >= 256) return false;
    const uint64_t id = NextFileOperationRequestId();
    if (!id) return false;
    auto preflight = DeletePaths(state, std::move(paths), false, true, true);
    if (!preflight) return false;
    RecordExtensionCall(principal, "files.deleteCurrentSelection", "accepted");
    history.emplace(id, DeleteRequestRecord{principal, std::move(preflight), false});
    *request_id = id;
    return true;
}

TransferRequestSnapshot GetDeleteRequestSnapshot(AppState& state, const CallerBinding& principal, uint64_t request) {
    TransferRequestSnapshot result;
    if (!request || !ValidateCallerBinding(principal) ||
        principal.Owner().key != reinterpret_cast<uintptr_t>(state.hwnd)) return result;
    auto owner = delete_history.find(state.hwnd);
    if (owner == delete_history.end()) return result;
    auto found = owner->second.find(request);
    if (found == owner->second.end() || found->second.principal.Package() != principal.Package() ||
        found->second.principal.Owner() != principal.Owner() || !found->second.preflight) return result;
    const auto& preflight = found->second.preflight;
    if (preflight->dispatch_failed) {
        result.state = TransferDisplayState::Failed;
        owner->second.erase(found);
        return result;
    }
    if (preflight->cancelled || preflight->cancel.load()) {
        result.state = TransferDisplayState::Cancelled;
        result.cancel_requested = found->second.cancel_requested;
        owner->second.erase(found);
        return result;
    }
    if (!preflight->done.load()) {
        result.state = preflight->waiting_confirmation ? TransferDisplayState::WaitingConfirmation
                                                       : TransferDisplayState::Preparing;
        return result;
    }
    if (preflight->waiting_confirmation) {
        result.state = TransferDisplayState::WaitingConfirmation;
        return result;
    }
    if (!preflight->task_id && !preflight->failed && !preflight->terminal && state.delete_preflight == preflight) {
        result.state = TransferDisplayState::Preparing;
        return result;
    }
    if (preflight->failed || preflight->terminal || !preflight->task_id) {
        result.state = preflight->failed ? TransferDisplayState::Failed : TransferDisplayState::Cancelled;
        owner->second.erase(found);
        return result;
    }
    const auto status = state.ops.QueryTask(preflight->task_id);
    if (!status) return result;
    result.task_id = preflight->task_id;
    result.total_items = status->total_items;
    result.completed_items = status->completed_items;
    result.percent = status->percent;
    result.cancel_requested = found->second.cancel_requested;
    if (status->phase == ops::OpPhase::Queued) result.state = TransferDisplayState::Queued;
    else if (status->active) result.state = result.cancel_requested || status->phase == ops::OpPhase::Cancelling
        ? TransferDisplayState::Cancelling : TransferDisplayState::Running;
    else if (status->phase == ops::OpPhase::Completed && status->completed_items == status->total_items)
        result.state = TransferDisplayState::Completed;
    else if (result.cancel_requested && status->completed_items == 0) result.state = TransferDisplayState::Cancelled;
    else if (status->completed_items > 0) result.state = TransferDisplayState::Partial;
    else result.state = TransferDisplayState::Failed;
    if (!status->active && status->phase != ops::OpPhase::Queued) {
        state.ops.ReleaseTask(preflight->task_id);
        owner->second.erase(found);
    }
    return result;
}

bool CancelDeleteRequest(AppState& state, const CallerBinding& principal, uint64_t request) {
    if (!request || principal.Owner().key != reinterpret_cast<uintptr_t>(state.hwnd)) return false;
    auto owner = delete_history.find(state.hwnd);
    if (owner == delete_history.end()) return false;
    auto found = owner->second.find(request);
    if (found == owner->second.end() || found->second.principal.Package() != principal.Package() ||
        found->second.principal.Owner() != principal.Owner() || !found->second.preflight) return false;
    auto& record = found->second;
    auto& preflight = *record.preflight;
    if (preflight.task_id) {
        if (!state.ops.CancelTask(preflight.task_id)) return false;
        record.cancel_requested = true;
        return true;
    }
    if (preflight.terminal || preflight.cancelled) return false;
    preflight.cancel = true;
    record.cancel_requested = true;
    if (preflight.confirmation) PostMessageW(preflight.confirmation, WM_CLOSE, 0, 0);
    if (state.delete_preflight == record.preflight) CancelDeletePreflight(state);
    return true;
}

void ReleaseDeleteRequests(AppState& state, const CallerBinding& principal) {
    const auto owner = delete_history.find(state.hwnd);
    if (owner == delete_history.end()) return;
    for (auto at = owner->second.begin(); at != owner->second.end();) {
        auto& record = at->second;
        if (record.principal.Package() != principal.Package() || record.principal.Owner() != principal.Owner()) {
            ++at;
            continue;
        }
        if (record.preflight && record.preflight->task_id) {
            state.ops.CancelTask(record.preflight->task_id);
            state.ops.ReleaseTask(record.preflight->task_id);
        }
        at = owner->second.erase(at);
    }
}

void CancelDeleteUi(HWND owner) {
    if (auto* state = GetAppState(owner)) {
        const auto deletes = delete_history.find(owner);
        if (deletes != delete_history.end()) {
            for (auto& [id, record] : deletes->second) {
                (void)id;
                if (!record.preflight) continue;
                if (record.preflight->task_id) {
                    state->ops.CancelTask(record.preflight->task_id);
                    state->ops.ReleaseTask(record.preflight->task_id);
                }
                else if (state->delete_preflight == record.preflight) CancelDeletePreflight(*state);
                else record.preflight->cancel = true;
            }
        }
    }
    delete_history.erase(owner);
}
}
