// Included by the native search-window fixture after its render helpers.
// 排序 / 分组 / facet chips on the real popup with injected results (no index
// service): frames for visual review plus checks of the shown order.
int RunOrderFlow(const std::filesystem::path& destination) {
    using State = pulse::GlobalSearchWindowTestPeer::State;
    using SetAwareness = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
    const auto set_awareness = reinterpret_cast<SetAwareness>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext"));
    if (set_awareness) set_awareness(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    pulse::l10n::Initialize(GetModuleHandleW(nullptr), L"zh-CN");
    const auto output = std::filesystem::absolute(destination);
    std::filesystem::create_directories(output / L"profile");
    SetEnvironmentVariableW(L"LOCALAPPDATA", (output / L"profile").c_str());
    int failures = 0;
    auto check = [&](bool ok, const char* label) { std::cout << (ok ? "[PASS] " : "[FAIL] ") << label << std::endl; failures += !ok; };

    pulse::GlobalSearchWindow window;
    pulse::GlobalSearchContext context;
    context.show_apps = true;
    int saved_sort = -1, saved_group = -1; bool saved_apps_first = false;
    window.SetContext(context);
    pulse::GlobalSearchEvents events;
    events.order_changed = [&](int sort, bool, int, int group, bool apps_first) { saved_sort = sort; saved_group = group; saved_apps_first = apps_first; };
    window.SetEvents(events);
    check(window.Show(nullptr, true, 1, L"D:\\工作"), "order fixture window opens");
    auto& state = pulse::GlobalSearchWindowTestPeer::Get(window);

    FILETIME now_ft{}; GetSystemTimeAsFileTime(&now_ft);
    const uint64_t now = (static_cast<uint64_t>(now_ft.dwHighDateTime) << 32) | now_ft.dwLowDateTime;
    constexpr uint64_t day = 864000000000ull, KB = 1024, MB = 1024 * 1024, GB = MB * 1024;
    using Cls = pulse::app::GlobalSearchRowClass;
    std::vector<State::Row> fixture;
    auto add = [&](const wchar_t* path, bool directory, uint64_t size, uint64_t age_hours, Cls cls, bool app = false) {
        State::Row row;
        row.path = path; row.name = row.path.substr(row.path.find_last_of(L'\\') + 1);
        row.directory = directory; row.size = size; row.mtime = age_hours == ~0ull ? 0 : now - age_hours * (day / 24);
        row.cls = cls; row.app = app;
        fixture.push_back(std::move(row));
    };
    add(L"D:\\项目\\2026\\年度报告", true, 0, 1, Cls::Jump);
    add(L"D:\\项目\\2026\\年度报告\\年度报告 2026.docx", false, 2470 * KB, 1, Cls::Best);
    add(L"C:\\Users\\Public\\Documents\\报告.pdf", false, 856 * KB, 18, Cls::Best);
    add(L"shell:AppsFolder\\Report.Viewer", false, 0, ~0ull, Cls::App, true);
    add(L"D:\\工作\\周报", true, 0, 50, Cls::Folder);
    add(L"D:\\项目\\2026\\年度报告\\报告素材", true, 0, 120, Cls::Folder);
    add(L"D:\\工作\\报告-会议纪要.txt", false, 4 * KB, 2, Cls::File);
    add(L"D:\\代码\\pulse-tools\\report_gen.py", false, 9 * KB, 3, Cls::File);
    add(L"D:\\图片\\Screenshots\\报告截图 2026-11-24.png", false, 640 * KB, 20, Cls::File);
    add(L"D:\\工作\\运营\\月度运营报告-11月.xlsx", false, 680 * KB, 26, Cls::File);
    add(L"D:\\工作\\周报\\周报-第47周.md", false, 14 * KB, 50, Cls::File);
    add(L"D:\\音乐\\录音\\报告录音-周会.m4a", false, 23 * MB, 46, Cls::File);
    add(L"D:\\项目\\2026\\年度报告\\报告素材\\报告封面.png", false, 2830 * KB, 120, Cls::File);
    add(L"D:\\项目\\2026\\年度报告\\报告素材\\报告配图-趋势.jpg", false, 1920 * KB, 150, Cls::File);
    add(L"D:\\项目\\2026\\年度报告\\报告素材-原始数据.zip", false, 1420 * MB, 320, Cls::File);
    add(L"D:\\工作\\产品\\用户调研报告.pptx", false, 12 * MB, 360, Cls::File);
    add(L"D:\\工作\\财务\\销售报告_10月.xlsx", false, 1100 * KB, 560, Cls::File);
    add(L"D:\\工作\\产品\\竞品分析报告.pdf", false, 2100 * KB, 700, Cls::File);
    add(L"D:\\视频\\2026\\发布会报告演示.mp4", false, 486 * MB, 1400, Cls::File);
    add(L"D:\\项目\\2025\\年度报告 2025-终稿.pdf", false, 5400 * KB, 7800, Cls::File);
    add(L"E:\\备份\\旧报告备份 2024.7z", false, 3600 * MB + GB / 10, 16000, Cls::File);

    // Results as a finished search for 报告 would leave them.
    auto load = [&](std::vector<State::Row> rows) {
        SetWindowTextW(state.edit, L"报告");
        state.Cancel();  // no index request: the fixture stands in for its results
        state.showing_recent = false; state.stale = false; state.busy = false; state.error.clear();
        state.merged_rows = std::move(rows);
        state.total = state.merged_rows.size(); state.truncated = false;
        state.Arrange(); state.UpdateFacets();
        state.selected = state.rows.empty() ? -1 : 0; state.keyboard_nav = true; state.scroll = state.scroll_target = 0;
        state.BuildLayout();
    };
    auto names = [&] { std::wstring out; for (const auto& row : state.rows) { out += row.name; out += L"|"; } return out; };
    auto capture = [&](const wchar_t* file) {
        state.compositor.CaptureNextFrame(); state.Render();
        check(CapturePresentedFrame(state, (output / file).wstring()), "capture order frame");
        state.compositor.ReleaseHeldFrame();
    };
    auto click = [&](State::Hot kind, int index) {
        state.Render();
        const auto hit = std::find_if(state.hot.begin(), state.hot.end(), [&](const auto& h) { return h.kind == kind && h.index == index; });
        if (hit == state.hot.end()) { check(false, "requested control is rendered and hit-testable"); return false; }
        const int x = state.Px((hit->rect.left + hit->rect.right) / 2), y = state.Px((hit->rect.top + hit->rect.bottom) / 2);
        SendMessageW(state.hwnd, WM_MOUSEMOVE, 0, MAKELPARAM(x, y));
        SendMessageW(state.hwnd, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(x, y));
        SendMessageW(state.hwnd, WM_LBUTTONUP, 0, MAKELPARAM(x, y));
        Pump(30);
        return true;
    };

    // 1. Default: match groups, counts on the type and size chips.
    load(fixture);
    check(state.groups.size() == 5 && state.groups[0].title == pulse::l10n::StringId::GlobalSearchJump &&
          state.groups[2].app, "default keeps 跳转 / 最佳匹配 / 应用 / 文件夹 / 文件");
    check(state.kind_counts_known && state.kind_counts[0] == fixture.size() && state.kind_counts[2] == 9,
          "type chip counts come from the complete results; 全部 matches the result count");
    state.Render();
    check(state.sort_button.right > state.sort_button.left && state.group_button.left > state.sort_button.right,
          "排序 and 分组 buttons sit in the result bar");
    capture(L"01-default-dark.png");

    // 2. Apps first (the popup's toggle and the setting share context.apps_first).
    state.context.apps_first = true; state.Rearrange();
    check(state.groups.front().app && state.rows.front().app, "应用排在最前 puts the apps group first");
    state.context.apps_first = false; state.Rearrange();

    // 3. Group by type, then fold the documents with a click on its header.
    state.group_by = pulse::app::GlobalSearchGroup::Type; state.collapsed.clear(); state.Rearrange();
    check(state.groups.size() >= 6 && state.groups[0].key == L"t:folder" && state.groups[0].foldable, "type groups start with folders");
    const auto documents = std::find_if(state.groups.begin(), state.groups.end(), [](const auto& g) { return g.key == L"t:document"; });
    const int doc_index = static_cast<int>(documents - state.groups.begin());
    click(State::HotGroupHeader, doc_index);
    check(state.Collapsed(L"t:document") && state.groups[static_cast<size_t>(doc_index)].count == 0 &&
          state.groups[static_cast<size_t>(doc_index)].total == 9, "header click folds the group, keeping its count");
    capture(L"02-type-folded-dark.png");
    click(State::HotGroupHeader, doc_index);
    check(!state.Collapsed(L"t:document"), "second click unfolds it");
    state.selected = 0;
    SendMessageW(state.edit, WM_KEYDOWN, VK_DOWN, 0);
    const int before = state.selected;
    // Ctrl+Down: next group's first row.
    BYTE keys[256]{}; GetKeyboardState(keys); keys[VK_CONTROL] = 0x80; SetKeyboardState(keys);
    SendMessageW(state.edit, WM_KEYDOWN, VK_DOWN, 0);
    keys[VK_CONTROL] = 0; SetKeyboardState(keys);
    check(state.selected > before && state.selected == static_cast<int>(state.groups[1].start), "Ctrl+Down jumps to the next group");

    // 4. Sort by date, newest first, grouped by date; select a whole group.
    state.sort_order = pulse::app::GlobalSearchSort::Modified; state.sort_desc = true;
    state.group_by = pulse::app::GlobalSearchGroup::Date; state.collapsed.clear(); state.Rearrange();
    check(state.groups.front().title == static_cast<pulse::l10n::StringId>(IDS_GROUP_D_FUTURE + 1), "date groups start with today");
    check(state.rows.front().name == L"年度报告" || state.rows.front().directory, "folders lead with folders on top");
    state.collapsed.push_back(state.groups.back().key); state.Rearrange();
    state.Render();
    const auto select_hot = [&](size_t g) {
        const float y = state.ListTop() - state.scroll + state.group_top[g] + 14;
        SendMessageW(state.hwnd, WM_MOUSEMOVE, 0, MAKELPARAM(state.Px(state.ListLeft() + 60), state.Px(y)));
        state.Render();
    };
    select_hot(1);
    check(std::any_of(state.hot.begin(), state.hot.end(), [](const auto& h) { return h.kind == State::HotGroupSelect && h.index == 1; }),
          "hovering a header shows 选择本组");
    capture(L"03-date-desc-dark.png");
    click(State::HotGroupSelect, 1);
    check(state.marked_paths.size() == state.groups[1].count, "选择本组 marks every row of the group");
    state.marked_paths.clear();

    // 5. Size order, light theme, thumbnails grouped by size.
    window.SetAppearance(false, pulse::ui::WindowEffect::None, L"", pulse::ui::GetAccentColor());
    state.sort_order = pulse::app::GlobalSearchSort::Size; state.sort_desc = true;
    state.group_by = pulse::app::GlobalSearchGroup::None; state.collapsed.clear(); state.Rearrange();
    check(state.groups.size() == 1 && !state.groups[0].header, "no grouping is one flat list");
    check(names().find(L"旧报告备份 2024.7z|报告素材-原始数据.zip|") != std::wstring::npos, "largest files first");
    state.selected = 5; state.ApplyOrder(pulse::app::GlobalSearchSort::Modified, true);
    state.Cancel();  // the re-search it starts is not part of the fixture
    check(state.selected == 0 && state.scroll_target == 0, "a new order starts at the top with the first row");
    capture(L"04-size-light.png");
    state.grid_view = true; state.group_by = pulse::app::GlobalSearchGroup::Size; state.Rearrange();
    check(state.grid_grouped, "thumbnail view keeps the groups");
    capture(L"05-grid-size-light.png");
    state.grid_view = false;

    // 6. Type chip -> extension chips (counts from the document results).
    window.SetAppearance(true, pulse::ui::WindowEffect::None, L"", pulse::ui::GetAccentColor());
    state.sort_order = pulse::app::GlobalSearchSort::Relevance; state.group_by = pulse::app::GlobalSearchGroup::Match;
    state.kind_filter = 2;
    std::vector<State::Row> docs;
    for (const auto& row : fixture) if (!row.app && state.KindIndex(row) == 2) docs.push_back(row);
    load(docs);
    check(state.ext_kind == 2 && state.ext_counts.size() >= 4 && state.ext_counts.front().first == L"pdf",
          "document results list their extensions, most first");
    check(state.FilterTokens().find(L"type:") != std::wstring::npos, "type chip still filters in the index query");
    state.ext_filter = L"pdf";
    check(state.FilterTokens().find(L" ext:pdf") != std::wstring::npos, "extension chip adds ext: to the index query");
    state.size_filter = 2;
    check(state.FilterTokens().find(L"size:>=1mb size:<128mb") != std::wstring::npos && state.FilterTokens().find(L"file:") != std::wstring::npos,
          "size chip adds a files-only size range");
    state.size_filter = 0; state.ext_filter.clear();
    capture(L"06-extension-chips-dark.png");

    // 7. Narrow: the sidebar hides, type chips move under the result bar.
    state.kind_filter = 0; load(fixture);
    SetWindowPos(state.hwnd, nullptr, 80, 80, 660, 560, SWP_NOZORDER | SWP_NOACTIVATE);
    Pump(30); state.BuildLayout();
    check(state.SideWidth() <= 0 && state.CategoryStrip(), "narrow window shows the category strip");
    capture(L"07-narrow-dark.png");

    // 8. Saving: the sort menu's choice reaches order_changed (menu answered by a timer).
    static State* order_state = nullptr;
    order_state = &state;
    SetTimer(state.hwnd, 0x5cb, 200, [](HWND hwnd, UINT, UINT_PTR id, DWORD) {
        KillTimer(hwnd, id);
        EnumThreadWindows(GetCurrentThreadId(), [](HWND candidate, LPARAM) -> BOOL {
            wchar_t name[64]{}; GetClassNameW(candidate, name, 64);
            if (IsWindowVisible(candidate) && std::wstring_view(name) == L"PulseFluentMenu") {
                PostMessageW(candidate, WM_KEYDOWN, VK_ESCAPE, 0); return FALSE;
            }
            return TRUE;
        }, 0);
        if (order_state->result_menu.IsOpen()) order_state->result_menu.Dismiss();
    });
    state.SortMenu();
    check(window.Visible() && GetFocus() == state.edit, "sort menu opens from the popup and Escape returns to the edit");
    order_state = nullptr;
    const auto old_sort = state.sort_order; const bool old_desc = state.sort_desc;
    state.sort_order = pulse::app::GlobalSearchSort::Type; state.sort_desc = false;
    state.ApplyOrder(old_sort, old_desc);
    check(saved_sort == static_cast<int>(pulse::app::GlobalSearchSort::Type) && saved_group == 0 && !saved_apps_first,
          "order changes are handed to the host to save");
    window.Shutdown();
    CoUninitialize();
    std::cout << "RESULT " << (failures ? "FAIL" : "PASS") << " order-flow\n";
    return failures ? 1 : 0;
}
