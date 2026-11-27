#include "settings_context_extension.h"
#include "app_internal.h"
#include "system_ctx_model.h"
#include "../ui/form_dialog.h"
#include "../common/json_value.h"
#include "../common/localization.h"

namespace pulse {
namespace {
using I = l10n::StringId;
bool ShowExtensionForm(AppState& state, std::wstring initial, bool invalid) {
    const HWND owner = state.hwnd;
    const AppState* identity = &state;
    auto dialog = std::make_shared<ui::FormDialog>();
    ui::FormDialogSpec spec;
    spec.title = l10n::Get(I::ContextTreeOtherType);
    spec.message = l10n::Get(invalid ? I::ContextTreeTypeInvalid : I::ContextTreeTypePrompt);
    spec.width_dip = 420;
    spec.form_height_dip = 60;
    spec.buttons = {{l10n::Get(I::View), ui::FormDialogButtonKind::Primary}, {l10n::Get(I::Cancel)}};
    spec.default_button = 0;
    spec.cancel_button = 1;
    extensions::UiTree tree;
    tree.root.id = "context_extension_form";
    extensions::UiNode input;
    input.id = "extension";
    input.type = extensions::UiNodeType::TextInput;
    input.state = "extension";
    input.placeholder = ".dwg";
    input.validation = extensions::UiValidation{true, 1, 34, json::Utf8(l10n::Get(I::ContextTreeTypeInvalid))};
    tree.root.children.push_back(std::move(input));
    tree.state = {{"extension", json::Utf8(initial)}};
    spec.form = std::move(tree);
    // FormDialog clears its callback before invoking it, releasing this keepalive.
    spec.on_closed = [dialog, owner, identity](int button, std::vector<extensions::UiStateEntry> states) {
        if (button != 0 || !IsWindow(owner) || GetAppState(owner) != identity) return;
        auto* live = GetAppState(owner);
        std::wstring typed;
        for (const auto& entry : states) if (entry.key == "extension") typed = json::Wide(entry.value);
        const auto extension = app::SystemCtxModel::NormalizeExtension(typed);
        if (extension.empty()) {
            ShowExtensionForm(*live, std::move(typed), true);
            return;
        }
        if (live->settings.SystemCtx().SelectExtension(extension)) {
            live->settings.SetScroll(0, 0);
            InvalidateRect(owner, nullptr, FALSE);
        }
    };
    if (dialog->Show(owner, std::move(spec), state.darkMode, state.accentColor)) return true;
    dialog->Destroy();
    return false;
}
}
bool ShowSystemContextExtension(AppState& state) {
    if (state.settings.SystemCtx().View()->state != ui::SysCtxView::State::Ready) return false;
    return ShowExtensionForm(state, L".dwg", false);
}
} // namespace pulse
