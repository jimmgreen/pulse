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
    for (const auto col : {ui::SortColumn::Size, ui::SortColumn::Mtime}) {
        for (const auto dir : {ui::SortDirection::Asc, ui::SortDirection::Desc}) {
            for (const auto group : {app::GroupBy::None, app::GroupBy::Size}) {
                app::ScopedEntryGrouping grouping(static_cast<int>(group));
                auto a = Entry(L"a.txt"), b = Entry(L"b.txt"), c = Entry(L"c.txt");
                a.size = 10; b.size = 20; c.size = 30;
                a.mtime.dwLowDateTime = 10; b.mtime.dwLowDateTime = 20; c.mtime.dwLowDateTime = 30;
                const auto before = sorted({a, b, c}, col, dir);
                a.size = 10000000; a.mtime.dwLowDateTime = 40;
                const auto expected = sorted({a, b, c}, col, dir);
                app::PreparedDirNotifyBatch prepared;
                prepared.events = {{FILE_ACTION_MODIFIED, a.name, {}}};
                prepared.metadata[a.name] = a;
                auto rows = before;
                Check(app::ApplyPreparedDirNotifyBatch(rows, prepared, col, dir) == app::NotifyPatch::Applied &&
                    names(rows) == names(expected), "modified metadata follows current comparator");
                Check(names(app::KeepEntryOrder(before, expected, {}, col, dir)) == names(expected),
                    "full refresh agrees with modified notification");
                prepared.events[0].action = FILE_ACTION_ADDED;
                rows = before;
                app::ApplyPreparedDirNotifyBatch(rows, prepared, col, dir);
                Check(names(rows) == names(expected), "same-name replacement follows comparator");
            }
        }
    }
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
        sorted({z, b}, ui::SortColumn::Name, ui::SortDirection::Asc), {},
        ui::SortColumn::Name, ui::SortDirection::Asc)) == names(held), "unchanged sort key preserves held rename row");
    auto a = Entry(L"a.txt");
    app::PreparedDirNotifyBatch rename;
    rename.events = {{FILE_ACTION_RENAMED_NEW_NAME, z.name, a.name}};
    rename.metadata[z.name] = z;
    rows = {a, b};
    app::ApplyPreparedDirNotifyBatch(rows, rename, ui::SortColumn::Name, ui::SortDirection::Asc);
    Check(names(rows) == names(held) && names(app::KeepEntryOrder({a, b}, {b, z}, {{a.name, z.name}},
        ui::SortColumn::Name, ui::SortDirection::Asc)) == names(held), "rename retains displayed row in batch and refresh");
    printf("SORT_UPDATES failures=%d\n", failures);
    return failures ? 1 : 0;
}
