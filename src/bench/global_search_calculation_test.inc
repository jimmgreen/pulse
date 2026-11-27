// Real production Changed/async directory/keyboard/mouse/paint paths. Clipboard
// and final file launch are captured by test seams; no user files are opened.
int RunCalculationCoexistTests(const wchar_t* output_path) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    pulse::l10n::Initialize(GetModuleHandleW(nullptr), L"zh-CN");
    const auto output = std::filesystem::absolute(output_path);
    const auto files = output / L"files";
    std::filesystem::create_directories(files / L"123-45-folder");
    for (const auto* name : {L"123-45.txt", L"123-45_notes.txt", L"2+2.txt", L"2026-11-26.txt", L"ordinary.txt"}) {
        std::ofstream file(files / name); file << "isolated calculator/search fixture\n";
    }
    int failures = 0;
    const auto check = [&](bool ok, const char* label) {
        std::cout << (ok ? "[PASS] " : "[FAIL] ") << label << '\n';
        if (!ok) ++failures;
    };
    calculation_test_input = true;
    calculation_test_edit_diagnostics = GetEnvironmentVariableW(L"PULSE_SEARCH_CALC_EDIT_DIAGNOSTICS", nullptr, 0) != 0;
    const bool previous_frame_copy = pulse::ui::Compositor::FrameCopyEnabled();
    pulse::ui::Compositor::SetFrameCopyEnabled(true);
    {
        using State = pulse::GlobalSearchWindowTestPeer::State;
        State state;
        state.context.show_apps = false;
        state.animations = false;
        state.dark = true;
        state.effect = pulse::ui::WindowEffect::None;
        check(state.Create(), "create hidden production search window");
        if (!state.hwnd) {
            calculation_test_input = false;
            pulse::ui::Compositor::SetFrameCopyEnabled(previous_frame_copy);
            CoUninitialize(); return 1;
        }
        const auto resize = [&](float width, float height, float scale) {
            state.scale = scale;
            SetWindowPos(state.hwnd, nullptr, 0, 0, state.Px(width), state.Px(height),
                SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            state.Layout(); state.BuildLayout();
        };
        resize(800, 610, 1);
        const auto query = [&](const wchar_t* text, bool directory) {
            state.content_mode = false;
            state.browse_folder = directory ? files.wstring() : std::wstring{};
            SetWindowTextW(state.edit, text);
            state.Changed();
            // Global-provider acceptance is fed explicitly below; do not submit
            // a real all-disk query or depend on an installed index service.
            if (!directory) KillTimer(state.hwnd, pulse::kDebounce);
        };
        const auto wait = [&] {
            const auto until = GetTickCount64() + 5000;
            while (state.busy && GetTickCount64() < until) Pump(10);
            check(!state.busy, "async isolated directory search completes");
        };
        const auto capture = [&](const wchar_t* name) {
            state.appear_start = 0;
            // Capture the settled layout, rather than a previous copy toast.
            state.toast.clear(); state.toast_until = 0;
            SendMessageW(state.edit, EM_SETSEL, 0, 0);
            SendMessageW(state.edit, EM_SCROLLCARET, 0, 0);
            const auto serial = state.compositor.FrameCopySerial();
            std::cout << "FRAME ready=" << state.EnsureTarget() << " enabled=" << pulse::ui::Compositor::FrameCopyEnabled()
                << " live=" << (state.target.Get() == state.compositor.Dc()) << " recovery=" << state.compositor.NeedsRecovery() << '\n';
            state.Paint();
            auto presented = output / name;
            presented.replace_filename(presented.stem().wstring() + L"-presented.png");
            const bool readback = CapturePresentedFrame(state.compositor, presented.wstring(), true);
            std::cout << "FRAME previous=" << serial << " submitted=" << state.compositor.FrameCopySerial()
                << " readback=" << readback << " target=" << static_cast<bool>(state.target)
                << " recovery=" << state.compositor.NeedsRecovery() << '\n';
            check(state.compositor.FrameCopySerial() > serial && readback,
                "capture a newly submitted compositor frame without synthetic edit overlay");
            const bool captured = readback && Capture(state, (output / name).wstring(), presented.wstring());
            check(captured, "capture actual production rendering");
        };
        query(L"123-45", false);
        check(state.calculation && state.calculation->text == L"78", "arithmetic result remains available");
        check(state.query == L"123-45" && state.raw_query == L"123-45", "search preserves original expression text");
        check(state.busy && !state.showing_recent, "calculation does not bypass global search scheduling");
        capture(L"loading-dark.png");
        const auto obsolete = state.generation;
        query(L"2+2", false);
        pulse::index::SearchResult old;
        pulse::index::Hit old_hit; old_hit.path = (files / L"123-45.txt").wstring();
        old.hits.push_back(old_hit); old.total = 1;
        state.AcceptFilenames(false, obsolete, std::move(old));
        check(state.rows.empty(), "obsolete filename reply cannot replace newer calculation search");
        pulse::index::SearchResult current;
        pulse::index::Hit hit; hit.path = (files / L"2+2.txt").wstring();
        current.hits.push_back(hit); current.total = 1;
        state.AcceptFilenames(false, state.generation, std::move(current));
        check(state.calculation && state.calculation->text == L"4" && state.rows.size() == 1,
            "provider acceptance retains calculation and filename result together");

        query(L"123-45", true); wait();
        check(state.calculation && state.rows.size() == 3, "real directory enumeration returns numeric-hyphen files and folder beside calculation");
        check(state.MakeHandoff().query == L"123-45", "continue-search retains original expression instead of result value");
        capture(L"results-dark.png");
        const auto has_hot = [&](State::Hot kind) {
            return std::any_of(state.hot.begin(), state.hot.end(), [&](const auto& item) { return item.kind == kind; });
        };
        check(has_hot(State::HotCalculation) && has_hot(State::HotRow), "calculation and file rows both have pointer targets");
        bool separated = true;
        for (const auto& item : state.hot) {
            if (item.kind == State::HotCalculation || item.kind == State::HotAlternative)
                separated = separated && item.rect.bottom <= state.ListTop();
        }
        check(separated && state.ListBottom() - state.ListTop() >= state.RowHeight(),
            "calculator card leaves a nonoverlapping usable result viewport");
        state.Key(VK_DOWN);
        check(state.keyboard_nav && state.selected == 0, "first Down from calculator selects first file result");

        auto found = std::find_if(state.rows.begin(), state.rows.end(), [](const auto& row) { return !row.directory; });
        check(found != state.rows.end(), "matching file exists for interaction checks");
        if (found != state.rows.end()) {
            state.selected = static_cast<int>(std::distance(state.rows.begin(), found));
            const auto expected = found->path;
            state.keyboard_nav = true;
            calculation_test_clipboard.clear();
            state.CopyPath();
            check(calculation_test_clipboard == pulse::path::StripExtendedPathPrefix(expected),
                "file copy-path remains available with calculator card");
            std::vector<std::wstring> staged;
            state.events.stage_paths = [&](const std::vector<std::wstring>& paths) { staged = paths; };
            state.StageSelection();
            check(staged.size() == 1 && staged.front() == expected, "file staging remains available with calculator card");
            std::wstring opened;
            state.events.open_path = [&](const std::wstring& path, bool, bool) { opened = path; return true; };
            state.Key(VK_RETURN);
            check(opened == expected, "Enter on explicit file selection opens file instead of copying calculation");
            state.events.open_path = {};
            state.events.stage_paths = {};
        }
        query(L"123-45", true); wait();
        state.keyboard_nav = false;
        calculation_test_clipboard.clear();
        state.Key(VK_RETURN);
        check(calculation_test_clipboard == L"78", "Enter while editing retains calculation copy behavior");
        calculation_test_keys[VK_CONTROL] = static_cast<SHORT>(0x8000);
        state.Key(VK_RETURN);
        calculation_test_keys[VK_CONTROL] = 0;
        check(calculation_test_clipboard == L"123-45", "Ctrl Enter while editing copies the expression");
        capture(L"click-targets.png");
        auto card = std::find_if(state.hot.begin(), state.hot.end(), [](const auto& item) { return item.kind == State::HotCalculation; });
        if (card != state.hot.end()) {
            const auto rect = card->rect;
            calculation_test_clipboard.clear();
            state.Click((rect.left + rect.right) / 2, (rect.top + rect.bottom) / 2);
            check(calculation_test_clipboard == L"78", "click calculator card copies its result");
        } else check(false, "click calculator card is available");
        auto row = std::find_if(state.hot.begin(), state.hot.end(), [](const auto& item) { return item.kind == State::HotRow; });
        if (row != state.hot.end()) {
            const auto rect = row->rect; const int index = row->index;
            state.Click((rect.left + rect.right) / 2, (rect.top + rect.bottom) / 2);
            check(state.keyboard_nav && state.selected == index, "click file row selects file despite calculation");
            std::wstring opened;
            const auto expected = state.rows[static_cast<size_t>(index)].path;
            state.events.open_path = [&](const std::wstring& path, bool, bool) { opened = path; return true; };
            state.Message(WM_LBUTTONDBLCLK, 0,
                MAKELPARAM(state.Px((rect.left + rect.right) / 2), state.Px((rect.top + rect.bottom) / 2)));
            check(opened == expected, "double click routes selected result despite calculation");
            state.events.open_path = {};
        } else check(false, "click file row is available");

        resize(640, 420, 1.5f); state.dark = false;
        capture(L"results-light-narrow-150.png");
        check(has_hot(State::HotCalculation) && has_hot(State::HotRow) && state.ListBottom() > state.ListTop(),
            "narrow 150 percent layout keeps calculator and file results usable");
        state.grid_view = true; state.BuildLayout(); capture(L"results-grid-light-150.png");
        check(has_hot(State::HotCalculation) && has_hot(State::HotRow), "grid view renders files alongside calculation");
        state.grid_view = false;
        query(L"999-3", true); wait(); capture(L"empty-light.png");
        check(state.calculation && state.rows.empty() && !state.busy, "no filename matches retains calculator and empty search state");
        state.error = pulse::l10n::Get(pulse::l10n::StringId::GlobalSearchFailed);
        capture(L"error-light.png"); state.error.clear();
        query(L"1m to cm", true); wait(); capture(L"conversion-light.png");
        check(state.calculation && !state.calculation->alternatives.empty(), "unit-conversion alternatives remain available");
        wchar_t native_query[128]{};
        GetWindowTextW(state.edit, native_query, 128);
        check(state.raw_query == native_query, "native edit retains complete unit-conversion expression");
        if (state.calculation && !state.calculation->alternatives.empty()) {
            state.hover_kind = State::HotAlternative; state.hover_index = 0;
            check(state.TooltipText() == state.calculation->alternatives.front().text,
                "conversion tooltip preserves the complete unmodified result and unit");
            state.hover_kind = State::HotNone; state.hover_index = -1;
        }
        state.preview_open = true; state.selected = -1; state.BuildLayout();
        capture(L"conversion-preview-narrow-150.png");
        check(state.ListBottom() - state.ListTop() >= state.RowHeight(),
            "minimum-size preview layout preserves one complete file row");
        state.preview_open = false;
        query(L"2026-11-26", true); wait();
        check(state.calculation && state.calculation->text == L"1989" && state.rows.size() == 1,
            "date-shaped numeric filename remains searchable alongside arithmetic interpretation");
        query(L"ordinary", true); wait();
        check(!state.calculation && state.rows.size() == 1, "switch back to ordinary text removes card and searches normally");
        query(L"123-", true); wait();
        check(state.query == L"123-" && !state.rows.empty(), "unfinished numeric-hyphen text continues filename search");
        query(L">", false);
        check(state.command_mode && !state.calculation && !state.commands.empty(), "command mode remains independent");
        state.content_mode = true; state.browse_folder.clear();
        SetWindowTextW(state.edit, L"123-45"); state.Changed(); KillTimer(state.hwnd, pulse::kDebounce);
        check(state.content_mode && !state.calculation && state.query == L"123-45", "content search does not acquire calculator card");
        state.Cancel();
    }
    calculation_test_input = false;
    pulse::ui::Compositor::SetFrameCopyEnabled(previous_frame_copy);
    CoUninitialize();
    std::cout << "RESULT " << (failures ? "FAIL" : "PASS") << " calculator-coexist failures=" << failures << '\n';
    return failures ? 1 : 0;
}
