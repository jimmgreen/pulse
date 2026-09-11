#include "video_codec.h"
#include <windows.h>
#include <objbase.h>
#include <algorithm>
#include <iterator>

namespace pulse::preview {

std::wstring VideoCodecDisplayName(std::wstring_view value) {
    const auto first = value.find_first_not_of(L" \t\r\n");
    if (first == std::wstring_view::npos) return {};
    value = value.substr(first, value.find_last_not_of(L" \t\r\n") - first + 1);
    std::wstring code(value);
    const bool identifier = value.front() == L'{' ||
        (value.size() == 36 && value[8] == L'-' && value[13] == L'-');
    if (identifier) {
        const std::wstring text = value.front() == L'{' ? code : L"{" + code + L"}";
        GUID subtype{};
        if (FAILED(CLSIDFromString(text.c_str(), &subtype))) return {};
        // Media Foundation/DirectShow encode a FOURCC in this GUID family's Data1.
        constexpr BYTE tail[] = {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71};
        if (subtype.Data2 != 0 || subtype.Data3 != 0x0010 ||
            !std::equal(std::begin(tail), std::end(tail), subtype.Data4)) return {};
        code.clear();
        for (unsigned i = 0; i < 4; ++i) {
            const auto ch = static_cast<wchar_t>((subtype.Data1 >> (i * 8)) & 0xff);
            if (ch < 0x20 || ch > 0x7e) return {};
            code += ch;
        }
    }
    std::wstring upper = code;
    for (auto& ch : upper) {
        if (ch >= L'a' && ch <= L'z') ch -= L'a' - L'A';
    }
    if (upper == L"H264" || upper == L"AVC1") return L"H.264 (AVC)";
    if (upper == L"HEVC" || upper == L"H265" || upper == L"HVC1" || upper == L"HEV1")
        return L"H.265 (HEVC)";
    if (upper == L"AV01") return L"AV1";
    if (upper == L"VP90") return L"VP9";
    if (upper == L"VP80") return L"VP8";
    if (upper == L"WVC1") return L"VC-1";
    if (upper == L"WMV3") return L"Windows Media Video 9";
    if (upper == L"MP4V" || upper == L"M4S2" || upper == L"MP4S") return L"MPEG-4 Part 2";
    if (upper == L"MJPG") return L"Motion JPEG";
    // Preserve readable provider names or an unmapped printable FOURCC.
    return code;
}

} // namespace pulse::preview
