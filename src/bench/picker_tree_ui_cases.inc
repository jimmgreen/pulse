void TreeCases(PulsePickerUiV2Api& api, PulsePickerUiV2Session session,
    const std::filesystem::path& root, const std::filesystem::path& output) {
    const auto fixture = root / (L"tree-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    const auto left = fixture / L"alpha", right = fixture / L"bravo", empty = fixture / L"empty";
    const auto nested = left / L"nested", gone = fixture / L"gone";
    for (const auto& directory : {nested, right, empty, gone}) std::filesystem::create_directories(directory);
    for (const auto& file : {left / L"same.txt", left / L"same-v2.txt", right / L"same.txt", left / L"image.png", left / L"hidden.txt", left / L"protected.txt", fixture / L"root.txt", nested / L"deep.txt"})
        std::ofstream(file, std::ios::binary) << "isolated real picker tree sample\n";
    const auto hidden = (left / L"hidden.txt").wstring();
    const bool hidden_set = SetFileAttributesW(hidden.c_str(), FILE_ATTRIBUTE_HIDDEN) != FALSE;
    Check(hidden_set, "tree fixture marks its isolated child hidden on the real filesystem");
    if (!hidden_set) return;
    const auto protected_path = (left / L"protected.txt").wstring();
    const bool protected_set = SetFileAttributesW(protected_path.c_str(), FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM) != FALSE;
    Check(protected_set, "tree fixture marks its isolated protected child hidden and system");
    if (!protected_set) return;
    const auto folder = fixture.wstring(), left_path = left.wstring(), right_path = right.wstring();
    const auto a = (left / L"same.txt").wstring(), b = (right / L"same.txt").wstring();
    const auto png = (left / L"image.png").wstring(), empty_path = empty.wstring();
    const auto nested_path = nested.wstring(), deep = (nested / L"deep.txt").wstring();
    const std::wstring text_label = L"Text", all_label = L"All files", text_pattern = L"*.txt", all_pattern = L"*.*";
    PulsePickerUiV2Filter filters[] = {
        {sizeof(PulsePickerUiV2Filter), 0, Text(text_label), Text(text_pattern)},
        {sizeof(PulsePickerUiV2Filter), 0, Text(all_label), Text(all_pattern)}
    };
    const auto row = [](const pulse::ui::PickerJumpUiTest& q, const std::wstring& path) {
        for (unsigned i = 0; i < q.tree_row_count && i < 64; ++i)
            if (q.tree_kinds[i] == 0 && path == q.tree_paths[i]) return static_cast<int>(i);
        return -1;
    };
    const auto query = [](HWND window, pulse::ui::PickerInteractionTest& q) {
        q = {};
        return Send(window, pulse::ui::kPickerInteractionTestMessage, 0, reinterpret_cast<LPARAM>(&q));
    };
    const auto select = [](HWND window, HWND sender, const std::wstring& path, bool add = false) {
        std::vector<pulse::ui::PickerJumpUiTest> storage(1);
        auto& q = storage.front(); q.tree_select_path = path.c_str(); q.tree_select_add = add;
        return Send(window, pulse::ui::kPickerJumpUiTestMessage, reinterpret_cast<WPARAM>(sender), reinterpret_cast<LPARAM>(&q));
    };
    const auto toggle = [](HWND window, HWND sender, const std::wstring& path) {
        std::vector<pulse::ui::PickerJumpUiTest> storage(1);
        auto& q = storage.front(); q.tree_toggle_path = path.c_str();
        return Send(window, pulse::ui::kPickerJumpUiTestMessage, reinterpret_cast<WPARAM>(sender), reinterpret_cast<LPARAM>(&q));
    };
    const auto key = [](HWND window, UINT code) {
        pulse::ui::PickerInteractionTest q; q.key = code;
        return Send(window, pulse::ui::kPickerInteractionTestMessage, 0, reinterpret_cast<LPARAM>(&q));
    };
    const auto unpack = [](const PulsePickerUiV2Buffer& buffer) {
        std::vector<std::wstring> paths;
        size_t offset = 0;
        while (offset < buffer.units && buffer.data[offset]) {
            size_t end = offset;
            while (end < buffer.units && buffer.data[end]) ++end;
            if (end == buffer.units) return std::vector<std::wstring>{};
            paths.emplace_back(buffer.data + offset, end - offset);
            offset = end + 1;
        }
        std::sort(paths.begin(), paths.end());
        return paths;
    };
    for (unsigned variant = 0; variant < 6; ++variant) {
        const bool multi = variant == 1, collapse = variant == 2, filtered = variant == 3;
        const bool empty_case = variant == 4, save = variant == 5;
        PulsePickerUiV2Config config{};
        config.size = sizeof(config); config.version = 2; config.initial_folder = Text(folder);
        config.access_policy = save ? PULSE_PICKER_UI_V2_ACCESS_LOCAL_SAVE_V1 : PULSE_PICKER_UI_V2_ACCESS_LOCAL_OPEN_V1;
        if (save) config.save_options = PULSE_PICKER_UI_V2_SAVE_NO_OVERWRITE_PROMPT;
        config.open_options = multi || collapse ? PULSE_PICKER_UI_V2_OPEN_ALLOW_MULTI_SELECT : 0;
        config.appearance = multi ? PULSE_PICKER_UI_V2_DARK : 0;
        if (filtered) { config.filters = filters; config.filter_count = 2; }
        const auto drive = [&](HWND window, HWND sender, State& state) {
            // Construct/reset the large path snapshot in reserved heap storage, without
            // an aggregate temporary on the default-size UI driver thread stack.
            std::vector<pulse::ui::PickerJumpUiTest> storage(1);
            auto& q = storage.front();
            const auto reset = [&] { storage.clear(); storage.emplace_back(); };
            const auto geometry = [&] {
                reset();
                return Send(window, pulse::ui::kPickerJumpUiTestMessage, reinterpret_cast<WPARAM>(sender),
                    reinterpret_cast<LPARAM>(&q)) && q.ready;
            };
            const auto ready = [&] { return geometry() && q.tree_ready; };
            if (!Wait(geometry, state.done)) return false;
            // Isolate each dialog from asynchronous selection changes in the user's main window.
            reset(); q.set_pulse_fixture = true; q.pulse_count = 0;
            if (!Send(window, pulse::ui::kPickerJumpUiTestMessage, reinterpret_cast<WPARAM>(sender), reinterpret_cast<LPARAM>(&q)) ||
                !Wait(geometry, state.done)) return false;
            reset(); q.display_prefs_json = LR"({"show_hidden_files":false,"show_protected_os_files":false,"show_file_extensions":true,"folder_sort_mode":0})";
            if (!Send(window, pulse::ui::kPickerJumpUiTestMessage, reinterpret_cast<WPARAM>(sender), reinterpret_cast<LPARAM>(&q))) return false;
            reset(); q.display_command = 25;
            if (!Send(window, pulse::ui::kPickerJumpUiTestMessage, reinterpret_cast<WPARAM>(sender), reinterpret_cast<LPARAM>(&q))) return false;
            for (const int command : {10, 14, 30, 44}) {
                reset(); q.display_command = command;
                if (!Send(window, pulse::ui::kPickerJumpUiTestMessage, reinterpret_cast<WPARAM>(sender), reinterpret_cast<LPARAM>(&q)) ||
                    !Wait(ready, state.done)) return false;
            }
            // Reset width for every Show: a preceding narrow dialog may seed the
            // next dialog's remembered geometry, including compact filter layout.
            reset(); q.client_width_dip = multi ? 640.0f : 1000.0f;
            if (!Send(window, pulse::ui::kPickerJumpUiTestMessage, reinterpret_cast<WPARAM>(sender), reinterpret_cast<LPARAM>(&q)) ||
                !Wait(ready, state.done)) return false;
            const unsigned navigation_events = state.folder_changed.load();
            const auto unchanged = [&] {
                pulse::ui::PickerInteractionTest current;
                return query(window, current) && current.current == folder && state.folder_changed == navigation_events;
            };
            const auto expand = [&](const std::wstring& path, const std::wstring& child) {
                if (!Wait(ready, state.done)) return false;
                const int index = row(q, path);
                if (index < 0 || (!q.tree_expanded[index] && !toggle(window, sender, path))) return false;
                return Wait([&] { return ready() && row(q, child) >= 0; }, state.done);
            };
            if (empty_case) {
                const auto gone_path = gone.wstring();
                const bool listed = row(q, gone_path) >= 0;
                std::error_code remove_error;
                const bool removed = listed && std::filesystem::remove(gone, remove_error);
                Check(removed && !remove_error, "remove only the isolated empty gone directory after its parent listing is ready");
                if (!removed || remove_error || !toggle(window, sender, gone_path)) return false;
                int failed_row = -1;
                const bool failed = Wait([&] {
                    if (!ready()) return false;
                    const int parent = row(q, gone_path);
                    failed_row = parent + 1;
                    return parent >= 0 && failed_row < static_cast<int>(q.tree_row_count) &&
                        q.tree_kinds[failed_row] == 3 && q.tree_depths[failed_row] == 1;
                }, state.done);
                Check(failed, "expanding a removed real directory presents a depth-one Failed row");
                if (!failed) return false;
                pulse::ui::PickerInteractionTest failed_before, failed_after;
                if (!query(window, failed_before)) return false;
                const POINT failed_point = q.tree_row_points[failed_row];
                const bool failed_hit = failed_point.x > 0 && failed_point.y > 0;
                Check(failed_hit, "failed tree hint exposes its real renderer row hit point");
                if (!failed_hit) return false;
                Click(window, failed_point);
                if (!Wait([&] { return query(window, failed_after) && ready(); }, state.done)) return false;
                const bool failed_inert = failed_after.selected_paths == failed_before.selected_paths && state.file_ok == 0 && unchanged();
                Check(failed_inert, "clicking Failed hint neither selects a fake path nor navigates nor emits FileOk");
                if (!failed_inert) return false;
                Check(Capture(window, output / L"tree-failed.png"), "capture actual failed-directory tree hint");
                if (!toggle(window, sender, empty_path)) return false;
                int placeholder = -1;
                const bool loaded = Wait([&] {
                    if (!ready()) return false;
                    const int parent = row(q, empty_path);
                    placeholder = parent + 1;
                    return parent >= 0 && placeholder < static_cast<int>(q.tree_row_count) &&
                        q.tree_kinds[placeholder] == 2 && q.tree_depths[placeholder] == 1;
                }, state.done);
                Check(loaded, "empty real directory expands to a depth-one Empty presentation row");
                if (!loaded) return false;
                pulse::ui::PickerInteractionTest before, after;
                if (!query(window, before)) return false;
                const POINT point = q.tree_row_points[placeholder];
                const bool hit = point.x > 0 && point.y > 0;
                Check(hit, "empty tree hint exposes its real renderer row hit point");
                if (!hit) return false;
                Click(window, point);
                if (!Wait([&] { return query(window, after) && ready(); }, state.done)) return false;
                Check(after.selected_paths == before.selected_paths && state.file_ok == 0 && unchanged(),
                    "clicking Empty hint neither selects a fake path nor navigates nor emits FileOk");
                Check(Capture(window, output / L"tree-empty.png"), "capture actual empty-tree hint");
                // Queue real child enumeration immediately before a navigation. The final-state check
                // is deterministic; this does not claim that every run forced a late worker completion.
                if (!toggle(window, sender, left_path) || !JumpFixture(window, sender, right_path, false, false)) return false;
                const bool moved = Wait([&] {
                    if (!query(window, after) || after.current != right_path || !ready()) return false;
                    return row(q, b) >= 0 && row(q, a) < 0 && row(q, left_path) < 0;
                }, state.done);
                Check(moved, "navigation during child work leaves only the new directory and no stale expanded paths");
                return moved && state.file_ok == 0;
            }
            if (variant == 0) {
                const int parent = row(q, left_path);
                const bool hit = parent >= 0 && q.tree_chevrons[parent].x > 0 && q.tree_chevrons[parent].y > 0;
                Check(hit, "real shared renderer exposes the alpha disclosure hit target");
                if (!hit) return false;
                Click(window, q.tree_chevrons[parent]);
                const bool expanded = Wait([&] {
                    const int child = ready() ? row(q, a) : -1;
                    return child >= 0 && q.tree_depths[child] == 1;
                }, state.done);
                Check(expanded && unchanged(), "mouse disclosure loads disk children inline without FolderChanged or current-folder change");
                if (!expanded || !unchanged()) return false;
                if (!select(window, sender, left_path) || !key(window, VK_LEFT)) return false;
                const bool closed = Wait([&] { return ready() && row(q, a) < 0; }, state.done);
                Check(closed && unchanged(), "VK_LEFT collapses the selected expanded directory without navigation");
                if (!closed || !key(window, VK_RIGHT)) return false;
                const bool reopened = Wait([&] { return ready() && row(q, a) >= 0; }, state.done);
                Check(reopened && unchanged(), "VK_RIGHT reopens its children through the production keyboard handler");
                if (!reopened) return false;
                if (!expand(nested_path, deep)) return false;
                const int nested_row = row(q, nested_path), deep_row = row(q, deep);
                const bool two_levels = nested_row >= 0 && deep_row >= 0 && q.tree_depths[nested_row] == 1 &&
                    q.tree_expanded[nested_row] && q.tree_depths[deep_row] == 2;
                Check(two_levels && unchanged(), "a real nested directory expands to depth two without changing the current folder");
                if (!two_levels || !unchanged()) return false;
            } else if (!expand(left_path, a)) return false;
            if (filtered) {
                Check(row(q, png) < 0 && row(q, hidden) < 0 && row(q, protected_path) < 0 && row(q, a) >= 0,
                    "expanded children inherit the caller TXT filter and hidden-file setting");
                if (row(q, png) >= 0 || row(q, hidden) >= 0 || row(q, protected_path) >= 0) return false;
                reset(); q.display_command = 2;
                if (!Send(window, pulse::ui::kPickerJumpUiTestMessage, reinterpret_cast<WPARAM>(sender), reinterpret_cast<LPARAM>(&q)) ||
                    !expand(left_path, a)) return false;
                const bool shown = Wait([&] { return ready() && q.show_hidden && row(q, hidden) >= 0; }, state.done);
                Check(shown && row(q, png) < 0, "Show hidden reloads expanded children while preserving TXT filtering");
                if (!shown || row(q, png) >= 0) return false;
                const bool protected_hidden = row(q, protected_path) < 0 && !q.show_protected;
                Check(protected_hidden, "Show hidden alone keeps hidden-system children excluded");
                if (!protected_hidden) return false;
                reset(); q.display_prefs_json = LR"({"show_hidden_files":true,"show_protected_os_files":true,"show_file_extensions":true,"folder_sort_mode":0})";
                if (!Send(window, pulse::ui::kPickerJumpUiTestMessage, reinterpret_cast<WPARAM>(sender), reinterpret_cast<LPARAM>(&q)) ||
                    !expand(left_path, a)) return false;
                const bool protected_shown = Wait([&] {
                    return ready() && q.show_hidden && q.show_protected && row(q, hidden) >= 0 && row(q, protected_path) >= 0;
                }, state.done);
                Check(protected_shown && row(q, png) < 0, "both isolated display gates reveal protected child while preserving TXT filtering");
                if (!protected_shown || row(q, png) >= 0) return false;
                const bool capsule = Wait([&] {
                    return ready() && q.tree_filter_buttons[1].right > q.tree_filter_buttons[1].left &&
                        q.tree_filter_buttons[1].bottom > q.tree_filter_buttons[1].top;
                }, state.done);
                Check(capsule, "Open exposes the real all-files context filter capsule");
                if (!capsule) {
                    RECT client{}; GetClientRect(window, &client);
                    std::printf("TREE_FILTER_GEOMETRY raw_count=%u caller0=%ld,%ld,%ld,%ld caller1=%ld,%ld,%ld,%ld compact=%ld,%ld,%ld,%ld client=%ld,%ld\n",
                        q.context_filter_count,
                        q.tree_filter_buttons[0].left, q.tree_filter_buttons[0].top, q.tree_filter_buttons[0].right, q.tree_filter_buttons[0].bottom,
                        q.tree_filter_buttons[1].left, q.tree_filter_buttons[1].top, q.tree_filter_buttons[1].right, q.tree_filter_buttons[1].bottom,
                        q.compact_filters.left, q.compact_filters.top, q.compact_filters.right, q.compact_filters.bottom,
                        client.right - client.left, client.bottom - client.top);
                    Check(Capture(window, output / L"tree-filter-geometry-failed.png"), "capture actual failed Open filter geometry");
                    return false;
                }
                const unsigned before_filter = state.filter_events.load();
                Click(window, Center(q.tree_filter_buttons[1]));
                const bool changed = Wait([&] {
                    return state.filter_index == 1 && state.filter_events == before_filter + 1 && ready();
                }, state.done);
                Check(changed, "Open all-files capsule emits exactly one TypeChanged at caller index one");
                if (!changed || !expand(left_path, a)) return false;
                const bool all = Wait([&] { return ready() && row(q, png) >= 0 && row(q, hidden) >= 0 && row(q, protected_path) >= 0; }, state.done);
                Check(all && unchanged(), "all-files change refreshes descendants without navigation and keeps hidden preference");
                return all && unchanged() && state.file_ok == 0;
            }
            if (multi || collapse) {
                if (!expand(right_path, b) || !select(window, sender, a) || !select(window, sender, b, true)) return false;
                pulse::ui::PickerInteractionTest selected;
                const bool two = Wait([&] {
                    if (!query(window, selected)) return false;
                    auto paths = selected.selected_paths; std::sort(paths.begin(), paths.end());
                    auto expected = std::vector<std::wstring>{a, b}; std::sort(expected.begin(), expected.end());
                    return paths == expected;
                }, state.done);
                Check(two && unchanged(), "sibling same.txt selections retain two distinct full filesystem paths");
                if (!two) return false;
                if (collapse) {
                    if (!toggle(window, sender, left_path)) return false;
                    const bool cleared = Wait([&] {
                        return ready() && row(q, a) < 0 && query(window, selected) && selected.selected_paths == std::vector<std::wstring>{b};
                    }, state.done);
                    Check(cleared, "collapsing one parent removes only its now-invisible child selection");
                    if (!cleared || !unchanged() || state.file_ok != 0) return false;
                    const bool new_folder_hit = q.new_folder.right > q.new_folder.left && q.new_folder.bottom > q.new_folder.top;
                    Check(new_folder_hit, "expanded tree retains a real New Folder toolbar hit target");
                    if (!new_folder_hit) return false;
                    Click(window, Center(q.new_folder));
                    const bool editing = Wait([&] {
                        return query(window, selected) && selected.rename_edit && IsWindowVisible(selected.rename_edit);
                    }, state.done);
                    Check(editing, "New Folder creates a visible provisional naming row after collapsing tree state");
                    if (!editing) return false;
                    const auto draft = fixture / L"tree-draft-cancelled";
                    if (!SendOnly(selected.rename_edit, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(L"tree-draft-cancelled")) ||
                        !SendOnly(selected.rename_edit, WM_KEYDOWN, VK_ESCAPE)) return false;
                    const bool cancelled = Wait([&] {
                        return query(window, selected) && !selected.rename_edit && ready();
                    }, state.done);
                    Check(cancelled && !std::filesystem::exists(draft), "Escape removes the provisional tree row without creating its directory");
                    if (!cancelled || std::filesystem::exists(draft)) return false;
                    if (!expand(left_path, a) || !expand(right_path, b)) return false;
                    const bool restored = row(q, a) >= 0 && row(q, b) >= 0 && unchanged() && state.file_ok == 0;
                    Check(restored, "both real sibling children reopen at correct tree rows after New Folder cancellation");
                    return restored;
                }
                Check(Capture(window, output / L"tree-dark-narrow-multi.png"), "capture actual narrow dark expanded sibling selection");
            } else {
                if (save && !expand(right_path, b)) return false;
                if (!select(window, sender, a)) return false;
                pulse::ui::PickerInteractionTest selected;
                const bool selected_child = Wait([&] { return query(window, selected) && selected.selected_paths == std::vector<std::wstring>{a}; }, state.done);
                Check(selected_child && unchanged(), "selecting an inline child retains its full path without navigating");
                if (!selected_child) return false;
                Check(Filename(window) == L"alpha\\same.txt", "inline filename shows readable root-relative path instead of a long absolute path");
                if (save) {
                    if (!Wait(ready, state.done)) return false;
                    if (!q.preview_requested && !key(window, VK_SPACE)) return false;
                    const bool probed = Wait([&] {
                        return ready() && q.preview_requested && q.save_draft_checked && q.live_conflict;
                    }, state.done);
                    Check(probed, "selected existing child reaches the real Save probe and visible conflict panel");
                    if (!probed) return false;
                    const bool exact_parent = std::wstring(q.tree_save_folder) == L"alpha" &&
                        std::wstring_view(q.tree_save_series).starts_with(L"2") &&
                        std::wstring(q.tree_save_series).find(L"same") != std::wstring::npos &&
                        q.series_badge_count == 1 && q.tree_save_clash && row(q, b) >= 0;
                    Check(exact_parent, "Save panel counts alpha exact plus v2 family, one related-version badge, and conflict without counting bravo duplicate");
                    if (!exact_parent) {
                        std::printf("TREE_SAVE_PANEL folder=%ls series=%ls related_badges=%u clash=%u bravo_row=%d\n",
                            q.tree_save_folder, q.tree_save_series, q.series_badge_count,
                            static_cast<unsigned>(q.tree_save_clash), row(q, b));
                        Check(Capture(window, output / L"tree-save-panel-failed.png"), "capture actual failed Save panel family and conflict state");
                        return false;
                    }
                }
                Check(Capture(window, output / (save ? L"tree-save-child.png" : L"tree-light.png")), "capture actual selected inline child");
            }
            pulse::ui::PickerInteractionTest actions;
            if (!query(window, actions)) return false;
            if (save) {
                if (!Wait([&] { return ready() && q.save_draft_checked; }, state.done)) return false;
                const RECT submit = q.live_conflict ? q.live_save : actions.primary;
                const bool visible_submit = submit.right > submit.left && submit.bottom > submit.top;
                Check(visible_submit, "Save exposes an actionable checked-conflict submit control after the probe");
                if (!visible_submit) return false;
                Click(window, Center(submit));
            } else Click(window, Center(actions.primary));
            return Wait([&] { return state.file_ok == 1; }, state.done);
        };
        auto expected_paths = std::vector<std::wstring>{a};
        if (multi) expected_paths.push_back(b);
        std::sort(expected_paths.begin(), expected_paths.end());
        const auto file_ok = [&](const PulsePickerUiV2Event& event) {
            std::vector<wchar_t> current(32768), filename(32768), selection(32768), candidate(32768), selections(65536), candidates(65536);
            PulsePickerUiV2Snapshot snapshot{}; snapshot.size = sizeof(snapshot);
            snapshot.folder = {current.data(), static_cast<uint32_t>(current.size())};
            snapshot.filename = {filename.data(), static_cast<uint32_t>(filename.size())};
            snapshot.selection = {selection.data(), static_cast<uint32_t>(selection.size())};
            snapshot.candidate = {candidate.data(), static_cast<uint32_t>(candidate.size())};
            snapshot.selection_paths = {selections.data(), static_cast<uint32_t>(selections.size())};
            snapshot.candidate_paths = {candidates.data(), static_cast<uint32_t>(candidates.size())};
            const auto hr = api.Query(session, event.show_id, event.generation, &snapshot);
            const bool exact = hr == S_OK && std::wstring(current.data()) == folder &&
                (multi ? snapshot.candidate_count == 2 && unpack(snapshot.candidate_paths) == expected_paths : std::wstring(candidate.data()) == a);
            Check(exact, save ? "Save FileOk candidate stays at selected child path, never root/same.txt" :
                "FileOk Query exposes exact child paths while the current folder remains the root");
        };
        const auto result = [&](const PulsePickerUiV2Result& value) { return value.path_count == 2 && unpack(value.paths) == expected_paths; };
        const char* label = multi ? "tree Open returns both sibling same.txt full paths exactly once" :
            collapse ? "tree collapse removes invisible selection without accepting" :
            filtered ? "tree child filter and hidden changes finish without accepting" :
            empty_case ? "tree empty hint and navigation leave no stale rows or FileOk" :
            save ? "tree Save accepts the selected nested file path exactly once" : "tree Open accepts exact nested path once after mouse and keyboard expansion";
        if (multi) Run(api, session, config, drive, label, {}, UINT32_MAX, {}, 2, 0, S_OK, result, file_ok);
        else if (collapse || filtered || empty_case) Run(api, session, config, drive, label);
        else Run(api, session, config, drive, label, a, UINT32_MAX, {}, 1, 0, S_OK, {}, file_ok);
    }
}
