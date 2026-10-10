// bitlocker_volume.h — recognizing a BitLocker volume that is still locked.
// Header-only so every target that lists drives can use it without new
// link dependencies.
#pragma once

#include <windows.h>
#include <cwctype>
#include <string>
#include <string_view>

namespace pulse::fs {

// Win32 file APIs on a locked BitLocker volume fail with FVE_E_LOCKED_VOLUME
// (0x80310000) as the thread's last error, for example right after a
// BitLocker-encrypted VHD is mounted. File Explorer then offers 解锁驱动器
// (the Drive "unlock-bde" verb) instead of opening the folder.
inline constexpr DWORD kBitLockerLockedError = 0x80310000u;

inline bool IsBitLockerLockedError(DWORD error) noexcept { return error == kBitLockerLockedError; }

// "W:\" for a local drive path (with or without the \\?\ prefix), empty for
// UNC, virtual and relative paths.
inline std::wstring DriveRootOf(std::wstring_view path) {
    if (path.starts_with(L"\\\\?\\")) path.remove_prefix(4);
    if (path.size() < 2 || path[1] != L':' || !iswalpha(path[0])) return {};
    if (path.size() > 2 && path[2] != L'\\' && path[2] != L'/') return {};
    return {static_cast<wchar_t>(towupper(path[0])), L':', L'\\'};
}

// File-system probe of the volume holding path; never shows UI. Removable
// media can stall, so call it off the UI thread.
inline bool IsBitLockerLocked(std::wstring_view path) {
    const std::wstring root = DriveRootOf(path);
    if (root.empty()) return false;
    DWORD previous = 0;
    const bool mode_set = SetThreadErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX, &previous) != FALSE;
    SetLastError(ERROR_SUCCESS);
    const bool locked = GetFileAttributesW(root.c_str()) == INVALID_FILE_ATTRIBUTES &&
                        IsBitLockerLockedError(GetLastError());
    if (mode_set) SetThreadErrorMode(previous, nullptr);
    return locked;
}

} // namespace pulse::fs
