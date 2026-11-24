// extension_columns_ui.h - extension columns and icon badges (pulse.columns)
// in the file list: which providers a pane shows, their user widths, and the
// column-menu rows that hide or show them.
#pragma once
#include <windows.h>
#include <cstdint>
#include <string>
#include <vector>

namespace pulse {
struct AppState;
namespace ui {
struct FluentMenuItem;
}

namespace app {

// Hooks the pane decorator; call once at startup.
void InstallExtensionColumns();

// AppPrefs: "<package>/<id>" keys of hidden providers, "<key>|<dip>" widths.
void SetExtensionColumnPrefs(const std::vector<std::wstring>& hidden, const std::vector<std::wstring>& widths);
std::vector<std::wstring> ExtensionColumnWidthPrefs();
// A drag resized a column (DIP; 0 = back to the extension's width).
void SetExtensionColumnWidth(const std::wstring& key, float dip);

// Registry::Revision(): moves when values arrive or providers change.
uint64_t ExtensionColumnsRevision();

// Column menu rows for the registered providers (popup-local commands
// kExtensionColumnToggleBase + n); empty without providers.
inline constexpr int kExtensionColumnToggleBase = 33200;
std::vector<ui::FluentMenuItem> BuildExtensionColumnMenuRows(const std::vector<std::wstring>& hidden,
                                                             const std::vector<std::wstring>& shown_keys);
// The hidden list after choosing `command`; false when it is not one of those rows.
bool ApplyExtensionColumnCommand(std::vector<std::wstring>& hidden, int command);

} // namespace app

// The pane's resize finished: store changed extension widths in AppPrefs.
void SaveExtensionColumnWidths(AppState& s);

} // namespace pulse
