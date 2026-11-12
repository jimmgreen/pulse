#include "../app/entry_group.h"
static int SortUpdatesTest() {
    const auto sorted = [](std::vector<fs::DirEntry> rows, ui::SortColumn col, ui::SortDirection dir) {
        std::stable_sort(rows.begin(), rows.end(), [&](const auto& a, const auto& b) {
            return app::EntryLess(a, b, col, dir);
        });
        return rows;
    };
    const auto names = [](const auto& rows) {
        std::wstring value;
        for (const auto& row : rows) value += row.name + L"|";
        return value;
    };
    const auto find_row = [](const std::vector<fs::DirEntry>& rows, const std::wstring& name) -> const fs::DirEntry* {
        for (const auto& row : rows)
            if (row.name == name) return &row;
        return nullptr;
    };

    // 1. Ungrouped: saving/modifying a file updates metadata in place without
    //    jumping its row position, even when sorted by Size or Mtime.
    for (const auto col : {ui::SortColumn::Name, ui::SortColumn::Size, ui::SortColumn::Mtime}) {
        for (const auto dir : {ui::SortDirection::Asc, ui::SortDirection::Desc}) {
            app::ScopedEntryGrouping grouping(static_cast<int>(app::GroupBy::None));
            auto a = Entry(L"a.txt"), b = Entry(L"b.txt"), c = Entry(L"c.txt");
            a.size = 10; b.size = 20; c.size = 30;
            a.mtime.dwLowDateTime = 10; b.mtime.dwLowDateTime = 20; c.mtime.dwLowDateTime = 30;
            const auto before = sorted({a, b, c}, col, dir);
            b.size = 10000000; b.mtime.dwLowDateTime = 90;
            const auto fresh_sorted = sorted({a, b, c}, col, dir);

            app::PreparedDirNotifyBatch prepared;
            prepared.events = {{FILE_ACTION_MODIFIED, b.name, {}}};
            prepared.metadata[b.name] = b;
            auto rows = before;
            Check(app::ApplyPreparedDirNotifyBatch(rows, prepared, col, dir) == app::NotifyPatch::Applied &&
                names(rows) == names(before) && find_row(rows, L"b.txt") &&
                find_row(rows, L"b.txt")->size == 10000000 &&
                find_row(rows, L"b.txt")->mtime.dwLowDateTime == 90,
                "modified metadata updates in place without jumping row order");

            const auto kept = app::KeepEntryOrder(before, fresh_sorted, col, dir);
            Check(names(kept) == names(before) && find_row(kept, L"b.txt") &&
                find_row(kept, L"b.txt")->size == 10000000,
                "background refresh preserves row order when metadata changes");

            prepared.events[0].action = FILE_ACTION_ADDED;
            rows = before;
            app::ApplyPreparedDirNotifyBatch(rows, prepared, col, dir);
            Check(names(rows) == names(before) && find_row(rows, L"b.txt") &&
                find_row(rows, L"b.txt")->size == 10000000,
                "same-name replacement retains displayed row");

            // MoveFileExW(tmp, b.txt, MOVEFILE_REPLACE_EXISTING): single batch
            // [ADDED ~tmp.tmp, REMOVED b.txt, RENAMED ~tmp.tmp -> b.txt]
            app::PreparedDirNotifyBatch move_single;
            move_single.events = {
                {FILE_ACTION_ADDED, L"~tmp.tmp", {}},
                {FILE_ACTION_REMOVED, b.name, {}},
                {FILE_ACTION_RENAMED_NEW_NAME, b.name, L"~tmp.tmp"}};
            move_single.metadata[L"~tmp.tmp"] = std::nullopt;
            move_single.metadata[b.name] = b;
            rows = before;
            Check(app::ApplyPreparedDirNotifyBatch(rows, move_single, col, dir) == app::NotifyPatch::Applied &&
                names(rows) == names(before) && find_row(rows, L"b.txt") &&
                find_row(rows, L"b.txt")->size == 10000000,
                "single-batch MoveFileExW atomic save retains target row");

            // MoveFileExW split across two batches: Batch 1 inserts ~WRD0001.tmp;
            // Batch 2 removes b.txt and renames ~WRD0001.tmp -> b.txt.
            auto tmp = Entry(L"~WRD0001.tmp");
            tmp.size = 10000000; tmp.mtime.dwLowDateTime = 85;
            app::PreparedDirNotifyBatch split_b1;
            split_b1.events = {{FILE_ACTION_ADDED, tmp.name, {}}};
            split_b1.metadata[tmp.name] = tmp;
            rows = before;
            app::ApplyPreparedDirNotifyBatch(rows, split_b1, col, dir);

            app::PreparedDirNotifyBatch split_b2;
            split_b2.events = {
                {FILE_ACTION_REMOVED, b.name, {}},
                {FILE_ACTION_RENAMED_NEW_NAME, b.name, tmp.name}};
            split_b2.metadata[b.name] = b;
            Check(app::ApplyPreparedDirNotifyBatch(rows, split_b2, col, dir) == app::NotifyPatch::Applied &&
                names(rows) == names(before) && find_row(rows, L"b.txt") &&
                find_row(rows, L"b.txt")->size == 10000000,
                "split-batch MoveFileExW atomic save drops temp row and retains target row");

            // Direct rename overwrite without preceding REMOVED (dup != at).
            rows = before;
            app::ApplyPreparedDirNotifyBatch(rows, split_b1, col, dir);
            app::PreparedDirNotifyBatch direct_overwrite;
            direct_overwrite.events = {{FILE_ACTION_RENAMED_NEW_NAME, b.name, tmp.name}};
            direct_overwrite.metadata[b.name] = b;
            Check(app::ApplyPreparedDirNotifyBatch(rows, direct_overwrite, col, dir) == app::NotifyPatch::Applied &&
                names(rows) == names(before) && find_row(rows, L"b.txt") &&
                find_row(rows, L"b.txt")->size == 10000000,
                "rename overwrite onto existing row keeps target slot instead of temp slot");

            // ReplaceFileW(b.txt, ~tmp2.tmp, b.bak) + DeleteFileW(b.bak)
            auto tmp2 = Entry(L"~tmp2.tmp");
            tmp2.size = 10000000; tmp2.mtime.dwLowDateTime = 88;
            auto bak = Entry(L"b.bak");
            bak.size = 20; bak.mtime.dwLowDateTime = 20;
            app::PreparedDirNotifyBatch replace_deleted_bak;
            replace_deleted_bak.events = {
                {FILE_ACTION_ADDED, tmp2.name, {}},
                {FILE_ACTION_RENAMED_NEW_NAME, bak.name, b.name},
                {FILE_ACTION_RENAMED_NEW_NAME, b.name, tmp2.name},
                {FILE_ACTION_REMOVED, bak.name, {}}};
            replace_deleted_bak.metadata[tmp2.name] = tmp2;
            replace_deleted_bak.metadata[bak.name] = bak;
            replace_deleted_bak.metadata[b.name] = b;
            rows = before;
            Check(app::ApplyPreparedDirNotifyBatch(rows, replace_deleted_bak, col, dir) == app::NotifyPatch::Applied &&
                names(rows) == names(before) && find_row(rows, L"b.txt") &&
                find_row(rows, L"b.txt")->size == 10000000,
                "ReplaceFileW save with deleted backup retains target row");

            std::vector<std::wstring> sel{L"b.txt"};
            std::wstring focus = L"b.txt";
            app::FollowNotifyRenames(replace_deleted_bak.events, rows, sel, focus);
            Check(sel.size() == 1 && sel[0] == L"b.txt" && focus == L"b.txt",
                "ReplaceFileW save keeps selection and focus on saved target file");

            // ReplaceFileW where editor retains b.bak on disk: b.txt keeps its
            // original slot relative to a.txt and c.txt while b.bak merges sorted.
            app::PreparedDirNotifyBatch replace_kept_bak;
            replace_kept_bak.events = {
                {FILE_ACTION_RENAMED_NEW_NAME, bak.name, b.name},
                {FILE_ACTION_RENAMED_NEW_NAME, b.name, L"~tmp2.tmp"}};
            replace_kept_bak.metadata[bak.name] = bak;
            replace_kept_bak.metadata[b.name] = b;
            rows = before;
            Check(app::ApplyPreparedDirNotifyBatch(rows, replace_kept_bak, col, dir) == app::NotifyPatch::Applied &&
                rows.size() == 4 && find_row(rows, L"b.bak") != nullptr,
                "ReplaceFileW with retained backup keeps both target and backup");
            std::vector<fs::DirEntry> without_bak;
            for (const auto& r : rows) if (r.name != L"b.bak") without_bak.push_back(r);
            Check(names(without_bak) == names(before),
                "ReplaceFileW with retained backup never moves target relative to peers");

            // Delete + recreate in same batch: [REMOVED b.txt, ADDED b.txt].
            app::PreparedDirNotifyBatch del_add;
            del_add.events = {
                {FILE_ACTION_REMOVED, b.name, {}},
                {FILE_ACTION_ADDED, b.name, {}}};
            del_add.metadata[b.name] = b;
            rows = before;
            Check(app::ApplyPreparedDirNotifyBatch(rows, del_add, col, dir) == app::NotifyPatch::Applied &&
                names(rows) == names(before) && find_row(rows, L"b.txt") &&
                find_row(rows, L"b.txt")->size == 10000000,
                "delete-and-recreate save retains target row");
        }
    }

    // 2. Grouped by Size: same-group edit stays in place; cross-group edit moves
    //    into the new group so grouped headers stay accurate.
    for (const auto col : {ui::SortColumn::Size, ui::SortColumn::Mtime}) {
        for (const auto dir : {ui::SortDirection::Asc, ui::SortDirection::Desc}) {
            app::ScopedEntryGrouping grouping(static_cast<int>(app::GroupBy::Size));
            auto a = Entry(L"a.txt"), b = Entry(L"b.txt"), c = Entry(L"c.txt");
            a.size = 10; b.size = 20; c.size = 30;
            a.mtime.dwLowDateTime = 10; b.mtime.dwLowDateTime = 20; c.mtime.dwLowDateTime = 30;
            const auto before = sorted({a, b, c}, col, dir);

            auto a_same_group = a;
            a_same_group.size = 25; a_same_group.mtime.dwLowDateTime = 40;
            app::PreparedDirNotifyBatch same_group_mod;
            same_group_mod.events = {{FILE_ACTION_MODIFIED, a.name, {}}};
            same_group_mod.metadata[a.name] = a_same_group;
            auto rows = before;
            Check(app::ApplyPreparedDirNotifyBatch(rows, same_group_mod, col, dir) == app::NotifyPatch::Applied &&
                names(rows) == names(before),
                "grouped modification within same group retains displayed row");

            a.size = 10000000; a.mtime.dwLowDateTime = 40;
            const auto expected = sorted({a, b, c}, col, dir);
            app::PreparedDirNotifyBatch cross_group_mod;
            cross_group_mod.events = {{FILE_ACTION_MODIFIED, a.name, {}}};
            cross_group_mod.metadata[a.name] = a;
            rows = before;
            Check(app::ApplyPreparedDirNotifyBatch(rows, cross_group_mod, col, dir) == app::NotifyPatch::Applied &&
                names(rows) == names(expected),
                "grouped modification across groups moves row into new group");
            Check(names(app::KeepEntryOrder(before, expected, col, dir)) == names(expected),
                "grouped refresh across groups agrees with notification");
        }
    }

    // 3. Single-event ApplyDirNotify on real disk files (in-place & temp-overwrite).
    {
        const auto dir_path = std::filesystem::absolute(L"bench_data/sort_updates_single_" +
            std::to_wstring(GetCurrentProcessId()));
        std::filesystem::create_directories(dir_path);
        std::ofstream(dir_path / L"a.txt") << "aa";
        std::ofstream(dir_path / L"b.txt") << "bbbb";
        std::ofstream(dir_path / L"~tmp.tmp") << "zzzzzzzzzzzz";
        fs::DirEntry ea, eb, etmp;
        app::FillDirEntry(dir_path.wstring(), L"a.txt", ea);
        app::FillDirEntry(dir_path.wstring(), L"b.txt", eb);
        app::FillDirEntry(dir_path.wstring(), L"~tmp.tmp", etmp);
        std::vector<fs::DirEntry> single_rows{ea, eb, etmp};
        MoveFileExW((dir_path / L"~tmp.tmp").c_str(), (dir_path / L"a.txt").c_str(), MOVEFILE_REPLACE_EXISTING);
        Check(app::ApplyDirNotify(single_rows, dir_path.wstring(),
            {FILE_ACTION_RENAMED_NEW_NAME, L"a.txt", L"~tmp.tmp"},
            ui::SortColumn::Name, ui::SortDirection::Asc) == app::NotifyPatch::Applied &&
            names(single_rows) == L"a.txt|b.txt|" && single_rows[0].size == 12,
            "single ApplyDirNotify temp-overwrite keeps target slot");
        MoveFileExW((dir_path / L"b.txt").c_str(), (dir_path / L"0.txt").c_str(), 0);
        Check(app::ApplyDirNotify(single_rows, dir_path.wstring(),
            {FILE_ACTION_RENAMED_NEW_NAME, L"0.txt", L"b.txt"},
            ui::SortColumn::Name, ui::SortDirection::Asc) == app::NotifyPatch::Applied &&
            names(single_rows) == L"0.txt|a.txt|",
            "single ApplyDirNotify rename moves to its sort position");
        std::filesystem::remove_all(dir_path);
    }

    // 4. Held renames and FollowNotifyRenames chains.
    auto z = Entry(L"z.txt"), b = Entry(L"b.txt");
    z.size = 10; b.size = 20;
    const std::vector<fs::DirEntry> held{z, b};
    app::PreparedDirNotifyBatch unchanged;
    unchanged.events = {{FILE_ACTION_MODIFIED, z.name, {}}};
    z.mtime.dwLowDateTime = 100;
    unchanged.metadata[z.name] = z;
    auto rows = held;
    app::ApplyPreparedDirNotifyBatch(rows, unchanged, ui::SortColumn::Name, ui::SortDirection::Asc);
    Check(names(rows) == names(held) && names(app::KeepEntryOrder(held,
        sorted({z, b}, ui::SortColumn::Name, ui::SortDirection::Asc),
        ui::SortColumn::Name, ui::SortDirection::Asc)) == names(held), "held row survives metadata update");
    auto a = Entry(L"a.txt");
    app::PreparedDirNotifyBatch rename;
    rename.events = {{FILE_ACTION_RENAMED_NEW_NAME, z.name, a.name}};
    rename.metadata[z.name] = z;
    rows = {a, b};
    app::ApplyPreparedDirNotifyBatch(rows, rename, ui::SortColumn::Name, ui::SortDirection::Asc);
    Check(names(rows) == L"b.txt|z.txt|" && names(app::KeepEntryOrder({a, b}, {b, z},
        ui::SortColumn::Name, ui::SortDirection::Asc)) == L"b.txt|z.txt|", "rename moves to its sort position in batch and refresh");
    {   // Renaming a temp file to a brand-new name is a new file: sorted, not the temp's row.
        auto c = Entry(L"c.txt"), tmp = Entry(L"~new.tmp"), n = Entry(L"b2.txt");
        app::PreparedDirNotifyBatch first_save;
        first_save.events = {{FILE_ACTION_ADDED, tmp.name, {}}, {FILE_ACTION_RENAMED_NEW_NAME, n.name, tmp.name}};
        first_save.metadata[tmp.name] = std::nullopt;
        first_save.metadata[n.name] = n;
        std::vector<fs::DirEntry> list{a, b, c};
        Check(app::ApplyPreparedDirNotifyBatch(list, first_save, ui::SortColumn::Name, ui::SortDirection::Asc) ==
            app::NotifyPatch::Applied && names(list) == L"a.txt|b.txt|b2.txt|c.txt|",
            "first save through a temp file lands at its sort position");
        auto upper = Entry(L"A.TXT");
        app::PreparedDirNotifyBatch case_only;
        case_only.events = {{FILE_ACTION_RENAMED_NEW_NAME, upper.name, a.name}};
        case_only.metadata[upper.name] = upper;
        list = {b, a, c};  // a held out of order: a case-only rename leaves it there
        app::ApplyPreparedDirNotifyBatch(list, case_only, ui::SortColumn::Name, ui::SortDirection::Asc);
        Check(names(list) == L"b.txt|A.TXT|c.txt|", "case-only rename keeps its row");
    }
    {
        std::vector<std::wstring> sel{L"a.txt"};
        std::wstring focus = L"a.txt";
        app::FollowNotifyRenames({{FILE_ACTION_RENAMED_NEW_NAME, L"mid.txt", L"a.txt"},
                                  {FILE_ACTION_RENAMED_NEW_NAME, L"z.txt", L"mid.txt"}},
                                 rows, sel, focus);
        Check(sel.size() == 1 && sel[0] == L"z.txt" && focus == L"z.txt",
            "FollowNotifyRenames follows real rename chains when old name is gone");
    }
    printf("SORT_UPDATES failures=%d\n", failures);
    return failures ? 1 : 0;
}
