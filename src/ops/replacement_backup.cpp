#include "replacement_backup.h"
#include "owned_transfer_output.h"
#include <utility>

namespace pulse::ops {
namespace replacement_backup_detail {
struct Handle {
    HANDLE value = INVALID_HANDLE_VALUE;
    ~Handle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
inline bool Rename(HANDLE file, const std::wstring& destination, DWORD& error) {
    if (destination.empty() || destination.size() > 32767) { error = ERROR_INVALID_NAME; return false; }
    const DWORD bytes = static_cast<DWORD>(destination.size() * sizeof(wchar_t));
    std::vector<unsigned char> storage(sizeof(FILE_RENAME_INFO) + bytes, 0);
    auto* rename = reinterpret_cast<FILE_RENAME_INFO*>(storage.data());
    rename->ReplaceIfExists = FALSE;
    rename->FileNameLength = bytes;
    memcpy(rename->FileName, destination.data(), bytes);
    if (!SetFileInformationByHandle(file, FileRenameInfo, rename, static_cast<DWORD>(storage.size()))) {
        error = GetLastError(); return false;
    }
    return true;
}
inline HANDLE Open(const std::wstring& path, DWORD access) {
    return CreateFileW(path.c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
        OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
}
inline bool Matches(HANDLE file, const VerificationStamp& expected, DWORD& error) {
    VerificationStamp actual{};
    if (!CaptureOwnedOutputStamp(file, actual)) { error = GetLastError(); if (!error) error = ERROR_FILE_INVALID; return false; }
    if ((!expected.index_high && !expected.index_low) || actual.volume != expected.volume ||
        actual.index_high != expected.index_high || actual.index_low != expected.index_low) {
        error = ERROR_FILE_INVALID; return false;
    }
    return true;
}
} // namespace replacement_backup_detail

// Same-volume absolute paths. Identity is captured before rename under the same
// handle, so a replacement winning the backup name cannot become our backup.
__declspec(noinline) bool CreateBoundReplacementBackup(const std::wstring& original, const std::wstring& backup,
                                        BoundReplacementBackup& result, DWORD& error, const std::atomic<bool>* cancel) {
    using namespace replacement_backup_detail;
    error = ERROR_SUCCESS;
    if (cancel && cancel->load()) { error = ERROR_CANCELLED; return false; }
    Handle file{Open(original, DELETE | FILE_READ_ATTRIBUTES)};
    if (file.value == INVALID_HANDLE_VALUE) { error = GetLastError(); return false; }
    BoundReplacementBackup captured{original, backup, {}};
    if (!CaptureOwnedOutputStamp(file.value, captured.identity)) {
        error = GetLastError(); if (!error) error = ERROR_FILE_INVALID; return false;
    }
    if (!Rename(file.value, backup, error)) return false;
    result = std::move(captured);
    return true;
}

// Never removes/overwrites the original name: a new occupant requires separate
// proven ownership, which this backup record deliberately does not claim.
__declspec(noinline) bool RestoreBoundReplacementBackup(const BoundReplacementBackup& backup, DWORD& error) {
    using namespace replacement_backup_detail;
    error = ERROR_SUCCESS;
    Handle file{Open(backup.backup, DELETE | FILE_READ_ATTRIBUTES)};
    if (file.value == INVALID_HANDLE_VALUE) { error = GetLastError(); return false; }
    return Matches(file.value, backup.identity, error) && Rename(file.value, backup.original, error);
}

// Files/reparse points and empty directories only. Nonempty directories remain
// intact: binding their root identity does not authorize deleting their children.
__declspec(noinline) bool CleanupBoundReplacementBackup(const BoundReplacementBackup& backup, DWORD& error, const std::atomic<bool>* cancel) {
    using namespace replacement_backup_detail;
    error = ERROR_SUCCESS;
    if (cancel && cancel->load()) { error = ERROR_CANCELLED; return false; }
    Handle file{Open(backup.backup, DELETE | FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES)};
    if (file.value == INVALID_HANDLE_VALUE) { error = GetLastError(); return false; }
    if (!Matches(file.value, backup.identity, error)) return false;
    FILE_BASIC_INFO basic{};
    if (!GetFileInformationByHandleEx(file.value, FileBasicInfo, &basic, sizeof(basic))) {
        error = GetLastError(); return false;
    }
    const DWORD attributes = basic.FileAttributes;
    const bool readonly = (attributes & FILE_ATTRIBUTE_READONLY) != 0;
    if (readonly) {
        FILE_BASIC_INFO writable{};
        writable.FileAttributes = attributes & ~FILE_ATTRIBUTE_READONLY;
        if (!writable.FileAttributes) writable.FileAttributes = FILE_ATTRIBUTE_NORMAL;
        if (!SetFileInformationByHandle(file.value, FileBasicInfo, &writable, sizeof(writable))) {
            error = GetLastError(); return false;
        }
    }
    FILE_DISPOSITION_INFO disposition{TRUE};
    if (!SetFileInformationByHandle(file.value, FileDispositionInfo, &disposition, sizeof(disposition))) {
        error = GetLastError();
        if (readonly) {
            FILE_BASIC_INFO restore{}; restore.FileAttributes = attributes;
            SetFileInformationByHandle(file.value, FileBasicInfo, &restore, sizeof(restore));
        }
        return false;
    }
    return true;
}
} // namespace pulse::ops
