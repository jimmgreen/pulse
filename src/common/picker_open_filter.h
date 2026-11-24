#pragma once
#include <string_view>

namespace pulse::picker_filter {
// Open accepts leaf-name masks, including the Shell question-mark semantics.
// Path syntax remains outside it. Native
// semicolon lists ignore empty segments, but an entirely empty list is rejected.
// A pipe is literal (and cannot match a filesystem name), not a list separator.
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
        const bool extension_mask = part.size() > 2 && part[0] == Char('*') && part[1] == Char('.');
        if (part.front() == Char(' ') || part.back() == Char(' ') || part.back() == Char('.')) return false;
        const auto dot = part.find(Char('.'));
        for (size_t i = 0; i < part.size(); ++i) {
            const Char c = part[i];
            if (c < Char(' ') || (extension_mask && c == Char(' ')) || c == Char('<') ||
                c == Char('>') || c == Char(':') || c == Char('"') || c == Char('/') || c == Char('\\')) return false;
            if (!extension_mask && (c == Char('|') ||
                (c == Char('*') && (dot == part.npos || i >= dot || part.find(Char('*')) != i)))) return false;
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
    // System Open skips dotless alternatives. The first dotted alternative
    // determines the entire suffix (even .tar.gz); a wildcard there leaves
    // the caller default in effect instead of searching later alternatives.
    while (!pattern.empty()) {
        const auto end = pattern.find(Char(';'));
        const auto part = pattern.substr(0, end);
        const auto dot = part.find(Char('.'));
        if (dot != part.npos) {
            const auto suffix = part.substr(dot + 1);
            return suffix.find(Char('*')) == suffix.npos && suffix.find(Char('?')) == suffix.npos
                ? suffix : std::basic_string_view<Char>{};
        }
        if (end == pattern.npos) break;
        pattern.remove_prefix(end + 1);
    }
    return {};
}
} // namespace pulse::picker_filter
