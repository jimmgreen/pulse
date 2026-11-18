#pragma once
#include <optional>
#include <string_view>
namespace pulse::app {
inline bool IsWinEFastEntry(int argc, const wchar_t* const* argv) noexcept {
    if (!argv) return false;
    return (argc == 2 && argv[1] && std::wstring_view(argv[1]) == L"--win-e") ||
        (argc == 3 && argv[1] && argv[2] && std::wstring_view(argv[1]) == L"--shell-open" &&
         std::wstring_view(argv[2]) == L"pulse:win-e");
}
// Once delivery has been attempted its outcome may be ambiguous. Never launch
// another process or resend under a new identity after that boundary.
template<class Find, class Forward>
std::optional<int> TryWinEFastEntry(int argc, const wchar_t* const* argv, Find find, Forward forward) {
    if (!IsWinEFastEntry(argc, argv) || !find()) return std::nullopt;
    return forward() ? 0 : 1;
}
}
