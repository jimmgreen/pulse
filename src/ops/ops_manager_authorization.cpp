#include "ops_manager.h"
#include "../common/localization.h"
#include "../common/runtime_log.h"

namespace pulse::ops {
void OpsManager::ResolveAuthorization(uint64_t task_id, bool retry) {
    if (auto executor = InteractiveExecutor(task_id)) { executor->ResolveAuthorization(task_id, retry); return; }
    {
        std::lock_guard lock(transfer_control_mutex_);
        if (authorization_task_ != task_id || authorization_choice_) return;
        authorization_choice_ = retry ? AuthorizationChoice::Retry : AuthorizationChoice::Skip;
    }
    transfer_control_cv_.notify_all();
}

AuthorizationChoice OpsManager::WaitForAuthorization(uint64_t task_id,
    const std::wstring& error, const std::wstring& source) {
    {
        std::lock_guard lock(transfer_control_mutex_);
        authorization_task_ = task_id;
        authorization_choice_.reset();
    }
    SetStatus([&](OpStatus& status) {
        status.authorization = AuthorizationState::ActionRequired;
        status.authorization_retrying = false;
        status.can_skip_authorization = true;
        status.can_pause = false;
        status.phase = OpPhase::Running;
        status.last_error = error;
        status.current_item = source;
        status.summary = l10n::Get(l10n::StringId::AdministratorPermissionRequired).c_str();
        status.bytes_per_second = 0;
        status.eta_seconds = 0;
    });
    std::unique_lock lock(transfer_control_mutex_);
    transfer_control_cv_.wait(lock, [&] {
        return transfer_cancel_.load() || authorization_choice_.has_value();
    });
    const auto choice = transfer_cancel_.load() ? AuthorizationChoice::Cancel : *authorization_choice_;
    authorization_task_ = 0;
    authorization_choice_.reset();
    lock.unlock();
    diagnostics::runtime::Event("file_operation_permission_choice", {
        {"task", task_id}, {"choice", static_cast<uint32_t>(choice)}});
    SetStatus([&](OpStatus& status) {
        // Retry stays on the ActionRequired view (actions disabled) until the
        // attempt shows a real UAC prompt, makes progress, or fails again.
        // Switching to Requesting here flashed "authorize in the Windows
        // prompt" although a reused elevated session shows no prompt at all.
        if (choice == AuthorizationChoice::Retry) {
            status.authorization_retrying = true;
        } else {
            status.authorization = AuthorizationState::None;
            status.authorization_retrying = false;
            status.can_skip_authorization = false;
        }
    });
    return choice;
}
}
