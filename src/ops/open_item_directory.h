#pragma once
#include "../common/path_utils.h"
#include "../fs/drive_kind.h"
#include <string>

namespace pulse::ops {
inline std::wstring OpenItemWorkingDirectory(const std::wstring& path,
    decltype(&GetFileAttributesW) attributes = GetFileAttributesW,
    bool (*remote_drive)(std::wstring_view) = fs::IsRemoteLikeDrive) {
    const std::wstring plain = pulse::path::StripExtendedPathPrefix(path);
    if (plain.size() < 3 || plain.rfind(L"::", 0) == 0) return {};
    const size_t slash = plain.find_last_of(L"\\/");
    if (slash == std::wstring::npos || slash == 0) return {};
    std::wstring dir = plain.substr(0, slash);
    if (dir.size() == 2 && dir[1] == L':') dir += L'\\';
    // The selected item's parent is already known. Do not reopen a remote
    // directory just to choose a working directory before Shell opens the file.
    // Shell still validates the actual target and reports any access failure.
    if (plain.starts_with(L"\\\\") || remote_drive(plain)) return dir;
    const DWORD attrs = attributes(dir.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) ? dir : std::wstring();
}
}
