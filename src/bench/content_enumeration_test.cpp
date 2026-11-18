#include "../index/content_search.h"
#include "../index/content_task_search.h"
#include "../index/content_scope.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>

using namespace pulse::index;
namespace {
std::mutex listing_mutex;
struct Listing { std::wstring directory, last; };
std::map<HANDLE, Listing> listings;
std::wstring denied, interrupted_directory;
std::atomic<bool>* cancel_during_next = nullptr;
bool inject_next = false;
std::wstring empty_directory;
DWORD empty_error = ERROR_SUCCESS;
int failures = 0;
void Check(bool value, const char* text) {
    std::cout << (value ? "[PASS] " : "[FAIL] ") << text << '\n';
    if (!value) ++failures;
}
std::wstring Key(std::wstring path) {
    if (path.starts_with(L"\\\\?\\")) path.erase(0, 4);
    return ContentScopeKey(std::move(path));
}
}
namespace pulse::index::content_listing {
HANDLE First(const std::wstring& query, WIN32_FIND_DATAW& data, DWORD flags) {
    const auto dir = Key(query.substr(0, query.size() - 2));
    if (empty_error && dir == empty_directory) { SetLastError(empty_error); return INVALID_HANDLE_VALUE; }
    if (dir == denied) { SetLastError(ERROR_ACCESS_DENIED); return INVALID_HANDLE_VALUE; }
    HANDLE handle = FindFirstFileExW(query.c_str(), FindExInfoBasic, &data, FindExSearchNameMatch, nullptr, flags);
    if (handle != INVALID_HANDLE_VALUE) {
        std::lock_guard lock(listing_mutex);
        listings[handle] = {dir, data.cFileName};
    }
    return handle;
}
BOOL Next(HANDLE handle, WIN32_FIND_DATAW& data) {
    std::lock_guard lock(listing_mutex);
    auto& listing = listings.at(handle);
    if (inject_next && listing.directory == interrupted_directory && listing.last == L"keep.txt") {
        if (cancel_during_next) cancel_during_next->store(true);
        SetLastError(ERROR_NETNAME_DELETED);
        return FALSE;
    }
    const BOOL ok = FindNextFileW(handle, &data);
    const DWORD error = GetLastError();
    if (ok) listing.last = data.cFileName;
    SetLastError(error);
    return ok;
}
void Close(HANDLE handle) {
    {
        std::lock_guard lock(listing_mutex);
        listings.erase(handle);
    }
    FindClose(handle);
    SetLastError(ERROR_INVALID_HANDLE);
}
}
int RunContentMatchPerf();
int wmain(int argc, wchar_t** argv) {
    if (argc == 2 && std::wstring_view(argv[1]) == L"--match-perf") return RunContentMatchPerf();
    namespace fs = std::filesystem;
    const auto fixture_parent = fs::absolute(fs::current_path() / L"bench_data").lexically_normal();
    const auto base = fixture_parent /
        (L"content-enumeration-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    fs::create_directories(fixture_parent);
    if (!fs::create_directory(base)) { Check(false, "exclusive fixture directory creation"); return 1; }
    const auto good = base / L"good", bad = base / L"denied", child = good / L"child", empty = base / L"empty";
    fs::create_directories(child);
    fs::create_directory(empty);
    empty_directory = Key(empty.wstring());
    fs::create_directories(bad);
    std::ofstream(good / L"keep.txt") << "enumeration_marker";
    std::ofstream(child / L"child.txt") << "enumeration_marker";
    for (bool task : {false, true}) {
        std::cout << "[INFO] " << (task ? "task supplement" : "ordinary content search") << '\n';
        for (int scenario = 0; scenario < 11; ++scenario) {
            std::atomic<bool> cancelled{false};
            denied = (scenario == 1 || scenario == 6 || scenario == 7) ? Key(bad.wstring()) : scenario == 2 ? Key(child.wstring()) : std::wstring{};
            interrupted_directory = Key(good.wstring());
            inject_next = scenario == 3 || scenario == 4;
            cancel_during_next = scenario == 4 ? &cancelled : nullptr;
            if (scenario == 5) cancelled = true;
            empty_error = scenario == 9 ? ERROR_FILE_NOT_FOUND : scenario == 10 ? ERROR_NO_MORE_FILES : ERROR_SUCCESS;
            ContentSearchRequest request;
            request.needle = L"enumeration_marker";
            request.roots = {good.wstring()};
            if (scenario == 1) request.roots.push_back(bad.wstring());
            if (scenario == 6) request.roots = {bad.wstring()};
            if (scenario == 7) request.roots = {bad.wstring(), good.wstring()};
            if (scenario >= 8) request.roots = {empty.wstring()};
            ContentSearchProgress final;
            size_t hits = 0, terminals = 0;
            auto callback = [&](const ContentSearchProgress& progress, std::vector<ContentHit> batch) {
                final = progress; hits += batch.size(); terminals += progress.done ? 1 : 0;
                return true;
            };
            bool ok = false;
            if (task) {
                ContentIndexConfig config;
                for (const auto& root : request.roots) config.roots.push_back({root, pulse::text::Encoding::Auto});
                ContentTaskCache cache;
                cache.version = [](const auto&) { return L"fixture-version"; };
                cache.excluded = [](const auto&) { return false; };
                cache.fresh = [](const auto&, uint64_t, uint64_t, const auto&) { return false; };
                ok = RunContentTaskSupplement(config, request, cancelled, cache, {}, 0, callback,
                    [](const auto&, uint64_t, std::wstring& body, uint64_t& bytes, DWORD*, pulse::text::Encoding, const auto&) {
                        body = L"enumeration_marker"; bytes = 18; return true;
                    });
            } else ok = RunContentSearch(request, cancelled, callback);
            const DWORD expected = (scenario == 0 || scenario >= 8) ? ERROR_SUCCESS : (scenario == 1 || scenario == 2 || scenario == 6 || scenario == 7)
                ? ERROR_ACCESS_DENIED : scenario == 3 ? ERROR_NETNAME_DELETED : ERROR_CANCELLED;
            const char* descriptions[] = {"normal EOF succeeds with all hits", "second failed root preserves hits and reports failure",
                "failed subtree preserves hits and reports failure", "FindNext interruption preserves hits and its original error",
                "cancellation during enumeration terminates", "pre-cancelled enumeration terminates", "inaccessible only root reports terminal failure",
                "first failed root remains visible after later successful root", "real empty directory succeeds",
                "FindFirst FILE_NOT_FOUND on existing empty directory succeeds", "FindFirst NO_MORE_FILES on existing empty directory succeeds"};
            bool valid = ok == (scenario == 0 || scenario >= 8) && final.done && terminals == 1 && final.error == expected;
            if (scenario == 0) valid = valid && hits == 2;
            if (scenario >= 8) valid = valid && hits == 0;
            if ((scenario >= 1 && scenario <= 3) || scenario == 7) valid = valid && hits >= 1;
            if (task && scenario >= 1 && scenario <= 3) valid = valid && final.total_files == 0;
            Check(valid, descriptions[scenario]);
            if (!valid) std::cout << "[INFO] ok=" << ok << " done=" << final.done << " error=" << final.error << " hits=" << hits << " terminals=" << terminals << '\n';
            Check(listings.empty(), "all directory handles closed");
        }
    }
    denied = Key(bad.wstring()); empty_error = ERROR_SUCCESS; inject_next = false; cancel_during_next = nullptr;
    ContentSearchRequest duplicates;
    duplicates.mode = ContentSearchMode::Duplicates;
    duplicates.roots = {good.wstring(), bad.wstring()};
    std::atomic<bool> cancelled{false};
    ContentSearchProgress final;
    size_t duplicate_hits = 0;
    const bool duplicate_ok = RunContentSearch(duplicates, cancelled, [&](const auto& progress, auto batch) {
        final = progress; duplicate_hits += batch.size(); return true;
    });
    Check(!duplicate_ok && final.done && final.error == ERROR_ACCESS_DENIED && duplicate_hits == 2,
        "duplicate search retains groups from accessible roots and reports partial failure");
    if (base.parent_path() != fixture_parent || !base.filename().wstring().starts_with(L"content-enumeration-")) {
        Check(false, "fixture cleanup stays within owned directory"); return 1;
    }
    fs::remove_all(base);
    return failures ? 1 : 0;
}
