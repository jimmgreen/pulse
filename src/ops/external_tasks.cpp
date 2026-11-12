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
    if (from.external_step && from.external_step <= to.external_steps.size()) to.external_step = from.external_step;
    to.external_note = from.external_note.substr(0, 256);
    to.external_detail = from.external_detail.substr(0, 1024);
    to.external_assurance = from.external_assurance.substr(0, 256);
    to.external_log = from.external_log.substr(0, 2048);
    to.external_result = from.external_result.substr(0, 256);
    to.external_result_path = from.external_result_path.substr(0, 32768);
    to.external_actions.assign(from.external_actions.begin(),
        from.external_actions.begin() + static_cast<ptrdiff_t>((std::min<size_t>)(from.external_actions.size(), 3)));
    to.external_linked = from.external_linked;
}
}
uint64_t OpsManager::CreateExternalTask(std::wstring title, std::wstring type_label, std::wstring source,
                                        std::wstring destination, std::vector<std::wstring> steps) {
    uint64_t id = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_ || update_preparing_ || external_tasks_.size() >= 32 || title.empty() || title.size() > 256 || type_label.size() > 128) return 0;
        id = next_seq_++;
        ExternalTaskState task;
        task.status.external = true;
        task.status.external_title = std::move(title);
        task.status.external_kind = std::move(type_label);
        task.status.source_label = std::move(source);
        task.status.destination_label = std::move(destination);
        if (steps.size() > 3) steps.resize(3);
        task.status.external_steps = std::move(steps);
        task.status.external_step = task.status.external_steps.empty() ? 0 : 1;
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
        auto& status = task->second.status;
        // Byte progress gives the task a speed, a graph and a remaining time. While a host
        // operation writes its output (linked), that operation's own counters do not apply.
        if (status.total_bytes > 0 && status.external_linked == 0) {
            const ULONGLONG now = GetTickCount64();
            auto& rate = task->second.rate;
            if (!task->second.rate_started) { rate.Reset(now, status.transferred_bytes); task->second.rate_started = true; }
            rate.Observe(now, status.transferred_bytes, status.total_bytes);
            status.speed_basis = OpSpeedBasis::Bytes;
            status.bytes_per_second = rate.speed();
            status.peak_bytes_per_second = (std::max)(status.peak_bytes_per_second, status.bytes_per_second);
            status.eta_seconds = rate.eta_seconds();
        } else if (status.external_linked != 0) {
            status.bytes_per_second = 0.0;
            status.eta_seconds = 0;
        }
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
        final.external_linked = 0;
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
        while (external_finals_.size() > 16) {
            external_action_handlers_.erase(external_finals_.front().task_id);
            external_finals_.pop_front();
        }
        external_tasks_.erase(task);
    }
    NotifyStatusChanged();
    return true;
}
void OpsManager::SetExternalTaskActionHandler(uint64_t id, std::function<void(const std::string&)> handler) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (handler && external_tasks_.contains(id)) external_action_handlers_[id] = std::move(handler);
}
bool OpsManager::InvokeExternalTaskAction(uint64_t id, const std::string& action) {
    std::function<void(const std::string&)> handler;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = external_action_handlers_.find(id);
        if (found == external_action_handlers_.end() || external_tasks_.contains(id)) return false;
        handler = std::move(found->second);
        external_action_handlers_.erase(found);
    }
    handler(action); // outside the lock: the plugin may start the follow-up task
    return true;
}
}
