#pragma once
#include "bounded_enumeration.h"
#include <limits>
#include <string>

namespace pulse::fs {
// Synchronous provider: call behind the bounded provider layer. Retains one
// native find record irrespective of the number of children.
inline int CountDirectoryChildren(const std::wstring& path, bool show_hidden,
                                  bool show_protected, const std::atomic_bool& cancel) {
    if (cancel) throw EnumerationCancelled();
    std::wstring pattern = path;
    if (!pattern.empty() && pattern.back() != L'\\') pattern += L'\\';
    pattern += L'*';
    WIN32_FIND_DATAW data{};
    const HANDLE handle = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &data,
        FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (handle == INVALID_HANDLE_VALUE) {
        if (GetLastError() == ERROR_FILE_NOT_FOUND) return 0;
        throw std::runtime_error("child count unavailable");
    }
    struct FindGuard { HANDLE handle; ~FindGuard() { FindClose(handle); } } guard{handle};
    int total = 0;
    do {
        if (cancel) throw EnumerationCancelled();
        if (wcscmp(data.cFileName, L".") == 0 || wcscmp(data.cFileName, L"..") == 0) continue;
        const DWORD attrs = data.dwFileAttributes;
        if ((attrs & FILE_ATTRIBUTE_HIDDEN) &&
            (!show_hidden || ((attrs & FILE_ATTRIBUTE_SYSTEM) && !show_protected))) continue;
        if (total == (std::numeric_limits<int>::max)()) throw std::runtime_error("child count overflow");
        ++total;
    } while (FindNextFileW(handle, &data));
    if (GetLastError() != ERROR_NO_MORE_FILES) throw std::runtime_error("child count incomplete");
    if (cancel) throw EnumerationCancelled();
    return total;
}
}
