#include "../app/button_press.h"
#include <iostream>

int main() {
    using namespace pulse;
    using H = ui::HitTestResult;
    int failures = 0;
    auto check = [&](bool ok, const char* label) {
        std::cout << (ok ? "[PASS] " : "[FAIL] ") << label << '\n';
        if (!ok) ++failures;
    };
    H hit;
    for (const auto region : {H::Close, H::Minimize, H::Maximize, H::TabNew, H::TabClose, H::Delete, H::NavBack,
             H::SettingsToggle, H::SettingsDisclosure, H::SettingsDialogManagement,
             H::SettingsPackAction, H::RowCheck, H::PaneClose, H::StatusUndo}) {
        hit.region = region;
        check(app::IsReleaseButton(hit), "ordinary command uses release activation");
    }
    for (const auto region : {H::Row, H::Tab, H::TabGroup, H::PaneSizeSlider,
             H::ColumnDivider, H::Splitter, H::Scrollbar, H::SettingsAccentSlider,
             H::ToolbarPin, H::ToolbarSort, H::ToolbarGroup, H::AddressBar,
             H::FavoritesBarItem, H::TrayCard}) {
        hit.region = region;
        check(!app::IsReleaseButton(hit), "selection/text/drag keeps press handling");
    }
    hit.region = H::SettingsToggle;
    hit.index = ui::kSettingsMenuRowHit;
    check(!app::IsReleaseButton(hit), "menu reorder row remains draggable");
    hit.region = H::SettingsSysCtx;
    hit.index = ui::kSysCtxPreviewBase;
    check(!app::IsReleaseButton(hit), "system menu preview remains draggable");
    hit.region = H::SettingsAssistant;
    hit.index = ui::kAsstThinkBase;
    check(!app::IsReleaseButton(hit), "assistant thinking slider stays continuous");
    hit.region = H::SettingsPackAction;
    hit.index = static_cast<int>(ui::PackAction::SzLevelBase);
    check(!app::IsReleaseButton(hit), "archive level slider stays continuous");

    hit.region = H::TabClose;
    hit.index = 2;
    app::ButtonPress press;
    app::ButtonPressContext context;
    context.generation = 10;
    check(!press.Matches(hit, context), "release without press is ignored");
    press.Begin(hit, context);
    check(press.active && press.inside && press.Matches(hit, context), "same target is armed");
    auto other = hit;
    other.region = H::TabNew;
    check(!press.Matches(other, context), "another button cannot activate");
    other = hit;
    ++other.index;
    check(!press.Matches(other, context), "another index cannot activate");
    other = hit;
    ++other.pane_index;
    check(!press.Matches(other, context), "another pane cannot activate");
    other = hit;
    other.path = L"changed";
    check(!press.Matches(other, context), "replaced path cannot activate");
    auto changed = context;
    ++changed.generation;
    check(!press.Matches(hit, changed), "stale view generation cannot activate");
    changed = context;
    ++changed.settings_page;
    check(!press.Matches(hit, changed), "changed settings page cannot activate");
    changed = context;
    changed.target_tab = &press;
    check(!press.Matches(hit, changed), "replacement tab at same index cannot activate");
    press.inside = false;
    check(press.Matches(hit, context), "leaving and returning can still activate");
    press.Cancel();
    check(!press.active && !press.inside && !press.Matches(hit, context), "cancel clears pending activation");
    std::cout << "failures=" << failures << '\n';
    return failures ? 1 : 0;
}
