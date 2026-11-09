#pragma once
#include <windows.h>
#include <cstdint>
#include <string>

namespace pulse::app {
// May block on remote providers. Call only after background admission.
inline bool ReadBreadcrumbRevision(const std::wstring& path, uint64_t& revision) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (path.empty() || !GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data) ||
        !(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) return false;
    revision = (static_cast<uint64_t>(data.ftLastWriteTime.dwHighDateTime) << 32) |
        data.ftLastWriteTime.dwLowDateTime;
    return true;
}
}
