// path_utils.h — Shared path helpers. Header-only on purpose: preview_host /
// shell_host / index host are separate exes and must not gain link dependencies.
#pragma once

#include <string>
#include <string_view>

namespace pulse::path {

// Shell APIs (SHCreateItemFromParsingName, IFileOperation, SHGetFileInfo)
// reject \\?\ extended paths even for short paths, while the fs layer hands
// them out for long-path support. \\?\UNC\server\share -> \\server\share,
// \\?\C:\dir -> C:\dir.
inline std::wstring StripExtendedPathPrefix(std::wstring_view p) {
    if (p.starts_with(L"\\\\?\\UNC\\"))
        return std::wstring(L"\\\\") + std::wstring(p.substr(8));
    if (p.starts_with(L"\\\\?\\")) return std::wstring(p.substr(4));
    return std::wstring(p);
}

} // namespace pulse::path
