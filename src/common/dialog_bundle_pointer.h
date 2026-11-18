#pragma once
#include <windows.h>

namespace pulse::dialog_bundle::detail {
// Atomic publication may replace the directory entry while this handle retains
// a complete old snapshot. In-place writes remain forbidden.
inline HANDLE OpenPointerSnapshot(const wchar_t* path) noexcept {
    return CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
}
}
