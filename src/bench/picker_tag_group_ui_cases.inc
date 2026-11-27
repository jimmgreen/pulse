void TagGroupCases(PulsePickerUiV2Api& api, PulsePickerUiV2Session session,
    const std::filesystem::path& root, const std::filesystem::path& output) {
    const auto unique = std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
    const auto fixture = root / (L"tag-groups-" + unique);
    std::filesystem::create_directories(fixture / L"child");
    for (const auto* leaf : {L"a.txt", L"b.txt", L"untagged.txt"})
        std::ofstream(fixture / leaf, std::ios::binary) << "isolated picker tag grouping fixture\n";
    const std::wstring folder = fixture.wstring(), expected_path = (fixture / L"a.txt").wstring();
    const std::wstring mapping = L"Local\\Pulse.PickerTagUiTest." + unique;
    const std::wstring missing_mapping = mapping + L".Missing";
    pulse::picker_tags::detail::Writer writer(mapping);
    pulse::picker_tags::Snapshot tags;
    tags.available = true; tags.revision = 1;
    tags.tags = {{L"工作", 0x3366aau}, {L"重要", 0xdd8844u}};
    tags.paths = {{(fixture / L"a.txt").wstring(), 0}, {(fixture / L"b.txt").wstring(), 1}};
    pulse::picker_tags::Publish(tags, writer);
    const bool published = pulse::picker_tags::Read(mapping) == tags;
    Check(published, "tag groups publish and read an isolated real shared catalog snapshot");
    if (!published) return;
    for (unsigned variant = 0; variant < 4; ++variant) {
        const bool narrow_dark = variant == 1, location = variant == 2, missing = variant == 3;
        PulsePickerUiV2Config config{};
        config.size = sizeof(config); config.version = 2;
        config.initial_folder = Text(folder);
        config.access_policy = PULSE_PICKER_UI_V2_ACCESS_LOCAL_OPEN_V1;
        config.appearance = narrow_dark ? PULSE_PICKER_UI_V2_DARK : 0;
        Run(api, session, config, [&](HWND window, HWND sender, State& state) {
            pulse::ui::PickerJumpUiTest q;
            const auto ready = [&] { return Wait([&] { return Geometry(window, sender, q); }, state.done); };
            if (!ready()) return false;
            // Reset only this isolated dialog's appearance/order snapshot, never persisted preferences.
            q = {}; q.display_prefs_json = L"{}";
            if (!Send(window, pulse::ui::kPickerJumpUiTestMessage,
                reinterpret_cast<WPARAM>(sender), reinterpret_cast<LPARAM>(&q)) || !ready()) return false;
            pulse::picker_tags::Publish(tags, writer);
            q = {}; q.tag_mapping = missing ? missing_mapping.c_str() : mapping.c_str();
            if (!Send(window, pulse::ui::kPickerJumpUiTestMessage,
                reinterpret_cast<WPARAM>(sender), reinterpret_cast<LPARAM>(&q)) || !ready()) return false;
            // Exercise the existing commands: Mixed, Name, Ascending, then requested grouping.
            for (const int command : {46, 10, 14, location ? 36 : 35}) {
                q = {}; q.display_command = command;
                if (!Send(window, pulse::ui::kPickerJumpUiTestMessage,
                    reinterpret_cast<WPARAM>(sender), reinterpret_cast<LPARAM>(&q)) || !ready()) return false;
            }
            if (narrow_dark) {
                q = {}; q.client_width_dip = 640;
                if (!Send(window, pulse::ui::kPickerJumpUiTestMessage,
                    reinterpret_cast<WPARAM>(sender), reinterpret_cast<LPARAM>(&q)) || !ready()) return false;
            }
            const int expected_group = missing ? 0 : location ? 6 : 5;
            const unsigned expected_count = missing ? 0u : location ? 1u : 3u;
            const bool grouped = Wait([&] {
                return Geometry(window, sender, q) && q.group_by == expected_group &&
                    q.group_count == expected_count && q.display_count == 4;
            }, state.done);
            Check(grouped, missing ? "missing tag catalog keeps four real entries flat instead of inventing an untagged group" :
                location ? "Location groups all four mixed entries under their shared real parent" :
                    "Tag displays two catalog groups followed by one untagged group");
            if (!grouped) return false;
            if (!missing && !location) {
                const bool metadata = std::wstring(q.group_labels[0]) == L"工作" &&
                    std::wstring(q.group_labels[1]) == L"重要" && q.group_colors[0] == 0x3366aau &&
                    q.group_colors[1] == 0xdd8844u && q.group_colors[2] == 0 &&
                    q.group_counts[0] == 1 && q.group_counts[1] == 1 && q.group_counts[2] == 2 &&
                    std::wstring(q.raw_names[0]) == L"a.txt" && std::wstring(q.raw_names[1]) == L"b.txt" &&
                    std::wstring(q.raw_names[2]) == L"child" && std::wstring(q.raw_names[3]) == L"untagged.txt";
                Check(metadata, "Tag preserves catalog rank, names, RGB colors, and places untagged directory/file last");
                if (!metadata) return false;
            } else if (location) {
                const bool parent = std::wstring(q.group_labels[0]) == folder && q.group_counts[0] == 4;
                Check(parent, "Location header identifies the actual fixture parent and counts all four entries");
                if (!parent) return false;
            }
            const auto screenshot = missing ? L"tag-group-missing-flat.png" : location ? L"tag-group-location.png" :
                narrow_dark ? L"tag-group-dark-narrow.png" : L"tag-group-light.png";
            Check(Capture(window, output / screenshot), "capture actual tag/location picker grouping state");
            if (!missing && !location) {
                q = {}; q.inspect_group_headers = true;
                if (!Send(window, pulse::ui::kPickerJumpUiTestMessage,
                    reinterpret_cast<WPARAM>(sender), reinterpret_cast<LPARAM>(&q))) return false;
                const bool header = q.group_count == 3 && q.group_header_points[0].y > 0 && !q.group_collapsed[0];
                Check(header, "shared renderer exposes a real visible expanded tag header hit target");
                if (!header) return false;
                Click(window, q.group_header_points[0]);
                const bool collapsed = Wait([&] {
                    return Geometry(window, sender, q) && q.group_count == 3 && q.group_collapsed[0];
                }, state.done);
                Check(collapsed, "clicking the real tag header collapses its group without changing group membership");
                if (!collapsed) return false;
                Check(Capture(window, output / (narrow_dark ? L"tag-group-dark-collapsed.png" : L"tag-group-light-collapsed.png")),
                    "capture actual collapsed tag group");
                q = {}; q.inspect_group_headers = true;
                if (!Send(window, pulse::ui::kPickerJumpUiTestMessage,
                    reinterpret_cast<WPARAM>(sender), reinterpret_cast<LPARAM>(&q)) || q.group_header_points[0].y <= 0)
                    return false;
                Click(window, q.group_header_points[0]);
                const bool reopened = Wait([&] {
                    return Geometry(window, sender, q) && q.group_count == 3 && !q.group_collapsed[0];
                }, state.done);
                Check(reopened, "clicking the collapsed tag header reopens its original group");
                if (!reopened) return false;
            }
            if (!SendOnly(GetDlgItem(window, 102), WM_SETTEXT, 0, reinterpret_cast<LPARAM>(L"a.txt")) ||
                !SendOnly(GetDlgItem(window, 102), WM_KEYDOWN, VK_RETURN)) return false;
            return Wait([&] { return state.file_ok == 1; }, state.done);
        }, "grouped or missing-catalog Open returns the actual a.txt path through exactly one FileOk", expected_path);
    }
}
