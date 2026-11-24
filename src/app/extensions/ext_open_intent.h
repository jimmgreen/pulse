#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace pulse::extensions {

inline constexpr std::wstring_view kExtOpenIntentPrefix = L"pulse:ext-open:";
inline constexpr std::wstring_view kExtPreviewIntentPrefix = L"pulse:ext-preview:";

inline bool ValidExtOpenAsciiToken(std::wstring_view value) noexcept {
    if (value.empty() || value.size() > 64) return false;
    for (const wchar_t c : value) {
        const bool ok = (c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z') ||
                        (c >= L'0' && c <= L'9') || c == L'.' || c == L'_' || c == L'-';
        if (!ok) return false;
    }
    return true;
}

struct ExtOpenIntent {
    std::string package_id;
    std::string handler_id;
    std::wstring path;
    bool preview = false;
    bool operator==(const ExtOpenIntent&) const = default;
};

inline std::wstring StripSurroundingQuotes(std::wstring_view raw) {
    while (raw.size() >= 2 && raw.front() == L'"' && raw.back() == L'"') {
        raw.remove_prefix(1);
        raw.remove_suffix(1);
    }
    return std::wstring(raw);
}

inline std::wstring FormatExtOpenIntent(std::wstring_view package_id,
                                        std::wstring_view handler_id,
                                        std::wstring_view raw_path,
                                        bool preview = false) {
    if (!ValidExtOpenAsciiToken(package_id) || !ValidExtOpenAsciiToken(handler_id)) return {};
    std::wstring path = StripSurroundingQuotes(raw_path);
    if (path.empty()) return {};
    for (const wchar_t c : path) {
        if (c < 0x20 || c == 0x7f) return {};
    }
    std::wstring out(preview ? kExtPreviewIntentPrefix : kExtOpenIntentPrefix);
    out.append(package_id);
    out.push_back(L'/');
    out.append(handler_id);
    out.push_back(L'/');
    out.append(path);
    return out;
}

inline bool StartsWithInsensitiveAscii(std::wstring_view raw, std::wstring_view prefix) noexcept {
    if (raw.size() <= prefix.size()) return false;
    for (size_t i = 0; i < prefix.size(); ++i) {
        wchar_t lhs = raw[i];
        wchar_t rhs = prefix[i];
        if (lhs >= L'A' && lhs <= L'Z') lhs = static_cast<wchar_t>(lhs - L'A' + L'a');
        if (rhs >= L'A' && rhs <= L'Z') rhs = static_cast<wchar_t>(rhs - L'A' + L'a');
        if (lhs != rhs) return false;
    }
    return true;
}

inline std::optional<ExtOpenIntent> ParseExtOpenIntent(std::wstring_view raw) {
    bool preview = false;
    std::wstring_view prefix;
    if (StartsWithInsensitiveAscii(raw, kExtPreviewIntentPrefix)) {
        prefix = kExtPreviewIntentPrefix;
        preview = true;
    } else if (StartsWithInsensitiveAscii(raw, kExtOpenIntentPrefix)) {
        prefix = kExtOpenIntentPrefix;
        preview = false;
    } else {
        return std::nullopt;
    }
    const std::wstring_view rest = raw.substr(prefix.size());
    const size_t first_slash = rest.find(L'/');
    if (first_slash == std::wstring_view::npos) return std::nullopt;
    const size_t second_slash = rest.find(L'/', first_slash + 1);
    if (second_slash == std::wstring_view::npos) return std::nullopt;

    const std::wstring_view pkg = rest.substr(0, first_slash);
    const std::wstring_view handler = rest.substr(first_slash + 1, second_slash - first_slash - 1);
    if (!ValidExtOpenAsciiToken(pkg) || !ValidExtOpenAsciiToken(handler)) return std::nullopt;

    std::wstring path = StripSurroundingQuotes(rest.substr(second_slash + 1));
    if (path.empty()) return std::nullopt;
    for (const wchar_t c : path) {
        if (c < 0x20 || c == 0x7f) return std::nullopt;
    }

    ExtOpenIntent out;
    out.package_id.reserve(pkg.size());
    for (const wchar_t c : pkg) out.package_id.push_back(static_cast<char>(c));
    out.handler_id.reserve(handler.size());
    for (const wchar_t c : handler) out.handler_id.push_back(static_cast<char>(c));
    out.path = std::move(path);
    out.preview = preview;
    return out;
}

inline bool IsExtOpenIntent(std::wstring_view raw) {
    return ParseExtOpenIntent(raw).has_value();
}

} // namespace pulse::extensions
