#pragma once
#include "../../../ui/fluent_menu.h"
#include <string_view>
#include <vector>

namespace pulse { struct AppState; }
namespace pulse::extensions::native {
// Local popup IDs, never accepted as a public execution protocol.
inline constexpr int kCommandBase = 0x700000;
inline constexpr int kToggleCommand = kCommandBase + 32;
inline constexpr int kPanelCommand = kToggleCommand + 1;
std::vector<ui::FluentMenuItem> PackageEntries(std::wstring_view query);
// Opaque session tokens; callers dispatch through HandlePackageCommand.
std::vector<ui::FluentMenuItem> EntriesForPackage(size_t package, std::wstring_view query);
bool HandlePackageCommand(AppState& state, int command);
}
