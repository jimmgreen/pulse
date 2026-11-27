// search_query.h — Structured advanced-search form ↔ Everything-style query.
#pragma once
#include "../index/index_query.h"
#include "../index/search_kinds.h"
#include <memory>
#include <string>
#include <string_view>
#include <memory>

namespace pulse::app {

enum class NameMatchHow : uint8_t { Contains, StartsWith, Exact, Regex };
enum class LocationScope : uint8_t { Indexed, CurrentFolder, CustomFolder };
enum class SizePreset : uint8_t { Any, Empty, Lt1MB, From1To10MB, Gt10MB, Gt100MB, Gt1GB, Custom };
enum class DatePreset : uint8_t { Any, Today, Yesterday, ThisWeek, ThisMonth, ThisYear, Custom };

struct AdvancedSearchOrigin;

struct AdvancedSearchSpec {
    std::wstring name;
    bool name_is_query = false;
    std::wstring original_query;
    std::shared_ptr<const AdvancedSearchSpec> parsed_baseline;
    bool pinyin_enabled = true;
    NameMatchHow name_how = NameMatchHow::Contains;
    index::SearchKind kind = index::SearchKind::Any;
    std::wstring custom_exts;
    LocationScope location = LocationScope::Indexed;
    std::wstring current_folder;
    std::wstring custom_folder;
    DatePreset date = DatePreset::Any;
    std::wstring date_from;
    std::wstring date_to;
    SizePreset size = SizePreset::Any;
    std::wstring size_custom;
    std::wstring content;
    index::ContentMatchMode content_mode = index::ContentMatchMode::AllWords;
    std::wstring content_exclude;
    bool whole_word = false;
    bool case_sensitive = false;
    std::wstring exclude_name;
    std::shared_ptr<const AdvancedSearchOrigin> origin;
};

struct SplitSearchQuery {
    std::wstring filename_needle;
    std::wstring path_prefix;
    index::ContentClause content;
    bool has_ext = false;
    bool has_name = false;
    bool has_folder = false;
};

std::wstring QuoteQueryValue(std::wstring_view value);
std::wstring CompileSearchQuery(const AdvancedSearchSpec& spec);
// Native name-search input; an explicit folder overrides only the global path scope.
std::wstring CompileNameQueryInput(std::wstring_view raw, std::wstring_view folder = {});
std::wstring NameQueryDraft(std::wstring_view raw);
AdvancedSearchSpec ParseSearchQuery(std::wstring_view raw, std::wstring_view current_folder = {});
SplitSearchQuery SplitSearchQueryText(std::wstring_view raw);
bool ContentSearchNeedsScope(const SplitSearchQuery& split);
std::wstring ApplyContentSearchGuards(std::wstring filename_needle, const SplitSearchQuery& split);
// Short label for titles: content text, else name, else empty. Never a raw path: query.
std::wstring SearchDisplayNeedle(std::wstring_view raw);
// " .log , md " -> "log;md". Empty if nothing usable remains.
std::wstring NormalizeExtensionList(std::wstring_view raw);
// Recognises filter words typed into the search box ("pdf", "图片", "本周", ">10mb").
// Recognised words are applied to `spec` (only where it has no value yet) and the
// remaining words are returned in `rest`. Returns false when nothing was recognised.
bool ExtractSmartFilters(std::wstring_view text, AdvancedSearchSpec& spec, std::wstring& rest);

// Follow-up suggestions shown on an empty search result page.
enum class SearchEmptyAction : uint8_t { ClearFilters, SearchContent, SearchEverywhere };
// Fills `out` (up to 3) with the suggestions that would change `raw`; returns the count.
int SearchEmptyActions(std::wstring_view raw, SearchEmptyAction out[3]);
// The query `raw` rewritten by `action`.
std::wstring ApplySearchEmptyAction(std::wstring_view raw, SearchEmptyAction action);

} // namespace pulse::app
