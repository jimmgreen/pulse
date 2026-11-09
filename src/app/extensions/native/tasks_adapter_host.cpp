#include "tasks_adapter_host.h"
#include "../../app_runtime.h"
#include <map>

namespace pulse::extensions::native {
namespace {
std::wstring Wide(const std::string& text) {
    if (text.empty()) return {};
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring result(static_cast<size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), result.data(), count);
    return result;
}
ops::OpStatus Progress(const TaskSnapshot& source) {
    ops::OpStatus value;
    value.percent = source.percent == UINT32_MAX ? -1.0f : static_cast<float>(source.percent);
    value.completed_items = source.completed_items; value.total_items = source.total_items;
    value.transferred_bytes = source.completed_bytes; value.total_bytes = source.total_bytes;
    value.current_item = Wide(source.current_item); value.last_error = Wide(source.error);
    return value;
}
}
TasksBackend MakeTasksBackend(HWND owner) {
    auto ids = std::make_shared<std::map<uint64_t, uint64_t>>();
    TasksBackend backend;
    backend.create = [owner, ids](const TaskSnapshot& value) {
        auto* app = GetAppState(owner);
        if (!app) return false;
        const auto [slot, inserted] = ids->emplace(value.id, 0);
        if (!inserted) return false;
        const auto id = app->ops.CreateExternalTask(Wide(value.title), Wide(value.type));
        if (!id) { ids->erase(slot); return false; }
        slot->second = id;
        return true;
    };
    backend.update = [owner, ids](const TaskSnapshot& value) {
        auto* app = GetAppState(owner); const auto found = ids->find(value.id);
        if (app && found != ids->end()) app->ops.UpdateExternalTask(found->second, Progress(value));
    };
    backend.finish = [owner, ids](const TaskSnapshot& value) {
        auto* app = GetAppState(owner); const auto found = ids->find(value.id);
        if (!app || found == ids->end()) return;
        const auto outcome = value.state == PULSE_TASK_COMPLETED ? ops::ExternalOutcome::Completed :
            value.state == PULSE_TASK_PARTIAL ? ops::ExternalOutcome::Partial :
            value.state == PULSE_TASK_CANCELLED ? ops::ExternalOutcome::Cancelled : ops::ExternalOutcome::Failed;
        app->ops.FinishExternalTask(found->second, outcome, Progress(value));
    };
    backend.cancelled = [owner, ids](uint64_t id) {
        const auto* app = GetAppState(owner); const auto found = ids->find(id);
        return !app || found == ids->end() || app->ops.ExternalTaskCancelled(found->second);
    };
    backend.release = [owner, ids](uint64_t id) {
        const auto found = ids->find(id);
        if (found == ids->end()) return;
        // Also retires a host record if creating the native handle failed.
        if (auto* app = GetAppState(owner)) {
            const auto status = app->ops.QueryTask(found->second);
            if (status && status->active) app->ops.FinishExternalTask(found->second, ops::ExternalOutcome::Cancelled, *status);
        }
        ids->erase(found);
    };
    return backend;
}
}
