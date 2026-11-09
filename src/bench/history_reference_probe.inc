#include "../app/app_ops_ui.h"
#include <fstream>
#include "../app/keymap_input.h"

int RunHistoryReferenceProbe(pulse::AppState& s) {
    using namespace pulse;
    int failures = 0;
    const auto check = [&](bool ok, const char* label) {
        std::cout << (ok ? "[PASS] " : "[FAIL] ") << label << std::endl;
        failures += !ok;
    };
    const auto output = std::filesystem::absolute(L"../bench_data/history-reference-ui").lexically_normal();
    std::filesystem::create_directories(output);
    const auto root = output / (L"fixture-" + std::to_wstring(GetCurrentProcessId()));
    std::filesystem::create_directories(root / L"from");
    std::filesystem::create_directories(root / L"to");
    std::ofstream(root / L"from" / L"first.txt") << "history first";
    std::ofstream(root / L"from" / L"second.txt") << "history second";
    s.appPrefs.status_hints_learned = false;
    s.appPrefs.show_hints = true;
    auto* intro_tab = ActiveTab(s);
    intro_tab->current_path = (root / L"from").wstring();
    intro_tab->loading = false;
    intro_tab->SetSnapshot(std::make_shared<std::vector<fs::DirEntry>>());
    s.hoverRegion = ui::HitTestResult::None;
    const auto initial = BuildVm(s, false);
    check(initial.status.hint_text == l10n::Get(l10n::StringId::StatusIntroHint), "new user sees HTML onboarding hint");
    s.compositor.Resize(1100, 720); s.renderer.SetScale(1); s.compositor.RecreateTextFormats(1);
    bool initial_drawn = true;
    for (int frame = 0; frame < 32; ++frame) {
        s.compositor.Dc()->BeginDraw();
        s.renderer.Render(initial, D2D1::RectF(0,0,1100,720), ui::MakeTheme(false,ui::HexColor(0x6474C7)));
        initial_drawn = SUCCEEDED(s.compositor.Dc()->EndDraw()) && initial_drawn;
        Sleep(16);
    }
    check(initial_drawn && s.compositor.SaveSnapshot((output/L"status-new-user.png").c_str()),
        "new-user onboarding status screenshot saved");
    s.appPrefs.status_hints_learned = true;
    const auto experienced = BuildVm(s, false);
    check(experienced.status.hint_text.empty() && experienced.status.hint_action_text.empty(),
        "experienced user has no onboarding hint even without in-memory history");
    s.appPrefs.status_hints_learned = false;
    s.ops.Start([] {});
    auto wait = [&](const auto& predicate) {
        const auto deadline = GetTickCount64() + 10000;
        while (!predicate() && GetTickCount64() < deadline) Sleep(5);
        return predicate();
    };
    auto copy = [&](const wchar_t* name) {
        ops::OpRequest request; request.type = ops::OpType::Copy;
        request.sources = {(root / L"from" / name).wstring()}; request.dest_dir = (root / L"to").wstring();
        const auto id = s.ops.Submit(std::move(request));
        check(wait([&] { return s.ops.FinalStatus(id).has_value(); }), "isolated copy completes through actual operations worker");
        return id;
    };
    const auto first = copy(L"first.txt"), second = copy(L"second.txt");
    auto* tab = ActiveTab(s); tab->current_path = (root / L"to").wstring();
    auto snapshot = std::make_shared<std::vector<fs::DirEntry>>();
    for (const auto* name : {L"first.txt", L"second.txt"}) {
        fs::DirEntry entry; entry.name = name;
        entry.full_path = (root / L"to" / name).wstring(); entry.attrs = FILE_ATTRIBUTE_NORMAL;
        snapshot->push_back(std::move(entry));
    }
    tab->SetSnapshot(snapshot); tab->loading = false; tab->view_mode = ui::ViewMode::Details;
    s.undoHistoryOpen = false;
    auto actual = BuildVm(s, false);
    const auto click_bounds = D2D1::RectF(0, 0, static_cast<float>(s.compositor.Width()), static_cast<float>(s.compositor.Height()));
    const auto chip = ui::MakeStatusBarMetrics(actual, click_bounds, 1.0f, 28.0f,
        s.compositor.DwriteFactory(), s.compositor.SmallFormat());
    check(chip.undo_chip.right - chip.undo_chip.left - 66 >=
        ui::MeasureTextWidth(s.compositor.DwriteFactory(), s.compositor.SmallFormat(), actual.status.undo_label),
        "short operation label fits beside actual chevron without ellipsis");
    HandleLButtonDown(&s, s.hwnd, WM_LBUTTONDOWN, 0,
        MAKELPARAM(static_cast<int>(chip.undo_button.right + 10), static_cast<int>(chip.undo_chip.top + 8)));
    check(s.undoHistoryOpen, "capsule label click opens actual history state");
    HandleMouseWheel(&s, s.hwnd, WM_MOUSEWHEEL, MAKEWPARAM(0, static_cast<WORD>(-WHEEL_DELTA)), 0);
    check(s.undoHistoryOffset == 1, "history wheel moves to older rows without scrolling files");
    s.undoHistoryOffset = 0;
    actual = BuildVm(s, false);
    check(actual.status.can_undo && !actual.status.can_redo && actual.status.history.size() == 2 &&
        actual.status.history.front().task_id == second, "real completed operations populate capsule and newest-first history");
    check(actual.status.history.front().detail.find(root.wstring()) == std::wstring::npos &&
        actual.status.history.front().detail == L"from → to",
        "history metadata uses short source and destination folder names");
    check(actual.status.hint_text.empty() && actual.status.hint_action_text.empty(),
        "completed operations retire right-side onboarding hints");
    check(actual.status.selection_text.empty(), "no selection omits status selection text");
    s.appPrefs.persist = true;
    UpdateOperationWindow(s, false);
    check(s.appPrefs.status_hints_learned, "operation update marks status onboarding learned");
    app::AppPrefs reloaded;
    check(reloaded.Load() && reloaded.status_hints_learned, "retired status onboarding persists in isolated profile");
    s.appPrefs.persist = false;
    s.hoverRegion = ui::HitTestResult::UndoHistoryRow; s.hoverControlIndex = 0;
    actual = BuildVm(s, false);
    check(actual.pane.history_preview_paths.size() == 1 &&
        actual.pane.history_preview_paths.front() == (root / L"to" / L"second.txt").wstring(),
        "hover previews actual operation destination without changing selection");
    s.compositor.Resize(1100, 720); s.renderer.SetScale(1.0f); s.compositor.RecreateTextFormats(1.0f);
    const auto actual_bounds = D2D1::RectF(0, 0, 1100, 720);
    const auto actual_theme = ui::MakeTheme(false, ui::HexColor(0x6474C7));
    s.compositor.Dc()->BeginDraw(); s.renderer.Render(actual, actual_bounds, actual_theme);
    const bool actual_drawn = SUCCEEDED(s.compositor.Dc()->EndDraw());
    check(actual_drawn && s.compositor.SaveSnapshot((output / L"history-real-hover.png").c_str()),
        "actual app history hover and real file list screenshot saved");
    s.undoHistoryOpen = false;
    for (bool selected : {false, true}) {
        if (selected) tab->SelectOnly(0); else tab->ClearSelection();
        auto state = BuildVm(s, false);
        check(selected ? state.status.selection_text == L"已选中 1 项" : state.status.selection_text.empty(),
            "HTML selection count appears only for selected items");
        state.hover_region = ui::HitTestResult::None;
        bool drawn = true;
        for (int frame = 0; frame < 32; ++frame) {
            s.compositor.Dc()->BeginDraw(); s.renderer.Render(state, actual_bounds, actual_theme);
            drawn = SUCCEEDED(s.compositor.Dc()->EndDraw()) && drawn;
            Sleep(16);
        }
        check(drawn && s.compositor.SaveSnapshot((output / (selected ? L"status-selected.png" : L"status-unselected.png")).c_str()),
            "actual status bar selected and unselected screenshots saved");
    }
    tab->ClearSelection();
    s.undoHistoryOpen = true;
    HandleUndoHistoryAction(s, first, false, false);
    check(wait([&] { return !std::filesystem::exists(root / L"to" / L"first.txt") &&
        !std::filesystem::exists(root / L"to" / L"second.txt") && !s.ops.UndoHistory().busy && s.ops.CanRedo(); }),
        "history undo-to runs both actual filesystem steps");
    HandleUndoHistoryAction(s, first, false, false);
    check(!std::filesystem::exists(root / L"to" / L"first.txt") && s.ops.CanRedo(),
        "stale displayed undo direction cannot mutate the newly undone history");
    actual = BuildVm(s, false);
    check(actual.status.can_redo && actual.status.history.size() == 2 && actual.status.history.front().undone,
        "successful undo exposes redo capsule and undone rows");
    check(app::RouteKey(app::Keymap{}, app::KeyChord{app::kKeyCtrl | app::kKeyShift, 'Z'}, 0).kind == app::KeyRoute::Pass,
        "default keymap preserves Ctrl+Shift+Z instead of mapping it to Undo");
    {
        app::ModifierOverride modifiers(app::kKeyCtrl | app::kKeyShift);
        check((GetKeyState(VK_CONTROL) & 0x8000) && (GetKeyState(VK_SHIFT) & 0x8000) &&
            !(GetKeyState(VK_MENU) & 0x8000), "keyboard override reaches the actual GetKeyState input path");
        HandleKeyDown(&s, s.hwnd, WM_KEYDOWN, 'Z', 0);
    }
    check(wait([&] { return std::filesystem::exists(root / L"to" / L"first.txt") && !s.ops.UndoHistory().busy; }),
        "Ctrl+Shift+Z redoes the next actual file operation");
    HandleUndoHistoryAction(s, second, true, false);
    check(wait([&] { return std::filesystem::exists(root / L"to" / L"first.txt") &&
        std::filesystem::exists(root / L"to" / L"second.txt") && !s.ops.UndoHistory().busy && !s.ops.CanRedo(); }),
        "history redo-to restores both actual filesystem steps");
    tab->current_path = (root / L"from").wstring();
    HandleUndoHistoryAction(s, second, false, true);
    std::wcout << L"[INFO] locate open=" << s.undoHistoryOpen << L" expected_only=" << s.arrivalSelect.expected_only
        << L" expected=" << s.arrivalSelect.expected.size() << L" path=" << ActiveTab(s)->current_path << std::endl;
    check(!s.undoHistoryOpen && s.arrivalSelect.expected_only && !s.arrivalSelect.expected.empty(),
        "history locate navigates and arms only expected result paths");
    s.undoHistoryOpen = true;
    HandleKeyDown(&s, s.hwnd, WM_KEYDOWN, VK_ESCAPE, 0);
    check(!s.undoHistoryOpen, "Escape closes actual app history state");
    s.ops.Stop();
    std::error_code cleanup_error; std::filesystem::remove_all(root, cleanup_error);
    check(!cleanup_error, "isolated real-operation fixture cleaned");
    (void)first;
    for (float scale : {1.0f, 1.5f}) for (bool dark : {false, true}) for (int width_dip : {640, 1100}) {
        const int width = static_cast<int>(width_dip * scale), height = static_cast<int>(720 * scale);
        s.scale = scale; s.compositor.Resize(width, height);
        s.compositor.RecreateTextFormats(scale); s.renderer.SetScale(scale);
        auto vm = app::BuildWindowViewModel(*s.pane, s.sidebar, true, false, dark, &s.places);
        vm.status.history_open = vm.status.can_undo = vm.status.can_redo = true;
        vm.status.undo_label = L"撤销 移动 2 个项目";
        vm.status.history = {
            {13, L"复制 3 个项目", L"源文件夹 → 目标文件夹", L"14:32", true, true, true},
            {12, L"移动 2 个项目", L"桌面 → 文档", L"14:28", false, true, true},
            {11, L"永久删除 1 个项目", L"Fixture · old.tmp", L"14:15", false, false, false},
            {10, L"重命名 1 个项目", L"Fixture · 季度计划.txt", L"13:58", false, true, false}
        };
        vm.status.history[0].icon_glyph=L"\xE8C8";
        vm.status.history[1].icon_glyph=L"\xE7C2";
        vm.status.history[3].icon_glyph=L"\xE8AC";
        vm.hover_region = ui::HitTestResult::UndoHistoryRow;
        vm.hover_control_index = 3;
        const auto bounds = D2D1::RectF(0, 0, static_cast<float>(width), static_cast<float>(height));
        const auto card = ui::UndoHistoryBounds(vm, bounds, scale);
        const auto metrics = ui::MakeStatusBarMetrics(vm, bounds, scale, 28 * scale,
            s.compositor.DwriteFactory(), s.compositor.SmallFormat());
        check(card.left >= 0 && card.right <= width && card.top >= 0 && card.bottom < metrics.bar.top,
            "history card stays above status bar at 100/150 percent DPI");
        check(card.right - card.left <= 360 * scale + 0.1f,
            "history card uses compact 360 DIP maximum width");
        const auto idle_hit = s.renderer.HitTest(vm, bounds, card.right - 30 * scale, ui::UndoHistoryRowTop(vm, card, 0, scale) + 22 * scale);
        check(idle_hit.region == ui::HitTestResult::UndoHistoryRow && idle_hit.sub_index == 2,
            "unhovered history row has no hidden active buttons");
        vm.hover_control_index = 0;
        const auto hit = s.renderer.HitTest(vm, bounds, card.right - 30 * scale, ui::UndoHistoryRowTop(vm, card, 0, scale) + 22 * scale);
        check(hit.region == ui::HitTestResult::UndoHistoryRow && hit.index == 0 && hit.sub_index == 0,
            "redo row action hits the displayed history index");
        vm.hover_control_index = 1;
        const auto locate = s.renderer.HitTest(vm, bounds, card.right - 130 * scale, ui::UndoHistoryRowTop(vm, card, 1, scale) + 22 * scale);
        check(locate.region == ui::HitTestResult::UndoHistoryRow && locate.index == 1 && locate.sub_index == 1,
            "locate has a separate hit target from undo-to");
        check(ui::StatusBarHitRegion(vm, bounds, metrics.redo_button.left + scale, metrics.redo_button.top + scale,
            scale, 28 * scale, &s.compositor) == ui::HitTestResult::StatusRedo, "conditional redo button hit target");
        const auto metadata_hit = s.renderer.HitTest(vm, bounds, card.right - 130 * scale, ui::UndoHistoryRowTop(vm, card, 1, scale) + 40 * scale);
        check(metadata_hit.sub_index == 2, "metadata below visible links does not trigger actions");
        const auto theme = ui::MakeTheme(dark, ui::HexColor(0x6474C7));
        s.compositor.Dc()->BeginDraw(); s.renderer.Render(vm, bounds, theme);
        const bool drawn = SUCCEEDED(s.compositor.Dc()->EndDraw());
        const auto file = output / (std::wstring(L"history-") + (dark ? L"dark-" : L"light-") + (scale == 1 ? L"100-" : L"150-") + std::to_wstring(width_dip) + L".png");
        check(drawn && s.compositor.SaveSnapshot(file.c_str()), "real renderer history screenshot saved");
        vm.status.history.clear(); vm.status.can_undo = vm.status.can_redo = false;
        vm.status.undo_label = l10n::Get(l10n::StringId::OpHistory);
        s.compositor.Dc()->BeginDraw(); s.renderer.Render(vm, bounds, theme);
        check(SUCCEEDED(s.compositor.Dc()->EndDraw()), "empty history renders without action rows");
    }
    for (int width : {360, 640, 1100}) for (bool query : {false, true}) {
        ui::WindowViewModel vm;
        vm.status.query_active = query; vm.status.task_is_update = !query;
        vm.status.query_text = vm.status.task_text = L"正在处理文件并更新结果";
        const auto bounds = D2D1::RectF(0, 0, static_cast<float>(width), 640);
        const auto metrics = ui::MakeStatusBarMetrics(vm, bounds, 1, 28,
            s.compositor.DwriteFactory(), s.compositor.SmallFormat());
        check(metrics.undo_chip.right <= metrics.undo_chip.left || metrics.task.left >= metrics.undo_chip.right,
            "search/update progress does not overlap undo capsule at 360/640/1100 widths");
        check(metrics.task.right <= width && metrics.task.left >= 0,
            "search/update progress stays inside narrow window");
    }
    for (float scale : {1.0f, 1.5f}) for (int width : {360, 640, 1100}) for (bool selected : {false, true}) {
        s.compositor.RecreateTextFormats(scale);
        ui::WindowViewModel vm; vm.status.ai_state = ui::StatusAiState::Idle;
        vm.status.status_text = L"24 个项目"; vm.status.selection_text = selected ? L"已选中 2 项" : L"";
        vm.status.undo_label = L"撤销 移动 2 项"; vm.status.can_redo = true;
        vm.status.hint_text = L"空格 快速预览";
        const auto bounds = D2D1::RectF(0, 0, width * scale, 720 * scale);
        const auto m = ui::MakeStatusBarMetrics(vm, bounds, scale, 28 * scale,
            s.compositor.DwriteFactory(), s.compositor.SmallFormat());
        check(m.status_label.left >= m.ai_chip.right &&
            (!selected || m.selection_label.left >= m.status_label.right) &&
            (m.undo_chip.right <= m.undo_chip.left || m.undo_chip.left >= (selected ? m.selection_label.right : m.status_label.right)),
            "HTML status order is AI, count, optional selection, history");
        check(m.undo_chip.right > m.undo_chip.left && std::abs(m.undo_chip.bottom-m.undo_chip.top-22*scale)<0.1f,
            "narrow status retains a 22 DIP history capsule");
        check(m.status_label.right-m.status_label.left >= ui::MeasureTextWidth(s.compositor.DwriteFactory(),s.compositor.SmallFormat(),vm.status.status_text)-0.1f,
            "history capsule never truncates item count");
        vm.status.task_text = L"正在复制文件"; vm.status.task_active = true;
        const auto task = ui::MakeStatusBarMetrics(vm,bounds,scale,28*scale,s.compositor.DwriteFactory(),s.compositor.SmallFormat());
        check(task.undo_chip.right <= task.undo_chip.left || task.task.left >= task.undo_chip.right,
            "task pill follows history without overlap");
    }
    return failures ? 1 : 0;
}
