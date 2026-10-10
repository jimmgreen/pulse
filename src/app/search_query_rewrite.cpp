#include "search_query_rewrite.h"
#include "../common/path_utils.h"
#include <algorithm>
#include <tuple>
#include <initializer_list>

namespace pulse::app {
struct AdvancedSearchOrigin { std::wstring raw; AdvancedSearchSpec fields; };
namespace {
enum Field : unsigned { Name = 1, Exclude = 2, Kind = 4, Scope = 8, Date = 16, Size = 32,
    Content = 64, ContentExclude = 128, ContentMode = 256, Whole = 512, Case = 1024, Pinyin = 2048 };
struct Token {
    std::wstring raw, body;
    bool quoted = false, negated = false;
};
// Preserve token spelling while following the backend's quote and OR boundaries.
// Classification below always uses the production parser rather than another AST.
std::vector<std::vector<Token>> TokenGroups(std::wstring_view raw) {
    std::vector<std::vector<Token>> groups(1);
    size_t i = 0;
    auto space = [&] { while (i < raw.size() && (raw[i] == L' ' || raw[i] == L'\t')) ++i; };
    while (i < raw.size()) {
        space(); if (i == raw.size()) break;
        if (raw[i] == L'|') { if (!groups.back().empty()) groups.emplace_back(); ++i; continue; }
        const auto start = i;
        Token token;
        while (i < raw.size() && raw[i] == L'!') { token.negated = !token.negated; ++i; }
        space(); if (i == raw.size()) break;
        const auto body = i;
        if (raw[i] == L'"') {
            token.quoted = true; ++i;
            while (i < raw.size() && raw[i] != L'"') ++i;
            if (i < raw.size()) ++i;
        } else {
            while (i < raw.size() && raw[i] != L' ' && raw[i] != L'\t' && raw[i] != L'|') {
                const auto ch = raw[i++];
                if ((ch == L':' || ch == L'：') && i < raw.size() && raw[i] == L'"') {
                    ++i; while (i < raw.size() && raw[i] != L'"') ++i;
                    if (i < raw.size()) ++i;
                    break;
                }
            }
        }
        if (body == i) continue;
        token.raw = raw.substr(start, i - start);
        token.body = raw.substr(body, i - body);
        groups.back().push_back(std::move(token));
    }
    return groups;
}
std::wstring Key(const Token& token) {
    if (token.quoted) return {};
    const auto colon = token.body.find_first_of(L":：");
    return index::Fold(token.body.substr(0, colon));
}
bool Is(std::wstring_view key, std::initializer_list<std::wstring_view> names) {
    return std::find(names.begin(), names.end(), key) != names.end();
}
unsigned Category(const Token& token, bool& scope_seen, bool& global, index::Term& term) {
    const auto parsed = index::ParseQuery(token.raw);
    const auto key = Key(token);
    global = true;
    if (!token.quoted && Is(key, {L"content", L"内容", L"包含"})) return token.negated ? ContentExclude : Content;
    if (!token.quoted && token.body.find_first_of(L":：") != std::wstring::npos) {
        if (Is(key, {L"contentmode", L"内容匹配"})) return ContentMode;
        if (Is(key, {L"ww", L"wholeword", L"全词"})) return Whole;
        if (Is(key, {L"case", L"casesensitive", L"区分大小写"})) return Case;
    }
    if (!parsed.pinyin_enabled) return Pinyin;
    if (!parsed.path_prefix.empty() && !scope_seen) { scope_seen = true; return Scope; }
    global = false;
    // A subsequent absolute path is a path predicate, not another global scope.
    if (!parsed.path_prefix.empty()) {
        const auto scoped = index::ParseQuery(L"path:C:\\ " + token.raw);
        if (!scoped.groups.empty() && !scoped.groups.front().empty()) term = scoped.groups.front().front();
        return 0;
    }
    if (parsed.groups.empty() || parsed.groups.front().empty()) return 0;
    term = parsed.groups.front().front();
    if (!token.quoted && Is(key, {L"size", L"大小"})) return Size;
    if (!token.quoted && Is(key, {L"dm", L"datemodified", L"修改", L"日期"})) return Date;
    unsigned field = (!term.exts.empty() || term.folder || term.file) ? Kind : 0;
    if (term.name_how != index::NameHow::Any && !term.name_in_path) field |= term.name_not ? Exclude : Name;
    return field;
}
std::wstring EffectiveScope(const AdvancedSearchSpec& spec) {
    if (spec.location == LocationScope::CurrentFolder) return path::StripExtendedPathPrefix(spec.current_folder);
    if (spec.location == LocationScope::CustomFolder) return path::StripExtendedPathPrefix(spec.custom_folder);
    return {};
}
void Append(std::wstring& target, std::wstring_view value) {
    if (value.empty()) return;
    if (!target.empty()) target += L' ';
    target += value;
}
std::wstring NamePredicate(const index::Term& term) {
    std::wstring value = term.name;
    if (term.name_how == index::NameHow::Exact) value = L"\"" + value + L"\"";
    else if (term.name_how == index::NameHow::Substring && value.find_first_of(L" :：|!") != std::wstring::npos)
        value = QuoteQueryValue(L"*" + value + L"*");
    else value = QuoteQueryValue(value);
    return (term.name_not ? L"!" : L"") + value;
}
}

void RememberSearchQuery(AdvancedSearchSpec& spec, std::wstring_view raw) {
    if (spec.date == DatePreset::Custom) {
        bool scope_seen = false;
        for (const auto& group : TokenGroups(raw)) for (const auto& token : group) {
            bool global; index::Term term;
            if (Category(token, scope_seen, global, term) != Date || token.negated) continue;
            const auto colon = token.body.find_first_of(L":：");
            if (colon == std::wstring::npos) continue;
            auto value = token.body.substr(colon + 1);
            if (value.size() >= 2 && value.front() == L'"' && value.back() == L'"') value = value.substr(1, value.size() - 2);
            const auto range = value.find(L"..");
            spec.date_from = value.substr(0, range);
            if (range != std::wstring::npos) spec.date_to = value.substr(range + 2);
            goto date_restored;
        }
    }
date_restored:
    auto origin = std::make_shared<AdvancedSearchOrigin>();
    origin->raw = raw; origin->fields = spec; origin->fields.origin.reset();
    spec.origin = std::move(origin);
}

std::wstring RewriteSearchQuery(const AdvancedSearchSpec& spec) {
    const auto& old = spec.origin->fields;
    unsigned changed = 0;
    AdvancedSearchSpec added;
    if (std::tie(spec.name, spec.name_how) != std::tie(old.name, old.name_how)) { changed |= Name; added.name = spec.name; added.name_how = spec.name_how; }
    if (spec.exclude_name != old.exclude_name) { changed |= Exclude; added.exclude_name = spec.exclude_name; }
    if (spec.kind != old.kind || (spec.kind == index::SearchKind::Custom && NormalizeExtensionList(spec.custom_exts) != NormalizeExtensionList(old.custom_exts))) {
        changed |= Kind; added.kind = spec.kind; added.custom_exts = spec.custom_exts;
    }
    if (EffectiveScope(spec) != EffectiveScope(old)) { changed |= Scope; added.location = spec.location; added.current_folder = spec.current_folder; added.custom_folder = spec.custom_folder; }
    if (std::tie(spec.date, spec.date_from, spec.date_to) != std::tie(old.date, old.date_from, old.date_to)) {
        changed |= Date; added.date = spec.date; added.date_from = spec.date_from; added.date_to = spec.date_to;
    }
    if (std::tie(spec.size, spec.size_custom) != std::tie(old.size, old.size_custom)) { changed |= Size; added.size = spec.size; added.size_custom = spec.size_custom; }
    if (spec.content != old.content) { changed |= Content | ContentMode; added.content = spec.content; added.content_mode = spec.content_mode; }
    if (spec.content_exclude != old.content_exclude) { changed |= ContentExclude; added.content_exclude = spec.content_exclude; }
    if (spec.content_mode != old.content_mode) { changed |= ContentMode | Content; added.content = spec.content; added.content_mode = spec.content_mode; }
    if (spec.whole_word != old.whole_word) { changed |= Whole; added.whole_word = spec.whole_word; }
    if (spec.case_sensitive != old.case_sensitive) { changed |= Case; added.case_sensitive = spec.case_sensitive; }
    if (spec.pinyin_enabled != old.pinyin_enabled) { changed |= Pinyin; added.pinyin_enabled = spec.pinyin_enabled; }
    if (!changed) return spec.origin->raw;

    std::wstring globals, extra;
    std::vector<std::wstring> branches;
    bool scope_seen = false, unconditional = false;
    for (const auto& group : TokenGroups(spec.origin->raw)) {
        std::wstring branch; bool predicate = false;
        for (const auto& token : group) {
            bool global; index::Term term;
            const auto field = Category(token, scope_seen, global, term);
            if (!global) predicate = true;
            if (!(field & changed)) {
                if (!global && term.name_in_path && (changed & Scope) && EffectiveScope(spec).empty() &&
                    !index::ParseQuery(token.raw).path_prefix.empty()) {
                    // With no global scope, keep a later absolute-path predicate
                    // from being consumed as the backend's new first scope.
                    auto value = term.name;
                    if (term.name_how == index::NameHow::Substring) value = L"*" + value + L"*";
                    Append(branch, L"path:" + QuoteQueryValue(value));
                } else Append(global ? globals : branch, token.raw);
            }
            else if ((field & Kind) && (field & (Name | Exclude))) {
                if (!(changed & Kind)) Append(branch, term.folder ? L"folder:" : L"file:");
                if (!(changed & (field & (Name | Exclude)))) Append(branch, NamePredicate(term));
            }
        }
        if (predicate) { unconditional |= branch.empty(); branches.push_back(std::move(branch)); }
    }
    scope_seen = false;
    for (const auto& group : TokenGroups(CompileSearchQuery(added))) for (const auto& token : group) {
        bool global; index::Term term; Category(token, scope_seen, global, term);
        Append(global ? globals : extra, token.raw);
    }
    if (branches.empty() || unconditional) branches = {L""};
    std::wstring query;
    for (auto& branch : branches) {
        Append(branch, extra);
        if (!query.empty()) query += L" | ";
        query += branch;
    }
    // Emit global scope before path predicates so the backend consumes the same
    // single global path field, including when the previous scope was replaced.
    Append(globals, query);
    return globals;
}

std::wstring CombineNativeQuery(std::wstring_view raw, std::wstring_view filters) {
    std::wstring globals, extra;
    std::vector<std::wstring> branches;
    bool scope_seen = false, unconditional = false;
    for (const auto& group : TokenGroups(raw)) {
        std::wstring branch; bool predicate = false;
        for (const auto& token : group) {
            bool global; index::Term term;
            Category(token, scope_seen, global, term);
            if (!global) predicate = true;
            Append(global ? globals : branch, token.raw);
        }
        if (predicate) { unconditional |= branch.empty(); branches.push_back(std::move(branch)); }
    }
    for (const auto& group : TokenGroups(filters)) for (const auto& token : group) {
        bool global; index::Term term;
        Category(token, scope_seen, global, term);
        Append(global ? globals : extra, token.raw);
    }
    if (branches.empty() || unconditional) branches = {L""};
    std::wstring query;
    for (auto& branch : branches) {
        Append(branch, extra);
        if (!query.empty()) query += L" | ";
        query += branch;
    }
    Append(globals, query);
    return globals;
}

std::wstring NameQueryDraft(std::wstring_view raw) {
    auto spec = ParseSearchQuery(raw);
    spec.location = LocationScope::Indexed;
    spec.current_folder.clear();
    spec.custom_folder.clear();
    return CompileSearchQuery(spec);
}

std::wstring CompileNameQueryInput(std::wstring_view raw, std::wstring_view folder) {
    if (folder.empty()) return std::wstring(raw);
    auto spec = ParseSearchQuery(raw);
    spec.location = LocationScope::CustomFolder;
    spec.custom_folder = folder;
    return CompileSearchQuery(spec);
}
} // namespace pulse::app
