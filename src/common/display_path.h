#pragma once

#include "windows_path_prefix.h"

#include <string>
#include <string_view>

namespace pulse::path {

// Presentation only: preserve the original paths used for filesystem operations.
inline std::wstring FriendlyPathText(std::wstring_view text) {
    std::wstring result(text);
    for (const std::wstring_view prefix : {std::wstring_view(L"\\\\?\\"),
                                          std::wstring_view(L"\\??\\")}) {
        size_t pos = 0;
        while ((pos = result.find(prefix, pos)) != std::wstring::npos) {
            const auto kind = detail::ClassifyExtendedDosPrefix(std::wstring_view(result).substr(pos));
            if (kind == detail::ExtendedDosPrefix::Unc) {
                result.replace(pos, 8, L"\\\\");
                pos += 2;
            } else if (kind == detail::ExtendedDosPrefix::Drive) {
                result.erase(pos, 4);
            } else {
                pos += prefix.size();
            }
        }
    }
    return result;
}

} // namespace pulse::path
