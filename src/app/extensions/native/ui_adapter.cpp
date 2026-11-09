#include "ui_adapter.h"
#include "../../../common/json_value.h"
#include "../../../ui/extension_panel.h"
#include "ui_model.h"
#include "ui_data_adapter.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <map>
#include <set>

namespace pulse::extensions::native {
namespace {
std::atomic<uint64_t> next_identity{0};
std::map<std::pair<HWND, uint32_t>, const void *> mount_slots;
std::set<HWND> legacy_sidebars;
struct PauseData {
    UiDataAdapter &data;
    explicit PauseData(UiDataAdapter &value) : data(value) { data.Pause(true); }
    ~PauseData() { data.Pause(false); }
};
bool Text(PulseStringView input, std::string &output, size_t limit = 1024) {
    if (input.length > limit || (input.length && !input.data))
        return false;
    output.assign(input.data ? input.data : "", input.length);
    return output.find('\0') == std::string::npos &&
           (output.empty() || MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, output.data(),
                                                  static_cast<int>(output.size()), nullptr, 0));
}
bool Key(PulseStringView input, std::string &output) {
    return Text(input, output, 64) && !output.empty() && std::all_of(output.begin(), output.end(), [](char c) {
               return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' ||
                      c == '_' || c == '.';
           });
}
bool States(const PulseUiState *input, uint32_t count, std::vector<UiStateEntry> &result) {
    if (count > 32 || (count && !input))
        return false;
    std::set<std::string> keys;
    for (uint32_t i = 0; i < count; ++i) {
        UiStateEntry entry;
        if (!Key(input[i].key, entry.key) || !Text(input[i].value, entry.value) || !keys.insert(entry.key).second)
            return false;
        result.push_back(std::move(entry));
    }
    return true;
}
} // namespace
struct UiAdapter::State : std::enable_shared_from_this<State> {
    HWND owner = nullptr;
    DWORD thread = GetCurrentThreadId();
    bool dark = false, stopped = false, embedded = false;
    D2D1_COLOR_F accent{};
    float scale = 1;
    std::function<bool()> alive;
    ui::ExtensionPanel panel;
    std::unique_ptr<UiDataAdapter> data;
    uint64_t panel_id = 0;
    size_t callbacks = 0;
    std::set<std::string> buttons;
    struct Subscription {
        uint64_t id;
        std::string node;
        PulseUiClickFn callback;
        void *user;
    };
    std::map<uint64_t, Subscription> subscriptions;
    struct StateSubscription {
        std::string key;
        uint32_t sources;
        uint64_t after_revision;
        PulseUiStateChangedFn callback;
        void *user;
    };
    std::map<uint64_t, StateSubscription> state_subscriptions;
    struct MountSubscription {
        PulseUiMountChangedFn callback;
        void *user;
        uint64_t delivered_revision = 0;
    };
    std::map<uint64_t, MountSubscription> mount_subscriptions;
    std::function<RECT(uint32_t)> host_bounds;
    std::function<void(bool)> host_sidebar;
    PulseUiMountInfo mount_info{sizeof(PulseUiMountInfo), PULSE_UI_MOUNT_WINDOW, 0, 0, 96, 0, 0, 0, 0};
    bool mount_dispatching = false;
    bool SlotAvailable(uint32_t placement) const {
        if (placement == PULSE_UI_MOUNT_WINDOW) return true;
        if (placement == PULSE_UI_MOUNT_SIDEBAR && legacy_sidebars.contains(owner)) return false;
        const auto found = mount_slots.find({owner, placement});
        return found == mount_slots.end() || found->second == this;
    }
    void ReleaseMount() {
        const auto found = mount_slots.find({owner, mount_info.placement});
        if (found != mount_slots.end() && found->second == this) mount_slots.erase(found);
        if (mount_info.placement == PULSE_UI_MOUNT_SIDEBAR && host_sidebar) host_sidebar(false);
        mount_info.placement = PULSE_UI_MOUNT_WINDOW;
    }
    void CaptureMount(bool force = false) {
        if (!panel.IsOpen()) return;
        RECT client{};
        GetClientRect(panel.Hwnd(), &client);
        const HWND focus = GetFocus();
        const float current_scale = panel.DpiScale();
        const auto dip = [current_scale](LONG pixels) { return static_cast<uint32_t>((std::max)(0L,
            static_cast<LONG>(std::lround(static_cast<float>(pixels) / current_scale)))); };
        auto info = mount_info;
        info.width_dip = dip(client.right - client.left);
        info.height_dip = dip(client.bottom - client.top);
        info.dpi = static_cast<uint32_t>(std::lround(current_scale * 96.0f));
        info.dark = dark ? 1u : 0u;
        info.focused = focus == panel.Hwnd() || IsChild(panel.Hwnd(), focus) ? 1u : 0u;
        if (force || info.width_dip != mount_info.width_dip || info.height_dip != mount_info.height_dip ||
            info.dpi != mount_info.dpi || info.dark != mount_info.dark || info.focused != mount_info.focused)
            ++info.revision;
        mount_info = info;
    }
    PulseStatus Mount(uint32_t placement) {
        if (embedded) return PULSE_ERROR_UNAVAILABLE;
        if (placement > PULSE_UI_MOUNT_WORKSPACE) return PULSE_ERROR_ARGUMENT;
        if (!SlotAvailable(placement)) return PULSE_ERROR_STATE;
        RECT bounds{};
        uint32_t actual = placement;
        if (placement != PULSE_UI_MOUNT_WINDOW) {
            if (!host_bounds || !host_sidebar) return PULSE_ERROR_UNAVAILABLE;
            bounds = host_bounds(placement);
            if (bounds.right <= bounds.left || bounds.bottom <= bounds.top) actual = PULSE_UI_MOUNT_WINDOW;
        }
        const auto previous = mount_info.placement;
        const float move_scale = previous == PULSE_UI_MOUNT_WINDOW && actual == PULSE_UI_MOUNT_WINDOW
            ? panel.DpiScale() : scale;
        // Release the reservation after the HWND move succeeds. The host
        // callback only changes transient renderer state, never preferences.
        if (!panel.Remount(owner, actual == PULSE_UI_MOUNT_WINDOW ? nullptr : &bounds, dark, accent, move_scale))
            return PULSE_ERROR_STATE;
        ReleaseMount();
        mount_info.placement = actual;
        if (actual != PULSE_UI_MOUNT_WINDOW) mount_slots[{owner, actual}] = this;
        if (actual == PULSE_UI_MOUNT_SIDEBAR && host_sidebar) host_sidebar(true);
        // Leaving this panel's sidebar expands the workspace rectangle.
        if (actual == PULSE_UI_MOUNT_WORKSPACE && previous == PULSE_UI_MOUNT_SIDEBAR) {
            bounds = host_bounds(actual);
            panel.UpdateDocked(bounds, dark, accent, scale);
        }
        CaptureMount(previous != actual || placement != actual);
        return PULSE_OK;
    }
    void DispatchMount() {
        if (mount_dispatching || !Panel(panel_id)) return;
        auto lifetime = shared_from_this();
        const auto instance = panel_id;
        const auto info = mount_info;
        std::vector<uint64_t> tokens;
        for (const auto &[id, subscription] : mount_subscriptions) {
            if (subscription.delivered_revision < info.revision) tokens.push_back(id);
        }
        mount_dispatching = true;
        for (const auto token : tokens) {
            if (!Panel(instance) || mount_info.revision != info.revision) break;
            auto found = mount_subscriptions.find(token);
            if (found == mount_subscriptions.end()) continue;
            const auto subscription = found->second;
            found->second.delivered_revision = info.revision;
            ++callbacks;
            try { subscription.callback(subscription.user, instance, &info); } catch (...) {}
            --callbacks;
        }
        mount_dispatching = false;
    }
    uint64_t event_generation = 0;
    void InvalidateStateSubscriptions() {
        ++event_generation;
        state_subscriptions.clear();
    }
    void RefreshStateSubscriptions() {
        // A model update invalidates the in-flight batch, not subscriptions to
        // surviving state keys. Reset the watermark even for an identical tree.
        ++event_generation;
        const auto revision = panel.TreeStateRevision();
        for (auto it = state_subscriptions.begin(); it != state_subscriptions.end();) {
            if (!it->second.key.empty() && !panel.TreeStateValue(it->second.key)) {
                it = state_subscriptions.erase(it);
            } else {
                it->second.after_revision = revision;
                ++it;
            }
        }
    }
    void DispatchState(uint64_t revision, const std::vector<ui::ExtensionStateChange> &changes) {
        auto lifetime = shared_from_this();
        const auto instance = panel_id, epoch = event_generation;
        if (!Panel(instance))
            return;
        std::vector<uint64_t> tokens;
        for (const auto &[id, subscription] : state_subscriptions)
            tokens.push_back(id);
        for (auto token : tokens) {
            if (!Panel(instance) || epoch != event_generation)
                break;
            auto found = state_subscriptions.find(token);
            if (found == state_subscriptions.end())
                continue;
            const auto subscription = found->second;
            std::vector<PulseUiStateChange> selected;
            for (const auto &change : changes) {
                if (change.revision <= subscription.after_revision || !(change.source_mask & subscription.sources) ||
                    (!subscription.key.empty() && subscription.key != change.key))
                    continue;
                selected.push_back({{change.key.data(), static_cast<uint32_t>(change.key.size())},
                                    {change.value.data(), static_cast<uint32_t>(change.value.size())},
                                    change.revision,
                                    change.source_mask,
                                    0});
            }
            if (selected.empty())
                continue;
            const PulseUiStateBatch batch{revision, selected.data(), static_cast<uint32_t>(selected.size()), 0};
            ++callbacks;
            try {
                subscription.callback(subscription.user, instance, &batch);
            } catch (...) {
            }
            --callbacks;
        }
    }
    bool Valid() const { return GetCurrentThreadId() == thread && !stopped && IsWindow(owner) && alive && alive(); }
    bool Panel(uint64_t id) const { return Valid() && id && id == panel_id && panel.IsOpen(); }
    void Closed() {
        const bool notify_owner = embedded && panel_id != 0;
        ReleaseMount();
        mount_subscriptions.clear();
        panel_id = 0;
        if (data)
            data->ResetBindings();
        subscriptions.clear();
        InvalidateStateSubscriptions();
        buttons.clear();
        if (notify_owner && IsWindow(owner)) InvalidateRect(owner, nullptr, FALSE);
    }
    void Dispatch(const std::string &node) {
        auto lifetime = shared_from_this();
        const std::string stable_node = node;
        if (!Panel(panel_id))
            return;
        const uint64_t instance = panel_id;
        std::vector<uint64_t> tokens;
        for (const auto &[id, subscription] : subscriptions)
            if (subscription.node == stable_node)
                tokens.push_back(id);
        for (uint64_t token : tokens) {
            if (!Panel(instance))
                break;
            auto found = subscriptions.find(token);
            if (found == subscriptions.end())
                continue;
            auto callback = found->second.callback;
            auto user = found->second.user;
            ++callbacks;
            try {
                callback(user, instance, {stable_node.data(), static_cast<uint32_t>(stable_node.size())});
            } catch (...) {
            }
            --callbacks;
        }
    }
    template <class F> static PulseStatus Guard(void *context, F operation) noexcept {
        if (!context)
            return PULSE_ERROR_ARGUMENT;
        try {
            auto state = static_cast<State *>(context)->shared_from_this();
            if (!state->Valid())
                return PULSE_ERROR_STATE;
            return operation(*state);
        } catch (...) {
            return PULSE_ERROR_STATE;
        }
    }
    static PulseStatus Show(State &self, const std::string &caption, UiTree tree, std::set<std::string> buttons,
                            uint64_t *handle) {
        ui::ExtensionPanelSpec spec;
        spec.content_only = self.embedded;
        spec.title = json::Wide(caption);
        spec.tree = std::move(tree);
        auto weak = self.weak_from_this();
        spec.tree_callbacks.on_command = [weak](const std::string &id) {
            if (auto state = weak.lock())
                state->Dispatch(id);
        };
        spec.tree_callbacks.on_state_changed = [weak](uint64_t revision,
                                                      const std::vector<ui::ExtensionStateChange> &changes) {
            if (auto state = weak.lock())
                state->DispatchState(revision, changes);
        };
        spec.tree_callbacks.command_enabled = [weak](const std::string &id) {
            if (auto state = weak.lock())
                return state->Valid() && state->buttons.contains(id);
            return false;
        };
        spec.tree_callbacks.request_data = [weak](const ui::UiDataRequest &request, ui::UiDataCompletion completion) {
            if (auto state = weak.lock())
                return state->data->Request(request, std::move(completion));
            return ui::UiDataCancel{};
        };
        spec.on_closed = [weak] {
            if (auto state = weak.lock())
                state->Closed();
        };
        self.buttons = std::move(buttons);
        const RECT initial_bounds{0, 0, 1, 1};
        const bool shown = self.embedded
            ? self.panel.ShowDocked(self.owner, initial_bounds, std::move(spec), self.dark, self.accent, self.scale, false)
            : self.panel.Show(self.owner, std::move(spec), self.dark, self.accent, self.scale);
        if (!shown) {
            self.Closed();
            return PULSE_ERROR_INITIALIZATION;
        }
        self.panel_id = ++next_identity;
        self.CaptureMount(true);
        *handle = self.panel_id;
        return PULSE_OK;
    }
    static PulseStatus PULSE_CALL CreateModel(void *context, PulseStringView title, const PulseUiModelTree *input,
                                              uint64_t *handle) {
        return Guard(context, [&](State &self) -> PulseStatus {
            if (!handle)
                return PULSE_ERROR_ARGUMENT;
            *handle = 0;
            if (self.panel.IsOpen())
                return PULSE_ERROR_STATE;
            std::string caption, error;
            UiTree tree;
            std::set<std::string> commands;
            if (!Text(title, caption, 128) || caption.empty())
                return PULSE_ERROR_ARGUMENT;
            const auto status = ConvertUiModel(input, tree, commands, error);
            if (status != PULSE_OK)
                return status;
            return Show(self, caption, std::move(tree), std::move(commands), handle);
        });
    }
    static PulseStatus PULSE_CALL UpdateModel(void *context, uint64_t panel, const PulseUiModelTree *input) {
        return Guard(context, [&](State &self) -> PulseStatus {
            if (!self.Panel(panel))
                return PULSE_ERROR_STATE;
            std::string error;
            UiTree tree;
            std::set<std::string> commands;
            const auto status = ConvertUiModel(input, tree, commands, error);
            if (status != PULSE_OK)
                return status;
            const bool updated = [&] { PauseData pause(*self.data); return self.panel.UpdateTree(tree); }();
            if (!updated)
                return PULSE_ERROR_STATE;
            self.data->ResetBindings();
            self.RefreshStateSubscriptions();
            self.buttons = std::move(commands);
            std::erase_if(self.subscriptions,
                          [&](const auto &entry) { return !self.buttons.contains(entry.second.node); });
            self.panel.RefreshEnabled();
            return PULSE_OK;
        });
    }
    static PulseStatus PULSE_CALL Set(void *context, uint64_t panel, const PulseUiState *entries, uint32_t count) {
        return Guard(context, [&](State &self) -> PulseStatus {
            if (!self.Panel(panel))
                return PULSE_ERROR_STATE;
            std::vector<UiStateEntry> changes;
            if (!States(entries, count, changes))
                return PULSE_ERROR_ARGUMENT;
            return self.panel.SetTreeStates(changes) ? PULSE_OK : PULSE_ERROR_ARGUMENT;
        });
    }
    static PulseStatus PULSE_CALL Get(void *context, uint64_t panel, PulseStringView input, char *buffer,
                                      uint32_t capacity, uint32_t *needed) {
        return Guard(context, [&](State &self) -> PulseStatus {
            if (!self.Panel(panel))
                return PULSE_ERROR_STATE;
            std::string key;
            if (!needed || !Key(input, key) || (!buffer && capacity))
                return PULSE_ERROR_ARGUMENT;
            auto value = self.panel.TreeStateValue(key);
            if (!value)
                return PULSE_ERROR_ARGUMENT;
            *needed = static_cast<uint32_t>(value->size());
            if (!buffer && !capacity)
                return PULSE_OK;
            if (capacity < value->size())
                return PULSE_ERROR_ARGUMENT;
            if (!value->empty())
                std::memcpy(buffer, value->data(), value->size());
            return PULSE_OK;
        });
    }
    static PulseStatus PULSE_CALL Subscribe(void *context, uint64_t panel, PulseStringView input,
                                            PulseUiClickFn callback, void *user, uint64_t *token) {
        return Guard(context, [&](State &self) -> PulseStatus {
            if (!self.Panel(panel))
                return PULSE_ERROR_STATE;
            std::string node;
            if (!token || !callback || !Key(input, node) || !self.buttons.contains(node) ||
                self.subscriptions.size() + self.state_subscriptions.size() >= 128)
                return PULSE_ERROR_ARGUMENT;
            const auto id = ++next_identity;
            self.subscriptions.emplace(id, Subscription{id, node, callback, user});
            *token = id;
            return PULSE_OK;
        });
    }
    static PulseStatus PULSE_CALL SubscribeState(void *context, uint64_t panel, PulseStringView input, uint32_t sources,
                                                 PulseUiStateChangedFn callback, void *user, uint64_t *token) {
        if (token)
            *token = 0;
        return Guard(context, [&](State &self) -> PulseStatus {
            if (!self.Panel(panel))
                return PULSE_ERROR_STATE;
            std::string key;
            if (!token || !callback || !sources || (sources & ~static_cast<uint32_t>(PULSE_UI_STATE_ALL)) ||
                !Text(input, key, 64) || (!key.empty() && (!Key(input, key) || !self.panel.TreeStateValue(key))) ||
                self.subscriptions.size() + self.state_subscriptions.size() >= 128)
                return PULSE_ERROR_ARGUMENT;
            const auto id = ++next_identity;
            self.state_subscriptions.emplace(
                id, StateSubscription{key, sources, self.panel.TreeStateRevision(), callback, user});
            *token = id;
            return PULSE_OK;
        });
    }
    static PulseStatus PULSE_CALL Unsubscribe(void *context, uint64_t token) {
        return Guard(context, [&](State &self) -> PulseStatus {
            const bool removed = self.subscriptions.erase(token) != 0;
            const bool state_removed = self.state_subscriptions.erase(token) != 0;
            return (self.mount_subscriptions.erase(token) || state_removed || removed) ? PULSE_OK : PULSE_ERROR_STATE;
        });
    }
    static PulseStatus PULSE_CALL MountSet(void *context, uint64_t panel, uint32_t placement) {
        return Guard(context, [&](State &self) -> PulseStatus {
            return self.Panel(panel) ? self.Mount(placement) : PULSE_ERROR_STATE;
        });
    }
    static PulseStatus PULSE_CALL MountGet(void *context, uint64_t panel, PulseUiMountInfo *info) {
        return Guard(context, [&](State &self) -> PulseStatus {
            if (!self.Panel(panel)) return PULSE_ERROR_STATE;
            if (!info || info->size != sizeof(*info)) return PULSE_ERROR_ARGUMENT;
            self.CaptureMount();
            *info = self.mount_info;
            return PULSE_OK;
        });
    }
    static PulseStatus PULSE_CALL MountSubscribe(void *context, uint64_t panel, PulseUiMountChangedFn callback,
                                                void *user, uint64_t *token) {
        if (token) *token = 0;
        return Guard(context, [&](State &self) -> PulseStatus {
            if (!self.Panel(panel)) return PULSE_ERROR_STATE;
            if (!token || !callback) return PULSE_ERROR_ARGUMENT;
            if (self.mount_subscriptions.size() >= 16) return PULSE_ERROR_STATE;
            const auto id = ++next_identity;
            self.mount_subscriptions.emplace(id, MountSubscription{callback, user, 0});
            *token = id;
            return PULSE_OK;
        });
    }
    static PulseStatus PULSE_CALL MountUnsubscribe(void *context, uint64_t token) {
        return Guard(context, [&](State &self) -> PulseStatus {
            return self.mount_subscriptions.erase(token) ? PULSE_OK : PULSE_ERROR_STATE;
        });
    }
    static PulseStatus PULSE_CALL Close(void *context, uint64_t panel) {
        return Guard(context, [&](State &self) -> PulseStatus {
            if (!self.Panel(panel))
                return PULSE_ERROR_STATE;
            self.Closed();
            self.panel.Close();
            return PULSE_OK;
        });
    }
};
UiAdapter::UiAdapter(HWND owner, bool dark, D2D1_COLOR_F accent, float scale, std::function<bool()> alive)
    : state_(std::make_shared<State>()) {
    state_->owner = owner;
    state_->dark = dark;
    state_->accent = accent;
    state_->scale = scale;
    state_->alive = std::move(alive);
    const auto weak = std::weak_ptr<State>(state_);
    UiDataBackend backend;
    backend.alive = [weak] { auto self = weak.lock(); return self && self->Valid(); };
    backend.valid = [weak](uint64_t panel) { auto self = weak.lock(); return self && self->Panel(panel); };
    backend.node = [weak](const std::string &id) -> std::optional<UiNode> {
        auto self = weak.lock(); return self ? self->panel.TreeDataNode(id) : std::nullopt;
    };
    backend.state = [weak](const std::string &key) -> std::optional<std::string> {
        auto self = weak.lock(); return self ? self->panel.TreeStateValue(key) : std::nullopt;
    };
    backend.commands = [weak] { auto self = weak.lock(); return self ? self->buttons : std::set<std::string>{}; };
    backend.bind = [weak](const std::string &id, const std::optional<UiDataSource> &source) {
        auto self = weak.lock(); return self && self->panel.BindTreeData(id, source);
    };
    backend.request = [weak](const std::string &id, const std::string &operation) {
        auto self = weak.lock(); return self && self->panel.RequestTreeData(id, operation);
    };
    state_->data = std::make_unique<UiDataAdapter>(std::move(backend));
}
UiAdapter::~UiAdapter() { Stop(); }
PulseStatus UiAdapter::QueryEvents(uint32_t version, void *table, uint32_t size) {
    if (version != PULSE_UI_EVENTS_VERSION_1 || size != sizeof(PulseUiEventsV1) || !table)
        return PULSE_ERROR_ABI;
    if (!state_->Valid())
        return PULSE_ERROR_STATE;
    const PulseUiEventsV1 value{sizeof(PulseUiEventsV1), PULSE_UI_EVENTS_VERSION_1, state_.get(), State::SubscribeState,
                                State::Unsubscribe};
    std::memcpy(table, &value, sizeof(value));
    return PULSE_OK;
}
PulseStatus UiAdapter::QueryModel(uint32_t version, void *table, uint32_t size) {
    if (version != PULSE_UI_MODEL_VERSION || size != sizeof(PulseUiModelV1) || !table)
        return PULSE_ERROR_ABI;
    if (!state_->Valid())
        return PULSE_ERROR_STATE;
    PulseUiModelV1 value{sizeof(PulseUiModelV1), PULSE_UI_MODEL_VERSION, state_.get(),
                         State::CreateModel,     State::UpdateModel,     State::Subscribe,
                         State::Set, State::Get, State::Unsubscribe, State::Close};
    std::memcpy(table, &value, sizeof(value));
    return PULSE_OK;
}
PulseStatus UiAdapter::QueryData(uint32_t version, void *table, uint32_t size) {
    if (!state_->Valid())
        return PULSE_ERROR_STATE;
    return state_->data->Query(version, table, size);
}
PulseStatus UiAdapter::QueryMount(uint32_t version, void *table, uint32_t size) {
    if (version != PULSE_UI_MOUNT_VERSION_1 || size != sizeof(PulseUiMountV1) || !table)
        return PULSE_ERROR_ABI;
    if (!state_->Valid()) return PULSE_ERROR_STATE;
    const PulseUiMountV1 value{sizeof(PulseUiMountV1), PULSE_UI_MOUNT_VERSION_1, state_.get(),
        State::MountSet, State::MountGet, State::MountSubscribe, State::MountUnsubscribe};
    std::memcpy(table, &value, sizeof(value));
    return PULSE_OK;
}
void UiAdapter::SyncHostLayout(HWND owner, bool dark, D2D1_COLOR_F accent, float scale,
                              std::function<RECT(uint32_t)> bounds, std::function<void(bool)> sidebar) {
    const auto state = state_;
    if (owner != state->owner || !state->Valid() || !std::isfinite(scale) || scale <= 0) return;
    state->host_bounds = std::move(bounds);
    state->host_sidebar = std::move(sidebar);
    state->dark = dark;
    state->accent = accent;
    state->scale = std::clamp(scale, 0.75f, 4.0f);
    if (!state->Panel(state->panel_id)) return;
    const float appearance_scale = state->mount_info.placement == PULSE_UI_MOUNT_WINDOW
        ? state->panel.DpiScale() : state->scale;
    state->panel.UpdateAppearance(state->dark, state->accent, appearance_scale);
    if (state->mount_info.placement != PULSE_UI_MOUNT_WINDOW) {
        const auto rect = state->host_bounds(state->mount_info.placement);
        if (rect.right <= rect.left || rect.bottom <= rect.top) state->Mount(PULSE_UI_MOUNT_WINDOW);
        else state->panel.UpdateDocked(rect, state->dark, state->accent, state->scale);
    }
    state->CaptureMount();
    state->DispatchMount();
}
bool UiAdapter::IsSidebar() const {
    return state_->Panel(state_->panel_id) && state_->mount_info.placement == PULSE_UI_MOUNT_SIDEBAR;
}
bool UiAdapter::IsWorkspace() const {
    return state_->Panel(state_->panel_id) && state_->mount_info.placement == PULSE_UI_MOUNT_WORKSPACE;
}
bool UiAdapter::SidebarOccupied(HWND owner) { return mount_slots.contains({owner, PULSE_UI_MOUNT_SIDEBAR}); }
void UiAdapter::SetLegacySidebar(HWND owner, bool occupied) {
    if (occupied) legacy_sidebars.insert(owner);
    else legacy_sidebars.erase(owner);
}
bool UiAdapter::Embed() {
    if (GetCurrentThreadId() != state_->thread || state_->stopped || state_->panel.IsOpen()) return false;
    state_->embedded = true; return true;
}
bool UiAdapter::ShowEmbedded(RECT bounds, bool dark, D2D1_COLOR_F accent, float scale) {
    const auto self = state_;
    if (!self->embedded || !self->Panel(self->panel_id) || bounds.right <= bounds.left || bounds.bottom <= bounds.top ||
        !std::isfinite(scale) || scale <= 0) return false;
    self->dark = dark; self->accent = accent; self->scale = std::clamp(scale, 0.75f, 4.0f);
    self->panel.UpdateDocked(bounds, dark, accent, self->scale);
    if (!(GetWindowLongPtrW(self->panel.Hwnd(), GWL_STYLE) & WS_VISIBLE)) ShowWindow(self->panel.Hwnd(), SW_SHOWNOACTIVATE);
    self->CaptureMount(); return true;
}
void UiAdapter::HideEmbedded() {
    if (state_->embedded && GetCurrentThreadId() == state_->thread && state_->panel.IsOpen() &&
        (GetWindowLongPtrW(state_->panel.Hwnd(), GWL_STYLE) & WS_VISIBLE)) ShowWindow(state_->panel.Hwnd(), SW_HIDE);
}
bool UiAdapter::HasPanel(uint64_t id) const { return state_->Panel(id); }
void UiAdapter::Stop() {
    if (state_) {
        state_->stopped = true;
        state_->data->Stop();
        state_->Closed();
        state_->panel.Close();
    }
}
bool UiAdapter::InCallback() const { return state_->callbacks != 0 || state_->data->InCallback(); }
HWND UiAdapter::NodeWindow(const std::string &id) const { return state_->panel.TreeNodeWindow(id); }
HWND UiAdapter::Window() const { return state_->panel.Hwnd(); }
std::optional<UiNode> UiAdapter::DataNode(const std::string &id) const { return state_->panel.TreeDataNode(id); }
bool UiAdapter::Snapshot(const std::wstring &path, bool dark, float scale, const std::string &reveal) {
    return state_->panel.SaveDebugSnapshot(path.c_str(), dark, scale, reveal);
}
bool UiAdapter::SnapshotCurrent(const std::wstring &path) {
    return state_->Panel(state_->panel_id) && state_->panel.SaveCurrentFrame(path.c_str());
}
PulseUiMountInfo UiAdapter::MountInfo() const {
    if (state_->Panel(state_->panel_id)) state_->CaptureMount();
    return state_->mount_info;
}
} // namespace pulse::extensions::native
