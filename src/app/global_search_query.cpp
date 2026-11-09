#include "global_search_query.h"
#include "search_query.h"
#include <chrono>
#include <cmath>
#include <ctime>
#include <cwctype>
#include <iomanip>
#include <locale>
#include <regex>
#include <sstream>

namespace pulse::app {
namespace {
std::wstring Number(double value) {
    std::wostringstream out;
    out.imbue(std::locale::classic());
    out << std::setprecision(12) << (value == 0 ? 0 : value);
    return out.str();
}
std::wstring DateText(std::chrono::year_month_day date) {
    std::wostringstream out;
    out << std::setfill(L'0') << std::setw(4) << int(date.year()) << L'-' << std::setw(2) << unsigned(date.month()) << L'-' << std::setw(2) << unsigned(date.day());
    return out.str();
}
class Expression {
public:
    Expression(std::wstring_view input, double answer) : input_(input), answer_(answer) {}
    double Parse() { const double v = Sum(); if (pos_ != input_.size()) valid_ = false; return v; }
    bool valid() const { return valid_; }
private:
    bool Take(wchar_t c) { if (pos_ < input_.size() && input_[pos_] == c) { ++pos_; return true; } return false; }
    double Sum() { double v = Product(); for (;;) { if (Take(L'+')) v += Product(); else if (Take(L'-')) v -= Product(); else return v; } }
    double Product() { double v = Unary(); for (;;) { if (Take(L'*')) v *= Unary(); else if (Take(L'/')) v /= Unary(); else if (Take(L'%')) v = std::fmod(v, Unary()); else return v; } }
    double Unary() {
        if (++depth_ > 64) { valid_ = false; --depth_; return 0; }
        double v;
        if (Take(L'+')) v = Unary();
        else if (Take(L'-')) v = -Unary();
        else { v = Atom(); if (Take(L'^')) v = std::pow(v, Unary()); }
        --depth_; return v;
    }
    double Atom() {
        if (Take(L'(')) { const double v = Sum(); if (!Take(L')')) valid_ = false; return v; }
        if (input_.substr(pos_, 3) == L"ans") { pos_ += 3; return answer_; }
        if (input_.substr(pos_, 4) == L"sqrt") { pos_ += 4; return std::sqrt(Unary()); }
        const size_t start = pos_;
        bool dot = false, digit = false;
        double value = 0, scale = 1;
        while (pos_ < input_.size()) {
            const wchar_t c = input_[pos_];
            if (c == L'.' && !dot) { dot = true; ++pos_; continue; }
            if (c < L'0' || c > L'9') break;
            digit = true; ++pos_;
            if (dot) { scale *= .1; value += (c - L'0') * scale; }
            else value = value * 10 + (c - L'0');
        }
        if (!digit || pos_ == start) valid_ = false;
        return value;
    }
    std::wstring_view input_;
    size_t pos_ = 0;
    double answer_;
    unsigned depth_ = 0;
    bool valid_ = true;
};
struct Unit { const wchar_t* name; int family; double factor; };
constexpr Unit units[] = {
    {L"kg",0,1},{L"g",0,.001},{L"lb",0,.45359237},{L"oz",0,.028349523125},{L"t",0,1000},
    {L"m",1,1},{L"km",1,1000},{L"cm",1,.01},{L"mm",1,.001},{L"mi",1,1609.344},{L"ft",1,.3048},{L"in",1,.0254},
    {L"b",2,1},{L"kb",2,1024},{L"mb",2,1048576},{L"gb",2,1073741824},
    {L"s",3,1},{L"min",3,60},{L"h",3,3600}
};
std::wstring UnitName(std::wstring s) {
    const std::pair<const wchar_t*, const wchar_t*> aliases[] = {
        {L"公斤",L"kg"},{L"千克",L"kg"},{L"磅",L"lb"},{L"英里",L"mi"},{L"公里",L"km"},
        {L"米",L"m"},{L"英尺",L"ft"},{L"英寸",L"in"},{L"°c",L"c"},{L"°f",L"f"}
    };
    for (const auto& a : aliases) if (s == a.first) return a.second;
    return s;
}
}

std::wstring NormalizeGlobalSearchQuery(std::wstring_view raw) {
    // Preserve explicit query operators and quoted filename literals verbatim.
    if (raw.find_first_of(L"\"|:") != std::wstring_view::npos) return std::wstring(raw);
    std::wstring expanded(raw);
    static const std::wregex combined(L"(^|\\s)(今天|昨天|本周|本月)(?:的)?(图片|照片|视频|音频|文档|代码|压缩包|文件夹)(?=\\s|$)");
    expanded = std::regex_replace(expanded, combined, L"$1$2 $3");
    bool last_week = false;
    static const std::wregex last(L"(^|\\s)上周(?:的)?(图片|照片|视频|音频|文档|代码|压缩包|文件夹)?(?=\\s|$)");
    if (std::regex_search(expanded, last)) { expanded = std::regex_replace(expanded, last, L"$1$2"); last_week = true; }
    AdvancedSearchSpec spec;
    std::wstring rest;
    const bool smart = ExtractSmartFilters(expanded, spec, rest);
    if (!smart && !last_week) return std::wstring(raw);
    spec.name = rest;
    std::wstring result = CompileSearchQuery(spec);
    if (last_week) {
        const std::time_t now = std::time(nullptr);
        std::tm local{};
        if (localtime_s(&local, &now) != 0) return std::wstring(raw);
        using namespace std::chrono;
        const sys_days today{year{local.tm_year + 1900}/month{static_cast<unsigned>(local.tm_mon + 1)}/day{static_cast<unsigned>(local.tm_mday)}};
        const sys_days monday = today - days{(local.tm_wday + 6) % 7};
        result += L" dm:" + DateText(year_month_day{monday - days{7}}) + L".." + DateText(year_month_day{monday - days{1}});
    }
    return result;
}

GlobalSearchParsedQuery ParseGlobalSearchQuery(std::wstring_view raw) {
    GlobalSearchParsedQuery parsed{NormalizeGlobalSearchQuery(raw), {}};
    if (raw.find_first_of(L"\"|") != std::wstring_view::npos) return parsed;
    for (size_t start = 0; start < raw.size();) {
        if (iswspace(raw[start])) { ++start; continue; }
        size_t end = start;
        while (end < raw.size() && !iswspace(raw[end])) ++end;
        const auto word = raw.substr(start, end - start);
        const bool explicit_filter = word.starts_with(L"ext:") || word.starts_with(L"dm:") ||
            word.starts_with(L"size:") || word == L"folder:" || word == L"file:";
        if (explicit_filter || (raw.find(L':') == std::wstring_view::npos && NormalizeGlobalSearchQuery(word) != word)) {
            parsed.tokens.push_back({std::wstring(word), start, end - start});
        }
        start = end;
    }
    return parsed;
}

std::optional<GlobalSearchCalculation> EvaluateGlobalSearchCalculation(std::wstring_view raw, double answer) {
    if (raw.empty() || raw.size() > 1024) return std::nullopt;
    std::wstring s;
    for (wchar_t c : raw) if (!iswspace(c)) s += c == L'×' ? L'*' : c == L'÷' ? L'/' : static_cast<wchar_t>(towlower(c));
    std::wsmatch m;
    static const std::wregex date(L"(?:今天|today)([+-])([0-9]{1,6})(天|d|周|w)");
    if (std::regex_match(s, m, date)) {
        const int offset = std::stoi(m[2]) * ((m[3] == L"周" || m[3] == L"w") ? 7 : 1) * (m[1] == L"-" ? -1 : 1);
        const std::time_t now = std::time(nullptr);
        std::tm local{};
        if (localtime_s(&local, &now) != 0) return std::nullopt;
        using namespace std::chrono;
        const year_month_day today{year{local.tm_year + 1900}, month{static_cast<unsigned>(local.tm_mon + 1)}, day{static_cast<unsigned>(local.tm_mday)}};
        const year_month_day target{sys_days{today} + days{offset}};
        if (int(target.year()) < 1 || int(target.year()) > 9999) return std::nullopt;
        std::wostringstream out;
        out << std::setfill(L'0') << std::setw(4) << int(target.year()) << L'-' << std::setw(2) << unsigned(target.month()) << L'-' << std::setw(2) << unsigned(target.day());
        return GlobalSearchCalculation{out.str(), 0, false, {}};
    }
    static const std::wregex percent(L"([0-9]+(?:\\.[0-9]+)?)%(?:of|的)([0-9]+(?:\\.[0-9]+)?)");
    static const std::wregex conversion(L"([+-]?[0-9]+(?:\\.[0-9]+)?)([^0-9+*/().=-]+?)(?:to|转|换成)([^0-9+*/().=-]+)");
    double value = 0;
    std::wstring suffix;
    std::vector<GlobalSearchCalculationAlternative> alternatives;
    try {
        if (std::regex_match(s, m, percent)) value = std::stod(m[1]) * std::stod(m[2]) / 100;
        else if (std::regex_match(s, m, conversion)) {
            const double n = std::stod(m[1]);
            const auto a = UnitName(m[2]), b = UnitName(m[3]);
            if (a == L"c" && b == L"f") value = n * 9 / 5 + 32;
            else if (a == L"f" && b == L"c") value = (n - 32) * 5 / 9;
            else {
                const Unit *from = nullptr, *to = nullptr;
                for (const auto& u : units) { if (a == u.name) from = &u; if (b == u.name) to = &u; }
                if (!from || !to || from->family != to->family) return std::nullopt;
                value = n * from->factor / to->factor;
                for (const auto& unit : units) {
                    if (unit.family != from->family || a == unit.name || b == unit.name) continue;
                    const double alternative = n * from->factor / unit.factor;
                    if (std::isfinite(alternative)) {
                        alternatives.push_back({Number(alternative) + L" " + unit.name,
                            m[1].str() + a + L" to " + unit.name});
                    }
                    if (alternatives.size() == 5) break;
                }
            }
            suffix = L" " + b;
        } else {
            if (s.find_first_of(L"+-*/^%(") == std::wstring::npos && s.find(L"sqrt") == std::wstring::npos) return std::nullopt;
            Expression expression(s, answer);
            value = expression.Parse();
            if (!expression.valid()) return std::nullopt;
        }
    } catch (const std::exception&) { return std::nullopt; }
    if (!std::isfinite(value)) return std::nullopt;
    return GlobalSearchCalculation{Number(value) + suffix, value, true, std::move(alternatives)};
}

std::vector<GlobalSearchCommand> MatchGlobalSearchCommands(std::wstring_view raw) {
    while (!raw.empty() && iswspace(raw.front())) raw.remove_prefix(1);
    if (raw.empty() || raw.front() != L'>') return {};
    raw.remove_prefix(1);
    while (!raw.empty() && iswspace(raw.front())) raw.remove_prefix(1);
    while (!raw.empty() && iswspace(raw.back())) raw.remove_suffix(1);
    std::wstring needle(raw);
    for (auto& c : needle) c = static_cast<wchar_t>(towlower(c));
    struct CommandWords { GlobalSearchCommand command; const wchar_t* keywords; };
    static constexpr CommandWords commands[] = {
        {GlobalSearchCommand::ToggleTheme, L"theme dark light 主题 主題 深色 浅色 淺色 外观 外觀 切换 切換"},
        {GlobalSearchCommand::ToggleView, L"view list thumbnails 视图 視圖 列表 缩略图 縮圖 切换 切換"},
        {GlobalSearchCommand::OpenStaging, L"open staging tray 打开暂存区 開啟暫存區"},
        {GlobalSearchCommand::ClearStaging, L"clear staging tray 清空暂存区 清空暫存區"},
        {GlobalSearchCommand::ContinueSearch, L"continue search main 在主界面继续搜索 在主介面繼續搜尋"},
        {GlobalSearchCommand::ClearHistory, L"clear search history 清空搜索历史 清空搜尋歷史"}
    };
    std::vector<GlobalSearchCommand> matches;
    for (const auto& command : commands) {
        bool match = true;
        std::wistringstream words(needle);
        for (std::wstring word; words >> word;) {
            if (std::wstring_view(command.keywords).find(word) == std::wstring_view::npos) { match = false; break; }
        }
        if (match) matches.push_back(command.command);
    }
    return matches;
}
}
