#pragma once
#include "runtime_config.h"
#include <string>
#include <cwchar>
namespace pulse::shell_bridge::runtime {
// Positive authority: a failed parent cannot authorize UI in descendants merely
// because writing its negative disable marker failed.
inline DWORD ValidateRuntimeReadyReceipt(const std::wstring& directory, const Session& session) noexcept {
    try {
        const auto path = directory + L"\\" + kReadyFile;
        HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                  FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (file == INVALID_HANDLE_VALUE) return GetLastError();
        struct Close { HANDLE value; ~Close() { CloseHandle(value); } } close{file};
        BY_HANDLE_FILE_INFORMATION info{}; LARGE_INTEGER size{}; DWORD read = 0; Ready ready;
        if (!GetFileInformationByHandle(file, &info) || !GetFileSizeEx(file, &size)) return GetLastError();
        if ((info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) ||
            size.QuadPart != static_cast<LONGLONG>(sizeof(ready))) return ERROR_INVALID_DATA;
        if (!ReadFile(file, &ready, sizeof(ready), &read, nullptr)) return GetLastError();
        if (read != sizeof(ready) || ready.magic != kReadyMagic || ready.version != kVersion ||
            ready.size != sizeof(ready) || ready.reserved || ready.target_pid != session.target_pid ||
            ready.machine != session.machine || CompareFileTime(&ready.target_created, &session.target_created) ||
            ready.nonce[39] || std::wmemcmp(ready.nonce, session.nonce, 40) ||
            ready.status != S_OK || !(ready.installed_entries & kCoCreateInstance)) return ERROR_INVALID_DATA;
        return ERROR_SUCCESS;
    } catch (...) { return ERROR_NOT_ENOUGH_MEMORY; }
}
}
