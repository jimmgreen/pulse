#pragma once
#include "../ipc/preview_protocol.h"
#include <span>

namespace pulse::preview {

inline ipc::PreviewTextEncoding Utf32Bom(std::span<const uint8_t> bytes) {
    if (bytes.size() >= 4 && bytes[0] == 0xFF && bytes[1] == 0xFE && !bytes[2] && !bytes[3])
        return ipc::PreviewTextEncoding::Utf32Le;
    if (bytes.size() >= 4 && !bytes[0] && !bytes[1] && bytes[2] == 0xFE && bytes[3] == 0xFF)
        return ipc::PreviewTextEncoding::Utf32Be;
    return ipc::PreviewTextEncoding::Unknown;
}

inline void AppendUtf32(std::span<const uint8_t> bytes, ipc::PreviewTextEncoding utf32, bool truncated,
                        std::wstring& text) {
    const bool le = utf32 == ipc::PreviewTextEncoding::Utf32Le;
    text.reserve((bytes.size() - 4) / 4);
    size_t i = 4;
    for (; i + 3 < bytes.size(); i += 4) {
        uint32_t cp = 0;
        for (size_t j = 0; j < 4; ++j) cp = (cp << 8) | bytes[i + (le ? 3 - j : j)];
        if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) cp = 0xFFFD;
        if (cp > 0xFFFF) {
            cp -= 0x10000;
            text.push_back(static_cast<wchar_t>(0xD800 + (cp >> 10)));
            cp = 0xDC00 + (cp & 0x3FF);
        }
        text.push_back(static_cast<wchar_t>(cp));
    }
    if (i < bytes.size() && !truncated) text.push_back(static_cast<wchar_t>(0xFFFD));
}

} // namespace pulse::preview
