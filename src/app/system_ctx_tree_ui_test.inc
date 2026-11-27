// Included only by the isolated, private-desktop settings review hook.
namespace {
thread_local pulse::ui::FluentMenu* tree_test_menu = nullptr;
thread_local HWND tree_test_dialog = nullptr;
void CALLBACK TreeMenuWatchdog(HWND, UINT, UINT_PTR, DWORD) {
    if (tree_test_menu) tree_test_menu->Dismiss();
}
void CALLBACK TreeDialogWatchdog(HWND, UINT, UINT_PTR, DWORD) {
    if (IsWindow(tree_test_dialog)) PostMessageW(tree_test_dialog, WM_CLOSE, 0, 0);
}

bool TreeCommandKeys(const std::vector<pulse::ui::FluentMenuItem>& rows, int command,
                     std::vector<WPARAM>& keys) {
    size_t enabled_before = 0;
    for (const auto& row : rows) {
        if (!row.enabled || row.heading) continue;
        std::vector<WPARAM> child_keys;
        if (row.command == command || TreeCommandKeys(row.children, command, child_keys)) {
            keys.insert(keys.end(), enabled_before, VK_DOWN);
            if (row.command == command) keys.push_back(VK_RETURN);
            else {
                keys.push_back(VK_RIGHT);
                keys.insert(keys.end(), child_keys.begin(), child_keys.end());
            }
            return true;
        }
        ++enabled_before;
    }
    return false;
}

template<class Check>
void RunSystemCtxTreeUiChecks(pulse::AppState& state, const std::filesystem::path& output, Check&& check) {
    using namespace pulse;
    using namespace pulse::app;
    ContextMenuPrefs prefs;
    prefs.persist = false;
    ContextMenuController controller;
    ContextMenuController::ShellOperations operations;
    operations.query = [](auto, HWND, bool, bool, auto) { return 71u; };
    controller.SetShellOperations(std::move(operations));
    controller.StartQuery(prefs, nullptr, {L"C:\\isolated-fixture\\sample.xyz"}, false, L".xyz", false, {}, {});
    auto row = [](uint32_t id, const wchar_t* title, const wchar_t* verb, uint8_t depth, bool parent) {
        ops::ShellMenuItem item;
        item.id = id; item.text = title; item.verb = verb;
        item.clsid = L"{D762F851-9B3F-4321-A312-C812A4295C11}";
        item.enabled = true; item.depth = depth; item.child = depth != 0; item.has_children = parent;
        return item;
    };
    controller.CompleteComQuery(71, {
        row(0, L"Application menu", L"fixture-menu", 0, true),
        row(101, L"Alpha", L"fixture-alpha", 1, false),
        row(102, L"Beta", L"fixture-beta", 1, false),
        row(0, L"Nested group", L"fixture-group", 1, true),
        row(103, L"Alpha", L"fixture-alpha", 2, false)}, 1, false);
    bool changed = false;
    auto display = controller.BuildDisplay(prefs, {}, changed);
    auto roots = prefs.NodeChildren(L"");
    check(roots.size() == 1 && roots[0].text == L"Application menu", "real controller populates organizer root");
    if (roots.size() != 1) return;
    const auto root_key = roots[0].key;
    const auto children = prefs.NodeChildren(root_key);
    check(children.size() == 3, "real controller discovers second and third levels");
    if (children.size() != 3) return;
    const auto alpha = children[0].key, beta = children[1].key, nested = children[2].key;
    ui::FluentMenu menu;
    check(menu.Create(state.hwnd, &state.compositor, state.scale), "settings organizer uses real menu renderer");
    menu.SetTheme(state.darkMode, state.accentColor);
    std::wstring parent;
    auto invoke = [&](ContextTreeActionKind kind, const std::wstring& key) {
        const auto page = BuildSettingsContextTreePage(prefs, parent);
        auto action = std::find_if(page.actions.begin(), page.actions.end(), [&](const auto& entry) {
            return entry.second.kind == kind && entry.second.key == key;
        });
        if (action == page.actions.end()) return ContextTreeActionResult::Rejected;
        std::vector<WPARAM> keys;
        if (!TreeCommandKeys(page.items, action->first, keys)) return ContextTreeActionResult::Rejected;
        for (auto key_press : keys) PostMessageW(state.hwnd, WM_KEYDOWN, key_press, 0);
        tree_test_menu = &menu;
        const auto timer = SetTimer(nullptr, 0, 3000, TreeMenuWatchdog);
        if (!timer) { tree_test_menu = nullptr; return ContextTreeActionResult::Rejected; }
        const int selected = menu.TrackPopup({100, 100}, page.items);
        KillTimer(nullptr, timer);
        tree_test_menu = nullptr;
        if (selected != action->first) return ContextTreeActionResult::Rejected;
        return ApplySettingsContextTreeAction(prefs, action->second, parent);
    };
    check(invoke(ContextTreeActionKind::Open, root_key) == ContextTreeActionResult::Navigated && parent == root_key,
        "actual popup keyboard opens application child organizer");
    check(invoke(ContextTreeActionKind::SetVisible, alpha) == ContextTreeActionResult::Changed && !prefs.NodeEnabled(alpha),
        "actual popup action hides one child by stable identity");
    display = controller.BuildDisplay(prefs, {}, changed);
    check(display.size() == 2 && display.back().command == CmdSettingsContextMenu &&
        display[0].text == L"Application menu" && display[0].children.size() == 2 && display[0].children[0].text == L"Beta" &&
        display[0].children[1].children.size() == 1 && display[0].children[1].children[0].text == L"Alpha",
        "organizer hide reaches production display without hiding same label under different parent");
    check(invoke(ContextTreeActionKind::MoveUp, beta) == ContextTreeActionResult::Changed,
        "actual popup moves sibling above hidden child");
    check(invoke(ContextTreeActionKind::SetVisible, alpha) == ContextTreeActionResult::Changed,
        "hidden child remains reachable and can be restored");
    display = controller.BuildDisplay(prefs, {}, changed);
    check(display.size() == 2 && display.back().command == CmdSettingsContextMenu &&
        display[0].text == L"Application menu" && display[0].children.size() == 3 && display[0].children[0].text == L"Beta" &&
        display[0].children[0].command == CmdShellComBase + 102,
        "organizer order reaches production display and keeps actual command identity");
    check(invoke(ContextTreeActionKind::ResetGroup, root_key) == ContextTreeActionResult::Changed,
        "actual popup restores the current group's defaults");
    display = controller.BuildDisplay(prefs, {}, changed);
    check(display.size() == 2 && display.back().command == CmdSettingsContextMenu &&
        display[0].text == L"Application menu" && display[0].children.size() == 3 && display[0].children[0].text == L"Alpha",
        "group reset restores production ordering and visibility");
    check(invoke(ContextTreeActionKind::Open, nested) == ContextTreeActionResult::Navigated && parent == nested,
        "third-level organizer navigation follows the actual child tree");
    check(invoke(ContextTreeActionKind::Back, root_key) == ContextTreeActionResult::Navigated && parent == root_key,
        "actual popup returns to the parent group");
    ContextTreeAction stale{ContextTreeActionKind::SetVisible, L"n:removed", false};
    check(ApplySettingsContextTreeAction(prefs, stale, parent) == ContextTreeActionResult::Rejected,
        "stale settings action does not change another menu row");
    const auto language = l10n::preference();
    const auto original_scale = state.scale;
    for (const auto* locale : {L"zh-CN", L"en-US"}) {
        l10n::SetLanguage(locale);
        for (float scale : {1.0f, 2.0f}) for (bool dark : {false, true}) {
            state.compositor.RecreateTextFormats(scale);
            ui::FluentMenu screenshot;
            screenshot.Create(state.hwnd, &state.compositor, scale);
            screenshot.SetTheme(dark, state.accentColor);
            const auto page = BuildSettingsContextTreePage(prefs, root_key);
            const auto name = L"tree-" + std::wstring(locale) + L"-" + std::to_wstring(static_cast<int>(scale * 100)) +
                (dark ? L"-dark.png" : L"-light.png");
            check(screenshot.SaveDebugSnapshot((output / name).c_str(), page.items),
                "production submenu organizer snapshot saved");
        }
    }
    l10n::SetLanguage(l10n::LanguageId(language));
    state.compositor.RecreateTextFormats(original_scale);

    auto pump = [] {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) return false;
            TranslateMessage(&message); DispatchMessageW(&message);
        }
        return true;
    };
    auto& system = state.settings.SystemCtx();
    system.EnsureLoaded();
    auto wait_ready = [&] {
        const auto deadline = GetTickCount64() + 15000;
        while (system.View()->state != ui::SysCtxView::State::Ready && GetTickCount64() < deadline) {
            if (!pump()) return false;
            Sleep(5);
        }
        return system.View()->state == ui::SysCtxView::State::Ready;
    };
    check(wait_ready(), "actual settings registry scan completes on worker before other-type entry");
    auto show_type = [&](WPARAM key_press) {
        if (!ShowSystemContextExtension(state)) return false;
        struct DialogSearch { HWND owner; HWND dialog = nullptr; } search{state.hwnd};
        EnumThreadWindows(GetCurrentThreadId(), [](HWND window, LPARAM data) -> BOOL {
            auto& find = *reinterpret_cast<DialogSearch*>(data);
            wchar_t name[64]{}; GetClassNameW(window, name, ARRAYSIZE(name));
            if (GetWindow(window, GW_OWNER) == find.owner && wcscmp(name, L"PulseFormDialogWindow") == 0) {
                find.dialog = window; return FALSE;
            }
            return TRUE;
        }, reinterpret_cast<LPARAM>(&search));
        if (!search.dialog) return false;
        tree_test_dialog = search.dialog;
        const auto timer = SetTimer(nullptr, 0, 3000, TreeDialogWatchdog);
        PostMessageW(search.dialog, WM_KEYDOWN, key_press, 0);
        const bool pumped = pump();
        if (timer) KillTimer(nullptr, timer);
        tree_test_dialog = nullptr;
        return pumped && !IsWindow(search.dialog);
    };
    check(show_type(VK_RETURN) && wait_ready(), "actual other-type dialog submits default DWG through production callback and worker");
    const auto selected = system.View();
    check(selected->sub >= 0 && static_cast<size_t>(selected->sub) < selected->subs.size() &&
        selected->subs[static_cast<size_t>(selected->sub)] == L".dwg", "other-type dialog reaches DWG scene rather than a hardcoded common type");
    check(show_type(VK_ESCAPE) && system.View()->subs == selected->subs,
        "actual other-type dialog cancellation leaves selected file type unchanged");
}
} // namespace
