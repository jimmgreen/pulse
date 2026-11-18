#include "tasks_adapter.h"
#include <algorithm>
#include <atomic>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>
#include <stdexcept>
namespace pulse::extensions::native {
namespace {
std::atomic<uint64_t> next_task{0};
bool Terminal(uint32_t state) { return state >= PULSE_TASK_COMPLETED && state <= PULSE_TASK_CANCELLED; }
std::string Copy(PulseStringView text, size_t limit, bool required = false) {
    if (text.length > limit || (text.length && !text.data) || (required && !text.length)) throw std::invalid_argument("text");
    if (!text.length) return {};
    if (std::memchr(text.data, 0, text.length) || !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data, static_cast<int>(text.length), nullptr, 0)) throw std::invalid_argument("utf8");
    return {text.data, text.length};
}
PulseStringView View(const std::string& text) { return {text.data(), static_cast<uint32_t>(text.size())}; }
}
struct TasksAdapter::State : std::enable_shared_from_this<State> {
    struct Task {
        std::mutex mutex;
        TaskSnapshot snapshot;
        PulseTaskWorkFn work = nullptr;
        PulseTaskCancelFn on_cancel = nullptr;
        void* user = nullptr;
        std::atomic<bool> cancel{false}, done{false};
        bool started = false, cancel_delivered = false, released = false;
        PulseTaskState result = PULSE_TASK_FAILED;
        uint64_t revision = 0, published = 0;
        static uint32_t PULSE_CALL Cancelled(void* context) { return static_cast<Task*>(context)->cancel ? 1u : 0u; }
        static PulseStatus PULSE_CALL Report(void* context, const PulseTaskProgress* progress) {
            auto* task = static_cast<Task*>(context);
            if (!task || !progress || progress->size < sizeof(*progress)) return PULSE_ERROR_ARGUMENT;
            try {
                if ((progress->percent != UINT32_MAX && progress->percent > 100) || progress->completed_items > progress->total_items ||
                    progress->completed_bytes > progress->total_bytes) return PULSE_ERROR_ARGUMENT;
                auto error = Copy(progress->error, 4096); auto item = Copy(progress->current_item, 32768);
                std::lock_guard lock(task->mutex);
                if (task->done) return PULSE_ERROR_STATE;
                auto& value = task->snapshot;
                if (progress->completed_items < value.completed_items || progress->completed_bytes < value.completed_bytes ||
                    (progress->percent != UINT32_MAX && value.percent != UINT32_MAX && progress->percent < value.percent)) return PULSE_ERROR_ARGUMENT;
                if (value.percent == progress->percent && value.completed_items == progress->completed_items &&
                    value.total_items == progress->total_items && value.completed_bytes == progress->completed_bytes &&
                    value.total_bytes == progress->total_bytes && value.error == error && value.current_item == item) return PULSE_OK;
                ++task->revision;
                value.percent = progress->percent; value.completed_items = progress->completed_items; value.total_items = progress->total_items;
                value.completed_bytes = progress->completed_bytes; value.total_bytes = progress->total_bytes;
                value.error = std::move(error); value.current_item = std::move(item); return PULSE_OK;
            } catch (...) { return PULSE_ERROR_ARGUMENT; }
        }
        TaskSnapshot Snapshot() { std::lock_guard lock(mutex); return snapshot; }
        PulseTaskState StateOf() { std::lock_guard lock(mutex); return snapshot.state; }
        bool TakeUpdate(TaskSnapshot& value) {
            std::lock_guard lock(mutex);
            if (published == revision || Terminal(snapshot.state)) return false;
            value = snapshot; published = revision; return true;
        }
    };
    HWND owner;
    DWORD thread = GetCurrentThreadId();
    std::function<bool()> live;
    TasksBackend backend;
    bool closed = false, polling = false;
    unsigned callbacks = 0;
    std::atomic<unsigned> workers{0};
    std::map<uint64_t, std::shared_ptr<Task>> tasks;
    State(HWND window, std::function<bool()> check, TasksBackend functions) : owner(window), live(std::move(check)), backend(std::move(functions)) {}
    bool Ready() const { return owner && !closed && GetCurrentThreadId() == thread && live && live(); }
    void DeliverCancel(const std::shared_ptr<Task>& task) {
        if (!task->cancel || task->cancel_delivered) return;
        task->cancel_delivered = true;
        if (task->on_cancel) { ++callbacks; try { task->on_cancel(task->user, task->snapshot.id); } catch (...) {} --callbacks; }
    }
    void Signal(const std::shared_ptr<Task>& task) {
        std::lock_guard lock(task->mutex);
        if (!Terminal(task->snapshot.state) && !task->cancel) { task->cancel = true; task->snapshot.state = PULSE_TASK_CANCELLING; ++task->revision; }
    }
    void Finalize(const std::shared_ptr<Task>& task) {
        if (!task->done || (task->cancel && !task->cancel_delivered)) return;
        TaskSnapshot value;
        { std::lock_guard lock(task->mutex); if (Terminal(task->snapshot.state)) return;
          task->snapshot.state = task->result; task->published = ++task->revision; value = task->snapshot; }
        if (!task->released && backend.finish) backend.finish(value);
    }
    void Start(const std::shared_ptr<Task>& task) {
        if (task->started || task->done || task->cancel || workers >= 2 || closed) return;
        task->started = true; ++workers; auto self = shared_from_this();
        try {
            std::thread([self, task] {
                PulseTaskState result = PULSE_TASK_FAILED;
                try { PulseTaskContext context{task->snapshot.id, task.get(), Task::Report, Task::Cancelled}; result = task->work(task->user, &context); }
                catch (...) { result = PULSE_TASK_FAILED; }
                { std::lock_guard lock(task->mutex); task->result = Terminal(result) ? result : PULSE_TASK_FAILED; task->done = true; }
                --self->workers;
            }).detach();
        } catch (...) { task->result = PULSE_TASK_FAILED; task->done = true; --workers; }
    }
    void Close() {
        if (closed || GetCurrentThreadId() != thread) return;
        closed = true;
        // closed blocks mutation through the public table, so cancellation can
        // drain without allocating a second container during exception cleanup.
        for (auto& [id, task] : tasks) {
            (void)id;
            Signal(task); DeliverCancel(task);
            if (!task->started && !task->done) { task->result = PULSE_TASK_CANCELLED; task->done = true; }
            try { Finalize(task); } catch (...) {}
            if (Terminal(task->StateOf()) && !task->released) {
                task->released = true;
                try { if (backend.release) backend.release(task->snapshot.id); } catch (...) {}
            }
        }
    }

    static PulseStatus PULSE_CALL Create(void* context, const PulseTaskDescriptor* descriptor, PulseTaskId* out) {
        if (out) *out = 0;
        try {
            auto* raw = static_cast<State*>(context);
            if (!raw || !descriptor || descriptor->size < sizeof(*descriptor) || !descriptor->work || !out) return PULSE_ERROR_ARGUMENT;
            auto self = raw->shared_from_this();
            if (!self->Ready() || self->callbacks || self->polling) return PULSE_ERROR_STATE;
            auto task = std::make_shared<Task>(); task->snapshot.title = Copy(descriptor->title, 128, true);
            task->snapshot.type = Copy(descriptor->type, 128, true); task->snapshot.percent = UINT32_MAX;
            task->work = descriptor->work; task->on_cancel = descriptor->on_cancel; task->user = descriptor->user;
            if (self->tasks.size() >= 16) {
                auto old = std::find_if(self->tasks.begin(), self->tasks.end(), [](const auto& item) { return Terminal(item.second->StateOf()); });
                if (old == self->tasks.end()) return PULSE_ERROR_UNAVAILABLE;
                if (self->backend.release) self->backend.release(old->first); self->tasks.erase(old);
            }
            auto id = next_task.load(); do { if (id == UINT64_MAX) return PULSE_ERROR_UNAVAILABLE; } while (!next_task.compare_exchange_weak(id, id + 1));
            task->snapshot.id = id + 1;
            if (!self->backend.create || !self->backend.create(task->snapshot)) return PULSE_ERROR_UNAVAILABLE;
            try { self->tasks.emplace(id + 1, task); } catch (...) { if (self->backend.release) self->backend.release(id + 1); throw; }
            *out = id + 1; self->Start(task); return PULSE_OK;
        } catch (...) { return PULSE_ERROR_ARGUMENT; }
    }
    static PulseStatus PULSE_CALL Cancel(void* context, PulseTaskId id) {
        try {
            auto* self = static_cast<State*>(context); if (!self || !id) return PULSE_ERROR_ARGUMENT;
            if (!self->Ready()) return PULSE_ERROR_STATE;
            auto found = self->tasks.find(id); if (found == self->tasks.end() || Terminal(found->second->StateOf())) return PULSE_ERROR_STATE;
            self->Signal(found->second); return PULSE_OK;
        } catch (...) { return PULSE_ERROR_STATE; }
    }
    static PulseStatus PULSE_CALL Get(void* context, PulseTaskId id, PulseTaskSnapshotFn callback, void* user) {
        try {
            auto* raw = static_cast<State*>(context); if (!raw || !id || !callback) return PULSE_ERROR_ARGUMENT;
            auto self = raw->shared_from_this(); if (!self->Ready() || self->callbacks) return PULSE_ERROR_STATE;
            auto found = self->tasks.find(id); if (found == self->tasks.end()) return PULSE_ERROR_STATE;
            const auto value = found->second->Snapshot();
            PulseTaskSnapshot snapshot{sizeof(snapshot), value.id, value.state, {sizeof(PulseTaskProgress), value.percent,
                value.completed_items, value.total_items, value.completed_bytes, value.total_bytes, View(value.error), View(value.current_item)}};
            ++self->callbacks; try { callback(user, &snapshot); } catch (...) { --self->callbacks; return PULSE_ERROR_STATE; } --self->callbacks;
            return PULSE_OK;
        } catch (...) { return PULSE_ERROR_STATE; }
    }
    static PulseStatus PULSE_CALL Release(void* context, PulseTaskId id) {
        try {
            auto* self = static_cast<State*>(context); if (!self || !id) return PULSE_ERROR_ARGUMENT;
            if (!self->Ready() || self->callbacks) return PULSE_ERROR_STATE;
            auto found = self->tasks.find(id); if (found == self->tasks.end() || !Terminal(found->second->StateOf())) return PULSE_ERROR_STATE;
            if (self->backend.release) self->backend.release(id); self->tasks.erase(found); return PULSE_OK;
        } catch (...) { return PULSE_ERROR_STATE; }
    }
    void Poll() {
        if (GetCurrentThreadId() != thread || polling || callbacks) return;
        if (!closed && !Ready()) Close();
        polling = true;
        std::vector<std::shared_ptr<Task>> copy; for (auto& [id, task] : tasks) { (void)id; copy.push_back(task); }
        for (const auto& task : copy) {
            if (!closed && !Terminal(task->StateOf()) && backend.cancelled && backend.cancelled(task->snapshot.id)) Signal(task);
            DeliverCancel(task);
            if (task->cancel && !task->started && !task->done) { task->result = PULSE_TASK_CANCELLED; task->done = true; }
            Finalize(task);
            if (closed && Terminal(task->StateOf()) && !task->released) {
                task->released = true; if (backend.release) backend.release(task->snapshot.id);
            }
            TaskSnapshot update;
            if (!closed && backend.update && task->TakeUpdate(update)) backend.update(update);
            Start(task);
        }
        polling = false;
    }
};
TasksAdapter::TasksAdapter(HWND owner, std::function<bool()> live, TasksBackend backend) : state_(std::make_shared<State>(owner, std::move(live), std::move(backend))) {}
TasksAdapter::~TasksAdapter() { Close(); }
PulseStatus TasksAdapter::Query(uint32_t version, void* out, uint32_t size) const noexcept {
    if (version != PULSE_TASKS_VERSION_1) return PULSE_ERROR_ABI;
    if (!out || size < sizeof(PulseTasksV1)) return PULSE_ERROR_ARGUMENT;
    PulseTasksV1 table{sizeof(table), PULSE_TASKS_VERSION_1, state_.get(), State::Create, State::Cancel, State::Get, State::Release};
    std::memcpy(out, &table, sizeof(table)); return PULSE_OK;
}
PulseStatus TasksAdapter::Cancel(uint64_t id) noexcept { return State::Cancel(state_.get(), id); }
void TasksAdapter::Poll() noexcept { auto self = state_; try { self->Poll(); } catch (...) { self->polling = false; try { self->Close(); } catch (...) {} } }
void TasksAdapter::Close() noexcept { auto self = state_; try { self->Close(); } catch (...) {} }
bool TasksAdapter::CallbackActive() const noexcept {
    if (state_->workers != 0 || state_->callbacks != 0) return true;
    for (const auto& [id, task] : state_->tasks) {
        (void)id;
        if (!Terminal(task->StateOf())) return true;
    }
    return false;
}
}
