#include "window_management_ui.h"
#include "app_internal.h"
#include "app_messages.h"
#include "app_navigation.h"
#include "keymap.h"
#include "window_arrange_ui.h"
#include "window_overview_ui.h"
#include "../common/windows_compat.h"
namespace pulse {
l10n::StringId WindowArrangeLabel(app::WindowArrangeLayout layout) {
    using I = l10n::StringId;
    constexpr I ids[] = {I::ArrangeSideBySide, I::ArrangeStacked, I::ArrangeColumns,
                         I::ArrangeTile, I::ArrangeMain, I::ArrangeCascade, I::ArrangeMainRight, I::ArrangeMainTop};
    const auto i = static_cast<unsigned>(layout);
    return ids[i < 8 ? i : 0];
}
ui::fluent::MenuPictogram WindowArrangePictogram(app::WindowArrangeLayout layout) {
    using P = ui::fluent::MenuPictogram;
    constexpr P icons[] = {P::LayoutSideBySide, P::LayoutStacked, P::LayoutColumns,
                           P::LayoutFour, P::LayoutThree, P::LayoutCascade, P::LayoutMainRight, P::LayoutMainTop};
    const auto i = static_cast<unsigned>(layout);
    return icons[i < 8 ? i : 0];
}
std::vector<ui::FluentMenuItem> WindowArrangementMenuItems() {
    std::vector<ui::FluentMenuItem> items;
    for (int row = 0; row < 3; ++row) {
        ui::FluentMenuItem tiles;
        for (int col = 0; col < 3; ++col) {
            const int i = row * 3 + col;
            const auto layout = static_cast<app::WindowArrangeLayout>(i);
            ui::FluentMenuSwatch tile;
            tile.command = 10 + i;
            tile.label = l10n::Get(i == 8 ? l10n::StringId::ArrangeMerge : WindowArrangeLabel(layout));
            tile.pictogram = i == 8 ? ui::fluent::MenuPictogram::WindowMerge : WindowArrangePictogram(layout);
            tile.mnemonic = static_cast<wchar_t>(L'1' + i);
            tiles.quick_swatches.push_back(std::move(tile));
        }
        items.push_back(std::move(tiles));
    }
    return items;
}
std::vector<ui::FluentMenuItem> TrayMenuItems(AppState &s) {
    using I = l10n::StringId;
    using P = ui::fluent::MenuPictogram;
    std::vector<ui::FluentMenuItem> items;
    auto add = [&](int command, I label, const wchar_t *glyph, P pictogram = P::None) {
        ui::FluentMenuItem item;
        item.command = command;
        item.text = l10n::Get(label);
        item.glyph = glyph;
        item.pictogram = pictogram;
        items.push_back(std::move(item));
    };
    add(1, I::Open, L"\xE8E5");
    add(3, I::ActNewWindow, L"", P::WindowNew);
    add(0, I::SettingsSecArrangeWindows, L"", P::WindowOverview);
    auto &arrange = items.back();
    // Tray arrangement includes hidden/minimized windows, just like its command.
    const auto count = WindowArrangementCandidates(s, (GetKeyState(VK_SHIFT) & 0x8000) != 0, {}, true).size();
    arrange.shortcut = std::to_wstring(count);
    arrange.enabled = count >= 2;
    for (int i = 0; arrange.enabled && i < 6; ++i) {
        const auto layout = static_cast<app::WindowArrangeLayout>(i);
        ui::FluentMenuItem child;
        child.command = 10 + i;
        child.text = l10n::Get(WindowArrangeLabel(layout));
        child.pictogram = WindowArrangePictogram(layout);
        arrange.children.push_back(std::move(child));
    }
    add(4, I::ActMergeWindows, L"", P::WindowMerge);
    // Merge spans all monitors, unlike arrangement's default current monitor.
    items.back().enabled = WindowArrangementCandidates(s, true, {}, true).size() >= 2;
    items.back().separator_after = true;
    add(2, I::TrayExit, L"\xE8BB");
    return items;
}
int TrackTrayMenu(AppState &s, POINT screen, HWND owner) {
    // Query before activating our popup: Explorer may close its overflow then.
    NOTIFYICONIDENTIFIER icon{sizeof(icon)};
    icon.hWnd = owner;
    icon.uID = 1;
    RECT anchor{};
    if (FAILED(Shell_NotifyIconGetRect(&icon, &anchor)) ||
        anchor.right <= anchor.left || anchor.bottom <= anchor.top) {
        anchor = {screen.x, screen.y, screen.x, screen.y};
    }
    screen = {anchor.left + (anchor.right - anchor.left) / 2,
              anchor.top + (anchor.bottom - anchor.top) / 2};
    // The notification icon can be on a different-DPI monitor from the UI.
    UINT dpi_x = static_cast<UINT>(96.0f * s.scale), dpi_y = dpi_x;
    using MonitorDpi = HRESULT(WINAPI*)(HMONITOR, MONITOR_DPI_TYPE, UINT*, UINT*);
    const auto monitor_dpi = reinterpret_cast<MonitorDpi>(compat::ScalingApi("GetDpiForMonitor"));
    if (monitor_dpi) monitor_dpi(MonitorFromPoint(screen, MONITOR_DEFAULTTONEAREST), MDT_EFFECTIVE_DPI, &dpi_x, &dpi_y);
    ui::FluentMenu menu;
    if (!menu.Create(owner, &s.compositor, static_cast<float>(dpi_x) / 96.0f)) return 0;
    menu.SetTopmost(true);
    menu.SetActivateOnOpen(true);
    menu.SetDropdownRect(anchor, true);
    menu.SetTheme(s.darkMode, s.accentColor);
    menu.SetRowHeightDip(static_cast<float>(app::MenuRowHeightDip(
        app::EffectiveRowHeightDip(s.appPrefs.row_height, s.appPrefs.ui_font_scale))));
    return menu.TrackPopup(screen, TrayMenuItems(s));
}
int PickWindowArrangement(AppState &s, POINT screen) {
    if (!EnsureMenu(s)) return -1;
    const int command = s.menu->TrackPopup(screen, WindowArrangementMenuItems());
    return command >= 10 && command < 19 ? command - 10 : -1;
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
    items.back().pictogram = ui::fluent::MenuPictogram::WindowNew;
    add(2, l10n::StringId::WindowOverviewTitle, L"\xE8A9", L"Ctrl+Shift+O");
    items.back().pictogram = ui::fluent::MenuPictogram::WindowOverview;
    add(3, l10n::StringId::ActTabToWindow, L"\xE8A7", L"Ctrl+Shift+M");
    items.back().pictogram = ui::fluent::MenuPictogram::WindowDetach;
    items.back().enabled = s.window_tabs.items.size() > 1;
    if (auto *tab = ActiveTab(s))
        items.back().text += L" · " + app::TabTitle(tab->current_path);
    items.back().separator_after = true;
    ui::FluentMenuItem heading;
    heading.heading = true;
    heading.text = l10n::Get(l10n::StringId::SettingsSecArrangeWindows);
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
    heading.shortcut = std::move(count.text);
    items.push_back(std::move(heading));
    for (auto& item : WindowArrangementMenuItems()) items.push_back(std::move(item));
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
    RECT owner{};
    GetWindowRect(s.hwnd, &owner);
    screen.x = (std::max)(owner.left + static_cast<LONG>(8*s.scale),
        (std::min)(screen.x, owner.right - static_cast<LONG>(328*s.scale)));
    const int cmd = s.menu->TrackPopup(screen, std::move(items));
    if (cmd == 1)
        PostMessageW(s.hwnd, WM_NEW_WINDOW, 0, 0);
    else if (cmd == 2)
        ToggleWindowOverview(s);
    else if (cmd == 3)
        PostMessageW(s.hwnd, WM_MOVE_TAB_TO_NEW_WINDOW, 0, 0);
    else if (cmd == 18)
        MergeAllWindows(s);
    else if (cmd >= 10 && cmd < 18)
        ArrangeWindows(s, static_cast<app::WindowArrangeLayout>(cmd - 10), (GetKeyState(VK_SHIFT) & 0x8000) != 0);
}
} // namespace pulse
