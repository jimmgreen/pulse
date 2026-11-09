// entry_sort_key_test.cpp - SortEntries must order rows exactly as std::sort
// with EntryLess does; the keyed fast path only changes the speed.
#include "../app/entry_sort.h"
#include "../app/entry_group.h"

#include <windows.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cwctype>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <vector>

using namespace pulse;

namespace {
int failures = 0;
void Check(bool ok, const char* what) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++failures;
}

std::vector<std::wstring> TrickyNames() {
    return {
        L"a", L"A", L"a1", L"a01", L"a001", L"a2", L"a10", L"a100", L"A10", L"a 10", L"a_10", L"a-10", L"a.10",
        L"file1.txt", L"file01.txt", L"file10.txt", L"file2.txt", L"File2.TXT", L"file.txt", L"file", L"file_",
        L"\u6587\u4ef61", L"\u6587\u4ef610", L"\u6587\u4ef62", L"\u6587\u6863", L"\u56fe\u7247", L"z", L"Z", L"_x", L"-x",
        L"~x", L"!x", L"#x", L"$x", L"(x)", L"[x]", L"x 1", L"x  1", L"x1y2", L"x1y10", L"x10y1", L"1", L"01",
        L"001", L"0", L"00", L"10", L"9", L"1.5", L"1.10", L"1,5", L"\u00df", L"ss", L"\u00e9", L"e", L"E",
        L"\u00c5ngstr\u00f6m", L"angstrom", L"\u3042", L"\u30a2", L"\ud55c", L"\u3010x\u3011", L"\uff08x\uff09",
        L"Report 2024-01-05", L"Report 2024-1-5", L"Report 2024-10-05", L"v1.2.10", L"v1.2.9", L"v1.10.0",
        L"12345678901234567890", L"12345678901234567891", L"0x10", L"0x9", L"a\uff5eb", L"a~b",
    };
}

// Names outside the keyed character set: they must send the list down the
// per-comparison path, and the order must still match.
std::vector<std::wstring> ExoticNames() {
    return {L"\u2460", L"\u2461x", L"\u0663", L"x\u0663" L"2", L"\u00bd", L"\uff21\uff22", L"a\u0301", L"\u0e48",
            L"\U0001F600", L"\U0001F600" L"10", L"\u2167", L"\u00b2"};
}

std::vector<fs::DirEntry> MakeRows(const std::vector<std::wstring>& names, std::mt19937& rng) {
    std::vector<fs::DirEntry> rows;
    rows.reserve(names.size());
    for (const auto& name : names) {
        fs::DirEntry e;
        e.name = name;
        e.full_path = L"C:\\t\\" + name;
        e.is_dir = rng() % 5 == 0;
        e.size = rng() % 4;                       // many equal sizes, so names break ties
        const uint64_t t = 133000000000000000ull + (rng() % 3) * 10000000ull;
        e.mtime.dwLowDateTime = static_cast<DWORD>(t);
        e.mtime.dwHighDateTime = static_cast<DWORD>(t >> 32);
        e.ctime = e.mtime;
        e.atime = e.mtime;
        rows.push_back(std::move(e));
    }
    return rows;
}

bool SameOrder(std::vector<fs::DirEntry> rows, ui::SortColumn col, ui::SortDirection dir) {
    auto expected = rows;
    std::sort(expected.begin(), expected.end(), [&](const fs::DirEntry& a, const fs::DirEntry& b) {
        return app::EntryLess(a, b, col, dir);
    });
    app::SortEntries(rows, col, dir);
    if (rows.size() != expected.size()) return false;
    for (size_t i = 0; i < rows.size(); ++i)
        if (rows[i].name != expected[i].name) return false;
    return true;
}

bool AllOrders(const std::vector<fs::DirEntry>& rows) {
    const ui::SortColumn cols[] = {ui::SortColumn::Name, ui::SortColumn::Size, ui::SortColumn::Mtime,
                                   ui::SortColumn::Type};
    const app::FolderSortMode modes[] = {app::FolderSortMode::FoldersFirst, app::FolderSortMode::FoldersLast,
                                         app::FolderSortMode::Mixed};
    bool ok = true;
    for (const auto mode : modes) {
        app::SetFolderSortMode(mode);
        for (const auto col : cols)
            for (const auto dir : {ui::SortDirection::Asc, ui::SortDirection::Desc})
                ok = SameOrder(rows, col, dir) && ok;
    }
    app::SetFolderSortMode(app::FolderSortMode::FoldersFirst);
    return ok;
}

std::vector<std::wstring> Combos(const std::vector<std::wstring>& pieces, size_t count, unsigned seed) {
    std::mt19937 rng(seed);
    std::vector<std::wstring> out;
    for (size_t i = 0; i < count; ++i)
        out.push_back(pieces[rng() % pieces.size()] + pieces[rng() % pieces.size()] + std::to_wstring(rng() % 1000));
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}
std::shared_ptr<const app::MediaGroupMap> test_media;
std::shared_ptr<const app::MediaGroupMap> TestMedia(const std::wstring&, app::GroupBy) { return test_media; }

void GroupFolderOrders() {
    using app::FolderSortMode;
    using app::GroupBy;
    using ui::SortColumn;
    using ui::SortDirection;
    const auto now = app::MakeGroupClock();
    std::vector<fs::DirEntry> source(7);
    const wchar_t* names[] = {L"z-old-folder", L"a-new-folder", L"m-link.lnk", L"b-old.mp4", L"z-new.mp4", L"a-unknown.txt", L"m-known.mp4"};
    for (size_t i = 0; i < source.size(); ++i) {
        auto& e = source[i];
        e.name = names[i];
        e.full_path = L"C:\\fixture\\" + e.name;
        e.is_dir = i < 2;
        e.size = (i + 1) * 17000;
        const uint64_t ticks = i % 2 ? now.today : now.year - 864000000000ull;
        e.mtime = FILETIME{static_cast<DWORD>(ticks), static_cast<DWORD>(ticks >> 32)};
        e.ctime = e.atime = e.mtime;
        e.net_group = static_cast<uint8_t>(i % 3);
        e.pc_device = i % 3 == 1;
        e.pc_launcher = i % 3 == 2;
    }
    source[2].link_target = L"C:\\fixture\\target";
    source[2].link_target_is_dir = true;
    auto media = std::make_shared<app::MediaGroupMap>();
    media->labels = {L"large", L"small"};
    for (size_t i : {size_t{3}, size_t{4}, size_t{6}}) {
        const auto& e = source[i];
        const uint64_t t = (uint64_t{e.mtime.dwHighDateTime} << 32) | e.mtime.dwLowDateTime;
        media->by_name[e.name] = {t, e.size, i == 4 ? 0 : 1};
    }
    test_media = media;
    app::SetMediaGroupSource(TestMedia);
    bool placement = true, consistent = true, spans = true, inside = true, partial = true, incremental = true, virtuals = true;
    bool unknown_last = true, stable_key = true, stable_search = true;
    for (int group = 0; group <= 14; ++group) {
        const auto by = static_cast<GroupBy>(group);
        const bool fixed = by == GroupBy::ThisPc || by == GroupBy::Network || by == GroupBy::AiDup;
        app::ScopedEntryGrouping grouping(group, L"C:\\fixture");
        for (const auto mode : {FolderSortMode::FoldersFirst, FolderSortMode::FoldersLast, FolderSortMode::Mixed}) {
            app::ScopedFolderSortMode folders(mode);
            for (const auto col : {SortColumn::Name, SortColumn::Type, SortColumn::Mtime, SortColumn::Created,
                                  SortColumn::Size, SortColumn::MediaTaken, SortColumn::MediaDims}) {
                for (const auto dir : {SortDirection::Asc, SortDirection::Desc}) {
                    auto clock = app::MakeGroupClock(col);
                    clock.folders = mode;
                    clock.media = media;
                    auto rows = source;
                    app::SortEntries(rows, col, dir);
                    consistent &= SameOrder(source, col, dir);
                    const auto is_folder = [](const auto& e) { return e.is_dir || e.link_target_is_dir; };
                    if (app::IsMediaGroup(by)) {
                        const fs::DirEntry* last_file = nullptr;
                        for (const auto& e : rows) if (!is_folder(e)) last_file = &e;
                        unknown_last &= last_file && last_file->name == L"a-unknown.txt";
                    }
                    if (group && !fixed && mode != FolderSortMode::Mixed) {
                        for (size_t i = 0; i < 3; ++i)
                            stable_key &= app::GroupKey(source[i], by, clock) == L"folders";
                    }
                    if (!fixed && mode != FolderSortMode::Mixed) {
                        for (size_t i = 0; i < rows.size(); ++i)
                            placement &= is_folder(rows[i]) == (mode == FolderSortMode::FoldersFirst ? i < 3 : i >= 4);
                    }
                    std::set<std::wstring> keys;
                    std::wstring previous;
                    for (size_t i = 0; i < rows.size() && group != 0; ++i) {
                        const auto key = app::GroupKey(rows[i], by, clock);
                        if (i == 0 || key != previous) spans &= keys.insert(key).second;
                        if (i > 0) {
                            const int compare = app::GroupCompare(rows[i-1], rows[i], by, clock, col, dir);
                            spans &= (compare == 0) == (key == previous);
                            if (key == previous)
                                inside &= !app::EntryLess(rows[i], rows[i-1], col, dir, mode);
                            if (fixed) virtuals &= compare <= 0;
                        }
                        previous = key;
                    }
                    if (group > 0 && group <= 9) {
                        auto indexed = source;
                        std::vector<std::wstring> snippets;
                        for (const auto& e : indexed) snippets.push_back(e.name);
                        app::StableGroupOrder(indexed, &snippets, by, col, dir, mode);
                        for (size_t i = 0; i < indexed.size(); ++i) {
                            stable_search &= snippets[i] == indexed[i].name;
                            if (i > 0) stable_search &= app::GroupCompare(indexed[i-1], indexed[i], by, clock, col, dir) <= 0;
                        }
                    }
                    auto top = source;
                    app::SortFirstEntries(top, col, dir, 3);
                    for (size_t i = 0; i < top.size(); ++i) partial &= top[i].name == rows[i].name;
                    std::vector<fs::DirEntry> inserted;
                    for (const auto& e : source) {
                        const auto at = std::lower_bound(inserted.begin(), inserted.end(), e, [&](const auto& a, const auto& b) {
                            return app::EntryLess(a, b, col, dir);
                        });
                        inserted.insert(at, e);
                    }
                    for (size_t i = 0; i < rows.size(); ++i) incremental &= inserted[i].name == rows[i].name;
                }
            }
        }
    }
    Check(placement, "all configurable groups and no grouping honor global folder first/last including shortcuts");
    Check(consistent, "cached full sort and EntryLess agree for every group, mode, column and direction");
    Check(spans, "group identities are contiguous, unique and match header boundaries in all modes");
    Check(inside, "within each group rows follow the selected column and direction");
    Check(partial && incremental, "top-K and incremental refresh insertion match the full grouped sort");
    Check(virtuals, "This PC, Network and duplicate groups retain their fixed hierarchy");
    Check(unknown_last, "unknown media metadata stays after known file groups in both directions");
    Check(stable_key, "directory collapse key survives folder placement, sort column and group changes");
    Check(stable_search, "indexed search regrouping follows the same directory policy and preserves parallel snippets");
    app::SetMediaGroupSource(nullptr);
    test_media.reset();
}
} // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--group-folders") {
        GroupFolderOrders();
        return failures ? 1 : 0;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--folder-bottom") {
        fs::DirEntry file, folder, link;
        file.name = L"middle.txt";
        folder.name = L"a-folder"; folder.is_dir = true;
        link.name = L"z-folder.lnk"; link.link_target = L"C:\\Folder"; link.link_target_is_dir = true;
        bool bottom = true, top = true, mixed = true;
        for (const auto direction : {ui::SortDirection::Asc, ui::SortDirection::Desc}) {
            for (int column = 0; column <= static_cast<int>(ui::SortColumn::MediaFps); ++column) {
                const auto col = static_cast<ui::SortColumn>(column);
                const app::ScopedFolderSortMode mode(app::FolderSortMode::FoldersLast);
                for (const auto& item : {folder, link}) {
                    bottom &= app::EntryLess(file, item, col, direction) && !app::EntryLess(item, file, col, direction);
                    top &= app::EntryLess(item, file, col, direction, app::FolderSortMode::FoldersFirst);
                }
                auto rows = std::vector<fs::DirEntry>{folder, file, link};
                app::SortEntries(rows, col, direction);
                bottom &= rows.front().name == file.name;
                app::SortFirstEntries(rows, col, direction, 1);
                bottom &= rows.size() == 1 && rows.front().name == file.name;
            }
            mixed &= app::EntryLess(folder, file, ui::SortColumn::Name, direction, app::FolderSortMode::Mixed) ==
                (direction == ui::SortDirection::Asc);
        }
        Check(bottom, "bottom keeps folders and directory shortcuts after files across columns and directions, including top-K");
        Check(top && mixed, "top and mixed retain their existing semantics");
        return failures ? 1 : 0;
    }

    if (argc == 2 && std::string_view(argv[1]) == "--group-perf") {
        std::mt19937 random(7482);
        std::vector<std::wstring> names;
        for (int i = 0; i < 50000; ++i) names.push_back(L"document_" + std::to_wstring(i) + L"_report.txt");
        auto fixture = MakeRows(names, random);
        std::shuffle(fixture.begin(), fixture.end(), random);
        auto catalog = std::make_shared<app::TagCatalogSnapshot>();
        catalog->tags.resize(8);
        for (size_t i = 0; i < names.size(); ++i)
            catalog->paths.emplace_back(L"c:\\t\\" + names[i], static_cast<int>(i % 8));
        app::PublishTagCatalog(catalog);
        for (const auto group : {app::GroupBy::None, app::GroupBy::Name, app::GroupBy::Tag}) {
            app::ScopedEntryGrouping grouping(static_cast<int>(group), L"C:\\t");
            std::vector<double> times;
            for (int run = 0; run < 6; ++run) {
                auto rows = fixture;
                const auto start = std::chrono::steady_clock::now();
                app::SortEntries(rows, ui::SortColumn::Name, ui::SortDirection::Asc);
                const double elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
                if (run) times.push_back(elapsed);
            }
            std::sort(times.begin(), times.end());
            std::printf("group=%d rows=%zu median_ms=%.3f min_ms=%.3f max_ms=%.3f\n",
                static_cast<int>(group), fixture.size(), times[2], times.front(), times.back());
            std::fflush(stdout);
        }
        app::PublishTagCatalog({});
        return 0;
    }

    std::mt19937 rng(42);
    Check(AllOrders(MakeRows(TrickyNames(), rng)), "keyed names: every column, direction and folder mode match EntryLess");
    Check(AllOrders(MakeRows(Combos(TrickyNames(), 6000, 7), rng)), "6000 combined names match EntryLess");

    {
        auto names = TrickyNames();
        names.push_back(std::wstring(600, L'x') + L"123.txt");
        auto rows = MakeRows(names, rng);
        const auto clock = app::MakeGroupClock();
        auto catalog = std::make_shared<app::TagCatalogSnapshot>();
        catalog->tags.resize(3);
        for (size_t i = 0; i < rows.size(); ++i) {
            auto& row = rows[i];
            std::wstring lower = row.name;
            for (auto& ch : lower) ch = static_cast<wchar_t>(std::towlower(ch));
            catalog->paths.emplace_back(L"c:\\t\\" + lower, static_cast<int>(i % 3));
            row.full_path = L"\\\\?\\C:\\" + std::wstring(280, L'p') + std::to_wstring(i % 4) + L"\\" + row.name;
            row.net_group = static_cast<uint8_t>(i % 4);
            row.pc_device = i % 7 == 0;
            row.pc_launcher = i % 11 == 0;
            row.size = (i % 8) * 512ull * 1024 * 1024;
            const auto ft = [](uint64_t t) { return FILETIME{static_cast<DWORD>(t), static_cast<DWORD>(t >> 32)}; };
            row.mtime = ft(clock.today - (i % 30) * 864000000000ull);
            row.ctime = ft(clock.today - (i % 6) * 864000000000ull);
            row.atime = ft(clock.tomorrow + (i % 3) * 864000000000ull);
        }
        app::PublishTagCatalog(catalog);
        bool same = true;
        for (int group = 1; group <= 9; ++group) {
            const app::ScopedEntryGrouping grouping(group, L"C:\\t");
            for (const auto mode : {app::FolderSortMode::FoldersFirst, app::FolderSortMode::FoldersLast, app::FolderSortMode::Mixed}) {
                const app::ScopedFolderSortMode folders(mode);
                for (const auto col : {ui::SortColumn::Name, ui::SortColumn::Size, ui::SortColumn::Type,
                        ui::SortColumn::Mtime, ui::SortColumn::Created, ui::SortColumn::Accessed, ui::SortColumn::Path})
                    for (const auto dir : {ui::SortDirection::Asc, ui::SortDirection::Desc})
                        same = SameOrder(rows, col, dir) && same;
            }
        }
        app::PublishTagCatalog({});
        Check(same, "cached ranks preserve all groups, columns, directions, folder modes and long names/paths");
    }

    auto mixed = TrickyNames();
    for (const auto& e : ExoticNames()) mixed.push_back(e);
    Check(AllOrders(MakeRows(mixed, rng)), "a list with enclosed/non-ASCII digits, marks and emoji still matches");
    Check(AllOrders(MakeRows(Combos(mixed, 6000, 11), rng)), "6000 combined names with exotic pieces match");

    {
        auto rows = MakeRows(Combos(TrickyNames(), 2000, 3), rng);
        const auto before = rows;
        bool threw = false;
        int calls = 0;
        try {
            app::SortEntries(rows, ui::SortColumn::Name, ui::SortDirection::Asc, [&] { if (++calls == 500) throw 1; });
        } catch (int) { threw = true; }
        bool untouched = rows.size() == before.size();
        for (size_t i = 0; untouched && i < rows.size(); ++i) untouched = rows[i].name == before[i].name;
        Check(threw && untouched, "a throwing tick abandons the sort and leaves the rows untouched");
    }
    {
        std::vector<fs::DirEntry> one = MakeRows({L"only"}, rng), none;
        app::SortEntries(one, ui::SortColumn::Name, ui::SortDirection::Asc);
        app::SortEntries(none, ui::SortColumn::Name, ui::SortDirection::Asc);
        Check(one.size() == 1 && none.empty(), "empty and single-row lists are left as they are");
    }
    {
        std::vector<std::wstring> names;
        for (int i = 0; i < 100000; ++i) {
            wchar_t buf[64];
            swprintf_s(buf, L"file_%06d_%c.txt", static_cast<int>(rng() % 1000000), L'a' + static_cast<int>(rng() % 26));
            names.push_back(buf);
        }
        std::sort(names.begin(), names.end());
        names.erase(std::unique(names.begin(), names.end()), names.end());
        auto rows = MakeRows(names, rng);
        std::shuffle(rows.begin(), rows.end(), rng);
        auto reference = rows;
        const auto t0 = std::chrono::steady_clock::now();
        std::sort(reference.begin(), reference.end(), [](const fs::DirEntry& a, const fs::DirEntry& b) {
            return app::EntryLess(a, b, ui::SortColumn::Name, ui::SortDirection::Asc);
        });
        const auto t1 = std::chrono::steady_clock::now();
        app::SortEntries(rows, ui::SortColumn::Name, ui::SortDirection::Asc);
        const auto t2 = std::chrono::steady_clock::now();
        bool same = true;
        for (size_t i = 0; same && i < rows.size(); ++i) same = rows[i].name == reference[i].name;
        const auto ms = [](auto d) { return std::chrono::duration<double, std::milli>(d).count(); };
        std::printf("  %zu rows by name: std::sort+EntryLess %.0f ms, SortEntries %.0f ms\n", rows.size(), ms(t1 - t0), ms(t2 - t1));
        Check(same, "100k rows: SortEntries matches std::sort with EntryLess");
    }
    {
        // A folder's own folders-first mode for the sorts on this thread.
        const auto entry = [](const wchar_t* name, bool dir) {
            fs::DirEntry e;
            e.name = name;
            e.is_dir = dir;
            return e;
        };
        const std::vector<fs::DirEntry> rows = {entry(L"c", false), entry(L"b", true), entry(L"a", false)};
        const auto order = [&](ui::SortDirection dir) {
            auto sorted = rows;
            app::SortEntries(sorted, ui::SortColumn::Name, dir);
            std::wstring names;
            for (const auto& e : sorted) names += e.name;
            return names;
        };
        app::SetFolderSortMode(app::FolderSortMode::FoldersFirst);
        Check(order(ui::SortDirection::Asc) == L"bac" && order(ui::SortDirection::Desc) == L"bca",
              "without a scope the settings default sorts");
        bool scoped = false, nested = false, restored = false, other_thread = false;
        {
            const app::ScopedFolderSortMode mixed_mode(app::FolderSortMode::Mixed);
            scoped = app::CurrentFolderSortMode() == app::FolderSortMode::Mixed &&
                     app::DefaultFolderSortMode() == app::FolderSortMode::FoldersFirst &&
                     order(ui::SortDirection::Asc) == L"abc" &&
                     app::EntryLess(rows[2], rows[1], ui::SortColumn::Name, ui::SortDirection::Asc);
            {
                const app::ScopedFolderSortMode bottom_mode(app::FolderSortMode::FoldersLast);
                nested = order(ui::SortDirection::Asc) == L"acb" && order(ui::SortDirection::Desc) == L"cab";
            }
            restored = app::CurrentFolderSortMode() == app::FolderSortMode::Mixed;
            std::thread([&] {
                other_thread = app::CurrentFolderSortMode() == app::FolderSortMode::FoldersFirst;
            }).join();
        }
        Check(scoped, "a folder's own mode sorts its rows; the settings default is unchanged");
        Check(nested && restored, "scopes nest and restore the outer mode");
        Check(other_thread, "a scope stays on its own thread");
        Check(app::CurrentFolderSortMode() == app::FolderSortMode::FoldersFirst && order(ui::SortDirection::Asc) == L"bac",
              "leaving the scope returns to the settings default");
    }
    {
        std::vector<fs::DirEntry> source;
        for (size_t i = 0; i < 100000; ++i) {
            fs::DirEntry entry;
            entry.name = L"文件" + std::to_wstring(100000 - i) + L".txt";
            entry.is_dir = i % 7 == 0;
            entry.size = i % 31;
            source.push_back(std::move(entry));
        }
        bool same = true;
        for (auto mode : {app::FolderSortMode::FoldersFirst, app::FolderSortMode::Mixed,
                          app::FolderSortMode::FoldersLast}) {
            app::ScopedFolderSortMode scope(mode);
            for (auto col : {ui::SortColumn::Name, ui::SortColumn::Size}) {
                for (auto dir : {ui::SortDirection::Asc, ui::SortDirection::Desc}) {
                    auto full = source, first = source;
                    size_t full_ticks = 0, first_ticks = 0;
                    app::SortEntries(full, col, dir, [&] { ++full_ticks; });
                    app::SortFirstEntries(first, col, dir, 48, [&] { ++first_ticks; });
                    same = same && first.size() == 48;
                    for (size_t i = 0; i < first.size(); ++i) same = same && first[i].name == full[i].name;
                    std::printf("  top48/100k work ticks: full=%zu partial=%zu\n", full_ticks, first_ticks);
                }
            }
        }
        Check(same, "top-K matches full sort: CJK natural names, size ties, directions, folder modes");
    }
    std::printf("== entry sort key tests: %s ==\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
