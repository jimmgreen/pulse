// details_column_menu.cpp — Column header right-click menu (#26, #36).
#include "details_column_menu.h"
#include "app_internal.h"
#include "app_commands.h"
#include "app_hosted_edit.h"
#include "../common/localization.h"
#include "../ui/details_column_set.h"
#include "../ui/ui_renderer.h"

namespace pulse {
namespace app {
namespace {

using I = l10n::StringId;
using K = ui::MainRenderer::ColumnKind;

constexpr const wchar_t* kIconCheck = L"\xE73E"; // Segoe Fluent Icons: CheckMark

struct ColumnRow { K kind; I label; };
// Same order as the header.
constexpr ColumnRow kRows[] = {
    {K::Date, I::ColumnModified}, {K::Created, I::ColumnCreated},
    {K::Accessed, I::ColumnAccessed}, {K::Type, I::ColumnType}, {K::Size, I::ColumnSize},
    {K::Title, I::ColumnTitle}, {K::Artist, I::ColumnArtist}, {K::Album, I::ColumnAlbum},
};

constexpr uint32_t Bit(K kind) { return 1u << static_cast<uint32_t>(kind); }

// Columns a search view never shows, whatever the space: search rows carry no
// creation / access times and no audio properties.
constexpr bool SearchExcluded(K kind) {
    return kind == K::Created || kind == K::Accessed ||
           kind == K::Title || kind == K::Artist || kind == K::Album;
}

ui::FluentMenuItem CheckRow(int command, I label, bool checked, bool enabled) {
    ui::FluentMenuItem item;
    item.command = command;
    item.text = l10n::Get(label);
    item.checked = checked;
    // TrackPopup only paints a checked row when the check arrives as a glyph.
    if (checked) item.glyph = kIconCheck;
    item.enabled = enabled;
    return item;
}

} // namespace

std::vector<ui::FluentMenuItem> BuildDetailsColumnMenu(uint32_t mask, uint32_t visible, bool search) {
    mask = ui::NormalizeDetailsColumns(mask);
    std::vector<ui::FluentMenuItem> items;
    items.reserve(std::size(kRows) + 2);
    items.push_back(CheckRow(kDetailsColumnToggleBase + static_cast<int>(K::Name),
                             I::ColumnName, true, false));
    for (const auto& row : kRows) {
        const bool checked = (mask & Bit(row.kind)) != 0;
        auto item = CheckRow(kDetailsColumnToggleBase + static_cast<int>(row.kind), row.label, checked, true);
        // A chosen column the pane cannot show says why, so ticking it is
        // never a silent no-op. A badge, not a shortcut caption: the caption
        // column is sized for short Latin key names and clips CJK text.
        if (checked && !(visible & Bit(row.kind))) {
            if (search && SearchExcluded(row.kind)) {
                item.badge_text = l10n::Get(I::DetailsColumnNotInSearch);
            } else {
                item.badge_text = l10n::Get(I::DetailsColumnNoRoom);
                item.tooltip = l10n::Get(I::DetailsColumnNoRoomTip);
            }
        }
        items.push_back(std::move(item));
    }
    items.back().separator_after = true;
    ui::FluentMenuItem reset;
    reset.command = kDetailsColumnsReset;
    reset.text = l10n::Get(I::DetailsColumnsReset);
    reset.enabled = mask != ui::kDetailsColumnsDefault;
    items.push_back(std::move(reset));
    return items;
}

uint32_t ApplyDetailsColumnCommand(uint32_t mask, int command) {
    mask = ui::NormalizeDetailsColumns(mask);
    if (command == kDetailsColumnsReset) return ui::kDetailsColumnsDefault;
    for (const auto& row : kRows) {
        if (command == kDetailsColumnToggleBase + static_cast<int>(row.kind))
            return mask ^ Bit(row.kind);
    }
    return mask;
}

} // namespace app

void ShowDetailsColumnMenu(AppState& s, POINT screen_pt, uint32_t visible, bool search) {
    if (!EnsureMenu(s)) return;
    const uint32_t mask = s.appPrefs.details_columns;
    const int command = s.menu->TrackPopup(screen_pt, app::BuildDetailsColumnMenu(mask, visible, search));
    const uint32_t next = app::ApplyDetailsColumnCommand(mask, command);
    if (next == mask) return;
    s.settings.DetailsColumns(next);
    if (s.renameIndex >= 0) LayoutRenameOverlay(s);
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

} // namespace pulse
