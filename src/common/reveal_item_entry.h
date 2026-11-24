#pragma once

#include <string_view>

namespace pulse::reveal_item_entry {
inline constexpr std::wstring_view kArgument = L"--reveal-item";
inline constexpr std::wstring_view kIntent = L"pulse:reveal-item:";

inline bool ValidTarget(std::wstring_view path) {
    if (path.size() <= 3 || path.size() + kIntent.size() >= 32768 ||
        !((path[0] >= L'A' && path[0] <= L'Z') || (path[0] >= L'a' && path[0] <= L'z')) ||
        path[1] != L':' || path[2] != L'\\') return false;
    path.remove_prefix(3);
    while (true) {
        const size_t separator = path.find(L'\\');
        const auto component = path.substr(0, separator);
        if (component.empty() || component.back() == L'.' || component.back() == L' ') return false;
        for (const auto ch : component) {
            if (ch < L' ' || std::wstring_view(L"\"*/:<>?|").find(ch) != std::wstring_view::npos) return false;
        }
        if (separator == std::wstring_view::npos) return true;
        path.remove_prefix(separator + 1);
    }
}
}
