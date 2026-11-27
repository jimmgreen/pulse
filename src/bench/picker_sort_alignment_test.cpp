#include "../app/picker_list_order.h"
#include "../app/app_prefs.h"

#include <shlobj.h>
#include <algorithm>
#include <filesystem>
#include <iostream>
#include <numeric>

namespace {
using namespace pulse;
int checks = 0;
int failures = 0;

void Check(bool passed, const std::string& name) {
    ++checks;
    if (!passed) ++failures;
    std::cout << (passed ? "[PASS] " : "[FAIL] ") << name << '\n';
}

FILETIME FileTime(uint64_t ticks) {
    return {static_cast<DWORD>(ticks), static_cast<DWORD>(ticks >> 32)};
}

std::vector<fs::DirEntry> RealRows(const std::filesystem::path& root) {
    struct Sample { const wchar_t* name; bool folder; DWORD size; int day; };
    const Sample samples[] = {
        {L"z-folder", true, 0, 3}, {L"a-folder", true, 0, 9}, {L"10-folder", true, 0, 1},
        {L"z10.txt", false, 17, 7}, {L"z2.TXT", false, 16000, 2}, {L"a.png", false, 20000, 8},
        {L"Q.pdf", false, 0, 4}, {L"2.log", false, 31, 5}, {L"中.txt", false, 23, 6},
        {L".hidden", false, 17, 1}, {L"a10", false, 10, 3}, {L"a2", false, 2, 9}
    };
    std::filesystem::create_directories(root);
    const auto clock = app::MakeGroupClock();
    constexpr uint64_t day = 24ull * 60 * 60 * 10000000;
    for (const auto& sample : samples) {
        const auto path = root / sample.name;
        if (sample.folder && !CreateDirectoryW(path.c_str(), nullptr)) return {};
        HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr, sample.folder ? OPEN_EXISTING : CREATE_NEW,
            sample.folder ? FILE_FLAG_BACKUP_SEMANTICS : FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return {};
        bool okay = true;
        if (!sample.folder) {
            std::vector<char> bytes(sample.size, 'x');
            DWORD written = 0;
            okay = WriteFile(file, bytes.data(), sample.size, &written, nullptr) && written == sample.size;
        }
        const FILETIME created = FileTime(clock.today - sample.day * day + day / 2);
        const FILETIME modified = FileTime(clock.today - (10 - sample.day) * day + day / 2);
        const FILETIME accessed = FileTime(clock.today - ((sample.day * 3) % 10) * day + day / 2);
        okay = SetFileTime(file, &created, &accessed, &modified) && okay;
        CloseHandle(file);
        if (!okay) return {};
    }
    std::vector<fs::DirEntry> rows;
    WIN32_FIND_DATAW data{};
    HANDLE find = FindFirstFileW((root / L"*").c_str(), &data);
    if (find == INVALID_HANDLE_VALUE) return {};
    do {
        if (wcscmp(data.cFileName, L".") == 0 || wcscmp(data.cFileName, L"..") == 0) continue;
        fs::DirEntry row;
        row.name = data.cFileName;
        row.is_dir = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        row.size = (static_cast<uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
        row.ctime = data.ftCreationTime;
        row.mtime = data.ftLastWriteTime;
        row.atime = data.ftLastAccessTime;
        row.full_path = (root / data.cFileName).wstring();
        rows.push_back(std::move(row));
    } while (FindNextFileW(find, &data));
    FindClose(find);
    // Enumeration order must not hide an accidental missing sort.
    std::reverse(rows.begin(), rows.end());
    return rows;
}

bool GroupsMatch(const std::vector<fs::DirEntry>& sorted, const ui::PickerListOrder& order, const std::wstring& folder) {
    const auto by = app::GroupByFromInt(order.group_by);
    const auto column = static_cast<ui::SortColumn>(order.column);
    auto clock = app::MakeGroupClock(column);
    clock.folders = app::FolderSortModeFromInt(order.folder_mode);
    if (by == app::GroupBy::Tag) clock.tags = app::TagGroupsForFolder(folder, order.tags);
    std::set<std::wstring> collapsed;
    if (!sorted.empty()) collapsed.insert(app::GroupKey(sorted.front(), by, clock));
    const auto groups = app::BuildPickerListGroups(sorted, order, collapsed, folder);
    if (by == app::GroupBy::None || sorted.empty()) return !groups;
    if (!groups || groups->empty()) return false;
    size_t next = 0;
    for (const auto& group : *groups) {
        if (group.first != static_cast<int>(next) || group.sample != group.first || group.count <= 0 ||
            group.collapsed != collapsed.contains(group.key)) return false;
        uint64_t bytes = 0;
        for (int i = 0; i < group.count; ++i) {
            if (next >= sorted.size()) return false;
            const auto& row = sorted[next++];
            if (app::GroupKey(row, by, clock) != group.key || app::GroupRank(row, by, clock) != group.rank) return false;
            if (!row.is_dir) bytes += row.size;
        }
        if (bytes != group.bytes) return false;
    }
    return next == sorted.size();
}

void CompareOrders(const std::vector<fs::DirEntry>& rows, const std::wstring& folder, const char* kind) {
    using ui::SortColumn;
    for (const auto column : {SortColumn::Name, SortColumn::Mtime, SortColumn::Type, SortColumn::Size,
                             SortColumn::Path, SortColumn::Created, SortColumn::Accessed}) {
        for (int direction = 0; direction <= 1; ++direction) {
            for (int mode = 0; mode <= 2; ++mode) {
                for (int group = 0; group <= 6; ++group) {
                    ui::PickerListOrder order{static_cast<int>(column), direction != 0, group, mode};
                    order.tags = app::CurrentTagCatalog();
                    auto expected = rows;
                    {
                        app::ScopedFolderSortMode folders(app::FolderSortModeFromInt(mode));
                        app::ScopedEntryGrouping grouping(group, folder);
                        app::SortEntries(expected, column, static_cast<ui::SortDirection>(direction));
                    }
                    std::vector<ui::PickerEntry> entries(rows.size());
                    for (size_t i = 0; i < entries.size(); ++i) entries[i].source = i;
                    app::OrderPickerEntries(entries, rows, folder, order);
                    bool equal = entries.size() == expected.size();
                    std::vector<fs::DirEntry> actual;
                    for (size_t i = 0; i < entries.size(); ++i) {
                        equal = equal && entries[i].source < rows.size();
                        if (entries[i].source >= rows.size()) break;
                        actual.push_back(rows[entries[i].source]);
                        equal = equal && actual.back().name == expected[i].name;
                    }
                    const std::string label = std::string(kind) + " column=" + std::to_string(order.column) +
                        " desc=" + std::to_string(direction) + " folder=" + std::to_string(mode) + " group=" + std::to_string(group);
                    Check(equal && GroupsMatch(actual, order, folder), label);
                }
            }
        }
    }
}

void Preferences() {
    using namespace app;
    AppPrefs prefs;
    const std::wstring folder = L"C:\\picker-sort-fixture";
    auto order = PickerListOrderPrefsFromPrefs(prefs)->Resolve(folder);
    Check(order == ui::PickerListOrder{}, "empty main prefs match picker defaults");
    Check(ParsePickerListOrderPrefs(L"{}")->Resolve(folder) == order, "empty JSON matches main prefs");
    prefs.folder_sorts.ApplyToAll({ui::SortColumn::Created, ui::SortDirection::Desc});
    prefs.folder_sort_mode = 1;
    prefs.folder_groups.ApplyToAll(GroupBy::Type);
    const ui::PickerListOrder global{static_cast<int>(ui::SortColumn::Created), true, 3, 1};
    Check(PickerListOrderPrefsFromPrefs(prefs)->Resolve(folder) == global, "global sort direction placement and grouping apply");
    prefs.folder_sorts.Set(folder, {ui::SortColumn::Accessed, ui::SortDirection::Asc});
    prefs.folder_sorts.SetFolderMode(folder, 2);
    prefs.folder_groups.Set(folder, GroupBy::None);
    const ui::PickerListOrder local{static_cast<int>(ui::SortColumn::Accessed), false, 0, 2};
    const auto snapshot = PickerListOrderPrefsFromPrefs(prefs);
    Check(snapshot->Resolve(L"\\\\?\\C:\\PICKER-sort-fixture\\") == local, "per-folder override wins with normalized path");
    Check(snapshot->Resolve(folder + L"\\child") == global, "parent override does not leak into child");
    std::wstring json = L"{\"folder_sort_mode\":1";
    prefs.folder_sorts.AppendJson(json);
    prefs.folder_groups.AppendJson(json);
    json += L"}";
    const auto parsed = ParsePickerListOrderPrefs(json);
    Check(parsed->Resolve(folder) == local && parsed->Resolve(folder + L"\\child") == global,
        "standalone JSON and in-process prefs preserve identical precedence");
    PWSTR raw = nullptr;
    const HRESULT hr = SHGetKnownFolderPath(FOLDERID_Downloads, 0, nullptr, &raw);
    Check(SUCCEEDED(hr) && raw, "Downloads known-folder path resolves");
    if (SUCCEEDED(hr) && raw) {
        const std::wstring downloads(raw);
        PickerListOrderPrefs download_prefs;
        Check(download_prefs.Resolve(downloads).group_by == static_cast<int>(GroupBy::Date), "Downloads defaults to date group");
        download_prefs.groups.ApplyToAll(GroupBy::Size);
        Check(download_prefs.Resolve(downloads).group_by == static_cast<int>(GroupBy::Size), "global group overrides Downloads default");
        download_prefs.groups.Set(downloads, GroupBy::None);
        Check(download_prefs.Resolve(downloads).group_by == 0, "explicit None overrides Downloads and global group");
    }
    CoTaskMemFree(raw);
    Check(ParsePickerListOrderPrefs(L"{\"folder_sort_mode\":99}")->Resolve(folder).folder_mode == 0,
        "invalid placement uses main default");
    Check(!BuildPickerListGroups({}, local, {}), "empty snapshot has no groups");
    Check(PickerSupportedGroup(static_cast<int>(GroupBy::Tag)) == 5 &&
        PickerSupportedGroup(static_cast<int>(GroupBy::Location)) == 6 &&
        PickerSupportedGroup(static_cast<int>(GroupBy::MediaMonth)) == 0,
        "tag and location groups supported while media remains explicit fallback");
}
void TagContext(const std::vector<fs::DirEntry>& rows, const std::wstring& folder) {
    using namespace app;
    auto catalog = std::make_shared<TagCatalogSnapshot>();
    catalog->revision = 17;
    catalog->tags = {{L"工作", 0x3366aa}, {L"重要", 0xdd8844}};
    for (size_t i = 0; i < rows.size(); ++i) {
        if (i % 3 == 2) continue;
        std::wstring path = rows[i].full_path;
        for (auto& c : path) c = static_cast<wchar_t>(towlower(c));
        catalog->paths.emplace_back(path, static_cast<int>(i % 2));
    }
    PublishTagCatalog(catalog);
    AppPrefs prefs;
    auto extended = std::make_shared<TagCatalogSnapshot>(*catalog);
    for (auto& [path, rank] : extended->paths) { (void)rank; path = L"\\\\?\\" + path; }
    const auto ordinary_map = TagGroupsForFolder(folder, catalog);
    const auto extended_map = TagGroupsForFolder(folder, extended);
    Check(ordinary_map->rank_by_name == extended_map->rank_by_name &&
        TagGroupsForFolder(L"\\\\?\\" + folder, catalog)->rank_by_name == ordinary_map->rank_by_name,
        "DOS and extended catalog paths match both main and picker folder spelling");
    prefs.folder_groups.Set(folder, GroupBy::Tag);
    auto frozen = PickerListOrderPrefsFromPrefs(prefs)->Resolve(folder);
    Check(frozen.group_by == 5 && frozen.tags == catalog, "picker inherits actual immutable primary-tag catalog");
    std::vector<ui::PickerEntry> entries(rows.size());
    for (size_t i = 0; i < rows.size(); ++i) entries[i].source = i;
    OrderPickerEntries(entries, rows, folder, frozen);
    std::vector<fs::DirEntry> ordered;
    for (const auto& entry : entries) ordered.push_back(rows[entry.source]);
    auto newer = std::make_shared<TagCatalogSnapshot>();
    newer->revision = 18;
    newer->tags = {{L"later unrelated label", 0}};
    PublishTagCatalog(newer);
    const auto groups = BuildPickerListGroups(ordered, frozen, {}, folder);
    bool labels = groups && groups->size() >= 3;
    if (groups) for (const auto& group : *groups) {
        if (group.rank >= 0 && group.rank < 2) {
            const auto& expected = catalog->tags[static_cast<size_t>(group.rank)];
            labels = labels && group.label == expected.name && group.color_rgb == expected.rgb;
        }
    }
    Check(labels && GroupsMatch(ordered, frozen, folder), "sort headers labels and colors use same frozen catalog after publisher changes");
    const auto collapsed = groups ? std::set<std::wstring>{groups->back().key} : std::set<std::wstring>{};
    const auto folded = BuildPickerListGroups(ordered, frozen, collapsed, folder);
    Check(folded && folded->back().collapsed, "tag group collapse retains shared stable key");
    PublishTagCatalog({});
    Check(PickerListOrderPrefsFromPrefs(prefs)->Resolve(folder).group_by == 0, "missing catalog does not label every file untagged");
    PublishTagCatalog(catalog);
}

}

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) { std::cerr << "usage: pulse_picker_sort_alignment_test UNIQUE_FIXTURE_DIRECTORY\n"; return 2; }
    const std::filesystem::path root = std::filesystem::absolute(argv[1]);
    if (std::filesystem::exists(root)) { std::cerr << "fixture directory must not exist\n"; return 2; }
    Preferences();
    const auto rows = RealRows(root);
    Check(rows.size() == 12, "12 real entries created, timestamps set and filesystem enumeration read");
    if (rows.size() == 12) {
        TagContext(rows, root.wstring());
        CompareOrders(rows, root.wstring(), "files-and-folders");
        std::vector<fs::DirEntry> folders;
        for (const auto& row : rows) if (row.is_dir) folders.push_back(row);
        CompareOrders(folders, root.wstring(), "folders-only");
        auto invalid = std::vector<ui::PickerEntry>(3);
        invalid[0].source = rows.size(); invalid[1].source = 0; invalid[2].source = rows.size() + 1;
        app::OrderPickerEntries(invalid, rows, root.wstring(), {});
        Check(invalid[0].source == 0, "invalid source indexes remain after valid rows");
        auto noncontiguous = std::vector<fs::DirEntry>{rows.front(), rows.back(), rows.front()};
        noncontiguous[0].name = L"a.txt"; noncontiguous[0].is_dir = false;
        noncontiguous[1].name = L"b.pdf"; noncontiguous[1].is_dir = false;
        noncontiguous[2].name = L"c.txt"; noncontiguous[2].is_dir = false;
        Check(!app::BuildPickerListGroups(noncontiguous, {2, false, 3, 2}, {}), "noncontiguous type groups draw flat");
    }
    std::cout << "RESULT=" << (failures ? "FAIL" : "PASS") << " checks=" << checks << " failures=" << failures << '\n';
    return failures ? 1 : 0;
}
