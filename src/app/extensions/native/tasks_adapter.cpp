#include "tasks_adapter.h"
#include "crash_guard.h"
#include "worker_budget.h"
#include <algorithm>
#include <atomic>
#include <cstring>
#include <map>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>
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
bool ActionId(const std::string& id) {
    if (id.empty() || id.size() > 64) return false;
    for (const char c : id)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-')) return false;
    return true;
}
// Fields after V1, all or nothing: a report either covers the whole struct or ends at current_item.
struct ProgressExtras {
    std::string note, detail, assurance, log, result, result_path;
    std::vector<TaskAction> actions;
    uint32_t step = 0;
    uint64_t linked_operation = 0;
};
ProgressExtras CopyExtras(const PulseTaskProgress& progress, size_t step_count) {
    ProgressExtras out;
    if (progress.size < sizeof(PulseTaskProgress)) return out;
    if (progress.step > step_count || progress.action_count > 3 || (progress.action_count && !progress.actions))
        throw std::invalid_argument("extras");
    out.step = progress.step; out.linked_operation = progress.linked_operation;
    out.note = Copy(progress.note, 256); out.detail = Copy(progress.detail, 1024); out.assurance = Copy(progress.assurance, 256);
    out.log = Copy(progress.log, 2048);
    out.result = Copy(progress.result, 256);
    out.result_path = Copy(progress.result_path, 32768);
    for (uint32_t i = 0; i < progress.action_count; ++i) {
        const auto& action = progress.actions[i];
        if (action.size < sizeof(PulseTaskAction)) throw std::invalid_argument("action");
        TaskAction value{Copy(action.id, 64, true), Copy(action.text, 64, true), (action.flags & PULSE_TASK_ACTION_PRIMARY) != 0};
        if (!ActionId(value.id)) throw std::invalid_argument("action id");
        out.actions.push_back(std::move(value));
    }
    return out;
}
}
struct TasksAdapter::State : std::enable_shared_from_this<State> {
    struct Task {
        std::mutex mutex;
        std::weak_ptr<State> owner_state;
        TaskSnapshot snapshot;
        PulseTaskWorkFn work = nullptr;
        PulseTaskCancelFn on_cancel = nullptr;
        void* user = nullptr;
        PulseTaskActionFn on_action = nullptr;
        void* action_user = nullptr;
        bool acted = false;
        std::atomic<bool> cancel{false}, done{false};
        bool started = false, cancel_delivered = false, released = false;
        PulseTaskState result = PULSE_TASK_FAILED;
        uint64_t revision = 0, published = 0;
        static uint32_t PULSE_CALL Cancelled(void* context) { return static_cast<Task*>(context)->cancel ? 1u : 0u; }
        static PulseStatus PULSE_CALL Report(void* context, const PulseTaskProgress* progress) {
            auto* task = static_cast<Task*>(context);
            if (!task || !progress || progress->size < PULSE_TASK_PROGRESS_SIZE_V1) return PULSE_ERROR_ARGUMENT;
            try {
                if ((progress->percent != UINT32_MAX && progress->percent > 100) || progress->completed_items > progress->total_items ||
                    progress->completed_bytes > progress->total_bytes) return PULSE_ERROR_ARGUMENT;
                auto error = Copy(progress->error, 4096); auto item = Copy(progress->current_item, 32768);
                auto extras = CopyExtras(*progress, task->snapshot.steps.size());
                std::weak_ptr<State> weak_owner;
                {
                    std::lock_guard lock(task->mutex);
                    if (task->done) return PULSE_ERROR_STATE;
                    auto& value = task->snapshot;
                    if (progress->completed_items < value.completed_items || progress->completed_bytes < value.completed_bytes ||
                        (progress->percent != UINT32_MAX && value.percent != UINT32_MAX && progress->percent < value.percent)) return PULSE_ERROR_ARGUMENT;
                    const uint32_t step = extras.step ? extras.step : value.step;
                    if (value.percent == progress->percent && value.completed_items == progress->completed_items &&
                        value.total_items == progress->total_items && value.completed_bytes == progress->completed_bytes &&
                        value.total_bytes == progress->total_bytes && value.error == error && value.current_item == item &&
                        value.step == step && value.linked_operation == extras.linked_operation && value.note == extras.note &&
                        value.detail == extras.detail && value.assurance == extras.assurance && value.actions == extras.actions &&
                        value.log == extras.log && value.result == extras.result &&
                        value.result_path == extras.result_path) return PULSE_OK;
                    ++task->revision;
                    value.percent = progress->percent; value.completed_items = progress->completed_items; value.total_items = progress->total_items;
                    value.completed_bytes = progress->completed_bytes; value.total_bytes = progress->total_bytes;
                    value.error = std::move(error); value.current_item = std::move(item);
                    value.step = step; value.linked_operation = extras.linked_operation;
                    value.note = std::move(extras.note); value.detail = std::move(extras.detail);
                    value.assurance = std::move(extras.assurance); value.actions = std::move(extras.actions);
                    value.log = std::move(extras.log);
                    value.result = std::move(extras.result); value.result_path = std::move(extras.result_path);
                    weak_owner = task->owner_state;
                }
                if (auto owner = weak_owner.lock()) owner->NotifyWake();
                return PULSE_OK;
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
    std::function<void()> wake;
    std::string package_id;
    std::atomic<bool> closed{false};
    bool polling = false;
    unsigned callbacks = 0, acting = 0;
    std::atomic<unsigned> workers{0};
    std::atomic<size_t> queued_count{0};
    std::atomic<uint64_t> waiter_id{0};
    std::map<uint64_t, std::shared_ptr<Task>> tasks;
    State(HWND window, std::function<bool()> check, TasksBackend functions,
          std::function<void()> on_wake, std::string pkg_id)
        : owner(window), live(std::move(check)), backend(std::move(functions)),
          wake(std::move(on_wake)), package_id(std::move(pkg_id)) {
        (void)owner;
    }
    ~State() { UnregisterPoolWaiter(); }
    void UnregisterPoolWaiter() noexcept {
        const uint64_t id = waiter_id.exchange(0, std::memory_order_acq_rel);
        if (id != 0) GlobalTaskWorkerPool().UnregisterWaiter(id);
    }
    void NotifyWake() const noexcept {
        if (!wake) return;
        try { wake(); } catch (...) {}
    }
    bool Ready() const { return !closed.load(std::memory_order_relaxed) && GetCurrentThreadId() == thread && live && live(); }
    size_t CountUnstarted() const noexcept {
        size_t count = 0;
        for (const auto& [id, task] : tasks) {
            (void)id;
            if (!task->started && !task->done && !task->cancel.load(std::memory_order_acquire)) ++count;
        }
        return count;
    }
    void DeliverCancel(const std::shared_ptr<Task>& task) {
        if (!task->cancel || task->cancel_delivered) return;
        task->cancel_delivered = true;
        if (task->on_cancel) {
            ++callbacks;
            try {
                const crash_guard::CallScope call_scope(package_id.c_str());
                task->on_cancel(task->user, task->snapshot.id);
            } catch (...) {}
            --callbacks;
        }
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
    void StartWithPermit(const std::shared_ptr<Task>& task, WorkerPermit permit) {
        task->started = true; ++workers; auto self = shared_from_this();
        try {
            std::thread([self, task, permit = std::move(permit)]() mutable {
                PulseTaskState result = PULSE_TASK_FAILED;
                try {
                    const crash_guard::CallScope call_scope(self->package_id.c_str());
                    PulseTaskContext context{task->snapshot.id, task.get(), Task::Report, Task::Cancelled};
                    result = task->work(task->user, &context);
                } catch (...) { result = PULSE_TASK_FAILED; }
                { std::lock_guard lock(task->mutex); task->result = Terminal(result) ? result : PULSE_TASK_FAILED; task->done = true; }
                --self->workers;
                permit.Release();
                self->NotifyWake();
            }).detach();
        } catch (...) {
            task->result = PULSE_TASK_FAILED;
            task->done = true;
            --workers;
            permit.Release();
            NotifyWake();
        }
    }
    void StartPending() {
        if (closed.load(std::memory_order_relaxed)) {
            queued_count.store(0, std::memory_order_seq_cst);
            return;
        }
        const size_t initial_pending = CountUnstarted();
        if (initial_pending == 0) {
            queued_count.store(0, std::memory_order_seq_cst);
            return;
        }
        // Publish queued_count before TryAcquire so a concurrent ReleaseOne on
        // another session's worker cannot miss waking this session.
        queued_count.store(initial_pending, std::memory_order_seq_cst);
        for (const auto& [id, task] : tasks) {
            (void)id;
            if (task->started || task->done || task->cancel.load(std::memory_order_acquire)) continue;
            if (workers.load(std::memory_order_acquire) >= kMaxSessionTaskWorkers || closed.load(std::memory_order_relaxed)) break;
            auto permit = GlobalTaskWorkerPool().TryAcquire();
            if (!permit) break;
            StartWithPermit(task, std::move(permit));
        }
        queued_count.store(CountUnstarted(), std::memory_order_seq_cst);
    }
    // A button on the ended task's window. Runs outside Poll so the plugin may create
    // the follow-up task from it; once per task, never after stop.
    void Act(const std::shared_ptr<Task>& task, const std::string& action) {
        if (!Ready() || polling || callbacks || task->acted || !task->on_action || !Terminal(task->StateOf())) return;
        const auto value = task->Snapshot();
        if (std::none_of(value.actions.begin(), value.actions.end(), [&](const TaskAction& item) { return item.id == action; })) return;
        task->acted = true;
        ++acting;
        try {
            const crash_guard::CallScope call_scope(package_id.c_str());
            task->on_action(task->action_user, value.id, View(action));
        } catch (...) {}
        --acting;
        NotifyWake();
    }
    void Close() {
        if (closed.load(std::memory_order_relaxed) || GetCurrentThreadId() != thread) return;
        closed.store(true, std::memory_order_release);
        queued_count.store(0, std::memory_order_seq_cst);
        UnregisterPoolWaiter();
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
        if (workers == 0 && callbacks == 0) {
            std::erase_if(tasks, [](const auto& item) {
                return Terminal(item.second->StateOf()) && item.second->released;
            });
        }
    }

    static PulseStatus PULSE_CALL Create(void* context, const PulseTaskDescriptor* descriptor, PulseTaskId* out) {
        if (out) *out = 0;
        try {
            auto* raw = static_cast<State*>(context);
            if (!raw || !descriptor || descriptor->size < PULSE_TASK_DESCRIPTOR_SIZE_V1 || !descriptor->work || !out) return PULSE_ERROR_ARGUMENT;
            auto self = raw->shared_from_this();
            if (!self->Ready() || self->callbacks || self->polling) return PULSE_ERROR_STATE;
            auto task = std::make_shared<Task>(); task->owner_state = self;
            task->snapshot.title = Copy(descriptor->title, 128, true);
            task->snapshot.type = Copy(descriptor->type, 128, true); task->snapshot.percent = UINT32_MAX;
            task->work = descriptor->work; task->on_cancel = descriptor->on_cancel; task->user = descriptor->user;
            if (descriptor->size >= sizeof(*descriptor)) {
                if (descriptor->step_count > 3 || (descriptor->step_count && !descriptor->steps)) return PULSE_ERROR_ARGUMENT;
                task->snapshot.source = Copy(descriptor->source, 1024);
                task->snapshot.destination = Copy(descriptor->destination, 1024);
                for (uint32_t i = 0; i < descriptor->step_count; ++i) task->snapshot.steps.push_back(Copy(descriptor->steps[i], 64, true));
                if (!task->snapshot.steps.empty()) task->snapshot.step = 1;
                task->on_action = descriptor->on_action; task->action_user = descriptor->action_user;
            }
            if (task->on_action) {
                std::weak_ptr<State> weak = self;
                std::weak_ptr<Task> weak_task = task;
                task->snapshot.act = [weak, weak_task](const std::string& action) {
                    const auto state = weak.lock(); const auto target = weak_task.lock();
                    if (state && target) state->Act(target, action);
                };
            }
            // Execution ownership records are never auto-evicted while un-released;
            // reject new tasks once retained capacity is full until Release(id).
            if (self->tasks.size() >= kMaxRetainedTasks) return PULSE_ERROR_UNAVAILABLE;
            auto id = next_task.load(); do { if (id == UINT64_MAX) return PULSE_ERROR_UNAVAILABLE; } while (!next_task.compare_exchange_weak(id, id + 1));
            task->snapshot.id = id + 1;
            if (!self->backend.create || !self->backend.create(task->snapshot)) return PULSE_ERROR_UNAVAILABLE;
            try { self->tasks.emplace(id + 1, task); } catch (...) { if (self->backend.release) self->backend.release(id + 1); throw; }
            *out = id + 1; self->StartPending(); self->NotifyWake(); return PULSE_OK;
        } catch (...) { return PULSE_ERROR_ARGUMENT; }
    }
    static PulseStatus PULSE_CALL Cancel(void* context, PulseTaskId id) {
        try {
            auto* self = static_cast<State*>(context); if (!self || !id) return PULSE_ERROR_ARGUMENT;
            if (!self->Ready()) return PULSE_ERROR_STATE;
            auto found = self->tasks.find(id); if (found == self->tasks.end() || Terminal(found->second->StateOf())) return PULSE_ERROR_STATE;
            self->Signal(found->second);
            self->queued_count.store(self->CountUnstarted(), std::memory_order_seq_cst);
            self->NotifyWake();
            return PULSE_OK;
        } catch (...) { return PULSE_ERROR_STATE; }
    }
    static PulseStatus PULSE_CALL Get(void* context, PulseTaskId id, PulseTaskSnapshotFn callback, void* user) {
        try {
            auto* raw = static_cast<State*>(context); if (!raw || !id || !callback) return PULSE_ERROR_ARGUMENT;
            auto self = raw->shared_from_this(); if (!self->Ready() || self->callbacks) return PULSE_ERROR_STATE;
            auto found = self->tasks.find(id); if (found == self->tasks.end()) return PULSE_ERROR_STATE;
            const auto value = found->second->Snapshot();
            std::vector<PulseTaskAction> actions;
            for (const auto& action : value.actions)
                actions.push_back({sizeof(PulseTaskAction), action.primary ? PULSE_TASK_ACTION_PRIMARY : 0u, View(action.id), View(action.text)});
            PulseTaskSnapshot snapshot{sizeof(snapshot), value.id, value.state, {sizeof(PulseTaskProgress), value.percent,
                value.completed_items, value.total_items, value.completed_bytes, value.total_bytes, View(value.error), View(value.current_item),
                value.step, static_cast<uint32_t>(actions.size()), value.linked_operation, View(value.note), View(value.detail),
                View(value.assurance), actions.empty() ? nullptr : actions.data(), View(value.log), View(value.result),
                View(value.result_path)}};
            ++self->callbacks;
            try {
                const crash_guard::CallScope call_scope(self->package_id.c_str());
                callback(user, &snapshot);
            } catch (...) { --self->callbacks; return PULSE_ERROR_STATE; }
            --self->callbacks;
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
        if (!closed.load(std::memory_order_relaxed) && !Ready()) Close();
        polling = true;
        std::vector<std::shared_ptr<Task>> copy; for (auto& [id, task] : tasks) { (void)id; copy.push_back(task); }
        for (const auto& task : copy) {
            const bool is_closed = closed.load(std::memory_order_relaxed);
            if (!is_closed && !Terminal(task->StateOf()) && backend.cancelled && backend.cancelled(task->snapshot.id)) Signal(task);
            DeliverCancel(task);
            if (task->cancel && !task->started && !task->done) { task->result = PULSE_TASK_CANCELLED; task->done = true; }
            Finalize(task);
            if (is_closed && Terminal(task->StateOf()) && !task->released) {
                task->released = true; if (backend.release) backend.release(task->snapshot.id);
            }
            TaskSnapshot update;
            if (!is_closed && backend.update && task->TakeUpdate(update)) backend.update(update);
        }
        if (!closed.load(std::memory_order_relaxed) && !tasks.empty()) {
            StartPending();
        } else {
            queued_count.store(0, std::memory_order_seq_cst);
        }
        if (closed.load(std::memory_order_relaxed) && workers == 0 && callbacks == 0) {
            std::erase_if(tasks, [](const auto& item) {
                return Terminal(item.second->StateOf()) && item.second->released;
            });
        }
        polling = false;
    }
};
TasksAdapter::TasksAdapter(HWND owner, std::function<bool()> live, TasksBackend backend,
                           std::function<void()> wake, std::string package_id)
    : state_(std::make_shared<State>(owner, std::move(live), std::move(backend),
                                     std::move(wake), std::move(package_id))) {
    std::weak_ptr<State> weak = state_;
    state_->waiter_id.store(
        GlobalTaskWorkerPool().RegisterWaiter([weak]() noexcept {
            if (const auto s = weak.lock()) {
                if (!s->closed.load(std::memory_order_acquire) &&
                    s->queued_count.load(std::memory_order_seq_cst) > 0) {
                    s->NotifyWake();
                }
            }
        }),
        std::memory_order_release);
}
TasksAdapter::~TasksAdapter() {
    Close();
    if (state_) state_->UnregisterPoolWaiter();
}
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
    if (state_->workers != 0 || state_->callbacks != 0 || state_->acting != 0) return true;
    for (const auto& [id, task] : state_->tasks) {
        (void)id;
        if (!Terminal(task->StateOf())) return true;
    }
    return false;
}
size_t TasksAdapter::RetainedCount() const noexcept {
    return state_->tasks.size();
}
size_t TasksAdapter::ActiveWorkerCount() const noexcept {
    return state_->workers.load(std::memory_order_acquire);
}
size_t TasksAdapter::QueuedCount() const noexcept {
    return state_->queued_count.load(std::memory_order_acquire);
}
}
