// Windowless column chooser shared by the main window and picker.
#include "../app/details_column_menu.h"
#include "../common/localization.h"
#include "details_column_set.h"
#include "ui_renderer.h"

namespace pulse {
namespace app {
namespace {

using I = l10n::StringId;
using K = ui::MainRenderer::ColumnKind;

constexpr const wchar_t* kIconCheck = L"\xE73E"; // Segoe Fluent Icons: CheckMark

struct ColumnRow { K kind; I label; };
// Same order as the header; 状态 last, since only cloud folders draw it.
constexpr ColumnRow kRows[] = {
    {K::Date, I::ColumnModified}, {K::Created, I::ColumnCreated},
    {K::Accessed, I::ColumnAccessed}, {K::Type, I::ColumnType}, {K::Size, I::ColumnSize},
    {K::Status, I::ColumnStatus},
};

constexpr uint32_t Bit(K kind) { return 1u << static_cast<uint32_t>(kind); }

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
            if (row.kind == K::Status) {
                item.badge_text = l10n::Get(I::DetailsColumnCloudOnly);
            } else if (search && (row.kind == K::Created || row.kind == K::Accessed)) {
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
} // namespace pulse
