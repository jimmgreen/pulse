#pragma once
#include <windows.h>
#include <string>
#include <string_view>
#include <utility>

namespace pulse {
inline constexpr size_t kSearchUriLimit = 32767;
inline constexpr size_t kSearchKeywordLimit = 4096;

inline bool IsPulseUri(std::wstring_view text) {
    constexpr std::wstring_view prefix = L"pulse://";
    if (text.size() < prefix.size()) return false;
    for (size_t i = 0; i < prefix.size(); ++i) {
        auto c = text[i];
        if (c >= L'A' && c <= L'Z') c += L'a' - L'A';
        if (c != prefix[i]) return false;
    }
    return true;
}

// Decode exactly once. Query/fragment delimiters must be escaped when part of
// a keyword; '+' is a literal plus, not form-url-encoded whitespace.
inline bool ParseSearchUri(std::wstring_view uri, std::wstring& keyword) {
    keyword.clear();
    if (!IsPulseUri(uri) || uri.size() > kSearchUriLimit || uri.substr(8, 7) != L"search/") return false;
    const auto value = uri.substr(15);
    std::string bytes;
    const auto hex = [](wchar_t c) -> int {
        if (c >= L'0' && c <= L'9') return c - L'0';
        if (c >= L'a' && c <= L'f') return c - L'a' + 10;
        if (c >= L'A' && c <= L'F') return c - L'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < value.size();) {
        const auto c = value[i];
        if (c == L'%') {
            if (i + 2 >= value.size()) return false;
            const int hi = hex(value[i + 1]), lo = hex(value[i + 2]);
            if (hi < 0 || lo < 0) return false;
            bytes += static_cast<char>((hi << 4) | lo);
            i += 3;
        } else {
            if (c == L'?' || c == L'#' || c == L'/') return false;
            const size_t count = c >= 0xD800 && c <= 0xDBFF ? 2 : 1;
            if (i + count > value.size()) return false;
            char encoded[4];
            const int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data() + i,
                static_cast<int>(count), encoded, sizeof(encoded), nullptr, nullptr);
            if (!n) return false;
            bytes.append(encoded, n);
            i += count;
        }
    }
    if (bytes.empty()) return false;
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
    if (length <= 0 || static_cast<size_t>(length) > kSearchKeywordLimit) return false;
    std::wstring decoded(static_cast<size_t>(length), L'\0');
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(), static_cast<int>(bytes.size()), decoded.data(), length)) return false;
    for (const auto c : decoded) if (c < L' ' || c == 0x7F) return false;
    if (decoded.find_first_not_of(L' ') == std::wstring::npos) return false;
    keyword = std::move(decoded);
    return true;
}
}
