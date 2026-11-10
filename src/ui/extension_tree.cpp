#include <initguid.h>

#include "../common/localization.h"
#include "FluentTokens.h"
#include "edit_host.h"
#include "extension_data_view.h"
#include "extension_tree.h"
#include "extension_tree_controls.h"
#include "extension_tree_layout.h"
#include "fluent_components.h"
#include "fluent_menu.h"
#include "typography.h"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <commctrl.h>
#include <map>
#include <oleacc.h>
#include <uxtheme.h>
#include <windowsx.h>

namespace pulse::ui {
using extensions::UiNode;
using extensions::UiNodeType;
namespace {
std::wstring Wide(const std::string &text) {
    int count = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring result(count, 0);
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), count);
    return result;
}
std::string ReadText(HWND window) {
    int count = GetWindowTextLengthW(window);
    std::wstring text(count + 1, 0);
    GetWindowTextW(window, text.data(), count + 1);
    int bytes = WideCharToMultiByte(CP_UTF8, 0, text.data(), count, nullptr, 0, nullptr, nullptr);
    std::string result(bytes, 0);
    WideCharToMultiByte(CP_UTF8, 0, text.data(), count, result.data(), bytes, nullptr, nullptr);
    return result;
}
} // namespace
struct ExtensionTreeView::Impl {
    struct Item {
        const UiNode *node;
        HWND window;
        bool visible = true;
        bool enabled = true;
        D2D1_RECT_F bounds{}, clip{};
        RECT applied_clip{};
        bool clip_applied = false;
        bool command_allowed = true;
        int keyboard_part = 0;
        std::shared_ptr<ExtensionDataView> data;
        std::shared_ptr<IAccessible> accessible;
    };
    ExtensionTreeView *owner{};
    Compositor compositor;
    fluent::Painter painter{&compositor};
    std::shared_ptr<FluentMenu> menu;
    HWND hovered{}, pressed{};
    int hover_part = -1, pressed_part = -1;
    bool bar_drag = false, bar_hot = false;
    float bar_grab = 0;
    bool rendering = false;
    bool ready = false;
    bool render_failed = false;
    bool refreshing_sources = false;
    HWND host{};
    HFONT font{};
    HBRUSH brush{};
    HBRUSH edit_brush{};
    D2D1_COLOR_F EditBackground() const { return ColorFromRef(EditBackColor(dark)); }
    D2D1_COLOR_F EditForeground() const { return ColorFromRef(EditTextColor(dark)); }
    bool dark{}, high_contrast{}, refreshing{}, build_failed{};
    float scale = 1;
    D2D1_COLOR_F accent{};
    Theme theme{};
    static COLORREF Color(D2D1_COLOR_F color) {
        return RGB(static_cast<BYTE>(std::clamp(color.r, 0.0f, 1.0f) * 255),
                   static_cast<BYTE>(std::clamp(color.g, 0.0f, 1.0f) * 255),
                   static_cast<BYTE>(std::clamp(color.b, 0.0f, 1.0f) * 255));
    }
    int scroll = 0, total = 0;
    unsigned generation = 0;
    std::map<std::string, std::shared_ptr<ExtensionDataController>> sources;
    std::map<std::string, std::string> source_queries, validation_errors;
    std::set<HWND> composing;
    static constexpr UINT DataMessage = WM_APP + 0x351;
    extensions::UiTree tree;
    ExtensionTreeCallbacks callbacks;
    std::vector<Item> items;
    std::vector<const UiNode *> nodes;
    ExtensionTreeLayout layout;
    const UiNode *nested_drag = nullptr;
    std::map<std::string, std::string> state;
    uint64_t state_revision = 0;
    static constexpr UINT StateMessage = WM_APP + 0x352;
    std::map<std::string, ExtensionStateChange> pending_state_events;
    bool state_message_pending = false, dispatching_state = false;
    unsigned state_notification_depth = 0;
    struct StateNotificationScope {
        Impl &self;
        explicit StateNotificationScope(Impl &value) : self(value) { ++self.state_notification_depth; }
        ~StateNotificationScope() {
            --self.state_notification_depth;
            self.QueueStateEvents();
        }
    };
    void QueueStateEvents() {
        if (!host || pending_state_events.empty() || state_message_pending || dispatching_state ||
            state_notification_depth)
            return;
        state_message_pending = PostMessageW(host, StateMessage, static_cast<WPARAM>(generation), 0) != FALSE;
    }
    void ResetStateEvents() {
        pending_state_events.clear();
        state_message_pending = false;
    }
    bool AssignState(const std::string &key, const std::string &value,
                     ExtensionStateSource source = ExtensionStateSource::Programmatic) {
        auto at = state.find(key);
        if (at == state.end() || at->second == value)
            return false;
        at->second = value;
        ++state_revision;
        if (callbacks.on_state_changed) {
            auto &change = pending_state_events[key];
            change.key = key;
            change.value = value;
            change.revision = state_revision;
            change.source_mask |= static_cast<uint32_t>(source);
            QueueStateEvents();
        }
        return true;
    }
    void DispatchStateEvents() {
        state_message_pending = false;
        if (dispatching_state || state_notification_depth || pending_state_events.empty() ||
            !callbacks.on_state_changed)
            return;
        auto pending = std::move(pending_state_events);
        pending_state_events.clear();
        std::vector<ExtensionStateChange> changes;
        for (auto &[key, change] : pending)
            changes.push_back(std::move(change));
        auto callback = callbacks.on_state_changed;
        dispatching_state = true;
        try {
            callback(state_revision, changes);
        } catch (...) {
        }
        dispatching_state = false;
        QueueStateEvents();
    }

    COLORREF Background() const { return high_contrast ? GetSysColor(COLOR_WINDOW) : Color(theme.bg); }
    COLORREF Foreground() const { return high_contrast ? GetSysColor(COLOR_WINDOWTEXT) : Color(theme.text); }
    Item *Find(HWND window) {
        for (auto &item : items)
            if (item.window == window)
                return &item;
        return nullptr;
    }
    Item *Find(const UiNode *node) {
        for (auto &item : items)
            if (item.node == node)
                return &item;
        return nullptr;
    }
    // A text field's trailing badge may name a state value ("state.ext") so it follows a choice.
    std::string BadgeText(const UiNode &node) const {
        if (!node.badge_text.starts_with("state."))
            return node.badge_text;
        auto at = state.find(node.badge_text.substr(6));
        return at == state.end() ? std::string{} : at->second;
    }
    bool BooleanBinding(const std::string &binding) const {
        if (binding.empty())
            return true;
        auto at = state.find(binding.substr(6));
        return at != state.end() && at->second == "true";
    }
    bool Valid(const std::string &key, const std::string &value) const {
        if (!extensions::ValidateUiStateValue(tree, key, value))
            return false;
        for (const auto &item : items)
            if (item.node->data_source && item.node->state == key && item.data && !item.data->ContainsRow(value))
                return false;
        return true;
    }
    const UiNode *Node(const std::string &id) const {
        for (auto node : nodes)
            if (node->id == id)
                return node;
        return nullptr;
    }
    void ApplyPage(const std::string &id, const UiDataPage &page) {
        const auto node = Node(id);
        if (!node || !node->data_source || !host)
            return;
        auto item = Find(node);
        if (!item || !item->data)
            return;
        const auto source = *node->data_source;
        if (page.status == "idle" || page.status == "ready" || !page.rows.empty()) {
            item->data->UpdateRows(page.rows);
            if (!node->state.empty() && !item->data->ContainsRow(state[node->state]))
                AssignState(node->state, "");
        }
        std::vector<extensions::UiStateEntry> updates;
        if (!source.status_state.empty())
            updates.push_back({source.status_state, page.status});
        if (!source.error_state.empty()) {
            auto error = page.error;
            if (error.size() > 1024) {
                size_t end = 1024;
                while (end && (static_cast<unsigned char>(error[end]) & 0xc0) == 0x80)
                    --end;
                error.resize(end);
            }
            updates.push_back({source.error_state, std::move(error)});
        }
        if (!source.progress_state.empty())
            updates.push_back({source.progress_state, std::to_string(page.progress)});
        owner->SetStates(updates);
        if (host) {
            Layout();
            InvalidateRect(host, nullptr, FALSE);
        }
    }
    bool RequestData(const std::string &id, const std::string &operation) {
        const auto node = Node(id);
        if (!node || !node->data_source || !host)
            return false;
        const auto source = *node->data_source;
        auto &slot = sources[id];
        if (!slot) {
            std::weak_ptr<Impl> weak = owner->impl_;
            auto revision = generation;
            HWND target = host;
            slot = std::make_shared<ExtensionDataController>(
                callbacks.request_data, [target] { PostMessageW(target, DataMessage, 0, 0); },
                [weak, revision, id](const UiDataPage &page) {
                    auto self = weak.lock();
                    if (self && self->host && self->generation == revision)
                        self->ApplyPage(id, page);
                });
        }
        auto controller = slot;
        if (operation == "cancel" && source.source != "context.transferTask" && source.source != "context.workflow") {
            controller->Cancel();
            UiDataPage page;
            page.status = "cancelled";
            page.progress = 0;
            ApplyPage(id, page);
            return true;
        }
        UiDataRequest request;
        request.node_id = id;
        request.source = source.source;
        request.page_size = source.page_size;
        request.filter = source.filter_state.empty() ? "" : state[source.filter_state];
        request.sort = source.sort_state.empty() ? "" : state[source.sort_state];
        request.operation = operation == "cancel" ? "cancel" : "read";
        bool append = operation == "nextPage";
        if (append) {
            request.cursor = controller->NextCursor();
            if (request.cursor.empty())
                return false;
        }
        source_queries[id] = request.filter + '\0' + request.sort;
        UiDataPage loading;
        loading.status = "loading";
        loading.progress = 0;
        ApplyPage(id, loading);
        if (!host)
            return false;
        controller->Request(std::move(request), append);
        return true;
    }
    void RefreshSources() {
        if (refreshing_sources)
            return;
        refreshing_sources = true;
        std::vector<std::string> changed;
        for (auto node : nodes)
            if (node->data_source) {
                const auto &source = *node->data_source;
                auto query = (source.filter_state.empty() ? "" : state[source.filter_state]) + '\0' +
                             (source.sort_state.empty() ? "" : state[source.sort_state]);
                if (!source_queries.contains(node->id) || source_queries[node->id] != query)
                    changed.push_back(node->id);
            }
        for (const auto &id : changed)
            if (host)
                RequestData(id, "refresh");
        refreshing_sources = false;
    }
    bool Allowed(const std::string &command) {
        if (command.empty() || !host)
            return false;
        for (const auto &action : tree.actions)
            if (action.id == command)
                return true;
        auto revision = generation;
        auto check = callbacks.command_enabled;
        return !command.empty() && bool(callbacks.on_command) && (!check || check(command)) && generation == revision &&
               host;
    }
    bool Invoke(const std::string &command) {
        auto lifetime = owner->impl_;
        if (!host)
            return false;
        for (const auto &action : tree.actions) {
            if (action.id == command) {
                const auto chosen = action;
                if (!composing.empty())
                    return false;
                bool valid = true;
                for (const auto &id : chosen.validate) {
                    const auto node = Node(id);
                    if (!node || !extensions::ValidateUiInput(*node, state[node->state])) {
                        valid = false;
                        if (node && node->validation) {
                            validation_errors[id] = node->validation->message;
                            if (auto item = Find(node))
                                AccessibleName(item->window, Wide(node->text + " " + node->validation->message));
                        }
                    } else {
                        validation_errors.erase(id);
                        if (auto item = Find(node))
                            AccessibleName(item->window, Wide(node->text));
                    }
                }
                Layout();
                InvalidateRect(host, nullptr, FALSE);
                if (!valid)
                    return false;
                for (const auto &update : chosen.updates)
                    if (!state.contains(update.key) || !Valid(update.key, update.value))
                        return false;
                if (chosen.data_operation) {
                    const auto target = Node(chosen.data_operation->target);
                    const auto &operation = chosen.data_operation->operation;
                    if (!target || !target->data_source ||
                        (operation != "refresh" && operation != "retry" && operation != "nextPage" &&
                         operation != "cancel"))
                        return false;
                    if (operation == "nextPage") {
                        auto source = sources.find(target->id);
                        if (source == sources.end() || source->second->NextCursor().empty())
                            return false;
                    }
                }
                if (!owner->SetStates(chosen.updates))
                    return false;
                return !chosen.data_operation ||
                       RequestData(chosen.data_operation->target, chosen.data_operation->operation);
            }
        }
        auto revision = generation;
        auto callback = callbacks.on_command;
        if (Allowed(command) && generation == revision && callback) {
            callback(command);
            return true;
        }
        return false;
    }
    void CommitValue(HWND window, const std::string &value) {
        auto item = Find(window);
        if (!item)
            return;
        auto key = item->node->state;
        if (!key.empty()) {
            if (Valid(key, value)) AssignState(key, value, ExtensionStateSource::User);
            owner->Refresh();
        } else {
            Layout();
            InvalidateRect(host, nullptr, FALSE);
        }
    }
    void AccessibleName(HWND window, const std::wstring &name) {
        IAccPropServices *properties = nullptr;
        if (SUCCEEDED(
                CoCreateInstance(CLSID_AccPropServices, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&properties)))) {
            properties->SetHwndPropStr(window, static_cast<DWORD>(OBJID_CLIENT), CHILDID_SELF, PROPID_ACC_NAME,
                                       name.c_str());
            properties->Release();
        }
        NotifyWinEvent(EVENT_OBJECT_NAMECHANGE, window, OBJID_CLIENT, CHILDID_SELF);
    }
    void ApplyDataEvent(HWND window, const ExtensionDataView::Event &event) {
        auto lifetime = owner->impl_;
        auto item = Find(window);
        if (!item || !item->data || !item->visible || !item->enabled)
            return;
        auto revision = generation;
        auto command = event.command;
        if (event.selection_changed) {
            auto name = Wide(item->node->text) + L" " + item->data->SelectedText();
            CommitValue(window, item->data->Selection());
            if (generation == revision)
                AccessibleName(window, name);
        }
        if (generation != revision)
            return;
        if (!command.empty())
            Invoke(command);
        if (host)
            InvalidateRect(host, nullptr, FALSE);
    }
    void ConnectDataAccessibility(HWND window) {
        auto item = Find(window);
        if (!item || !item->data)
            return;
        std::weak_ptr<Impl> weak = owner->impl_;
        unsigned revision = generation;
        DataAccessibleCallbacks access;
        access.snapshot = [weak, revision, window]() -> std::optional<DataAccessibleSnapshot> {
            auto self = weak.lock();
            if (!self || self->generation != revision || !self->host || !IsWindow(window))
                return std::nullopt;
            auto current = self->Find(window);
            if (!current || !current->data)
                return std::nullopt;
            DataAccessibleSnapshot snapshot;
            snapshot.name = Wide(current->node->text);
            snapshot.default_action_name = l10n::Get(l10n::StringId::Open);
            snapshot.type = current->node->type;
            snapshot.enabled = current->visible && current->enabled && IsWindowEnabled(window);
            snapshot.focused = GetFocus() == window;
            GetWindowRect(window, &snapshot.screen_bounds);
            snapshot.rows = current->data->AccessibilityRows();
            for (auto &row : snapshot.rows) {
                auto bounds = row.screen_bounds;
                row.offscreen = row.offscreen || !current->visible || bounds.bottom <= current->clip.top ||
                                bounds.top >= current->clip.bottom || bounds.right <= current->clip.left ||
                                bounds.left >= current->clip.right;
                row.enabled = row.enabled && snapshot.enabled;
                MapWindowPoints(self->host, nullptr, reinterpret_cast<POINT *>(&row.screen_bounds), 2);
            }
            return snapshot;
        };
        auto select = [weak, revision, window](const std::string &id) {
            auto self = weak.lock();
            if (!self || self->generation != revision || !self->host)
                return false;
            auto current = self->Find(window);
            if (!current || !current->data || !current->visible || !current->enabled || !IsWindowEnabled(window))
                return false;
            auto key = current->node->state;
            if (!key.empty() && !self->Valid(key, id))
                return false;
            if (!current->data->SetSelection(id))
                return false;
            if (!key.empty()) {
                self->AssignState(key, id, ExtensionStateSource::User);
                self->owner->Refresh();
            } else
                self->Layout();
            return self->generation == revision && self->host;
        };
        access.select = select;
        access.activate = [weak, revision, window, select](const std::string &id) {
            if (!select(id))
                return false;
            auto self = weak.lock();
            if (!self || self->generation != revision)
                return false;
            auto current = self->Find(window);
            if (!current || !current->data)
                return false;
            bool branch = false;
            for (const auto &row : current->data->AccessibilityRows())
                if (row.id == id) {
                    branch = row.has_children;
                    break;
                }
            auto event = current->data->KeyDown(VK_RETURN);
            if (branch && event.handled) {
                InvalidateRect(self->host, nullptr, FALSE);
                NotifyWinEvent(EVENT_OBJECT_STATECHANGE, window, OBJID_CLIENT, CHILDID_SELF);
                return true;
            }
            return !event.command.empty() && self->Invoke(event.command);
        };
        IAccessible *raw = nullptr;
        if (SUCCEEDED(CreateExtensionDataAccessible(window, std::move(access), &raw)))
            item->accessible = std::shared_ptr<IAccessible>(raw, [](IAccessible *value) { value->Release(); });
    }
    fluent::ScrollbarSpec Scrollbar() const {
        RECT client{};
        GetClientRect(host, &client);
        fluent::ScrollbarSpec bar;
        bar.viewport = {static_cast<float>(client.right) - 14 * scale, 0, static_cast<float>(client.right),
                        static_cast<float>(client.bottom)};
        bar.offset = static_cast<float>(scroll);
        bar.viewport_extent = static_cast<float>(client.bottom);
        bar.content_extent = static_cast<float>(total);
        bar.opacity = 1;
        bar.expand_progress = bar_drag || bar_hot ? 1.0f : 0.0f;
        return bar;
    }
    void DragScrollbar(float y) {
        auto bar = Scrollbar();
        auto thumb = fluent::ScrollbarThumbRect(bar, scale, high_contrast);
        scroll = static_cast<int>(std::lround(
            fluent::ScrollbarDragOffset(bar.viewport.top, bar.viewport_extent, thumb.bottom - thumb.top,
                                        std::max(0.0f, bar.content_extent - bar.viewport_extent), y, bar_grab)));
        Layout();
    }
    void OpenSelect(HWND window) {
        auto lifetime = owner->impl_;
        auto item = Find(window);
        if (!item || !item->enabled || !item->visible)
            return;
        auto node = *item->node;
        unsigned revision = generation;
        std::vector<FluentMenuItem> choices;
        int selected = static_cast<int>(SendMessageW(window, CB_GETCURSEL, 0, 0));
        for (size_t index = 0; index < node.options.size(); ++index) {
            FluentMenuItem choice;
            choice.command = static_cast<int>(index) + 1;
            choice.text = Wide(node.options[index]);
            // Like Pulse's own dropdowns: the check alone marks the choice (radio adds a dot to every row).
            choice.checked = selected == static_cast<int>(index);
            choices.push_back(std::move(choice));
        }
        if (!menu) {
            menu = std::make_shared<FluentMenu>();
            if (!menu->Create(host, &compositor, scale)) {
                menu.reset();
                return;
            }
        }
        menu->SetTheme(dark, accent);
        RECT rect{};
        GetWindowRect(window, &rect);
        auto active_menu = menu;
        int chosen = active_menu->TrackDropdown(rect, std::move(choices));
        if (generation != revision || !host)
            return;
        if (chosen > 0) {
            SendMessageW(window, CB_SETCURSEL, chosen - 1, 0);
            SendMessageW(host, WM_COMMAND, MAKEWPARAM(0, CBN_SELCHANGE), reinterpret_cast<LPARAM>(window));
        }
        InvalidateRect(host, nullptr, FALSE);
    }
    void OpenCommandMenu(HWND window) {
        auto lifetime = owner->impl_;
        auto item = Find(window);
        if (!item || !item->visible || !item->enabled)
            return;
        auto node = *item->node;
        auto revision = generation;
        if (!menu) {
            menu = std::make_shared<FluentMenu>();
            if (!menu->Create(host, &compositor, scale)) {
                menu.reset();
                return;
            }
        }
        menu->SetTheme(dark, accent);
        auto active_menu = menu;
        RECT anchor{};
        GetWindowRect(window, &anchor);
        std::vector<const std::vector<extensions::UiMenuItem> *> path{&node.menu};
        while (host && generation == revision && !path.empty()) {
            std::vector<FluentMenuItem> entries;
            std::map<int, const extensions::UiMenuItem *> commands;
            std::function<bool(const extensions::UiMenuItem &)> enabled = [&](const auto &entry) {
                if (!entry.enabled)
                    return false;
                if (!entry.children.empty()) {
                    for (const auto &child : entry.children)
                        if (enabled(child))
                            return true;
                    return false;
                }
                return Allowed(entry.command);
            };
            int command = 1;
            for (const auto &entry : *path.back()) {
                FluentMenuItem row;
                row.command = command;
                row.text = Wide(entry.text);
                row.glyph = Wide(entry.glyph);
                row.shortcut = Wide(entry.shortcut);
                row.enabled = enabled(entry);
                row.checked = entry.checked;
                row.separator_after = entry.separator;
                commands.emplace(command++, &entry);
                entries.push_back(std::move(row));
                if (generation != revision)
                    return;
            }
            int selected = active_menu->TrackDropdown(anchor, std::move(entries));
            if (generation != revision || !host)
                return;
            if (!selected) {
                if (path.size() == 1)
                    return;
                path.pop_back();
                continue;
            }
            auto found = commands.find(selected);
            if (found == commands.end())
                return;
            const auto &entry = *found->second;
            if (!entry.children.empty()) {
                path.push_back(&entry.children);
                continue;
            }
            if (entry.enabled)
                Invoke(entry.command);
            return;
        }
    }
    void ActivatePart(HWND window, int part) {
        auto lifetime = owner->impl_;
        auto item = Find(window);
        if (!item || !item->visible || !item->enabled || !IsWindowEnabled(window))
            return;
        auto node = *item->node;
        if (node.type == UiNodeType::InfoBar) {
            if (part == 2)
                Invoke(node.close_command);
            else if (part >= 0 && part < static_cast<int>(node.actions.size()) && node.actions[part].enabled)
                Invoke(node.actions[part].command);
            return;
        }
        if (node.type == UiNodeType::PaneHeader) {
            Invoke(part == 0 ? node.filter_command : part == 1 ? node.sort_command : std::string{});
            return;
        }
        if (node.type == UiNodeType::StagingTray && part == 0) {
            Invoke(node.command);
            return;
        }
        if (node.type == UiNodeType::MenuButton || (node.type == UiNodeType::SplitButton && part == 1) ||
            (node.type == UiNodeType::Breadcrumb && !node.menu.empty())) {
            OpenCommandMenu(window);
            return;
        }
        Invoke(node.command);
    }
    bool Render() {
        if (!ready || !host || rendering)
            return false;
        if (compositor.NeedsRecovery()) {
            // Recovery can reuse the DC address; release factory-bound Painter resources first.
            painter.SetCompositor(nullptr);
            if (!compositor.Recover())
                return false;
            painter.SetCompositor(&compositor);
            compositor.RecreateTextFormats(scale);
            painter.SetScale(scale);
            painter.InvalidateTypography();
        }
        rendering = true;
        auto *dc = compositor.Dc();
        if (!dc) {
            rendering = false;
            return false;
        }
        dc->BeginDraw();
        dc->Clear(theme.bg);
        painter.BeginFrame(theme, high_contrast);
        bool successful = true;
        for (const auto &item : items) {
            if (!item.visible)
                continue;
            const auto &node = *item.node;
            const auto bounds = item.bounds;
            fluent::ControlState visual;
            visual.enabled = IsWindowEnabled(item.window) != FALSE;
            visual.hovered = hovered == item.window;
            visual.pressed = pressed == item.window;
            visual.focused = GetFocus() == item.window;
            // Text fields show focus with their accent underline; a ring on top doubles it.
            visual.keyboard_focus = visual.focused && node.type != UiNodeType::TextInput &&
                                    node.type != UiNodeType::CommandSearch;
            dc->PushAxisAlignedClip(item.clip, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
            if (item.data)
                item.data->Draw(painter, compositor, theme, visual);
            else {
                ExtensionControlVisual control;
                control.host = host;
                control.window = item.window;
                control.bounds = bounds;
                control.state = visual;
                control.hover_part = hover_part;
                control.pressed_part = pressed_part;
                control.scale = scale;
                control.dark = dark;
                if (node.badge_text.starts_with("state.")) {
                    auto resolved = node;
                    resolved.badge_text = BadgeText(node);
                    successful = DrawExtensionControl(resolved, control, compositor, painter, theme) && successful;
                } else
                    successful = DrawExtensionControl(node, control, compositor, painter, theme) && successful;
            }
            auto error = validation_errors.find(node.id);
            if (error != validation_errors.end())
                painter.DrawWrappedCaption(Wide(error->second), {bounds.left, bounds.bottom + 4 * scale},
                                           bounds.right - bounds.left, theme.danger);
            dc->PopAxisAlignedClip();
        }
        auto scrollbar = Scrollbar();
        if (scrollbar.content_extent > scrollbar.viewport_extent)
            painter.DrawScrollbar(scrollbar);
        for (const auto &[node, viewport] : layout.Viewports()) {
            if (!viewport.visible)
                continue;
            auto bar = layout.Scrollbar(node, nested_drag == node);
            dc->PushAxisAlignedClip(viewport.clip, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
            painter.DrawScrollbar(bar);
            dc->PopAxisAlignedClip();
        }
        HRESULT result = dc->EndDraw();
        if (SUCCEEDED(result))
            result = compositor.Present();
        if (result == D2DERR_RECREATE_TARGET || result == DXGI_ERROR_DEVICE_REMOVED ||
            result == DXGI_ERROR_DEVICE_RESET || result == DXGI_ERROR_DRIVER_INTERNAL_ERROR)
            compositor.NotifyDeviceLost(result);
        render_failed = !successful || FAILED(result);
        rendering = false;
        return !render_failed;
    }
    static LRESULT CALLBACK ChildProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam, UINT_PTR,
                                      DWORD_PTR data) {
        auto self = reinterpret_cast<Impl *>(data);
        auto item = self->Find(window);
        auto lifetime = self->owner->impl_;
        StateNotificationScope notification(*self);
        if (message == WM_IME_STARTCOMPOSITION)
            self->composing.insert(window);
        if (message == WM_IME_ENDCOMPOSITION) {
            self->composing.erase(window);
            auto result = DefSubclassProc(window, message, wparam, lparam);
            if (self->host && self->Find(window))
                self->CommitValue(window, ReadText(window));
            return result;
        }
        if (message == WM_NCDESTROY)
            self->composing.erase(window);
        if (message == WM_GETOBJECT && static_cast<LONG>(lparam) == OBJID_CLIENT && item && item->accessible)
            return LresultFromObject(IID_IAccessible, wparam, item->accessible.get());

        if (message == WM_LBUTTONDOWN || message == WM_MOUSEMOVE || message == WM_LBUTTONUP) {
            POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            MapWindowPoints(window, self->host, &point, 1);
            bool on_bar =
                fluent::ScrollbarHitTest(self->Scrollbar(), static_cast<float>(point.x), static_cast<float>(point.y));
            if (on_bar || self->bar_drag || self->nested_drag ||
                self->layout.HitScrollbar(static_cast<float>(point.x), static_cast<float>(point.y))) {
                return SendMessageW(self->host, message, wparam, MAKELPARAM(point.x, point.y));
            }
            if (message == WM_MOUSEMOVE && self->bar_hot) {
                self->bar_hot = false;
                InvalidateRect(self->host, nullptr, FALSE);
            }
        }

        if (item && item->node->type != UiNodeType::TextInput && item->node->type != UiNodeType::CommandSearch &&
            (message == WM_PAINT || message == WM_PRINTCLIENT || message == WM_ERASEBKGND || message == WM_NCPAINT)) {
            if (message == WM_PAINT) {
                PAINTSTRUCT paint{};
                BeginPaint(window, &paint);
                EndPaint(window, &paint);
            }
            return 0;
        }
        if (item && item->node->type == UiNodeType::Select) {
            if (message == CB_SHOWDROPDOWN) {
                if (wparam)
                    self->OpenSelect(window);
                return 0;
            }
            if ((message == WM_SYSKEYDOWN && wparam == VK_DOWN) || message == WM_LBUTTONDOWN ||
                (message == WM_KEYDOWN &&
                 (wparam == VK_SPACE || wparam == VK_RETURN || wparam == VK_DOWN || wparam == VK_F4))) {
                SetFocus(window);
                self->OpenSelect(window);
                return 0;
            }
        }
        if (item && item->data && item->visible && item->enabled) {
            POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            if (message == WM_LBUTTONDOWN || message == WM_LBUTTONUP || message == WM_MOUSEMOVE ||
                message == WM_LBUTTONDBLCLK)
                MapWindowPoints(window, self->host, &point, 1);
            ExtensionDataView::Event event;
            if (message == WM_LBUTTONDOWN) {
                SetFocus(window);
                event = item->data->PointerDown(static_cast<float>(point.x), static_cast<float>(point.y));
                if (item->data->Dragging())
                    SetCapture(window);
            } else if (message == WM_MOUSEMOVE)
                event = item->data->PointerMove(static_cast<float>(point.x), static_cast<float>(point.y));
            else if (message == WM_LBUTTONUP) {
                event = item->data->PointerUp();
                if (GetCapture() == window)
                    ReleaseCapture();
            } else if (message == WM_LBUTTONDBLCLK)
                event = item->data->DoubleClick(static_cast<float>(point.x), static_cast<float>(point.y));
            else if (message == WM_KEYDOWN && wparam != VK_TAB && wparam != VK_ESCAPE)
                event = item->data->KeyDown(wparam);
            else if (message == WM_MOUSEWHEEL || message == WM_MOUSEHWHEEL) {
                if (item->data->Wheel(GET_WHEEL_DELTA_WPARAM(wparam), message == WM_MOUSEHWHEEL)) {
                    InvalidateRect(self->host, nullptr, FALSE);
                    return 0;
                }
            }
            if (event.handled) {
                self->ApplyDataEvent(window, event);
                return 0;
            }
        }
        if ((message == WM_LBUTTONDOWN || message == WM_MOUSEMOVE || message == WM_LBUTTONUP ||
             message == WM_KEYDOWN) &&
            item && (item->node->type == UiNodeType::Slider || item->node->type == UiNodeType::SplitPane) &&
            item->visible && item->enabled) {
            bool split = item->node->type == UiNodeType::SplitPane;
            int maximum = split ? 100 : item->node->stops - 1;
            int next = static_cast<int>(SendMessageW(window, TBM_GETPOS, 0, 0));
            bool update = false;
            if (message == WM_LBUTTONDOWN || (message == WM_MOUSEMOVE && GetCapture() == window)) {
                POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
                MapWindowPoints(window, self->host, &point, 1);
                if (split) {
                    float length = item->node->orientation == "horizontal" ? item->bounds.right - item->bounds.left
                                                                           : item->bounds.bottom - item->bounds.top;
                    float at = item->node->orientation == "horizontal" ? point.x - item->bounds.left
                                                                       : point.y - item->bounds.top;
                    float divider = 8 * self->scale;
                    float position = std::max(0.0f, length - divider) * next / 100;
                    if (message == WM_LBUTTONDOWN && (at < position || at > position + divider))
                        return 0;
                    next = static_cast<int>(std::lround((at - divider / 2) * 100 / std::max(1.0f, length - divider)));
                } else
                    next = static_cast<int>(std::lround(fluent::StepSliderPositionAt(
                        item->bounds, item->node->stops, static_cast<float>(point.x), self->scale)));
                if (message == WM_LBUTTONDOWN) {
                    SetFocus(window);
                    SetCapture(window);
                }
                update = true;
            } else if (message == WM_LBUTTONUP && GetCapture() == window) {
                ReleaseCapture();
                return 0;
            } else if (message == WM_KEYDOWN) {
                if (wparam == VK_LEFT || wparam == VK_DOWN) {
                    --next;
                    update = true;
                }
                if (wparam == VK_RIGHT || wparam == VK_UP) {
                    ++next;
                    update = true;
                }
                if (wparam == VK_HOME) {
                    next = 0;
                    update = true;
                }
                if (wparam == VK_END) {
                    next = maximum;
                    update = true;
                }
            }
            if (update) {
                next = std::clamp(next, 0, maximum);
                SendMessageW(window, TBM_SETPOS, TRUE, next);
                self->CommitValue(window, std::to_string(next));
                return 0;
            }
        }
        if (item && (item->node->type == UiNodeType::MenuButton || item->node->type == UiNodeType::SplitButton ||
                     item->node->type == UiNodeType::InfoBar || item->node->type == UiNodeType::PaneHeader ||
                     item->node->type == UiNodeType::StagingTray || item->node->type == UiNodeType::Breadcrumb)) {
            POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            if (message == WM_LBUTTONDOWN || message == WM_LBUTTONUP || message == WM_MOUSEMOVE)
                MapWindowPoints(window, self->host, &point, 1);
            if (message == WM_LBUTTONDOWN) {
                int part = ExtensionControlPart(*item->node, item->bounds, static_cast<float>(point.x),
                                                static_cast<float>(point.y), self->scale, self->painter);
                if (part >= 0) {
                    SetFocus(window);
                    item->keyboard_part = part;
                    self->pressed = window;
                    self->pressed_part = part;
                    SetCapture(window);
                    InvalidateRect(self->host, nullptr, FALSE);
                    return 0;
                }
            }
            if (message == WM_LBUTTONUP && GetCapture() == window) {
                int part = ExtensionControlPart(*item->node, item->bounds, static_cast<float>(point.x),
                                                static_cast<float>(point.y), self->scale, self->painter);
                ReleaseCapture();
                self->pressed = nullptr;
                if (part == self->pressed_part)
                    self->ActivatePart(window, part);
                return 0;
            }
            if (message == WM_KEYDOWN && (wparam == VK_LEFT || wparam == VK_RIGHT) &&
                (item->node->type == UiNodeType::InfoBar || item->node->type == UiNodeType::PaneHeader ||
                 item->node->type == UiNodeType::SplitButton)) {
                int count = item->node->type == UiNodeType::InfoBar ? 3 : 2;
                item->keyboard_part = (item->keyboard_part + (wparam == VK_RIGHT ? 1 : count - 1)) % count;
                if (item->node->type == UiNodeType::InfoBar) {
                    std::wstring name;
                    if (item->keyboard_part == 2)
                        name = l10n::Get(l10n::StringId::Close);
                    else if (item->keyboard_part < static_cast<int>(item->node->actions.size()))
                        name = Wide(item->node->actions[item->keyboard_part].text);
                    if (!name.empty())
                        self->AccessibleName(window, name);
                }
                InvalidateRect(self->host, nullptr, FALSE);
                return 0;
            }
            if (message == WM_KEYDOWN &&
                (wparam == VK_RETURN || wparam == VK_SPACE || wparam == VK_F4 || wparam == VK_DOWN)) {
                self->ActivatePart(window, wparam == VK_F4 || wparam == VK_DOWN ? 1 : item->keyboard_part);
                return 0;
            }
        }
        if (item && (item->node->type == UiNodeType::Tabs || item->node->type == UiNodeType::Segmented) &&
            message == WM_LBUTTONDOWN) {
            RECT rect{};
            GetClientRect(window, &rect);
            int count = static_cast<int>(item->node->type == UiNodeType::Tabs ? item->node->children.size()
                                                                              : item->node->options.size());
            int index =
                std::clamp(GET_X_LPARAM(lparam) * count / std::max(1L, rect.right), 0L, static_cast<LONG>(count - 1));
            if (item->node->type == UiNodeType::Tabs && index >= 0 && index < count) {
                const auto &tab = item->node->children[index];
                float cell_width = static_cast<float>(rect.right) / std::max(1, count);
                float right = cell_width * (index + 1);
                float x = static_cast<float>(GET_X_LPARAM(lparam));
                if (!tab.close_command.empty() && x >= right - 34 * self->scale && x < right - 16 * self->scale) {
                    auto command = tab.close_command;
                    self->Invoke(command);
                    return 0;
                }
            }
            self->pressed = window;
            self->pressed_part = index;
            SetFocus(window);
            TabCtrl_SetCurSel(window, index);
            NMHDR notify{window, 0, TCN_SELCHANGE};
            SendMessageW(self->host, WM_NOTIFY, 0, reinterpret_cast<LPARAM>(&notify));
            return 0;
        }
        if (item && item->node->type == UiNodeType::Tabs && message == WM_KEYDOWN &&
            (wparam == VK_DELETE || (wparam == 'W' && GetKeyState(VK_CONTROL) < 0))) {
            int selected = TabCtrl_GetCurSel(window);
            if (selected >= 0 && selected < static_cast<int>(item->node->children.size())) {
                auto command = item->node->children[selected].close_command;
                self->Invoke(command);
            }
            return 0;
        }
        int part = -1;
        if (item && (message == WM_MOUSEMOVE || message == WM_LBUTTONDOWN)) {
            POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            MapWindowPoints(window, self->host, &point, 1);
            part = ExtensionControlPart(*item->node, item->bounds, static_cast<float>(point.x),
                                        static_cast<float>(point.y), self->scale, self->painter);
            if (item->node->type == UiNodeType::List)
                part = GET_Y_LPARAM(lparam) / std::max(1, static_cast<int>(32 * self->scale)) +
                       static_cast<int>(SendMessageW(window, LB_GETTOPINDEX, 0, 0));
        }
        if (message == WM_MOUSEMOVE) {
            self->hovered = window;
            self->hover_part = part;
            TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, window, 0};
            TrackMouseEvent(&track);
            InvalidateRect(self->host, nullptr, FALSE);
        }
        if (message == WM_MOUSELEAVE) {
            self->hovered = nullptr;
            InvalidateRect(self->host, nullptr, FALSE);
        }
        if (message == WM_LBUTTONDOWN) {
            self->pressed = window;
            self->pressed_part = part;
        }
        if (message == WM_LBUTTONUP)
            self->pressed = nullptr;
        if (message == WM_KEYDOWN && wparam == VK_TAB) {
            HWND next = GetNextDlgTabItem(self->host, window, GetKeyState(VK_SHIFT) < 0);
            if (next)
                SetFocus(next);
            return 0;
        }
        if (message == WM_KEYDOWN && wparam == VK_ESCAPE) {
            SendMessageW(GetParent(self->host), WM_KEYDOWN, wparam, lparam);
            return 0;
        }
        if (message == WM_MOUSEWHEEL)
            return SendMessageW(self->host, message, wparam, lparam);
        if (message == WM_SETFOCUS) {
            if (item && self->layout.Reveal(item->node))
                self->Layout();
            RECT rect{}, client{};
            GetWindowRect(window, &rect);
            MapWindowPoints(nullptr, self->host, reinterpret_cast<POINT *>(&rect), 2);
            GetClientRect(self->host, &client);
            if (rect.top < 0)
                self->scroll = std::max(0, self->scroll + static_cast<int>(rect.top) - 8);
            else if (rect.bottom > client.bottom)
                self->scroll += static_cast<int>(rect.bottom - client.bottom) + 8;
            self->Layout();
            InvalidateRect(window, nullptr, TRUE);
        }
        if (message == WM_KILLFOCUS)
            InvalidateRect(window, nullptr, TRUE);
        LRESULT result = 0;
        if (item && (item->node->type == UiNodeType::TextInput || item->node->type == UiNodeType::CommandSearch)) {
            if (!HandleChildEditMessage(self->compositor, self->compositor.TextFormat(), self->EditForeground(),
                                        self->EditBackground(), self->edit_brush, window, message, wparam, lparam,
                                        result))
                result =
                    DefPresentedChildEditProc(self->compositor, self->compositor.TextFormat(), self->EditForeground(),
                                              self->EditBackground(), window, message, wparam, lparam);
        } else
            result = DefSubclassProc(window, message, wparam, lparam);
        if (self->host && (message == WM_SETFOCUS || message == WM_KILLFOCUS || message == WM_LBUTTONDOWN ||
                           message == WM_LBUTTONUP || message == WM_KEYDOWN || message == WM_CHAR))
            InvalidateRect(self->host, nullptr, FALSE);
        return result;
    }
    static LRESULT CALLBACK Proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
        auto self = reinterpret_cast<Impl *>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            self = static_cast<Impl *>(reinterpret_cast<CREATESTRUCTW *>(lparam)->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            self->host = window;
        }
        if (!self)
            return DefWindowProcW(window, message, wparam, lparam);
        auto lifetime = self->owner->impl_;
        if (message == WM_NCDESTROY) {
            auto sources = std::move(self->sources);
            self->sources.clear();
            for (auto &[id, source] : sources)
                source->Close();
            self->host = nullptr;
            self->callbacks = {};
            self->ResetStateEvents();
            ++self->generation;
            SetWindowLongPtrW(window, GWLP_USERDATA, 0);
            return DefWindowProcW(window, message, wparam, lparam);
        }

        if (message == StateMessage) {
            if (static_cast<uint64_t>(wparam) == self->generation) self->DispatchStateEvents();
            return 0;
        }
        if (message == DataMessage) {
            auto sources = self->sources;
            for (auto &[id, controller] : sources)
                controller->Drain();
            return 0;
        }
        if (message == WM_PAINT) {
            PAINTSTRUCT paint{};
            BeginPaint(window, &paint);
            self->Render();
            EndPaint(window, &paint);
            return 0;
        }
        if (message == WM_TIMER && wparam == 1) {
            bool animate = false;
            for (const auto &item : self->items)
                animate = animate || (item.visible && item.node->type == UiNodeType::Progress &&
                                      item.node->progress_state == "indeterminate");
            if (animate)
                InvalidateRect(window, nullptr, FALSE);
            else
                KillTimer(window, 1);
            return 0;
        }
        if (message == WM_LBUTTONDOWN) {
            float x = static_cast<float>(GET_X_LPARAM(lparam)), y = static_cast<float>(GET_Y_LPARAM(lparam));
            if (auto node = self->layout.HitScrollbar(x, y)) {
                self->nested_drag = node;
                auto bar = self->layout.Scrollbar(node, true);
                auto thumb = fluent::ScrollbarThumbRect(bar, self->scale, self->high_contrast);
                self->bar_grab = fluent::ScrollbarGrabOffset(thumb.top, thumb.bottom - thumb.top, y);
                SetCapture(window);
                self->layout.Drag(node, y, self->bar_grab);
                self->Layout();
                return 0;
            }
            auto bar = self->Scrollbar();
            if (fluent::ScrollbarHitTest(bar, x, y)) {
                auto thumb = fluent::ScrollbarThumbRect(bar, self->scale, self->high_contrast);
                self->bar_grab = fluent::ScrollbarGrabOffset(thumb.top, thumb.bottom - thumb.top, y);
                self->bar_drag = true;
                SetCapture(window);
                self->DragScrollbar(y);
                return 0;
            }
        }
        if (message == WM_MOUSEMOVE) {
            float x = static_cast<float>(GET_X_LPARAM(lparam)), y = static_cast<float>(GET_Y_LPARAM(lparam));
            if (self->nested_drag) {
                self->layout.Drag(self->nested_drag, y, self->bar_grab);
                self->Layout();
                return 0;
            }
            if (self->bar_drag) {
                self->DragScrollbar(y);
                return 0;
            }
            bool hot = fluent::ScrollbarHitTest(self->Scrollbar(), x, y);
            if (hot != self->bar_hot) {
                self->bar_hot = hot;
                InvalidateRect(window, nullptr, FALSE);
            }
            TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, window, 0};
            TrackMouseEvent(&track);
        }
        if (message == WM_MOUSELEAVE) {
            self->bar_hot = false;
            InvalidateRect(window, nullptr, FALSE);
        }
        if (message == WM_LBUTTONUP && (self->bar_drag || self->nested_drag)) {
            self->bar_drag = false;
            self->nested_drag = nullptr;
            if (GetCapture() == window)
                ReleaseCapture();
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        }
        if (message == WM_CAPTURECHANGED) {
            self->bar_drag = false;
            self->nested_drag = nullptr;
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        }
        if (message == WM_SIZE) {
            if (self->ready)
                self->compositor.Resize(LOWORD(lparam), HIWORD(lparam));
            self->Layout();
            return 0;
        }
        if (message == WM_SETFOCUS) {
            HWND first = GetNextDlgTabItem(window, nullptr, FALSE);
            if (first)
                SetFocus(first);
            return 0;
        }
        if (message == WM_MOUSEWHEEL) {
            POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            ScreenToClient(window, &point);
            if (self->layout.ScrollAt(static_cast<float>(point.x), static_cast<float>(point.y),
                                      static_cast<float>(-GET_WHEEL_DELTA_WPARAM(wparam)) / WHEEL_DELTA * 48)) {
                self->Layout();
                return 0;
            }
            RECT rect{};
            GetClientRect(window, &rect);
            self->scroll = std::clamp(self->scroll - GET_WHEEL_DELTA_WPARAM(wparam) / WHEEL_DELTA * 48, 0,
                                      std::max(0, self->total - static_cast<int>(rect.bottom)));
            self->Layout();
            return 0;
        }
        if (message == WM_ERASEBKGND)
            return 1;
        if (message == WM_CTLCOLORSTATIC || message == WM_CTLCOLOREDIT || message == WM_CTLCOLORLISTBOX ||
            message == WM_CTLCOLORBTN) {
            auto dc = reinterpret_cast<HDC>(wparam);
            SetTextColor(dc, self->Foreground());
            SetBkColor(dc, self->Background());
            return reinterpret_cast<LRESULT>(self->brush);
        }
        if (message == WM_NOTIFY) {
            StateNotificationScope notification(*self);
            auto header = reinterpret_cast<NMHDR *>(lparam);
            if (header->code == TCN_SELCHANGE) {
                auto item = self->Find(header->hwndFrom);
                if (item && item->visible && item->enabled && !item->node->state.empty()) {
                    auto value = std::to_string(TabCtrl_GetCurSel(item->window));
                    if (self->Valid(item->node->state, value))
                        self->AssignState(item->node->state, value, ExtensionStateSource::User);
                }
                self->owner->Refresh();
                return 0;
            }
        }
        if (message == WM_COMMAND && !self->refreshing) {
            StateNotificationScope notification(*self);
            auto item = self->Find(reinterpret_cast<HWND>(lparam));
            if (!item || !item->visible || !item->enabled || !IsWindowEnabled(item->window))
                return 0;
            auto node = *item->node;
            HWND child = item->window;
            auto code = HIWORD(wparam);
            if (code == BN_CLICKED) {
                if (IsExtensionBooleanControl(node)) {
                    if (node.type == UiNodeType::Radio)
                        SendMessageW(child, BM_SETCHECK, BST_CHECKED, 0);
                    auto value = SendMessageW(child, BM_GETCHECK, 0, 0) == BST_CHECKED ? "true" : "false";
                    auto revision = self->generation;
                    self->CommitValue(child, value);
                    if (self->generation == revision && !node.command.empty() && node.type != UiNodeType::StagingTray)
                        self->Invoke(node.command);
                    return 0;
                }
                self->ActivatePart(child, item->keyboard_part);
                return 0;
            }
            bool changed = false;
            std::string value;
            if ((node.type == UiNodeType::TextInput || node.type == UiNodeType::CommandSearch) && code == EN_CHANGE) {
                if (self->composing.contains(child))
                    return 0;
                value = ReadText(child);
                changed = true;
            }
            if ((node.type == UiNodeType::Checkbox || node.type == UiNodeType::Switch) && code == BN_CLICKED) {
                value = SendMessageW(child, BM_GETCHECK, 0, 0) == BST_CHECKED ? "true" : "false";
                changed = true;
            }
            if (node.type == UiNodeType::Select && code == CBN_SELCHANGE) {
                value = std::to_string(SendMessageW(child, CB_GETCURSEL, 0, 0));
                changed = true;
            }
            if (node.type == UiNodeType::List && code == LBN_SELCHANGE) {
                value = std::to_string(SendMessageW(child, LB_GETCURSEL, 0, 0));
                changed = true;
            }
            if (changed && !node.state.empty()) {
                if (self->Valid(node.state, value))
                    self->AssignState(node.state, value, ExtensionStateSource::User);
                self->owner->Refresh();
            }
            return 0;
        }
        return DefWindowProcW(window, message, wparam, lparam);
    }
    void Build(const UiNode &node) {
        nodes.push_back(&node);
        if (Find(&node)) {
            for (const auto &child : node.children)
                Build(child);
            return;
        }
        const wchar_t *type = nullptr;
        DWORD style = WS_CHILD | WS_VISIBLE | WS_TABSTOP;
        switch (node.type) {
        case UiNodeType::Text:
        case UiNodeType::Group:
            type = L"STATIC";
            style = WS_CHILD | WS_VISIBLE | SS_NOPREFIX;
            break;
        case UiNodeType::MenuButton:
        case UiNodeType::SplitButton:
        case UiNodeType::InfoBar:
        case UiNodeType::SidebarItem:
        case UiNodeType::Breadcrumb:
        case UiNodeType::TrayCard:
        case UiNodeType::DriveItem:
        case UiNodeType::PaneHeader:
        case UiNodeType::FileRow:
        case UiNodeType::StagingItem:
        case UiNodeType::TitleBarButton:
        case UiNodeType::Button:
            type = L"BUTTON";
            if (IsExtensionBooleanControl(node))
                style |= BS_AUTOCHECKBOX;
            break;
        case UiNodeType::Checkbox:
        case UiNodeType::Switch:
        case UiNodeType::SidebarSection:
        case UiNodeType::StagingTray:
            type = L"BUTTON";
            style |= BS_AUTOCHECKBOX;
            break;
        case UiNodeType::TextInput:
        case UiNodeType::CommandSearch:
            type = L"EDIT";
            style |= WS_BORDER | ES_AUTOHSCROLL;
            break;
        case UiNodeType::Select:
            type = L"COMBOBOX";
            style |= CBS_DROPDOWNLIST | WS_VSCROLL;
            break;
        case UiNodeType::List:
            type = L"LISTBOX";
            style |= LBS_NOTIFY | WS_BORDER | WS_VSCROLL;
            break;
        case UiNodeType::Progress:
            type = PROGRESS_CLASSW;
            style = WS_CHILD | WS_VISIBLE;
            break;
        case UiNodeType::Divider:
            type = L"STATIC";
            style = WS_CHILD | WS_VISIBLE | SS_ETCHEDHORZ;
            break;
        case UiNodeType::Segmented:
        case UiNodeType::Tabs:
            type = WC_TABCONTROLW;
            break;
        case UiNodeType::Radio:
            type = L"BUTTON";
            style |= BS_RADIOBUTTON;
            break;
        case UiNodeType::Slider:
        case UiNodeType::SplitPane:
            type = TRACKBAR_CLASSW;
            style |= TBS_NOTICKS;
            break;
        case UiNodeType::VirtualList:
        case UiNodeType::Tree:
        case UiNodeType::DataTable:
            type = L"STATIC";
            style |= SS_NOTIFY;
            break;
        case UiNodeType::Badge:
        case UiNodeType::Tag:
        case UiNodeType::EmptyState:
        case UiNodeType::StatusBar:
        case UiNodeType::Tooltip:
            type = L"STATIC";
            style = WS_CHILD | WS_VISIBLE | SS_NOPREFIX;
            break;
        default:
            break;
        }
        // A message-only info bar (no action, no close) is not a control: keep it out of the tab order,
        // otherwise it takes the dialog's initial focus ahead of the field below it.
        if (node.type == UiNodeType::InfoBar && node.actions.empty() && node.close_command.empty())
            style &= ~static_cast<DWORD>(WS_TABSTOP);
        if (type) {
            HWND child = (node.type == UiNodeType::TextInput || node.type == UiNodeType::CommandSearch)
                             ? CreatePulsePaintedChildEdit(host, Wide(node.text).c_str(),
                                   node.input_kind == "password" ? static_cast<DWORD>(ES_PASSWORD) : 0)
                             : CreateWindowExW(0, type, Wide(node.text).c_str(), style, 0, 0, 1, 1, host, nullptr,
                                               GetModuleHandleW(nullptr), nullptr);
            if (!child) {
                build_failed = true;
                return;
            }
            items.push_back({&node, child});
            if (IsExtensionDataControl(node)) {
                items.back().data = std::make_shared<ExtensionDataView>();
                items.back().data->Configure(node);
                ConnectDataAccessibility(child);
            }
            SetWindowSubclass(child, ChildProc, 1, reinterpret_cast<DWORD_PTR>(this));
            SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
            SetWindowTheme(child, high_contrast ? nullptr : dark ? L"DarkMode_Explorer" : L"Explorer", nullptr);
            if (!node.text.empty()) {
                IAccPropServices *accessibility = nullptr;
                if (SUCCEEDED(CoCreateInstance(CLSID_AccPropServices, nullptr, CLSCTX_INPROC_SERVER,
                                               IID_PPV_ARGS(&accessibility)))) {
                    accessibility->SetHwndPropStr(child, static_cast<DWORD>(OBJID_CLIENT), CHILDID_SELF,
                                                  PROPID_ACC_NAME, Wide(node.text).c_str());
                    accessibility->Release();
                }
            }
            if (IsExtensionBooleanControl(node))
                SendMessageW(child, BM_SETCHECK,
                             (node.type == UiNodeType::SidebarSection || node.type == UiNodeType::StagingTray
                                  ? node.expanded
                                  : node.checked)
                                 ? BST_CHECKED
                                 : BST_UNCHECKED,
                             0);
            if (node.type == UiNodeType::Slider || node.type == UiNodeType::SplitPane) {
                SendMessageW(child, TBM_SETRANGE, TRUE,
                             MAKELPARAM(0, node.type == UiNodeType::Slider ? node.stops - 1 : 100));
                SendMessageW(child, TBM_SETPOS, TRUE, node.value);
            }
            if (node.type == UiNodeType::Progress)
                SendMessageW(child, PBM_SETPOS, node.value, 0);
            if (node.type == UiNodeType::TextInput || node.type == UiNodeType::CommandSearch)
                SendMessageW(child, EM_SETLIMITTEXT, 1024, 0);
            if (node.type == UiNodeType::Select || node.type == UiNodeType::List)
                for (const auto &option : node.options)
                    SendMessageW(child, node.type == UiNodeType::List ? LB_ADDSTRING : CB_ADDSTRING, 0,
                                 reinterpret_cast<LPARAM>(Wide(option).c_str()));
            if (node.type == UiNodeType::Select)
                SendMessageW(child, CB_SETCURSEL, node.value, 0);
            if (node.type == UiNodeType::List) {
                SendMessageW(child, LB_SETCURSEL, node.value, 0);
                SendMessageW(child, LB_SETITEMHEIGHT, 0, static_cast<LPARAM>(32 * scale));
            }
            if (node.type == UiNodeType::Segmented) {
                int index = 0;
                for (const auto &option : node.options) {
                    auto title = Wide(option);
                    TCITEMW entry{};
                    entry.mask = TCIF_TEXT;
                    entry.pszText = title.data();
                    TabCtrl_InsertItem(child, index++, &entry);
                }
                TabCtrl_SetCurSel(child, node.value);
            }
            if (node.type == UiNodeType::Tabs) {
                int index = 0;
                for (const auto &tab : node.children) {
                    auto title = Wide(tab.text);
                    TCITEMW entry{};
                    entry.mask = TCIF_TEXT;
                    entry.pszText = title.data();
                    TabCtrl_InsertItem(child, index++, &entry);
                }
            }
        }
        for (const auto &child : node.children)
            Build(child);
    }
    void Layout() {
        if (!host)
            return;
        RECT client{};
        GetClientRect(host, &client);
        ExtensionLayoutCallbacks options;
        options.visible = [&](const UiNode &node) { return node.visible && BooleanBinding(node.visible_binding); };
        options.enabled = [&](const UiNode &node) { return node.enabled && BooleanBinding(node.enabled_binding); };
        options.selected = [&](const UiNode &node) {
            auto item = Find(&node);
            if (!item)
                return 0;
            if (node.type == UiNodeType::SplitPane)
                return static_cast<int>(SendMessageW(item->window, TBM_GETPOS, 0, 0));
            if (node.type == UiNodeType::SidebarSection || node.type == UiNodeType::StagingTray)
                return SendMessageW(item->window, BM_GETCHECK, 0, 0) == BST_CHECKED ? 1 : 0;
            return TabCtrl_GetCurSel(item->window);
        };
        options.feedback_height = [&](const UiNode &node, float width) {
            auto error = validation_errors.find(node.id);
            return error == validation_errors.end()
                       ? 0.0f
                       : 4 * scale + painter.MeasureWrappedCaptionHeight(Wide(error->second), width);
        };
        options.measure = [&](const UiNode &node, float width) {
            auto item = Find(&node);
            auto effective = node;
            if (item && (node.type == UiNodeType::StagingTray || node.type == UiNodeType::SidebarSection))
                effective.expanded = SendMessageW(item->window, BM_GETCHECK, 0, 0) == BST_CHECKED;
            float height =
                ready ? MeasureExtensionControl(effective, item ? Wide(ReadText(item->window)) : Wide(node.text), width,
                                                scale, painter)
                      : std::max(32, node.min_height) * scale;
            auto error = validation_errors.find(node.id);
            if (error != validation_errors.end())
                height += 4 * scale + painter.MeasureWrappedCaptionHeight(Wide(error->second), width);
            return height;
        };
        options.place = [&](const UiNode &node, D2D1_RECT_F bounds, D2D1_RECT_F clip, bool visible, bool enabled) {
            auto item = Find(&node);
            if (!item)
                return;
            auto error = validation_errors.find(node.id);
            if (error != validation_errors.end())
                bounds.bottom -=
                    4 * scale + painter.MeasureWrappedCaptionHeight(Wide(error->second), bounds.right - bounds.left);
            item->bounds = bounds;
            item->clip = clip;
            item->visible = visible;
            item->enabled = enabled;
            int x = static_cast<int>(bounds.left), y = static_cast<int>(bounds.top),
                width = std::max(1, static_cast<int>(bounds.right - bounds.left)),
                height = std::max(1, static_cast<int>(bounds.bottom - bounds.top));
            RECT desired{x, y, x + width, y + height};
            if (node.type == UiNodeType::TextInput || node.type == UiNodeType::CommandSearch) {
                int inset = static_cast<int>((node.type == UiNodeType::CommandSearch ? 34
                                              : node.glyph.empty()                   ? 10
                                                                                     : 34) *
                                             scale);
                float trailing = 10 * scale;
                if (!node.trailing_glyph.empty())
                    trailing += 18 * scale;
                if (!node.keycap.empty())
                    trailing += painter.MeasureBadgeWidth(Wide(node.keycap)) + 6 * scale;
                if (const auto badge = BadgeText(node); !badge.empty())
                    trailing += painter.MeasureBadgeWidth(Wide(badge)) + 6 * scale;
                int line = EditLineHeight(item->window, font, height);
                desired.left = x + inset;
                desired.top = y + (height - line) / 2;
                desired.right = desired.left + std::max(1, width - inset - static_cast<int>(trailing));
                desired.bottom = desired.top + line;
            }
            RECT actual{};
            GetWindowRect(item->window, &actual);
            MapWindowPoints(nullptr, host, reinterpret_cast<POINT *>(&actual), 2);
            if (!EqualRect(&actual, &desired)) {
                MoveWindow(item->window, desired.left, desired.top, desired.right - desired.left,
                           desired.bottom - desired.top, TRUE);
                GetWindowRect(item->window, &actual);
                MapWindowPoints(nullptr, host, reinterpret_cast<POINT *>(&actual), 2);
            }
            int left = std::max(0, static_cast<int>(clip.left - actual.left)),
                top = std::max(0, static_cast<int>(clip.top - actual.top)),
                right = std::max(left, static_cast<int>(std::min(actual.right - actual.left,
                                                                 static_cast<LONG>(clip.right) - actual.left))),
                bottom = std::max(top, static_cast<int>(std::min(actual.bottom - actual.top,
                                                                 static_cast<LONG>(clip.bottom) - actual.top)));
            const RECT desired_clip{left, top, right, bottom};
            // The tree owns each child region. Retry failed applications; keep
            // calculating clipping when geometry is unchanged (scrolling).
            if (!item->clip_applied || !EqualRect(&item->applied_clip, &desired_clip)) {
                HRGN region = CreateRectRgn(left, top, right, bottom);
                if (region) {
                    if (SetWindowRgn(item->window, region, TRUE)) {
                        item->applied_clip = desired_clip;
                        item->clip_applied = true;
                    } else
                        DeleteObject(region);
                }
            }
            const bool window_visible = (GetWindowLongPtrW(item->window, GWL_STYLE) & WS_VISIBLE) != 0;
            if (window_visible != visible)
                ShowWindow(item->window, visible ? SW_SHOW : SW_HIDE);
            const bool window_enabled = enabled && item->command_allowed;
            if ((IsWindowEnabled(item->window) != FALSE) != window_enabled)
                EnableWindow(item->window, window_enabled);
            if (item->data)
                item->data->SetViewport(bounds, scale);
        };
        bool reserved = total > client.bottom;
        auto viewport_for = [&](bool reserve) {
            constexpr float inset = static_cast<float>(kExtensionTreeContentInset);
            return D2D1_RECT_F{inset, inset,
                               std::max(inset + 1, static_cast<float>(client.right) - inset - (reserve ? 14 * scale : 0)),
                               std::max(inset + 1, static_cast<float>(client.bottom) - inset)};
        };
        auto viewport = viewport_for(reserved);
        total = static_cast<int>(
                    std::ceil(layout.Arrange(tree.root, viewport, static_cast<float>(scroll), scale, options))) +
                2 * kExtensionTreeContentInset;
        if ((total > client.bottom) != reserved) {
            viewport = viewport_for(total > client.bottom);
            total = static_cast<int>(
                        std::ceil(layout.Arrange(tree.root, viewport, static_cast<float>(scroll), scale, options))) +
                    2 * kExtensionTreeContentInset;
        }
        int clamped = std::clamp(scroll, 0, std::max(0, total - static_cast<int>(client.bottom)));
        if (clamped != scroll) {
            scroll = clamped;
            layout.Arrange(tree.root, viewport, static_cast<float>(scroll), scale, options);
        }
        HWND focus = GetFocus();
        auto focused = Find(focus);
        if (focused && (!focused->visible || !focused->enabled))
            SetFocus(host);
        InvalidateRect(host, nullptr, FALSE);
    }
};
ExtensionTreeView::ExtensionTreeView() : impl_(std::make_shared<Impl>()) {}
ExtensionTreeView::~ExtensionTreeView() { Close(); }
bool ExtensionTreeView::Create(HWND parent, const extensions::UiTree &tree, ExtensionTreeCallbacks callbacks, bool dark,
                               D2D1_COLOR_F accent, float scale) {
    Close();
    if (!IsWindow(parent) || !std::isfinite(scale) || scale <= 0 || scale > 8)
        return false;
    auto lifetime = impl_;
    auto &self = *lifetime;
    self.owner = this;
    self.tree = tree;
    ++self.state_revision;
    self.callbacks = std::move(callbacks);
    self.scroll = 0;
    self.bar_drag = false;
    self.bar_hot = false;
    self.build_failed = false;
    for (const auto &entry : tree.state)
        self.state[entry.key] = entry.value;
    SetTheme(dark, accent, scale);
    WNDCLASSW type{};
    type.lpfnWndProc = Impl::Proc;
    type.hInstance = GetModuleHandleW(nullptr);
    type.lpszClassName = L"PulseExtensionTree";
    type.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&type);
    CreateWindowExW(WS_EX_CONTROLPARENT, type.lpszClassName, L"", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN, 0, 0, 1, 1,
                    parent, nullptr, type.hInstance, &self);
    if (!self.host)
        return false;
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_WIN95_CLASSES};
    InitCommonControlsEx(&controls);
    self.compositor.SetCompositionAboveChildren(true);
    if (!self.compositor.Init(self.host)) {
        Close();
        return false;
    }
    self.ready = true;
    self.painter.SetCompositor(&self.compositor);
    self.compositor.RecreateTextFormats(scale);
    self.painter.SetScale(scale);
    self.painter.BeginFrame(self.theme, self.high_contrast);
    self.Build(self.tree.root);
    if (self.build_failed) {
        Close();
        return false;
    }
    Refresh();
    return self.host != nullptr;
}
void ExtensionTreeView::Close() {
    auto &self = *impl_;
    if (self.host || !self.state.empty()) ++self.state_revision;
    ++self.generation;
    self.callbacks = {};
    self.ResetStateEvents();
    auto sources = std::move(self.sources);
    self.sources.clear();
    for (auto &[id, source] : sources)
        source->Close();
    self.source_queries.clear();
    self.validation_errors.clear();
    self.composing.clear();
    if (self.menu) {
        self.menu->Dismiss();
        self.menu.reset();
    }
    self.ready = false;
    if (GetCapture() == self.host)
        ReleaseCapture();
    self.bar_drag = false;
    self.refreshing = false;
    if (self.host)
        DestroyWindow(self.host);
    self.host = nullptr;
    self.painter.SetCompositor(nullptr);
    self.compositor.Shutdown();
    self.items.clear();
    self.nodes.clear();
    self.layout.Reset();
    self.nested_drag = nullptr;
    self.state.clear();
    if (self.font)
        DeleteObject(self.font);
    self.font = nullptr;
    if (self.brush)
        DeleteObject(self.brush);
    self.brush = nullptr;
    if (self.edit_brush)
        DeleteObject(self.edit_brush);
    self.edit_brush = nullptr;
}
void ExtensionTreeView::SetBounds(RECT bounds) {
    if (impl_->host)
        MoveWindow(impl_->host, bounds.left, bounds.top, std::max(0L, bounds.right - bounds.left),
                   std::max(0L, bounds.bottom - bounds.top), TRUE);
}
void ExtensionTreeView::SetTheme(bool dark, D2D1_COLOR_F accent, float scale) {
    auto &self = *impl_;
    if (!std::isfinite(scale) || scale <= 0 || scale > 8)
        return;
    HIGHCONTRASTW contrast{};
    contrast.cbSize = sizeof(contrast);
    SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0);
    bool high_contrast = (contrast.dwFlags & HCF_HIGHCONTRASTON) != 0;
    if (self.font && self.dark == dark && self.scale == scale && self.high_contrast == high_contrast &&
        self.accent.r == accent.r && self.accent.g == accent.g && self.accent.b == accent.b &&
        self.accent.a == accent.a)
        return;
    self.accent = accent;
    self.theme = high_contrast ? MakeHighContrastTheme() : MakeTheme(dark, accent);
    if (self.ready) {
        self.compositor.RecreateTextFormats(scale);
        self.painter.SetScale(scale);
        self.painter.InvalidateTypography();
    }
    self.dark = dark;
    self.scale = scale;
    self.high_contrast = high_contrast;
    if (self.brush)
        DeleteObject(self.brush);
    self.brush = CreateSolidBrush(self.Background());
    if (self.edit_brush)
        DeleteObject(self.edit_brush);
    self.edit_brush = CreateSolidBrush(EditBackColor(dark));
    HFONT old = self.font;
    self.font = typography::CreateEditFont(scale);
    for (auto &item : self.items) {
        item.clip_applied = false;
        SendMessageW(item.window, WM_SETFONT, reinterpret_cast<WPARAM>(self.font), TRUE);
        SetWindowTheme(item.window, high_contrast ? nullptr : dark ? L"DarkMode_Explorer" : L"Explorer", nullptr);
        InvalidateRect(item.window, nullptr, TRUE);
    }
    if (old)
        DeleteObject(old);
    self.Layout();
}
void ExtensionTreeView::Refresh() {
    auto lifetime = impl_;
    auto &self = *lifetime;
    unsigned generation = self.generation;
    self.refreshing = true;
    auto items = self.items;
    for (const auto &item : items) {
        auto node = *item.node;
        bool enabled = node.enabled;
        if (!node.command.empty())
            enabled = enabled && self.Allowed(node.command);
        if (node.type == UiNodeType::StagingItem && node.command.empty())
            enabled = false;
        if (self.generation != generation)
            return;
        if (auto current = self.Find(item.window))
            current->command_allowed = enabled;
        std::optional<std::string> value;
        if (node.binding.starts_with("state."))
            value = StateValue(node.binding.substr(6));
        else if (!node.binding.empty()) {
            auto read = self.callbacks.read_binding;
            value = read ? read(node.binding) : std::nullopt;
            if (self.generation != generation)
                return;
            if (!value)
                value = std::string{};
        } else if (!node.state.empty())
            value = StateValue(node.state);
        if (value) {
            int numeric = 0;
            auto parsed = std::from_chars(value->data(), value->data() + value->size(), numeric);
            if (parsed.ec != std::errc{} || parsed.ptr != value->data() + value->size())
                numeric = 0;
            if (item.data)
                item.data->SetSelection(*value);
            else if (node.type == UiNodeType::Slider || node.type == UiNodeType::SplitPane)
                SendMessageW(item.window, TBM_SETPOS, TRUE, numeric);
            else if (node.type == UiNodeType::Select)
                SendMessageW(item.window, CB_SETCURSEL, numeric, 0);
            else if (node.type == UiNodeType::List)
                SendMessageW(item.window, LB_SETCURSEL, numeric, 0);
            else if (node.type == UiNodeType::Progress)
                SendMessageW(item.window, PBM_SETPOS, std::clamp(numeric, 0, 100), 0);
            else if (node.type == UiNodeType::Tabs || node.type == UiNodeType::Segmented)
                TabCtrl_SetCurSel(item.window, numeric);
            else if (IsExtensionBooleanControl(node))
                SendMessageW(item.window, BM_SETCHECK, *value == "true" ? BST_CHECKED : BST_UNCHECKED, 0);
            else if ((node.type == UiNodeType::Text || node.type == UiNodeType::TextInput ||
                      node.type == UiNodeType::CommandSearch) &&
                     !self.composing.contains(item.window) && ReadText(item.window) != *value)
                SetWindowTextW(item.window, Wide(*value).c_str());
        }
        InvalidateRect(item.window, nullptr, TRUE);
    }
    self.refreshing = false;
    self.Layout();
    self.RefreshSources();
}
HWND ExtensionTreeView::Hwnd() const { return impl_->host; }
HWND ExtensionTreeView::NodeWindow(const std::string &id) const {
    for (const auto &item : impl_->items)
        if (item.node->id == id)
            return item.window;
    return nullptr;
}
std::optional<std::string> ExtensionTreeView::StateValue(const std::string &key) const {
    auto entry = impl_->state.find(key);
    return entry == impl_->state.end() ? std::nullopt : std::optional<std::string>(entry->second);
}
bool ExtensionTreeView::InputComposing() const { return !impl_->composing.empty(); }
int ExtensionTreeView::ContentHeight() const {
    return std::max(0, impl_->total - 2 * kExtensionTreeContentInset);
}
uint64_t ExtensionTreeView::StateRevision() const { return impl_->state_revision; }
std::vector<extensions::UiStateEntry> ExtensionTreeView::StateValues() const {
    std::vector<extensions::UiStateEntry> values;
    if (!impl_->host) return values;
    for (const auto& entry : impl_->tree.state) {
        const auto at = impl_->state.find(entry.key);
        if (at != impl_->state.end()) values.push_back({entry.key, at->second});
    }
    return values;
}
bool ExtensionTreeView::SetState(const std::string &key, const std::string &value) { return SetStates({{key, value}}); }
bool ExtensionTreeView::SetStates(const std::vector<extensions::UiStateEntry> &changes) {
    auto lifetime = impl_;
    auto &self = *lifetime;
    Impl::StateNotificationScope notification(self);
    if (!self.host)
        return false;
    std::map<std::string, std::string> pending;
    for (const auto &change : changes) {
        if (!self.state.contains(change.key) || pending.contains(change.key) || !self.Valid(change.key, change.value))
            return false;
        pending.emplace(change.key, change.value);
    }
    bool changed = false;
    for (const auto &[key, value] : pending)
        changed = self.AssignState(key, value) || changed;
    if (changed)
        Refresh();
    return true;
}
bool ExtensionTreeView::UpdateTree(const extensions::UiTree &replacement) {
    auto lifetime = impl_;
    auto &self = *lifetime;
    if (!self.host || !self.composing.empty())
        return false;
    std::map<std::string, const UiNode *> proposed;
    std::function<bool(const UiNode &)> inspect = [&](const UiNode &node) {
        if (node.id.empty() || !proposed.emplace(node.id, &node).second || node.type >= UiNodeType::Count)
            return false;
        for (const auto &child : node.children)
            if (!inspect(child))
                return false;
        return true;
    };
    if (!inspect(replacement.root))
        return false;
    std::map<std::string, std::string> next_state;
    for (const auto &entry : replacement.state) {
        auto old = self.state.find(entry.key);
        auto value = old == self.state.end() ? entry.value : old->second;
        if (!extensions::ValidateUiStateValue(replacement, entry.key, value) ||
            !next_state.emplace(entry.key, value).second)
            return false;
    }
    auto current_content = self.tree;
    for (auto& entry : current_content.state) entry.value = self.state.at(entry.key);
    auto next_content = replacement;
    for (auto& entry : next_content.state) entry.value = next_state.at(entry.key);
    if (current_content == next_content) {
        self.ResetStateEvents();
        return true;
    }
    ++self.state_revision;
    self.ResetStateEvents();
    HWND focus = GetFocus();
    if (self.menu)
        self.menu->Dismiss();
    ++self.generation;
    auto sources = std::move(self.sources);
    self.sources.clear();
    for (auto &[id, source] : sources)
        source->Close();
    self.source_queries.clear();
    if (!self.host)
        return false;
    if (GetCapture() == self.host || IsChild(self.host, GetCapture()))
        ReleaseCapture();
    self.bar_drag = false;
    auto old_tree = std::move(self.tree);
    self.tree = replacement;
    std::map<std::string, const UiNode *> actual;
    std::function<void(const UiNode &)> collect = [&](const UiNode &node) {
        actual[node.id] = &node;
        for (const auto &child : node.children)
            collect(child);
    };
    collect(self.tree.root);
    self.refreshing = true;
    std::vector<Impl::Item> kept;
    for (auto &item : self.items) {
        auto at = actual.find(item.node->id);
        if (at == actual.end() || at->second->type != item.node->type || at->second->options != item.node->options) {
            DestroyWindow(item.window);
            continue;
        }
        item.node = at->second;
        item.accessible.reset();
        if (item.data)
            item.data->UpdateNode(*item.node);
        self.AccessibleName(item.window, Wide(item.node->text));
        if (item.node->type == UiNodeType::Tabs) {
            TabCtrl_DeleteAllItems(item.window);
            int index = 0;
            for (const auto &tab : item.node->children) {
                auto label = Wide(tab.text);
                TCITEMW entry{};
                entry.mask = TCIF_TEXT;
                entry.pszText = label.data();
                TabCtrl_InsertItem(item.window, index++, &entry);
            }
        }
        kept.push_back(std::move(item));
    }
    self.items = std::move(kept);
    self.nodes.clear();
    self.layout.Reset();
    self.nested_drag = nullptr;
    self.state = std::move(next_state);
    self.validation_errors.clear();
    self.Build(self.tree.root);
    for (const auto &item : self.items)
        if (item.data)
            self.ConnectDataAccessibility(item.window);
    self.refreshing = false;
    if (self.build_failed) {
        Close();
        return false;
    }
    Refresh();
    if (IsWindow(focus) && IsWindowEnabled(focus) && IsWindowVisible(focus))
        SetFocus(focus);
    return self.host != nullptr;
}
std::optional<extensions::UiNode> ExtensionTreeView::DataNode(const std::string &id) const {
    const auto node = impl_->Node(id);
    const auto item = node ? impl_->Find(node) : nullptr;
    if (!item || !item->data)
        return std::nullopt;
    auto snapshot = item->data->SnapshotNode();
    snapshot.data_source = node->data_source;
    return snapshot;
}
bool ExtensionTreeView::BindDataSource(const std::string &id, const std::optional<extensions::UiDataSource> &source) {
    auto lifetime = impl_;
    auto &self = *lifetime;
    const auto node = self.Node(id);
    const auto revision = self.generation;
    auto item = node ? self.Find(node) : nullptr;
    if (!self.host || !item || !item->data || !node->rows.empty() || (source && node->data_source))
        return false;
    if (source && !node->state.empty() && !self.state[node->state].empty())
        return false;
    if (source) {
        for (const auto &[key, values] : std::vector<std::pair<std::string, std::vector<std::string>>>{
            {source->status_state, {"idle", "loading", "ready", "error", "cancelled"}},
            {source->error_state, {"", "data_error"}}, {source->progress_state, {"0", "100"}}}) {
            if (!key.empty() && std::any_of(values.begin(), values.end(), [&](const auto &value) {
                    return !extensions::ValidateUiStateValue(self.tree, key, value);
                }))
                return false;
        }
        if (!source->progress_state.empty()) {
            const auto value = self.state[source->progress_state];
            if (value.empty() || value.size() > 3 || (value.size() > 1 && value.front() == '0') ||
                !std::all_of(value.begin(), value.end(), [](char c) { return c >= '0' && c <= '9'; }) ||
                std::stoi(value) > 100)
                return false;
        }
    }
    auto found = self.sources.find(id);
    if (found != self.sources.end()) {
        auto old = std::move(found->second);
        self.sources.erase(found);
        old->Close();
    }
    // Cancellation may synchronously close or replace the tree through native code.
    if (!self.host || self.generation != revision || self.Node(id) != node)
        return false;
    const_cast<UiNode *>(node)->data_source = source;
    self.source_queries.erase(id);
    item->data->UpdateRows({});
    if (!node->state.empty())
        self.AssignState(node->state, "");
    if (source) {
        self.source_queries[id] = (source->filter_state.empty() ? "" : self.state[source->filter_state]) + '\0' +
            (source->sort_state.empty() ? "" : self.state[source->sort_state]);
    }
    self.Layout();
    InvalidateRect(self.host, nullptr, FALSE);
    return true;
}
bool ExtensionTreeView::RequestData(const std::string &id, const std::string &operation) {
    auto lifetime = impl_;
    return lifetime->RequestData(id, operation);
}
bool ExtensionTreeView::RevealNode(const std::string &id) {
    auto lifetime = impl_;
    auto &self = *lifetime;
    std::vector<const UiNode *> path;
    std::function<bool(const UiNode &)> find = [&](const UiNode &node) {
        path.push_back(&node);
        if (node.id == id)
            return true;
        for (const auto &child : node.children)
            if (find(child))
                return true;
        path.pop_back();
        return false;
    };
    if (!find(self.tree.root))
        return false;
    std::vector<extensions::UiStateEntry> changes;
    for (size_t index = 0; index + 1 < path.size(); ++index) {
        if (path[index]->type == UiNodeType::SidebarSection || path[index]->type == UiNodeType::StagingTray) {
            if (!path[index]->state.empty())
                changes.push_back({path[index]->state, "true"});
            else if (auto item = self.Find(path[index]))
                SendMessageW(item->window, BM_SETCHECK, BST_CHECKED, 0);
        }
        if (path[index]->type == UiNodeType::Tabs) {
            auto &children = path[index]->children;
            int selected = static_cast<int>(path[index + 1] - children.data());
            if (!path[index]->state.empty())
                changes.push_back({path[index]->state, std::to_string(selected)});
            else if (auto item = self.Find(path[index]))
                TabCtrl_SetCurSel(item->window, selected);
        }
    }
    if (!changes.empty() && !SetStates(changes))
        return false;
    self.Layout();
    for (auto node : path)
        if (!node->visible || !self.BooleanBinding(node->visible_binding))
            return false;
    if (self.layout.Reveal(path.back()))
        self.Layout();
    auto bounds = self.layout.Bounds(path.back());
    if (!bounds)
        return false;
    RECT client{};
    GetClientRect(self.host, &client);
    if (bounds->top < 8 || bounds->bottom - bounds->top > client.bottom - 16)
        self.scroll += static_cast<int>(bounds->top) - 8;
    else if (bounds->bottom > client.bottom - 8)
        self.scroll += static_cast<int>(bounds->bottom) - client.bottom + 8;
    self.Layout();
    return true;
}
extensions::UiTree ExtensionTreeView::SnapshotTree() const {
    auto snapshot = impl_->tree;
    for (auto &entry : snapshot.state) {
        auto found = impl_->state.find(entry.key);
        if (found != impl_->state.end())
            entry.value = found->second;
    }
    std::function<void(UiNode &)> capture = [&](UiNode &node) {
        auto original = impl_->Node(node.id);
        auto item = original ? impl_->Find(original) : nullptr;
        if (item && item->data)
            node.rows = item->data->SnapshotNode().rows;
        node.data_source.reset();
        if (item && !node.binding.empty() && !node.binding.starts_with("state.")) {
            if (node.type == UiNodeType::Progress)
                node.value = static_cast<int>(SendMessageW(item->window, PBM_GETPOS, 0, 0));
            else
                node.text = ReadText(item->window);
            node.binding.clear();
        }
        for (auto &child : node.children)
            capture(child);
    };
    capture(snapshot.root);
    return snapshot;
}
void ExtensionTreeView::CopySnapshotViewState(const ExtensionTreeView &source) {
    if (!impl_->host || !source.impl_->host)
        return;
    impl_->validation_errors = source.impl_->validation_errors;
    std::map<std::string, bool> commands;
    for (const auto &item : source.impl_->items)
        if (!item.node->command.empty())
            commands[item.node->command] = item.command_allowed;
    impl_->callbacks.on_command = [](const std::string &) {};
    impl_->callbacks.command_enabled = [commands = std::move(commands)](const std::string &id) {
        auto found = commands.find(id);
        return found != commands.end() && found->second;
    };
    impl_->callbacks.request_data = {};
    impl_->callbacks.on_state_changed = {};
    impl_->ResetStateEvents();
    impl_->scroll = static_cast<int>(source.impl_->scroll * impl_->scale / source.impl_->scale);
    for (auto &item : impl_->items) {
        auto original_node = source.impl_->Node(item.node->id);
        auto original = original_node ? source.impl_->Find(original_node) : nullptr;
        if (!original)
            continue;
        item.command_allowed = original->command_allowed;
        if (item.data && original->data)
            item.data->CopyViewState(*original->data);
    }
    impl_->Layout();
}
bool ExtensionTreeView::PaintTo(HDC target, int x, int y) {
    auto lifetime = impl_;
    auto &self = *lifetime;
    if (!self.ready)
        return false;
    self.compositor.CaptureNextFrame();
    if (!self.Render())
        return false;
    auto *frame = self.compositor.HeldFrame();
    if (!frame)
        return false;
    auto size = frame->GetPixelSize();
    ComPtr<ID2D1Bitmap1> readable;
    auto properties = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
                                              frame->GetPixelFormat(), 96, 96);
    if (FAILED(self.compositor.Dc()->CreateBitmap(size, nullptr, 0, &properties, &readable)) ||
        FAILED(readable->CopyFromBitmap(nullptr, frame, nullptr)))
        return false;
    D2D1_MAPPED_RECT mapped{};
    if (FAILED(readable->Map(D2D1_MAP_OPTIONS_READ, &mapped)))
        return false;
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = static_cast<LONG>(mapped.pitch / 4);
    info.bmiHeader.biHeight = -static_cast<LONG>(size.height);
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    SetDIBitsToDevice(target, x, y, size.width, size.height, 0, 0, 0, size.height, mapped.bits, &info, DIB_RGB_COLORS);
    readable->Unmap();
    return true;
}
} // namespace pulse::ui
