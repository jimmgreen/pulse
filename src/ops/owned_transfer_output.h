#pragma once
#include "verification_worker.h"
#include <cstddef>
#include <cstring>
#include <vector>

namespace pulse::ops {
// Capture from the actual CopyFile2 destination handle, never by reopening its name.
inline bool CaptureOwnedOutputStamp(HANDLE file, VerificationStamp& stamp) {
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(file, &info) || (!info.nFileIndexHigh && !info.nFileIndexLow)) return false;
    stamp.volume = info.dwVolumeSerialNumber;
    stamp.index_high = info.nFileIndexHigh;
    stamp.index_low = info.nFileIndexLow;
    stamp.bytes = (static_cast<uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
    stamp.last_write = info.ftLastWriteTime;
    return true;
}

// Returns whether the rename committed; metadata including read-only is unchanged.
// destination must be an absolute path on the same volume as staging.
inline bool CommitVerifiedOutput(const std::wstring& staging, const std::wstring& destination,
                                 const VerificationStamp& owned, DWORD& error) {
    error = ERROR_SUCCESS;
    if (destination.empty() || destination.size() > 32767) { error = ERROR_INVALID_NAME; return false; }
    const DWORD name_bytes = static_cast<DWORD>(destination.size() * sizeof(wchar_t));
    std::vector<unsigned char> storage(sizeof(FILE_RENAME_INFO) + name_bytes, 0);
    auto* rename = reinterpret_cast<FILE_RENAME_INFO*>(storage.data());
    rename->ReplaceIfExists = FALSE;
    rename->RootDirectory = nullptr;
    rename->FileNameLength = name_bytes;
    memcpy(rename->FileName, destination.data(), name_bytes);
    HANDLE file = CreateFileW(staging.c_str(), DELETE | FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file == INVALID_HANDLE_VALUE) { error = GetLastError(); return false; }
    BY_HANDLE_FILE_INFORMATION actual{};
    if (!GetFileInformationByHandle(file, &actual)) error = GetLastError();
    else if ((!owned.index_high && !owned.index_low) || actual.dwVolumeSerialNumber != owned.volume ||
        actual.nFileIndexHigh != owned.index_high || actual.nFileIndexLow != owned.index_low)
        error = ERROR_FILE_INVALID;
    bool committed = false;
    if (!error) {
        committed = SetFileInformationByHandle(file, FileRenameInfo, rename, static_cast<DWORD>(storage.size())) != FALSE;
        if (!committed) error = GetLastError();
    }
    CloseHandle(file);
    return committed;
}

inline bool DeleteVerifiedOutput(const std::wstring& path, const VerificationStamp& owned, DWORD& error) {
    error = ERROR_SUCCESS;
    HANDLE file = CreateFileW(path.c_str(), DELETE | FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        error = GetLastError();
        // CopyFile2 can already have removed its failed output. A retry still
        // uses FAIL_IF_EXISTS, so a new owner winning the name is never overwritten.
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
            error = ERROR_SUCCESS;
            return true;
        }
        return false;
    }
    BY_HANDLE_FILE_INFORMATION actual{};
    if (!GetFileInformationByHandle(file, &actual)) error = GetLastError();
    else if ((!owned.index_high && !owned.index_low) || actual.dwVolumeSerialNumber != owned.volume ||
        actual.nFileIndexHigh != owned.index_high || actual.nFileIndexLow != owned.index_low)
        error = ERROR_FILE_INVALID;
    // The callback precedes final metadata application: only identity is stable.
    // No DELETE sharing pins this name/object relation throughout disposition.
    bool changed_attributes = false;
    if (!error && (actual.dwFileAttributes & FILE_ATTRIBUTE_READONLY)) {
        FILE_BASIC_INFO basic{};
        basic.FileAttributes = actual.dwFileAttributes & ~FILE_ATTRIBUTE_READONLY;
        if (!basic.FileAttributes) basic.FileAttributes = FILE_ATTRIBUTE_NORMAL;
        if (!SetFileInformationByHandle(file, FileBasicInfo, &basic, sizeof(basic))) error = GetLastError();
        else changed_attributes = true;
    }
    if (!error) {
        FILE_DISPOSITION_INFO disposition{TRUE};
        if (!SetFileInformationByHandle(file, FileDispositionInfo, &disposition, sizeof(disposition))) error = GetLastError();
    }
    if (error && changed_attributes) {
        FILE_BASIC_INFO restore{};
        restore.FileAttributes = actual.dwFileAttributes;
        SetFileInformationByHandle(file, FileBasicInfo, &restore, sizeof(restore));
    }
    CloseHandle(file);
    return error == ERROR_SUCCESS;
}
} // namespace pulse::ops
