#include "../app/search_query.h"
#include <iostream>

using namespace pulse;

namespace {

int failures = 0;

void Check(bool condition, const wchar_t* name) {
    std::wcout << (condition ? L"[PASS] " : L"[FAIL] ") << name << L'\n';
    if (!condition) ++failures;
}

} // namespace

int wmain() {
    app::AdvancedSearchSpec spec;
    spec.name = L"\u62a5\u544a";
    spec.kind = index::SearchKind::Document;
    spec.date = app::DatePreset::ThisWeek;
    spec.content = L"\u53d1\u7968 \u91d1\u989d";
    spec.content_mode = index::ContentMatchMode::AllWords;
    const std::wstring compiled = app::CompileSearchQuery(spec);
    Check(compiled.find(L"type:document") != std::wstring::npos, L"compile emits type:document");
    Check(compiled.find(L"dm:thisweek") != std::wstring::npos, L"compile emits dm:thisweek");
    Check(compiled.find(L"content:") != std::wstring::npos, L"compile emits content terms");

    const auto roundtrip = app::ParseSearchQuery(compiled);
    Check(roundtrip.kind == index::SearchKind::Document, L"parse restores document kind");
    Check(roundtrip.date == app::DatePreset::ThisWeek, L"parse restores this-week date");
    Check(roundtrip.content.find(L"\u53d1\u7968") != std::wstring::npos, L"parse restores content words");

    spec.content_mode = index::ContentMatchMode::Phrase;
    spec.content = L"hello world";
    const std::wstring phrase = app::CompileSearchQuery(spec);
    const auto split_phrase = app::SplitSearchQueryText(phrase);
    Check(split_phrase.content.mode == index::ContentMatchMode::Phrase, L"phrase mode roundtrip");

    spec = {};
    spec.content = L"todo";
    const std::wstring bare = app::CompileSearchQuery(spec);
    const auto split = app::SplitSearchQueryText(bare);
    Check(app::ContentSearchNeedsScope(split), L"content-only query needs a scope");
    const std::wstring guarded = app::ApplyContentSearchGuards(split.filename_needle, split);
    Check(guarded.find(L"ext:") != std::wstring::npos, L"guard adds known text extensions");

    spec.kind = index::SearchKind::Document;
    const std::wstring scoped = app::CompileSearchQuery(spec);
    Check(!app::ContentSearchNeedsScope(app::SplitSearchQueryText(scoped)),
          L"type filter satisfies content scope");

    spec = {};
    spec.location = app::LocationScope::CustomFolder;
    spec.custom_folder = L"C:\\Users\\TestUser\\Desktop";
    spec.content = L"alpha";
    const std::wstring located = app::CompileSearchQuery(spec);
    const auto split_loc = app::SplitSearchQueryText(located);
    Check(!split_loc.path_prefix.empty() && !app::ContentSearchNeedsScope(split_loc),
          L"folder location satisfies content scope");

    spec = {};
    spec.name = L"pulse";
    spec.name_how = app::NameMatchHow::StartsWith;
    Check(app::CompileSearchQuery(spec).find(L"pulse*") != std::wstring::npos,
          L"starts-with compiles to wildcard");

    spec = {};
    spec.size = app::SizePreset::From1To10MB;
    const auto size_roundtrip = app::ParseSearchQuery(app::CompileSearchQuery(spec));
    Check(size_roundtrip.size == app::SizePreset::From1To10MB,
          L"1-10MB size preset roundtrip");

    spec = {};
    spec.location = app::LocationScope::CustomFolder;
    spec.custom_folder = L"\\\\?\\C:\\Users\\TestUser\\Desktop";
    spec.content = L"alpha";
    const std::wstring stripped = app::CompileSearchQuery(spec);
    Check(stripped.find(L"\\\\?\\") == std::wstring::npos, L"query path drops \\\\?\\ prefix");
    Check(stripped.find(L"path:C:\\Users\\TestUser\\Desktop") != std::wstring::npos,
          L"query path uses a display Win32 path");

    spec = {};
    spec.content = L"\u53d1\u7968";
    spec.location = app::LocationScope::CustomFolder;
    spec.custom_folder = L"C:\\Users\\TestUser\\Desktop\\PulseSearchTest";
    const std::wstring titled = app::CompileSearchQuery(spec);
    Check(app::SearchDisplayNeedle(titled) == L"\u53d1\u7968",
          L"display needle uses content text not path:");
    Check(app::SearchDisplayNeedle(
              L"path:C:\\Users\\TestUser\\Desktop\\PulseSearchTest contentmode:any content:\u53d1\u7968")
              == L"\u53d1\u7968",
          L"display needle parses a scoped content query");

    Check(app::NormalizeExtensionList(L" .LOG , *.md; txt ") == L"LOG;md;txt",
          L"extension list strips dots stars and separators");
    spec = {};
    spec.kind = index::SearchKind::Custom;
    spec.custom_exts = L".log;md";
    const std::wstring custom = app::CompileSearchQuery(spec);
    Check(custom.find(L"ext:log;md") != std::wstring::npos, L"custom kind compiles to ext:");
    const auto parsed_custom = app::ParseSearchQuery(custom);
    Check(parsed_custom.kind == index::SearchKind::Custom &&
              parsed_custom.custom_exts.find(L"log") != std::wstring::npos,
          L"custom extensions roundtrip");

    spec = {};
    spec.name = L"annual report";
    spec.name_how = app::NameMatchHow::Contains;
    const std::wstring contains = app::CompileSearchQuery(spec);
    Check(contains.find(L"\"") == std::wstring::npos, L"multi-word contains stays unquoted");
    const auto contains_q = index::ParseQuery(contains);
    Check(contains_q.groups.size() == 1 && contains_q.groups[0].size() == 2 &&
              contains_q.groups[0][0].name_how == index::NameHow::Substring &&
              contains_q.groups[0][1].name_how == index::NameHow::Substring,
          L"multi-word contains compiles to substring tokens");
    const auto contains_spec = app::ParseSearchQuery(contains);
    Check(contains_spec.name_how == app::NameMatchHow::Contains &&
              contains_spec.name.find(L"annual") != std::wstring::npos &&
              contains_spec.name.find(L"report") != std::wstring::npos,
          L"multi-word contains roundtrip keeps Contains");

    spec = {};
    spec.name = L"annual report";
    spec.name_how = app::NameMatchHow::StartsWith;
    const std::wstring starts = app::CompileSearchQuery(spec);
    const auto starts_q = index::ParseQuery(starts);
    Check(starts_q.groups.size() == 1 && starts_q.groups[0].size() == 1 &&
              starts_q.groups[0][0].name_how == index::NameHow::Wildcard,
          L"multi-word starts-with compiles to one wildcard");
    const auto starts_spec = app::ParseSearchQuery(starts);
    Check(starts_spec.name_how == app::NameMatchHow::StartsWith &&
              starts_spec.name.find(L"annual") != std::wstring::npos,
          L"multi-word starts-with roundtrip");

    spec = {};
    spec.content = L"todo";
    spec.date = app::DatePreset::ThisMonth;
    const auto date_spec = app::ParseSearchQuery(app::CompileSearchQuery(spec));
    Check(date_spec.date == app::DatePreset::ThisMonth, L"thismonth preset roundtrip");
    const auto quoted_date = app::ParseSearchQuery(L"content:\"dm:thismonth\"");
    Check(quoted_date.date == app::DatePreset::Any,
          L"quoted dm:thismonth does not flip the date preset");

    for (const auto* raw : {L"a|b", L"!ext:log", L"ext:txt | ext:log", L"!draft !temp",
                            L"report dm:2020", L"report dm:2020-01-01..2020-12-31"}) {
        auto parsed = app::ParseSearchQuery(raw);
        Check(app::CompileSearchQuery(parsed) == raw, L"unchanged structured roundtrip preserves complete native syntax");
        parsed.location = app::LocationScope::CustomFolder; parsed.custom_folder = L"C:\\wanted";
        const auto scoped_roundtrip = index::ParseQuery(app::CompileSearchQuery(parsed));
        const auto original = index::ParseQuery(raw);
        Check(scoped_roundtrip.groups.size() == original.groups.size() && scoped_roundtrip.path_prefix == L"C:\\wanted",
              L"scope-only editing preserves boolean branches and applies requested location");
        for (const auto* name : {L"a.txt", L"b.log", L"draft.txt", L"temp.log", L"report.txt"}) {
            auto matches = [&](const auto& query) {
                for (const auto& group : query.groups) {
                    bool yes = true;
                    for (const auto& term : group) yes &= index::MatchTerm(name, name, false, 2000000, 132300000000000000ull, term);
                    if (yes) return true;
                }
                return query.groups.empty();
            };
            Check(matches(scoped_roundtrip) == matches(original), L"scope edit preserves filename/date/negation membership");
        }
    }
    const auto custom_date = app::ParseSearchQuery(L"dm:2020-01-01..2020-12-31");
    Check(custom_date.date_from == L"2020-01-01" && custom_date.date_to == L"2020-12-31", L"custom date bounds are restored into form");
    auto boolean = app::ParseSearchQuery(L"a|b"); boolean.kind = index::SearchKind::Custom; boolean.custom_exts = L"txt";
    const auto filtered = index::ParseQuery(app::CompileSearchQuery(boolean));
    Check(filtered.groups.size() == 2 && filtered.groups[0].size() == 2 && filtered.groups[1].size() == 2,
          L"new structured extension filter constrains every original OR branch");
    for (const auto* raw : {L"ext:pdf", L"size:>1mb", L"folder:", L"a|b", L"report ext:pdf", L"report"}) {
        app::AdvancedSearchSpec typed; typed.name = raw; typed.name_is_query = true;
        Check(app::CompileSearchQuery(typed) == raw, L"native address input matches global raw query independently of token count");
    }
    app::AdvancedSearchSpec typed_filters; typed_filters.name = L"a|b"; typed_filters.name_is_query = true;
    typed_filters.kind = index::SearchKind::Custom; typed_filters.custom_exts = L"txt";
    const auto combined = index::ParseQuery(app::CompileSearchQuery(typed_filters));
    Check(combined.groups.size() == 2 && combined.groups[0].size() == 2 && combined.groups[1].size() == 2,
          L"native OR input combines structured filters into every branch");
    app::AdvancedSearchSpec literal; literal.name = L"ext:pdf";
    Check(app::CompileSearchQuery(literal) == L"\"ext:pdf\"", L"advanced literal-name field retains explicit quoting contract");

    literal.name = L"report ext:pdf";
    Check(!index::QueryHasExtFilter(index::ParseQuery(app::CompileSearchQuery(literal))),
          L"advanced literal name does not gain native operators when another word is added");

    {
        app::AdvancedSearchSpec regex_spec;
        regex_spec.name = LR"(^aa[([]2022[)\]]xx$)";
        regex_spec.name_how = static_cast<app::NameMatchHow>(3);
        const auto raw_regex = app::CompileSearchQuery(regex_spec);
        const auto parsed_regex = index::ParseQuery(raw_regex);
        auto matches = [&](std::wstring_view name) {
            for (const auto& group : parsed_regex.groups) {
                bool ok = true;
                for (const auto& term : group)
                    ok = ok && index::MatchTerm(name, name, false, 0, 0, term);
                if (ok) return true;
            }
            return false;
        };
        Check(matches(L"aa(2022)xx") && matches(L"aa[2022]xx") && !matches(L"aa(2023)xx"),
              L"regex regression: parentheses and brackets match, other year rejected");
        Check(app::ParseSearchQuery(raw_regex).name_how == static_cast<app::NameMatchHow>(3),
              L"regex regression: advanced match mode survives query roundtrip");
    }

    {
        auto query_matches = [](std::wstring_view raw, std::wstring_view name, uint64_t size = 0) {
            const auto query = index::ParseQuery(raw);
            if (index::QueryHasInvalidRegex(query)) return false;
            for (const auto& group : query.groups) {
                bool ok = true;
                for (const auto& term : group)
                    ok = ok && index::MatchTerm(L"C:\\fixture\\" + std::wstring(name), name, false, size, 0, term);
                if (ok) return true;
            }
            return false;
        };
        app::AdvancedSearchSpec regex_spec;
        regex_spec.name_how = app::NameMatchHow::Regex;
        for (const auto* pattern : {LR"(^aa[([]2022[)\]]xx$)", LR"((Alpha|Beta)\D+\d{4}\.TXT$)",
                                   LR"(^(报告|照片) \d{4}\.txt$)", LR"re(^[^"]+"x$)re", L" foo "}) {
            regex_spec.name = pattern;
            const auto raw = app::CompileSearchQuery(regex_spec);
            auto parsed = app::ParseSearchQuery(raw);
            Check(parsed.name == pattern && parsed.name_how == app::NameMatchHow::Regex,
                  L"regex pattern roundtrip preserves escapes, OR, quotes, spaces and case");
            parsed.kind = index::SearchKind::Custom; parsed.custom_exts = L"txt";
            const auto edited = app::CompileSearchQuery(parsed);
            const auto again = app::ParseSearchQuery(edited);
            Check(again.name == pattern && again.name_how == app::NameMatchHow::Regex,
                  L"editing extensions preserves regex predicate");
            Check(index::FilenameQueryText(raw) == raw, L"filename query extraction preserves regex token");
        }
        Check(query_matches(LR"(regex:"^(Alpha|Beta)\D+\d{4}\.TXT$")", L"alpha-(2022.TXT"),
              L"regex alternation and uppercase escape D retain semantics under case-insensitive matching");
        Check(!query_matches(LR"(regex:"^\D+$")", L"2022") &&
              query_matches(LR"(regex:"^\D+$")", L"中文"),
              L"regex escape D is not folded to digit class");
        Check(query_matches(LR"(regex:"^(报告|照片) \d{4}\.txt$")", L"报告 2022.txt"),
              L"Unicode regex with spaces matches a filename");
        Check(query_matches(LR"(regex:"2022")", L"aa(2022)xx.txt") &&
              !query_matches(LR"(regex:"^2022$")", L"aa(2022)xx.txt"),
              L"regex searches substrings; anchors request whole-name matching");
        Check(query_matches(LR"(regex:"^aa" ext:txt size:>10)", L"aa(2022)xx.txt", 20) &&
              !query_matches(LR"(regex:"^aa" ext:txt size:>10)", L"aa(2022)xx.jpg", 20) &&
              !query_matches(LR"(regex:"^aa" ext:txt size:>10)", L"aa(2022)xx.txt", 5),
              L"regex composes with extension and size filters");
        Check(query_matches(LR"(!regex:"2022")", L"report2023.txt") &&
              !query_matches(LR"(!regex:"2022")", L"report2022.txt"),
              L"native regex negation excludes matching names");
        for (const auto* raw : {LR"(regex:"[")", LR"(!regex:"[")", LR"(regex:"")",
                                LR"(regex:"(" | alpha)"}) {
            Check(index::QueryHasInvalidRegex(index::ParseQuery(raw)) &&
                  !query_matches(raw, L"alpha.txt"), L"invalid regex fails closed, including negation and OR");
        }
        Check(index::QueryHasInvalidRegex(index::ParseQuery(index::RegexQueryToken(std::wstring(1025, L'a')))),
              L"regex length limit rejects oversized patterns");
        Check(!index::QueryIsSimpleName(index::ParseQuery(LR"(regex:"^aa")")) &&
              !index::QueryCanNarrow(LR"(regex:"aa")", LR"(regex:"aa|bb")"),
              L"regex bypasses literal candidate and incremental-narrowing optimizations");
        const auto q = index::ParseQuery(LR"(regex:"^aa")");
        Check(!q.groups.front().front().pinyin &&
              index::RankName(L"aa.txt", 6, false, q) > 0,
              L"regex ranking works without pinyin expansion");
        Check(!query_matches(L"aa[2022]xx", L"aa(2022)xx") &&
              query_matches(L"aa?2022?xx*", L"aa(2022)xx.txt"),
              L"literal brackets and wildcard behavior remain unchanged");
        auto parsed = app::ParseSearchQuery(LR"(regex:"^aa" ext:txt | regex:"^bb" ext:txt)");
        parsed.size = app::SizePreset::Gt10MB;
        auto rewritten = app::CompileSearchQuery(parsed);
        Check(query_matches(rewritten, L"aa.txt", 20 * 1024 * 1024) &&
              query_matches(rewritten, L"bb.txt", 20 * 1024 * 1024) &&
              !query_matches(rewritten, L"bb.txt", 1), L"OR regex branches retain added structured filters");
    }

    if (failures) {
        std::wcout << L"FAILED " << failures << L"\n";
        return 1;
    }
    std::wcout << L"All search query tests passed.\n";
    return 0;
}
