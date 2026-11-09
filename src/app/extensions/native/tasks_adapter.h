#pragma once
#include "../../../../sdk/native/include/pulse/tasks.h"
#include <windows.h>
#include <functional>
#include <memory>
#include <string>
namespace pulse::extensions::native {
struct TaskSnapshot {
    uint64_t id = 0;
    std::string title, type, error, current_item;
    PulseTaskState state = PULSE_TASK_RUNNING;
    uint32_t percent = 0;
    uint64_t completed_items = 0, total_items = 0, completed_bytes = 0, total_bytes = 0;
};
struct TasksBackend {
    std::function<bool(const TaskSnapshot&)> create;
    std::function<void(const TaskSnapshot&)> update, finish;
    std::function<bool(uint64_t)> cancelled;
    std::function<void(uint64_t)> release;
};
class TasksAdapter {
public:
    TasksAdapter(HWND owner, std::function<bool()> live, TasksBackend backend);
    ~TasksAdapter();
    PulseStatus Query(uint32_t version, void* out, uint32_t size) const noexcept;
    PulseStatus Cancel(uint64_t id) noexcept;
    void Poll() noexcept;
    void Close() noexcept;
    bool CallbackActive() const noexcept;
private:
    struct State;
    std::shared_ptr<State> state_;
};
}
