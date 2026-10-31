#pragma once
#include "../common/localization.h"
#include "../ui/fluent_menu.h"
#include "window_arrange.h"
namespace pulse {
struct AppState;
l10n::StringId WindowArrangeLabel(app::WindowArrangeLayout layout);
ui::fluent::MenuPictogram WindowArrangePictogram(app::WindowArrangeLayout layout);
void ShowWindowManagementMenu(AppState &s, POINT screen);
} // namespace pulse
