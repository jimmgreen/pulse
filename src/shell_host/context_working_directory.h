#pragma once

#include "../common/path_utils.h"

namespace pulse::shell {

inline std::wstring ContextWorkingDirectory(std::wstring_view path, bool background) {
    std::wstring dir = pulse::path::StripExtendedPathPrefix(path);
    if (dir.starts_with(L"::")) return {};
    if (!background) {
        const auto slash = dir.find_last_of(L'\\');
        if (slash != std::wstring::npos) {
            const bool drive_root = slash == 2 && dir[1] == L':';
            dir.resize(drive_root ? 3 : slash);
        }
    }
    return dir;
}

} // namespace pulse::shell
