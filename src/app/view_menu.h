#pragma once

#include "../ui/fluent_menu.h"
#include "../ui/view_layout.h"
#include <string>
#include <vector>

namespace pulse::app {

struct BackgroundViewOptions;

// can_apply_all: real folder, offer "apply view and sort to all folders".
// Checkbox choices stay flat so the background View flyout remains one level.
std::vector<ui::FluentMenuItem> BuildViewMenu(ui::ViewMode current_mode,
                                              bool details_panel = false,
                                              bool can_apply_all = false,
                                              const std::wstring& item_checkboxes = L"auto");

ui::FluentMenuItem BuildViewSubmenuItem(const BackgroundViewOptions& options);

} // namespace pulse::app
