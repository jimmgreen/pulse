#pragma once
#include <windows.h>
#include <string>
#include <string_view>
#include <utility>

// User-selected filesystem objects only. Never use this policy to authorize DLL loading.
namespace pulse::picker_user_storage {
struct Path {
    std::wstring value;
    std::wstring root;
    size_t root_length = 0;
    bool unc = false;
};
inline bool Parse(std::wstring_view input, Path& output) {
    output = {};
    std::wstring value(input);
    if (value.size() >= 8 && CompareStringOrdinal(value.data(), 8, L"\\\\?\\UNC\\", 8, TRUE) == CSTR_EQUAL)
        value = L"\\\\" + value.substr(8);
    else if (value.starts_with(L"\\\\?\\")) value.erase(0, 4);
    for (wchar_t c : value) if (c < 32 || std::wstring_view(L"/*?\"<>|").find(c) != std::wstring_view::npos) return false;
    size_t root_length = 0;
    bool unc = value.starts_with(L"\\\\");
    if (unc) {
        const auto server = value.find(L'\\', 2);
        if (server == value.npos || server == 2 || server + 1 == value.size()) return false;
        const auto share = value.find(L'\\', server + 1);
        root_length = share == value.npos ? value.size() : share;
        if (root_length == server + 1 || value.find(L':') != value.npos) return false;
    } else {
        if (value.size() < 3 || !((value[0] >= L'A' && value[0] <= L'Z') || (value[0] >= L'a' && value[0] <= L'z')) ||
            value[1] != L':' || value[2] != L'\\' || value.find(L':', 2) != value.npos) return false;
        root_length = 3;
    }
    for (size_t begin = unc ? 2 : 3; begin < value.size();) {
        const auto end = value.find(L'\\', begin);
        const auto part = std::wstring_view(value).substr(begin, end == value.npos ? value.size() - begin : end - begin);
        if (part.empty() || part == L"." || part == L"..") return false;
        if (end == value.npos) break;
        begin = end + 1;
    }
    while (value.size() > root_length && value.back() == L'\\') value.pop_back();
    output.value = std::move(value); output.root_length = root_length; output.unc = unc;
    output.root = output.value.substr(0, root_length);
    if (output.root.back() != L'\\') output.root += L'\\';
    return true;
}
inline bool SupportedDrive(UINT kind) {
    return kind == DRIVE_FIXED || kind == DRIVE_REMOVABLE || kind == DRIVE_REMOTE;
}
inline constexpr DWORD kUnsupportedAttributes = FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_OFFLINE | 0x00040000u | 0x00400000u;
// Callbacks run on the existing picker worker; injection tests never access actual remote media.
template<class Drive, class Attributes, class Cancelled>
DWORD CheckAncestors(const Path& path, bool file, Drive drive, Attributes attributes, Cancelled cancelled) {
    if (cancelled()) return ERROR_CANCELLED;
    const UINT kind = path.unc ? DRIVE_REMOTE : drive(path.root);
    if (kind == DRIVE_NO_ROOT_DIR) return ERROR_PATH_NOT_FOUND;
    if (kind == DRIVE_UNKNOWN) return ERROR_NOT_READY;
    if (!SupportedDrive(kind)) return ERROR_NOT_SUPPORTED;
    for (size_t end = path.root_length;;) {
        if (cancelled()) return ERROR_CANCELLED;
        DWORD flags = 0;
        const DWORD error = attributes(path.value.substr(0, end), flags);
        if (cancelled()) return ERROR_CANCELLED;
        if (error) return error;
        if (flags & kUnsupportedAttributes) return ERROR_NOT_SUPPORTED;
        if (end == path.value.size()) return file && (flags & FILE_ATTRIBUTE_DIRECTORY) ? ERROR_DIRECTORY : ERROR_SUCCESS;
        if (!(flags & FILE_ATTRIBUTE_DIRECTORY)) return ERROR_DIRECTORY;
        size_t begin = end;
        while (begin < path.value.size() && path.value[begin] == L'\\') ++begin;
        const auto next = path.value.find(L'\\', begin);
        end = next == path.value.npos ? path.value.size() : next;
    }
}
inline bool SamePath(std::wstring_view requested, std::wstring_view resolved) noexcept {
    try {
        Path left, right;
        return Parse(requested, left) && Parse(resolved, right) &&
            CompareStringOrdinal(left.value.c_str(), -1, right.value.c_str(), -1, TRUE) == CSTR_EQUAL;
    } catch (...) {
        return false; // Identity cannot be established; callers still close their probe handle.
    }
}
}
