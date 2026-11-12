#include "dialogs_adapter_host.h"
#include "../../app_runtime.h"
#include "../../app_state.h"
#include "../../../ui/form_dialog.h"

namespace pulse::extensions::native {

DialogsBackend MakeDialogsBackend(HWND owner, bool dark, D2D1_COLOR_F accent) {
    struct HostState {
        HWND owner = nullptr;
        bool dark = false;
        D2D1_COLOR_F accent{};
        ui::FormDialog dialog;
    };
    auto host = std::make_shared<HostState>();
    host->owner = owner;
    host->dark = dark;
    host->accent = accent;

    DialogsBackend backend;
    backend.valid_owner = [host] { return IsWindow(host->owner) != FALSE; };
    backend.show = [host](DialogPresentationSpec src) -> bool {
        if (const auto* app = GetAppState(host->owner)) {
            host->dark = app->darkMode;
            host->accent = app->accentColor;
        }
        ui::FormDialogSpec spec;
        spec.title = std::move(src.title);
        spec.message = std::move(src.message);
        spec.heading = std::move(src.heading);
        spec.icon = static_cast<ui::FormDialogIcon>(src.legacy_icon);
        spec.extension_icon = src.extension_icon;
        spec.subject_kind = static_cast<ui::FormDialogSubject>(src.subject_kind);
        spec.subject = std::move(src.subject);
        spec.subject_detail = std::move(src.subject_detail);
        spec.footnote = std::move(src.footnote);
        spec.footnote_icon = static_cast<ui::FormDialogIcon>(src.legacy_footnote_icon);
        spec.extension_footnote_icon = src.extension_footnote_icon;
        spec.form = std::move(src.form);
        spec.buttons.reserve(src.buttons.size());
        for (auto& b : src.buttons) {
            ui::FormDialogButton item;
            item.text = std::move(b.text);
            item.kind = b.kind == PULSE_DIALOG_BUTTON_PRIMARY ? ui::FormDialogButtonKind::Primary
                : b.kind == PULSE_DIALOG_BUTTON_DANGER ? ui::FormDialogButtonKind::Danger
                : ui::FormDialogButtonKind::Standard;
            spec.buttons.push_back(std::move(item));
        }
        spec.default_button = src.default_button;
        spec.cancel_button = src.cancel_button;
        spec.width_dip = src.width_dip;
        spec.form_height_dip = src.form_height_dip;
        spec.on_states_changed = std::move(src.on_states_changed);
        spec.on_command = std::move(src.on_command);
        spec.command_enabled = std::move(src.command_enabled);
        spec.on_closed = std::move(src.on_closed);
        return host->dialog.Show(host->owner, std::move(spec), host->dark, host->accent);
    };
    backend.set_states = [host](const std::vector<extensions::UiStateEntry>& entries) {
        return host->dialog.SetStates(entries);
    };
    backend.update_form = [host](const extensions::UiTree& form) {
        return host->dialog.UpdateForm(form);
    };
    backend.get_state = [host](const std::string& key) {
        return host->dialog.StateValue(key);
    };
    backend.refresh_form = [host] {
        host->dialog.RefreshForm();
    };
    backend.dismiss = [host] {
        host->dialog.Dismiss();
    };
    backend.destroy = [host] {
        host->dialog.Destroy();
    };
    backend.is_open = [host] {
        return host->dialog.IsOpen();
    };
    backend.active_hwnd = [host] {
        return host->dialog.Hwnd();
    };
    backend.set_appearance = [host](bool is_dark, D2D1_COLOR_F color) {
        host->dark = is_dark;
        host->accent = color;
    };
    return backend;
}

DialogsAdapter::DialogsAdapter(HWND owner, bool dark, D2D1_COLOR_F accent, std::function<bool()> alive,
                               std::shared_ptr<DiagnosticsSink> diagnostics)
    : DialogsAdapter(MakeDialogsBackend(owner, dark, accent), std::move(alive), std::move(diagnostics)) {}

} // namespace pulse::extensions::native
