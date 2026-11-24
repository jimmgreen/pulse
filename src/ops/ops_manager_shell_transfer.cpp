#include "ops_manager.h"
#include "elevated_transfer.h"
#include "elevated_transfer_client.h"
#include "confirmed_move_roots.h"
#include "../common/localization.h"
#include "../common/runtime_log.h"
#include <algorithm>

namespace pulse::ops {
void OpsManager::RunAuthorizedTransfer(const OpRequest& req, uint64_t task_id) {
    SetStatus([&](OpStatus& status) {
        status.can_pause = true;
        status.phase = OpPhase::Running;
        status.total_items = req.sources.size();
        status.summary = l10n::Get(l10n::StringId::AuthorizingFileOperations).c_str();
    });
    const auto policy = req.collision_policy == CollisionPolicy::Replace ? ShellCollisionPolicy::Replace
        : req.collision_policy == CollisionPolicy::KeepBoth ? ShellCollisionPolicy::KeepBoth
        : ShellCollisionPolicy::System;
    const auto started = GetTickCount64();
    std::wstring detailed_error;
    bool received_progress = false;
    ElevatedTransferCallbacks callbacks;
    callbacks.verify_copies = verify_copies_.load();
    callbacks.paused = [&] { return transfer_pause_.load(); };
    callbacks.authorization = [&](bool awaiting) {
        SetStatus([&](OpStatus& status) {
            status.authorization = awaiting ? AuthorizationState::Requesting : AuthorizationState::None;
            status.can_pause = !awaiting;
            if (awaiting) {
                status.percent = -1;
                status.summary = l10n::Get(l10n::StringId::RequestingAdministratorPermission).c_str();
            }
        });
    };
    callbacks.progress = [&](const OpStatus& remote) {
        received_progress = true;
        if (!remote.last_error.empty()) detailed_error = remote.last_error;
        SetStatus([&](OpStatus& status) {
            const auto completed_ops = status.completed_ops;
            auto source_label = std::move(status.source_label);
            auto destination_label = std::move(status.destination_label);
            status = remote;
            status.type = req.type;
            status.source_label = std::move(source_label);
            status.destination_label = std::move(destination_label);
            status.task_id = task_id;
            status.completed_ops = completed_ops;
            // Publish completion only after receiving the actual affected paths.
            status.active = true;
        });
    };
    callbacks.conflict = [&](const ConflictItemInfo& remote) {
        ConflictItemInfo info = remote;
        info.task_id = task_id;
        {
            std::lock_guard<std::mutex> lock(transfer_control_mutex_);
            info.token = next_conflict_token_++;
            pending_conflict_ = info;
            resolved_conflict_token_ = 0;
        }
        SetStatus([&](OpStatus& status) {
            status.phase = OpPhase::WaitingForConflict;
            status.current_item = info.source;
            status.summary = l10n::Get(l10n::StringId::WaitingForFileConflictsToBeResolved).c_str();
            status.bytes_per_second = 0;
            status.eta_seconds = 0;
        });
        std::unique_lock<std::mutex> lock(transfer_control_mutex_);
        transfer_control_cv_.wait(lock, [&] {
            return transfer_cancel_.load() || resolved_conflict_token_ == info.token;
        });
        ElevatedConflictAnswer answer;
        if (!transfer_cancel_.load()) {
            answer.choice = resolved_conflict_choice_;
            answer.apply_to_all = resolved_conflict_apply_all_;
        }
        pending_conflict_.reset();
        if (answer.choice == ConflictChoice::Cancel) transfer_cancel_.store(true);
        return answer;
    };
    auto pending = req.sources;
    ShellTransferResult result;
    size_t skipped = 0;
    while (!pending.empty() && !transfer_cancel_.load()) {
        detailed_error.clear();
        received_progress = false;
        result = TransferWithElevatedHelper(pending, req.dest_dir,
            req.type == OpType::Move, UiWindow(), transfer_cancel_, policy, callbacks, req.new_name);
        if (result.helper_unavailable && !result.cancelled && !result.mutated &&
            !transfer_cancel_.load() && !verify_copies_.load() && req.new_name.empty() &&
            !req.is_undo && !req.generated_output && !transfer_pause_.load()) {
            SetStatus([](OpStatus& status) { status.can_pause = false; });
            result = TransferWithShell(pending, req.dest_dir, req.type == OpType::Move,
                UiWindow(), transfer_cancel_, policy);
            break; // No automatic repeat after Shell execution.
        }
        if (result.cancelled) break;
        if (SUCCEEDED(result.hr) || result.mutated || transfer_cancel_.load()) break;
        // A denied UAC dialog has changed no files. Keep the task in its own
        // window; only an explicit retry can launch another authorization.
        const std::wstring reason = result.error.empty()
            ? l10n::Get(l10n::StringId::AdministratorPermissionWasNotGrantedRetryOrSkipThisItem).c_str()
            : result.error;
        SetStatus([&](OpStatus& status) { status.native_handoff_directory = req.dest_dir; });
        bool retry = false;
        while (!pending.empty() && !transfer_cancel_.load()) {
            const auto choice = WaitForAuthorization(task_id, reason, pending.front());
            if (choice == AuthorizationChoice::Retry) { retry = true; break; }
            if (choice == AuthorizationChoice::Cancel) break;
            pending.erase(pending.begin());
            ++skipped;
        }
        if (pending.empty()) result = ShellTransferResult{};
        if (!retry) break;
    }
    result.cancelled = result.cancelled || transfer_cancel_.load();
    diagnostics::runtime::Event("file_transfer_authorized_result", {
        {"task", task_id}, {"hresult", static_cast<uint32_t>(result.hr)},
        {"cancelled", result.cancelled}, {"completed", result.sources.size()},
        {"elapsed_ms", GetTickCount64() - started}});
    if (result.mutated && !result.undo_safe &&
        (result.sources.empty() || FAILED(result.hr) || result.cancelled)) {
        CompletedOperation refresh;
        refresh.task_id = task_id;
        refresh.type = req.type;
        refresh.sources = req.sources;
        refresh.refresh_directories.push_back(req.dest_dir);
        refresh.refresh_only = true;
        std::lock_guard<std::mutex> lock(mutex_);
        completions_.push_back(std::move(refresh));
    }
    // Refresh-only covers unreported mutations; it must not suppress verified
    // CompletedItem mappings needed by tags, the staging tray and clipboard.
    if (!result.sources.empty()) {
        if (req.type == OpType::Move) {
            const auto roots = ConfirmReportedMoveRoots(req.sources, result.sources, result.destinations);
            moved_sources_.insert(moved_sources_.end(), roots.begin(), roots.end());
        }
        OpRequest completed = req;
        completed.sources = result.sources;
        // Publishing a completion refreshes both panes. Unsafe Shell merges and
        // replacements must not enter our undo stack as whole-directory deletes.
        if (!result.undo_safe) completed.is_undo = true;
        PushUndo(completed, task_id, &result.destinations);
    }
    std::wstring error = result.error.empty() ? detailed_error : result.error;
    if (FAILED(result.hr) && !result.cancelled) {
        wchar_t* message = nullptr;
        if (error.empty() && FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, static_cast<DWORD>(result.hr), 0,
                reinterpret_cast<LPWSTR>(&message), 0, nullptr) && message) {
            error = message;
            LocalFree(message);
        } else if (error.empty()) error = l10n::Get(l10n::StringId::FileOperationFailed).c_str();
    }
    SetStatus([&](OpStatus& status) {
        status.active = false;
        status.authorization = AuthorizationState::None;
        status.can_skip_authorization = false;
        ++status.completed_ops;
        if (!received_progress) status.completed_items = result.sources.size();
        status.total_items = (std::max)(status.total_items, status.completed_items);
        status.bytes_per_second = 0;
        status.eta_seconds = 0;
        status.native_handoff_directory.clear();
        if (result.cancelled) {
            status.phase = OpPhase::Failed;
            status.last_error = l10n::Get(l10n::StringId::Canceled).c_str();
            status.summary = status.last_error;
        } else if (FAILED(result.hr)) {
            status.phase = OpPhase::Failed;
            status.last_error = error;
            status.native_handoff_directory = req.dest_dir;
            status.summary = l10n::Get(l10n::StringId::FileOperationFailed).c_str();
        } else {
            status.phase = OpPhase::Completed;
            status.percent = 100;
            status.last_error.clear();
            status.summary = skipped == req.sources.size()
                ? l10n::Get(l10n::StringId::SelectedItemsSkipped).c_str()
                : l10n::Get(l10n::StringId::FileOperationCompleted).c_str();
        }
    });
    transfer_active_.store(false);
}
}
