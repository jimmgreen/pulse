#include "../ops/replacement_backup.h"
#include <objbase.h>
#include <filesystem>
#include <cstdio>
#include <array>
#include <bcrypt.h>
#include <cstddef>
#include <cstring>
namespace fs = std::filesystem;
using namespace pulse::ops;
namespace {
int failures = 0;
void Check(bool value, const char* name) {
    printf("[%s] %s\n", value ? "PASS" : "FAIL", name);
    if (!value) ++failures;
}
bool Write(const fs::path& path, char value) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD count = 0;
    const bool ok = WriteFile(file, &value, 1, &count, nullptr) && count == 1;
    CloseHandle(file); return ok;
}
bool Has(const fs::path& path, char expected) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    char value = 0; DWORD count = 0; LARGE_INTEGER size{};
    const bool ok = GetFileSizeEx(file, &size) && size.QuadPart == 1 &&
        ReadFile(file, &value, 1, &count, nullptr) && count == 1 && value == expected;
    CloseHandle(file); return ok;
}

bool DigestHandle(HANDLE file, std::array<unsigned char, 32>& digest) {
    LARGE_INTEGER zero{};
    if (!SetFilePointerEx(file, zero, nullptr, FILE_BEGIN)) return false;
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    bool ok = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0;
    if (ok) ok = BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) >= 0;
    std::array<unsigned char, 4096> bytes{};
    while (ok) {
        DWORD read = 0;
        ok = ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr) != FALSE;
        if (!ok || !read) break;
        ok = BCryptHashData(hash, bytes.data(), read, 0) >= 0;
    }
    if (ok) ok = BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) >= 0;
    if (hash) BCryptDestroyHash(hash);
    if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    return ok;
}
bool StreamsFromHandle(HANDLE file, bool expect_ads) {
    // A fixed upper bound suffices for the two explicitly created test streams.
    alignas(FILE_STREAM_INFO) std::array<unsigned char, 4096> buffer{};
    if (!GetFileInformationByHandleEx(file, FileStreamInfo, buffer.data(), static_cast<DWORD>(buffer.size()))) return false;
    bool base = false, ads = false;
    size_t offset = 0;
    for (;;) {
        if (offset > buffer.size() - offsetof(FILE_STREAM_INFO, StreamName)) return false;
        const auto* info = reinterpret_cast<const FILE_STREAM_INFO*>(buffer.data() + offset);
        if (info->StreamNameLength % sizeof(wchar_t) ||
            info->StreamNameLength > buffer.size() - offset - offsetof(FILE_STREAM_INFO, StreamName)) return false;
        const std::wstring name(info->StreamName, info->StreamNameLength / sizeof(wchar_t));
        if (name == L"::$DATA") base = true;
        else if (name == L":before:$DATA") ads = true;
        else return false;
        if (!info->NextEntryOffset) break;
        if (info->NextEntryOffset < offsetof(FILE_STREAM_INFO, StreamName) || info->NextEntryOffset > buffer.size() - offset) return false;
        offset += info->NextEntryOffset;
    }
    return base && ads == expect_ads;
}
void PendingDispositionCases(const fs::path& root) {
    for (int mode = 0; mode != 2; ++mode) {
        const auto path = root / (L"pending" + std::to_wstring(mode));
        Check(Write(path, 'A'), "pending probe fixture content");
        HANDLE file = CreateFileW(path.c_str(), GENERIC_READ | DELETE | FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        Check(file != INVALID_HANDLE_VALUE, "open held ordinary handle without DELETE_ON_CLOSE");
        if (file == INVALID_HANDLE_VALUE) continue;
        std::array<unsigned char, 32> before{}, pending{};
        Check(DigestHandle(file, before), "SHA256 from held handle before delete pending");
        const fs::path existing(path.wstring() + L":before");
        if (mode == 1) {
            // Deliberately wins the gap after base handle acquisition, before disposition.
            HANDLE stream = CreateFileW(existing.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                nullptr, CREATE_NEW, 0, nullptr);
            const DWORD open_error = stream == INVALID_HANDLE_VALUE ? GetLastError() : ERROR_SUCCESS;
            DWORD wrote = 0; const char value = 'X';
            const bool written = stream != INVALID_HANDLE_VALUE && WriteFile(stream, &value, 1, &wrote, nullptr) && wrote == 1;
            if (stream != INVALID_HANDLE_VALUE) CloseHandle(stream);
            printf("[DIAG] pre-pending ADS open_error=%lu\n", open_error);
            Check(written, "ADS can win before delete-pending transition");
        }
        FILE_DISPOSITION_INFO disposition{TRUE};
        const bool marked = SetFileInformationByHandle(file, FileDispositionInfo, &disposition, sizeof(disposition)) != FALSE;
        Check(marked, "classic delete pending accepted while original handle stays open");
        if (!marked) { CloseHandle(file); continue; }
        FILE_STANDARD_INFO standard{};
        Check(GetFileInformationByHandleEx(file, FileStandardInfo, &standard, sizeof(standard)) && standard.DeletePending,
            "held handle reports delete-pending state");
        const fs::path late(path.wstring() + L":after");
        HANDLE attempted = CreateFileW(late.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, CREATE_NEW, 0, nullptr);
        const DWORD late_error = attempted == INVALID_HANDLE_VALUE ? GetLastError() : ERROR_SUCCESS;
        if (attempted != INVALID_HANDLE_VALUE) CloseHandle(attempted);
        printf("[DIAG] pending new ADS open_error=%lu\n", late_error);
        Check(attempted == INVALID_HANDLE_VALUE && (late_error == ERROR_ACCESS_DENIED || late_error == ERROR_DELETE_PENDING),
            "delete pending rejects new ADS even with permissive sharing");
        Check(StreamsFromHandle(file, mode == 1), "existing handle enumerates all streams while pending");
        Check(DigestHandle(file, pending) && before == pending, "existing handle can verify full SHA256 while pending");
        disposition.DeleteFile = FALSE;
        const bool undone = SetFileInformationByHandle(file, FileDispositionInfo, &disposition, sizeof(disposition)) != FALSE;
        const DWORD undo_error = undone ? ERROR_SUCCESS : GetLastError();
        printf("[DIAG] rollback pending error=%lu\n", undo_error);
        Check(undone, "clear pending after verification rejection or cancellation");
        if (!undone) {
            // Never explicitly close a still-pending uncommitted handle. This
            // probe fails and retains the fixture. Process termination will
            // still close leaked handles: a product cannot promise recovery in
            // this condition and must surface/quarantine it before shutdown.
            printf("[FAIL] pending handle retained; rollback failed; process exit may delete fixture=%ls\n", path.c_str());
            continue;
        }
        const bool cleared = GetFileInformationByHandleEx(file, FileStandardInfo, &standard, sizeof(standard)) && !standard.DeletePending;
        Check(cleared, "held handle confirms pending was cleared before close");
        if (!cleared) {
            printf("[FAIL] unable to confirm rollback; handle retained; process exit may delete fixture=%ls\n", path.c_str());
            continue;
        }
        CloseHandle(file);
        Check(Has(path, 'A') && (mode == 0 || Has(existing, 'X')) && !fs::exists(late),
            "rollback preserves base content and preexisting ADS after final close");
    }
}
}
int wmain(int argc, wchar_t** argv) {
    const bool pending_only = argc == 2 && wcscmp(argv[1], L"--pending-only") == 0;
    if (argc != 1 && !pending_only) return 2;
    GUID guid{}; wchar_t name[40]{};
    if (FAILED(CoCreateGuid(&guid)) || !StringFromGUID2(guid, name, 40)) return 1;
    const auto base = fs::absolute(L"bench_data").lexically_normal();
    const auto root = base / (std::wstring(L"replacement_backup_") + name);
    std::error_code ec;
    if (!fs::create_directories(root, ec) || ec) return 1;
    if (pending_only) {
        PendingDispositionCases(root);
        if (!failures) { fs::remove_all(root, ec); Check(!ec, "cleanup isolated pending probe fixture"); }
        else printf("[DIAG] retained pending probe fixture=%ls\n", root.c_str());
        return failures ? 1 : 0;
    }
    DWORD error = 0;
    for (int mode = 0; mode != 4; ++mode) {
        const auto original = root / (L"original" + std::to_wstring(mode));
        const auto backup = root / (L"backup" + std::to_wstring(mode));
        const auto retained = root / (L"retained" + std::to_wstring(mode));
        BoundReplacementBackup bound;
        Check(Write(original, 'A'), "create original fixture");
        const bool created = CreateBoundReplacementBackup(original.wstring(), backup.wstring(), bound, error);
        Check(created && !error && Has(backup, 'A') && !fs::exists(original), "bind and move exact backup object");
        if (!created) continue;
        if (mode == 0) {
            Check(RestoreBoundReplacementBackup(bound, error) && !error && Has(original, 'A') && !fs::exists(backup),
                "normal identity-bound restoration succeeds");
        } else if (mode == 1) {
            // Deterministically reproduce replacement between backup creation and cleanup/restore.
            Check(MoveFileExW(backup.c_str(), retained.c_str(), 0) && Write(backup, 'X'), "external writer replaces backup name");
            Check(!CleanupBoundReplacementBackup(bound, error) && error == ERROR_FILE_INVALID && Has(backup, 'X') && Has(retained, 'A'),
                "cleanup rejects substituted backup and preserves both objects");
            Check(!RestoreBoundReplacementBackup(bound, error) && error == ERROR_FILE_INVALID && !fs::exists(original) && Has(backup, 'X'),
                "restore rejects substituted backup without restoring external object");
        } else if (mode == 2) {
            Check(Write(original, 'X'), "external writer occupies restore destination");
            Check(!RestoreBoundReplacementBackup(bound, error) && error != ERROR_SUCCESS && Has(original, 'X') && Has(backup, 'A'),
                "restore does not overwrite new original occupant");
        } else {
            Check(CleanupBoundReplacementBackup(bound, error) && !error && !fs::exists(backup), "normal bound file cleanup succeeds");
        }
    }
    {
        const auto original = root / L"readonly-file", backup = root / L"readonly-backup";
        BoundReplacementBackup bound;
        Check(Write(original, 'R') && SetFileAttributesW(original.c_str(), FILE_ATTRIBUTE_READONLY),
            "create readonly ordinary file");
        const bool created = CreateBoundReplacementBackup(original.wstring(), backup.wstring(), bound, error);
        const DWORD attributes = GetFileAttributesW(backup.c_str());
        Check(created && !error && !fs::exists(original) && Has(backup, 'R') &&
            attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_READONLY),
            "readonly backup preserves content and readonly attribute");
        if (created) {
            Check(CleanupBoundReplacementBackup(bound, error) && !error && !fs::exists(backup),
                "bound readonly file cleanup clears attribute and deletes owned object");
        }
    }
    {
        const auto original = root / L"empty-directory", backup = root / L"empty-directory-backup";
        BoundReplacementBackup bound;
        Check(fs::create_directory(original), "create empty directory");
        const bool created = CreateBoundReplacementBackup(original.wstring(), backup.wstring(), bound, error);
        Check(created && !error && !fs::exists(original) && fs::is_directory(backup) && fs::is_empty(backup),
            "empty directory backup preserves exact empty directory");
        if (created) {
            Check(CleanupBoundReplacementBackup(bound, error) && !error && !fs::exists(backup),
                "bound empty directory cleanup succeeds without recursion");
        }
    }
    const auto directory = root / L"directory", backup = root / L"directory-backup";
    BoundReplacementBackup bound;
    Check(fs::create_directory(directory) && Write(directory / L"child", 'A'), "create nonempty directory fixture");
    const bool bound_directory = CreateBoundReplacementBackup(directory.wstring(), backup.wstring(), bound, error);
    Check(bound_directory, "bind directory backup");
    if (bound_directory) {
        Check(Write(backup / L"external-child", 'X'), "external writer adds child to same backup directory");
        Check(!CleanupBoundReplacementBackup(bound, error) && error == ERROR_DIR_NOT_EMPTY &&
            Has(backup / L"child", 'A') && Has(backup / L"external-child", 'X'), "nonempty directory cleanup preserves all children");
        Check(RestoreBoundReplacementBackup(bound, error) && Has(directory / L"child", 'A') && Has(directory / L"external-child", 'X'),
            "normal directory restoration renames intact object");
    }
    {
        const auto original = root / L"cancel-entry", backup = root / L"cancel-entry-backup";
        BoundReplacementBackup bound;
        std::atomic<bool> cancel{true};
        Check(Write(original, 'C'), "create cancellation entry fixture");
        Check(!CreateBoundReplacementBackup(original.wstring(), backup.wstring(), bound, error, &cancel) &&
            error == ERROR_CANCELLED && Has(original, 'C') && !fs::exists(backup), "cancelled create leaves original unchanged");
        cancel.store(false);
        const bool created = CreateBoundReplacementBackup(original.wstring(), backup.wstring(), bound, error, &cancel);
        Check(created, "create backup before cancelled cleanup");
        if (created) {
            cancel.store(true);
            Check(!CleanupBoundReplacementBackup(bound, error, &cancel) && error == ERROR_CANCELLED && Has(backup, 'C'),
                "cancelled cleanup leaves backup unchanged");
        }
    }
    if (!failures && root.parent_path() == base && root.filename().wstring().starts_with(L"replacement_backup_")) {
        fs::remove_all(root, ec); Check(!ec, "cleanup isolated GUID fixture");
    } else printf("[DIAG] retained fixture=%ls\n", root.c_str());
    return failures ? 1 : 0;
}
