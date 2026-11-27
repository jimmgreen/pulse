#include "picker_format_segments.h"
#include "../common/localization.h"

#include <algorithm>
#include <cwctype>

namespace pulse::ui {
namespace {

std::wstring_view Trim(std::wstring_view text) {
    while (!text.empty() && std::iswspace(text.front())) text.remove_prefix(1);
    while (!text.empty() && std::iswspace(text.back())) text.remove_suffix(1);
    return text;
}

std::vector<std::wstring_view> Tokens(std::wstring_view pattern) {
    std::vector<std::wstring_view> tokens;
    while (!pattern.empty()) {
        const size_t end = pattern.find(L';');
        const auto token = Trim(pattern.substr(0, end));
        if (!token.empty()) tokens.push_back(token);
        if (end == std::wstring_view::npos) break;
        pattern.remove_prefix(end + 1);
    }
    return tokens;
}

bool IsAnyToken(std::wstring_view token) { return token == L"*" || token == L"*.*"; }

std::wstring Upper(std::wstring text) {
    for (auto& c : text) c = static_cast<wchar_t>(std::towupper(c));
    return text;
}

std::wstring ExtensionText(const std::vector<std::wstring>& extensions) {
    std::wstring result;
    for (const auto& extension : extensions) {
        if (!result.empty()) result += L" ";
        result += L"." + extension;
    }
    return result;
}

l10n::StringId DescriptionId(std::wstring_view extension) {
    using Id = l10n::StringId;
    if (extension == L"png") return Id::PickerFormatNotePng;
    if (extension == L"jpg" || extension == L"jpeg" || extension == L"jpe") return Id::PickerFormatNoteJpeg;
    if (extension == L"webp") return Id::PickerFormatNoteWebp;
    if (extension == L"psd" || extension == L"pdd" || extension == L"psdt") return Id::PickerFormatNotePsd;
    if (extension == L"psb") return Id::PickerFormatNotePsb;
    if (extension == L"txt") return Id::PickerFormatNoteText;
    return static_cast<Id>(0);
}

} // namespace

bool PickerFilterIsAny(const PickerFilter& filter) {
    const auto tokens = Tokens(filter.pattern);
    return std::any_of(tokens.begin(), tokens.end(), IsAnyToken);
}

std::wstring PickerFilterName(const PickerFilter& filter) {
    std::wstring_view name = Trim(filter.label);
    // "PNG (*.png)", "文本文档（*.txt）": drop a trailing pattern list.
    if (!name.empty() && (name.back() == L')' || name.back() == L'\xFF09')) {
        const size_t open = name.find_last_of(L"(\xFF08");
        if (open != std::wstring_view::npos && name.substr(open).find(L'*') != std::wstring_view::npos) {
            const auto stripped = Trim(name.substr(0, open));
            if (!stripped.empty()) name = stripped;
        }
    }
    return std::wstring(name);
}

std::vector<std::wstring> PickerFilterExtensions(const PickerFilter& filter) {
    std::vector<std::wstring> extensions;
    for (const auto token : Tokens(filter.pattern)) {
        if (IsAnyToken(token) || token.size() < 3 || token.substr(0, 2) != L"*.") continue;
        std::wstring extension(token.substr(2));
        if (extension.find_first_of(L"*?") != std::wstring::npos) continue;
        for (auto& c : extension) c = static_cast<wchar_t>(std::towlower(c));
        if (std::find(extensions.begin(), extensions.end(), extension) == extensions.end())
            extensions.push_back(std::move(extension));
    }
    return extensions;
}

std::vector<std::wstring> PickerFormatLabels(const std::vector<PickerFilter>& filters) {
    std::vector<std::wstring> labels;
    labels.reserve(filters.size());
    for (const auto& filter : filters) {
        const std::wstring name = PickerFilterName(filter);
        if (PickerFilterIsAny(filter)) { labels.push_back(l10n::Get(l10n::StringId::PickerAnyFormat)); continue; }
        const auto extensions = PickerFilterExtensions(filter);
        if (extensions.empty()) { labels.push_back(name.empty() ? filter.pattern : name); continue; }
        const auto upper_name = Upper(name);
        // Preserve meaningful format-family names, not arbitrary short descriptions
        // such as Text / Images. Exact extension names may use a non-first suffix.
        if (upper_name == L"PHOTOSHOP" || upper_name == L"JPEG 2000") {
            labels.push_back(upper_name == L"PHOTOSHOP" ? L"Photoshop" : L"JPEG 2000");
        } else if (std::any_of(extensions.begin(), extensions.end(), [&](const auto& extension) { return Upper(extension) == upper_name; })) {
            labels.push_back(upper_name == L"WEBP" ? L"WebP" : upper_name);
        } else labels.push_back(Upper(extensions.front()));
    }
    // Three "EPS" types read as one: those keep the program's own names.
    std::vector<std::wstring> result = labels;
    for (size_t i = 0; i < labels.size(); ++i) {
        if (std::count(labels.begin(), labels.end(), labels[i]) < 2) continue;
        const auto name = PickerFilterName(filters[i]);
        if (!name.empty()) result[i] = name;
    }
    return result;
}

std::wstring PickerFormatNote(const PickerFilter& filter) {
    if (PickerFilterIsAny(filter)) return l10n::Get(l10n::StringId::PickerFormatNoteAny);
    const auto extensions = PickerFilterExtensions(filter);
    const auto extension_text = ExtensionText(extensions);
    // Do not describe a mixed filter (PNG/JPEG, or PNG plus a wildcard mask) as
    // if every file had the first extension's properties.
    if (!extensions.empty()) {
        const auto id = DescriptionId(extensions.front());
        const auto tokens = Tokens(filter.pattern);
        const bool concrete_only = std::all_of(tokens.begin(), tokens.end(), [](std::wstring_view token) {
            return token.size() > 2 && token.substr(0, 2) == L"*." &&
                token.substr(2).find_first_of(L"*?") == std::wstring_view::npos;
        });
        if (id != static_cast<l10n::StringId>(0) && concrete_only &&
            std::all_of(extensions.begin(), extensions.end(), [id](const auto& extension) { return DescriptionId(extension) == id; })) {
            std::wstring result = l10n::Get(id);
            if (extensions.size() > 1) result += L" · " + extension_text;
            return result;
        }
    }
    std::wstring result = PickerFilterName(filter);
    if (result.empty()) return extension_text.empty() ? filter.pattern : extension_text;
    if (!extension_text.empty()) result += L" · " + extension_text;
    return result;
}

std::wstring PickerFormatStaticText(const PickerFilter& filter) {
    std::wstring result = PickerFormatLabels({filter}).front();
    const std::wstring detail = PickerFilterIsAny(filter) ?
        l10n::Get(l10n::StringId::PickerFormatNoteAny) : ExtensionText(PickerFilterExtensions(filter));
    if (!detail.empty()) result += L" · " + detail;
    return result;
}

std::vector<int> PickerFormatSegmentOrder(const std::vector<PickerFilter>& filters, size_t selected,
                                          const std::vector<size_t>& recent) {
    std::vector<int> regular, any;
    for (size_t i = 0; i < filters.size(); ++i)
        (PickerFilterIsAny(filters[i]) ? any : regular).push_back(static_cast<int>(i));
    std::vector<int> order;
    if (regular.size() <= kPickerFormatMaxInline) {
        order = regular;
    } else {
        std::vector<size_t> wanted{selected};
        wanted.insert(wanted.end(), recent.begin(), recent.end());
        for (const int index : regular) wanted.push_back(static_cast<size_t>(index));
        for (const size_t index : wanted) {
            if (order.size() + 1 >= kPickerFormatMaxInline) break;
            const int value = static_cast<int>(index);
            if (std::find(regular.begin(), regular.end(), value) != regular.end() &&
                std::find(order.begin(), order.end(), value) == order.end()) order.push_back(value);
        }
        std::sort(order.begin(), order.end());
        order.push_back(kPickerFormatMore);
    }
    order.insert(order.end(), any.begin(), any.end());
    return order;
}

} // namespace pulse::ui
