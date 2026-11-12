#pragma once
#include "sdk.hpp"
#include "ui.h"
#include "ui_model.h"
#include "model_builder.hpp"
#include "ui_events.h"
#include "dispatch.h"
#include "commands.h"
#include "diagnostics.h"
#include "dialogs.h"
#include "icons.h"
#include "pickers.h"
#include "tasks.h"
#include "workbench.h"
#include <algorithm>
#include <cstdio>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
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
    PulseCommandsV2 v2_{};
    bool has_v2_ = false;
    PulseCommandId id_ = 0;
    std::shared_ptr<Callback> callback_;
public:
    CommandRegistration() = default;
    ~CommandRegistration() { Reset(); }
    CommandRegistration(const CommandRegistration&) = delete;
    CommandRegistration& operator=(const CommandRegistration&) = delete;
    CommandRegistration(CommandRegistration&& other) noexcept { *this = std::move(other); }
    CommandRegistration& operator=(CommandRegistration&& other) noexcept {
        if (this != &other) {
            Reset();
            api_ = other.api_;
            v2_ = other.v2_;
            has_v2_ = std::exchange(other.has_v2_, false);
            id_ = std::exchange(other.id_, 0);
            callback_ = std::move(other.callback_);
        }
        return *this;
    }
    PulseCommandId Id() const noexcept { return id_; }
    void Reset() noexcept {
        if (id_) { const auto id = std::exchange(id_, 0); try { api_.unregister_command(api_.context, id); } catch (...) {} }
        has_v2_ = false;
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
    PulseStatus Register(const PulseCommandsV2& api, const PulseCommandDescriptor& descriptor,
                         std::function<void(PulseCommandId, const PulseContextSelectionSnapshot&)> fn) {
        Reset();
        if (!api.register_command || !api.unregister_command || !fn) return PULSE_ERROR_ARGUMENT;
        auto callback = std::make_shared<Callback>(); callback->fn = std::move(fn);
        auto input = descriptor; input.invoke = Callback::Invoke; input.user = callback.get();
        PulseCommandId id = 0; const auto status = api.register_command(api.context, &input, &id);
        if (status == PULSE_OK) {
            api_ = {sizeof(PulseCommandsV1), PULSE_COMMANDS_VERSION_1, api.context, api.register_command, api.unregister_command};
            v2_ = api;
            has_v2_ = true;
            id_ = id;
            callback_ = std::move(callback);
        }
        return status;
    }
    PulseStatus Update(const PulseCommandDescriptor& descriptor) {
        if (!id_ || !has_v2_ || !v2_.update_command || !callback_) return PULSE_ERROR_UNAVAILABLE;
        auto input = descriptor; input.invoke = Callback::Invoke; input.user = callback_.get();
        return v2_.update_command(v2_.context, id_, &input);
    }
    PulseStatus SetFlags(uint32_t flags) {
        if (!id_ || !has_v2_ || !v2_.set_command_flags) return PULSE_ERROR_UNAVAILABLE;
        return v2_.set_command_flags(v2_.context, id_, flags);
    }
};
inline constexpr uint32_t DialogIconFromId(PulseIconId id) noexcept {
    return id | PULSE_DIALOG_ICON_EXT_FLAG;
}
class IconBuilder {
    std::string name_;
    uint32_t view_box_milli_ = 16000;
    struct LayerSpec {
        std::string path;
        uint32_t draw_flags = PULSE_ICON_DRAW_STROKE;
        uint32_t color_role = PULSE_ICON_COLOR_INHERIT;
        uint32_t argb = 0;
        uint32_t stroke_width_milli = 0;
        uint32_t opacity_milli = 1000;
    };
    std::vector<LayerSpec> layers_;
    mutable std::vector<PulseIconLayer> built_layers_;
    mutable PulseIconDescriptor built_{};
public:
    explicit IconBuilder(std::string name = {}) : name_(std::move(name)) {}
    IconBuilder& ViewBox(double units) {
        view_box_milli_ = units > 0.0 ? static_cast<uint32_t>(units * 1000.0 + 0.5) : 16000u;
        return *this;
    }
    IconBuilder& StrokePath(std::string path, double width_dip = 1.35, bool closed = false,
                            uint32_t role = PULSE_ICON_COLOR_INHERIT, uint32_t argb = 0, double opacity = 1.0) {
        LayerSpec s;
        s.path = std::move(path);
        s.draw_flags = PULSE_ICON_DRAW_STROKE | (closed ? PULSE_ICON_DRAW_CLOSED : 0u);
        s.color_role = role;
        s.argb = argb;
        s.stroke_width_milli = width_dip > 0.0 ? static_cast<uint32_t>(width_dip * 1000.0 + 0.5) : 0u;
        s.opacity_milli = static_cast<uint32_t>(std::clamp(opacity, 0.01, 1.0) * 1000.0 + 0.5);
        layers_.push_back(std::move(s));
        return *this;
    }
    IconBuilder& FillPath(std::string path, uint32_t role = PULSE_ICON_COLOR_INHERIT, uint32_t argb = 0, double opacity = 1.0) {
        LayerSpec s;
        s.path = std::move(path);
        s.draw_flags = PULSE_ICON_DRAW_FILL | PULSE_ICON_DRAW_CLOSED;
        s.color_role = role;
        s.argb = argb;
        s.opacity_milli = static_cast<uint32_t>(std::clamp(opacity, 0.01, 1.0) * 1000.0 + 0.5);
        layers_.push_back(std::move(s));
        return *this;
    }
    const PulseIconDescriptor& Build() const {
        built_layers_.clear();
        built_layers_.reserve(layers_.size());
        for (const auto& layer : layers_) {
            PulseIconLayer raw{};
            raw.size = sizeof(raw);
            raw.draw_flags = layer.draw_flags;
            raw.path_data = View(layer.path);
            raw.color_role = layer.color_role;
            raw.argb = layer.argb;
            raw.stroke_width_milli = layer.stroke_width_milli;
            raw.opacity_milli = layer.opacity_milli;
            built_layers_.push_back(raw);
        }
        built_ = {};
        built_.size = sizeof(built_);
        built_.view_box_milli = view_box_milli_;
        built_.name = View(name_);
        built_.layers = built_layers_.empty() ? nullptr : built_layers_.data();
        built_.layer_count = static_cast<uint32_t>(built_layers_.size());
        return built_;
    }
};
class IconRegistration {
    PulseIconsV1 api_{};
    PulseIconId id_ = 0;
public:
    IconRegistration() = default;
    ~IconRegistration() { Reset(); }
    IconRegistration(const IconRegistration&) = delete;
    IconRegistration& operator=(const IconRegistration&) = delete;
    IconRegistration(IconRegistration&& other) noexcept { *this = std::move(other); }
    IconRegistration& operator=(IconRegistration&& other) noexcept {
        if (this != &other) { Reset(); api_ = other.api_; id_ = std::exchange(other.id_, 0); }
        return *this;
    }
    PulseIconId Id() const noexcept { return id_; }
    std::string Glyph() const { return IconGlyph(id_); }
    uint32_t DialogIcon() const noexcept { return DialogIconFromId(id_); }
    void Reset() noexcept {
        if (id_) { const auto id = std::exchange(id_, 0); try { api_.unregister_icon(api_.context, id); } catch (...) {} }
    }
    PulseStatus Register(const PulseIconsV1& api, const PulseIconDescriptor& descriptor) {
        Reset();
        if (!api.register_icon || !api.unregister_icon) return PULSE_ERROR_ARGUMENT;
        PulseIconId id = 0;
        const auto status = api.register_icon(api.context, &descriptor, &id);
        if (status == PULSE_OK) { api_ = api; id_ = id; }
        return status;
    }
    PulseStatus Update(const PulseIconDescriptor& descriptor) const {
        if (!id_ || !api_.update_icon) return PULSE_ERROR_STATE;
        return api_.update_icon(api_.context, id_, &descriptor);
    }
};
// UI-thread owned, except work runs on a host worker. Keep the group alive until
// Collect empties it or plugin destroy after the host has drained workers AND
// cancellation callbacks. Destruction neither cancels nor waits. The host never
// auto-evicts un-released terminal task records (returning PULSE_ERROR_UNAVAILABLE
// once retained capacity is full), so delayed Collect() reclaims every completed
// slot and its captures; unknown task IDs from custom/faulty hosts remain
// conservatively retained until that drained destruction boundary.
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
    // Buttons of ended tasks arrive here; it lives as long as the group (plugin lifetime).
    struct ActionSink {
        std::function<void(PulseTaskId, std::string_view)> handler;
        static void PULSE_CALL Act(void* user, PulseTaskId id, PulseStringView action) noexcept {
            try {
                auto* self = static_cast<ActionSink*>(user);
                if (self && self->handler) self->handler(id, std::string_view(action.data ? action.data : "", action.length));
            } catch (...) {}
        }
    };
    PulseTasksV1 api_{};
    std::vector<std::shared_ptr<Slot>> slots_;
    std::unique_ptr<ActionSink> actions_ = std::make_unique<ActionSink>();
public:
    // The route line and named steps a task shows (PulseTaskDescriptor after V1).
    struct TaskOptions {
        std::string source, destination;
        std::vector<std::string> steps;
    };
    explicit TaskGroup(const PulseTasksV1& api) : api_(api) {}
    // Receives PulseTaskAction ids picked on ended tasks, on the UI thread.
    void OnAction(std::function<void(PulseTaskId, std::string_view)> handler) { actions_->handler = std::move(handler); }
    TaskGroup(const TaskGroup&) = delete;
    TaskGroup& operator=(const TaskGroup&) = delete;
    TaskGroup(TaskGroup&&) = delete;
    TaskGroup& operator=(TaskGroup&&) = delete;
    size_t Pending() const noexcept { return slots_.size(); }
    PulseStatus Start(std::string_view title, std::string_view type,
                      std::function<PulseTaskState(const PulseTaskContext&)> work,
                      std::function<void(PulseTaskId)> on_cancel, PulseTaskId* out) {
        return Start(title, type, TaskOptions{}, std::move(work), std::move(on_cancel), out);
    }
    PulseStatus Start(std::string_view title, std::string_view type, const TaskOptions& options,
                      std::function<PulseTaskState(const PulseTaskContext&)> work,
                      std::function<void(PulseTaskId)> on_cancel, PulseTaskId* out) {
        if (out) *out = 0;
        if (!out || !work || !api_.create || !api_.get || !api_.release || !api_.cancel) return PULSE_ERROR_ARGUMENT;
        auto slot = std::make_shared<Slot>(); slot->work = std::move(work); slot->cancel = std::move(on_cancel);
        slots_.push_back(slot);
        std::vector<PulseStringView> steps;
        for (const auto& step : options.steps) steps.push_back(View(step));
        PulseTaskDescriptor descriptor{sizeof(descriptor), View(title), View(type), Slot::Work, slot.get(), Slot::Cancel,
            View(options.source), View(options.destination), steps.empty() ? nullptr : steps.data(),
            static_cast<uint32_t>(steps.size()), actions_->handler ? ActionSink::Act : nullptr, actions_.get()};
        PulseStatus status = PULSE_ERROR_STATE;
        try { status = api_.create(api_.context, &descriptor, &slot->id); }
        catch (...) { slot->starting = false; slot->work = {}; slot->cancel = {}; std::erase(slots_, slot); throw; }
        slot->starting = false;
        if (status != PULSE_OK) { slot->work = {}; slot->cancel = {}; std::erase(slots_, slot); }
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
                slot->work = {};
                slot->cancel = {};
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
    PulseStatus Create(const PulseUiModelV1& ui, std::string_view title, const PulseUiModelTree& tree) {
        Close();
        if (!ui.create || !ui.close) return PULSE_ERROR_ARGUMENT;
        uint64_t id = 0;
        const auto status = ui.create(ui.context, View(title), &tree, &id);
        if (status == PULSE_OK) { ui_ = ui; id_ = id; }
        return status;
    }
    PulseStatus Create(const PulseUiModelV1& ui, std::string_view title, const ModelTree& tree) {
        return Create(ui, title, tree.Build());
    }
    PulseStatus Update(const PulseUiModelTree& tree) const {
        if (!id_ || !ui_.update) return PULSE_ERROR_STATE;
        return ui_.update(ui_.context, id_, &tree);
    }
    PulseStatus Update(const ModelTree& tree) const {
        return Update(tree.Build());
    }
    PulseStatus Set(std::string_view key, std::string_view value) const {
        if (!id_) return PULSE_ERROR_STATE;
        const PulseUiState state{View(key), View(value)}; return ui_.set_states(ui_.context, id_, &state, 1);
    }
    PulseStatus SetStates(const std::vector<std::pair<std::string, std::string>>& entries) const {
        if (!id_) return PULSE_ERROR_STATE;
        std::vector<PulseUiState> raw;
        raw.reserve(entries.size());
        for (const auto& [k, v] : entries) raw.push_back({View(k), View(v)});
        return ui_.set_states(ui_.context, id_, raw.empty() ? nullptr : raw.data(), static_cast<uint32_t>(raw.size()));
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
struct Diagnostic {
    PulseStatus status = PULSE_OK;
    uint64_t sequence = 0;
    std::string interface_name;
    std::string operation;
    std::string subject;
    std::string field;
    std::string message;
};
class DiagnosticsClient {
    PulseDiagnosticsV1 api_{};
    static std::string CopyView(PulseStringView v) {
        return (v.data && v.length) ? std::string(v.data, v.length) : std::string{};
    }
    static void PULSE_CALL Capture(void* user, const PulseDiagnosticInfo* info) noexcept {
        try {
            if (!user || !info) return;
            auto& out = *static_cast<Diagnostic*>(user);
            out.status = info->status;
            out.sequence = info->sequence;
            out.interface_name = CopyView(info->interface_name);
            out.operation = CopyView(info->operation);
            out.subject = CopyView(info->subject);
            out.field = CopyView(info->field);
            out.message = CopyView(info->message);
        } catch (...) {}
    }
public:
    DiagnosticsClient() = default;
    explicit DiagnosticsClient(const PulseDiagnosticsV1& api) noexcept : api_(api) {}
    static std::optional<DiagnosticsClient> TryQuery(const Host& host) noexcept {
        PulseDiagnosticsV1 table{};
        if (host.Query(PULSE_DIAGNOSTICS_INTERFACE_NAME, PULSE_DIAGNOSTICS_VERSION_1, table) == PULSE_OK)
            return DiagnosticsClient(table);
        return std::nullopt;
    }
    bool Available() const noexcept { return api_.get_last_error != nullptr; }
    PulseStatus LastError(Diagnostic& out) const {
        out = {};
        if (!api_.get_last_error) return PULSE_ERROR_UNAVAILABLE;
        return api_.get_last_error(api_.context, Capture, &out);
    }
    PulseStatus Clear() const noexcept {
        if (!api_.clear_last_error) return PULSE_ERROR_UNAVAILABLE;
        return api_.clear_last_error(api_.context);
    }
};
struct PickerResultData {
    uint32_t outcome = PULSE_PICKER_FAILED;
    PulsePickerRequestId request_id = 0;
    uint32_t filter_index = 0;
    std::vector<std::string> paths;
    std::string selected_id;
    std::string error;
};
class PickerClient {
    struct Slot : std::enable_shared_from_this<Slot> {
        PickerClient* owner = nullptr;
        std::function<void(const PickerResultData&)> callback;
        static std::string CopyView(PulseStringView v) {
            return (v.data && v.length) ? std::string(v.data, v.length) : std::string{};
        }
        static void PULSE_CALL Complete(void* user, const PulsePickerResult* raw) noexcept {
            try {
                auto self = static_cast<Slot*>(user)->shared_from_this();
                if (self->owner && self->owner->slot_.get() == self.get()) {
                    self->owner->active_id_ = 0;
                    self->owner->slot_.reset();
                }
                if (!raw || !self->callback) return;
                PickerResultData data;
                data.outcome = raw->outcome;
                data.request_id = raw->request_id;
                data.filter_index = raw->filter_index;
                if (raw->paths && raw->path_count) {
                    data.paths.reserve(raw->path_count);
                    for (uint32_t i = 0; i < raw->path_count; ++i)
                        data.paths.push_back(CopyView(raw->paths[i]));
                }
                data.selected_id = CopyView(raw->selected_id);
                data.error = CopyView(raw->error);
                self->callback(data);
            } catch (...) {}
        }
    };
    PulsePickersV1 api_{};
    PulsePickerRequestId active_id_ = 0;
    std::shared_ptr<Slot> slot_;
public:
    PickerClient() = default;
    explicit PickerClient(const PulsePickersV1& api) noexcept : api_(api) {}
    ~PickerClient() { CancelActive(); if (slot_) slot_->owner = nullptr; }
    PickerClient(const PickerClient&) = delete;
    PickerClient& operator=(const PickerClient&) = delete;
    static std::optional<PickerClient> TryQuery(const Host& host) noexcept {
        PulsePickersV1 table{};
        if (host.Query(PULSE_PICKERS_INTERFACE_NAME, PULSE_PICKERS_VERSION_1, table) == PULSE_OK)
            return std::optional<PickerClient>(std::in_place, table);
        return std::nullopt;
    }
    PulsePickerRequestId ActiveRequestId() const noexcept { return active_id_; }
    PulseStatus CancelActive() noexcept {
        if (!active_id_ || !api_.cancel) return PULSE_ERROR_STATE;
        const auto id = std::exchange(active_id_, 0);
        return api_.cancel(api_.context, id);
    }
    PulseStatus OpenFiles(const PulseOpenFilesRequest& request, std::function<void(const PickerResultData&)> fn,
                          PulsePickerRequestId* out_id = nullptr) {
        if (!api_.open_files || !fn) return PULSE_ERROR_ARGUMENT;
        auto slot = std::make_shared<Slot>();
        slot->owner = this;
        slot->callback = std::move(fn);
        PulsePickerRequestId id = 0;
        const auto status = api_.open_files(api_.context, &request, Slot::Complete, slot.get(), &id);
        if (status == PULSE_OK) { active_id_ = id; slot_ = std::move(slot); if (out_id) *out_id = id; }
        return status;
    }
    PulseStatus SaveFile(const PulseSaveFileRequest& request, std::function<void(const PickerResultData&)> fn,
                         PulsePickerRequestId* out_id = nullptr) {
        if (!api_.save_file || !fn) return PULSE_ERROR_ARGUMENT;
        auto slot = std::make_shared<Slot>();
        slot->owner = this;
        slot->callback = std::move(fn);
        PulsePickerRequestId id = 0;
        const auto status = api_.save_file(api_.context, &request, Slot::Complete, slot.get(), &id);
        if (status == PULSE_OK) { active_id_ = id; slot_ = std::move(slot); if (out_id) *out_id = id; }
        return status;
    }
    PulseStatus PickFolders(const PulsePickFoldersRequest& request, std::function<void(const PickerResultData&)> fn,
                            PulsePickerRequestId* out_id = nullptr) {
        if (!api_.pick_folders || !fn) return PULSE_ERROR_ARGUMENT;
        auto slot = std::make_shared<Slot>();
        slot->owner = this;
        slot->callback = std::move(fn);
        PulsePickerRequestId id = 0;
        const auto status = api_.pick_folders(api_.context, &request, Slot::Complete, slot.get(), &id);
        if (status == PULSE_OK) { active_id_ = id; slot_ = std::move(slot); if (out_id) *out_id = id; }
        return status;
    }
    PulseStatus QuickPick(const PulseQuickPickRequest& request, std::function<void(const PickerResultData&)> fn,
                          PulsePickerRequestId* out_id = nullptr) {
        if (!api_.quick_pick || !fn) return PULSE_ERROR_ARGUMENT;
        auto slot = std::make_shared<Slot>();
        slot->owner = this;
        slot->callback = std::move(fn);
        PulsePickerRequestId id = 0;
        const auto status = api_.quick_pick(api_.context, &request, Slot::Complete, slot.get(), &id);
        if (status == PULSE_OK) { active_id_ = id; slot_ = std::move(slot); if (out_id) *out_id = id; }
        return status;
    }
};
class DialogSession {
public:
    using StateList = std::vector<std::pair<std::string, std::string>>;
private:
    struct LifecycleSlot : std::enable_shared_from_this<LifecycleSlot> {
        DialogSession* owner = nullptr;
        std::function<void(uint64_t, const StateList&)> on_changed;
        std::function<void(uint64_t, PulseStatus, std::string_view, const StateList&)> on_closed;
        static StateList CopyStates(const PulseUiState* states, uint32_t count) {
            StateList out;
            if (!states || !count) return out;
            out.reserve(count);
            for (uint32_t i = 0; i < count; ++i) {
                out.emplace_back(
                    (states[i].key.data && states[i].key.length) ? std::string(states[i].key.data, states[i].key.length) : std::string{},
                    (states[i].value.data && states[i].value.length) ? std::string(states[i].value.data, states[i].value.length) : std::string{});
            }
            return out;
        }
        static void PULSE_CALL StateChanged(void* user, uint64_t dialog, const PulseUiState* changes, uint32_t count) noexcept {
            try {
                auto self = static_cast<LifecycleSlot*>(user)->shared_from_this();
                if (self->on_changed) self->on_changed(dialog, CopyStates(changes, count));
            } catch (...) {}
        }
        static void PULSE_CALL Closed(void* user, uint64_t dialog, PulseStatus status, PulseStringView button,
                                      const PulseUiState* states, uint32_t count) noexcept {
            try {
                auto self = static_cast<LifecycleSlot*>(user)->shared_from_this();
                if (self->owner && self->owner->lifecycle_.get() == self.get()) {
                    self->owner->id_ = 0;
                    self->owner->commands_.clear();
                    self->owner->lifecycle_.reset();
                }
                if (self->on_closed) {
                    const std::string_view btn(button.data ? button.data : "", button.length);
                    self->on_closed(dialog, status, btn, CopyStates(states, count));
                }
            } catch (...) {}
        }
    };
    struct CommandSlot : std::enable_shared_from_this<CommandSlot> {
        uint64_t token = 0;
        std::function<void(uint64_t, std::string_view)> fn;
        static void PULSE_CALL Invoke(void* user, uint64_t dialog, PulseStringView cmd) noexcept {
            try {
                auto self = static_cast<CommandSlot*>(user)->shared_from_this();
                if (self->fn) self->fn(dialog, std::string_view(cmd.data ? cmd.data : "", cmd.length));
            } catch (...) {}
        }
    };
    PulseDialogsV1 v1_{};
    PulseDialogsV2 v2_{};
    bool has_v2_ = false;
    uint64_t id_ = 0;
    std::shared_ptr<LifecycleSlot> lifecycle_;
    std::vector<std::shared_ptr<CommandSlot>> commands_;
public:
    DialogSession() = default;
    explicit DialogSession(const PulseDialogsV1& api) noexcept : v1_(api), has_v2_(false) {}
    explicit DialogSession(const PulseDialogsV2& api) noexcept
        : v1_{sizeof(PulseDialogsV1), PULSE_DIALOGS_VERSION_1, api.context, api.show, api.set_states, api.close},
          v2_(api),
          has_v2_(true) {}
    ~DialogSession() {
        Close();
        if (lifecycle_) lifecycle_->owner = nullptr;
    }
    DialogSession(const DialogSession&) = delete;
    DialogSession& operator=(const DialogSession&) = delete;
    PulseStatus Init(const Host& host) noexcept {
        Close();
        PulseDialogsV2 v2{};
        if (host.Query(PULSE_DIALOGS_INTERFACE_NAME, PULSE_DIALOGS_VERSION_2, v2) == PULSE_OK) {
            v1_ = {sizeof(PulseDialogsV1), PULSE_DIALOGS_VERSION_1, v2.context, v2.show, v2.set_states, v2.close};
            v2_ = v2;
            has_v2_ = true;
            return PULSE_OK;
        }
        PulseDialogsV1 v1{};
        const auto status = host.Query(PULSE_DIALOGS_INTERFACE_NAME, PULSE_DIALOGS_VERSION_1, v1);
        if (status == PULSE_OK) {
            v1_ = v1;
            v2_ = {};
            has_v2_ = false;
        }
        return status;
    }
    bool SupportsV2() const noexcept { return has_v2_; }
    uint64_t Id() const noexcept { return id_; }
    void Close() noexcept {
        if (has_v2_ && v2_.unsubscribe_command) {
            for (const auto& c : commands_) {
                if (c->token) v2_.unsubscribe_command(v2_.context, c->token);
            }
        }
        commands_.clear();
        if (id_ && v1_.close) {
            const uint64_t cur = std::exchange(id_, 0);
            v1_.close(v1_.context, cur);
        }
    }
    PulseStatus Show(const PulseDialogRequest& request,
                     std::function<void(uint64_t, const StateList&)> on_changed,
                     std::function<void(uint64_t, PulseStatus, std::string_view, const StateList&)> on_closed) {
        if (!v1_.show || !on_closed) return PULSE_ERROR_ARGUMENT;
        if (id_) return PULSE_ERROR_STATE;
        auto slot = std::make_shared<LifecycleSlot>();
        slot->owner = this;
        slot->on_changed = std::move(on_changed);
        slot->on_closed = std::move(on_closed);
        uint64_t id = 0;
        const auto show_fn = has_v2_ ? v2_.show : v1_.show;
        const auto ctx = has_v2_ ? v2_.context : v1_.context;
        const auto status = show_fn(ctx, &request,
                                    slot->on_changed ? LifecycleSlot::StateChanged : nullptr,
                                    LifecycleSlot::Closed, slot.get(), &id);
        if (status == PULSE_OK) {
            id_ = id;
            lifecycle_ = std::move(slot);
        }
        return status;
    }
    PulseStatus SetState(std::string_view key, std::string_view value) const {
        if (!id_ || !v1_.set_states) return PULSE_ERROR_STATE;
        const PulseUiState state{View(key), View(value)};
        return v1_.set_states(v1_.context, id_, &state, 1);
    }
    PulseStatus SetStates(const StateList& entries) const {
        if (!id_ || !v1_.set_states) return PULSE_ERROR_STATE;
        std::vector<PulseUiState> views;
        views.reserve(entries.size());
        for (const auto& [k, v] : entries) views.push_back({View(k), View(v)});
        return v1_.set_states(v1_.context, id_, views.empty() ? nullptr : views.data(), static_cast<uint32_t>(views.size()));
    }
    PulseStatus GetState(std::string_view key, std::string& out) const {
        if (!has_v2_ || !v2_.get_state) return PULSE_ERROR_UNAVAILABLE;
        if (!id_) return PULSE_ERROR_STATE;
        uint32_t needed = 0;
        auto status = v2_.get_state(v2_.context, id_, View(key), nullptr, 0, &needed);
        if (status != PULSE_OK) return status;
        std::string buf(needed, '\0');
        status = v2_.get_state(v2_.context, id_, View(key), buf.data(), needed, &needed);
        if (status == PULSE_OK) out = std::move(buf);
        return status;
    }
    PulseStatus UpdateModel(const PulseUiModelTree& tree) const {
        if (!has_v2_ || !v2_.update_model) return PULSE_ERROR_UNAVAILABLE;
        if (!id_) return PULSE_ERROR_STATE;
        return v2_.update_model(v2_.context, id_, &tree);
    }
    PulseStatus UpdateModel(const ModelTree& tree) const {
        return UpdateModel(tree.Build());
    }
    PulseStatus OnCommand(std::string_view command, std::function<void(uint64_t, std::string_view)> fn) {
        if (!has_v2_ || !v2_.subscribe_command) return PULSE_ERROR_UNAVAILABLE;
        if (!id_) return PULSE_ERROR_STATE;
        if (!fn) return PULSE_ERROR_ARGUMENT;
        auto slot = std::make_shared<CommandSlot>();
        slot->fn = std::move(fn);
        uint64_t token = 0;
        const auto status = v2_.subscribe_command(v2_.context, id_, View(command), CommandSlot::Invoke, slot.get(), &token);
        if (status == PULSE_OK) {
            slot->token = token;
            commands_.push_back(std::move(slot));
        }
        return status;
    }
    PulseStatus BindFolderPicker(PickerClient& pickers, std::string_view command, std::string state_key,
                                 std::string title = {}) {
        return OnCommand(command, [this, &pickers, key = std::move(state_key), ttl = std::move(title)](uint64_t dlg, std::string_view) {
            std::string initial;
            (void)GetState(key, initial);
            PulsePickFoldersRequest req{};
            req.size = sizeof(req);
            req.title = View(ttl);
            req.initial_path = View(initial);
            (void)pickers.PickFolders(req, [this, dlg, key](const PickerResultData& res) {
                if (res.outcome == PULSE_PICKER_ACCEPTED && !res.paths.empty() && Id() == dlg) {
                    (void)SetState(key, res.paths.front());
                }
            });
        });
    }
};
class WorkbenchClient {
    struct ActionSlot : std::enable_shared_from_this<ActionSlot> {
        std::function<void(PulseNotificationId, std::string_view)> fn;
        static void PULSE_CALL Invoke(void* user, PulseNotificationId id, PulseStringView action_id) noexcept {
            try {
                auto self = static_cast<ActionSlot*>(user)->shared_from_this();
                if (self->fn) self->fn(id, std::string_view(action_id.data ? action_id.data : "", action_id.length));
            } catch (...) {}
        }
    };
    PulseWorkbenchV1 api_{};
    std::deque<std::shared_ptr<ActionSlot>> slots_;
    ActionSlot* RetainSlot(std::function<void(PulseNotificationId, std::string_view)> fn) {
        if (!fn) return nullptr;
        while (slots_.size() >= 32) slots_.pop_front();
        auto slot = std::make_shared<ActionSlot>();
        slot->fn = std::move(fn);
        slots_.push_back(slot);
        return slot.get();
    }
public:
    WorkbenchClient() = default;
    explicit WorkbenchClient(const PulseWorkbenchV1& api) noexcept : api_(api) {}
    WorkbenchClient(const WorkbenchClient&) = delete;
    WorkbenchClient& operator=(const WorkbenchClient&) = delete;
    PulseStatus ShowSnack(const PulseSnackRequest& request,
                          std::function<void(PulseNotificationId, std::string_view)> on_action = {},
                          PulseNotificationId* out_id = nullptr) {
        if (!api_.show_snack) return PULSE_ERROR_UNAVAILABLE;
        ActionSlot* slot = RetainSlot(std::move(on_action));
        return api_.show_snack(api_.context, &request, slot ? ActionSlot::Invoke : nullptr, slot, out_id);
    }
    PulseStatus ShowNotice(const PulseNoticeRequest& request,
                           std::function<void(PulseNotificationId, std::string_view)> on_action = {},
                           PulseNotificationId* out_id = nullptr) {
        if (!api_.show_notice) return PULSE_ERROR_UNAVAILABLE;
        ActionSlot* slot = RetainSlot(std::move(on_action));
        return api_.show_notice(api_.context, &request, slot ? ActionSlot::Invoke : nullptr, slot, out_id);
    }
    PulseStatus RevealPath(std::string_view path, uint32_t mode = PULSE_NAVIGATE_REVEAL_ITEM) const {
        if (!api_.reveal_path) return PULSE_ERROR_UNAVAILABLE;
        return api_.reveal_path(api_.context, View(path), mode);
    }
    PulseStatus ShowTextDiff(std::string_view left_path, std::string_view right_path) const {
        if (!api_.show_text_diff) return PULSE_ERROR_UNAVAILABLE;
        return api_.show_text_diff(api_.context, View(left_path), View(right_path));
    }
    PulseStatus SetClipboardText(std::string_view text) const {
        if (!api_.set_clipboard_text) return PULSE_ERROR_UNAVAILABLE;
        return api_.set_clipboard_text(api_.context, View(text));
    }
};
}
