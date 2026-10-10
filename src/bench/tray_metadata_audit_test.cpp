#include "../app/tray_compare_metadata.h"
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
using pulse::app::TrayCompareMetadata;
using pulse::app::TrayMetadataKey;
using Status = pulse::app::TrayMetadataSnapshot::Status;
static int failures;
static void Check(bool value, const char* message) {
    printf("[%s] %s\n", value ? "PASS" : "FAIL", message); failures += !value;
}
static bool Wait(const std::function<bool()>& done, ULONGLONG limit = 3000) {
    const auto until = GetTickCount64() + limit;
    while (!done()) { if (GetTickCount64() > until) return false; Sleep(5); }
    return true;
}
static TrayMetadataKey Key(const std::wstring& first, const std::wstring& second) {
    TrayMetadataKey key; key.paths = {first, second}; return key;
}
// Refreshes follow the production schedule: 2 s after success, 5 s after an error.
constexpr ULONGLONG kRefreshWait = 8000;
int main() {
    HANDLE entered = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE release = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE returned = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::atomic<int> calls = 0;
    const DWORD ui = GetCurrentThreadId();
    std::atomic<bool> off_ui = true;
    auto cache = std::make_unique<TrayCompareMetadata>([&](const std::wstring& path, WIN32_FILE_ATTRIBUTE_DATA& data, const std::atomic<bool>&) {
        if (GetCurrentThreadId() == ui) off_ui = false;
        ++calls;
        if (path == L"old") { SetEvent(entered); WaitForSingleObject(release, 3000); }
        data.nFileSizeLow = path == L"new" ? 42 : 7;
        SetEvent(returned);
        return DWORD{ERROR_SUCCESS};
    });
    cache->Read(Key(L"old", L"unused"), nullptr);
    Check(WaitForSingleObject(entered, 3000) == WAIT_OBJECT_0, "delayed provider runs on worker");
    const auto start = GetTickCount64();
    for (int i = 0; i < 10000; ++i) cache->Read(Key(L"new", L"other"), nullptr);
    Check(GetTickCount64() - start < 250 && calls == 1, "10000 UI requests stay bounded while provider blocks");
    SetEvent(release);
    Check(Wait([&] { return cache->Read(Key(L"new", L"other"), nullptr).status == Status::Ready; }),
        "latest pair completes after old provider releases");
    const auto snapshot = cache->Read(Key(L"new", L"other"), nullptr);
    Check(snapshot.files[0].nFileSizeLow == 42 && calls == 3 && off_ui,
        "old pair cannot publish or read its second file after replacement");
    cache->Cancel();
    const int stopped_calls = calls;
    Sleep(20);
    Check(calls == stopped_calls, "cancelled comparison schedules no more probes");
    ResetEvent(entered); ResetEvent(release); ResetEvent(returned);
    cache->Read(Key(L"old", L"unused"), nullptr);
    Check(WaitForSingleObject(entered, 3000) == WAIT_OBJECT_0, "second delayed query enters provider");
    const auto close_start = GetTickCount64();
    cache.reset();
    Check(GetTickCount64() - close_start < 100, "closing while provider blocks never joins on UI thread");
    SetEvent(release);
    Check(WaitForSingleObject(returned, 3000) == WAIT_OBJECT_0, "closed cache drains without accessing UI state");
    CloseHandle(entered); CloseHandle(release); CloseHandle(returned);
    std::atomic<int> errors = 0;
    TrayCompareMetadata failing([&](const std::wstring&, WIN32_FILE_ATTRIBUTE_DATA&, const std::atomic<bool>&) {
        ++errors; return DWORD{ERROR_ACCESS_DENIED};
    });
    Check(Wait([&] { return failing.Read(Key(L"unreadable", L"other"), nullptr).status == Status::Error; }),
        "failed metadata attempt reports completion");
    for (int i = 0; i < 100; ++i) failing.Read(Key(L"unreadable", L"other"), nullptr);
    Check(errors == 1 && failing.Read(Key(L"unreadable", L"other"), nullptr).error == ERROR_ACCESS_DENIED,
        "unreadable pair retains error without retrying before its backoff");
    std::atomic<bool> deny = false;
    TrayCompareMetadata recovering([&](const std::wstring&, WIN32_FILE_ATTRIBUTE_DATA&, const std::atomic<bool>&) {
        return static_cast<DWORD>(deny ? ERROR_ACCESS_DENIED : ERROR_SUCCESS);
    });
    const auto refresh = Key(L"refresh", L"other");
    Check(Wait([&] { return recovering.Read(refresh, nullptr).status == Status::Ready; }),
        "successful metadata query completes");
    deny = true;
    Check(Wait([&] { return recovering.Read(refresh, nullptr).status == Status::Error; }, kRefreshWait) &&
        recovering.Read(refresh, nullptr).error == ERROR_ACCESS_DENIED,
        "temporary metadata error is reported by a later refresh of the same pair");
    deny = false;
    Check(Wait([&] { return recovering.Read(refresh, nullptr).status == Status::Ready; }, kRefreshWait) &&
        recovering.Read(refresh, nullptr).error == ERROR_SUCCESS,
        "the retry after a temporary error recovers the pair");
    const auto root = std::filesystem::absolute(L"bench_data/tray-metadata-" + std::to_wstring(GetCurrentProcessId()));
    std::filesystem::create_directories(root);
    const auto file = root / L"sample.txt";
    { std::ofstream output(file, std::ios::binary); output << "fixture"; }
    TrayCompareMetadata actual;
    const auto pair = Key(file.wstring(), file.wstring());
    const auto ready_with = [&](DWORD size) {
        const auto value = actual.Read(pair, nullptr);
        return value.status == Status::Ready && value.files[0].nFileSizeLow == size;
    };
    Check(Wait([&] { return ready_with(7); }), "production Windows provider reads actual isolated file metadata");
    { std::ofstream output(file, std::ios::binary); output << "updated fixture"; }
    Check(Wait([&] { return ready_with(15); }, kRefreshWait), "same file pair refreshes actual changed file size");
    std::filesystem::remove(file);
    Check(Wait([&] {
        const auto value = actual.Read(pair, nullptr);
        return value.status == Status::Error && value.error == ERROR_FILE_NOT_FOUND;
    }, kRefreshWait), "deleted fixture reports missing metadata rather than stale size");
    { std::ofstream output(file, std::ios::binary); output << "restored"; }
    Check(Wait([&] { return ready_with(8); }, kRefreshWait),
        "recreated fixture recovers actual metadata without changing selected pair");
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    return failures ? 1 : 0;
}
