#pragma once
#include "../common/localization.h"
#include "../ui/fluent_menu.h"
#include "window_arrange.h"
namespace pulse {
struct AppState;
l10n::StringId WindowArrangeLabel(app::WindowArrangeLayout layout);
ui::fluent::MenuPictogram WindowArrangePictogram(app::WindowArrangeLayout layout);
std::vector<ui::FluentMenuItem> WindowArrangementMenuItems();
std::vector<ui::FluentMenuItem> TrayMenuItems(AppState &s);
int TrackTrayMenu(AppState &s, POINT screen, HWND owner);
int PickWindowArrangement(AppState &s, POINT screen);
void ShowWindowManagementMenu(AppState &s, POINT screen);
} // namespace pulse
