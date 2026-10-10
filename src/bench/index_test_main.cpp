#include "../index/index_config.h"
#include "../index/index_executable.h"
#include "../index/index_query.h"
#include "../index/network_index.h"
#include "../index/network_crawl_schedule.h"
#include "../index/index_shard.h"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <mutex>
#include <vector>

namespace pulse::index {
struct NetworkIndexTestAccess {
    static bool ReadConfig(NetworkIndex& index, const std::wstring& path, std::vector<std::wstring>& roots) {
        return index.ReadConfig(path, roots);
    }
};
}

using namespace pulse::index;

namespace {

int failures = 0;

void Check(bool condition, const wchar_t* name) {
    std::wcout << (condition ? L"[PASS] " : L"[FAIL] ") << name << L'\n';
    if (!condition) ++failures;
}

} // namespace

void RunNetworkIntegration(const std::wstring& root) {
    const std::wstring config_path = NetworkConfigPath();
    const bool config_existed = GetFileAttributesW(config_path.c_str()) != INVALID_FILE_ATTRIBUTES;
    std::vector<std::wstring> previous;
    std::wstring error;
    if (!LoadNetworkRoots(previous, &error) || !previous.empty()) {
        Check(false, L"network integration requires an unused network-root config");
        return;
    }

    NetworkIndex network;
    network.Start(nullptr, 0, 0);
    const bool added = network.AddRoot(root, &error);
    Check(added, L"network integration adds UNC root");
    bool ready = false;
    for (int i = 0; added && i < 300; ++i) {
        const auto roots = network.Roots();
        if (!roots.empty() && roots[0].online && !roots[0].building) {
            ready = true;
            break;
        }
        Sleep(100);
    }
    Check(ready, L"network integration recursively builds shard");

    bool found = false;
    if (ready) {
        Query query;
        query.needle = L"network_index.cpp";
        query.limit = 16;
        network.SearchAsync(query, 1);
        SearchResult result;
        for (int i = 0; i < 100 && !network.TakeResult(1, result); ++i) Sleep(50);
        found = std::any_of(result.hits.begin(), result.hits.end(), [](const Hit& hit) {
            return hit.name == L"network_index.cpp";
        });
    }
    Check(found, L"network integration searches UNC file");

    bool watched = false;
    const std::wstring watch_name = L"pulse-network-watch-test.tmp";
    const std::wstring watch_path = root + L"\\" + watch_name;
    bool watcher_ready = false;
    for (int i = 0; i < 50; ++i) {
        const auto roots = network.Roots();
        if (!roots.empty() && roots[0].watching) {
            watcher_ready = true;
            break;
        }
        Sleep(100);
    }
    Check(watcher_ready, L"network integration starts SMB change watcher");
    HANDLE fixture = CreateFileW(watch_path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                 FILE_ATTRIBUTE_TEMPORARY, nullptr);
    if (fixture != INVALID_HANDLE_VALUE) {
        const char bytes[] = "network change notification";
        DWORD written = 0;
        WriteFile(fixture, bytes, static_cast<DWORD>(sizeof(bytes)), &written, nullptr);
        CloseHandle(fixture);
        for (uint32_t attempt = 0; attempt < 100 && !watched; ++attempt) {
            Query query;
            query.needle = watch_name;
            query.limit = 4;
            const uint32_t id = 10 + attempt;
            network.SearchAsync(query, id);
            SearchResult result;
            for (int poll = 0; poll < 5 && !network.TakeResult(id, result); ++poll) Sleep(20);
            watched = std::any_of(result.hits.begin(), result.hits.end(), [&](const Hit& hit) {
                return hit.name == watch_name;
            });
            if (!watched) Sleep(100);
        }
        DeleteFileW(watch_path.c_str());
    }
    Check(watched, L"network integration tracks SMB directory change");

    // Polls a name search until `accept` holds for the matching hit paths.
    uint32_t next_id = 1000;
    auto wait_for_search = [&](const std::wstring& name, auto accept) {
        for (int attempt = 0; attempt < 100; ++attempt) {
            Query query;
            query.needle = name;
            query.limit = 8;
            const uint32_t id = next_id++;
            network.SearchAsync(query, id);
            SearchResult result;
            bool answered = false;
            for (int poll = 0; poll < 25 && !(answered = network.TakeResult(id, result)); ++poll) Sleep(20);
            if (answered) {
                std::vector<std::wstring> paths;
                for (const auto& hit : result.hits)
                    if (hit.name == name) paths.push_back(hit.path);
                if (accept(paths)) return true;
            }
            Sleep(100);
        }
        return false;
    };
    auto only_path = [](const std::wstring& expected) {
        return [expected](const std::vector<std::wstring>& paths) {
            return paths.size() == 1 && _wcsicmp(paths[0].c_str(), expected.c_str()) == 0;
        };
    };
    auto no_path = [](const std::vector<std::wstring>& paths) { return paths.empty(); };
    if (watched) {
        Check(wait_for_search(watch_name, no_path), L"network integration drops deleted SMB file");
    }

    // A directory created after the crawl, then renamed and removed: search
    // must follow its contents without waiting for a re-crawl.
    const std::wstring dir_before = root + L"\\pulse-network-dir-test";
    const std::wstring dir_after = root + L"\\pulse-network-dir-renamed";
    const std::wstring inner_name = L"pulse-network-inner-probe.tmp";
    bool dir_tracked = false;
    bool rename_tracked = false;
    bool removal_tracked = false;
    if (watched && CreateDirectoryW(dir_before.c_str(), nullptr)) {
        HANDLE inner = CreateFileW((dir_before + L"\\" + inner_name).c_str(), GENERIC_WRITE, 0,
                                   nullptr, CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY, nullptr);
        if (inner != INVALID_HANDLE_VALUE) CloseHandle(inner);
        dir_tracked = inner != INVALID_HANDLE_VALUE &&
            wait_for_search(inner_name, only_path(dir_before + L"\\" + inner_name));
        if (dir_tracked && MoveFileW(dir_before.c_str(), dir_after.c_str())) {
            rename_tracked = wait_for_search(inner_name, only_path(dir_after + L"\\" + inner_name));
            DeleteFileW((dir_after + L"\\" + inner_name).c_str());
            if (RemoveDirectoryW(dir_after.c_str()))
                removal_tracked = wait_for_search(inner_name, no_path);
        }
    }
    DeleteFileW((dir_before + L"\\" + inner_name).c_str());
    RemoveDirectoryW(dir_before.c_str());
    DeleteFileW((dir_after + L"\\" + inner_name).c_str());
    RemoveDirectoryW(dir_after.c_str());
    Check(dir_tracked, L"network integration indexes new SMB directory contents");
    Check(rename_tracked, L"network integration follows renamed SMB directory");
    Check(removal_tracked, L"network integration drops removed SMB directory contents");
    Check(network.RemoveRoot(root, &error), L"network integration removes UNC root");
    network.Stop();
    if (!config_existed) DeleteFileW(config_path.c_str());
}

// #74: scoped name search in a network folder no ready root covers.
void RunLiveNetworkTests() {
    Check(IsNetworkFolderPath(L"\\\\192.168.1.5\\Video") &&
          IsNetworkFolderPath(L"\\\\?\\UNC\\srv\\share\\a") &&
          !IsNetworkFolderPath(L"C:\\Windows") && !IsNetworkFolderPath(L"\\\\?\\C:\\Windows") &&
          !IsNetworkFolderPath(L""),
          L"live network: UNC paths are network folders, local paths are not");

    NetworkRootInfo ready;
    ready.path = L"\\\\srv\\share";
    ready.online = true;
    ready.indexed_items = 10;
    NetworkRootInfo crawling = ready;
    crawling.path = L"\\\\srv\\new";
    crawling.building = true;
    const std::vector<NetworkRootInfo> roots{ready, crawling};
    Check(NetworkRootsCover(roots, L"\\\\SRV\\share\\Movies\\", true) &&
          !NetworkRootsCover(roots, L"\\\\srv\\shared", true) &&
          !NetworkRootsCover(roots, L"\\\\srv\\other", false),
          L"live network: a ready root covers its subtree only");
    Check(!NetworkRootsCover(roots, L"\\\\srv\\new\\a", true) &&
          NetworkRootsCover(roots, L"\\\\srv\\new\\a", false),
          L"live network: a root on its first crawl is configured but not ready");

    wchar_t temp[MAX_PATH]{};
    GetTempPathW(MAX_PATH, temp);
    const std::filesystem::path dir = std::filesystem::path(temp) /
        (L"pulse_live_net_" + std::to_wstring(GetCurrentProcessId()));
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir / L"sub" / L"LanChat Notes", ec);
    std::filesystem::create_directories(dir / L"sub" / L"deep", ec);
    for (const auto& file : {dir / L"lanchat.exe", dir / L"lanchat-aarch64.apk", dir / L"notes.txt",
                             dir / L"sub" / L"deep" / L"lanchat.log"})
        std::ofstream(file) << "x";

    auto walk = [](const std::wstring& folder, const std::wstring& needle, bool folders_only,
                   bool cancel = false) {
        LiveNetworkMatches matches;
        std::mutex mutex;
        LiveNetworkWalk(folder, needle, folders_only, matches, mutex,
                        [cancel] { return cancel; }, [] {});
        return matches;
    };
    const auto all = walk(dir.wstring(), L"lanchat", false);
    Check(all.complete && all.error == 0 && all.total == 4 && all.hits.size() == 4,
          L"live network: walk finds matches in every subfolder, case-insensitive");
    const auto folders = walk(dir.wstring() + L"\\", L"lanchat", true);
    Check(folders.total == 1 && folders.hits.size() == 1 && folders.hits[0].is_dir &&
          folders.hits[0].name == L"LanChat Notes",
          L"live network: folders-only keeps the matching folder");
    const auto notes = walk(dir.wstring(), L"notes", false);
    Check(notes.total == 2, L"live network: a second needle matches file and folder names");

    Query page;
    page.needle = L"lanchat";
    page.rank = false;
    page.sort = ResultSort::Name;
    page.offset = 1;
    page.limit = 2;
    const auto selected = SelectLiveNetworkHits(page, all);
    Check(selected.total == 4 && selected.hits.size() == 2 &&
          selected.hits[0].name == L"lanchat-aarch64.apk" && selected.hits[1].name == L"lanchat.exe",
          L"live network: pages are ordered by name with offset and limit");
    page.sort_desc = true;
    page.offset = 0;
    page.limit = 1;
    const auto desc = SelectLiveNetworkHits(page, all);
    Check(desc.hits.size() == 1 && desc.hits[0].name == L"lanchat.log",
          L"live network: descending name order");

    const auto cancelled = walk(dir.wstring(), L"lanchat", false, true);
    Check(!cancelled.complete && cancelled.total == 0, L"live network: a cancelled walk stops");
    const auto missing = walk((dir / L"missing").wstring(), L"lanchat", false);
    Check(missing.complete && missing.error != 0 && missing.total == 0,
          L"live network: an unreadable scope reports its error");

    // Same tree over the loopback admin share when this account can reach it.
    const std::wstring local = dir.wstring();
    if (local.size() > 2 && local[1] == L':') {
        const std::wstring unc = L"\\\\127.0.0.1\\" + std::wstring(1, local[0]) + L"$" + local.substr(2);
        if (GetFileAttributesW(unc.c_str()) != INVALID_FILE_ATTRIBUTES) {
            const auto smb = walk(unc, L"lanchat", false);
            Check(IsNetworkFolderPath(unc) && smb.complete && smb.total == 4 &&
                  smb.hits[0].path.rfind(L"\\\\127.0.0.1\\", 0) == 0,
                  L"live network: walk over a loopback SMB share");
        } else {
            std::wcout << L"[SKIP] live network: loopback admin share not reachable\n";
        }
    }
    std::filesystem::remove_all(dir, ec);
}

int RunNetworkConfigAudit() {
    const auto root = std::filesystem::absolute(std::filesystem::path("bench_data") /
        ("network-config-audit-" + std::to_string(GetCurrentProcessId())));
    std::filesystem::create_directories(root);
    const auto path = root / "network-index.json";
    NetworkIndex index;
    std::vector<std::wstring> roots;
    Check(NetworkIndexTestAccess::ReadConfig(index, path.wstring(), roots) && roots.empty(), L"missing network configuration is empty");
    for (const std::string json : {std::string("{\"roots\":[]}"), std::string("{\"roots\":["),
            std::string("{\"roots\":42}"), std::string("{\"roots\":[1]}"),
            std::string("{\"roots\":[]} trailing"), std::string("\xff")}) {
        std::ofstream(path, std::ios::binary | std::ios::trunc) << json;
        const bool valid = json == "{\"roots\":[]}";
        Check(NetworkIndexTestAccess::ReadConfig(index, path.wstring(), roots) == valid,
            L"production config loader distinguishes malformed input from empty roots");
        if (!valid) {
            std::wstring error;
            Check(!index.ConfigError().empty(), L"startup retains explicit configuration error");
            Check(!index.AddRoot(L"\\\\fixture-server\\share", &error) && !error.empty(),
                L"failed load prevents adding a root");
            Check(!index.RemoveRoot(L"\\\\fixture-server\\share", &error), L"failed load prevents root removal");
            std::ifstream saved(path, std::ios::binary);
            const std::string bytes((std::istreambuf_iterator<char>(saved)), std::istreambuf_iterator<char>());
            Check(bytes == json, L"failed mutation preserves original configuration bytes");
        }
    }
    std::ofstream(path, std::ios::binary | std::ios::trunc) << "{\"roots\":[]}";
    HANDLE locked = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    Check(locked != INVALID_HANDLE_VALUE && !NetworkIndexTestAccess::ReadConfig(index, path.wstring(), roots),
        L"sharing-denied configuration read remains an error");
    if (locked != INVALID_HANDLE_VALUE) CloseHandle(locked);
    Check(NetworkIndexTestAccess::ReadConfig(index, path.wstring(), roots) && index.ConfigError().empty(),
        L"repaired config reload clears failure state");
    std::filesystem::remove_all(root);
    return failures ? 1 : 0;
}

// Pulse.Index.exe resolution for a pulse.exe started outside the install folder.
void RunIndexExecutableTests() {
    const std::wstring installed = L"C:\\Program Files\\Pulse\\Pulse.Index.exe";
    Check(IndexExecutableFromServiceCommand(L"\"C:\\Program Files\\Pulse\\Pulse.Index.exe\" --service") == installed,
          L"service command: quoted image path");
    Check(IndexExecutableFromServiceCommand(L"C:\\Program Files\\Pulse\\pulse.index.exe --service") ==
              L"C:\\Program Files\\Pulse\\pulse.index.exe",
          L"service command: unquoted path with spaces");
    Check(IndexExecutableFromServiceCommand(L"\"C:\\Tools\\Other.exe\" --run C:\\x\\Pulse.Index.exe").empty() &&
              IndexExecutableFromServiceCommand(L"\"C:\\Pulse\\NotPulse.Index.exe\" --service").empty() &&
              IndexExecutableFromServiceCommand(L"Pulse.Index.exe --service").empty() &&
              IndexExecutableFromServiceCommand(L"").empty(),
          L"service command: other images rejected");
    const std::wstring downloads = L"D:\\Users\\qwer5\\Downloads\\Pulse.Index.exe";
    const std::wstring service = L"\"" + installed + L"\" --service";
    auto only = [](std::initializer_list<std::wstring> files) {
        return [files = std::vector<std::wstring>(files)](const std::wstring& path) {
            return std::find(files.begin(), files.end(), path) != files.end();
        };
    };
    Check(ResolveIndexExecutable(downloads, service, only({installed})) == installed,
          L"pulse.exe outside the install folder uses the service executable");
    Check(ResolveIndexExecutable(downloads, service, only({downloads, installed})) == downloads,
          L"sibling Pulse.Index.exe stays preferred");
    Check(ResolveIndexExecutable(downloads, L"", only({})) == downloads &&
              ResolveIndexExecutable(downloads, service, only({})) == downloads,
          L"no service or missing service file keeps the sibling path for the error");
    // Live: an installed PulseIndex service resolves to an existing executable
    // without elevation (this process runs as the signed-in user).
    const std::wstring live = InstalledServiceCommand(L"PulseIndex");
    if (!live.empty()) {
        const std::wstring image = IndexExecutableFromServiceCommand(live);
        Check(!image.empty() && GetFileAttributesW(image.c_str()) != INVALID_FILE_ATTRIBUTES,
              L"installed PulseIndex service resolves to its Pulse.Index.exe");
    }
}

int wmain(int argc, wchar_t** argv) {
    if (argc == 2 && std::wstring_view(argv[1]) == L"--network-config-only") return RunNetworkConfigAudit();
    RunLiveNetworkTests();
    RunIndexExecutableTests();
    Check(NormalizeVolumeId(L"  \\\\?\\Volume{abc}\\  ") == L"\\\\?\\VOLUME{ABC}\\",
          L"volume id normalization");

    IndexConfig defaults;
    const auto volumes = EnumerateLocalVolumes(defaults);
    Check(!volumes.empty(), L"local volume enumeration");

    bool eligibility_ok = true;
    bool stable_id_ok = true;
    std::wstring first_supported;
    for (const auto& volume : volumes) {
        if (volume.kind != VolumeKind::Fixed && volume.kind != VolumeKind::Removable)
            eligibility_ok = false;
        if (volume.supported && !volume.enabled) eligibility_ok = false;
        if (!volume.supported && volume.enabled) eligibility_ok = false;
        if (volume.supported && volume.id.rfind(L"\\\\?\\VOLUME{", 0) != 0)
            stable_id_ok = false;
        if (first_supported.empty() && volume.supported) first_supported = volume.id;
    }
    Check(eligibility_ok, L"NTFS eligibility defaults");
    Check(stable_id_ok, L"stable volume GUID identity");

    if (!first_supported.empty()) {
        IndexConfig excluded;
        excluded.excluded_volume_ids.insert(NormalizeVolumeId(first_supported));
        const auto filtered = EnumerateLocalVolumes(excluded);
        bool disabled = false;
        for (const auto& volume : filtered)
            if (NormalizeVolumeId(volume.id) == NormalizeVolumeId(first_supported))
                disabled = volume.supported && !volume.enabled;
        Check(disabled, L"excluded volume is disabled");
    } else {
        Check(false, L"at least one supported NTFS volume");
    }

    Check(NormalizeNetworkRoot(L"  \\\\server/share/folder\\  ") ==
              L"\\\\server\\share\\folder",
          L"network UNC root normalization");
    Check(NormalizeNetworkRoot(L"\\\\?\\UNC\\server\\share\\folder") ==
              L"\\\\server\\share\\folder",
          L"network long UNC normalization");
    Check(NormalizeNetworkRoot(L"\\\\192.0.2.10\\示例共享盘\\") ==
          L"\\\\192.0.2.10\\示例共享盘",
          L"network Chinese UNC root normalization");
    Check(NormalizeNetworkRoot(L"C:\\local").empty(),
          L"network root rejects local path");

    {
        IndexConfig path_config;
        path_config.excluded_paths = { L"C:\\Program Files\\Pulse" };
        Check(path_config.IsPathExcluded(L"C:\\Program Files\\Pulse") &&
                  path_config.IsPathExcluded(L"c:\\program files\\pulse\\Pulse.Index.exe") &&
                  !path_config.IsPathExcluded(L"C:\\Program Files\\PulseTools"),
              L"excluded path matches subtree boundaries");
    }

    {
        wchar_t temp_dir[MAX_PATH]{};
        GetTempPathW(ARRAYSIZE(temp_dir), temp_dir);
        const std::wstring config_path = std::wstring(temp_dir) +
            L"pulse-network-index-utf8-test.json";
        const std::vector<std::wstring> expected{
            L"\\\\192.0.2.10\\示例共享盘",
            L"\\\\server\\share\\设计资料"
        };
        std::wstring config_error;
        std::vector<std::wstring> loaded;
        const bool saved = SaveNetworkRootsFile(config_path, expected, &config_error);
        const bool loaded_ok = saved && LoadNetworkRootsFile(config_path, loaded, &config_error);
        Check(loaded_ok && loaded == expected,
              L"network config UTF-8 Chinese UNC roundtrip");
        HANDLE config = CreateFileW(config_path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        bool contains_utf8 = false;
        if (config != INVALID_HANDLE_VALUE) {
            LARGE_INTEGER size{};
            if (GetFileSizeEx(config, &size) && size.QuadPart > 0 && size.QuadPart < 4096) {
                std::vector<char> bytes(static_cast<size_t>(size.QuadPart));
                DWORD read = 0;
                if (ReadFile(config, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr)) {
                    const std::string raw(bytes.data(), bytes.data() + read);
                    contains_utf8 = raw.find("\xE7\xA4\xBA\xE4\xBE\x8B\xE5\x85\xB1\xE4\xBA\xAB\xE7\x9B\x98") !=
                                    std::string::npos;
                }
            }
            CloseHandle(config);
        }
        Check(contains_utf8, L"network config stores complete UTF-8 bytes");
        DeleteFileW(config_path.c_str());
        DeleteFileW((config_path + L".tmp").c_str());
    }

    Query merged_query;
    merged_query.needle = L"report";
    merged_query.offset = 1;
    merged_query.limit = 2;
    merged_query.rank = false;
    merged_query.sort = ResultSort::Name;
    SearchResult local;
    local.total = 2;
    local.hits.push_back({L"C:\\a-report.txt", L"a-report.txt", false, 1, 10});
    local.hits.push_back({L"C:\\c-report.txt", L"c-report.txt", false, 3, 30});
    SearchResult network;
    network.total = 2;
    network.hits.push_back({L"\\\\server\\share\\b-report.txt", L"b-report.txt", false, 2, 20});
    network.hits.push_back({L"\\\\server\\share\\d-report.txt", L"d-report.txt", false, 4, 40});
    const SearchResult merged = MergeSearchResults(merged_query, std::move(local),
                                                   std::move(network));
    Check(merged.total == 4 && merged.hits.size() == 2 &&
              merged.hits[0].name == L"b-report.txt" &&
              merged.hits[1].name == L"c-report.txt",
          L"local and network result merge pagination");

    {
        using namespace pulse::index;
        Check(QueryPrimaryNameLen(ParseQuery(L"a")) == 1, L"query: single-char needle length");
        Check(QueryIsSimpleName(ParseQuery(L"pulse")), L"query: simple name token");
        Check(!QueryIsSimpleName(ParseQuery(L"ext:pdf")), L"query: ext filter is not simple name");
        const auto content_q = ParseQuery(L"report content:\u53d1\u7968");
        Check(QueryHasContent(content_q) && QueryHasNameFilter(content_q) &&
                  content_q.content.needles.size() == 1,
              L"query: content term is split from filename");
        Check(FilenameQueryText(L"report content:\u53d1\u7968") == L"report",
              L"query: filename text strips content tokens");
        const auto kind_q = ParseQuery(L"\u7c7b\u578b:\u6587\u6863");
        Check(QueryHasExtFilter(kind_q) && !kind_q.groups.empty() &&
                  !kind_q.groups[0][0].exts.empty(),
              L"query: Chinese type:document expands to extensions");
        const auto colon_q = ParseQuery(L"\u5185\u5bb9\uff1aTODO");
        Check(QueryHasContent(colon_q) && !colon_q.content.needles.empty(),
              L"query: fullwidth colon content alias");
        const auto phrase_q = ParseQuery(L"content:\"hello world\"");
        Check(phrase_q.content.mode == ContentMatchMode::Phrase &&
                  phrase_q.content.needles.size() == 1,
              L"query: quoted content is a phrase");
        const auto path_q = ParseQuery(L"content:foo path:C:\\Users\\TestUser");
        Check(path_q.path_prefix.size() >= 3 && QueryHasContent(path_q) &&
                  FilenameQueryText(L"content:foo path:C:\\Users\\TestUser").find(L"path:") ==
                      std::wstring::npos,
              L"query: absolute path: becomes path_prefix");
        const auto long_path_q = ParseQuery(L"path:\\\\?\\C:\\Users\\TestUser\\Desktop");
        Check(long_path_q.path_prefix.find(L"\\\\?\\") == std::wstring::npos &&
                  long_path_q.path_prefix.find(L"C:\\Users\\TestUser\\Desktop") != std::wstring::npos,
              L"query: \\\\?\\ path prefix is stripped");
        const auto two_path_q = ParseQuery(L"path:C:\\a path:C:\\b");
        bool saw_second_path = false;
        for (const auto& group : two_path_q.groups) {
            for (const auto& term : group) {
                if (term.name_in_path && term.name.find(L"c:\\b") != std::wstring::npos)
                    saw_second_path = true;
            }
        }
        Check(two_path_q.path_prefix.find(L"C:\\a") != std::wstring::npos && saw_second_path,
              L"query: extra absolute path: is kept as a path filter");
        const auto not_path_q = ParseQuery(L"!path:C:\\skip");
        bool saw_not_path = false;
        for (const auto& group : not_path_q.groups) {
            for (const auto& term : group) {
                if (term.name_in_path && term.name_not) saw_not_path = true;
            }
        }
        Check(not_path_q.path_prefix.empty() && saw_not_path,
              L"query: negated absolute path: is kept as an exclusion");
        SYSTEMTIME now{};
        GetLocalTime(&now);
        wchar_t month_tok[32]{};
        swprintf_s(month_tok, L"dm:%04u-%02u", now.wYear, now.wMonth);
        const auto month_q = ParseQuery(L"dm:thismonth");
        const auto ymd_q = ParseQuery(month_tok);
        Check(!month_q.groups.empty() && !month_q.groups[0].empty() &&
                  !ymd_q.groups.empty() && !ymd_q.groups[0].empty() &&
                  month_q.groups[0][0].date_lo == ymd_q.groups[0][0].date_lo &&
                  month_q.groups[0][0].date_hi == ymd_q.groups[0][0].date_hi,
              L"query: thismonth matches the calendar month");
        const auto quoted_wild = ParseQuery(L"\"annual report*\"");
        Check(!quoted_wild.groups.empty() && !quoted_wild.groups[0].empty() &&
                  quoted_wild.groups[0][0].name_how == NameHow::Wildcard,
              L"query: quoted wildcard is a wildcard not an exact name");
    }
    Check(QueryCanNarrow(L"p", L"pu") && QueryCanNarrow(L"pu", L"pul"),
          L"query: incremental typing can narrow");

    const auto shard = MakeShardPaths(L"C:\\ProgramData\\Pulse\\Index\\Volumes",
                                      L"\\\\?\\VOLUME{TEST}");
    Check(shard.directory.find(L"\\") != std::wstring::npos &&
              shard.base_a != shard.base_b && shard.wal_a != shard.wal_b,
          L"shard paths are stable and slot-separated");
    wchar_t temp_dir[MAX_PATH]{};
    GetTempPathW(ARRAYSIZE(temp_dir), temp_dir);
    const std::wstring manifest_path = std::wstring(temp_dir) + L"pulse-shard-manifest-test.json";
    ShardManifest manifest;
    manifest.generation = 7;
    manifest.active_built = 1234;
    manifest.active_wal_bytes = 56;
    manifest.active_slot = 1;
    manifest.active_bytes = 0x123456789abcdef0ull;
    manifest.active_crc64 = 0xfedcba9876543210ull;
    manifest.source_id = L"\\\\?\\VOLUME{TEST}";
    std::wstring manifest_error;
    const bool saved = SaveShardManifest(manifest_path, manifest, &manifest_error);
    ShardManifest loaded;
    const bool loaded_ok = saved && LoadShardManifest(manifest_path, loaded, &manifest_error);
    DeleteFileW(manifest_path.c_str());
    Check(loaded_ok && loaded.generation == manifest.generation &&
              loaded.active_slot == manifest.active_slot && loaded.source_id == manifest.source_id &&
              loaded.active_bytes == manifest.active_bytes && loaded.active_crc64 == manifest.active_crc64,
          L"shard manifest atomic roundtrip");

    const std::wstring v9_root = std::wstring(temp_dir) + L"pulse-v9-store-test";
    const auto v9_paths = MakeShardPaths(v9_root, L"\\\\?\\VOLUME{V9-TEST}");
    auto write_fake_base = [](const std::wstring& path, uint64_t built) {
        BYTE bytes[128]{};
        memcpy(bytes, "PIDX", 4);
        const uint32_t version = 8;
        memcpy(bytes + 4, &version, sizeof(version));
        memcpy(bytes + 16, &built, sizeof(built));
        HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return false;
        DWORD written = 0;
        const bool ok = WriteFile(h, bytes, sizeof(bytes), &written, nullptr) &&
                        written == sizeof(bytes) && FlushFileBuffers(h) != FALSE;
        CloseHandle(h);
        return ok;
    };
    DeleteFileW(v9_paths.manifest.c_str());
    DeleteFileW(v9_paths.base_a.c_str());
    DeleteFileW(v9_paths.base_b.c_str());
    const std::wstring v9_temp_a = v9_paths.base_a + L".tmp";
    Check(write_fake_base(v9_temp_a, 100) &&
              PublishShardBase(v9_paths, v9_temp_a, 100, 0, manifest, &manifest_error),
          L"v9 first base publish");
    std::wstring active_path;
    Check(ResolveActiveShard(v9_paths, manifest, active_path, &manifest_error) &&
              active_path == v9_paths.base_a,
          L"v9 active slot resolves");
    const std::wstring v9_temp_b = v9_paths.base_a + L".tmp";
    Check(write_fake_base(v9_temp_b, 200) &&
              PublishShardBase(v9_paths, v9_temp_b, 200, 0, manifest, &manifest_error) &&
              manifest.active_slot == 1,
          L"v9 second base flips slot");
    HANDLE corrupt = CreateFileW(v9_paths.base_b.c_str(), GENERIC_WRITE, 0, nullptr,
                                 OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (corrupt != INVALID_HANDLE_VALUE) {
        BYTE zero = 0;
        DWORD written = 0;
        WriteFile(corrupt, &zero, 1, &written, nullptr);
        CloseHandle(corrupt);
    }
    Check(ResolveActiveShard(v9_paths, manifest, active_path, &manifest_error) &&
              active_path == v9_paths.base_a && manifest.active_built == 100,
          L"v9 corrupted active slot falls back");
    DeleteFileW(v9_paths.manifest.c_str());
    DeleteFileW(v9_paths.base_a.c_str());
    DeleteFileW(v9_paths.base_b.c_str());
    RemoveDirectoryW(v9_paths.directory.c_str());
    RemoveDirectoryW(v9_root.c_str());

    {
        // Network re-crawl policy (network_crawl_schedule.h).
        namespace cs = pulse::index::crawl_schedule;
        using namespace std::chrono_literals;
        const cs::Clock::time_point t0{std::chrono::hours(1)};
        const cs::Clock::time_point never{};
        Check(cs::ChangeDue(t0, t0 + 20s, never, 0s) == t0 + 20s + cs::kChangeQuiet,
              L"network changes wait for a quiet period before a crawl");
        Check(cs::ChangeDue(t0, t0 + 30min, never, 0s) == t0 + cs::kChangeMaxDelay,
              L"continuous network changes still crawl within the maximum delay");
        Check(cs::ChangeDue(t0, t0, t0 - 1min, 7min) == t0 - 1min + 28min,
              L"change-triggered crawls keep 4x the last crawl time apart");
        Check(cs::ChangeDue(t0, t0, t0 - 1min, 10s) == t0 - 1min + cs::kChangeMinSpacing,
              L"short crawls are still spaced by the minimum change spacing");
        Check(cs::ReconcileInterval(7min) == 70min && cs::ReconcileInterval(1min) == cs::kReconcileMin,
              L"unwatched reconcile interval is 10x the crawl time, at least 30 minutes");
        Check(cs::UnwatchedDue(t0, 7min, true) == t0 + cs::kOfflineRetry,
              L"a failed network crawl retries after the offline delay");
        Check(cs::StartupDue(t0, 5min) == t0 + 25min,
              L"a fresh shard is reused until it reaches the reconcile age");
        Check(cs::StartupDue(t0, 3h) == t0 + cs::kStartupMinDelay,
              L"a stale shard is reconciled shortly after start, not immediately");
        Check(cs::ChangeSpacing(7min) > 7min * 3,
              L"crawl duty cycle stays below one third for a 7 minute crawl");
    }

    if (argc == 3 && wcscmp(argv[1], L"--network-root") == 0)
        RunNetworkIntegration(argv[2]);

    return failures == 0 ? 0 : 1;
}
