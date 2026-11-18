// Included by the native search-window fixture after its render helpers.
int RunMockupFlow(const std::filesystem::path& destination, bool interactive) {
    using State = pulse::GlobalSearchWindowTestPeer::State;
    using SetAwareness = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
    const auto set_awareness = reinterpret_cast<SetAwareness>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext"));
    if (set_awareness) set_awareness(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    pulse::l10n::Initialize(GetModuleHandleW(nullptr), L"zh-CN");
    const auto output = std::filesystem::absolute(destination);
    const auto files = output / L"files";
    std::filesystem::create_directories(files / L"Nested");
    std::filesystem::create_directories(output / L"profile");
    SetEnvironmentVariableW(L"LOCALAPPDATA", (output / L"profile").c_str());
    std::ofstream(files / L"alpha.txt") << "Pulse mockup fixture alpha content";
    std::ofstream(files / L"beta.txt") << "Pulse mockup fixture beta content";
    std::ofstream(files / L"Nested" / L"inside.txt") << "nested fixture";
    const auto image_source = std::filesystem::absolute(L"bench_data/search-mockup-20261103/reference.png");
    const bool has_image = std::filesystem::exists(image_source);
    if (has_image) std::filesystem::copy_file(image_source, files / L"photo.png", std::filesystem::copy_options::overwrite_existing);
    int failures = 0;
    auto check = [&](bool ok, const char* label) { std::cout << (ok ? "[PASS] " : "[FAIL] ") << label << std::endl; failures += !ok; };
    pulse::GlobalSearchWindow window;
    pulse::GlobalSearchContext context;
    context.show_apps = false;
    context.recent_files = {{files.wstring(), true}, {(files / L"alpha.txt").wstring(), false}, {(files / L"beta.txt").wstring(), false}};
    if (has_image) context.recent_files.push_back({(files / L"photo.png").wstring(), false});
    window.SetContext(context);
    std::vector<std::wstring> staged;
    pulse::GlobalSearchEvents events;
    events.stage_paths = [&](const auto& paths) { for (const auto& path : paths) if (std::find(staged.begin(), staged.end(), path) == staged.end()) staged.push_back(path); };
    events.staged_paths = [&] { return staged; };
    events.unstage_path = [&](const auto& path) { std::erase(staged, path); };
    events.clear_staging = [&] { staged.clear(); };
    window.SetEvents(events);
    check(window.Show(nullptr, false, 1, files.wstring()), "native mockup window opens");
    auto& state = pulse::GlobalSearchWindowTestPeer::Get(window);
    auto click = [&](State::Hot kind, int index) {
        state.Render();
        const auto hit = std::find_if(state.hot.begin(), state.hot.end(), [&](const auto& h) { return h.kind == kind && h.index == index; });
        if (hit == state.hot.end()) { check(false, "requested control is rendered and hit-testable"); return; }
        const int x = state.Px((hit->rect.left + hit->rect.right) / 2), y = state.Px((hit->rect.top + hit->rect.bottom) / 2);
        SendMessageW(state.hwnd, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(x, y));
        SendMessageW(state.hwnd, WM_LBUTTONUP, 0, MAKELPARAM(x, y));
        Pump(30);
    };
    click(State::HotView, 1);
    check(state.grid_view, "thumbnail segment changes actual result layout");
    bool slider_mapping_ok = true;
    for (float dpi : {1.0f, 1.5f, 2.0f}) {
        const D2D1_RECT_F bounds{0, 0, pulse::ui::kIconSizeSliderWidth * dpi, 28 * dpi};
        slider_mapping_ok &= pulse::ui::IconSizeSliderValueAt(bounds, -100, dpi, dpi) == pulse::ui::kMinIconDip;
        slider_mapping_ok &= pulse::ui::IconSizeSliderValueAt(bounds, bounds.right + 100, dpi, dpi) == pulse::ui::kMaxIconDip;
        for (float stop : pulse::ui::IconSizeSliderStops(dpi)) {
            const float x0 = pulse::ui::kIconSizeTrackLeft * dpi;
            const float x1 = bounds.right - pulse::ui::kIconSizeTrackRight * dpi;
            const float x = x0 + (x1 - x0) * pulse::ui::IconSizeSliderPosition(stop);
            slider_mapping_ok &= pulse::ui::IconSizeSliderValueAt(bounds, x, dpi, dpi) == stop;
        }
    }
    check(slider_mapping_ok, "shared main/search slider endpoints and preset snapping at 100/150/200 percent");
    state.Render();
    const auto slider = state.tile_size_slider;
    const int sy = state.Px((slider.top + slider.bottom) / 2);
    SendMessageW(state.hwnd, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(state.Px((slider.left + slider.right) / 2), sy));
    SendMessageW(state.hwnd, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(state.Px(slider.right + 20), sy));
    SendMessageW(state.hwnd, WM_LBUTTONUP, 0, MAKELPARAM(state.Px(slider.right + 20), sy));
    check(state.tile_width == pulse::ui::kMaxIconDip + 24 && GetCapture() != state.hwnd,
        "native shared slider drag changes thumbnail size and releases capture");
    state.tile_width = 170;
    click(State::HotView, 0);
    check(!state.grid_view, "list segment restores actual result layout");
    state.selected = 1; state.ToggleMarked();
    state.selected = 2; state.ToggleMarked();
    state.StageSelection();
    check(staged.size() == 2, "multi-selection stages both real paths");
    SetWindowTextW(state.edit, L"12.5*8+3");
    check(state.calculation.has_value() && state.calculation->text == L"103", "native edit evaluates arithmetic card");
    SetWindowTextW(state.edit, L"5kg to lb");
    check(state.calculation.has_value(), "native edit evaluates unit conversion card");
    SetWindowTextW(state.edit, L"上周的图片");
    check(!state.parsed_query.tokens.empty(), "natural language exposes removable filter token");
    click(State::HotToken, 0);
    check(state.parsed_query.tokens.empty(), "filter chip removes parsed natural-language filter");
    SetWindowTextW(state.edit, L">");
    check(state.command_mode && !state.commands.empty(), "native edit exposes executable commands");
    state.StageSelection();
    check(staged.size() == 2, "command results cannot stage empty paths");
    const auto toggle = std::find(state.commands.begin(), state.commands.end(), pulse::app::GlobalSearchCommand::ToggleView);
    if (toggle != state.commands.end()) {
        state.selected = static_cast<int>(toggle - state.commands.begin());
        SendMessageW(state.edit, WM_KEYDOWN, VK_RETURN, 0);
        check(state.grid_view && !state.command_mode, "Enter executes view command and restores results");
        click(State::HotView, 0);
    } else check(false, "view command is available");
    SetWindowTextW(state.edit, L"");
    check(staged.size() == 2, "staging survives a different query");
    state.selected = 0; state.keyboard_nav = true;
    SendMessageW(state.edit, WM_KEYDOWN, VK_RIGHT, 0);
    const auto deadline = GetTickCount64() + 5000;
    while (state.busy && GetTickCount64() < deadline) Pump(20);
    check(state.browse_folder == files.wstring() && !state.busy && state.rows.size() == (has_image ? 4u : 3u), "right arrow enumerates actual folder asynchronously");
    SendMessageW(state.edit, WM_KEYDOWN, VK_BACK, 0); Pump(30);
    check(state.browse_folder.empty(), "Backspace returns to global result context");
    state.browse_folder = files.wstring(); state.Changed();
    auto abandoned = state.directory_request;
    state.browse_folder.clear(); state.Changed(); Pump(80);
    check(abandoned && abandoned->cancelled && state.browse_folder.empty() && state.showing_recent,
        "leaving folder cancels and discards late enumeration");
    const auto csv = output / L"sample.csv";
    std::ofstream(csv) << "Name,Count\nAlpha,12\nBeta,34\n";
    State::Row csv_row;
    csv_row.path = csv.wstring(); csv_row.name = L"sample.csv";
    state.rows = {csv_row}; state.selected = 0; state.preview_open = true;
    state.BuildLayout(); state.SyncPreview();
    const auto preview_deadline = GetTickCount64() + 10000;
    const auto& preview_table = pulse::ui::DetailsPreviewProbe::Table(state.preview);
    do { state.Render(); Pump(30); } while (!preview_table.HasData() && GetTickCount64() < preview_deadline);
    check(preview_table.HasData() && preview_table.PlainText().find(L"Alpha") != std::wstring::npos,
        "real CSV preview reuses the preview host and shared table view");
    auto& preview_surface = pulse::ui::DetailsPreviewProbe::Surface(state.preview);
    preview_surface.CaptureNextFrame(); pulse::ui::DetailsPreviewProbe::Render(state.preview);
    check(CapturePresentedFrame(preview_surface, (output / L"csv-preview.png").wstring()), "capture shared CSV preview");
    preview_surface.ReleaseHeldFrame();
    state.preview_open = false; state.Changed(true); Pump(50);
    static State* menu_state = nullptr;
    static bool menu_owner_visible = false, menu_escape_sent = false;
    menu_state = &state; menu_owner_visible = menu_escape_sent = false;
    state.selected = 1;
    SetTimer(state.hwnd, 0x5ca, 180, [](HWND hwnd, UINT, UINT_PTR id, DWORD) {
        KillTimer(hwnd, id);
        menu_owner_visible = IsWindowVisible(hwnd) && menu_state->result_menu.IsOpen();
        EnumThreadWindows(GetCurrentThreadId(), [](HWND candidate, LPARAM) -> BOOL {
            wchar_t name[64]{}; GetClassNameW(candidate, name, 64);
            if (IsWindowVisible(candidate) && std::wstring_view(name) == L"PulseFluentMenu") {
                menu_escape_sent = true; PostMessageW(candidate, WM_KEYDOWN, VK_ESCAPE, 0); return FALSE;
            }
            return TRUE;
        }, 0);
        if (!menu_escape_sent) menu_state->result_menu.Dismiss();
    });
    POINT menu_point{state.Px(state.ListLeft() + 80), state.Px(state.ListTop() + 90)};
    ClientToScreen(state.hwnd, &menu_point);
    state.ResultMenu(menu_point);
    std::cout << "[DIAG] menu_owner_visible=" << menu_owner_visible << " escape_sent=" << menu_escape_sent
        << " search_visible=" << window.Visible() << " edit_focused=" << (GetFocus() == state.edit) << std::endl;
    check(menu_owner_visible && menu_escape_sent && window.Visible() && GetFocus() == state.edit,
        "shared Fluent menu keeps search visible and Escape restores its edit focus");
    menu_state = nullptr;
    for (const auto dark : {false, true}) {
        window.SetAppearance(dark, pulse::ui::WindowEffect::None, L"", pulse::ui::GetAccentColor());
        state.grid_view = dark; state.BuildLayout();
        state.compositor.CaptureNextFrame(); state.Render();
        check(CapturePresentedFrame(state, (output / (dark ? L"dark-grid.png" : L"light-list.png")).wstring()), "capture native result layout");
        state.compositor.ReleaseHeldFrame();
    }
    SetWindowPos(state.hwnd, nullptr, 80, 80, 660, 520, SWP_NOZORDER);
    state.compositor.CaptureNextFrame(); state.Render();
    check(CapturePresentedFrame(state, (output / L"narrow.png").wstring()), "capture narrow native layout");
    state.compositor.ReleaseHeldFrame();
    if (interactive) {
        // Expose the standalone tool window to desktop automation inventory.
        const auto ex_style = GetWindowLongPtrW(state.hwnd, GWL_EXSTYLE);
        SetWindowLongPtrW(state.hwnd, GWL_EXSTYLE, (ex_style & ~WS_EX_TOOLWINDOW) | WS_EX_APPWINDOW);
        // The desktop test runner changes foreground during observations.
        // Activation/cancellation is covered separately with the real handler.
        SetWindowSubclass(state.hwnd, [](HWND hwnd, UINT message, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) -> LRESULT {
            if (message == WM_ACTIVATEAPP && !wp) return 0;
            return DefSubclassProc(hwnd, message, wp, lp);
        }, 7, 0);
        check(window.Show(nullptr, false, 1, files.wstring()), "interactive window activates after automated checks");
        SetWindowPos(state.hwnd, nullptr, 80, 80, 1440, 960, SWP_NOZORDER);
        window.SetAppearance(false, pulse::ui::WindowEffect::None, L"", pulse::ui::GetAccentColor());
        state.grid_view = false; state.BuildLayout(); state.Render();
        std::cout << "[READY] interactive fixture" << std::endl;
        const auto until = GetTickCount64() + 300000;
        while (window.Visible() && GetTickCount64() < until) Pump(30);
    }
    window.Shutdown();
    CoUninitialize();
    return failures ? 1 : 0;
}
