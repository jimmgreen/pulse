#pragma once
#include "../../../../sdk/native/include/pulse/tasks.h"
#include <windows.h>
#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <vector>
namespace pulse::extensions::native {
inline constexpr size_t kMaxRetainedTasks = 64;
inline constexpr size_t kMaxSessionTaskWorkers = 2;

struct TaskAction {
    std::string id, text;
    bool primary = false;
    bool operator==(const TaskAction&) const = default;
};
struct TaskSnapshot {
    uint64_t id = 0;
    std::string title, type, error, current_item;
    PulseTaskState state = PULSE_TASK_RUNNING;
    uint32_t percent = 0;
    uint64_t completed_items = 0, total_items = 0, completed_bytes = 0, total_bytes = 0;
    // Descriptor/progress fields added after V1; empty for older plugins.
    std::string source, destination, note, detail, assurance, log, result, result_path;
    std::vector<std::string> steps;
    uint32_t step = 0;
    uint64_t linked_operation = 0;
    std::vector<TaskAction> actions;
    // Set when the plugin handles actions: delivers one on the owner UI thread.
    std::function<void(const std::string&)> act;
};
struct TasksBackend {
    std::function<bool(const TaskSnapshot&)> create;
    std::function<void(const TaskSnapshot&)> update, finish;
    std::function<bool(uint64_t)> cancelled;
    std::function<void(uint64_t)> release;
};
class TasksAdapter {
public:
    TasksAdapter(HWND owner, std::function<bool()> live, TasksBackend backend,
                 std::function<void()> wake = {}, std::string package_id = {});
    ~TasksAdapter();
    PulseStatus Query(uint32_t version, void* out, uint32_t size) const noexcept;
    PulseStatus Cancel(uint64_t id) noexcept;
    void Poll() noexcept;
    void Close() noexcept;
    bool CallbackActive() const noexcept;
    size_t RetainedCount() const noexcept;
    size_t ActiveWorkerCount() const noexcept;
    size_t QueuedCount() const noexcept;
private:
    struct State;
    std::shared_ptr<State> state_;
};
}
