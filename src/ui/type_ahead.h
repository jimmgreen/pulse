// type_ahead.h — Shared "jump to the next entry starting with what I typed".
#pragma once
#include <cwctype>
#include <string_view>

namespace pulse::ui {

// Next index strictly after `from` whose name starts with `prefix`.
// Wraps around to the beginning and gives up after a full lap (-1 when
// nothing matches, or when count <= 0 / prefix is empty).
//
// `name_at` takes a view index and returns the name to match. The returned
// view only has to stay alive until `name_at` is called again, so a caller
// whose source returns by value can hand out a view into a reusable scratch
// buffer instead of copying every row.
//
// `prefix` longer than one character is supported (multi-character
// accumulation) but the main list currently only feeds single characters.
template <class NameAt>
int NextPrefixMatch(int count, int from, NameAt&& name_at, std::wstring_view prefix) {
    if (count <= 0 || prefix.empty()) return -1;
    for (int step = 1; step <= count; ++step) {
        const int index = ((from < 0 ? -1 : from) + step + count) % count;
        const std::wstring_view name = name_at(index);
        if (name.size() < prefix.size()) continue;
        bool match = true;
        for (size_t i = 0; i < prefix.size(); ++i) {
            if (std::towlower(name[i]) != std::towlower(prefix[i])) { match = false; break; }
        }
        if (match) return index;
    }
    return -1;
}

} // namespace pulse::ui
