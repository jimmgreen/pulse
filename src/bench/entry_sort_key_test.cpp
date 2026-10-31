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
    const app::FolderSortMode modes[] = {app::FolderSortMode::FoldersFirst, app::FolderSortMode::FollowDirection,
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
} // namespace

int main(int argc, char** argv) {
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
            for (const auto mode : {app::FolderSortMode::FoldersFirst, app::FolderSortMode::FollowDirection, app::FolderSortMode::Mixed}) {
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
                const app::ScopedFolderSortMode follow_mode(app::FolderSortMode::FollowDirection);
                nested = order(ui::SortDirection::Asc) == L"bac" && order(ui::SortDirection::Desc) == L"cab";
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
    std::printf("== entry sort key tests: %s ==\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
