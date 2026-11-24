#pragma once

#include <string_view>

namespace pulse::path::detail {

// Device and volume-GUID namespaces have no ordinary DOS/UNC spelling.
enum class ExtendedDosPrefix { None, Drive, Unc };

inline constexpr ExtendedDosPrefix ClassifyExtendedDosPrefix(std::wstring_view path) noexcept {
    if (!path.starts_with(L"\\\\?\\") && !path.starts_with(L"\\??\\")) return ExtendedDosPrefix::None;
    path.remove_prefix(4);
    if (path.size() >= 4 && (path[0] == L'U' || path[0] == L'u') &&
        (path[1] == L'N' || path[1] == L'n') && (path[2] == L'C' || path[2] == L'c') && path[3] == L'\\')
        return ExtendedDosPrefix::Unc;
    if (path.size() >= 3 && path[1] == L':' && path[2] == L'\\' &&
        ((path[0] >= L'A' && path[0] <= L'Z') || (path[0] >= L'a' && path[0] <= L'z')))
        return ExtendedDosPrefix::Drive;
    return ExtendedDosPrefix::None;
}

} // namespace pulse::path::detail
