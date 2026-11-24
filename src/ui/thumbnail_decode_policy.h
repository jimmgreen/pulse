#pragma once
#include "../common/preview_extensions.h"
#include <array>
#include <cstdint>
#include <string_view>

namespace pulse::ui {
// WIC selects by content, not suffix. Version the image-route namespace so a
// JPEG named .png cannot retain the old EXIF 5..8 transposed resize dimensions.
// Unknown suffixes use RunSniffed without grid_fast and keep their old keys.
inline uint32_t ThumbnailDecodePolicy(std::wstring_view path) noexcept {
    const auto dot = path.find_last_of(L'.');
    if (dot == std::wstring_view::npos) return 0;
    const auto extension = path.substr(dot);
    std::array<wchar_t, 32> lower{};
    if (extension.size() > lower.size()) return 0;
    for (size_t i = 0; i < extension.size(); ++i) {
        const wchar_t ch = extension[i];
        lower[i] = ch >= L'A' && ch <= L'Z' ? ch + (L'a' - L'A') : ch;
    }
    return preview::IsImageExtension({lower.data(), extension.size()}) ? 0x45584931u : 0;
}
} // namespace pulse::ui
