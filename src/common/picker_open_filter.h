#pragma once
#include <string_view>

namespace pulse::picker_filter {
// Open supports basename-independent extension masks only. Question marks,
// path syntax and filename-prefix globs remain outside this contract. Native
// semicolon lists ignore empty segments, but an entirely empty list is rejected.
template<class Char>
bool OpenPattern(std::basic_string_view<Char> pattern) {
    bool valid_part = false;
    size_t start = 0;
    while (start < pattern.size()) {
        const size_t end = pattern.find(Char(';'), start);
        const auto part = pattern.substr(start, end == pattern.npos ? pattern.size() - start : end - start);
        if (part.empty()) { start = end == pattern.npos ? pattern.size() : end + 1; continue; }
        if (part.size() == 1 && part[0] == Char('*')) {
            if (valid_part) return false;
            return end == pattern.npos || pattern.find_first_not_of(Char(';'), end) == pattern.npos;
        }
        if (part.size() <= 2 || part[0] != Char('*') || part[1] != Char('.') || part.back() == Char('.')) return false;
        for (const Char c : part.substr(2)) {
            if (c <= Char(' ') || c == Char('?') || c == Char('<') || c == Char('>') || c == Char(':') ||
                c == Char('"') || c == Char('/') || c == Char('\\') || c == Char('|')) return false;
        }
        valid_part = true;
        if (end == pattern.npos) return true;
        start = end + 1;
    }
    return valid_part;
}

template<class Char>
std::basic_string_view<Char> FirstLiteralExtension(std::basic_string_view<Char> pattern) {
    if (!OpenPattern(pattern)) return {};
    while (!pattern.empty() && pattern.front() == Char(';')) pattern.remove_prefix(1);
    const auto end = pattern.find(Char(';'));
    const auto part = pattern.substr(0, end);
    if (part.size() < 3) return {};
    const auto first = part.substr(2);
    return first.find(Char('*')) == first.npos ? first : std::basic_string_view<Char>{};
}
} // namespace pulse::picker_filter
