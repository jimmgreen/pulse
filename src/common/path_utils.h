// path_utils.h — Shared path helpers. Header-only on purpose: preview_host /
// shell_host / index host are separate exes and must not gain link dependencies.
#pragma once

#include "windows_path_prefix.h"

#include <cwctype>
#include <string>
#include <string_view>
#include <windows.h>
#include <shellapi.h>

namespace pulse::path {

inline bool Exists(std::wstring_view p) {
    const std::wstring path(p);
    return !path.empty() && GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

inline bool EqualInsensitive(std::wstring_view left, std::wstring_view right) {
    return CompareStringOrdinal(left.data(), static_cast<int>(left.size()),
                                right.data(), static_cast<int>(right.size()), TRUE) == CSTR_EQUAL;
}

// One key for all spellings _wcsicmp calls equal (it lowers each character
// alike), so paths can go in hash sets.
inline std::wstring FoldedKey(std::wstring_view p) {
    std::wstring key(p);
    for (wchar_t& c : key) c = static_cast<wchar_t>(std::towlower(c));
    return key;
}

// Shell APIs (SHCreateItemFromParsingName, IFileOperation, SHGetFileInfo)
// reject \\?\ extended paths even for short paths, while the fs layer hands
// them out for long-path support. \\?\UNC\server\share -> \\server\share,
// \\?\C:\dir -> C:\dir.
inline std::wstring StripExtendedPathPrefix(std::wstring_view p) {
    switch (detail::ClassifyExtendedDosPrefix(p)) {
    case detail::ExtendedDosPrefix::Unc:
        return std::wstring(L"\\\\") + std::wstring(p.substr(8));
    case detail::ExtendedDosPrefix::Drive: p.remove_prefix(4); break;
    case detail::ExtendedDosPrefix::None: break;
    }
    return std::wstring(p);
}

} // namespace pulse::path
