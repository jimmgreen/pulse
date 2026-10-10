#include "search_query.h"
#include "search_query_rewrite.h"
#include "../common/path_utils.h"

#include <algorithm>
#include <cwctype>
#include <vector>

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

// Raw size tokens ("size:>2mb size:<5mb") so a custom range survives a round trip.
std::wstring CollectSizeTokens(std::wstring_view raw) {
    std::wstring out;
    for (const auto& word : SplitWords(raw)) {
        const std::wstring tok = index::Fold(word);
        if (!tok.starts_with(L"size:") && !tok.starts_with(L"\u5927\u5c0f:")) continue;
        if (!out.empty()) out.push_back(L' ');
        out.append(word);
    }
    return out;
}

bool ParseSmartSize(const std::wstring& tok, AdvancedSearchSpec& spec) {
    size_t i = 0;
    if (i < tok.size() && (tok[i] == L'>' || tok[i] == L'<')) ++i; else return false;
    if (i < tok.size() && tok[i] == L'=') ++i;
    const size_t digits = i;
    while (i < tok.size() && (iswdigit(tok[i]) || tok[i] == L'.')) ++i;
    if (i == digits) return false;
    const std::wstring unit = tok.substr(i);
    if (!unit.empty() && unit != L"k" && unit != L"kb" && unit != L"m" && unit != L"mb" &&
        unit != L"g" && unit != L"gb") return false;
    if (tok == L">10mb" || tok == L">10m") spec.size = SizePreset::Gt10MB;
    else if (tok == L">100mb" || tok == L">100m") spec.size = SizePreset::Gt100MB;
    else if (tok == L">1gb" || tok == L">1g") spec.size = SizePreset::Gt1GB;
    else if (tok == L"<1mb" || tok == L"<1m") spec.size = SizePreset::Lt1MB;
    else {
        spec.size = SizePreset::Custom;
        spec.size_custom = L"size:" + tok;
    }
    return true;
}

} // namespace

std::wstring QuoteQueryValue(std::wstring_view value) {
    bool need = !value.empty() && value.front() == L'!';
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
    if (spec.name_is_query) {
        // The name holds native input typed by the user: keep its syntax and
        // add the structured fields to every OR branch.
        auto fields = spec;
        fields.name.clear();
        fields.name_is_query = false;
        fields.origin.reset();
        std::wstring raw = spec.name;
        Trim(raw);
        const auto filters = CompileSearchQuery(fields);
        return filters.empty() ? raw : CombineNativeQuery(raw, filters);
    }
    if (spec.origin) return RewriteSearchQuery(spec);
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
                for (const auto& word : words) append(QuoteQueryValue(word));
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
    case SizePreset::Gt100MB: append(L"size:>100mb"); break;
    case SizePreset::Gt1GB: append(L"size:>1gb"); break;
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
    if (!spec.pinyin_enabled) append(L"nopinyin:");
    return q;
}

AdvancedSearchSpec ParseSearchQuery(std::wstring_view raw, std::wstring_view current_folder) {
    AdvancedSearchSpec spec;
    spec.current_folder = path::StripExtendedPathPrefix(current_folder);
    const auto compiled = index::ParseQuery(raw);
    spec.pinyin_enabled = compiled.pinyin_enabled;
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
            else if (term.size_how == index::SizeHow::Gt &&
                     term.size_lo == 100ull * 1024ull * 1024ull)
                spec.size = SizePreset::Gt100MB;
            else if (term.size_how == index::SizeHow::Gt &&
                     term.size_lo == 1024ull * 1024ull * 1024ull)
                spec.size = SizePreset::Gt1GB;
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
    if (spec.size == SizePreset::Custom) spec.size_custom = CollectSizeTokens(raw);
    spec.date = DetectDatePreset(raw);
    RememberSearchQuery(spec, raw);
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

bool ExtractSmartFilters(std::wstring_view text, AdvancedSearchSpec& spec, std::wstring& rest) {
    using index::SearchKind;
    static constexpr const wchar_t* kExts[] = {
        L"pdf", L"doc", L"docx", L"xls", L"xlsx", L"ppt", L"pptx", L"txt", L"md", L"csv",
        L"rtf", L"jpg", L"jpeg", L"png", L"gif", L"bmp", L"webp", L"heic", L"svg", L"mp4",
        L"mkv", L"avi", L"mov", L"mp3", L"wav", L"flac", L"m4a", L"zip", L"rar", L"7z",
        L"iso", L"exe", L"msi", L"dwg", L"psd",
    };
    struct KindWord { const wchar_t* word; SearchKind kind; };
    static constexpr KindWord kKinds[] = {
        {L"\u6587\u4ef6\u5939", SearchKind::Folder}, {L"folder", SearchKind::Folder},
        {L"folders", SearchKind::Folder}, {L"\u6587\u6863", SearchKind::Document},
        {L"document", SearchKind::Document}, {L"documents", SearchKind::Document},
        {L"docs", SearchKind::Document}, {L"\u56fe\u7247", SearchKind::Image},
        {L"\u7167\u7247", SearchKind::Image}, {L"image", SearchKind::Image},
        {L"images", SearchKind::Image}, {L"photo", SearchKind::Image},
        {L"photos", SearchKind::Image}, {L"pictures", SearchKind::Image},
        {L"\u89c6\u9891", SearchKind::Video}, {L"video", SearchKind::Video},
        {L"videos", SearchKind::Video}, {L"\u97f3\u4e50", SearchKind::Audio},
        {L"\u97f3\u9891", SearchKind::Audio}, {L"music", SearchKind::Audio},
        {L"audio", SearchKind::Audio}, {L"\u538b\u7f29\u5305", SearchKind::Archive},
        {L"archive", SearchKind::Archive}, {L"archives", SearchKind::Archive},
        {L"\u4ee3\u7801", SearchKind::Code},
    };
    struct DateWord { const wchar_t* word; DatePreset date; };
    static constexpr DateWord kDates[] = {
        {L"\u4eca\u5929", DatePreset::Today}, {L"today", DatePreset::Today},
        {L"\u6628\u5929", DatePreset::Yesterday}, {L"yesterday", DatePreset::Yesterday},
        {L"\u672c\u5468", DatePreset::ThisWeek}, {L"\u8fd9\u5468", DatePreset::ThisWeek},
        {L"\u672c\u6708", DatePreset::ThisMonth}, {L"\u8fd9\u4e2a\u6708", DatePreset::ThisMonth},
        {L"\u4eca\u5e74", DatePreset::ThisYear},
    };
    const auto words = SplitWords(text);
    std::vector<std::wstring> kept;
    std::vector<std::wstring> exts;
    bool found = false;
    for (size_t i = 0; i < words.size(); ++i) {
        const std::wstring w = index::Fold(words[i]);
        if (w.find(L':') != std::wstring::npos || w.find(L'\uff1a') != std::wstring::npos) {
            kept.push_back(words[i]);  // Already query syntax: leave it alone.
            continue;
        }
        if (w == L"this" && i + 1 < words.size() && spec.date == DatePreset::Any) {
            const std::wstring next = index::Fold(words[i + 1]);
            const DatePreset date = next == L"week" ? DatePreset::ThisWeek
                : next == L"month" ? DatePreset::ThisMonth
                : next == L"year" ? DatePreset::ThisYear : DatePreset::Any;
            if (date != DatePreset::Any) {
                spec.date = date;
                found = true;
                ++i;
                continue;
            }
        }
        bool used = false;
        const std::wstring bare = !w.empty() && w.front() == L'.' ? w.substr(1) : w;
        if (spec.kind == SearchKind::Any || spec.kind == SearchKind::Custom) {
            for (const wchar_t* ext : kExts) {
                if (bare == ext) {
                    if (std::find(exts.begin(), exts.end(), bare) == exts.end()) exts.push_back(bare);
                    used = true;
                    break;
                }
            }
        }
        if (!used && spec.kind == SearchKind::Any && exts.empty()) {
            for (const auto& kind : kKinds) {
                if (w == kind.word) { spec.kind = kind.kind; used = true; break; }
            }
        }
        if (!used && spec.date == DatePreset::Any) {
            for (const auto& date : kDates) {
                if (w == date.word) { spec.date = date.date; used = true; break; }
            }
        }
        if (!used && spec.size == SizePreset::Any) used = ParseSmartSize(w, spec);
        if (used) found = true;
        else kept.push_back(words[i]);
    }
    if (!exts.empty()) {
        spec.kind = KindFromExts(exts);
        spec.custom_exts.clear();
        if (spec.kind == SearchKind::Custom) {
            for (const auto& ext : exts) {
                if (!spec.custom_exts.empty()) spec.custom_exts.push_back(L';');
                spec.custom_exts.append(ext);
            }
        }
    }
    rest = JoinWords(kept);
    return found;
}

namespace {
bool SpecHasFilters(const AdvancedSearchSpec& spec) {
    return spec.kind != index::SearchKind::Any || spec.date != DatePreset::Any ||
           spec.size != SizePreset::Any;
}
} // namespace

int SearchEmptyActions(std::wstring_view raw, SearchEmptyAction out[3]) {
    const AdvancedSearchSpec spec = ParseSearchQuery(raw);
    int n = 0;
    if (SpecHasFilters(spec)) out[n++] = SearchEmptyAction::ClearFilters;
    if (spec.content.empty() && !spec.name.empty()) out[n++] = SearchEmptyAction::SearchContent;
    if (spec.location != LocationScope::Indexed) out[n++] = SearchEmptyAction::SearchEverywhere;
    return n;
}

std::wstring ApplySearchEmptyAction(std::wstring_view raw, SearchEmptyAction action) {
    AdvancedSearchSpec spec = ParseSearchQuery(raw);
    switch (action) {
    case SearchEmptyAction::ClearFilters:
        spec.kind = index::SearchKind::Any;
        spec.custom_exts.clear();
        spec.date = DatePreset::Any;
        spec.date_from.clear();
        spec.date_to.clear();
        spec.size = SizePreset::Any;
        spec.size_custom.clear();
        break;
    case SearchEmptyAction::SearchContent:
        spec.content = spec.name;
        spec.name.clear();
        break;
    case SearchEmptyAction::SearchEverywhere:
        spec.location = LocationScope::Indexed;
        spec.current_folder.clear();
        spec.custom_folder.clear();
        break;
    }
    return CompileSearchQuery(spec);
}

} // namespace pulse::app
