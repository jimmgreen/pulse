// Private-desktop regression for dynamically discovered open-with settings.
template<class Check>
void RunOpenWithSettingsUiChecks(pulse::AppState& state, const std::filesystem::path& output, Check&& check) {
    using namespace pulse;
    using namespace pulse::app;
    ContextMenuPrefs prefs; prefs.persist = false;
    ContextMenuController controller;
    controller.StartQuery(prefs, nullptr, {L"C:\\fixture\\sample.xlsx"}, false, L".xlsx", false, {}, {});
    StaticVerb word; word.verb = L"__openwith"; word.display = L"用 Word 打开";
    word.app_path = L"C:\\fixture\\WINWORD.EXE";
    StaticVerb notepad = word; notepad.display = L"用 Notepad3 打开"; notepad.app_path = L"C:\\fixture\\Notepad3.exe";
    controller.CompleteStaticVerbs(L".xlsx", {word, notepad}, controller.cache_generation());
    bool changed = false;
    auto display = controller.BuildDisplay(prefs, {}, changed);
    const auto roots = prefs.NodeChildren(L"");
    auto found = std::find_if(roots.begin(), roots.end(), [&](const auto& node) { return node.text == word.display; });
    check(found != roots.end() && found->configurable, "real controller makes Word available to the settings organizer");
    if (found == roots.end() || !found->configurable) return;
    const auto key = found->key;
    ui::FluentMenu menu;
    check(menu.Create(state.hwnd, &state.compositor, state.scale), "dynamic app organizer uses production popup renderer");
    menu.SetTheme(state.darkMode, state.accentColor);
    auto invoke = [&](bool visible) {
        const auto page = BuildSettingsContextTreePage(prefs);
        auto action = std::find_if(page.actions.begin(), page.actions.end(), [&](const auto& pair) {
            return pair.second.kind == ContextTreeActionKind::SetVisible && pair.second.key == key && pair.second.visible == visible;
        });
        if (action == page.actions.end()) return false;
        std::vector<WPARAM> keys;
        if (!TreeCommandKeys(page.items, action->first, keys)) return false;
        for (auto press : keys) PostMessageW(state.hwnd, WM_KEYDOWN, press, 0);
        tree_test_menu = &menu;
        const auto timer = SetTimer(nullptr, 0, 3000, TreeMenuWatchdog);
        if (!timer) { tree_test_menu = nullptr; return false; }
        const auto selected = menu.TrackPopup({100, 100}, page.items);
        KillTimer(nullptr, timer); tree_test_menu = nullptr;
        std::wstring parent;
        return selected == action->first &&
            ApplySettingsContextTreeAction(prefs, action->second, parent) == ContextTreeActionResult::Changed;
    };
    check(invoke(false), "actual popup navigation selects hide for Word");
    display = controller.BuildDisplay(prefs, {}, changed);
    check(std::none_of(display.begin(), display.end(), [&](const auto& row) { return row.text == word.display; }) &&
          std::any_of(display.begin(), display.end(), [&](const auto& row) { return row.text == notepad.display; }),
          "production next menu hides Word without hiding Notepad");
    check(menu.SaveDebugSnapshot((output / L"menu-word-hidden.png").c_str(), display), "hidden result rendered by actual menu");
    const auto language = l10n::preference();
    for (const auto* locale : {L"zh-CN", L"en-US"}) {
        l10n::SetLanguage(locale);
        for (float scale : {1.0f, 1.5f, 2.0f}) for (bool dark : {false, true}) {
            state.compositor.RecreateTextFormats(scale);
            ui::FluentMenu snapshot;
            if (!snapshot.Create(state.hwnd, &state.compositor, scale)) {
                check(false, "organizer scaled renderer creation"); continue;
            }
            snapshot.SetTheme(dark, state.accentColor);
            auto page = BuildSettingsContextTreePage(prefs);
            const auto name = L"organizer-" + std::wstring(locale) + L"-" +
                std::to_wstring(static_cast<int>(scale * 100)) + (dark ? L"-dark.png" : L"-light.png");
            check(snapshot.SaveDebugSnapshot((output / name).c_str(), page.items),
                  "dynamic app organizer file-type and hidden state rendered at requested DPI/theme");
        }
    }
    l10n::SetLanguage(l10n::LanguageId(language));
    state.compositor.RecreateTextFormats(state.scale);
    check(invoke(true), "actual popup restores Word from the hidden row");
    display = controller.BuildDisplay(prefs, {}, changed);
    check(std::any_of(display.begin(), display.end(), [&](const auto& row) { return row.text == word.display; }),
          "restoring the row returns Word to the real menu output");
}
