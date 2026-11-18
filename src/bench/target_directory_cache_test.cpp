#include "../ops/bounded_target_listing.h"
#include "../ops/directory_metadata.h"
#include <windows.h>
#include <cstdio>
#include <filesystem>

namespace {
int failures = 0;
void Check(bool ok, const char* label) {
    printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
    if (!ok) ++failures;
}
bool Touch(const std::wstring& path) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool ok = WriteFile(file, "fixture", 7, &written, nullptr) && written == 7;
    CloseHandle(file);
    return ok;
}
}
int wmain() {
    wchar_t executable[32768]{};
    if (!GetModuleFileNameW(nullptr, executable, 32768)) return 2;
    const auto root = std::filesystem::path(executable).parent_path().parent_path() / L"bench_data" /
        (L"target_directory_" + std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(GetTickCount64()));
    std::error_code error;
    std::filesystem::create_directories(root / L"deep" / L"nested", error);
    if (error) return 2;
    const auto folder = (root / L"deep" / L"nested").wstring();
    const auto existing = folder + L"\\Existing.txt";
    if (!Touch(existing)) return 2;
    pulse::ops::BoundedTargetListing cache;
    for (int i = 0; i < 16; ++i) cache.SnapshotAbsent(folder, L"missing.txt", 100);
    Check(cache.SnapshotAbsent(folder, L"missing.txt", 100), "deep directory snapshot finds an absent name");
    Check(!cache.SnapshotAbsent(folder, L"existing.TXT", 100), "case variants still require a live probe");
    Check(!cache.SnapshotAbsent(folder, L"SHORT~1.TXT", 100) &&
          !cache.SnapshotAbsent(folder, L"trailing. ", 100), "aliases and trimmed names never bypass probes");
    const auto arrived = folder + L"\\missing.txt";
    if (!Touch(arrived)) return 2;
    Check(cache.SnapshotAbsent(folder, L"missing.txt", 100), "concurrent creation demonstrates snapshot is advisory");
    HANDLE exclusive = CreateFileW(arrived.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    const DWORD create_error = GetLastError();
    Check(exclusive == INVALID_HANDLE_VALUE && (create_error == ERROR_FILE_EXISTS || create_error == ERROR_ALREADY_EXISTS),
          "exclusive creation rejects a target that appeared after the snapshot");
    if (exclusive != INVALID_HANDLE_VALUE) CloseHandle(exclusive);
    cache.Added(folder, L"missing.txt");
    Check(!cache.SnapshotAbsent(folder, L"missing.txt", 100), "task-created names invalidate absence hints");
    cache.Invalidate(folder);
    Check(!cache.SnapshotAbsent(folder, L"other.txt", 100), "explicit invalidation falls back to live lookup");
    for (int i = 0; i < 16; ++i) cache.SnapshotAbsent(folder, L"other.txt", 100);
    Check(!cache.SnapshotAbsent(folder, L"other.txt", 600), "expired snapshot falls back to live lookup");
    for (size_t i = 0; i < 32; ++i)
        cache.SnapshotAbsent(folder + L"\\not-there-" + std::to_wstring(i), L"x", 1000);
    Check(cache.Size() == pulse::ops::BoundedTargetListing::kCapacity, "directory cache has a fixed capacity");
    for (int i = 0; i < 20; ++i)
        Check(!cache.SnapshotAbsent(folder + L"\\not-there-31", L"x", 1000), "failed enumeration never becomes absent");

    FILETIME now{};
    GetSystemTimeAsFileTime(&now);
    DWORD metadata_error = ERROR_SUCCESS;
    Check(pulse::ops::ApplyDirectoryMetadata(folder, now, now, now, FILE_ATTRIBUTE_HIDDEN, metadata_error) &&
          metadata_error == ERROR_SUCCESS, "directory timestamps and attributes apply successfully");
    WIN32_FILE_ATTRIBUTE_DATA data{};
    Check(GetFileAttributesExW(folder.c_str(), GetFileExInfoStandard, &data) &&
          (data.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN) && CompareFileTime(&data.ftLastWriteTime, &now) == 0,
          "successful metadata writes are visible on the destination");
    Check(!pulse::ops::ApplyDirectoryMetadata(folder + L"\\missing-directory", now, now, now, 0, metadata_error) &&
          metadata_error != ERROR_SUCCESS, "missing directory returns the actual metadata error");
    Check(!pulse::ops::ApplyDirectoryMetadata(existing, now, now, now, 0, metadata_error) &&
          metadata_error == ERROR_DIRECTORY, "file substituted for directory is rejected");
    Check(GetFileAttributesW(existing.c_str()) != INVALID_FILE_ATTRIBUTES,
          "metadata failures preserve copied content");
    wprintf(L"[INFO] retained fixture: %ls\n", root.c_str());
    return failures ? 1 : 0;
}
