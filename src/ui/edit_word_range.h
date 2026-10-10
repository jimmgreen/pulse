// edit_word_range.h — Double-click word ranges for single-line hosted editors.
#pragma once
#include <cwctype>
#include <string_view>
#include <utility>

namespace pulse::ui {
// Filename-oriented classes: punctuation such as '.', '-' and '(' separates
// words, CJK scripts form their own runs and '_' stays inside a word.
enum class EditCharClass { Empty, Space, Word, Ideograph, Kana, Hangul, Symbol };

inline EditCharClass ClassifyEditCodePoint(char32_t c) {
    if (c == U'\x3000' || c == U'\xA0' || (c < 0x10000 && std::iswspace(static_cast<wint_t>(c))))
        return EditCharClass::Space;
    if ((c >= 0x3040 && c <= 0x30FF) || (c >= 0x31F0 && c <= 0x31FF) || (c >= 0xFF66 && c <= 0xFF9F))
        return EditCharClass::Kana;
    if ((c >= 0xAC00 && c <= 0xD7AF) || (c >= 0x1100 && c <= 0x11FF) || (c >= 0x3130 && c <= 0x318F))
        return EditCharClass::Hangul;
    if ((c >= 0x4E00 && c <= 0x9FFF) || (c >= 0x3400 && c <= 0x4DBF) || (c >= 0xF900 && c <= 0xFAFF) ||
        (c >= 0x3005 && c <= 0x3007) || (c >= 0x2E80 && c <= 0x2FDF) || (c >= 0x20000 && c <= 0x3FFFF))
        return EditCharClass::Ideograph;
    if (c == U'_' || (c < 0x80 && std::iswalnum(static_cast<wint_t>(c)))) return EditCharClass::Word;
    // CJK punctuation and fullwidth symbols are not letters even where the
    // CRT tables call them alphabetic.
    if ((c >= 0x3000 && c <= 0x303F) || (c >= 0xFF00 && c <= 0xFF0F) || (c >= 0xFF1A && c <= 0xFF20) ||
        (c >= 0xFF3B && c <= 0xFF40) || (c >= 0xFF5B && c <= 0xFF65))
        return EditCharClass::Symbol;
    if (c >= 0x80 && c < 0x10000 && std::iswalnum(static_cast<wint_t>(c))) return EditCharClass::Word;
    return EditCharClass::Symbol;
}

// Combining marks, variation selectors and joiners extend the previous character.
inline bool IsEditExtendUnit(wchar_t c) {
    return (c >= 0x0300 && c <= 0x036F) || (c >= 0xFE00 && c <= 0xFE0F) || c == 0x200D ||
        (c >= 0x1AB0 && c <= 0x1AFF) || (c >= 0x20D0 && c <= 0x20FF);
}

inline EditCharClass ClassifyEditUnit(std::wstring_view text, size_t index) {
    while (index > 0 && IsEditExtendUnit(text[index])) --index;
    const wchar_t c = text[index];
    if (c >= 0xD800 && c <= 0xDBFF && index + 1 < text.size() &&
        text[index + 1] >= 0xDC00 && text[index + 1] <= 0xDFFF)
        return ClassifyEditCodePoint(0x10000 + ((static_cast<char32_t>(c) - 0xD800) << 10) +
            (static_cast<char32_t>(text[index + 1]) - 0xDC00));
    if (c >= 0xDC00 && c <= 0xDFFF && index > 0 && text[index - 1] >= 0xD800 && text[index - 1] <= 0xDBFF)
        return ClassifyEditUnit(text, index - 1);
    return ClassifyEditCodePoint(c);
}

// [start, end) of the run that contains the character at char_index. An index
// past the end selects the last run, matching a double-click after the text.
inline std::pair<int, int> EditWordRangeAt(std::wstring_view text, int char_index) {
    const int length = static_cast<int>(text.size());
    if (length <= 0) return {0, 0};
    const int at = char_index < 0 ? 0 : (char_index >= length ? length - 1 : char_index);
    const EditCharClass kind = ClassifyEditUnit(text, static_cast<size_t>(at));
    int start = at;
    int end = at + 1;
    while (start > 0 && ClassifyEditUnit(text, static_cast<size_t>(start - 1)) == kind) --start;
    while (end < length && ClassifyEditUnit(text, static_cast<size_t>(end)) == kind) ++end;
    return {start, end};
}
} // namespace pulse::ui
