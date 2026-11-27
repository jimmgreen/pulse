void AnyFormatCases(PulsePickerUiV2Api& api, PulsePickerUiV2Session session,
    const std::filesystem::path& root, const std::filesystem::path& output) {
    const std::wstring folder = root.wstring(), initial_name = L"any-format-start.txt";
    const std::wstring text_label = L"Text", png_label = L"PNG", all_label = L"All files";
    const std::wstring text_pattern = L"*.txt", png_pattern = L"*.png", all_pattern = L"*.*";
    PulsePickerUiV2Filter filters[] = {
        {sizeof(PulsePickerUiV2Filter), 0, Text(text_label), Text(text_pattern)},
        {sizeof(PulsePickerUiV2Filter), 0, Text(png_label), Text(png_pattern)},
        {sizeof(PulsePickerUiV2Filter), 0, Text(all_label), Text(all_pattern)}
    };
    for (unsigned variant = 0; variant < 2; ++variant) {
        const bool keep_any = variant == 0;
        const uint32_t expected_filter = keep_any ? 2u : 1u;
        const std::wstring typed_name = keep_any ? L"known.png" : L"known-from-text.png";
        const std::wstring expected_path = (root / typed_name).wstring();
        PulsePickerUiV2Config config{};
        config.size = sizeof(config); config.version = 2;
        config.initial_folder = Text(folder); config.filename = Text(initial_name);
        config.access_policy = PULSE_PICKER_UI_V2_ACCESS_LOCAL_SAVE_V1;
        config.save_options = PULSE_PICKER_UI_V2_SAVE_OVERWRITE_PROMPT;
        config.filters = filters; config.filter_count = static_cast<uint32_t>(std::size(filters));
        config.filter_index = 0;
        auto drive = [&](HWND window, HWND sender, State& state) {
            pulse::ui::PickerJumpUiTest q;
            if (!Wait([&] { return Geometry(window, sender, q); }, state.done)) return false;
            const auto before_click = state.filter_events.load();
            if (keep_any) {
                int any_segment = -1;
                for (unsigned i = 0; i < q.format_count; ++i)
                    if (q.format_indices[i] == 2 && q.formats[i].right > q.formats[i].left)
                        any_segment = static_cast<int>(i);
                Check(any_segment >= 0, "G06 exposes caller all-files format as a real clickable segment");
                if (any_segment < 0) return false;
                Click(window, Center(q.formats[any_segment]));
                const bool selected = Wait([&] {
                    return state.filter_index == 2 && state.filter_events == before_click + 1;
                }, state.done);
                Check(selected, "G06 all-files click emits one TypeChanged with caller Query index 2");
                if (!selected) return false;
            }
            const auto before_typing = state.filter_events.load();
            if (!SendOnly(GetDlgItem(window, 102), WM_SETTEXT, 0,
                reinterpret_cast<LPARAM>(typed_name.c_str()))) return false;
            const auto expected_events = before_typing + (keep_any ? 0u : 1u);
            const bool synchronized = Wait([&] {
                return Filename(window) == typed_name && state.filter_index == expected_filter &&
                    state.filter_events == expected_events;
            }, state.done);
            Check(synchronized, keep_any ?
                "G06 typed known PNG keeps all-files caller Query index without extra TypeChanged" :
                "G06 concrete TXT control switches to PNG with exactly one TypeChanged");
            if (!synchronized) return false;
            // Validation temporarily clears Geometry.ready; it is not a format-change failure.
            if (!Wait([&] { return Geometry(window, sender, q); }, state.done)) return false;
            Check(Capture(window, output / (keep_any ? L"any-format-known-png.png" : L"any-format-text-to-png.png")),
                "G06 captures actual picker format selection after typing a known extension");
            const bool stable = state.filter_index == expected_filter && state.filter_events == expected_events &&
                Filename(window) == typed_name;
            Check(stable, "G06 settled picker preserves exact filename and expected TypeChanged count");
            if (!stable) return false;
            if (!SendOnly(GetDlgItem(window, 102), WM_KEYDOWN, VK_RETURN)) return false;
            return Wait([&] { return state.file_ok == 1; }, state.done);
        };
        auto file_ok = [&](const PulsePickerUiV2Event& event) {
            std::vector<wchar_t> current_folder(32768), filename(32768), selection(32768), candidate(32768);
            PulsePickerUiV2Snapshot snapshot{}; snapshot.size = sizeof(snapshot);
            snapshot.folder = {current_folder.data(), static_cast<uint32_t>(current_folder.size())};
            snapshot.filename = {filename.data(), static_cast<uint32_t>(filename.size())};
            snapshot.selection = {selection.data(), static_cast<uint32_t>(selection.size())};
            snapshot.candidate = {candidate.data(), static_cast<uint32_t>(candidate.size())};
            const auto hr = api.Query(session, event.show_id, event.generation, &snapshot);
            Check(hr == S_OK && snapshot.filter_index == expected_filter &&
                std::wstring(filename.data()) == typed_name && std::wstring(candidate.data()) == expected_path,
                keep_any ? "G06 FileOk caller Query returns known.png with original all-files index 2" :
                    "G06 FileOk caller Query returns typed PNG with inferred concrete index 1");
        };
        Run(api, session, config, drive,
            keep_any ? "G06 all-files Save returns exact PNG path and caller index 2 once" :
                "G06 concrete TXT control Save returns exact PNG path and caller index 1 once",
            expected_path, expected_filter, {}, 0, 0, S_OK, {}, file_ok);
    }
}
