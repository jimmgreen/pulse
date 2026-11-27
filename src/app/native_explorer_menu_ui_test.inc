// Included only by the private-desktop system-context UI test hook.
namespace pulse {
void SetNativeExplorerDispatchTestSink(std::function<DWORD(const app::native_explorer::Request&)> sink);
}
template<class Check>
void RunNativeExplorerMenuUiChecks(pulse::AppState& state,
                                   const std::filesystem::path& output, Check&& check) {
    using namespace pulse;
    using namespace pulse::app;
    const auto language = l10n::preference();
    for (const auto* locale : {L"zh-CN", L"en-US"}) {
        l10n::SetLanguage(locale);
        for (float scale : {1.0f, 1.5f, 2.0f}) for (bool dark : {false, true}) {
            state.compositor.RecreateTextFormats(scale);
            ui::FluentMenu menu;
            if (!menu.Create(state.hwnd, &state.compositor, scale)) {
                check(false, "native Explorer menu renderer creation");
                continue;
            }
            menu.SetTheme(dark, state.accentColor);
            for (bool background : {false, true}) {
                auto rows = background ? BuildBackgroundMenu(false, false, {}, true) :
                                         BuildItemMenu(false, {}, true, true);
                ContextMenuPrefs prefs;
                prefs.persist = false;
                ApplyBuiltinMenuPrefs(rows, prefs, background ? BuiltinMenuSurface::Background :
                                                               BuiltinMenuSurface::Item);
                const int command = background ? CmdOpenNativeExplorerHere : CmdOpenNativeExplorer;
                check(std::count_if(rows.begin(), rows.end(), [=](const auto& row) {
                    return row.command == command && row.enabled && !row.text.empty();
                }) == 1, "production menu contains exactly one actionable native Explorer row");
                const auto name = std::wstring(background ? L"background-" : L"item-") + locale +
                    L"-" + std::to_wstring(static_cast<int>(scale * 100)) +
                    (dark ? L"-dark.png" : L"-light.png");
                check(menu.SaveDebugSnapshot((output / name).c_str(), rows),
                      "native Explorer row production-renderer snapshot saved");
                if (scale == 1.0f && !dark) {
                    std::vector<WPARAM> keys;
                    const bool found = TreeCommandKeys(rows, command, keys);
                    check(found, "native Explorer row is reachable through real menu keyboard navigation");
                    if (!found) continue;
                    for (auto key : keys) PostMessageW(state.hwnd, WM_KEYDOWN, key, 0);
                    tree_test_menu = &menu;
                    const auto timer = SetTimer(nullptr, 0, 3000, TreeMenuWatchdog);
                    if (!timer) {
                        tree_test_menu = nullptr;
                        check(false, "native Explorer popup watchdog creation");
                        continue;
                    }
                    const int selected = menu.TrackPopup({100, 100}, rows);
                    KillTimer(nullptr, timer);
                    tree_test_menu = nullptr;
                    check(selected == command, "actual popup returns the correct item/background native Explorer command");
                }
            }
        }
    }
    l10n::SetLanguage(l10n::LanguageId(language));
    state.compositor.RecreateTextFormats(state.scale);
    auto* tab = ActiveTab(state);
    check(state.isolatedTest && tab, "dispatcher checks run only in an isolated app instance");
    if (!state.isolatedTest || !tab) return;
    auto saved_tab = std::move(*tab);
    *tab = Tab{};
    tab->current_path = output.wstring();
    auto entries = std::make_shared<std::vector<fs::DirEntry>>();
    fs::DirEntry file; file.name = L"sample.txt";
    fs::DirEntry folder; folder.name = L"folder"; folder.is_dir = true;
    entries->push_back(file); entries->push_back(folder);
    tab->SetSnapshot(entries);
    std::vector<native_explorer::Request> submitted;
    SetNativeExplorerDispatchTestSink([&](const native_explorer::Request& request) {
        submitted.push_back(request);
        return ERROR_SUCCESS;
    });
    tab->SelectOnly(0);
    DispatchMenuCommand(state, CmdOpenNativeExplorer);
    check(submitted.size() == 1 && submitted.back().intent == native_explorer::Intent::SelectFile &&
          submitted.back().path == (output / L"sample.txt").wstring(),
          "production dispatcher queues file reveal rather than opening the file");
    tab->SelectOnly(1);
    DispatchMenuCommand(state, CmdOpenNativeExplorer);
    check(submitted.size() == 2 && submitted.back().intent == native_explorer::Intent::OpenFolder &&
          submitted.back().path == (output / L"folder").wstring(),
          "production dispatcher queues opening the selected folder");
    DispatchMenuCommand(state, CmdOpenNativeExplorerHere);
    check(submitted.size() == 3 && submitted.back().intent == native_explorer::Intent::OpenFolder &&
          submitted.back().path == output.wstring(),
          "production background dispatcher ignores retained selected folder");
    tab->current_path = L"pulse:vfs:fixture/archive";
    DispatchMenuCommand(state, CmdOpenNativeExplorerHere);
    check(submitted.size() == 3, "production dispatcher rejects virtual-folder native launch");
    SetNativeExplorerDispatchTestSink({});
    *tab = std::move(saved_tab);
}
