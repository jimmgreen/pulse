// details_column_menu.h — Column header right-click menu (#26, #36).
//
// File Explorer opens a column chooser on the Details header; Pulse does the
// same with one checked row per optional column plus "Reset columns". The
// choice is global (AppPrefs::details_columns). The builder and the toggle
// are windowless so the self-test can check them without a GUI.
#pragma once
#include "../ui/fluent_menu.h"
#include <windows.h>
#include <cstdint>
#include <string>
#include <vector>

namespace pulse {
struct AppState;

namespace app {

// Popup-local command ids: toggle base + MainRenderer::ColumnKind value.
inline constexpr int kDetailsColumnToggleBase = 33000;
inline constexpr int kDetailsColumnsReset = 33100;

// Name (always on, disabled), modified, created, accessed, type, size, then reset.
// `visible` holds the columns the pane currently draws (1 << ColumnKind); a
// chosen column outside it gets a reason badge ("No room", or "Not in search"
// for created / accessed in search results).
std::vector<ui::FluentMenuItem> BuildDetailsColumnMenu(uint32_t mask, uint32_t visible = 0xFFFFFFFFu,
                                                       bool search = false);
// The mask after choosing `command`; unknown commands leave it unchanged.
uint32_t ApplyDetailsColumnCommand(uint32_t mask, int command);

} // namespace app

// shown_extension_keys: extension columns the pane draws ("<package>/<id>").
void ShowDetailsColumnMenu(AppState& s, POINT screen_pt, uint32_t visible, bool search,
                           const std::vector<std::wstring>& shown_extension_keys = {});

} // namespace pulse