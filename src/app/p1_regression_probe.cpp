#ifdef PULSE_WITH_SELFTEST
#include "app_input.h"
#include "app_runtime.h"
#include "app_navigation.h"
#include "entry_group.h"
#include "entry_sort.h"
#include "context_menu_controller.h"
#include "session.h"
#include <algorithm>
#include <cstdio>

namespace pulse::ui {
struct P1MenuTestPeer {
    static bool Seed(FluentMenu& menu, const std::vector<FluentMenuItem>& items) {
        if (!menu.EnsureWindow()) return false;
        menu.model_.SetItems(items);
        menu.model_.Layout(menu.compositor_->DwriteFactory(), 1.0f);
        menu.open_ = true;
        menu.LayoutWindow({100, 100});
        menu.OpenSubmenu(0);
        return menu.first_sub_.parent_row == 0;
    }
    static int ChildCommand(FluentMenu& menu) { return menu.InvokeAt(menu.first_sub_.model, 0, 50.0f); }
    static int RootChildCommand(FluentMenu& menu) { return menu.model_.At(0)->children[0].command; }
    static void Close(FluentMenu& menu) { menu.CloseSubmenu(); }
    static void Reopen(FluentMenu& menu) { menu.OpenSubmenu(0); }
    static int Click(FluentMenu& menu) {
        const int y = FluentMenu::kShadowMargin + static_cast<int>(menu.first_sub_.model.RowTopPx(0) + 5);
        menu.OnSubMouse({50, y}, true);
        return menu.result_;
    }
};
}

using namespace pulse;
void RecordP1StartupRollback(const AppState& s) {
    if (!GetEnvironmentVariableW(L"PULSE_TEST_DATA_DIR", nullptr, 0) ||
        !GetEnvironmentVariableW(L"PULSE_TEST_STARTUP_FAIL", nullptr, 0)) return;
    FILE* log = nullptr;
    const auto path = app::GetPulseDataDir() + L"\\startup-rollback.txt";
    if (_wfopen_s(&log, path.c_str(), L"w") || !log) return;
    std::fprintf(log, "WM_DESTROY startupComplete=%d isolatedTest=%d\n", s.startupComplete, s.isolatedTest);
    std::fclose(log);
}
int RunMenuIdentityChecks(AppState& s, FILE* log) {
    int failures = 0;
    auto check = [&](bool ok, const char* label) {
        std::fprintf(log, "[%s] %s\n", ok ? "PASS" : "FAIL", label);
        std::fflush(log);
        failures += !ok;
    };
    for (const bool reorder : {false, true}) {
        ui::FluentMenu menu;
        check(menu.Create(s.hwnd, &s.compositor, 1.0f), "M14 create real Windows popup");
        ui::FluentMenuItem parent, a, b;
        parent.text = L"Shell fixture"; a.text = L"A"; a.command = 10; b.text = L"B"; b.command = 11;
        parent.children = {a, b};
        std::vector<ui::FluentMenuItem> old{parent}, fresh{parent};
        fresh[0].children[0].command = 11;
        fresh[0].children[1].command = 10;
        if (reorder) std::reverse(fresh[0].children.begin(), fresh[0].children.end());
        check(ui::P1MenuTestPeer::Seed(menu, old), "M14 open real flyout from cached snapshot");
        check(!menu.ReplaceItems(fresh), "M14 expanded flyout rejects swapped/reordered session snapshot");
        check(ui::P1MenuTestPeer::RootChildCommand(menu) == 10 && ui::P1MenuTestPeer::ChildCommand(menu) == 10,
              "M14 root and keyboard-invoke identity retain displayed A");
        if (!reorder) {
            ui::P1MenuTestPeer::Close(menu);
            check(menu.ReplaceItems(fresh), "M14 closed flyout accepts matching fresh IDs");
            ui::P1MenuTestPeer::Reopen(menu);
            check(ui::P1MenuTestPeer::ChildCommand(menu) == 11, "M14 reopened A carries fresh ID");
            check(ui::P1MenuTestPeer::Click(menu) == 11, "M14 flyout mouse click returns displayed A ID");
        } else {
            check(ui::P1MenuTestPeer::Click(menu) == 10, "M14 rejected reorder mouse click retains old A identity");
        }
    }
    for (const bool removed : {false, true}) {
        app::ContextMenuController controller;
        app::ContextMenuPrefs prefs; prefs.persist = false;
        uint32_t invoked = 0;
        std::wstring invoked_text;
        app::ContextMenuController::ShellOperations operations;
        operations.query = [](auto, HWND, bool, bool, auto) { return 17u; };
        operations.invoke = [&](uint32_t, uint32_t id, auto, auto text) { invoked = id; invoked_text = text; };
        controller.SetShellOperations(std::move(operations));
        controller.StartQuery(prefs, nullptr, {L"C:\\P1-fixture.txt"}, false, L".txt", false, {}, {});
        ops::ShellMenuItem a, b; a.id = 10; a.text = L"A"; a.verb = L"a";
        b.id = 11; b.text = L"B"; b.verb = L"b";
        controller.CompleteComQuery(17, {a, b}, GetTickCount64(), true);
        controller.OpenMenu({});
        a.id = 11; b.id = 10;
        controller.CompleteComQuery(17, removed ? std::vector<ops::ShellMenuItem>{b}
                                              : std::vector<ops::ShellMenuItem>{a, b}, GetTickCount64());
        controller.CloseMenu();
        controller.ExecuteShellCommand(app::CmdShellComBase + 10, {}, {});
        check(removed ? invoked == 0 : invoked == 11 && invoked_text == L"A",
              "M14 production controller remaps shown A or rejects removed A; never invokes B");
    }
    {
        app::ContextMenuController controller;
        app::ContextMenuPrefs prefs; prefs.persist = false;
        std::wstring invoked;
        app::ContextMenuController::ShellOperations operations;
        operations.query = [](auto, HWND, bool, bool, auto) { return 17u; };
        operations.execute_verb = [&](const auto&, const auto& verb) { invoked = verb; };
        controller.SetShellOperations(std::move(operations));
        app::StaticVerb a, b; a.verb = L"a"; a.display = L"A"; b.verb = L"b"; b.display = L"B";
        controller.CompleteStaticVerbs(L".txt", {a, b}, controller.cache_generation());
        controller.StartQuery(prefs, nullptr, {L"C:\\P1-fixture.txt"}, false, L".txt", false, {}, {});
        controller.OpenMenu({});
        controller.CompleteStaticVerbs(L".txt", {b, a}, controller.cache_generation());
        controller.CloseMenu();
        controller.ExecuteShellCommand(app::CmdShellStaticBase, {}, {});
        check(invoked == L"a", "M14 static verb reorder retains displayed snapshot");
    }
    return failures;
}
int RunLayoutRegressionProbe(AppState& s, const wchar_t* output);
int RunP1RegressionProbe(AppState& s, const wchar_t* output) {
    wchar_t layout_scope[32]{};
    if (GetEnvironmentVariableW(L"PULSE_TEST_P1_SCOPE", layout_scope, ARRAYSIZE(layout_scope)) &&
        std::wstring_view(layout_scope) == L"layout") return RunLayoutRegressionProbe(s, output);
    FILE* log = nullptr;
    if (!s.isolatedTest || _wfopen_s(&log, output, L"w") || !log) return 2;
    int failures = 0;
    auto check = [&](bool ok, const char* label) {
        std::fprintf(log, "[%s] %s\n", ok ? "PASS" : "FAIL", label);
        std::fflush(log);
        failures += !ok;
    };
    if (std::wstring_view(layout_scope) == L"menu") {
        const int result = RunMenuIdentityChecks(s, log);
        std::fclose(log);
        return result ? 1 : 0;
    }
    auto* tab = ActiveTab(s);
    if (!tab) { std::fclose(log); return 3; }
    wchar_t fixture_root[32768]{};
    const DWORD fixture_length = GetEnvironmentVariableW(L"PULSE_TEST_DATA_DIR", fixture_root, ARRAYSIZE(fixture_root));
    if (!fixture_length || fixture_length >= ARRAYSIZE(fixture_root)) { std::fclose(log); return 3; }
    tab->current_path = std::wstring(fixture_root) + L"\\selection-fixture";
    auto entries = std::make_shared<std::vector<fs::DirEntry>>(3);
    for (int i = 0; i < 3; ++i) {
        (*entries)[i].name = i == 0 ? L"a.keep" : i == 1 ? L"b.secret" : L"c.keep";
        (*entries)[i].attrs = FILE_ATTRIBUTE_NORMAL;
        (*entries)[i].size = static_cast<uint64_t>(i + 1);
    }
    tab->SetSnapshot(entries);
    tab->loading = false;
    tab->view_mode = ui::ViewMode::Details;
    tab->filter_text = L"*.keep";
    tab->group_by = 0;
    tab->compare_marks.reset();
    auto selected_pair = [&] {
        auto paths = SelectedFullPaths(*tab);
        std::sort(paths.begin(), paths.end());
        const std::vector<std::wstring> expected{tab->current_path + L"\\a.keep", tab->current_path + L"\\c.keep"};
        uint64_t bytes = 0;
        int files = 0, folders = 0;
        tab->SelectionSizeSummary(&bytes, &files, &folders);
        return tab->SelectedCount() == 2 && tab->IsSelected(0) && !tab->IsSelected(1) && tab->IsSelected(2) &&
            paths == expected && bytes == 4 && files == 2 && folders == 0;
    };
    auto range_click = [&](int from, int to) {
        tab->SelectOnly(from);
        HandleListRowClick(s, to, false, true);
    };
    range_click(0, 2);
    check(selected_pair(), "M10 text filter forward Shift-click exports only visible operation sources");
    range_click(2, 0);
    check(selected_pair(), "M10 text filter reverse Shift-click");
    BYTE old_keys[256]{};
    GetKeyboardState(old_keys);
    BYTE shift_keys[256]{};
    shift_keys[VK_SHIFT] = 0x80;
    SetKeyboardState(shift_keys);
    SetFocus(s.hwnd);
    for (const auto key : {VK_DOWN, VK_END, VK_NEXT}) {
        tab->SelectOnly(0);
        HandleKeyDown(&s, s.hwnd, WM_KEYDOWN, key, 0);
        check(selected_pair(), "M10 Shift Down/End/PageDown uses displayed members");
    }
    for (const auto key : {VK_UP, VK_HOME, VK_PRIOR}) {
        tab->SelectOnly(2);
        HandleKeyDown(&s, s.hwnd, WM_KEYDOWN, key, 0);
        check(selected_pair(), "M10 Shift Up/Home/PageUp uses displayed members");
    }
    SetKeyboardState(old_keys);
    tab->SelectOnly(1);
    HandleListRowClick(s, 2, false, true);
    check(tab->SelectedCount() == 1 && tab->IsSelected(2), "M10 filtered-out anchor restarts at target");

    const auto tag = s.places.CreateTag(L"P1Keep", 0x008800);
    std::vector<app::TagAdsUpdate> deferred_ads;
    const std::vector<std::wstring> tagged_paths{tab->current_path + L"\\a.keep", tab->current_path + L"\\c.keep"};
    s.places.SetTaggedBatch(s.places.FindTagIndex(tag), tagged_paths, true, &deferred_ads);
    tab->filter_text = L"#P1Keep";
    range_click(0, 2);
    check(selected_pair(), "M10 tag filter exports only tagged visible members");
    tab->filter_text = L"*.keep #P1Keep";
    range_click(2, 0);
    check(selected_pair(), "M10 combined tag and text reverse range");
    tab->filter_text.clear();
    tab->compare_marks = std::make_shared<std::vector<uint8_t>>(std::initializer_list<uint8_t>{1, 0, 1});
    tab->compare_diff_only = true;
    range_click(0, 2);
    check(selected_pair(), "M10 difference-only filter excludes identical file");
    tab->filter_text = L"*.keep";
    range_click(2, 0);
    check(selected_pair(), "M10 combined difference and text filters");
    tab->compare_marks.reset();
    tab->filter_text.clear();
    auto hidden = std::make_shared<std::vector<fs::DirEntry>>(*entries);
    (*hidden)[1].attrs = FILE_ATTRIBUTE_HIDDEN;
    tab->SetSnapshot(hidden);
    range_click(0, 2);
    check(selected_pair(), "M10 hidden-attribute control remains excluded");

    tab->SetSnapshot(entries);
    tab->filter_text = L"*.keep";
    auto descending = std::make_shared<std::vector<fs::DirEntry>>(*entries);
    std::reverse(descending->begin(), descending->end());
    tab->SetSnapshot(descending);
    tab->sort_direction = ui::SortDirection::Desc;
    range_click(0, 2);
    check(selected_pair(), "M10 descending snapshot selects only displayed entries");
    tab->SetSnapshot(entries);
    tab->filter_text.clear();
    ui::PaneViewModel grouped;
    app::FillPaneViewModel(grouped, *s.pane, &s.places);
    auto groups = std::make_shared<ui::ListGroups>(3);
    for (int i = 0; i < 3; ++i) { (*groups)[i].first = i; (*groups)[i].count = 1; }
    (*groups)[1].collapsed = true;
    grouped.groups = groups;
    tab->SelectRange(0, 2, &grouped);
    check(selected_pair(), "M10 collapsed middle group is not selectable");

    for (const auto direction : {ui::SortDirection::Asc, ui::SortDirection::Desc}) {
        auto sorted = std::make_shared<std::vector<fs::DirEntry>>(*entries);
        std::sort(sorted->begin(), sorted->end(), [&](const auto& a, const auto& b) {
            return app::EntryLess(a, b, ui::SortColumn::Name, direction, app::FolderSortMode::Mixed);
        });
        tab->sort_direction = direction;
        tab->SetSnapshot(sorted);
        tab->filter_text = L"*.keep";
        range_click(0, 2);
        check(selected_pair(), "M10 sorted forward range exports exact sources and size");
        range_click(2, 0);
        check(selected_pair(), "M10 sorted reverse range exports exact sources and size");
    }
    tab->sort_direction = ui::SortDirection::Asc;
    tab->filter_text.clear();
    auto attribute_hidden = std::make_shared<std::vector<fs::DirEntry>>(*entries);
    (*attribute_hidden)[1].attrs = FILE_ATTRIBUTE_HIDDEN;
    tab->SetShowHiddenFiles(false);
    tab->SetSnapshot(attribute_hidden);
    range_click(0, 2);
    check(selected_pair(), "M10 attribute-hidden control excludes operation source");
    tab->SetShowHiddenFiles(true);
    range_click(0, 2);
    uint64_t bytes = 0;
    int files = 0, folders = 0;
    tab->SelectionSizeSummary(&bytes, &files, &folders);
    check(tab->SelectedCount() == 3 && SelectedFullPaths(*tab).size() == 3 && bytes == 6 && files == 3,
          "M10 revealed normal hidden item joins displayed range");
    (*attribute_hidden)[1].attrs = FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM;
    tab->SetShowProtectedOsFiles(false);
    tab->SetSnapshot(attribute_hidden);
    range_click(2, 0);
    check(selected_pair(), "M10 protected attribute control remains excluded");
    auto records = std::make_shared<std::vector<fs::DirEntry>>(*entries);
    (*records)[1].change_record_only = true;
    tab->SetSnapshot(records);
    range_click(0, 2);
    tab->SelectionSizeSummary(&bytes, &files, &folders);
    check(tab->SelectedCount() == 3 && SelectedFullPaths(*tab) ==
          std::vector<std::wstring>{tab->current_path + L"\\a.keep", tab->current_path + L"\\c.keep"} &&
          bytes == 4 && files == 2, "M10 record-only row never becomes file operation source");
    {
        auto store = std::make_shared<index::ContentResultStore>(nullptr, 0);
        std::vector<index::ContentHit> hits;
        for (const auto& entry : *entries) {
            index::ContentHit hit;
            hit.name = entry.name;
            hit.path = tab->current_path + L"\\" + entry.name;
            hit.size = entry.size;
            hits.push_back(std::move(hit));
        }
        const bool stored = store->StreamUpsert(hits, index::ContentResultSort::Name, false);
        store->SetFilter([](const fs::DirEntry& entry) { return entry.name.ends_with(L".keep"); });
        const auto started = GetTickCount64();
        while (store->Filtering() && GetTickCount64() - started < 5000) Sleep(1);
        index::ContentResultStore::Row row;
        while (!store->Get(1, row) && GetTickCount64() - started < 5000) Sleep(1);
        tab->content_results = store;
        tab->content_order_revision = store->OrderRevision();
        tab->filter_text = L"*.keep";
        range_click(0, 1);
        check(stored && !store->Filtering() && store->Count() == 2 && store->Ready(1) &&
              tab->SelectedCount() == 2 && SelectedFullPaths(*tab) ==
              std::vector<std::wstring>{tab->current_path + L"\\a.keep", tab->current_path + L"\\c.keep"},
              "M10 paged content selection keeps store display indices and exact operation sources");
        tab->content_results.reset();
        tab->content_action_rows.clear();
        tab->SetSnapshot(entries);
        tab->filter_text.clear();
    }
    wchar_t scope[32]{};
    if (GetEnvironmentVariableW(L"PULSE_TEST_P1_SCOPE", scope, ARRAYSIZE(scope)) &&
        std::wstring_view(scope) == L"selection") {
        std::fclose(log);
        return failures ? 1 : 0;
    }

    failures += RunMenuIdentityChecks(s, log);
    check(s.startupComplete, "M18 normal WM_CREATE commits initialization state");
    const bool shot = s.shot.active;
    const bool persist = s.isolatedTestPersist;
    s.shot.active = false;
    s.isolatedTestPersist = true;
    s.startupComplete = false;
    check(!PrepareSessionForUpdate(s), "M18 update handshake cannot save uninitialized state");
    s.startupComplete = true;
    s.appPrefs.persist = true;
    check(PrepareSessionForUpdate(s), "M18 update handshake saves initialized session into isolated data dir");
    s.appPrefs.persist = false;
    s.shot.active = shot;
    s.isolatedTestPersist = persist;
    std::fclose(log);
    return failures ? 1 : 0;
}
#endif
