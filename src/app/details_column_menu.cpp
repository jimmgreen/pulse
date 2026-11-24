// details_column_menu.cpp — Column header right-click menu (#26, #36).
#include "details_column_menu.h"
#include "app_internal.h"
#include "app_commands.h"
#include "app_hosted_edit.h"
#include "extension_columns_ui.h"

namespace pulse {

void ShowDetailsColumnMenu(AppState& s, POINT screen_pt, uint32_t visible, bool search,
                           const std::vector<std::wstring>& shown_extension_keys) {
    if (!EnsureMenu(s)) return;
    const uint32_t mask = s.appPrefs.details_columns;
    auto items = app::BuildDetailsColumnMenu(mask, visible, search);
    // Extension columns and badges (pulse.columns), before 重置列.
    auto extension_rows = app::BuildExtensionColumnMenuRows(s.appPrefs.extension_columns_hidden, shown_extension_keys);
    if (!extension_rows.empty() && !items.empty()) {
        extension_rows.back().separator_after = true;
        items.insert(items.end() - 1, std::make_move_iterator(extension_rows.begin()),
                     std::make_move_iterator(extension_rows.end()));
    }
    const int command = s.menu->TrackPopup(screen_pt, std::move(items));
    std::vector<std::wstring> hidden = s.appPrefs.extension_columns_hidden;
    if (app::ApplyExtensionColumnCommand(hidden, command)) {
        s.settings.ExtensionColumns(hidden, s.appPrefs.extension_column_widths);
        if (s.renameIndex >= 0) LayoutRenameOverlay(s);
        InvalidateRect(s.hwnd, nullptr, FALSE);
        return;
    }
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