// type_ahead_test.cpp — Regression cases for the shared prefix matcher (#93).
#include "../ui/type_ahead.h"

#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

int main() {
    using pulse::ui::NextPrefixMatch;
    int failures = 0;
    const auto check = [&](bool ok, const char* label) {
        std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
        if (!ok) ++failures;
    };

    const std::vector<std::wstring> names = {L"apple", L"Banana", L"avocado", L"", L"文档.docx"};
    const auto name_at = [&names](int index) -> std::wstring_view {
        return names[static_cast<size_t>(index)];
    };
    const int count = static_cast<int>(names.size());
    const auto find = [&](int from, std::wstring_view prefix) {
        return NextPrefixMatch(count, from, name_at, prefix);
    };

    check(find(-1, L"a") == 0, "first press from no selection selects the first match");
    check(find(0, L"a") == 2, "pressing the same letter again advances to the next match");
    check(find(2, L"a") == 0, "the last match wraps back to the first");
    check(find(0, L"A") == 2 && find(2, L"A") == 0, "matching is case-insensitive");
    check(find(0, L"b") == 1 && find(1, L"b") == 1, "a single match keeps selecting itself");
    check(find(0, L"z") == -1, "no match reports -1 and leaves the selection alone");
    check(find(0, L"") == -1, "an empty prefix matches nothing");
    check(find(0, L"文") == 4 && find(4, L"文") == 4,
        "non-ASCII names match on their first character");
    check(find(3, L"x") == -1, "an empty entry name never matches");

    const auto empty_at = [](int) -> std::wstring_view { return {}; };
    check(NextPrefixMatch(0, -1, empty_at, L"a") == -1, "an empty list reports -1");

    // Two-character prefixes: reserved for accumulating input, already covered
    // here so the shared helper cannot regress silently.
    check(find(-1, L"av") == 2 && find(2, L"av") == 2 && find(-1, L"ap") == 0,
        "a longer prefix narrows the match and still wraps");
    check(find(-1, L"apple pie") == -1, "a prefix longer than any name matches nothing");

    return failures ? 1 : 0;
}
