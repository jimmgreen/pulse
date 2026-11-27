#pragma once
#include <string>
#include <string_view>
#include <vector>
#include <utility>

namespace pulse::ui {

enum class SaveNameSuggestionKind { Version, Date, Original, RemoveCopy };
struct SaveNameSuggestion {
    SaveNameSuggestionKind kind;
    std::wstring filename;
};

namespace save_suggestion_detail {
inline bool Digit(wchar_t c) { return c >= L'0' && c <= L'9'; }
inline wchar_t LowerAscii(wchar_t c) { return c >= L'A' && c <= L'Z' ? c + (L'a' - L'A') : c; }
inline bool EqualAscii(std::wstring_view a, std::wstring_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) if (LowerAscii(a[i]) != LowerAscii(b[i])) return false;
    return true;
}
inline bool ValidLeaf(std::wstring_view leaf) {
    if (leaf.empty() || leaf == L"." || leaf == L".." || leaf.back() == L'.' || leaf.back() == L' ') return false;
    for (const auto c : leaf)
        if (c < 32 || std::wstring_view(L"<>:\"/\\|?*").find(c) != std::wstring_view::npos) return false;
    auto device = leaf.substr(0, leaf.find(L'.'));
    while (!device.empty() && device.back() == L' ') device.remove_suffix(1);
    if (EqualAscii(device, L"CON") || EqualAscii(device, L"PRN") || EqualAscii(device, L"AUX") || EqualAscii(device, L"NUL") ||
        EqualAscii(device, L"CONIN$") || EqualAscii(device, L"CONOUT$")) return false;
    if (device.size() == 4 && (EqualAscii(device.substr(0, 3), L"COM") || EqualAscii(device.substr(0, 3), L"LPT"))) {
        const auto n = device[3];
        if ((n >= L'1' && n <= L'9') || n == L'\u00b9' || n == L'\u00b2' || n == L'\u00b3') return false;
    }
    return true;
}
inline bool ValidFilename(std::wstring_view name) {
    const auto slash = name.find_last_of(L"\\/");
    const auto leaf = slash == name.npos ? name : name.substr(slash + 1);
    if (!ValidLeaf(leaf)) return false;
    // Preserve an extended-path marker, but reject wildcard paths and alternate streams.
    const bool extended = name.starts_with(L"\\\\?\\");
    for (size_t i = 0; i < name.size(); ++i) {
        const auto c = name[i];
        if (c < 32 || c == L'*' || (c == L'?' && !(extended && i == 2)) ||
            c == L'"' || c == L'<' || c == L'>' || c == L'|') return false;
        if (c == L':') {
            const size_t drive = extended ? 4 : 0;
            if (i != drive + 1 || name.size() <= i + 1 || (name[i + 1] != L'\\' && name[i + 1] != L'/') ||
                !((name[drive] >= L'A' && name[drive] <= L'Z') || (name[drive] >= L'a' && name[drive] <= L'z'))) return false;
        }
    }
    return true;
}
inline bool ValidDate(std::wstring_view date) {
    if (date.size() != 10 || date[4] != L'-' || date[7] != L'-') return false;
    for (size_t i = 0; i < date.size(); ++i) if (i != 4 && i != 7 && !Digit(date[i])) return false;
    const int year = (date[0] - L'0') * 1000 + (date[1] - L'0') * 100 + (date[2] - L'0') * 10 + date[3] - L'0';
    const int month = (date[5] - L'0') * 10 + date[6] - L'0';
    const int day = (date[8] - L'0') * 10 + date[9] - L'0';
    if (!year || month < 1 || month > 12) return false;
    constexpr int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    const int leap = month == 2 && year % 4 == 0 && (year % 100 != 0 || year % 400 == 0) ? 1 : 0;
    return day >= 1 && day <= days[month - 1] + leap;
}
inline std::wstring IncrementVersion(std::wstring_view stem) {
    size_t digits = stem.size();
    while (digits && Digit(stem[digits - 1])) --digits;
    if (!digits || digits == stem.size() || (stem[digits - 1] != L'v' && stem[digits - 1] != L'V')) return {};
    const size_t marker = digits - 1;
    if (marker && std::wstring_view(L" _-.").find(stem[marker - 1]) == std::wstring_view::npos) return {};
    std::wstring result(stem);
    for (size_t i = result.size(); i > digits;) {
        --i;
        if (result[i] != L'9') { ++result[i]; return result; }
        result[i] = L'0';
    }
    result.insert(digits, 1, L'1');
    return result;
}
inline std::wstring_view WithoutCopy(std::wstring_view stem) {
    constexpr std::wstring_view chinese = L" - 副本", english = L" - Copy";
    if (stem.ends_with(chinese)) return stem.substr(0, stem.size() - chinese.size());
    if (stem.size() >= english.size() && EqualAscii(stem.substr(stem.size() - english.size()), english))
        return stem.substr(0, stem.size() - english.size());
    if (stem.ends_with(L')')) {
        const auto begin = stem.rfind(L" (");
        if (begin != stem.npos) {
            const auto number = stem.substr(begin + 2, stem.size() - begin - 3);
            if (!number.empty() && number.front() != L'0' && (number.size() > 1 || number[0] >= L'2')) {
                bool numeric = true;
                for (auto c : number) numeric &= Digit(c);
                if (numeric) return stem.substr(0, begin);
            }
        }
    }
    return stem;
}
}

// Suggestions only: never probes existence, resolves paths, or applies a caller's default extension.
inline std::vector<SaveNameSuggestion> BuildSaveNameSuggestions(std::wstring_view filename,
    std::wstring_view original_filename, std::wstring_view date) {
    using namespace save_suggestion_detail;
    std::vector<SaveNameSuggestion> suggestions;
    if (!ValidFilename(filename)) return suggestions;
    const auto slash = filename.find_last_of(L"\\/");
    const auto prefix = slash == filename.npos ? std::wstring_view{} : filename.substr(0, slash + 1);
    const auto leaf = filename.substr(prefix.size());
    const auto dot = leaf.find_last_of(L'.');
    const bool has_extension = dot != leaf.npos && dot != 0;
    const auto stem = has_extension ? leaf.substr(0, dot) : leaf;
    const auto extension = has_extension ? leaf.substr(dot) : std::wstring_view{};
    const auto add = [&](SaveNameSuggestionKind kind, std::wstring candidate) {
        if (candidate == filename || !ValidFilename(candidate)) return;
        for (const auto& prior : suggestions) if (prior.filename == candidate) return;
        if (suggestions.size() < 4) suggestions.push_back({kind, std::move(candidate)});
    };
    const auto with_stem = [&](std::wstring_view value) {
        return std::wstring(prefix) + std::wstring(value) + std::wstring(extension);
    };
    if (const auto version = IncrementVersion(stem); !version.empty()) add(SaveNameSuggestionKind::Version, with_stem(version));
    if (ValidDate(date)) {
        auto base = stem;
        if (base.size() >= 11 && base[base.size() - 11] == L'_' && ValidDate(base.substr(base.size() - 10)))
            base.remove_suffix(11);
        if (!base.empty()) add(SaveNameSuggestionKind::Date, with_stem(std::wstring(base) + L"_" + std::wstring(date)));
    }
    if (!original_filename.empty()) add(SaveNameSuggestionKind::Original, std::wstring(original_filename));
    if (const auto clean = WithoutCopy(stem); clean != stem && !clean.empty()) add(SaveNameSuggestionKind::RemoveCopy, with_stem(clean));
    return suggestions;
}
}
