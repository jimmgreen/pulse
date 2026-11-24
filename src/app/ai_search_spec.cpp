// ai_search_spec.cpp - search_files arguments <-> AdvancedSearchSpec <-> chips.
#include "ai_search_spec.h"
#include "ai_tools_fs.h"
#include "../common/json_value.h"
#include "../common/localization.h"
#include "../index/search_kinds.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cwctype>

namespace pulse::app::ai {
namespace {

using I = l10n::StringId;

constexpr wchar_t kOpenFrom[] = L"1970-01-01";
constexpr wchar_t kOpenTo[] = L"2999-12-31";

ToolOutcome Invalid(const std::string& message) { return ToolOutcome::Fail("invalid_args", message); }
std::wstring T(I id) { return l10n::Get(id).c_str(); }
std::wstring Fill(std::wstring text, std::wstring_view key, const std::wstring& value) {
    if (const size_t at = text.find(key); at != std::wstring::npos) text.replace(at, key.size(), value);
    return text;
}

bool OneLine(const std::wstring& s, size_t maximum) {
    return !s.empty() && s.size() <= maximum && s.find_first_of(L"\r\n\t\"") == std::wstring::npos;
}

std::wstring Trimmed(std::wstring s) {
    while (!s.empty() && iswspace(s.back())) s.pop_back();
    size_t i = 0;
    while (i < s.size() && iswspace(s[i])) ++i;
    return s.substr(i);
}

bool IsDate(const std::string& d) {
    if (d.size() != 10 || d[4] != '-' || d[7] != '-') return false;
    for (size_t i = 0; i < d.size(); ++i)
        if (i != 4 && i != 7 && (d[i] < '0' || d[i] > '9')) return false;
    const int month = std::atoi(d.substr(5, 2).c_str());
    const int day = std::atoi(d.substr(8, 2).c_str());
    return month >= 1 && month <= 12 && day >= 1 && day <= 31;
}

// "100MB", "1.5 gb", "500kb" -> "100mb"; empty when unusable.
std::wstring SizeArg(const std::string& raw) {
    std::string s;
    for (char ch : raw)
        if (ch != ' ') s.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    size_t i = 0;
    while (i < s.size() && ((s[i] >= '0' && s[i] <= '9') || s[i] == '.')) ++i;
    if (i == 0 || i > 12) return {};
    const std::string unit = s.substr(i);
    if (unit != "b" && unit != "kb" && unit != "mb" && unit != "gb" && unit != "tb") return {};
    return json::Wide(s);
}

// "size:>100mb" -> "100 MB" (the number and an upper-case unit).
std::wstring SizeShown(std::wstring v) {
    size_t i = 0;
    while (i < v.size() && (iswdigit(v[i]) || v[i] == L'.')) ++i;
    std::wstring unit = v.substr(i);
    for (wchar_t& ch : unit) ch = static_cast<wchar_t>(towupper(ch));
    return v.substr(0, i) + L" " + unit;
}

std::wstring KindName(index::SearchKind kind) {
    switch (kind) {
        case index::SearchKind::Folder: return T(I::AiQKindFolder);
        case index::SearchKind::Document: return T(I::AiQKindDoc);
        case index::SearchKind::Image: return T(I::AiQKindImage);
        case index::SearchKind::Video: return T(I::AiQKindVideo);
        case index::SearchKind::Audio: return T(I::AiQKindAudio);
        case index::SearchKind::Archive: return T(I::AiQKindArchive);
        case index::SearchKind::Code: return T(I::AiQKindCode);
        default: return {};
    }
}

bool InList(const std::wstring& list, const std::wstring& ext) {
    size_t start = 0;
    while (start <= list.size()) {
        const size_t end = list.find(L';', start);
        const std::wstring item = list.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start);
        if (_wcsicmp(item.c_str(), ext.c_str()) == 0) return true;
        if (end == std::wstring::npos) break;
        start = end + 1;
    }
    return false;
}

// "pdf;png;jpg" -> "PDF、图片": pictures, videos and sounds by their kind,
// anything else by its extension.
std::wstring ExtsShown(const std::wstring& exts) {
    static const index::SearchKind kGroups[] = {index::SearchKind::Image, index::SearchKind::Video,
                                                index::SearchKind::Audio};
    std::vector<std::wstring> parts;
    const auto add = [&](const std::wstring& part) {
        if (std::find(parts.begin(), parts.end(), part) == parts.end()) parts.push_back(part);
    };
    size_t start = 0;
    while (start < exts.size()) {
        size_t end = exts.find(L';', start);
        if (end == std::wstring::npos) end = exts.size();
        const std::wstring ext = exts.substr(start, end - start);
        start = end + 1;
        if (ext.empty()) continue;
        bool grouped = false;
        for (index::SearchKind kind : kGroups)
            if (InList(index::KindExtensions(kind), ext)) { add(KindName(kind)); grouped = true; break; }
        if (!grouped) {
            std::wstring up = ext;
            for (wchar_t& ch : up) ch = static_cast<wchar_t>(towupper(ch));
            add(up);
        }
    }
    std::wstring out;
    const std::wstring sep = T(I::AiQListSep);
    for (size_t i = 0; i < parts.size(); ++i) out += (i ? sep : L"") + parts[i];
    return out;
}

// One day as the design shows it: 9月1日 (this year), 2025年9月1日.
std::wstring DayShown(const std::wstring& d, int this_year) {
    if (d.size() != 10 || d[4] != L'-' || d[7] != L'-') return d;
    const int year = _wtoi(d.substr(0, 4).c_str());
    const std::wstring month = std::to_wstring(_wtoi(d.substr(5, 2).c_str()));
    const std::wstring day = std::to_wstring(_wtoi(d.substr(8, 2).c_str()));
    std::wstring pic = T(year == this_year ? I::AiQDay : I::AiQDayYear);
    pic = Fill(pic, L"{y}", std::to_wstring(year));
    pic = Fill(pic, L"{m}", month);
    return Fill(pic, L"{d}", day);
}

std::wstring Leaf(const std::wstring& path) {
    std::wstring p = path;
    while (p.size() > 3 && (p.back() == L'\\' || p.back() == L'/')) p.pop_back();
    const size_t at = p.find_last_of(L"\\/");
    return at == std::wstring::npos || at + 1 >= p.size() ? p : p.substr(at + 1);
}

} // namespace

bool SearchSpecFromArgs(const json::Value& args, AdvancedSearchSpec& spec, ToolOutcome& error) {
    spec = {};
    spec.name_how = NameMatchHow::Contains;
    if (const json::Value* v = args.find("name"); v && !v->as_string().empty()) {
        const std::wstring name = Trimmed(json::Wide(v->as_string()));
        if (!OneLine(name, 128)) { error = Invalid("name must be 1 to 128 characters on one line, without quotes"); return false; }
        spec.name = name;
    }
    if (const json::Value* v = args.find("exclude"); v && !v->as_string().empty()) {
        const std::wstring ex = Trimmed(json::Wide(v->as_string()));
        if (!OneLine(ex, 64)) { error = Invalid("exclude must be 1 to 64 characters on one line"); return false; }
        spec.exclude_name = ex;
    }
    if (const json::Value* v = args.find("type"); v && !v->as_string().empty()) {
        const std::string& t = v->as_string();
        static const struct { const char* id; index::SearchKind kind; } kTypes[] = {
            {"folder", index::SearchKind::Folder},     {"document", index::SearchKind::Document},
            {"image", index::SearchKind::Image},       {"video", index::SearchKind::Video},
            {"audio", index::SearchKind::Audio},       {"archive", index::SearchKind::Archive},
            {"code", index::SearchKind::Code},
        };
        bool known = false;
        for (const auto& k : kTypes)
            if (t == k.id) { spec.kind = k.kind; known = true; }
        if (!known) {
            error = Invalid("type must be one of: folder, document, image, video, audio, archive, code");
            return false;
        }
    }
    if (const json::Value* v = args.find("extensions"); v && v->is_array() && v->size()) {
        if (v->size() > 24) { error = Invalid("at most 24 extensions"); return false; }
        std::wstring list;
        for (const json::Value& e : v->items()) {
            std::wstring ext = json::Wide(e.as_string());
            while (!ext.empty() && ext.front() == L'.') ext.erase(ext.begin());
            const bool ok = !ext.empty() && ext.size() <= 12 &&
                            std::all_of(ext.begin(), ext.end(), [](wchar_t ch) { return iswalnum(ch) != 0; });
            if (!ok) { error = Invalid("extensions are like \"pdf\" or \"jpg\": letters and digits only"); return false; }
            for (wchar_t& ch : ext) ch = static_cast<wchar_t>(towlower(ch));
            if (!InList(list, ext)) list += (list.empty() ? L"" : L";") + ext;
        }
        // Extensions name the files more exactly than a type does.
        spec.kind = index::SearchKind::Custom;
        spec.custom_exts = list;
    }
    const json::Value* after = args.find("modified_after");
    const json::Value* before = args.find("modified_before");
    if ((after && !after->as_string().empty()) || (before && !before->as_string().empty())) {
        const std::string a = after ? after->as_string() : std::string();
        const std::string b = before ? before->as_string() : std::string();
        if ((!a.empty() && !IsDate(a)) || (!b.empty() && !IsDate(b))) {
            error = Invalid("modified_after and modified_before are dates as YYYY-MM-DD");
            return false;
        }
        if (!a.empty() && !b.empty() && a > b) { error = Invalid("modified_after is later than modified_before"); return false; }
        spec.date = DatePreset::Custom;
        spec.date_from = a.empty() ? kOpenFrom : json::Wide(a);
        spec.date_to = b.empty() ? kOpenTo : json::Wide(b);
    }
    std::wstring size;
    for (const char* key : {"larger_than", "smaller_than"}) {
        const json::Value* v = args.find(key);
        if (!v || v->as_string().empty()) continue;
        const std::wstring amount = SizeArg(v->as_string());
        if (amount.empty()) { error = Invalid(std::string(key) + " is a size like \"100MB\", \"1.5GB\" or \"500KB\""); return false; }
        size += (size.empty() ? L"" : L" ") + std::wstring(L"size:") + (key[0] == 'l' ? L">" : L"<") + amount;
    }
    if (!size.empty()) {
        spec.size = SizePreset::Custom;
        spec.size_custom = size;
    }
    if (const json::Value* v = args.find("content"); v && !v->as_string().empty()) {
        const std::wstring content = Trimmed(json::Wide(v->as_string()));
        if (!OneLine(content, 128)) {
            error = Invalid("content must be 1 to 128 characters on one line, without quotes");
            return false;
        }
        if (spec.kind == index::SearchKind::Folder) {
            error = Invalid("content search applies to files, not type \"folder\"");
            return false;
        }
        spec.content = content;
    }
    if (const json::Value* v = args.find("content_exclude"); v && !v->as_string().empty()) {
        const std::wstring ex = Trimmed(json::Wide(v->as_string()));
        if (!OneLine(ex, 64)) {
            error = Invalid("content_exclude must be 1 to 64 characters on one line, without quotes");
            return false;
        }
        spec.content_exclude = ex;
    }
    if (const json::Value* v = args.find("content_mode"); v && !v->as_string().empty()) {
        const std::string& m = v->as_string();
        if (m == "all") spec.content_mode = index::ContentMatchMode::AllWords;
        else if (m == "any") spec.content_mode = index::ContentMatchMode::AnyWord;
        else if (m == "phrase") spec.content_mode = index::ContentMatchMode::Phrase;
        else {
            error = Invalid("content_mode must be one of: all, any, phrase");
            return false;
        }
    }
    if (const json::Value* v = args.find("folder"); v && !v->as_string().empty()) {
        std::wstring folder;
        if (!CheckPathArg(*v, "folder", folder, error)) return false;
        spec.location = LocationScope::CustomFolder;
        spec.custom_folder = folder;
    }
    if (!SearchHasCondition(spec)) {
        error = Invalid("give at least one condition: name, content, type, extensions, a modified date or a size");
        return false;
    }
    return true;
}

bool SearchHasCondition(const AdvancedSearchSpec& spec) {
    return !Trimmed(spec.name).empty() || !Trimmed(spec.exclude_name).empty() ||
           spec.kind != index::SearchKind::Any || !Trimmed(spec.content).empty() || spec.date != DatePreset::Any ||
           spec.size != SizePreset::Any;
}

std::wstring SearchDateText(const std::wstring& from, const std::wstring& to, int this_year) {
    // dm:a alone (a day, a month, a year) as it was typed.
    if (to.empty()) return from.empty() || from == kOpenFrom ? std::wstring() : DayShown(from, this_year);
    if (from.empty()) return to == kOpenTo ? std::wstring() : DayShown(to, this_year);
    const bool open_from = from == kOpenFrom;
    const bool open_to = to == kOpenTo;
    if (open_from && open_to) return {};
    if (open_to) return Fill(T(I::AiQAfter), L"{d}", DayShown(from, this_year));
    if (open_from) return Fill(T(I::AiQBefore), L"{d}", DayShown(to, this_year));
    if (from == to) return DayShown(from, this_year);
    return DayShown(from, this_year) + L" \u2013 " + DayShown(to, this_year);
}

std::vector<SearchChip> SearchChips(const AdvancedSearchSpec& spec, std::wstring (*folder_label)(const std::wstring&)) {
    std::vector<SearchChip> out;
    SYSTEMTIME now{};
    GetLocalTime(&now);
    if (const std::wstring name = Trimmed(spec.name); !name.empty())
        out.push_back({SearchField::Name, T(I::AiQKeyName), Fill(T(I::AiQContains), L"{x}", name)});
    if (const std::wstring ex = Trimmed(spec.exclude_name); !ex.empty())
        out.push_back({SearchField::Exclude, T(I::AiQKeyExclude), Fill(T(I::AiQNotContains), L"{x}", ex)});
    if (spec.kind == index::SearchKind::Custom) {
        if (const std::wstring exts = ExtsShown(NormalizeExtensionList(spec.custom_exts)); !exts.empty())
            out.push_back({SearchField::Type, T(I::AiQKeyType), exts});
    } else if (spec.kind != index::SearchKind::Any) {
        out.push_back({SearchField::Type, T(I::AiQKeyType), KindName(spec.kind)});
    }
    if (const std::wstring content = Trimmed(spec.content); !content.empty())
        out.push_back({SearchField::Content, T(I::AiQKeyContent), Fill(T(I::AiQContains), L"{x}", content)});
    std::wstring date;
    switch (spec.date) {
        case DatePreset::Today: date = T(I::AiQToday); break;
        case DatePreset::Yesterday: date = T(I::AiQYesterday); break;
        case DatePreset::ThisWeek: date = T(I::AiQThisWeek); break;
        case DatePreset::ThisMonth: date = T(I::AiQThisMonth); break;
        case DatePreset::ThisYear: date = T(I::AiQThisYear); break;
        case DatePreset::Custom: date = SearchDateText(spec.date_from, spec.date_to, now.wYear); break;
        default: break;
    }
    if (!date.empty()) out.push_back({SearchField::Date, T(I::AiQKeyDate), date});
    std::wstring size;
    switch (spec.size) {
        case SizePreset::Empty: size = L"0"; break;
        case SizePreset::Lt1MB: size = Fill(T(I::AiQSmaller), L"{a}", L"1 MB"); break;
        case SizePreset::From1To10MB: size = L"1 MB \u2013 10 MB"; break;
        case SizePreset::Gt10MB: size = Fill(T(I::AiQLarger), L"{a}", L"10 MB"); break;
        case SizePreset::Gt100MB: size = Fill(T(I::AiQLarger), L"{a}", L"100 MB"); break;
        case SizePreset::Gt1GB: size = Fill(T(I::AiQLarger), L"{a}", L"1 GB"); break;
        case SizePreset::Custom: {
            // size:>a size:<b -> "a – b", one side -> 大于 a / 小于 b.
            std::wstring lo, hi;
            size_t start = 0;
            const std::wstring& raw = spec.size_custom;
            while (start < raw.size()) {
                size_t end = raw.find(L' ', start);
                if (end == std::wstring::npos) end = raw.size();
                std::wstring tok = raw.substr(start, end - start);
                start = end + 1;
                if (const size_t colon = tok.find(L':'); colon != std::wstring::npos) tok = tok.substr(colon + 1);
                if (tok.empty()) continue;
                const bool more = tok[0] == L'>';
                const bool less = tok[0] == L'<';
                size_t i = (more || less) ? 1 : 0;
                if (i < tok.size() && tok[i] == L'=') ++i;
                (more ? lo : hi) = SizeShown(tok.substr(i));
                if (!more && !less) lo = hi;
            }
            if (!lo.empty() && !hi.empty() && lo != hi) size = lo + L" \u2013 " + hi;
            else if (!lo.empty() && lo == hi) size = lo;
            else if (!lo.empty()) size = Fill(T(I::AiQLarger), L"{a}", lo);
            else if (!hi.empty()) size = Fill(T(I::AiQSmaller), L"{a}", hi);
            break;
        }
        default: break;
    }
    if (!size.empty()) out.push_back({SearchField::Size, T(I::AiQKeySize), size});
    std::wstring folder;
    if (spec.location == LocationScope::CustomFolder) folder = spec.custom_folder;
    else if (spec.location == LocationScope::CurrentFolder) folder = spec.current_folder;
    folder = Trimmed(folder);
    if (folder.empty()) {
        out.push_back({SearchField::Folder, T(I::AiQKeyFolder), T(I::AiQEverywhere), true});
    } else {
        std::wstring label = folder_label ? folder_label(folder) : std::wstring();
        if (label.empty()) label = Leaf(folder);
        out.push_back({SearchField::Folder, T(I::AiQKeyFolder), label});
    }
    return out;
}

AdvancedSearchSpec SearchSpecFromQuery(const std::wstring& query) {
    AdvancedSearchSpec spec = ParseSearchQuery(query);
    spec.origin.reset();
    spec.parsed_baseline.reset();
    spec.name_is_query = false;
    if (spec.date == DatePreset::Custom) {
        size_t start = 0;
        while (start < query.size()) {
            size_t end = query.find(L' ', start);
            if (end == std::wstring::npos) end = query.size();
            const std::wstring tok = query.substr(start, end - start);
            start = end + 1;
            const size_t colon = tok.find(L':');
            if (colon == std::wstring::npos) continue;
            const std::wstring key = tok.substr(0, colon);
            if (_wcsicmp(key.c_str(), L"dm") != 0 && _wcsicmp(key.c_str(), L"datemodified") != 0 &&
                key != L"\u4fee\u6539" && key != L"\u65e5\u671f")
                continue;
            const std::wstring value = tok.substr(colon + 1);
            if (const size_t dots = value.find(L".."); dots != std::wstring::npos) {
                spec.date_from = value.substr(0, dots);
                spec.date_to = value.substr(dots + 2);
                if (spec.date_from.empty()) spec.date_from = kOpenFrom;
                if (spec.date_to.empty()) spec.date_to = kOpenTo;
            } else {
                spec.date_from = value;
            }
            break;
        }
        if (spec.date_from.empty() && spec.date_to.empty()) spec.date = DatePreset::Any;
    }
    return spec;
}

void ClearSearchField(AdvancedSearchSpec& spec, SearchField field) {
    // A spec the user's own query produced compiles from its fields from now on.
    spec.origin.reset();
    spec.parsed_baseline.reset();
    spec.name_is_query = false;
    switch (field) {
        case SearchField::Name: spec.name.clear(); spec.name_how = NameMatchHow::Contains; break;
        case SearchField::Exclude: spec.exclude_name.clear(); break;
        case SearchField::Type: spec.kind = index::SearchKind::Any; spec.custom_exts.clear(); break;
        case SearchField::Content: spec.content.clear(); spec.content_exclude.clear(); break;
        case SearchField::Date: spec.date = DatePreset::Any; spec.date_from.clear(); spec.date_to.clear(); break;
        case SearchField::Size: spec.size = SizePreset::Any; spec.size_custom.clear(); break;
        case SearchField::Folder:
            spec.location = LocationScope::Indexed;
            spec.custom_folder.clear();
            break;
    }
}

} // namespace pulse::app::ai
