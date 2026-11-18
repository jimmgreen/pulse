#pragma once
#include "rename_identity_types.h"
#include <winternl.h>
#include <filesystem>
#include <string>
#include <vector>
#include <cstring>

namespace pulse::ops {
namespace rename_detail {
struct Handle {
    HANDLE value;
    explicit Handle(HANDLE h) : value(h) {}
    ~Handle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
};
inline DWORD Read(HANDLE handle, RenameFileIdentity& identity) {
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(handle, &info)) return GetLastError();
    identity = {info.dwVolumeSerialNumber, info.nFileIndexHigh, info.nFileIndexLow};
    return ERROR_SUCCESS;
}
}
inline DWORD CaptureRenameIdentity(const std::wstring& source, RenameIdentity& identity) {
    const auto parent = std::filesystem::path(source).parent_path().wstring();
    rename_detail::Handle directory(CreateFileW(parent.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS, nullptr));
    if (directory.value == INVALID_HANDLE_VALUE) return GetLastError();
    rename_detail::Handle file(CreateFileW(source.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (file.value == INVALID_HANDLE_VALUE) return GetLastError();
    DWORD error = rename_detail::Read(directory.value, identity.parent);
    return error ? error : rename_detail::Read(file.value, identity.source);
}
// Keep both validated objects open without delete sharing until the rename ends.
inline DWORD RenameIdentified(const std::wstring& source, const std::wstring& name,
                              const RenameIdentity& expected) {
    const auto parent = std::filesystem::path(source).parent_path().wstring();
    rename_detail::Handle directory(CreateFileW(parent.c_str(), FILE_READ_ATTRIBUTES | FILE_LIST_DIRECTORY,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr));
    if (directory.value == INVALID_HANDLE_VALUE) return GetLastError();
    rename_detail::Handle file(CreateFileW(source.c_str(), DELETE | FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (file.value == INVALID_HANDLE_VALUE) return GetLastError();
    RenameIdentity actual;
    DWORD error = rename_detail::Read(directory.value, actual.parent);
    if (!error) error = rename_detail::Read(file.value, actual.source);
    if (error) return error;
    if (!(actual.source == expected.source) || !(actual.parent == expected.parent)) return ERROR_FILE_INVALID;
    const DWORD bytes = static_cast<DWORD>(name.size() * sizeof(wchar_t));
    std::vector<unsigned char> buffer(sizeof(FILE_RENAME_INFO) + bytes);
    auto* info = reinterpret_cast<FILE_RENAME_INFO*>(buffer.data());
    info->ReplaceIfExists = FALSE;
    info->RootDirectory = directory.value;
    info->FileNameLength = bytes;
    std::memcpy(info->FileName, name.data(), bytes);
    // Win32 rejects a relative rename with RootDirectory on some Windows builds.
    // The native operation keeps resolution bound to our validated directory.
    using SetInformation = NTSTATUS (NTAPI*)(HANDLE, PIO_STATUS_BLOCK, PVOID, ULONG, FILE_INFORMATION_CLASS);
    using StatusError = ULONG (WINAPI*)(NTSTATUS);
    const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    const auto set_information = reinterpret_cast<SetInformation>(GetProcAddress(ntdll, "NtSetInformationFile"));
    const auto status_error = reinterpret_cast<StatusError>(GetProcAddress(ntdll, "RtlNtStatusToDosError"));
    if (!set_information || !status_error) return ERROR_NOT_SUPPORTED;
    IO_STATUS_BLOCK status{};
    const NTSTATUS result = set_information(file.value, &status, info,
        static_cast<ULONG>(buffer.size()), static_cast<FILE_INFORMATION_CLASS>(10)); // FileRenameInformation
    return result >= 0 ? ERROR_SUCCESS : status_error(result);
}
}
