#include "../ui/thumbnail_cache.h"
#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <map>
#include <thread>

namespace pulse::ui {
struct ThumbnailCacheTestAccess {
    static int Run(const std::filesystem::path& folder) {
        std::vector<std::filesystem::path> files;
        for (const auto& entry : std::filesystem::directory_iterator(folder))
            if (entry.is_regular_file()) files.push_back(entry.path());
        std::sort(files.begin(), files.end());
        if (files.empty() || files.size() > 128) return 2;
        ThumbnailCache cache(96ull << 20, 128);
        cache.SetParallelism(4);
        cache.Warm();
        const auto started = std::chrono::steady_clock::now();
        const auto elapsed = [&] { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count(); };
        {
            std::lock_guard lock(cache.mutex_);
            for (size_t i = 0; i < files.size(); ++i) {
                WIN32_FILE_ATTRIBUTE_DATA attrs{};
                if (!GetFileAttributesExW(files[i].c_str(), GetFileExInfoStandard, &attrs)) return 3;
                ThumbnailCache::Request request;
                request.id = static_cast<uint32_t>(i + 1);
                request.generation = 1;
                request.epoch = cache.epoch_.load();
                request.pixels = 256;
                request.attrs = attrs.dwFileAttributes;
                request.timeout_ms = 60000;
                request.path = files[i].wstring();
                request.modified = (uint64_t(attrs.ftLastWriteTime.dwHighDateTime) << 32) | attrs.ftLastWriteTime.dwLowDateTime;
                request.size = (uint64_t(attrs.nFileSizeHigh) << 32) | attrs.nFileSizeLow;
                request.key = cache.Key(request.path, request.pixels, request.modified, request.size);
                cache.pending_.insert(request.key);
                cache.queue_.push_back(std::move(request));
            }
            cache.cv_.notify_all();
        }
        std::map<DWORD, HANDLE> processes;
        const DWORD own_pid = GetCurrentProcessId();
        processes.emplace(own_pid, OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, own_pid));
        uint64_t peak_commit = 0, peak_ws = 0;
        double next_process_scan = 0, first = -1, half = -1, ninety = -1;
        size_t completed = 0, failed = 0;
        while (elapsed() < 120000) {
            const double now = elapsed();
            if (now >= next_process_scan) {
                HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
                if (snapshot != INVALID_HANDLE_VALUE) {
                    std::vector<std::pair<DWORD, DWORD>> tree;
                    PROCESSENTRY32W process{sizeof(process)};
                    if (Process32FirstW(snapshot, &process)) do {
                        tree.emplace_back(process.th32ProcessID, process.th32ParentProcessID);
                    } while (Process32NextW(snapshot, &process));
                    CloseHandle(snapshot);
                    for (int depth = 0; depth < 4; ++depth) for (const auto& [pid, parent] : tree)
                        if (processes.contains(parent) && !processes.contains(pid)) {
                            HANDLE handle = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
                            if (handle) processes.emplace(pid, handle);
                        }
                }
                next_process_scan = now + 100;
            }
            uint64_t commit = 0, ws = 0;
            for (const auto& [pid, handle] : processes) {
                (void)pid;
                PROCESS_MEMORY_COUNTERS_EX memory{}; memory.cb = sizeof(memory);
                if (handle && WaitForSingleObject(handle, 0) != WAIT_OBJECT_0 &&
                    GetProcessMemoryInfo(handle, reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory))) {
                    commit += memory.PrivateUsage;
                    ws += memory.WorkingSetSize;
                }
            }
            peak_commit = (std::max)(peak_commit, commit);
            peak_ws = (std::max)(peak_ws, ws);
            {
                std::lock_guard lock(cache.mutex_);
                completed = cache.items_.size(); failed = 0;
                for (const auto& [key, item] : cache.items_) {
                    (void)key;
                    if (item.failed || item.pixels.empty()) ++failed;
                }
            }
            if (completed && first < 0) first = now;
            if (completed * 2 >= files.size() && half < 0) half = now;
            if (completed * 10 >= files.size() * 9 && ninety < 0) ninety = now;
            if (completed == files.size()) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        std::printf("files,completed,failed,first_ms,half_ms,ninety_ms,total_ms,peak_private_commit,peak_working_set\n");
        std::printf("%zu,%zu,%zu,%.3f,%.3f,%.3f,%.3f,%llu,%llu\n", files.size(), completed, failed,
            first, half, ninety, elapsed(), static_cast<unsigned long long>(peak_commit), static_cast<unsigned long long>(peak_ws));
        std::fflush(stdout);
        cache.Reset();
        for (const auto& [pid, handle] : processes) { (void)pid; if (handle) CloseHandle(handle); }
        return completed == files.size() && failed == 0 ? 0 : 1;
    }
};
}
int wmain(int argc, wchar_t** argv) {
    if (argc != 2) return 2;
    SetEnvironmentVariableW(L"PULSE_TEST_THUMB_CACHE", L"off");
    SetEnvironmentVariableW(L"PULSE_THUMBNAIL_BOOST", L"0");
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const int result = pulse::ui::ThumbnailCacheTestAccess::Run(std::filesystem::absolute(argv[1]));
    if (SUCCEEDED(com)) CoUninitialize();
    return result;
}