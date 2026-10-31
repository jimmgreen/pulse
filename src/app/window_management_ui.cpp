#include "window_management_ui.h"
#include "app_internal.h"
#include "app_messages.h"
#include "app_navigation.h"
#include "keymap.h"
#include "window_arrange_ui.h"
#include "window_overview_ui.h"
namespace pulse {
l10n::StringId WindowArrangeLabel(app::WindowArrangeLayout layout) {
    using I = l10n::StringId;
    constexpr I ids[] = {I::ArrangeSideBySide, I::ArrangeStacked, I::ArrangeColumns,
                         I::ArrangeTile,       I::ArrangeMain,    I::ArrangeCascade};
    const auto i = static_cast<unsigned>(layout);
    return ids[i < 6 ? i : 0];
}
ui::fluent::MenuPictogram WindowArrangePictogram(app::WindowArrangeLayout layout) {
    using P = ui::fluent::MenuPictogram;
    constexpr P icons[] = {P::LayoutSideBySide, P::LayoutStacked, P::LayoutColumns,
                           P::LayoutFour,       P::LayoutThree,   P::LayoutCascade};
    const auto i = static_cast<unsigned>(layout);
    return icons[i < 6 ? i : 0];
}
void ShowWindowManagementMenu(AppState &s, POINT screen) {
    if (!EnsureMenu(s))
        return;
    std::vector<ui::FluentMenuItem> items;
    auto add = [&](int cmd, l10n::StringId label, const wchar_t *glyph, const wchar_t *key = L"") {
        ui::FluentMenuItem item;
        item.command = cmd;
        item.text = l10n::Get(label);
        item.glyph = glyph;
        item.shortcut = app::ShownShortcut(key);
        items.push_back(std::move(item));
    };
    add(1, l10n::StringId::ActNewWindow, L"\xE78B", L"Ctrl+N");
    add(2, l10n::StringId::WindowOverviewTitle, L"\xE8A9", L"Ctrl+Shift+O");
    add(3, l10n::StringId::ActTabToWindow, L"\xE8A7", L"Ctrl+Shift+M");
    items.back().enabled = s.window_tabs.items.size() > 1;
    if (auto *tab = ActiveTab(s))
        items.back().text += L" · " + app::TabTitle(tab->current_path);
    items.back().separator_after = true;
    ui::FluentMenuItem heading;
    heading.heading = true;
    heading.text = l10n::Get(l10n::StringId::SettingsSecArrangeWindows);
    items.push_back(std::move(heading));
    ui::FluentMenuItem count;
    count.heading = true;
    count.text = l10n::Get(l10n::StringId::ArrangeCount);
    size_t tabs = 0;
    s.core.windows.ForEach([&](HWND, AppState &window) { tabs += window.window_tabs.items.size(); });
    auto replace = [&](const wchar_t *token, size_t n) {
        const auto at = count.text.find(token);
        if (at != std::wstring::npos)
            count.text.replace(at, wcslen(token), std::to_wstring(n));
    };
    replace(L"{windows}", s.core.windows.Count());
    replace(L"{tabs}", tabs);
    items.push_back(std::move(count));
    for (int row = 0; row < 2; ++row) {
        ui::FluentMenuItem tiles;
        for (int col = 0; col < 3; ++col) {
            const int i = row * 3 + col;
            const auto layout = static_cast<app::WindowArrangeLayout>(i);
            ui::FluentMenuSwatch tile;
            tile.command = 10 + i;
            tile.label = l10n::Get(WindowArrangeLabel(layout));
            tile.pictogram = WindowArrangePictogram(layout);
            tile.mnemonic = static_cast<wchar_t>(L'1' + i);
            tiles.quick_swatches.push_back(std::move(tile));
        }
        items.push_back(std::move(tiles));
    }
    ui::FluentMenuItem hint;
    hint.heading = true;
    hint.text = l10n::Get(l10n::StringId::ArrangeHint);
    items.push_back(hint);
    if (s.core.windows.Count() == 1) {
        hint.text = l10n::Get(s.window_tabs.items.size() > 1 ? l10n::StringId::ArrangeSingleHint
                                                             : l10n::StringId::ArrangeSingleTabHint);
        hint.tooltip = hint.text;
        items.push_back(hint);
    }
    items.back().separator_after = true;
    add(4, l10n::StringId::ActMergeWindows, L"\xE8C8", L"Ctrl+Shift+J");
    items.back().enabled = s.core.windows.Count() > 1;
    const int cmd = s.menu->TrackPopup(screen, std::move(items));
    if (cmd == 1)
        PostMessageW(s.hwnd, WM_NEW_WINDOW, 0, 0);
    else if (cmd == 2)
        ToggleWindowOverview(s);
    else if (cmd == 3)
        PostMessageW(s.hwnd, WM_MOVE_TAB_TO_NEW_WINDOW, 0, 0);
    else if (cmd == 4)
        MergeAllWindows(s);
    else if (cmd >= 10 && cmd < 16)
        ArrangeWindows(s, static_cast<app::WindowArrangeLayout>(cmd - 10), (GetKeyState(VK_SHIFT) & 0x8000) != 0);
}
} // namespace pulse
