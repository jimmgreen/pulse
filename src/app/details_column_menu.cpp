// details_column_menu.cpp — Column header right-click menu (#26, #36).
#include "details_column_menu.h"
#include "app_internal.h"
#include "app_commands.h"
#include "app_hosted_edit.h"

namespace pulse {

void ShowDetailsColumnMenu(AppState& s, POINT screen_pt, uint32_t visible, bool search) {
    if (!EnsureMenu(s)) return;
    const uint32_t mask = s.appPrefs.details_columns;
    const int command = s.menu->TrackPopup(screen_pt, app::BuildDetailsColumnMenu(mask, visible, search));
    const uint32_t next = app::ApplyDetailsColumnCommand(mask, command);
    // Reset also brings back the classic order (header drags).
    const bool reorder = command == app::kDetailsColumnsReset &&
                         s.appPrefs.details_column_order != ui::kDetailsColumnOrderDefault;
    if (next == mask && !reorder) return;
    if (reorder) s.settings.DetailsColumnOrder(ui::kDetailsColumnOrderDefault);
    s.settings.DetailsColumns(next);
    if (s.renameIndex >= 0) LayoutRenameOverlay(s);
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

} // namespace pulse
