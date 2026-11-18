#include "dialogs_adapter.h"
#include "ui_model.h"
#include "../../app_runtime.h"
#include "../../app_state.h"
#include "../../../ui/form_dialog.h"
#include <cstring>
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
bool Wide(PulseStringView value, std::wstring& out) {
    out.clear();
    if (!value.length) return true;
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data, static_cast<int>(value.length), nullptr, 0);
    if (count <= 0) return false;
    out.resize(static_cast<size_t>(count));
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data, static_cast<int>(value.length), out.data(), count);
    return true;
}
PulseStringView View(const std::string& value) { return {value.data(), static_cast<uint32_t>(value.size())}; }
std::vector<PulseUiState> Views(const std::vector<extensions::UiStateEntry>& entries) {
    std::vector<PulseUiState> views;
    views.reserve(entries.size());
    for (const auto& entry : entries) views.push_back({View(entry.key), View(entry.value)});
    return views;
}
} // namespace

struct DialogsAdapter::State : std::enable_shared_from_this<State> {
    DWORD thread = GetCurrentThreadId();
    HWND owner;
    bool dark;
    D2D1_COLOR_F accent;
    std::function<bool()> alive;
    bool closed = false;
    int callbacks = 0;
    uint64_t next_id = 1, open_id = 0;
    ui::FormDialog dialog;
    State(HWND window, bool is_dark, D2D1_COLOR_F color, std::function<bool()> live)
        : owner(window), dark(is_dark), accent(color), alive(std::move(live)) {}
    bool Valid() const { return GetCurrentThreadId() == thread && !closed && IsWindow(owner) && alive && alive(); }
    bool Open(uint64_t id) const { return id && id == open_id && dialog.IsOpen(); }
    template<class F> static PulseStatus Guard(void* context, F action) noexcept {
        if (!context) return PULSE_ERROR_ARGUMENT;
        try {
            auto self = static_cast<State*>(context)->shared_from_this();
            return self->Valid() ? action(*self) : PULSE_ERROR_STATE;
        } catch (...) { return PULSE_ERROR_STATE; }
    }

    static PulseStatus PULSE_CALL Show(void* context, const PulseDialogRequest* request, PulseDialogStateChanged changed,
                                       PulseDialogClosed done, void* user, uint64_t* out) {
        if (out) *out = 0;
        return Guard(context, [&](State& self) -> PulseStatus {
            if (!request || !out || !done || request->size < sizeof(PulseDialogRequest)) return PULSE_ERROR_ARGUMENT;
            if (self.dialog.IsOpen()) return PULSE_ERROR_STATE;
            const auto& r = *request;
            if (r.button_count < 1 || r.button_count > 3 || !r.buttons || r.width > 720 || r.height > 640 ||
                (r.default_button != PULSE_DIALOG_NO_BUTTON && r.default_button >= r.button_count) ||
                (r.cancel_button != PULSE_DIALOG_NO_BUTTON && r.cancel_button >= r.button_count) ||
                !ValidText(r.title, 128, true) || !ValidText(r.message, 2048, false))
                return PULSE_ERROR_ARGUMENT;
            ui::FormDialogSpec spec;
            if (!Wide(r.title, spec.title) || !Wide(r.message, spec.message)) return PULSE_ERROR_ARGUMENT;
            std::vector<std::string> ids;
            std::set<std::string> unique;
            for (uint32_t i = 0; i < r.button_count; ++i) {
                const auto& button = r.buttons[i];
                if (button.size < sizeof(PulseDialogButton) || button.kind > PULSE_DIALOG_BUTTON_DANGER ||
                    !ValidText(button.id, 64, true) || !ValidText(button.text, 96, true)) return PULSE_ERROR_ARGUMENT;
                ui::FormDialogButton item;
                if (!Wide(button.text, item.text) || item.text.size() > 32) return PULSE_ERROR_ARGUMENT;
                item.kind = button.kind == PULSE_DIALOG_BUTTON_PRIMARY ? ui::FormDialogButtonKind::Primary
                    : button.kind == PULSE_DIALOG_BUTTON_DANGER ? ui::FormDialogButtonKind::Danger
                    : ui::FormDialogButtonKind::Standard;
                ids.emplace_back(button.id.data, button.id.length);
                if (!unique.insert(ids.back()).second) return PULSE_ERROR_ARGUMENT;
                spec.buttons.push_back(std::move(item));
            }
            if (r.model) {
                extensions::UiTree tree;
                std::set<std::string> commands;
                std::string error;
                const auto converted = ConvertUiModel(r.model, tree, commands, error);
                if (converted != PULSE_OK) return converted;
                if (!commands.empty()) return PULSE_ERROR_UNAVAILABLE;
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
            spec.on_closed = [weak, id, done, user, ids](int button, std::vector<extensions::UiStateEntry> states) {
                const auto state = weak.lock();
                if (!state || state->open_id != id) return;
                state->open_id = 0;
                if (!state->Valid()) return;
                const auto views = Views(states);
                const std::string pressed = button >= 0 && static_cast<size_t>(button) < ids.size() ? ids[static_cast<size_t>(button)] : std::string{};
                ++state->callbacks;
                try {
                    done(user, id, button >= 0 ? PULSE_OK : PULSE_ERROR_STATE, View(pressed), views.data(),
                         static_cast<uint32_t>(views.size()));
                } catch (...) {}
                --state->callbacks;
            };
            self.open_id = id;
            // The theme and accent can change while Pulse runs: use the window's current ones.
            if (const auto* app = GetAppState(self.owner)) { self.dark = app->darkMode; self.accent = app->accentColor; }
            if (!self.dialog.Show(self.owner, std::move(spec), self.dark, self.accent)) {
                self.open_id = 0;
                return PULSE_ERROR_INITIALIZATION;
            }
            *out = id;
            return PULSE_OK;
        });
    }

    static PulseStatus PULSE_CALL SetStates(void* context, uint64_t id, const PulseUiState* states, uint32_t count) {
        return Guard(context, [&](State& self) -> PulseStatus {
            if (!self.Open(id)) return PULSE_ERROR_STATE;
            if ((count && !states) || count > 256) return PULSE_ERROR_ARGUMENT;
            std::vector<extensions::UiStateEntry> entries;
            for (uint32_t i = 0; i < count; ++i) {
                if (!ValidText(states[i].key, 128, true) || !ValidText(states[i].value, 65536, false)) return PULSE_ERROR_ARGUMENT;
                entries.push_back({std::string(states[i].key.data, states[i].key.length),
                                   states[i].value.length ? std::string(states[i].value.data, states[i].value.length) : std::string{}});
            }
            return self.dialog.SetStates(entries) ? PULSE_OK : PULSE_ERROR_ARGUMENT;
        });
    }

    static PulseStatus PULSE_CALL CloseDialog(void* context, uint64_t id) {
        return Guard(context, [&](State& self) -> PulseStatus {
            if (!self.Open(id)) return PULSE_ERROR_STATE;
            self.dialog.Dismiss();
            return PULSE_OK;
        });
    }
};

DialogsAdapter::DialogsAdapter(HWND owner, bool dark, D2D1_COLOR_F accent, std::function<bool()> alive)
    : state_(std::make_shared<State>(owner, dark, accent, std::move(alive))) {}
DialogsAdapter::~DialogsAdapter() { Close(); }
PulseStatus DialogsAdapter::Query(uint32_t version, void* out, uint32_t size) const noexcept {
    try {
        if (!state_->Valid()) return PULSE_ERROR_STATE;
        if (version != PULSE_DIALOGS_VERSION_1) return PULSE_ERROR_ABI;
        if (!out || size < sizeof(PulseDialogsV1)) return PULSE_ERROR_ARGUMENT;
        const PulseDialogsV1 api{sizeof(api), PULSE_DIALOGS_VERSION_1, state_.get(), State::Show, State::SetStates, State::CloseDialog};
        std::memcpy(out, &api, sizeof(api));
        return PULSE_OK;
    } catch (...) { return PULSE_ERROR_STATE; }
}
void DialogsAdapter::SetAppearance(bool dark, D2D1_COLOR_F accent) { state_->dark = dark; state_->accent = accent; }
void DialogsAdapter::Close() {
    if (GetCurrentThreadId() != state_->thread || state_->closed) return;
    state_->closed = true;
    state_->open_id = 0;
    state_->dialog.Destroy();
}
bool DialogsAdapter::CallbackActive() const { return state_->callbacks > 0; }
}
