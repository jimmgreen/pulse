// Compile the production implementation in this test translation unit so the
// filesystem fault injection and private shard fixtures cannot affect shipping
// binaries. All cache paths and actual Win32 directory handles are isolated.
#include <windows.h>
#include <winnetwk.h>
#include <functional>
#include <string>
#include "../index/index_config.h"
namespace pulse::index { std::wstring FixtureUserIndexRoot(); }
HANDLE WINAPI FixtureFindFirstFileExW(LPCWSTR, FINDEX_INFO_LEVELS, LPVOID, FINDEX_SEARCH_OPS, LPVOID, DWORD);
BOOL WINAPI FixtureFindNextFileW(HANDLE, LPWIN32_FIND_DATAW);
BOOL WINAPI FixtureFindClose(HANDLE);
DWORD WINAPI FixtureWNetGetUniversalNameW(LPCWSTR, DWORD, LPVOID, LPDWORD);
#define WNetGetUniversalNameW FixtureWNetGetUniversalNameW
#define UserIndexRoot FixtureUserIndexRoot
#define FindFirstFileExW FixtureFindFirstFileExW
#define FindNextFileW FixtureFindNextFileW
#define FindClose FixtureFindClose
#include "../index/network_index.cpp"
#undef WNetGetUniversalNameW
#undef UserIndexRoot
#undef FindFirstFileExW
#undef FindNextFileW
#undef FindClose
#include <filesystem>
#include <iostream>
#include <cstring>

namespace {
std::wstring cache_root;
std::wstring denied_directory;
int fail_next_after = -1;
int next_calls = 0;
std::wstring callback_name;
std::function<void()> next_callback;
bool mapped_fixture = false;
int mapped_calls = 0;
size_t synthetic_count = 0, synthetic_at = 0;
HANDLE SyntheticHandle() { return reinterpret_cast<HANDLE>(static_cast<uintptr_t>(0x12345)); }
void SyntheticEntry(LPWIN32_FIND_DATAW data) {
    *data = {};
    const bool last = synthetic_at + 1 == synthetic_count;
    const auto name = last ? std::wstring(L"bestmatch.txt") :
        L"z-" + std::to_wstring(synthetic_at) + L"-bestmatch.txt";
    wcscpy_s(data->cFileName, name.c_str());
    data->nFileSizeLow = last ? 9999999 : 1;
    data->ftLastWriteTime.dwLowDateTime = last ? 9999999 : 1;
}
}
std::wstring pulse::index::FixtureUserIndexRoot() { return cache_root; }
DWORD WINAPI FixtureWNetGetUniversalNameW(LPCWSTR path, DWORD level, LPVOID buffer, LPDWORD bytes) {
    if (!mapped_fixture) return ::WNetGetUniversalNameW(path, level, buffer, bytes);
    ++mapped_calls;
    SetLastError(ERROR_ACCESS_DENIED); // Deliberately unrelated to the documented return value.
    constexpr wchar_t unc[] = L"\\\\fixture\\share\\folder";
    const DWORD required = sizeof(UNIVERSAL_NAME_INFOW) + sizeof(unc);
    if (!buffer || *bytes < required) { *bytes = required; return ERROR_MORE_DATA; }
    auto info = static_cast<UNIVERSAL_NAME_INFOW*>(buffer);
    info->lpUniversalName = reinterpret_cast<wchar_t*>(info + 1);
    memcpy(info->lpUniversalName, unc, sizeof(unc));
    return NO_ERROR;
}

HANDLE WINAPI FixtureFindFirstFileExW(LPCWSTR path, FINDEX_INFO_LEVELS level, LPVOID data,
                                      FINDEX_SEARCH_OPS search, LPVOID filter, DWORD flags) {
    if (synthetic_count) { synthetic_at = 0; SyntheticEntry(static_cast<LPWIN32_FIND_DATAW>(data)); return SyntheticHandle(); }
    if (!denied_directory.empty() && std::wstring_view(path).find(denied_directory) != std::wstring_view::npos) {
        SetLastError(ERROR_ACCESS_DENIED); return INVALID_HANDLE_VALUE;
    }
    return ::FindFirstFileExW(path, level, data, search, filter, flags);
}
BOOL WINAPI FixtureFindNextFileW(HANDLE handle, LPWIN32_FIND_DATAW data) {
    if (handle == SyntheticHandle()) {
        if (++synthetic_at < synthetic_count) { SyntheticEntry(data); return TRUE; }
        SetLastError(ERROR_NO_MORE_FILES); return FALSE;
    }
    if (next_callback && callback_name == data->cFileName) {
        auto callback = std::move(next_callback); next_callback = {}; callback();
    }
    if (fail_next_after >= 0 && next_calls++ >= fail_next_after) {
        SetLastError(ERROR_BAD_NETPATH); return FALSE;
    }
    return ::FindNextFileW(handle, data);
}
BOOL WINAPI FixtureFindClose(HANDLE handle) {
    return handle == SyntheticHandle() ? TRUE : ::FindClose(handle);
}
namespace pulse::index {
struct NetworkIndexTestAccess {
    static void Notify(NetworkIndex& index, const std::wstring& root, DWORD action, const std::wstring& name) {
        std::vector<BYTE> packet(offsetof(FILE_NOTIFY_INFORMATION, FileName) + name.size() * sizeof(wchar_t));
        auto info = reinterpret_cast<FILE_NOTIFY_INFORMATION*>(packet.data());
        info->Action = action; info->FileNameLength = static_cast<DWORD>(name.size() * sizeof(wchar_t));
        memcpy(info->FileName, name.data(), info->FileNameLength);
        index.ObserveChanges(root, packet.data(), static_cast<DWORD>(packet.size()));
    }
    static std::shared_ptr<NetworkIndex::Shard> Shard(std::initializer_list<std::pair<std::wstring, uint64_t>> items) {
        std::vector<NetworkRecord> records; std::vector<wchar_t> pool;
        for (const auto& [path, size] : items) {
            NetworkRecord record{}; record.path_off = static_cast<uint32_t>(pool.size());
            record.path_len = static_cast<uint32_t>(path.size());
            record.name_off = static_cast<uint32_t>(path.rfind(L'\\') + 1);
            record.name_len = static_cast<uint16_t>(path.size() - record.name_off); record.size = size;
            pool.insert(pool.end(), path.begin(), path.end()); records.push_back(record);
        }
        return NetworkIndex::Shard::FromMemory(std::move(records), std::move(pool));
    }
    static bool Run() {
        namespace fs = std::filesystem;
        bool ok = true;
        auto check = [&](bool result, const char* label) {
            std::cout << (result ? "[PASS] " : "[FAIL] ") << label << '\n'; ok &= result;
        };
        const auto root = fs::absolute(fs::path("bench_data") / ("network-worker-audit-" + std::to_string(GetCurrentProcessId())));
        mapped_fixture = true; mapped_calls = 0;
        check(NormalizeNetworkRoot(L"Z:\\folder") == L"\\\\fixture\\share\\folder" && mapped_calls == 2,
            "mapped-drive normalization follows API return code despite unrelated last-error value");
        mapped_fixture = false;
        const auto data = root / "files"; fs::create_directories(data / "denied");
        cache_root = (root / "cache").wstring(); fs::create_directories(cache_root);
        std::ofstream(data / "first.txt") << "first"; std::ofstream(data / "second.txt") << "second";
        std::ofstream(data / "denied" / "child.txt") << "child";
        {
            NetworkIndex index; index.running_ = true;
            NetworkIndex::RootState state; state.info.path = data.wstring(); index.roots_.push_back(std::move(state));
            index.BuildRoot(data.wstring(), index.generation_);
            const auto old = index.roots_[0].shard;
            check(old && old->count == 4, "production crawl publishes complete isolated snapshot");
            fail_next_after = 2; next_calls = 0;
            index.BuildRoot(data.wstring(), index.generation_);
            check(index.roots_[0].shard == old && index.roots_[0].last_crawl_failed && index.roots_[0].info.progress == 0,
                "mid-enumeration error preserves old shard and reports incomplete crawl");
            fail_next_after = -1;
            denied_directory = L"\\denied\\";
            index.BuildRoot(data.wstring(), index.generation_);
            check(index.roots_[0].shard == old && index.roots_[0].last_crawl_failed,
                "unreadable subtree never publishes a partial replacement");
            denied_directory.clear();
            index.roots_[0].info.watching = true;
            std::vector<std::wstring> due;
            index.CollectDueRootsLocked(index.roots_[0].last_crawl_end + std::chrono::minutes(6), due);
            check(due.size() == 1, "failed crawl retries even when a watch is healthy");
        }
        {
            LiveNetworkMatches matches; std::mutex mutex;
            fail_next_after = 2; next_calls = 0;
            LiveNetworkWalk(data.wstring(), L"", false, matches, mutex, {}, {});
            check(matches.error == ERROR_BAD_NETPATH && matches.finished, "live walk finishes with the enumeration failure as partial-result error");
            fail_next_after = -1;
        }
        {
            NetworkIndex index; index.running_ = true;
            NetworkIndex::RootState state; state.info.path = data.wstring(); index.roots_.push_back(std::move(state));
            index.watch_wake_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            index.watch_thread_ = std::thread([&] { index.WatchLoop(); });
            bool watched = false, reconcile = false;
            for (int attempt = 0; attempt < 200 && !watched; ++attempt) {
                {
                    std::lock_guard lock(index.mu_);
                    watched = index.roots_[0].info.watching;
                    reconcile = index.dirty_roots_.contains(data.wstring());
                }
                if (!watched) Sleep(10);
            }
            check(watched && reconcile, "actual first directory watch queues gap reconciliation before any shard exists");
            index.Stop();
        }
        fs::create_directories(data / "scan"); std::ofstream(data / "scan" / "item.txt") << "old";
        {
            NetworkIndex index; index.running_ = true;
            NetworkIndex::RootState state; state.info.path = data.wstring(); state.overlay = std::make_shared<NetworkIndex::Overlay>();
            state.overlay->pending_scans.push_back((data / "scan").wstring()); index.roots_.push_back(std::move(state));
            callback_name = L"item.txt";
            next_callback = [&] {
                std::ofstream(data / "scan" / "item.txt", std::ios::trunc) << "newer-data";
                Notify(index, data.wstring(), FILE_ACTION_MODIFIED, L"scan\\item.txt");
            };
            index.ScanPendingSubtrees();
            const auto item = index.roots_[0].overlay->entries.find((data / "scan" / "item.txt").wstring());
            check(!next_callback && item != index.roots_[0].overlay->entries.end() && item->second.size == 10,
                "late subtree scan cannot overwrite actual newer watch metadata");
            index.roots_[0].overlay->pending_scans.push_back((data / "scan").wstring());
            next_callback = [&] { Notify(index, data.wstring(), FILE_ACTION_REMOVED, L"scan"); };
            index.ScanPendingSubtrees();
            auto view = index.OverlayViewLocked(index.roots_[0]);
            check(!next_callback && view && view->added->count == 0 && view->Hides((data / "scan" / "item.txt").wstring()),
                "late subtree result cannot revive newer ancestor deletion");
        }
        {
            NetworkIndex index;
            NetworkIndex::RootState parent, child;
            parent.info.path = L"\\\\fixture\\share"; parent.info.online = true;
            parent.shard = Shard({{L"\\\\fixture\\share\\nested\\same.txt", 1}, {L"\\\\fixture\\share\\outside.txt", 2}});
            child.info.path = L"\\\\fixture\\share\\nested"; child.info.online = true;
            child.shard = Shard({{L"\\\\fixture\\share\\nested\\same.txt", 9}});
            index.roots_.push_back(std::move(parent)); index.roots_.push_back(std::move(child));
            index.running_ = true; index.search_thread_ = std::thread([&] { index.SearchLoop(); });
            Query query; query.needle = L"ext:txt"; query.rank = false; query.sort = ResultSort::Size; query.limit = 10;
            index.SearchAsync(query, 1); SearchResult result; bool ready = false;
            for (int attempt = 0; attempt < 200 && !ready; ++attempt) { ready = index.TakeResult(1, result); if (!ready) Sleep(10); }
            bool fresh = false; for (const auto& hit : result.hits) if (hit.name == L"same.txt" && hit.size == 9) fresh = true;
            check(ready && result.total == 2 && result.hits.size() == 2 && fresh,
                "actual search worker deduplicates overlapping roots before total and sorting, preferring specific scope");
            query.offset = 1; query.limit = 1; index.SearchAsync(query, 2); ready = false;
            for (int attempt = 0; attempt < 200 && !ready; ++attempt) { ready = index.TakeResult(2, result); if (!ready) Sleep(10); }
            check(ready && result.total == 2 && result.hits.size() == 1 && result.hits[0].size == 9,
                "overlap pagination uses unique membership");
            index.Stop();
        }
        synthetic_count = kSearchPageCap + 1;
        for (const auto sort : {ResultSort::Name, ResultSort::Size, ResultSort::Mtime}) {
            Query query; query.needle = L"bestmatch"; query.rank = false; query.sort = sort;
            query.sort_desc = sort != ResultSort::Name; query.limit = 1;
            LiveNetworkMatches matches; std::mutex mutex;
            LiveNetworkWalk(data.wstring(), query.needle, false, matches, mutex, {}, {}, &query);
            const auto result = SelectLiveNetworkHits(query, matches);
            check(matches.finished && matches.complete && result.total == synthetic_count &&
                  matches.hits.size() == kSearchPageCap && result.hits.size() == 1 && result.hits[0].name == L"bestmatch.txt",
                  "late hit beyond retained cap participates in selected global sort");
            query.offset = 1;
            const auto page = SelectLiveNetworkHits(query, matches);
            check(page.hits.size() == 1 && page.hits[0].name != L"bestmatch.txt" && page.total == synthetic_count,
                  "ordered retained set supports subsequent unique page");
        }
        {
            Query query; query.needle = L"bestmatch"; query.rank = true; query.limit = 1;
            LiveNetworkMatches matches; std::mutex mutex;
            LiveNetworkWalk(data.wstring(), query.needle, false, matches, mutex, {}, {}, &query);
            const auto result = SelectLiveNetworkHits(query, matches);
            check(result.hits.size() == 1 && result.hits[0].name == L"bestmatch.txt",
                  "late best-ranked hit beyond cap is retained");
        }
        {
            LiveNetworkMatches matches; std::mutex mutex; size_t calls = 0;
            LiveNetworkWalk(data.wstring(), L"", false, matches, mutex, [&] { return ++calls > 32; }, {});
            check(matches.finished && !matches.complete && matches.total < synthetic_count,
                  "large-directory cancellation terminates with explicit incomplete state");
        }
        synthetic_count = 0;
        next_callback = {}; cache_root.clear(); fs::remove_all(root);
        return ok;
    }
};
}
int main() { return pulse::index::NetworkIndexTestAccess::Run() ? 0 : 1; }
