#include "../index/index_config.h"
#include "../index/network_index.h"
#include <algorithm>
#include <iostream>

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
    Check(network.RemoveRoot(root, &error), L"network integration removes UNC root");
    network.Stop();
    if (!config_existed) DeleteFileW(config_path.c_str());
}

int wmain(int argc, wchar_t** argv) {
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
    Check(NormalizeNetworkRoot(L"\\\\192.168.0.254\\工程项目盘\\") ==
              L"\\\\192.168.0.254\\工程项目盘",
          L"network Chinese UNC root normalization");
    Check(NormalizeNetworkRoot(L"C:\\local").empty(),
          L"network root rejects local path");

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

    if (argc == 3 && wcscmp(argv[1], L"--network-root") == 0)
        RunNetworkIntegration(argv[2]);

    return failures == 0 ? 0 : 1;
}
