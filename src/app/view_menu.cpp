#include "view_menu.h"
#include "context_menu.h"
#include "../common/localization.h"
#include "../ui/item_selection_geometry.h"
#include <algorithm>
#include <cwchar>

namespace pulse::app {

namespace {

constexpr const wchar_t* kGlyphViewMenu = L"\xE8A9";

ui::FluentMenuItem Item(int cmd, const wchar_t* text, const wchar_t* glyph,
                        const wchar_t* shortcut = nullptr, bool enabled = true) {
    ui::FluentMenuItem it;
    it.command = cmd;
    it.text = text;
    it.glyph = glyph;
    if (shortcut) it.shortcut = shortcut;
    it.enabled = enabled;
    it.mnemonic = BuiltinMenuMnemonic(cmd);
    if (cmd == CmdNone && glyph) {
        if (std::wcscmp(glyph, kGlyphViewMenu) == 0) it.mnemonic = L'V';
    }
    return it;
}

} // namespace

std::vector<ui::FluentMenuItem> BuildViewMenu(ui::ViewMode current_mode, bool details_panel,
                                              bool can_apply_all, const std::wstring& item_checkboxes) {
    static constexpr l10n::StringId labels[] = {
        l10n::StringId::ViewExtraLarge, l10n::StringId::ViewLarge,
        l10n::StringId::MediumIcons, l10n::StringId::ViewSmall,
        l10n::StringId::ViewList, l10n::StringId::ViewDetails,
        l10n::StringId::ViewTiles, l10n::StringId::ViewContent,
    };
    static constexpr const wchar_t* glyphs[] = {
        L"\xE7F4", L"\xE7F4", L"\xE7F4", L"\xECA5",
        L"\xEA37", L"\xE8A5", L"\xECA5", L"\xE8FD"
    };
    std::vector<ui::FluentMenuItem> items;
    items.reserve(can_apply_all ? 13 : 12);
    // File Explorer's Ctrl+Shift+1..8 (explorer_shortcuts.h).
    static constexpr const wchar_t* keys[] = {
        L"Ctrl+Shift+1", L"Ctrl+Shift+2", L"Ctrl+Shift+3", L"Ctrl+Shift+4",
        L"Ctrl+Shift+5", L"Ctrl+Shift+6", L"Ctrl+Shift+7", L"Ctrl+Shift+8"
    };
    for (int i = 0; i < 8; ++i) {
        auto row = Item(CmdViewBase + i, l10n::Get(labels[i]).c_str(), glyphs[i], keys[i]);
        if (i == 0) row.glyph_scale = 1.16f;
        else if (i == 1) row.glyph_scale = 1.0f;
        else if (i == 2) row.glyph_scale = 0.82f;
        row.radio = ui::ViewModeIndex(current_mode) == i;
        items.push_back(std::move(row));
    }
    items.back().separator_after = true;
    auto panel = Item(CmdDetailsPanel, l10n::Get(l10n::StringId::DetailsPane).c_str(), L"\xE700");
    panel.checked = details_panel;
    panel.separator_after = true;
    items.push_back(std::move(panel));
    struct CheckboxRow {
        int command;
        ui::CheckboxVisibility visibility;
        const wchar_t* chinese;
        const wchar_t* english;
    };
    static constexpr CheckboxRow checkbox_rows[] = {
        { CmdItemCheckboxAuto, ui::CheckboxVisibility::Auto,
          L"项目复选框：自动显示", L"Item checkboxes: Show automatically" },
        { CmdItemCheckboxAlways, ui::CheckboxVisibility::Always,
          L"项目复选框：始终显示", L"Item checkboxes: Always show" },
        { CmdItemCheckboxNever, ui::CheckboxVisibility::Never,
          L"项目复选框：隐藏", L"Item checkboxes: Hide" },
    };
    const auto visibility = ui::ResolveCheckboxVisibility(item_checkboxes);
    for (const auto& row : checkbox_rows) {
        auto child = Item(row.command, l10n::Pick(row.chinese, row.english), L"");
        child.radio_group = true;
        child.radio = visibility == row.visibility;
        items.push_back(std::move(child));
    }
    if (can_apply_all) {
        items.back().separator_after = true;
        items.push_back(Item(CmdApplyViewToAllFolders,
            l10n::Get(l10n::StringId::ApplyViewAllFolders).c_str(), L"\xE8B3"));
    }
    return items;
}

ui::FluentMenuItem BuildViewSubmenuItem(const BackgroundViewOptions& options) {
    auto view = Item(CmdNone, l10n::Get(l10n::StringId::View).c_str(), kGlyphViewMenu);
    view.children = BuildViewMenu(options.view_mode, options.details_panel, options.filesystem,
                                  options.item_checkboxes);
    return view;
}

} // namespace pulse::app
