#pragma once
#include "../common/path_utils.h"
#include "../common/localization.h"
#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace pulse::ops::interactive_names {

enum class Family { Exact, NumberedFile, NumberedDirectory, NumberedUnknown, Transfer };
struct Root {
    std::wstring parent; // includes its final separator
    std::wstring leaf;
    Family family = Family::Exact;
    // Only read (a copy's source, a template): two reads of one place never
    // conflict; a read and a write, or two writes, do.
    bool read = false;
};
inline Root Make(std::wstring value, Family family = Family::Exact) {
    value = path::StripExtendedPathPrefix(value);
    std::replace(value.begin(), value.end(), L'/', L'\\');
    while (value.size() > 3 && value.back() == L'\\') value.pop_back();
    const auto offset = value.rfind(L'\\') + 1;
    return {value.substr(0, offset), value.substr(offset), family};
}
// Common-letter comparisons need no OS lookup. For mixed letters normalize
// every mount (including the first); temporary aliases and lookup failures
// never prove that the requests are independent.
template<class Resolve>
bool NormalizeVolumes(std::vector<Root>& left, std::vector<Root>& right, Resolve resolve) {
    const auto drive = [](const Root& root) {
        return root.parent.size() > 2 && root.parent[1] == L':' && root.parent[2] == L'\\'
            ? static_cast<wchar_t>(std::towupper(root.parent[0])) : L'\0';
    };
    if (left.empty() || right.empty()) return false;
    const auto letter = drive(left.front());
    const auto same = [&](const Root& root) { return letter && drive(root) == letter; };
    if (std::all_of(left.begin(), left.end(), same) && std::all_of(right.begin(), right.end(), same)) return true;
    std::map<wchar_t, std::wstring> volumes;
    const auto canonical = [&](Root& root) {
        const wchar_t key = drive(root);
        if (!key) return false;
        auto [it, added] = volumes.try_emplace(key);
        if (added && !resolve(key, it->second)) return false;
        root.parent.replace(0, 3, it->second);
        return true;
    };
    return std::all_of(left.begin(), left.end(), canonical) && std::all_of(right.begin(), right.end(), canonical);
}
inline bool Equal(std::wstring_view a, std::wstring_view b) {
    return a == b || (a.size() == b.size() && !a.empty() && path::EqualInsensitive(a, b));
}
inline bool Starts(std::wstring_view value, std::wstring_view prefix) {
    return value.size() >= prefix.size() && Equal(value.substr(0, prefix.size()), prefix);
}
// A control character cannot occur in a Win32 file name. It denotes one or
// more ASCII decimal digits as emitted by to_wstring (a conservative superset).
constexpr wchar_t kNumber = L'\x01';
inline bool Digit(wchar_t c) { return c >= L'0' && c <= L'9'; }
inline bool PatternsMeet(std::wstring_view a, std::wstring_view b) {
    if (Equal(a, b)) return true;
    using State = std::pair<size_t, size_t>;
    std::vector<State> todo;
    std::set<State> seen;
    State s{};
    for (;;) {
        auto [i, j] = s;
        while (i < a.size() && j < b.size() && a[i] != kNumber && b[j] != kNumber &&
               (a[i] == b[j] || Equal(a.substr(i, 1), b.substr(j, 1)))) { ++i; ++j; }
        if (i == a.size() && j == b.size()) return true;
        if (i < a.size() && j < b.size()) {
            const bool an = a[i] == kNumber, bn = b[j] == kNumber;
            if ((an && (bn || Digit(b[j]))) || (bn && Digit(a[i]))) {
                // Each number can finish here or absorb another digit. Both
                // cannot stay put: that returns to precisely the same state.
                if (seen.emplace(i, j).second) {
                    todo.emplace_back(i + 1, j + 1);
                    if (an) todo.emplace_back(i, j + 1);
                    if (bn) todo.emplace_back(i + 1, j);
                }
            }
        }
        if (todo.empty()) return false;
        s = todo.back(); todo.pop_back();
    }
}
inline bool HasCopySuffix(std::wstring_view leaf) {
    for (auto lang : {l10n::Language::ZhCN, l10n::Language::ZhTW, l10n::Language::EnUS}) {
        for (auto id : {l10n::StringId::Copy_2, l10n::StringId::Copy_3}) {
            const auto& suffix = l10n::GetForLanguage(id, lang);
            if (suffix.empty()) return true; // An uninitialized catalog cannot rule out a nested name.
            for (size_t i = 0; i + suffix.size() <= leaf.size(); ++i) {
                if (suffix.front() == L' ' && leaf[i] != L' ') continue;
                if (Equal(leaf.substr(i, suffix.size()), suffix)) return true;
            }
        }
    }
    return false;
}
inline auto Forms(const Root& root, bool nested_copy) {
    std::vector<std::wstring> names{root.leaf};
    const bool transfer = root.family == Family::Transfer;
    if (root.family == Family::Exact) return names;
    const std::wstring n(1, kNumber), numbered = L" (" + n + L")";
    auto add = [&](std::wstring s) {
        if (std::find(names.begin(), names.end(), s) == names.end()) names.push_back(std::move(s));
    };
    const auto variants = [&](const std::wstring& leaf, size_t split) {
        const auto stem = leaf.substr(0, split), ext = leaf.substr(split);
        if (!transfer) { add(stem + numbered + ext); return; }
        // UniqueCopyPath may run before or after a UI language switch. Source
        // strings directly from every shipped catalog, not hard-coded labels.
        for (auto lang : {l10n::Language::ZhCN, l10n::Language::ZhTW, l10n::Language::EnUS}) {
            auto copy = stem + l10n::GetForLanguage(l10n::StringId::Copy_2, lang);
            add(copy + ext); add(copy + numbered + ext);
            add(stem + l10n::GetForLanguage(l10n::StringId::Copy_3, lang) + n + ext);
        }
    };
    const auto expand = [&](const std::wstring& leaf) {
        const auto dot = leaf.rfind(L'.');
        const bool split = dot != std::wstring::npos && dot > 0;
        if (!split || root.family != Family::NumberedFile) variants(leaf, leaf.size());
        if (split && root.family != Family::NumberedDirectory) variants(leaf, dot);
    };
    expand(root.leaf);
    if (transfer) {
        // Copying onto the same object (including a distinct hard link) can
        // choose a localized name in planning AND another at execution. Delay
        // the second expansion until the opposing spelling could meet it.
        if (nested_copy) {
            const size_t first_names = names.size();
            for (size_t i = 1; i < first_names; ++i) {
                const auto first = names[i];
                expand(first);
            }
        }
        const auto outputs = names.size();
        const auto tail = std::to_wstring(GetCurrentProcessId()) + L"-" + n + L"-" + n;
        for (size_t i = 0; i < outputs; ++i)
            for (auto marker : {L".pulse-copy-", L".pulse-verify-", L".pulse-backup-", L".pulse-generated-original-"}) {
                auto base = names[i] + marker + tail;
                names.push_back(base); names.push_back(base + L"-" + n);
            }
    }
    return names;
}
inline bool NamesMeet(const Root& a, const Root& b) {
    if (Equal(a.leaf, b.leaf)) return true;
    if (a.family == Family::Exact && b.family == Family::Exact) return false;
    // Check the fixed prefix before constructing localized/temporary variants;
    // disjoint file stems are by far the most common case in a bulk selection.
    const auto stem = [](const Root& r) -> std::wstring_view {
        auto dot = r.leaf.rfind(L'.');
        return std::wstring_view(r.leaf).substr(0, r.family == Family::Exact || !dot ? std::wstring::npos : dot);
    };
    const auto x = stem(a), y = stem(b);
    if (!Starts(x, y) && !Starts(y, x)) return false;
    const bool nested = (a.family == Family::Transfer || b.family == Family::Transfer) &&
        (HasCopySuffix(a.leaf) || HasCopySuffix(b.leaf));
    const auto aa = Forms(a, nested), bb = Forms(b, nested);
    for (const auto& left : aa) for (const auto& right : bb)
        if (PatternsMeet(left, right)) return true;
    return false;
}
inline bool Overlaps(const Root& a, const Root& b) {
    if (Equal(a.parent, b.parent)) return NamesMeet(a, b);
    const Root& outer = a.parent.size() < b.parent.size() ? a : b;
    const Root& inner = a.parent.size() < b.parent.size() ? b : a;
    if (!Starts(inner.parent, outer.parent)) return false;
    const auto start = outer.parent.size();
    const auto slash = inner.parent.find(L'\\', start);
    return NamesMeet(outer, {outer.parent, inner.parent.substr(start, slash - start), Family::Exact});
}
} // namespace pulse::ops::interactive_names
