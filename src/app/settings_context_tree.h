#pragma once
#include "context_menu_prefs.h"
#include "../ui/fluent_menu.h"
#include <map>

namespace pulse::app {
enum class ContextTreeActionKind { Open, Back, SetVisible, MoveUp, MoveDown, ResetGroup };
struct ContextTreeAction {
    ContextTreeActionKind kind = ContextTreeActionKind::Open;
    std::wstring key;
    bool visible = true;
};
struct ContextTreePage {
    std::vector<ui::FluentMenuItem> items;
    std::map<int, ContextTreeAction> actions;
};
enum class ContextTreeActionResult { None, Navigated, Changed, Rejected };
// Builds the same rows used by the settings popup. All actions carry stable keys.
ContextTreePage BuildSettingsContextTreePage(const ContextMenuPrefs& prefs,
    const std::wstring& parent_key = {}, const std::wstring& root_key = {});
ContextTreeActionResult ApplySettingsContextTreeAction(ContextMenuPrefs& prefs,
    const ContextTreeAction& action, std::wstring& parent_key);
} // namespace pulse::app
