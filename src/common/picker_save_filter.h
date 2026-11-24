#pragma once
#include <string_view>

namespace pulse::picker_filter {
// A Save name mask describes a listing, not a filename template. Only the
// measured basename forms are admitted; DOS wildcards and path syntax are not.
template<class Char>
struct SaveMask {
    bool valid = false;
    bool name_mask = false;
    std::basic_string_view<Char> extension;
};

template<class Char>
SaveMask<Char> ParseSaveMask(std::basic_string_view<Char> pattern) {
    using View = std::basic_string_view<Char>;
    SaveMask<Char> parsed;
    if (pattern.size() == 1 && pattern.front() == Char('*')) return {true, true, {}};
    if (pattern.size() == 3 && pattern[0] == Char('*') && pattern[1] == Char('.') && pattern[2] == Char('*'))
        return {true, false, {}};
    for (size_t start = 0; start < pattern.size();) {
        const auto end = pattern.find(Char(';'), start);
        const View part = pattern.substr(start, end == View::npos ? View::npos : end - start);
        const bool leading_dot = part.size() > 1 && part.front() == Char('.') && part.back() != Char('.') &&
            part.find(Char('.'), 1) == View::npos && part.find(Char('*')) == View::npos;
        if (part.empty() || (part.front() == Char('.') && !leading_dot) || part.back() == Char('.')) return {};
        const bool conventional = part.size() > 2 && part[0] == Char('*') && part[1] == Char('.');
        const auto dot = part.find(Char('.'));
        const auto star = part.find(Char('*'));
        if (!conventional) {
            parsed.name_mask = true;
            // Exact portable leaf or a literal prefix, one star, then a simple extension.
            if (star != View::npos && (!star || dot != star + 1 || part.find(Char('.'), dot + 1) != View::npos)) return {};
            if (dot != View::npos && part.find(Char('.'), dot + 1) != View::npos) return {};
        } else if (part[2] == Char('.')) return {};
        for (size_t i = 0; i < part.size(); ++i) {
            const Char c = part[i];
            if (c <= Char(' ') || c == Char('?') || c == Char('"') || c == Char('<') || c == Char('>') ||
                c == Char('|') || c == Char('/') || c == Char('\\') || c == Char(':') ||
                (c == Char('*') && i != star)) return {};
            if (!conventional && c != Char('*') && c != Char('.') && c != Char('-') && c != Char('_') &&
                !(c >= Char('A') && c <= Char('Z')) && !(c >= Char('a') && c <= Char('z')) &&
                !(c >= Char('0') && c <= Char('9'))) return {};
        }
        if (parsed.extension.empty() && dot != View::npos) parsed.extension = part.substr(dot + 1);
        if (end == View::npos) { parsed.valid = true; return parsed; }
        start = end + 1;
    }
    return {};
}
} // namespace pulse::picker_filter
