#include "../index/index_config.h"
#include "../index/index_query.h"
#include "../index/network_index.h"
#include "../index/index_shard.h"
#include <algorithm>
#include <cstring>
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

    {
        wchar_t temp_dir[MAX_PATH]{};
        GetTempPathW(ARRAYSIZE(temp_dir), temp_dir);
        const std::wstring config_path = std::wstring(temp_dir) +
            L"pulse-network-index-utf8-test.json";
        const std::vector<std::wstring> expected{
            L"\\\\192.168.0.254\\工程项目盘",
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
                    contains_utf8 = raw.find("\xE5\xB7\xA5\xE7\xA8\x8B\xE9\xA1\xB9\xE7\x9B\xAE\xE7\x9B\x98") !=
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
    }

    if (argc == 3 && wcscmp(argv[1], L"--network-root") == 0)
        RunNetworkIntegration(argv[2]);

    return failures == 0 ? 0 : 1;
}
