#pragma once
#include "commands_adapter.h"
#include "../../../ui/fluent_menu.h"
#include <string_view>
namespace pulse { struct AppState; }
namespace pulse::extensions::native {
void WarmCommandProjectionCache(AppState&);
void WarmSevenZipMenuPreferences(AppState&);
bool SevenZipMenuPreferencesReady();
std::vector<PackageCommandSummary> KnownPackageCommands(AppState&, size_t package);
std::vector<ui::FluentMenuItem> CommandRows(AppState&, std::wstring_view query = {});
// Context menu: like CommandRows, plus each command's detail line and its package
// group (a caption above the rows, or one submenu row when the group asks for it).
// any_selection keeps commands the current selection disables (settings previews).
std::vector<ui::FluentMenuItem> ContextMenuCommandRows(AppState&,
    bool* waiting_for_seven_zip = nullptr, std::string_view only_package = {}, bool any_selection = false);
bool HandleContributedCommand(AppState&, int);
// Shell-open worker confirmed no archive application; use the original path, not the current selection.
bool HandleArchiveOpenFallback(AppState&, const std::wstring& path);
// Opens the archive at `path` with pulse.7zip's VFS without showing it (Quick
// Look and the details pane drag items out of it and drop files into it).
// Returns the mount's root (pulse:vfs:...), or empty when pulse.7zip is not
// enabled or cannot open it.
std::wstring MountArchiveQuietly(AppState&, const std::wstring& path);
bool HandleExtensionFileOpen(AppState&, std::string_view package_id, std::string_view handler_id,
                             const std::wstring& path, bool force_preview = false);
std::vector<std::wstring> FallbackOpenExtensions();
bool HandleContributedShortcut(AppState&, WPARAM key);
std::vector<ui::FluentMenuItem> PinnedCommandRows(AppState&);
// PULSE_COMMAND_TOOLBAR: pins each flagged command once (command_pins_host.cpp).
void ApplyToolbarSuggestions(AppState&, const std::vector<CommandInfo>&);
std::wstring CommandIdentity(AppState&, int);
bool ToggleCommandPin(AppState&, int);
bool IsCommandPinned(AppState&, int);
}