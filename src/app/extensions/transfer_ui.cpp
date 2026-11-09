#include "transfer_ui.h"
#include "audit_log.h"
#include "fileops_request_id.h"
#include "../../common/localization.h"
#include "../app_runtime.h"
#include "transfer_confirmation.h"
#include "transfer_preparation.h"
#include <algorithm>
#include <limits>
#include <map>
#include <unordered_map>

namespace pulse::extensions {
namespace {
constexpr UINT_PTR kTransferTimer = 0x455854;
struct Pending {
    CallerBinding principal;
    std::wstring display_name;
    std::atomic<bool> cancelled{false};
    std::atomic<bool> ready{false};
    transfer_preparation::Result result;
    HWND confirmation = nullptr;
    bool confirming = false;
    std::function<bool()> target_valid;
    std::function<bool()> sources_valid;
    uint64_t request_id = 0;
    explicit Pending(const CallerBinding &binding) : principal(binding) {}
};
// UI-thread map; a worker owns only its shared Pending and immutable request.
std::unordered_map<HWND, std::shared_ptr<Pending>> pending;
std::unordered_map<HWND, uint64_t> last_tasks;
struct Feedback {
    CallerBinding principal;
    TransferDisplayState state;
    uint64_t request_id = 0;
};
std::unordered_map<HWND, Feedback> last_feedback;
struct RequestRecord {
    CallerBinding principal;
    TransferDisplayState state;
    uint64_t task = 0;
    bool retained = false;
};
std::unordered_map<HWND, std::map<uint64_t, RequestRecord>> request_history;
std::function<void(HWND)> probe_observer;
std::function<void(const std::vector<std::wstring> &)> preparation_probe_observer;
transfer_plan::Store &Plans() {
    static transfer_plan::Store plans(HostAuthorizationRegistry());
    return plans;
}
std::wstring Wide(std::string_view text) {
    const auto count =
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (count <= 0)
        return {};
    std::wstring value(static_cast<size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), value.data(), count);
    return value;
}
void Finish(HWND owner, const std::shared_ptr<Pending> &job,
            TransferDisplayState result = TransferDisplayState::Unavailable) {
    const auto found = pending.find(owner);
    if (found != pending.end() && found->second == job) {
        if (result != TransferDisplayState::Unavailable)
            last_feedback.insert_or_assign(owner, Feedback{job->principal, result, job->request_id});
        auto history = request_history.find(owner);
        if (history != request_history.end()) {
            auto record = history->second.find(job->request_id);
            if (record != history->second.end())
                record->second.state = result;
        }
        KillTimer(owner, kTransferTimer);
        pending.erase(found);
    }
}
void CALLBACK Tick(HWND owner, UINT, UINT_PTR, DWORD) {
    const auto found = pending.find(owner);
    if (found == pending.end())
        return;
    const auto job = found->second;
    if (!ValidateCallerBinding(job->principal) || (job->target_valid && !job->target_valid()) ||
        (job->sources_valid && !job->sources_valid()))
        job->cancelled = true;
    if (job->cancelled) {
        if (job->confirmation)
            PostMessageW(job->confirmation, WM_CLOSE, 0, 0);
        if (job->ready && job->result.plan_id)
            Plans().Cancel(*job->result.plan_id, job->principal);
        if (!job->confirming)
            Finish(owner, job, TransferDisplayState::Cancelled);
        return;
    }
    if (!job->ready || job->confirming)
        return;
    auto *state = reinterpret_cast<AppState *>(GetWindowLongPtrW(owner, GWLP_USERDATA));
    if (!state || state->hwnd != owner) {
        job->cancelled = true;
        Finish(owner, job);
        return;
    }
    if (!job->result) {
        ShowStatusFeedback(*state, l10n::Get(l10n::StringId::ExtensionTransferPrepareFailed));
        Finish(owner, job, TransferDisplayState::Failed);
        return;
    }
    const auto &id = *job->result.plan_id;
    auto plan = Plans().Read(id, job->principal);
    auto confirmation = plan ? BuildTransferConfirmation(**plan.value, job->principal.Package(), job->display_name)
                             : TransferConfirmation{};
    if (!plan || !confirmation) {
        Plans().Cancel(id, job->principal);
        ShowStatusFeedback(*state, l10n::Get(l10n::StringId::ExtensionTransferPrepareFailed));
        Finish(owner, job, TransferDisplayState::Failed);
        return;
    }
    job->confirming = true;
    const bool isolated = state->isolatedTest;
    confirmation.spec->on_open = [job, isolated](HWND window) {
        job->confirmation = window;
        if (isolated && probe_observer)
            probe_observer(window);
    };
    const auto choice = ui::ShowConfirmDialogEx(owner, *confirmation.spec, state->darkMode, state->accentColor);
    job->confirming = false;
    // A modal dialog dispatches messages: the owner or package may have gone.
    state = IsWindow(owner) ? reinterpret_cast<AppState *>(GetWindowLongPtrW(owner, GWLP_USERDATA)) : nullptr;
    if (choice != ui::ConfirmChoice::Confirm || job->cancelled || !state || state->hwnd != owner ||
        !ValidateCallerBinding(job->principal) || (job->target_valid && !job->target_valid()) ||
        (job->sources_valid && !job->sources_valid())) {
        Plans().Cancel(id, job->principal);
        Finish(owner, job, TransferDisplayState::Cancelled);
        return;
    }
    auto receipt = Plans().Confirm(id, job->principal);
    auto permit = receipt ? Plans().Consume(*receipt.value, job->principal)
                          : transfer_plan::Result<transfer_plan::ExecutionPermit>{};
    bool retain_result = false;
    if (const auto history = request_history.find(owner); history != request_history.end()) {
        const auto record = history->second.find(job->request_id);
        retain_result = record != history->second.end() && record->second.retained;
    }
    const uint64_t task = permit ? state->ops.SubmitControlled(*permit.value, HostAuthorizationRegistry(), retain_result) : 0;
    RecordExtensionCall(job->principal, "files.transfer", task ? "submitted" : "submit_failed");
    if (task) {
        last_tasks[owner] = task;
        auto history = request_history.find(owner);
        if (history != request_history.end()) {
            auto record = history->second.find(job->request_id);
            if (record != history->second.end())
                record->second.task = task;
        }
    } else
        ShowStatusFeedback(*state, l10n::Get(l10n::StringId::ExtensionTransferPrepareFailed));
    Finish(owner, job, task ? TransferDisplayState::Queued : TransferDisplayState::Failed);
}
} // namespace

namespace {
std::optional<transfer_preparation::Request> CaptureTransferRequest(AppState &state,
    const std::wstring* explicit_target = nullptr, const std::vector<std::wstring>* explicit_sources = nullptr,
    const std::vector<uint64_t>* source_sizes = nullptr, const std::vector<uint64_t>* source_modified = nullptr,
    const std::wstring* tree_source = nullptr, uint64_t tree_source_modified = 0) {
    if (pending.contains(state.hwnd))
        return std::nullopt;
    const auto *source = (explicit_sources || tree_source) ? nullptr : ActiveTab(state);
    if (!explicit_sources && !tree_source && (!source || source->loading || source->content_results || source->search_entries || source->current_path.empty() ||
        source->snapshot_path != source->current_path || source->SelectedCount() <= 0 || source->SelectedCount() > 64))
        return std::nullopt;
    if (explicit_sources && (explicit_sources->empty() || explicit_sources->size() > 64 ||
        (source_sizes && source_sizes->size() != explicit_sources->size()) ||
        (source_modified && source_modified->size() != explicit_sources->size()))) return std::nullopt;
    transfer_preparation::Request request;
    request.operation = transfer_plan::Operation::Copy;
    if (tree_source) {
        request.directory_tree = true;
        request.tree_source = *tree_source;
        request.tree_source_modified = tree_source_modified;
    }
    if (explicit_target) {
        if (explicit_target->empty()) return std::nullopt;
        request.target = *explicit_target;
    } else {
        const auto* layout = state.window_tabs.Active();
        if (!layout) return std::nullopt;
        const auto panes = layout->VisiblePanes();
        if (panes.size() != 2 || std::find(panes.begin(), panes.end(), state.pane) == panes.end()) return std::nullopt;
        const auto* target = (panes[0] == state.pane ? panes[1] : panes[0])->ActiveTab();
        if (!target || target->loading || target->content_results || target->search_entries || target->current_path.empty() ||
            target->snapshot_path != target->current_path) return std::nullopt;
        request.target = target->current_path;
    }
    if (explicit_sources) {
        request.sources = *explicit_sources;
        if (source_sizes) request.source_sizes = *source_sizes;
        if (source_modified) request.source_modified = *source_modified;
        return request;
    }
    if (tree_source) return request;
    std::vector<int> selected;
    selected.reserve(static_cast<size_t>(source->SelectedCount()));
    if (source->all_selected) {
        for (size_t i = 0; i < source->EntryCount(); ++i) {
            if (!source->IsSelected(static_cast<int>(i)))
                continue;
            selected.push_back(static_cast<int>(i));
            if (selected.size() > 64)
                return std::nullopt;
        }
    } else
        selected.assign(source->selected.begin(), source->selected.end());
    std::sort(selected.begin(), selected.end());
    for (int i : selected) {
        if (i < 0 || static_cast<size_t>(i) >= source->EntryCount() || !source->IsSelected(i) ||
            source->EntryAt(static_cast<size_t>(i)).is_dir)
            return std::nullopt;
        request.sources.push_back(EntryFullPath(*source, i));
        if (request.sources.size() > 64)
            return std::nullopt;
    }
    if (request.sources.empty())
        return std::nullopt;
    return request;
}
} // namespace
bool CanTransferToOtherPane(AppState &state) { return CaptureTransferRequest(state).has_value(); }

static bool BeginTransfer(AppState &state, const CallerBinding &principal, transfer_plan::Operation operation,
    const std::wstring* explicit_target, std::function<bool()> target_valid,
    const std::vector<std::wstring>* explicit_sources = nullptr, const std::vector<uint64_t>* source_sizes = nullptr,
    const std::vector<uint64_t>* source_modified = nullptr, std::function<bool()> sources_valid = {},
    const std::wstring* tree_source = nullptr, uint64_t tree_source_modified = 0) {
    const auto reject = [&] {
        ShowStatusFeedback(state, l10n::Get(explicit_target ? l10n::StringId::ExtensionTransferPrepareFailed
            : l10n::StringId::ExtensionTransferNeedsTwoPanes));
        return false;
    };
    if (principal.Owner().key != reinterpret_cast<uintptr_t>(state.hwnd) || !ValidateCallerBinding(principal) ||
        pending.contains(state.hwnd) || (explicit_target && (!target_valid || !target_valid())) ||
        ((explicit_sources || tree_source) && (!sources_valid || !sources_valid())))
        return reject();
    if (operation != transfer_plan::Operation::Copy && operation != transfer_plan::Operation::Move)
        return reject();
    auto &history = request_history[state.hwnd];
    size_t ordinary = static_cast<size_t>(std::count_if(history.begin(), history.end(),
        [](const auto& item) { return !item.second.retained; }));
    for (auto at = history.begin(); at != history.end();) {
        if (!ValidateCallerBinding(at->second.principal)) {
            if (at->second.task) {
                state.ops.CancelTask(at->second.task);
                state.ops.ReleaseControlled(at->second.task, at->second.principal);
            }
            if (!at->second.retained) --ordinary;
            at = history.erase(at);
        } else if (!at->second.retained && ordinary >= 32) {
            const auto status = at->second.task ? state.ops.QueryControlled(at->second.task, at->second.principal)
                                                : ops::ControlledTaskStatus{};
            if (!at->second.task || status.result || status.error != transfer_plan::Error::None) {
                at = history.erase(at);
                --ordinary;
            } else ++at;
        } else ++at;
    }
    if (ordinary >= 32 || history.size() >= 256) return reject();
    auto captured = CaptureTransferRequest(state, explicit_target, explicit_sources, source_sizes, source_modified,
        tree_source, tree_source_modified);
    if (!captured)
        return reject();
    auto request = std::move(*captured);
    request.operation = operation;
    const auto job = std::make_shared<Pending>(principal);
    job->target_valid = std::move(target_valid);
    job->sources_valid = std::move(sources_valid);
    job->request_id = NextFileOperationRequestId();
    if (!job->request_id) return reject();
    last_tasks.erase(state.hwnd);
    last_feedback.erase(state.hwnd);
    const auto* manifest = PackageManifest(principal.Package().slot);
    job->display_name = Wide(manifest ? manifest->name : principal.Package().id);
    pending.emplace(state.hwnd, job);
    history.emplace(job->request_id, RequestRecord{principal, TransferDisplayState::Preparing});
    if (!SetTimer(state.hwnd, kTransferTimer, 50, Tick)) {
        pending.erase(state.hwnd);
        history.erase(job->request_id);
        return reject();
    }
    if (state.isolatedTest && preparation_probe_observer) {
        auto observer = std::move(preparation_probe_observer);
        preparation_probe_observer = {};
        observer(request.sources);
    }
    auto &plans = Plans();
    state.worker.EnqueueIo([job, request = std::move(request), &plans] {
        try {
            job->result = transfer_preparation::Prepare(request, job->principal, HostAuthorizationRegistry(), plans,
                                                        job->cancelled);
        } catch (...) {
            job->result = {transfer_plan::Error::InvalidDraft, ERROR_NOT_ENOUGH_MEMORY, std::nullopt};
        }
        job->ready = true;
    });
    ShowStatusFeedback(state, l10n::Get(l10n::StringId::ExtensionTransferPreparing));
    return true;
}

bool BeginTransferToOtherPane(AppState& state, const CallerBinding& principal, transfer_plan::Operation operation) {
    return BeginTransfer(state, principal, operation, nullptr, {});
}
bool BeginTransferToDirectory(AppState& state, const CallerBinding& principal, transfer_plan::Operation operation,
    std::wstring target, std::function<bool()> target_valid) {
    return BeginTransfer(state, principal, operation, &target, std::move(target_valid));
}
bool BeginTransferEntriesToDirectory(AppState& state, const CallerBinding& principal, transfer_plan::Operation operation,
    std::vector<std::wstring> sources, std::vector<uint64_t> sizes, std::vector<uint64_t> modified,
    std::wstring target, std::function<bool()> sources_valid, std::function<bool()> target_valid) {
    return BeginTransfer(state, principal, operation, &target, std::move(target_valid), &sources, &sizes,
        &modified, std::move(sources_valid));
}
bool BeginTransferDirectoryTree(AppState& state, const CallerBinding& principal, std::wstring source,
    uint64_t source_modified, std::function<bool()> source_valid, std::wstring target,
    std::function<bool()> target_valid) {
    return BeginTransfer(state, principal, transfer_plan::Operation::Copy, &target, std::move(target_valid),
        nullptr, nullptr, nullptr, std::move(source_valid), &source, source_modified);
}

TransferDisplayState ReadTransferDisplayState(AppState &state, const CallerBinding &principal) {
    if (!ValidateCallerBinding(principal) || principal.Owner().key != reinterpret_cast<uintptr_t>(state.hwnd))
        return TransferDisplayState::Unavailable;
    const auto same = [&](const CallerBinding &recorded) {
        return ValidateCallerBinding(recorded) && recorded.Package() == principal.Package() &&
               recorded.Owner() == principal.Owner();
    };
    if (const auto found = pending.find(state.hwnd); found != pending.end()) {
        if (!same(found->second->principal))
            return TransferDisplayState::Unavailable;
        return found->second->confirming ? TransferDisplayState::WaitingConfirmation : TransferDisplayState::Preparing;
    }
    const auto feedback = last_feedback.find(state.hwnd);
    if (feedback == last_feedback.end() || !same(feedback->second.principal))
        return TransferDisplayState::Unavailable;
    const auto task = last_tasks.find(state.hwnd);
    if (task == last_tasks.end())
        return feedback->second.state;
    const auto status = state.ops.QueryControlled(task->second, principal);
    if (status.error != transfer_plan::Error::None)
        return TransferDisplayState::Unavailable;
    if (status.result)
        return ClassifyTransferResult(*status.result);
    if (status.cancel_requested)
        return TransferDisplayState::Cancelling;
    return status.phase == ops::OpPhase::Queued ? TransferDisplayState::Queued : TransferDisplayState::Running;
}
std::wstring TransferDisplayLabel(TransferDisplayState state) {
    using I = l10n::StringId;
    switch (state) {
    case TransferDisplayState::Unavailable:
        return {};
    case TransferDisplayState::Preparing:
        return l10n::Get(I::ExtensionTransferStatePreparing);
    case TransferDisplayState::WaitingConfirmation:
        return l10n::Get(I::ExtensionTransferStateWaiting);
    case TransferDisplayState::Queued:
        return l10n::Get(I::ExtensionTransferStateQueued);
    case TransferDisplayState::Running:
        return l10n::Get(I::ExtensionTransferStateRunning);
    case TransferDisplayState::Cancelling:
        return l10n::Get(I::ExtensionTransferStateCancelling);
    case TransferDisplayState::Completed:
        return l10n::Get(I::ExtensionTransferStateCompleted);
    case TransferDisplayState::Partial:
        return l10n::Get(I::ExtensionTransferStatePartial);
    case TransferDisplayState::Cancelled:
        return l10n::Get(I::ExtensionTransferStateCancelled);
    case TransferDisplayState::Failed:
        return l10n::Get(I::ExtensionTransferStateFailed);
    }
    return {};
}

bool CancelOwnedTransferUi(AppState &state, const CallerBinding &principal) {
    if (!ValidateCallerBinding(principal) || principal.Owner().key != reinterpret_cast<uintptr_t>(state.hwnd))
        return false;
    const auto same = [&](const CallerBinding &recorded) {
        return ValidateCallerBinding(recorded) && recorded.Package() == principal.Package() &&
               recorded.Owner() == principal.Owner();
    };
    if (const auto found = pending.find(state.hwnd); found != pending.end()) {
        const auto job = found->second;
        if (!same(job->principal))
            return false;
        job->cancelled = true;
        if (job->confirmation)
            PostMessageW(job->confirmation, WM_CLOSE, 0, 0);
        if (job->ready && job->result.plan_id)
            Plans().Cancel(*job->result.plan_id, principal);
        if (!job->confirming)
            Finish(state.hwnd, job, TransferDisplayState::Cancelled);
        return true;
    }
    const auto feedback = last_feedback.find(state.hwnd);
    const auto task = last_tasks.find(state.hwnd);
    if (feedback == last_feedback.end() || !same(feedback->second.principal) || task == last_tasks.end())
        return false;
    return state.ops.CancelControlled(task->second, principal);
}

uint64_t CurrentTransferRequest(AppState &state, const CallerBinding &principal) {
    if (!ValidateCallerBinding(principal) || principal.Owner().key != reinterpret_cast<uintptr_t>(state.hwnd))
        return 0;
    const auto same = [&](const CallerBinding &recorded) {
        return recorded.Package() == principal.Package() && recorded.Owner() == principal.Owner();
    };
    auto active = pending.find(state.hwnd);
    if (active != pending.end())
        return same(active->second->principal) ? active->second->request_id : 0;
    auto completed = last_feedback.find(state.hwnd);
    return completed != last_feedback.end() && same(completed->second.principal) ? completed->second.request_id : 0;
}
TransferDisplayState ReadTransferRequestState(AppState &state, const CallerBinding &principal, uint64_t request) {
    return GetTransferRequestSnapshot(state, principal, request).state;
}
TransferRequestSnapshot GetTransferRequestSnapshot(AppState &state, const CallerBinding &principal, uint64_t request) {
    TransferRequestSnapshot result;
    if (!request || !ValidateCallerBinding(principal) ||
        principal.Owner().key != reinterpret_cast<uintptr_t>(state.hwnd))
        return result;
    auto history = request_history.find(state.hwnd);
    if (history == request_history.end())
        return result;
    auto record = history->second.find(request);
    if (record == history->second.end() || record->second.principal.Package() != principal.Package() ||
        record->second.principal.Owner() != principal.Owner())
        return result;
    auto active = pending.find(state.hwnd);
    if (active != pending.end() && active->second->request_id == request) {
        result.state = active->second->confirming ? TransferDisplayState::WaitingConfirmation : TransferDisplayState::Preparing;
        return result;
    }
    if (!record->second.task) {
        result.state = record->second.state;
        return result;
    }
    const auto status = state.ops.QueryControlled(record->second.task, principal);
    if (status.error != transfer_plan::Error::None)
        return result;
    result.task_id = record->second.task;
    result.cancel_requested = status.cancel_requested;
    result.transferred_bytes = status.progress.transferred_bytes;
    result.total_bytes = status.progress.total_bytes;
    result.completed_items = status.progress.completed_items;
    result.total_items = status.progress.total_items;
    result.percent = status.progress.percent;
    if (status.result) {
        result.state = ClassifyTransferResult(*status.result);
        result.system_error = status.result->error;
        if (!result.system_error) {
            for (const auto& item : status.result->items) {
                if (item.error) { result.system_error = item.error; break; }
                if (item.cleanup_error) { result.system_error = item.cleanup_error; break; }
            }
        }
    } else result.state = status.cancel_requested ? TransferDisplayState::Cancelling :
        status.phase == ops::OpPhase::Queued ? TransferDisplayState::Queued : TransferDisplayState::Running;
    return result;
}

std::optional<TransferRequestItemSnapshot> GetTransferRequestItem(AppState& state,
    const CallerBinding& principal, uint64_t request, uint32_t index) {
    const auto snapshot = GetTransferRequestSnapshot(state, principal, request);
    if (!snapshot.task_id) return std::nullopt;
    const auto task = state.ops.QueryControlled(snapshot.task_id, principal);
    if (!task.result || !task.result->directory_tree || index >= task.result->items.size()) return std::nullopt;
    const auto& item = task.result->items[index];
    TransferRequestItemSnapshot result;
    result.relative_path = item.relative_destination;
    result.bytes = item.bytes;
    result.system_error = item.error ? item.error : item.cleanup_error;
    switch (item.state) {
    case ops::ControlledTransferState::Copied: result.state = TransferRequestItemState::Completed; break;
    case ops::ControlledTransferState::Cancelled: result.state = TransferRequestItemState::Cancelled; break;
    case ops::ControlledTransferState::NotStarted: result.state = TransferRequestItemState::Pending; break;
    default: result.state = TransferRequestItemState::Failed; break;
    }
    return result;
}

bool CancelTransferRequest(AppState &state, const CallerBinding &principal, uint64_t request) {
    if (!request || principal.Owner().key != reinterpret_cast<uintptr_t>(state.hwnd))
        return false;
    const auto active = pending.find(state.hwnd);
    if (active != pending.end() && active->second->request_id == request) {
        const auto job = active->second;
        if (job->principal.Package() != principal.Package() || job->principal.Owner() != principal.Owner())
            return false;
        // Revocation must still dismiss this exact request's confirmation; no new authority is issued.
        job->cancelled = true;
        if (job->confirmation)
            PostMessageW(job->confirmation, WM_CLOSE, 0, 0);
        return true;
    }
    const auto history = request_history.find(state.hwnd);
    if (history != request_history.end()) {
        const auto record = history->second.find(request);
        if (record != history->second.end() && record->second.principal.Package() == principal.Package() &&
            record->second.principal.Owner() == principal.Owner() && record->second.task)
            return state.ops.CancelControlled(record->second.task, principal);
    }
    return false;
}

bool RetainTransferRequest(AppState& state, const CallerBinding& principal, uint64_t request) {
    if (!request || !ValidateCallerBinding(principal) ||
        principal.Owner().key != reinterpret_cast<uintptr_t>(state.hwnd)) return false;
    const auto history = request_history.find(state.hwnd);
    if (history == request_history.end()) return false;
    const auto record = history->second.find(request);
    if (record == history->second.end() || record->second.principal.Package() != principal.Package() ||
        record->second.principal.Owner() != principal.Owner()) return false;
    if (record->second.retained) return true;
    // Native submission takes the lease synchronously, before the preparation timer can submit a task.
    if (record->second.task) return false;
    const auto count = std::count_if(history->second.begin(), history->second.end(), [&](const auto& item) {
        return item.second.retained && item.second.principal.Package() == principal.Package() &&
            item.second.principal.Owner() == principal.Owner();
    });
    if (count >= 32) return false;
    record->second.retained = true;
    return true;
}

bool ConsumeTransferRequest(AppState& state, const CallerBinding& principal, uint64_t request) {
    const auto snapshot = GetTransferRequestSnapshot(state, principal, request);
    if (snapshot.state != TransferDisplayState::Completed && snapshot.state != TransferDisplayState::Partial &&
        snapshot.state != TransferDisplayState::Cancelled && snapshot.state != TransferDisplayState::Failed) return false;
    const auto history = request_history.find(state.hwnd);
    if (history == request_history.end()) return false;
    const auto record = history->second.find(request);
    if (record == history->second.end() || !record->second.retained ||
        record->second.principal.Package() != principal.Package() || record->second.principal.Owner() != principal.Owner())
        return false;
    if (record->second.task) state.ops.ReleaseControlled(record->second.task, principal);
    history->second.erase(record);
    return true;
}

void ReleaseTransferRequests(AppState& state, const CallerBinding& principal) {
    if (principal.Owner().key != reinterpret_cast<uintptr_t>(state.hwnd)) return;
    const auto history = request_history.find(state.hwnd);
    if (history == request_history.end()) return;
    for (auto at = history->second.begin(); at != history->second.end();) {
        auto& record = at->second;
        if (!record.retained || record.principal.Package() != principal.Package() || record.principal.Owner() != principal.Owner()) {
            ++at;
            continue;
        }
        CancelTransferRequest(state, principal, at->first);
        if (record.task) {
            state.ops.CancelTask(record.task);
            state.ops.ReleaseControlled(record.task, principal);
        }
        at = history->second.erase(at);
    }
}

void CancelTransferUi(HWND owner) {
    if (auto* state = GetAppState(owner)) {
        if (const auto history = request_history.find(owner); history != request_history.end()) {
            for (const auto& [id, record] : history->second) {
                (void)id;
                if (!record.task) continue;
                state->ops.CancelTask(record.task);
                state->ops.ReleaseControlled(record.task, record.principal);
            }
        }
    }
    request_history.erase(owner);
    last_tasks.erase(owner);
    last_feedback.erase(owner);
    const auto found = pending.find(owner);
    if (found == pending.end())
        return;
    const auto job = found->second;
    job->cancelled = true;
    if (job->confirmation)
        PostMessageW(job->confirmation, WM_CLOSE, 0, 0);
    if (job->ready && job->result.plan_id)
        Plans().Cancel(*job->result.plan_id, job->principal);
    Finish(owner, job);
}
bool TransferUiPending(HWND owner) { return pending.contains(owner); }
void SetTransferUiProbeObserver(std::function<void(HWND)> observer) { probe_observer = std::move(observer); }
uint64_t LastTransferTask(HWND owner) {
    const auto found = last_tasks.find(owner);
    return found == last_tasks.end() ? 0 : found->second;
}
} // namespace pulse::extensions

namespace pulse::extensions {
void SetTransferPreparationProbeObserver(std::function<void(const std::vector<std::wstring> &)> observer) {
    preparation_probe_observer = std::move(observer);
}
} // namespace pulse::extensions
