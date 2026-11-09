#pragma once
#include "../../../ui/fluent_menu.h"
#include <string_view>
namespace pulse { struct AppState; }
namespace pulse::extensions::native {
std::vector<ui::FluentMenuItem> CommandRows(AppState&, std::wstring_view query = {});
bool HandleContributedCommand(AppState&, int);
bool HandleContributedShortcut(AppState&, WPARAM key);
std::vector<ui::FluentMenuItem> PinnedCommandRows(AppState&);
std::wstring CommandIdentity(AppState&, int);
bool ToggleCommandPin(AppState&, int);
bool IsCommandPinned(AppState&, int);
}
