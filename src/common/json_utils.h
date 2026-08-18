#pragma once

#include <string>
#include <vector>

namespace pulse::json {

inline void Escape(const std::wstring& input, std::wstring& output) {
    for (wchar_t c : input) {
        if (c == L'\\') output += L"\\\\";
        else if (c == L'"') output += L"\\\"";
        else if (c == L'\n') output += L"\\n";
        else if (c == L'\r') output += L"\\r";
        else if (c == L'\t') output += L"\\t";
        else output += c;
    }
}

inline void SkipWhitespace(const std::wstring& input, size_t& pos) {
    while (pos < input.size() &&
           (input[pos] == L' ' || input[pos] == L'\n' ||
            input[pos] == L'\r' || input[pos] == L'\t')) {
        ++pos;
    }
}

inline size_t ValuePosition(const std::wstring& input, const std::wstring& key) {
    const std::wstring quoted = L"\"" + key + L"\"";
    size_t pos = input.find(quoted);
    if (pos == std::wstring::npos) return pos;
    pos = input.find(L':', pos + quoted.size());
    if (pos == std::wstring::npos) return pos;
    ++pos;
    SkipWhitespace(input, pos);
    return pos;
}

inline std::wstring UnescapeString(const std::wstring& input, size_t& pos) {
    std::wstring output;
    while (pos < input.size() && input[pos] != L'"') {
        if (input[pos] == L'\\' && pos + 1 < input.size()) {
            ++pos;
            if (input[pos] == L'n') output += L'\n';
            else if (input[pos] == L'r') output += L'\r';
            else if (input[pos] == L't') output += L'\t';
            else output += input[pos];
        } else {
            output += input[pos];
        }
        ++pos;
    }
    if (pos < input.size()) ++pos;
    return output;
}

inline std::wstring ExtractString(const std::wstring& input, const std::wstring& key,
                                  std::wstring fallback = {}) {
    size_t pos = ValuePosition(input, key);
    if (pos == std::wstring::npos || pos >= input.size() || input[pos] != L'"')
        return fallback;
    ++pos;
    return UnescapeString(input, pos);
}

inline int ExtractInt(const std::wstring& input, const std::wstring& key, int fallback = 0) {
    size_t pos = ValuePosition(input, key);
    if (pos == std::wstring::npos || pos >= input.size()) return fallback;
    int sign = 1;
    if (input[pos] == L'-') { sign = -1; ++pos; }
    if (pos >= input.size() || input[pos] < L'0' || input[pos] > L'9') return fallback;
    int value = 0;
    while (pos < input.size() && input[pos] >= L'0' && input[pos] <= L'9') {
        value = value * 10 + (input[pos] - L'0');
        ++pos;
    }
    return value * sign;
}

inline bool ExtractBool(const std::wstring& input, const std::wstring& key,
                        bool fallback = false) {
    const size_t pos = ValuePosition(input, key);
    if (pos == std::wstring::npos) return fallback;
    if (input.compare(pos, 4, L"true") == 0) return true;
    if (input.compare(pos, 5, L"false") == 0) return false;
    return fallback;
}

inline std::vector<std::wstring> ExtractStringArray(const std::wstring& input,
                                                    const std::wstring& key) {
    std::vector<std::wstring> output;
    size_t pos = ValuePosition(input, key);
    if (pos == std::wstring::npos || pos >= input.size() || input[pos] != L'[')
        return output;
    ++pos;
    while (pos < input.size()) {
        SkipWhitespace(input, pos);
        while (pos < input.size() && input[pos] == L',') {
            ++pos;
            SkipWhitespace(input, pos);
        }
        if (pos >= input.size() || input[pos] == L']') break;
        if (input[pos] != L'"') { ++pos; continue; }
        ++pos;
        output.push_back(UnescapeString(input, pos));
    }
    return output;
}

} // namespace pulse::json
