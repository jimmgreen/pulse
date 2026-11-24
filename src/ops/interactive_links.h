// interactive_links.h — Where a reparse point leads, for lane admission.
//
// A junction, symbolic link or volume mount point names another place: the
// scheduler adds that place (resolved, as \\?\Volume{...}\path) to the
// operation's footprint instead of keeping the operation out of the lanes.
// Cloud-file placeholders (OneDrive), deduplicated and WOF-compressed items
// are reparse points that stay where they are: they count as plain. Anything
// else (app execution aliases, unknown tags, links that cannot be resolved or
// lead to a share) proves nothing and keeps the operation queued.
#pragma once
#include <windows.h>
#include <winioctl.h>
#include <string>

namespace pulse::ops {

enum class InteractiveReparse { Plain, Link, Unsafe };

inline InteractiveReparse ClassifyInteractiveReparseTag(DWORD tag) {
    if (IsReparseTagNameSurrogate(tag)) return InteractiveReparse::Link;
    // The cloud tags carry a per-provider nibble (IO_REPARSE_TAG_CLOUD_1..F).
    if ((tag & 0xFFFF0FFFu) == IO_REPARSE_TAG_CLOUD) return InteractiveReparse::Plain;
    switch (tag) {
    case IO_REPARSE_TAG_DEDUP:
    case IO_REPARSE_TAG_WOF:
    case IO_REPARSE_TAG_ONEDRIVE:
    case IO_REPARSE_TAG_STORAGE_SYNC:
        return InteractiveReparse::Plain;
    default:
        return InteractiveReparse::Unsafe;
    }
}

// `api_path`: an item known (or found) to carry FILE_ATTRIBUTE_REPARSE_POINT.
inline InteractiveReparse ClassifyInteractiveReparse(const std::wstring& api_path) {
    WIN32_FIND_DATAW data{};
    HANDLE find = FindFirstFileExW(api_path.c_str(), FindExInfoBasic, &data, FindExSearchNameMatch, nullptr, 0);
    if (find == INVALID_HANDLE_VALUE) return InteractiveReparse::Unsafe;
    FindClose(find);
    if (!(data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) return InteractiveReparse::Plain;
    return ClassifyInteractiveReparseTag(data.dwReserved0);
}

inline bool IsVolumeGuidPath(const std::wstring& path) {
    return path.size() > 11 && _wcsnicmp(path.c_str(), L"\\\\?\\Volume{", 11) == 0;
}

// The place a link leads to, fully resolved, as \\?\Volume{...}\path without a
// trailing separator (the volume root keeps its own). Empty: not proven
// (dangling, no access, or a network location).
inline std::wstring ResolveInteractiveLink(const std::wstring& api_path) {
    HANDLE handle = CreateFileW(api_path.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return {};
    std::wstring out(512, L'\0');
    DWORD length = GetFinalPathNameByHandleW(handle, out.data(), static_cast<DWORD>(out.size()),
                                             FILE_NAME_NORMALIZED | VOLUME_NAME_GUID);
    if (length >= out.size()) {
        out.assign(static_cast<size_t>(length) + 1, L'\0');
        length = GetFinalPathNameByHandleW(handle, out.data(), static_cast<DWORD>(out.size()),
                                           FILE_NAME_NORMALIZED | VOLUME_NAME_GUID);
    }
    CloseHandle(handle);
    if (length == 0 || length >= out.size()) return {};
    out.resize(length);
    if (!IsVolumeGuidPath(out)) return {};
    const size_t root = out.find(L'\\', 11);  // after \\?\Volume{...}
    while (out.size() > root + 1 && out.back() == L'\\') out.pop_back();
    return out;
}

}  // namespace pulse::ops
