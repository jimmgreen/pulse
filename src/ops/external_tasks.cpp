#include "ops_manager.h"
#include "../common/localization.h"
#include <algorithm>
#include <cmath>

namespace pulse::ops {
namespace {
void CopyProgress(OpStatus& to, const OpStatus& from) {
    to.percent = std::isfinite(from.percent) ? std::clamp(from.percent, -1.0f, 100.0f) : -1.0f;
    to.current_item = from.current_item.substr(0, 4096);
    to.last_error = from.last_error.substr(0, 4096);
    to.total_bytes = from.total_bytes;
    to.transferred_bytes = from.transferred_bytes;
    to.total_items = from.total_items;
    to.completed_items = from.completed_items;
    to.failed_items = from.failed_items;
    to.enumeration_complete = from.enumeration_complete;
    to.summary = from.summary.empty() ? to.external_title : from.summary.substr(0, 512);
}
}
uint64_t OpsManager::CreateExternalTask(std::wstring title, std::wstring type_label) {
    uint64_t id = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_ || update_preparing_ || external_tasks_.size() >= 32 || title.empty() || title.size() > 256 || type_label.size() > 128) return 0;
        id = next_seq_++;
        ExternalTaskState task;
        task.status.external = true;
        task.status.external_title = std::move(title);
        task.status.external_kind = std::move(type_label);
        task.status.summary = task.status.external_title;
        task.status.type = OpType::External;
        task.status.task_id = id;
        task.status.active = true;
        task.status.can_pause = false;
        task.status.phase = OpPhase::Running;
        external_tasks_.emplace(id, std::move(task));
    }
    NotifyStatusChanged();
    return id;
}
bool OpsManager::UpdateExternalTask(uint64_t id, const OpStatus& progress) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto task = external_tasks_.find(id);
        if (task == external_tasks_.end()) return false;
        CopyProgress(task->second.status, progress);
        task->second.status.phase = task->second.cancelled ? OpPhase::Cancelling : OpPhase::Running;
    }
    NotifyStatusChanged();
    return true;
}
bool OpsManager::ExternalTaskCancelled(uint64_t id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto task = external_tasks_.find(id);
    return task == external_tasks_.end() || task->second.cancelled || stopping_;
}
bool OpsManager::FinishExternalTask(uint64_t id, ExternalOutcome outcome, const OpStatus& progress) {
    if (outcome < ExternalOutcome::Completed || outcome > ExternalOutcome::Cancelled) return false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto task = external_tasks_.find(id);
        if (task == external_tasks_.end()) return false;
        OpStatus final = task->second.status;
        CopyProgress(final, progress);
        final.active = false;
        final.external_outcome = outcome;
        using I = l10n::StringId;
        const auto label = outcome == ExternalOutcome::Completed ? I::OpCompleted :
            outcome == ExternalOutcome::Partial ? I::OpPartialBadge :
            outcome == ExternalOutcome::Cancelled ? I::Canceled : I::OpFailed;
        final.summary = final.external_title + L" \u00b7 " + l10n::Get(label);
        final.phase = outcome == ExternalOutcome::Completed ? OpPhase::Completed : OpPhase::Failed;
        if (outcome == ExternalOutcome::Completed) final.percent = 100.0f;
        final.terminal_sequence = ++terminal_sequence_;
        final.completed_ops = status_.completed_ops;
        external_finals_.push_back(std::move(final));
        while (external_finals_.size() > 16) external_finals_.pop_front();
        external_tasks_.erase(task);
    }
    NotifyStatusChanged();
    return true;
}
}
