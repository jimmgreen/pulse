#pragma once
#include <filesystem>
#include <string_view>

namespace archive_paths {
inline bool Separator(wchar_t ch) { return ch == L'\\' || ch == L'/'; }

// MSVC filesystem parses the extended drive prefix as a UNC server name,
// so parent_path can discard the slash that makes a drive root absolute.
inline size_t RootLength(std::wstring_view path) {
    const auto drive = [](wchar_t ch) { return (ch >= L'A' && ch <= L'Z') || (ch >= L'a' && ch <= L'z'); };
    if (path.size() >= 3 && drive(path[0]) && path[1] == L':' && Separator(path[2])) return 3;
    if (path.size() >= 7 && path.substr(0, 4) == L"\\\\?\\" && drive(path[4]) && path[5] == L':' && path[6] == L'\\') return 7;
    size_t server = 0;
    if (path.size() >= 8 && path.substr(0, 4) == L"\\\\?\\" &&
        (path[4] == L'U' || path[4] == L'u') && (path[5] == L'N' || path[5] == L'n') &&
        (path[6] == L'C' || path[6] == L'c') && path[7] == L'\\') server = 8;
    else if (path.size() > 2 && Separator(path[0]) && Separator(path[1]) && path[2] != L'?' && path[2] != L'.') server = 2;
    if (!server) return 0; // Includes genuine drive-relative paths, e.g. E:foo.
    const auto share = path.find_first_of(L"\\/", server);
    if (share == std::wstring_view::npos || share == server || share + 1 == path.size()) return 0;
    const auto end = path.find_first_of(L"\\/", share + 1);
    if (end == share + 1) return 0;
    return end == std::wstring_view::npos ? path.size() : end + 1;
}

inline std::filesystem::path Parent(const std::filesystem::path& path) {
    const auto value = path.wstring();
    const auto root = RootLength(value);
    if (!root) return path.parent_path();
    if (value.size() <= root) return path;
    const auto separator = value.find_last_of(L"\\/");
    return std::filesystem::path(value.substr(0, separator < root ? root : separator));
}
inline std::wstring ParentArchiveName(const std::filesystem::path& path) {
    const auto parent = Parent(path);
    const auto value = parent.wstring();
    if (value.empty() || RootLength(value) == value.size()) return L"Archive";
    const auto name = parent.filename().wstring();
    return name.empty() ? L"Archive" : name;
}
}
