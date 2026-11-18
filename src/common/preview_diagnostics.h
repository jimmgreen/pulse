#pragma once
#include <cstdint>
#include <string_view>

namespace pulse::preview {
inline std::wstring_view DiagnosticExtension(std::wstring_view path) {
    const auto dot = path.find_last_of(L'.');
    const auto slash = path.find_last_of(L"\\/");
    return dot != std::wstring_view::npos && (slash == std::wstring_view::npos || dot > slash)
        ? path.substr(dot) : std::wstring_view{};
}
// Hash the exact UTF-16 path carried by IPC so both processes can correlate
// requests without recording file names. This is an identifier, not a secret.
inline uint64_t DiagnosticPathId(std::wstring_view path) {
    uint64_t hash = 14695981039346656037ull;
    for (const wchar_t unit : path) {
        hash = (hash ^ (static_cast<uint16_t>(unit) & 0xffu)) * 1099511628211ull;
        hash = (hash ^ (static_cast<uint16_t>(unit) >> 8)) * 1099511628211ull;
    }
    return hash;
}
}
