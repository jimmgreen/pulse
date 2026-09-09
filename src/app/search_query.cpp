#include "search_query.h"
#include "../common/path_utils.h"

#include <cwctype>

namespace pulse::app {
namespace {

void Trim(std::wstring& s) {
    size_t a = 0;
    while (a < s.size() && iswspace(s[a])) ++a;
    size_t b = s.size();
    while (b > a && iswspace(s[b - 1])) --b;
    s = s.substr(a, b - a);
}

std::wstring JoinWords(const std::vector<std::wstring>& words) {
    std::wstring out;
    for (const auto& w : words) {
        if (w.empty()) continue;
        if (!out.empty()) out.push_back(L' ');
        out.append(w);
    }
    return out;
}

std::vector<std::wstring> SplitWords(std::wstring_view s) {
    std::vector<std::wstring> out;
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && iswspace(s[i])) ++i;
        size_t j = i;
        while (j < s.size() && !iswspace(s[j])) ++j;
        if (j > i) out.emplace_back(s.substr(i, j - i));
        i = j;
    }
    return out;
}

bool EqualsI(std::wstring_view a, std::wstring_view b) {
    return index::Fold(a) == index::Fold(b);
}

index::SearchKind KindFromExts(const std::vector<std::wstring>& exts) {
    if (exts.empty()) return index::SearchKind::Any;
    std::wstring joined;
    for (size_t i = 0; i < exts.size(); ++i) {
        if (i) joined.push_back(L';');
        joined.append(exts[i]);
    }
    const index::SearchKind kinds[] = {
        index::SearchKind::Document, index::SearchKind::Image, index::SearchKind::Video,
        index::SearchKind::Audio, index::SearchKind::Archive, index::SearchKind::Code,
    };
    for (auto kind : kinds) {
        if (EqualsI(joined, index::KindExtensions(kind))) return kind;
    }
    return index::SearchKind::Custom;
}

std::wstring QuotePathQueryValue(std::wstring_view value) {
    std::wstring path = path::StripExtendedPathPrefix(value);
    Trim(path);
    bool need = false;
    for (wchar_t c : path) {
        if (c == L' ' || c == L'\t' || c == L'|' || c == L'"') {
            need = true;
            break;
        }
    }
    if (!need) return path;
    std::wstring out = L"\"";
    for (wchar_t c : path) {
        if (c != L'"') out.push_back(c);
    }
    out.push_back(L'"');
    return out;
}

DatePreset DetectDatePreset(std::wstring_view raw) {
    DatePreset found = DatePreset::Any;
    size_t i = 0;
    while (i < raw.size()) {
        if (raw[i] == L'"') {
            ++i;
            while (i < raw.size() && raw[i] != L'"') ++i;
            if (i < raw.size()) ++i;
            continue;
        }
        if (raw[i] == L' ' || raw[i] == L'\t' || raw[i] == L'|') {
            ++i;
            continue;
        }
        size_t j = i;
        while (j < raw.size() && raw[j] != L' ' && raw[j] != L'\t' && raw[j] != L'|' &&
               raw[j] != L'"')
            ++j;
        const std::wstring tok = index::Fold(raw.substr(i, j - i));
        if (found == DatePreset::Any) {
            if (tok == L"dm:today" || tok == L"datemodified:today") found = DatePreset::Today;
            else if (tok == L"dm:yesterday" || tok == L"datemodified:yesterday")
                found = DatePreset::Yesterday;
            else if (tok == L"dm:thisweek" || tok == L"datemodified:thisweek")
                found = DatePreset::ThisWeek;
            else if (tok == L"dm:thismonth" || tok == L"datemodified:thismonth")
                found = DatePreset::ThisMonth;
            else if (tok == L"dm:thisyear" || tok == L"datemodified:thisyear")
                found = DatePreset::ThisYear;
            else if (tok.starts_with(L"dm:") || tok.starts_with(L"datemodified:") ||
                     tok.starts_with(L"\u4fee\u6539:") || tok.starts_with(L"\u65e5\u671f:"))
                found = DatePreset::Custom;
        }
        i = j;
    }
    return found;
}

} // namespace

std::wstring QuoteQueryValue(std::wstring_view value) {
    bool need = false;
    for (wchar_t c : value) {
        if (c == L' ' || c == L'\t' || c == L'|' || c == L'"' || c == L':' || c == L'：') {
            need = true;
            break;
        }
    }
    if (!need) return std::wstring(value);
    std::wstring out = L"\"";
    for (wchar_t c : value) {
        if (c != L'"') out.push_back(c);
    }
    out.push_back(L'"');
    return out;
}

std::wstring CompileSearchQuery(const AdvancedSearchSpec& spec) {
    std::wstring q;
    auto append = [&](std::wstring_view token) {
        if (token.empty()) return;
        if (!q.empty()) q.push_back(L' ');
        q.append(token);
    };
    std::wstring name = spec.name;
    Trim(name);
    if (!name.empty()) {
        if (spec.name_how == NameMatchHow::Exact) {
            append(L"\"" + name + L"\"");
        } else if (spec.name_how == NameMatchHow::StartsWith) {
            if (name.find_first_of(L"*?") == std::wstring::npos) name += L"*";
            append(QuoteQueryValue(name));
        } else {
            const auto words = SplitWords(name);
            if (words.size() > 1) {
                for (const auto& word : words) append(word);
            } else {
                append(QuoteQueryValue(name));
            }
        }
    }
    std::wstring exclude = spec.exclude_name;
    Trim(exclude);
    if (!exclude.empty()) {
        append(L"!" + QuoteQueryValue(exclude));
    }
    if (spec.kind == index::SearchKind::Folder) {
        append(L"folder:");
    } else if (spec.kind == index::SearchKind::Custom) {
        const std::wstring exts = NormalizeExtensionList(spec.custom_exts);
        if (!exts.empty()) append(L"ext:" + exts);
    } else if (spec.kind != index::SearchKind::Any) {
        append(std::wstring(L"type:") + index::KindId(spec.kind));
    }
    std::wstring folder;
    if (spec.location == LocationScope::CustomFolder) folder = spec.custom_folder;
    else if (spec.location == LocationScope::CurrentFolder) folder = spec.current_folder;
    Trim(folder);
    if (!folder.empty()) {
        append(L"path:" + QuotePathQueryValue(folder));
    }
    switch (spec.date) {
    case DatePreset::Today: append(L"dm:today"); break;
    case DatePreset::Yesterday: append(L"dm:yesterday"); break;
    case DatePreset::ThisWeek: append(L"dm:thisweek"); break;
    case DatePreset::ThisMonth: append(L"dm:thismonth"); break;
    case DatePreset::ThisYear: append(L"dm:thisyear"); break;
    case DatePreset::Custom: {
        std::wstring a = spec.date_from, b = spec.date_to;
        Trim(a); Trim(b);
        if (!a.empty() && !b.empty()) append(L"dm:" + a + L".." + b);
        else if (!a.empty()) append(L"dm:" + a);
        else if (!b.empty()) append(L"dm:" + b);
        break;
    }
    default: break;
    }
    switch (spec.size) {
    case SizePreset::Empty: append(L"size:empty"); break;
    case SizePreset::Lt1MB: append(L"size:<1mb"); break;
    case SizePreset::From1To10MB: append(L"size:>=1mb") ; append(L"size:<=10mb"); break;
    case SizePreset::Gt10MB: append(L"size:>10mb"); break;
    case SizePreset::Custom: {
        std::wstring custom = spec.size_custom;
        Trim(custom);
        if (!custom.empty()) {
            if (custom.find(L"size:") == std::wstring::npos &&
                custom.find(L"大小:") == std::wstring::npos)
                append(L"size:" + custom);
            else
                append(custom);
        }
        break;
    }
    default: break;
    }
    std::wstring content = spec.content;
    Trim(content);
    if (!content.empty()) {
        if (spec.content_mode == index::ContentMatchMode::Phrase) {
            append(L"content:" + QuoteQueryValue(content));
            append(L"contentmode:phrase");
        } else if (spec.content_mode == index::ContentMatchMode::AnyWord) {
            append(L"contentmode:any");
            for (const auto& word : SplitWords(content))
                append(L"content:" + QuoteQueryValue(word));
        } else {
            for (const auto& word : SplitWords(content))
                append(L"content:" + QuoteQueryValue(word));
        }
    }
    std::wstring excluded = spec.content_exclude;
    Trim(excluded);
    if (!excluded.empty()) {
        for (const auto& word : SplitWords(excluded))
            append(L"!content:" + QuoteQueryValue(word));
    }
    if (spec.whole_word) append(L"ww:");
    if (spec.case_sensitive) append(L"case:");
    return q;
}

AdvancedSearchSpec ParseSearchQuery(std::wstring_view raw, std::wstring_view current_folder) {
    AdvancedSearchSpec spec;
    spec.current_folder = path::StripExtendedPathPrefix(current_folder);
    const auto compiled = index::ParseQuery(raw);
    spec.content_mode = compiled.content.mode;
    spec.whole_word = compiled.content.whole_word;
    spec.case_sensitive = compiled.content.case_sensitive;
    spec.content = JoinWords(compiled.content.needles);
    spec.content_exclude = JoinWords(compiled.content.excluded);
    if (!compiled.path_prefix.empty()) {
        spec.custom_folder = compiled.path_prefix;
        if (!spec.current_folder.empty() &&
            EqualsI(spec.custom_folder, spec.current_folder))
            spec.location = LocationScope::CurrentFolder;
        else
            spec.location = LocationScope::CustomFolder;
    }
    bool saw_name = false;
    bool saw_ge_1mb = false;
    bool saw_le_10mb = false;
    for (const auto& group : compiled.groups) {
        for (const auto& term : group) {
            if (term.folder && term.name_how == index::NameHow::Any && term.exts.empty())
                spec.kind = index::SearchKind::Folder;
            if (!term.exts.empty() && spec.kind != index::SearchKind::Folder) {
                spec.kind = KindFromExts(term.exts);
                if (spec.kind == index::SearchKind::Custom) {
                    spec.custom_exts.clear();
                    for (size_t i = 0; i < term.exts.size(); ++i) {
                        if (i) spec.custom_exts.push_back(L';');
                        spec.custom_exts.append(term.exts[i]);
                    }
                }
            }
            if (term.size_how == index::SizeHow::Ge && term.size_lo == 1024ull * 1024ull)
                saw_ge_1mb = true;
            if (term.size_how == index::SizeHow::Le && term.size_lo == 10ull * 1024ull * 1024ull)
                saw_le_10mb = true;
            if (term.size_how == index::SizeHow::Eq && term.size_lo == 0)
                spec.size = SizePreset::Empty;
            else if (term.size_how == index::SizeHow::Lt && term.size_lo == 1024ull * 1024ull)
                spec.size = SizePreset::Lt1MB;
            else if (term.size_how == index::SizeHow::Gt &&
                     term.size_lo == 10ull * 1024ull * 1024ull)
                spec.size = SizePreset::Gt10MB;
            else if (term.size_how != index::SizeHow::Any && spec.size == SizePreset::Any)
                spec.size = SizePreset::Custom;
            if (term.name_how != index::NameHow::Any && !term.name_in_path) {
                if (term.name_not) {
                    spec.exclude_name = term.name;
                } else if (!saw_name) {
                    spec.name = term.name;
                    if (term.name_how == index::NameHow::Exact) spec.name_how = NameMatchHow::Exact;
                    else if (term.name_how == index::NameHow::Wildcard &&
                             !term.name.empty() && term.name.back() == L'*' &&
                             term.name.find(L'?') == std::wstring::npos &&
                             term.name.find(L'*') == term.name.size() - 1) {
                        spec.name.pop_back();
                        spec.name_how = NameMatchHow::StartsWith;
                    } else {
                        spec.name_how = NameMatchHow::Contains;
                    }
                    saw_name = true;
                } else if (!term.name_not && spec.name_how == NameMatchHow::Contains &&
                           term.name_how == index::NameHow::Substring) {
                    spec.name.push_back(L' ');
                    spec.name.append(term.name);
                }
            }
        }
    }
    if (saw_ge_1mb && saw_le_10mb) spec.size = SizePreset::From1To10MB;
    spec.date = DetectDatePreset(raw);
    return spec;
}

SplitSearchQuery SplitSearchQueryText(std::wstring_view raw) {
    SplitSearchQuery split;
    const auto compiled = index::ParseQuery(raw);
    split.filename_needle = index::FilenameQueryText(raw);
    split.path_prefix = compiled.path_prefix;
    split.content = compiled.content;
    split.has_ext = index::QueryHasExtFilter(compiled);
    split.has_name = index::QueryHasNameFilter(compiled);
    split.has_folder = index::QueryHasFolderFilter(compiled);
    return split;
}

bool ContentSearchNeedsScope(const SplitSearchQuery& split) {
    if (!split.content.present()) return false;
    if (!split.path_prefix.empty()) return false;
    if (split.has_ext || split.has_name || split.has_folder) return false;
    return true;
}

std::wstring ApplyContentSearchGuards(std::wstring filename_needle, const SplitSearchQuery& split) {
    if (!split.content.present()) return filename_needle;
    if (split.has_ext || split.has_folder) return filename_needle;
    if (!filename_needle.empty()) filename_needle.push_back(L' ');
    filename_needle.append(index::TextExtensionQueryToken());
    return filename_needle;
}

std::wstring SearchDisplayNeedle(std::wstring_view raw) {
    const auto spec = ParseSearchQuery(raw);
    if (!spec.content.empty()) return spec.content;
    if (!spec.name.empty()) return spec.name;
    return {};
}

std::wstring NormalizeExtensionList(std::wstring_view raw) {
    std::wstring out;
    size_t i = 0;
    while (i < raw.size()) {
        while (i < raw.size() && (iswspace(raw[i]) || raw[i] == L';' || raw[i] == L',' ||
                                  raw[i] == L'*'))
            ++i;
        size_t j = i;
        while (j < raw.size() && !iswspace(raw[j]) && raw[j] != L';' && raw[j] != L',') ++j;
        std::wstring one(raw.substr(i, j - i));
        while (!one.empty() && one.front() == L'.') one.erase(one.begin());
        while (!one.empty() && one.back() == L'.') one.pop_back();
        if (!one.empty()) {
            if (!out.empty()) out.push_back(L';');
            out.append(one);
        }
        i = j;
    }
    return out;
}

} // namespace pulse::app
