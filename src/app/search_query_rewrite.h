#pragma once
#include "search_query.h"

namespace pulse::app {
void RememberSearchQuery(AdvancedSearchSpec& spec, std::wstring_view raw);
std::wstring RewriteSearchQuery(const AdvancedSearchSpec& spec);
// Appends compiled structured filters to every OR branch of native input.
std::wstring CombineNativeQuery(std::wstring_view raw, std::wstring_view filters);
} // namespace pulse::app
