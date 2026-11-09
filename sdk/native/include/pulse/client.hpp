#pragma once
#include "sdk.hpp"
#include "ui.h"
#include "ui_model.h"
#include "model_builder.hpp"
#include "ui_events.h"
#include "dispatch.h"
#include "commands.h"
#include "tasks.h"
#include <algorithm>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>
namespace pulse::sdk {
// All RAII destruction is UI-thread-only. Functors and their allocations never
// cross the DLL boundary; only the static C trampolines and user pointers do.
class Subscription {
    struct Callback : std::enable_shared_from_this<Callback> {
        std::function<void(uint64_t, PulseStringView)> command;
        std::function<void(uint64_t, const PulseUiStateBatch&)> state;
        static void PULSE_CALL Command(void* user, uint64_t panel, PulseStringView id) noexcept {
            try { auto self = static_cast<Callback*>(user)->shared_from_this(); auto fn = self->command; fn(panel, id); } catch (...) {}
        }
        static void PULSE_CALL State(void* user, uint64_t panel, const PulseUiStateBatch* batch) noexcept {
            try { auto self = static_cast<Callback*>(user)->shared_from_this(); auto fn = self->state; if (batch) fn(panel, *batch); } catch (...) {}
        }
    };
    PulseUiModelV1 ui_{};
    uint64_t token_ = 0;
    std::shared_ptr<Callback> callback_;
public:
    Subscription() = default;
    ~Subscription() { Reset(); }
    Subscription(const Subscription&) = delete;
    Subscription& operator=(const Subscription&) = delete;
    Subscription(Subscription&& other) noexcept { *this = std::move(other); }
    Subscription& operator=(Subscription&& other) noexcept {
        if (this != &other) { Reset(); ui_ = other.ui_; token_ = std::exchange(other.token_, 0); callback_ = std::move(other.callback_); }
        return *this;
    }
    void Reset() noexcept { if (token_) ui_.unsubscribe(ui_.context, std::exchange(token_, 0)); callback_.reset(); }
    PulseStatus Command(const PulseUiModelV1& model, uint64_t panel, std::string_view id,
                        std::function<void(uint64_t, PulseStringView)> fn) {
        Reset(); auto callback = std::make_shared<Callback>(); callback->command = std::move(fn);
        ui_ = model; const auto result = model.subscribe_command(model.context, panel, View(id), Callback::Command, callback.get(), &token_);
        if (result == PULSE_OK) callback_ = std::move(callback); return result;
    }
    PulseStatus State(const PulseUiModelV1& ui, const PulseUiEventsV1& events, uint64_t panel, std::string_view key, uint32_t sources,
                      std::function<void(uint64_t, const PulseUiStateBatch&)> fn) {
        Reset(); auto callback = std::make_shared<Callback>(); callback->state = std::move(fn);
        ui_ = ui; const auto result = events.subscribe_state(events.context, panel, View(key), sources, Callback::State, callback.get(), &token_);
        if (result == PULSE_OK) callback_ = std::move(callback); return result;
    }
};
class CommandRegistration {
    struct Callback : std::enable_shared_from_this<Callback> {
        std::function<void(PulseCommandId, const PulseContextSelectionSnapshot&)> fn;
        static void PULSE_CALL Invoke(void* user, PulseCommandId id, const PulseContextSelectionSnapshot* snapshot) noexcept {
            try { auto self = static_cast<Callback*>(user)->shared_from_this(); if (snapshot) self->fn(id, *snapshot); } catch (...) {}
        }
    };
    PulseCommandsV1 api_{};
    PulseCommandId id_ = 0;
    std::shared_ptr<Callback> callback_;
public:
    CommandRegistration() = default;
    ~CommandRegistration() { Reset(); }
    CommandRegistration(const CommandRegistration&) = delete;
    CommandRegistration& operator=(const CommandRegistration&) = delete;
    CommandRegistration(CommandRegistration&& other) noexcept { *this = std::move(other); }
    CommandRegistration& operator=(CommandRegistration&& other) noexcept {
        if (this != &other) { Reset(); api_ = other.api_; id_ = std::exchange(other.id_, 0); callback_ = std::move(other.callback_); }
        return *this;
    }
    PulseCommandId Id() const noexcept { return id_; }
    void Reset() noexcept {
        if (id_) { const auto id = std::exchange(id_, 0); try { api_.unregister_command(api_.context, id); } catch (...) {} }
        callback_.reset();
    }
    PulseStatus Register(const PulseCommandsV1& api, const PulseCommandDescriptor& descriptor,
                         std::function<void(PulseCommandId, const PulseContextSelectionSnapshot&)> fn) {
        Reset();
        if (!api.register_command || !api.unregister_command || !fn) return PULSE_ERROR_ARGUMENT;
        auto callback = std::make_shared<Callback>(); callback->fn = std::move(fn);
        auto input = descriptor; input.invoke = Callback::Invoke; input.user = callback.get();
        PulseCommandId id = 0; const auto status = api.register_command(api.context, &input, &id);
        if (status == PULSE_OK) { api_ = api; id_ = id; callback_ = std::move(callback); }
        return status;
    }
};
// UI-thread owned, except work runs on a host worker. Keep the group alive until
// Collect empties it or plugin destroy after the host has drained workers AND
// cancellation callbacks. Destruction neither cancels nor waits. Unknown/evicted
// task IDs are conservatively retained until that drained destruction boundary.
// Do not destroy the group from its callbacks. Collect/CancelAll may reenter.
class TaskGroup {
    struct Slot : std::enable_shared_from_this<Slot> {
        std::function<PulseTaskState(const PulseTaskContext&)> work;
        std::function<void(PulseTaskId)> cancel;
        PulseTaskId id = 0;
        bool starting = true, collecting = false;
        static PulseTaskState PULSE_CALL Work(void* user, const PulseTaskContext* context) noexcept {
            try { auto self = static_cast<Slot*>(user)->shared_from_this(); return context ? self->work(*context) : PULSE_TASK_FAILED; }
            catch (...) { return PULSE_TASK_FAILED; }
        }
        static void PULSE_CALL Cancel(void* user, PulseTaskId id) noexcept {
            try { auto self = static_cast<Slot*>(user)->shared_from_this(); if (self->cancel) self->cancel(id); } catch (...) {}
        }
    };
    PulseTasksV1 api_{};
    std::vector<std::shared_ptr<Slot>> slots_;
public:
    explicit TaskGroup(const PulseTasksV1& api) : api_(api) {}
    TaskGroup(const TaskGroup&) = delete;
    TaskGroup& operator=(const TaskGroup&) = delete;
    TaskGroup(TaskGroup&&) = delete;
    TaskGroup& operator=(TaskGroup&&) = delete;
    size_t Pending() const noexcept { return slots_.size(); }
    PulseStatus Start(std::string_view title, std::string_view type,
                      std::function<PulseTaskState(const PulseTaskContext&)> work,
                      std::function<void(PulseTaskId)> on_cancel, PulseTaskId* out) {
        if (out) *out = 0;
        if (!out || !work || !api_.create || !api_.get || !api_.release || !api_.cancel) return PULSE_ERROR_ARGUMENT;
        auto slot = std::make_shared<Slot>(); slot->work = std::move(work); slot->cancel = std::move(on_cancel);
        slots_.push_back(slot);
        const PulseTaskDescriptor descriptor{sizeof(descriptor), View(title), View(type), Slot::Work, slot.get(), Slot::Cancel};
        PulseStatus status = PULSE_ERROR_STATE;
        try { status = api_.create(api_.context, &descriptor, &slot->id); }
        catch (...) { slot->starting = false; throw; }
        slot->starting = false;
        if (status != PULSE_OK) std::erase(slots_, slot);
        else *out = slot->id;
        return status;
    }
    size_t Collect() {
        size_t released = 0; const auto copy = slots_;
        for (const auto& slot : copy) {
            if (slot->starting || slot->collecting || !slot->id) continue;
            slot->collecting = true;
            struct Reset { bool& flag; ~Reset() { flag = false; } } reset{slot->collecting};
            bool terminal = false;
            const auto status = api_.get(api_.context, slot->id, [](void* value, const PulseTaskSnapshot* snapshot) {
                if (snapshot) *static_cast<bool*>(value) = snapshot->state >= PULSE_TASK_COMPLETED && snapshot->state <= PULSE_TASK_CANCELLED;
            }, &terminal);
            if (status == PULSE_OK && terminal && api_.release(api_.context, slot->id) == PULSE_OK) {
                std::erase(slots_, slot); ++released;
            }
        }
        return released;
    }
    void CancelAll() {
        const auto copy = slots_;
        for (const auto& slot : copy) if (!slot->starting && slot->id) api_.cancel(api_.context, slot->id);
    }
};
class Panel {
    PulseUiModelV1 ui_{};
    uint64_t id_ = 0;
public:
    Panel() = default;
    ~Panel() { Close(); }
    Panel(const Panel&) = delete;
    Panel& operator=(const Panel&) = delete;
    void Adopt(const PulseUiModelV1& ui, uint64_t id) noexcept { Close(); ui_ = ui; id_ = id; }
    uint64_t Id() const noexcept { return id_; }
    void Close() noexcept { if (id_) ui_.close(ui_.context, std::exchange(id_, 0)); }
    PulseStatus Set(std::string_view key, std::string_view value) const {
        if (!id_) return PULSE_ERROR_STATE;
        const PulseUiState state{View(key), View(value)}; return ui_.set_states(ui_.context, id_, &state, 1);
    }
    PulseStatus Get(std::string_view key, std::string& value) const {
        if (!id_) return PULSE_ERROR_STATE;
        uint32_t needed = 0;
        auto code = ui_.get_state(ui_.context, id_, View(key), nullptr, 0, &needed);
        if (code != PULSE_OK) return code;
        std::string copy(needed, '\0');
        code = ui_.get_state(ui_.context, id_, View(key), copy.data(), needed, &needed);
        if (code == PULSE_OK) value = std::move(copy); return code;
    }
};
// Post is worker-safe. Close is UI-thread-only and suppresses queued functors.
// Join all producers before destroying this object. Neither Close nor destructor
// joins workers. Slots stay owned until post returns even if callback runs first.
class DispatchQueue {
    struct State;
    struct Slot : std::enable_shared_from_this<Slot> {
        std::weak_ptr<State> owner;
        std::function<void()> fn;
        uint64_t token = 0;
        bool posting = true, finished = false;
    };
    struct State { std::mutex mutex; bool closed = false; std::vector<std::shared_ptr<Slot>> slots; };
    PulseDispatchV1 api_;
    std::shared_ptr<State> state_ = std::make_shared<State>();
    static void PULSE_CALL Invoke(void* user) noexcept {
        try {
            auto slot = static_cast<Slot*>(user)->shared_from_this();
            auto state = slot->owner.lock(); if (!state) return;
            std::function<void()> fn;
            { std::lock_guard lock(state->mutex); if (!state->closed) fn = slot->fn; }
            try { if (fn) fn(); } catch (...) {}
            { std::lock_guard lock(state->mutex); slot->finished = true; }
        } catch (...) {}
    }
public:
    explicit DispatchQueue(const PulseDispatchV1& api) : api_(api) {}
    DispatchQueue(const DispatchQueue&) = delete;
    DispatchQueue& operator=(const DispatchQueue&) = delete;
    ~DispatchQueue() { Close(); }
    PulseStatus Post(std::function<void()> fn) {
        auto state = state_; auto slot = std::make_shared<Slot>(); slot->owner = state; slot->fn = std::move(fn);
        { std::lock_guard lock(state->mutex);
          if (state->closed) return PULSE_ERROR_STATE;
          std::erase_if(state->slots, [](const auto& item) { return item->finished && !item->posting; });
          if (state->slots.size() >= PULSE_DISPATCH_MAX_PENDING) return PULSE_ERROR_STATE;
          state->slots.push_back(slot); }
        uint64_t token = 0; const auto code = api_.post(api_.context, Invoke, slot.get(), &token);
        bool cancel = false;
        { std::lock_guard lock(state->mutex); slot->token = token; slot->posting = false;
          if (code != PULSE_OK) slot->finished = true; cancel = state->closed && code == PULSE_OK; }
        if (cancel && api_.cancel(api_.context, token) == PULSE_OK) { std::lock_guard lock(state->mutex); slot->finished = true; }
        return code;
    }
    void Close() noexcept {
        auto state = state_;
        // Do not hold the mutex across host calls: a callback can close itself.
        size_t count;
        { std::lock_guard lock(state->mutex); state->closed = true; count = state->slots.size(); }
        for (size_t i = 0; i < count; ++i) {
            std::shared_ptr<Slot> slot; uint64_t token;
            { std::lock_guard lock(state->mutex); slot = state->slots[i]; token = slot->token; }
            if (token) api_.cancel(api_.context, token);
        }
        // Retain slots until UI-thread destruction after producers have joined.
        // STATE is never interpreted as permission to free running user data.

    }
};
}
