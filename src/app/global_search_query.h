#pragma once
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace pulse::app {
enum class GlobalSearchCommand { ToggleTheme, ToggleView, OpenStaging, ClearStaging, ContinueSearch, ClearHistory };
std::vector<GlobalSearchCommand> MatchGlobalSearchCommands(std::wstring_view raw);
struct GlobalSearchCalculationAlternative {
    std::wstring text;
    std::wstring query;
};
struct GlobalSearchCalculation {
    std::wstring text;
    double value = 0;
    bool has_value = false;
    std::vector<GlobalSearchCalculationAlternative> alternatives;
};
struct GlobalSearchQueryToken {
    std::wstring label;
    size_t start = 0;
    size_t length = 0;
};
struct GlobalSearchParsedQuery {
    std::wstring query;
    std::vector<GlobalSearchQueryToken> tokens;
};
GlobalSearchParsedQuery ParseGlobalSearchQuery(std::wstring_view raw);
std::wstring NormalizeGlobalSearchQuery(std::wstring_view raw);
std::optional<GlobalSearchCalculation> EvaluateGlobalSearchCalculation(
    std::wstring_view raw, double answer = 0);
}
