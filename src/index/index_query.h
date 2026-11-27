// index_query.h — Everything-style query compile (plan.md 阶段 2.5 B).
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace pulse::index {

const wchar_t* FoldTable();
inline wchar_t FoldChar(wchar_t c) { return FoldTable()[static_cast<uint16_t>(c)]; }
std::wstring Fold(std::wstring_view s);

bool ContainsFolded(const wchar_t* s, uint32_t n, const std::wstring& needle);
bool EqualsFolded(const wchar_t* s, uint32_t n, const std::wstring& needle);
bool StartsWithFolded(const wchar_t* s, uint32_t n, const std::wstring& needle);
bool WildcardFolded(const wchar_t* s, uint32_t n, const std::wstring& pat);
bool WordStartFolded(const wchar_t* s, uint32_t n, const std::wstring& needle);

enum class NameHow : uint8_t { Any, Substring, Exact, Wildcard, Regex };
enum class SizeHow : uint8_t { Any, Eq, Gt, Ge, Lt, Le, Range };
enum class DateHow : uint8_t { Any, Range };
enum class ContentMatchMode : uint8_t { AllWords, Phrase, AnyWord };

struct NameRegex;

struct Term {
    std::shared_ptr<const NameRegex> name_regex;
    std::wstring name; // case-folded except regex, whose escapes are case-sensitive
    NameHow name_how = NameHow::Any;
    bool name_in_path = false;
    bool name_not = false;
    bool pinyin = false;

    std::vector<std::wstring> exts; // folded, no leading dot
    bool ext_not = false;

    bool folder = false;
    bool file = false;

    SizeHow size_how = SizeHow::Any;
    uint64_t size_lo = 0;
    uint64_t size_hi = 0; // Range: exclusive high; 0 = open
    bool size_not = false;

    DateHow date_how = DateHow::Any;
    uint64_t date_lo = 0; // FILETIME
    uint64_t date_hi = 0; // exclusive
    bool date_not = false;
};

struct ContentClause {
    std::vector<std::wstring> needles;
    std::vector<std::wstring> excluded;
    ContentMatchMode mode = ContentMatchMode::AllWords;
    bool whole_word = false;
    bool case_sensitive = false;

    bool present() const { return !needles.empty() || !excluded.empty(); }
};

struct CompiledQuery {
    std::vector<std::vector<Term>> groups; // OR of AND-groups
    ContentClause content;
    bool pinyin_enabled = true;
    std::wstring path_prefix; // original casing; empty = whole index
};

CompiledQuery ParseQuery(std::wstring_view raw);
bool QueryHasInvalidRegex(const CompiledQuery& q);
std::wstring RegexQueryToken(std::wstring_view pattern);
std::wstring FilenameQueryText(std::wstring_view raw);
std::wstring QueryWithoutPathPrefix(std::wstring_view raw);
bool QueryCanNarrow(std::wstring_view prev, std::wstring_view next);
bool QueryUsesAttrs(const CompiledQuery& q);
size_t QueryPrimaryNameLen(const CompiledQuery& q);
bool QueryIsSimpleName(const CompiledQuery& q);
bool QueryHasPinyin(const CompiledQuery& q);
bool QueryHasContent(const CompiledQuery& q);
bool QueryHasExtFilter(const CompiledQuery& q);
bool QueryHasNameFilter(const CompiledQuery& q);
bool QueryHasFolderFilter(const CompiledQuery& q);
bool MatchName(const wchar_t* s, uint32_t n, const Term& t);
bool MatchExt(const wchar_t* s, uint32_t n, const Term& t);
bool MatchSize(uint64_t bytes, const Term& t);
bool MatchDate(uint64_t filetime, const Term& t);
bool MatchTerm(std::wstring_view path, std::wstring_view name, bool is_dir,
               uint64_t size, uint64_t modified, const Term& term);
int RankName(const wchar_t* s, uint32_t n, bool is_dir, const CompiledQuery& q);

} // namespace pulse::index
