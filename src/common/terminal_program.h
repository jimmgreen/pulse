#pragma once
#include <string_view>

namespace pulse::common {
// A literal executable name/path, never a shell command or argument template.
// Empty retains the built-in Windows Terminal choice.
inline bool IsTerminalProgramValid(std::wstring_view program) {
    if (program.empty()) return true;
    for (const wchar_t c : program)
        if (c < L' ' || c == L'"' || c == L'<' || c == L'>' || c == L'|' || c == L'*') return false;
    const bool extended = program.starts_with(L"\\\\?\\");
    const bool extended_unc = program.starts_with(L"\\\\?\\UNC\\");
    const size_t start = extended_unc ? 8 : extended ? 4 : 0;
    if (program.find(L'?', start) != std::wstring_view::npos) return false;
    const auto path = program.substr(start);
    const auto colon = path.find(L':');
    if (colon != std::wstring_view::npos && (colon != 1 || path.size() < 3 ||
        !((path[0] >= L'A' && path[0] <= L'Z') || (path[0] >= L'a' && path[0] <= L'z')) ||
        (path[2] != L'\\' && path[2] != L'/') || path.find(L':', colon + 1) != std::wstring_view::npos)) return false;
    const auto separator = path.find_last_of(L"\\/");
    const bool unc = extended_unc || path.starts_with(L"\\\\");
    if ((separator != std::wstring_view::npos || extended) && colon != 1 && !unc) return false;
    const auto name = separator == std::wstring_view::npos ? path : path.substr(separator + 1);
    if (name.size() <= 4) return false;
    const auto extension = name.substr(name.size() - 4);
    return extension[0] == L'.' && (extension[1] == L'e' || extension[1] == L'E') &&
        (extension[2] == L'x' || extension[2] == L'X') && (extension[3] == L'e' || extension[3] == L'E');
}
}
