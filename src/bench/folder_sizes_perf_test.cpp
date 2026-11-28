#include "../app/folder_sizes.h"
#include "../fs/fs_enum.h"
#include <windows.h>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string_view>

namespace {
using Clock = std::chrono::steady_clock;
using State = pulse::app::FolderSizeState;
double Milliseconds(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}
const char* StateName(State state) {
    switch (state) {
    case State::Ready: return "complete-scan";
    case State::Partial: return "partial-scan";
    case State::Unavailable: return "unavailable";
    case State::Indexed: return "index-estimate";
    case State::Cached: return "cached";
    case State::Updating: return "updating";
    default: return "calculating";
    }
}
struct Reference {
    uint64_t bytes = 0, files = 0, folders = 0;
    uint64_t skipped_reparse = 0, skipped_offline = 0, skipped_depth = 0, unreadable = 0;
    bool partial = false, timed_out = false;
};
void Enumerate(const std::wstring& path, Reference& value, ULONGLONG deadline, unsigned depth = 0) {
    if (GetTickCount64() >= deadline) { value.timed_out = true; return; }
    WIN32_FIND_DATAW data{};
    HANDLE find = FindFirstFileExW((path + L"\\*").c_str(), FindExInfoBasic, &data,
        FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (find == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        const bool failed = error != ERROR_FILE_NOT_FOUND && error != ERROR_NO_MORE_FILES;
        value.partial |= failed;
        if (failed) ++value.unreadable;
        return;
    }
    do {
        if (GetTickCount64() >= deadline) { value.timed_out = true; break; }
        if (!wcscmp(data.cFileName, L".") || !wcscmp(data.cFileName, L"..")) continue;
        if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            ++value.folders;
            if (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
                ++value.skipped_reparse; value.partial = true;
            } else if (data.dwFileAttributes & FILE_ATTRIBUTE_OFFLINE) {
                ++value.skipped_offline; value.partial = true;
            } else if (depth >= 127) {
                ++value.skipped_depth; value.partial = true;
            } else Enumerate(path + L"\\" + data.cFileName, value, deadline, depth + 1);
        } else {
            ++value.files;
            const auto bytes = (static_cast<uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
            if (bytes > UINT64_MAX - value.bytes) value.partial = true;
            else value.bytes += bytes;
        }
    } while (!value.timed_out && FindNextFileW(find, &data));
    if (!value.timed_out && GetLastError() != ERROR_NO_MORE_FILES) value.partial = true;
    FindClose(find);
}
bool Wait(const std::function<bool()>& predicate, DWORD timeout = 10000) {
    const auto deadline = GetTickCount64() + timeout;
    while (GetTickCount64() < deadline) { if (predicate()) return true; Sleep(5); }
    return predicate();
}
int Real(const wchar_t* input) {
    const auto path = pulse::fs::NormalizePath(input);
    const DWORD attrs = GetFileAttributesW(path.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES || !(attrs & FILE_ATTRIBUTE_DIRECTORY) ||
        (attrs & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_OFFLINE)) || pulse::fs::IsUncPath(path)) {
        std::printf("[FAIL] benchmark requires an accessible local non-reparse directory\n"); return 2;
    }
    pulse::app::FolderSizes sizes;
    sizes.SetIndexEnabled(false); // Exact metadata traversal, no service, watches or persistent cache.
    const auto cpu = [] {
        FILETIME created{}, exited{}, kernel{}, user{};
        GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user);
        return ((uint64_t(kernel.dwHighDateTime) << 32) | kernel.dwLowDateTime) +
            ((uint64_t(user.dwHighDateTime) << 32) | user.dwLowDateTime);
    };
    const auto cpu_start = cpu();
    ULONG64 cycles_start = 0, cycles_end = 0;
    QueryProcessCycleTime(GetCurrentProcess(), &cycles_start);
    const auto start = Clock::now();
    sizes.Sync({{path}}, {});
    // Progress trace: walked entries, worker activity and job churn once per second.
    uint64_t next_trace = GetTickCount64() + 1000;
    const bool finished = Wait([&] {
        const auto value = sizes.Get(path);
        if (GetTickCount64() >= next_trace) {
            next_trace += 1000;
            const auto work = sizes.GetWork(path);
            const auto stats = sizes.ReadStats();
            std::printf("[TRACE] t=%.0f activity=%d work_entries=%llu work_bytes=%llu entries=%llu started=%llu cancelled=%llu completed=%llu deferred=%llu\n",
                Milliseconds(start), static_cast<int>(work.activity), static_cast<unsigned long long>(work.entries),
                static_cast<unsigned long long>(work.bytes), static_cast<unsigned long long>(stats.entries_scanned),
                static_cast<unsigned long long>(stats.jobs_started), static_cast<unsigned long long>(stats.jobs_cancelled),
                static_cast<unsigned long long>(stats.jobs_completed), static_cast<unsigned long long>(stats.deferred_jobs));
            std::fflush(stdout);
        }
        return value.state == State::Ready || value.state == State::Partial || value.state == State::Unavailable;
    }, 60000);
    const auto scan_ms = Milliseconds(start);
    const auto cpu_ms = static_cast<double>(cpu() - cpu_start) / 10000.0;
    QueryProcessCycleTime(GetCurrentProcess(), &cycles_end);
    std::printf("[COST] cpu_ms=%.3f cycles=%llu\n", cpu_ms,
        static_cast<unsigned long long>(cycles_end - cycles_start));
    const auto value = sizes.Get(path);
    sizes.Stop();
    const auto reference_start = Clock::now();
    Reference reference;
    Enumerate(path, reference, GetTickCount64() + 60000);
    const auto reference_ms = Milliseconds(reference_start);
    bool same = finished && !reference.timed_out && value.has_value && value.bytes == reference.bytes &&
        (value.state == State::Partial) == reference.partial;
#ifndef PULSE_FOLDER_SIZE_BASELINE
    same = same && value.partial == reference.partial;
#endif
    std::printf("[REAL] path=%ls source=%s bytes=%llu scan_ms=%.3f reference_ms=%.3f reference_bytes=%llu "
        "files=%llu folders=%llu partial=%d deadline=%d matches=%d\n", path.c_str(), StateName(value.state),
        static_cast<unsigned long long>(value.bytes), scan_ms, reference_ms,
        static_cast<unsigned long long>(reference.bytes), static_cast<unsigned long long>(reference.files),
        static_cast<unsigned long long>(reference.folders), reference.partial, !finished || reference.timed_out, same);
    std::printf("[SOURCE] logical file sizes; skipped_reparse=%llu skipped_offline=%llu skipped_depth=%llu unreadable=%llu lower_bound=%d\n",
        static_cast<unsigned long long>(reference.skipped_reparse), static_cast<unsigned long long>(reference.skipped_offline),
        static_cast<unsigned long long>(reference.skipped_depth), static_cast<unsigned long long>(reference.unreadable), value.partial);
    return same ? 0 : 1;
}
int Regression() {
    const auto fixture = std::filesystem::current_path() / L"bench_data" / L"folder_sizes_focused" /
        (std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    std::filesystem::create_directories(fixture / L"nested" / L"child");
    std::filesystem::create_directory(fixture / L"empty");
    { std::ofstream file(fixture / L"nested" / L"a.bin", std::ios::binary); file << std::string(1234, 'a'); }
    { std::ofstream file(fixture / L"nested" / L"child" / L"b.bin", std::ios::binary); file << std::string(5678, 'b'); }
    bool ok = true;
    const auto check = [&](bool passed, const char* label) {
        std::printf("[%s] %s\n", passed ? "PASS" : "FAIL", label); ok &= passed;
    };
    pulse::app::FolderSizes sizes;
    sizes.SetIndexEnabled(false);
    const auto nested = (fixture / L"nested").wstring(), empty = (fixture / L"empty").wstring();
    sizes.Sync({{nested}, {empty}}, {});
    check(Wait([&] { return sizes.Get(nested).state == State::Ready; }) && sizes.Get(nested).bytes == 6912,
        "exact nested logical size matches fixture bytes");
    check(Wait([&] { return sizes.Get(empty).state == State::Ready; }) && sizes.Get(empty).has_value && sizes.Get(empty).bytes == 0,
        "empty folder is a complete known zero");
    sizes.Stop();
    const auto offline = fixture / L"nested" / L"offline";
    std::filesystem::create_directory(offline);
    { std::ofstream file(offline / L"skipped.bin", std::ios::binary); file << std::string(99, 'c'); }
    if (SetFileAttributesW(offline.c_str(), FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_OFFLINE)) {
        const auto cache_file = (fixture / L"partial-cache.json").wstring();
        {
            pulse::app::FolderSizes partial;
            partial.SetIndexEnabled(false); partial.SetCachePath([cache_file] { return cache_file; });
            partial.Sync({{nested}}, {});
            check(Wait([&] { return partial.Get(nested).state == State::Partial; }) &&
                partial.Get(nested).bytes == 6912 && partial.Get(nested).partial,
                "skipped subtree produces an explicit lower bound with exact readable bytes");
            partial.Sync({{nested, false}}, {});
            partial.Invalidate(nested);
            check(partial.Get(nested).state == State::Cached && partial.Get(nested).partial && partial.Get(nested).bytes == 6912,
                "invalidation and manual state preserve incomplete-byte provenance");
            partial.Stop();
        }
        {
            pulse::app::FolderSizes restored;
            restored.SetIndexEnabled(false); restored.SetCachePath([cache_file] { return cache_file; });
            restored.Sync({{nested, false}}, {});
            check(Wait([&] { return restored.Get(nested).has_value; }) && restored.Get(nested).partial &&
                restored.Get(nested).state == State::Cached && restored.Get(nested).bytes == 6912,
                "cache reload preserves partial provenance after updating state was saved");
            SetFileAttributesW(offline.c_str(), FILE_ATTRIBUTE_DIRECTORY);
            restored.Calculate(nested);
            check(Wait([&] { return restored.Get(nested).state == State::Ready; }) &&
                restored.Get(nested).bytes == 7011 && !restored.Get(nested).partial,
                "successful complete rescan clears lower-bound marker");
            restored.Stop();
        }
        SetFileAttributesW(offline.c_str(), FILE_ATTRIBUTE_DIRECTORY);
    } else std::printf("[SKIP] filesystem cannot set offline directory attribute\n");
    return ok ? 0 : 1;
}
}
int wmain(int argc, wchar_t** argv) {
    if (argc == 3 && std::wstring_view(argv[1]) == L"--real-root") return Real(argv[2]);
    // Several roots in one process: per-process start-up costs (worker thread,
    // drive-kind probe) are paid once, as in the app.
    if (argc >= 3 && std::wstring_view(argv[1]) == L"--real-roots") {
        int result = 0;
        for (int i = 2; i < argc; ++i) result |= Real(argv[i]);
        return result;
    }
    return Regression();
}