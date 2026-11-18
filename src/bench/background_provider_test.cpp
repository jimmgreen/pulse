#include "../fs/bounded_enumeration.h"
#include "../fs/count_directory.h"
#include <cstdio>
#include <filesystem>
#include <vector>

int main() {
    using namespace pulse::fs;
    std::atomic_bool release{false};
    std::vector<std::thread> callers;
    for (int i = 0; i < 4; ++i) callers.emplace_back([&] {
        try {
            RunBoundedEnumeration<int>(true, [&](const auto&) {
                while (!release.load()) Sleep(1);
                return 1;
            }, [] { return false; }, std::chrono::milliseconds(100));
        } catch (...) {}
    });
    for (auto& caller : callers) caller.join();
    bool ok = active_network_enumerations.load() == 4;
    for (int i = 0; i < 100; ++i) {
        try {
            RunBoundedEnumeration<int>(true, [](const auto&) { return 0; },
                [] { return false; }, std::chrono::milliseconds(1));
            ok = false;
        } catch (...) {}
        ok = ok && active_network_enumerations.load() == 4;
    }
    ok = ok && RunBoundedEnumeration<int>(false, [](const auto&) { return 7; },
        [] { return false; }) == 7;
    release = true;
    for (int i = 0; i < 1000 && active_network_enumerations.load(); ++i) Sleep(1);
    ok = ok && active_network_enumerations.load() == 0;
    std::puts(ok ? "[PASS] Retired provider quota, 100 retries, local isolation, cleanup" : "[FAIL] Provider quota");
    std::atomic_bool cancel{false};
    const auto root = std::filesystem::absolute(L"bench_data").wstring() + L"\\count_directory_" +
        std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(GetTickCount64());
    std::error_code error;
    std::filesystem::create_directories(root, error);
    if (error) return 1;
    bool count_ok = CountDirectoryChildren(root, false, false, cancel) == 0;
    std::vector<std::wstring> created;
    for (int i = 0; i < 10000; ++i) {
        auto path = root + L"\\file" + std::to_wstring(i);
        const DWORD attrs = i == 0 ? FILE_ATTRIBUTE_HIDDEN :
            i == 1 ? FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM : FILE_ATTRIBUTE_NORMAL;
        HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
            CREATE_NEW, attrs, nullptr);
        if (file == INVALID_HANDLE_VALUE) { count_ok = false; break; }
        CloseHandle(file);
        created.push_back(std::move(path));
    }
    const auto start = GetTickCount64();
    count_ok = count_ok && CountDirectoryChildren(root, false, false, cancel) == 9998;
    count_ok = count_ok && CountDirectoryChildren(root, true, false, cancel) == 9999;
    count_ok = count_ok && CountDirectoryChildren(root, true, true, cancel) == 10000;
    std::printf("  10000 children, 3 visibility modes: %llu ms; native record bytes=%zu\n",
        GetTickCount64() - start, sizeof(WIN32_FIND_DATAW));
    cancel = true;
    bool cancelled = false, failed = false;
    try { CountDirectoryChildren(root, true, true, cancel); }
    catch (const EnumerationCancelled&) { cancelled = true; }
    cancel = false;
    try { CountDirectoryChildren(root + L"\\missing", true, true, cancel); }
    catch (const std::runtime_error&) { failed = true; }
    for (const auto& path : created) { SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL); DeleteFileW(path.c_str()); }
    count_ok = count_ok && cancelled && failed && RemoveDirectoryW(root.c_str());
    std::puts(count_ok ? "[PASS] Streaming count: empty, hidden/system, 10000 children, cancellation, missing path, cleanup" : "[FAIL] Streaming count");
    ok = ok && count_ok;
    return ok ? 0 : 1;
}
