#include "../app/global_search_query.h"
#include "../index/index_query.h"
#include <cmath>
#include <iostream>

int main() {
    using namespace pulse::app;
    int failures = 0;
    auto check = [&](bool ok, const char* name) { std::cout << (ok ? "[PASS] " : "[FAIL] ") << name << '\n'; if (!ok) ++failures; };
    auto value = [&](const wchar_t* input, double expected, double ans = 0) {
        const auto result = EvaluateGlobalSearchCalculation(input, ans);
        return result && result->has_value && std::abs(result->value - expected) < 1e-8;
    };
    check(value(L"12.5*8+3", 103), "arithmetic precedence");
    check(value(L"2^3^2", 512) && value(L"-2^2", -4), "power and unary precedence");
    check(value(L"ans*2", 14, 7), "previous answer");
    check(value(L"sqrt(81)+5%2", 10), "sqrt and remainder");
    check(value(L"5kg to lb", 5/.45359237), "mass conversion");
    check(value(L"-40c to f", -40), "negative temperature");
    check(value(L"25% of 200", 50), "percentage");
    check(!EvaluateGlobalSearchCalculation(L"1/0") && !EvaluateGlobalSearchCalculation(L"sqrt(-1)"), "non-finite rejected");
    check(!EvaluateGlobalSearchCalculation(L"2kg to m") && !EvaluateGlobalSearchCalculation(L"2+bad") && !EvaluateGlobalSearchCalculation(L"123"), "invalid input preserved for file search");
    check(!EvaluateGlobalSearchCalculation(std::wstring(100, L'(') + L"1"), "depth bounded");
    const auto date = EvaluateGlobalSearchCalculation(L"今天+30天");
    check(date && !date->has_value && date->text.size() == 10, "calendar calculation");
    const auto natural = NormalizeGlobalSearchQuery(L"上周的图片");
    const auto parsed = pulse::index::ParseQuery(natural);
    check(pulse::index::QueryHasExtFilter(parsed) && pulse::index::QueryUsesAttrs(parsed), "last week image filters reach query parser");
    check(NormalizeGlobalSearchQuery(L"\"今天的图片\"") == L"\"今天的图片\"", "quoted names preserved");
    check(NormalizeGlobalSearchQuery(L"path:C:\\test 图片") == L"path:C:\\test 图片", "explicit query preserved");
    const auto chips = ParseGlobalSearchQuery(L"品牌 上周的图片");
    check(chips.tokens.size() == 1 && chips.tokens[0].start == 3 && chips.tokens[0].length == 5, "removable token retains source span");
    check(NormalizeGlobalSearchQuery(L"今天的图片备份") == L"今天的图片备份", "natural filter requires word boundary");
    const auto with_name = pulse::index::ParseQuery(NormalizeGlobalSearchQuery(L"上周的图片 品牌"));
    check(pulse::index::QueryHasNameFilter(with_name) && pulse::index::QueryHasExtFilter(with_name) && pulse::index::QueryUsesAttrs(with_name), "natural filters preserve trailing name");
    const auto size_query = pulse::index::ParseQuery(NormalizeGlobalSearchQuery(L"size:>10mb"));
    check(pulse::index::QueryUsesAttrs(size_query), "explicit size unit filter retained");
    check(pulse::index::QueryUsesAttrs(pulse::index::ParseQuery(NormalizeGlobalSearchQuery(L">10mb"))), "smart size unit filter compiled");
    const auto converted = EvaluateGlobalSearchCalculation(L"5kg to lb");
    check(converted && converted->alternatives.size() == 3 && value(converted->alternatives[0].query.c_str(), 5000), "alternate unit pills retain original source precision");
    check(MatchGlobalSearchCommands(L">").size() == 6 && MatchGlobalSearchCommands(L"主题").empty(), "commands require prefix");
    check(MatchGlobalSearchCommands(L">主题") == std::vector{GlobalSearchCommand::ToggleTheme}, "theme command matching");
    check(MatchGlobalSearchCommands(L">clear history") == std::vector{GlobalSearchCommand::ClearHistory}, "multiple command keywords");
    check(MatchGlobalSearchCommands(L">清空暂存区") == std::vector{GlobalSearchCommand::ClearStaging}, "full localized command matching");
    check(MatchGlobalSearchCommands(L">not-a-command").empty(), "unknown command rejected");
    return failures ? 1 : 0;
}
