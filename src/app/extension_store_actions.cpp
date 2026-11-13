#include "extension_store_actions.h"
#include "extension_store_ui.h"
#include "extension_store_seven_zip.h"
#include "app_commands.h"
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
int Choice(AppState& s, int field, const std::vector<std::wstring>& labels, int selected) {
    std::vector<ui::FluentMenuItem> items;
    for (size_t i = 0; i < labels.size(); ++i) {
        ui::FluentMenuItem item;
        item.command = static_cast<int>(i) + 1;
        item.text = labels[i]; item.radio_group = true;
        item.radio = static_cast<int>(i) == selected;
        items.push_back(std::move(item));
    }
    return Menu(s, field, std::move(items)) - 1;
}
}
bool ExtensionStoreDetailAction(AppState& s, int action) {
    if (action < Value(Action::StoreChipBase)) return false;
    ui::WindowViewModel vm;
    FillExtensionStoreView(s, vm);
    const auto& view = vm.settings_store_view;
    const auto& z = view.seven_zip;
    auto management_menu = [&](int index, int field) {
        if (index < 0 || static_cast<size_t>(index) >= view.installed.size()) return true;
        const auto& installed = view.installed[index];
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
    auto save = [&](const std::string& key, const std::string& value) {
        if (z.available && SetSevenZipState(s, key, value)) s.store_saved_at = GetTickCount64();
    };
    auto range = [&](Action base, int count) { return action >= Value(base) && action < Value(base) + count; };
    if (range(Action::StoreChipBase, 5)) {
        s.store_category = action - Value(Action::StoreChipBase); s.settings.SetScroll(0, 0);
    } else if (action == Value(Action::StoreRetry)) return ExtensionStoreAction(s, Value(Action::StoreRefresh));
    else if (action == Value(Action::StoreSources)) return ExtensionStoreAction(s, Value(Action::StoreManage));
    else if (action == Value(Action::StoreDetailBack)) { s.store_detail = false; s.settings.SetScroll(0, 0); }
    else if (range(Action::StoreDetailTabBase, 4)) { s.store_detail_tab = action - Value(Action::StoreDetailTabBase); s.settings.SetScroll(0, 0); }
    else if (range(Action::SzPreviewBase, 2)) s.store_preview_kind = action - Value(Action::SzPreviewBase);
    else if (action == Value(Action::SzSource)) {
        if (z.item >= 0 && static_cast<size_t>(z.item) < view.items.size()) {
            const auto& url = view.items[z.item].source;
            if (url.starts_with(L"https://") || url.starts_with(L"http://"))
                ShellExecuteW(s.hwnd, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
    }
    else if (action == Value(Action::StoreDetailEnable) || action == Value(Action::SzHistoryRollback)) {
        if (z.installed_index >= 0 && static_cast<size_t>(z.installed_index) < view.installed.size()) {
            const auto& installed = view.installed[z.installed_index];
            const bool rollback = action == Value(Action::SzHistoryRollback);
            if (rollback ? installed.can_rollback : (installed.enabled ? installed.can_disable : installed.can_enable))
                return ExtensionStoreAction(s, Value(rollback ? Action::InstalledRollbackBase : installed.enabled ? Action::InstalledDisableBase : Action::InstalledEnableBase) + z.installed_index);
        }
    } else if (action == Value(Action::StoreDetailMore)) {
        return management_menu(z.installed_index, 5);
    } else if (range(Action::SzModeBase, 3)) save("mode", std::to_string(action - Value(Action::SzModeBase)));
    else if (range(Action::SzLocationBase, 2)) save("location", std::to_string(action - Value(Action::SzLocationBase)));
    else if (range(Action::SzFormatBase, 6)) save("format", std::to_string(action - Value(Action::SzFormatBase)));
    else if (range(Action::SzLevelBase, 6)) {
        if (z.available && z.format != 2) {
            const int level = action - Value(Action::SzLevelBase);
            if (GetKeyState(VK_LBUTTON) < 0) {
                s.store_level_drag = static_cast<float>(level);
                SetCapture(s.hwnd);
            } else save("level", std::to_string(level));
        }
    }
    else if (range(Action::SzPlacementBase, 2)) save(action == Value(Action::SzPlacementBase) ? "placement0" : "placement1", "true");
    else if (range(Action::SzCommandBase, 6)) {
        const int bit = action - Value(Action::SzCommandBase);
        save("cmd" + std::to_string(bit), (z.commands & (1u << bit)) ? "false" : "true");
    } else if (action == Value(Action::SzRemember) || action == Value(Action::StoreQuickRemember)) save("remember", z.remember ? "false" : "true");
    else if (action == Value(Action::SzBrowse)) { if (z.available) BrowseSevenZipFolder(s); }
    else if (action == Value(Action::SzTake)) {
        if (z.take_available && !z.take_busy) { auto config = z.take_config; config[0] = z.take ? '0' : '1'; save("takecfg", config); }
    } else if (range(Action::SzTakeIncBase, 16)) {
        const auto index = action - Value(Action::SzTakeIncBase);
        if (z.take && !z.take_busy && index < static_cast<int>(z.take_rows.size())) {
            unsigned groups = static_cast<unsigned>(std::strtoul(z.take_config.substr(4, 3).c_str(), nullptr, 16)) ^ (1u << index);
            char hex[4]{};
            sprintf_s(hex, "%03x", groups & 0xfffu);
            save("takecfg", z.take_config.substr(0, 4) + hex);
        }
    } else if (range(Action::SzTakeAskBase, 16)) {
        const auto index = static_cast<size_t>(action - Value(Action::SzTakeAskBase));
        if (z.available && index < z.take_rows.size() && z.take_rows[index].ask) ClickSevenZip(s, "ask" + std::to_string(index));
    } else if (action == Value(Action::SzTakeNext)) { if (z.available && z.ask_any) ClickSevenZip(s, "asknext"); }
    else if (action == Value(Action::SzTakeWinSettings)) { if (z.available && z.ask_any) ClickSevenZip(s, "winsettings"); }
    else if (range(Action::SzDoubleClickBase, 2)) {
        if (z.take_available && !z.take_busy) {
            auto config = z.take_config;
            config[2] = action == Value(Action::SzDoubleClickBase) ? '0' : '1';
            if (config != z.take_config) save("takecfg", config);
        }
    }
    else if (action == Value(Action::StoreQuickMode) && z.available) {
        const int selected = Choice(s, 2, {l10n::Get(Id::SzModeSmart), l10n::Get(Id::SzModeFolder), l10n::Get(Id::SzModeHere)}, z.mode);
        if (selected >= 0) save("mode", std::to_string(selected));
    } else if (action == Value(Action::StoreQuickFormat) && z.available) {
        const int selected = Choice(s, 3, {L"7z", L"zip", L"tar", L"tar.gz", L"tar.xz", L"tar.bz2"}, z.format);
        if (selected >= 0) save("format", std::to_string(selected));
    } else if (action == Value(Action::StoreQuickLevel) && z.available && z.format != 2) {
        const int selected = Choice(s, 4, {l10n::Get(Id::SzLevel0), l10n::Get(Id::SzLevel1), l10n::Get(Id::SzLevel2), l10n::Get(Id::SzLevel3), l10n::Get(Id::SzLevel4), l10n::Get(Id::SzLevel5)}, z.level);
        if (selected >= 0) save("level", std::to_string(selected));
    }
    InvalidateRect(s.hwnd, nullptr, FALSE);
    return true;
}
bool ExtensionStoreLevelDragging(const AppState& s) { return s.store_level_drag >= 0.0f; }
void ExtensionStoreLevelDrag(AppState& s, int x) {
    if (!ExtensionStoreLevelDragging(s)) return;
    const auto bounds = s.renderer.SettingsStoreField(6);
    if (bounds.right <= bounds.left) return;
    s.store_level_drag = ui::fluent::StepSliderPositionAt(bounds, 6, static_cast<float>(x), s.scale);
    InvalidateRect(s.hwnd, nullptr, FALSE);
}
bool ExtensionStoreLevelDragEnd(AppState& s) {
    if (!ExtensionStoreLevelDragging(s)) return false;
    const int level = static_cast<int>(std::lround(s.store_level_drag));
    s.store_level_drag = -1.0f;
    if (SetSevenZipState(s, "level", std::to_string(level))) s.store_saved_at = GetTickCount64();
    InvalidateRect(s.hwnd, nullptr, FALSE);
    return true;
}
}

