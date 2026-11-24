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

    // 1. Saving/modifying and atomic replacement follow the current sort.
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
                names(rows) == names(fresh_sorted) && find_row(rows, L"b.txt") &&
                find_row(rows, L"b.txt")->size == 10000000 &&
                find_row(rows, L"b.txt")->mtime.dwLowDateTime == 90,
                "modified metadata moves to current sort position");

            const auto kept = app::KeepEntryOrder(before, fresh_sorted, col, dir);
            Check(names(kept) == names(fresh_sorted) && find_row(kept, L"b.txt") &&
                find_row(kept, L"b.txt")->size == 10000000,
                "background refresh follows current sort when metadata changes");

            prepared.events[0].action = FILE_ACTION_ADDED;
            rows = before;
            app::ApplyPreparedDirNotifyBatch(rows, prepared, col, dir);
            Check(names(rows) == names(fresh_sorted) && find_row(rows, L"b.txt") &&
                find_row(rows, L"b.txt")->size == 10000000,
                "same-name replacement follows current sort");

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
                names(rows) == names(fresh_sorted) && find_row(rows, L"b.txt") &&
                find_row(rows, L"b.txt")->size == 10000000,
                "single-batch MoveFileExW atomic save sorts target row");

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
                names(rows) == names(fresh_sorted) && find_row(rows, L"b.txt") &&
                find_row(rows, L"b.txt")->size == 10000000,
                "split-batch MoveFileExW atomic save drops temp row and sorts target row");

            // Direct rename overwrite without preceding REMOVED (dup != at).
            rows = before;
            app::ApplyPreparedDirNotifyBatch(rows, split_b1, col, dir);
            app::PreparedDirNotifyBatch direct_overwrite;
            direct_overwrite.events = {{FILE_ACTION_RENAMED_NEW_NAME, b.name, tmp.name}};
            direct_overwrite.metadata[b.name] = b;
            Check(app::ApplyPreparedDirNotifyBatch(rows, direct_overwrite, col, dir) == app::NotifyPatch::Applied &&
                names(rows) == names(fresh_sorted) && find_row(rows, L"b.txt") &&
                find_row(rows, L"b.txt")->size == 10000000,
                "rename overwrite onto existing row sorts the replacement instead of holding either slot");

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
                names(rows) == names(fresh_sorted) && find_row(rows, L"b.txt") &&
                find_row(rows, L"b.txt")->size == 10000000,
                "ReplaceFileW save with deleted backup sorts target row");

            std::vector<std::wstring> sel{L"b.txt"};
            std::wstring focus = L"b.txt";
            app::FollowNotifyRenames(replace_deleted_bak.events, rows, sel, focus);
            Check(sel.size() == 1 && sel[0] == L"b.txt" && focus == L"b.txt",
                "ReplaceFileW save keeps selection and focus on saved target file");

            // ReplaceFileW with a retained backup sorts both target and backup.
            app::PreparedDirNotifyBatch replace_kept_bak;
            replace_kept_bak.events = {
                {FILE_ACTION_RENAMED_NEW_NAME, bak.name, b.name},
                {FILE_ACTION_RENAMED_NEW_NAME, b.name, L"~tmp2.tmp"}};
            replace_kept_bak.metadata[bak.name] = bak;
            replace_kept_bak.metadata[b.name] = b;
            rows = before;
            Check(app::ApplyPreparedDirNotifyBatch(rows, replace_kept_bak, col, dir) == app::NotifyPatch::Applied &&
                names(rows) == names(sorted({a, b, c, bak}, col, dir)),
                "ReplaceFileW with retained backup keeps both target and backup");
            std::vector<fs::DirEntry> without_bak;
            for (const auto& r : rows) if (r.name != L"b.bak") without_bak.push_back(r);
            Check(names(without_bak) == names(fresh_sorted),
                "ReplaceFileW with retained backup sorts target relative to peers");

            // Delete + recreate in same batch: [REMOVED b.txt, ADDED b.txt].
            app::PreparedDirNotifyBatch del_add;
            del_add.events = {
                {FILE_ACTION_REMOVED, b.name, {}},
                {FILE_ACTION_ADDED, b.name, {}}};
            del_add.metadata[b.name] = b;
            rows = before;
            Check(app::ApplyPreparedDirNotifyBatch(rows, del_add, col, dir) == app::NotifyPatch::Applied &&
                names(rows) == names(fresh_sorted) && find_row(rows, L"b.txt") &&
                find_row(rows, L"b.txt")->size == 10000000,
                "delete-and-recreate save sorts target row");
        }
    }

    // 2. Both same-group and cross-group edits follow current sort/group order.
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
                names(rows) == names(sorted({a_same_group, b, c}, col, dir)),
                "grouped modification within same group follows current sort");

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
        auto single_rows = sorted({ea, eb, etmp}, ui::SortColumn::Size, ui::SortDirection::Asc);
        MoveFileExW((dir_path / L"~tmp.tmp").c_str(), (dir_path / L"a.txt").c_str(), MOVEFILE_REPLACE_EXISTING);
        Check(app::ApplyDirNotify(single_rows, dir_path.wstring(),
            {FILE_ACTION_RENAMED_NEW_NAME, L"a.txt", L"~tmp.tmp"},
            ui::SortColumn::Size, ui::SortDirection::Asc) == app::NotifyPatch::Applied &&
            names(single_rows) == L"b.txt|a.txt|" && single_rows[1].size == 12,
            "single ApplyDirNotify temp-overwrite reorders by replacement size");
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
        ui::SortColumn::Name, ui::SortDirection::Asc)) == L"b.txt|z.txt|", "unchanged name key stays stable while refresh adopts sorted snapshot");
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
        list = {b, a, c};  // The comparator uses case-sensitive tie breaking for this rename.
        app::ApplyPreparedDirNotifyBatch(list, case_only, ui::SortColumn::Name, ui::SortDirection::Asc);
        Check(names(list) == L"A.TXT|b.txt|c.txt|", "case-only rename follows comparator tie breaking");
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
    // Folder modes and every ordinary mutation share the same comparator.
    for (const auto mode : {app::FolderSortMode::FoldersFirst, app::FolderSortMode::FoldersLast,
                            app::FolderSortMode::Mixed}) {
        app::ScopedFolderSortMode folders(mode);
        for (const auto col : {ui::SortColumn::Name, ui::SortColumn::Size, ui::SortColumn::Mtime}) {
            for (const auto direction : {ui::SortDirection::Asc, ui::SortDirection::Desc}) {
                app::ScopedEntryGrouping grouping(static_cast<int>(app::GroupBy::None));
                auto folder = Entry(L"folder"), first = Entry(L"a.txt"), last = Entry(L"z.txt");
                folder.is_dir = true;
                folder.mtime.dwLowDateTime = 20;
                first.size = 10; first.mtime.dwLowDateTime = 10;
                last.size = 30; last.mtime.dwLowDateTime = 30;
                auto list = sorted({folder, first, last}, col, direction);
                first.size = 50; first.mtime.dwLowDateTime = 50;
                app::PreparedDirNotifyBatch patch;
                patch.events = {{FILE_ACTION_MODIFIED, first.name, {}}};
                patch.metadata[first.name] = first;
                Check(app::ApplyPreparedDirNotifyBatch(list, patch, col, direction) == app::NotifyPatch::Applied &&
                    names(list) == names(sorted({folder, first, last}, col, direction)),
                    "file metadata reorder respects folder mode and direction");
                auto added = Entry(L"m.txt"); added.size = 25; added.mtime.dwLowDateTime = 25;
                patch.events = {{FILE_ACTION_ADDED, added.name, {}}}; patch.metadata[added.name] = added;
                app::ApplyPreparedDirNotifyBatch(list, patch, col, direction);
                Check(names(list) == names(sorted({folder, first, last, added}, col, direction)),
                    "new entry inserts in current sort with folder mode");
                patch.events = {{FILE_ACTION_REMOVED, last.name, {}}};
                app::ApplyPreparedDirNotifyBatch(list, patch, col, direction);
                Check(names(list) == names(sorted({folder, first, added}, col, direction)),
                    "deletion preserves remaining current sort");
                auto renamed = first; renamed.name = L"zz.txt";
                patch.events = {{FILE_ACTION_RENAMED_NEW_NAME, renamed.name, first.name}};
                patch.metadata[renamed.name] = renamed;
                app::ApplyPreparedDirNotifyBatch(list, patch, col, direction);
                std::vector<std::wstring> selected{first.name}; std::wstring focused = first.name;
                app::FollowNotifyRenames(patch.events, list, selected, focused);
                Check(names(list) == names(sorted({folder, renamed, added}, col, direction)) &&
                    selected == std::vector<std::wstring>{renamed.name} && focused == renamed.name,
                    "rename sorts and selection follows identity across folder modes");
            }
        }
    }
    printf("SORT_UPDATES failures=%d\n", failures);
    return failures ? 1 : 0;
}


static int SortUpdatesLiveWatchTest() {
    const auto root = std::filesystem::absolute(L"bench_data/sort_live_watch_" +
        std::to_wstring(GetCurrentProcessId()));
    std::filesystem::create_directories(root);
    for (const auto col : {ui::SortColumn::Size, ui::SortColumn::Mtime}) {
        for (const auto direction : {ui::SortDirection::Asc, ui::SortDirection::Desc}) {
            const auto write = [&](const wchar_t* name, size_t size, ULONGLONG time) {
                std::ofstream(root / name, std::ios::binary | std::ios::trunc) << std::string(size, 'x');
                HANDLE file = CreateFileW((root / name).c_str(), FILE_WRITE_ATTRIBUTES,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
                FILETIME stamp{static_cast<DWORD>(time), static_cast<DWORD>(time >> 32)};
                const bool ok = file != INVALID_HANDLE_VALUE && SetFileTime(file, nullptr, nullptr, &stamp);
                if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
                return ok;
            };
            Check(write(L"a.txt", 2, 130000000000000000ULL) && write(L"b.txt", 4, 131000000000000000ULL),
                "live watcher fixture has deterministic size and modification time");
            fs::DirEntry a, b;
            app::FillDirEntry(root.wstring(), L"a.txt", a);
            app::FillDirEntry(root.wstring(), L"b.txt", b);
            std::vector<fs::DirEntry> rows{a, b};
            app::SortEntries(rows, col, direction);
            HANDLE directory = CreateFileW(root.c_str(), FILE_LIST_DIRECTORY,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
            OVERLAPPED operation{};
            operation.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            alignas(FILE_NOTIFY_INFORMATION) BYTE buffer[4096]{};
            const bool armed = directory != INVALID_HANDLE_VALUE && operation.hEvent &&
                ReadDirectoryChangesW(directory, buffer, sizeof(buffer), FALSE,
                    FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE, nullptr, &operation, nullptr);
            Check(armed, "real ReadDirectoryChangesW request arms before write");
            Check(write(L"a.txt", 12, 132000000000000000ULL), "real file write changes size and modification time");
            DWORD received = 0;
            const bool completed = armed && WaitForSingleObject(operation.hEvent, 5000) == WAIT_OBJECT_0 &&
                GetOverlappedResult(directory, &operation, &received, FALSE) && received > 0;
            std::vector<fs::DirNotifyEvent> events;
            if (completed) {
                DWORD offset = 0;
                for (;;) {
                    const auto* event = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(buffer + offset);
                    events.push_back({event->Action, std::wstring(event->FileName,
                        event->FileNameLength / sizeof(wchar_t)), {}});
                    if (!event->NextEntryOffset) break;
                    offset += event->NextEntryOffset;
                }
            }
            Check(completed && !events.empty(), "real watcher delivers filesystem modification events");
            if (directory != INVALID_HANDLE_VALUE) {
                CancelIoEx(directory, &operation);
                if (armed) GetOverlappedResult(directory, &operation, &received, TRUE);
                CloseHandle(directory);
            }
            if (operation.hEvent) CloseHandle(operation.hEvent);
            app::PreparedDirNotifyBatch prepared;
            std::thread worker([&] { prepared = app::PrepareDirNotifyBatch(root.wstring(), events); });
            worker.join();
            const auto result = app::ApplyPreparedDirNotifyBatch(rows, prepared, col, direction);
            const std::wstring expected = direction == ui::SortDirection::Asc ? L"b.txt" : L"a.txt";
            Check(completed && result == app::NotifyPatch::Applied && rows.size() == 2 && rows[0].name == expected,
                "real watcher metadata preparation and patch reorder ascending and descending");
        }
    }
    std::filesystem::remove_all(root);
    printf("SORT_LIVE_WATCH failures=%d\n", failures);
    return failures ? 1 : 0;
}
