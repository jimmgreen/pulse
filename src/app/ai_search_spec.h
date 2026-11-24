// ai_search_spec.h - Pulse 助手's search_files tool as Pulse's own search.
//
// The model turns a sentence into conditions (name, type or extensions,
// modified dates, size, folder); they become an AdvancedSearchSpec, so the
// query in the search box, the advanced search dialog and the chips on the
// conversation's 搜索条件 card all describe the same search. Removing a
// chip clears one field and searches again locally (no model call).
// No AppState, no UI: pulse_ai_tools_test runs it.
#pragma once
#include "ai_broker.h"
#include "search_query.h"
#include <string>
#include <vector>

namespace pulse::app::ai {

// search_files {name?, exclude?, type?, extensions?, content?, content_exclude?,
// content_mode?, modified_after?, modified_before?, larger_than?, smaller_than?,
// folder?}: at least one condition. False with `error` (invalid_args) when refused.
bool SearchSpecFromArgs(const json::Value& args, AdvancedSearchSpec& spec, ToolOutcome& error);

enum class SearchField : uint8_t { Name, Exclude, Type, Content, Date, Size, Folder };

struct SearchChip {
    SearchField field = SearchField::Name;
    std::wstring key;       // 名称
    std::wstring value;     // 含「发票」
    bool fixed = false;     // 所有位置: nothing to remove
};

// The card's chips in the design's order: name, exclude, type, content,
// modified, size, then where (所有位置 when no folder). `folder_label`
// names a folder for the 位置 chip (empty: the folder's last part).
std::vector<SearchChip> SearchChips(const AdvancedSearchSpec& spec,
                                    std::wstring (*folder_label)(const std::wstring&) = nullptr);
// The spec of a query edited in Pulse (the advanced search dialog, the search
// box): ParseSearchQuery plus the dm: range it keeps only as text; it then
// compiles from its fields.
AdvancedSearchSpec SearchSpecFromQuery(const std::wstring& query);
// Clears one condition (the spec stays a valid search).
void ClearSearchField(AdvancedSearchSpec& spec, SearchField field);
// True when the spec still has a condition besides where to look.
bool SearchHasCondition(const AdvancedSearchSpec& spec);
// "9月1日 – 9月30日" for a dm range; `this_year` drops the year when it matches.
std::wstring SearchDateText(const std::wstring& from, const std::wstring& to, int this_year);

} // namespace pulse::app::ai
