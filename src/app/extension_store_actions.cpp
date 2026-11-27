#include "extension_store_actions.h"
#include "extension_store_quick.h"
#include "extension_store_ui.h"
#include "extension_developer_mode.h"
#include "extensions/native/settings_pages_host.h"
#include "../ui/settings_page_ids.h"
#include "app_commands.h"
#include "app_navigation.h"
#include "app_state.h"
#include "../common/localization.h"
#include "../ui/fluent_menu.h"
#include "../ui/ui_view_model.h"
#include "../ui/ui_renderer.h"
#include <cmath>
#include <shellapi.h>

namespace pulse {
namespace {
using Action = ui::PackAction;
using Id = l10n::StringId;
int Value(Action action) { return static_cast<int>(action); }
int Menu(AppState& s, int field, std::vector<ui::FluentMenuItem> items) {
    if (!EnsureMenu(s)) return 0;
    s.menu->SetTheme(s.darkMode, s.accentColor);
    const auto rect = s.renderer.SettingsStoreField(field);
    POINT anchor{static_cast<LONG>(std::lround(rect.left)), static_cast<LONG>(std::lround(rect.bottom))};
    if (field < 0) GetCursorPos(&anchor);
    else ClientToScreen(s.hwnd, &anchor);
    return s.menu->TrackPopup(anchor, std::move(items));
}
}
int ExtensionDetailInstalledIndex(const ui::ExtensionStoreView& view, std::wstring_view focused_id) {
    if (focused_id.empty()) return -1;
    for (size_t i = 0; i < view.installed.size(); ++i)
        if (view.installed[i].id == focused_id) return static_cast<int>(i);
    return -1;
}
bool ExtensionStoreDetailAction(AppState& s, int action) {
    if (action < Value(Action::StoreChipBase)) return false;
    ui::WindowViewModel vm;
    FillExtensionStoreView(s, vm);
    const auto& view = vm.settings_store_view;
    const int active_detail_index = view.detail_item;
    const ui::ExtensionStoreItem* detail_item =
        (active_detail_index >= 0 && static_cast<size_t>(active_detail_index) < view.items.size())
            ? &view.items[static_cast<size_t>(active_detail_index)]
            : nullptr;
    int detail_installed_index = detail_item && detail_item->installed_index >= 0
        ? detail_item->installed_index
        : (detail_item ? ExtensionDetailInstalledIndex(view, std::wstring(detail_item->id.begin(), detail_item->id.end()))
                       : -1);
    if (s.settings.page() == ui::kSettingsPageNativeExtensions) {
        // Resolve the focused identity each click; never reuse a stale catalog index.
        const auto focused = extensions::native::FocusedSettings(s.hwnd);
        const std::wstring id(focused.package_id.begin(), focused.package_id.end());
        detail_installed_index = ExtensionDetailInstalledIndex(view, id);
    }
    if (action >= Value(Action::StoreItemDetailBase) && action < Value(Action::StoreItemDetailBase) + 10000) {
        const size_t idx = static_cast<size_t>(action - Value(Action::StoreItemDetailBase));
        if (idx < view.items.size()) {
            s.store_detail = true;
            s.store_detail_id = view.items[idx].id;
            s.store_detail_tab = 0;
            s.settings_store_browse = true;
            s.settings_store_installed = false;
            if (s.settings.page() != 8) OpenSettingsTab(s, 8);
            s.settings.SetScroll(0, 0);
            InvalidateRect(s.hwnd, nullptr, FALSE);
        }
        return true;
    }

    auto management_menu = [&](int index, int field) {
        if (index < 0 || static_cast<size_t>(index) >= view.installed.size()) return true;
        const auto& installed = view.installed[index];
        if (installed.development_index >= 0)
            return app::DeveloperRowMenu(s, ExtensionStoreRoot(s), installed.development_index, DeveloperTakenIds(s));
        ui::FluentMenuItem uninstall, rollback;
        uninstall.command = 1; uninstall.text = l10n::Get(Id::StoreUninstall);
        uninstall.enabled = installed.can_uninstall; uninstall.vector_icon = ui::StoreIcon::Trash;
        rollback.command = 2; rollback.text = l10n::Get(Id::StoreRollback);
        rollback.enabled = installed.can_rollback; rollback.vector_icon = ui::StoreIcon::Undo;
        const int choice = Menu(s, field, {uninstall, rollback});
        return !choice || ExtensionStoreAction(s, Value(choice == 1 ? Action::InstalledUninstallBase : Action::InstalledRollbackBase) + index);
    };
    if (action >= Value(Action::InstalledMoreBase) && action < Value(Action::InstalledMoreBase) + 10000)
        return management_menu(action - Value(Action::InstalledMoreBase), -1);
    auto range = [&](Action base, int count) { return action >= Value(base) && action < Value(base) + count; };
    if (range(Action::StoreChipBase, 5)) {
        s.store_category = action - Value(Action::StoreChipBase); s.settings.SetScroll(0, 0);
    } else if (action == Value(Action::StoreRetry)) return ExtensionStoreAction(s, Value(Action::StoreRefresh));
    else if (action == Value(Action::StoreSources)) return ExtensionStoreAction(s, Value(Action::StoreManage));
    else if (action == Value(Action::StoreDetailBack)) { s.store_detail = false; s.store_detail_id.clear(); s.settings.SetScroll(0, 0); }
    else if (range(Action::StoreDetailTabBase, 4)) { s.store_detail_tab = action - Value(Action::StoreDetailTabBase); s.settings.SetScroll(0, 0); }
    else if (action == Value(Action::StoreDetailInstall)) {
        if (active_detail_index >= 0 && detail_item && detail_item->action_enabled)
            return ExtensionStoreAction(s, Value(Action::StoreItemBase) + active_detail_index);
    } else if (action == Value(Action::StoreDetailOpenSettings) || action == Value(Action::StoreDetailOpenMenu)) {
        if (detail_item && detail_item->settings_package >= 0) {
            if (action == Value(Action::StoreDetailOpenMenu))
                extensions::native::RequestSettingsPagesContextMenu(s.hwnd, static_cast<size_t>(detail_item->settings_package));
            else {
                // 设置 opens 常用设置, not whichever of the package's own tabs was open last.
                extensions::native::RequestSettingsPagesPackage(s.hwnd, static_cast<size_t>(detail_item->settings_package));
                extensions::native::SetFocusedSettingsPluginTab(s.hwnd, 0);
            }
            OpenSettingsTab(s, ui::kSettingsPageNativeExtensions);
            InvalidateRect(s.hwnd, nullptr, FALSE);
        }
        return true;
    } else if (range(Action::StoreDetailPluginTabBase, 6)) {
        // 支持格式 and the package's other tabs: its settings page, opened on that tab.
        if (detail_item && detail_item->settings_package >= 0) {
            extensions::native::RequestSettingsPagesPackage(s.hwnd, static_cast<size_t>(detail_item->settings_package));
            extensions::native::SetFocusedSettingsPluginTab(s.hwnd, 1 + action - Value(Action::StoreDetailPluginTabBase));
            extensions::native::SetFocusedSettingsMode(s.hwnd, 0);
            OpenSettingsTab(s, ui::kSettingsPageNativeExtensions);
            InvalidateRect(s.hwnd, nullptr, FALSE);
        }
        return true;
    } else if (action == Value(Action::SzSource)) {
        if (detail_item) {
            const auto& url = detail_item->source;
            if (url.starts_with(L"https://") || url.starts_with(L"http://"))
                ShellExecuteW(s.hwnd, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
    } else if (action == Value(Action::StoreDetailHomepage)) {
        if (detail_item) {
            const auto& url = detail_item->homepage;
            if (url.starts_with(L"https://") || url.starts_with(L"http://"))
                ShellExecuteW(s.hwnd, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
    }
    else if (action == Value(Action::StoreDetailEnable) || action == Value(Action::SzHistoryRollback)) {
        if (detail_installed_index >= 0 && static_cast<size_t>(detail_installed_index) < view.installed.size()) {
            const auto& installed = view.installed[detail_installed_index];
            const bool rollback = action == Value(Action::SzHistoryRollback);
            if (rollback ? installed.can_rollback : (installed.enabled ? installed.can_disable : installed.can_enable))
                return ExtensionStoreAction(s, Value(rollback ? Action::InstalledRollbackBase : installed.enabled ? Action::InstalledDisableBase : Action::InstalledEnableBase) + detail_installed_index);
        }
    } else if (action == Value(Action::StoreDetailMore)) {
        return management_menu(detail_installed_index, 5);
    } else if (action >= Value(Action::StoreQuickControlBase)) {
        HandleGenericQuickControlAction(s, view, action);
    }
    InvalidateRect(s.hwnd, nullptr, FALSE);
    return true;
}
}
