#pragma once
#include "../app/context_menu.h"
#include "../app/app_model.h"
#include <algorithm>

// Include from the app's focused regression entry point; no windows or IO needed.
template<class Check>
void RunRecentChangesMenuChecks(Check check) {
    using namespace pulse;
    app::Tab tab;
    tab.current_path = L"C:\\fixture";
    auto entries = std::make_shared<std::vector<fs::DirEntry>>(2);
    (*entries)[0].name = L"folder";
    (*entries)[0].is_dir = true;
    (*entries)[1].name = L"file.txt";
    tab.snapshot = entries;
    tab.SelectOnly(0);
    check(app::RecentChangesMenuPath(tab, false) == L"C:\\fixture\\folder", "single folder recent changes target");
    check(app::RecentChangesMenuPath(tab, true) == tab.current_path, "background uses directory despite selection");
    (*entries)[0].change_record_only = true;
    check(app::RecentChangesMenuPath(tab, false).empty(), "deleted directory has no recent changes command");
    (*entries)[0].change_record_only = false;
    tab.SelectRange(0, 1);
    check(app::RecentChangesMenuPath(tab, false).empty(), "multiple selection has no recent changes command");
    tab.SelectOnly(1);
    check(app::RecentChangesMenuPath(tab, false).empty(), "ordinary file has no recent changes command");
    (*entries)[1].link_target = L"C:\\target";
    (*entries)[1].link_target_is_dir = true;
    check(app::RecentChangesMenuPath(tab, false) == L"C:\\target", "resolved folder shortcut uses its target");
    tab.current_path = L"pulse:search:";
    check(app::RecentChangesMenuPath(tab, true).empty(), "virtual background has no recent changes command");
    tab.SelectOnly(0);
    check(app::RecentChangesMenuPath(tab, false).empty(), "virtual row without concrete path has no command");
    (*entries)[0].full_path = L"C:\\real";
    check(app::RecentChangesMenuPath(tab, false) == L"C:\\real", "real folder search result retains command");
    auto menu = app::BuildItemMenu(false, {}, true);
    app::AppendRecentChangesCommand(menu, L"C:\\real");
    app::ContextMenuPrefs prefs;
    app::ShellMenuEntry native;
    native.command = app::CmdShellStaticBase;
    native.text = L"Native extension";
    app::AppendShellSection(menu, app::ApplyExplorerPrefs(prefs, {native}));
    check(std::count_if(menu.begin(), menu.end(), [](const auto& item) {
        return item.command == app::CmdViewRecentChanges;
    }) == 1, "native menu merge preserves exactly one built-in recent changes command");
}
