#pragma once
#include "sdk.hpp"
#include "associations.h"
#include "ui.h"
#include "ui_model.h"
#include "model_builder.hpp"
#include "ui_events.h"
#include "ui_data.h"
#include "dispatch.h"
#include "commands.h"
#include "diagnostics.h"
#include "dialogs.h"
#include "icons.h"
#include "pickers.h"
#include "search.h"
#include "settings_pages.h"
#include "shell.h"
#include "tasks.h"
#include "tray.h"
#include "vfs.h"
#include "columns.h"
#include "statusbar.h"
#include "fileevents.h"
#include "preview.h"
#include "workbench.h"
#include <algorithm>
#include <thread>
#include <cstddef>
#include <chrono>
#include <cstdio>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_set>
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
    std::string CanvasToken() const { return "#" + std::to_string(id_); }
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
    PulseStatus Register(const PulseIconsV2& api, const PulseIconDescriptor& descriptor) {
        const PulseIconsV1 v1{sizeof(PulseIconsV1), PULSE_ICONS_VERSION_1, api.context,
                              api.register_icon, api.update_icon, api.unregister_icon, api.resolve_builtin};
        return Register(v1, descriptor);
    }
    PulseStatus Update(const PulseIconDescriptor& descriptor) const {
        if (!id_ || !api_.update_icon) return PULSE_ERROR_STATE;
        return api_.update_icon(api_.context, id_, &descriptor);
    }
};
class IconsClient {
    PulseIconsV1 v1_{};
    PulseIconsV2 v2_{};
    bool has_v2_ = false;
public:
    IconsClient() = default;
    explicit IconsClient(const PulseIconsV1& api) noexcept : v1_(api), has_v2_(false) {}
    explicit IconsClient(const PulseIconsV2& api) noexcept
        : v1_{sizeof(PulseIconsV1), PULSE_ICONS_VERSION_1, api.context,
              api.register_icon, api.update_icon, api.unregister_icon, api.resolve_builtin},
          v2_(api),
          has_v2_(true) {}
    static std::optional<IconsClient> TryQuery(const Host& host) noexcept {
        IconsClient out;
        if (out.Init(host) == PULSE_OK) return out;
        return std::nullopt;
    }
    PulseStatus Init(const Host& host) noexcept {
        PulseIconsV2 v2{};
        if (host.Query(PULSE_ICONS_INTERFACE_NAME, PULSE_ICONS_VERSION_2, v2) == PULSE_OK) {
            *this = IconsClient(v2);
            return PULSE_OK;
        }
        PulseIconsV1 v1{};
        const auto st = host.Query(PULSE_ICONS_INTERFACE_NAME, PULSE_ICONS_VERSION_1, v1);
        if (st == PULSE_OK) *this = IconsClient(v1);
        return st;
    }
    bool Available() const noexcept { return v1_.register_icon != nullptr; }
    bool SupportsV2() const noexcept { return has_v2_; }
    const PulseIconsV1& TableV1() const noexcept { return v1_; }
    PulseStatus ResolveBuiltin(std::string_view name, PulseIconId& out_id) const noexcept {
        out_id = 0;
        if (!v1_.resolve_builtin) return PULSE_ERROR_UNAVAILABLE;
        return v1_.resolve_builtin(v1_.context, View(name), &out_id);
    }
    PulseStatus Resolve(std::string_view token, PulseIconId& out_id) const noexcept {
        out_id = 0;
        if (has_v2_ && v2_.resolve_icon) return v2_.resolve_icon(v2_.context, View(token), &out_id);
        if (token.size() >= 5 && token.substr(0, 5) == "icon:") token.remove_prefix(5);
        return ResolveBuiltin(token, out_id);
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
        DiagnosticsClient out;
        if (out.Init(host) == PULSE_OK) return out;
        return std::nullopt;
    }
    PulseStatus Init(const Host& host) noexcept {
        PulseDiagnosticsV1 table{};
        const auto status = host.Query(PULSE_DIAGNOSTICS_INTERFACE_NAME, PULSE_DIAGNOSTICS_VERSION_1, table);
        if (status == PULSE_OK) api_ = table;
        return status;
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
    PickerClient(PickerClient&& other) noexcept
        : api_(std::exchange(other.api_, {})),
          active_id_(std::exchange(other.active_id_, 0)),
          slot_(std::move(other.slot_)) {
        if (slot_) slot_->owner = this;
    }
    PickerClient& operator=(PickerClient&& other) noexcept {
        if (this != &other) {
            CancelActive();
            if (slot_) slot_->owner = nullptr;
            api_ = std::exchange(other.api_, {});
            active_id_ = std::exchange(other.active_id_, 0);
            slot_ = std::move(other.slot_);
            if (slot_) slot_->owner = this;
        }
        return *this;
    }
    static std::optional<PickerClient> TryQuery(const Host& host) noexcept {
        PickerClient out;
        if (out.Init(host) == PULSE_OK) return out;
        return std::nullopt;
    }
    PulseStatus Init(const Host& host) noexcept {
        CancelActive();
        if (slot_) { slot_->owner = nullptr; slot_.reset(); }
        PulsePickersV1 table{};
        const auto status = host.Query(PULSE_PICKERS_INTERFACE_NAME, PULSE_PICKERS_VERSION_1, table);
        if (status == PULSE_OK) api_ = table;
        return status;
    }
    bool Available() const noexcept { return api_.open_files != nullptr; }
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
    DialogSession(DialogSession&& other) noexcept
        : v1_(std::exchange(other.v1_, {})),
          v2_(std::exchange(other.v2_, {})),
          has_v2_(std::exchange(other.has_v2_, false)),
          id_(std::exchange(other.id_, 0)),
          lifecycle_(std::move(other.lifecycle_)),
          commands_(std::move(other.commands_)) {
        if (lifecycle_) lifecycle_->owner = this;
    }
    DialogSession& operator=(DialogSession&& other) noexcept {
        if (this != &other) {
            Close();
            if (lifecycle_) lifecycle_->owner = nullptr;
            v1_ = std::exchange(other.v1_, {});
            v2_ = std::exchange(other.v2_, {});
            has_v2_ = std::exchange(other.has_v2_, false);
            id_ = std::exchange(other.id_, 0);
            lifecycle_ = std::move(other.lifecycle_);
            commands_ = std::move(other.commands_);
            if (lifecycle_) lifecycle_->owner = this;
        }
        return *this;
    }
    static std::optional<DialogSession> TryQuery(const Host& host) noexcept {
        DialogSession out;
        if (out.Init(host) == PULSE_OK) return out;
        return std::nullopt;
    }
    bool Available() const noexcept { return v1_.show != nullptr; }
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
    struct Router {
        struct Entry {
            PulseNotificationId id = 0;
            bool in_flight = false;
            std::function<void(PulseNotificationId, std::string_view)> fn;
        };
        std::deque<Entry> entries;
        static std::mutex& RegistryMutex() {
            static std::mutex m;
            return m;
        }
        static std::unordered_set<const Router*>& LiveRouters() {
            static std::unordered_set<const Router*> s;
            return s;
        }
        Router() {
            std::lock_guard lock(RegistryMutex());
            LiveRouters().insert(this);
        }
        ~Router() {
            std::lock_guard lock(RegistryMutex());
            LiveRouters().erase(this);
        }
        Router(const Router&) = delete;
        Router& operator=(const Router&) = delete;
        static void PULSE_CALL Invoke(void* user, PulseNotificationId id, PulseStringView action_id) noexcept {
            try {
                if (!user) return;
                std::function<void(PulseNotificationId, std::string_view)> target;
                {
                    std::lock_guard lock(RegistryMutex());
                    auto* self = static_cast<Router*>(user);
                    if (LiveRouters().find(self) == LiveRouters().end()) return;
                    for (auto it = self->entries.rbegin(); it != self->entries.rend(); ++it) {
                        if ((id != 0 && it->id == id) || it->in_flight) {
                            target = it->fn;
                            break;
                        }
                    }
                }
                if (target) target(id, std::string_view(action_id.data ? action_id.data : "", action_id.length));
            } catch (...) {}
        }
    };
    PulseWorkbenchV1 api_{};
    PulseWorkbenchV2 v2_{};
    PulseWorkbenchV3 v3_{};
    bool has_v2_ = false;
    bool has_v3_ = false;
    std::unique_ptr<Router> router_ = std::make_unique<Router>();
    template<class CallFn>
    PulseStatus DispatchWithAction(std::function<void(PulseNotificationId, std::string_view)> on_action,
                                   PulseNotificationId* out_id, CallFn&& call_fn) {
        if (!on_action) {
            return call_fn(nullptr, nullptr, out_id);
        }
        if (!router_) router_ = std::make_unique<Router>();
        {
            std::lock_guard lock(Router::RegistryMutex());
            while (router_->entries.size() >= 64) router_->entries.pop_front();
            router_->entries.push_back(Router::Entry{0, true, std::move(on_action)});
        }
        PulseNotificationId assigned_id = 0;
        PulseStatus status = PULSE_ERROR_STATE;
        try {
            status = call_fn(Router::Invoke, router_.get(), &assigned_id);
        } catch (...) {
            std::lock_guard lock(Router::RegistryMutex());
            if (!router_->entries.empty() && router_->entries.back().in_flight) router_->entries.pop_back();
            throw;
        }
        {
            std::lock_guard lock(Router::RegistryMutex());
            if (!router_->entries.empty() && router_->entries.back().in_flight) {
                if (status == PULSE_OK) {
                    router_->entries.back().id = assigned_id;
                    router_->entries.back().in_flight = false;
                } else {
                    router_->entries.pop_back();
                }
            }
        }
        if (status == PULSE_OK && out_id) *out_id = assigned_id;
        return status;
    }
public:
    class AppearanceSubscription {
        struct Slot : std::enable_shared_from_this<Slot> {
            bool active = true;
            std::function<void(const PulseHostAppearance&)> fn;
            static void PULSE_CALL OnAppearance(void* user, const PulseHostAppearance* appearance) noexcept {
                try {
                    if (!user || !appearance) return;
                    auto self = static_cast<Slot*>(user)->shared_from_this();
                    if (self->active && self->fn) self->fn(*appearance);
                } catch (...) {}
            }
        };
        PulseWorkbenchV2 api_{};
        PulseAppearanceSubscription token_ = 0;
        std::shared_ptr<Slot> slot_;
        friend class WorkbenchClient;
        AppearanceSubscription(const PulseWorkbenchV2& api, PulseAppearanceSubscription token,
                               std::shared_ptr<Slot> slot) noexcept
            : api_(api), token_(token), slot_(std::move(slot)) {}
    public:
        AppearanceSubscription() = default;
        ~AppearanceSubscription() { Reset(); }
        AppearanceSubscription(const AppearanceSubscription&) = delete;
        AppearanceSubscription& operator=(const AppearanceSubscription&) = delete;
        AppearanceSubscription(AppearanceSubscription&& other) noexcept
            : api_(std::exchange(other.api_, {})),
              token_(std::exchange(other.token_, 0)),
              slot_(std::move(other.slot_)) {}
        AppearanceSubscription& operator=(AppearanceSubscription&& other) noexcept {
            if (this != &other) {
                Reset();
                api_ = std::exchange(other.api_, {});
                token_ = std::exchange(other.token_, 0);
                slot_ = std::move(other.slot_);
            }
            return *this;
        }
        bool Active() const noexcept { return token_ != 0; }
        PulseAppearanceSubscription Id() const noexcept { return token_; }
        void Reset() noexcept {
            if (slot_) slot_->active = false;
            if (token_ != 0 && api_.unsubscribe_appearance) {
                api_.unsubscribe_appearance(api_.context, token_);
            }
            token_ = 0;
            api_ = {};
            slot_.reset();
        }
    };

    class LocaleSubscription {
        struct Slot : std::enable_shared_from_this<Slot> {
            bool active = true;
            std::function<void(const PulseHostLocale&)> fn;
            static void PULSE_CALL OnLocale(void* user, const PulseHostLocale* locale) noexcept {
                try {
                    if (!user || !locale) return;
                    auto self = static_cast<Slot*>(user)->shared_from_this();
                    if (self->active && self->fn) self->fn(*locale);
                } catch (...) {}
            }
        };
        PulseWorkbenchV3 api_{};
        PulseLocaleSubscription token_ = 0;
        std::shared_ptr<Slot> slot_;
        friend class WorkbenchClient;
        LocaleSubscription(const PulseWorkbenchV3& api, PulseLocaleSubscription token,
                           std::shared_ptr<Slot> slot) noexcept
            : api_(api), token_(token), slot_(std::move(slot)) {}
    public:
        LocaleSubscription() = default;
        ~LocaleSubscription() { Reset(); }
        LocaleSubscription(const LocaleSubscription&) = delete;
        LocaleSubscription& operator=(const LocaleSubscription&) = delete;
        LocaleSubscription(LocaleSubscription&& other) noexcept
            : api_(std::exchange(other.api_, {})),
              token_(std::exchange(other.token_, 0)),
              slot_(std::move(other.slot_)) {}
        LocaleSubscription& operator=(LocaleSubscription&& other) noexcept {
            if (this != &other) {
                Reset();
                api_ = std::exchange(other.api_, {});
                token_ = std::exchange(other.token_, 0);
                slot_ = std::move(other.slot_);
            }
            return *this;
        }
        bool Active() const noexcept { return token_ != 0; }
        PulseLocaleSubscription Id() const noexcept { return token_; }
        void Reset() noexcept {
            if (slot_) slot_->active = false;
            if (token_ != 0 && api_.unsubscribe_locale) {
                api_.unsubscribe_locale(api_.context, token_);
            }
            token_ = 0;
            api_ = {};
            slot_.reset();
        }
    };

    WorkbenchClient() = default;
    explicit WorkbenchClient(const PulseWorkbenchV1& api) : api_(api), has_v2_(false), has_v3_(false) {}
    explicit WorkbenchClient(const PulseWorkbenchV2& api)
        : api_{sizeof(PulseWorkbenchV1), PULSE_WORKBENCH_VERSION_1, api.context,
               api.show_snack, api.show_notice, api.reveal_path,
               api.show_text_diff, api.set_clipboard_text},
          v2_(api),
          has_v2_(true),
          has_v3_(false) {}
    explicit WorkbenchClient(const PulseWorkbenchV3& api)
        : api_{sizeof(PulseWorkbenchV1), PULSE_WORKBENCH_VERSION_1, api.context,
               api.show_snack, api.show_notice, api.reveal_path,
               api.show_text_diff, api.set_clipboard_text},
          v2_{sizeof(PulseWorkbenchV2), PULSE_WORKBENCH_VERSION_2, api.context,
              api.show_snack, api.show_notice, api.reveal_path,
              api.show_text_diff, api.set_clipboard_text,
              api.open_search, api.reveal_path_from_rect, api.query_appearance,
              api.subscribe_appearance, api.unsubscribe_appearance},
          v3_(api),
          has_v2_(true),
          has_v3_(true) {}
    WorkbenchClient(const WorkbenchClient&) = delete;
    WorkbenchClient& operator=(const WorkbenchClient&) = delete;
    WorkbenchClient(WorkbenchClient&&) noexcept = default;
    WorkbenchClient& operator=(WorkbenchClient&&) noexcept = default;
    static std::optional<WorkbenchClient> TryQuery(const Host& host) {
        WorkbenchClient out;
        if (out.Init(host) == PULSE_OK) return out;
        return std::nullopt;
    }
    PulseStatus Init(const Host& host) noexcept {
        PulseWorkbenchV3 table3{};
        if (host.Query(PULSE_WORKBENCH_INTERFACE_NAME, PULSE_WORKBENCH_VERSION_3, table3) == PULSE_OK) {
            api_ = {sizeof(PulseWorkbenchV1), PULSE_WORKBENCH_VERSION_1, table3.context,
                    table3.show_snack, table3.show_notice, table3.reveal_path,
                    table3.show_text_diff, table3.set_clipboard_text};
            v2_ = {sizeof(PulseWorkbenchV2), PULSE_WORKBENCH_VERSION_2, table3.context,
                   table3.show_snack, table3.show_notice, table3.reveal_path,
                   table3.show_text_diff, table3.set_clipboard_text,
                   table3.open_search, table3.reveal_path_from_rect, table3.query_appearance,
                   table3.subscribe_appearance, table3.unsubscribe_appearance};
            v3_ = table3;
            has_v2_ = true;
            has_v3_ = true;
            return PULSE_OK;
        }
        PulseWorkbenchV2 table2{};
        if (host.Query(PULSE_WORKBENCH_INTERFACE_NAME, PULSE_WORKBENCH_VERSION_2, table2) == PULSE_OK) {
            api_ = {sizeof(PulseWorkbenchV1), PULSE_WORKBENCH_VERSION_1, table2.context,
                    table2.show_snack, table2.show_notice, table2.reveal_path,
                    table2.show_text_diff, table2.set_clipboard_text};
            v2_ = table2;
            v3_ = {};
            has_v2_ = true;
            has_v3_ = false;
            return PULSE_OK;
        }
        PulseWorkbenchV1 table1{};
        const auto status = host.Query(PULSE_WORKBENCH_INTERFACE_NAME, PULSE_WORKBENCH_VERSION_1, table1);
        if (status == PULSE_OK) {
            api_ = table1;
            v2_ = {};
            v3_ = {};
            has_v2_ = false;
            has_v3_ = false;
        }
        return status;
    }
    bool Available() const noexcept { return api_.show_snack != nullptr; }
    bool SupportsV2() const noexcept { return has_v2_; }
    bool SupportsV3() const noexcept { return has_v3_; }
    PulseStatus ShowSnack(const PulseSnackRequest& request,
                          std::function<void(PulseNotificationId, std::string_view)> on_action = {},
                          PulseNotificationId* out_id = nullptr) {
        if (!api_.show_snack) return PULSE_ERROR_UNAVAILABLE;
        return DispatchWithAction(std::move(on_action), out_id,
            [this, &request](PulseWorkbenchActionFn cb, void* user, PulseNotificationId* id_ptr) {
                return api_.show_snack(api_.context, &request, cb, user, id_ptr);
            });
    }
    PulseStatus ShowNotice(const PulseNoticeRequest& request,
                           std::function<void(PulseNotificationId, std::string_view)> on_action = {},
                           PulseNotificationId* out_id = nullptr) {
        if (!api_.show_notice) return PULSE_ERROR_UNAVAILABLE;
        return DispatchWithAction(std::move(on_action), out_id,
            [this, &request](PulseWorkbenchActionFn cb, void* user, PulseNotificationId* id_ptr) {
                return api_.show_notice(api_.context, &request, cb, user, id_ptr);
            });
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
    PulseStatus OpenSearch(std::string_view query) const {
        if (!has_v2_ || !v2_.open_search) return PULSE_ERROR_UNAVAILABLE;
        return v2_.open_search(v2_.context, View(query));
    }
    PulseStatus RevealPathFromRect(std::string_view path, uint32_t mode = PULSE_NAVIGATE_REVEAL_ITEM,
                                   const PulseScreenRect* origin_rect = nullptr) const {
        if (!has_v2_ || !v2_.reveal_path_from_rect) return PULSE_ERROR_UNAVAILABLE;
        return v2_.reveal_path_from_rect(v2_.context, View(path), mode, origin_rect);
    }
    PulseStatus QueryAppearance(PulseHostAppearance& out_appearance) const {
        if (!has_v2_ || !v2_.query_appearance) return PULSE_ERROR_UNAVAILABLE;
        out_appearance.size = sizeof(PulseHostAppearance);
        return v2_.query_appearance(v2_.context, &out_appearance);
    }
    PulseStatus SubscribeAppearance(AppearanceSubscription& out_sub,
                                    std::function<void(const PulseHostAppearance&)> fn) const {
        out_sub.Reset();
        if (!fn) return PULSE_ERROR_ARGUMENT;
        if (!has_v2_ || !v2_.subscribe_appearance) return PULSE_ERROR_UNAVAILABLE;
        auto slot = std::make_shared<AppearanceSubscription::Slot>();
        slot->fn = std::move(fn);
        PulseAppearanceSubscription tok = 0;
        const auto st = v2_.subscribe_appearance(
            v2_.context, AppearanceSubscription::Slot::OnAppearance, slot.get(), &tok);
        if (st == PULSE_OK && tok != 0) {
            out_sub = AppearanceSubscription(v2_, tok, std::move(slot));
        }
        return st;
    }
    std::optional<AppearanceSubscription> SubscribeAppearance(
        std::function<void(const PulseHostAppearance&)> fn,
        PulseStatus* out_status = nullptr) const {
        AppearanceSubscription sub;
        const PulseStatus st = SubscribeAppearance(sub, std::move(fn));
        if (out_status) *out_status = st;
        if (st == PULSE_OK) return sub;
        return std::nullopt;
    }
    PulseStatus QueryLocale(PulseHostLocale& out_locale) const {
        if (!has_v3_ || !v3_.query_locale) return PULSE_ERROR_UNAVAILABLE;
        out_locale = {};
        out_locale.size = sizeof(PulseHostLocale);
        return v3_.query_locale(v3_.context, &out_locale);
    }
    std::string QueryLanguageTag() const {
        PulseHostLocale locale{};
        if (QueryLocale(locale) != PULSE_OK) return {};
        size_t len = 0;
        while (len < sizeof(locale.language_tag) && locale.language_tag[len] != '\0') ++len;
        return std::string(locale.language_tag, len);
    }
    PulseStatus SubscribeLocale(LocaleSubscription& out_sub,
                                std::function<void(const PulseHostLocale&)> fn) const {
        out_sub.Reset();
        if (!fn) return PULSE_ERROR_ARGUMENT;
        if (!has_v3_ || !v3_.subscribe_locale) return PULSE_ERROR_UNAVAILABLE;
        auto slot = std::make_shared<LocaleSubscription::Slot>();
        slot->fn = std::move(fn);
        PulseLocaleSubscription tok = 0;
        const auto st = v3_.subscribe_locale(
            v3_.context, LocaleSubscription::Slot::OnLocale, slot.get(), &tok);
        if (st == PULSE_OK && tok != 0) {
            out_sub = LocaleSubscription(v3_, tok, std::move(slot));
        }
        return st;
    }
    std::optional<LocaleSubscription> SubscribeLocale(
        std::function<void(const PulseHostLocale&)> fn,
        PulseStatus* out_status = nullptr) const {
        LocaleSubscription sub;
        const PulseStatus st = SubscribeLocale(sub, std::move(fn));
        if (out_status) *out_status = st;
        if (st == PULSE_OK) return sub;
        return std::nullopt;
    }
};

struct TrayEntry {
    uint64_t item_id = 0;
    uint64_t batch_id = 0;
    uint64_t size_bytes = 0;
    uint32_t flags = PULSE_TRAY_ITEM_NONE;
    std::string path;
    std::string name;
};

struct TraySnapshot {
    uint64_t revision = 0;
    uint64_t total_bytes = 0;
    std::vector<TrayEntry> entries;

    static TraySnapshot CopyFrom(const PulseTraySnapshot& raw) {
        TraySnapshot snap;
        snap.revision = raw.revision;
        snap.total_bytes = raw.total_bytes;
        if (raw.entries && raw.entry_count > 0) {
            snap.entries.reserve(raw.entry_count);
            for (uint32_t i = 0; i < raw.entry_count; ++i) {
                const auto& e = raw.entries[i];
                TrayEntry item;
                item.item_id = e.item_id;
                item.batch_id = e.batch_id;
                item.size_bytes = e.size_bytes;
                item.flags = e.flags;
                if (e.path.data && e.path.length) item.path.assign(e.path.data, e.path.length);
                if (e.name.data && e.name.length) item.name.assign(e.name.data, e.name.length);
                snap.entries.push_back(std::move(item));
            }
        }
        return snap;
    }
};

class TrayClient {
    PulseTrayV1 api_{};
public:
    class Subscription {
        struct Slot : std::enable_shared_from_this<Slot> {
            bool active = true;
            std::function<void(const TraySnapshot&)> fn;
            static void PULSE_CALL OnSnapshot(void* user, const PulseTraySnapshot* snap) noexcept {
                try {
                    if (!user || !snap) return;
                    auto self = static_cast<Slot*>(user)->shared_from_this();
                    if (self->active && self->fn) self->fn(TraySnapshot::CopyFrom(*snap));
                } catch (...) {}
            }
        };
        PulseTrayV1 api_{};
        PulseTraySubscription token_ = 0;
        std::shared_ptr<Slot> slot_;
        friend class TrayClient;
        Subscription(const PulseTrayV1& api, PulseTraySubscription token, std::shared_ptr<Slot> slot) noexcept
            : api_(api), token_(token), slot_(std::move(slot)) {}
    public:
        Subscription() = default;
        ~Subscription() { Reset(); }
        Subscription(const Subscription&) = delete;
        Subscription& operator=(const Subscription&) = delete;
        Subscription(Subscription&& other) noexcept
            : api_(std::exchange(other.api_, {})),
              token_(std::exchange(other.token_, 0)),
              slot_(std::move(other.slot_)) {}
        Subscription& operator=(Subscription&& other) noexcept {
            if (this != &other) {
                Reset();
                api_ = std::exchange(other.api_, {});
                token_ = std::exchange(other.token_, 0);
                slot_ = std::move(other.slot_);
            }
            return *this;
        }
        bool Active() const noexcept { return token_ != 0; }
        PulseTraySubscription Id() const noexcept { return token_; }
        void Reset() noexcept {
            if (slot_) slot_->active = false;
            if (token_ != 0 && api_.unsubscribe) {
                api_.unsubscribe(api_.context, token_);
            }
            token_ = 0;
            api_ = {};
            slot_.reset();
        }
    };

    TrayClient() = default;
    explicit TrayClient(const PulseTrayV1& api) noexcept : api_(api) {}
    TrayClient(const TrayClient&) = delete;
    TrayClient& operator=(const TrayClient&) = delete;
    TrayClient(TrayClient&&) noexcept = default;
    TrayClient& operator=(TrayClient&&) noexcept = default;

    static std::optional<TrayClient> TryQuery(const Host& host) noexcept {
        TrayClient out;
        if (out.Init(host) == PULSE_OK) return out;
        return std::nullopt;
    }
    PulseStatus Init(const Host& host) noexcept {
        PulseTrayV1 table{};
        const auto st = host.Query(PULSE_TRAY_INTERFACE_NAME, PULSE_TRAY_VERSION_1, table);
        if (st == PULSE_OK) api_ = table;
        return st;
    }
    bool Available() const noexcept { return api_.snapshot != nullptr; }
    const PulseTrayV1& Raw() const noexcept { return api_; }

    PulseStatus Snapshot(TraySnapshot& out) const {
        if (!api_.snapshot) return PULSE_ERROR_UNAVAILABLE;
        struct Capture {
            TraySnapshot* dst = nullptr;
            static void PULSE_CALL Copy(void* user, const PulseTraySnapshot* snap) noexcept {
                try {
                    auto* c = static_cast<Capture*>(user);
                    if (c && c->dst && snap) *c->dst = TraySnapshot::CopyFrom(*snap);
                } catch (...) {}
            }
        } cap{&out};
        return api_.snapshot(api_.context, Capture::Copy, &cap);
    }

    PulseStatus Collect(const std::vector<std::string>& paths, bool move_intent = false) const {
        if (!api_.collect) return PULSE_ERROR_UNAVAILABLE;
        std::vector<PulseStringView> views;
        views.reserve(paths.size());
        for (const auto& p : paths) views.push_back(View(p));
        return api_.collect(api_.context, views.empty() ? nullptr : views.data(),
                            static_cast<uint32_t>(views.size()), move_intent ? 1u : 0u);
    }

    PulseStatus RemoveItem(uint64_t item_id) const {
        if (!api_.remove_item) return PULSE_ERROR_UNAVAILABLE;
        return api_.remove_item(api_.context, item_id);
    }

    PulseStatus Clear() const {
        if (!api_.clear) return PULSE_ERROR_UNAVAILABLE;
        return api_.clear(api_.context);
    }

    PulseStatus ReleaseToDirectory(std::string_view destination_dir,
                                   uint32_t release_mode = PULSE_TRAY_RELEASE_BATCH_DEFAULT) const {
        if (!api_.release_to_directory) return PULSE_ERROR_UNAVAILABLE;
        return api_.release_to_directory(api_.context, View(destination_dir), release_mode);
    }

    PulseStatus Subscribe(Subscription& out_sub, std::function<void(const TraySnapshot&)> fn) const {
        out_sub.Reset();
        if (!fn) return PULSE_ERROR_ARGUMENT;
        if (!api_.subscribe) return PULSE_ERROR_UNAVAILABLE;
        auto slot = std::make_shared<Subscription::Slot>();
        slot->fn = std::move(fn);
        PulseTraySubscription tok = 0;
        const auto st = api_.subscribe(api_.context, Subscription::Slot::OnSnapshot, slot.get(), &tok);
        if (st == PULSE_OK && tok != 0) {
            out_sub = Subscription(api_, tok, std::move(slot));
        }
        return st;
    }

    std::optional<Subscription> Subscribe(std::function<void(const TraySnapshot&)> fn,
                                          PulseStatus* out_status = nullptr) const {
        Subscription sub;
        const PulseStatus st = Subscribe(sub, std::move(fn));
        if (out_status) *out_status = st;
        if (st == PULSE_OK) return sub;
        return std::nullopt;
    }
};

struct SearchHit {
    std::string path;
    std::string name;
    uint64_t size_bytes = 0;
    int64_t modified_unix_ms = 0;
    uint32_t flags = PULSE_SEARCH_HIT_NONE;
};

struct SearchReplyData {
    PulseStatus status = PULSE_OK;
    PulseSearchRequestId request_id = 0;
    uint64_t total_matches = 0;
    bool index_connected = false;
    std::vector<SearchHit> hits;

    static SearchReplyData CopyFrom(const PulseSearchReply& raw) {
        SearchReplyData out;
        out.status = raw.status;
        out.request_id = raw.request_id;
        out.total_matches = raw.total_matches;
        out.index_connected = raw.index_connected != 0;
        if (raw.hits && raw.hit_count > 0) {
            out.hits.reserve(raw.hit_count);
            for (uint32_t i = 0; i < raw.hit_count; ++i) {
                const auto& src = raw.hits[i];
                SearchHit h;
                if (src.path.data && src.path.length) h.path.assign(src.path.data, src.path.length);
                if (src.name.data && src.name.length) h.name.assign(src.name.data, src.name.length);
                h.size_bytes = src.size_bytes;
                h.modified_unix_ms = src.modified_unix_ms;
                h.flags = src.flags;
                out.hits.push_back(std::move(h));
            }
        }
        return out;
    }
};

class SearchClient {
    PulseSearchV1 api_{};
public:
    class Request {
        struct Slot : std::enable_shared_from_this<Slot> {
            Request* owner = nullptr;
            bool active = true;
            std::function<void(const SearchReplyData&)> fn;
            static void PULSE_CALL OnReply(void* user, const PulseSearchReply* reply) noexcept {
                try {
                    if (!user || !reply) return;
                    auto self = static_cast<Slot*>(user)->shared_from_this();
                    if (!self->active) return;
                    self->active = false;
                    if (self->owner) self->owner->id_ = 0;
                    if (self->fn) self->fn(SearchReplyData::CopyFrom(*reply));
                } catch (...) {}
            }
        };
        PulseSearchV1 api_{};
        PulseSearchRequestId id_ = 0;
        std::shared_ptr<Slot> slot_;
        friend class SearchClient;
    public:
        Request() = default;
        ~Request() { Cancel(); }
        Request(const Request&) = delete;
        Request& operator=(const Request&) = delete;
        Request(Request&& other) noexcept
            : api_(std::exchange(other.api_, {})),
              id_(std::exchange(other.id_, 0)),
              slot_(std::move(other.slot_)) {
            if (slot_) slot_->owner = this;
        }
        Request& operator=(Request&& other) noexcept {
            if (this != &other) {
                Cancel();
                api_ = std::exchange(other.api_, {});
                id_ = std::exchange(other.id_, 0);
                slot_ = std::move(other.slot_);
                if (slot_) slot_->owner = this;
            }
            return *this;
        }
        bool Pending() const noexcept { return id_ != 0; }
        PulseSearchRequestId Id() const noexcept { return id_; }
        void Cancel() noexcept {
            if (slot_) {
                slot_->active = false;
                slot_->owner = nullptr;
                slot_.reset();
            }
            if (id_ != 0 && api_.cancel) {
                api_.cancel(api_.context, id_);
            }
            id_ = 0;
            api_ = {};
        }
    };

    SearchClient() = default;
    explicit SearchClient(const PulseSearchV1& api) noexcept : api_(api) {}
    SearchClient(const SearchClient&) = delete;
    SearchClient& operator=(const SearchClient&) = delete;
    SearchClient(SearchClient&&) noexcept = default;
    SearchClient& operator=(SearchClient&&) noexcept = default;

    static std::optional<SearchClient> TryQuery(const Host& host) noexcept {
        SearchClient out;
        if (out.Init(host) == PULSE_OK) return out;
        return std::nullopt;
    }
    PulseStatus Init(const Host& host) noexcept {
        PulseSearchV1 table{};
        const auto st = host.Query(PULSE_SEARCH_INTERFACE_NAME, PULSE_SEARCH_VERSION_1, table);
        if (st == PULSE_OK) api_ = table;
        return st;
    }
    bool Available() const noexcept { return api_.query != nullptr; }
    const PulseSearchV1& Raw() const noexcept { return api_; }

    PulseStatus Query(const PulseSearchRequest& request,
                      Request& out_request,
                      std::function<void(const SearchReplyData&)> fn) const {
        out_request.Cancel();
        if (!fn) return PULSE_ERROR_ARGUMENT;
        if (!api_.query) return PULSE_ERROR_UNAVAILABLE;
        auto slot = std::make_shared<Request::Slot>();
        slot->fn = std::move(fn);
        PulseSearchRequestId id = 0;
        const auto st = api_.query(api_.context, &request, Request::Slot::OnReply, slot.get(), &id);
        if (st == PULSE_OK && id != 0 && slot->active) {
            out_request.api_ = api_;
            out_request.id_ = id;
            slot->owner = &out_request;
            out_request.slot_ = std::move(slot);
        }
        return st;
    }

    std::optional<Request> Query(std::string_view query,
                                 std::function<void(const SearchReplyData&)> fn,
                                 uint32_t flags = PULSE_SEARCH_FLAG_PINYIN,
                                 uint32_t max_results = 20,
                                 std::string_view kind_token = {},
                                 std::string_view scope_directory = {},
                                 PulseStatus* out_status = nullptr) const {
        PulseSearchRequest req{};
        req.size = sizeof(req);
        req.flags = flags;
        req.max_results = max_results;
        req.query = View(query);
        req.kind_token = View(kind_token);
        req.scope_directory = View(scope_directory);
        Request handle;
        const PulseStatus st = Query(req, handle, std::move(fn));
        if (out_status) *out_status = st;
        if (st == PULSE_OK) return handle;
        return std::nullopt;
    }

    PulseStatus Cancel(PulseSearchRequestId request_id) const noexcept {
        if (!api_.cancel) return PULSE_ERROR_UNAVAILABLE;
        return api_.cancel(api_.context, request_id);
    }
};

struct ColorTag {
    std::string id;
    std::string name;
    uint32_t argb = 0;
    int32_t favorite_slot = 0;
};

struct ShellMenuItem {
    uint32_t item_id = 0;
    uint32_t flags = 0;
    uint32_t depth = 0;
    uint32_t mnemonic_codepoint = 0;
    std::string verb;
    std::string label;
    std::string handler_name;
};

struct ShellMenuReplyData {
    PulseStatus status = PULSE_OK;
    PulseShellMenuSessionId session_id = 0;
    bool partial = false;
    std::vector<ShellMenuItem> items;

    static ShellMenuReplyData CopyFrom(const PulseShellMenuReply& raw) {
        ShellMenuReplyData out;
        out.status = raw.status;
        out.session_id = raw.session_id;
        out.partial = raw.partial != 0;
        if (raw.items && raw.item_count > 0) {
            out.items.reserve(raw.item_count);
            for (uint32_t i = 0; i < raw.item_count; ++i) {
                const auto& src = raw.items[i];
                ShellMenuItem item;
                item.item_id = src.item_id;
                item.flags = src.flags;
                item.depth = src.depth;
                item.mnemonic_codepoint = src.mnemonic_codepoint;
                if (src.verb.data && src.verb.length) item.verb.assign(src.verb.data, src.verb.length);
                if (src.label.data && src.label.length) item.label.assign(src.label.data, src.label.length);
                if (src.handler_name.data && src.handler_name.length) {
                    item.handler_name.assign(src.handler_name.data, src.handler_name.length);
                }
                out.items.push_back(std::move(item));
            }
        }
        return out;
    }
};

struct ShellPreviewResult {
    PulseStatus status = PULSE_OK;
    PulseShellPreviewRequestId request_id = 0;
    std::string path;
    std::string display_type;
    std::string text_excerpt;
    uint64_t size_bytes = 0;
    int64_t modified_unix_ms = 0;
    uint32_t image_width = 0;
    uint32_t image_height = 0;
    uint32_t thumb_width = 0;
    uint32_t thumb_height = 0;
    uint32_t thumb_stride = 0;
    std::vector<uint8_t> thumb_bgra;

    static ShellPreviewResult CopyFrom(const PulseShellPreviewSummary& raw) {
        ShellPreviewResult out;
        out.status = raw.status;
        out.request_id = raw.request_id;
        if (raw.path.data && raw.path.length) out.path.assign(raw.path.data, raw.path.length);
        if (raw.display_type.data && raw.display_type.length) {
            out.display_type.assign(raw.display_type.data, raw.display_type.length);
        }
        if (raw.text_excerpt.data && raw.text_excerpt.length) {
            out.text_excerpt.assign(raw.text_excerpt.data, raw.text_excerpt.length);
        }
        out.size_bytes = raw.size_bytes;
        out.modified_unix_ms = raw.modified_unix_ms;
        out.image_width = raw.image_width;
        out.image_height = raw.image_height;
        out.thumb_width = raw.thumb_width;
        out.thumb_height = raw.thumb_height;
        out.thumb_stride = raw.thumb_stride;
        if (raw.thumb_bgra && raw.thumb_byte_count > 0) {
            out.thumb_bgra.assign(raw.thumb_bgra, raw.thumb_bgra + raw.thumb_byte_count);
        }
        return out;
    }
};

class ShellClient {
    PulseShellV1 api_{};
public:
    class MenuSession {
        struct Slot : std::enable_shared_from_this<Slot> {
            MenuSession* owner = nullptr;
            bool active = true;
            std::function<void(const ShellMenuReplyData&)> fn;
            static void PULSE_CALL OnReply(void* user, const PulseShellMenuReply* reply) noexcept {
                try {
                    if (!user || !reply) return;
                    auto self = static_cast<Slot*>(user)->shared_from_this();
                    if (!self->active) return;
                    if (reply->status != PULSE_OK) {
                        self->active = false;
                        if (self->owner) self->owner->id_ = 0;
                    }
                    if (self->fn) self->fn(ShellMenuReplyData::CopyFrom(*reply));
                } catch (...) {}
            }
        };
        PulseShellV1 api_{};
        PulseShellMenuSessionId id_ = 0;
        std::shared_ptr<Slot> slot_;
        friend class ShellClient;
    public:
        MenuSession() = default;
        ~MenuSession() { Close(); }
        MenuSession(const MenuSession&) = delete;
        MenuSession& operator=(const MenuSession&) = delete;
        MenuSession(MenuSession&& other) noexcept
            : api_(std::exchange(other.api_, {})),
              id_(std::exchange(other.id_, 0)),
              slot_(std::move(other.slot_)) {
            if (slot_) slot_->owner = this;
        }
        MenuSession& operator=(MenuSession&& other) noexcept {
            if (this != &other) {
                Close();
                api_ = std::exchange(other.api_, {});
                id_ = std::exchange(other.id_, 0);
                slot_ = std::move(other.slot_);
                if (slot_) slot_->owner = this;
            }
            return *this;
        }
        bool Open() const noexcept { return id_ != 0; }
        PulseShellMenuSessionId Id() const noexcept { return id_; }
        PulseStatus Invoke(uint32_t item_id, std::string_view verb = {}) noexcept {
            if (id_ == 0 || !api_.invoke_context_menu) return PULSE_ERROR_STATE;
            if (slot_) {
                slot_->active = false;
                slot_->owner = nullptr;
                slot_.reset();
            }
            const auto sid = std::exchange(id_, 0);
            const auto st = api_.invoke_context_menu(api_.context, sid, item_id, View(verb));
            api_ = {};
            return st;
        }
        void Close() noexcept {
            if (slot_) {
                slot_->active = false;
                slot_->owner = nullptr;
                slot_.reset();
            }
            if (id_ != 0 && api_.close_context_menu) {
                api_.close_context_menu(api_.context, id_);
            }
            id_ = 0;
            api_ = {};
        }
    };

    class PreviewRequest {
        struct Slot : std::enable_shared_from_this<Slot> {
            PreviewRequest* owner = nullptr;
            bool active = true;
            std::function<void(const ShellPreviewResult&)> fn;
            static void PULSE_CALL OnPreview(void* user, const PulseShellPreviewSummary* summary) noexcept {
                try {
                    if (!user || !summary) return;
                    auto self = static_cast<Slot*>(user)->shared_from_this();
                    if (!self->active) return;
                    self->active = false;
                    if (self->owner) self->owner->id_ = 0;
                    if (self->fn) self->fn(ShellPreviewResult::CopyFrom(*summary));
                } catch (...) {}
            }
        };
        PulseShellV1 api_{};
        PulseShellPreviewRequestId id_ = 0;
        std::shared_ptr<Slot> slot_;
        friend class ShellClient;
    public:
        PreviewRequest() = default;
        ~PreviewRequest() { Cancel(); }
        PreviewRequest(const PreviewRequest&) = delete;
        PreviewRequest& operator=(const PreviewRequest&) = delete;
        PreviewRequest(PreviewRequest&& other) noexcept
            : api_(std::exchange(other.api_, {})),
              id_(std::exchange(other.id_, 0)),
              slot_(std::move(other.slot_)) {
            if (slot_) slot_->owner = this;
        }
        PreviewRequest& operator=(PreviewRequest&& other) noexcept {
            if (this != &other) {
                Cancel();
                api_ = std::exchange(other.api_, {});
                id_ = std::exchange(other.id_, 0);
                slot_ = std::move(other.slot_);
                if (slot_) slot_->owner = this;
            }
            return *this;
        }
        bool Pending() const noexcept { return id_ != 0; }
        PulseShellPreviewRequestId Id() const noexcept { return id_; }
        void Cancel() noexcept {
            if (slot_) {
                slot_->active = false;
                slot_->owner = nullptr;
                slot_.reset();
            }
            if (id_ != 0 && api_.cancel_preview) {
                api_.cancel_preview(api_.context, id_);
            }
            id_ = 0;
            api_ = {};
        }
    };

    ShellClient() = default;
    explicit ShellClient(const PulseShellV1& api) noexcept : api_(api) {}
    ShellClient(const ShellClient&) = delete;
    ShellClient& operator=(const ShellClient&) = delete;
    ShellClient(ShellClient&&) noexcept = default;
    ShellClient& operator=(ShellClient&&) noexcept = default;

    static std::optional<ShellClient> TryQuery(const Host& host) noexcept {
        ShellClient out;
        if (out.Init(host) == PULSE_OK) return out;
        return std::nullopt;
    }
    PulseStatus Init(const Host& host) noexcept {
        PulseShellV1 table{};
        const auto st = host.Query(PULSE_SHELL_INTERFACE_NAME, PULSE_SHELL_VERSION_1, table);
        if (st == PULSE_OK) api_ = table;
        return st;
    }
    bool Available() const noexcept { return api_.get_tag_catalog != nullptr; }
    const PulseShellV1& Raw() const noexcept { return api_; }

    PulseStatus GetTagCatalog(std::vector<ColorTag>& out) const {
        out.clear();
        if (!api_.get_tag_catalog) return PULSE_ERROR_UNAVAILABLE;
        struct Capture {
            std::vector<ColorTag>* dst = nullptr;
            static void PULSE_CALL Copy(void* user, const PulseColorTagInfo* tags, uint32_t count) noexcept {
                try {
                    auto* c = static_cast<Capture*>(user);
                    if (!c || !c->dst || !tags) return;
                    c->dst->reserve(count);
                    for (uint32_t i = 0; i < count; ++i) {
                        ColorTag item;
                        if (tags[i].id.data && tags[i].id.length) {
                            item.id.assign(tags[i].id.data, tags[i].id.length);
                        }
                        if (tags[i].name.data && tags[i].name.length) {
                            item.name.assign(tags[i].name.data, tags[i].name.length);
                        }
                        item.argb = tags[i].argb;
                        item.favorite_slot = tags[i].favorite_slot;
                        c->dst->push_back(std::move(item));
                    }
                } catch (...) {}
            }
        } cap{&out};
        return api_.get_tag_catalog(api_.context, Capture::Copy, &cap);
    }

    PulseStatus GetPathTags(std::string_view path, std::vector<ColorTag>& out) const {
        out.clear();
        if (!api_.get_path_tags) return PULSE_ERROR_UNAVAILABLE;
        struct Capture {
            std::vector<ColorTag>* dst = nullptr;
            static void PULSE_CALL Copy(void* user, const PulseColorTagInfo* tags, uint32_t count) noexcept {
                try {
                    auto* c = static_cast<Capture*>(user);
                    if (!c || !c->dst || !tags) return;
                    c->dst->reserve(count);
                    for (uint32_t i = 0; i < count; ++i) {
                        ColorTag item;
                        if (tags[i].id.data && tags[i].id.length) {
                            item.id.assign(tags[i].id.data, tags[i].id.length);
                        }
                        if (tags[i].name.data && tags[i].name.length) {
                            item.name.assign(tags[i].name.data, tags[i].name.length);
                        }
                        item.argb = tags[i].argb;
                        item.favorite_slot = tags[i].favorite_slot;
                        c->dst->push_back(std::move(item));
                    }
                } catch (...) {}
            }
        } cap{&out};
        return api_.get_path_tags(api_.context, View(path), Capture::Copy, &cap);
    }

    PulseStatus SetPathTag(std::string_view path, std::string_view tag_id, bool tagged) const {
        if (!api_.set_path_tag) return PULSE_ERROR_UNAVAILABLE;
        return api_.set_path_tag(api_.context, View(path), View(tag_id), tagged ? 1u : 0u);
    }

    PulseStatus QueryContextMenu(const PulseShellMenuRequest& request,
                                 MenuSession& out_session,
                                 std::function<void(const ShellMenuReplyData&)> fn) const {
        out_session.Close();
        if (!fn) return PULSE_ERROR_ARGUMENT;
        if (!api_.query_context_menu) return PULSE_ERROR_UNAVAILABLE;
        auto slot = std::make_shared<MenuSession::Slot>();
        slot->fn = std::move(fn);
        PulseShellMenuSessionId sid = 0;
        const auto st = api_.query_context_menu(api_.context, &request, MenuSession::Slot::OnReply, slot.get(), &sid);
        if (st == PULSE_OK && sid != 0 && slot->active) {
            out_session.api_ = api_;
            out_session.id_ = sid;
            slot->owner = &out_session;
            out_session.slot_ = std::move(slot);
        }
        return st;
    }

    PulseStatus QueryContextMenu(const std::vector<std::string>& paths,
                                 uint32_t flags,
                                 MenuSession& out_session,
                                 std::function<void(const ShellMenuReplyData&)> fn) const {
        std::vector<PulseStringView> views;
        views.reserve(paths.size());
        for (const auto& p : paths) views.push_back(View(p));
        PulseShellMenuRequest req{};
        req.size = sizeof(req);
        req.flags = flags;
        req.paths = views.empty() ? nullptr : views.data();
        req.path_count = static_cast<uint32_t>(views.size());
        return QueryContextMenu(req, out_session, std::move(fn));
    }

    PulseStatus InvokeContextMenu(PulseShellMenuSessionId session_id,
                                  uint32_t item_id,
                                  std::string_view verb = {}) const noexcept {
        if (!api_.invoke_context_menu) return PULSE_ERROR_UNAVAILABLE;
        return api_.invoke_context_menu(api_.context, session_id, item_id, View(verb));
    }

    PulseStatus CloseContextMenu(PulseShellMenuSessionId session_id) const noexcept {
        if (!api_.close_context_menu) return PULSE_ERROR_UNAVAILABLE;
        return api_.close_context_menu(api_.context, session_id);
    }

    PulseStatus ReadPreview(std::string_view path,
                            uint32_t max_thumb_px,
                            PreviewRequest& out_request,
                            std::function<void(const ShellPreviewResult&)> fn) const {
        out_request.Cancel();
        if (!fn) return PULSE_ERROR_ARGUMENT;
        if (!api_.read_preview) return PULSE_ERROR_UNAVAILABLE;
        auto slot = std::make_shared<PreviewRequest::Slot>();
        slot->fn = std::move(fn);
        PulseShellPreviewRequestId rid = 0;
        const auto st = api_.read_preview(
            api_.context, View(path), max_thumb_px, PreviewRequest::Slot::OnPreview, slot.get(), &rid);
        if (st == PULSE_OK && rid != 0 && slot->active) {
            out_request.api_ = api_;
            out_request.id_ = rid;
            slot->owner = &out_request;
            out_request.slot_ = std::move(slot);
        }
        return st;
    }

    std::optional<PreviewRequest> ReadPreview(std::string_view path,
                                              uint32_t max_thumb_px,
                                              std::function<void(const ShellPreviewResult&)> fn,
                                              PulseStatus* out_status = nullptr) const {
        PreviewRequest req;
        const PulseStatus st = ReadPreview(path, max_thumb_px, req, std::move(fn));
        if (out_status) *out_status = st;
        if (st == PULSE_OK) return req;
        return std::nullopt;
    }

    PulseStatus CancelPreview(PulseShellPreviewRequestId request_id) const noexcept {
        if (!api_.cancel_preview) return PULSE_ERROR_UNAVAILABLE;
        return api_.cancel_preview(api_.context, request_id);
    }
};

class SettingsPagesClient {
    PulseSettingsPagesV1 v1_{};
    PulseSettingsPagesV2 v2_{};
    bool has_v2_ = false;
public:
    SettingsPagesClient() = default;
    explicit SettingsPagesClient(const PulseSettingsPagesV1& api) noexcept : v1_(api) {}
    explicit SettingsPagesClient(const PulseSettingsPagesV2& api) noexcept
        : v1_{sizeof(PulseSettingsPagesV1), PULSE_SETTINGS_PAGES_VERSION_1, api.context,
              api.create, api.query, api.remove},
          v2_(api),
          has_v2_(true) {}

    PulseStatus Attach(const Host& host) noexcept {
        v1_ = {};
        v2_ = {};
        has_v2_ = false;
        PulseSettingsPagesV2 v2{};
        if (host.Query(PULSE_SETTINGS_PAGES_INTERFACE_NAME, PULSE_SETTINGS_PAGES_VERSION_2, v2) == PULSE_OK) {
            v1_ = {sizeof(PulseSettingsPagesV1), PULSE_SETTINGS_PAGES_VERSION_1, v2.context,
                   v2.create, v2.query, v2.remove};
            v2_ = v2;
            has_v2_ = true;
            return PULSE_OK;
        }
        return host.Query(PULSE_SETTINGS_PAGES_INTERFACE_NAME, PULSE_SETTINGS_PAGES_VERSION_1, v1_);
    }
    static std::optional<SettingsPagesClient> TryQuery(const Host& host) noexcept {
        SettingsPagesClient client;
        if (client.Attach(host) == PULSE_OK) return client;
        return std::nullopt;
    }
    bool Available() const noexcept { return v1_.create != nullptr; }
    bool HasV2() const noexcept { return has_v2_; }

    PulseStatus Create(std::string_view title, const PulseUiModelTree& tree, uint64_t& out_page) const noexcept {
        out_page = 0;
        if (!v1_.create) return PULSE_ERROR_UNAVAILABLE;
        return v1_.create(v1_.context, View(title), &tree, &out_page);
    }
    PulseStatus CreateTab(std::string_view id, std::string_view title, const PulseUiModelTree& tree,
                          uint64_t& out_page, std::string_view glyph = {}, int32_t order = 0) const noexcept {
        out_page = 0;
        if (!has_v2_ || !v2_.create_tab) return PULSE_ERROR_UNAVAILABLE;
        PulseSettingsTabDesc desc{};
        desc.size = sizeof(desc);
        desc.id = View(id);
        desc.title = View(title);
        desc.glyph = View(glyph);
        desc.order = order;
        return v2_.create_tab(v2_.context, &desc, &tree, &out_page);
    }
    PulseStatus SetStatus(uint64_t page, uint32_t kind, std::string_view text = {}) const noexcept {
        if (!has_v2_ || !v2_.set_status) return PULSE_ERROR_UNAVAILABLE;
        return v2_.set_status(v2_.context, page, kind, View(text));
    }
    PulseStatus SetSidePreview(uint64_t page, const PulseUiModelTree* preview, uint32_t width_dip = 320) const noexcept {
        if (!has_v2_ || !v2_.set_side_preview) return PULSE_ERROR_UNAVAILABLE;
        return v2_.set_side_preview(v2_.context, page, preview, width_dip);
    }
    PulseStatus Remove(uint64_t page) const noexcept {
        if (!v1_.remove) return PULSE_ERROR_UNAVAILABLE;
        return v1_.remove(v1_.context, page);
    }
    PulseStatus QueryModel(uint64_t page, PulseUiModelV1& out) const noexcept {
        if (!v1_.query) return PULSE_ERROR_UNAVAILABLE;
        out = {};
        return v1_.query(v1_.context, page, View(PULSE_UI_MODEL_INTERFACE), PULSE_UI_MODEL_VERSION, &out, sizeof(out));
    }
    PulseStatus QueryEvents(uint64_t page, PulseUiEventsV1& out) const noexcept {
        if (!v1_.query) return PULSE_ERROR_UNAVAILABLE;
        out = {};
        return v1_.query(v1_.context, page, View(PULSE_UI_EVENTS_INTERFACE_NAME), PULSE_UI_EVENTS_VERSION_1, &out, sizeof(out));
    }
    PulseStatus QueryData(uint64_t page, PulseUiDataV1& out) const noexcept {
        if (!v1_.query) return PULSE_ERROR_UNAVAILABLE;
        out = {};
        return v1_.query(v1_.context, page, View(PULSE_UI_DATA_INTERFACE), PULSE_UI_DATA_VERSION, &out, sizeof(out));
    }
};

struct AssociationStatusData {
    uint32_t owner = PULSE_ASSOC_OWNER_UNREGISTERED;
    uint32_t flags = PULSE_ASSOC_STATUS_FLAG_NONE;
    std::string extension;
    std::string handler_id;
    std::string current_app;
};

struct AssociationEntrySpec {
    uint32_t action = PULSE_ASSOC_ACTION_COMMAND;
    std::string handler_id;
    std::string extension;
    std::string display_name;
    std::string verb_label;
    std::string icon_resource;
};

class AssociationsClient {
    PulseAssociationsV1 api_{};

public:
    class Request {
        struct Slot : std::enable_shared_from_this<Slot> {
            Request* owner = nullptr;
            bool active = true;
            std::function<void(PulseStatus, const std::vector<AssociationStatusData>&)> status_fn;
            std::function<void(PulseStatus)> complete_fn;

            static void PULSE_CALL OnStatus(void* user,
                                            PulseAssociationRequestId,
                                            PulseStatus status,
                                            const PulseAssociationStatus* items,
                                            uint32_t count) noexcept {
                try {
                    if (!user) return;
                    auto self = static_cast<Slot*>(user)->shared_from_this();
                    if (!self->active) return;
                    self->active = false;
                    auto fn = std::move(self->status_fn);
                    if (self->owner) {
                        self->owner->id_ = 0;
                        self->owner->slot_.reset();
                        self->owner = nullptr;
                    }
                    std::vector<AssociationStatusData> out;
                    if (items && count > 0) {
                        out.reserve(count);
                        for (uint32_t i = 0; i < count; ++i) {
                            AssociationStatusData row;
                            row.owner = items[i].owner;
                            row.flags = items[i].flags;
                            if (items[i].extension.data && items[i].extension.length) {
                                row.extension.assign(items[i].extension.data, items[i].extension.length);
                            }
                            if (items[i].handler_id.data && items[i].handler_id.length) {
                                row.handler_id.assign(items[i].handler_id.data, items[i].handler_id.length);
                            }
                            if (items[i].current_app.data && items[i].current_app.length) {
                                row.current_app.assign(items[i].current_app.data, items[i].current_app.length);
                            }
                            out.push_back(std::move(row));
                        }
                    }
                    if (fn) fn(status, out);
                } catch (...) {}
            }

            static void PULSE_CALL OnComplete(void* user,
                                              PulseAssociationRequestId,
                                              PulseStatus status) noexcept {
                try {
                    if (!user) return;
                    auto self = static_cast<Slot*>(user)->shared_from_this();
                    if (!self->active) return;
                    self->active = false;
                    auto fn = std::move(self->complete_fn);
                    if (self->owner) {
                        self->owner->id_ = 0;
                        self->owner->slot_.reset();
                        self->owner = nullptr;
                    }
                    if (fn) fn(status);
                } catch (...) {}
            }
        };

        PulseAssociationsV1 api_{};
        PulseAssociationRequestId id_ = 0;
        std::shared_ptr<Slot> slot_;
        friend class AssociationsClient;

    public:
        Request() = default;
        ~Request() { Cancel(); }
        Request(const Request&) = delete;
        Request& operator=(const Request&) = delete;
        Request(Request&& other) noexcept
            : api_(std::exchange(other.api_, {})),
              id_(std::exchange(other.id_, 0)),
              slot_(std::move(other.slot_)) {
            if (slot_) slot_->owner = this;
        }
        Request& operator=(Request&& other) noexcept {
            if (this != &other) {
                Cancel();
                api_ = std::exchange(other.api_, {});
                id_ = std::exchange(other.id_, 0);
                slot_ = std::move(other.slot_);
                if (slot_) slot_->owner = this;
            }
            return *this;
        }

        bool Pending() const noexcept { return id_ != 0; }
        PulseAssociationRequestId Id() const noexcept { return id_; }
        void Cancel() noexcept {
            if (slot_) {
                slot_->active = false;
                slot_->owner = nullptr;
                slot_.reset();
            }
            if (id_ != 0 && api_.cancel) {
                api_.cancel(api_.context, id_);
            }
            id_ = 0;
            api_ = {};
        }
    };

    AssociationsClient() = default;
    explicit AssociationsClient(const PulseAssociationsV1& api) noexcept : api_(api) {}
    AssociationsClient(const AssociationsClient&) = delete;
    AssociationsClient& operator=(const AssociationsClient&) = delete;
    AssociationsClient(AssociationsClient&&) noexcept = default;
    AssociationsClient& operator=(AssociationsClient&&) noexcept = default;

    static std::optional<AssociationsClient> TryQuery(const Host& host) noexcept {
        AssociationsClient out;
        if (out.Init(host) == PULSE_OK) return out;
        return std::nullopt;
    }

    PulseStatus Init(const Host& host) noexcept {
        PulseAssociationsV1 table{};
        const auto st = host.Query(PULSE_ASSOCIATIONS_INTERFACE_NAME, PULSE_ASSOCIATIONS_VERSION_1, table);
        if (st == PULSE_OK) api_ = table;
        return st;
    }

    bool Available() const noexcept { return api_.query != nullptr; }
    const PulseAssociationsV1& Raw() const noexcept { return api_; }

    PulseStatus Query(const std::vector<std::string>& extensions,
                      Request& out_request,
                      std::function<void(PulseStatus, const std::vector<AssociationStatusData>&)> fn) const {
        out_request.Cancel();
        if (!fn) return PULSE_ERROR_ARGUMENT;
        if (!api_.query) return PULSE_ERROR_UNAVAILABLE;
        std::vector<PulseStringView> views;
        views.reserve(extensions.size());
        for (const auto& ext : extensions) views.push_back(View(ext));
        auto slot = std::make_shared<Request::Slot>();
        slot->status_fn = std::move(fn);
        PulseAssociationRequestId rid = 0;
        const auto st = api_.query(api_.context, views.empty() ? nullptr : views.data(),
                                   static_cast<uint32_t>(views.size()),
                                   Request::Slot::OnStatus, slot.get(), &rid);
        if (st == PULSE_OK && rid != 0 && slot->active) {
            out_request.api_ = api_;
            out_request.id_ = rid;
            slot->owner = &out_request;
            out_request.slot_ = std::move(slot);
        }
        return st;
    }

    PulseStatus QueryAll(Request& out_request,
                         std::function<void(PulseStatus, const std::vector<AssociationStatusData>&)> fn) const {
        return Query({}, out_request, std::move(fn));
    }

    std::optional<Request> Query(const std::vector<std::string>& extensions,
                                 std::function<void(PulseStatus, const std::vector<AssociationStatusData>&)> fn,
                                 PulseStatus* out_status = nullptr) const {
        Request req;
        const auto st = Query(extensions, req, std::move(fn));
        if (out_status) *out_status = st;
        if (st == PULSE_OK) return req;
        return std::nullopt;
    }

    PulseStatus Apply(const std::vector<AssociationEntrySpec>& entries,
                      Request& out_request,
                      std::function<void(PulseStatus)> fn) const {
        out_request.Cancel();
        if (!fn) return PULSE_ERROR_ARGUMENT;
        if (!api_.apply) return PULSE_ERROR_UNAVAILABLE;
        std::vector<PulseAssociationEntry> raw;
        raw.reserve(entries.size());
        for (const auto& entry : entries) {
            PulseAssociationEntry item{};
            item.size = sizeof(item);
            item.action = entry.action;
            item.handler_id = View(entry.handler_id);
            item.extension = View(entry.extension);
            item.display_name = View(entry.display_name);
            item.verb_label = View(entry.verb_label);
            item.icon_resource = View(entry.icon_resource);
            raw.push_back(item);
        }
        auto slot = std::make_shared<Request::Slot>();
        slot->complete_fn = std::move(fn);
        PulseAssociationRequestId rid = 0;
        const auto st = api_.apply(api_.context, raw.empty() ? nullptr : raw.data(),
                                   static_cast<uint32_t>(raw.size()),
                                   Request::Slot::OnComplete, slot.get(), &rid);
        if (st == PULSE_OK && rid != 0 && slot->active) {
            out_request.api_ = api_;
            out_request.id_ = rid;
            slot->owner = &out_request;
            out_request.slot_ = std::move(slot);
        }
        return st;
    }

    std::optional<Request> Apply(const std::vector<AssociationEntrySpec>& entries,
                                 std::function<void(PulseStatus)> fn,
                                 PulseStatus* out_status = nullptr) const {
        Request req;
        const auto st = Apply(entries, req, std::move(fn));
        if (out_status) *out_status = st;
        if (st == PULSE_OK) return req;
        return std::nullopt;
    }

    PulseStatus Remove(const std::vector<std::string>& extensions,
                       Request& out_request,
                       std::function<void(PulseStatus)> fn) const {
        out_request.Cancel();
        if (!fn) return PULSE_ERROR_ARGUMENT;
        if (!api_.remove) return PULSE_ERROR_UNAVAILABLE;
        std::vector<PulseStringView> views;
        views.reserve(extensions.size());
        for (const auto& ext : extensions) views.push_back(View(ext));
        auto slot = std::make_shared<Request::Slot>();
        slot->complete_fn = std::move(fn);
        PulseAssociationRequestId rid = 0;
        const auto st = api_.remove(api_.context, views.empty() ? nullptr : views.data(),
                                    static_cast<uint32_t>(views.size()),
                                    Request::Slot::OnComplete, slot.get(), &rid);
        if (st == PULSE_OK && rid != 0 && slot->active) {
            out_request.api_ = api_;
            out_request.id_ = rid;
            slot->owner = &out_request;
            out_request.slot_ = std::move(slot);
        }
        return st;
    }

    PulseStatus RemoveAll(Request& out_request, std::function<void(PulseStatus)> fn) const {
        return Remove({}, out_request, std::move(fn));
    }

    PulseStatus Cancel(PulseAssociationRequestId request_id) const noexcept {
        if (!api_.cancel) return PULSE_ERROR_UNAVAILABLE;
        return api_.cancel(api_.context, request_id);
    }

    PulseStatus OpenChooser(std::string_view extension) const noexcept {
        if (!api_.open_chooser) return PULSE_ERROR_UNAVAILABLE;
        return api_.open_chooser(api_.context, View(extension));
    }

    PulseStatus OpenDefaultApps() const noexcept {
        if (!api_.open_default_apps) return PULSE_ERROR_UNAVAILABLE;
        return api_.open_default_apps(api_.context);
    }
};

struct VfsEntryData {
    std::string name;
    uint64_t size_bytes = 0;
    int64_t modified_unix_ms = 0;
    uint32_t flags = 0;
    std::string type_text;
};

struct VfsChange {
    uint32_t kind = PULSE_VFS_CHANGE_PUT;
    std::string_view rel_path;
    std::string_view source_path_utf8;
    std::string_view new_rel_path;
};

struct VfsOperation {
    const PulseVfsOperation* raw = nullptr;
    bool Cancelled() const noexcept {
        return raw && raw->cancel && raw->cancel->is_cancelled && raw->cancel->is_cancelled(raw->cancel->context) != 0;
    }
    void Progress(uint64_t done_bytes, uint64_t total_bytes, uint64_t done_items, uint64_t total_items,
                  std::string_view current = {}) const noexcept {
        if (!raw || !raw->progress) return;
        PulseVfsProgress p{};
        p.size = sizeof(p);
        p.completed_bytes = done_bytes;
        p.total_bytes = total_bytes;
        p.completed_items = done_items;
        p.total_items = total_items;
        p.current = PulseStringView{current.data(), static_cast<uint32_t>(current.size())};
        raw->progress(raw->context, &p);
    }
    void Message(std::string_view text) const noexcept {
        if (raw && raw->message) raw->message(raw->context, PulseStringView{text.data(), static_cast<uint32_t>(text.size())});
    }
};

struct VfsProviderCallbacks {
    uint32_t capabilities = PULSE_VFS_CAP_LIST_DIR | PULSE_VFS_CAP_READONLY;
    std::string icon_glyph;
    std::function<PulseStatus(std::string_view rel_path,
                              const std::function<bool(const VfsEntryData&)>& emit,
                              const std::function<bool()>& cancelled)> list_dir;
    std::function<PulseStatus(std::string_view rel_path,
                              std::string_view dest_path_utf8,
                              const std::function<bool()>& cancelled)> extract_file;
    std::function<PulseStatus(std::string_view rel_path,
                              VfsEntryData& out_entry,
                              const std::function<bool()>& cancelled)> stat;
    // PULSE_VFS_CAP_EXTRACT_ITEMS / PULSE_VFS_CAP_MODIFY; see VfsOperation.
    std::function<PulseStatus(const std::vector<std::string_view>& rel_paths,
                              std::string_view dest_dir_utf8,
                              const VfsOperation& op)> extract_items;
    std::function<PulseStatus(const std::vector<VfsChange>& changes,
                              const VfsOperation& op)> modify;
};

class VfsClient {
    PulseVfsV1 api_{};

    struct ProviderSlot : std::enable_shared_from_this<ProviderSlot> {
        VfsProviderCallbacks cb;

        static bool CancelFlag(const PulseVfsCancelToken* cancel) noexcept {
            return cancel && cancel->is_cancelled && cancel->is_cancelled(cancel->context) != 0;
        }

        static PulseStatus PULSE_CALL ListDir(void* user,
                                              PulseStringView,
                                              PulseStringView rel_path,
                                              PulseVfsEmitEntryFn emit_entry,
                                              void* emit_context,
                                              const PulseVfsCancelToken* cancel) noexcept {
            try {
                if (!user || !emit_entry) return PULSE_ERROR_ARGUMENT;
                auto* slot = static_cast<ProviderSlot*>(user);
                if (!slot->cb.list_dir) return PULSE_ERROR_UNAVAILABLE;
                const std::string_view rel(rel_path.data ? rel_path.data : "", rel_path.length);
                const auto emit = [emit_entry, emit_context](const VfsEntryData& item) -> bool {
                    PulseVfsEntry raw{};
                    raw.size = sizeof(raw);
                    raw.flags = item.flags;
                    raw.size_bytes = item.size_bytes;
                    raw.modified_unix_ms = item.modified_unix_ms;
                    raw.name = View(item.name);
                    raw.type_text = View(item.type_text);
                    return emit_entry(emit_context, &raw) != 0;
                };
                const auto cancelled = [cancel]() noexcept { return CancelFlag(cancel); };
                return slot->cb.list_dir(rel, emit, cancelled);
            } catch (...) {
                return PULSE_ERROR_STATE;
            }
        }

        static PulseStatus PULSE_CALL ExtractFile(void* user,
                                                  PulseStringView,
                                                  PulseStringView rel_path,
                                                  PulseStringView dest_path_utf8,
                                                  const PulseVfsCancelToken* cancel) noexcept {
            try {
                if (!user) return PULSE_ERROR_ARGUMENT;
                auto* slot = static_cast<ProviderSlot*>(user);
                if (!slot->cb.extract_file) return PULSE_ERROR_UNAVAILABLE;
                const std::string_view rel(rel_path.data ? rel_path.data : "", rel_path.length);
                const std::string_view dest(dest_path_utf8.data ? dest_path_utf8.data : "", dest_path_utf8.length);
                const auto cancelled = [cancel]() noexcept { return CancelFlag(cancel); };
                return slot->cb.extract_file(rel, dest, cancelled);
            } catch (...) {
                return PULSE_ERROR_STATE;
            }
        }

        static PulseStatus PULSE_CALL Stat(void* user,
                                           PulseStringView,
                                           PulseStringView rel_path,
                                           PulseVfsEmitEntryFn emit_entry,
                                           void* emit_context,
                                           const PulseVfsCancelToken* cancel) noexcept {
            try {
                if (!user || !emit_entry) return PULSE_ERROR_ARGUMENT;
                auto* slot = static_cast<ProviderSlot*>(user);
                if (!slot->cb.stat) return PULSE_ERROR_UNAVAILABLE;
                const std::string_view rel(rel_path.data ? rel_path.data : "", rel_path.length);
                const auto cancelled = [cancel]() noexcept { return CancelFlag(cancel); };
                VfsEntryData item;
                const PulseStatus st = slot->cb.stat(rel, item, cancelled);
                if (st != PULSE_OK) return st;
                PulseVfsEntry raw{};
                raw.size = sizeof(raw);
                raw.flags = item.flags;
                raw.size_bytes = item.size_bytes;
                raw.modified_unix_ms = item.modified_unix_ms;
                raw.name = View(item.name);
                raw.type_text = View(item.type_text);
                (void)emit_entry(emit_context, &raw);
                return PULSE_OK;
            } catch (...) {
                return PULSE_ERROR_STATE;
            }
        }

        static PulseStatus PULSE_CALL ExtractItems(void* user,
                                                   PulseStringView,
                                                   const PulseStringView* rel_paths,
                                                   uint32_t count,
                                                   PulseStringView dest_dir_utf8,
                                                   const PulseVfsOperation* op) noexcept {
            try {
                if (!user || (count && !rel_paths)) return PULSE_ERROR_ARGUMENT;
                auto* slot = static_cast<ProviderSlot*>(user);
                if (!slot->cb.extract_items) return PULSE_ERROR_UNAVAILABLE;
                std::vector<std::string_view> rels;
                rels.reserve(count);
                for (uint32_t i = 0; i < count; ++i)
                    rels.emplace_back(rel_paths[i].data ? rel_paths[i].data : "", rel_paths[i].length);
                const std::string_view dest(dest_dir_utf8.data ? dest_dir_utf8.data : "", dest_dir_utf8.length);
                VfsOperation wrap;
                wrap.raw = op && op->size >= sizeof(PulseVfsOperation) ? op : nullptr;
                return slot->cb.extract_items(rels, dest, wrap);
            } catch (...) {
                return PULSE_ERROR_STATE;
            }
        }

        static PulseStatus PULSE_CALL Modify(void* user,
                                             PulseStringView,
                                             const PulseVfsChange* changes,
                                             uint32_t count,
                                             const PulseVfsOperation* op) noexcept {
            try {
                if (!user || (count && !changes)) return PULSE_ERROR_ARGUMENT;
                auto* slot = static_cast<ProviderSlot*>(user);
                if (!slot->cb.modify) return PULSE_ERROR_UNAVAILABLE;
                const auto text = [](PulseStringView v) { return std::string_view(v.data ? v.data : "", v.length); };
                std::vector<VfsChange> list;
                list.reserve(count);
                for (uint32_t i = 0; i < count; ++i) {
                    if (changes[i].size < sizeof(PulseVfsChange)) return PULSE_ERROR_ARGUMENT;
                    list.push_back(VfsChange{changes[i].kind, text(changes[i].rel_path), text(changes[i].source_path_utf8),
                                             text(changes[i].new_rel_path)});
                }
                VfsOperation wrap;
                wrap.raw = op && op->size >= sizeof(PulseVfsOperation) ? op : nullptr;
                return slot->cb.modify(list, wrap);
            } catch (...) {
                return PULSE_ERROR_STATE;
            }
        }
    };

public:
    class MountHandle {
        friend class VfsClient;
        PulseVfsV1 api_{};
        PulseVfsMountHandle handle_ = 0;
        std::shared_ptr<ProviderSlot> slot_;

    public:
        MountHandle() = default;
        ~MountHandle() { Reset(); }
        MountHandle(const MountHandle&) = delete;
        MountHandle& operator=(const MountHandle&) = delete;
        MountHandle(MountHandle&& other) noexcept
            : api_(other.api_),
              handle_(std::exchange(other.handle_, 0)),
              slot_(std::move(other.slot_)) {}
        MountHandle& operator=(MountHandle&& other) noexcept {
            if (this != &other) {
                Reset();
                api_ = other.api_;
                handle_ = std::exchange(other.handle_, 0);
                slot_ = std::move(other.slot_);
            }
            return *this;
        }

        void Reset() noexcept {
            const auto h = std::exchange(handle_, 0);
            if (h != 0 && api_.unregister_mount) {
                (void)api_.unregister_mount(api_.context, h);
            }
            api_ = {};
            slot_.reset();
        }

        bool Active() const noexcept { return handle_ != 0; }
        PulseVfsMountHandle Raw() const noexcept { return handle_; }

        PulseStatus Invalidate(std::string_view rel_path = {}) const noexcept {
            if (handle_ == 0 || !api_.invalidate_path) return PULSE_ERROR_STATE;
            return api_.invalidate_path(api_.context, handle_, View(rel_path));
        }

        PulseStatus Open(std::string_view rel_path = {},
                         uint32_t mode = PULSE_VFS_OPEN_CURRENT_TAB) const noexcept {
            if (handle_ == 0 || !api_.open_path) return PULSE_ERROR_STATE;
            return api_.open_path(api_.context, handle_, View(rel_path), mode);
        }
    };

    VfsClient() = default;
    explicit VfsClient(const PulseVfsV1& api) noexcept : api_(api) {}

    PulseStatus Attach(const Host& host) noexcept {
        return host.Query(PULSE_VFS_INTERFACE_NAME, PULSE_VFS_VERSION_1, api_);
    }
    static std::optional<VfsClient> TryQuery(const Host& host) noexcept {
        VfsClient client;
        if (client.Attach(host) == PULSE_OK) return client;
        return std::nullopt;
    }

    bool Available() const noexcept { return api_.register_mount != nullptr; }
    const PulseVfsV1& Table() const noexcept { return api_; }

    static std::string FormatUri(std::string_view package_id,
                                 std::string_view mount_id,
                                 std::string_view rel_path = {}) {
        std::string uri = "pulse:vfs:";
        uri.append(package_id);
        uri.push_back('/');
        uri.append(mount_id);
        std::string_view rel = rel_path;
        while (!rel.empty() && (rel.front() == '/' || rel.front() == '\\')) rel.remove_prefix(1);
        while (!rel.empty() && (rel.back() == '/' || rel.back() == '\\')) rel.remove_suffix(1);
        if (!rel.empty()) {
            uri.push_back('/');
            for (char c : rel) uri.push_back(c == '\\' ? '/' : c);
        }
        return uri;
    }

    PulseStatus RegisterMount(const PulseVfsMountDesc& desc, MountHandle& out_handle) const {
        out_handle.Reset();
        if (!api_.register_mount) return PULSE_ERROR_UNAVAILABLE;
        PulseVfsMountHandle raw_handle = 0;
        const PulseStatus st = api_.register_mount(api_.context, &desc, &raw_handle);
        if (st == PULSE_OK && raw_handle != 0) {
            out_handle.api_ = api_;
            out_handle.handle_ = raw_handle;
        }
        return st;
    }

    PulseStatus RegisterProvider(std::string_view mount_id,
                                 std::string_view display_name,
                                 VfsProviderCallbacks callbacks,
                                 MountHandle& out_handle) const {
        out_handle.Reset();
        if (!api_.register_mount) return PULSE_ERROR_UNAVAILABLE;
        auto slot = std::make_shared<ProviderSlot>();
        slot->cb = std::move(callbacks);
        PulseVfsMountDesc desc{};
        desc.size = sizeof(desc);
        desc.capabilities = slot->cb.capabilities;
        desc.mount_id = View(mount_id);
        desc.display_name = View(display_name);
        desc.icon_glyph = View(slot->cb.icon_glyph);
        desc.list_dir = slot->cb.list_dir ? ProviderSlot::ListDir : nullptr;
        desc.extract_file = slot->cb.extract_file ? ProviderSlot::ExtractFile : nullptr;
        desc.stat = slot->cb.stat ? ProviderSlot::Stat : nullptr;
        desc.user = slot.get();
        desc.extract_items = slot->cb.extract_items ? ProviderSlot::ExtractItems : nullptr;
        desc.modify = slot->cb.modify ? ProviderSlot::Modify : nullptr;
        PulseVfsMountHandle raw_handle = 0;
        const PulseStatus st = api_.register_mount(api_.context, &desc, &raw_handle);
        if (st == PULSE_OK && raw_handle != 0) {
            out_handle.api_ = api_;
            out_handle.handle_ = raw_handle;
            out_handle.slot_ = std::move(slot);
        }
        return st;
    }

    std::optional<MountHandle> RegisterProvider(std::string_view mount_id,
                                              std::string_view display_name,
                                              VfsProviderCallbacks callbacks,
                                              PulseStatus* out_status = nullptr) const {
        MountHandle handle;
        const PulseStatus st = RegisterProvider(mount_id, display_name, std::move(callbacks), handle);
        if (out_status) *out_status = st;
        if (st == PULSE_OK) return handle;
        return std::nullopt;
    }

    PulseStatus UnregisterMount(PulseVfsMountHandle handle) const noexcept {
        if (!api_.unregister_mount) return PULSE_ERROR_UNAVAILABLE;
        return api_.unregister_mount(api_.context, handle);
    }

    PulseStatus InvalidatePath(PulseVfsMountHandle handle, std::string_view rel_path = {}) const noexcept {
        if (!api_.invalidate_path) return PULSE_ERROR_UNAVAILABLE;
        return api_.invalidate_path(api_.context, handle, View(rel_path));
    }

    PulseStatus OpenPath(PulseVfsMountHandle handle,
                         std::string_view rel_path = {},
                         uint32_t mode = PULSE_VFS_OPEN_CURRENT_TAB) const noexcept {
        if (!api_.open_path) return PULSE_ERROR_UNAVAILABLE;
        return api_.open_path(api_.context, handle, View(rel_path), mode);
    }
};
// pulse.columns: one listed item handed to a column or badge callback.
struct ColumnItem {
    std::string_view path;          // full UTF-8 path, valid during the call
    bool is_dir = false;
    uint64_t size_bytes = 0;
    int64_t modified_unix_ms = 0;   // <= 0 when unknown
};

// Text of one Details cell; empty text = blank cell.
struct ColumnText {
    std::string text;
    uint32_t tone = PULSE_COLUMN_TONE_DEFAULT;
};

// Badge on one item's icon; empty icon = no badge.
struct BadgeIcon {
    std::string icon;               // "lock", "star", "my-icon", "<package>/<name>", "#<id>"
    uint32_t tone = PULSE_COLUMN_TONE_DEFAULT;
};

struct ColumnSpec {
    std::string id;
    std::string title;
    std::string extensions;         // optional ".jpg;.png"
    uint32_t width_dip = 0;         // 0 = 120
    uint32_t align = PULSE_COLUMN_ALIGN_START;
    uint32_t applies_to = 0;        // PULSE_COLUMN_APPLIES_*; 0 = files and folders
};

// Called on a host worker thread, item by item; return std::nullopt to leave it blank.
using ColumnTextFn = std::function<std::optional<ColumnText>(const ColumnItem& item)>;
using BadgeIconFn = std::function<std::optional<BadgeIcon>(const ColumnItem& item)>;

class ColumnsClient {
    PulseColumnsV1 api_{};

    struct ProviderSlot {
        ColumnTextFn text;
        BadgeIconFn badge;

        static PulseStatus PULSE_CALL Query(void* user, const PulseColumnBatch* batch) noexcept {
            try {
                if (!user || !batch || batch->size < sizeof(PulseColumnBatch) || (batch->count && !batch->items))
                    return PULSE_ERROR_ARGUMENT;
                auto* slot = static_cast<ProviderSlot*>(user);
                for (uint32_t i = 0; i < batch->count; ++i) {
                    if (batch->is_cancelled && batch->is_cancelled(batch->context)) break;
                    const PulseColumnItem& raw = batch->items[i];
                    ColumnItem item;
                    item.path = std::string_view(raw.path.data ? raw.path.data : "", raw.path.length);
                    item.is_dir = (raw.flags & PULSE_COLUMN_ITEM_DIRECTORY) != 0;
                    item.size_bytes = raw.size_bytes;
                    item.modified_unix_ms = raw.modified_unix_ms;
                    if (slot->text) {
                        const std::optional<ColumnText> value = slot->text(item);
                        if (!value || !batch->set_text) continue;
                        PulseColumnValue out{sizeof(PulseColumnValue), value->tone, View(value->text)};
                        (void)batch->set_text(batch->context, i, &out);
                    } else if (slot->badge) {
                        const std::optional<BadgeIcon> value = slot->badge(item);
                        if (!value || !batch->set_badge) continue;
                        PulseBadgeValue out{sizeof(PulseBadgeValue), value->tone, View(value->icon)};
                        (void)batch->set_badge(batch->context, i, &out);
                    }
                }
                return PULSE_OK;
            } catch (...) {
                return PULSE_ERROR_STATE;
            }
        }
    };

public:
    // Owns one registered column or badge; unregisters when reset or destroyed.
    class Registration {
        friend class ColumnsClient;
        PulseColumnsV1 api_{};
        PulseColumnHandle handle_ = 0;
        std::unique_ptr<ProviderSlot> slot_;

    public:
        Registration() = default;
        ~Registration() { Reset(); }
        Registration(const Registration&) = delete;
        Registration& operator=(const Registration&) = delete;
        Registration(Registration&& other) noexcept
            : api_(other.api_), handle_(std::exchange(other.handle_, 0)), slot_(std::move(other.slot_)) {}
        Registration& operator=(Registration&& other) noexcept {
            if (this != &other) {
                Reset();
                api_ = other.api_;
                handle_ = std::exchange(other.handle_, 0);
                slot_ = std::move(other.slot_);
            }
            return *this;
        }

        // Unregister waits for a running callback, so the slot is free to go.
        void Reset() noexcept {
            const auto h = std::exchange(handle_, 0);
            if (h != 0 && api_.unregister) (void)api_.unregister(api_.context, h);
            api_ = {};
            slot_.reset();
        }

        bool Active() const noexcept { return handle_ != 0; }
        PulseColumnHandle Raw() const noexcept { return handle_; }

        PulseStatus SetTitle(std::string_view title) const noexcept {
            if (handle_ == 0 || !api_.set_title) return PULSE_ERROR_STATE;
            return api_.set_title(api_.context, handle_, View(title));
        }
        // Empty path: ask again for every item (e.g. after a setting changed).
        PulseStatus Invalidate(std::string_view path = {}) const noexcept {
            if (handle_ == 0 || !api_.invalidate) return PULSE_ERROR_STATE;
            return api_.invalidate(api_.context, handle_, View(path));
        }
    };

    ColumnsClient() = default;
    explicit ColumnsClient(const PulseColumnsV1& api) noexcept : api_(api) {}

    PulseStatus Attach(const Host& host) noexcept {
        return host.Query(PULSE_COLUMNS_INTERFACE_NAME, PULSE_COLUMNS_VERSION_1, api_);
    }
    static std::optional<ColumnsClient> TryQuery(const Host& host) noexcept {
        ColumnsClient client;
        if (client.Attach(host) == PULSE_OK) return client;
        return std::nullopt;
    }

    bool Available() const noexcept { return api_.register_column != nullptr; }
    const PulseColumnsV1& Table() const noexcept { return api_; }

    PulseStatus AddColumn(const ColumnSpec& spec, ColumnTextFn text, Registration& out) const {
        out.Reset();
        if (!api_.register_column) return PULSE_ERROR_UNAVAILABLE;
        if (!text) return PULSE_ERROR_ARGUMENT;
        auto slot = std::make_unique<ProviderSlot>();
        slot->text = std::move(text);
        PulseColumnDesc desc{};
        desc.size = sizeof(desc);
        desc.applies_to = spec.applies_to;
        desc.id = View(spec.id);
        desc.title = View(spec.title);
        desc.extensions = View(spec.extensions);
        desc.width_dip = spec.width_dip;
        desc.align = spec.align;
        desc.query = ProviderSlot::Query;
        desc.user = slot.get();
        PulseColumnHandle raw = 0;
        const PulseStatus st = api_.register_column(api_.context, &desc, &raw);
        if (st == PULSE_OK && raw != 0) {
            out.api_ = api_;
            out.handle_ = raw;
            out.slot_ = std::move(slot);
        }
        return st;
    }

    // width_dip and align do not apply to badges.
    PulseStatus AddBadge(const ColumnSpec& spec, BadgeIconFn badge, Registration& out) const {
        out.Reset();
        if (!api_.register_badge) return PULSE_ERROR_UNAVAILABLE;
        if (!badge) return PULSE_ERROR_ARGUMENT;
        auto slot = std::make_unique<ProviderSlot>();
        slot->badge = std::move(badge);
        PulseBadgeDesc desc{};
        desc.size = sizeof(desc);
        desc.applies_to = spec.applies_to;
        desc.id = View(spec.id);
        desc.title = View(spec.title);
        desc.extensions = View(spec.extensions);
        desc.query = ProviderSlot::Query;
        desc.user = slot.get();
        PulseColumnHandle raw = 0;
        const PulseStatus st = api_.register_badge(api_.context, &desc, &raw);
        if (st == PULSE_OK && raw != 0) {
            out.api_ = api_;
            out.handle_ = raw;
            out.slot_ = std::move(slot);
        }
        return st;
    }

    std::optional<Registration> AddColumn(const ColumnSpec& spec, ColumnTextFn text,
                                          PulseStatus* out_status = nullptr) const {
        Registration reg;
        const PulseStatus st = AddColumn(spec, std::move(text), reg);
        if (out_status) *out_status = st;
        if (st == PULSE_OK) return reg;
        return std::nullopt;
    }
    std::optional<Registration> AddBadge(const ColumnSpec& spec, BadgeIconFn badge,
                                         PulseStatus* out_status = nullptr) const {
        Registration reg;
        const PulseStatus st = AddBadge(spec, std::move(badge), reg);
        if (out_status) *out_status = st;
        if (st == PULSE_OK) return reg;
        return std::nullopt;
    }
};

// ---- Status bar items (pulse.statusbar) ----
struct StatusItem {
    std::string id;                 // unique within the package
    std::string text;               // short, e.g. "3 jobs"; text or icon is required
    std::string icon;               // "sync", "my-icon", "<package>/<name>", "#<id>"
    std::string tooltip;
    uint32_t tone = PULSE_STATUS_TONE_DEFAULT;
    std::function<void()> on_click; // UI thread; empty = a plain label
};

class StatusBarClient {
    PulseStatusBarV1 api_{};

    struct ItemSlot {
        std::function<void()> click;
        static void PULSE_CALL Click(void* user, PulseStatusItemHandle) noexcept {
            try {
                auto* slot = static_cast<ItemSlot*>(user);
                if (!slot || !slot->click) return;
                const auto click = slot->click;   // a copy: the callback may update its own item
                click();
            } catch (...) {}
        }
    };

    static PulseStatusItemDesc Desc(const StatusItem& spec, ItemSlot* slot) noexcept {
        PulseStatusItemDesc desc{};
        desc.size = sizeof(desc);
        desc.tone = spec.tone;
        desc.id = View(spec.id);
        desc.text = View(spec.text);
        desc.icon = View(spec.icon);
        desc.tooltip = View(spec.tooltip);
        desc.on_click = spec.on_click ? ItemSlot::Click : nullptr;
        desc.user = slot;
        return desc;
    }

public:
    // Owns one status-bar item; removes it when reset or destroyed.
    class Item {
        friend class StatusBarClient;
        PulseStatusBarV1 api_{};
        PulseStatusItemHandle handle_ = 0;
        std::unique_ptr<ItemSlot> slot_;

    public:
        Item() = default;
        ~Item() { Reset(); }
        Item(const Item&) = delete;
        Item& operator=(const Item&) = delete;
        Item(Item&& other) noexcept
            : api_(other.api_), handle_(std::exchange(other.handle_, 0)), slot_(std::move(other.slot_)) {}
        Item& operator=(Item&& other) noexcept {
            if (this != &other) {
                Reset();
                api_ = other.api_;
                handle_ = std::exchange(other.handle_, 0);
                slot_ = std::move(other.slot_);
            }
            return *this;
        }

        // remove_item waits for a running click callback, so the slot is free to go.
        void Reset() noexcept {
            const auto h = std::exchange(handle_, 0);
            if (h != 0 && api_.remove_item) (void)api_.remove_item(api_.context, h);
            api_ = {};
            slot_.reset();
        }

        bool Active() const noexcept { return handle_ != 0; }
        PulseStatusItemHandle Raw() const noexcept { return handle_; }

        // Replaces text, icon, tooltip, tone and the click callback; spec.id may be empty.
        PulseStatus Update(const StatusItem& spec) {
            if (handle_ == 0 || !api_.update_item || !slot_) return PULSE_ERROR_STATE;
            auto next = spec.on_click;
            const PulseStatusItemDesc desc = Desc(spec, slot_.get());
            const PulseStatus st = api_.update_item(api_.context, handle_, &desc);
            if (st == PULSE_OK) slot_->click = std::move(next);
            return st;
        }
        PulseStatus SetVisible(bool visible) const noexcept {
            if (handle_ == 0 || !api_.set_visible) return PULSE_ERROR_STATE;
            return api_.set_visible(api_.context, handle_, visible ? 1u : 0u);
        }
    };

    StatusBarClient() = default;
    explicit StatusBarClient(const PulseStatusBarV1& api) noexcept : api_(api) {}

    PulseStatus Attach(const Host& host) noexcept {
        return host.Query(PULSE_STATUSBAR_INTERFACE_NAME, PULSE_STATUSBAR_VERSION_1, api_);
    }
    static std::optional<StatusBarClient> TryQuery(const Host& host) noexcept {
        StatusBarClient client;
        if (client.Attach(host) == PULSE_OK) return client;
        return std::nullopt;
    }

    bool Available() const noexcept { return api_.add_item != nullptr; }
    const PulseStatusBarV1& Table() const noexcept { return api_; }

    PulseStatus Add(const StatusItem& spec, Item& out) const {
        out.Reset();
        if (!api_.add_item) return PULSE_ERROR_UNAVAILABLE;
        auto slot = std::make_unique<ItemSlot>();
        slot->click = spec.on_click;
        const PulseStatusItemDesc desc = Desc(spec, slot.get());
        PulseStatusItemHandle raw = 0;
        const PulseStatus st = api_.add_item(api_.context, &desc, &raw);
        if (st == PULSE_OK && raw != 0) {
            out.api_ = api_;
            out.handle_ = raw;
            out.slot_ = std::move(slot);
        }
        return st;
    }
    std::optional<Item> Add(const StatusItem& spec, PulseStatus* out_status = nullptr) const {
        Item item;
        const PulseStatus st = Add(spec, item);
        if (out_status) *out_status = st;
        if (st == PULSE_OK) return item;
        return std::nullopt;
    }
};

// ---- Folder and selection changes (pulse.context) ----
// An owned copy of a context snapshot.
struct FolderContext {
    std::string directory;          // UTF-8; empty while nothing is loaded
    std::string selection_name;     // the single selected entry's name; empty for 0 or 2+
    std::string view_mode;
    uint64_t selection_count = 0;
    uint64_t revision = 0, navigation_revision = 0, selection_revision = 0;
    uint32_t changes = 0;           // PULSE_CONTEXT_* bits of this delivery
    bool loading = false;
    bool FolderChanged() const noexcept { return (changes & (PULSE_CONTEXT_INITIAL | PULSE_CONTEXT_NAVIGATION)) != 0; }
    bool SelectionChanged() const noexcept { return (changes & (PULSE_CONTEXT_INITIAL | PULSE_CONTEXT_SELECTION)) != 0; }
    static FolderContext Copy(const PulseContextSnapshot& raw) {
        auto str = [](PulseStringView v) { return std::string(v.data ? v.data : "", v.data ? v.length : 0); };
        FolderContext out;
        out.directory = str(raw.directory);
        out.selection_name = str(raw.selection_name);
        out.view_mode = str(raw.view_mode);
        out.selection_count = raw.selection_count;
        out.revision = raw.revision;
        out.navigation_revision = raw.navigation_revision;
        out.selection_revision = raw.selection_revision;
        out.changes = raw.changes;
        out.loading = raw.loading != 0;
        return out;
    }
};
using FolderContextFn = std::function<void(const FolderContext&)>;

class ContextClient {
    PulseContextV1 api_{};

    struct Slot {
        FolderContextFn fn;
        static void PULSE_CALL Call(void* user, const PulseContextSnapshot* raw) noexcept {
            try {
                auto* slot = static_cast<Slot*>(user);
                if (!slot || !slot->fn || !raw || raw->size < sizeof(PulseContextSnapshot)) return;
                const auto fn = slot->fn;   // a copy: the callback may drop its own subscription
                fn(FolderContext::Copy(*raw));
            } catch (...) {}
        }
    };

public:
    // Owns one subscription; unsubscribes when reset or destroyed.
    class Subscription {
        friend class ContextClient;
        PulseContextV1 api_{};
        PulseContextSubscriptionId id_ = 0;
        std::unique_ptr<Slot> slot_;

    public:
        Subscription() = default;
        ~Subscription() { Reset(); }
        Subscription(const Subscription&) = delete;
        Subscription& operator=(const Subscription&) = delete;
        Subscription(Subscription&& other) noexcept
            : api_(other.api_), id_(std::exchange(other.id_, 0)), slot_(std::move(other.slot_)) {}
        Subscription& operator=(Subscription&& other) noexcept {
            if (this != &other) {
                Reset();
                api_ = other.api_;
                id_ = std::exchange(other.id_, 0);
                slot_ = std::move(other.slot_);
            }
            return *this;
        }
        void Reset() noexcept {
            const auto id = std::exchange(id_, 0);
            if (id != 0 && api_.unsubscribe) (void)api_.unsubscribe(api_.context, id);
            api_ = {};
            slot_.reset();
        }
        bool Active() const noexcept { return id_ != 0; }
    };

    ContextClient() = default;
    explicit ContextClient(const PulseContextV1& api) noexcept : api_(api) {}

    PulseStatus Attach(const Host& host) noexcept {
        return host.Query(PULSE_CONTEXT_INTERFACE_NAME, PULSE_CONTEXT_VERSION_1, api_);
    }
    static std::optional<ContextClient> TryQuery(const Host& host) noexcept {
        ContextClient client;
        if (client.Attach(host) == PULSE_OK) return client;
        return std::nullopt;
    }
    bool Available() const noexcept { return api_.subscribe != nullptr; }
    const PulseContextV1& Table() const noexcept { return api_; }

    // The latest loaded context, synchronously.
    PulseStatus Get(FolderContext& out) const {
        if (!api_.get) return PULSE_ERROR_UNAVAILABLE;
        struct Capture { FolderContext* out; bool got; } capture{&out, false};
        const PulseStatus st = api_.get(api_.context, [](void* user, const PulseContextSnapshot* raw) noexcept {
            try {
                auto* c = static_cast<Capture*>(user);
                if (!raw || raw->size < sizeof(PulseContextSnapshot)) return;
                *c->out = FolderContext::Copy(*raw);
                c->got = true;
            } catch (...) {}
        }, &capture);
        return st == PULSE_OK && !capture.got ? PULSE_ERROR_STATE : st;
    }

    // fn runs on the UI thread after navigation, selection or view changes (coalesced at
    // the host's session tick); the first delivery carries PULSE_CONTEXT_INITIAL.
    PulseStatus Subscribe(FolderContextFn fn, Subscription& out) const {
        out.Reset();
        if (!api_.subscribe) return PULSE_ERROR_UNAVAILABLE;
        if (!fn) return PULSE_ERROR_ARGUMENT;
        auto slot = std::make_unique<Slot>();
        slot->fn = std::move(fn);
        PulseContextSubscriptionId id = 0;
        const PulseStatus st = api_.subscribe(api_.context, Slot::Call, slot.get(), &id);
        if (st == PULSE_OK && id != 0) {
            out.api_ = api_;
            out.id_ = id;
            out.slot_ = std::move(slot);
        }
        return st;
    }
    std::optional<Subscription> Subscribe(FolderContextFn fn, PulseStatus* out_status = nullptr) const {
        Subscription sub;
        const PulseStatus st = Subscribe(std::move(fn), sub);
        if (out_status) *out_status = st;
        if (st == PULSE_OK) return sub;
        return std::nullopt;
    }
};

// ---- Finished file operations (pulse.fileevents) ----
struct FileEventItem {
    std::string source;       // empty for CREATE_*
    std::string destination;  // empty for RECYCLE / DELETE / EMPTY_RECYCLE
};
struct FileEvent {
    uint32_t kind = 0;        // PULSE_FILE_EVENT_*
    uint64_t sequence = 0;
    std::vector<FileEventItem> items;
    uint32_t total_count = 0;
    bool Truncated() const noexcept { return total_count > items.size(); }
    static FileEvent Copy(const PulseFileEvent& raw) {
        auto str = [](PulseStringView v) { return std::string(v.data ? v.data : "", v.data ? v.length : 0); };
        FileEvent out;
        out.kind = raw.kind;
        out.sequence = raw.sequence;
        out.total_count = raw.total_count;
        if (raw.items) {
            out.items.reserve(raw.item_count);
            for (uint32_t i = 0; i < raw.item_count; ++i)
                out.items.push_back({str(raw.items[i].source), str(raw.items[i].destination)});
        }
        return out;
    }
};
using FileEventFn = std::function<void(const FileEvent&)>;

class FileEventsClient {
    PulseFileEventsV1 api_{};

    struct Slot {
        FileEventFn fn;
        static void PULSE_CALL Call(void* user, const PulseFileEvent* raw) noexcept {
            try {
                auto* slot = static_cast<Slot*>(user);
                if (!slot || !slot->fn || !raw || raw->size < sizeof(PulseFileEvent)) return;
                const auto fn = slot->fn;
                fn(FileEvent::Copy(*raw));
            } catch (...) {}
        }
    };

public:
    // Owns one subscription; unsubscribes when reset or destroyed.
    class Subscription {
        friend class FileEventsClient;
        PulseFileEventsV1 api_{};
        PulseFileEventSubscription id_ = 0;
        std::unique_ptr<Slot> slot_;

    public:
        Subscription() = default;
        ~Subscription() { Reset(); }
        Subscription(const Subscription&) = delete;
        Subscription& operator=(const Subscription&) = delete;
        Subscription(Subscription&& other) noexcept
            : api_(other.api_), id_(std::exchange(other.id_, 0)), slot_(std::move(other.slot_)) {}
        Subscription& operator=(Subscription&& other) noexcept {
            if (this != &other) {
                Reset();
                api_ = other.api_;
                id_ = std::exchange(other.id_, 0);
                slot_ = std::move(other.slot_);
            }
            return *this;
        }
        void Reset() noexcept {
            const auto id = std::exchange(id_, 0);
            if (id != 0 && api_.unsubscribe) (void)api_.unsubscribe(api_.context, id);
            api_ = {};
            slot_.reset();
        }
        bool Active() const noexcept { return id_ != 0; }
    };

    FileEventsClient() = default;
    explicit FileEventsClient(const PulseFileEventsV1& api) noexcept : api_(api) {}

    PulseStatus Attach(const Host& host) noexcept {
        return host.Query(PULSE_FILEEVENTS_INTERFACE_NAME, PULSE_FILEEVENTS_VERSION_1, api_);
    }
    static std::optional<FileEventsClient> TryQuery(const Host& host) noexcept {
        FileEventsClient client;
        if (client.Attach(host) == PULSE_OK) return client;
        return std::nullopt;
    }
    bool Available() const noexcept { return api_.subscribe != nullptr; }
    const PulseFileEventsV1& Table() const noexcept { return api_; }

    // kinds: PULSE_FILE_EVENT_MASK(...) bits, 0 = every kind. fn runs on the UI thread.
    PulseStatus Subscribe(uint32_t kinds, FileEventFn fn, Subscription& out) const {
        out.Reset();
        if (!api_.subscribe) return PULSE_ERROR_UNAVAILABLE;
        if (!fn) return PULSE_ERROR_ARGUMENT;
        auto slot = std::make_unique<Slot>();
        slot->fn = std::move(fn);
        PulseFileEventSubscription id = 0;
        const PulseStatus st = api_.subscribe(api_.context, kinds, Slot::Call, slot.get(), &id);
        if (st == PULSE_OK && id != 0) {
            out.api_ = api_;
            out.id_ = id;
            out.slot_ = std::move(slot);
        }
        return st;
    }
    std::optional<Subscription> Subscribe(uint32_t kinds, FileEventFn fn, PulseStatus* out_status = nullptr) const {
        Subscription sub;
        const PulseStatus st = Subscribe(kinds, std::move(fn), sub);
        if (out_status) *out_status = st;
        if (st == PULSE_OK) return sub;
        return std::nullopt;
    }
};
// pulse.preview: one archive row of a PublishArchive result.
struct PreviewArchiveItem {
    std::string path;               // UTF-8, "/"-separated inside the archive
    uint64_t size_bytes = 0, packed_bytes = 0;
    uint64_t modified_ticks = 0;    // Windows FILETIME ticks, 0 = unknown
    bool directory = false, encrypted = false;
};

// One preview or thumbnail request, handed to a PreviewFn on a host worker thread.
// Valid only during the call; publish at most one result, then return.
class PreviewContext {
    const PulsePreviewRequest& raw_;
public:
    explicit PreviewContext(const PulsePreviewRequest& raw) noexcept : raw_(raw) {}
    std::string_view Path() const noexcept { return {raw_.path.data ? raw_.path.data : "", raw_.path.length}; }
    uint64_t Modified() const noexcept { return raw_.modified; }        // FILETIME ticks
    uint64_t SizeBytes() const noexcept { return raw_.size_bytes; }
    uint32_t Pixels() const noexcept { return raw_.pixels; }            // wanted longest edge
    // A grid thumbnail (only a bitmap is used; keep it quick) rather than the preview pane.
    bool Thumbnail() const noexcept {
        return raw_.size >= offsetof(PulsePreviewRequest, purpose) + sizeof(uint32_t) &&
               raw_.purpose == PULSE_PREVIEW_PURPOSE_THUMBNAIL;
    }
    // Poll in loops; return promptly once true (the output would be discarded).
    bool Cancelled() const noexcept { return raw_.cancelled && raw_.cancelled(raw_.context) != 0; }

    // Premultiplied BGRA, top-down. stride 0 = width * 4. Max 4096 x 4096 and 16 MiB.
    PulseStatus PublishBitmap(uint32_t width, uint32_t height, const uint8_t* pbgra, uint32_t stride = 0) const noexcept {
        if (!raw_.publish) return PULSE_ERROR_UNAVAILABLE;
        if (!stride) stride = width * 4;
        PulsePreviewResult result{};
        result.size = sizeof(result);
        result.kind = PULSE_PREVIEW_BITMAP;
        result.width = width; result.height = height; result.stride = stride;
        result.pixels = pbgra;
        result.byte_count = static_cast<uint32_t>(static_cast<uint64_t>(stride) * height);
        return raw_.publish(raw_.context, &result);
    }
    // format: short label such as "7z" or "ZIP". Up to 4096 rows.
    PulseStatus PublishArchive(std::string_view format, const std::vector<PreviewArchiveItem>& items,
                               bool incomplete = false) const {
        if (!raw_.publish) return PULSE_ERROR_UNAVAILABLE;
        std::vector<PulsePreviewArchiveEntry> rows;
        rows.reserve(items.size());
        for (const auto& item : items) {
            PulsePreviewArchiveEntry row{};
            row.path = View(item.path);
            row.size_bytes = item.size_bytes; row.packed_bytes = item.packed_bytes;
            row.modified_ticks = item.modified_ticks;
            row.flags = (item.directory ? PULSE_PREVIEW_ENTRY_DIRECTORY : 0u) |
                        (item.encrypted ? PULSE_PREVIEW_ENTRY_ENCRYPTED : 0u);
            rows.push_back(row);
        }
        PulsePreviewResult result{};
        result.size = sizeof(result);
        result.kind = PULSE_PREVIEW_ARCHIVE;
        result.format = View(format);
        result.incomplete = incomplete ? 1u : 0u;
        result.entries = rows.empty() ? nullptr : rows.data();
        result.entry_count = static_cast<uint32_t>(rows.size());
        return raw_.publish(raw_.context, &result);
    }
    // A pulse.ui.model tree (local state and actions only; no native command callbacks).
    PulseStatus PublishModel(const PulseUiModelTree& tree) const noexcept {
        if (!raw_.publish) return PULSE_ERROR_UNAVAILABLE;
        PulsePreviewResult result{};
        result.size = sizeof(result);
        result.kind = PULSE_PREVIEW_UI_MODEL;
        result.model = &tree;
        return raw_.publish(raw_.context, &result);
    }
    PulseStatus PublishModel(const ModelTree& tree) const { return PublishModel(tree.Build()); }
    const PulsePreviewRequest& Raw() const noexcept { return raw_; }
};

// Return PULSE_OK after one successful Publish*, or an error (nothing to show).
using PreviewFn = std::function<PulseStatus(const PreviewContext& request)>;

class PreviewClient {
    PulsePreviewV1 api_{};

    struct ProviderSlot {
        PreviewFn fn;
        static PulseStatus PULSE_CALL Run(void* user, const PulsePreviewRequest* request) noexcept {
            try {
                if (!user || !request || request->size < offsetof(PulsePreviewRequest, purpose)) return PULSE_ERROR_ARGUMENT;
                auto* slot = static_cast<ProviderSlot*>(user);
                if (!slot->fn) return PULSE_ERROR_UNAVAILABLE;
                return slot->fn(PreviewContext(*request));
            } catch (...) {
                return PULSE_ERROR_STATE;
            }
        }
    };

public:
    // Owns one registered provider; unregisters when reset or destroyed (owner UI thread).
    class Provider {
        friend class PreviewClient;
        PulsePreviewV1 api_{};
        PulsePreviewProviderId id_ = 0;
        std::unique_ptr<ProviderSlot> slot_;

    public:
        Provider() = default;
        ~Provider() { Reset(); }
        Provider(const Provider&) = delete;
        Provider& operator=(const Provider&) = delete;
        Provider(Provider&& other) noexcept
            : api_(other.api_), id_(std::exchange(other.id_, 0)), slot_(std::move(other.slot_)) {}
        Provider& operator=(Provider&& other) noexcept {
            if (this != &other) {
                Reset();
                api_ = other.api_;
                id_ = std::exchange(other.id_, 0);
                slot_ = std::move(other.slot_);
            }
            return *this;
        }

        // The host refuses (STATE) while a callback still runs: retry briefly, then keep the
        // slot alive for good rather than free memory a worker may still be using.
        void Reset() noexcept {
            const auto id = std::exchange(id_, 0);
            if (id != 0 && api_.unregister_provider) {
                PulseStatus st = api_.unregister_provider(api_.context, id);
                for (int i = 0; st == PULSE_ERROR_STATE && i < 40; ++i) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                    st = api_.unregister_provider(api_.context, id);
                }
                if (st == PULSE_ERROR_STATE) (void)slot_.release();
            }
            api_ = {};
            slot_.reset();
        }
        bool Active() const noexcept { return id_ != 0; }
        PulsePreviewProviderId Raw() const noexcept { return id_; }
    };

    PreviewClient() = default;
    explicit PreviewClient(const PulsePreviewV1& api) noexcept : api_(api) {}

    PulseStatus Attach(const Host& host) noexcept {
        return host.Query(PULSE_PREVIEW_INTERFACE_NAME, PULSE_PREVIEW_VERSION_1, api_);
    }
    static std::optional<PreviewClient> TryQuery(const Host& host) noexcept {
        PreviewClient client;
        if (client.Attach(host) == PULSE_OK) return client;
        return std::nullopt;
    }
    bool Available() const noexcept { return api_.register_provider != nullptr; }
    const PulsePreviewV1& Table() const noexcept { return api_; }

    // extension: lowercase dotted ASCII such as ".foo". flags: PULSE_PREVIEW_PROVIDER_THUMBNAIL
    // to also make grid thumbnails. One provider per extension per window; up to 16.
    PulseStatus Register(std::string_view extension, PreviewFn fn, uint32_t flags, Provider& out) const {
        out.Reset();
        if (!api_.register_provider) return PULSE_ERROR_UNAVAILABLE;
        if (!fn) return PULSE_ERROR_ARGUMENT;
        auto slot = std::make_unique<ProviderSlot>();
        slot->fn = std::move(fn);
        PulsePreviewProvider desc{};
        desc.size = sizeof(desc);
        desc.extension = View(extension);
        desc.callback = ProviderSlot::Run;
        desc.user = slot.get();
        desc.flags = flags;
        PulsePreviewProviderId id = 0;
        const PulseStatus st = api_.register_provider(api_.context, &desc, &id);
        if (st == PULSE_OK && id != 0) {
            out.api_ = api_;
            out.id_ = id;
            out.slot_ = std::move(slot);
        }
        return st;
    }
};
}
