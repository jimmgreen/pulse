#pragma once
#include "picker_save_suggestions.h"
#include <windows.h>
#include <string>
#include <string_view>

namespace pulse::ui {
// The mockup's family rule: ignore the final extension and an explicit trailing
// v<number>, with an optional dash, underscore or space. Plain numbers stay.
inline std::wstring PickerSaveSeriesKey(std::wstring_view filename) {
    if (!save_suggestion_detail::ValidFilename(filename)) return {};
    const auto slash = filename.find_last_of(L"\\/");
    auto leaf = filename.substr(slash == filename.npos ? 0 : slash + 1);
    const auto dot = leaf.find_last_of(L'.');
    if (dot != leaf.npos && dot > 0) leaf = leaf.substr(0, dot);
    size_t digits = leaf.size();
    while (digits > 0 && save_suggestion_detail::Digit(leaf[digits - 1])) --digits;
    if (digits < leaf.size() && digits > 0 && (leaf[digits - 1] == L'v' || leaf[digits - 1] == L'V')) {
        size_t end = digits - 1;
        if (end && (leaf[end - 1] == L'-' || leaf[end - 1] == L'_' || leaf[end - 1] == L' ')) --end;
        leaf = leaf.substr(0, end);
    }
    return std::wstring(leaf);
}
inline bool PickerSaveSeriesMatches(std::wstring_view key, std::wstring_view candidate, bool directory = false) {
    if (directory || key.empty()) return false;
    const auto other = PickerSaveSeriesKey(candidate);
    return !other.empty() && CompareStringOrdinal(key.data(), static_cast<int>(key.size()),
        other.data(), static_cast<int>(other.size()), TRUE) == CSTR_EQUAL;
}
} // namespace pulse::ui
