#include "extension_store_quick.h"
#include "app_commands.h"
#include "app_state.h"
#include "extensions/manifest.h"
#include "extensions/quick_settings_model.h"
#include "extensions/runtime.h"
#include "extensions/native/commands_adapter.h"
#include "extensions/native/session.h"
#include "extensions/native/settings_pages_adapter.h"
#include "../common/json_value.h"
#include "../common/localization.h"
#include "../ui/extension_store_view.h"
#include "../ui/fluent_menu.h"
#include "../ui/ui_renderer.h"
#include "../ui/ui_view_model.h"
#include <cmath>
#include <vector>

namespace pulse {
namespace {

using Id = l10n::StringId;

std::wstring LocalizedText(const extensions::ManifestLocalizedText& value) {
    const auto language = l10n::effective_language();
    if (language == l10n::Language::EnUS) return json::Wide(value.en.empty() ? value.zh : value.en);
    if (language == l10n::Language::ZhTW) {
        if (!value.tw.empty()) return json::Wide(value.tw);
        if (!value.zh.empty()) return l10n::HantText(json::Wide(value.zh));
        return json::Wide(value.en);
    }
    return json::Wide(value.zh.empty() ? value.en : value.zh);
}

int PopupChoice(AppState& s, int action, const std::vector<std::wstring>& labels, int selected) {
    if (!EnsureMenu(s)) return -1;
    s.menu->SetTheme(s.darkMode, s.accentColor);
    const auto rect = s.renderer.SettingsStoreQuickControlRect(action);
    POINT anchor{static_cast<LONG>(std::lround(rect.left)), static_cast<LONG>(std::lround(rect.bottom))};
    if (rect.right <= rect.left || rect.bottom <= rect.top) {
        GetCursorPos(&anchor);
    } else {
        ClientToScreen(s.hwnd, &anchor);
    }
    std::vector<ui::FluentMenuItem> items;
    items.reserve(labels.size());
    for (size_t i = 0; i < labels.size(); ++i) {
        ui::FluentMenuItem item;
        item.command = static_cast<int>(i) + 1;
        item.text = labels[i];
        item.radio_group = true;
        item.radio = static_cast<int>(i) == selected;
        items.push_back(std::move(item));
    }
    return s.menu->TrackPopup(anchor, std::move(items)) - 1;
}

} // namespace

void FillGenericQuickCards(AppState& s, ui::ExtensionStoreView& view) {
    view.quick_cards.clear();
    for (size_t item_idx = 0; item_idx < view.items.size(); ++item_idx) {
        if (view.quick_cards.size() >= extensions::kMaxStoreQuickCards) break;
        const auto& item = view.items[item_idx];
        if (!item.installed || item.settings_package < 0) continue;

        const size_t pkg = static_cast<size_t>(item.settings_package);
        const auto* manifest = extensions::PackageManifest(pkg);
        if (!manifest || !manifest->native || !extensions::PackageEnabled(pkg)) continue;
        if (!manifest->showcase || !manifest->showcase->quick_settings) continue;

        extensions::native::EnsureCommandPackageStarted(s, pkg);
        auto* page = extensions::native::PackageSettings(s.hwnd, pkg);
        const bool has_page = page && page->Page();

        const auto read_state = [page, has_page](const std::string& key) -> std::optional<std::string> {
            return has_page ? page->State(key) : std::nullopt;
        };
        const auto& decl = *manifest->showcase->quick_settings;
        const auto resolved = extensions::ResolveQuickSettingsCard(decl, read_state, LocalizedText, has_page);

        const size_t card_idx = view.quick_cards.size();
        ui::ExtensionQuickCardView card;
        card.item_index = static_cast<int>(item_idx);
        card.package_index = static_cast<int>(pkg);
        card.package_id = manifest->id;
        card.title = resolved.title;
        card.status = resolved.status;
        card.available = resolved.available;
        card.has_menu_preview = resolved.has_menu_preview;
        card.preview_submenu = resolved.preview_submenu;
        card.preview_caption = resolved.preview_caption;

        card.rows.reserve(resolved.rows.size());
        for (const auto& r : resolved.rows) {
            ui::ExtensionQuickRowView row;
            row.title = r.title;
            row.description = r.description;
            row.controls.reserve(r.controls.size());
            for (const auto& c : r.controls) {
                ui::ExtensionQuickControlView ctrl;
                ctrl.kind = static_cast<int>(c.kind);
                ctrl.action = extensions::EncodeQuickControlAction(card_idx, c.flat_index);
                ctrl.label = c.label;
                ctrl.checked = c.checked;
                ctrl.disabled = c.disabled;
                ctrl.width_dip = c.width_dip;
                ctrl.option_labels = c.option_labels;
                ctrl.selected_option = c.selected_option;
                row.controls.push_back(std::move(ctrl));
            }
            card.rows.push_back(std::move(row));
        }

        if (card.available && decl.menu_preview) {
            std::vector<extensions::native::CommandsAdapter::PreviewEntry> entries;
            entries.reserve(decl.menu_preview->sample_entries.size());
            for (const auto& sample : decl.menu_preview->sample_entries) {
                entries.push_back({sample.name, sample.directory});
            }
            const std::string sample_dir = json::Utf8(l10n::Get(Id::SzSampleParent));
            for (const auto& info : extensions::native::CommandsAdapter::Preview(
                     s.hwnd, manifest->id, entries, sample_dir)) {
                card.preview_rows.push_back({
                    json::Wide(info.title),
                    json::Wide(info.detail),
                    info.icon,
                    ui::StoreCommandIconFor(info.icon, info.identity),
                });
            }
        }
        view.quick_cards.push_back(std::move(card));
    }
}

bool HandleGenericQuickControlAction(AppState& s, const ui::ExtensionStoreView& view, int action) {
    size_t card_idx = 0;
    size_t ctrl_idx = 0;
    if (!extensions::DecodeQuickControlAction(action, card_idx, ctrl_idx)) return false;
    if (card_idx >= view.quick_cards.size()) return false;
    const auto& card = view.quick_cards[card_idx];
    if (!card.available || card.package_index < 0) return true;

    const size_t pkg = static_cast<size_t>(card.package_index);
    const auto* manifest = extensions::PackageManifest(pkg);
    if (!manifest || !manifest->native || !extensions::PackageEnabled(pkg) ||
        !manifest->showcase || !manifest->showcase->quick_settings) {
        return true;
    }
    auto* page = extensions::native::PackageSettings(s.hwnd, pkg);
    if (!page || !page->Page()) return true;

    const auto read_state = [page](const std::string& key) -> std::optional<std::string> {
        return page->State(key);
    };

    const ui::ExtensionQuickControlView* target_view = nullptr;
    for (const auto& row : card.rows) {
        for (const auto& c : row.controls) {
            if (c.action == action) {
                target_view = &c;
                break;
            }
        }
        if (target_view) break;
    }
    if (!target_view || target_view->disabled) return true;

    int chosen_option = -1;
    if (target_view->kind == static_cast<int>(extensions::QuickSettingControlKind::Select)) {
        chosen_option = PopupChoice(s, action, target_view->option_labels, target_view->selected_option);
        if (chosen_option < 0) return true;
    }

    const auto edit = extensions::ComputeQuickControlEdit(
        *manifest->showcase->quick_settings, ctrl_idx, read_state, chosen_option);
    if (!edit) return true;
    if (edit->is_click) {
        page->Click(edit->key_or_command);
    } else if (page->CommitState(edit->key_or_command, edit->value)) {
        s.store_saved_at = GetTickCount64();
    }
    return true;
}

} // namespace pulse
