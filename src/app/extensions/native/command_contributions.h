#pragma once
#include "commands_adapter.h"
#include "../../../ui/fluent_menu.h"
#include <string_view>
namespace pulse { struct AppState; }
namespace pulse::extensions::native {
std::vector<PackageCommandSummary> KnownPackageCommands(AppState&, size_t package);
std::vector<ui::FluentMenuItem> CommandRows(AppState&, std::wstring_view query = {});
// Context menu: like CommandRows, plus each command's detail line and its package
// group (a caption above the rows, or one submenu row when the group asks for it).
std::vector<ui::FluentMenuItem> ContextMenuCommandRows(AppState&);
bool HandleContributedCommand(AppState&, int);
bool HandleContributedShortcut(AppState&, WPARAM key);
std::vector<ui::FluentMenuItem> PinnedCommandRows(AppState&);
std::wstring CommandIdentity(AppState&, int);
bool ToggleCommandPin(AppState&, int);
bool IsCommandPinned(AppState&, int);
}
