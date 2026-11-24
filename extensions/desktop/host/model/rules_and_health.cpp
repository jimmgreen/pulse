// rules_and_health.cpp - Group rule matching, version comparator, desktop health analysis, and Uninstall registry probe.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "desktop_model.h"

#include <algorithm>
#include <array>
#include <cwctype>
#include <limits>
#include <unordered_set>

namespace pulse::desktop::model {
namespace {

constexpr int64_t kMsPerDay = 86400000LL;

bool IsAsciiDigit(wchar_t ch) noexcept {
    return ch >= L'0' && ch <= L'9';
}

bool IsAsciiLower(wchar_t ch) noexcept {
    return ch >= L'a' && ch <= L'z';
}

bool IsAsciiUpper(wchar_t ch) noexcept {
    return ch >= L'A' && ch <= L'Z';
}

bool IsAsciiAlpha(wchar_t ch) noexcept {
    return IsAsciiLower(ch) || IsAsciiUpper(ch);
}

bool IsAsciiAlnum(wchar_t ch) noexcept {
    return IsAsciiAlpha(ch) || IsAsciiDigit(ch);
}

bool IsCjkChar(wchar_t ch) noexcept {
    return (ch >= 0x4E00 && ch <= 0x9FFF) ||
           (ch >= 0x3400 && ch <= 0x4DBF) ||
           (ch >= 0xF900 && ch <= 0xFAFF);
}

wchar_t FoldChar(wchar_t ch) noexcept {
    if (IsAsciiUpper(ch)) {
        return static_cast<wchar_t>(ch - L'A' + L'a');
    }
    return static_cast<wchar_t>(std::towlower(static_cast<wint_t>(ch)));
}

bool EqualsIgnoreCase(std::wstring_view a, std::wstring_view b) noexcept {
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i) {
        if (FoldChar(a[i]) != FoldChar(b[i])) {
            return false;
        }
    }
    return true;
}

bool StartsWithIgnoreCase(std::wstring_view text, std::wstring_view prefix) noexcept {
    if (prefix.empty() || text.size() < prefix.size()) {
        return false;
    }
    return EqualsIgnoreCase(text.substr(0, prefix.size()), prefix);
}

bool ContainsIgnoreCase(std::wstring_view text, std::wstring_view needle) noexcept {
    if (needle.empty() || text.size() < needle.size()) {
        return false;
    }
    const size_t limit = text.size() - needle.size();
    for (size_t i = 0; i <= limit; ++i) {
        if (EqualsIgnoreCase(text.substr(i, needle.size()), needle)) {
            return true;
        }
    }
    return false;
}

std::wstring_view StripLeadingDot(std::wstring_view ext) noexcept {
    if (!ext.empty() && ext.front() == L'.') {
        ext.remove_prefix(1);
    }
    return ext;
}

std::wstring_view ExtractExtension(const DesktopItem& item) noexcept {
    if (!item.extension.empty()) {
        return item.extension;
    }
    if (item.is_folder) {
        return {};
    }
    const size_t dot = item.name.rfind(L'.');
    if (dot == std::wstring::npos || dot == 0) {
        return {};
    }
    return std::wstring_view(item.name).substr(dot);
}

std::wstring_view ExtractStem(const DesktopItem& item) noexcept {
    const std::wstring_view name = item.name;
    if (item.is_folder) {
        return name;
    }
    const size_t dot = name.rfind(L'.');
    if (dot == std::wstring::npos || dot == 0) {
        return name;
    }
    return name.substr(0, dot);
}

bool ExtensionInList(std::wstring_view ext_with_or_without_dot,
                     std::span<const std::wstring_view> list) noexcept {
    const std::wstring_view bare = StripLeadingDot(ext_with_or_without_dot);
    if (bare.empty()) {
        return false;
    }
    for (const std::wstring_view candidate : list) {
        if (EqualsIgnoreCase(bare, StripLeadingDot(candidate))) {
            return true;
        }
    }
    return false;
}

uint64_t ParseSaturatedUint64(std::wstring_view s, size_t& pos) noexcept {
    uint64_t val = 0;
    while (pos < s.size() && IsAsciiDigit(s[pos])) {
        const uint64_t digit = static_cast<uint64_t>(s[pos] - L'0');
        if (val > (std::numeric_limits<uint64_t>::max() - digit) / 10u) {
            val = std::numeric_limits<uint64_t>::max();
        } else {
            val = val * 10u + digit;
        }
        ++pos;
    }
    return val;
}

size_t ParseVersionSegmentsFrom(std::wstring_view s, size_t start,
                                std::array<uint64_t, 16>& out) noexcept {
    size_t count = 0;
    size_t pos = start;
    while (pos < s.size() && IsAsciiDigit(s[pos])) {
        const uint64_t seg = ParseSaturatedUint64(s, pos);
        if (count < out.size()) {
            out[count++] = seg;
        }
        if (pos + 1 < s.size() && s[pos] == L'.' && IsAsciiDigit(s[pos + 1])) {
            ++pos;
        } else {
            break;
        }
    }
    return count;
}

size_t ExtractVersionSegments(std::wstring_view s, std::array<uint64_t, 16>& out) noexcept {
    // Pass 1: prefer dotted numeric tokens like "26.00", "1.96.2", "v22.11.0".
    for (size_t i = 0; i < s.size(); ++i) {
        if (!IsAsciiDigit(s[i])) {
            continue;
        }
        size_t j = i;
        while (j < s.size() && IsAsciiDigit(s[j])) {
            ++j;
        }
        if (j + 1 < s.size() && s[j] == L'.' && IsAsciiDigit(s[j + 1])) {
            return ParseVersionSegmentsFrom(s, i, out);
        }
        i = j - 1;
    }

    // Pass 2: fallback to first standalone numeric segment (ignoring leading v/V).
    for (size_t i = 0; i < s.size(); ++i) {
        if (IsAsciiDigit(s[i])) {
            return ParseVersionSegmentsFrom(s, i, out);
        }
    }
    return 0;
}

std::wstring_view ExtractExplicitInstallerVersion(std::wstring_view stem) noexcept {
    for (size_t i = 0; i < stem.size(); ++i) {
        if (!IsAsciiDigit(stem[i])) {
            continue;
        }
        size_t end = i;
        bool has_dot = false;
        while (end < stem.size() && IsAsciiDigit(stem[end])) {
            ++end;
            if (end + 1 < stem.size() && stem[end] == L'.' && IsAsciiDigit(stem[end + 1])) {
                has_dot = true;
                ++end;
            }
        }
        if (has_dot) {
            return stem.substr(i, end - i);
        }
        // Also accept explicit "v22" / "-v3" prefixes when bounded cleanly.
        if (i > 0 && (stem[i - 1] == L'v' || stem[i - 1] == L'V')) {
            const bool clean_before_v = (i == 1) || !IsAsciiAlpha(stem[i - 2]);
            if (clean_before_v) {
                return stem.substr(i, end - i);
            }
        }
        i = end - 1;
    }
    return {};
}

bool IsNoiseAppToken(std::wstring_view lower_tok) noexcept {
    constexpr std::array<std::wstring_view, 24> kStopwords{{
        L"x64", L"x86", L"arm64", L"amd64", L"win64", L"win32", L"win",
        L"windows", L"microsoft", L"setup", L"install", L"installer",
        L"update", L"updater", L"redistributable", L"runtime", L"edition",
        L"version", L"official", L"release", L"portable", L"app", L"for", L"the"
    }};
    for (const std::wstring_view w : kStopwords) {
        if (lower_tok == w) {
            return true;
        }
    }
    bool has_non_digit = false;
    for (const wchar_t ch : lower_tok) {
        if (!IsAsciiDigit(ch)) {
            has_non_digit = true;
            break;
        }
    }
    return !has_non_digit;
}

bool ContainsBoundedToken(std::wstring_view stem, std::wstring_view lower_tok, bool is_cjk) noexcept {
    if (lower_tok.empty() || stem.size() < lower_tok.size()) {
        return false;
    }
    const size_t limit = stem.size() - lower_tok.size();
    for (size_t pos = 0; pos <= limit; ++pos) {
        if (!EqualsIgnoreCase(stem.substr(pos, lower_tok.size()), lower_tok)) {
            continue;
        }
        if (is_cjk) {
            return true;
        }
        const bool start_ok =
            (pos == 0) ||
            !IsAsciiAlnum(stem[pos - 1]) ||
            (IsAsciiLower(stem[pos - 1]) && IsAsciiUpper(stem[pos]));
        const size_t end = pos + lower_tok.size();
        const bool end_ok =
            (end == stem.size()) ||
            !IsAsciiAlnum(stem[end]) ||
            (IsAsciiLower(stem[end - 1]) && IsAsciiUpper(stem[end])) ||
            (IsAsciiAlpha(stem[end - 1]) && IsAsciiDigit(stem[end]));
        if (start_ok && end_ok) {
            return true;
        }
    }
    return false;
}

std::wstring CompactAlnumLower(std::wstring_view s) {
    std::wstring out;
    out.reserve(s.size());
    for (const wchar_t ch : s) {
        if (IsAsciiAlnum(ch) || IsCjkChar(ch)) {
            out.push_back(FoldChar(ch));
        }
    }
    return out;
}

bool StemMatchesInstalledApp(std::wstring_view stem, const InstalledAppRecord& app) {
    if (stem.empty() || app.display_name.empty()) {
        return false;
    }

    const std::wstring stem_compact = CompactAlnumLower(stem);
    const std::wstring_view name = app.display_name;

    // Extract candidate tokens separated by spaces/parentheses/punctuation, plus hyphen-joined tokens like "7-Zip".
    auto test_raw_token = [&](std::wstring_view raw) -> bool {
        std::wstring lower;
        lower.reserve(raw.size());
        bool has_cjk = false;
        for (const wchar_t ch : raw) {
            lower.push_back(FoldChar(ch));
            if (IsCjkChar(ch)) {
                has_cjk = true;
            }
        }
        const size_t min_len = has_cjk ? 2u : 3u;
        if (lower.size() < min_len || IsNoiseAppToken(lower)) {
            return false;
        }
        if (ContainsBoundedToken(stem, lower, has_cjk)) {
            return true;
        }
        const std::wstring compact_tok = CompactAlnumLower(raw);
        if (compact_tok.size() >= 4u && !IsNoiseAppToken(compact_tok) &&
            stem_compact.find(compact_tok) != std::wstring::npos) {
            return true;
        }
        return false;
    };

    // Pass A: tokens allowing internal '-' (e.g. "7-Zip").
    size_t i = 0;
    while (i < name.size()) {
        while (i < name.size() && !(IsAsciiAlnum(name[i]) || IsCjkChar(name[i]))) {
            ++i;
        }
        const size_t start = i;
        while (i < name.size() &&
               (IsAsciiAlnum(name[i]) || IsCjkChar(name[i]) ||
                (name[i] == L'-' && i + 1 < name.size() && IsAsciiAlnum(name[i + 1])))) {
            ++i;
        }
        if (i > start && test_raw_token(name.substr(start, i - start))) {
            return true;
        }
    }

    // Pass B: pure alphanumeric/CJK sub-tokens (e.g. "Node" from "Node.js").
    i = 0;
    while (i < name.size()) {
        while (i < name.size() && !(IsAsciiAlnum(name[i]) || IsCjkChar(name[i]))) {
            ++i;
        }
        const size_t start = i;
        while (i < name.size() && (IsAsciiAlnum(name[i]) || IsCjkChar(name[i]))) {
            ++i;
        }
        if (i > start && test_raw_token(name.substr(start, i - start))) {
            return true;
        }
    }

    return false;
}

bool HasParenthesizedCopySuffix(std::wstring_view stem) noexcept {
    // Matches "(1)" .. "(99)" copy suffixes without flagging 4-digit years like "(2026)".
    for (size_t i = 0; i + 2 < stem.size(); ++i) {
        if (stem[i] != L'(') {
            continue;
        }
        size_t j = i + 1;
        while (j < stem.size() && IsAsciiDigit(stem[j])) {
            ++j;
        }
        const size_t digits = j - (i + 1);
        if (digits >= 1 && digits <= 2 && j < stem.size() && stem[j] == L')') {
            if (stem[i + 1] != L'0') {
                return true;
            }
        }
    }
    return false;
}

struct ScopedRegKey {
    HKEY key = nullptr;
    ScopedRegKey() = default;
    explicit ScopedRegKey(HKEY k) noexcept : key(k) {}
    ~ScopedRegKey() {
        if (key != nullptr) {
            RegCloseKey(key);
        }
    }
    ScopedRegKey(const ScopedRegKey&) = delete;
    ScopedRegKey& operator=(const ScopedRegKey&) = delete;
};

std::wstring TrimWideWhitespace(std::wstring_view s) {
    size_t first = 0;
    while (first < s.size() && std::iswspace(static_cast<wint_t>(s[first]))) {
        ++first;
    }
    size_t last = s.size();
    while (last > first && std::iswspace(static_cast<wint_t>(s[last - 1]))) {
        --last;
    }
    return std::wstring(s.substr(first, last - first));
}

std::wstring QueryRegStringValue(HKEY key, const wchar_t* value_name) {
    DWORD type = 0;
    DWORD byte_len = 0;
    LSTATUS st = RegQueryValueExW(key, value_name, nullptr, &type, nullptr, &byte_len);
    if (st != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ) || byte_len < sizeof(wchar_t)) {
        return {};
    }

    std::wstring buf(byte_len / sizeof(wchar_t), L'\0');
    st = RegQueryValueExW(key, value_name, nullptr, &type,
                          reinterpret_cast<LPBYTE>(buf.data()), &byte_len);
    if (st != ERROR_SUCCESS) {
        return {};
    }

    const size_t nul_pos = buf.find(L'\0');
    if (nul_pos != std::wstring::npos) {
        buf.resize(nul_pos);
    }
    return TrimWideWhitespace(buf);
}

void EnumerateUninstallHive(HKEY root, REGSAM extra_sam,
                            std::unordered_set<std::wstring>& seen,
                            std::vector<InstalledAppRecord>& out) {
    constexpr const wchar_t* kUninstallSubkey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall";
    HKEY raw_hive = nullptr;
    if (RegOpenKeyExW(root, kUninstallSubkey, 0, KEY_READ | extra_sam, &raw_hive) != ERROR_SUCCESS) {
        return;
    }
    ScopedRegKey hive(raw_hive);

    std::array<wchar_t, 260> subkey_name{};
    for (DWORD index = 0;; ++index) {
        DWORD name_cch = static_cast<DWORD>(subkey_name.size());
        const LSTATUS enum_st = RegEnumKeyExW(
            hive.key, index, subkey_name.data(), &name_cch,
            nullptr, nullptr, nullptr, nullptr);
        if (enum_st == ERROR_NO_MORE_ITEMS) {
            break;
        }
        if (enum_st != ERROR_SUCCESS) {
            continue;
        }

        HKEY raw_entry = nullptr;
        if (RegOpenKeyExW(hive.key, subkey_name.data(), 0, KEY_READ | extra_sam, &raw_entry) != ERROR_SUCCESS) {
            continue;
        }
        ScopedRegKey entry(raw_entry);

        std::wstring display_name = QueryRegStringValue(entry.key, L"DisplayName");
        if (display_name.empty()) {
            continue;
        }
        std::wstring display_version = QueryRegStringValue(entry.key, L"DisplayVersion");
        std::wstring publisher = QueryRegStringValue(entry.key, L"Publisher");

        std::wstring dedup_key;
        dedup_key.reserve(display_name.size() + 1 + display_version.size());
        for (const wchar_t ch : display_name) {
            dedup_key.push_back(FoldChar(ch));
        }
        dedup_key.push_back(L'\n');
        for (const wchar_t ch : display_version) {
            dedup_key.push_back(FoldChar(ch));
        }

        if (seen.insert(std::move(dedup_key)).second) {
            InstalledAppRecord rec{};
            rec.display_name = std::move(display_name);
            rec.display_version = std::move(display_version);
            rec.publisher = std::move(publisher);
            out.push_back(std::move(rec));
        }
    }
}

} // namespace

std::wstring FormatRuleSummary(const GroupRule& rule) {
    if (rule.fallback_catch_all) {
        if (rule.match_folders && !rule.match_files) {
            return L"其他文件夹";
        }
        return L"其他桌面文件";
    }

    constexpr std::array<std::wstring_view, 12> kImageExts{{
        L".png", L".jpg", L".jpeg", L".webp", L".gif", L".bmp",
        L".svg", L".avif", L".ico", L".tif", L".tiff", L".heic"
    }};

    std::vector<std::wstring> parts;

    if (!rule.extensions.empty()) {
        bool all_images = true;
        for (const std::wstring& ext : rule.extensions) {
            if (!ExtensionInList(ext, kImageExts)) {
                all_images = false;
                break;
            }
        }

        if (all_images && rule.extensions.size() >= 4) {
            parts.push_back(L"图片");
        } else {
            std::vector<std::wstring> ext_tokens;
            const auto push_unique = [&](std::wstring tok) {
                for (const std::wstring& existing : ext_tokens) {
                    if (EqualsIgnoreCase(existing, tok)) {
                        return;
                    }
                }
                ext_tokens.push_back(std::move(tok));
            };

            for (const std::wstring& raw_ext : rule.extensions) {
                const std::wstring_view bare = StripLeadingDot(raw_ext);
                if (bare.empty()) {
                    continue;
                }
                if (EqualsIgnoreCase(bare, L"doc") || EqualsIgnoreCase(bare, L"docx")) {
                    push_unique(L"Word");
                } else if (EqualsIgnoreCase(bare, L"xls") || EqualsIgnoreCase(bare, L"xlsx") ||
                           EqualsIgnoreCase(bare, L"csv")) {
                    push_unique(L"Excel");
                } else if (EqualsIgnoreCase(bare, L"ppt") || EqualsIgnoreCase(bare, L"pptx")) {
                    push_unique(L"PPT");
                } else if (EqualsIgnoreCase(bare, L"pdf")) {
                    push_unique(L"PDF");
                } else if (EqualsIgnoreCase(bare, L"md") || EqualsIgnoreCase(bare, L"markdown")) {
                    push_unique(L"Markdown");
                } else {
                    std::wstring dotted = L".";
                    dotted.reserve(bare.size() + 1);
                    for (const wchar_t ch : bare) {
                        dotted.push_back(FoldChar(ch));
                    }
                    push_unique(std::move(dotted));
                }
            }

            if (ext_tokens.size() <= 3) {
                for (std::wstring& tok : ext_tokens) {
                    parts.push_back(std::move(tok));
                }
            } else {
                std::wstring joined = ext_tokens[0] + L" · " + ext_tokens[1] + L" · " + ext_tokens[2] + L" 等";
                parts.push_back(std::move(joined));
            }
        }
    } else if (rule.match_folders && !rule.match_files) {
        parts.push_back(L"文件夹");
    }

    if (!rule.name_prefixes.empty()) {
        std::wstring p = L"前缀 " + rule.name_prefixes.front();
        if (rule.name_prefixes.size() > 1) {
            p += L" 等";
        }
        parts.push_back(std::move(p));
    }

    if (!rule.name_contains.empty()) {
        std::wstring c = L"包含 " + rule.name_contains.front();
        if (rule.name_contains.size() > 1) {
            c += L" 等";
        }
        parts.push_back(std::move(c));
    }

    if (!rule.tag_id.empty()) {
        parts.push_back(L"标签 " + rule.tag_id);
    }

    if (rule.modified_within_days > 0) {
        parts.push_back(std::to_wstring(rule.modified_within_days) + L"天内");
    }

    if (parts.empty()) {
        return L"手动归入";
    }

    std::wstring summary;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) {
            summary += L" · ";
        }
        summary += parts[i];
    }
    return summary;
}

bool MatchesDesktopSearchQuery(const DesktopItem& item, std::wstring_view query) {
    const std::wstring trimmed = TrimWideWhitespace(query);
    if (trimmed.empty()) {
        return false;
    }

    if (ContainsIgnoreCase(item.name, trimmed)) {
        return true;
    }

    const std::wstring_view ext = ExtractExtension(item);
    if (!ext.empty() && ContainsIgnoreCase(ext, trimmed)) {
        return true;
    }

    constexpr std::array<std::wstring_view, 12> kImageExts{{
        L".png", L".jpg", L".jpeg", L".webp", L".gif", L".bmp",
        L".svg", L".avif", L".ico", L".tif", L".tiff", L".heic"
    }};
    constexpr std::array<std::wstring_view, 13> kDocExts{{
        L".docx", L".doc", L".pdf", L".md", L".markdown", L".txt", L".rtf",
        L".pptx", L".ppt", L".xlsx", L".xls", L".csv", L".odt"
    }};
    constexpr std::array<std::wstring_view, 5> kInstallerExts{{
        L".exe", L".msi", L".msix", L".msixbundle", L".appx"
    }};
    constexpr std::array<std::wstring_view, 5> kSheetExts{{
        L".xlsx", L".xls", L".csv", L".tsv", L".ods"
    }};
    constexpr std::array<std::wstring_view, 4> kSlideExts{{
        L".pptx", L".ppt", L".key", L".odp"
    }};
    constexpr std::array<std::wstring_view, 10> kArchiveExts{{
        L".zip", L".7z", L".rar", L".tar", L".gz",
        L".tgz", L".xz", L".bz2", L".iso", L".cab"
    }};

    if (trimmed == L"文件夹") {
        return item.is_folder;
    }
    if (item.is_folder) {
        return false;
    }

    if (trimmed == L"图片") {
        return ExtensionInList(ext, kImageExts);
    }
    if (trimmed == L"文档") {
        return ExtensionInList(ext, kDocExts);
    }
    if (trimmed == L"安装包") {
        return ExtensionInList(ext, kInstallerExts);
    }
    if (trimmed == L"表格") {
        return ExtensionInList(ext, kSheetExts);
    }
    if (trimmed == L"演示") {
        return ExtensionInList(ext, kSlideExts);
    }
    if (trimmed == L"压缩包") {
        return ExtensionInList(ext, kArchiveExts);
    }

    return false;
}

bool ItemMatchesRule(const DesktopItem& item, const GroupRule& rule, int64_t now_unix_ms) {
    if (item.is_folder ? !rule.match_folders : !rule.match_files) {
        return false;
    }

    const bool has_any_criterion =
        !rule.extensions.empty() ||
        !rule.name_prefixes.empty() ||
        !rule.name_contains.empty() ||
        !rule.tag_id.empty() ||
        rule.modified_within_days > 0;

    if (!rule.fallback_catch_all && !has_any_criterion) {
        return false;
    }

    if (!rule.tag_id.empty() && !EqualsIgnoreCase(item.tag_id, rule.tag_id)) {
        return false;
    }

    if (rule.modified_within_days > 0) {
        const int64_t threshold_ms = static_cast<int64_t>(rule.modified_within_days) * kMsPerDay;
        if (item.modified_unix_ms <= 0 || (now_unix_ms - item.modified_unix_ms) > threshold_ms) {
            return false;
        }
    }

    if (!rule.extensions.empty()) {
        const std::wstring_view item_ext = StripLeadingDot(ExtractExtension(item));
        if (item_ext.empty()) {
            return false;
        }
        bool ext_matched = false;
        for (const std::wstring& ext : rule.extensions) {
            const std::wstring_view rule_ext = StripLeadingDot(ext);
            if (!rule_ext.empty() && EqualsIgnoreCase(item_ext, rule_ext)) {
                ext_matched = true;
                break;
            }
        }
        if (!ext_matched) {
            return false;
        }
    }

    if (!rule.name_prefixes.empty()) {
        bool prefix_matched = false;
        for (const std::wstring& prefix : rule.name_prefixes) {
            if (!prefix.empty() && StartsWithIgnoreCase(item.name, prefix)) {
                prefix_matched = true;
                break;
            }
        }
        if (!prefix_matched) {
            return false;
        }
    }

    if (!rule.name_contains.empty()) {
        bool substr_matched = false;
        for (const std::wstring& sub : rule.name_contains) {
            if (!sub.empty() && ContainsIgnoreCase(item.name, sub)) {
                substr_matched = true;
                break;
            }
        }
        if (!substr_matched) {
            return false;
        }
    }

    return true;
}

std::optional<size_t> ClassifyItemToGroup(const DesktopItem& item,
                                          std::span<const DesktopGroup> groups,
                                          int64_t now_unix_ms) {
    if (item.hidden_by_mapped_group) {
        return std::nullopt;
    }

    if (!item.mapped_owner_group_id.empty()) {
        for (size_t i = 0; i < groups.size(); ++i) {
            if (!groups[i].mapped_folder_path.empty() &&
                groups[i].id == item.mapped_owner_group_id) {
                return i;
            }
        }
        return std::nullopt;
    }

    if (item.force_loose || item.in_apps_tile) {
        return std::nullopt;  // loose, or in the 应用 tile
    }

    if (!item.pinned_group_id.empty()) {
        for (size_t i = 0; i < groups.size(); ++i) {
            if (groups[i].mapped_folder_path.empty() &&
                groups[i].id == item.pinned_group_id) {
                return i;
            }
        }
    }

    for (size_t i = 0; i < groups.size(); ++i) {
        if (groups[i].mapped_folder_path.empty() &&
            !groups[i].rule.fallback_catch_all &&
            ItemMatchesRule(item, groups[i].rule, now_unix_ms)) {
            return i;
        }
    }

    for (size_t i = 0; i < groups.size(); ++i) {
        if (groups[i].mapped_folder_path.empty() &&
            groups[i].rule.fallback_catch_all &&
            ItemMatchesRule(item, groups[i].rule, now_unix_ms)) {
            return i;
        }
    }

    return std::nullopt;
}

int CompareVersionStrings(std::wstring_view a, std::wstring_view b) noexcept {
    std::array<uint64_t, 16> segs_a{};
    std::array<uint64_t, 16> segs_b{};
    const size_t na = ExtractVersionSegments(a, segs_a);
    const size_t nb = ExtractVersionSegments(b, segs_b);

    if (na == 0 && nb == 0) {
        return 0;
    }
    if (na == 0) {
        return -1;
    }
    if (nb == 0) {
        return 1;
    }

    const size_t n = std::max(na, nb);
    for (size_t i = 0; i < n; ++i) {
        const uint64_t va = (i < na) ? segs_a[i] : 0u;
        const uint64_t vb = (i < nb) ? segs_b[i] : 0u;
        if (va < vb) {
            return -1;
        }
        if (va > vb) {
            return 1;
        }
    }
    return 0;
}

HealthReport AnalyzeDesktopHealth(std::span<const DesktopItem> items,
                                  std::span<const InstalledAppRecord> installed_apps,
                                  int64_t now_unix_ms,
                                  int stale_screenshot_days) {
    constexpr std::array<std::wstring_view, 4> kInstallerExts{{
        L".exe", L".msi", L".msix", L".msixbundle"
    }};
    constexpr std::array<std::wstring_view, 9> kImageExts{{
        L".png", L".jpg", L".jpeg", L".webp", L".gif", L".bmp", L".svg", L".avif", L".tif"
    }};
    constexpr std::array<std::wstring_view, 7> kScreenshotMarkers{{
        L"screenshot", L"屏幕截图", L"截图", L"snipaste", L"pixpin", L"img_", L"微信图片"
    }};
    constexpr std::array<std::wstring_view, 3> kTempExts{{
        L".tmp", L".crdownload", L".part"
    }};
    constexpr std::array<std::wstring_view, 4> kOneTimeLogExts{{
        L".log", L".dmp", L".bak", L".old"
    }};
    constexpr std::array<std::wstring_view, 8> kOneTimeArchiveExts{{
        L".zip", L".7z", L".rar", L".tar", L".gz", L".tgz", L".xz", L".iso"
    }};

    HealthReport report{};
    int penalty = 0;
    const int64_t screenshot_threshold_ms =
        static_cast<int64_t>(std::max(0, stale_screenshot_days)) * kMsPerDay;
    constexpr int64_t kOneTimeArchiveThresholdMs = 14LL * kMsPerDay;
    constexpr int64_t kUntouchedFileThresholdMs = 30LL * kMsPerDay;

    for (const DesktopItem& item : items) {
        if (item.hidden_by_mapped_group || !item.mapped_owner_group_id.empty()) {
            continue;
        }
        // 1. Folders: flag empty folders; never misreport non-empty folders.
        if (item.is_folder) {
            if (item.folder_is_empty) {
                HealthIssue issue{};
                issue.kind = HealthIssueKind::EmptyFolder;
                issue.path = item.path;
                issue.name = item.name;
                issue.reclaimable_bytes = item.size_bytes;
                issue.reason = L"空文件夹";
                issue.selected_by_default = true;
                report.total_reclaimable_bytes += issue.reclaimable_bytes;
                report.issues.push_back(std::move(issue));
                penalty += 4;
            }
            continue;
        }

        const std::wstring_view ext = ExtractExtension(item);
        const std::wstring_view stem = ExtractStem(item);

        // 2. Broken shortcut
        if (item.is_shortcut && item.link_target_missing) {
            HealthIssue issue{};
            issue.kind = HealthIssueKind::BrokenShortcut;
            issue.path = item.path;
            issue.name = item.name;
            issue.reclaimable_bytes = item.size_bytes;
            issue.reason = item.link_target.empty()
                               ? L"快捷方式指向的目标已不存在"
                               : (L"目标已不存在: " + item.link_target);
            issue.selected_by_default = true;
            report.total_reclaimable_bytes += issue.reclaimable_bytes;
            report.issues.push_back(std::move(issue));
            penalty += 8;
            continue;
        }

        // 3. Redundant installer matching an already-installed application
        if (!item.is_shortcut && ExtensionInList(ext, kInstallerExts)) {
            if (!item.installed_app_hint.empty()) {
                HealthIssue issue{};
                issue.kind = HealthIssueKind::InstalledInstaller;
                issue.path = item.path;
                issue.name = item.name;
                issue.reclaimable_bytes = item.size_bytes;
                issue.reason = StartsWithIgnoreCase(item.installed_app_hint, L"已安装")
                                   ? item.installed_app_hint
                                   : (L"已安装 " + item.installed_app_hint);
                issue.selected_by_default = true;
                report.total_reclaimable_bytes += issue.reclaimable_bytes;
                report.issues.push_back(std::move(issue));
                penalty += 10;
                continue;
            }

            const std::wstring_view installer_ver = ExtractExplicitInstallerVersion(stem);
            const InstalledAppRecord* matched_app = nullptr;
            for (const InstalledAppRecord& app : installed_apps) {
                if (!StemMatchesInstalledApp(stem, app)) {
                    continue;
                }
                if (!installer_ver.empty() && !app.display_version.empty() &&
                    CompareVersionStrings(installer_ver, app.display_version) > 0) {
                    continue;
                }
                matched_app = &app;
                break;
            }

            if (matched_app != nullptr) {
                HealthIssue issue{};
                issue.kind = HealthIssueKind::InstalledInstaller;
                issue.path = item.path;
                issue.name = item.name;
                issue.reclaimable_bytes = item.size_bytes;
                issue.reason = L"已安装 " + matched_app->display_name;
                if (!matched_app->display_version.empty()) {
                    issue.reason += L" " + matched_app->display_version;
                }
                issue.selected_by_default = true;
                report.total_reclaimable_bytes += issue.reclaimable_bytes;
                report.issues.push_back(std::move(issue));
                penalty += 10;
                continue;
            }
        }

        // 4. Duplicate, incomplete download, temporary, or zero-byte file
        const bool is_temp_ext = ExtensionInList(ext, kTempExts);
        const bool is_dup_name =
            HasParenthesizedCopySuffix(stem) ||
            ContainsIgnoreCase(stem, L" - 副本") ||
            ContainsIgnoreCase(stem, L"- 副本") ||
            ContainsIgnoreCase(stem, L" - Copy");
        const bool is_zero_byte = !item.is_shortcut && item.size_bytes == 0;

        if (is_temp_ext || is_dup_name || is_zero_byte) {
            HealthIssue issue{};
            issue.kind = HealthIssueKind::DuplicateOrTemp;
            issue.path = item.path;
            issue.name = item.name;
            issue.reclaimable_bytes = item.size_bytes;
            if (is_temp_ext) {
                issue.reason = L"未完成下载或临时文件";
            } else if (is_zero_byte) {
                issue.reason = L"0 字节空文件";
            } else {
                issue.reason = L"重复副本文件";
            }
            issue.selected_by_default = true;
            report.total_reclaimable_bytes += issue.reclaimable_bytes;
            report.issues.push_back(std::move(issue));
            penalty += 6;
            continue;
        }

        // 5. One-time-use logs / stale archives (unchecked by default)
        if (!item.is_shortcut) {
            const int64_t elapsed_ms =
                (item.modified_unix_ms > 0) ? (now_unix_ms - item.modified_unix_ms) : 0;
            const bool is_log_like = ExtensionInList(ext, kOneTimeLogExts);
            const bool is_stale_archive =
                ExtensionInList(ext, kOneTimeArchiveExts) &&
                item.modified_unix_ms > 0 &&
                elapsed_ms > kOneTimeArchiveThresholdMs;

            if (is_log_like || is_stale_archive) {
                HealthIssue issue{};
                issue.kind = HealthIssueKind::OneTimeUse;
                issue.path = item.path;
                issue.name = item.name;
                issue.reclaimable_bytes = item.size_bytes;
                issue.reason = L"用过可能不再需要";
                issue.selected_by_default = false;
                report.issues.push_back(std::move(issue));
                penalty += 3;
                continue;
            }
        }

        // 6. Stale screenshot (>stale_screenshot_days) or untouched desktop file (>30 days)
        if (!item.is_shortcut && item.modified_unix_ms > 0) {
            const int64_t elapsed_ms = now_unix_ms - item.modified_unix_ms;
            if (ExtensionInList(ext, kImageExts) && elapsed_ms > screenshot_threshold_ms) {
                bool is_screenshot = false;
                for (const std::wstring_view marker : kScreenshotMarkers) {
                    if (ContainsIgnoreCase(stem, marker) || StartsWithIgnoreCase(item.name, marker)) {
                        is_screenshot = true;
                        break;
                    }
                }
                if (is_screenshot) {
                    const int64_t age_days = elapsed_ms / kMsPerDay;
                    HealthIssue issue{};
                    issue.kind = HealthIssueKind::StaleScreenshot;
                    issue.path = item.path;
                    issue.name = item.name;
                    issue.reclaimable_bytes = item.size_bytes;
                    issue.reason = std::to_wstring(age_days) + L" 天前的历史截图";
                    issue.selected_by_default = true;
                    report.total_reclaimable_bytes += issue.reclaimable_bytes;
                    report.issues.push_back(std::move(issue));
                    penalty += 5;
                    continue;
                }
            }

            if (elapsed_ms > kUntouchedFileThresholdMs) {
                const int64_t age_days = elapsed_ms / kMsPerDay;
                HealthIssue issue{};
                issue.kind = HealthIssueKind::StaleScreenshot;
                issue.path = item.path;
                issue.name = item.name;
                issue.reclaimable_bytes = item.size_bytes;
                issue.reason = std::to_wstring(age_days) + L" 天未修改的桌面文件";
                issue.selected_by_default = true;
                report.total_reclaimable_bytes += issue.reclaimable_bytes;
                report.issues.push_back(std::move(issue));
                penalty += 5;
            }
        }
    }

    report.score = std::clamp(100 - penalty, 20, 100);
    return report;
}

std::vector<InstalledAppRecord> QueryInstalledAppsFromRegistry() {
    std::vector<InstalledAppRecord> records;
    records.reserve(128);
    std::unordered_set<std::wstring> seen;

    EnumerateUninstallHive(HKEY_LOCAL_MACHINE, KEY_WOW64_64KEY, seen, records);
    EnumerateUninstallHive(HKEY_LOCAL_MACHINE, KEY_WOW64_32KEY, seen, records);
    EnumerateUninstallHive(HKEY_CURRENT_USER, 0, seen, records);

    return records;
}

} // namespace pulse::desktop::model
