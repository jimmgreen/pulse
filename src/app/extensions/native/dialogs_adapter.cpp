#include "dialogs_adapter.h"
#include "diagnostics_adapter.h"
#include "ui_model.h"
#include "../icon_registry.h"
#include <algorithm>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace pulse::extensions::native {
namespace {
bool ValidText(PulseStringView value, size_t max_bytes, bool required) {
    if (!value.data) return !required && value.length == 0;
    if (value.length > max_bytes || (required && value.length == 0)) return false;
    return std::memchr(value.data, 0, value.length) == nullptr;
}
bool ValidUtf8(PulseStringView value, size_t max_bytes, bool required, std::string& out) {
    out.clear();
    if (!ValidText(value, max_bytes, required)) return false;
    if (!value.length) return true;
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data, static_cast<int>(value.length), nullptr, 0))
        return false;
    out.assign(value.data, value.length);
    return true;
}
bool ValidIdentifier(PulseStringView value, std::string& out) {
    if (!ValidUtf8(value, 64, true, out)) return false;
    return std::all_of(out.begin(), out.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
               c == '.' || c == '_' || c == '-';
    });
}
bool Wide(PulseStringView value, std::wstring& out) {
    out.clear();
    if (!value.length) return true;
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data, static_cast<int>(value.length), nullptr, 0);
    if (count <= 0) return false;
    out.resize(static_cast<size_t>(count));
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data, static_cast<int>(value.length), out.data(), count);
    return true;
}
PulseStringView View(const std::string& value) {
    return {value.empty() ? "" : value.data(), static_cast<uint32_t>(value.size())};
}
std::vector<PulseUiState> Views(const std::vector<extensions::UiStateEntry>& entries) {
    std::vector<PulseUiState> views;
    views.reserve(entries.size());
    for (const auto& entry : entries) views.push_back({View(entry.key), View(entry.value)});
    return views;
}
} // namespace

struct DialogsAdapter::State : std::enable_shared_from_this<State> {
    struct CommandSubscription {
        uint64_t id = 0;
        std::string command;
        PulseDialogCommandFn callback = nullptr;
        void* user = nullptr;
    };

    DWORD thread = GetCurrentThreadId();
    DialogsBackend backend;
    std::function<bool()> alive;
    std::shared_ptr<DiagnosticsSink> diagnostics;
    bool closed = false;
    int callbacks = 0;
    uint64_t next_id = 1;
    uint64_t next_sub_id = 1;
    uint64_t open_id = 0;
    bool has_form = false;
    std::set<std::string> declared_commands;
    std::map<uint64_t, CommandSubscription> command_subscriptions;

    State(DialogsBackend impl, std::function<bool()> live, std::shared_ptr<DiagnosticsSink> sink)
        : backend(std::move(impl)), alive(std::move(live)), diagnostics(std::move(sink)) {}

    bool Valid() const {
        return GetCurrentThreadId() == thread && !closed &&
               (!backend.valid_owner || backend.valid_owner()) &&
               (!alive || alive());
    }
    bool IsDialogOpen() const {
        return open_id != 0 && backend.is_open && backend.is_open();
    }
    bool Open(uint64_t id) const {
        return id && id == open_id && IsDialogOpen();
    }
    bool HasCommandSubscription(const std::string& command) const {
        return declared_commands.contains(command) &&
               std::any_of(command_subscriptions.begin(), command_subscriptions.end(),
                           [&](const auto& entry) { return entry.second.command == command; });
    }
    void ClearActiveDialogState() {
        open_id = 0;
        has_form = false;
        declared_commands.clear();
        command_subscriptions.clear();
    }

    PulseStatus Fail(PulseStatus status, std::string_view op, std::string_view subject,
                     std::string_view field, std::string_view msg) const {
        if (diagnostics) diagnostics->Record(status, PULSE_DIALOGS_INTERFACE_NAME, op, subject, field, msg);
        return status;
    }
    PulseStatus FailParsed(PulseStatus status, std::string_view op, std::string_view formatted,
                           std::string_view default_subject = {}) const {
        if (diagnostics) diagnostics->RecordParsed(status, PULSE_DIALOGS_INTERFACE_NAME, op, formatted, default_subject);
        return status;
    }

    template<class F> static PulseStatus Guard(void* context, F action) noexcept {
        if (!context) return PULSE_ERROR_ARGUMENT;
        try {
            auto self = static_cast<State*>(context)->shared_from_this();
            return self->Valid() ? action(*self) : PULSE_ERROR_STATE;
        } catch (...) {
            return PULSE_ERROR_STATE;
        }
    }

    void DispatchCommand(uint64_t dialog_id, const std::string& command) {
        auto keep = shared_from_this();
        if (!Valid() || !Open(dialog_id)) return;
        std::vector<uint64_t> tokens;
        for (const auto& [sub_id, sub] : command_subscriptions) {
            if (sub.command == command) tokens.push_back(sub_id);
        }
        for (const uint64_t token : tokens) {
            if (!Valid() || !Open(dialog_id)) break;
            const auto found = command_subscriptions.find(token);
            if (found == command_subscriptions.end()) continue;
            const auto sub = found->second;
            ++callbacks;
            try { sub.callback(sub.user, dialog_id, View(command)); } catch (...) {}
            --callbacks;
        }
    }

    static PulseStatus ShowImpl(void* context, const PulseDialogRequest* request, PulseDialogStateChanged changed,
                                PulseDialogClosed done, void* user, uint64_t* out, bool allow_commands) noexcept {
        if (out) *out = 0;
        return Guard(context, [&](State& self) -> PulseStatus {
            if (!request || !out || !done || request->size < PULSE_DIALOG_REQUEST_SIZE_V1)
                return self.Fail(PULSE_ERROR_ARGUMENT, "show", "", "size",
                                 "null request/callback/output or invalid PulseDialogRequest size");
            if (self.IsDialogOpen())
                return self.Fail(PULSE_ERROR_STATE, "show", "", "", "a dialog is already open for this session");
            const auto& r = *request;
            if (r.button_count < 1 || r.button_count > 3 || !r.buttons)
                return self.Fail(PULSE_ERROR_ARGUMENT, "show", "", "buttons", "dialog requires 1..3 footer buttons");
            if (r.width > 720 || r.height > 640)
                return self.Fail(PULSE_ERROR_ARGUMENT, "show", "", r.width > 720 ? "width" : "height",
                                 "dialog width (<=720) or height (<=640) out of bounds");
            if ((r.default_button != PULSE_DIALOG_NO_BUTTON && r.default_button >= r.button_count) ||
                (r.cancel_button != PULSE_DIALOG_NO_BUTTON && r.cancel_button >= r.button_count))
                return self.Fail(PULSE_ERROR_ARGUMENT, "show", "", "default_button",
                                 "default_button or cancel_button index out of range");
            if (!ValidText(r.title, 128, true) || !ValidText(r.message, 2048, false))
                return self.Fail(PULSE_ERROR_ARGUMENT, "show", "", !ValidText(r.title, 128, true) ? "title" : "message",
                                 "invalid title (1..128 bytes) or message (0..2048 bytes)");
            DialogPresentationSpec spec;
            if (!Wide(r.title, spec.title) || !Wide(r.message, spec.message))
                return self.Fail(PULSE_ERROR_ARGUMENT, "show", "", "title", "invalid UTF-8 in title or message");
            if (r.size >= sizeof(PulseDialogRequest)) {
                auto decode_icon = [](uint32_t raw, uint32_t& legacy, std::optional<uint32_t>& ext) -> bool {
                    legacy = PULSE_DIALOG_ICON_NONE;
                    ext.reset();
                    if ((raw & PULSE_DIALOG_ICON_EXT_FLAG) != 0) {
                        const uint32_t id = raw & ~PULSE_DIALOG_ICON_EXT_FLAG;
                        if (!icons::GlobalRegistry().Exists(id)) return false;
                        ext = id;
                        return true;
                    }
                    if (raw > PULSE_DIALOG_ICON_LOCK) return false;
                    legacy = raw;
                    return true;
                };
                if (!decode_icon(r.icon, spec.legacy_icon, spec.extension_icon) ||
                    !decode_icon(r.footnote_icon, spec.legacy_footnote_icon, spec.extension_footnote_icon))
                    return self.Fail(PULSE_ERROR_ARGUMENT, "show", "", "icon", "unknown dialog icon identifier");
                if (r.subject_kind > PULSE_DIALOG_SUBJECT_ARCHIVE || !ValidText(r.heading, 128, false) ||
                    !ValidText(r.subject, 260, false) || !ValidText(r.subject_detail, 256, false) ||
                    !ValidText(r.footnote, 256, false) || !Wide(r.heading, spec.heading) ||
                    !Wide(r.subject, spec.subject) || !Wide(r.subject_detail, spec.subject_detail) ||
                    !Wide(r.footnote, spec.footnote))
                    return self.Fail(PULSE_ERROR_ARGUMENT, "show", "", "heading",
                                     "invalid extended header/subject/footnote field");
                spec.subject_kind = r.subject_kind;
            }
            std::vector<std::string> ids;
            std::set<std::string> unique;
            for (uint32_t i = 0; i < r.button_count; ++i) {
                const auto& button = r.buttons[i];
                const std::string btn_subject = "buttons[" + std::to_string(i) + "]";
                if (button.size < sizeof(PulseDialogButton) || button.kind > PULSE_DIALOG_BUTTON_DANGER ||
                    !ValidText(button.id, 64, true) || !ValidText(button.text, 96, true))
                    return self.Fail(PULSE_ERROR_ARGUMENT, "show", btn_subject, "id", "invalid footer button spec");
                DialogButtonSpec item;
                if (!Wide(button.text, item.text) || item.text.size() > 32)
                    return self.Fail(PULSE_ERROR_ARGUMENT, "show", btn_subject, "text",
                                     "button text must be valid UTF-8 and <=32 UTF-16 code units");
                item.kind = button.kind;
                item.id.assign(button.id.data, button.id.length);
                if (!unique.insert(item.id).second)
                    return self.Fail(PULSE_ERROR_ARGUMENT, "show", btn_subject, "id", "duplicate footer button id");
                ids.push_back(item.id);
                spec.buttons.push_back(std::move(item));
            }
            std::set<std::string> model_commands;
            if (r.model) {
                extensions::UiTree tree;
                std::string error;
                const auto converted = ConvertUiModel(r.model, tree, model_commands, error);
                if (converted != PULSE_OK)
                    return self.FailParsed(converted, "show", error, "model");
                if (!allow_commands && !model_commands.empty())
                    return self.Fail(PULSE_ERROR_UNAVAILABLE, "show", "model", "commands",
                                     "pulse.dialogs V1 rejects model commands; query pulse.dialogs version 2");
                spec.form = std::move(tree);
                spec.form_height_dip = r.height ? static_cast<float>(r.height) : 240.0f;
            }
            spec.width_dip = r.width ? static_cast<float>(r.width) : 480.0f;
            spec.default_button = r.default_button == PULSE_DIALOG_NO_BUTTON ? -1 : static_cast<int>(r.default_button);
            spec.cancel_button = r.cancel_button == PULSE_DIALOG_NO_BUTTON ? -1 : static_cast<int>(r.cancel_button);
            const uint64_t id = self.next_id++;
            std::weak_ptr<State> weak = self.shared_from_this();
            if (changed) {
                spec.on_states_changed = [weak, id, changed, user](const std::vector<extensions::UiStateEntry>& entries) {
                    const auto state = weak.lock();
                    if (!state || !state->Valid() || state->open_id != id) return;
                    const auto views = Views(entries);
                    ++state->callbacks;
                    try { changed(user, id, views.data(), static_cast<uint32_t>(views.size())); } catch (...) {}
                    --state->callbacks;
                };
            }
            if (allow_commands) {
                spec.on_command = [weak, id](const std::string& command) {
                    if (const auto state = weak.lock()) state->DispatchCommand(id, command);
                };
                spec.command_enabled = [weak, id](const std::string& command) {
                    const auto state = weak.lock();
                    return state && state->Valid() && state->open_id == id &&
                           state->HasCommandSubscription(command);
                };
            }
            spec.on_closed = [weak, id, done, user, ids](int button, std::vector<extensions::UiStateEntry> states) {
                const auto state = weak.lock();
                if (!state || state->open_id != id) return;
                state->ClearActiveDialogState();
                if (!state->Valid()) return;
                const auto views = Views(states);
                const std::string pressed = button >= 0 && static_cast<size_t>(button) < ids.size()
                    ? ids[static_cast<size_t>(button)] : std::string{};
                ++state->callbacks;
                try {
                    done(user, id, button >= 0 ? PULSE_OK : PULSE_ERROR_STATE, View(pressed), views.data(),
                         static_cast<uint32_t>(views.size()));
                } catch (...) {}
                --state->callbacks;
            };
            self.open_id = id;
            self.has_form = spec.form.has_value();
            self.declared_commands = std::move(model_commands);
            self.command_subscriptions.clear();
            if (!self.backend.show || !self.backend.show(std::move(spec))) {
                self.ClearActiveDialogState();
                return self.Fail(PULSE_ERROR_INITIALIZATION, "show", "", "", "failed to present modal dialog window");
            }
            *out = id;
            return PULSE_OK;
        });
    }

    static PulseStatus PULSE_CALL ShowV1(void* context, const PulseDialogRequest* request,
                                         PulseDialogStateChanged changed, PulseDialogClosed done,
                                         void* user, uint64_t* out) noexcept {
        return ShowImpl(context, request, changed, done, user, out, false);
    }

    static PulseStatus PULSE_CALL ShowV2(void* context, const PulseDialogRequest* request,
                                         PulseDialogStateChanged changed, PulseDialogClosed done,
                                         void* user, uint64_t* out) noexcept {
        return ShowImpl(context, request, changed, done, user, out, true);
    }

    static PulseStatus PULSE_CALL SetStates(void* context, uint64_t id, const PulseUiState* states, uint32_t count) noexcept {
        return Guard(context, [&](State& self) -> PulseStatus {
            if (!self.Open(id))
                return self.Fail(PULSE_ERROR_STATE, "set_states", "", "dialog", "dialog is not open");
            if ((count && !states) || count > 256)
                return self.Fail(PULSE_ERROR_ARGUMENT, "set_states", "", "states", "invalid state array or count > 256");
            std::vector<extensions::UiStateEntry> entries;
            entries.reserve(count);
            for (uint32_t i = 0; i < count; ++i) {
                if (!ValidText(states[i].key, 128, true) || !ValidText(states[i].value, 65536, false))
                    return self.Fail(PULSE_ERROR_ARGUMENT, "set_states", "states[" + std::to_string(i) + "]", "key",
                                     "invalid state key or value");
                entries.push_back({std::string(states[i].key.data, states[i].key.length),
                                   states[i].value.length ? std::string(states[i].value.data, states[i].value.length) : std::string{}});
            }
            if (!self.backend.set_states || !self.backend.set_states(entries))
                return self.Fail(PULSE_ERROR_ARGUMENT, "set_states", "", "states",
                                 "undeclared state key or value rejected by dialog form");
            return PULSE_OK;
        });
    }

    static PulseStatus PULSE_CALL CloseDialog(void* context, uint64_t id) noexcept {
        return Guard(context, [&](State& self) -> PulseStatus {
            if (!self.Open(id))
                return self.Fail(PULSE_ERROR_STATE, "close", "", "dialog", "dialog is not open");
            if (self.backend.dismiss) self.backend.dismiss();
            return PULSE_OK;
        });
    }

    static PulseStatus PULSE_CALL SubscribeCommand(void* context, uint64_t id, PulseStringView command,
                                                   PulseDialogCommandFn callback, void* user,
                                                   uint64_t* subscription) noexcept {
        if (subscription) *subscription = 0;
        return Guard(context, [&](State& self) -> PulseStatus {
            if (!self.Open(id))
                return self.Fail(PULSE_ERROR_STATE, "subscribe_command", "", "dialog", "dialog is not open");
            std::string cmd;
            if (!subscription || !callback || !ValidIdentifier(command, cmd))
                return self.Fail(PULSE_ERROR_ARGUMENT, "subscribe_command", "", "command",
                                 "invalid command identifier, callback, or subscription output pointer");
            if (!self.declared_commands.contains(cmd))
                return self.Fail(PULSE_ERROR_ARGUMENT, "subscribe_command", cmd, "command",
                                 "command is not declared in dialog model.commands");
            if (self.command_subscriptions.size() >= 128)
                return self.Fail(PULSE_ERROR_STATE, "subscribe_command", cmd, "",
                                 "dialog command subscription limit (128) reached");
            const uint64_t sub_id = self.next_sub_id++;
            self.command_subscriptions.emplace(sub_id, CommandSubscription{sub_id, std::move(cmd), callback, user});
            *subscription = sub_id;
            if (self.backend.refresh_form) self.backend.refresh_form();
            return PULSE_OK;
        });
    }

    static PulseStatus PULSE_CALL UnsubscribeCommand(void* context, uint64_t subscription) noexcept {
        return Guard(context, [&](State& self) -> PulseStatus {
            if (self.command_subscriptions.erase(subscription) == 0)
                return self.Fail(PULSE_ERROR_STATE, "unsubscribe_command", "", "subscription",
                                 "unknown or expired dialog command subscription");
            if (self.backend.refresh_form) self.backend.refresh_form();
            return PULSE_OK;
        });
    }

    static PulseStatus PULSE_CALL UpdateModel(void* context, uint64_t id, const PulseUiModelTree* model) noexcept {
        return Guard(context, [&](State& self) -> PulseStatus {
            if (!self.Open(id) || !self.has_form)
                return self.Fail(PULSE_ERROR_STATE, "update_model", "", "dialog",
                                 "dialog is not open with a form model");
            extensions::UiTree tree;
            std::set<std::string> commands;
            std::string error;
            const auto status = ConvertUiModel(model, tree, commands, error);
            if (status != PULSE_OK)
                return self.FailParsed(status, "update_model", error, "model");
            if (!self.backend.update_form || !self.backend.update_form(tree))
                return self.Fail(PULSE_ERROR_STATE, "update_model", "", "model",
                                 "dialog rejected tree update (incompatible stateful control identity)");
            self.declared_commands = std::move(commands);
            std::erase_if(self.command_subscriptions,
                          [&](const auto& entry) { return !self.declared_commands.contains(entry.second.command); });
            if (self.backend.refresh_form) self.backend.refresh_form();
            return PULSE_OK;
        });
    }

    static PulseStatus PULSE_CALL GetState(void* context, uint64_t id, PulseStringView key_view,
                                           char* buffer, uint32_t capacity, uint32_t* needed) noexcept {
        return Guard(context, [&](State& self) -> PulseStatus {
            if (!self.Open(id) || !self.has_form)
                return self.Fail(PULSE_ERROR_STATE, "get_state", "", "dialog", "dialog is not open with a form model");
            std::string key;
            if (!needed || !ValidUtf8(key_view, 128, true, key) || (!buffer && capacity))
                return self.Fail(PULSE_ERROR_ARGUMENT, "get_state", "", "key",
                                 "invalid state key or output buffer arguments");
            const auto value = self.backend.get_state ? self.backend.get_state(key) : std::nullopt;
            if (!value)
                return self.Fail(PULSE_ERROR_ARGUMENT, "get_state", key, "key",
                                 "state key is not declared in dialog model");
            *needed = static_cast<uint32_t>(value->size());
            if (!buffer && !capacity) return PULSE_OK;
            if (capacity < value->size())
                return self.Fail(PULSE_ERROR_ARGUMENT, "get_state", key, "capacity",
                                 "buffer capacity is smaller than state value length");
            if (!value->empty()) std::memcpy(buffer, value->data(), value->size());
            return PULSE_OK;
        });
    }
};

DialogsAdapter::DialogsAdapter(DialogsBackend backend, std::function<bool()> alive,
                               std::shared_ptr<DiagnosticsSink> diagnostics)
    : state_(std::make_shared<State>(std::move(backend), std::move(alive), std::move(diagnostics))) {}

DialogsAdapter::~DialogsAdapter() { Close(); }

void DialogsAdapter::SetDiagnostics(std::shared_ptr<DiagnosticsSink> sink) {
    if (state_) state_->diagnostics = std::move(sink);
}

PulseStatus DialogsAdapter::Query(uint32_t version, void* out, uint32_t size) const noexcept {
    try {
        if (!state_->Valid()) return PULSE_ERROR_STATE;
        if (version == PULSE_DIALOGS_VERSION_1) {
            if (!out || size < sizeof(PulseDialogsV1)) return PULSE_ERROR_ARGUMENT;
            const PulseDialogsV1 api{
                sizeof(PulseDialogsV1),
                PULSE_DIALOGS_VERSION_1,
                state_.get(),
                State::ShowV1,
                State::SetStates,
                State::CloseDialog,
            };
            std::memcpy(out, &api, sizeof(api));
            return PULSE_OK;
        }
        if (version == PULSE_DIALOGS_VERSION_2) {
            if (!out || size < sizeof(PulseDialogsV2)) return PULSE_ERROR_ARGUMENT;
            const PulseDialogsV2 api{
                sizeof(PulseDialogsV2),
                PULSE_DIALOGS_VERSION_2,
                state_.get(),
                State::ShowV2,
                State::SetStates,
                State::CloseDialog,
                State::SubscribeCommand,
                State::UnsubscribeCommand,
                State::UpdateModel,
                State::GetState,
            };
            std::memcpy(out, &api, sizeof(api));
            return PULSE_OK;
        }
        return PULSE_ERROR_ABI;
    } catch (...) {
        return PULSE_ERROR_STATE;
    }
}

void DialogsAdapter::SetAppearance(bool dark, D2D1_COLOR_F accent) {
    if (state_ && state_->backend.set_appearance) state_->backend.set_appearance(dark, accent);
}

void DialogsAdapter::Close() {
    if (!state_ || GetCurrentThreadId() != state_->thread || state_->closed) return;
    state_->closed = true;
    state_->ClearActiveDialogState();
    if (state_->backend.destroy) state_->backend.destroy();
}

bool DialogsAdapter::CallbackActive() const {
    return state_ && state_->callbacks > 0;
}

HWND DialogsAdapter::ActiveModalWindow() const noexcept {
    if (!state_ || !state_->Valid() || !state_->IsDialogOpen() || !state_->backend.active_hwnd) return nullptr;
    return state_->backend.active_hwnd();
}

} // namespace pulse::extensions::native
